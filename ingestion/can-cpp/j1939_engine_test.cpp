// Unit tests for j1939_engine.hpp and usv725_can.hpp, with frames recorded on
// usv-725 on 15 Oct 2026 (boat on the hard, engine off, ignition on) and the
// PC steering tests of the same night.
//   g++ -std=c++17 -Wall -Wextra j1939_engine_test.cpp -o j1939_engine_test
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "j1939_engine.hpp"
#include "usv725_can.hpp"

static int checks = 0, fails = 0;
#define CHECK(cond) do { ++checks; if (!(cond)) { ++fails; \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static std::vector<uint8_t> Hex(const char* s) {
    std::vector<uint8_t> out;
    for (size_t i = 0; s[i] && s[i + 1]; i += 2) {
        char b[3] = {s[i], s[i + 1], 0};
        out.push_back(static_cast<uint8_t>(std::strtoul(b, nullptr, 16)));
    }
    return out;
}

using Map = std::map<std::string, double>;

static Map Engine(usv::j1939engine::Decoder& dec, uint32_t id, const char* data,
                  usv::usv722::Dm1* dm1 = nullptr, bool* got = nullptr) {
    auto d = Hex(data);
    usv::usv722::Dm1 local;
    bool g = false;
    Map m;
    for (const auto& v : dec.Feed(id, true, d.data(), static_cast<uint8_t>(d.size()),
                                  dm1 ? dm1 : &local, got ? got : &g))
        m[std::string(v.measurement) + "." + v.field] = v.value;
    return m;
}

static Map Helm(uint32_t id, bool ext, const char* data) {
    auto d = Hex(data);
    Map m;
    for (const auto& v : usv::usv725::Decode(id, ext, d.data(), static_cast<uint8_t>(d.size())))
        m[std::string(v.measurement) + "." + v.field] = v.value;
    return m;
}

static bool Near(double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol; }

