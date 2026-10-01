// Signal K -> InfluxDB bridge (FR-2 navigation, FR-4 vessel health, FR-8).
//
// Connects to the Signal K server's WebSocket delta stream and maps Signal K
// paths to InfluxDB measurements/fields. Signal K units are SI (m/s, rad, K,
// Pa, ratios); conversion to display units (kn, deg, °C, kPa, %) happens
// here so dashboards stay unit-simple.
//
// Env: SIGNALK_URL, INFLUX_URL, INFLUX_TOKEN, INFLUX_ORG, INFLUX_BUCKET,
//      VESSEL_ID
// Reconnection: IXWebSocket automatic backoff + status heartbeat.

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "../cpp-common/influx_writer.hpp"

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
    // server holds (Freeboard manual planning) — NOT the autonomy route.
    // Kept as its own field; mission.dist_to_waypoint has exactly one
    // producer (grafana_bridge, from DIST_TO_NEXT_WPT).
    {"navigation.courseRhumbline.nextPoint.distance",
     {"nav", "course_next_point", [](double v) { return v; }}},
    // Engine (FR-4), instance 0
    {"propulsion.0.revolutions",
     {"engine", "rpm", [](double v) { return v * 60.0; }}},          // Hz->RPM
    {"propulsion.0.coolantTemperature",
     {"engine", "coolant_temp", [](double v) { return v - 273.15; }}},
    {"propulsion.0.oilPressure",
     {"engine", "oil_pressure", [](double v) { return v / 1000.0; }}},  // Pa->kPa
    {"propulsion.0.fuel.level",
     {"engine", "fuel_level", [](double v) { return v * 100.0; }}},
    {"propulsion.0.fuel.rate",
     {"engine", "fuel_rate", [](double v) { return v * 3.6e6; }}},   // m3/s->L/h
    {"propulsion.0.runTime",
     {"engine", "runtime_h", [](double v) { return v / 3600.0; }}},
    // Electrical (FR-4)
    {"electrical.batteries.0.voltage",
     {"electrical", "battery_voltage", [](double v) { return v; }}},
    {"electrical.batteries.0.current",
     {"electrical", "battery_current", [](double v) { return v; }}},
    {"electrical.batteries.0.capacity.stateOfCharge",
     {"electrical", "soc", [](double v) { return v * 100.0; }}},
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
        if (!delta.contains("updates")) return;
        for (const auto& update : delta["updates"]) {
            if (!update.contains("values")) continue;
            for (const auto& item : update["values"]) {
                if (!item.contains("path") || !item.contains("value")) continue;
                handle_value(item["path"].get<std::string>(), item["value"]);
            }
        }
        ++msg_count_;
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
            msg_count_ = 0;
            last_emit_ = now;
        }
        writer_.flush();
    }

private:
    static constexpr double kCogMinSogKn = 0.5;   // COG published above this SOG
    double last_sog_kn_ = 0.0;

    static double mono_s() {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }

    void handle_value(const std::string& path, const json& value) {
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
        } else if (path == "electrical.batteries.0.chargingMode"
                   && value.is_string()) {
            writer_.str_field("electrical", "", "charging_status",
                              value.get<std::string>());
        } else {
            auto it = kPaths.find(path);
            if (it == kPaths.end() || !value.is_number()) return;
            writer_.field(it->second.measurement, "", it->second.field,
                          it->second.convert(value.get<double>()));
        }
    }

    std::mutex mu_;
    usv::InfluxWriter writer_{"telemetry"};
    uint64_t msg_count_ = 0;
    double last_emit_ = 0.0;
    bool up_ = false;
};

}  // namespace

int main() {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    ix::initNetSystem();

    const std::string url = usv::env_or(
        "SIGNALK_URL", "ws://localhost:3000/signalk/v1/stream?subscribe=all");

    Bridge bridge;
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
