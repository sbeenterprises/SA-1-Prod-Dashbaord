// USV telemetry simulator (C++). Bench testing only — generates a plausible
// full telemetry stream covering every measurement the dashboards query,
// so the stack can be demonstrated and validated without a vessel.
//
// Plays a scripted ALPHA MISSION scenario on loop, mirroring alpha.bhv:
//   Parked -> mission start OUTSIDE the region (entry gate closed,
//   EnteringRegion) -> survey with the distance-tiered speed governor
//   (full speed > 500 m, tier-1 to 500 m, tier-2 inside 100 m,
//   capture at 20 m) -> operator PAUSE -> mid-route station-keep ->
//   obstacle-avoidance spawn -> operator-link loss (failsafe engaged,
//   mission disarmed) -> link restored (survey HELD awaiting re-arm) ->
//   operator re-arm -> geofence excursion + recovery -> survey complete ->
//   parked -> return-to-base -> parked at base -> next mission.
//
// The mission state string is derived with the SAME mission_logic.hpp the
// real grafana_bridge uses, so the simulator doubles as an end-to-end
// validation of the state machine. NOTE: the scenario intentionally fires
// the geofence / operator-link / comms alerts once per cycle — that is the
// bench test for FR-11. Stop the simulator before real operations.
//
// Env: INFLUX_URL, INFLUX_TOKEN, INFLUX_ORG, INFLUX_BUCKET, VESSEL_ID

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "../ingestion/cpp-common/influx_writer.hpp"
#include "../ingestion/moos-cpp/mission_logic.hpp"

