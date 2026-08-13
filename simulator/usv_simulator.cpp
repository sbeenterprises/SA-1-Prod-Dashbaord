// USV telemetry simulator (C++). Bench testing only — generates a plausible
// full telemetry stream covering every measurement the dashboards query,
// so the stack can be demonstrated and validated without a vessel.
//
// Env: INFLUX_URL, INFLUX_TOKEN, INFLUX_ORG, INFLUX_BUCKET, VESSEL_ID

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "../ingestion/cpp-common/influx_writer.hpp"

namespace {

std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

constexpr double PI = 3.14159265358979323846;
constexpr double D2R = PI / 180.0;

struct Wp { double lat, lon; };
const std::vector<Wp> kWaypoints = {
    {29.60, -91.20}, {29.62, -91.18}, {29.64, -91.21},
    {29.63, -91.24}, {29.61, -91.23}, {29.60, -91.20},
};

double bearing_deg(const Wp& a, const Wp& b) {
    double lat1 = a.lat * D2R, lon1 = a.lon * D2R;
    double lat2 = b.lat * D2R, lon2 = b.lon * D2R;
    double y = std::sin(lon2 - lon1) * std::cos(lat2);
    double x = std::cos(lat1) * std::sin(lat2)
             - std::sin(lat1) * std::cos(lat2) * std::cos(lon2 - lon1);
    return std::fmod(std::atan2(y, x) / D2R + 360.0, 360.0);
}

double dist_m(const Wp& a, const Wp& b) {
    double lat1 = a.lat * D2R, lon1 = a.lon * D2R;
    double lat2 = b.lat * D2R, lon2 = b.lon * D2R;
    double h = std::pow(std::sin((lat2 - lat1) / 2), 2)
             + std::cos(lat1) * std::cos(lat2)
             * std::pow(std::sin((lon2 - lon1) / 2), 2);
    return 6371000.0 * 2 * std::asin(std::sqrt(h));
}

}  // namespace

