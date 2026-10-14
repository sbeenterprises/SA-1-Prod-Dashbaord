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
//    StationKeeping / ReturnToBase / EnteringRegion / AwaitingRearm /
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

#include "../cpp-common/engine_profile.hpp"
#include "../cpp-common/j1939_faults.hpp"
#include "../cpp-common/http_get.hpp"
#include "../cpp-common/influx_query.hpp"
#include "../cpp-common/influx_writer.hpp"
#include "mission_logic.hpp"

namespace {

constexpr double MS_TO_KN = 1.94384;
constexpr double KN_TO_MS = 1.0 / MS_TO_KN;   // RADAR_TARGET spd is knots ($RATTM)
// FieldMap scale for the helm's own speed variables (NAV_SPEED,
// DESIRED_SPEED, SPEED_CAP, CMD_SPEED, PID_SPEED_ERROR): converted with
// nav_speed_units, because they are knots on the boats (NAV_SPEED from the
// GPS through iDivisorNMEA, the helm speeds set to match) and m/s in the
// simulator (uSimMarineV22).
constexpr double kHelmSpeed = -1.0;

struct FieldMap {
    const char* measurement;
    const char* field;
    double scale = 1.0;   // unit conversion (schema uses display units)
    const char* tags = "";  // extra line-protocol tags, e.g. "instance=0"
};

// Numeric MOOS variables -> measurement/field (passthrough; a variable may
// feed several fields). MOOS speeds are m/s; the schema stores knots.
//
// Producers on the vessel: iDivisorNMEA (NAV_*, GPS_*, ANGULO_LEME),
// planchaPID (DESIRED_THRUST/RUDDER, SPEED_CAP, CMD_SPEED, XTE_NOW,
// NEXT_TURN_ANGLE, DIST_TO_STATION), pHelmIvP (DESIRED_HEADING/SPEED),
// iCANRec (ENGINE_*, SAE J1939). In simulation uSimMarineV23 publishes the
// NAV_* set (incl. NAV_HEADING_OVER_GROUND) and there is no engine data.
const std::multimap<std::string, FieldMap> kNumericVars = {
    // Navigation (FR-2)
    {"NAV_X",                    {"nav", "x"}},
    {"NAV_Y",                    {"nav", "y"}},
    {"NAV_LAT",                  {"nav", "lat"}},
    {"NAV_LONG",                 {"nav", "lon"}},
    {"NAV_HEADING",              {"nav", "heading"}},
    {"NAV_SPEED",                {"nav", "sog", kHelmSpeed}},
    {"NAV_COG",                  {"nav", "cog"}},           // iDivisorNMEA
    {"NAV_HEADING_OVER_GROUND",  {"nav", "cog"}},           // uSimMarine
    {"NAV_YAW",                  {"nav", "yaw"}},           // vessel: see ignore_vars
    {"NAV_PITCH",                {"nav", "pitch"}},
    {"NAV_ROLL",                 {"nav", "roll"}},
    {"NAV_DEPTH",                {"nav", "depth"}},
    {"GPS_SATS",                 {"nav", "satellites"}},
    {"GPS_HDOP",                 {"nav", "hdop"}},
    {"GPS_QUALITY",              {"nav", "gps_quality"}},
    {"XTE_NOW",                  {"nav", "cross_track_error"}},   // planchaPID
    {"CROSS_TRACK_ERROR",        {"nav", "cross_track_error"}},   // legacy name
    // Autopilot (FR-3). NAV_HEADING/NAV_SPEED double as the "actual" side
    // of the desired-vs-actual pairs on the Autopilot dashboard.
    {"NAV_HEADING",              {"autopilot", "actual_heading"}},
    {"NAV_SPEED",                {"autopilot", "actual_speed", kHelmSpeed}},
    {"DESIRED_HEADING",          {"autopilot", "desired_heading"}},
    {"DESIRED_SPEED",            {"autopilot", "desired_speed", kHelmSpeed}},
    {"DESIRED_RUDDER",           {"autopilot", "rudder_cmd"}},
    {"DESIRED_THRUST",           {"autopilot", "throttle_cmd"}},
    {"ANGULO_LEME",              {"autopilot", "rudder_actual"}},  // $AGRSA
    // planchaPID speed governor (m/s -> kn)
    {"SPEED_CAP",                {"autopilot", "speed_cap", kHelmSpeed}},
    {"CMD_SPEED",                {"autopilot", "cmd_speed", kHelmSpeed}},
    {"NEXT_TURN_ANGLE",          {"autopilot", "next_turn_angle"}},
    {"DIST_TO_STATION",          {"mission", "dist_to_station"}},
    // Legacy pMarinePID outputs (not published by planchaPID; if they ever
    // appear they take precedence over the bridge-derived errors).
    {"PID_HEADING_ERROR",        {"autopilot", "heading_error"}},
    {"PID_SPEED_ERROR",          {"autopilot", "speed_error", kHelmSpeed}},
    {"PID_HEADING_OUT",          {"autopilot", "pid_heading_out"}},
    {"PID_SPEED_OUT",            {"autopilot", "pid_speed_out"}},
    // Engine (FR-4), iCANRec J1939 decode. Units are already display units
    // (rpm, C, kPa, L/h, %, h, count, V). Every point is tagged with the
    // engine instance (0, or the _<n> suffix of the variable name: REC-7)
    // and the vessel's engine type. ENGINE_LAMPS and ENGINE_DTC_TEXT are
    // handled in HandleMessage (one variable fans out to the lamp fields;
    // the text is enriched with the Mercury fault names).
    {"ENGINE_SPEED",                      {"engine", "rpm"}},
    {"ENGINE_COOLANT_TEMP",               {"engine", "coolant_temp"}},
    {"ENGINE_OIL_TEMP",                   {"engine", "oil_temp"}},
    {"ENGINE_OIL_PRESSURE",               {"engine", "oil_pressure"}},
    {"ENGINE_FUEL_RATE",                  {"engine", "fuel_rate"}},
    {"ENGINE_PERCENT_LOAD",               {"engine", "percent_load"}},
    {"ENGINE_PERCENT_TORQUE",             {"engine", "percent_torque"}},
    {"ENGINE_ACCELERATOR_PEDAL_POSITION", {"engine", "pedal_position"}},
    {"ENGINE_INTAKE_MANIFOLD_PRESSURE",   {"engine", "intake_manifold_pressure"}},
    {"ENGINE_ACTIVE_DTCS",                {"engine", "active_dtcs"}},
    {"ENGINE_DTC_SPN",                    {"engine", "dtc_spn"}},
    {"ENGINE_DTC_FMI",                    {"engine", "dtc_fmi"}},
    {"ENGINE_DTC_OC",                     {"engine", "dtc_oc"}},
    {"ENGINE_HOURS",                      {"engine", "runtime_h"}},
    {"ENGINE_FUEL_LEVEL",                 {"engine", "fuel_level"}},
    // The ECM's own supply (J1939 VEP1 SPN 168). The house battery and the
    // system voltage come from the Victron shunt through signalk_bridge.
    {"ENGINE_BATTERY_VOLTAGE",            {"engine", "ecu_voltage"}},
};
// ENGINE_LAMPS (J1939 DM1 byte 1): bit pairs protect (0), amber (2), red
// stop (4), MIL (6); 01 = lamp on. Written as the raw byte and one 0/1
// field per lamp, the fields the Fault Lamps tiles read.
const struct { int shift; const char* field; } kLampBits[] = {
    {0, "lamp_protect"}, {2, "lamp_amber"}, {4, "lamp_red"}, {6, "lamp_mil"}};

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
    // iWebSocket: route id sent with AUTONOMOUS_START (#ID_ROUTE=...)
    {"ID_ROUTE",           {"mission_state", "mission_id"}},
    {"REGION_WARNING",     {"mission_state", "region_warning"}},
    // planchaPID status strings (posted every iterate: written on change,
    // re-emitted at 1 Hz by the snapshot)
    {"ARRIVAL_STATE",      {"mission_state", "arrival_state"}},
    {"CORRIDOR_STATUS",    {"mission_state", "corridor_status"}},
    {"CRASH_STOP_STATUS",  {"mission_state", "crash_stop_status"}},
    {"WPT_STAT",           {"mission_state", "wpt_stat"}},
    // Operator console (Swiftships-UI) state for the Readiness dashboard:
    // ENC_STATUS = "selected:<chart name>" | "none"
    {"ENC_STATUS",         {"mission_state", "enc_status"}},
};

