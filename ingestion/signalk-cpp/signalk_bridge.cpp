// Signal K -> InfluxDB bridge (FR-2 navigation, FR-4 vessel health, FR-8).
//
// Connects to the Signal K server's WebSocket delta stream and maps Signal K
// paths to InfluxDB measurements/fields. Signal K units are SI (m/s, rad, K,
// Pa, ratios); conversion to display units (kn, deg, °C, kPa, %) happens
// here so dashboards stay unit-simple.
//
// Env: SIGNALK_URL, INFLUX_URL, INFLUX_TOKEN, INFLUX_ORG, INFLUX_BUCKET,
//      VESSEL_ID, SIGNALK_BATTERY (battery instance to chart, e.g. House;
//      default: the first electrical.batteries.<id> seen)
//      SIGNALK_FUEL_TANK (tanks.fuel.<id> instance for the fuel level;
//      default: the first fuel tank seen)
//      SIGNALK_BATTERY_CURRENT_SIGN (-1 when the shunt reports charging as
//      negative current; default 1, the Signal K convention)
//      ENGINE_SOURCE / ENGINE_TYPE (cpp-common/engine_profile.hpp): engine
//      fields are written only when Signal K is this vessel's engine source;
//      every engine point carries instance and engine_type tags.
// Engine paths (propulsion.<id>.*, notifications.propulsion.<id>.*,
// tanks.fuel.<id>.currentLevel) are mapped in signalk_engine.hpp.
// Reconnection: IXWebSocket automatic backoff + status heartbeat.

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include "../cpp-common/engine_profile.hpp"
#include "../cpp-common/influx_writer.hpp"
#include "signalk_engine.hpp"

using json = nlohmann::json;

namespace {

constexpr double RAD = 57.29577951308232;
constexpr double MS_TO_KN = 1.94384;

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop = true; }

struct Mapping {
    const char* measurement;
    const char* field;
    std::function<double(double)> convert;
};

const std::map<std::string, Mapping> kPaths = {
    // Navigation (FR-2)
    // navigation.speedOverGround / courseOverGroundTrue: handled in
    // handle_value (COG is suppressed below kCogMinSogKn).
    {"navigation.headingTrue",
     {"nav", "heading", [](double v) { return v * RAD; }}},
    {"navigation.gnss.satellites",
     {"nav", "satellites", [](double v) { return v; }}},
    {"navigation.gnss.horizontalDilution",
     {"nav", "hdop", [](double v) { return v; }}},
    {"navigation.courseRhumbline.crossTrackError",
     {"nav", "cross_track_error", [](double v) { return v; }}},
    // The SK course computer's next point is whatever course the Signal K
    // server holds (Swiftships-UI manual planning) — NOT the autonomy route.
    // Kept as its own field; mission.dist_to_waypoint has exactly one
    // producer (grafana_bridge, from DIST_TO_NEXT_WPT).
    {"navigation.courseRhumbline.nextPoint.distance",
     {"nav", "course_next_point", [](double v) { return v; }}},
    // Engine (FR-4): propulsion.<id>.* in handle_propulsion() below.
    // Electrical (FR-4). Battery paths (electrical.batteries.<id>.*) are
    // handled in handle_battery(): the instance is named by the source
    // (usv-722's Victron VE.Direct shunt publishes "House", not "0").
    {"electrical.alternators.0.voltage",
     {"electrical", "alternator_voltage", [](double v) { return v; }}},
    // Power (FR-4)
    {"electrical.loads.total.power",
     {"power", "total_power", [](double v) { return v; }}},
    {"electrical.system.voltage",
     {"power", "system_voltage", [](double v) { return v; }}},
};

