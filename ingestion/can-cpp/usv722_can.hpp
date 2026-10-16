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
//   0CFECAD2        J1939 DM1 (address 210), single frame: B0 lamps, B2..B5 =
//                   one fault (SPN, FMI, occurrence count); all zero = none
//   1CECFFD2 +      the same DM1 with two or more faults, sent as a J1939
//   1CEBFFD2        broadcast (TP.CM BAM + TP.DT), reassembled by Dm1Receiver
//
// Steering scale (all three steering values): 0 = full port, 511 = centre,
// 1012 = full starboard; % = (raw - 511) / 511 to port, / 501 to starboard;
// degrees = % x the drive's maximum angle (50 by default).
//
// Not on this bus: engine RPM, temperatures, oil pressure, trim position
// (behind the engine's own network). Unit-tested by usv722_can_test.cpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../cpp-common/j1939_faults.hpp"

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

// ---------------------------------------------------------------- DM1
// J1939 DM1 (PGN 65226) active diagnostic trouble codes. Payload: B0 lamp
// status (2 bits each: MIL 7-6, red stop 5-4, amber warning 3-2, protect
// 1-0; 01 = on), B1 flash bits, then 4 bytes per fault:
//   SPN = B0 + 256*B1 + 65536*(B2 >> 5), FMI = B2 & 0x1F,
//   occurrence count = B3 & 0x7F.  SPN 0 / FMI 0 = no active fault.
struct Dm1Fault {
    uint32_t spn;
    uint8_t fmi;
    uint8_t oc;
};

struct Dm1 {
    bool codes_available = false;   // false when the fault bytes are all FF
    uint8_t lamps = 0;
    std::vector<Dm1Fault> faults;
};

inline Dm1 ParseDm1(const uint8_t* d, size_t len) {
    Dm1 m;
    if (len < 2) return m;
    m.lamps = d[0];
    for (size_t i = 2; i + 3 < len; i += 4) {
        if (d[i] == 0xFF && d[i + 1] == 0xFF && d[i + 2] == 0xFF) continue;   // padding / n.a.
        m.codes_available = true;
        const uint32_t spn = d[i] | (d[i + 1] << 8) | ((d[i + 2] >> 5) << 16);
        const uint8_t fmi = d[i + 2] & 0x1F;
        if (spn == 0 && fmi == 0) continue;                                  // "no fault"
        m.faults.push_back({spn, fmi, static_cast<uint8_t>(d[i + 3] & 0x7F)});
    }
    return m;
}

// Lamp on = its 2-bit field is 01 (10 reserved, 11 not available).
inline double LampOn(uint8_t lamps, int shift) { return ((lamps >> shift) & 0x03) == 1 ? 1.0 : 0.0; }

inline std::vector<Value> Dm1Values(const Dm1& m) {
    std::vector<Value> out;
    out.push_back({"engine", "fault_lamps", static_cast<double>(m.lamps)});
    out.push_back({"engine", "lamp_protect", LampOn(m.lamps, 0)});
    out.push_back({"engine", "lamp_amber", LampOn(m.lamps, 2)});
    out.push_back({"engine", "lamp_red", LampOn(m.lamps, 4)});
    out.push_back({"engine", "lamp_mil", LampOn(m.lamps, 6)});
    if (!m.codes_available) return out;
    out.push_back({"engine", "active_dtcs", static_cast<double>(m.faults.size())});
    const Dm1Fault first = m.faults.empty() ? Dm1Fault{0, 0, 0} : m.faults.front();
    out.push_back({"engine", "dtc_spn", static_cast<double>(first.spn)});
    out.push_back({"engine", "dtc_fmi", static_cast<double>(first.fmi)});
    out.push_back({"engine", "dtc_oc", static_cast<double>(first.oc)});
    return out;
}

// J1939-73 failure mode wording lives in cpp-common/j1939_faults.hpp.
using usv::j1939::FmiText;

// Field the summary text is written to (string field engine.dtc_text).
inline const Value kDm1TextField = {"engine", "dtc_text", 0.0};

