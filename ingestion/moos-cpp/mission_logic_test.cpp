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

// Outside autonomy: a throttle or rudder command from Swiftships-UI is
// Remote (the boats' iWebSocket never posts REMOTE_CONTROL), drifting with
// no command is Moving, at rest Idle / Parked. Autonomy driving wins.
static void test_remote_and_moving() {
    MissionFlags f;
    CHECK_EQ_STR(DeriveMissionState(f), "Idle");
    f.commanded = true;
    CHECK_EQ_STR(DeriveMissionState(f), "Remote");
    f.moving = true;                                // driving and moving: Remote
    CHECK_EQ_STR(DeriveMissionState(f), "Remote");
    f.commanded = false;                            // drifting, no command
    CHECK_EQ_STR(DeriveMissionState(f), "Moving");
    f.moving = false;
    CHECK_EQ_STR(DeriveMissionState(f), "Idle");
    // Helm parked (CONTROLE_MANUAL): console commands Remote, drift Moving,
    // at rest Parked; with the survey loaded too (mission on, not started).
    f.manual_override = true; f.waypoint = true;
    CHECK(!autonomy_driving(f));
    f.commanded = true;  CHECK_EQ_STR(DeriveMissionState(f), "Remote");
    f.commanded = false; f.moving = true; CHECK_EQ_STR(DeriveMissionState(f), "Moving");
    f.moving = false;    CHECK_EQ_STR(DeriveMissionState(f), "Parked");
    f.manual_override = false;
    // Autonomy driving: its own commands and motion never read Remote / Moving.
    f.commanded = true; f.moving = true;
    CHECK(autonomy_driving(f));
    CHECK_EQ_STR(DeriveMissionState(f), "Autonomous");  f.waypoint = false;
    f.returning = true; CHECK_EQ_STR(DeriveMissionState(f), "ReturnToBase"); f.returning = false;
    f.stationing = true; CHECK_EQ_STR(DeriveMissionState(f), "StationKeeping"); f.stationing = false;
    f.trailing = true;  CHECK_EQ_STR(DeriveMissionState(f), "Following");   f.trailing = false;
    f.pause = true;     CHECK_EQ_STR(DeriveMissionState(f), "Paused");      f.pause = false;
    // The operating-region fence (REGION) is not a mode: with it enforced
    // and nothing else, the operator's commands still read Remote, drift
    // reads Moving, and a survey inside it reads Autonomous.
    f.region = true;
    CHECK(!autonomy_mode(f));
    CHECK_EQ_STR(DeriveMissionState(f), "Remote");
    f.commanded = false; CHECK_EQ_STR(DeriveMissionState(f), "Moving");
    f.moving = false;    CHECK_EQ_STR(DeriveMissionState(f), "Idle");
    f.waypoint = true;   CHECK_EQ_STR(DeriveMissionState(f), "Autonomous"); f.waypoint = false;
    f.region = false; f.commanded = true; f.moving = true;
    f.return_hold = true; CHECK_EQ_STR(DeriveMissionState(f), "HoldingAtBase"); f.return_hold = false;
    // Comms loss and REMOTE_CONTROL keep their places.
    f.comms_lost = "loiter"; CHECK_EQ_STR(DeriveMissionState(f), "CommsLost"); f.comms_lost = "false";
    f.commanded = false; f.moving = false;
    f.remote_control = true; CHECK_EQ_STR(DeriveMissionState(f), "Remote"); f.remote_control = false;
    CHECK(!autonomy_mode(f));
}