class Bridge {
public:
    void handle_delta(const json& delta) {
        std::lock_guard<std::mutex> lock(mu_);
        // Hello message: {"name": ..., "version": ..., "self": "vessels.urn:..."}
        // names this vessel's context, so deltas for other vessels (AIS
        // targets, which Signal K sends with subscribe=all) can be told apart.
        if (delta.contains("self") && delta["self"].is_string()) {
            self_ = delta["self"].get<std::string>();
            if (self_.rfind("vessels.", 0) != 0) self_ = "vessels." + self_;
            std::cerr << "signal k self context: " << self_ << "\n";
        }
        if (!delta.contains("updates")) return;
        if (!IsSelf(delta)) {
            ++other_count_;
            return;
        }
        for (const auto& update : delta["updates"]) {
            if (!update.contains("values")) continue;
            for (const auto& item : update["values"]) {
                if (!item.contains("path") || !item.contains("value")) continue;
                handle_value(item["path"].get<std::string>(), item["value"]);
            }
        }
        ++msg_count_;
    }

    // Own-ship deltas only. Another vessel's position, speed or heading
    // written as ours makes the track jump between AIS targets. A delta with
    // no context, "vessels.self" or the context from the hello is own ship;
    // anything else (other vessels, aircraft, aton, ...) is skipped.
    bool IsSelf(const json& delta) const {
        if (!delta.contains("context") || !delta["context"].is_string()) return true;
        const std::string ctx = delta["context"].get<std::string>();
        if (ctx.empty() || ctx == "vessels.self") return true;
        return !self_.empty() && ctx == self_;
    }

    void set_up(bool up) {
        std::lock_guard<std::mutex> lock(mu_);
        up_ = up;
        writer_.field("comms", "", "signalk_ws_up", up ? 1 : 0);
        writer_.flush();
    }

    void tick() {
        std::lock_guard<std::mutex> lock(mu_);
        const double now = mono_s();
        if (last_emit_ == 0.0) last_emit_ = now;
        if (now - last_emit_ >= 5.0) {
            writer_.field("comms", "", "signalk_msg_rate",
                          msg_count_ / (now - last_emit_));
            // Heartbeat: the link state is re-written with the rate so a
            // dashboard window that starts hours after the last change still
            // sees UP/DOWN instead of "No data".
            writer_.field("comms", "", "signalk_ws_up", up_ ? 1 : 0);
            if (other_count_ && !other_logged_) {
                std::cerr << "signal k: skipping deltas for other vessels (AIS); "
                             "own ship = " << (self_.empty() ? "vessels.self" : self_) << "\n";
                other_logged_ = true;
            }
            msg_count_ = 0;
            last_emit_ = now;
        }
        writer_.flush();
    }

private:
    static constexpr double kCogMinSogKn = 0.5;   // COG published above this SOG
    double last_sog_kn_ = 0.0;
    std::string battery_id_ = usv::env_or("SIGNALK_BATTERY", "");
    std::string fuel_tank_id_ = usv::env_or("SIGNALK_FUEL_TANK", "");
    double current_sign_ = usv::env_or("SIGNALK_BATTERY_CURRENT_SIGN", "1") == "-1" ? -1.0 : 1.0;
    bool have_charging_mode_ = false;
    const char* last_charge_state_ = "";
    double last_charge_t_ = -1e18;
    std::set<std::string> unknown_engines_;
    std::set<int> seen_notifications_;
    usv::sk::EngineNotifications notifications_;
    double batt_v_ = 0.0, batt_v_t_ = -1e18;
    bool have_system_voltage_ = false, have_total_power_ = false;

    static double mono_s() {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }

