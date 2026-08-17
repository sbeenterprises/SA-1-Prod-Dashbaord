// Native MOOS-IvP -> InfluxDB bridge (FR-1, FR-2, FR-3, FR-8), integrated
// with the Alpha mission (alpha.bhv). See docs/alpha-mission-integration.md
// for the variable inventory, the derived mission state machine, and the
// design rationale.
//
// A standard CMOOSApp: connects to the MOOSDB like any community app,
// registers for navigation / autopilot / mission / autonomy variables and
// writes them to InfluxDB in batched line protocol. Read-only: it never
// publishes commands to the MOOSDB.
//
// What it does beyond 1:1 passthrough:
//  - Derives the operational mission state (Idle / Autonomous / Paused /
//    StationKeeping / ReturnToHome / EnteringRegion / AwaitingRearm /
//    CommsLost / Parked / Completed) from the mode flags the Alpha mission
//    actually publishes (nothing on the vessel publishes MISSION_STATE).
//  - Parses the active behavior set out of IVPHELM_SUMMARY.
//  - Computes progress / waypoint total (VIEW_SEGLIST), ETA and distance
//    traveled — yielding to any external publisher of the legacy variables.
//  - Emits operational events (state changes, waypoint captures, geofence
//    breach/recovery, comms-loss failsafe and re-arm, helm all-stops,
//    obstacle-avoidance spawns) into the events bucket.
//
// Run: moos_bridge [mission.moos] [AppName]
// With no .moos file, connects using MOOS_HOST / MOOS_PORT env vars.

#include "MOOS/libMOOS/App/MOOSApp.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "../cpp-common/influx_writer.hpp"
#include "mission_logic.hpp"

