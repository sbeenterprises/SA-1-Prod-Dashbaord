// j1939_engine.hpp - SAE J1939 engine ECU decoder for can_bridge, no I/O.
//
// Reads the engine's own broadcasts on the drive bus, so the dashboard does
// not depend on any MOOS application for engine data. Switched on by
// ENGINE_SOURCE=can (EngineProfile::can_j1939_engine); the engine's source
// address is J1939_ENGINE_SA (0 by default: the Mercury Diesel 2.0L ECU on
// usv-725). Frames are matched on PGN and source address, any priority.
//
// Source: candump captures on usv-725, 15 Oct 2026 (engine off),
// and SAE J1939-71. Byte numbers below are 0-based (B0 = first byte).
//
//   PGN 61444 EEC1  B2 actual torque - 125 %       -> percent_torque
//                   B3-4 x 0.125 rpm               -> rpm
//                   B7 demand torque - 125 %       -> demand_torque
//   PGN 61443 EEC2  B1 x 0.4 % accelerator pedal   -> pedal_position
//                   B2 % load at current speed     -> percent_load
//   PGN 65262 ET1   B0 - 40 C coolant              -> coolant_temp
//                   B1 - 40 C fuel                 -> fuel_temp
//                   B2-3 x 0.03125 - 273 C oil     -> oil_temp
//   PGN 65263 EFL/P1 B2 x 0.4 % oil level          -> oil_level
//                   B3 x 4 kPa oil pressure        -> oil_pressure
//   PGN 65266 LFE   B0-1 x 0.05 L/h                -> fuel_rate
//   PGN 65270 IC1   B1 x 2 kPa boost               -> intake_manifold_pressure
//                   B2 - 40 C intake manifold      -> intake_manifold_temp
//                   B5-6 x 0.03125 - 273 C exhaust -> exhaust_temp
//   PGN 65269 AMB   B0 x 0.5 kPa barometric        -> baro_pressure
//                   B5 - 40 C air inlet            -> air_inlet_temp
//   PGN 65271 VEP1  B4-5 x 0.05 V battery at ECU   -> ecu_voltage
//   PGN 65253 HOURS B0-3 x 0.05 h                  -> runtime_h (sent only
//                   when requested: HoursRequest() builds the PGN 59904
//                   request, sent by can_bridge when CAN_ENGINE_HOURS_REQUEST=1)
//   PGN 65279 WFI   B0 bits 0-1: 00 no, 01 yes     -> water_in_fuel
//   PGN 65226 DM1   single frame, or the BAM transport (TP.CM 0xECFF + TP.DT
//                   0xEBFF) for two or more faults: lamps, SPN / FMI / count
//                   (usv722::Dm1Receiver, the same reassembly as usv-722)
//
// A raw value in the J1939 "not available" / "error" range (1 byte above
// 250, 2 bytes above 0xFAFF, 4 bytes above 0xFAFFFFFF) is not published, so
// the dashboard reads N/A, never 0. Fuel level is not read from the engine
// (the Mercury ECU does not send PGN 65276): it comes from the NMEA 2000
// tank sender, PGN 127505, in can_bridge. Unit-tested by j1939_engine_test.cpp.
#pragma once

#include <cstdint>
#include <vector>

#include "usv722_can.hpp"

namespace usv {
namespace j1939engine {

using usv722::Value;

// PGN of a 29-bit J1939 identifier (PDU1: the PS byte is an address).
inline uint32_t Pgn(uint32_t id) {
    const uint32_t dp = (id >> 24) & 0x01, pf = (id >> 16) & 0xFF, ps = (id >> 8) & 0xFF;
    return pf < 240 ? (dp << 16) | (pf << 8) : (dp << 16) | (pf << 8) | ps;
}
inline uint8_t Source(uint32_t id) { return static_cast<uint8_t>(id & 0xFF); }

inline bool U8(const uint8_t* d, uint8_t len, int i, uint32_t* out) {
    if (i >= len || d[i] > 250) return false;
    *out = d[i];
    return true;
}
inline bool U16(const uint8_t* d, uint8_t len, int i, uint32_t* out) {
    if (i + 1 >= len) return false;
    const uint32_t v = d[i] | (d[i + 1] << 8);
    if (v > 0xFAFF) return false;
    *out = v;
    return true;
}
inline bool U32(const uint8_t* d, uint8_t len, int i, uint32_t* out) {
    if (i + 3 >= len) return false;
    const uint32_t v = static_cast<uint32_t>(d[i]) | (static_cast<uint32_t>(d[i + 1]) << 8) |
                       (static_cast<uint32_t>(d[i + 2]) << 16) | (static_cast<uint32_t>(d[i + 3]) << 24);
    if (v > 0xFAFFFFFFu) return false;
    *out = v;
    return true;
}

class Decoder {
public:
    explicit Decoder(uint8_t engine_sa = 0) : sa_(engine_sa), dm1_(engine_sa) {}