    // electrical.batteries.<id>.<field> for one battery instance: the one
    // named in SIGNALK_BATTERY, else the first instance seen. Returns true
    // when the path was a battery path (handled or deliberately ignored).
    bool handle_battery(const std::string& path, const json& value) {
        static const std::string kPrefix = "electrical.batteries.";
        if (path.compare(0, kPrefix.size(), kPrefix) != 0) return false;
        const size_t dot = path.find('.', kPrefix.size());
        if (dot == std::string::npos) return true;
        const std::string id = path.substr(kPrefix.size(), dot - kPrefix.size());
        const std::string field = path.substr(dot + 1);
        if (battery_id_.empty()) {
            battery_id_ = id;
            std::cerr << "signalk_bridge: charting battery '" << id
                      << "' (set SIGNALK_BATTERY to choose another)\n";
        }
        if (id != battery_id_) return true;

        if (field == "chargingMode" && value.is_string()) {
            have_charging_mode_ = true;
            writer_.str_field("electrical", "", "charging_status",
                              value.get<std::string>());
            return true;
        }
        if (!value.is_number()) return true;
        double v = value.get<double>();
        // Signal K current is positive into the battery. A shunt wired or
        // configured the other way round is corrected with
        // SIGNALK_BATTERY_CURRENT_SIGN=-1 (current and the derived power).
        if (field == "current") v *= current_sign_;
        if (field == "voltage") {
            writer_.field("electrical", "", "battery_voltage", v);
            // No separate DC bus sensor: the battery is the system voltage.
            if (!have_system_voltage_)
                writer_.field("power", "", "system_voltage", v);
            batt_v_ = v; batt_v_t_ = mono_s();
        } else if (field == "current") {
            writer_.field("electrical", "", "battery_current", v);
            // A battery monitor (Victron SmartShunt / BMV over VE.Direct)
            // publishes no chargingMode; the state follows the current:
            // above +0.5 A charging, below -0.5 A discharging, else idle.
            // Written on change and every 5 s.
            if (!have_charging_mode_) {
                const char* st = v > 0.5 ? "charging" : (v < -0.5 ? "discharging" : "idle");
                const double now = mono_s();
                if (st != last_charge_state_ || now - last_charge_t_ >= 5.0) {
                    writer_.str_field("electrical", "", "charging_status", st);
                    last_charge_state_ = st;
                    last_charge_t_ = now;
                }
            }
            // Signal K current is positive when charging; the power drawn
            // from the battery (V x -I) stands in for total load power when
            // no electrical.loads.total.power is published.
            if (!have_total_power_ && mono_s() - batt_v_t_ < 10.0)
                writer_.field("power", "", "total_power", batt_v_ * -v);
        } else if (field == "capacity.stateOfCharge") {
            writer_.field("electrical", "", "soc", v * 100.0);
        }
        return true;
    }

    // propulsion.<id>.<rest>: every engine Signal K publishes, keyed by the
    // N2K engine instance or the name the server was given (0 / port ->
    // instance 0, 1 / starboard -> instance 1). Returns true when the path
    // was an engine path (written, or deliberately ignored on this vessel).
    bool handle_propulsion(const std::string& path, const json& value) {
        static const std::string kPrefix = "propulsion.";
        if (path.compare(0, kPrefix.size(), kPrefix) != 0) return false;
        if (!profile_.signalk_engine) return true;   // another bridge owns the engine
        const size_t dot = path.find('.', kPrefix.size());
        if (dot == std::string::npos) return true;
        const std::string id = path.substr(kPrefix.size(), dot - kPrefix.size());
        const std::string rest = path.substr(dot + 1);
        const int inst = usv::sk::EngineInstance(id);
        if (inst < 0) {
            if (unknown_engines_.insert(id).second)
                std::cerr << "signalk_bridge: ignoring propulsion." << id
                          << " (not an engine instance this release charts)\n";
            return true;
        }
        const std::string tags = profile_.Tags("instance=" + std::to_string(inst));
        if (rest == "transmission.gear") {
            double g;
            if (value.is_string() && usv::sk::GearValue(value.get<std::string>(), &g))
                writer_.field("engine", tags, "gear", g);
            return true;
        }
        if (!value.is_number()) return true;
        usv::sk::EngineField f;
        if (!usv::sk::MapPropulsion(rest, value.get<double>(), &f)) return true;
        // alternator_voltage lives in the electrical measurement (one
        // alternator charted, as electrical.alternators.0.voltage).
        writer_.field(f.measurement, std::strcmp(f.measurement, "engine") == 0 ? tags : "",
                      f.field, f.value);
        return true;
    }