namespace {

constexpr double MS_TO_KN = 1.94384;

struct FieldMap {
    const char* measurement;
    const char* field;
    double scale = 1.0;  // unit conversion (schema uses display units)
};

// Numeric MOOS variables -> measurement/field (passthrough; a variable may
// feed several fields). MOOS speeds are m/s; the schema stores knots.
const std::multimap<std::string, FieldMap> kNumericVars = {
    // Navigation (FR-2)
    {"NAV_X",              {"nav", "x"}},
    {"NAV_Y",              {"nav", "y"}},
    {"NAV_LAT",            {"nav", "lat"}},
    {"NAV_LONG",           {"nav", "lon"}},
    {"NAV_HEADING",        {"nav", "heading"}},
    {"NAV_SPEED",          {"nav", "sog", MS_TO_KN}},
    {"NAV_YAW",            {"nav", "yaw"}},
    {"NAV_PITCH",          {"nav", "pitch"}},
    {"NAV_ROLL",           {"nav", "roll"}},
    // Autopilot (FR-3). NAV_HEADING/NAV_SPEED double as the "actual" side
    // of the desired-vs-actual pairs on the Autopilot dashboard.
    {"NAV_HEADING",        {"autopilot", "actual_heading"}},
    {"NAV_SPEED",          {"autopilot", "actual_speed", MS_TO_KN}},
    {"DESIRED_HEADING",    {"autopilot", "desired_heading"}},
    {"DESIRED_SPEED",      {"autopilot", "desired_speed", MS_TO_KN}},
    {"DESIRED_RUDDER",     {"autopilot", "rudder_cmd"}},
    {"DESIRED_THRUST",     {"autopilot", "throttle_cmd"}},
    {"PID_HEADING_ERROR",  {"autopilot", "heading_error"}},
    {"PID_SPEED_ERROR",    {"autopilot", "speed_error", MS_TO_KN}},
    {"PID_HEADING_OUT",    {"autopilot", "pid_heading_out"}},
    {"PID_SPEED_OUT",      {"autopilot", "pid_speed_out"}},
    {"CROSS_TRACK_ERROR",  {"nav", "cross_track_error"}},
};

// Legacy generic mission variables. Kept for compatibility: if some app
// publishes one, the bridge yields that field to the external producer and
// stops computing it internally (single-writer rule).
const std::map<std::string, FieldMap> kLegacyMissionVars = {
    {"MISSION_PROGRESS",   {"mission", "progress_pct"}},
    {"WPT_TOTAL",          {"mission", "waypoints_total"}},
    {"WPT_ETA",            {"mission", "eta_s"}},
    {"DIST_TRAVELED",      {"mission", "distance_traveled"}},
    {"WPT_DIST",           {"mission", "dist_to_waypoint"}},
};

// String passthrough -> mission_state fields.
const std::map<std::string, FieldMap> kStringPassthrough = {
    {"MISSION_NAME",       {"mission_state", "mission_name"}},
    {"MISSION_ID",         {"mission_state", "mission_id"}},
    // An externally published MISSION_STATE never overwrites the derived
    // state; it is stored alongside for comparison.
    {"MISSION_STATE",      {"mission_state", "state_external"}},
    {"OPR_STATUS",         {"mission_state", "opr_status"}},
};

// Alpha mission mode flags (posted as strings "true"/"false"; some tools
// post numeric 0/1 — both are accepted).
const std::set<std::string> kFlagVars = {
    "DEPLOY", "WAYPOINT", "RETURN", "STATIONING", "PAUSE",
    "MOOS_MANUAL_OVERRIDE", "MISSION_ARMED", "SURVEY_COMPLETE",
};

// Everything else we subscribe to.
const std::set<std::string> kSpecialVars = {
    "COMMS_LOST", "SURVEY_ENTRY_GATE",
    "IVPHELM_STATE", "IVPHELM_ALLSTOP", "IVPHELM_SUMMARY",
    "OPR_BREACHED", "VIEW_SEGLIST", "OBSTACLE_ALERT",
    "DIST_TO_NEXT_WPT", "WPT_INDEX",
    "ACTIVE_BEHAVIOR", "STATION_KEEP_STATUS",
};

// alpha.bhv initializes DIST_TO_NEXT_WPT = 99999 before any behavior posts
// a real value; treat anything this large as "no target yet".
constexpr double kNoTargetDist = 90000.0;

double MonoSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace

class GrafanaBridge : public CMOOSApp {
protected:
    // Configuration comes from the mission file's ProcessConfig block
    // (see example.moos), with environment variables as defaults:
    //
    //   ProcessConfig = grafana_bridge
    //   {
    //     AppTick   = 10
    //     CommsTick = 20
    //     influx_url    = localhost:8086   // no scheme: // starts a comment
    //     influx_tls    = false            // true -> https
    //     influx_token  = <token>
    //     influx_org    = usv
    //     influx_bucket = telemetry
    //     events_bucket = events
    //     vessel_id     = usv-01
    //   }
    bool OnStartUp() override {
        usv::InfluxOptions opts = usv::InfluxOptions::FromEnv("telemetry");
        std::string events_bucket = usv::env_or("EVENTS_BUCKET", "events");

        std::string v;
        // NOTE: give influx_url WITHOUT scheme (host:port) in the .moos file:
        // "//" starts a MOOS comment and would truncate "http://...".
        if (m_MissionReader.GetConfigurationParam("influx_url", v)) opts.url = v;
        bool tls = false;
        m_MissionReader.GetConfigurationParam("influx_tls", tls);
        if (opts.url.find("://") == std::string::npos)
            opts.url = (tls ? "https://" : "http://") + opts.url;
        if (m_MissionReader.GetConfigurationParam("influx_token", v)) opts.token = v;
        if (m_MissionReader.GetConfigurationParam("influx_org", v)) opts.org = v;
        if (m_MissionReader.GetConfigurationParam("influx_bucket", v)) opts.bucket = v;
        if (m_MissionReader.GetConfigurationParam("events_bucket", v)) events_bucket = v;
        if (m_MissionReader.GetConfigurationParam("vessel_id", v)) opts.vessel = v;

        telemetry_ = std::make_unique<usv::InfluxWriter>(opts);
        usv::InfluxOptions ev_opts = opts;
        ev_opts.bucket = events_bucket;
        events_ = std::make_unique<usv::InfluxWriter>(ev_opts);

        MOOSTrace("grafana_bridge: influx=%s bucket=%s vessel=%s\n",
                  opts.url.c_str(), opts.bucket.c_str(), opts.vessel.c_str());
        return true;
    }