int main() {
    usv::j1939engine::Decoder dec(0);

    // PGN helper: PDU1 drops the destination, PDU2 keeps the group extension.
    CHECK(usv::j1939engine::Pgn(0x0CF00400) == 61444);
    CHECK(usv::j1939engine::Pgn(0x18EA00F9) == 59904);
    CHECK(usv::j1939engine::Pgn(0x18000017) == 0);
    CHECK(usv::j1939engine::Pgn(0x19F21170) == 127505);

    // EEC1: 0 rpm, actual / demand torque 0 % (7D), controller 23 in B5.
    {
        auto m = Engine(dec, 0x0CF00400, "00FF7D000017F07D");
        CHECK(m.count("engine.rpm") && Near(m["engine.rpm"], 0.0));
        CHECK(Near(m["engine.percent_torque"], 0.0));
        CHECK(Near(m["engine.demand_torque"], 0.0));
        // 700 rpm = 5600 x 0.125 (E0 15), torque 30 %.
        auto r = Engine(dec, 0x0CF00400, "00FF9BE0150017F09B");
        CHECK(Near(r["engine.rpm"], 700.0) && Near(r["engine.percent_torque"], 30.0));
        // Not available (FF FF) is not published.
        auto na = Engine(dec, 0x0CF00400, "FFFFFFFFFFFFFFFF");
        CHECK(na.empty());
    }
    // Any priority: the same PGN at priority 6.
    CHECK(Engine(dec, 0x18F00400, "00FF7D000017F07D").count("engine.rpm") == 1);
    // Another source address is ignored (the helm's EEC2 from 208).
    CHECK(Engine(dec, 0x0CF003D0, "FFFAFFFFFFFFFFFF").empty());

    // EEC2: load 0 %, pedal not available (FF) -> not published.
    {
        auto m = Engine(dec, 0x0CF00300, "FFFF00FFFFFF00FF");
        CHECK(m.count("engine.percent_load") && Near(m["engine.percent_load"], 0.0));
        CHECK(m.count("engine.pedal_position") == 0);
    }
    // ET1: coolant 4A = 34 C, fuel 4F = 39 C, oil 0x2636 x 0.03125 - 273 = 32.6875 C.
    {
        auto m = Engine(dec, 0x18FEEE00, "4A4F3626FFFFFFFF");
        CHECK(Near(m["engine.coolant_temp"], 34.0));
        CHECK(Near(m["engine.fuel_temp"], 39.0));
        CHECK(Near(m["engine.oil_temp"], 32.6875));
    }
    // EFL/P1: oil level E7 = 92.4 %, oil pressure 19 = 100 kPa.
    {
        auto m = Engine(dec, 0x18FEEF00, "FFFFE719FFFFFFFF");
        CHECK(Near(m["engine.oil_level"], 92.4));
        CHECK(Near(m["engine.oil_pressure"], 100.0));
        CHECK(m.size() == 2);
    }
    // LFE: 0 L/h.
    CHECK(Near(Engine(dec, 0x18FEF200, "0000FFFA0000FFFF")["engine.fuel_rate"], 0.0));
    // IC1: boost 32 = 100 kPa, intake 50 = 40 C, exhaust 0x2689 = 35.28 C.
    {
        auto m = Engine(dec, 0x18FEF600, "FF325032FF8926FF");
        CHECK(Near(m["engine.intake_manifold_pressure"], 100.0));
        CHECK(Near(m["engine.intake_manifold_temp"], 40.0));
        CHECK(Near(m["engine.exhaust_temp"], 0x2689 * 0.03125 - 273.0));
    }
    // AMB: baro C9 = 100.5 kPa, air inlet 3C = 20 C.
    {
        auto m = Engine(dec, 0x18FEF500, "C9FFFFFFFF3CFFFF");
        CHECK(Near(m["engine.baro_pressure"], 100.5) && Near(m["engine.air_inlet_temp"], 20.0));
    }
    // VEP1: EC 00 = 11.8 V; F9 00 = 12.45 V on the charger.
    CHECK(Near(Engine(dec, 0x18FEF700, "FFFFFFFFEC00EC00")["engine.ecu_voltage"], 11.8));
    CHECK(Near(Engine(dec, 0x18FEF700, "FFFFFFFFF900F900")["engine.ecu_voltage"], 12.45));
    // HOURS (on request): 0x00003A98 x 0.05 = 750 h.
    CHECK(Near(Engine(dec, 0x18FEE500, "983A0000FFFFFFFF")["engine.runtime_h"], 750.0));
    // WFI: FC = no water, FD = water.
    CHECK(Near(Engine(dec, 0x18FEFF00, "FCDFFFFFFFFFFFFF")["engine.water_in_fuel"], 0.0));
    CHECK(Near(Engine(dec, 0x18FEFF00, "FDDFFFFFFFFFFFFF")["engine.water_in_fuel"], 1.0));

    // DM1 single frame: protect lamp, SPN 639 FMI 9 x1.
    {
        usv::usv722::Dm1 dm;
        bool got = false;
        auto m = Engine(dec, 0x18FECA00, "01FF7F020901FFFF", &dm, &got);
        CHECK(got && m.empty());
        CHECK(dm.lamps == 0x01 && dm.faults.size() == 1);
        CHECK(dm.faults[0].spn == 639 && dm.faults[0].fmi == 9 && dm.faults[0].oc == 1);
        CHECK(usv::usv722::Dm1Text(dm, true).find("SPN 639 FMI 9") == 0);
        // MIL + protect (41).
        Engine(dec, 0x18FECA00, "41FF7F020901FFFF", &dm, &got);
        CHECK(got && usv::usv722::LampOn(dm.lamps, 6) == 1.0 && usv::usv722::LampOn(dm.lamps, 0) == 1.0);
        // No fault: 00 FF 00 00 00 00 -> faults empty, codes available.
        Engine(dec, 0x18FECA00, "00FF00000000FFFF", &dm, &got);
        CHECK(got && dm.faults.empty() && dm.codes_available);
    }
    // The engine's configuration BAMs (PGN 65251, 65249) are not DM1s and
    // produce nothing.
    {
        usv::usv722::Dm1 dm;
        bool got = false;
        Engine(dec, 0x18ECFF00, "201C0004FFE3FE00", &dm, &got);
        CHECK(!got);
        for (const char* p : {"01C015A0007DA040", "021FA0803EA0C05D", "03A04083FFFAE803", "04508714FFFFFFFF"}) {
            auto m = Engine(dec, 0x18EBFF00, p, &dm, &got);
            CHECK(!got && m.empty());
        }
    }
    // DM1 with two faults as a BAM from the engine (built from the same
    // layout): SPN 639 FMI 9 x1 and SPN 100 FMI 18 x2 (J143 Oil_Press_Low).
    {
        usv::j1939engine::Decoder d2(0);
        usv::usv722::Dm1 dm;
        bool got = false;
        Engine(d2, 0x1CECFF00, "200A0002FFCAFE00", &dm, &got);
        Engine(d2, 0x1CEBFF00, "0101FF7F02090164", &dm, &got);
        Engine(d2, 0x1CEBFF00, "02001202FFFFFFFF", &dm, &got);
        CHECK(got && dm.faults.size() == 2);
        CHECK(dm.faults.size() == 2 && dm.faults[1].spn == 100 && dm.faults[1].fmi == 18 && dm.faults[1].oc == 2);
        CHECK(usv::usv722::Dm1Text(dm, true).find("J143") != std::string::npos);
    }
    // Hours request: 18EA00F9 E5 FE 00.
    CHECK(dec.HoursRequestId(249) == 0x18EA00F9u);
    CHECK(usv::j1939engine::Decoder::kHoursRequest[0] == 0xE5 &&
          usv::j1939engine::Decoder::kHoursRequest[1] == 0xFE &&
          usv::j1939engine::Decoder::kHoursRequest[2] == 0x00);
    // Another engine address.
    {
        usv::j1939engine::Decoder d1(1);
        CHECK(Engine(d1, 0x0CF00400, "00FF7D000017F07D").empty());
        CHECK(Engine(d1, 0x0CF00401, "00FF7D000017F07D").count("engine.rpm") == 1);
    }

    // ---------------------------------------------------------- usv725 helm
    // 0x210 command frame (B5 = 00): wheel hard to port, 10 -> -17 degrees.
    {
        auto m = Helm(0x210, false, "00000AFF000004");
        CHECK(m.size() == 1 && Near(m["autopilot.helm_rudder"], -17.0));
        auto c = Helm(0x210, false, "0000FFFF000005");          // 511 centre
        CHECK(Near(c["autopilot.helm_rudder"], 0.0));
        auto s = Helm(0x210, false, "0000FEFF000007");          // 1022 starboard
        CHECK(Near(s["autopilot.helm_rudder"], 17.0));
    }
    // 0x210 status frame (B5 = 64): PC test, following 461 at 460, motor 0 %.
    {
        auto m = Helm(0x210, false, "0005CDCC006465");
        CHECK(Near(m["autopilot.rudder_actual"], (460.0 - 511.0) / 501.0 * 17.0));
        CHECK(Near(m["autopilot.rudder_actuator_cmd"], (461.0 - 511.0) / 501.0 * 17.0));
        CHECK(Near(m["autopilot.steer_drive_pct"], 0.0));
        // Moving at 100 % drive (64): full sweep, position 0x205 = 517 + ...
        auto mv = Helm(0x210, false, "000005AF4B6465");
        CHECK(Near(mv["autopilot.steer_drive_pct"], 75.0));
        // Full starboard reached: FE + 256*3 = 1022 cmd, FF + 256*3 = 1023 position.
        auto fs = Helm(0x210, false, "0000FEFF00646F");
        CHECK(Near(fs["autopilot.rudder_actuator_cmd"], 17.0));
        CHECK(fs["autopilot.rudder_actual"] > 17.0 && fs["autopilot.rudder_actual"] < 17.1);
    }
    // Other B5 values and short frames: nothing.
    CHECK(Helm(0x210, false, "00000AFF001204").empty());
    CHECK(Helm(0x210, false, "00000AFF00").empty());
    CHECK(Helm(0x356, false, "00").empty());
    // Helm lever thrust 0CF003D0: FA = 100 %, 3E = 24.8 %.
    CHECK(Near(Helm(0x0CF003D0, true, "FFFAFFFFFFFFFFFF")["engine.throttle_helm_pct"], 100.0));
    CHECK(Near(Helm(0x0CF003D0, true, "FF3EFFFFFFFFFFFF")["engine.throttle_helm_pct"], 24.8));
    // Gear lever 18F005D0: 7D neutral.
    CHECK(Near(Helm(0x18F005D0, true, "7DFFFFFFFFFFFFFF")["engine.gear_helm"], 0.0));
    CHECK(Near(Helm(0x18F005D0, true, "7EFFFFFFFFFFFFFF")["engine.gear"], 1.0));
    CHECK(Near(Helm(0x18F005D0, true, "7CFFFFFFFFFFFFFF")["engine.gear"], -1.0));
    CHECK(Near(Helm(0x0CF003D0, true, "FF7DFFFFFFFFFFFF")["engine.throttle_pct"], 50.0));
    CHECK(Helm(0x18F005D0, true, "FFFFFFFFFFFFFFFF").empty());   // not available
    // Trim switch at priority 3 (0CFF08D0) and 6 (18FF08D0): F1 up, F4 down, F0 idle.
    CHECK(Near(Helm(0x0CFF08D0, true, "F1FFFFFFFFFFFFFF")["engine.trim_switch"], 1.0));
    CHECK(Near(Helm(0x18FF08D0, true, "F4FFFFFFFFFFFFFF")["engine.trim_switch"], -1.0));
    CHECK(Near(Helm(0x0CFF08D0, true, "F0FFFFFFFFFFFFFF")["engine.trim_switch"], 0.0));
    // Key panel at priority 3 (0CFF0F10, 00) and 0 (00FF0F10, 01 RUN).
    CHECK(Near(Helm(0x0CFF0F10, true, "00")["engine.key_state"], 0.0));
    CHECK(Near(Helm(0x00FF0F10, true, "01")["engine.key_state"], 1.0));
    CHECK(Near(Helm(0x00FF0F10, true, "03")["engine.key_state"], 2.0));
    // Speed request TSC1 from 23: E0 15 = 700 rpm, 40 83 = 4200 rpm, 00 31 = 1568 rpm.
    CHECK(Near(Helm(0x18000017, true, "C1E015E1FFFFFFFF")["engine.speed_request"], 700.0));
    CHECK(Near(Helm(0x18000017, true, "C14083E1FFFFFFFF")["engine.speed_request"], 4200.0));
    CHECK(Near(Helm(0x18000017, true, "C10031E1FFFFFFFF")["engine.speed_request"], 1568.0));
    // Not decoded by the usv725 helm profile: engine PGNs, SA 26, the PC's
    // own 18FF11D0, the 210 DM1, AIS / N2K.
    CHECK(Helm(0x0CF00400, true, "00FF7D000017F07D").empty());
    CHECK(Helm(0x18F30A1A, true, "D96AFFFFFFFFE501").empty());
    CHECK(Helm(0x18FF11D0, true, "CD01FFFFFFFFFFFF").empty());
    CHECK(Helm(0x0CFECAD2, true, "00FF0000007FFFFF").empty());
    CHECK(Helm(0x19F21170, true, "002461FFFFFFFFFF").empty());
    // Default calibration.
    {
        auto c = usv::usv725::DefaultCal();
        CHECK(c.port == 10.0 && c.centre == 511.0 && c.stbd == 1022.0 && c.max_deg == 17.0);
    }

    std::printf("j1939_engine_test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