    // tanks.fuel.<id>.currentLevel (ratio) -> engine.fuel_level (%), for the
    // tank named in SIGNALK_FUEL_TANK or the first fuel tank seen.
    bool handle_tank(const std::string& path, const json& value) {
        static const std::string kPrefix = "tanks.fuel.";
        if (path.compare(0, kPrefix.size(), kPrefix) != 0) return false;
        if (!profile_.signalk_engine) return true;
        const size_t dot = path.find('.', kPrefix.size());
        if (dot == std::string::npos) return true;
        const std::string id = path.substr(kPrefix.size(), dot - kPrefix.size());
        const std::string field = path.substr(dot + 1);
        if (field != "currentLevel" || !value.is_number()) return true;
        if (fuel_tank_id_.empty()) {
            fuel_tank_id_ = id;
            std::cerr << "signalk_bridge: fuel level from tank '" << id
                      << "' (set SIGNALK_FUEL_TANK to choose another)\n";
        }
        if (id != fuel_tank_id_) return true;
        writer_.field("engine", profile_.Tags("instance=0"), "fuel_level",
                      value.get<double>() * 100.0);
        return true;
    }

    // notifications.propulsion.<id>.<name>: value {state, message} or null.
    // Every change rewrites the instance's active count, fault text and
    // lamps, so the Engine page follows the server's own alarm state.
    bool handle_notification(const std::string& path, const json& value) {
        static const std::string kPrefix = "notifications.propulsion.";
        if (path.compare(0, kPrefix.size(), kPrefix) != 0) return false;
        if (!profile_.signalk_engine) return true;
        const size_t dot = path.find('.', kPrefix.size());
        if (dot == std::string::npos) return true;
        const int inst = usv::sk::EngineInstance(path.substr(kPrefix.size(), dot - kPrefix.size()));
        if (inst < 0) return true;
        const std::string name = path.substr(dot + 1);
        std::string state, message;
        if (value.is_object()) {
            if (value.contains("state") && value["state"].is_string())
                state = value["state"].get<std::string>();
            if (value.contains("message") && value["message"].is_string())
                message = value["message"].get<std::string>();
        }
        const bool first = seen_notifications_.insert(inst).second;
        if (!notifications_.Update(inst, name, state, message) && !first) return true;
        const std::string tags = profile_.Tags("instance=" + std::to_string(inst));
        const usv::sk::Lamps l = notifications_.LampsOf(inst);
        writer_.field("engine", tags, "active_dtcs", notifications_.Active(inst));
        writer_.str_field("engine", tags, "dtc_text", notifications_.Text(inst));
        writer_.field("engine", tags, "lamp_red", l.red ? 1 : 0);
        writer_.field("engine", tags, "lamp_mil", l.mil ? 1 : 0);
        writer_.field("engine", tags, "lamp_amber", l.amber ? 1 : 0);
        return true;
    }

