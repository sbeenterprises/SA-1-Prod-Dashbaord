// Unit tests for vessel_profile.hpp: usv.env parsing, the particulars card
// rows and number formatting. g++ -std=c++17 vessel_profile_test.cpp && ./a.out
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unistd.h>

#include "vessel_profile.hpp"

static int fails = 0, checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { ++fails; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static void clear_env() {
    for (const char* k : {"VESSEL_CLASS", "VESSEL_LOA_M", "VESSEL_BEAM_M", "VESSEL_HEIGHT_M", "VESSEL_DRAFT_M",
                          "VESSEL_PAYLOAD_KG", "VESSEL_FUEL_L", "VESSEL_RANGE_NM", "VESSEL_MAX_SPEED_KN",
                          "VESSEL_PROPULSION", "VESSEL_NAVIGATION", "VESSEL_CAMERAS", "VESSEL_COMMS",
                          "VESSEL_RIDE_CONTROL", "USV_ENV_FILE"})
        unsetenv(k);
}

static std::string write_file(const std::string& body) {
    static int n = 0;
    const std::string path = "/tmp/vessel_profile_test_" + std::to_string(getpid()) + "_" + std::to_string(n++) + ".env";
    std::ofstream(path) << body;
    return path;
}

int main() {
    clear_env();
    // 1. usv-725 as installed.
    {
        auto p = usv::VesselProfile::FromEnv(write_file(
            "VESSEL_CLASS=\"S3 Swift Sea-Stalker\"\nVESSEL_LOA_M=5.9\nVESSEL_BEAM_M=1.8\nVESSEL_HEIGHT_M=2.5\n"
            "VESSEL_PAYLOAD_KG=454\nVESSEL_FUEL_L=1135\nVESSEL_RANGE_NM=1000\nVESSEL_MAX_SPEED_KN=30\n"
            "VESSEL_PROPULSION=\"Mercury Diesel 2.0L, Bravo 3 sterndrive\"\n"
            "VESSEL_NAVIGATION=\"Simrad HALO 24 radar, VN-300 INS\"\n"
            "VESSEL_CAMERAS=\"FLIR M364 thermal, bullet\"\nVESSEL_COMMS=\"Starlink BLOS\"\n"
            "VESSEL_RIDE_CONTROL=\"Seakeeper Ride 450\"\n"));
        CHECK(p.cls == "S3 Swift Sea-Stalker");
        CHECK(p.loa_m == 5.9 && p.beam_m == 1.8 && p.height_m == 2.5 && p.draft_m == 0);
        CHECK(p.fuel_l == 1135 && p.range_nm == 1000 && p.max_speed_kn == 30 && p.payload_kg == 454);
        CHECK(!p.empty());
        auto rows = p.CardRows();
        CHECK(rows.size() == 8);
        CHECK(rows[0].first == "Length x beam x height" && rows[0].second == "5.9 m x 1.8 m x 2.5 m");
        CHECK(rows[1].first == "Payload" && rows[1].second == "454 kg (1,001 lb)");
        CHECK(rows[2].first == "Fuel capacity" && rows[2].second == "1,135 L (300 gal)");
        CHECK(rows[3].first == "Range / max speed" && rows[3].second == "1,000 nm / 30 kn");
        CHECK(rows[4].second == "Mercury Diesel 2.0L, Bravo 3 sterndrive");
        CHECK(rows[6].first == "Cameras / comms" && rows[6].second == "FLIR M364 thermal, bullet | Starlink BLOS");
        CHECK(rows[7].first == "Ride control" && rows[7].second == "Seakeeper Ride 450");
        CHECK(p.Describe().find("vessel_class=\"S3 Swift Sea-Stalker\"") == 0);
    }
    // 2. usv-722: draft and displacement style values, no ride control.
    {
        auto p = usv::VesselProfile::FromEnv(write_file(
            "VESSEL_CLASS=\"S2 Sea-Striker\"\nVESSEL_LOA_M=4.76\nVESSEL_BEAM_M=1.68\nVESSEL_DRAFT_M=1.04\n"
            "VESSEL_PAYLOAD_KG=159\nVESSEL_FUEL_L=265\nVESSEL_RANGE_NM=320\nVESSEL_MAX_SPEED_KN=30\n"
            "VESSEL_PROPULSION=\"Yamaha F115 outboard\"\n"));
        auto rows = p.CardRows();
        CHECK(rows[0].second == "4.8 m x 1.7 m");
        CHECK(rows[1].first == "Draft" && rows[1].second == "1.0 m");
        CHECK(rows[3].second == "265 L (70 gal)");
        CHECK(rows.size() == 6);
    }
    // 3. Nothing set: empty profile, no rows, no crash.
    {
        auto p = usv::VesselProfile::FromEnv(write_file("ENGINE_TYPE=yamaha_outboard\n"));
        CHECK(p.empty() && p.CardRows().empty() && p.cls.empty());
        CHECK(p.Describe() == "vessel=(no particulars)");
    }
    // 4. Environment wins over the file; bad numbers ignored.
    {
        setenv("VESSEL_CLASS", "From env", 1);
        setenv("VESSEL_FUEL_L", "abc", 1);
        auto p = usv::VesselProfile::FromEnv(write_file("VESSEL_CLASS=From file\nVESSEL_FUEL_L=500\nVESSEL_MAX_SPEED_KN=-3\n"));
        CHECK(p.cls == "From env");
        CHECK(p.fuel_l == 0.0);          // "abc" in the environment, not the file's 500
        CHECK(p.max_speed_kn == 0.0);    // negative ignored
        clear_env();
    }
    // 5. Formatting.
    CHECK(usv::VesselProfile::Fmt(1135, 0) == "1,135");
    CHECK(usv::VesselProfile::Fmt(1067.4, 0) == "1,067");
    CHECK(usv::VesselProfile::Fmt(5.9, 1) == "5.9");
    CHECK(usv::VesselProfile::Fmt(1234567, 0) == "1,234,567");
    CHECK(usv::VesselProfile::Fmt(999, 0) == "999");
    std::printf("vessel_profile_test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