static void test_motion_latch() {
    MotionLatch m;                                  // 1 %, rudder 1, 0.5 kn, hold 10 s
    CHECK(!m.commanded(0.0) && !m.moving(0.0));
    m.set_speed(0.3, 1.0);                          // GPS noise at the dock
    CHECK(!m.moving(1.0));
    m.set_thrust(20.0, 2.0);                        // operator throttle
    CHECK(m.commanded(2.0));
    m.set_thrust(0.0, 3.0);
    CHECK(m.commanded(11.9));                       // held 10 s after the last sign (t=2)
    CHECK(!m.commanded(12.1));
    m.set_rudder(-12.0, 20.0);                      // rudder alone counts
    CHECK(m.commanded(20.0));
    m.set_rudder(0.4, 21.0);                        // below 1: not a command
    CHECK(!m.commanded(31.5));
    m.set_thrust(-15.0, 40.0);                      // astern counts
    CHECK(m.commanded(40.0));
    m.set_thrust(0.0, 41.0);
    m.set_speed(1.8, 60.0);                         // drifting on the current
    CHECK(m.moving(60.0) && !m.commanded(60.0));
    m.update(65.0);                                 // still drifting (no new fix)
    CHECK(m.moving(74.0));
    m.set_speed(0.2, 80.0);
    CHECK(!m.moving(90.5));
    // Autonomy ends: the autopilot's last outputs are dropped.
    m.set_thrust(35.0, 100.0); m.set_rudder(8.0, 100.0);
    CHECK(m.commanded(100.0));
    m.autonomy_ended();
    CHECK(!m.commanded(100.0));
    m.update(101.0);
    CHECK(!m.commanded(101.0));
    m.set_thrust(10.0, 102.0);                      // a new console command counts
    CHECK(m.commanded(102.0));
    // moving_kn = 0: no Moving state.
    MotionLatch t; t.moving_kn = 0.0;
    t.set_speed(5.0, 1.0);
    CHECK(!t.moving(1.0));
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
    // LatLonToLocal is the exact inverse of LocalToLatLon.
    XY back = LatLonToLocal(ll.lat, ll.lon, 29.71970895, -91.14705165);
    CHECK(std::fabs(back.x - 1000.0) < 1e-6);
    CHECK(std::fabs(back.y - 1000.0) < 1e-6);
    XY zero = LatLonToLocal(29.7, -91.1, 29.7, -91.1);
    CHECK(std::fabs(zero.x) < 1e-9 && std::fabs(zero.y) < 1e-9);
    // usv-722 symptom: a contact at Morgan City converted with a 0 latitude
    // origin lands ~3,300 km north of an own ship on the real datum; from
    // LAT/LON on the mission datum it is a few hundred metres away.
    XY wrong = LatLonToLocal(29.7250, -91.1450, 0.0, -91.14705165);
    CHECK(wrong.y > 3.2e6);
    XY right = LatLonToLocal(29.7250, -91.1450, 29.71970895, -91.14705165);
    CHECK(std::hypot(right.x, right.y) < 1000.0);
    // NODE_REPORT LAT/LON parsing (keys already lower-cased).
    double la = 0, lo = 0;
    CHECK(ReportLatLon(ParseKeyValuesLower("NAME=AIS367045970,X=1,Y=2,LAT=29.72,LON=-91.15"), &la, &lo));
    CHECK(std::fabs(la - 29.72) < 1e-12 && std::fabs(lo + 91.15) < 1e-12);
    CHECK(ReportLatLon(ParseKeyValuesLower("NAME=a,LAT=29.72,LONG=-91.15"), &la, &lo));
    CHECK(!ReportLatLon(ParseKeyValuesLower("NAME=a,X=1,Y=2"), &la, &lo));
    CHECK(!ReportLatLon(ParseKeyValuesLower("NAME=a,LAT=0,LON=0"), &la, &lo));
    CHECK(!ReportLatLon(ParseKeyValuesLower("NAME=a,LAT=95,LON=10"), &la, &lo));
    CHECK(!ReportLatLon(ParseKeyValuesLower("NAME=a,LAT=abc,LON=10"), &la, &lo));
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

static void test_camera_readiness() {
    const double now = 1000.0;
    ReadinessConfig cfg;
    // frigate_bridge summary: 2 enabled, both streaming
    LatestStore s;
    s.put("camera_summary", "frigate_up", 1, now);
    s.put("camera_summary", "enabled", 2, now);
    s.put("camera_summary", "streaming", 2, now);
    auto v = EvaluateReadiness(s, now, true, cfg);
    CHECK(find_check(v, "cameras").status == READY_PASS);
    CHECK_EQ_STR(find_check(v, "cameras").evidence, "2 / 2 cameras streaming");
    // one camera lost its stream
    s.put("camera_summary", "streaming", 1, now);
    v = EvaluateReadiness(s, now, true, cfg);
    CHECK(find_check(v, "cameras").status == READY_FAIL);
    CHECK_EQ_STR(find_check(v, "cameras").evidence, "1 / 2 cameras streaming");
    // Frigate down beats the (stale-but-fresh) counts
    s.put("camera_summary", "frigate_up", 0, now);
    v = EvaluateReadiness(s, now, true, cfg);
    CHECK_EQ_STR(find_check(v, "cameras").evidence, "Frigate NVR not reachable");
    // nothing enabled: N/A in simulation, FAIL on a vessel
    LatestStore e;
    e.put("camera_summary", "frigate_up", 1, now);
    e.put("camera_summary", "enabled", 0, now);
    e.put("camera_summary", "streaming", 0, now);
    CHECK(find_check(EvaluateReadiness(e, now, false, cfg), "cameras").status == READY_NA);
    CHECK(find_check(EvaluateReadiness(e, now, true, cfg), "cameras").status == READY_FAIL);
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
    CHECK(v.size() == 18);
    CHECK(find_check(v, "nav").status == READY_NA);           // no navigation guard
    CHECK_EQ_STR(find_check(v, "nav").evidence, "no navigation guard on this vessel");
    CHECK(find_check(v, "nav").seq == 2);
    CHECK(find_check(v, "speed_cap").status == READY_NA);     // no VESSEL_MAX_SPEED_KN
    CHECK_EQ_STR(find_check(v, "speed_cap").evidence, "no VESSEL_MAX_SPEED_KN in usv.env");
    CHECK(find_check(v, "apps").status == READY_NA);          // no app list configured
    CHECK(find_check(v, "endurance").status == READY_NA);     // no vessel constants
    CHECK(find_check(v, "gps").status == READY_PASS);      // no quality data, sim
    CHECK_EQ_STR(find_check(v, "course").evidence, "Som Rt0 · 4 waypoints · region 5 vertices");
    CHECK(find_check(v, "course").status == READY_PASS);
    CHECK(find_check(v, "enc").status == READY_CONSOLE);
    CHECK(find_check(v, "autonomy").status == READY_PASS);
    CHECK_EQ_STR(find_check(v, "autonomy").evidence, "61 msg/s · helm PARK");
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
    CHECK(!r.ready && r.failed == 0 && r.console == 2 && r.na == 9 && r.passed == 7);   // N/A 9: speed cap (v1.2.33), nav guard (v1.2.43)
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
                 "route 3.2 nm needs 0.8 h (x1.5) · fuel 82 % = 164 L of 200 L = 13.7 h · battery 5.3 h");
    // v1.2.33: tank size from usv.env when real.moos has no fuel_tank_l;
    // litres shown even when the hours cannot be computed; Speed cap check.
    {
        ReadinessConfig c2 = cfg;
        c2.fuel_tank_l = 0; c2.vessel_fuel_l = 1135;
        CHECK_EQ_STR(find_check(EvaluateReadiness(s, now, true, c2), "endurance").evidence,
                     "route 3.2 nm needs 0.8 h (x1.5) · fuel 82 % = 931 L of 1,135 L = 77.6 h · battery 5.3 h");
        c2.cruise_fuel_lph = 0; c2.battery_kwh = 0;
        auto e = find_check(EvaluateReadiness(s, now, true, c2), "endurance");
        CHECK(e.status == READY_NA);
        CHECK_EQ_STR(e.evidence, "fuel 82 % = 931 L of 1,135 L · set cruise_fuel_lph for hours");
        c2.vessel_max_speed_kn = 30;
        auto sc = find_check(EvaluateReadiness(s, now, true, c2), "speed_cap");
        CHECK(sc.status == READY_NA);
        CHECK_EQ_STR(sc.evidence, "no SPEED_CAP published · hull max 30 kn");
        s.put("autopilot", "speed_cap", 12, now);
        sc = find_check(EvaluateReadiness(s, now, true, c2), "speed_cap");
        CHECK(sc.status == READY_PASS);
        CHECK_EQ_STR(sc.evidence, "cap 12.0 kn set · hull max 30 kn");
        s.put("autopilot", "speed_cap", 31.5, now);
        sc = find_check(EvaluateReadiness(s, now, true, c2), "speed_cap");
        CHECK(sc.status == READY_FAIL);
        CHECK_EQ_STR(sc.evidence, "cap 31.5 kn set · hull max 30 kn");
        s.put("autopilot", "speed_cap", 0, now);
        CHECK(find_check(EvaluateReadiness(s, now, true, c2), "speed_cap").status == READY_FAIL);
        s.put("autopilot", "speed_cap", 12, now);
    }
    CHECK_EQ_STR(find_check(x, "course").evidence,
                 "Som Rt0 · 4 waypoints · 3.2 nm · region 5 vertices · route inside region");
    CHECK(find_check(x, "engine").evidence.find("no DTC") != std::string::npos);
    CHECK_EQ_STR(find_check(x, "gps").evidence, "RTK Fixed · 14 sats · HDOP 0.8");
    CHECK_EQ_STR(find_check(x, "steering").evidence, "rudder 2.0° · cmd 3.0°");
    CHECK_EQ_STR(find_check(x, "can").evidence, "interface UP · 212 msg/s");
    CHECK_EQ_STR(find_check(x, "engine").evidence, "coolant 41 °C · oil 310 kPa · fuel 82 % · no DTC");
    CHECK(find_check(x, "ais").status == READY_PASS);
    // usv-722: no engine sensors on its CAN bus, but key switch, gear and
    // thrust to the engine are (can_bridge, usv722_can.hpp).
    {
        LatestStore k;
        k.put("engine", "key_state", 1, now);
        k.put("engine", "gear", 0, now);
        k.put("engine", "throttle_pct", 0, now);
        k.put("engine", "fuel_level", 23, now);
        k.put("engine", "active_dtcs", 0, now);
        CHECK_EQ_STR(find_check(EvaluateReadiness(k, now, true), "engine").evidence,
                     "key RUN · gear NEUTRAL · thrust 0 % · fuel 23 % · no DTC");
        CHECK(find_check(EvaluateReadiness(k, now, true), "engine").status == READY_PASS);
        k.put("engine", "key_state", 0, now);
        CHECK(find_check(EvaluateReadiness(k, now, true), "engine").status == READY_FAIL);
        CHECK(find_check(EvaluateReadiness(k, now, true), "engine").evidence.rfind("key OFF", 0) == 0);
        LatestStore only_key;
        only_key.put("engine", "key_state", 2, now);
        CHECK_EQ_STR(find_check(EvaluateReadiness(only_key, now, true), "engine").evidence, "key START");
    }
    // usv-725: J1939 engine through iCANRec (rpm, coolant, oil pressure,
    // faults; no key switch, no fuel level). Oil pressure is judged only
    // with the engine running.
    {
        LatestStore d;
        d.put("engine", "rpm", 1540, now);
        d.put("engine", "coolant_temp", 81, now);
        d.put("engine", "oil_pressure", 340, now);
        d.put("engine", "active_dtcs", 0, now);
        CHECK_EQ_STR(find_check(EvaluateReadiness(d, now, true), "engine").evidence,
                     "rpm 1540 · coolant 81 °C · oil 340 kPa · no DTC");
        CHECK(find_check(EvaluateReadiness(d, now, true), "engine").status == READY_PASS);
        d.put("engine", "oil_pressure", 120, now);
        CHECK_EQ_STR(find_check(EvaluateReadiness(d, now, true), "engine").evidence,
                     "rpm 1540 · coolant 81 °C · oil 120 kPa LOW · no DTC");
        CHECK(find_check(EvaluateReadiness(d, now, true), "engine").status == READY_FAIL);
        // Key on, engine stopped: 0 rpm and 0 kPa is not a fault.
        d.put("engine", "rpm", 0, now);
        d.put("engine", "oil_pressure", 0, now);
        CHECK_EQ_STR(find_check(EvaluateReadiness(d, now, true), "engine").evidence,
                     "rpm 0 · coolant 81 °C · oil 0 kPa · no DTC");
        CHECK(find_check(EvaluateReadiness(d, now, true), "engine").status == READY_PASS);
        LatestStore only_rpm;
        only_rpm.put("engine", "rpm", 620, now);
        CHECK_EQ_STR(find_check(EvaluateReadiness(only_rpm, now, true), "engine").evidence, "rpm 620");
    }

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
    CHECK_EQ_STR(ry.blocking, "GPS position fix; Course planning; Apps; Steering / actuators; Engine pre-start; Safety flags; Console checklist");

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

    // Vessels without ENC / without a console checklist: N/A, not CONSOLE,
    // and they no longer hold the verdict at NOT READY.
    {
        ReadinessConfig nc;
        nc.enc_fitted = false;
        nc.console_checklist = false;
        LatestStore e;
        auto v2 = EvaluateReadiness(e, now, false, nc);
        CHECK(find_check(v2, "enc").status == READY_NA);
        CHECK_EQ_STR(find_check(v2, "enc").evidence, "no ENC chart system on this vessel");
        CHECK(find_check(v2, "checklist").status == READY_NA);
        CHECK(SummarizeReadiness(v2).console == 0);
        auto v3 = EvaluateReadiness(e, now, false);
        CHECK(find_check(v3, "enc").status == READY_CONSOLE);
        CHECK(find_check(v3, "checklist").status == READY_CONSOLE);
    }

    // A route planned safe on the ENC (Swiftships-UI saves it as
    // "<name> (safe)") stands in for the operating region and shows an ENC
    // chart is in use; "(unsafe)" fails the Course row.
    {
        ReadinessConfig nc;
        nc.enc_fitted = false;
        LatestStore r;
        r.put_str("mission_state", "route_name", "10-19-2026 (safe)", now - 5);
        r.put("mission", "waypoints_total", 6, now - 5);
        r.put("mission", "route_length_m", 0.6 * 1852.0, now - 5);
        auto a = EvaluateReadiness(r, now, true, nc);
        CHECK(find_check(a, "course").status == READY_PASS);
        CHECK_EQ_STR(find_check(a, "course").evidence,
                     "10-19-2026 (safe) · 6 waypoints · 0.6 nm · checked safe on ENC");
        CHECK(find_check(a, "enc").status == READY_PASS);
        CHECK_EQ_STR(find_check(a, "enc").evidence, "ENC in use · route checked safe on chart");
        // Console ENC_STATUS still wins when it is published.
        r.put_str("mission_state", "enc_status", "none", now);
        CHECK(find_check(EvaluateReadiness(r, now, true), "enc").status == READY_FAIL);
        // Plain route name, no region: FAIL with the reason.
        LatestStore p;
        p.put_str("mission_state", "route_name", "testgps", now - 5);
        p.put("mission", "waypoints_total", 6, now - 5);
        auto b = EvaluateReadiness(p, now, true, nc);
        CHECK(find_check(b, "course").status == READY_FAIL);
        CHECK_EQ_STR(find_check(b, "course").evidence,
                     "testgps · 6 waypoints · no operating region or ENC safe route");
        CHECK(find_check(b, "enc").status == READY_NA);
        // Route that failed the ENC check: FAIL even with a region.
        p.put_str("mission_state", "route_name", "testgps (unsafe)", now - 5);
        p.put("mission", "region_vertices", 4, now - 5);
        auto c = EvaluateReadiness(p, now, true, nc);
        CHECK(find_check(c, "course").status == READY_FAIL);
        CHECK_EQ_STR(find_check(c, "course").evidence,
                     "testgps (unsafe) · 6 waypoints · region 4 vertices · ROUTE NOT SAFE ON ENC");
        CHECK(find_check(c, "enc").status == READY_NA);
    }

    // ENC cells listed by the onboard chart server (bridge poll of Signal
    // K -> chart.enc_*): the row passes on the cell covering the boat,
    // fails when no cell covers it, and the console's ENC_STATUS still
    // wins. Stale chart fields (poll older than 5 min) fall back.
    {
        ReadinessConfig nc;
        nc.enc_fitted = false;
        LatestStore k;
        k.put("chart", "enc_cells", 10, now - 30);
        k.put("chart", "enc_covers", 1, now - 30);
        k.put_str("chart", "enc_cell", "US3LA1CB", now - 30);
        auto a = EvaluateReadiness(k, now, true, nc);
        CHECK(find_check(a, "enc").status == READY_PASS);
        CHECK_EQ_STR(find_check(a, "enc").evidence, "ENC US3LA1CB · covers position · 10 cells loaded");
        k.put("chart", "enc_covers", 0, now - 30);
        k.put_str("chart", "enc_cell", "", now - 30);
        auto b = EvaluateReadiness(k, now, true, nc);
        CHECK(find_check(b, "enc").status == READY_FAIL);
        CHECK_EQ_STR(find_check(b, "enc").evidence, "ENC 10 cells loaded · NO CELL COVERS POSITION");
        k.put("chart", "enc_covers", -1, now - 30);
        auto c = EvaluateReadiness(k, now, true, nc);
        CHECK(find_check(c, "enc").status == READY_PASS);
        CHECK_EQ_STR(find_check(c, "enc").evidence, "ENC 10 cells loaded · position unknown");
        k.put("chart", "enc_cells", 1, now - 30);
        CHECK_EQ_STR(find_check(EvaluateReadiness(k, now, true, nc), "enc").evidence,
                     "ENC 1 cell loaded · position unknown");
        k.put_str("mission_state", "enc_status", "none", now);
        CHECK(find_check(EvaluateReadiness(k, now, true, nc), "enc").status == READY_FAIL);
        CHECK_EQ_STR(find_check(EvaluateReadiness(k, now, true, nc), "enc").evidence, "none");
        LatestStore stale;
        stale.put("chart", "enc_cells", 10, now - 400);
        stale.put("chart", "enc_covers", 1, now - 400);
        stale.put_str("chart", "enc_cell", "US3LA1CB", now - 400);
        CHECK(find_check(EvaluateReadiness(stale, now, true, nc), "enc").status == READY_NA);
        LatestStore none;
        none.put("chart", "enc_cells", 0, now - 30);
        none.put("chart", "enc_covers", -1, now - 30);
        CHECK(find_check(EvaluateReadiness(none, now, true, nc), "enc").status == READY_NA);
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

// Production iWebSocket (comms loss return to base): COMMS_LOSS, RETURN,
// RETURN_HOLD, BASE_COORDS.
static void test_comms_loss_return() {
    MissionFlags f;
    CHECK_EQ_STR(DeriveMissionState(f), "Idle");
    CHECK(operator_link(f) == 0);
    CHECK(!comms_return(f));

    // At the dock, helm parked, UI closed: link lost, boat stays.
    f.manual_override = true;
    f.comms_loss = true;
    CHECK_EQ_STR(DeriveMissionState(f), "Parked");
    CHECK(operator_link(f) == 4);
    CHECK(!comms_return(f));
    CHECK(!comms_lost_active(f.comms_lost));      // simulation phase untouched

    // Underway: iWebSocket engages RETURN and releases the park.
    f.manual_override = false;
    f.waypoint = false;
    f.returning = true;
    CHECK_EQ_STR(DeriveMissionState(f), "ReturnToBase");
    CHECK(operator_link(f) == 3);
    CHECK(comms_return(f));

    // Arrival: comms_return endflags RETURN=false, RETURN_HOLD=true.
    f.returning = false;
    f.return_hold = true;
    CHECK_EQ_STR(DeriveMissionState(f), "HoldingAtBase");
    CHECK(operator_link(f) == 5);
    CHECK(comms_return(f));

    // Link back: the hold continues until the operator's next command.
    f.comms_loss = false;
    CHECK_EQ_STR(DeriveMissionState(f), "HoldingAtBase");
    CHECK(operator_link(f) == 0);
    CHECK(!comms_return(f));

    // Operator return to base with the link up is not a comms failsafe.
    f.return_hold = false;
    f.returning = true;
    CHECK_EQ_STR(DeriveMissionState(f), "ReturnToBase");
    CHECK(operator_link(f) == 0);
    CHECK(!comms_return(f));

    // Priorities: remote control, park and pause outrank the hold.
    MissionFlags h; h.return_hold = true;
    h.pause = true;            CHECK_EQ_STR(DeriveMissionState(h), "Paused");
    h.pause = false; h.remote_control = true;
                               CHECK_EQ_STR(DeriveMissionState(h), "Remote");
    // The simulation phases keep their meaning when both are present.
    MissionFlags s; s.comms_lost = "grace"; s.comms_loss = true; s.returning = true;
    CHECK(operator_link(s) == 1);

    // BASE_COORDS
    double la = 0, lo = 0;
    CHECK(ParseLatLon("29.72282,-91.21539", &la, &lo) && la == 29.72282 && lo == -91.21539);
    CHECK(ParseLatLon(" 29.7 , -91.2 ", &la, &lo) && la == 29.7);
    CHECK(!ParseLatLon("", &la, &lo));
    CHECK(!ParseLatLon("29.7", &la, &lo));
    CHECK(!ParseLatLon("lat,lon", &la, &lo));
    CHECK(!ParseLatLon("95,-91", &la, &lo));
    CHECK(!ParseLatLon("29.7,-91.2x", &la, &lo));
}

static void test_min_cpa_pick() {
    // usv-722 2026-10-14: two AIS contacts opening at ~1.7 km and a ship
    // ~40 nm out closing to a CPA of ~75 km in two hours.
    MinCpaPick p;
    p.Add(1690, -1); p.Add(1824, -1); p.Add(75000, 7000);
    CHECK(std::fabs(p.cpa - 1690) < 1e-9);
    CHECK(p.tcpa == -1);
    // A closing contact inside 12 nm still ranks ahead of an opening one,
    // even with a larger CPA (the TCPA is what the operator watches).
    MinCpaPick q;
    q.Add(1690, -1); q.Add(3000, 240); q.Add(75000, 7000);
    CHECK(std::fabs(q.cpa - 3000) < 1e-9);
    CHECK(std::fabs(q.tcpa - 240) < 1e-9);
    // Order does not matter.
    MinCpaPick r;
    r.Add(75000, 7000); r.Add(3000, 240); r.Add(1690, -1);
    CHECK(std::fabs(r.cpa - 3000) < 1e-9);
    // Only far contacts: the smallest CPA, with its TCPA.
    MinCpaPick f;
    f.Add(90000, 9000); f.Add(60000, 8000);
    CHECK(std::fabs(f.cpa - 60000) < 1e-9);
    CHECK(std::fabs(f.tcpa - 8000) < 1e-9);
    // Closing at exactly 12 nm is still preferred; unknown CPA is skipped.
    MinCpaPick e;
    e.Add(-1, 10); e.Add(500, -1); e.Add(kMinCpaRankM, 600);
    CHECK(std::fabs(e.cpa - kMinCpaRankM) < 1e-9);
    MinCpaPick none;
    none.Add(-1, -1);
    CHECK(none.cpa < 0 && none.tcpa < 0);
}

static void test_colav_alerts() {
    std::vector<ColavAlert> v;
    // The shape iWebSocket 7df059d posts (jsoncpp FastWriter, one line).
    const std::string js =
        "[{\"cn\":\"TUG_ANNA\",\"mmsi\":367123450,\"active\":true,\"for\":12,"
        "\"mode\":\"giveway:stern\",\"hdg\":271.5,\"spd\":4.2,\"lat\":29.7,\"lon\":-91.2,"
        "\"len\":24,\"type\":\"tug\",\"age\":1.5,\"range\":812.4,\"bng\":44.1,"
        "\"relBng\":12.3,\"cpa\":96.2,\"tcpa\":118.0},"
        "{\"cn\":\"R7\",\"mmsi\":-1,\"active\":false,\"for\":3,\"mode\":\"standon:inextremis\","
        "\"hdg\":-1,\"spd\":-1,\"lat\":0,\"lon\":0,\"len\":-1,\"type\":\"\",\"age\":-1,"
        "\"range\":-1,\"bng\":-1,\"relBng\":-1,\"cpa\":-1,\"tcpa\":-1}]";
    CHECK(ParseColavAlerts(js, &v));
    CHECK(v.size() == 2);
    if (v.size() == 2) {
        CHECK_EQ_STR(v[0].cn, "TUG_ANNA");
        CHECK(v[0].active);
        CHECK(std::fabs(v[0].cpa - 96.2) < 1e-9);
        CHECK(std::fabs(v[0].tcpa - 118.0) < 1e-9);
        CHECK(std::fabs(v[0].spd - 4.2) < 1e-9);
        CHECK(std::fabs(v[0].rel_bng - 12.3) < 1e-9);
        CHECK(v[0].mmsi == 367123450);
        CHECK_EQ_STR(v[0].mode, "giveway:stern");
        CHECK(!v[1].active);
        CHECK(v[1].cpa == -1);
    }
    // Pretty-printed (StyledWriter) and empty arrays.
    CHECK(ParseColavAlerts("[\n  {\n    \"cn\" : \"A\",\n    \"active\" : true\n  }\n]\n", &v));
    CHECK(v.size() == 1);
    CHECK(ParseColavAlerts("[]", &v) && v.empty());
    CHECK(ParseColavAlerts(" [ ] ", &v) && v.empty());
    // Escapes in names.
    CHECK(ParseColavAlerts("[{\"cn\":\"A\\\"B\",\"active\":true}]", &v) && v.size() == 1 && v[0].cn == "A\"B");
    // Garbage is rejected and leaves nothing behind.
    CHECK(!ParseColavAlerts("", &v) && v.empty());
    CHECK(!ParseColavAlerts("{\"cn\":\"A\"}", &v) && v.empty());
    CHECK(!ParseColavAlerts("[{\"cn\":\"A\",", &v) && v.empty());
    CHECK(!ParseColavAlerts("[{\"cn\":\"A\",\"active\":tru}]", &v) && v.empty());
    // Entries without a contact name are dropped.
    CHECK(ParseColavAlerts("[{\"mode\":\"cpa\"}]", &v) && v.empty());

    CHECK_EQ_STR(ColregsOf("headon").rule, "Rule 14");
    CHECK_EQ_STR(ColregsOf("headon:").name, "Head-on");
    CHECK_EQ_STR(ColregsOf("overtaking:port").name, "Overtaking, passing to port");
    CHECK_EQ_STR(ColregsOf("overtaking:starboard").rule, "Rule 13");
    CHECK_EQ_STR(ColregsOf("giveway:stern").name, "Give-way, crossing astern");
    CHECK_EQ_STR(ColregsOf("giveway:bow").name, "Give-way, crossing ahead");
    CHECK_EQ_STR(ColregsOf("giveway:bow").rule, "Rule 16");
    CHECK_EQ_STR(ColregsOf("standon").name, "Stand-on");
    CHECK(!ColregsOf("standon").extremis);
    CHECK_EQ_STR(ColregsOf("standon_ot").name, "Stand-on, being overtaken");
    CHECK(ColregsOf("standon:inextremis").extremis);
    CHECK_EQ_STR(ColregsOf("standon:inextremis").rule, "Rule 17(b)");
    CHECK_EQ_STR(ColregsOf("standon_ot:inextremis").name, "Being overtaken, in extremis");
    CHECK(!ColregsOf("giveway:inextremis").extremis);
    CHECK_EQ_STR(ColregsOf("cpa").rule, "");
    CHECK_EQ_STR(ColregsOf("cpa").name, "Keep clear (CPA)");
    CHECK_EQ_STR(ColregsOf("none").name, "Avoiding");
    CHECK_EQ_STR(ColregsOf("").name, "Avoiding");
}

// Chart listing of the onboard chart server (Signal K resources/charts,
// as usv-722 serves it: ten S-57 cells converted from the NOAA ENCs) and
// the Readiness coverage pick.
static void test_enc_charts() {
    const std::string body =
        "{\"US1GLBCF\":{\"name\":\"US1GLBCF\",\"description\":\"Gulf of America\","
        "\"bounds\":[-98,17.791496,-76.1,33.6085],\"minzoom\":9,\"maxzoom\":16,\"format\":\"pbf\","
        "\"type\":\"S-57\",\"scale\":250000,\"identifier\":\"US1GLBCF\","
        "\"tilemapUrl\":\"/signalk/chart-tiles/US1GLBCF/{z}/{x}/{y}\",\"chartLayers\":[]},"
        "\"US3LA1CB\":{\"name\":\"US3LA1CB\",\"description\":\"Gulf Intracoastal Waterway - Morgan City to Vermilion Bay and Gulf of America\","
        "\"bounds\":[-92.4,28.8,-91.2,30],\"minzoom\":9,\"maxzoom\":16,\"format\":\"pbf\",\"type\":\"S-57\","
        "\"scale\":250000,\"identifier\":\"US3LA1CB\",\"tilemapUrl\":\"/signalk/chart-tiles/US3LA1CB/{z}/{x}/{y}\",\"chartLayers\":[]},"
        "\"US3LA1CC\":{\"name\":\"US3LA1CC\",\"description\":\"Gulf Intracoastal Waterway - New Orleans to Morgan Ciy and Gulf of America\","
        "\"bounds\":[-91.2,28.8,-90,30],\"minzoom\":9,\"maxzoom\":16,\"format\":\"pbf\",\"type\":\"S-57\","
        "\"scale\":250000,\"identifier\":\"US3LA1CC\",\"url\":\"/signalk/chart-tiles/US3LA1CC/{z}/{x}/{y}\",\"layers\":[]},"
        "\"US5LMOBB\":{\"name\":\"US5LMOBB\",\"description\":\"Gulf of America\","
        "\"bounds\":[-91.575,29.175,-91.5,29.25],\"minzoom\":9,\"maxzoom\":16,\"format\":\"pbf\",\"type\":\"S-57\","
        "\"scale\":250000,\"identifier\":\"US5LMOBB\",\"tilemapUrl\":\"/signalk/chart-tiles/US5LMOBB/{z}/{x}/{y}\",\"chartLayers\":[]},"
        "\"osm\":{\"name\":\"OpenStreetMap\",\"description\":\"Base map\",\"bounds\":[-180,-85,180,85],"
        "\"type\":\"tilelayer\",\"format\":\"png\",\"identifier\":\"osm\",\"tilemapUrl\":\"https://tile.openstreetmap.org/{z}/{x}/{y}.png\"}}";
    const auto charts = ParseSignalKCharts(body);
    CHECK(charts.size() == 5);
    CHECK_EQ_STR(charts[0].id, "US1GLBCF");
    CHECK(charts[0].is_enc() && charts[0].has_bounds);
    CHECK(std::fabs(charts[0].min_lon + 98.0) < 1e-9 && std::fabs(charts[0].max_lat - 33.6085) < 1e-9);
    CHECK_EQ_STR(charts[1].description, "Gulf Intracoastal Waterway - Morgan City to Vermilion Bay and Gulf of America");
    CHECK(!charts[4].is_enc());
    // usv-722 at Berwick (29.7241, -91.2179): US1GLBCF and US3LA1CB cover
    // it, US3LA1CC starts east of -91.2, the US5 harbour cells lie south;
    // the smaller US3 cell is the pick. Four ENC cells, the base map not
    // counted.
    EncCoverage c = EncCoverageAt(charts, true, 29.724138, -91.217902);
    CHECK(c.cells == 4);
    CHECK_EQ_STR(c.cell, "US3LA1CB");
    // East of -91.2: the neighbouring cell.
    CHECK_EQ_STR(EncCoverageAt(charts, true, 29.72, -91.10).cell, "US3LA1CC");
    // Inside the harbour cell: the most detailed pick.
    CHECK_EQ_STR(EncCoverageAt(charts, true, 29.2, -91.55).cell, "US5LMOBB");
    // Lumut, Malaysia: nothing covers it.
    CHECK(EncCoverageAt(charts, true, 4.2245, 100.5868).cell.empty());
    CHECK(EncCoverageAt(charts, true, 4.2245, 100.5868).cells == 4);
    // Position unknown: count only.
    CHECK(EncCoverageAt(charts, false, 0, 0).cell.empty());
    // Object key as id when identifier and name are absent; bounds in the
    // wrong order are ignored (no coverage, still a cell).
    const auto odd = ParseSignalKCharts("{ \"X1\" : { \"type\": \"ENC\", \"bounds\": [10, 5, 1, 2] } }");
    CHECK(odd.size() == 1);
    CHECK_EQ_STR(odd[0].id, "X1");
    CHECK(odd[0].is_enc() && !odd[0].has_bounds);
    CHECK(EncCoverageAt(odd, true, 3, 5).cells == 1 && EncCoverageAt(odd, true, 3, 5).cell.empty());
    // Empty, malformed and non-object bodies give no cells.
    CHECK(ParseSignalKCharts("").empty());
    CHECK(ParseSignalKCharts("{}").empty());
    CHECK(ParseSignalKCharts("[]").empty());
    CHECK(ParseSignalKCharts("{\"A\":{\"type\":\"S-57\"").empty());
    CHECK(ParseSignalKCharts("<html>502 Bad Gateway</html>").empty());
}

// v1.2.43: safe route from the planner note, navigation integrity, ALL STOP,
// drive interface, service mode, region breach.
static void test_v1243_readiness() {
    const double now = 1000.0;
    // Planner note in the route description (Plan Safe Route updates the
    // route in place; no "(safe)" in the name any more).
    CHECK_EQ_STR(ParsePlannerNote("Planned in water >= 3 m with 10 m clearance (US3LA1CB, US5LMOBB)."),
                 "water >= 3 m, 10 m clearance, US3LA1CB, US5LMOBB");
    CHECK_EQ_STR(ParsePlannerNote("Survey leg. Planned in water >= 2.5 m with 8 m clearance ()."),
                 "water >= 2.5 m, 8 m clearance");
    CHECK_EQ_STR(ParsePlannerNote("Planned in water >= 4 m"), "water >= 4 m");
    CHECK_EQ_STR(ParsePlannerNote("Survey leg"), "");
    CHECK_EQ_STR(ParsePlannerNote(""), "");
    CHECK_EQ_STR(ParsePlannerNote("Planned in water >= m"), "");
    {
        ReadinessConfig nc;
        nc.enc_fitted = false;
        LatestStore r;
        r.put_str("mission_state", "route_name", "L2-2026", now - 5);
        r.put("mission", "waypoints_total", 11, now - 5);
        r.put("mission", "route_length_m", 0.9 * 1852.0, now - 5);
        r.put_str("mission_state", "route_plan", "water >= 3 m, 10 m clearance, US3LA1CB", now - 5);
        auto a = EvaluateReadiness(r, now, true, nc);
        CHECK(find_check(a, "course").status == READY_PASS);
        CHECK_EQ_STR(find_check(a, "course").evidence,
                     "L2-2026 · 11 waypoints · 0.9 nm · checked safe on ENC (water >= 3 m, 10 m clearance, US3LA1CB)");
        CHECK(find_check(a, "enc").status == READY_PASS);
        // "-" (route has no note) is not a plan.
        r.put_str("mission_state", "route_plan", "-", now - 5);
        CHECK(find_check(EvaluateReadiness(r, now, true, nc), "course").status == READY_FAIL);
        // A region still applies its own rule; the plan is not repeated.
        r.put_str("mission_state", "route_plan", "water >= 3 m", now - 5);
        r.put("mission", "region_vertices", 8, now - 5);
        CHECK_EQ_STR(find_check(EvaluateReadiness(r, now, true, nc), "course").evidence,
                     "L2-2026 · 11 waypoints · 0.9 nm · region 8 vertices");
    }
    // Navigation integrity.
    {
        ReadinessConfig cfg;
        LatestStore n;
        CHECK(find_check(EvaluateReadiness(n, now, true, cfg), "nav").status == READY_NA);
        cfg.nav_guard_expected = true;
        CHECK(find_check(EvaluateReadiness(n, now, true, cfg), "nav").status == READY_FAIL);
        CHECK_EQ_STR(find_check(EvaluateReadiness(n, now, true, cfg), "nav").evidence,
                     "navigation guard not reporting · autonomy starts refused");
        n.put("autonomy", "nav_guard", 0, now - 1);
        CHECK(find_check(EvaluateReadiness(n, now, true, cfg), "nav").status == READY_PASS);
        CHECK_EQ_STR(find_check(EvaluateReadiness(n, now, true, cfg), "nav").evidence,
                     "heading, speed and track consistent with GPS");
        n.put("autonomy", "nav_guard", 1, now - 1);
        n.put_str("mission_state", "nav_status_msg", "heading vs GPS track 48 deg", now - 1);
        CHECK(find_check(EvaluateReadiness(n, now, true, cfg), "nav").status == READY_PASS);
        CHECK_EQ_STR(find_check(EvaluateReadiness(n, now, true, cfg), "nav").evidence,
                     "warning: heading vs GPS track 48 deg");
        n.put("autonomy", "nav_guard", 2, now - 1);
        CHECK(find_check(EvaluateReadiness(n, now, true, cfg), "nav").status == READY_FAIL);
        CHECK_EQ_STR(find_check(EvaluateReadiness(n, now, true, cfg), "nav").evidence,
                     "FAULT: heading vs GPS track 48 deg · autonomy starts refused");
        // Stale (guard went silent 20 s ago): not reporting.
        n.put("autonomy", "nav_guard", 0, now - 20);
        CHECK(find_check(EvaluateReadiness(n, now, true, cfg), "nav").status == READY_FAIL);
    }
    // ALL STOP and region breach in Safety flags; service mode evidence;
    // drive interface on the CAN and steering rows.
    {
        LatestStore k;
        k.put("autonomy", "all_stop", 1, now - 1);
        CHECK(find_check(EvaluateReadiness(k, now, true), "safety").status == READY_FAIL);
        CHECK_EQ_STR(find_check(EvaluateReadiness(k, now, true), "safety").evidence, "ALL STOP LATCHED");
        k.put("autonomy", "all_stop", 0, now - 1);
        k.put_str("mission_state", "region_state", "breached", now - 1);
        CHECK_EQ_STR(find_check(EvaluateReadiness(k, now, true), "safety").evidence, "REGION BREACHED");
        k.put_str("mission_state", "region_state", "contained", now - 1);
        CHECK(find_check(EvaluateReadiness(k, now, true), "safety").status == READY_PASS);
        CHECK_EQ_STR(find_check(EvaluateReadiness(k, now, true), "safety").evidence,
                     "no geofence breach, no HIGH collision risk, no crash stop, no ALL STOP");

        k.put("comms", "moos_msg_rate", 60, now - 1);
        k.put_str("mission_state", "helm_state", "DRIVE", now - 1);
        k.put("autopilot", "service_mode", 1, now - 1);
        CHECK(find_check(EvaluateReadiness(k, now, true), "autonomy").status == READY_PASS);
        CHECK_EQ_STR(find_check(EvaluateReadiness(k, now, true), "autonomy").evidence,
                     "60 msg/s · helm DRIVE · SERVICE MODE (tuning gains)");

        k.put("canbus", "interface_up", 1, now - 1);
        k.put("canbus", "msg_rate", 121, now - 1);
        CHECK_EQ_STR(find_check(EvaluateReadiness(k, now, true), "can").evidence, "interface UP · 121 msg/s");
        k.put_str("can_drive", "status", "ACTIVE", now - 1);
        k.put_str("can_drive", "conflict", "none", now - 1);
        CHECK(find_check(EvaluateReadiness(k, now, true), "can").status == READY_PASS);
        CHECK_EQ_STR(find_check(EvaluateReadiness(k, now, true), "can").evidence,
                     "interface UP · 121 msg/s · drive active");
        k.put_str("can_drive", "conflict", "rudder,thrust", now - 1);
        CHECK(find_check(EvaluateReadiness(k, now, true), "can").status == READY_FAIL);
        CHECK_EQ_STR(find_check(EvaluateReadiness(k, now, true), "can").evidence,
                     "interface UP · 121 msg/s · drive active · BLOCKED, ANOTHER NODE SENDS rudder,thrust");
        k.put_str("can_drive", "conflict", "none", now - 1);
        k.put_str("can_drive", "status", "INHIBITED", now - 1);
        CHECK_EQ_STR(find_check(EvaluateReadiness(k, now, true), "can").evidence,
                     "interface UP · 121 msg/s · DRIVE PROFILE NOT LOADED");
        CHECK(find_check(EvaluateReadiness(k, now, true), "can").status == READY_FAIL);
        k.put_str("can_drive", "status", "STANDBY", now - 1);
        CHECK(find_check(EvaluateReadiness(k, now, true), "can").status == READY_PASS);

        k.put("autopilot", "rudder_actual", 2.0, now - 1);
        k.put("autopilot", "rudder_cmd", 1.5, now - 1);
        CHECK(find_check(EvaluateReadiness(k, now, true), "steering").status == READY_PASS);
        k.put("can_drive", "rudder_mismatch", 1, now - 1);
        CHECK(find_check(EvaluateReadiness(k, now, true), "steering").status == READY_FAIL);
        CHECK_EQ_STR(find_check(EvaluateReadiness(k, now, true), "steering").evidence,
                     "rudder 2.0° · cmd 1.5° · RUDDER NOT FOLLOWING DRIVE COMMAND");
    }
    // Drive rudder mismatch with no recent rudder values: still a FAIL.
    {
        LatestStore m;
        m.put("can_drive", "rudder_mismatch", 1, now - 1);
        CHECK(find_check(EvaluateReadiness(m, now, false), "steering").status == READY_FAIL);
        CHECK_EQ_STR(find_check(EvaluateReadiness(m, now, false), "steering").evidence,
                     "RUDDER NOT FOLLOWING DRIVE COMMAND");
        m.put("can_drive", "rudder_mismatch", 0, now - 1);
        CHECK(find_check(EvaluateReadiness(m, now, false), "steering").status == READY_NA);
    }
    // ALL STOP outranks every other state.
    {
        MissionFlags f;
        f.waypoint = true;
        f.all_stop = true;
        f.comms_lost = "loiter";
        CHECK_EQ_STR(DeriveMissionState(f), "AllStop");
        f.all_stop = false;
        CHECK_EQ_STR(DeriveMissionState(f), "CommsLost");
    }
}

// The boat's audit trail lines (iWebSocket CONTROL_EVENT / UI_AUTH_EVENT).
static void test_audit_trail() {
    AuditFields f = ParseAuditFields("take user=alice role=operator gen=1");
    CHECK_EQ_STR(f.action, "take");
    CHECK_EQ_STR(f.kv["user"], "alice");
    CHECK_EQ_STR(f.kv["gen"], "1");
    f = ParseAuditFields("lost user=alice (not heard)");
    CHECK_EQ_STR(f.note, "not heard");
    f = ParseAuditFields("refused action=release user=bob reason=the boat is running a mission, manual control");
    CHECK_EQ_STR(f.kv["reason"], "the boat is running a mission, manual control");
    CHECK_EQ_STR(f.kv["action"], "release");

    AuditTrail t;
    auto e = t.feed("control", "start command_auth=signalk exclusive_control=true (nobody in control)", 1.0);
    CHECK(e.size() == 1);
    CHECK_EQ_STR(e[0].message, "Boat link started, nobody in control");
    CHECK_EQ_STR(e[0].details, "login check signalk · exclusive control true");
    e = t.feed("auth", "login user=alice level=readwrite role=operator ip=192.168.22.5 session=k3f9a1x0", 2.0);
    CHECK(e.size() == 1 && e[0].category == "login" && e[0].user == "alice" && e[0].role == "operator");
    CHECK_EQ_STR(e[0].message, "alice logged in");
    CHECK_EQ_STR(e[0].details, "level readwrite · ip 192.168.22.5 · session k3f9a1x0");
    CHECK(!e[0].popup);
    e = t.feed("control", "take user=alice role=operator gen=1", 3.0);
    CHECK_EQ_STR(e[0].message, "alice took control");
    CHECK_EQ_STR(t.holder(), "alice");
    CHECK_EQ_STR(t.holder_role(), "operator");
    e = t.feed("auth", "login user=carol level=admin role=supervisor ip=192.168.22.9 session=q81mz2c4", 4.0);
    CHECK(t.session_count() == 2);
    CHECK_EQ_STR(t.sessions(), "alice (operator, 192.168.22.5); carol (supervisor, 192.168.22.9)");
    // Forced takeover: two lines, one event with the reason.
    e = t.feed("control", "force user=carol role=supervisor gen=5 from=alice", 5.0);
    CHECK(e.empty());
    CHECK_EQ_STR(t.holder(), "carol");
    e = t.feed("control", "force reason=operator not answering", 5.1);
    CHECK(e.size() == 1);
    CHECK_EQ_STR(e[0].message, "carol forced a takeover from alice");
    CHECK_EQ_STR(e[0].details, "generation 5 · reason operator not answering");
    CHECK(e[0].severity == "warning" && e[0].popup && e[0].category == "control");
    // Without its reason line the grant is still reported (after 2 s, or
    // at the next line).
    t.feed("control", "force user=alice role=supervisor gen=6 from=carol", 6.0);
    CHECK(t.flush(7.0).empty());
    e = t.flush(8.5);
    CHECK(e.size() == 1);
    CHECK_EQ_STR(e[0].message, "alice forced a takeover from carol");
    t.feed("control", "force user=carol role=supervisor gen=7 from=alice", 9.0);
    e = t.feed("control", "all_stop user=carol", 9.2);
    CHECK(e.size() == 2);
    CHECK_EQ_STR(e[0].message, "carol forced a takeover from alice");
    CHECK_EQ_STR(e[1].message, "ALL STOP pressed by carol");
    CHECK(e[1].category == "all_stop" && e[1].severity == "critical");
    e = t.feed("control", "all_stop_reset user=carol", 9.5);
    CHECK_EQ_STR(e[0].message, "ALL STOP reset by carol");
    e = t.feed("control", "lost user=carol (not heard)", 10.0);
    CHECK_EQ_STR(e[0].message, "Control lost: carol not heard");
    CHECK(e[0].popup && e[0].severity == "warning");
    CHECK_EQ_STR(e[0].role, "supervisor");          // from the holder
    CHECK_EQ_STR(t.holder(), "");
    CHECK_EQ_STR(t.lost_holder(), "carol");
    e = t.feed("control", "handover user=alice role=operator gen=9 from=carol", 11.0);
    CHECK_EQ_STR(e[0].message, "Control handed to alice by carol");
    CHECK_EQ_STR(t.holder(), "alice");
    CHECK_EQ_STR(t.lost_holder(), "");
    e = t.feed("control", "request user=bob holder=alice", 12.0);
    CHECK_EQ_STR(e[0].message, "bob asked alice for control");
    e = t.feed("control", "deny user=bob holder=alice", 12.5);
    CHECK_EQ_STR(e[0].message, "alice kept control (bob refused)");
    e = t.feed("control", "cancel user=bob (disconnected)", 12.7);
    CHECK_EQ_STR(e[0].message, "bob's request for control cancelled (disconnected)");
    e = t.feed("auth", "rejected key=DESIRED_THRUST user=bob level=readonly why=level", 13.0);
    CHECK(e[0].category == "refused" && e[0].popup);
    CHECK_EQ_STR(e[0].message, "Command refused: DESIRED_THRUST from bob");
    CHECK_EQ_STR(e[0].details, "why read-only account · level readonly");
    e = t.feed("auth", "rejected key=AUTONOMOUS_START user=- why=stale", 13.5);
    CHECK_EQ_STR(e[0].message, "Command refused: AUTONOMOUS_START from (no login)");
    CHECK_EQ_STR(e[0].details, "why control changed hands before it arrived");
    e = t.feed("auth", "rejected login=logged-out-token ip=10.0.0.4 session=abc", 14.0);
    CHECK_EQ_STR(e[0].message, "Login refused: token of a logged-out session");
    CHECK(e[0].category == "refused");
    CHECK_EQ_STR(e[0].details, "why logged-out session · ip 10.0.0.4 · session abc");
    e = t.feed("control", "refused action=take user=bob reason=controlled by alice", 14.5);
    CHECK(e[0].category == "refused");
    CHECK_EQ_STR(e[0].message, "Control take refused for bob");
    CHECK_EQ_STR(e[0].details, "reason controlled by alice");
    e = t.feed("auth", "logout user=carol ip=192.168.22.9 session=q81mz2c4", 15.0);
    CHECK_EQ_STR(e[0].message, "carol logged out");
    CHECK(t.session_count() == 1);
    e = t.feed("auth", "disconnect user=alice role=operator ip=192.168.22.5 session=k3f9a1x0", 16.0);
    CHECK(t.session_count() == 0);
    CHECK_EQ_STR(t.sessions(), "");
    e = t.feed("control", "release user=alice", 17.0);
    CHECK_EQ_STR(e[0].message, "alice released control");
    CHECK_EQ_STR(e[0].role, "operator");            // from the holder
    CHECK_EQ_STR(t.holder(), "");
    e = t.feed("control", "revoke why=moos-reconnect user=alice", 18.0);
    CHECK_EQ_STR(e[0].message, "Control revoked (moos-reconnect)");
    // A restart of the boat link forgets the holder and the logins.
    t.feed("control", "take user=alice role=operator gen=1", 19.0);
    t.feed("auth", "anonymous level=readonly role=viewer ip=10.0.0.7 session=zz", 19.5);
    CHECK(t.session_count() == 1);
    CHECK_EQ_STR(t.sessions(), "no login (viewer, 10.0.0.7)");
    CHECK(t.open_sessions().size() == 1 && t.open_sessions()[0].key == "zz" && t.open_sessions()[0].user.empty());
    t.feed("control", "start command_auth=signalk exclusive_control=true (nobody in control)", 20.0);
    CHECK_EQ_STR(t.holder(), "");
    CHECK(t.session_count() == 0);
    // A login with the Signal K cookie carries no tab session (usv-722,
    // 23 Oct): kept by user and IP, removed by its disconnect or logout.
    {
        AuditTrail u;
        u.feed("auth", "login user=admin level=admin role=supervisor ip=192.168.22.11 session=-", 1.0);
        CHECK(u.session_count() == 1);
        CHECK_EQ_STR(u.sessions(), "admin (supervisor, 192.168.22.11)");
        u.feed("auth", "login user=bob level=readonly role=viewer ip=192.168.22.12", 2.0);
        CHECK(u.session_count() == 2);
        u.feed("auth", "disconnect user=bob role=viewer ip=192.168.22.12 session=-", 3.0);
        CHECK(u.session_count() == 1);
        u.feed("auth", "logout user=admin ip=192.168.22.11 session=-", 4.0);
        CHECK(u.session_count() == 0);
    }
    // Empty or garbage lines produce nothing.
    CHECK(t.feed("control", "", 21.0).empty());
    CHECK(t.feed("auth", "   ", 21.0).empty());
}

int main() {
    test_remote_and_moving();
    test_motion_latch();
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
    test_camera_readiness();
    test_comms_loss_return();
    test_enc_charts();
    test_v1243_readiness();
    test_audit_trail();
    test_colav_alerts();
    test_min_cpa_pick();

    std::printf("mission_logic_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
