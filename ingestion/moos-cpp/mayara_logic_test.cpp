// Unit tests for mayara_logic.hpp (mayara targets -> RADAR_TARGET). No MOOS
// needed; needs nlohmann-json:
//   g++ -std=c++17 mayara_logic_test.cpp -o t && ./t

#include "mayara_logic.hpp"

#include <cstdio>
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

using namespace usv::mayara;
using json = nlohmann::json;

static void test_radar_ids() {
    // 3.x radar list
    auto ids = RadarIds(json::parse(
        R"({"version":"3.0.0","radars":{"nav6959":{"name":"HALO","brand":"Navico"}}})"));
    CHECK(ids.size() == 1);
    CHECK(!ids.empty() && ids[0] == "nav6959");
    // bare id-keyed object
    ids = RadarIds(json::parse(R"({"nav1":{"name":"a"},"nav2":{"name":"b"}})"));
    CHECK(ids.size() == 2);
    // array forms (older builds)
    ids = RadarIds(json::parse(R"([{"id":"nav1"},{"id":7},"nav3",{"x":1}])"));
    CHECK(ids.size() == 3);
    CHECK(ids.size() == 3 && ids[1] == "7" && ids[2] == "nav3");
    // nothing discovered
    CHECK(RadarIds(json::parse(R"({"version":"3","radars":{}})")).empty());
    CHECK(RadarIds(json::parse("null")).empty());
}

static void test_power_state() {
    CHECK(PowerState(json::parse(R"({"id":"power","value":2})")) == kPowerTransmit);
    CHECK(PowerState(json::parse(R"({"id":"power","value":1})")) == kPowerStandby);
    CHECK(PowerState(json::parse(R"({"value":"transmit"})")) == kPowerTransmit);
    CHECK(PowerState(json::parse(R"({"value":"Standby"})")) == kPowerStandby);
    CHECK(PowerState(json::parse(R"({"value":"2"})")) == kPowerTransmit);
    CHECK(PowerState(json::parse("0")) == kPowerOff);
    CHECK(PowerState(json::parse(R"({"id":"power"})")) == kPowerUnknown);
    CHECK(PowerState(json::parse(R"({"value":5})")) == kPowerUnknown);
}

static void test_full_target() {
    // bearing 2.335 rad = 133.8 deg, course 4.06 rad = 232.6 deg,
    // 2.93 m/s = 5.70 kn
    const json t = json::parse(R"({
        "id": 3, "status": "tracking",
        "position": {"bearing": 2.335, "distance": 1067,
                     "latitude": 29.71, "longitude": -91.14},
        "motion": {"course": 4.06, "speed": 2.93},
        "danger": {"cpa": 350.12, "tcpa": 138.4, "isDangerous": true},
        "acquisition": "auto", "sourceZone": 1})");
    std::string spec, id;
    CHECK(TargetToSpec(t, Options{}, &spec, &id));
    CHECK_EQ_STR(spec,
                 "id=R3,range=1067.0,bearing=133.8,spd=5.70,cog=232.6,cpa=350.1,tcpa=138");
    CHECK_EQ_STR(id, "R3");
}

static void test_partial_targets() {
    std::string spec;
    // No motion / danger yet: position only
    CHECK(TargetToSpec(json::parse(
        R"({"id":4,"status":"tracking","position":{"bearing":0,"distance":500}})"),
        Options{}, &spec));
    CHECK_EQ_STR(spec, "id=R4,range=500.0,bearing=0.0");

    // Stationary target: zero motion is posted
    CHECK(TargetToSpec(json::parse(
        R"({"id":5,"position":{"bearing":1.5707963,"distance":200},"motion":{"course":0,"speed":0}})"),
        Options{}, &spec));
    CHECK_EQ_STR(spec, "id=R5,range=200.0,bearing=90.0,spd=0.00,cog=0.0");

    // Passed CPA (negative TCPA): CPA/TCPA dropped, no stale alert
    CHECK(TargetToSpec(json::parse(
        R"({"id":6,"position":{"bearing":3.14159265,"distance":800},"danger":{"cpa":120,"tcpa":-45}})"),
        Options{}, &spec));
    CHECK_EQ_STR(spec, "id=R6,range=800.0,bearing=180.0");

    // Negative bearing wraps into [0,360)
    CHECK(TargetToSpec(json::parse(
        R"({"id":7,"position":{"bearing":-0.1745329,"distance":300}})"),
        Options{}, &spec));
    CHECK_EQ_STR(spec, "id=R7,range=300.0,bearing=350.0");

    // Bearing of exactly 2*pi reads 0, not 360
    CHECK(TargetToSpec(json::parse(
        R"({"id":8,"position":{"bearing":6.283185307179586,"distance":300}})"),
        Options{}, &spec));
    CHECK_EQ_STR(spec, "id=R8,range=300.0,bearing=0.0");

    // Numeric strings are accepted
    CHECK(TargetToSpec(json::parse(
        R"({"id":"9","position":{"bearing":"0","distance":"250"}})"),
        Options{}, &spec));
    CHECK_EQ_STR(spec, "id=R9,range=250.0,bearing=0.0");
}

