// Per-vessel particulars shared by the C++ bridges: the hull's class name,
// main dimensions, fuel capacity, range, maximum speed and fitted
// equipment, from the general arrangement drawing. Static facts, written
// to InfluxDB by signalk_bridge every 60 s (the `vessel` measurement) so the
// dashboards can show the class name in the header band, the particulars
// card on the Readiness page, fuel in litres and range at current burn.
//
// /etc/usv/usv.env keys (all optional; an unset key leaves its row out):
//   VESSEL_CLASS         "S3 Swift Sea-Stalker"     class name (header band)
//   VESSEL_LOA_M         5.9                        length overall, metres
//   VESSEL_BEAM_M        1.8
//   VESSEL_HEIGHT_M      2.5
//   VESSEL_DRAFT_M       1.04
//   VESSEL_PAYLOAD_KG    454
//   VESSEL_FUEL_L        1135                       usable fuel, litres
//   VESSEL_RANGE_NM      1000                       design range
//   VESSEL_MAX_SPEED_KN  30                         hull maximum (speed cap check)
//   VESSEL_PROPULSION    "Mercury Diesel 2.0L, Bravo 3 sterndrive"
//   VESSEL_NAVIGATION    "Simrad HALO 24 radar, VN-300 INS"
//   VESSEL_CAMERAS       "FLIR M364 thermal, bullet"
//   VESSEL_COMMS         "Starlink BLOS"
//   VESSEL_RIDE_CONTROL  "Seakeeper Ride 450"
//
// Read like the engine profile: the process environment first (systemd
// EnvironmentFile), then usv.env itself (grafana_bridge under pAntler).
// Unit-tested by vessel_profile_test.cpp.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "engine_profile.hpp"

namespace usv {

struct VesselProfile {
    std::string cls;          // VESSEL_CLASS
    double loa_m = 0, beam_m = 0, height_m = 0, draft_m = 0, payload_kg = 0;
    double fuel_l = 0, range_nm = 0, max_speed_kn = 0;
    std::string propulsion, navigation, cameras, comms, ride_control;

    static VesselProfile FromEnv(const std::string& file = "") {
        const char* f = std::getenv("USV_ENV_FILE");
        const std::string path = !file.empty() ? file : (f ? f : "/etc/usv/usv.env");
        std::map<std::string, std::string> fromfile;
        bool loaded = false;
        auto get = [&](const char* key) -> std::string {
            if (const char* v = std::getenv(key)) return Trim(v);
            if (!loaded) { fromfile = ReadEnvFile(path); loaded = true; }
            auto it = fromfile.find(key);
            return it == fromfile.end() ? "" : Trim(it->second);
        };
        auto num = [&](const char* key) -> double {
            const std::string s = get(key);
            if (s.empty()) return 0.0;
            char* end = nullptr;
            const double v = std::strtod(s.c_str(), &end);
            return (end && end != s.c_str() && v > 0.0) ? v : 0.0;
        };
        VesselProfile p;
        p.cls = get("VESSEL_CLASS");
        p.loa_m = num("VESSEL_LOA_M");
        p.beam_m = num("VESSEL_BEAM_M");
        p.height_m = num("VESSEL_HEIGHT_M");
        p.draft_m = num("VESSEL_DRAFT_M");
        p.payload_kg = num("VESSEL_PAYLOAD_KG");
        p.fuel_l = num("VESSEL_FUEL_L");
        p.range_nm = num("VESSEL_RANGE_NM");
        p.max_speed_kn = num("VESSEL_MAX_SPEED_KN");
        p.propulsion = get("VESSEL_PROPULSION");
        p.navigation = get("VESSEL_NAVIGATION");
        p.cameras = get("VESSEL_CAMERAS");
        p.comms = get("VESSEL_COMMS");
        p.ride_control = get("VESSEL_RIDE_CONTROL");
        return p;
    }

    bool empty() const {
        return cls.empty() && loa_m == 0 && fuel_l == 0 && max_speed_kn == 0 &&
               propulsion.empty() && navigation.empty();
    }

    // The particulars card, one row per line as "label" / "value", in
    // display order; rows without data are left out. The bridge writes each
    // as a string field named "<nn> <label>" so one query lists the card.
    std::vector<std::pair<std::string, std::string>> CardRows() const {
        std::vector<std::pair<std::string, std::string>> rows;
        auto m = [](double v) { return Fmt(v, 1) + " m"; };
        if (loa_m > 0 || beam_m > 0 || height_m > 0) {
            std::string dims;
            if (loa_m > 0) dims += m(loa_m);
            if (beam_m > 0) dims += (dims.empty() ? "" : " x ") + m(beam_m);
            if (height_m > 0) dims += (dims.empty() ? "" : " x ") + m(height_m);
            rows.push_back({"Length x beam x height", dims});
        }
        if (draft_m > 0) rows.push_back({"Draft", m(draft_m)});
        if (payload_kg > 0)
            rows.push_back({"Payload", Fmt(payload_kg, 0) + " kg (" + Fmt(payload_kg * 2.20462, 0) + " lb)"});
        if (fuel_l > 0)
            rows.push_back({"Fuel capacity", Fmt(fuel_l, 0) + " L (" + Fmt(fuel_l / 3.78541, 0) + " gal)"});
        if (range_nm > 0 || max_speed_kn > 0) {
            std::string v;
            if (range_nm > 0) v += Fmt(range_nm, 0) + " nm";
            if (max_speed_kn > 0) v += (v.empty() ? "" : " / ") + Fmt(max_speed_kn, 0) + " kn";
            rows.push_back({range_nm > 0 && max_speed_kn > 0 ? "Range / max speed"
                            : (range_nm > 0 ? "Range" : "Max speed"), v});
        }
        if (!propulsion.empty()) rows.push_back({"Propulsion", propulsion});
        if (!navigation.empty()) rows.push_back({"Navigation", navigation});
        if (!cameras.empty() || !comms.empty()) {
            std::string v = cameras;
            if (!comms.empty()) v += (v.empty() ? "" : " | ") + comms;
            rows.push_back({!cameras.empty() && !comms.empty() ? "Cameras / comms"
                            : (!cameras.empty() ? "Cameras" : "Comms"), v});
        }
        if (!ride_control.empty()) rows.push_back({"Ride control", ride_control});
        return rows;
    }

    std::string Describe() const {
        if (empty()) return "vessel=(no particulars)";
        return "vessel_class=\"" + cls + "\" fuel_l=" + Fmt(fuel_l, 0) +
               " max_speed_kn=" + Fmt(max_speed_kn, 0) + " range_nm=" + Fmt(range_nm, 0);
    }

    // Numbers with thousands separators and a fixed number of decimals
    // ("1,135", "5.9"), as the card and the readiness evidence show them.
    static std::string Fmt(double v, int decimals) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
        std::string s(buf);
        const size_t dot = s.find('.');
        std::string ip = dot == std::string::npos ? s : s.substr(0, dot);
        const std::string fp = dot == std::string::npos ? "" : s.substr(dot);
        for (int i = static_cast<int>(ip.size()) - 3; i > 0; i -= 3) ip.insert(i, ",");
        return ip + fp;
    }

private:
    static std::string Trim(const std::string& s) {
        const size_t a = s.find_first_not_of(" \t");
        if (a == std::string::npos) return "";
        const size_t b = s.find_last_not_of(" \t");
        return s.substr(a, b - a + 1);
    }
};

}  // namespace usv