// Alpha mission mode flags (posted as strings "true"/"false"; some tools
// post numeric 0/1 — both are accepted). MOOS_MANUAL_OVERIDE (single R) is
// the misspelled variant iWebSocket still posts for CONTROLE_MANUAL; treated
// as the same flag (see docs/wire-protocol.md in the MOOS repo).
const std::set<std::string> kFlagVars = {
    "DEPLOY", "WAYPOINT", "RETURN", "STATIONING", "PAUSE",
    "MOOS_MANUAL_OVERRIDE", "MOOS_MANUAL_OVERIDE",
    "MISSION_ARMED", "SURVEY_COMPLETE",
    "REMOTE_CONTROL", "CRASH_STOP",
    // Production iWebSocket comms-loss failsafe (return to base and hold).
    "COMMS_LOSS", "RETURN_HOLD",
};

// Everything else we subscribe to.
const std::set<std::string> kSpecialVars = {
    "COMMS_LOST", "SURVEY_ENTRY_GATE", "BASE_COORDS",
    "IVPHELM_STATE", "IVPHELM_ALLSTOP", "IVPHELM_SUMMARY",
    "OPR_BREACHED", "VIEW_SEGLIST", "OPR_POLY_ACTUAL", "REGION_UPDATES", "OBSTACLE_ALERT",
    "CHECKLIST_STATUS", "DB_CLIENTS",
    "DIST_TO_NEXT_WPT", "WPT_INDEX",
    "ACTIVE_BEHAVIOR", "STATION_KEEP_STATUS",
    "BHV_WARNING", "BHV_ERROR",
    // Radar / contacts: NODE_REPORT (iRadarTargets, AIS via iDivisorNMEA,
    // pContactSpawn in simulation), RADAR_TARGET / RADAR_TTM_RATE (proposed
    // iRadarTargets additions with CPA/TCPA and a liveness rate).
    "NODE_REPORT", "RADAR_TARGET", "RADAR_TTM_RATE",
};

// Variables whose every sample feeds a derived quantity (odometry, heading
// and speed error), so they are cached before the rate limiter runs.
const std::set<std::string> kCachedVars = {
    "NAV_SPEED", "NAV_X", "NAV_Y", "NAV_HEADING",
    "DESIRED_HEADING", "DESIRED_SPEED",
};

