// usv722_can.hpp - steering, throttle, gear, key and fault frames of the
// usv-722 helm / drive CAN bus (slcan0, 250 kbit/s), no I/O.
//
// Source: USV-722-CAN-ID-Mapping.xlsx (captures of 9 Oct 2026). Every frame
// is matched on its full CAN id (source address included), so the same
// J1939 PGN from another sender is never misread.
//
//   0x210 (11-bit)  steering actuator status, about 18 per second
//                   B2 + 256*(B6 & 3)        rudder command it is following
//                   B3 + 256*((B6 >> 2) & 3) rudder ACTUAL position
//                   B4                       steering motor drive, %
//                   B6 bit 4                 set while the PC steers
//   18FF11D0        Glendinning helm (208) wheel, PGN 65297: B0 + 256*B1
//   0CF003D0        helm lever thrust, J1939 EEC2 61443: B1 * 0.4 %
//   00F003E2        thrust sent to the engine (control processor 226)
//   18F005D0        helm lever gear, J1939 ETC2 61445: B0 - 125
//   00F005E2        gear sent to the engine
//   18FF08D0        trim switch, PGN 65288: B0 bit 0 up, bit 2 down
//   18FF0F10        key switch, PGN 65295: 00 off, 01 run, 03 start
//   0CFECAD2        J1939 DM1 (address 210): B2..B5 = active fault, 0 = none
//
// Steering scale (all three steering values): 0 = full port, 511 = centre,
// 1012 = full starboard; % = (raw - 511) / 511 to port, / 501 to starboard;
// degrees = % x the drive's maximum angle (50 by default).
//
// Not on this bus: engine RPM, temperatures, oil pressure, trim position
// (behind the engine's own network). Unit-tested by usv722_can_test.cpp.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace usv {
namespace usv722 {

struct Value {
    const char* measurement;
    const char* field;
    double value;
};

struct SteerCal {
    double port = 0.0;        // raw at full port
    double centre = 511.0;    // raw at centre
    double stbd = 1012.0;     // raw at full starboard
    double max_deg = 50.0;    // rudder angle at full lock
};

// Raw steering count -> degrees (negative = port).
inline double SteerDeg(double raw, const SteerCal& c) {
    if (raw < c.centre) {
        const double span = c.centre - c.port;
        return span > 0 ? (raw - c.centre) / span * c.max_deg : 0.0;
    }
    const double span = c.stbd - c.centre;
    return span > 0 ? (raw - c.centre) / span * c.max_deg : 0.0;
}

// Gear byte (J1939 ETC2): 7C reverse, 7D neutral, 7E forward -> -1 / 0 / 1.
// Anything else (FF = not available) -> false.
inline bool Gear(uint8_t b, double* out) {
    if (b < 0x7C || b > 0x7E) return false;
    *out = static_cast<int>(b) - 0x7D;
    return true;
}

// Thrust byte (J1939 EEC2 byte 1): 0..250 = 0..100 %, above = not available.
inline bool Thrust(uint8_t b, double* out) {
    if (b > 250) return false;
    *out = b * 0.4;
    return true;
}

// Decode one frame. `id` is the CAN id without flag bits, `extended` says
// whether it is a 29-bit id. Unknown frames give an empty vector.
inline std::vector<Value> Decode(uint32_t id, bool extended, const uint8_t* d,
                                 uint8_t len, const SteerCal& cal = SteerCal()) {
    std::vector<Value> out;
    if (!extended) {
        if (id == 0x210 && len >= 7) {
            const double cmd = d[2] + 256.0 * (d[6] & 0x03);
            const double act = d[3] + 256.0 * ((d[6] >> 2) & 0x03);
            out.push_back({"autopilot", "rudder_actual", SteerDeg(act, cal)});
            out.push_back({"autopilot", "rudder_actuator_cmd", SteerDeg(cmd, cal)});
            out.push_back({"autopilot", "steer_drive_pct", static_cast<double>(d[4])});
            out.push_back({"autopilot", "steer_pc_control", (d[6] & 0x10) ? 1.0 : 0.0});
        }
        return out;
    }
    double v = 0.0;
    switch (id) {
        case 0x18FF11D0:   // helm wheel
            if (len >= 2) {
                const double raw = d[0] + 256.0 * d[1];
                if (raw <= 1023) out.push_back({"autopilot", "helm_rudder", SteerDeg(raw, cal)});
            }
            break;
        case 0x0CF003D0:   // helm lever thrust
            if (len >= 2 && Thrust(d[1], &v)) out.push_back({"engine", "throttle_helm_pct", v});
            break;
        case 0x00F003E2:   // thrust to the engine
            if (len >= 2 && Thrust(d[1], &v)) out.push_back({"engine", "throttle_pct", v});
            break;
        case 0x18F005D0:   // helm lever gear
            if (len >= 1 && Gear(d[0], &v)) out.push_back({"engine", "gear_helm", v});
            break;
        case 0x00F005E2:   // gear to the engine
            if (len >= 1 && Gear(d[0], &v)) out.push_back({"engine", "gear", v});
            break;
        case 0x18FF08D0:   // trim switch (button state only)
            if (len >= 1)
                out.push_back({"engine", "trim_switch",
                               (d[0] & 0x01) ? 1.0 : ((d[0] & 0x04) ? -1.0 : 0.0)});
            break;
        case 0x18FF0F10:   // key switch
            if (len >= 1) {
                const uint8_t k = d[0] & 0x03;
                out.push_back({"engine", "key_state", k == 3 ? 2.0 : (k & 1 ? 1.0 : 0.0)});
            }
            break;
        case 0x0CFECAD2:   // J1939 DM1: one active fault per single frame
            if (len >= 6) {
                const bool none = d[2] == 0 && d[3] == 0 && d[4] == 0;
                const bool na = d[2] == 0xFF && d[3] == 0xFF && d[4] == 0xFF;
                if (!na) out.push_back({"engine", "active_dtcs", none ? 0.0 : 1.0});
                out.push_back({"engine", "fault_lamps", static_cast<double>(d[0])});
            }
            break;
        default:
            break;
    }
    return out;
}

}  // namespace usv722
}  // namespace usv