    bool OnConnectToServer() override {
        size_t n = 0;
        std::set<std::string> registered;
        auto reg = [&](const std::string& var) {
            if (registered.insert(var).second) {
                Register(var, 0.0);
                ++n;
            }
        };
        for (const auto& [var, _] : kNumericVars) reg(var);
        for (const auto& [var, _] : kLegacyMissionVars) reg(var);
        for (const auto& [var, _] : kStringPassthrough) reg(var);
        for (const auto& var : kFlagVars) reg(var);
        for (const auto& var : kSpecialVars) reg(var);
        MOOSTrace("grafana_bridge: connected, %zu vars registered\n", n);
        // No telemetry write here on purpose: this callback can fire before
        // OnStartUp() has built the writers, so a one-shot write would be
        // silently dropped — and a "1" never followed by a "0" could not
        // signal a disconnect anyway. Iterate() emits the live state.
        return true;
    }

    bool OnNewMail(MOOSMSG_LIST& mail) override {
        if (!telemetry_) return true;   // mail before OnStartUp finished
        const double now = MonoSeconds();
        for (auto& msg : mail) {
            HandleMessage(msg, now);
            ++msg_count_;
        }
        // Mode flags may have changed within this batch — derive once.
        RefreshDerivedState(now);
        return true;
    }

    bool Iterate() override {
        if (!telemetry_) return true;
        const double now = MonoSeconds();

        // 1 Hz: autonomy snapshot + derived mission quantities. Periodic
        // (not just on-change) so stat panels stay fresh, the state
        // timelines are continuous, and threshold alerts self-clear.
        if (now - last_snapshot_ >= 1.0) {
            last_snapshot_ = now;
            WriteSnapshot(now);
        }

        // 5 s: link heartbeat + message rate.
        if (last_rate_emit_ == 0.0) last_rate_emit_ = now;
        if (now - last_rate_emit_ >= 5.0) {
            const double rate = msg_count_ / (now - last_rate_emit_);
            telemetry_->field("comms", "", "moos_msg_rate", rate);
            // Live link state (not a one-shot): 1 while the MOOSDB session
            // is up, 0 if it drops, so a stale/zero series means
            // "disconnected".
            telemetry_->field("comms", "", "moos_connected",
                              m_Comms.IsConnected() ? 1 : 0);
            msg_count_ = 0;
            last_rate_emit_ = now;
        }
        telemetry_->flush();
        return true;
    }

private:
    // ------------------------------------------------------------- events
    void Event(const std::string& type, const std::string& severity,
               const std::string& message) {
        const std::string tags =
            "severity=" + severity + ",source=moos,type=" + type;
        events_->str_field("events", tags, "message", message);
        events_->field("events", tags, "value", 1);
        events_->flush();
    }

    // ------------------------------------------------------ mail handling
    static std::string AsString(const CMOOSMsg& msg) {
        if (msg.IsString()) return msg.GetString();
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%g", msg.GetDouble());
        return buf;
    }

