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

    std::printf("usv722_can_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
