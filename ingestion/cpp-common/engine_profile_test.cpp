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
                          "MOOS_ENGINE", "USV_ENV_FILE", "ENGINE_MODEL"})
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