    void HandleMessage(const CMOOSMsg& msg, double now) {
        const std::string& key = msg.GetKey();

        // 1) Plain numeric passthrough (may fan out to several fields).
        if (msg.IsDouble()) {
            auto range = kNumericVars.equal_range(key);
            for (auto it = range.first; it != range.second; ++it)
                telemetry_->field(it->second.measurement, "",
                                  it->second.field,
                                  msg.GetDouble() * it->second.scale);
            if (range.first != range.second) {
                if (key == "NAV_SPEED") last_speed_mps_ = msg.GetDouble();
                if (key == "NAV_X") { nav_x_ = msg.GetDouble(); have_x_ = true; }
                // Odometry: integrate on NAV_Y arrival (NAV_X cached above).
                if (key == "NAV_Y" && have_x_)
                    odom_.add(nav_x_, msg.GetDouble());
            }
        }

        // 2) Legacy generic mission variables -> passthrough + yield.
        auto legacy = kLegacyMissionVars.find(key);
        if (legacy != kLegacyMissionVars.end() && msg.IsDouble()) {
            // WPT_DIST is superseded by DIST_TO_NEXT_WPT when both exist.
            if (key == "WPT_DIST" && seen_dist_to_next_) return;
            external_fields_.insert(legacy->second.field);
            telemetry_->field(legacy->second.measurement, "",
                              legacy->second.field, msg.GetDouble());
            return;
        }

        // 3) String passthrough.
        auto sp = kStringPassthrough.find(key);
        if (sp != kStringPassthrough.end() && msg.IsString()) {
            telemetry_->str_field(sp->second.measurement, "",
                                  sp->second.field, msg.GetString());
            return;
        }

        // 4) Alpha mission variables.
        if (key == "DIST_TO_NEXT_WPT" && msg.IsDouble()) {
            seen_dist_to_next_ = true;
            const double d = msg.GetDouble();
            if (d < kNoTargetDist) {   // 99999 = "no target yet" initializer
                last_dist_ = d;
                last_dist_time_ = now;
                if (external_fields_.count("dist_to_waypoint") == 0)
                    telemetry_->field("mission", "", "dist_to_waypoint", d);
            }
        } else if (key == "WPT_INDEX" && msg.IsDouble()) {
            const int idx = static_cast<int>(msg.GetDouble());
            if (flags_.waypoint && last_wpt_index_ >= 0 &&
                idx != last_wpt_index_) {
                char m[128];
                if (idx > last_wpt_index_)
                    std::snprintf(m, sizeof(m),
                                  "Waypoint %d captured — heading to %d of %d",
                                  last_wpt_index_, idx, survey_wpt_total_);
                else
                    std::snprintf(m, sizeof(m),
                                  "Route loop complete — restarting at "
                                  "waypoint %d", idx);
                Event("waypoint", "info", m);
            }
            last_wpt_index_ = idx;
            telemetry_->field("mission", "", "waypoint_index", idx);
        } else if (kFlagVars.count(key)) {
            HandleFlag(key, usv::truthy(AsString(msg)));
        } else if (key == "COMMS_LOST") {
            HandleCommsLost(AsString(msg));
        } else if (key == "SURVEY_ENTRY_GATE") {
            flags_.entry_gate = usv::lower(usv::trim(AsString(msg)));
            telemetry_->str_field("mission_state", "", "entry_gate",
                                  flags_.entry_gate);
        } else if (key == "IVPHELM_STATE" && msg.IsString()) {
            HandleHelmState(msg.GetString());
        } else if (key == "IVPHELM_ALLSTOP" && msg.IsString()) {
            HandleAllStop(msg.GetString());
        } else if (key == "IVPHELM_SUMMARY" && msg.IsString()) {
            HandleHelmSummary(msg.GetString());
        } else if (key == "OPR_BREACHED" && msg.IsString()) {
            HandleGeofence(msg.GetString(), now);
        } else if (key == "VIEW_SEGLIST" && msg.IsString()) {
            HandleSeglist(msg.GetString());
        } else if (key == "OBSTACLE_ALERT" && msg.IsString()) {
            const std::string id = usv::ParseObstacleId(msg.GetString());
            if (obstacles_.post(id, now))
                Event("obstacle", "warning",
                      "Obstacle avoidance active: " + id);
        } else if (key == "ACTIVE_BEHAVIOR" && msg.IsString()) {
            external_active_behavior_ = true;
            WriteActiveBehavior(msg.GetString());
        } else if (key == "STATION_KEEP_STATUS" && msg.IsString()) {
            external_station_keep_ = true;
            telemetry_->str_field("mission_state", "", "station_keep",
                                  msg.GetString());
        }
    }

