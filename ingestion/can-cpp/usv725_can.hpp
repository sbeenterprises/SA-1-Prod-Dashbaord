// usv725_can.hpp - helm / drive frames of the usv-725 CAN bus (slcan0,
// 250 kbit/s), no I/O. Selected by HELM_PROFILE=usv725; the usv-722 decoder
// (usv722_can.hpp) is not used on this boat and is unchanged.
//
// Source: candump captures on usv-725, 15 Oct 2026. J1939
// frames are matched on PGN and source address with any priority: the
// usv-725 trim and key frames carry priority 3 / 0 where usv-722 has 6.
//
//   0x210 (11-bit)  steering controller / actuator, two frame kinds told
//                   apart by B5:
//                   B5 = 00  wheel command: B2 + 256*(B6 & 3)  -> helm_rudder
//                   B5 = 64  actuator status:
//                            B3 + 256*((B6 >> 2) & 3) position -> rudder_actual
//                            B2 + 256*(B6 & 3) command followed -> rudder_actuator_cmd
//                            B4 motor drive %                  -> steer_drive_pct
//   EEC2  61443 from 208 (helm)  B1 x 0.4 % lever thrust -> engine.throttle_helm_pct
//                                                             and engine.throttle_pct
//   ETC2  61445 from 208         B0 - 125 gear lever      -> engine.gear_helm
//                                                             and engine.gear
//                   usv-725 has no control processor (usv-722: address 226)
//                   between the lever and the engine: the controller at 23
//                   acts on these lever messages, whether the helm or the PC
//                   sends them, so they are also the thrust and gear sent to
//                   the engine (the Gear and Engine Thrust tiles).
//   65288 from 208               B0 bit 0 up, bit 2 down  -> engine.trim_switch
//   65295 from 16 (key panel)    00 off, 01 run, 03 start -> engine.key_state
//   TSC1  PGN 0 from 23 to the engine  B1-2 x 0.125 rpm   -> engine.speed_request
//                   (the controller's engine speed request: 700 rpm idle to
//                   4200 rpm at full lever, rpm = 700 + 35 x lever %)
//
// Steering scale: 10 = full port, 511 = centre, 1022 = full starboard; the
// drive turns 17 degrees each side (measured 15 Oct 2026). STEER_RAW_PORT /
// STEER_RAW_CENTER / STEER_RAW_STBD / STEER_MAX_DEG override it.
//
// Not decoded: the 210 "who it follows" byte (B1; its usv-722 coding does
// not hold here) and the Glendinning DM1 from address 210 (the engine's
// own DM1 owns the fault fields when ENGINE_SOURCE=can). Unit-tested by
// usv725_can_test.cpp.
#pragma once

#include <cstdint>
#include <vector>

#include "j1939_engine.hpp"
#include "usv722_can.hpp"

namespace usv {
namespace usv725 {

using usv722::Value;
using usv722::SteerCal;

inline SteerCal DefaultCal() {
    SteerCal c;
    c.port = 10.0;
    c.centre = 511.0;
    c.stbd = 1022.0;
    c.max_deg = 17.0;
    return c;
}

constexpr uint8_t kHelm = 0xD0;        // 208, Glendinning helm head
constexpr uint8_t kKeyPanel = 0x10;    // 16
constexpr uint8_t kController = 0x17;  // 23, sends TSC1 to the engine

inline std::vector<Value> Decode(uint32_t id, bool extended, const uint8_t* d, uint8_t len,
                                 const SteerCal& cal = DefaultCal()) {
    std::vector<Value> out;
    if (!extended) {
        if (id == 0x210 && len >= 7) {
            const double cmd = d[2] + 256.0 * (d[6] & 0x03);
            if (d[5] == 0x00) {
                out.push_back({"autopilot", "helm_rudder", usv722::SteerDeg(cmd, cal)});
            } else if (d[5] == 0x64) {
                const double act = d[3] + 256.0 * ((d[6] >> 2) & 0x03);
                out.push_back({"autopilot", "rudder_actual", usv722::SteerDeg(act, cal)});
                out.push_back({"autopilot", "rudder_actuator_cmd", usv722::SteerDeg(cmd, cal)});
                if (d[4] <= 100)
                    out.push_back({"autopilot", "steer_drive_pct", static_cast<double>(d[4])});
            }
        }
        return out;
    }
    const uint32_t pgn = j1939engine::Pgn(id);
    const uint8_t sa = j1939engine::Source(id);
    double v = 0.0;
    if (sa == kHelm) {
        switch (pgn) {
            case 61443:   // EEC2, lever thrust
                if (len >= 2 && usv722::Thrust(d[1], &v)) {
                    out.push_back({"engine", "throttle_helm_pct", v});
                    out.push_back({"engine", "throttle_pct", v});
                }
                break;
            case 61445:   // ETC2, lever gear
                if (len >= 1 && usv722::Gear(d[0], &v)) {
                    out.push_back({"engine", "gear_helm", v});
                    out.push_back({"engine", "gear", v});
                }
                break;
            case 65288:   // trim switch (button state only)
                if (len >= 1)
                    out.push_back({"engine", "trim_switch",
                                   (d[0] & 0x01) ? 1.0 : ((d[0] & 0x04) ? -1.0 : 0.0)});
                break;
            default:
                break;
        }
    } else if (sa == kKeyPanel && pgn == 65295 && len >= 1) {
        const uint8_t k = d[0] & 0x03;
        out.push_back({"engine", "key_state", k == 3 ? 2.0 : (k & 1 ? 1.0 : 0.0)});
    } else if (sa == kController && pgn == 0 && len >= 3) {
        uint32_t raw = 0;
        if (j1939engine::U16(d, len, 1, &raw)) out.push_back({"engine", "speed_request", raw * 0.125});
    }
    return out;
}

}  // namespace usv725
}  // namespace usv
