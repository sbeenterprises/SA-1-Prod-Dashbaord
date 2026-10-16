// Unit tests for engine_profile.hpp: env-file parsing, the switches derived
// from ENGINE_SOURCE, explicit overrides and the tag string.
// Build: g++ -std=c++17 -o engine_profile_test engine_profile_test.cpp && ./engine_profile_test
#include "engine_profile.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

static int fails = 0, checks = 0;
#define CHECK(cond) do { ++checks; if (!(cond)) { ++fails; \
    std::printf("  FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void clear_env() {
    for (const char* k : {"ENGINE_TYPE", "ENGINE_SOURCE", "ENGINE_COUNT", "ENGINE_INSTANCES",
                          "HELM_PROFILE", "SIGNALK_ENGINE", "CAN_N2K_ENGINE", "CAN_DM1",
                          "MOOS_ENGINE", "USV_ENV_FILE", "ENGINE_MODEL", "CAN_J1939_ENGINE",
                          "J1939_ENGINE_SA"})
        unsetenv(k);
}

static std::string write_file(const std::string& body) {
    const std::string path = "/tmp/engine_profile_test.env";
    std::ofstream(path) << body;
    return path;
}

int main() {
    // 1. Env file parsing: comments, quotes, trailing comments, spaces.
    {
        auto m = usv::ReadEnvFile(write_file(
            "# comment\n"
            "VESSEL_ID=usv-725\n"
            "ENGINE_TYPE=mercury_diesel   # trailing comment\n"
            "BRAND_TITLE=\"Swiftships USV Console\"\n"
            "  SPACED = value with spaces \n"
            "EMPTY=\n"
            "NOEQUALS\n"));
        CHECK(m["VESSEL_ID"] == "usv-725");
        CHECK(m["ENGINE_TYPE"] == "mercury_diesel");
        CHECK(m["BRAND_TITLE"] == "Swiftships USV Console");
        CHECK(m["SPACED"] == "value with spaces");
        CHECK(m.count("EMPTY") == 1 && m["EMPTY"].empty());
        CHECK(m.count("NOEQUALS") == 0);
        CHECK(m.count("# comment") == 0);
    }
    // 2. Unset source: every producer on (pre-v1.2.20 behaviour).
    {
        clear_env();
        auto p = usv::EngineProfile::FromEnv(write_file("VESSEL_ID=usv-01\n"));
        CHECK(p.source.empty());
        CHECK(p.signalk_engine && p.moos_engine && p.can_n2k_engine && p.can_dm1);
        CHECK(p.helm_profile == "usv722");
        CHECK(p.count == 1 && p.instances == "0");
        CHECK(p.Tags("instance=0") == "instance=0");
        CHECK(p.Tags("") == "");
    }
    // 3. usv-725: moos source from the file, no environment.
    {
        clear_env();
        auto p = usv::EngineProfile::FromEnv(write_file(
            "ENGINE_TYPE=mercury_diesel\nENGINE_SOURCE=moos\nHELM_PROFILE=none\n"));
        CHECK(p.type == "mercury_diesel");
        CHECK(p.source == "moos");
        CHECK(p.moos_engine);
        CHECK(!p.signalk_engine);
        CHECK(!p.can_n2k_engine);
        CHECK(!p.can_dm1);
        CHECK(p.helm_profile == "none");
        CHECK(p.model == "Mercury diesel");                     // default name
        CHECK(p.Tags("instance=0") == "instance=0,engine_type=mercury_diesel,engine_model=Mercury\\ diesel");
        CHECK(p.Tags("") == "engine_type=mercury_diesel,engine_model=Mercury\\ diesel");
    }
    // 4. usv-722: signalk source from the environment wins over the file.
    {
        clear_env();
        setenv("ENGINE_SOURCE", "signalk", 1);
        setenv("ENGINE_TYPE", "Yamaha Outboard", 1);
        setenv("ENGINE_COUNT", "2", 1);
        auto p = usv::EngineProfile::FromEnv(write_file("ENGINE_SOURCE=moos\n"));
        CHECK(p.source == "signalk");
        CHECK(p.type == "yamaha_outboard");   // slug: no spaces in a tag value
        CHECK(p.signalk_engine);
        CHECK(!p.moos_engine);
        CHECK(!p.can_n2k_engine);
        CHECK(p.can_dm1);                     // Glendinning DM1 stays on
        CHECK(p.count == 2 && p.instances == "0,1");
    }
    // 5. can source, explicit override of one switch, bad values ignored.
    {
        clear_env();
        setenv("ENGINE_SOURCE", "CAN", 1);
        setenv("CAN_DM1", "0", 1);
        setenv("SIGNALK_ENGINE", "maybe", 1);
        setenv("ENGINE_COUNT", "7", 1);
        auto p = usv::EngineProfile::FromEnv(write_file(""));
        CHECK(p.source == "can");
        CHECK(p.can_n2k_engine);
        CHECK(!p.can_dm1);                    // override
        CHECK(!p.signalk_engine);             // "maybe" ignored, derived value kept
        CHECK(p.count == 1);
    }
    // 6. Unknown source falls back to unset.
    {
        clear_env();
        setenv("ENGINE_SOURCE", "smartcraft", 1);
        auto p = usv::EngineProfile::FromEnv(write_file(""));
        CHECK(p.source.empty());
        CHECK(p.signalk_engine && p.moos_engine && p.can_n2k_engine && p.can_dm1);
    }
    // 7. USV_ENV_FILE names the file when none is passed.
    {
        clear_env();
        setenv("USV_ENV_FILE", write_file("ENGINE_SOURCE=moos\n").c_str(), 1);
        auto p = usv::EngineProfile::FromEnv();
        CHECK(p.source == "moos");
    }
    // 9. ENGINE_MODEL: quoted in the file, spaces escaped in the tag.
    {
        clear_env();
        auto p = usv::EngineProfile::FromEnv(write_file(
            "ENGINE_TYPE=yamaha_outboard\nENGINE_MODEL=\"Yamaha Outboard Engine F115 B 4-Stroke\"\n"));
        CHECK(p.model == "Yamaha Outboard Engine F115 B 4-Stroke");
        CHECK(p.Tags("instance=0") ==
              "instance=0,engine_type=yamaha_outboard,engine_model=Yamaha\\ Outboard\\ Engine\\ F115\\ B\\ 4-Stroke");
        CHECK(usv::EngineProfile::EscapeTag("a,b=c d") == "a\\,b\\=c\\ d");
        clear_env();
        auto q = usv::EngineProfile::FromEnv(write_file("ENGINE_SOURCE=moos\n"));
        CHECK(q.model.empty() && q.Tags("instance=0") == "instance=0");   // no type, no tags
    }
    // 10. The usv-722 usv.env as installed (v1.2.27): every switch keeps the
    //     value it had before the J1939 engine decoder existed.
    {
        clear_env();
        auto p = usv::EngineProfile::FromEnv(write_file(
            "VESSEL_ID=usv-722\nGRAFANA_PORT=3100\nCAN_INTERFACE=slcan0\n"
            "ENGINE_TYPE=yamaha_outboard\n"
            "ENGINE_MODEL=\"Yamaha Outboard Engine F115 B 4-Stroke\"\n"
            "ENGINE_SOURCE=signalk\nHELM_PROFILE=usv722\nSIGNALK_FUEL_TANK=none\n"
            "SIGNALK_BATTERY_CURRENT_SIGN=-1\nNAV_SPEED_UNITS=knots\n"));
        CHECK(p.source == "signalk" && p.helm_profile == "usv722");
        CHECK(p.signalk_engine && !p.moos_engine && !p.can_n2k_engine && p.can_dm1);
        CHECK(!p.can_j1939_engine);           // new decoder stays off on usv-722
        CHECK(p.j1939_engine_sa == 0);
    }
    // 11. usv-725 with the engine read from the bus by can_bridge.
    {
        clear_env();
        auto p = usv::EngineProfile::FromEnv(write_file(
            "VESSEL_ID=usv-725\nENGINE_TYPE=mercury_diesel\n"
            "ENGINE_MODEL=\"Mercury Diesel 2.0L\"\nENGINE_SOURCE=can\nHELM_PROFILE=usv725\n"));
        CHECK(p.source == "can" && p.helm_profile == "usv725");
        CHECK(p.can_j1939_engine && p.can_n2k_engine);
        CHECK(!p.can_dm1);                    // the engine's own DM1 owns the fault fields
        CHECK(!p.moos_engine && !p.signalk_engine);
        CHECK(p.j1939_engine_sa == 0);
        CHECK(p.Describe().find("can_j1939_engine=1 (engine SA 0)") != std::string::npos);
    }
    // 12. J1939_ENGINE_SA in decimal or hex; out of range ignored; the
    //     decoder never turns on without ENGINE_SOURCE=can or the override.
    {
        clear_env();
        auto a = usv::EngineProfile::FromEnv(write_file("ENGINE_SOURCE=can\nJ1939_ENGINE_SA=0x01\n"));
        CHECK(a.j1939_engine_sa == 1);
        auto b = usv::EngineProfile::FromEnv(write_file("ENGINE_SOURCE=can\nJ1939_ENGINE_SA=300\n"));
        CHECK(b.j1939_engine_sa == 0);
        auto c = usv::EngineProfile::FromEnv(write_file("J1939_ENGINE_SA=0\n"));
        CHECK(!c.can_j1939_engine);           // ENGINE_SOURCE unset
        auto d = usv::EngineProfile::FromEnv(write_file("ENGINE_SOURCE=moos\n"));
        CHECK(!d.can_j1939_engine);
        auto e = usv::EngineProfile::FromEnv(write_file("ENGINE_SOURCE=signalk\nCAN_J1939_ENGINE=1\n"));
        CHECK(e.can_j1939_engine);            // explicit override only
    }
    // 13. An env file that cannot be opened (missing here; a mode 600 root
    //     file for grafana_bridge on the boats, v1.2.28 on usv-725) gives the
    //     default profile and says so in Describe(); a readable one does not.
    {
        clear_env();
        auto a = usv::EngineProfile::FromEnv("/nonexistent/usv.env");
        CHECK(!a.env_file_readable && a.env_file == "/nonexistent/usv.env");
        CHECK(a.source.empty() && a.moos_engine && a.signalk_engine);
        CHECK(a.Describe().find("WARNING: cannot read /nonexistent/usv.env") != std::string::npos);
        auto b = usv::EngineProfile::FromEnv(write_file("ENGINE_SOURCE=can\n"));
        CHECK(b.env_file_readable && b.Describe().find("WARNING") == std::string::npos);
        // Keys from the environment never touch the file: no warning either.
        setenv("ENGINE_SOURCE", "can", 1);
        setenv("ENGINE_TYPE", "mercury_diesel", 1);
        auto c = usv::EngineProfile::FromEnv("/nonexistent/usv.env");
        CHECK(c.source == "can");
        clear_env();
    }
    // 8. ENGINE_* instance suffix (REC-7): ENGINE_SPEED_1 is instance 1.
    {
        int inst = 9;
        CHECK(usv::EngineVarBase("ENGINE_SPEED", &inst) == "ENGINE_SPEED" && inst == 0);
        CHECK(usv::EngineVarBase("ENGINE_SPEED_1", &inst) == "ENGINE_SPEED" && inst == 1);
        CHECK(usv::EngineVarBase("ENGINE_OIL_PRESSURE_2", &inst) == "ENGINE_OIL_PRESSURE" && inst == 2);
        CHECK(usv::EngineVarBase("ENGINE_DTC_SPN", &inst) == "ENGINE_DTC_SPN" && inst == 0);
        CHECK(usv::EngineVarBase("ENGINE_SPEED_", &inst) == "ENGINE_SPEED_" && inst == 0);
        CHECK(usv::EngineVarBase("ENGINE_SPEED_123", &inst) == "ENGINE_SPEED_123" && inst == 0);  // not an instance
        CHECK(usv::EngineVarBase("NAV_X", &inst) == "NAV_X" && inst == 0);
    }
    std::printf("engine_profile_test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