    void HandleFlag(const std::string& key, bool val) {
        bool* slot = nullptr;
        if      (key == "DEPLOY")               slot = &flags_.deploy;
        else if (key == "WAYPOINT")             slot = &flags_.waypoint;
        else if (key == "RETURN")               slot = &flags_.returning;
        else if (key == "STATIONING")           slot = &flags_.stationing;
        else if (key == "PAUSE")                slot = &flags_.pause;
        else if (key == "MOOS_MANUAL_OVERRIDE") slot = &flags_.manual_override;
        else if (key == "MISSION_ARMED")        slot = &flags_.armed;
        else if (key == "SURVEY_COMPLETE")      slot = &flags_.survey_complete;
        if (!slot || *slot == val) return;
        *slot = val;

        if (key == "WAYPOINT" && val) {
            // Survey (re)start: reset the mission-scoped accumulators.
            mission_start_ = MonoSeconds();
            odom_.reset();
            flags_.survey_complete = false;
            char m[96];
            std::snprintf(m, sizeof(m), "Survey started (%d waypoints)",
                          survey_wpt_total_);
            Event("mission", "info", m);
        } else if (key == "RETURN" && val) {
            Event("mission", "info", "Return-to-base started");
            if (mission_start_ == 0.0) mission_start_ = MonoSeconds();
        } else if (key == "SURVEY_COMPLETE" && val) {
            Event("mission", "info",
                  "Survey complete — stopping at final waypoint");
        } else if (key == "MISSION_ARMED") {
            if (val)
                Event("mission", "info",
                      "Mission re-armed by operator (AUTONOMOUS_START/RESUME)");
            else if (!usv::comms_lost_active(flags_.comms_lost))
                Event("alarm", "warning", "Mission disarmed");
        } else if (key == "MOOS_MANUAL_OVERRIDE") {
            Event("state_change", "info",
                  val ? "Helm parked (MOOS_MANUAL_OVERRIDE)"
                      : "Manual override released — helm active");
        }

        if (!external_station_keep_ && key == "STATIONING")
            telemetry_->str_field("mission_state", "", "station_keep",
                                  val ? "true" : "false");
    }

    void HandleCommsLost(const std::string& raw) {
        const bool was = usv::comms_lost_active(flags_.comms_lost);
        flags_.comms_lost = usv::trim(raw);
        const bool is = usv::comms_lost_active(flags_.comms_lost);
        telemetry_->str_field("mission_state", "", "comms_failsafe",
                              is ? flags_.comms_lost : "ok");
        if (is && !was) {
            Event("alarm", "critical",
                  "Operator link LOST — failsafe '" + flags_.comms_lost +
                  "' engaged, mission disarmed");
        } else if (!is && was) {
            Event("alarm", "info",
                  flags_.armed
                      ? "Operator link restored"
                      : "Operator link restored — survey held until "
                        "operator re-arm");
        }
    }

    void HandleHelmState(const std::string& s) {
        if (s == helm_state_str_) return;
        helm_state_str_ = s;
        flags_.helm_state = s;
        telemetry_->str_field("mission_state", "", "helm_state", s);
        Event("state_change",
              s == "MALCONFIG" ? "critical" : "info", "Helm: " + s);
    }

    void HandleAllStop(const std::string& reason) {
        if (reason == allstop_str_) return;
        const bool was_bad = usv::allstop_unexpected(allstop_str_);
        const bool is_bad = usv::allstop_unexpected(reason);
        allstop_str_ = reason;
        flags_.allstop = reason;
        telemetry_->str_field("mission_state", "", "allstop_reason", reason);
        if (is_bad)
            Event("alarm", "critical", "Helm ALLSTOP: " + reason);
        else if (was_bad)
            Event("alarm", "info", "Helm all-stop cleared (" + reason + ")");
    }

