// Unit tests for usv722_can.hpp, using the frames of
// USV-722-CAN-ID-Mapping.xlsx (captures of 9 Oct 2026).
#include "usv722_can.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

static int g_pass = 0, g_fail = 0;
#define CHECK(e) do { if (e) ++g_pass; else { ++g_fail; \
    std::printf("  [FAIL] %s:%d  %s\n", __FILE__, __LINE__, #e); } } while (0)

using namespace usv::usv722;

static bool Near(double a, double b, double tol = 0.05) { return std::fabs(a - b) < tol; }

static const Value* Find(const std::vector<Value>& v, const char* field) {
    for (const auto& x : v)
        if (std::strcmp(x.field, field) == 0) return &x;
    return nullptr;
}

static std::vector<Value> Ext(uint32_t id, std::initializer_list<uint8_t> b) {
    std::vector<uint8_t> d(b);
    return Decode(id, true, d.data(), static_cast<uint8_t>(d.size()));
}

int main() {
    const SteerCal cal;

    // Steering scale: 0 / 511 / 1012 -> -50 / 0 / +50 deg
    CHECK(Near(SteerDeg(0, cal), -50.0));
    CHECK(Near(SteerDeg(511, cal), 0.0));
    CHECK(Near(SteerDeg(1012, cal), 50.0));
    CHECK(Near(SteerDeg(383, cal), -12.52, 0.05));   // sheet: -0.2505 x 50

    // Actuator status 0x210: "00 FF 7F 7D 00 64 55" -> actual 381, cmd 383
    {
        const uint8_t f[] = {0x00, 0xFF, 0x7F, 0x7D, 0x00, 0x64, 0x55};
        auto v = Decode(0x210, false, f, 7, cal);
        const Value* act = Find(v, "rudder_actual");
        const Value* cmd = Find(v, "rudder_actuator_cmd");
        CHECK(act && Near(act->value, SteerDeg(381, cal)));
        CHECK(act && std::strcmp(act->measurement, "autopilot") == 0);
        CHECK(cmd && Near(cmd->value, SteerDeg(383, cal)));
        const Value* drv = Find(v, "steer_drive_pct");
        CHECK(drv && drv->value == 0.0);
        const Value* pc = Find(v, "steer_pc_control");
        CHECK(pc && pc->value == 1.0);                 // 0x55 bit 4 set
    }
    {   // drive 30 %, helm steering (bit 4 clear), high bits 3 -> full starboard
        const uint8_t f[] = {0x00, 0x04, 0xF4, 0xF4, 0x1E, 0x64, 0x0F};
        auto v = Decode(0x210, false, f, 7, cal);
        CHECK(Near(Find(v, "rudder_actual")->value, 50.0));
        CHECK(Find(v, "steer_drive_pct")->value == 30.0);
        CHECK(Find(v, "steer_pc_control")->value == 0.0);
    }
    CHECK(Decode(0x210, false, (const uint8_t*)"\0\0\0", 3, cal).empty());   // short frame
    CHECK(Decode(0x211, false, (const uint8_t*)"\0\0\0\0\0\0\0", 7, cal).empty());

    // Helm wheel 18FF11D0
    CHECK(Near(Find(Ext(0x18FF11D0, {0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}), "helm_rudder")->value, -50.0));
    CHECK(Near(Find(Ext(0x18FF11D0, {0xFF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}), "helm_rudder")->value, 0.0));
    CHECK(Near(Find(Ext(0x18FF11D0, {0xF4, 0x03, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}), "helm_rudder")->value, 50.0));
    CHECK(Ext(0x18FF11D0, {0xFF, 0xFF}).empty());       // not available

    // Thrust: helm 7B = 49.2 %, engine 3E = 24.8 %, FA = 100 %, FF = n/a
    CHECK(Near(Find(Ext(0x0CF003D0, {0xFF, 0x7B, 0xFF}), "throttle_helm_pct")->value, 49.2));
    CHECK(Near(Find(Ext(0x00F003E2, {0xFF, 0x3E, 0xFF}), "throttle_pct")->value, 24.8));
    CHECK(Near(Find(Ext(0x00F003E2, {0xFF, 0xFA}), "throttle_pct")->value, 100.0));
    CHECK(Ext(0x00F003E2, {0xFF, 0xFF}).empty());

    // Gear: 7E forward, 7D neutral, 7C reverse, FF n/a
    CHECK(Find(Ext(0x18F005D0, {0x7E}), "gear_helm")->value == 1.0);
    CHECK(Find(Ext(0x00F005E2, {0x7D}), "gear")->value == 0.0);
    CHECK(Find(Ext(0x00F005E2, {0x7C}), "gear")->value == -1.0);
    CHECK(Ext(0x00F005E2, {0xFF}).empty());

    // Same PGN from another source address is ignored
    CHECK(Ext(0x0CF00300, {0xFF, 0x7B}).empty());
    CHECK(Ext(0x18F00500, {0x7E}).empty());

    // Trim switch F0 idle, F1 up, F4 down
    CHECK(Find(Ext(0x18FF08D0, {0xF0}), "trim_switch")->value == 0.0);
    CHECK(Find(Ext(0x18FF08D0, {0xF1}), "trim_switch")->value == 1.0);
    CHECK(Find(Ext(0x18FF08D0, {0xF4}), "trim_switch")->value == -1.0);

    // Key switch 00 off, 01 run, 03 start
    CHECK(Find(Ext(0x18FF0F10, {0x00}), "key_state")->value == 0.0);
    CHECK(Find(Ext(0x18FF0F10, {0x01}), "key_state")->value == 1.0);
    CHECK(Find(Ext(0x18FF0F10, {0x03}), "key_state")->value == 2.0);

    // DM1: "00 FF 00 00 00 7F FF FF" = no active fault
    {
        auto v = Ext(0x0CFECAD2, {0x00, 0xFF, 0x00, 0x00, 0x00, 0x7F, 0xFF, 0xFF});
        CHECK(Find(v, "active_dtcs") && Find(v, "active_dtcs")->value == 0.0);
        CHECK(Find(v, "fault_lamps")->value == 0.0);
        auto f = Ext(0x0CFECAD2, {0x04, 0xFF, 0x6E, 0x00, 0x03, 0x01, 0xFF, 0xFF});
        CHECK(Find(f, "active_dtcs")->value == 1.0);
    }

    // DM1 details: amber lamp, SPN 110 (0x6E) FMI 3, occurrence 1
    {
        auto f = Ext(0x0CFECAD2, {0x04, 0xFF, 0x6E, 0x00, 0x03, 0x01, 0xFF, 0xFF});
        CHECK(Find(f, "lamp_amber")->value == 1.0);
        CHECK(Find(f, "lamp_red")->value == 0.0);
        CHECK(Find(f, "lamp_protect")->value == 0.0);
        CHECK(Find(f, "lamp_mil")->value == 0.0);
        CHECK(Find(f, "dtc_spn")->value == 110.0);
        CHECK(Find(f, "dtc_fmi")->value == 3.0);
        CHECK(Find(f, "dtc_oc")->value == 1.0);
        // No fault: SPN/FMI/OC written as 0 so the tiles clear
        auto n = Ext(0x0CFECAD2, {0x00, 0xFF, 0x00, 0x00, 0x00, 0x7F, 0xFF, 0xFF});
        CHECK(Find(n, "dtc_spn")->value == 0.0);
        CHECK(Find(n, "lamp_amber")->value == 0.0);
        // Fault bytes not available: lamps only, no fault count
        auto na = Ext(0x0CFECAD2, {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
        CHECK(Find(na, "active_dtcs") == nullptr);
        CHECK(Find(na, "fault_lamps") != nullptr);
        // Lamp bits: MIL 7-6, red 5-4, amber 3-2, protect 1-0 (01 = on, 11 = n.a.)
        auto l = Ext(0x0CFECAD2, {0x51, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF});   // 01 01 00 01
        CHECK(Find(l, "lamp_mil")->value == 1.0 && Find(l, "lamp_red")->value == 1.0);
        CHECK(Find(l, "lamp_amber")->value == 0.0 && Find(l, "lamp_protect")->value == 1.0);
        auto l3 = Ext(0x0CFECAD2, {0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF});  // all n.a.
        CHECK(Find(l3, "lamp_mil")->value == 0.0 && Find(l3, "lamp_amber")->value == 0.0);
        // 19-bit SPN: high 3 bits in B4 bits 7-5. SPN 524286, FMI 31
        // (all ones, FF FF FF, is "not available" and is skipped)
        const uint8_t big[] = {0x00, 0xFF, 0xFE, 0xFF, 0xFF, 0x05, 0xFF, 0xFF};
        Dm1 m = ParseDm1(big, 8);
        CHECK(m.faults.size() == 1 && m.faults[0].spn == 524286 && m.faults[0].fmi == 31);
        // Summary text
        const uint8_t one[] = {0x04, 0xFF, 0x6E, 0x00, 0x03, 0x01, 0xFF, 0xFF};
        CHECK(Dm1Text(ParseDm1(one, 8)) == "SPN 110 FMI 3 voltage high / short to high (x1)");
        const uint8_t none[] = {0x00, 0xFF, 0x00, 0x00, 0x00, 0x7F, 0xFF, 0xFF};
        CHECK(Dm1Text(ParseDm1(none, 8)) == "NONE");
        const uint8_t nav[] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        CHECK(Dm1Text(ParseDm1(nav, 8)) == "N/A");
        CHECK(std::string(FmiText(31)) == "condition exists");
        CHECK(std::string(FmiText(25)) == "reserved");
    }

    // DM1 receiver: single frame from 210, ignores other senders
    {
        Dm1Receiver rx;
        Dm1 m; bool multi = true;
        const uint8_t one[] = {0x04, 0xFF, 0x6E, 0x00, 0x03, 0x01, 0xFF, 0xFF};
        CHECK(rx.Feed(0x0CFECAD2, one, 8, &m, &multi) && !multi && m.faults.size() == 1);
        CHECK(!rx.Feed(0x0CFECA00, one, 8, &m));               // other source address
        CHECK(!rx.Feed(0x0CF003D0, one, 8, &m));               // other PGN
    }

    // DM1 with three faults via J1939 BAM: 2 + 3*4 = 14 bytes in 2 packets
    {
        const uint8_t payload[14] = {0x10, 0xFF,               // red stop lamp on
                                     0x6E, 0x00, 0x03, 0x02,   // SPN 110 FMI 3 x2
                                     0x64, 0x00, 0x01, 0x01,   // SPN 100 FMI 1 x1
                                     0xBE, 0x00, 0x00, 0x05};  // SPN 190 FMI 0 x5
        const uint8_t cm[] = {0x20, 14, 0, 2, 0xFF, 0xCA, 0xFE, 0x00};
        uint8_t dt1[8] = {1}, dt2[8] = {2, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        std::memcpy(dt1 + 1, payload, 7);
        std::memcpy(dt2 + 1, payload + 7, 7);
        Dm1Receiver rx;
        Dm1 m; bool multi = false;
        CHECK(!rx.Feed(0x1CECFFD2, cm, 8, &m, &multi));
        CHECK(!rx.Feed(0x1CEBFFD2, dt1, 8, &m, &multi));
        CHECK(rx.Feed(0x1CEBFFD2, dt2, 8, &m, &multi) && multi);
        CHECK(m.faults.size() == 3);
        CHECK(m.faults[1].spn == 100 && m.faults[1].fmi == 1 && m.faults[2].spn == 190 && m.faults[2].oc == 5);
        auto v = Dm1Values(m);
        CHECK(Find(v, "active_dtcs")->value == 3.0 && Find(v, "lamp_red")->value == 1.0);
        CHECK(Find(v, "dtc_spn")->value == 110.0);
        CHECK(Dm1Text(m) == "SPN 110 FMI 3 voltage high / short to high (x2); "
                            "SPN 100 FMI 1 below normal, most severe (x1); "
                            "SPN 190 FMI 0 above normal, most severe (x5)");
        // Mercury Diesel hull: the J codes from the Mercury specification
        CHECK(Dm1Text(m, true) == "SPN 110 FMI 3 J144 Sensor_Cool_T_High (x2); "
                                  "SPN 100 FMI 1 J415 Oil_Press_Very_Low (x1); "
                                  "SPN 190 FMI 0 J234 Engine_Overspeed (x5)");
        // A lost packet drops the message; the next BAM still works
        CHECK(!rx.Feed(0x1CECFFD2, cm, 8, &m));
        CHECK(!rx.Feed(0x1CEBFFD2, dt2, 8, &m));               // packet 2 before 1
        CHECK(!rx.Feed(0x1CEBFFD2, dt1, 8, &m));               // stale, not active
        CHECK(!rx.Feed(0x1CECFFD2, cm, 8, &m) && !rx.Feed(0x1CEBFFD2, dt1, 8, &m)
              && rx.Feed(0x1CEBFFD2, dt2, 8, &m) && m.faults.size() == 3);
        // BAM carrying another PGN is ignored
        const uint8_t cm_other[] = {0x20, 14, 0, 2, 0xFF, 0x00, 0xF0, 0x00};
        CHECK(!rx.Feed(0x1CECFFD2, cm_other, 8, &m) && !rx.Feed(0x1CEBFFD2, dt1, 8, &m)
              && !rx.Feed(0x1CEBFFD2, dt2, 8, &m));
        // BAM from another source is ignored
        CHECK(!rx.Feed(0x1CECFF00, cm, 8, &m));
    }

    std::printf("usv722_can_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
