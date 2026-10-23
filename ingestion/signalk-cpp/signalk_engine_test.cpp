// Unit tests for signalk_engine.hpp (SK-1, SK-2, SK-3 in
// docs/requirements-usv-725.md): every engine path in both id forms, the
// unit conversions, the gear enum and the notification tracker.
// Build: g++ -std=c++17 -o signalk_engine_test signalk_engine_test.cpp && ./signalk_engine_test
#include "signalk_engine.hpp"

#include <cmath>
#include <cstdio>
#include <string>

static int fails = 0, checks = 0;
#define CHECK(cond) do { ++checks; if (!(cond)) { ++fails; \
    std::printf("  FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); } } while (0)

static bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

int main() {
    using namespace usv::sk;

    // 1. Engine ids: the N2K instance or the name the server was given.
    CHECK(EngineInstance("0") == 0);
    CHECK(EngineInstance("port") == 0);
    CHECK(EngineInstance("1") == 1);
    CHECK(EngineInstance("starboard") == 1);
    CHECK(EngineInstance("2") == 2);
    CHECK(EngineInstance("main") == -1);
    CHECK(EngineInstance("") == -1);
    CHECK(EngineInstance("1a") == -1);

    // 2. Every numeric path and its conversion (SI -> display units).
    struct V { const char* rest; double in; const char* measurement; const char* field; double out; double tol; };
    const V vectors[] = {
        {"revolutions", 42.0, "engine", "rpm", 2520.0, 0.01},              // Hz
        {"temperature", 358.15, "engine", "coolant_temp", 85.0, 0.01},      // K (n2k-signalk 127489)
        {"coolantTemperature", 353.15, "engine", "coolant_temp", 80.0, 0.01},
        {"oilPressure", 340000.0, "engine", "oil_pressure", 340.0, 0.01},   // Pa
        {"oilTemperature", 363.15, "engine", "oil_temp", 90.0, 0.01},
        {"boostPressure", 186000.0, "engine", "intake_manifold_pressure", 186.0, 0.01},
        {"fuel.rate", 1.0e-5, "engine", "fuel_rate", 36.0, 0.001},           // m3/s -> L/h
        {"fuel.level", 0.91, "engine", "fuel_level", 91.0, 0.01},
        {"runTime", 7200.0, "engine", "runtime_h", 2.0, 0.001},
        {"engineLoad", 0.64, "engine", "percent_load", 64.0, 0.01},
        {"engineTorque", 0.5, "engine", "percent_torque", 50.0, 0.01},
        {"engineLoad", -0.01, "engine", "percent_load", 0.0, 1e-9},          // stopped engine
        {"engineTorque", -0.01, "engine", "percent_torque", 0.0, 1e-9},
        {"drive.trimState", 0.25, "engine", "trim_position", 25.0, 0.01},
        {"alternatorVoltage", 14.2, "electrical", "alternator_voltage", 14.2, 0.001},
    };
    for (const V& v : vectors) {
        EngineField f{};
        CHECK(MapPropulsion(v.rest, v.in, &f));
        CHECK(std::string(f.measurement) == v.measurement);
        CHECK(std::string(f.field) == v.field);
        CHECK(near(f.value, v.out, v.tol));
    }
    {
        EngineField f{};
        CHECK(!MapPropulsion("transmission.oilTemperature", 1.0, &f));   // not charted
        CHECK(!MapPropulsion("label", 1.0, &f));
    }

    // 3. Gear enum.
    {
        double g = 9;
        CHECK(GearValue("Forward", &g) && g == 1);
        CHECK(GearValue("Neutral", &g) && g == 0);
        CHECK(GearValue("Reverse", &g) && g == -1);
        CHECK(!GearValue("Fault", &g));
        CHECK(!GearValue("", &g));
    }

    // 4. Notification names as operator wording.
    CHECK(Words("checkEngine") == "Check Engine");
    CHECK(Words("lowOilPressure") == "Low Oil Pressure");
    CHECK(Words("warning1") == "Warning 1");
    CHECK(Words("egrSystem") == "Egr System");

    // 5. Notification tracker: count, text, lamps, per instance; clearing
    //    by state normal and by null (empty state).
    {
        EngineNotifications n;
        CHECK(n.Active(0) == 0);
        CHECK(n.Text(0) == "NONE");
        CHECK(!n.LampsOf(0).mil && !n.LampsOf(0).red && !n.LampsOf(0).amber);

        CHECK(n.Update(0, "checkEngine", "alarm", "Check Engine"));
        CHECK(!n.Update(0, "checkEngine", "alarm", "Check Engine"));   // repeat: no change
        CHECK(n.Active(0) == 1);
        CHECK(n.Text(0) == "Check Engine (alarm)");
        CHECK(n.LampsOf(0).mil && !n.LampsOf(0).red && !n.LampsOf(0).amber);

        CHECK(n.Update(0, "lowOilPressure", "emergency", "Oil pressure 80 kPa"));
        CHECK(n.Active(0) == 2);
        CHECK(n.Text(0) == "Check Engine (alarm); Low Oil Pressure (emergency): Oil pressure 80 kPa");
        CHECK(n.LampsOf(0).mil && n.LampsOf(0).red && !n.LampsOf(0).amber);

        CHECK(n.Update(1, "waterInFuel", "warn", ""));                     // other engine
        CHECK(n.Active(1) == 1 && n.Active(0) == 2);
        CHECK(n.Text(1) == "Water In Fuel (warn)");
        CHECK(n.LampsOf(1).amber && !n.LampsOf(1).mil && !n.LampsOf(1).red);

        CHECK(n.Update(0, "checkEngine", "normal", ""));                   // bit cleared
        CHECK(n.Active(0) == 1);
        CHECK(!n.Update(0, "checkEngine", "normal", ""));                  // already clear
        CHECK(n.Update(0, "lowOilPressure", "", ""));                      // null value
        CHECK(n.Active(0) == 0);
        CHECK(n.Text(0) == "NONE");
        CHECK(n.Update(1, "waterInFuel", "normal", ""));
        CHECK(n.Active(1) == 0);
    }

    std::printf("signalk_engine_test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
