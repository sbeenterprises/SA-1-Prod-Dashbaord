// mayara_logic.hpp — pure conversion logic for mayara_bridge.
//
// mayara-server (MarineYachtRadar, 3.x) tracks ARPA/MARPA targets and serves
// them through the Signal K Radar API:
//
//   GET /signalk/v2/api/vessels/self/radars                     radar list
//   GET /signalk/v2/api/vessels/self/radars/{id}/controls/power power state
//   GET /signalk/v2/api/vessels/self/radars/{id}/targets        tracked targets
//
// A target (ArpaTargetApi) looks like:
//   { "id": 3, "status": "tracking",
//     "position": { "bearing": 2.335, "distance": 1067,
//                   "latitude": 29.71, "longitude": -91.14 },
//     "motion":   { "course": 4.06, "speed": 2.93 },
//     "danger":   { "cpa": 350.1, "tcpa": 138, "isDangerous": false },
//     "acquisition": "auto", "sourceZone": 1, ... }
// Units: bearing / course in radians TRUE, distance / cpa in metres, speed
// in m/s, tcpa in seconds (negative = CPA already passed).
//
// The bridge turns each target into the RADAR_TARGET string grafana_bridge
// already parses (moos_bridge.cpp HandleRadarTarget):
//   "id=R3,range=1067.0,bearing=133.8,spd=5.70,cog=232.6,cpa=350.1,tcpa=138"
// with bearing / cog in degrees true and spd in KNOTS ($RATTM convention).
//
// No MOOS dependency: unit-tested by mayara_logic_test.cpp.
#pragma once

#include <nlohmann/json.hpp>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace usv {
namespace mayara {

using json = nlohmann::json;

constexpr double kRadToDeg = 57.29577951308232;
constexpr double kMsToKn = 1.9438444924406046;

struct Options {
    std::string id_prefix = "R";    // contact id = prefix + mayara target id
    bool include_acquiring = false;  // also post targets still "acquiring"
};

// Power control values (mayara / Signal K Radar API).
enum Power { kPowerUnknown = -1, kPowerOff = 0, kPowerStandby = 1, kPowerTransmit = 2 };

namespace detail {

inline bool Num(const json& j, const char* key, double* out) {
    if (!j.is_object()) return false;
    auto it = j.find(key);
    if (it == j.end()) return false;
    if (it->is_number()) {
        *out = it->get<double>();
        return std::isfinite(*out);
    }
    if (it->is_string()) {
        try {
            size_t used = 0;
            const std::string s = it->get<std::string>();
            const double d = std::stod(s, &used);
            if (used != s.size() || !std::isfinite(d)) return false;
            *out = d;
            return true;
        } catch (...) {
            return false;
        }
    }
    return false;
}

inline std::string IdString(const json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number_unsigned()) return std::to_string(v.get<unsigned long long>());
    if (v.is_number_integer()) return std::to_string(v.get<long long>());
    if (v.is_number_float()) {
        const double d = v.get<double>();
        if (std::isfinite(d) && d == std::floor(d))
            return std::to_string(static_cast<long long>(d));
    }
    return "";
}

inline double WrapDeg360(double d) {
    d = std::fmod(d, 360.0);
    if (d < 0) d += 360.0;
    if (d >= 360.0) d = 0.0;   // fmod rounding at the wrap
    return d;
}

inline std::string Lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace detail

// Radar ids from the radar list. Accepts the 3.x shape
// {"version":..,"radars":{"nav6959":{..}}}, a bare id-keyed object, or an
// array of {"id":..} objects / id strings (older builds).
inline std::vector<std::string> RadarIds(const json& j) {
    std::vector<std::string> ids;
    if (j.is_object() && j.contains("radars")) return RadarIds(j["radars"]);
    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it)
            if (it.value().is_object()) ids.push_back(it.key());
    } else if (j.is_array()) {
        for (const auto& e : j) {
            if (e.is_string()) {
                ids.push_back(e.get<std::string>());
            } else if (e.is_object() && e.contains("id")) {
                const std::string id = detail::IdString(e["id"]);
                if (!id.empty()) ids.push_back(id);
            }
        }
    }
    return ids;
}