    void handle_value(const std::string& path, const json& value) {
        if (path == "electrical.system.voltage") have_system_voltage_ = true;
        if (path == "electrical.loads.total.power") have_total_power_ = true;
        if (handle_battery(path, value)) return;
        if (handle_propulsion(path, value)) return;
        if (handle_tank(path, value)) return;
        if (handle_notification(path, value)) return;
        if (path == "navigation.position" && value.is_object()) {
            if (value.contains("latitude"))
                writer_.field("nav", "", "lat", value["latitude"].get<double>());
            if (value.contains("longitude"))
                writer_.field("nav", "", "lon", value["longitude"].get<double>());
        } else if (path == "navigation.attitude" && value.is_object()) {
            if (value.contains("roll"))
                writer_.field("nav", "", "roll", value["roll"].get<double>() * RAD);
            if (value.contains("pitch"))
                writer_.field("nav", "", "pitch", value["pitch"].get<double>() * RAD);
            if (value.contains("yaw"))
                writer_.field("nav", "", "yaw", value["yaw"].get<double>() * RAD);
        } else if (path == "navigation.gnss.type" && value.is_string()) {
            writer_.str_field("nav", "", "fix_type", value.get<std::string>());
        } else if (path == "navigation.speedOverGround" && value.is_number()) {
            last_sog_kn_ = value.get<double>() * MS_TO_KN;
            writer_.field("nav", "", "sog", last_sog_kn_);
        } else if (path == "navigation.courseOverGroundTrue" && value.is_number()) {
            // COG is undefined at rest: an INS derives it from near-zero
            // velocities and it swings through 360 deg alongside. Written
            // only under way so the compass needle and trend show a real
            // course; the heading tiles are unaffected.
            if (last_sog_kn_ >= kCogMinSogKn)
                writer_.field("nav", "", "cog", value.get<double>() * RAD);
        } else if (path == "navigation.vectornav.insMode" && value.is_number()) {
            // VectorNav INS (signalk-vectornav-vnins plugin): 0 not tracking,
            // 1 aligning, 2 tracking, 3 GNSS lost (dead reckoning). Mapped to
            // the fix_type string the Readiness GPS check and the Navigation
            // "GPS Fix" tile read; only "INS tracking" counts as a fix.
            static const char* kInsMode[] = {"no fix", "INS aligning",
                                             "INS tracking", "GNSS lost"};
            const int mode = value.get<int>();
            writer_.field("nav", "", "ins_mode", mode);
            if (mode >= 0 && mode <= 3)
                writer_.str_field("nav", "", "fix_type", kInsMode[mode]);
        } else {
            auto it = kPaths.find(path);
            if (it == kPaths.end() || !value.is_number()) return;
            writer_.field(it->second.measurement, "", it->second.field,
                          it->second.convert(value.get<double>()));
        }
    }

public:
    const usv::EngineProfile& profile() const { return profile_; }

private:
    std::mutex mu_;
    usv::EngineProfile profile_ = usv::EngineProfile::FromEnv();
    usv::InfluxWriter writer_{"telemetry"};
    uint64_t msg_count_ = 0;
    std::string self_;            // own vessel context from the hello message
    uint64_t other_count_ = 0;    // deltas for other vessels, skipped
    bool other_logged_ = false;
    double last_emit_ = 0.0;
    bool up_ = false;
};

}  // namespace

int main() {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    ix::initNetSystem();

    const std::string url = usv::env_or(
        "SIGNALK_URL", "ws://localhost:3000/signalk/v1/stream?subscribe=self");

    Bridge bridge;
    std::cerr << "signalk_bridge: " << bridge.profile().Describe()
              << (bridge.profile().signalk_engine
                      ? "\n" : " (engine paths ignored on this vessel)\n");
    ix::WebSocket ws;
    ws.setUrl(url);
    ws.setPingInterval(20);
    // IXWebSocket reconnects automatically with capped backoff
    ws.setMaxWaitBetweenReconnectionRetries(10000);

    ws.setOnMessageCallback([&](const ix::WebSocketMessagePtr& msg) {
        switch (msg->type) {
            case ix::WebSocketMessageType::Message: {
                json delta = json::parse(msg->str, nullptr, false);
                if (!delta.is_discarded()) bridge.handle_delta(delta);
                break;
            }
            case ix::WebSocketMessageType::Open:
                std::cerr << "signal k connected: " << url << "\n";
                bridge.set_up(true);
                break;
            case ix::WebSocketMessageType::Close:
            case ix::WebSocketMessageType::Error:
                std::cerr << "signal k disconnected — retrying\n";
                bridge.set_up(false);
                break;
            default:
                break;
        }
    });
    ws.start();

    while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        bridge.tick();
    }
    ws.stop();
    ix::uninitNetSystem();
    return 0;
}