double EpochMs() {
    using namespace std::chrono;
    return static_cast<double>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch())
            .count());
}

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

        // Deployment tuning (all optional; defaults suit the simulation).
        //   mission_name   = alpha        // shown on the Mission dashboard
        //   max_rate_hz    = 5            // ceiling for nav/autopilot passthrough
        //   engine_rate_hz = 2            // ceiling for ENGINE_* passthrough
        //   ignore_vars    = NAV_YAW      // vessel: NAV_YAW is the rudder angle
        //   radar_cpa_alert_m = 500       // HIGH collision risk: CPA below this ...
        //   tcpa_alert_s   = 300          // ... within this many seconds
        //   contact_ttl_s  = 10           // contact expiry without an update
        //   signalk_http   = localhost:3000  // route-name lookup (ID_ROUTE ->
        //                                   // /signalk/v{2,1}/api/resources/routes/<id>);
        //                                   // empty disables the lookup
        //   signalk_token  = <jwt>          // only if Signal K security denies reads
        //   readiness_profile = vessel      // pre-departure checks: "vessel" = a
        //                                   // check without sensor data FAILS;
        //                                   // "sim" = it reads N/A (no engine/CAN/
        //                                   // camera data in simulation)
        //   readiness_period_s = 5          // evaluation and write period
        //   required_apps  = pHelmIvP,...   // MOOS apps that must be connected
        //                                   // (default: every Run= in the Antler block)
        //   optional_apps  = pMarineViewer,uMACView,uPlotViewer   // reported, never a fault
        //   fuel_tank_l / cruise_fuel_lph / cruise_speed_kn / battery_kwh /
        //   cruise_power_kw / endurance_margin   // Endurance check constants
        //   checklist_items = Name:Description|Name2:Description2   // console
        //   enc_fitted = false          // no ENC chart system: row reads N/A
        //   console_checklist = false   // console does not publish CHECKLIST_STATUS: N/A
        //                                   // checklist item list (short-form fallback)
        double hz = 5.0;
        if (m_MissionReader.GetConfigurationParam("max_rate_hz", hz)) {}
        limiter_.set_default(hz);
        double engine_hz = 2.0;
        m_MissionReader.GetConfigurationParam("engine_rate_hz", engine_hz);
        {
            std::vector<std::string> fields = {"fault_lamps"};
            for (const auto& [var, fm] : kNumericVars)
                if (std::string(fm.measurement) == "engine") fields.push_back(fm.field);
            for (const auto& lb : kLampBits) fields.push_back(lb.field);
            // One limiter key per field and engine instance (0 unsuffixed,
            // "#1" .. "#3" for a second, third or fourth engine).
            for (const auto& f : fields)
                for (int inst = 0; inst < 4; ++inst)
                    limiter_.set_rate("engine." + f + (inst ? "#" + std::to_string(inst) : ""),
                                      engine_hz);
        }
        // Engine profile of this vessel (/etc/usv/usv.env: ENGINE_SOURCE,
        // ENGINE_TYPE). ENGINE_* passthrough is on when MOOS (iCANRec) is the
        // engine source or no source is named; the mission file can override:
        //   engine_passthrough = true | false
        //   engine_type = mercury_diesel          // tag on every engine point
        engine_profile_ = usv::EngineProfile::FromEnv();
        engine_passthrough_ = engine_profile_.moos_engine;
        m_MissionReader.GetConfigurationParam("engine_passthrough", engine_passthrough_);
        if (m_MissionReader.GetConfigurationParam("engine_type", v) && !v.empty())
            engine_profile_.type = usv::trim(v);
        limiter_.set_rate("autopilot.error", 2.0);
        if (m_MissionReader.GetConfigurationParam("ignore_vars", v))
            for (const auto& s : usv::SplitCsv(v)) ignore_vars_.insert(s);
        if (!m_MissionReader.GetConfigurationParam("mission_name", mission_name_))
            m_MissionReader.GetValue("Community", mission_name_);
        m_MissionReader.GetConfigurationParam("radar_cpa_alert_m", radar_cpa_alert_m_);
        m_MissionReader.GetConfigurationParam("tcpa_alert_s", tcpa_alert_s_);
        m_MissionReader.GetValue("Community", own_name_);
        double ttl = 10.0;
        m_MissionReader.GetConfigurationParam("contact_ttl_s", ttl);
        contacts_ = usv::ContactTracker(ttl);
        if (m_MissionReader.GetConfigurationParam("signalk_http", v))
            signalk_http_ = v;
        if (!signalk_http_.empty() && signalk_http_.find("://") == std::string::npos)
            signalk_http_ = "http://" + signalk_http_;
        m_MissionReader.GetConfigurationParam("signalk_token", signalk_token_);
        // Units of the helm's speeds (NAV_SPEED and the rest): knots on the
        // boats, m/s in the simulator. NAV_SPEED_UNITS in /etc/usv/usv.env,
        // or nav_speed_units in this block (wins); default mps, the units
        // the bridge has always assumed.
        {
            std::string units = usv::lower(usv::trim(usv::UsvSetting("NAV_SPEED_UNITS")));
            m_MissionReader.GetConfigurationParam("nav_speed_units", units);
            units = usv::lower(usv::trim(units));
            const bool knots = units == "knots" || units == "kn" || units == "kts";
            helm_speed_to_kn_ = knots ? 1.0 : MS_TO_KN;
            helm_speed_to_mps_ = knots ? KN_TO_MS : 1.0;
            MOOSTrace("grafana_bridge: nav_speed_units=%s\n", knots ? "knots" : "mps");
        }
        std::string profile = "vessel";
        m_MissionReader.GetConfigurationParam("readiness_profile", profile);
        readiness_strict_ = usv::lower(usv::trim(profile)) != "sim";
        m_MissionReader.GetConfigurationParam("readiness_period_s", readiness_period_s_);
        if (readiness_period_s_ < 1.0) readiness_period_s_ = 1.0;
        m_MissionReader.GetConfigurationParam("fuel_tank_l", readiness_cfg_.fuel_tank_l);
        m_MissionReader.GetConfigurationParam("cruise_fuel_lph", readiness_cfg_.cruise_fuel_lph);
        m_MissionReader.GetConfigurationParam("cruise_speed_kn", readiness_cfg_.cruise_speed_kn);
        m_MissionReader.GetConfigurationParam("battery_kwh", readiness_cfg_.battery_kwh);
        m_MissionReader.GetConfigurationParam("cruise_power_kw", readiness_cfg_.cruise_power_kw);
        m_MissionReader.GetConfigurationParam("endurance_margin", readiness_cfg_.endurance_margin);
        if (m_MissionReader.GetConfigurationParam("required_apps", v))
            for (const auto& a : usv::SplitCsv(v)) readiness_cfg_.required_apps.push_back(usv::trim(a));
        else {
            // Default: every app the Antler block launches.
            STRING_LIST antler;
            if (m_MissionReader.GetConfiguration("ANTLER", antler))
                readiness_cfg_.required_apps =
                    usv::ParseAntlerApps(std::vector<std::string>(antler.begin(), antler.end()));
        }
        v = "pMarineViewer,uMACView,uPlotViewer";   // GUI viewers: never a departure fault
        m_MissionReader.GetConfigurationParam("optional_apps", v);
        for (const auto& a : usv::SplitCsv(v)) readiness_cfg_.optional_apps.push_back(usv::trim(a));
        if (m_MissionReader.GetConfigurationParam("checklist_items", v))
            readiness_cfg_.checklist_items = usv::ParseChecklistItems(v);
        // Console-fed readiness rows: N/A instead of CONSOLE on vessels
        // without an ENC chart system / without a published checklist.
        m_MissionReader.GetConfigurationParam("enc_fitted", readiness_cfg_.enc_fitted);
        m_MissionReader.GetConfigurationParam("console_checklist", readiness_cfg_.console_checklist);
        // Map overlays need the MOOS local-grid datum (global block of the
        // mission file). Without it no geometry is written, nothing else
        // changes.
        have_datum_ = m_MissionReader.GetValue("LatOrigin", lat0_) &&
                      m_MissionReader.GetValue("LongOrigin", lon0_);

        telemetry_ = std::make_unique<usv::InfluxWriter>(opts);
        // A previous bridge build may have written rows for check ids that
        // no longer exist; clear them so a restart never shows leftovers.
        telemetry_->erase("_measurement=\"readiness\"");
        telemetry_->erase("_measurement=\"checklist\"");
        usv::InfluxOptions ev_opts = opts;
        ev_opts.bucket = events_bucket;
        events_ = std::make_unique<usv::InfluxWriter>(ev_opts);

        // Readiness (FR-12): keep the latest value of everything this bridge
        // writes, and poll InfluxDB for the few fields other producers write
        // (Telegraf host metrics and probes, Signal K link, CAN bridge,
        // cameras). Evaluation itself is usv::EvaluateReadiness.
        telemetry_->observe(
            [this](const std::string& m, const std::string& f, double v) {
                latest_.put(m, f, v, MonoSeconds());
            },
            [this](const std::string& m, const std::string& f, const std::string& v) {
                latest_.put_str(m, f, v, MonoSeconds());
            });
        poller_.start(opts, {
            {"comms", "signalk_ws_up", ""},
            // Position and fix quality written by signalk_bridge, for vessels
            // whose GNSS/INS feeds Signal K rather than MOOS NAV_LAT/NAV_LONG
            // (own fresher NAV_* samples still take precedence).
            {"nav", "lat", ""},
            {"nav", "lon", ""},
            // Own heading (deg) / SOG (kn) for the traffic picture while MOOS
            // NAV_* are unusable (NAV_LAT/NAV_LONG = 0).
            {"nav", "heading", ""},
            {"nav", "sog", ""},
            {"nav", "fix_type", ""},
            // Electrical / engine readiness on vessels whose battery shunt
            // and engine reach Signal K (signalk_bridge) instead of MOOS
            // (usv-722: Victron VE.Direct "House" battery).
            {"electrical", "battery_voltage", ""},
            {"electrical", "soc", ""},
            {"engine", "coolant_temp", ""},
            {"engine", "oil_pressure", ""},
            {"engine", "fuel_level", ""},
            // usv-722 helm / drive frames decoded by can_bridge: rudder
            // feedback from the steering actuator, key switch, gear, thrust
            // to the engine and J1939 DM1 faults.
            {"autopilot", "rudder_actual", ""},
            {"engine", "key_state", ""},
            {"engine", "gear", ""},
            {"engine", "throttle_pct", ""},
            {"engine", "active_dtcs", ""},
            {"nav", "satellites", ""},
            {"nav", "hdop", ""},
            {"ping", "result_code", "r.url == \"8.8.8.8\""},
            {"ping", "average_response_ms", "r.url == \"8.8.8.8\""},
            {"canbus", "interface_up", ""},
            {"canbus", "msg_rate", ""},
            {"camera", "connected", ""},
            {"camera_summary", "frigate_up", ""},
            {"camera_summary", "enabled", ""},
            {"camera_summary", "streaming", ""},
            {"cpu", "usage_user", "r.cpu == \"cpu-total\""},
            {"mem", "used_percent", ""},
            {"disk", "used_percent", "r.path == \"/\""},
            // last known course (survives a community restart, like the
            // Route tile; the check flags values older than 10 min)
            {"mission_state", "route_name", ""},
            {"mission", "waypoints_total", ""},
            {"mission", "region_vertices", ""},
        }, readiness_period_s_);

        MOOSTrace("grafana_bridge: influx=%s bucket=%s vessel=%s\n",
                  opts.url.c_str(), opts.bucket.c_str(), opts.vessel.c_str());
        MOOSTrace("grafana_bridge: %s engine_passthrough=%d\n",
                  engine_profile_.Describe().c_str(), engine_passthrough_ ? 1 : 0);
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
        Register("DB_CLIENTS", 0.0);
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
        FlushBehaviorEvent(now);

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

        // Pre-departure readiness, evaluated onboard (mission_logic.hpp).
        if (now - last_readiness_ >= readiness_period_s_) {
            last_readiness_ = now;
            WriteReadiness(now);
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
        if (ignore_vars_.count(key)) return;
        // MOOS own-ship navigation sanity. On usv-722 (2026-10-06)
        // iDivisorNMEA posted NAV_LAT/NAV_LONG = 0 and NAV_HEADING/NAV_SPEED
        // = 0 on every sentence while no RMC/HDT reached it, and
        // pNodeReporter turned 0,0 into NAV_X/NAV_Y at the equator: own ship
        // sat 3,300 km south and overwrote Signal K's position. A 0,0 fix is
        // never a position, so it is dropped; and while that is all MOOS
        // offers (no non-zero NAV_LAT for kOwnFixMaxAgeS) NAV_HEADING,
        // NAV_SPEED, NAV_X and NAV_Y are dropped too and own ship comes from
        // Signal K (WriteTraffic).
        if ((key == "NAV_LAT" || key == "NAV_LONG") && msg.IsDouble()) {
            if (msg.GetDouble() == 0.0) {
                last_zero_fix_ = now;
                if (!warned_zero_fix_) {
                    warned_zero_fix_ = true;
                    Event("traffic", "warning",
                          "MOOS NAV_LAT/NAV_LONG = 0 (no fix): own-ship NAV_* "
                          "ignored, using Signal K position/heading/speed. "
                          "Check the app posting NAV_LAT (iDivisorNMEA needs "
                          "RMC/HDT on its UDP feed).");
                }
                return;
            }
            if (key == "NAV_LAT") last_good_fix_ = now;
        }
        if ((key == "NAV_HEADING" || key == "NAV_SPEED" || key == "NAV_X" ||
             key == "NAV_Y") && MoosNavInvalid(now))
            return;

        // 1) Plain numeric passthrough (may fan out to several fields),
        //    rate-limited per field. Derived-quantity inputs are cached
        //    from every sample first.
        if (msg.IsDouble()) {
            if (kCachedVars.count(key)) CacheSample(key, msg.GetDouble(), now);
            // ENGINE_* (iCANRec) is written only when MOOS is this vessel's
            // engine source. ENGINE_<NAME>_<n> is engine instance n; every
            // engine point carries instance and engine_type tags.
            const bool engine_var = key.rfind("ENGINE_", 0) == 0;
            int inst = 0;
            const std::string base = engine_var ? usv::EngineVarBase(key, &inst) : key;
            const std::string isuf = inst ? "#" + std::to_string(inst) : "";
            const std::string etags =
                engine_var ? engine_profile_.Tags("instance=" + std::to_string(inst)) : "";
            if (engine_var && !engine_passthrough_) return;
            if (base == "ENGINE_LAMPS") {
                const int lamps = static_cast<int>(msg.GetDouble());
                if (lamps >= 0 && lamps <= 255) {
                    if (limiter_.allow("engine.fault_lamps" + isuf, now))
                        telemetry_->field("engine", etags, "fault_lamps", lamps);
                    for (const auto& lb : kLampBits)
                        if (limiter_.allow(std::string("engine.") + lb.field + isuf, now))
                            telemetry_->field("engine", etags, lb.field,
                                              ((lamps >> lb.shift) & 3) == 1 ? 1 : 0);
                }
                return;
            }
            auto range = kNumericVars.equal_range(base);
            for (auto it = range.first; it != range.second; ++it) {
                const FieldMap& fm = it->second;
                // Legacy PID_* producers take precedence over derived errors.
                if (key.rfind("PID_", 0) == 0)
                    external_fields_.insert(fm.field);
                const std::string k =
                    std::string(fm.measurement) + "." + fm.field + isuf;
                if (!limiter_.allow(k, now)) continue;
                const std::string tags =
                    (engine_var && std::string(fm.measurement) == "engine")
                        ? etags : std::string(fm.tags);
                telemetry_->field(fm.measurement, tags, fm.field,
                                  msg.GetDouble() * (fm.scale == kHelmSpeed
                                                     ? helm_speed_to_kn_ : fm.scale));
            }
        }

        // ENGINE_DTC_TEXT (iCANRec, "SPN n FMI m (xk); ..." or "NONE"):
        // the Mercury fault names are added on the mercury_diesel hull, the
        // J1939 failure-mode wording elsewhere. Written on change and every
        // 5 s so a dashboard window that opens later still has a value.
        if (msg.IsString() && key.rfind("ENGINE_DTC_TEXT", 0) == 0) {
            if (!engine_passthrough_) return;
            int inst = 0;
            usv::EngineVarBase(key, &inst);
            const std::string text = usv::j1939::EnrichDtcText(
                msg.GetString(), engine_profile_.type == "mercury_diesel");
            auto& st = dtc_text_[inst];
            if (text != st.first || now - st.second >= 5.0) {
                telemetry_->str_field("engine",
                                      engine_profile_.Tags("instance=" + std::to_string(inst)),
                                      "dtc_text", text);
                st = {text, now};
            }
            return;
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

        // 3) String passthrough, change-only (planchaPID re-posts its status
        //    strings every iterate at 20 Hz; the snapshot re-emits at 1 Hz).
        auto sp = kStringPassthrough.find(key);
        if (sp != kStringPassthrough.end() && msg.IsString()) {
            std::string& prev = last_str_[sp->second.field];
            if (prev != msg.GetString()) {
                prev = msg.GetString();
                telemetry_->str_field(sp->second.measurement, "",
                                      sp->second.field, prev);
                if (key == "ID_ROUTE") {
                    mission_id_ = prev;
                    last_str_["route_name"] = ResolveRouteName(prev);
                    telemetry_->str_field("mission_state", "", "route_name",
                                          last_str_["route_name"]);
                }
                if (key == "CRASH_STOP_STATUS" && prev == "braking")
                    Event("alarm", "critical",
                          "Crash stop: emergency braking engaged");
                if (key == "CORRIDOR_STATUS" && prev == "stop")
                    Event("alarm", "critical",
                          "Corridor limit reached: steering back inside");
            }
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
        } else if (key == "BASE_COORDS") {
            // "lat,lon": where the comms-loss failsafe returns the boat.
            double la = 0, lo = 0;
            if (usv::ParseLatLon(AsString(msg), &la, &lo)) {
                if (have_base_ && (la != base_lat_ || lo != base_lon_)) {
                    char m[96];
                    std::snprintf(m, sizeof(m), "Return base set to %.5f, %.5f", la, lo);
                    Event("mission", "info", m);
                }
                base_lat_ = la; base_lon_ = lo; have_base_ = true;
                telemetry_->field("mission", "", "base_lat", base_lat_);
                telemetry_->field("mission", "", "base_lon", base_lon_);
            }
        } else if (key == "SURVEY_ENTRY_GATE") {
            // iWebSocket re-posts this every iterate (20 Hz): change-only.
            const std::string gate = usv::lower(usv::trim(AsString(msg)));
            if (gate != flags_.entry_gate || !seen_entry_gate_) {
                seen_entry_gate_ = true;
                flags_.entry_gate = gate;
                telemetry_->str_field("mission_state", "", "entry_gate", gate);
            }
        } else if (key == "NODE_REPORT" && msg.IsString()) {
            HandleNodeReport(msg.GetString(), now);
        } else if (key == "RADAR_TARGET" && msg.IsString()) {
            HandleRadarTarget(msg.GetString(), now);
        } else if (key == "RADAR_TTM_RATE" && msg.IsDouble()) {
            radar_ttm_rate_ = msg.GetDouble();
            radar_ttm_time_ = now;
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
        } else if ((key == "OPR_POLY_ACTUAL" || key == "REGION_UPDATES") && msg.IsString()) {
            // OPR_POLY_ACTUAL: polygon posted by the OpRegion behaviour.
            // REGION_UPDATES: "core_poly=pts={x,y:...}" sent by the operator
            // console (iWebSocket) to BHV_OpRegionV24 when a region is
            // selected, already in the local grid. Either one is the
            // operating region; ParsePts reads the pts={} block of both.
            WriteGeometry("region", msg.GetString(), /*close=*/true);
        } else if (key == "CHECKLIST_STATUS" && msg.IsString()) {
            HandleChecklist(msg.GetString());
        } else if (key == "DB_CLIENTS" && msg.IsString()) {
            latest_.put_str("moos", "clients", msg.GetString(), now);
        } else if (key == "OBSTACLE_ALERT" && msg.IsString()) {
            const std::string id = usv::ParseObstacleId(msg.GetString());
            if (obstacles_.post(id, now))
                Event("obstacle", "warning",
                      "Obstacle avoidance active: " + id);
        } else if (key == "BHV_WARNING" && msg.IsString()) {
            HandleBhvNotice("warning", msg.GetString());
        } else if (key == "BHV_ERROR" && msg.IsString()) {
            HandleBhvNotice("critical", msg.GetString());
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
        else if (key == "MOOS_MANUAL_OVERRIDE" ||
                 key == "MOOS_MANUAL_OVERIDE")  slot = &flags_.manual_override;
        else if (key == "MISSION_ARMED")        slot = &flags_.armed;
        else if (key == "SURVEY_COMPLETE")      slot = &flags_.survey_complete;
        else if (key == "REMOTE_CONTROL")       slot = &flags_.remote_control;
        else if (key == "CRASH_STOP")           slot = &flags_.crash_stop;
        else if (key == "COMMS_LOSS")           slot = &flags_.comms_loss;
        else if (key == "RETURN_HOLD")          slot = &flags_.return_hold;
        if (!slot || *slot == val) return;
        *slot = val;

        if (key == "WAYPOINT" && val) {
            // Survey (re)start: reset the mission-scoped accumulators.
            mission_start_ = MonoSeconds();
            mission_start_ms_ = EpochMs();
            odom_.reset();
            flags_.survey_complete = false;
            char m[96];
            std::snprintf(m, sizeof(m), "Survey started (%d waypoints)",
                          survey_wpt_total_);
            MissionEvent(m, /*ended=*/false);
        } else if (key == "WAYPOINT" && !val) {
            // Survey ended (endflag, abort, or RTB): close the mission span
            // so the replay index knows its time range.
            if (mission_start_ms_ > 0.0)
                MissionEvent(flags_.survey_complete ? "Survey finished"
                                                    : "Survey ended",
                             /*ended=*/true);
        } else if (key == "REMOTE_CONTROL") {
            Event("state_change", "info",
                  val ? "Remote control engaged (operator has the helm)"
                      : "Remote control released — autonomy standby");
        } else if (key == "CRASH_STOP") {
            if (val) Event("alarm", "critical", "CRASH STOP commanded");
            else     Event("alarm", "info", "Crash stop cleared");
        } else if (key == "RETURN" && val) {
            if (flags_.comms_loss)
                Event("alarm", "critical",
                      "Comms failsafe: no Swiftships-UI link, returning to base");
            else
                Event("mission", "info", "Return-to-base started");
            if (mission_start_ == 0.0) mission_start_ = MonoSeconds();
        } else if (key == "COMMS_LOSS") {
            // iWebSocket: nothing heard from any Swiftships-UI for
            // comms_loss_timeout s. It returns to base when the helm is in
            // DRIVE or the boat is underway (RETURN follows), else stays.
            if (val)
                Event("alarm", "warning",
                      "Operator link lost: no frame from the Swiftships-UI");
            else
                Event("alarm", "info",
                      (flags_.returning || flags_.return_hold)
                          ? "Operator link restored; return / hold on the base "
                            "continues until the operator's next command"
                          : "Operator link restored");
        } else if (key == "RETURN_HOLD") {
            Event("mission", "info",
                  val ? (flags_.comms_loss
                             ? "Arrived at base after comms loss: holding station"
                             : "Arrived at base: holding station")
                      : "Hold on the base ended");
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

    // COMMS_LOST carries a phase (see usv::comms_phase): false / grace /
    // <action> / rtb. Only the hold phase derives the CommsLost state; the
    // others are surfaced as autonomy.comms_phase plus events.
    void HandleCommsLost(const std::string& raw) {
        const std::string v = usv::trim(raw);
        const int was = usv::comms_phase(flags_.comms_lost);
        const int is = usv::comms_phase(v);
        flags_.comms_lost = v;
        if (is == was) return;
        telemetry_->str_field("mission_state", "", "comms_failsafe",
                              is == 0 ? "ok" : usv::lower(v));
        switch (is) {
            case 1:
                Event("alarm", "warning",
                      "Operator link lost — grace period, mission continues");
                break;
            case 2:
                Event("alarm", "critical",
                      "Operator link LOST — failsafe '" + v +
                      "' engaged, mission disarmed");
                break;
            case 3:
                Event("alarm", "critical",
                      "Comms failsafe: returning to base");
                break;
            default:
                Event("alarm", "info",
                      was == 1
                          ? "Operator link restored within grace period"
                          : flags_.armed
                                ? "Operator link restored"
                                : "Operator link restored — survey held "
                                  "until operator re-arm");
                break;
        }
    }

    // Mission span events carry epoch-ms bounds and the mission id as a tag
    // so the Replay dashboard can list missions and jump to their range.
    void MissionEvent(const std::string& message, bool ended) {
        std::string tags = "severity=info,source=moos,type=mission";
        if (!mission_id_.empty()) tags += ",mission_id=" + EscapeTag(mission_id_);
        events_->str_field("events", tags, "message", message);
        events_->field("events", tags, "value", 1);
        events_->field("events", tags, "start_ms", mission_start_ms_);
        if (ended) {
            events_->field("events", tags, "end_ms", EpochMs());
            mission_start_ms_ = 0.0;
        }
        events_->flush();
    }

    static std::string EscapeTag(const std::string& s) {
        std::string out;
        for (char c : s) {
            if (c == ',' || c == '=' || c == ' ') out += '\\';
            out += c;
        }
        return out;
    }

    // Derived-quantity inputs, updated from every sample (before limiting).
    void CacheSample(const std::string& key, double v, double now) {
        if (key == "NAV_SPEED") last_speed_mps_ = v * helm_speed_to_mps_;
        else if (key == "NAV_X") { nav_x_ = v; have_x_ = true; }
        else if (key == "NAV_Y") {
            nav_y_ = v; have_y_ = true;
            if (have_x_) odom_.add(nav_x_, v);   // odometry
        }
        else if (key == "NAV_HEADING") nav_hdg_ = v;
        else if (key == "DESIRED_HEADING") { desired_hdg_ = v; have_desired_hdg_ = true; }
        else if (key == "DESIRED_SPEED") { desired_spd_ = v * helm_speed_to_mps_; have_desired_spd_ = true; }

        // Heading / speed error: planchaPID publishes neither (the PID_*
        // names belong to pMarinePID), so derive them at 2 Hz unless an
        // external producer has appeared.
        if ((key == "NAV_HEADING" || key == "NAV_SPEED") &&
            limiter_.allow("autopilot.error", now)) {
            if (have_desired_hdg_ && !external_fields_.count("heading_error"))
                telemetry_->field("autopilot", "", "heading_error",
                                  usv::HeadingError(desired_hdg_, nav_hdg_));
            if (have_desired_spd_ && !external_fields_.count("speed_error"))
                telemetry_->field("autopilot", "", "speed_error",
                                  (desired_spd_ - last_speed_mps_) * MS_TO_KN);
        }
    }

    // NODE_REPORT: "NAME=contato_26,TYPE=ship,TIME=...,X=..,Y=..,SPD=..,HDG=.."
    // (iRadarTargets / pContactSpawn / AIS). Position: LAT/LON converted
    // with this mission's datum when the report carries them, else X/Y.
    // Range and CPA are computed in WriteTraffic against own ship.
    void HandleNodeReport(const std::string& report, double now) {
        auto kv = usv::ParseKeyValuesLower(report);
        auto name = kv.find("name");
        if (name == kv.end() || name->second.empty()) return;
        if (name->second == mission_name_ || name->second == own_name_) return;
        auto num = [&](const char* k, double dflt) {
            auto it = kv.find(k);
            if (it == kv.end() || it->second.empty()) return dflt;
            try { return std::stod(it->second); } catch (...) { return dflt; }
        };
        auto x = kv.find("x"), y = kv.find("y");
        const bool have_xy = x != kv.end() && y != kv.end();
        // Prefer LAT/LON converted with this mission's datum: the app that
        // posts AIS contacts may compute its X/Y from a different origin
        // (or none), which shifts every contact by the datum difference.
        double lat = 0.0, lon = 0.0;
        const bool have_ll = have_datum_ && usv::ReportLatLon(kv, &lat, &lon);
        if (!have_xy && !have_ll) {
            contacts_.post(name->second, now, -1);
            return;
        }
        double cx = num("x", 0.0), cy = num("y", 0.0);
        if (have_ll) {
            const usv::XY p = usv::LatLonToLocal(lat, lon, lat0_, lon0_);
            if (have_xy && !warned_report_datum_ &&
                std::hypot(p.x - cx, p.y - cy) > kDatumMismatchM) {
                warned_report_datum_ = true;
                char m[256];
                std::snprintf(m, sizeof(m),
                              "NODE_REPORT %s: X/Y differ from LAT/LON by %.0f km; "
                              "the posting app uses another datum. Using LAT/LON.",
                              name->second.c_str(),
                              std::hypot(p.x - cx, p.y - cy) / 1000.0);
                Event("traffic", "warning", m);
                std::fprintf(stderr, "grafana_bridge: %s\n", m);
            }
            cx = p.x; cy = p.y;
        }
        std::string type;
        if (auto t = kv.find("type"); t != kv.end()) type = t->second;
        contacts_.post_report(name->second, now, cx, cy,
                              num("spd", -1.0), num("hdg", -1.0), type);
    }

    // RADAR_TARGET (proposed iRadarTargets addition):
    // "id=26,range=1066.8,bearing=133.8,spd=5.7,cog=232.7,cpa=350.1,tcpa=138"
    void HandleRadarTarget(const std::string& spec, double now) {
        auto kv = usv::ParseKeyValuesLower(spec);
        auto id = kv.find("id");
        if (id == kv.end() || id->second.empty()) return;
        auto num = [&](const char* k) {
            auto it = kv.find(k);
            if (it == kv.end() || it->second.empty()) return -1.0;
            try { return std::stod(it->second); } catch (...) { return -1.0; }
        };
        const double cpa = num("cpa");
        const double spd = num("spd");
        contacts_.post_radar(id->second, now, num("range"), num("bearing"),
                             spd >= 0 ? spd * KN_TO_MS : -1.0, num("cog"), cpa, num("tcpa"));
        if (cpa >= 0 && cpa < radar_cpa_alert_m_ &&
            alerted_contacts_.insert(id->second).second) {
            char m[128];
            std::snprintf(m, sizeof(m),
                          "Radar contact %s CPA %.0f m (TCPA %.0f s)",
                          id->second.c_str(), cpa, num("tcpa"));
            Event("radar", "warning", m);
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

    // Helm behavior diagnostics (BHV_WARNING / BHV_ERROR). The class of
    // failure the Alpha mission has actually hit — e.g. the compound
    // condition the IvP parser rejected (MALCONFIG, alpha.bhv §AvdColregs
    // note) — surfaces here first. pHelmIvP may re-post the same text every
    // iteration, so only a CHANGED text is event-worthy.
    void HandleBhvNotice(const char* severity, const std::string& text) {
        const std::string t = usv::trim(text);
        if (t.empty() || t == last_bhv_notice_) return;
        last_bhv_notice_ = t;
        Event("alarm", severity, "Helm behavior: " + t);
    }

    void HandleHelmSummary(const std::string& summary) {
        if (external_active_behavior_) return;
        const std::string active = usv::ParseHelmBehaviors(summary);
        WriteActiveBehavior(active.empty() ? "none" : active);
        // Store the raw summary too (debug value for engineers).
        telemetry_->str_field("mission_state", "", "helm_summary", summary);
    }

    // The field follows the helm immediately; the EVENT is posted only once
    // the new set has held for kBehaviorSettleS, because behaviors such as
    // station-keep run intermittently (inner radius) and would otherwise
    // flood the mission log with none / station-keep pairs.
    static constexpr double kBehaviorSettleS = 5.0;

    void WriteActiveBehavior(const std::string& active) {
        if (active == last_active_behavior_) return;
        last_active_behavior_ = active;
        telemetry_->str_field("mission_state", "", "active_behavior", active);
        pending_behavior_ = active;
        pending_behavior_since_ = MonoSeconds();
    }

    void FlushBehaviorEvent(double now) {
        if (pending_behavior_since_ <= 0.0) return;
        if (now - pending_behavior_since_ < kBehaviorSettleS) return;
        pending_behavior_since_ = 0.0;
        if (pending_behavior_ == announced_behavior_) return;
        announced_behavior_ = pending_behavior_;
        Event("state_change", "info", "Active behaviors: " + announced_behavior_);
    }

    void HandleGeofence(const std::string& val, double now) {
        const std::string v = usv::lower(usv::trim(val));
        // poly = hard breach; recovering = beyond save_dist, being steered
        // back. Levels chosen so "breach" (1) is the alertable condition.
        geofence_.post(v == "poly" ? 1 : 2, now);
        // Event emission happens in WriteSnapshot on level transitions
        // (also covers the decay back to 0 = recovered).
    }

    // Planned route / operating region as lat-lon vertex sets for the
    // Navigation, Replay and Fleet maps (measurement "geometry", tag kind,
    // one series per vertex index, all vertices of a set share one
    // timestamp). The previous set of the same kind is deleted first so a
    // shorter route never leaves stale vertices behind. The region polygon
    // is closed (first vertex repeated) so a route layer draws its outline.
    void WriteGeometry(const std::string& kind, const std::string& spec,
                       bool close) {
        std::string& prev = last_geometry_[kind];
        if (spec == prev) return;
        prev = spec;
        if (!have_datum_) return;
        std::vector<usv::XY> pts = usv::ParsePts(spec);
        if (kind == "route") route_pts_ = pts;
        telemetry_->erase("_measurement=\"geometry\" AND kind=\"" + kind + "\"");
        if (kind == "region") {
            region_pts_ = pts;
            telemetry_->field("mission", "", "region_vertices",
                              static_cast<double>(pts.size()));
        }
        if (kind == "route" && !pts.empty())
            telemetry_->field("mission", "", "route_length_m", usv::RouteLengthM(pts));
        if (!route_pts_.empty() && region_pts_.size() >= 3)
            telemetry_->field("mission", "", "route_in_region",
                              usv::RouteInsideRegion(route_pts_, region_pts_) ? 1.0 : 0.0);
        if (pts.empty()) { telemetry_->flush(); return; }   // "NONE": cleared
        if (close) pts.push_back(pts.front());
        const long long ts = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        for (size_t i = 0; i < pts.size(); ++i) {
            const usv::LatLon ll = usv::LocalToLatLon(pts[i].x, pts[i].y, lat0_, lon0_);
            char tags[64];
            std::snprintf(tags, sizeof tags, "kind=%s,idx=%03zu", kind.c_str(), i);
            telemetry_->field("geometry", tags, "lat", ll.lat, ts);
            telemetry_->field("geometry", tags, "lon", ll.lon, ts);
            telemetry_->field("geometry", tags, "seq", static_cast<double>(i), ts);
        }
        telemetry_->flush();
    }

    // Per-contact rows (measurement "contacts", tags contact/source) plus the
    // "traffic" summary and the legacy "radar" aggregates, 1 Hz. Contacts
    // are only written while active, so a dashboard query over the last
    // 30 s sees exactly the live picture.
    void WriteTraffic(double now) {
        usv::OwnShip own;
        own.valid = have_x_ && have_y_;
        own.x = nav_x_; own.y = nav_y_; own.spd_mps = last_speed_mps_; own.hdg = nav_hdg_;
        // MOOS NAV_* not usable (see HandleMessage): heading and speed from
        // Signal K, position only from the Signal K fix below.
        if (MoosNavInvalid(now)) {
            own.valid = false;
            const usv::Sample* h = latest_.get("nav", "heading", now, kOwnFixMaxAgeS);
            const usv::Sample* v = latest_.get("nav", "sog", now, kOwnFixMaxAgeS);
            if (h && !h->is_str) own.hdg = h->num;
            own.spd_mps = (v && !v->is_str) ? v->num * KN_TO_MS : 0.0;
        }
        // Own position in the same frame as LAT/LON contacts: a fresh own
        // fix (NAV_LAT/NAV_LONG, or Signal K's nav.lat/lon via the poller)
        // converted with the mission datum, so own ship and contacts never
        // sit on two different origins.
        if (have_datum_) {
            const usv::Sample* la = latest_.get("nav", "lat", now, kOwnFixMaxAgeS);
            const usv::Sample* lo = latest_.get("nav", "lon", now, kOwnFixMaxAgeS);
            if (la && lo && !la->is_str && !lo->is_str &&
                !(la->num == 0.0 && lo->num == 0.0) &&
                std::fabs(la->num) <= 90.0 && std::fabs(lo->num) <= 180.0) {
                const usv::XY p = usv::LatLonToLocal(la->num, lo->num, lat0_, lon0_);
                own.x = p.x; own.y = p.y; own.valid = true;
            }
        }
        contacts_.update_geometry(own, radar_cpa_alert_m_, tcpa_alert_s_);
        // AIS cannot hear a ship 100 nm away: a reported contact that far
        // out means own ship and contacts sit on different datums. Warn
        // once per run so the cause shows up on the Alarms dashboard.
        if (!warned_far_contact_) {
            for (const auto& [id, c] : contacts_.contacts()) {
                if (c.source == "report" && c.range_m > kImplausibleContactM) {
                    warned_far_contact_ = true;
                    char m[256];
                    std::snprintf(m, sizeof(m),
                                  "Contact %s reported %.0f nm away: own ship and "
                                  "contacts use different datums (check LatOrigin/"
                                  "LongOrigin of the app posting NODE_REPORT)",
                                  id.c_str(), c.range_m / 1852.0);
                    Event("traffic", "warning", m);
                    std::fprintf(stderr, "grafana_bridge: %s\n", m);
                    break;
                }
            }
        }
        const int n = contacts_.active(now);
        if (n == 0) alerted_contacts_.clear();

        int reports = 0, radar_only = 0, high = 0, medium = 0;
        double min_cpa = -1, min_tcpa = -1, closest = -1;
        for (const auto& [id, c] : contacts_.contacts()) {
            if (c.source == "report") ++reports; else ++radar_only;
            if (c.risk == 2) ++high; else if (c.risk == 1) ++medium;
            // Closing contacts rank first; an opening contact's CPA is its
            // current range (ComputeCpa), reported with TCPA -1 = opening.
            const bool closing = c.tcpa_s >= 0, best_closing = min_tcpa >= 0;
            if (c.cpa_m >= 0 && (min_cpa < 0 || (closing && !best_closing) ||
                                 (closing == best_closing && c.cpa_m < min_cpa))) {
                min_cpa = c.cpa_m; min_tcpa = c.tcpa_s;
            }
            if (c.range_m >= 0 && (closest < 0 || c.range_m < closest)) closest = c.range_m;
            const std::string tags = "contact=" + EscapeTag(id) +
                                     ",source=" + c.source;
            if (c.range_m >= 0) telemetry_->field("contacts", tags, "range_m", c.range_m);
            if (c.bearing >= 0) {
                telemetry_->field("contacts", tags, "bearing", c.bearing);
                telemetry_->field("contacts", tags, "rel_bearing", c.rel_bearing);
            }
            if (c.spd_mps >= 0) telemetry_->field("contacts", tags, "spd_kn", c.spd_mps * MS_TO_KN);
            if (c.hdg >= 0) telemetry_->field("contacts", tags, "hdg", c.hdg);
            if (c.rel_speed_mps >= 0)
                telemetry_->field("contacts", tags, "rel_speed_kn", c.rel_speed_mps * MS_TO_KN);
            if (c.cpa_m >= 0) telemetry_->field("contacts", tags, "cpa_m", c.cpa_m);
            telemetry_->field("contacts", tags, "tcpa_s", c.tcpa_s);   // -1 = opening
            telemetry_->field("contacts", tags, "risk", c.risk);
            telemetry_->str_field("contacts", tags, "encounter",
                                  c.encounter.empty() ? "-" : c.encounter);
            telemetry_->str_field("contacts", tags, "type", c.type.empty() ? "unknown" : c.type);
            // One event per contact entering HIGH risk; cleared when it drops.
            if (c.risk == 2) {
                if (alerted_contacts_.insert(id).second) {
                    char m[160];
                    std::snprintf(m, sizeof m, "Collision risk HIGH: %s CPA %.0f m, TCPA %.0f s (%s)",
                                  id.c_str(), c.cpa_m, c.tcpa_s,
                                  c.encounter.empty() ? "radar" : c.encounter.c_str());
                    Event("traffic", "critical", m);
                }
            } else {
                alerted_contacts_.erase(id);
            }
        }

        const bool avoidance = last_active_behavior_.find("avd") != std::string::npos ||
                               last_active_behavior_.find("colreg") != std::string::npos ||
                               last_active_behavior_.find("avoid") != std::string::npos;
        const int risk_level = high ? 2 : (medium ? 1 : 0);
        telemetry_->field("traffic", "", "contacts", n);
        telemetry_->field("traffic", "", "reports", reports);
        telemetry_->field("traffic", "", "radar_only", radar_only);
        telemetry_->field("traffic", "", "high_risk", high);
        telemetry_->field("traffic", "", "risk_level", risk_level);
        telemetry_->field("traffic", "", "avoidance_active", avoidance ? 1 : 0);
        // -1 = nothing to report: the tiles map it to "none" instead of
        // holding the last real value for the whole dashboard window.
        telemetry_->field("traffic", "", "closest_range_m", closest >= 0 ? closest : -1.0);
        telemetry_->field("traffic", "", "min_cpa_m", min_cpa >= 0 ? min_cpa : -1.0);
        telemetry_->field("traffic", "", "min_tcpa_s", min_cpa >= 0 ? min_tcpa : -1.0);
        const std::string worst = contacts_.worst_id();
        telemetry_->str_field("traffic", "", "worst_contact", worst.empty() ? "-" : worst);
        // MOOS action: the course the route asks for (bearing to the next
        // waypoint), what the helm commands and what the boat does.
        const bool route_active = own.valid && (flags_.waypoint || flags_.returning) &&
                                  !route_pts_.empty() && last_wpt_index_ >= 0 &&
                                  last_wpt_index_ < static_cast<int>(route_pts_.size());
        if (route_active) {
            const usv::XY& wp = route_pts_[last_wpt_index_];
            const double brg = std::fmod(std::atan2(wp.x - own.x, wp.y - own.y) * 180.0 / M_PI + 360.0, 360.0);
            telemetry_->field("traffic", "", "planned_course", brg);
            if (have_desired_hdg_)
                telemetry_->field("traffic", "", "course_deviation",
                                  usv::WrapDeg180(desired_hdg_ - brg));
        } else {
            telemetry_->field("traffic", "", "planned_course", -1.0);   // no route
            telemetry_->field("traffic", "", "course_deviation", -999.0);
        }
        telemetry_->str_field("traffic", "", "reason",
                              avoidance ? "Collision avoidance active" :
                              high ? "High-risk contact, helm not avoiding" :
                              medium ? "Contact closing (MEDIUM), monitoring" :
                              n ? "Traffic monitored, no avoidance needed" : "No contacts");

        const bool heartbeat = now - radar_ttm_time_ <= 30.0;
        const bool recent = now - contacts_.last_seen() <= 30.0;
        if (heartbeat || recent || radar_seen_) {
            radar_seen_ = true;
            telemetry_->field("radar", "", "online", (heartbeat || recent) ? 1.0 : 0.0);
            telemetry_->field("radar", "", "targets", n);
            if (closest >= 0) telemetry_->field("radar", "", "closest_range_m", closest);
            if (min_cpa >= 0) {
                telemetry_->field("radar", "", "cpa_m", min_cpa);
                telemetry_->field("radar", "", "tcpa_s", min_tcpa);
            }
            if (heartbeat) telemetry_->field("radar", "", "ttm_rate", radar_ttm_rate_);
        }
    }

    // Console pre-departure checklist (manual items ticked by the operator),
    // mirrored on the Readiness dashboard. Stored and re-emitted at 1 Hz.
    void HandleChecklist(const std::string& spec) {
        const usv::Checklist c = usv::ParseChecklist(spec);
        if (c.total < 0) return;
        checklist_ = c;
        have_checklist_ = true;
        if (spec != last_checklist_spec_) {
            last_checklist_spec_ = spec;
            char m[160];
            std::snprintf(m, sizeof m, "Console checklist %d/%d%s%s", c.done, c.total,
                          c.by.empty() ? "" : " by ", c.by.c_str());
            Event("state_change", "info", m);
        }
        WriteChecklist();
    }

    // mission_state.checklist_* (count, pending list, operator) plus one row
    // per line of the Readiness dashboard's checklist table (measurement
    // "checklist", tag seq). The row set is erased when the spec changes so
    // items ticked since disappear; rows are re-emitted every readiness
    // period so the table window stays fresh.
    void WriteChecklist() {
        if (!have_checklist_) {
            // No console checklist on this vessel: one explanatory row so
            // the Readiness checklist panel is not an empty placeholder.
            // Rewritten every period: the panel reads the last 60 s only.
            if (!readiness_cfg_.console_checklist) {
                if (checklist_rows_written_ != 1) {
                    telemetry_->erase("_measurement=\"checklist\"");
                    checklist_rows_written_ = 1;
                }
                telemetry_->field("checklist", "seq=000", "seq", 0.0);
                telemetry_->str_field("checklist", "seq=000", "item", "Console checklist");
                telemetry_->str_field("checklist", "seq=000", "description", "not published by this console");
                telemetry_->str_field("checklist", "seq=000", "state", "N/A");
            }
            return;
        }
        telemetry_->field("mission_state", "", "checklist_done", checklist_.done);
        telemetry_->field("mission_state", "", "checklist_total", checklist_.total);
        telemetry_->str_field("mission_state", "", "checklist_pending",
                              checklist_.pending.empty() ? "-" : checklist_.pending);
        telemetry_->str_field("mission_state", "", "checklist_by",
                              checklist_.by.empty() ? "-" : checklist_.by);
        const auto rows = usv::ChecklistRows(checklist_, readiness_cfg_.checklist_items);
        if (rows.size() != checklist_rows_written_) {
            telemetry_->erase("_measurement=\"checklist\"");
            checklist_rows_written_ = rows.size();
        }
        for (size_t i = 0; i < rows.size(); ++i) {
            char tags[24];
            std::snprintf(tags, sizeof tags, "seq=%03zu", i);
            telemetry_->field("checklist", tags, "seq", static_cast<double>(i));
            telemetry_->str_field("checklist", tags, "item", rows[i].item);
            telemetry_->str_field("checklist", tags, "description",
                                  rows[i].description.empty() ? "-" : rows[i].description);
            telemetry_->str_field("checklist", tags, "state", rows[i].state);
        }
    }

    // One row per check (measurement "readiness", tag check) plus the
    // summary the tiles read, every readiness_period_s. Values other
    // producers write come from the InfluxDB poller; their age is carried
    // over so a stale series still reads as missing.
    void WriteReadiness(double now) {
        for (const auto& [key, lv] : poller_.snapshot()) {
            const size_t dot = key.find('.');
            if (dot == std::string::npos) continue;
            const std::string m = key.substr(0, dot), f = key.substr(dot + 1);
            // Never let a polled (older) value replace a fresher own sample.
            const usv::Sample* own = latest_.get(m, f, now, 1e9);
            if (own && own->t >= now - lv.age_s) continue;
            if (lv.is_str) latest_.put_str(m, f, lv.str, now - lv.age_s);
            else latest_.put(m, f, lv.value, now - lv.age_s);
        }
        WriteChecklist();
        const auto checks = usv::EvaluateReadiness(latest_, now, readiness_strict_, readiness_cfg_);
        const usv::ReadinessSummary sum = usv::SummarizeReadiness(checks);
        for (const auto& c : checks) {
            const std::string tags = "check=" + c.id;
            telemetry_->field("readiness", tags, "seq", c.seq);
            telemetry_->field("readiness", tags, "status", c.status);
            telemetry_->str_field("readiness", tags, "name", c.name);
            telemetry_->str_field("readiness", tags, "evidence", c.evidence);
        }
        telemetry_->field("readiness_summary", "", "ready", sum.ready ? 1 : 0);
        telemetry_->field("readiness_summary", "", "passed", sum.passed);
        telemetry_->field("readiness_summary", "", "failed", sum.failed);
        telemetry_->field("readiness_summary", "", "na", sum.na);
        telemetry_->field("readiness_summary", "", "console", sum.console);
        telemetry_->field("readiness_summary", "", "checks", static_cast<double>(checks.size()));
        telemetry_->str_field("readiness_summary", "", "blocking",
                              sum.blocking.empty() ? "-" : sum.blocking);
        telemetry_->str_field("readiness_summary", "", "profile",
                              readiness_strict_ ? "vessel" : "sim");
        // Change-only event so the Mission Events list shows when the boat
        // became ready and what blocked it.
        const std::string state = sum.ready ? "READY" : "NOT READY: " +
            (sum.blocking.empty() ? std::string("console checklist / ENC not published")
                                  : sum.blocking);
        if (state != last_readiness_state_) {
            last_readiness_state_ = state;
            Event("readiness", sum.ready ? "info" : "warning", "Readiness " + state);
        }
    }

    void HandleSeglist(const std::string& spec) {
        const usv::SeglistInfo info = usv::ParseSeglist(spec);
        if (info.points <= 0) return;
        seglists_[info.label] = info.points;
        // The survey (or return) route is the planned track to draw.
        if (info.label.find("survey") != std::string::npos ||
            info.label.find("return") != std::string::npos)
            WriteGeometry("route", spec, /*close=*/false);
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
        telemetry_->field("autonomy", "", "comms_phase",
                          usv::comms_phase(flags_.comms_lost));
        telemetry_->field("autonomy", "", "remote_control",
                          b(flags_.remote_control));
        telemetry_->field("autonomy", "", "crash_stop", b(flags_.crash_stop));
        // Operator link for both iWebSocket variants (usv::operator_link):
        // the tiles read this; comms_phase / comms_lost keep their meaning.
        telemetry_->field("autonomy", "", "operator_link", usv::operator_link(flags_));
        telemetry_->field("autonomy", "", "comms_loss", b(flags_.comms_loss));
        telemetry_->field("autonomy", "", "comms_return", b(usv::comms_return(flags_)));
        telemetry_->field("autonomy", "", "return_hold", b(flags_.return_hold));
        // Return base re-written so last() over any window finds it.
        if (have_base_) {
            telemetry_->field("mission", "", "base_lat", base_lat_);
            telemetry_->field("mission", "", "base_lon", base_lon_);
        }

        // Derived state written periodically too, so the state timeline is
        // continuous and last() panels never go stale. Same for the
        // change-only strings and the mission identity.
        telemetry_->str_field("mission_state", "", "state", last_state_);
        for (const char* f : {"arrival_state", "corridor_status",
                              "crash_stop_status", "mission_id", "route_name",
                              "region_warning"}) {
            auto it = last_str_.find(f);
            if (it != last_str_.end() && !it->second.empty())
                telemetry_->str_field("mission_state", "", f, it->second);
        }
        WriteChecklist();
        // Change-only helm / mission strings: re-emit so last() over any
        // dashboard time range finds a value (they may not change for hours).
        if (!helm_state_str_.empty())
            telemetry_->str_field("mission_state", "", "helm_state", helm_state_str_);
        telemetry_->str_field("mission_state", "", "allstop_reason", allstop_str_);
        if (!last_active_behavior_.empty())
            telemetry_->str_field("mission_state", "", "active_behavior",
                                  last_active_behavior_);
        if (seen_entry_gate_)
            telemetry_->str_field("mission_state", "", "entry_gate", flags_.entry_gate);
        telemetry_->str_field("mission_state", "", "comms_failsafe",
                              usv::comms_phase(flags_.comms_lost) == 0
                                  ? "ok" : usv::lower(flags_.comms_lost));
        if (!external_station_keep_)
            telemetry_->str_field("mission_state", "", "station_keep",
                                  flags_.stationing ? "true" : "false");
        if (!mission_name_.empty())
            telemetry_->str_field("mission_state", "", "mission_name",
                                  mission_name_);

        // Traffic (FR-6): contact geometry from own-ship state, per-contact
        // rows for the Traffic dashboard, radar aggregates for the alert
        // rules. "online" = a TTM rate heartbeat within 30 s, or any contact
        // update within 30 s (simulation has no TTM heartbeat).
        WriteTraffic(now);

        // Derived mission quantities — each yields to an external producer.
        const bool wpt_active = flags_.waypoint || flags_.returning;
        if (!external_fields_.count("waypoints_total") &&
            survey_wpt_total_ > 0) {
            telemetry_->field("mission", "", "waypoints_total",
                              survey_wpt_total_);
            if (last_wpt_index_ >= 0)
                telemetry_->field("mission", "", "waypoints_remaining",
                                  std::max(0, survey_wpt_total_ - last_wpt_index_));
        }
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
    std::string pending_behavior_, announced_behavior_;
    double pending_behavior_since_ = 0.0;
    std::string last_bhv_notice_;

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
    double helm_speed_to_kn_ = MS_TO_KN;   // nav_speed_units: mps (default) or knots
    double helm_speed_to_mps_ = 1.0;
    double base_lat_ = 0.0, base_lon_ = 0.0;   // BASE_COORDS
    bool have_base_ = false;
    double nav_x_ = 0.0, nav_y_ = 0.0;
    bool have_x_ = false, have_y_ = false;
    double nav_hdg_ = 0.0, desired_hdg_ = 0.0, desired_spd_ = 0.0;
    bool have_desired_hdg_ = false, have_desired_spd_ = false;
    double mission_start_ = 0.0;      // monotonic, for elapsed_s
    double mission_start_ms_ = 0.0;   // epoch ms, for the replay index

    // Deployment configuration (OnStartUp).
    usv::RateLimiter limiter_{5.0};
    usv::EngineProfile engine_profile_;
    bool engine_passthrough_ = true;
    std::map<int, std::pair<std::string, double>> dtc_text_;   // per instance: last text, time
    std::set<std::string> ignore_vars_;
    std::string mission_name_;
    std::string mission_id_;
    std::string signalk_http_ = "http://localhost:3000";
    bool have_datum_ = false;
    // Traffic frame: a NODE_REPORT whose X/Y and LAT/LON disagree by more
    // than this is posted with another datum (warned once per run); an own
    // LAT/LON fix older than kOwnFixMaxAgeS falls back to NAV_X/NAV_Y.
    static constexpr double kDatumMismatchM = 1000.0;
    static constexpr double kOwnFixMaxAgeS = 10.0;
    bool warned_report_datum_ = false;
    static constexpr double kImplausibleContactM = 100.0 * 1852.0;
    bool warned_far_contact_ = false;
    // MOOS own-ship validity: last NAV_LAT/NAV_LONG of 0 and last real fix.
    double last_zero_fix_ = -1e18, last_good_fix_ = -1e18;
    bool warned_zero_fix_ = false;
    bool MoosNavInvalid(double now) const {
        return now - last_zero_fix_ < kOwnFixMaxAgeS &&
               now - last_good_fix_ > kOwnFixMaxAgeS;
    }
    double lat0_ = 0.0, lon0_ = 0.0;
    std::map<std::string, std::string> last_geometry_;
    std::string signalk_token_;   // optional bearer token when SK security is on

    // The console starts a mission with ID_ROUTE=<Signal K route uuid>; the
    // operator knows the route by its name, so look it up once per change
    // (one bounded GET on the MOOS thread, 800 ms worst case). Falls back
    // to a readable short id when Signal K is unreachable.
    std::string ResolveRouteName(const std::string& id) {
        std::string name;
        if (!signalk_http_.empty() && !id.empty() &&
            usv::lower(id) != "return_to_base") {
            // Signal K server v2 serves resources under /signalk/v2/...; v1
            // keys routes by "urn:mrn:signalk:uuid:<uuid>". Try each shape
            // and say in the log which one answered (journalctl -u <antler>).
            const std::string cands[] = {
                signalk_http_ + "/signalk/v2/api/resources/routes/" + id,
                signalk_http_ + "/signalk/v1/api/resources/routes/" + id,
                signalk_http_ + "/signalk/v1/api/resources/routes/urn:mrn:signalk:uuid:" + id,
            };
            for (const auto& url : cands) {
                long status = 0;
                const std::string body = usv::HttpGet(url, 800, signalk_token_, &status);
                name = usv::JsonStringMember(body, "name");
                std::fprintf(stderr, "[grafana_bridge] route lookup %s -> HTTP %ld%s%s\n",
                             url.c_str(), status,
                             name.empty() ? "" : " name=", name.c_str());
                if (!name.empty()) break;
            }
        }
        return usv::RouteDisplayName(id, name);
    }
    double radar_cpa_alert_m_ = 500.0;
    double tcpa_alert_s_ = 300.0;
    std::string own_name_;                 // Community: own NODE_REPORT is not a contact
    usv::Checklist checklist_;
    bool have_checklist_ = false;
    std::string last_checklist_spec_;
    size_t checklist_rows_written_ = 0;
    usv::LatestStore latest_;              // latest value per field for the readiness checks
    usv::LatestPoller poller_;             // fields written by other producers
    bool readiness_strict_ = true;
    usv::ReadinessConfig readiness_cfg_;
    std::vector<usv::XY> region_pts_;
    double readiness_period_s_ = 5.0;
    double last_readiness_ = 0.0;
    std::string last_readiness_state_;
    std::vector<usv::XY> route_pts_;       // planned route (local grid) for the planned course

    // Change-only string cache: field -> last written value.
    std::map<std::string, std::string> last_str_;
    bool seen_entry_gate_ = false;

    // Radar / contacts.
    usv::ContactTracker contacts_{10.0};
    std::set<std::string> alerted_contacts_;
    double radar_ttm_rate_ = 0.0, radar_ttm_time_ = -1e18;
    bool radar_seen_ = false;

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