// "NONE", or "SPN 110 FMI 3 voltage high / short to high (x1); SPN ...";
// with mercury = true the Mercury Diesel fault names ("SPN 100 FMI 18 J143
// Oil_Press_Low (x1)") from cpp-common/j1939_faults.hpp. A DM1 may list
// the same SPN / FMI more than once with its own occurrence count (the
// usv-725 ECU lists SPN 639 FMI 9, J1939 message missing, once per message
// it waits for); those entries are folded into one, "(x75, x1, x77)", in
// the order of first appearance, so the text reads as the distinct faults.
inline std::string Dm1Text(const Dm1& m, bool mercury = false) {
    if (!m.codes_available) return "N/A";
    if (m.faults.empty()) return "NONE";
    std::string s;
    for (size_t i = 0; i < m.faults.size(); ++i) {
        const auto& f = m.faults[i];
        bool seen = false;
        for (size_t j = 0; j < i && !seen; ++j)
            seen = m.faults[j].spn == f.spn && m.faults[j].fmi == f.fmi;
        if (seen) continue;
        std::string entry = usv::j1939::FaultText(f.spn, f.fmi, f.oc, mercury);
        for (size_t j = i + 1; j < m.faults.size(); ++j)
            if (m.faults[j].spn == f.spn && m.faults[j].fmi == f.fmi)
                entry.insert(entry.size() - 1, ", x" + std::to_string(m.faults[j].oc));
        if (!s.empty()) s += "; ";
        s += entry;
    }
    return s;
}

// DM1 receiver for one source address: the single-frame DM1 (one fault)
// and the J1939 broadcast transport (TP.CM BAM, TP.DT) used when two or
// more faults are active. Feed every extended frame; returns true with
// `out` filled when a complete DM1 has arrived.
class Dm1Receiver {
public:
    explicit Dm1Receiver(uint8_t source = 0xD2) : src_(source) {}

    bool Feed(uint32_t id, const uint8_t* d, uint8_t len, Dm1* out, bool* multi = nullptr) {
        if ((id & 0xFF) != src_) return false;
        const uint32_t pf_ps = (id >> 8) & 0xFFFF;
        if (pf_ps == 0xFECA) {                                  // single-frame DM1
            *out = ParseDm1(d, len);
            if (multi) *multi = false;
            return true;
        }
        if (pf_ps == 0xECFF && len >= 8 && d[0] == 0x20) {     // TP.CM BAM
            const size_t size = d[1] | (d[2] << 8);
            const uint32_t pgn = d[5] | (d[6] << 8) | (d[7] << 16);
            active_ = pgn == 0xFECA && size >= 2 && size <= 1785 && d[3] > 0 &&
                      static_cast<size_t>(d[3]) * 7 >= size;
            if (active_) { size_ = size; packets_ = d[3]; next_ = 1; buf_.clear(); }
            return false;
        }
        if (pf_ps == 0xEBFF && active_ && len >= 1) {           // TP.DT
            if (d[0] != next_) { active_ = false; return false; }   // lost a packet: drop
            buf_.insert(buf_.end(), d + 1, d + (len < 8 ? len : 8));
            if (++next_ <= packets_) return false;
            active_ = false;
            buf_.resize(size_, 0xFF);
            *out = ParseDm1(buf_.data(), buf_.size());
            if (multi) *multi = true;
            return true;
        }
        return false;
    }

private:
    uint8_t src_;
    bool active_ = false;
    size_t size_ = 0;
    int packets_ = 0, next_ = 1;
    std::vector<uint8_t> buf_;
};

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
        case 0x0CFECAD2:   // J1939 DM1, single frame (0 or 1 active fault)
            if (len >= 6) {
                const auto dm = Dm1Values(ParseDm1(d, len));
                out.insert(out.end(), dm.begin(), dm.end());
            }
            break;
        default:
            break;
    }
    return out;
}

}  // namespace usv722
}  // namespace usv