    void HandleHelmSummary(const std::string& summary) {
        if (external_active_behavior_) return;
        const std::string active = usv::ParseActiveBehaviors(summary);
        WriteActiveBehavior(active.empty() ? "none" : active);
        // Store the raw summary too (debug value for engineers).
        telemetry_->str_field("mission_state", "", "helm_summary", summary);
    }

    void WriteActiveBehavior(const std::string& active) {
        if (active == last_active_behavior_) return;
        last_active_behavior_ = active;
        telemetry_->str_field("mission_state", "", "active_behavior", active);
        Event("state_change", "info", "Active behaviors: " + active);
    }

    void HandleGeofence(const std::string& val, double now) {
        const std::string v = usv::lower(usv::trim(val));
        // poly = hard breach; recovering = beyond save_dist, being steered
        // back. Levels chosen so "breach" (1) is the alertable condition.
        geofence_.post(v == "poly" ? 1 : 2, now);
        // Event emission happens in WriteSnapshot on level transitions
        // (also covers the decay back to 0 = recovered).
    }

    void HandleSeglist(const std::string& spec) {
        const usv::SeglistInfo info = usv::ParseSeglist(spec);
        if (info.points <= 0) return;
        seglists_[info.label] = info.points;
        // Prefer the survey route's seglist; fall back to the largest
        // non-return list (labels are "<vessel>_<behavior>").
        int total = 0;
        for (const auto& [label, pts] : seglists_) {
            if (label.find("survey") != std::string::npos) {
                total = pts;
                break;
            }
            if (label.find("return") == std::string::npos)
                total = std::max(total, pts);
        }
        if (total > 0) survey_wpt_total_ = total;
    }

    // ------------------------------------------------- derived state
    void RefreshDerivedState(double now) {
        const std::string state = usv::DeriveMissionState(flags_);
        if (state != last_state_) {
            last_state_ = state;
            telemetry_->str_field("mission_state", "", "state", state);
            Event("state_change", "info", "Mission state: " + state);
        }
        (void)now;
    }

    void WriteSnapshot(double now) {
        // Cold start with no mail yet: derive (and event) the initial state
        // so the snapshot never writes an empty state string.
        if (last_state_.empty()) RefreshDerivedState(now);

        // Geofence level (decays to 0 after 30 s without OPR_BREACHED).
        const int gf = geofence_.level(now);
        if (gf != last_geofence_) {
            if (gf == 1)
                Event("geofence", "critical",
                      "Geofence BREACHED — outside operating region");
            else if (gf == 2)
                Event("geofence", "warning",
                      "Geofence recovery — steering back inside region");
            else
                Event("geofence", "info", "Geofence clear — inside region");
            last_geofence_ = gf;
        }

        // Numeric autonomy flags (0/1 so they can be graphed and alerted).
        auto b = [](bool v) { return v ? 1.0 : 0.0; };
        telemetry_->field("autonomy", "", "deploy", b(flags_.deploy));
        telemetry_->field("autonomy", "", "waypoint", b(flags_.waypoint));
        telemetry_->field("autonomy", "", "returning", b(flags_.returning));
        telemetry_->field("autonomy", "", "stationing", b(flags_.stationing));
        telemetry_->field("autonomy", "", "pause", b(flags_.pause));
        telemetry_->field("autonomy", "", "armed", b(flags_.armed));
        telemetry_->field("autonomy", "", "comms_lost",
                          b(usv::comms_lost_active(flags_.comms_lost)));
        telemetry_->field("autonomy", "", "entry_gate_open",
                          b(usv::gate_open(flags_.entry_gate)));
        telemetry_->field("autonomy", "", "manual_override",
                          b(flags_.manual_override));
        telemetry_->field("autonomy", "", "survey_complete",
                          b(flags_.survey_complete));
        telemetry_->field("autonomy", "", "awaiting_rearm",
                          b(usv::awaiting_rearm(flags_)));
        telemetry_->field("autonomy", "", "helm_drive",
                          b(helm_state_str_ == "DRIVE"));
        telemetry_->field("autonomy", "", "allstop",
                          b(usv::allstop_unexpected(allstop_str_)));
        telemetry_->field("autonomy", "", "geofence_breached", gf);
        telemetry_->field("autonomy", "", "obstacle_alerts",
                          obstacles_.active(now));

        // Derived state written periodically too, so the state timeline is
        // continuous and last() panels never go stale.
        telemetry_->str_field("mission_state", "", "state", last_state_);

        // Derived mission quantities — each yields to an external producer.
        const bool wpt_active = flags_.waypoint || flags_.returning;
        if (!external_fields_.count("waypoints_total") &&
            survey_wpt_total_ > 0)
            telemetry_->field("mission", "", "waypoints_total",
                              survey_wpt_total_);
        if (!external_fields_.count("progress_pct") && survey_wpt_total_ > 0) {
            const double pct =
                flags_.survey_complete
                    ? 100.0
                    : usv::ProgressPct(std::max(0, last_wpt_index_),
                                       survey_wpt_total_);
            telemetry_->field("mission", "", "progress_pct", pct);
        }
        if (!external_fields_.count("eta_s") && wpt_active &&
            now - last_dist_time_ < 10.0)
            telemetry_->field("mission", "", "eta_s",
                              usv::EtaSeconds(last_dist_, last_speed_mps_));
        if (!external_fields_.count("distance_traveled") &&
            mission_start_ > 0.0)
            telemetry_->field("mission", "", "distance_traveled",
                              odom_.total());
        if (mission_start_ > 0.0 && last_state_ != "Idle" &&
            last_state_ != "Completed")
            telemetry_->field("mission", "", "elapsed_s",
                              now - mission_start_);
    }

