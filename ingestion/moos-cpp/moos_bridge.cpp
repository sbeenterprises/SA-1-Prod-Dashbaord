// Native MOOS-IvP -> InfluxDB bridge (FR-1, FR-2, FR-3, FR-8).
//
// A standard CMOOSApp: connects to the MOOSDB like any community app,
// registers for navigation / autopilot / mission variables, and writes
// them to InfluxDB in batched line protocol.
//
// Run: moos_bridge [mission.moos] [AppName]
// With no .moos file, connects using MOOS_HOST / MOOS_PORT env vars.

#include "MOOS/libMOOS/App/MOOSApp.h"

#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../cpp-common/influx_writer.hpp"

namespace {

struct FieldMap { const char* measurement; const char* field; };

// Numeric MOOS variables -> measurement/field.
// Adjust to your helm configuration (see docs/deployment.md).
const std::map<std::string, FieldMap> kNumericVars = {
    // Navigation (FR-2)
    {"NAV_X",              {"nav", "x"}},
    {"NAV_Y",              {"nav", "y"}},
    {"NAV_LAT",            {"nav", "lat"}},
    {"NAV_LONG",           {"nav", "lon"}},
    {"NAV_HEADING",        {"nav", "heading"}},
    {"NAV_SPEED",          {"nav", "sog"}},
    {"NAV_YAW",            {"nav", "yaw"}},
    {"NAV_PITCH",          {"nav", "pitch"}},
    {"NAV_ROLL",           {"nav", "roll"}},
    // Autopilot (FR-3)
    {"DESIRED_HEADING",    {"autopilot", "desired_heading"}},
    {"DESIRED_SPEED",      {"autopilot", "desired_speed"}},
    {"DESIRED_RUDDER",     {"autopilot", "rudder_cmd"}},
    {"DESIRED_THRUST",     {"autopilot", "throttle_cmd"}},
    {"PID_HEADING_ERROR",  {"autopilot", "heading_error"}},
    {"PID_SPEED_ERROR",    {"autopilot", "speed_error"}},
    {"PID_HEADING_OUT",    {"autopilot", "pid_heading_out"}},
    {"PID_SPEED_OUT",      {"autopilot", "pid_speed_out"}},
    // Mission (FR-1)
    {"MISSION_PROGRESS",   {"mission", "progress_pct"}},
    {"WPT_INDEX",          {"mission", "waypoint_index"}},
    {"WPT_TOTAL",          {"mission", "waypoints_total"}},
    {"WPT_DIST",           {"mission", "dist_to_waypoint"}},
    {"WPT_ETA",            {"mission", "eta_s"}},
    {"CROSS_TRACK_ERROR",  {"nav", "cross_track_error"}},
    {"DIST_TRAVELED",      {"mission", "distance_traveled"}},
};

// String MOOS variables -> measurement/field (mission_state).
const std::map<std::string, FieldMap> kStringVars = {
    {"MISSION_NAME",        {"mission_state", "mission_name"}},
    {"MISSION_ID",          {"mission_state", "mission_id"}},
    {"MISSION_STATE",       {"mission_state", "state"}},
    {"IVPHELM_SUMMARY",     {"mission_state", "helm_summary"}},
    {"ACTIVE_BEHAVIOR",     {"mission_state", "active_behavior"}},
    {"STATION_KEEP_STATUS", {"mission_state", "station_keep"}},
    {"DEPLOY",              {"mission_state", "deploy"}},
};

// State changes that also land in the events bucket (mission timeline).
bool IsEventVar(const std::string& name) {
    return name == "MISSION_STATE" || name == "ACTIVE_BEHAVIOR" ||
           name == "DEPLOY";
}

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
        for (const auto& [var, _] : kNumericVars) Register(var, 0.0);
        for (const auto& [var, _] : kStringVars) Register(var, 0.0);
        MOOSTrace("grafana_bridge: connected, %zu vars registered\n",
                  kNumericVars.size() + kStringVars.size());
        // No telemetry write here on purpose: this callback can fire before
        // OnStartUp() has built the writers, so a one-shot write would be
        // silently dropped — and a "1" never followed by a "0" could not
        // signal a disconnect anyway. Iterate() emits the live state.
        return true;
    }

    bool OnNewMail(MOOSMSG_LIST& mail) override {
        if (!telemetry_) return true;   // mail before OnStartUp finished
        for (auto& msg : mail) {
            const std::string& key = msg.GetKey();
            if (msg.IsDouble()) {
                auto it = kNumericVars.find(key);
                if (it != kNumericVars.end())
                    telemetry_->field(it->second.measurement, "",
                                      it->second.field, msg.GetDouble());
            } else if (msg.IsString()) {
                auto it = kStringVars.find(key);
                if (it != kStringVars.end()) {
                    const std::string val = msg.GetString();
                    telemetry_->str_field(it->second.measurement, "",
                                          it->second.field, val);
                    if (IsEventVar(key) && last_event_[key] != val) {
                        last_event_[key] = val;
                        events_->str_field("events",
                                           "source=moos,type=state_change",
                                           "message", key + "=" + val);
                        events_->field("events",
                                       "source=moos,type=state_change",
                                       "value", 1);
                        events_->flush();
                    }
                }
            }
            ++msg_count_;
        }
        return true;
    }

    bool Iterate() override {
        if (!telemetry_) return true;
        const double now = MonoSeconds();
        if (last_rate_emit_ == 0.0) last_rate_emit_ = now;
        if (now - last_rate_emit_ >= 5.0) {
            const double rate = msg_count_ / (now - last_rate_emit_);
            telemetry_->field("comms", "", "moos_msg_rate", rate);
            // Live link state (not a one-shot): 1 while the MOOSDB session is
            // up, 0 if it drops, so a stale/zero series means "disconnected".
            telemetry_->field("comms", "", "moos_connected",
                              m_Comms.IsConnected() ? 1 : 0);
            msg_count_ = 0;
            last_rate_emit_ = now;
        }
        telemetry_->flush();
        return true;
    }

private:
    std::unique_ptr<usv::InfluxWriter> telemetry_;
    std::unique_ptr<usv::InfluxWriter> events_;
    std::map<std::string, std::string> last_event_;
    uint64_t msg_count_ = 0;
    double last_rate_emit_ = 0.0;
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