    uint8_t sa() const { return sa_; }

    // One frame. Returns the engine values it carries (empty for any other
    // frame). When a complete DM1 has arrived, *got_dm1 is set and *dm1
    // holds it; the caller writes Dm1Values() and the fault text.
    std::vector<Value> Feed(uint32_t id, bool extended, const uint8_t* d, uint8_t len,
                            usv722::Dm1* dm1, bool* got_dm1) {
        std::vector<Value> out;
        *got_dm1 = false;
        if (!extended || Source(id) != sa_) return out;
        if (dm1_.Feed(id, d, len, dm1)) {
            *got_dm1 = true;
            return out;
        }
        uint32_t v = 0;
        switch (Pgn(id)) {
            case 61444:   // EEC1
                if (U8(d, len, 2, &v)) out.push_back({"engine", "percent_torque", v - 125.0});
                if (U16(d, len, 3, &v)) out.push_back({"engine", "rpm", v * 0.125});
                if (U8(d, len, 7, &v)) out.push_back({"engine", "demand_torque", v - 125.0});
                break;
            case 61443:   // EEC2
                if (U8(d, len, 1, &v)) out.push_back({"engine", "pedal_position", v * 0.4});
                if (U8(d, len, 2, &v)) out.push_back({"engine", "percent_load", static_cast<double>(v)});
                break;
            case 65262:   // ET1
                if (U8(d, len, 0, &v)) out.push_back({"engine", "coolant_temp", v - 40.0});
                if (U8(d, len, 1, &v)) out.push_back({"engine", "fuel_temp", v - 40.0});
                if (U16(d, len, 2, &v)) out.push_back({"engine", "oil_temp", v * 0.03125 - 273.0});
                break;
            case 65263:   // EFL/P1
                if (U8(d, len, 2, &v)) out.push_back({"engine", "oil_level", v * 0.4});
                if (U8(d, len, 3, &v)) out.push_back({"engine", "oil_pressure", v * 4.0});
                break;
            case 65266:   // LFE
                if (U16(d, len, 0, &v)) out.push_back({"engine", "fuel_rate", v * 0.05});
                break;
            case 65270:   // IC1
                if (U8(d, len, 1, &v)) out.push_back({"engine", "intake_manifold_pressure", v * 2.0});
                if (U8(d, len, 2, &v)) out.push_back({"engine", "intake_manifold_temp", v - 40.0});
                if (U16(d, len, 5, &v)) out.push_back({"engine", "exhaust_temp", v * 0.03125 - 273.0});
                break;
            case 65269:   // AMB
                if (U8(d, len, 0, &v)) out.push_back({"engine", "baro_pressure", v * 0.5});
                if (U8(d, len, 5, &v)) out.push_back({"engine", "air_inlet_temp", v - 40.0});
                break;
            case 65271:   // VEP1
                if (U16(d, len, 4, &v)) out.push_back({"engine", "ecu_voltage", v * 0.05});
                break;
            case 65253:   // HOURS
                if (U32(d, len, 0, &v)) out.push_back({"engine", "runtime_h", v * 0.05});
                break;
            case 65279:   // WFI
                if (len >= 1 && (d[0] & 0x03) <= 1)
                    out.push_back({"engine", "water_in_fuel", static_cast<double>(d[0] & 0x03)});
                break;
            default:
                break;
        }
        return out;
    }

    // PGN 59904 request for PGN 65253 (engine hours), addressed to the
    // engine, from `requester` (an address no device on the bus uses; 249
    // is reserved by J1939 for off-board diagnostic tools). Data E5 FE 00.
    uint32_t HoursRequestId(uint8_t requester) const {
        return 0x18EA0000u | (static_cast<uint32_t>(sa_) << 8) | requester;
    }
    static constexpr uint8_t kHoursRequest[3] = {0xE5, 0xFE, 0x00};

private:
    uint8_t sa_;
    usv722::Dm1Receiver dm1_;
};

}  // namespace j1939engine
}  // namespace usv
