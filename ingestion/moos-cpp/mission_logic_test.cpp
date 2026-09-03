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
    CHECK_EQ_STR(DeriveMissionState(f), "ReturnToHome");

    // Pause gates waypt_return too (condition PAUSE=false).
    f.pause = true;
    CHECK_EQ_STR(DeriveMissionState(f), "Paused");
    f.pause = false;

    // Arrival at base: endflags RETURN=false + MOOS_MANUAL_OVERRIDE=true.
    f.returning = false;
    f.manual_override = true;
    CHECK_EQ_STR(DeriveMissionState(f), "Parked");
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
    CHECK_EQ_STR(DeriveMissionState(f), "ReturnToHome");
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

    std::printf("mission_logic_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