int main() {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    usv::InfluxWriter tel("telemetry");
    usv::InfluxWriter ev("events");

    std::mt19937 rng(42);
    auto gauss = [&](double mean, double sd) {
        return std::normal_distribution<double>(mean, sd)(rng);
    };
    auto uni = [&](double lo, double hi) {
        return std::uniform_real_distribution<double>(lo, hi)(rng);
    };

    auto emit_event = [&](const std::string& msg,
                          const std::string& sev = "info",
                          const std::string& type = "state_change") {
        ev.str_field("events", "type=" + type + ",severity=" + sev,
                     "message", msg);
        ev.field("events", "type=" + type + ",severity=" + sev, "value", 1);
        ev.flush();
    };

    double lat = kWaypoints[0].lat, lon = kWaypoints[0].lon;
    size_t wpt_i = 1;
    double heading = 0, speed = 0, fuel = 92.0, soc = 88.0, dist_total = 0;
    const std::string state = "Autonomous";
    auto t0 = std::chrono::steady_clock::now();
    uint64_t tick = 0;

    emit_event("Mission DELTA-RUN-042 started, state=" + state, "info",
               "mission");

    while (!g_stop) {
        ++tick;
        const Wp here{lat, lon};
        const Wp& target = kWaypoints[wpt_i];
        double d = dist_m(here, target);
        double brg = bearing_deg(here, target);

        double err = std::fmod(brg - heading + 540.0, 360.0) - 180.0;
        heading = std::fmod(heading
                            + std::clamp(err * 0.4, -6.0, 6.0)
                            + gauss(0, 0.4) + 360.0, 360.0);
        double desired_speed = 4.5;
        speed += (desired_speed - speed) * 0.1 + gauss(0, 0.05);
        speed = std::max(0.0, speed);

        double step = speed * 0.514444;             // kn -> m per 1 s tick
        dist_total += step;
        lat += step * std::cos(heading * D2R) / 111320.0;
        lon += step * std::sin(heading * D2R)
             / (111320.0 * std::cos(lat * D2R));

        if (d < 25.0) {
            emit_event("Waypoint " + std::to_string(wpt_i) + " reached",
                       "info", "waypoint");
            if (++wpt_i >= kWaypoints.size()) {
                emit_event("Mission completed", "info", "mission");
                wpt_i = 1;
                dist_total = 0;
                t0 = std::chrono::steady_clock::now();
            }
        }

        double rudder = std::clamp(err * 0.8 + gauss(0, 1), -30.0, 30.0);
        double throttle = std::clamp(speed / 6.0 * 100 + gauss(0, 2), 0.0, 100.0);
        double rpm = 700 + throttle * 32 + gauss(0, 25);
        fuel = std::max(2.0, fuel - 0.0004 * throttle / 100);
        soc = std::clamp(soc - 0.001 + (rpm > 1500 ? 0.003 : 0.0), 20.0, 100.0);
        double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t0).count();

        // nav
        tel.field("nav", "", "lat", lat);
        tel.field("nav", "", "lon", lon);
        tel.field("nav", "", "heading", heading);
        tel.field("nav", "", "cog", std::fmod(heading + gauss(0, 2) + 360, 360));
        tel.field("nav", "", "sog", speed);
        tel.field("nav", "", "roll", gauss(0, 2.5));
        tel.field("nav", "", "pitch", gauss(1, 1.5));
        tel.field("nav", "", "yaw", heading);
        tel.field("nav", "", "satellites", std::floor(uni(9, 15)));
        tel.field("nav", "", "hdop", uni(0.7, 1.4));
        tel.str_field("nav", "", "fix_type",
                      uni(0, 1) > 0.05 ? "RTK Fixed" : "3D");
        tel.field("nav", "", "cross_track_error", gauss(0, 3.0));
        // mission
        double nwp = static_cast<double>(kWaypoints.size() - 1);
        tel.field("mission", "", "progress_pct",
                  100.0 * (wpt_i - 1 + std::max(0.0, 1 - d / 2000.0)) / nwp);
        tel.field("mission", "", "waypoint_index", static_cast<double>(wpt_i));
        tel.field("mission", "", "waypoints_total", nwp);
        tel.field("mission", "", "dist_to_waypoint", d);
        tel.field("mission", "", "eta_s", d / std::max(0.1, speed * 0.514444));
        tel.field("mission", "", "distance_traveled", dist_total);
        tel.field("mission", "", "elapsed_s", elapsed);
        // mission_state
        tel.str_field("mission_state", "", "mission_name", "DELTA-RUN-042");
        tel.str_field("mission_state", "", "mission_id", "M-2026-042");
        tel.str_field("mission_state", "", "state", state);
        tel.str_field("mission_state", "", "active_behavior", "waypt_survey");
        tel.str_field("mission_state", "", "station_keep", "false");
        // autopilot
        tel.field("autopilot", "", "desired_heading", brg);
        tel.field("autopilot", "", "actual_heading", heading);
        tel.field("autopilot", "", "desired_speed", desired_speed);
        tel.field("autopilot", "", "actual_speed", speed);
        tel.field("autopilot", "", "rudder_cmd", rudder);
        tel.field("autopilot", "", "throttle_cmd", throttle);
        tel.field("autopilot", "", "heading_error", err);
        tel.field("autopilot", "", "speed_error", desired_speed - speed);
        tel.field("autopilot", "", "pid_heading_out", rudder / 30.0);
        tel.field("autopilot", "", "pid_speed_out", throttle / 100.0);
        // engine
        tel.field("engine", "", "rpm", rpm);
        tel.field("engine", "", "coolant_temp", 78 + throttle * 0.12 + gauss(0, 1));
        tel.field("engine", "", "oil_pressure", 320 + rpm * 0.03 + gauss(0, 5));
        tel.field("engine", "", "fuel_level", fuel);
        tel.field("engine", "", "fuel_rate", throttle * 0.22 + gauss(0, 0.2));
        tel.field("engine", "", "runtime_h", 512.4 + elapsed / 3600.0);
        // electrical
        tel.field("electrical", "", "battery_voltage", 12.9 + gauss(0, 0.06));
        tel.field("electrical", "", "battery_current",
                  -8 + (rpm > 1500 ? 14 : 0) + gauss(0, 1));
        tel.field("electrical", "", "soc", soc);
        tel.str_field("electrical", "", "charging_status",
                      rpm > 1500 ? "charging" : "discharging");
        tel.field("electrical", "", "alternator_voltage",
                  rpm > 1000 ? 14.2 : 0.0);
        // power
        tel.field("power", "", "total_power", 240 + gauss(0, 20));
        tel.field("power", "", "system_voltage", 12.9 + gauss(0, 0.05));
        tel.field("power", "", "distribution_ok", 1);
        // canbus
        tel.field("canbus", "", "msg_rate", 380 + gauss(0, 25));
        tel.field("canbus", "", "utilization_pct", 34 + gauss(0, 3));
        tel.field("canbus", "", "error_frames", uni(0, 1) < 0.02 ? 1 : 0);
        tel.field("canbus", "", "dropped_frames", 0);
        tel.field("canbus", "", "active_pgns", 27);
        tel.field("canbus", "", "interface_up", 1);
        // radar
        tel.field("radar", "", "online", 1);
        tel.field("radar", "", "targets", std::floor(uni(0, 7)));
        tel.field("radar", "", "closest_range_m", uni(200, 3000));
        tel.field("radar", "", "cpa_m", uni(150, 2500));
        tel.field("radar", "", "tcpa_s", uni(60, 900));
        tel.field("radar", "", "detection_rate", 0.97 + gauss(0, 0.01));
        // camera
        tel.field("camera", "cam=bow", "connected", 1);
        tel.field("camera", "cam=bow", "stream_up", 1);
        tel.field("camera", "cam=bow", "fps", 29.5 + gauss(0, 0.5));
        tel.field("camera", "cam=bow", "bitrate_kbps", 4200 + gauss(0, 150));
        tel.field("camera", "cam=bow", "recording", 1);
        tel.field("camera", "cam=bow", "temp_c", 41 + gauss(0, 1));
        tel.field("camera", "cam=bow", "ptz_pan", 0);
        tel.field("camera", "cam=bow", "ptz_tilt", -5);
        // comms
        tel.field("comms", "", "internet_up", 1);
        tel.field("comms", "", "starlink_up", 1);
        tel.field("comms", "", "vpn_up", 1);
        tel.field("comms", "", "signalk_ws_up", 1);
        tel.field("comms", "", "latency_ms", 45 + gauss(0, 8));
        tel.field("comms", "", "packet_loss_pct", std::max(0.0, gauss(0.3, 0.3)));
        tel.field("comms", "", "up_mbps", 8 + gauss(0, 1));
        tel.field("comms", "", "down_mbps", 60 + gauss(0, 8));
        tel.field("comms", "", "moos_msg_rate", 120 + gauss(0, 10));
        tel.field("comms", "", "moos_connected", 1);
        tel.field("comms", "", "signalk_msg_rate", 85 + gauss(0, 8));
        tel.flush();

        if (tick % 300 == 0)
            emit_event("GPS fix quality degraded to 3D", "warning", "alarm");
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return 0;
}