namespace {

std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

constexpr double PI = 3.14159265358979323846;
constexpr double D2R = PI / 180.0;
constexpr double KN_TO_MS = 0.514444;

struct Wp { double lat, lon; };
// Survey pattern off Morgan City; legs ~700–900 m so every governor tier
// (>500 / 100–500 / <100 m) is exercised on each leg. Index 0 = base.
const std::vector<Wp> kWaypoints = {
    {29.6000, -91.2000},   // base / start
    {29.6072, -91.1960},
    {29.6118, -91.2035},
    {29.6060, -91.2110},
    {29.5988, -91.2075},
    {29.6000, -91.2000},   // close the loop at base
};

// Governor tiers (sim speeds in knots).
constexpr double kCruiseKn = 16.0;   // "full UI speed" beyond 500 m
constexpr double kTier1Kn = 9.0;     // inside 500 m
constexpr double kTier2Kn = 4.0;     // inside 100 m
constexpr double kEntryKn = 6.0;     // opr24 recovery speed
constexpr double kReturnKn = 12.0;
constexpr double kCaptureM = 20.0;   // capture_radius

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

double governor_speed(double dist) {
    if (dist > 500.0) return kCruiseKn;
    if (dist > 100.0) return kTier1Kn;
    return kTier2Kn;
}

// Scenario interludes, scheduled by seconds since mission start. Windows
// are placed so the survey (~12 min over 5 legs) contains all of them.
struct Scenario {
    static constexpr double kEntryUntil = 30;         // gate closed
    static constexpr double kPauseFrom = 150, kPauseTo = 180;
    static constexpr double kStationFrom = 240, kStationTo = 290;
    static constexpr double kObstacleFrom = 330, kObstacleTo = 390;
    static constexpr double kCommsFrom = 450, kCommsTo = 495;
    static constexpr double kRearmAt = 525;           // held 495..525
    static constexpr double kGeofenceFrom = 585;      // breach 10 s,
    static constexpr double kGeofenceRecTo = 610;     // recovering to 610
};

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
        const std::string tags = "severity=" + sev + ",source=simulator" +
                                 ",type=" + type;
        ev.str_field("events", tags, "message", msg);
        ev.field("events", tags, "value", 1);
        ev.flush();
    };

    // Vessel physical state.
    double lat = kWaypoints[0].lat, lon = kWaypoints[0].lon;
    double heading = 45.0, speed = 0.0;           // kn
    double fuel = 92.0, soc = 88.0;
    double dist_total = 0.0;

    // Mission state (same struct + derivation as grafana_bridge).
    usv::MissionFlags flags;
    flags.manual_override = true;                  // cold start: parked
    std::string last_state;
    std::string helm_state = "PARK", allstop = "ManualOverride";
    std::string active_bhvs = "none";
    int geofence_level = 0, obstacle_alerts = 0;
    bool comms_link_up = true;

    // Phase bookkeeping.
    enum Phase { PARKED_PRE, SURVEY, COMPLETED_HOLD, RETURN, PARKED_POST };
    Phase phase = PARKED_PRE;
    double phase_started = 0.0;                    // tick of phase entry
    double mission_elapsed = -1.0;                 // -1 = no mission yet
    size_t wpt_i = 1;
    int mission_no = 42;
    int last_reported_gf = 0;
    bool obstacle_evented = false;
    const int wpt_total = static_cast<int>(kWaypoints.size()) - 1;

    uint64_t tick = 0;

    while (!g_stop) {
        ++tick;
        const double t = static_cast<double>(tick);
        const double me = mission_elapsed < 0 ? -1 : t - mission_elapsed;

        // ----------------------------------------------- phase transitions
        switch (phase) {
            case PARKED_PRE:
                if (t - phase_started >= 15) {
                    // Operator starts the mission with the boat OUTSIDE the
                    // region: gate closed, opr24 recovers inside first.
                    phase = SURVEY;
                    phase_started = t;
                    mission_elapsed = t;
                    wpt_i = 1;
                    dist_total = 0;
                    flags = usv::MissionFlags{};
                    flags.deploy = flags.waypoint = true;
                    flags.entry_gate = "closed";
                    helm_state = "DRIVE";
                    allstop = "clear";
                    char m[96];
                    std::snprintf(m, sizeof(m),
                                  "Survey started (%d waypoints) — vessel "
                                  "outside region, recovering inside first",
                                  wpt_total);
                    emit_event(m, "info", "mission");
                }
                break;
            case SURVEY:
                break;  // interludes + capture logic below
            case COMPLETED_HOLD:
                if (t - phase_started >= 45) {
                    phase = RETURN;
                    phase_started = t;
                    flags.manual_override = false;
                    flags.returning = true;
                    helm_state = "DRIVE";
                    allstop = "clear";
                    wpt_i = 0;  // head for base
                    emit_event("Return-to-base started", "info", "mission");
                }
                break;
            case RETURN:
                break;
            case PARKED_POST:
                if (t - phase_started >= 30) {
                    phase = PARKED_PRE;
                    phase_started = t;
                    mission_elapsed = -1;
                    ++mission_no;
                    flags = usv::MissionFlags{};
                    flags.manual_override = true;
                }
                break;
        }

        // -------------------------------------- scripted survey interludes
        double target_speed = 0.0;
        const Wp here{lat, lon};
        const Wp& target = kWaypoints[std::min(wpt_i, kWaypoints.size() - 1)];
        double d = dist_m(here, target);

        if (phase == SURVEY) {
            const double m = me;
            // Entry gate latch (start-outside recovery).
            if (!usv::gate_open(flags.entry_gate) && m >= Scenario::kEntryUntil) {
                flags.entry_gate = "open";
                emit_event("Vessel entered operating region — survey "
                           "enabled (entry gate latched open)",
                           "info", "mission");
            }
            // Pause window.
            const bool want_pause =
                m >= Scenario::kPauseFrom && m < Scenario::kPauseTo;
            if (want_pause != flags.pause) flags.pause = want_pause;
            // Mid-route station-keep window (the comms-loss failsafe owns
            // the STATIONING flag while the link is down).
            const bool want_station =
                m >= Scenario::kStationFrom && m < Scenario::kStationTo;
            if (comms_link_up) flags.stationing = want_station;
            // Obstacle-avoidance window.
            if (m >= Scenario::kObstacleFrom && m < Scenario::kObstacleTo) {
                obstacle_alerts = 1;
                if (!obstacle_evented) {
                    obstacle_evented = true;
                    emit_event("Obstacle avoidance active: ob_enc_12",
                               "warning", "obstacle");
                }
            } else {
                obstacle_alerts = 0;
                if (m >= Scenario::kObstacleTo) obstacle_evented = false;
            }
            // Operator-link loss -> failsafe -> explicit re-arm.
            if (m >= Scenario::kCommsFrom && m < Scenario::kCommsTo) {
                if (comms_link_up) {
                    comms_link_up = false;
                    flags.comms_lost = "loiter";
                    flags.armed = false;
                    flags.stationing = true;   // planchaPID holds position
                    emit_event("Operator link LOST — failsafe 'loiter' "
                               "engaged, mission disarmed",
                               "critical", "alarm");
                }
            } else if (!comms_link_up && m >= Scenario::kCommsTo) {
                comms_link_up = true;
                flags.comms_lost = "false";
                flags.stationing = false;
                emit_event("Operator link restored — survey held until "
                           "operator re-arm", "info", "alarm");
            }
            if (!flags.armed && m >= Scenario::kRearmAt) {
                flags.armed = true;
                emit_event("Mission re-armed by operator "
                           "(AUTONOMOUS_RESUME)", "info", "mission");
            }
            // Geofence excursion at a boundary-adjacent waypoint.
            if (m >= Scenario::kGeofenceFrom && m < Scenario::kGeofenceFrom + 10)
                geofence_level = 1;
            else if (m >= Scenario::kGeofenceFrom + 10 &&
                     m < Scenario::kGeofenceRecTo)
                geofence_level = 2;
            else
                geofence_level = 0;

            // Speed demand.
            const bool holding = flags.pause || flags.stationing ||
                                 usv::comms_lost_active(flags.comms_lost) ||
                                 !flags.armed;
            if (holding)
                target_speed = 0.0;
            else if (!usv::gate_open(flags.entry_gate))
                target_speed = kEntryKn;
            else
                target_speed = governor_speed(d);

            // Waypoint capture.
            if (!holding && d < kCaptureM) {
                if (wpt_i >= kWaypoints.size() - 1) {
                    // Final waypoint: survey complete, boat stops and parks.
                    flags.survey_complete = true;
                    flags.waypoint = false;
                    flags.deploy = false;
                    flags.manual_override = true;
                    helm_state = "PARK";
                    allstop = "ManualOverride";
                    phase = COMPLETED_HOLD;
                    phase_started = t;
                    emit_event("Survey complete — stopping at final "
                               "waypoint", "info", "mission");
                } else {
                    char msg[96];
                    std::snprintf(msg, sizeof(msg),
                                  "Waypoint %zu captured — heading to %zu "
                                  "of %d", wpt_i, wpt_i + 1, wpt_total);
                    emit_event(msg, "info", "waypoint");
                    ++wpt_i;
                }
            }
        } else if (phase == RETURN) {
            target_speed = std::min(kReturnKn, governor_speed(d));
            if (d < kCaptureM) {
                flags.returning = false;
                flags.manual_override = true;
                helm_state = "PARK";
                allstop = "ManualOverride";
                phase = PARKED_POST;
                phase_started = t;
                emit_event("Arrived at base — helm parked", "info",
                           "mission");
            }
        }

        // ---------------------------------------------------- physics step
        double brg = bearing_deg(here, target);
        double err = std::fmod(brg - heading + 540.0, 360.0) - 180.0;
        const bool driving = !flags.manual_override && target_speed > 0.01;
        if (driving)
            heading = std::fmod(heading + std::clamp(err * 0.4, -6.0, 6.0)
                                + gauss(0, 0.4) + 360.0, 360.0);
        // Accel/decel limited (coast decel ~0.25 m/s^2 ≈ 0.5 kn/s).
        speed += std::clamp(target_speed - speed, -0.5, 0.35) + gauss(0, 0.04);
        speed = std::max(0.0, speed);

        double step = speed * KN_TO_MS;             // m per 1 s tick
        if (driving || speed > 0.05) {
            dist_total += step;
            lat += step * std::cos(heading * D2R) / 111320.0;
            lon += step * std::sin(heading * D2R)
                 / (111320.0 * std::cos(lat * D2R));
        }
        d = dist_m(Wp{lat, lon}, target);

        // ------------------------------------------------- derived strings
        const std::string state = usv::DeriveMissionState(flags);
        if (state != last_state) {
            last_state = state;
            emit_event("Mission state: " + state, "info", "state_change");
        }
        if (geofence_level != last_reported_gf) {
            if (geofence_level == 1)
                emit_event("Geofence BREACHED — outside operating region",
                           "critical", "geofence");
            else if (geofence_level == 2)
                emit_event("Geofence recovery — steering back inside region",
                           "warning", "geofence");
            else
                emit_event("Geofence clear — inside region", "info",
                           "geofence");
            last_reported_gf = geofence_level;
        }

        const bool in_survey = phase == SURVEY;
        if (in_survey && flags.pause)
            active_bhvs = "pause_hold_speed, pause_hold_heading, opr24";
        else if (flags.stationing)
            active_bhvs = "station_keep, opr24";
        else if (in_survey && flags.waypoint && flags.armed &&
                 usv::gate_open(flags.entry_gate))
            active_bhvs = obstacle_alerts
                              ? "waypt_survey, opr24, avoid_obstacle_ob_enc_12"
                              : "waypt_survey, opr24";
        else if (in_survey)
            active_bhvs = "opr24";
        else if (phase == RETURN)
            active_bhvs = "waypt_return, opr24";
        else
            active_bhvs = "none";

        const bool mission_live = mission_elapsed > 0 && state != "Idle" &&
                                  state != "Completed" && state != "Parked";
        const double elapsed = mission_elapsed > 0 ? t - mission_elapsed : 0;
        const double rudder = driving
            ? std::clamp(err * 0.8 + gauss(0, 1), -30.0, 30.0) : 0.0;
        const double throttle = std::clamp(
            speed / kCruiseKn * 100 + gauss(0, 2), 0.0, 100.0);
        const double rpm = flags.manual_override
            ? 650 + gauss(0, 10) : 700 + throttle * 32 + gauss(0, 25);
        fuel = std::max(2.0, fuel - 0.0004 * throttle / 100);
        soc = std::clamp(soc - 0.001 + (rpm > 1500 ? 0.003 : 0.0),
                         20.0, 100.0);

        // ------------------------------------------------------ telemetry
        // nav
        tel.field("nav", "", "lat", lat);
        tel.field("nav", "", "lon", lon);
        tel.field("nav", "", "x", (lon - kWaypoints[0].lon) * 111320.0
                                  * std::cos(lat * D2R));
        tel.field("nav", "", "y", (lat - kWaypoints[0].lat) * 111320.0);
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
        tel.field("nav", "", "cross_track_error",
                  driving ? gauss(0, 3.0) : 0.0);
        // mission
        const double dist_next = (flags.waypoint || flags.returning) ? d : 0;
        tel.field("mission", "", "progress_pct",
                  flags.survey_complete
                      ? 100.0
                      : usv::ProgressPct(static_cast<double>(wpt_i) - 1.0,
                                         wpt_total));
        tel.field("mission", "", "waypoint_index",
                  static_cast<double>(wpt_i));
        tel.field("mission", "", "waypoints_total", wpt_total);
        tel.field("mission", "", "waypoints_remaining",
                  std::max(0, wpt_total - static_cast<int>(wpt_i)));
        tel.field("mission", "", "dist_to_waypoint", dist_next);
        tel.field("mission", "", "eta_s",
                  usv::EtaSeconds(dist_next, speed * KN_TO_MS));
        tel.field("mission", "", "distance_traveled", dist_total);
        tel.field("mission", "", "elapsed_s", mission_live ? elapsed : 0);
        // mission_state
        char mission_id[32];
        std::snprintf(mission_id, sizeof(mission_id), "M-2026-%03d",
                      mission_no);
        tel.str_field("mission_state", "", "mission_name", "ALPHA-SURVEY");
        tel.str_field("mission_state", "", "mission_id", mission_id);
        tel.str_field("mission_state", "", "state", state);
        tel.str_field("mission_state", "", "active_behavior", active_bhvs);
        tel.str_field("mission_state", "", "station_keep",
                      flags.stationing ? "true" : "false");
        tel.str_field("mission_state", "", "helm_state", helm_state);
        tel.str_field("mission_state", "", "allstop_reason", allstop);
        tel.str_field("mission_state", "", "comms_failsafe",
                      usv::comms_lost_active(flags.comms_lost)
                          ? flags.comms_lost : "ok");
        tel.str_field("mission_state", "", "entry_gate", flags.entry_gate);
        tel.str_field("mission_state", "", "opr_status",
                      geofence_level == 0 ? "inside" : "recovering");
        // autonomy (numeric 0/1 mirrors of the flags, as grafana_bridge)
        auto bf = [&](const char* f, bool v) {
            tel.field("autonomy", "", f, v ? 1.0 : 0.0);
        };
        bf("deploy", flags.deploy);
        bf("waypoint", flags.waypoint);
        bf("returning", flags.returning);
        bf("stationing", flags.stationing);
        bf("pause", flags.pause);
        bf("armed", flags.armed);
        bf("comms_lost", usv::comms_lost_active(flags.comms_lost));
        bf("entry_gate_open", usv::gate_open(flags.entry_gate));
        bf("manual_override", flags.manual_override);
        bf("survey_complete", flags.survey_complete);
        bf("awaiting_rearm", usv::awaiting_rearm(flags));
        bf("helm_drive", helm_state == "DRIVE");
        bf("allstop", usv::allstop_unexpected(allstop));
        bf("remote_control", false);
        bf("crash_stop", false);
        tel.field("autonomy", "", "comms_phase",
                  usv::comms_phase(flags.comms_lost));
        tel.field("autonomy", "", "geofence_breached", geofence_level);
        tel.field("autonomy", "", "obstacle_alerts", obstacle_alerts);
        // autopilot
        tel.field("autopilot", "", "desired_heading", driving ? brg : heading);
        tel.field("autopilot", "", "actual_heading", heading);
        tel.field("autopilot", "", "desired_speed", target_speed);
        tel.field("autopilot", "", "actual_speed", speed);
        tel.field("autopilot", "", "rudder_cmd", rudder);
        tel.field("autopilot", "", "throttle_cmd", throttle);
        tel.field("autopilot", "", "heading_error", driving ? err : 0.0);
        tel.field("autopilot", "", "speed_error", target_speed - speed);
        tel.field("autopilot", "", "pid_heading_out", rudder / 30.0);
        tel.field("autopilot", "", "pid_speed_out", throttle / 100.0);
        // engine
        tel.field("engine", "", "rpm", rpm);
        tel.field("engine", "", "coolant_temp", 78 + throttle * 0.12 + gauss(0, 1));
        tel.field("engine", "", "oil_pressure", 320 + rpm * 0.03 + gauss(0, 5));
        tel.field("engine", "", "fuel_level", fuel);
        tel.field("engine", "", "fuel_rate", throttle * 0.22 + gauss(0, 0.2));
        tel.field("engine", "", "runtime_h", 512.4 + t / 3600.0);
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
        // comms — the shore link drops during the failsafe window; the
        // onboard MOOS/Signal K stacks stay alive (telemetry is recorded
        // locally and the dashboards catch up when the link returns).
        tel.field("comms", "", "internet_up", comms_link_up ? 1 : 0);
        tel.field("comms", "", "starlink_up", comms_link_up ? 1 : 0);
        tel.field("comms", "", "vpn_up", comms_link_up ? 1 : 0);
        tel.field("comms", "", "signalk_ws_up", 1);
        tel.field("comms", "", "latency_ms",
                  comms_link_up ? 45 + gauss(0, 8) : 0);
        tel.field("comms", "", "packet_loss_pct",
                  comms_link_up ? std::max(0.0, gauss(0.3, 0.3)) : 100.0);
        tel.field("comms", "", "up_mbps", comms_link_up ? 8 + gauss(0, 1) : 0);
        tel.field("comms", "", "down_mbps",
                  comms_link_up ? 60 + gauss(0, 8) : 0);
        tel.field("comms", "", "moos_msg_rate", 120 + gauss(0, 10));
        tel.field("comms", "", "moos_connected", 1);
        tel.field("comms", "", "signalk_msg_rate", 85 + gauss(0, 8));
        tel.flush();

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return 0;
}
