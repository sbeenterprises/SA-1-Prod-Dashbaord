// Unit tests for mission_logic.hpp — the Alpha mission state machine and
// parsers. No framework dependency; builds and runs anywhere with a C++17
// compiler:  g++ -std=c++17 mission_logic_test.cpp -o t && ./t
//
// Scenarios follow the semantics documented in alpha.bhv (comms-loss
// failsafe, explicit re-arm, start-outside entry gate, park-on-complete).

#include "mission_logic.hpp"

#include <cstdio>
#include <cstring>
#include <string>

static int g_pass = 0, g_fail = 0;

#define CHECK(expr)                                                       \
    do {                                                                  \
        if (expr) {                                                       \
            ++g_pass;                                                     \
        } else {                                                          \
            ++g_fail;                                                     \
            std::printf("  [FAIL] %s:%d  %s\n", __FILE__, __LINE__,       \
                        #expr);                                           \
        }                                                                 \
    } while (0)

#define CHECK_EQ_STR(got, want)                                           \
    do {                                                                  \
        const std::string g_ = (got), w_ = (want);                        \
        if (g_ == w_) {                                                   \
            ++g_pass;                                                     \
        } else {                                                          \
            ++g_fail;                                                     \
            std::printf("  [FAIL] %s:%d  got \"%s\" want \"%s\"\n",       \
                        __FILE__, __LINE__, g_.c_str(), w_.c_str());      \
        }                                                                 \
    } while (0)

using namespace usv;

static void test_flag_parsing() {
    CHECK(truthy("true"));
    CHECK(truthy("TRUE"));
    CHECK(truthy(" 1 "));
    CHECK(!truthy("false"));
    CHECK(!truthy(""));
    CHECK(!truthy("open"));

    CHECK(!comms_lost_active("false"));
    CHECK(!comms_lost_active("FALSE"));
    CHECK(!comms_lost_active(""));
    CHECK(comms_lost_active("loiter"));       // failsafe action in force
    CHECK(comms_lost_active("station_keep"));
    CHECK(comms_lost_active("true"));

    CHECK(gate_open("open"));
    CHECK(gate_open(""));       // no bridge / no region -> behaves normally
    CHECK(!gate_open("closed"));
    CHECK(!gate_open(" CLOSED "));
}

static void test_allstop_classification() {
    CHECK(!allstop_unexpected("clear"));
    CHECK(!allstop_unexpected(""));
    CHECK(!allstop_unexpected("ManualOverride"));   // parked on purpose
    CHECK(!allstop_unexpected("NothingToDo"));      // idle, no behavior
    CHECK(allstop_unexpected("MissingDecVars:course"));  // the pause freeze
    CHECK(allstop_unexpected("NoIvPFunctions"));
    CHECK(allstop_unexpected("BehaviorError"));
}

static void test_state_machine() {
    MissionFlags f;  // defaults = alpha.bhv initialize lines
    CHECK_EQ_STR(DeriveMissionState(f), "Idle");

    // Operator starts the survey.
    f.deploy = f.waypoint = true;
    CHECK_EQ_STR(DeriveMissionState(f), "Autonomous");

    // Pause during survey (pause_hold_speed/heading active).
    f.pause = true;
    CHECK_EQ_STR(DeriveMissionState(f), "Paused");
    f.pause = false;

    // Mid-route station-keep hold (planchaPID posts STATIONING on arrival).
    f.stationing = true;
    CHECK_EQ_STR(DeriveMissionState(f), "StationKeeping");

    // Comms loss: iWebSocket posts the failsafe action and clears the
    // re-arm latch; boat station-keeps at the loss position. CommsLost must
    // dominate the concurrent STATIONING.
    f.comms_lost = "loiter";
    f.armed = false;
    CHECK_EQ_STR(DeriveMissionState(f), "CommsLost");

    // Link restored: COMMS_LOST=false alone must NOT resume the survey.
    f.comms_lost = "false";
    f.stationing = false;
    CHECK_EQ_STR(DeriveMissionState(f), "AwaitingRearm");
    CHECK(awaiting_rearm(f));

    // Operator AUTONOMOUS_RESUME re-arms explicitly.
    f.armed = true;
    CHECK_EQ_STR(DeriveMissionState(f), "Autonomous");
    CHECK(!awaiting_rearm(f));

    // Operator requests return-to-base.
    f.waypoint = false;
    f.returning = true;
    CHECK_EQ_STR(DeriveMissionState(f), "ReturnToBase");

    // Pause gates waypt_return too (condition PAUSE=false).
    f.pause = true;
    CHECK_EQ_STR(DeriveMissionState(f), "Paused");
    f.pause = false;

    // Arrival at base: endflags RETURN=false + MOOS_MANUAL_OVERRIDE=true.
    f.returning = false;
    f.manual_override = true;
    CHECK_EQ_STR(DeriveMissionState(f), "Parked");

    // Operator takes the helm (REMOTE_CONTROL passthrough parks the helm
    // underneath): Remote, not Parked; comms loss still dominates.
    f.remote_control = true;
    CHECK_EQ_STR(DeriveMissionState(f), "Remote");
    f.comms_lost = "loiter";
    CHECK_EQ_STR(DeriveMissionState(f), "CommsLost");
    f.comms_lost = "false";
    f.remote_control = false;
}

static void test_helm_behaviors_fallback() {
    // Real pHelmIvP summary from the s1_morgan_city_signalk log: survey and
    // op-region running, no objective function this iteration.
    const std::string summ =
        "iter=107712,utc_time=1788337837.13,ofnum=0,active_goal=false,"
        "running_bhvs=waypt_survey$1788337837.13$10/10:opr24$1788310807.00$2/2,"
        "active_bhvs=none";
    CHECK_EQ_STR(ParseActiveBehaviors(summ), "");
    CHECK_EQ_STR(ParseHelmBehaviors(summ), "waypt_survey, opr24");
    CHECK_EQ_STR(ParseHelmBehaviors("iter=1,running_bhvs=none,active_bhvs=none"), "");
    CHECK_EQ_STR(ParseHelmBehaviors("iter=1,running_bhvs=a$1$1,active_bhvs=b$1$1"), "a");
    CHECK_EQ_STR(ParseHelmBehaviors("iter=1,active_bhvs=b$1$1"), "b");
}

static void test_geometry() {
    auto pts = ParsePts("pts={-6862,302.2:-6762.7,261.5:-6517.9,472.2},label=alpha_waypt_survey");
    CHECK(pts.size() == 3);
    CHECK(std::fabs((pts[1].x) - (-6762.7)) < 1e-9);
    CHECK(std::fabs((pts[2].y) - (472.2)) < 1e-9);
    CHECK(ParsePts("NONE").empty());
    CHECK(ParsePts("pts={}").empty());
    CHECK(ParsePts("pts={1,2:garbage}").empty());
    // Datum of s1_morgan_city: 1000 m north ~ 0.00904 deg, 1000 m east
    // ~ 0.01035 deg at 29.7 N.
    LatLon ll = LocalToLatLon(1000.0, 1000.0, 29.71970895, -91.14705165);
    CHECK(std::fabs((ll.lat - 29.71970895) - (0.00904)) < 0.00005);
    CHECK(std::fabs((ll.lon + 91.14705165) - (0.01035)) < 0.00005);
    LatLon o = LocalToLatLon(0.0, 0.0, 29.7, -91.1);
    CHECK(std::fabs((o.lat) - (29.7)) < 1e-12);
    CHECK(std::fabs((o.lon) - (-91.1)) < 1e-12);
}

static void test_traffic_geometry() {
    // Own ship at origin heading north 5 m/s; contact 1000 m east heading
    // west at 5 m/s: crossing, closes to CPA ~707 m after ~100 s.
    double cpa = 0, tcpa = 0;
    ComputeCpa(0, 0, 5, 0, 1000, 0, 5, 270, &cpa, &tcpa);
    CHECK(std::fabs(cpa - 707.1) < 1.0);
    CHECK(std::fabs(tcpa - 100.0) < 0.5);
    // Head-on: contact 2000 m north heading south at 5 m/s -> CPA 0 in 200 s.
    ComputeCpa(0, 0, 5, 0, 0, 2000, 5, 180, &cpa, &tcpa);
    CHECK(cpa < 0.01 && std::fabs(tcpa - 200.0) < 0.5);
    // Opening contact: CPA is the current range, tcpa < 0.
    ComputeCpa(0, 0, 5, 0, 0, -500, 5, 180, &cpa, &tcpa);
    CHECK(std::fabs(cpa - 500.0) < 0.01 && tcpa < 0);
    CHECK_EQ_STR(EncounterType(0, 180), "head-on");
    CHECK_EQ_STR(EncounterType(0, 10), "overtaking");
    CHECK_EQ_STR(EncounterType(0, 90), "crossing");
    CHECK_EQ_STR(EncounterType(350, 200), "head-on");
    CHECK(RiskLevel(80, 100, 500, 300) == 2);
    CHECK(RiskLevel(600, 100, 500, 300) == 1);
    CHECK(RiskLevel(80, 500, 500, 300) == 1);
    CHECK(RiskLevel(2000, 100, 500, 300) == 0);
    CHECK(RiskLevel(80, -1, 500, 300) == 0);   // opening

    ContactTracker t(10.0);
    OwnShip own; own.valid = true; own.x = 0; own.y = 0; own.spd_mps = 5; own.hdg = 0;
    t.post_report("TGT-27", 1.0, 1000, 0, 5, 270, "ship");
    t.post_report("FAR", 1.0, 0, 5000, 0, 0, "buoy");
    t.post("R-1", 1.0, 3000, 2500, 900);      // radar-only, opening slowly
    t.update_geometry(own, 500, 300);
    const auto& c = t.contacts().at("TGT-27");
    CHECK(std::fabs(c.bearing - 90.0) < 0.01);
    CHECK(std::fabs(c.rel_bearing - 90.0) < 0.01);
    CHECK_EQ_STR(c.encounter, "crossing");
    CHECK_EQ_STR(c.source, "report");
    CHECK(c.risk == 1);    // CPA 707 m < 1000 m, TCPA 100 s
    CHECK_EQ_STR(t.contacts().at("R-1").source, "radar");
    CHECK_EQ_STR(t.worst_id(), "TGT-27");
    CHECK(std::fabs(t.closest_range() - 1000.0) < 0.01);
    // Radar-only target with polar data: relative bearing and encounter
    // from own heading, no position needed.
    t.post_radar("R-31", 1.0, 900, 319, 1.5, 139, 350, 140);
    t.update_geometry(own, 500, 300);
    const auto& r = t.contacts().at("R-31");
    CHECK(std::fabs(r.rel_bearing - (-41.0)) < 0.01);
    CHECK_EQ_STR(r.encounter, "crossing");
    CHECK(r.risk == 2);   // CPA 350 < 500 within 140 s
    CHECK_EQ_STR(t.worst_id(), "R-31");
}

static void test_checklist() {
    Checklist c = ParseChecklist("done=9,total=11,pending=Weather Conditions|Port Clearance,by=douglas");
    CHECK(c.done == 9 && c.total == 11);
    CHECK_EQ_STR(c.pending, "Weather Conditions|Port Clearance");
    CHECK_EQ_STR(c.by, "douglas");
    Checklist d = ParseChecklist("done=11,total=11,pending=,by=ops");
    CHECK(d.done == 11 && d.pending.empty());
    Checklist e = ParseChecklist("garbage");
    CHECK(e.done == -1 && e.total == -1);
}

static void test_route_naming() {
    const std::string body =
        "{\"name\":\"Som Rt0\",\"description\":\"x\",\"feature\":{\"type\":\"Feature\"}}";
    CHECK_EQ_STR(JsonStringMember(body, "name"), "Som Rt0");
    CHECK_EQ_STR(JsonStringMember(body, "description"), "x");
    CHECK_EQ_STR(JsonStringMember(body, "missing"), "");
    CHECK_EQ_STR(JsonStringMember("{ \"name\" : \"A \\\"B\\\" C\" }", "name"), "A \"B\" C");
    CHECK_EQ_STR(JsonStringMember("{\"name\": 42}", "name"), "");
    CHECK_EQ_STR(JsonStringMember("", "name"), "");
    CHECK_EQ_STR(RouteDisplayName("7a860f96-36e5-4eb3-8a14-1", "Som Rt0"), "Som Rt0");
    CHECK_EQ_STR(RouteDisplayName("7a860f96-36e5-4eb3-8a14-1", ""), "7a860f96");
    CHECK_EQ_STR(RouteDisplayName("return_to_base", ""), "Return to base");
    CHECK_EQ_STR(RouteDisplayName("survey7", ""), "survey7");
}

static void test_survey_completion() {
    MissionFlags f;
    f.deploy = f.waypoint = true;

    // Finite survey ends: endflags WAYPOINT=false, SURVEY_COMPLETE=true.
    // Boat may still be recovering inside the region before iWebSocket
    // parks it — Completed either way.
    f.waypoint = false;
    f.survey_complete = true;
    CHECK_EQ_STR(DeriveMissionState(f), "Completed");
    f.manual_override = true;  // parked once inside
    CHECK_EQ_STR(DeriveMissionState(f), "Completed");
}

static void test_entry_gate() {
    MissionFlags f;
    // Mission started with the boat OUTSIDE the region: gate closed, survey
    // suppressed, opr24 recovers the boat inside first.
    f.waypoint = true;
    f.entry_gate = "closed";
    CHECK_EQ_STR(DeriveMissionState(f), "EnteringRegion");

    // Gate latches open on first entry and stays open for the mission.
    f.entry_gate = "open";
    CHECK_EQ_STR(DeriveMissionState(f), "Autonomous");
}

static void test_comms_lost_dominates() {
    MissionFlags f;
    f.comms_lost = "loiter";
    f.waypoint = true;
    f.pause = true;
    f.manual_override = true;
    CHECK_EQ_STR(DeriveMissionState(f), "CommsLost");
}

static void test_helm_summary_parsing() {
    const std::string summary =
        "iter=482,utc_time=1755400000.1,ofnum=3,var=course:speed,"
        "active_bhvs=waypt_survey$0.60$100$4$0.00$0/0$1:"
        "opr24$0.60$300$5$0.00$0/0$1:avoid_obstacle_ob_2$0.5$350$3$0$0/0$1,"
        "running_bhvs=none,idle_bhvs=station_keep:waypt_return,"
        "completed_bhvs=none";
    CHECK_EQ_STR(ParseActiveBehaviors(summary),
                 "waypt_survey, opr24, avoid_obstacle_ob_2");

    CHECK_EQ_STR(ParseActiveBehaviors("iter=1,active_bhvs=none"), "");
    CHECK_EQ_STR(ParseActiveBehaviors(""), "");
    CHECK_EQ_STR(ParseActiveBehaviors("garbage-without-keys"), "");
    // Single behavior, no '$' fields.
    CHECK_EQ_STR(ParseActiveBehaviors("active_bhvs=waypt_survey"),
                 "waypt_survey");
}

static void test_seglist_parsing() {
    auto s = ParseSeglist(
        "pts={-6409,144:-6380,200:-6300,180:-6250,260},"
        "label=alpha_waypt_survey,edge_color=white,vertex_size=3");
    CHECK(s.points == 4);
    CHECK_EQ_STR(s.label, "alpha_waypt_survey");

    auto r = ParseSeglist("pts={10,20},label=alpha_waypt_return");
    CHECK(r.points == 1);
    CHECK_EQ_STR(r.label, "alpha_waypt_return");

    CHECK(ParseSeglist("pts={}").points == 0);
    CHECK(ParseSeglist("").points == 0);
    CHECK(ParseSeglist("label=only").points == 0);
}

static void test_obstacle_id_parsing() {
    CHECK_EQ_STR(ParseObstacleId("name=ob_12#pts={1,2:3,4},label=ob_12"),
                 "ob_12");
    CHECK_EQ_STR(ParseObstacleId("x=5,y=9,label=wreck_3,range=40"),
                 "wreck_3");
    CHECK_EQ_STR(ParseObstacleId("  bare_id  "), "bare_id");
}

static void test_decay_latch() {
    DecayLatch g(30.0);
    CHECK(g.level(1000.0) == 0);           // nothing posted yet
    g.post(1, 1000.0);                     // OPR_BREACHED=poly
    CHECK(g.level(1000.0) == 1);
    CHECK(g.level(1029.0) == 1);           // held within the quiet window
    g.post(2, 1010.0);                     // OPR_BREACHED=recovering
    CHECK(g.level(1035.0) == 2);
    CHECK(g.level(1041.0) == 0);           // decayed 30 s after last post
}

static void test_obstacle_tracker() {
    ObstacleTracker t(120.0);
    CHECK(t.post("ob_1", 0.0));            // new -> event
    CHECK(!t.post("ob_1", 50.0));          // refresh, not new
    CHECK(t.post("ob_2", 60.0));
    CHECK(t.active(100.0) == 2);
    CHECK(t.active(175.0) == 1);           // ob_1 expired (last seen 50)
    CHECK(t.active(400.0) == 0);
    CHECK(t.post("ob_1", 500.0));          // re-alert after expiry -> event
}

static void test_odometry() {
    OdometryIntegrator o;
    o.add(0, 0);
    o.add(3, 4);                           // +5 m
    CHECK(std::abs(o.total() - 5.0) < 1e-9);
    o.add(500, 500);                       // GPS jump -> guarded, not added
    CHECK(std::abs(o.total() - 5.0) < 1e-9);
    o.add(503, 504);                       // +5 m from the new fix
    CHECK(std::abs(o.total() - 10.0) < 1e-9);
    o.reset();
    CHECK(o.total() == 0.0);
}

static void test_derived_quantities() {
    CHECK(std::abs(ProgressPct(3, 6) - 50.0) < 1e-9);
    CHECK(ProgressPct(9, 6) == 100.0);     // clamped (repeat routes cycle)
    CHECK(ProgressPct(2, 0) == 0.0);       // no seglist yet
    CHECK(std::abs(EtaSeconds(100.0, 5.0) - 20.0) < 1e-9);
    CHECK(std::abs(EtaSeconds(100.0, 0.0) - 500.0) < 1e-9);  // speed floor
}

static void test_key_values_continuation() {
    // A value containing a comma-free '=' -less continuation segment must
    // re-attach to the previous key rather than corrupt the map.
    auto kv = ParseKeyValues("a=1,b=x,y,c=3");
    CHECK_EQ_STR(kv["a"], "1");
    CHECK_EQ_STR(kv["b"], "x,y");
    CHECK_EQ_STR(kv["c"], "3");
}


// COMMS_LOST phases (iWebSocket comms watchdog): grace keeps the mission
// running, <action> is the hold, rtb hands over to the return behavior.
static void test_comms_phases() {
    CHECK(comms_phase("false") == 0);
    CHECK(comms_phase("") == 0);
    CHECK(comms_phase("grace") == 1);
    CHECK(comms_phase("GRACE") == 1);
    CHECK(comms_phase("loiter") == 2);
    CHECK(comms_phase("stop") == 2);
    CHECK(comms_phase("rtb") == 3);
    CHECK(!comms_lost_active("grace"));       // survey still driving
    CHECK(!comms_lost_active("rtb"));         // waypt_return driving

    MissionFlags f;
    f.waypoint = true;
    f.comms_lost = "grace";
    CHECK_EQ_STR(DeriveMissionState(f), "Autonomous");
    f.comms_lost = "loiter";
    f.armed = false;
    f.stationing = true;
    CHECK_EQ_STR(DeriveMissionState(f), "CommsLost");
    f.comms_lost = "rtb";                     // iWebSocket posts RETURN=true
    f.stationing = false;
    f.returning = true;
    CHECK_EQ_STR(DeriveMissionState(f), "ReturnToBase");
    // awaiting_rearm only once the link is back and the hold is released
    f.comms_lost = "false";
    f.returning = false;
    CHECK(awaiting_rearm(f));
}

static void test_heading_error() {
    CHECK(std::abs(HeadingError(10, 350) - 20.0) < 1e-9);    // wrap starboard
    CHECK(std::abs(HeadingError(350, 10) + 20.0) < 1e-9);    // wrap port
    CHECK(std::abs(HeadingError(90, 90)) < 1e-9);
    CHECK(std::abs(HeadingError(0, 180) + 180.0) < 1e-9);    // [-180, 180)
    CHECK(std::abs(HeadingError(270, 45) + 135.0) < 1e-9);
}

static void test_split_csv() {
    auto v = SplitCsv(" NAV_YAW, NAV_PITCH ,,GYRO ");
    CHECK(v.size() == 3);
    CHECK_EQ_STR(v[0], "NAV_YAW");
    CHECK_EQ_STR(v[1], "NAV_PITCH");
    CHECK_EQ_STR(v[2], "GYRO");
    CHECK(SplitCsv("").empty());
}

static void test_rate_limiter() {
    RateLimiter r(5.0);                      // default 200 ms
    r.set_rate("autopilot.error", 2.0);      // 500 ms
    CHECK(r.allow("nav.sog", 100.00));
    CHECK(!r.allow("nav.sog", 100.10));
    CHECK(r.allow("nav.sog", 100.25));
    CHECK(r.allow("nav.heading", 100.10));   // independent key
    CHECK(r.allow("autopilot.error", 1.0));
    CHECK(!r.allow("autopilot.error", 1.4));
    CHECK(r.allow("autopilot.error", 1.5));
    RateLimiter unlimited(0.0);              // 0 Hz = no limiting
    CHECK(unlimited.allow("x", 1.0));
    CHECK(unlimited.allow("x", 1.0));
}

static void test_contact_tracker() {
    ContactTracker t(10.0);
    CHECK(t.active(0.0) == 0);
    CHECK(t.closest_range() < 0);
    t.post("c1", 0.0, 800.0);
    t.post("c2", 1.0, 300.0, 250.0, 120.0);
    t.post("c3", 2.0, 1500.0, 900.0, 600.0);
    CHECK(t.active(2.0) == 3);
    CHECK(std::abs(t.closest_range() - 300.0) < 1e-9);
    CHECK(std::abs(t.worst_cpa().cpa_m - 250.0) < 1e-9);
    CHECK(std::abs(t.worst_cpa().tcpa_s - 120.0) < 1e-9);
    CHECK(std::abs(t.last_seen() - 2.0) < 1e-9);
    t.post("c1", 5.0, 700.0);                // refresh keeps it alive
    CHECK(t.active(12.5) == 1);              // c2 (t=1), c3 (t=2) expired; c1 seen at 5
    CHECK(std::abs(t.closest_range() - 700.0) < 1e-9);
    CHECK(t.worst_cpa().cpa_m < 0);          // c1 never had a CPA
    CHECK(t.active(20.0) == 0);

    auto kv = ParseKeyValuesLower("NAME=contato_26,TYPE=ship,X=120.5,Y=-40");
    CHECK_EQ_STR(kv["name"], "contato_26");
    CHECK_EQ_STR(kv["x"], "120.5");
}

static const ReadinessCheck& find_check(const std::vector<ReadinessCheck>& v, const char* id) {
    for (const auto& c : v) if (c.id == id) return c;
    static ReadinessCheck none;
    return none;
}

static void test_readiness() {
    LatestStore s;
    const double now = 1000.0;
    // Simulation host: position, route, region, helm, MOOS, Signal K,
    // host metrics, no engine / CAN / camera data, console silent.
    s.put("nav", "lat", 29.7, now - 1);
    s.put_str("mission_state", "route_name", "Som Rt0", now - 50);
    s.put("mission", "waypoints_total", 4, now - 5);
    s.put("mission", "region_vertices", 5, now - 500);
    s.put("comms", "moos_msg_rate", 61, now - 3);
    s.put_str("mission_state", "helm_state", "PARK", now - 1);
    s.put("autonomy", "allstop", 0, now - 1);
    s.put("comms", "signalk_ws_up", 1, now - 4);
    s.put("ping", "result_code", 0, now - 20);
    s.put("ping", "average_response_ms", 38, now - 20);
    s.put("autonomy", "comms_phase", 0, now - 1);
    s.put("cpu", "usage_user", 4, now - 8);
    s.put("mem", "used_percent", 11, now - 8);
    s.put("disk", "used_percent", 63, now - 8);
    s.put("autonomy", "geofence_breached", 0, now - 1);
    s.put("autonomy", "crash_stop", 0, now - 1);
    s.put("traffic", "risk_level", 0, now - 1);
    s.put("radar", "online", 1, now - 2);

    auto v = EvaluateReadiness(s, now, false);
    CHECK(v.size() == 16);
    CHECK(find_check(v, "apps").status == READY_NA);          // no app list configured
    CHECK(find_check(v, "endurance").status == READY_NA);     // no vessel constants
    CHECK(find_check(v, "gps").status == READY_PASS);      // no quality data, sim
    CHECK_EQ_STR(find_check(v, "course").evidence, "Som Rt0 · 4 waypoints · region 5 vertices");
    CHECK(find_check(v, "course").status == READY_PASS);
    CHECK(find_check(v, "enc").status == READY_CONSOLE);
    CHECK(find_check(v, "autonomy").status == READY_PASS);
    CHECK_EQ_STR(find_check(v, "autonomy").evidence, "MOOS 61 msg/s · helm PARK");
    CHECK(find_check(v, "steering").status == READY_NA);
    CHECK(find_check(v, "comms").status == READY_PASS);
    CHECK_EQ_STR(find_check(v, "comms").evidence, "Signal K UP · Internet UP · 38 ms");
    CHECK(find_check(v, "can").status == READY_NA);
    s.put("canbus", "interface_up", 0, now);
    CHECK(find_check(EvaluateReadiness(s, now, false), "can").status == READY_NA);   // sim host, no hardware
    CHECK(find_check(EvaluateReadiness(s, now, true), "can").status == READY_FAIL);
    CHECK(find_check(v, "engine").status == READY_NA);
    CHECK(find_check(v, "electrical").status == READY_NA);
    CHECK(find_check(v, "ais").status == READY_PASS);         // radar online, no reports
    CHECK_EQ_STR(find_check(v, "ais").evidence, "no contact reports (5 min) · radar online");
    CHECK(find_check(v, "cameras").status == READY_NA);
    CHECK(find_check(v, "obc").status == READY_PASS);
    CHECK(find_check(v, "safety").status == READY_PASS);
    CHECK(find_check(v, "checklist").status == READY_CONSOLE);
    ReadinessSummary r = SummarizeReadiness(v);
    CHECK(!r.ready && r.failed == 0 && r.console == 2 && r.na == 7 && r.passed == 7);
    CHECK(r.blocking.empty());

    // Strict (vessel) profile: missing sensors fail and GPS quality is required.
    auto w = EvaluateReadiness(s, now, true);
    CHECK(find_check(w, "gps").status == READY_FAIL);
    CHECK(find_check(w, "engine").status == READY_FAIL);
    CHECK(find_check(w, "can").status == READY_FAIL);
    CHECK(find_check(w, "ais").status == READY_PASS);   // empty harbour is not a fault
    ReadinessSummary rw = SummarizeReadiness(w);
    CHECK(!rw.ready && rw.failed >= 5);
    CHECK(rw.blocking.find("Engine pre-start") != std::string::npos);

    // Console publishes, sensors arrive, everything passes.
    s.put_str("mission_state", "enc_status", "selected:US5LA31M", now);
    s.put("mission_state", "checklist_done", 11, now);
    s.put("mission_state", "checklist_total", 11, now);
    s.put("nav", "satellites", 14, now);
    s.put("nav", "hdop", 0.8, now);
    s.put_str("nav", "fix_type", "RTK Fixed", now);
    s.put("autopilot", "rudder_actual", 2.0, now);
    s.put("autopilot", "rudder_cmd", 3.0, now);
    s.put("canbus", "interface_up", 1, now);
    s.put("canbus", "msg_rate", 212, now);
    s.put("engine", "coolant_temp", 41, now);
    s.put("engine", "oil_pressure", 310, now);
    s.put("engine", "fuel_level", 82, now);
    s.put("electrical", "battery_voltage", 25.6, now);
    s.put("camera", "connected", 1, now);
    s.put("traffic", "reports", 2, now);
    s.put("engine", "active_dtcs", 0, now);
    s.put("electrical", "soc", 80, now);
    s.put("mission", "route_length_m", 5926, now);      // 3.2 nm
    s.put("mission", "route_in_region", 1, now);
    s.put_str("moos", "clients", "MOOSDB,pHelmIvP,pMarinePID,grafana_bridge,iWebSocket", now);
    ReadinessConfig cfg;
    cfg.fuel_tank_l = 200; cfg.cruise_fuel_lph = 12; cfg.cruise_speed_kn = 6;
    cfg.battery_kwh = 10; cfg.cruise_power_kw = 1.5;
    cfg.required_apps = {"pHelmIvP", "pMarinePID", "grafana_bridge", "iWebSocket"};
    auto x = EvaluateReadiness(s, now, true, cfg);
    ReadinessSummary rx = SummarizeReadiness(x);
    CHECK(rx.ready && rx.failed == 0 && rx.console == 0);
    CHECK_EQ_STR(find_check(x, "apps").evidence, "4 / 4 running");
    CHECK_EQ_STR(find_check(x, "endurance").evidence,
                 "route 3.2 nm needs 0.8 h (x1.5) · fuel 13.7 h · battery 5.3 h");
    CHECK_EQ_STR(find_check(x, "course").evidence,
                 "Som Rt0 · 4 waypoints · 3.2 nm · region 5 vertices · route inside region");
    CHECK(find_check(x, "engine").evidence.find("no DTC") != std::string::npos);
    CHECK_EQ_STR(find_check(x, "gps").evidence, "RTK Fixed · 14 sats · HDOP 0.8");
    CHECK_EQ_STR(find_check(x, "steering").evidence, "rudder 2.0° · cmd 3.0°");
    CHECK_EQ_STR(find_check(x, "can").evidence, "interface UP · 212 msg/s");
    CHECK_EQ_STR(find_check(x, "engine").evidence, "coolant 41 °C · oil 310 kPa · fuel 82 % · no DTC");
    CHECK(find_check(x, "ais").status == READY_PASS);

    // Faults: rudder not following, low fuel, pending checklist, HIGH risk,
    // stale position, a missing app, active DTCs, route leaving the region,
    // fuel short for the route.
    s.put_str("moos", "clients", "MOOSDB,pHelmIvP,grafana_bridge", now);
    s.put("engine", "active_dtcs", 2, now);
    s.put("mission", "route_in_region", 0, now);
    s.put("autopilot", "rudder_actual", 20.0, now);
    s.put("engine", "fuel_level", 9, now);
    s.put("mission_state", "checklist_done", 9, now);
    s.put_str("mission_state", "checklist_pending", "Weather Conditions|Port Clearance", now);
    s.put("traffic", "risk_level", 2, now);
    s.put("nav", "lat", 29.7, now - 60);
    auto y = EvaluateReadiness(s, now, true, cfg);
    CHECK(find_check(y, "apps").status == READY_FAIL);
    CHECK_EQ_STR(find_check(y, "apps").evidence, "2 / 4 running · missing: pMarinePID, iWebSocket");
    cfg.optional_apps = {"iWebSocket"};
    CHECK_EQ_STR(find_check(EvaluateReadiness(s, now, true, cfg), "apps").evidence,
                 "2 / 3 running · missing: pMarinePID · optional not running: iWebSocket");
    cfg.required_apps = {"pHelmIvP", "grafana_bridge", "pMarineViewer"};
    cfg.optional_apps = {"pMarineViewer"};
    CHECK(find_check(EvaluateReadiness(s, now, true, cfg), "apps").status == READY_PASS);
    cfg.required_apps = {"pHelmIvP", "pMarinePID", "grafana_bridge", "iWebSocket"};
    cfg.optional_apps.clear();
    CHECK(find_check(y, "engine").evidence.find("2 ACTIVE DTC") != std::string::npos);
    CHECK(find_check(y, "course").status == READY_FAIL);
    CHECK(find_check(y, "course").evidence.find("ROUTE OUTSIDE REGION") != std::string::npos);
    CHECK(find_check(y, "endurance").status == READY_PASS);   // fuel 9 % is still 1.5 h for a 0.8 h plan
    s.put("engine", "fuel_level", 3, now);                     // 0.5 h: short
    CHECK(find_check(EvaluateReadiness(s, now, true, cfg), "endurance").status == READY_FAIL);
    s.put("engine", "fuel_level", 9, now);
    CHECK(find_check(y, "steering").status == READY_FAIL);
    CHECK(find_check(y, "steering").evidence.find("NOT FOLLOWING") != std::string::npos);
    CHECK(find_check(y, "engine").status == READY_FAIL);
    CHECK(find_check(y, "checklist").status == READY_FAIL);
    CHECK_EQ_STR(find_check(y, "checklist").evidence, "9 / 11 ticked · pending: Weather Conditions,Port Clearance");
    CHECK(find_check(y, "safety").status == READY_FAIL);
    CHECK_EQ_STR(find_check(y, "safety").evidence, "COLLISION RISK HIGH");
    CHECK(find_check(y, "gps").status == READY_FAIL);
    CHECK_EQ_STR(find_check(y, "gps").evidence, "no position");
    ReadinessSummary ry = SummarizeReadiness(y);
    CHECK(!ry.ready && ry.failed == 7);
    CHECK_EQ_STR(ry.blocking, "GPS position fix; Course planning; MOOS apps; Steering / actuators; Engine pre-start; Safety flags; Console checklist");

    // GPS via Signal K: VectorNav INS mode mapped to fix_type by signalk_bridge
    // (position polled back from InfluxDB). Only "INS tracking" is a fix.
    {
        LatestStore g;
        g.put("nav", "lat", 29.72, now);
        g.put("nav", "lon", -91.21, now);
        g.put_str("nav", "fix_type", "INS aligning", now);
        CHECK(find_check(EvaluateReadiness(g, now, true), "gps").status == READY_FAIL);
        CHECK_EQ_STR(find_check(EvaluateReadiness(g, now, true), "gps").evidence, "INS aligning");
        g.put_str("nav", "fix_type", "GNSS lost", now);
        CHECK(find_check(EvaluateReadiness(g, now, true), "gps").status == READY_FAIL);
        g.put_str("nav", "fix_type", "INS tracking", now);
        CHECK(find_check(EvaluateReadiness(g, now, true), "gps").status == READY_PASS);
        CHECK_EQ_STR(find_check(EvaluateReadiness(g, now, true), "gps").evidence, "INS tracking");
        g.put_str("nav", "fix_type", "no fix", now);
        CHECK(find_check(EvaluateReadiness(g, now, true), "gps").status == READY_FAIL);
    }

    // Course planning falls back to the last known route (flagged after 10 min)
    s.put_str("mission_state", "route_name", "Rt WP Alpha", now - 1200);
    CHECK_EQ_STR(find_check(EvaluateReadiness(s, now, true), "course").evidence,
                 "Rt WP Alpha (last known) · 4 waypoints · 3.2 nm · region 5 vertices · ROUTE OUTSIDE REGION");

    // Console checklist rows: short form (pending names only)
    auto rows = ChecklistRows(ParseChecklist("done=9,total=11,pending=Weather Conditions|Port Clearance,by=douglas"));
    CHECK(rows.size() == 3);
    CHECK_EQ_STR(rows[0].item, "Weather Conditions");
    CHECK_EQ_STR(rows[0].state, "PENDING");
    CHECK_EQ_STR(rows[2].item, "Console checklist 9 / 11");
    CHECK_EQ_STR(rows[2].state, "OPEN");
    auto done = ChecklistRows(ParseChecklist("done=11,total=11,pending=,by=ops"));
    CHECK(done.size() == 1 && done[0].state == "COMPLETE");
    CHECK(ChecklistRows(ParseChecklist("garbage")).empty());
    // Short form + configured item list: everything not pending is ticked
    auto items = ParseChecklistItems("Chart Navigation System:Verify charts are loaded and current|"
                                     "GPS Position Fix:Confirm accurate GPS positioning|"
                                     "Weather Conditions:Current and forecast weather acceptable|"
                                     "Port Clearance:Harbor control and departure clearance");
    CHECK(items.size() == 4 && items[1].description == "Confirm accurate GPS positioning");
    auto full = ChecklistRows(ParseChecklist("done=2,total=4,pending=Weather Conditions|Port Clearance,by=douglas"), items);
    CHECK(full.size() == 5);
    CHECK(full[0].item == "Chart Navigation System" && full[0].state == "TICKED");
    CHECK_EQ_STR(full[0].description, "Verify charts are loaded and current");
    CHECK(full[2].item == "Weather Conditions" && full[2].state == "PENDING");
    CHECK_EQ_STR(full[4].item, "Console checklist 2 / 4");
    CHECK_EQ_STR(full[4].description, "ticked by douglas");
    // Full form from the console (items=Name:state:Description|...), commas in descriptions
    Checklist cf = ParseChecklist("done=1,total=2,by=ops,items=Engine Pre-Start Check:ticked:Oil, coolant, and fuel levels verified|Port Clearance:pending:Harbor control");
    CHECK(cf.items.size() == 2);
    CHECK(cf.items[0].ticked && cf.items[0].description == "Oil, coolant, and fuel levels verified");
    CHECK(!cf.items[1].ticked);
    auto cfr = ChecklistRows(cf, items);   // console list wins over config
    CHECK(cfr.size() == 3 && cfr[0].state == "TICKED" && cfr[1].state == "PENDING");

    // Geometry helpers and the Antler app list
    std::vector<XY> box = {{0, 0}, {100, 0}, {100, 100}, {0, 100}};
    CHECK(PointInPolygon(50, 50, box) && !PointInPolygon(150, 50, box));
    CHECK(RouteInsideRegion({{10, 10}, {90, 90}}, box));
    CHECK(!RouteInsideRegion({{10, 10}, {190, 90}}, box));
    CHECK(std::fabs(RouteLengthM({{0, 0}, {300, 400}, {300, 0}}) - 900.0) < 1e-9);
    auto apps = ParseAntlerApps({"MSBetweenLaunches = 200", "Run = MOOSDB @ NewConsole = false",
                                 "Run = pHelmIvP @ NewConsole = false", "Run = uTimerScript @ NewConsole=false ~ uTimerScript_A",
                                 "// Run = pOld @ NewConsole = false", "Run = grafana_bridge @ NewConsole = false // dashboard"});
    CHECK(apps.size() == 3);
    CHECK_EQ_STR(apps[0], "pHelmIvP");
    CHECK_EQ_STR(apps[1], "uTimerScript_A");
    CHECK_EQ_STR(apps[2], "grafana_bridge");
    auto miss = MissingApps(apps, "MOOSDB,pHelmIvP,grafana_bridge");
    CHECK(miss.size() == 1 && miss[0] == "uTimerScript_A");

    // LatestStore age test
    CHECK(s.get("nav", "lat", now, 30.0) == nullptr);
    CHECK(s.get("nav", "lat", now, 120.0) != nullptr);
    CHECK(s.get("nav", "nothing", now) == nullptr);
}

int main() {
    test_flag_parsing();
    test_allstop_classification();
    test_state_machine();
    test_survey_completion();
    test_entry_gate();
    test_comms_lost_dominates();
    test_helm_summary_parsing();
    test_seglist_parsing();
    test_obstacle_id_parsing();
    test_decay_latch();
    test_obstacle_tracker();
    test_odometry();
    test_derived_quantities();
    test_key_values_continuation();
    test_comms_phases();
    test_heading_error();
    test_split_csv();
    test_rate_limiter();
    test_contact_tracker();
    test_route_naming();
    test_checklist();
    test_geometry();
    test_traffic_geometry();
    test_helm_behaviors_fallback();
    test_readiness();

    std::printf("mission_logic_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
