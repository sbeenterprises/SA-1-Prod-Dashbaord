// Signal K engine paths -> dashboard engine fields (SK-1, SK-2 in
// docs/requirements-usv-725.md). Header-only and free of the WebSocket and
// JSON libraries so signalk_engine_test.cpp builds anywhere.
//
// Signal K publishes one engine as propulsion.<id>.* where <id> is the N2K
// engine instance ("0", "1") or a name the server was given ("port",
// "starboard"). Units are SI: revolutions in Hz, temperatures in K,
// pressures in Pa, fuel rate in m3/s, run time in s, load and trim as
// ratios 0..1.
#pragma once

#include <cctype>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace usv {
namespace sk {

// "0" / "port" -> 0, "1" / "starboard" -> 1, any other integer -> itself,
// anything else -> -1 (not an engine this release charts).
inline int EngineInstance(const std::string& id) {
    if (id == "port") return 0;
    if (id == "starboard") return 1;
    if (id.empty() || id.size() > 3) return -1;
    for (char c : id)
        if (!std::isdigit(static_cast<unsigned char>(c))) return -1;
    return std::stoi(id);
}

struct EngineField {
    const char* measurement;
    const char* field;
    double value;
};

// Numeric propulsion.<id>.<rest> paths. Returns false for a path the
// dashboards do not chart (the server publishes more than these).
inline bool MapPropulsion(const std::string& rest, double v, EngineField* out) {
    struct Row { const char* rest; const char* measurement; const char* field; double (*conv)(double); };
    static const Row kRows[] = {
        {"revolutions", "engine", "rpm", [](double x) { return x * 60.0; }},            // Hz -> rpm
        {"temperature", "engine", "coolant_temp", [](double x) { return x - 273.15; }}, // 127489 temperature
        {"coolantTemperature", "engine", "coolant_temp", [](double x) { return x - 273.15; }},
        {"oilPressure", "engine", "oil_pressure", [](double x) { return x / 1000.0; }},  // Pa -> kPa
        {"oilTemperature", "engine", "oil_temp", [](double x) { return x - 273.15; }},
        {"boostPressure", "engine", "intake_manifold_pressure", [](double x) { return x / 1000.0; }},
        {"fuel.rate", "engine", "fuel_rate", [](double x) { return x * 3.6e6; }},       // m3/s -> L/h
        {"fuel.level", "engine", "fuel_level", [](double x) { return x * 100.0; }},     // a source that puts the tank under the engine
        {"runTime", "engine", "runtime_h", [](double x) { return x / 3600.0; }},
        // A stopped engine reports -0.01 (usv-722 Yamaha gateway): 0 %, not -1 %.
        {"engineLoad", "engine", "percent_load", [](double x) { return x < 0.0 ? 0.0 : x * 100.0; }},
        {"engineTorque", "engine", "percent_torque", [](double x) { return x < 0.0 ? 0.0 : x * 100.0; }},
        {"drive.trimState", "engine", "trim_position", [](double x) { return x * 100.0; }},
        {"alternatorVoltage", "electrical", "alternator_voltage", [](double x) { return x; }},
    };
    for (const Row& r : kRows) {
        if (rest == r.rest) {
            *out = {r.measurement, r.field, r.conv(v)};
            return true;
        }
    }
    return false;
}

// transmission.gear: Forward 1, Neutral 0, Reverse -1 (the can_bridge
// convention). "Fault" and anything else: not written.
inline bool GearValue(const std::string& s, double* out) {
    if (s == "Forward") { *out = 1; return true; }
    if (s == "Neutral") { *out = 0; return true; }
    if (s == "Reverse") { *out = -1; return true; }
    return false;
}

// "lowOilPressure" -> "Low Oil Pressure"
inline std::string Words(const std::string& camel) {
    std::string s;
    for (size_t i = 0; i < camel.size(); ++i) {
        const char c = camel[i];
        if (i == 0) { s += static_cast<char>(std::toupper(static_cast<unsigned char>(c))); continue; }
        if (std::isupper(static_cast<unsigned char>(c)) ||
            (std::isdigit(static_cast<unsigned char>(c)) &&
             !std::isdigit(static_cast<unsigned char>(camel[i - 1]))))
            s += ' ';
        s += c;
    }
    return s;
}

struct Lamps { bool red = false, mil = false, amber = false; };

// notifications.propulsion.<id>.<name>: the server raises one notification
// per discrete engine status bit (N2K 127489) with state alarm / warn /
// alert / emergency, and sets it back to normal (or null) when the bit
// clears. One tracker per engine instance gives the active count, the
// fault text and the lamps the Engine page shows.
class EngineNotifications {
public:
    // Returns true when the instance's summary changed.
    bool Update(int inst, const std::string& name, const std::string& state,
                const std::string& message) {
        auto& m = by_inst_[inst];
        const bool active = !state.empty() && state != "normal" && state != "nominal";
        auto it = m.find(name);
        if (!active) {
            if (it == m.end()) return false;
            m.erase(it);
            return true;
        }
        const Entry e{state, message};
        if (it != m.end() && it->second.state == e.state && it->second.message == e.message)
            return false;
        m[name] = e;
        return true;
    }

    int Active(int inst) const {
        auto it = by_inst_.find(inst);
        return it == by_inst_.end() ? 0 : static_cast<int>(it->second.size());
    }

    // "NONE", or "Check Engine (alarm); Low Oil Pressure (alarm): <message>"
    std::string Text(int inst) const {
        auto it = by_inst_.find(inst);
        if (it == by_inst_.end() || it->second.empty()) return "NONE";
        std::string s;
        for (const auto& kv : it->second) {
            if (!s.empty()) s += "; ";
            s += Words(kv.first) + " (" + kv.second.state + ")";
            if (!kv.second.message.empty() && kv.second.message != Words(kv.first))
                s += ": " + kv.second.message;
        }
        return s;
    }

    // Lamps: red (stop) for the conditions that end a run, MIL for the check
    // engine bit, amber for any other active notification.
    Lamps LampsOf(int inst) const {
        Lamps l;
        auto it = by_inst_.find(inst);
        if (it == by_inst_.end()) return l;
        for (const auto& kv : it->second) {
            const std::string& n = kv.first;
            if (n == "checkEngine") l.mil = true;
            else if (n == "emergencyStopMode" || n == "engineShuttingDown" ||
                     n == "overTemperature" || n == "lowOilPressure") l.red = true;
            else l.amber = true;
        }
        return l;
    }

private:
    struct Entry { std::string state, message; };
    std::map<int, std::map<std::string, Entry>> by_inst_;
};

}  // namespace sk
}  // namespace usv