    // ---------------------------------------------------------- members
    std::unique_ptr<usv::InfluxWriter> telemetry_;
    std::unique_ptr<usv::InfluxWriter> events_;

    usv::MissionFlags flags_;
    std::string last_state_;
    std::string helm_state_str_;
    std::string allstop_str_ = "clear";
    std::string last_active_behavior_;

    usv::DecayLatch geofence_{30.0};
    int last_geofence_ = 0;
    usv::ObstacleTracker obstacles_{120.0};
    usv::OdometryIntegrator odom_;
    std::map<std::string, int> seglists_;

    // External producers we yield to (legacy variable compatibility).
    std::set<std::string> external_fields_;
    bool external_active_behavior_ = false;
    bool external_station_keep_ = false;
    bool seen_dist_to_next_ = false;

    int last_wpt_index_ = -1;
    int survey_wpt_total_ = 0;
    double last_dist_ = 0.0, last_dist_time_ = -1e18;
    double last_speed_mps_ = 0.0;
    double nav_x_ = 0.0;
    bool have_x_ = false;
    double mission_start_ = 0.0;

    uint64_t msg_count_ = 0;
    double last_rate_emit_ = 0.0;
    double last_snapshot_ = 0.0;
};

int main(int argc, char* argv[]) {
    GrafanaBridge app;
    if (argc > 1) {
        // Standard MOOS invocation: moos_bridge mission.moos [flags]
        // Mission file explicitly first so ProcessConfig = grafana_bridge
        // is parsed; remaining argv switches (--moos_*) still apply.
        app.Run("grafana_bridge", argv[1], argc, argv);
        return 0;
    }
    // No mission file: take server from env and loop with reconnection.
    const std::string host_arg =
        "--moos_host=" + usv::env_or("MOOS_HOST", "localhost");
    const std::string port_arg =
        "--moos_port=" + usv::env_or("MOOS_PORT", "9000");
    std::vector<char*> args = {
        argv[0],
        const_cast<char*>(host_arg.c_str()),
        const_cast<char*>(port_arg.c_str()),
    };
    while (true) {
        app.Run("grafana_bridge", static_cast<int>(args.size()), args.data());
        std::cerr << "MOOS connection ended — retrying in 5s\n";
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
    return 0;
}
