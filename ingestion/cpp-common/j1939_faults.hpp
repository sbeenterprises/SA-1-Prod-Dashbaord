// SAE J1939 fault naming shared by the bridges: the J1939-73 failure mode
// text, and the Mercury Diesel 2.0L (Cummins-based ECM) fault names from
// Table 2 "J1939 SPN-FMI Faults Supported" of the Mercury J1939 integration
// specification (65 rows, 61 distinct SPN / FMI pairs; the two pairs the
// table lists under several J codes carry every code).
//
// FaultText(spn, fmi, oc, mercury) gives the operator wording the
// dashboards show in engine.dtc_text:
//   mercury: "SPN 100 FMI 18 J143 Oil_Press_Low (x1)"
//   other:   "SPN 100 FMI 18 below normal, moderately severe (x1)"
// EnrichDtcText() adds the Mercury name to a text that carries only
// "SPN n FMI m" pairs (what iCANRec publishes as ENGINE_DTC_TEXT).
#pragma once

#include <cstdint>
#include <cstdio>
#include <regex>
#include <string>

namespace usv {
namespace j1939 {

// J1939-73 failure mode identifiers, short operator wording.
inline const char* FmiText(uint8_t fmi) {
    static const char* const kText[] = {
        "above normal, most severe", "below normal, most severe",
        "erratic or incorrect data", "voltage high / short to high",
        "voltage low / short to low", "current low / open circuit",
        "current high / short to ground", "mechanical system not responding",
        "abnormal frequency or pulse", "abnormal update rate",
        "abnormal rate of change", "root cause not known",
        "bad intelligent device", "out of calibration",
        "special instructions", "above normal, least severe",
        "above normal, moderately severe", "below normal, least severe",
        "below normal, moderately severe", "received network data in error",
        "data drifted high", "data drifted low"};
    if (fmi < sizeof(kText) / sizeof(kText[0])) return kText[fmi];
    return fmi == 31 ? "condition exists" : "reserved";
}

struct MercuryFault {
    uint32_t spn;
    uint8_t fmi;
    const char* code;   // Cummins / Mercury J code(s)
    const char* name;   // fault description as the specification spells it
};

inline const MercuryFault* MercuryFaults(size_t* count) {
    static const MercuryFault kTable[] = {
    {    91,  3, "J131", "Sensor_Throt_High"},
    {    91,  4, "J132", "Sensor_Throt_Low"},
    {    97,  3, "J428", "Sensor_WIF_High"},
    {    97,  4, "J429", "Sensor_WIF_Low"},
    {    97, 15, "J418", "Water_In_Fuel"},
    {   100,  1, "J415", "Oil_Press_Very_Low"},
    {   100,  3, "J135", "Sensor_Oil_Press_High"},
    {   100,  4, "J141", "Sensor_Oil_Press_Low"},
    {   100, 18, "J143", "Oil_Press_Low"},
    {   102,  2, "J2973", "Boost_IR_Error"},
    {   102,  3, "J122", "Sensor_Boost_High"},
    {   102,  4, "J123", "Sensor_Boost_Low"},
    {   102, 16, "J124", "Boost_Press_High"},
    {   105,  0, "J155", "IMT_High"},
    {   105,  3, "J153", "Sensor_IMT_High"},
    {   105,  4, "J154", "Sensor_IMT_Low"},
    {   108,  3, "J221", "Snsr_Amb_Air_Press_High"},
    {   108,  4, "J222", "Snsr_Amb_Air_Press_Low"},
    {   110,  0, "J151", "Cool_T_High"},
    {   110,  3, "J144", "Sensor_Cool_T_High"},
    {   110,  4, "J145", "Sensor_Cool_T_Low"},
    {   111,  1, "J235", "Cool_Lvl_Very_Low"},
    {   111,  2, "J422", "Sensor_Clnt_Level_Error"},
    {   111,  3, "J195", "Sensor_Cool_Lvl_High"},
    {   111,  4, "J196", "Sensor_Cool_Lvl_Low"},
    {   111, 18, "J197", "Cool_Lvl_Low"},
    {   157,  0, "J449", "Fuel_Press_Very_High"},
    {   157,  1, "J2249", "Low_Fuel_Press2"},
    {   157,  2, "J554", "Fuel_Pressure_InRange_Error"},
    {   157,  3, "J451", "Sensor_Fuel_P_High"},
    {   157,  4, "J452", "Sensor_Fuel_P_Low"},
    {   157, 16, "J553", "High_Fuel_Pressure"},
    {   157, 18, "J559", "Low_Fuel_Pressure"},
    {   175,  0, "J214", "Oil_Temperature_High"},
    {   175,  3, "J212", "Sensor_Oil_T_High"},
    {   175,  4, "J213", "Sensor_Oil_T_Low"},
    {   190,  0, "J234", "Engine_Overspeed"},
    {   190,  2, "J115/J689/J2321", "Main_Crank_EPS_Error"},
    {   190, 10, "J121", "Bkup_Cam_EPS_Error"},
    {   612,  2, "J115", "Main_Crank_EPS_Error"},
    {   620,  3, "J2185", "Throt_Sensor_Supply_High"},
    {   620,  4, "J2186", "Throt_Sensor_Supply_Low"},
    {   633, 31, "J2311", "IMV_Resistance_Error"},
    {   644,  2, "J237", "Engine_Sync_Error"},
    {   651,  5, "J322", "Injector_1_Error"},
    {   652,  5, "J331", "Injector_2_Error"},
    {   653,  5, "J324", "Injector_3_Error"},
    {   654,  5, "J332", "Injector_4_Error"},
    {   655,  5, "J323", "Injector_5_Error"},
    {   656,  5, "J325", "Injector_6_Error"},
    {   723,  2, "J778/J121/J2322", "Bkup_Cam_EPS_Error"},
    {   974,  3, "J133", "Sensor_Rem_Throt_High"},
    {   974,  4, "J134", "Sensor_Rem_Throt_Low"},
    {  1075,  3, "J2265", "Prime_Pump_High_Error"},
    {  1075,  4, "J2266", "Prime_Pump_Low_Error"},
    {  1347,  3, "J272", "IMV_Shorted_High"},
    {  1347,  4, "J271", "IMV_Shorted_Low"},
    {  1347,  7, "J281", "Cyl_Press_Imbalance_Error"},
    {  1377,  2, "J497", "Engine_Sync_Switch_Error"},
    {520197,  3, "J2185", "Throt_Sensor_Supply_High"},
    {520197,  4, "J2186", "Throt_Sensor_Supply_Low"},
    };
    *count = sizeof(kTable) / sizeof(kTable[0]);
    return kTable;
}

inline const MercuryFault* FindMercuryFault(uint32_t spn, uint8_t fmi) {
    size_t n = 0;
    const MercuryFault* t = MercuryFaults(&n);
    for (size_t i = 0; i < n; ++i)
        if (t[i].spn == spn && t[i].fmi == fmi) return &t[i];
    return nullptr;
}

// One fault for engine.dtc_text.
inline std::string FaultText(uint32_t spn, uint8_t fmi, uint8_t oc, bool mercury) {
    char buf[160];
    const MercuryFault* m = mercury ? FindMercuryFault(spn, fmi) : nullptr;
    if (m)
        std::snprintf(buf, sizeof(buf), "SPN %u FMI %u %s %s (x%u)",
                      static_cast<unsigned>(spn), static_cast<unsigned>(fmi),
                      m->code, m->name, static_cast<unsigned>(oc));
    else
        std::snprintf(buf, sizeof(buf), "SPN %u FMI %u %s (x%u)",
                      static_cast<unsigned>(spn), static_cast<unsigned>(fmi),
                      FmiText(fmi), static_cast<unsigned>(oc));
    return buf;
}

// "SPN 100 FMI 18 (x1); SPN 9 FMI 2 (x3)" ->
// "SPN 100 FMI 18 J143 Oil_Press_Low (x1); SPN 9 FMI 2 erratic or incorrect data (x3)".
// A pair that already carries a name (anything but "(" after the FMI) is
// left as it is, so the text can be passed through twice.
inline std::string EnrichDtcText(const std::string& text, bool mercury) {
    static const std::regex kPair("SPN (\\d+) FMI (\\d+)(?= \\(|$|;)");
    std::string out;
    auto begin = std::sregex_iterator(text.begin(), text.end(), kPair);
    size_t last = 0;
    for (auto it = begin; it != std::sregex_iterator(); ++it) {
        const std::smatch& m = *it;
        const unsigned long spn = std::stoul(m[1].str());
        const unsigned long fmi = std::stoul(m[2].str());
        out += text.substr(last, m.position(0) + m.length(0) - last);
        const MercuryFault* f = (mercury && fmi < 32) ? FindMercuryFault(spn, static_cast<uint8_t>(fmi)) : nullptr;
        if (f) out += std::string(" ") + f->code + " " + f->name;
        else if (fmi < 32) out += std::string(" ") + FmiText(static_cast<uint8_t>(fmi));
        last = m.position(0) + m.length(0);
    }
    out += text.substr(last);
    return out;
}

}  // namespace j1939
}  // namespace usv