static void test_rejected_targets() {
    std::string spec = "unchanged";
    const Options o;
    CHECK(!TargetToSpec(json::parse(
        R"({"id":1,"status":"lost","position":{"bearing":1,"distance":100}})"), o, &spec));
    CHECK(!TargetToSpec(json::parse(
        R"({"id":1,"status":"acquiring","position":{"bearing":1,"distance":100}})"), o, &spec));
    CHECK(!TargetToSpec(json::parse(R"({"id":1,"status":"tracking"})"), o, &spec));
    CHECK(!TargetToSpec(json::parse(
        R"({"id":1,"position":{"distance":100}})"), o, &spec));
    CHECK(!TargetToSpec(json::parse(
        R"({"position":{"bearing":1,"distance":100}})"), o, &spec));
    CHECK(!TargetToSpec(json::parse(
        R"({"id":1,"position":{"bearing":1,"distance":-5}})"), o, &spec));
    CHECK(!TargetToSpec(json::parse(
        R"({"id":1,"position":{"bearing":"abc","distance":100}})"), o, &spec));
    CHECK(!TargetToSpec(json::parse("[]"), o, &spec));
    CHECK_EQ_STR(spec, "unchanged");

    // Acquiring is posted when asked for
    Options acq;
    acq.include_acquiring = true;
    CHECK(TargetToSpec(json::parse(
        R"({"id":1,"status":"acquiring","position":{"bearing":0,"distance":100}})"), acq, &spec));
    CHECK_EQ_STR(spec, "id=R1,range=100.0,bearing=0.0");
}

static void test_prefix_and_lists() {
    Options o;
    o.id_prefix = "HALO-";
    const json arr = json::parse(R"([
        {"id":1,"status":"tracking","position":{"bearing":0,"distance":100}},
        {"id":2,"status":"lost","position":{"bearing":0,"distance":100}},
        {"id":3,"status":"tracking","position":{"bearing":0,"distance":300}}])");
    auto specs = TargetsToSpecs(arr, o);
    CHECK(specs.size() == 2);
    CHECK(specs.size() == 2 && specs[0] == "id=HALO-1,range=100.0,bearing=0.0");
    CHECK(specs.size() == 2 && specs[1] == "id=HALO-3,range=300.0,bearing=0.0");

    // {"targets":[...]} wrapper
    specs = TargetsToSpecs(json::parse(
        R"({"targets":[{"id":1,"position":{"bearing":0,"distance":100}}]})"), Options{});
    CHECK(specs.size() == 1);

    // id-keyed object without an id field inside
    specs = TargetsToSpecs(json::parse(
        R"({"12":{"position":{"bearing":0,"distance":100}}})"), Options{});
    CHECK(specs.size() == 1 && specs[0] == "id=R12,range=100.0,bearing=0.0");

    // Empty / unexpected payloads
    CHECK(TargetsToSpecs(json::parse("[]"), Options{}).empty());
    CHECK(TargetsToSpecs(json::parse("null"), Options{}).empty());
    CHECK(TargetsToSpecs(json::parse(R"({"error":"Target tracking not enabled"})"),
                         Options{}).empty());
}

int main() {
    test_radar_ids();
    test_power_state();
    test_full_target();
    test_partial_targets();
    test_rejected_targets();
    test_prefix_and_lists();
    std::printf("mayara_logic_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