// Power state from GET .../controls/power ({"id":"power","value":2,...}) or
// a bare value. Strings "transmit"/"standby"/"off" are accepted too.
inline int PowerState(const json& j) {
    const json* v = &j;
    if (j.is_object()) {
        auto it = j.find("value");
        if (it == j.end()) return kPowerUnknown;
        v = &*it;
    }
    if (v->is_number()) {
        const double d = v->get<double>();
        if (d == 0 || d == 1 || d == 2) return static_cast<int>(d);
        return kPowerUnknown;
    }
    if (v->is_string()) {
        const std::string s = detail::Lower(v->get<std::string>());
        if (s == "transmit" || s == "2") return kPowerTransmit;
        if (s == "standby" || s == "1") return kPowerStandby;
        if (s == "off" || s == "0") return kPowerOff;
    }
    return kPowerUnknown;
}

// One mayara target -> RADAR_TARGET string. Returns false (and leaves *spec
// untouched) for targets that must not be posted: lost, acquiring (unless
// opts.include_acquiring), or missing id / bearing / distance.
inline bool TargetToSpec(const json& t, const Options& opts, std::string* spec,
                         std::string* id_out = nullptr) {
    if (!t.is_object() || !t.contains("id")) return false;
    const std::string tid = detail::IdString(t["id"]);
    if (tid.empty()) return false;

    if (auto st = t.find("status"); st != t.end() && st->is_string()) {
        const std::string s = detail::Lower(st->get<std::string>());
        if (s == "lost") return false;
        if (s == "acquiring" && !opts.include_acquiring) return false;
    }

    auto pos = t.find("position");
    if (pos == t.end() || !pos->is_object()) return false;
    double bearing_rad = 0, distance_m = 0;
    if (!detail::Num(*pos, "bearing", &bearing_rad) ||
        !detail::Num(*pos, "distance", &distance_m) || distance_m < 0)
        return false;

    const std::string id = opts.id_prefix + tid;
    char buf[64];
    std::string s = "id=" + id;
    std::snprintf(buf, sizeof(buf), ",range=%.1f", distance_m);
    s += buf;
    std::snprintf(buf, sizeof(buf), ",bearing=%.1f",
                  detail::WrapDeg360(bearing_rad * kRadToDeg));
    s += buf;

    // Motion is omitted until mayara's filter has an estimate.
    if (auto mo = t.find("motion"); mo != t.end() && mo->is_object()) {
        double course = 0, speed = 0;
        if (detail::Num(*mo, "course", &course) &&
            detail::Num(*mo, "speed", &speed) && speed >= 0) {
            std::snprintf(buf, sizeof(buf), ",spd=%.2f,cog=%.1f", speed * kMsToKn,
                          detail::WrapDeg360(course * kRadToDeg));
            s += buf;
        }
    }

    // CPA/TCPA only for an approach still ahead (tcpa >= 0). A passed CPA
    // (negative TCPA) is history; grafana_bridge would otherwise raise a CPA
    // alert for a vessel that has already gone by.
    if (auto dg = t.find("danger"); dg != t.end() && dg->is_object()) {
        double cpa = 0, tcpa = 0;
        if (detail::Num(*dg, "cpa", &cpa) && detail::Num(*dg, "tcpa", &tcpa) &&
            cpa >= 0 && tcpa >= 0) {
            std::snprintf(buf, sizeof(buf), ",cpa=%.1f,tcpa=%.0f", cpa, tcpa);
            s += buf;
        }
    }

    *spec = s;
    if (id_out) *id_out = id;
    return true;
}

// The targets response -> RADAR_TARGET strings. Accepts a JSON array (3.x),
// {"targets":[...]}, or an id-keyed object.
inline std::vector<std::string> TargetsToSpecs(const json& j, const Options& opts) {
    std::vector<std::string> out;
    auto one = [&](const json& t) {
        std::string spec;
        if (TargetToSpec(t, opts, &spec)) out.push_back(spec);
    };
    if (j.is_array()) {
        for (const auto& t : j) one(t);
    } else if (j.is_object() && j.contains("targets")) {
        return TargetsToSpecs(j["targets"], opts);
    } else if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (!it.value().is_object()) continue;
            if (it.value().contains("id")) {
                one(it.value());
            } else {
                json t = it.value();
                t["id"] = it.key();
                one(t);
            }
        }
    }
    return out;
}

}  // namespace mayara
}  // namespace usv
