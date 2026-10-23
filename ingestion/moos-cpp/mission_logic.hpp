// Mission-state derivation and MOOS-string parsers for the Alpha mission
// (alpha.bhv). Pure C++17, no MOOS dependency, so the state machine that
// operators rely on is unit-testable off-vessel (mission_logic_test.cpp).
//
// Semantics mirror the behavior file:
//  - COMMS_LOST carries the failsafe action ("loiter", ...) while the
//    operator link is down; "false" means link ok.
//  - MISSION_ARMED is the explicit re-arm latch: cleared by iWebSocket on a
//    comms loss, set true ONLY by an operator AUTONOMOUS_START/RESUME. A
//    link merely returning must NOT read as a resumed mission.
//  - SURVEY_ENTRY_GATE is the start-only latch: "closed" when a mission
//    starts outside the region (opr24 recovers the boat inside first).
//  - MOOS_MANUAL_OVERRIDE=true parks the helm; planchaPID actively zeroes
//    thrust while parked.
//  - The production iWebSocket (moos-ivp-morgan-stern-drive-mercury, comms
//    loss return to base) publishes COMMS_LOSS=true/false instead of the
//    COMMS_LOST phases: after comms_loss_timeout s without a frame from any
//    Swiftships-UI it engages RETURN (comms_return behaviour) when the helm
//    is in DRIVE or the boat is underway, and comms_hold keeps station on
//    the base on arrival (RETURN_HOLD=true). With the helm parked and the
//    boat idle it stays where it is. Both variants are handled; a vessel
//    publishes one or the other.
#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace usv {

// ------------------------------------------------------------ small helpers

inline std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

inline std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

// MOOS mode flags arrive as strings ("true"/"false"); be liberal in what we
// accept ("TRUE", "1", "yes", "on").
inline bool truthy(const std::string& v) {
    const std::string t = lower(trim(v));
    return t == "true" || t == "1" || t == "yes" || t == "on";
}

// COMMS_LOST phases, as posted by iWebSocket (WebSocket.cpp, comms watchdog):
//   false          link up                                   -> 0
//   grace          link down, mission KEEPS RUNNING for
//                  COMMS_GRACE s; a reconnect just continues -> 1
//   <action>       grace expired: station-keep hold at the
//                  loss position, MISSION_ARMED cleared       -> 2
//   rtb            held COMMS_HOLD s with a base defined:
//                  return to base (RETURN=true is posted)     -> 3
inline int comms_phase(const std::string& v) {
    const std::string t = lower(trim(v));
    if (t.empty() || t == "false" || t == "0" || t == "ok") return 0;
    if (t == "grace") return 1;
    if (t == "rtb") return 3;
    return 2;
}

// "Lost" for mission-state purposes means the failsafe HOLD is in force.
// During "grace" the survey is still driving, and during "rtb" the return
// behavior is driving; both derive their state from the mode flags.
inline bool comms_lost_active(const std::string& v) {
    return comms_phase(v) == 2;
}

// Split "a, b,c" -> {"a","b","c"} (trimmed, empties dropped).
inline std::vector<std::string> SplitCsv(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t comma = s.find(',', start);
        std::string item = trim(s.substr(
            start, comma == std::string::npos ? std::string::npos
                                              : comma - start));
        if (!item.empty()) out.push_back(item);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

// Signed heading error in [-180, 180): positive = desired is to starboard.
inline double HeadingError(double desired_deg, double actual_deg) {
    return std::fmod(desired_deg - actual_deg + 540.0, 360.0) - 180.0;
}

// SURVEY_ENTRY_GATE: defaults open; only the literal "closed" suppresses
// the survey (matches iWebSocket's publication).
inline bool gate_open(const std::string& v) {
    return lower(trim(v)) != "closed";
}

// IVPHELM_ALLSTOP reasons that are normal operation, not a fault:
//   clear          - helm producing decisions
//   ManualOverride - helm parked on purpose (MOOS_MANUAL_OVERRIDE)
//   NothingToDo    - deployed with no active behavior (idle)
// Anything else (MissingDecVars:..., NoIvPFunctions, MALCONFIG fallout...)
// is an unexpected all-stop and alert-worthy.
inline bool allstop_unexpected(const std::string& reason) {
    const std::string r = lower(trim(reason));
    if (r.empty() || r == "clear") return false;
    if (r.rfind("manualoverride", 0) == 0) return false;
    if (r.rfind("nothingtodo", 0) == 0) return false;
    return true;
}

// ------------------------------------------------------- mission state

struct MissionFlags {
    // Defaults track the `initialize` lines in alpha.bhv (cold-start safe).
    bool deploy = false;
    bool waypoint = false;
    bool returning = false;
    bool stationing = false;
    bool pause = false;
    bool manual_override = false;
    bool remote_control = false;       // REMOTE_CONTROL (manual passthrough)
    bool crash_stop = false;           // CRASH_STOP (emergency braking)
    bool armed = true;                 // MISSION_ARMED
    bool survey_complete = false;      // latched by waypt_survey endflag
    std::string entry_gate = "open";   // SURVEY_ENTRY_GATE
    std::string comms_lost = "false";  // COMMS_LOST (simulation iWebSocket)
    bool comms_loss = false;           // COMMS_LOSS (production iWebSocket)
    bool return_hold = false;          // RETURN_HOLD: holding station on the base
    bool trailing = false;             // TRAILING: following an AIS contact (BHV_Trail)
    bool region = false;               // REGION: operating region enforced (BHV_OpRegionV24); a fence, not a mode
    bool commanded = false;            // throttle or rudder command in force (MotionLatch, set by the bridge)
    bool moving = false;               // speed over ground at or above the drift threshold (MotionLatch)
    bool all_stop = false;             // ALL_STOP: operator ALL STOP latched (iWebSocket) until ALL_STOP_RESET
    std::string helm_state;            // IVPHELM_STATE (informational)
    std::string allstop = "clear";     // IVPHELM_ALLSTOP (informational)
};

// An autonomy mode that drives the boat (or deliberately holds it) is
// requested: survey, return, station keeping, contact following, the
// comms-loss hold on the base, or a pause. REGION is not one: the
// operating-region fence stays enforced through a survey and after it, and
// on its own it only recovers the boat after a breach.
inline bool autonomy_mode(const MissionFlags& f) {
    return f.waypoint || f.returning || f.stationing || f.trailing ||
           f.return_hold || f.pause;
}

// Autonomy is driving the boat: an autonomy mode is requested and the helm
// is not parked. Outside it, throttle and rudder commands come from the
// operator's console.
inline bool autonomy_driving(const MissionFlags& f) {
    return autonomy_mode(f) && !f.manual_override;
}

// Operational mission state, priority-ordered (first match wins). Remote
// (operator driving through REMOTE_CONTROL passthrough) outranks the helm
// flags it sets underneath (manual override / park). Outside autonomy
// (no autonomy mode, or the helm parked):
//   - a throttle or rudder command in force means the operator is driving
//     from Swiftships-UI (DESIRED_THRUST / DESIRED_RUDDER straight from the
//     console; the boats' iWebSocket never posts REMOTE_CONTROL): Remote;
//   - moving with no command (drifting on current or wind): Moving;
//   - at rest: Parked / Completed / Idle as before.
// Helm health is intentionally NOT folded in: a frozen helm while Paused
// must show as Paused + an all-stop alert, not mask one with the other.
// The operator's ALL STOP (ALL_STOP=true from iWebSocket) outranks
// everything: autonomy is ended, rudder and thrust are held at 0 and every
// start is refused until it is reset.
inline const char* DeriveMissionState(const MissionFlags& f) {
    if (f.all_stop)                              return "AllStop";
    if (comms_lost_active(f.comms_lost))         return "CommsLost";
    if (f.remote_control)                        return "Remote";
    if (!autonomy_driving(f) && f.commanded)     return "Remote";
    if (!autonomy_driving(f) && f.moving)        return "Moving";
    if (f.manual_override && f.survey_complete)  return "Completed";
    if (f.manual_override)                       return "Parked";
    if (f.pause)                                 return "Paused";
    if (f.return_hold)                           return "HoldingAtBase";
    if (f.stationing)                            return "StationKeeping";
    if (f.returning)                             return "ReturnToBase";
    if (f.trailing)                              return "Following";
    if (f.waypoint && !gate_open(f.entry_gate))  return "EnteringRegion";
    if (f.waypoint && !f.armed)                  return "AwaitingRearm";
    if (f.waypoint)                              return "Autonomous";
    if (f.survey_complete)                       return "Completed";
    return "Idle";
}

// Operator commands and drift, for the Remote and Moving states. A
// throttle (|DESIRED_THRUST| at or above thrust_pct) or rudder
// (|DESIRED_RUDDER| at or above rudder) command in force counts as
// commanded; speed over ground at or above moving_kn counts as moving (0
// turns it off). Each is held hold_s after its last sign so the state does
// not flicker at a stop. When autonomy stops driving, the autopilot's last
// outputs are dropped: only a new console command counts after that.
struct MotionLatch {
    double thrust_pct = 1.0;
    double rudder = 1.0;
    double moving_kn = 0.5;
    double hold_s = 10.0;
    double thrust = 0.0;               // last DESIRED_THRUST
    double rudder_cmd = 0.0;           // last DESIRED_RUDDER
    double sog_kn = 0.0;               // last speed over ground (kn)
    double last_cmd = -1e18;
    double last_move = -1e18;

    void set_thrust(double v, double now) { thrust = v; update(now); }
    void set_rudder(double v, double now) { rudder_cmd = v; update(now); }
    void set_speed(double kn, double now) { sog_kn = kn; update(now); }
    void autonomy_ended() { thrust = 0.0; rudder_cmd = 0.0; last_cmd = -1e18; }
    void update(double now) {
        const bool thr = thrust_pct > 0.0 && std::fabs(thrust) >= thrust_pct;
        const bool rud = rudder > 0.0 && std::fabs(rudder_cmd) >= rudder;
        if (thr || rud) last_cmd = now;
        if (moving_kn > 0.0 && sog_kn >= moving_kn) last_move = now;
    }
    bool commanded(double now) const { return now - last_cmd <= hold_s; }
    bool moving(double now) const { return now - last_move <= hold_s; }
};

// Operator link as one code for the dashboards (autonomy.operator_link):
//   0 link ok
//   1 grace, 2 failsafe hold, 3 returning to base   (COMMS_LOST phases)
//   3 returning to base after a COMMS_LOSS (production)
//   4 no Swiftships-UI link, boat stays (helm parked and idle: no action)
//   5 returned after a COMMS_LOSS, holding station on the base
inline int operator_link(const MissionFlags& f) {
    const int p = comms_phase(f.comms_lost);
    if (p > 0) return p;
    if (!f.comms_loss) return 0;
    if (f.return_hold) return 5;
    if (f.returning) return 3;
    return 4;
}

// The comms-loss failsafe is driving the boat (production iWebSocket): the
// link is lost and the boat is returning to, or holding on, its base.
inline bool comms_return(const MissionFlags& f) {
    return f.comms_loss && (f.returning || f.return_hold);
}

// "lat,lon" (BASE_COORDS from iWebSocket) -> true with both values when
// they parse and lie within the valid ranges.
inline bool ParseLatLon(const std::string& s, double* lat, double* lon) {
    const size_t comma = s.find(',');
    if (comma == std::string::npos) return false;
    char* end = nullptr;
    const std::string a = trim(s.substr(0, comma)), b = trim(s.substr(comma + 1));
    if (a.empty() || b.empty()) return false;
    const double la = std::strtod(a.c_str(), &end);
    if (*end) return false;
    const double lo = std::strtod(b.c_str(), &end);
    if (*end) return false;
    if (!(la >= -90 && la <= 90 && lo >= -180 && lo <= 180)) return false;
    *lat = la; *lon = lo;
    return true;
}

// --------------------------------------------- Min CPA across the contacts
// The traffic summary's Min CPA / TCPA. A contact still closing on its
// closest approach ranks ahead of one already opening (whose CPA is just its
// current range), but only within kMinCpaRankM: an AIS ship 40 nm out that
// is slowly closing is no more relevant than one opening at 1 nm, and it
// ranks with the others by CPA alone. Feed each contact with Add(); cpa -1
// (unknown) is skipped, tcpa -1 = opening.
constexpr double kMinCpaRankM = 12.0 * 1852.0;   // 12 nm
struct MinCpaPick {
    double cpa = -1.0, tcpa = -1.0;
    bool preferred = false;
    void Add(double c_cpa, double c_tcpa) {
        if (c_cpa < 0) return;
        const bool pref = c_tcpa >= 0 && c_cpa <= kMinCpaRankM;
        if (cpa < 0 || (pref && !preferred) || (pref == preferred && c_cpa < cpa)) {
            cpa = c_cpa; tcpa = c_tcpa; preferred = pref;
        }
    }
};

// ------------------------------------------------ COLAV_ALERTS (iWebSocket)

// One contact the helm is steering around, as iWebSocket reports it in
// COLAV_ALERTS (a JSON array): -1 is unknown. spd is m/s (NODE_REPORT units).
struct ColavAlert {
    std::string cn, mode, type;
    double mmsi = 0, for_s = 0, hdg = -1, spd = -1, range = -1, bng = -1,
           rel_bng = -1, cpa = -1, tcpa = -1, lat = 0, lon = 0, len = 0, age = -1;
    bool active = false;
};

// Minimal reader for the flat array of flat objects iWebSocket writes
// (jsoncpp, no nesting): strings with escapes, numbers, true / false / null.
// Returns false, with `out` emptied, on anything else.
inline bool ParseColavAlerts(const std::string& js, std::vector<ColavAlert>* out) {
    out->clear();
    size_t i = 0;
    auto ws = [&]() { while (i < js.size() && std::isspace(static_cast<unsigned char>(js[i]))) ++i; };
    auto str = [&](std::string* v) -> bool {
        if (i >= js.size() || js[i] != '"') return false;
        ++i; v->clear();
        while (i < js.size() && js[i] != '"') {
            if (js[i] == '\\' && i + 1 < js.size()) {
                const char e = js[++i];
                if (e == 'n') *v += '\n'; else if (e == 't') *v += '\t';
                else if (e == 'u' && i + 4 < js.size()) { *v += '?'; i += 4; }
                else *v += e;
            } else {
                *v += js[i];
            }
            ++i;
        }
        if (i >= js.size()) return false;
        ++i;
        return true;
    };
    ws();
    if (i >= js.size() || js[i] != '[') return false;
    ++i; ws();
    if (i < js.size() && js[i] == ']') return true;
    while (i < js.size()) {
        ws();
        if (js[i] != '{') { out->clear(); return false; }
        ++i;
        ColavAlert a;
        while (true) {
            ws();
            if (i < js.size() && js[i] == '}') { ++i; break; }
            std::string key;
            if (!str(&key)) { out->clear(); return false; }
            ws();
            if (i >= js.size() || js[i] != ':') { out->clear(); return false; }
            ++i; ws();
            std::string sval; double num = 0; bool is_num = false, bval = false, is_bool = false;
            if (i < js.size() && js[i] == '"') {
                if (!str(&sval)) { out->clear(); return false; }
            } else if (js.compare(i, 4, "true") == 0) { bval = true; is_bool = true; i += 4; }
            else if (js.compare(i, 5, "false") == 0) { is_bool = true; i += 5; }
            else if (js.compare(i, 4, "null") == 0) { i += 4; }
            else {
                char* end = nullptr;
                num = std::strtod(js.c_str() + i, &end);
                if (end == js.c_str() + i) { out->clear(); return false; }
                i = static_cast<size_t>(end - js.c_str());
                is_num = true;
            }
            if (key == "cn") a.cn = sval;
            else if (key == "mode") a.mode = sval;
            else if (key == "type") a.type = sval;
            else if (key == "active") a.active = is_bool && bval;
            else if (is_num) {
                if (key == "mmsi") a.mmsi = num; else if (key == "for") a.for_s = num;
                else if (key == "hdg") a.hdg = num; else if (key == "spd") a.spd = num;
                else if (key == "range") a.range = num; else if (key == "bng") a.bng = num;
                else if (key == "relBng") a.rel_bng = num; else if (key == "cpa") a.cpa = num;
                else if (key == "tcpa") a.tcpa = num; else if (key == "lat") a.lat = num;
                else if (key == "lon") a.lon = num; else if (key == "len") a.len = num;
                else if (key == "age") a.age = num;
            }
            ws();
            if (i < js.size() && js[i] == ',') { ++i; continue; }
            if (i < js.size() && js[i] == '}') { ++i; break; }
            out->clear(); return false;
        }
        if (!a.cn.empty()) out->push_back(a);
        ws();
        if (i < js.size() && js[i] == ',') { ++i; continue; }
        if (i < js.size() && js[i] == ']') return true;
        out->clear(); return false;
    }
    out->clear();
    return false;
}

// COLREGS situation of a BHV_AvdColregs mode ("giveway:stern"), worded as
// the Swiftships-UI words it. extremis: stand-on (crossing or being
// overtaken) in extremis, Rule 17(b), the one situation that is an alarm.
struct ColregsSituation { std::string rule, name; bool extremis = false; };
inline ColregsSituation ColregsOf(const std::string& mode) {
    const size_t c = mode.find(':');
    const std::string base = lower(trim(c == std::string::npos ? mode : mode.substr(0, c)));
    const std::string sub = c == std::string::npos ? "" : lower(trim(mode.substr(c + 1)));
    ColregsSituation s;
    if ((base == "standon" || base == "standon_ot") && sub == "inextremis") {
        s.rule = "Rule 17(b)";
        s.name = base == "standon" ? "Stand-on, in extremis" : "Being overtaken, in extremis";
        s.extremis = true;
        return s;
    }
    if (base == "headon")          { s.rule = "Rule 14"; s.name = "Head-on"; }
    else if (base == "overtaking") { s.rule = "Rule 13"; s.name = "Overtaking";
        if (sub == "port" || sub == "starboard") s.name += ", passing to " + sub; }
    else if (base == "giveway")    { s.rule = "Rule 16"; s.name = "Give-way";
        if (sub == "stern") s.name += ", crossing astern";
        else if (sub == "bow") s.name += ", crossing ahead"; }
    else if (base == "standon")    { s.rule = "Rule 17"; s.name = "Stand-on"; }
    else if (base == "standon_ot") { s.rule = "Rule 17"; s.name = "Stand-on, being overtaken"; }
    else if (base == "cpa")        { s.rule = "";        s.name = "Keep clear (CPA)"; }
    else                           { s.rule = "";        s.name = "Avoiding"; }
    return s;
}

// The no-silent-resume hold: survey requested but the operator has not
// re-armed after a comms loss. Surfaced as its own field + warning alert.
inline bool awaiting_rearm(const MissionFlags& f) {
    return f.waypoint && !f.armed && !comms_lost_active(f.comms_lost);
}

// --------------------------------------------- IVPHELM_SUMMARY parsing

// Split a MOOS-style comma-separated k=v string; a segment without '=' is
// re-attached to the previous value (values never start a new key).
inline std::map<std::string, std::string> ParseKeyValues(
        const std::string& s) {
    std::map<std::string, std::string> out;
    std::string cur_key;
    size_t start = 0;
    while (start <= s.size()) {
        size_t comma = s.find(',', start);
        const std::string seg =
            s.substr(start, comma == std::string::npos ? std::string::npos
                                                       : comma - start);
        size_t eq = seg.find('=');
        if (eq != std::string::npos) {
            cur_key = trim(seg.substr(0, eq));
            out[cur_key] = trim(seg.substr(eq + 1));
        } else if (!cur_key.empty() && !seg.empty()) {
            out[cur_key] += "," + seg;  // continuation of previous value
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

// Extract the active behavior names from IVPHELM_SUMMARY.
// active_bhvs value: "name$dur$pwt$...:name2$..." ; "none" when empty.
// Returns "name, name2" (or "" when none).
inline std::string ParseBehaviorList(const std::string& summary,
                                     const std::string& key) {
    auto kv = ParseKeyValues(summary);
    auto it = kv.find(key);
    if (it == kv.end()) return "";
    const std::string& v = it->second;
    if (lower(v) == "none" || v.empty()) return "";
    std::string out;
    size_t start = 0;
    while (start <= v.size()) {
        size_t colon = v.find(':', start);
        std::string entry =
            v.substr(start, colon == std::string::npos ? std::string::npos
                                                       : colon - start);
        std::string name = trim(entry.substr(0, entry.find('$')));
        if (!name.empty()) {
            if (!out.empty()) out += ", ";
            out += name;
        }
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    return out;
}

inline std::string ParseActiveBehaviors(const std::string& summary) {
    return ParseBehaviorList(summary, "active_bhvs");
}

// The operator-facing behavior list is the helm's RUNNING set (the
// behaviors engaged for the mission: waypt_survey, station-keep, opr
// region...). "active" in IvP terms means "produced an objective function
// this iteration", which flickers between none and a subset several times
// a minute (speed/course balks, station-keep inside its radius) and would
// spam the events table. active_bhvs is used only when running_bhvs is
// absent (older helm builds).
inline std::string ParseHelmBehaviors(const std::string& summary) {
    const std::string running = ParseBehaviorList(summary, "running_bhvs");
    if (!running.empty()) return running;
    return ParseActiveBehaviors(summary);
}

// --------------------------------------------- VIEW_SEGLIST parsing

struct SeglistInfo {
    std::string label;
    int points = 0;
};

// Parse "pts={x1,y1:x2,y2:...},label=name,edge_color=..." (BHV_Waypoint
// visual). Returns point count and label; count 0 when malformed/empty.
inline SeglistInfo ParseSeglist(const std::string& spec) {
    SeglistInfo info;
    size_t open = spec.find("pts={");
    if (open != std::string::npos) {
        size_t close = spec.find('}', open);
        if (close != std::string::npos) {
            const std::string body = spec.substr(open + 5, close - open - 5);
            if (!trim(body).empty()) {
                int n = 1;
                for (char c : body)
                    if (c == ':') ++n;
                info.points = n;
            }
        }
    }
    size_t lp = spec.find("label=");
    if (lp != std::string::npos) {
        size_t end = spec.find(',', lp);
        info.label = trim(spec.substr(
            lp + 6, end == std::string::npos ? std::string::npos
                                             : end - lp - 6));
    }
    return info;
}

// Extract an obstacle id from an OBSTACLE_ALERT post. pObstacleMgr posts
// "name=<id>#..." / "...,label=<id>,..." variants; fall back to the whole
// (trimmed) string so distinct alerts still count distinctly.
inline std::string ParseObstacleId(const std::string& alert) {
    for (const char* key : {"name=", "label="}) {
        size_t p = alert.find(key);
        if (p != std::string::npos) {
            size_t start = p + std::string(key).size();
            size_t end = alert.find_first_of(",#", start);
            std::string id = trim(alert.substr(
                start, end == std::string::npos ? std::string::npos
                                                : end - start));
            if (!id.empty()) return id;
        }
    }
    return trim(alert);
}

// --------------------------------------------- time-decayed trackers

// Holds the last posted level until `quiet_s` elapse without a new post,
// then reads 0. Used for OPR_BREACHED (edge posts, no explicit "recovered")
// so the geofence panel/alert self-clears after recovery.
class DecayLatch {
public:
    explicit DecayLatch(double quiet_s) : quiet_s_(quiet_s) {}
    void post(int level, double now) { level_ = level; last_ = now; }
    int level(double now) const {
        return (now - last_ <= quiet_s_) ? level_ : 0;
    }

private:
    double quiet_s_;
    double last_ = -1e18;
    int level_ = 0;
};

// Tracks alerted obstacle ids with a last-seen TTL (BHV_AvoidObstacleV24
// dies at completed_dist without notifying us).
class ObstacleTracker {
public:
    explicit ObstacleTracker(double ttl_s = 120.0) : ttl_s_(ttl_s) {}

    // Returns true when this id is newly alerted (event-worthy).
    bool post(const std::string& id, double now) {
        const bool fresh =
            seen_.find(id) == seen_.end() || now - seen_[id] > ttl_s_;
        seen_[id] = now;
        return fresh;
    }

    int active(double now) {
        int n = 0;
        for (auto it = seen_.begin(); it != seen_.end();) {
            if (now - it->second > ttl_s_) {
                it = seen_.erase(it);
            } else {
                ++n;
                ++it;
            }
        }
        return n;
    }

private:
    double ttl_s_;
    std::map<std::string, double> seen_;
};

// Integrates distance traveled from NAV_X/NAV_Y (local grid, meters) with a
// jump guard against GPS re-acquisition steps. Reset on survey start.
class OdometryIntegrator {
public:
    void reset() {
        has_last_ = false;
        total_ = 0.0;
    }
    double add(double x, double y) {
        if (has_last_) {
            const double dx = x - lx_, dy = y - ly_;
            const double step = std::sqrt(dx * dx + dy * dy);
            if (step < 100.0) total_ += step;  // jump guard
        }
        lx_ = x;
        ly_ = y;
        has_last_ = true;
        return total_;
    }
    double total() const { return total_; }

private:
    bool has_last_ = false;
    double lx_ = 0.0, ly_ = 0.0, total_ = 0.0;
};

// ------------------------------------------------------- rate limiting

// Per-key minimum-interval gate. Keys are "measurement.field". NAV_* arrive
// at 10 Hz and planchaPID outputs at 20 Hz (measured from the vessel
// .alog); nothing above the dashboard refresh rate is visible, and the
// 1-minute downsample discards it, so the bridge caps what it writes.
class RateLimiter {
public:
    explicit RateLimiter(double default_hz = 5.0) { set_default(default_hz); }
    void set_default(double hz) { default_min_ = hz > 0 ? 1.0 / hz : 0.0; }
    void set_rate(const std::string& key, double hz) {
        min_[key] = hz > 0 ? 1.0 / hz : 0.0;
    }
    // true when a sample for `key` may be written at time `now`.
    bool allow(const std::string& key, double now) {
        auto it = min_.find(key);
        const double min_dt = (it == min_.end()) ? default_min_ : it->second;
        auto last = last_.find(key);
        if (last != last_.end() && now - last->second < min_dt) return false;
        last_[key] = now;
        return true;
    }

private:
    double default_min_ = 0.2;
    std::map<std::string, double> min_;
    std::map<std::string, double> last_;
};

// ------------------------------------------------ radar contact tracking

// Contacts as seen by the vessel (NODE_REPORT from iRadarTargets / AIS /
// pContactSpawn in simulation, or RADAR_TARGET with CPA/TCPA). Entries
// expire after `ttl_s` without an update, so the target count follows what
// the radar currently holds rather than everything ever seen.
struct Contact {
    double range_m = -1;   // < 0 = unknown
    double cpa_m = -1;
    double tcpa_s = -1;    // < 0 = diverging / unknown
    double last_seen = -1e18;
    // Kinematics (MOOS local grid) when the source reports them.
    bool have_pos = false;
    double x = 0.0, y = 0.0;
    double spd_mps = -1;   // < 0 = unknown
    double hdg = -1;       // < 0 = unknown
    double bearing = -1;      // true bearing from own ship (deg)
    double rel_bearing = 0;   // -180..180, negative = port
    double rel_speed_mps = -1;
    std::string type;         // NODE_REPORT TYPE (ship, kayak, ...)
    std::string source;       // "report" (AIS / NODE_REPORT) or "radar"
    std::string encounter;    // head-on | crossing | overtaking | ""
    int risk = 0;             // 0 clear, 1 medium, 2 high
};

struct OwnShip {
    bool valid = false;
    double x = 0.0, y = 0.0, spd_mps = 0.0, hdg = 0.0;
};

inline double WrapDeg180(double d) {
    while (d > 180.0) d -= 360.0;
    while (d <= -180.0) d += 360.0;
    return d;
}

// Closest point of approach from current positions and velocities
// (x east, y north, heading clockwise from north). tcpa < 0 means the
// contact is opening (CPA is the current range). Both in metres / seconds.
inline void ComputeCpa(double ox, double oy, double ospd, double ohdg,
                       double cx, double cy, double cspd, double chdg,
                       double* cpa_m, double* tcpa_s) {
    const double rx = cx - ox, ry = cy - oy;
    const double ovx = ospd * std::sin(ohdg * M_PI / 180.0);
    const double ovy = ospd * std::cos(ohdg * M_PI / 180.0);
    const double cvx = cspd * std::sin(chdg * M_PI / 180.0);
    const double cvy = cspd * std::cos(chdg * M_PI / 180.0);
    const double vx = cvx - ovx, vy = cvy - ovy;
    const double v2 = vx * vx + vy * vy;
    const double range = std::sqrt(rx * rx + ry * ry);
    if (v2 < 1e-6) { *cpa_m = range; *tcpa_s = -1; return; }
    const double t = -(rx * vx + ry * vy) / v2;
    if (t <= 0) { *cpa_m = range; *tcpa_s = -1; return; }
    const double px = rx + vx * t, py = ry + vy * t;
    *cpa_m = std::sqrt(px * px + py * py);
    *tcpa_s = t;
}

// COLREGs-style geometric classification from the heading difference:
// reciprocal courses head-on, near-parallel overtaking, otherwise crossing.
inline const char* EncounterType(double own_hdg, double contact_hdg) {
    const double d = std::fabs(WrapDeg180(contact_hdg - own_hdg));
    if (d >= 150.0) return "head-on";
    if (d <= 30.0) return "overtaking";
    return "crossing";
}

// 2 = HIGH: closing inside the alert CPA within the alert TCPA.
// 1 = MEDIUM: closing inside twice the alert CPA, or inside the alert CPA
//     but further out in time. 0 otherwise (opening contacts included).
inline int RiskLevel(double cpa_m, double tcpa_s, double cpa_alert_m,
                     double tcpa_alert_s) {
    if (cpa_m < 0 || tcpa_s < 0) return 0;
    if (cpa_m < cpa_alert_m && tcpa_s < tcpa_alert_s) return 2;
    if (cpa_m < 2.0 * cpa_alert_m && tcpa_s < 2.0 * tcpa_alert_s) return 1;
    return 0;
}

class ContactTracker {
public:
    explicit ContactTracker(double ttl_s = 10.0) : ttl_s_(ttl_s) {}

    void post(const std::string& id, double now, double range_m,
              double cpa_m = -1, double tcpa_s = -1) {
        Contact& c = contacts_[id];
        c.range_m = range_m;
        if (cpa_m >= 0) c.cpa_m = cpa_m;
        if (tcpa_s >= 0) c.tcpa_s = tcpa_s;
        c.last_seen = now;
        if (c.source.empty()) c.source = "radar";
    }

    // Radar target with polar data (RADAR_TARGET: range, true bearing,
    // speed, course, CPA/TCPA as the radar computed them).
    void post_radar(const std::string& id, double now, double range_m,
                    double bearing, double spd_mps, double cog,
                    double cpa_m, double tcpa_s) {
        Contact& c = contacts_[id];
        c.range_m = range_m;
        if (bearing >= 0) c.bearing = bearing;
        if (spd_mps >= 0) c.spd_mps = spd_mps;
        if (cog >= 0) c.hdg = cog;
        if (cpa_m >= 0) c.cpa_m = cpa_m;
        c.tcpa_s = tcpa_s >= 0 ? tcpa_s : -1;
        c.source = "radar";
        c.last_seen = now;
    }

    // NODE_REPORT / AIS-style contact with position and motion.
    void post_report(const std::string& id, double now, double x, double y,
                     double spd_mps, double hdg, const std::string& type) {
        Contact& c = contacts_[id];
        c.have_pos = true;
        c.x = x; c.y = y; c.spd_mps = spd_mps; c.hdg = hdg;
        if (!type.empty()) c.type = type;
        c.source = "report";
        c.last_seen = now;
    }

    // Refresh range / bearing / CPA / risk of every contact that carries a
    // position, from the own-ship state. Radar-only contacts keep the
    // values their source reported.
    void update_geometry(const OwnShip& own, double cpa_alert_m,
                         double tcpa_alert_s) {
        for (auto& [id, c] : contacts_) {
            if (!c.have_pos && own.valid && c.bearing >= 0) {
                // Radar-only: polar data from the sensor, relative values
                // and the encounter type from own heading / course.
                c.rel_bearing = WrapDeg180(c.bearing - own.hdg);
                if (c.hdg >= 0) c.encounter = EncounterType(own.hdg, c.hdg);
                if (c.spd_mps >= 0 && c.hdg >= 0) {
                    const double ovx = own.spd_mps * std::sin(own.hdg * M_PI / 180.0);
                    const double ovy = own.spd_mps * std::cos(own.hdg * M_PI / 180.0);
                    const double cvx = c.spd_mps * std::sin(c.hdg * M_PI / 180.0);
                    const double cvy = c.spd_mps * std::cos(c.hdg * M_PI / 180.0);
                    c.rel_speed_mps = std::hypot(cvx - ovx, cvy - ovy);
                }
            }
            if (c.have_pos && own.valid) {
                const double dx = c.x - own.x, dy = c.y - own.y;
                c.range_m = std::sqrt(dx * dx + dy * dy);
                c.bearing = std::fmod(std::atan2(dx, dy) * 180.0 / M_PI + 360.0, 360.0);
                c.rel_bearing = WrapDeg180(c.bearing - own.hdg);
                if (c.spd_mps >= 0 && c.hdg >= 0) {
                    ComputeCpa(own.x, own.y, own.spd_mps, own.hdg,
                               c.x, c.y, c.spd_mps, c.hdg, &c.cpa_m, &c.tcpa_s);
                    const double ovx = own.spd_mps * std::sin(own.hdg * M_PI / 180.0);
                    const double ovy = own.spd_mps * std::cos(own.hdg * M_PI / 180.0);
                    const double cvx = c.spd_mps * std::sin(c.hdg * M_PI / 180.0);
                    const double cvy = c.spd_mps * std::cos(c.hdg * M_PI / 180.0);
                    c.rel_speed_mps = std::hypot(cvx - ovx, cvy - ovy);
                    c.encounter = EncounterType(own.hdg, c.hdg);
                }
            }
            c.risk = RiskLevel(c.cpa_m, c.tcpa_s, cpa_alert_m, tcpa_alert_s);
        }
    }

    // Drops expired contacts; returns the active count.
    int active(double now) {
        for (auto it = contacts_.begin(); it != contacts_.end();) {
            if (now - it->second.last_seen > ttl_s_) it = contacts_.erase(it);
            else ++it;
        }
        return static_cast<int>(contacts_.size());
    }
    // Smallest known range among active contacts (< 0 when none).
    double closest_range() const {
        double best = -1;
        for (const auto& [id, c] : contacts_)
            if (c.range_m >= 0 && (best < 0 || c.range_m < best)) best = c.range_m;
        return best;
    }
    // CPA/TCPA of the contact with the smallest CPA (< 0 when none known).
    Contact worst_cpa() const {
        Contact best;
        for (const auto& [id, c] : contacts_)
            if (c.cpa_m >= 0 && (best.cpa_m < 0 || c.cpa_m < best.cpa_m)) best = c;
        return best;
    }
    // Id of the highest-risk contact: highest risk level, then smallest
    // CPA, then smallest range. Empty when there are no contacts.
    std::string worst_id() const {
        std::string best;
        const Contact* bc = nullptr;
        for (const auto& [id, c] : contacts_) {
            if (!bc || c.risk > bc->risk ||
                (c.risk == bc->risk && c.cpa_m >= 0 &&
                 (bc->cpa_m < 0 || c.cpa_m < bc->cpa_m))) {
                bc = &c; best = id;
            }
        }
        return best;
    }
    // Time of the most recent post of any contact.
    double last_seen() const {
        double t = -1e18;
        for (const auto& [id, c] : contacts_) t = std::max(t, c.last_seen);
        return t;
    }
    const std::map<std::string, Contact>& contacts() const { return contacts_; }

private:
    double ttl_s_;
    std::map<std::string, Contact> contacts_;
};

// Parse "NAME=x,X=1.0,Y=2.0,..." (NODE_REPORT) or "id=x,range=..,cpa=..,
// tcpa=.." (RADAR_TARGET) into a key/value map with lower-cased keys.
inline std::map<std::string, std::string> ParseKeyValuesLower(
        const std::string& s) {
    std::map<std::string, std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t comma = s.find(',', start);
        const std::string seg = s.substr(
            start, comma == std::string::npos ? std::string::npos
                                              : comma - start);
        size_t eq = seg.find('=');
        if (eq != std::string::npos)
            out[lower(trim(seg.substr(0, eq)))] = trim(seg.substr(eq + 1));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

// ---------------------------------------------------- derived quantities

inline double ProgressPct(double wpt_index, int wpt_total) {
    if (wpt_total <= 0) return 0.0;
    double p = 100.0 * wpt_index / static_cast<double>(wpt_total);
    return std::max(0.0, std::min(100.0, p));
}

// ETA to the next waypoint; floor the speed so a momentary hover doesn't
// explode the estimate.
inline double EtaSeconds(double dist_m, double speed_mps) {
    return dist_m / std::max(0.2, speed_mps);
}

// ------------------------------------------------------- route naming

// Minimal JSON string-member extraction ("name": "...") for the Signal K
// route resource; avoids a JSON dependency in the MOOS app. Handles the
// common escapes (\" \\ \/ \n \t), returns "" when absent.
inline std::string JsonStringMember(const std::string& body,
                                    const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = 0;
    while ((pos = body.find(needle, pos)) != std::string::npos) {
        size_t p = pos + needle.size();
        while (p < body.size() && isspace(static_cast<unsigned char>(body[p]))) ++p;
        if (p >= body.size() || body[p] != ':') { pos = p; continue; }
        ++p;
        while (p < body.size() && isspace(static_cast<unsigned char>(body[p]))) ++p;
        if (p >= body.size() || body[p] != '"') return "";
        std::string out;
        for (++p; p < body.size() && body[p] != '"'; ++p) {
            if (body[p] == '\\' && p + 1 < body.size()) {
                ++p;
                switch (body[p]) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    default:  out += body[p]; break;   // \" \\ \/
                }
            } else {
                out += body[p];
            }
        }
        return out;
    }
    return "";
}

// Human name for an ID_ROUTE value: the Signal K route name when the
// lookup produced one, a readable label for the built-in return route,
// otherwise the id shortened to its first block (UUIDs are unreadable on
// a status tile).
inline std::string RouteDisplayName(const std::string& id,
                                    const std::string& sk_name) {
    if (!sk_name.empty()) return sk_name;
    if (lower(id) == "return_to_base") return "Return to base";
    const size_t dash = id.find('-');
    return dash != std::string::npos && dash >= 4 ? id.substr(0, dash) : id;
}

// Swiftships-UI's Plan Safe Route writes the planned path into the selected
// route and adds a note to its description:
//   "Planned in water >= 3 m with 10 m clearance (US3LA1CB, US5LMOBB)."
// A route carrying the note was checked against the ENC (depth, land,
// hazards). Returns "water >= 3 m, 10 m clearance, US3LA1CB, US5LMOBB" (the
// cells when listed), "" when the description has no such note.
inline std::string ParsePlannerNote(const std::string& description) {
    const std::string tag = "Planned in water >=";
    const size_t at = description.find(tag);
    if (at == std::string::npos) return "";
    const char* p = description.c_str() + at + tag.size();
    char* end = nullptr;
    const double depth = std::strtod(p, &end);
    if (end == p) return "";
    std::string out = "water >= ";
    {
        char b[32];
        std::snprintf(b, sizeof b, "%g", depth);
        out += b;
        out += " m";
    }
    const std::string rest(end);
    const size_t with = rest.find("with ");
    if (with != std::string::npos) {
        const char* q = rest.c_str() + with + 5;
        char* qe = nullptr;
        const double clear = std::strtod(q, &qe);
        if (qe != q && std::string(qe).find("clearance") != std::string::npos) {
            char b[32];
            std::snprintf(b, sizeof b, "%g", clear);
            out += ", " + std::string(b) + " m clearance";
        }
    }
    const size_t lp = rest.find('(');
    const size_t rp = lp == std::string::npos ? std::string::npos : rest.find(')', lp);
    if (lp != std::string::npos && rp != std::string::npos && rp > lp + 1)
        out += ", " + trim(rest.substr(lp + 1, rp - lp - 1));
    return out;
}

// ------------------------------------------------ boat control audit trail
//
// iWebSocket's audit trail, one line per event, posted to MOOS as
// CONTROL_EVENT (control lease, ALL STOP) and UI_AUTH_EVENT (logins,
// refused commands), without the timestamp its daily file carries:
//   CONTROL_EVENT = take user=alice role=operator gen=1
//   CONTROL_EVENT = force user=carol role=supervisor gen=5 from=alice
//   CONTROL_EVENT = force reason=operator not answering
//   CONTROL_EVENT = lost user=alice (not heard)
//   UI_AUTH_EVENT = login user=alice level=readwrite role=operator ip=192.168.22.5 session=k3f9a1x0
//   UI_AUTH_EVENT = rejected key=DESIRED_THRUST user=bob level=readonly why=level
// A forced takeover is two lines (the grant, then the reason); they are
// joined into one event.

struct AuditFields {
    std::string action;
    std::map<std::string, std::string> kv;
    std::string note;   // a trailing "(not heard)" / "(logout)" etc.
};

inline AuditFields ParseAuditFields(const std::string& text) {
    AuditFields a;
    const std::string t = trim(text);
    size_t i = 0;
    auto next_token = [&](size_t from) {
        size_t e = t.find(' ', from);
        return e == std::string::npos ? t.size() : e;
    };
    bool first = true;
    while (i < t.size()) {
        while (i < t.size() && t[i] == ' ') ++i;
        if (i >= t.size()) break;
        if (t[i] == '(') {
            a.note = trim(t.substr(i));
            if (!a.note.empty() && a.note.front() == '(') a.note.erase(0, 1);
            if (!a.note.empty() && a.note.back() == ')') a.note.pop_back();
            break;
        }
        const size_t e = next_token(i);
        const std::string tok = t.substr(i, e - i);
        const size_t eq = tok.find('=');
        if (first && eq == std::string::npos) {
            a.action = tok;
        } else if (eq != std::string::npos) {
            const std::string key = tok.substr(0, eq);
            if (key == "reason") {          // free text to the end of the line
                a.kv[key] = trim(t.substr(i + eq + 1));
                break;
            }
            a.kv[key] = tok.substr(eq + 1);
        } else {
            a.note += (a.note.empty() ? "" : " ") + tok;
        }
        first = false;
        i = e;
    }
    return a;
}

struct AuditEvent {
    std::string category;   // control | login | refused | all_stop
    std::string action;     // take, force, login, rejected ...
    std::string severity;   // info | warning | critical
    std::string user;       // "" when none
    std::string role;
    std::string message;    // operator wording
    std::string details;    // the remaining fields (ip, session, generation ...)
    bool popup = false;     // also an operator popup (Mission Events, severity)
};

inline std::string AuditRefusedWhy(const std::string& why) {
    if (why == "level") return "read-only account";
    if (why == "role") return "role may not command";
    if (why == "control") return "not in control";
    if (why == "moos") return "boat link down";
    if (why == "stale") return "control changed hands before it arrived";
    if (why == "all_stop") return "ALL STOP latched";
    return why.empty() ? "refused" : why;
}

inline AuditEvent DescribeAudit(const std::string& kind, const AuditFields& f) {
    AuditEvent ev;
    ev.action = f.action;
    ev.severity = "info";
    ev.category = kind == "control" ? "control" : "login";
    auto get = [&f](const char* k) {
        auto it = f.kv.find(k);
        return it == f.kv.end() ? std::string() : it->second;
    };
    const std::string user = get("user") == "-" ? "" : get("user");
    ev.user = user;
    ev.role = get("role");
    const std::string who = user.empty() ? "(no login)" : user;
    std::string det;
    auto add = [&det](const std::string& k, const std::string& v) {
        if (!v.empty() && v != "-") det += (det.empty() ? "" : " · ") + k + " " + v;
    };
    const std::string& a = f.action;
    if (kind == "control") {
        if (a == "take") {
            ev.message = who + " took control";
            add("generation", get("gen"));
            add("from", get("from"));
        } else if (a == "handover" || a == "handover-timeout") {
            ev.message = "Control handed to " + who +
                         (get("from").empty() ? "" : " by " + get("from")) +
                         (a == "handover-timeout" ? " (request not answered)" : "");
            add("generation", get("gen"));
        } else if (a == "force") {
            ev.message = who + " forced a takeover" +
                         (get("from").empty() ? "" : " from " + get("from"));
            ev.severity = "warning";
            ev.popup = true;
            add("generation", get("gen"));
            add("reason", get("reason"));
        } else if (a == "release") {
            ev.message = who + " released control";
        } else if (a == "request") {
            ev.message = who + " asked " + (get("holder").empty() ? "the operator" : get("holder")) + " for control";
        } else if (a == "deny") {
            ev.message = (get("holder").empty() ? "The operator" : get("holder")) + " kept control (" + who + " refused)";
        } else if (a == "cancel") {
            ev.message = who + "'s request for control cancelled" + (f.note.empty() ? "" : " (" + f.note + ")");
        } else if (a == "logout") {
            ev.message = who + " logged out holding control: control released";
        } else if (a == "lost") {
            ev.message = "Control lost: " + who + " not heard";
            ev.severity = "warning";
            ev.popup = true;
        } else if (a == "revoke") {
            ev.message = "Control revoked" + (get("why").empty() ? "" : " (" + get("why") + ")");
            ev.severity = "warning";
            ev.popup = true;
        } else if (a == "refused") {
            ev.category = "refused";
            ev.message = "Control " + (get("action").empty() ? "action" : get("action")) + " refused for " + who;
            ev.severity = "warning";
            add("reason", get("reason"));
        } else if (a == "all_stop") {
            ev.category = "all_stop";
            ev.message = "ALL STOP pressed by " + who;
            ev.severity = "critical";
        } else if (a == "all_stop_reset") {
            ev.category = "all_stop";
            ev.message = "ALL STOP reset by " + who;
        } else if (a == "start") {
            ev.message = "Boat link started, nobody in control";
            add("login check", get("command_auth"));
            add("exclusive control", get("exclusive_control"));
        } else {
            ev.message = "Control " + a + (user.empty() ? "" : " " + user);
        }
    } else {
        if (a == "login") {
            ev.message = who + " logged in";
            add("level", get("level"));
        } else if (a == "logout") {
            ev.message = who + " logged out";
        } else if (a == "anonymous") {
            ev.message = "Connection without a login";
            add("level", get("level"));
        } else if (a == "disconnect") {
            ev.message = who + " disconnected";
        } else if (a == "rejected" && !get("login").empty()) {
            ev.category = "refused";
            ev.message = "Login refused: token of a logged-out session";
            ev.severity = "warning";
            ev.popup = true;
        } else if (a == "rejected") {
            ev.category = "refused";
            ev.message = "Command refused: " + (get("key").empty() ? std::string("command") : get("key")) +
                         " from " + who;
            ev.severity = "warning";
            ev.popup = true;
            add("why", AuditRefusedWhy(get("why")));
            add("level", get("level"));
        } else {
            ev.message = "Login " + a + (user.empty() ? "" : " " + user);
        }
        add("ip", get("ip"));
        add("session", get("session"));
    }
    if (!f.note.empty() && a != "cancel" && a != "start") add("note", f.note);
    ev.details = det;
    return ev;
}

// Joins the two-line forced takeover, keeps who holds control and which
// logins are open. Feed every CONTROL_EVENT / UI_AUTH_EVENT line; take the
// finished events from feed() and flush().
class AuditTrail {
public:
    std::vector<AuditEvent> feed(const std::string& kind, const std::string& text, double now) {
        std::vector<AuditEvent> out;
        const AuditFields f = ParseAuditFields(text);
        if (f.action.empty()) return out;
        if (kind == "control" && f.action == "force" && f.kv.count("reason") && !f.kv.count("user")) {
            if (have_pending_) {
                AuditFields joined = pending_;
                joined.kv["reason"] = f.kv.at("reason");
                out.push_back(DescribeAudit("control", joined));
                have_pending_ = false;
            } else {
                AuditFields solo = f;
                out.push_back(DescribeAudit("control", solo));
            }
            return out;
        }
        flush_into(out, now, true);
        // Release, lost and logout lines name the user but not the role:
        // take it from whoever held control.
        const std::string prev_holder = holder_, prev_role = holder_role_;
        Track(kind, f);
        if (kind == "control" && f.action == "force") {
            pending_ = f;
            pending_t_ = now;
            have_pending_ = true;
            return out;
        }
        AuditEvent ev = DescribeAudit(kind, f);
        if (kind == "control" && ev.role.empty() && !ev.user.empty() && ev.user == prev_holder &&
            (f.action == "release" || f.action == "lost" || f.action == "logout"))
            ev.role = prev_role;
        out.push_back(ev);
        return out;
    }
    // A forced takeover whose reason line never came (after 2 s).
    std::vector<AuditEvent> flush(double now) {
        std::vector<AuditEvent> out;
        flush_into(out, now, false);
        return out;
    }
    const std::string& holder() const { return holder_; }
    const std::string& holder_role() const { return holder_role_; }
    // The holder who went silent ("lost", not heard), until someone takes,
    // receives or releases control; "" otherwise.
    const std::string& lost_holder() const { return lost_user_; }
    struct OpenSession { std::string key, user, role, ip; };
    std::vector<OpenSession> open_sessions() const {
        std::vector<OpenSession> v;
        for (const auto& kv : sessions_)
            v.push_back(OpenSession{kv.first, kv.second.user, kv.second.role, kv.second.ip});
        return v;
    }
    // "alice (operator, 192.168.22.5); bob (viewer, 192.168.22.7)", "" when none.
    std::string sessions() const {
        std::string s;
        for (const auto& kv : sessions_) {
            const Session& x = kv.second;
            s += (s.empty() ? "" : "; ") + (x.user.empty() ? std::string("no login") : x.user) +
                 " (" + (x.role.empty() ? std::string("viewer") : x.role) +
                 (x.ip.empty() ? "" : ", " + x.ip) + ")";
        }
        return s;
    }
    size_t session_count() const { return sessions_.size(); }

private:
    struct Session { std::string user, role, ip; };
    void flush_into(std::vector<AuditEvent>& out, double now, bool force_out) {
        if (have_pending_ && (force_out || now - pending_t_ >= 2.0)) {
            out.push_back(DescribeAudit("control", pending_));
            have_pending_ = false;
        }
    }
    void Track(const std::string& kind, const AuditFields& f) {
        auto get = [&f](const char* k) {
            auto it = f.kv.find(k);
            return it == f.kv.end() ? std::string() : it->second;
        };
        const std::string& a = f.action;
        if (kind == "control") {
            if (a == "take" || a == "handover" || a == "handover-timeout" || a == "force") {
                holder_ = get("user") == "-" ? "" : get("user");
                holder_role_ = get("role");
                lost_user_.clear();
            } else if (a == "release" || a == "logout" || a == "lost" || a == "revoke" || a == "start") {
                lost_user_ = a == "lost" ? (get("user") == "-" ? "" : get("user")) : "";
                holder_.clear();
                holder_role_.clear();
            }
            if (a == "start") sessions_.clear();
        } else {
            // A login made with the Signal K cookie at connect time carries
            // no tab session ("-"): such a login is keyed by user and IP.
            std::string sess = get("session");
            const std::string user = get("user") == "-" ? "" : get("user");
            if (sess.empty() || sess == "-") {
                if (user.empty() && get("ip").empty()) return;
                sess = user + "@" + get("ip");
            }
            if (a == "login" || a == "anonymous")
                sessions_[sess] = Session{user, get("role"), get("ip")};
            else if (a == "logout" || a == "disconnect") {
                sessions_.erase(sess);
                // the logout line names no role: drop the user's cookie
                // login on that IP too
                sessions_.erase(user + "@" + get("ip"));
            }
        }
    }
    AuditFields pending_;
    double pending_t_ = 0.0;
    bool have_pending_ = false;
    std::string holder_, holder_role_, lost_user_;
    std::map<std::string, Session> sessions_;
};

// ------------------------------------------------ ENC cells (chart server)

// One chart the onboard chart server lists (Signal K,
// GET /signalk/v1/api/resources/charts). ENC cells converted from S-57
// carry type "S-57"; raster charts and base maps carry other types and do
// not count as an ENC.
struct ChartCell {
    std::string id;            // identifier (the object key when absent)
    std::string name;
    std::string description;
    std::string type;          // "S-57" for ENC cells
    bool has_bounds = false;
    double min_lon = 0.0, min_lat = 0.0, max_lon = 0.0, max_lat = 0.0;   // [W, S, E, N]

    bool is_enc() const {
        const std::string t = lower(type);
        return t.find("s-57") != std::string::npos || t.find("s57") != std::string::npos ||
               t.find("enc") != std::string::npos;
    }
    bool contains(double lat, double lon) const {
        return has_bounds && lon >= min_lon && lon <= max_lon &&
               lat >= min_lat && lat <= max_lat;
    }
    // Smaller bounds = larger scale = the more detailed cell.
    double area() const { return (max_lon - min_lon) * (max_lat - min_lat); }
};

// Parses the chart listing: a JSON object keyed by chart id whose members
// are flat objects ("name", "description", "bounds": [W, S, E, N], "type",
// "identifier", ...). Depth-aware scan, no JSON library (like
// JsonStringMember). Malformed or empty input gives an empty list.
inline std::vector<ChartCell> ParseSignalKCharts(const std::string& body) {
    std::vector<ChartCell> out;
    int depth = 0;
    bool in_str = false;
    std::string last_key;      // last string seen at depth 1 (the chart id)
    std::string cur;           // text of the chart object being read
    size_t obj_start = std::string::npos;
    for (size_t i = 0; i < body.size(); ++i) {
        const char c = body[i];
        if (in_str) {
            if (c == '\\') { ++i; continue; }
            if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') {
            if (depth == 1) {
                size_t e = i + 1;
                while (e < body.size() && body[e] != '"') { if (body[e] == '\\') ++e; ++e; }
                last_key = body.substr(i + 1, e - i - 1);
            }
            in_str = true;
            continue;
        }
        if (c == '{') {
            ++depth;
            if (depth == 2) obj_start = i;
        } else if (c == '}') {
            if (depth == 2 && obj_start != std::string::npos) {
                const std::string obj = body.substr(obj_start, i - obj_start + 1);
                ChartCell cell;
                cell.id = JsonStringMember(obj, "identifier");
                cell.name = JsonStringMember(obj, "name");
                if (cell.id.empty()) cell.id = cell.name.empty() ? last_key : cell.name;
                if (cell.name.empty()) cell.name = cell.id;
                cell.description = JsonStringMember(obj, "description");
                cell.type = JsonStringMember(obj, "type");
                const size_t b = obj.find("\"bounds\"");
                if (b != std::string::npos) {
                    const size_t lb = obj.find('[', b);
                    if (lb != std::string::npos) {
                        double v[4];
                        const char* p = obj.c_str() + lb + 1;
                        int n = 0;
                        for (; n < 4; ++n) {
                            char* end = nullptr;
                            v[n] = std::strtod(p, &end);
                            if (end == p) break;
                            p = end;
                            while (*p == ' ' || *p == ',') ++p;
                        }
                        if (n == 4 && v[0] <= v[2] && v[1] <= v[3]) {
                            cell.has_bounds = true;
                            cell.min_lon = v[0]; cell.min_lat = v[1];
                            cell.max_lon = v[2]; cell.max_lat = v[3];
                        }
                    }
                }
                if (!cell.id.empty()) out.push_back(cell);
                obj_start = std::string::npos;
            }
            if (depth > 0) --depth;
        }
    }
    return out;
}

// What the Readiness Chart / ENC row needs: how many ENC cells the chart
// server lists and, when the boat's position is known, the most detailed
// cell covering it ("" when none does).
struct EncCoverage {
    int cells = 0;
    std::string cell;
};

inline EncCoverage EncCoverageAt(const std::vector<ChartCell>& charts, bool have_pos,
                                 double lat, double lon) {
    EncCoverage out;
    double best = 1e18;
    for (const auto& c : charts) {
        if (!c.is_enc()) continue;
        ++out.cells;
        if (have_pos && c.contains(lat, lon)) {
            const double a = c.area();
            if (a < best || (a == best && c.id < out.cell)) { best = a; out.cell = c.id; }
        }
    }
    return out;
}

// ------------------------------------------------- geometry (map overlays)

struct XY { double x = 0.0, y = 0.0; };

// Points of a MOOS "pts={x,y:x,y:...}" spec (VIEW_SEGLIST, VIEW_POLYGON,
// OPR_POLY_ACTUAL). Empty for "NONE", malformed or missing pts.
inline std::vector<XY> ParsePts(const std::string& spec) {
    std::vector<XY> out;
    const size_t open = spec.find("pts={");
    if (open == std::string::npos) return out;
    const size_t close = spec.find('}', open);
    if (close == std::string::npos) return out;
    const std::string body = spec.substr(open + 5, close - open - 5);
    size_t start = 0;
    while (start <= body.size()) {
        const size_t colon = body.find(':', start);
        const std::string seg = trim(body.substr(
            start, colon == std::string::npos ? std::string::npos : colon - start));
        const size_t comma = seg.find(',');
        if (comma != std::string::npos) {
            try {
                out.push_back({std::stod(seg.substr(0, comma)),
                               std::stod(seg.substr(comma + 1))});
            } catch (...) { return {}; }
        } else if (!seg.empty()) {
            return {};   // malformed vertex: reject the whole spec
        }
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    return out;
}

struct LatLon { double lat = 0.0, lon = 0.0; };

// MOOS local grid (x east, y north, metres from LatOrigin/LongOrigin) to
// geographic coordinates. Metres-per-degree series expansion; well under
// a metre of error across a mission-sized area.
inline LatLon LocalToLatLon(double x, double y, double lat0, double lon0) {
    const double phi = lat0 * M_PI / 180.0;
    const double m_per_deg_lat = 111132.92 - 559.82 * std::cos(2 * phi)
                                 + 1.175 * std::cos(4 * phi);
    const double m_per_deg_lon = 111412.84 * std::cos(phi)
                                 - 93.5 * std::cos(3 * phi);
    return {lat0 + y / m_per_deg_lat, lon0 + x / m_per_deg_lon};
}

// Inverse of LocalToLatLon: geographic coordinates to the MOOS local grid
// of the given datum. Used for AIS / NODE_REPORT contacts that carry
// LAT/LON, so their position never depends on the datum the posting app
// used for its own X/Y.
inline XY LatLonToLocal(double lat, double lon, double lat0, double lon0) {
    const double phi = lat0 * M_PI / 180.0;
    const double m_per_deg_lat = 111132.92 - 559.82 * std::cos(2 * phi)
                                 + 1.175 * std::cos(4 * phi);
    const double m_per_deg_lon = 111412.84 * std::cos(phi)
                                 - 93.5 * std::cos(3 * phi);
    XY p;
    p.x = (lon - lon0) * m_per_deg_lon;
    p.y = (lat - lat0) * m_per_deg_lat;
    return p;
}

// LAT / LON (or LONG) from a parsed NODE_REPORT. False when absent,
// unparsable, out of range, or the 0,0 placeholder some posters send
// before they have a fix.
inline bool ReportLatLon(const std::map<std::string, std::string>& kv,
                         double* lat, double* lon) {
    auto la = kv.find("lat");
    auto lo = kv.find("lon");
    if (lo == kv.end()) lo = kv.find("long");
    if (la == kv.end() || lo == kv.end() || la->second.empty() || lo->second.empty())
        return false;
    try {
        *lat = std::stod(la->second);
        *lon = std::stod(lo->second);
    } catch (...) {
        return false;
    }
    if (!std::isfinite(*lat) || !std::isfinite(*lon)) return false;
    if (std::fabs(*lat) > 90.0 || std::fabs(*lon) > 180.0) return false;
    if (*lat == 0.0 && *lon == 0.0) return false;
    return true;
}

// ------------------------------------------------- console checklist

// CHECKLIST_STATUS from the operator console (see docs/deployment.md,
// "Console checklist"): "done=9,total=11,pending=Weather Conditions|Port
// Clearance,by=douglas". Missing numbers parse as -1.
struct ChecklistItem {
    std::string name, description;
    bool ticked = false;
};

struct Checklist {
    int done = -1, total = -1;
    std::string pending;   // "|"-separated item names, may be empty
    std::string by;
    // Full list when the console sends items=Name:ticked:Description|...
    std::vector<ChecklistItem> items;
};

inline std::vector<std::string> SplitBar(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char ch : s + "|") {
        if (ch == '|') {
            cur = trim(cur);
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else cur += ch;
    }
    return out;
}

// "Name:Description|Name2:Description2" (readiness config checklist_items)
// or "Name:ticked:Description|..." (console items=). Missing parts are
// empty; a middle token of ticked / pending / done / open is the state.
inline std::vector<ChecklistItem> ParseChecklistItems(const std::string& spec) {
    std::vector<ChecklistItem> out;
    for (const std::string& part : SplitBar(spec)) {
        ChecklistItem it;
        const size_t c1 = part.find(':');
        if (c1 == std::string::npos) { it.name = trim(part); out.push_back(it); continue; }
        it.name = trim(part.substr(0, c1));
        std::string rest = trim(part.substr(c1 + 1));
        const size_t c2 = rest.find(':');
        const std::string first = lower(trim(rest.substr(0, c2)));
        if (first == "ticked" || first == "done" || first == "pending" || first == "open") {
            it.ticked = (first == "ticked" || first == "done");
            rest = c2 == std::string::npos ? "" : trim(rest.substr(c2 + 1));
        }
        it.description = rest;
        out.push_back(it);
    }
    return out;
}

// Rows for the console-checklist table (measurement checklist): one row
// per item (TICKED / PENDING, with its description) and a footer with the
// count and the operator. When the console sends only counts and the
// pending names, the configured item list (checklist_items) supplies the
// names: every configured item not pending is ticked. With neither, the
// table shows the head line and the pending items only.
struct ChecklistRow { std::string item, description, state; };

inline std::vector<ChecklistRow> ChecklistRows(const Checklist& c,
                                               const std::vector<ChecklistItem>& configured = {}) {
    std::vector<ChecklistRow> rows;
    if (c.total < 0) return rows;
    const bool complete = c.done >= c.total;
    const std::string count = std::to_string(c.done) + " / " + std::to_string(c.total);
    const std::vector<std::string> pending = SplitBar(c.pending);
    auto is_pending = [&](const std::string& name) {
        for (const auto& pn : pending) if (lower(pn) == lower(name)) return true;
        return false;
    };
    if (!c.items.empty()) {
        for (const auto& it : c.items)
            rows.push_back({it.name, it.description, it.ticked ? "TICKED" : "PENDING"});
    } else if (!configured.empty()) {
        for (const auto& it : configured)
            rows.push_back({it.name, it.description, is_pending(it.name) ? "PENDING" : "TICKED"});
        for (const auto& pn : pending) {   // pending items the config does not know
            bool known = false;
            for (const auto& it : configured) if (lower(it.name) == lower(pn)) known = true;
            if (!known) rows.push_back({pn, "", "PENDING"});
        }
    } else {
        for (const auto& pn : pending) rows.push_back({pn, "", "PENDING"});
    }
    rows.push_back({"Console checklist " + count, c.by.empty() ? "" : "ticked by " + c.by,
                    complete ? "COMPLETE" : "OPEN"});
    return rows;
}

inline Checklist ParseChecklist(const std::string& spec) {
    Checklist c;
    auto kv = ParseKeyValues(spec);
    auto num = [&](const char* k) {
        auto it = kv.find(k);
        if (it == kv.end()) return -1;
        try { return std::stoi(it->second); } catch (...) { return -1; }
    };
    c.done = num("done");
    c.total = num("total");
    if (auto it = kv.find("pending"); it != kv.end()) c.pending = trim(it->second);
    if (auto it = kv.find("by"); it != kv.end()) c.by = trim(it->second);
    if (auto it = kv.find("items"); it != kv.end()) c.items = ParseChecklistItems(it->second);
    return c;
}

// ------------------------------------------------ pre-departure readiness
// Every automatic check is decided here, onboard, from the latest sample
// of each telemetry field (what the bridge itself writes plus a few values
// other producers write to InfluxDB: host metrics, Signal K link, CAN
// interface, cameras). The dashboard only displays the result. Status
// codes are shared with the Readiness dashboard's value mappings.
enum ReadinessStatus { READY_PASS = 0, READY_FAIL = 1, READY_NA = 2, READY_CONSOLE = 3 };

struct Sample {
    bool is_str = false;
    double num = 0.0;
    std::string str;
    double t = 0.0;   // monotonic seconds when the sample was seen
};

// Latest value per "measurement.field", with an age test.
class LatestStore {
public:
    void put(const std::string& m, const std::string& f, double v, double t) {
        Sample& s = map_[m + "." + f];
        s.is_str = false; s.num = v; s.t = t;
    }
    void put_str(const std::string& m, const std::string& f, const std::string& v, double t) {
        Sample& s = map_[m + "." + f];
        s.is_str = true; s.str = v; s.t = t;
    }
    // nullptr when never seen or older than max_age seconds.
    const Sample* get(const std::string& m, const std::string& f, double now,
                      double max_age = 120.0) const {
        auto it = map_.find(m + "." + f);
        if (it == map_.end() || now - it->second.t > max_age) return nullptr;
        return &it->second;
    }
    size_t size() const { return map_.size(); }

private:
    std::map<std::string, Sample> map_;
};

struct ReadinessCheck {
    int seq = 0;
    std::string id;         // tag value on the readiness measurement
    std::string name;       // operator wording
    std::string evidence;   // what was seen, one line
    int status = READY_NA;
};

struct ReadinessSummary {
    bool ready = false;
    int passed = 0, failed = 0, na = 0, console = 0;
    std::string blocking;   // "; "-separated names of the failed checks
};

inline std::string fmt1(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.1f", v);
    return b;
}
// "1,067" for the litres in the readiness evidence.
inline std::string fmt_thousands(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.0f", v);
    std::string s(b);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(i, ",");
    return s;
}
inline std::string fmt0(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.0f", v);
    return b;
}

// Point in polygon (ray casting), local grid. Used for "route inside the
// operating region".
inline bool PointInPolygon(double x, double y, const std::vector<XY>& poly) {
    bool inside = false;
    const size_t n = poly.size();
    if (n < 3) return false;
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const XY& a = poly[i];
        const XY& b = poly[j];
        if ((a.y > y) != (b.y > y) &&
            x < (b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x)
            inside = !inside;
    }
    return inside;
}

inline bool RouteInsideRegion(const std::vector<XY>& route, const std::vector<XY>& region) {
    if (route.empty() || region.size() < 3) return false;
    for (const XY& p : route)
        if (!PointInPolygon(p.x, p.y, region)) return false;
    return true;
}

inline double RouteLengthM(const std::vector<XY>& route) {
    double len = 0.0;
    for (size_t i = 1; i < route.size(); ++i)
        len += std::hypot(route[i].x - route[i - 1].x, route[i].y - route[i - 1].y);
    return len;
}

// App names from the mission file's ANTLER block ("Run = pHelmIvP @
// NewConsole=false", "Run = uTimerScript @ ... ~ uTimerScript_A"): the
// MOOSDB client name is the alias when given, else the app name.
inline std::vector<std::string> ParseAntlerApps(const std::vector<std::string>& lines) {
    std::vector<std::string> apps;
    for (const std::string& raw : lines) {
        std::string line = raw;
        const size_t cmt = line.find("//");
        if (cmt != std::string::npos) line = line.substr(0, cmt);
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        if (lower(trim(line.substr(0, eq))) != "run") continue;
        std::string rhs = line.substr(eq + 1);
        std::string name;
        const size_t tilde = rhs.find('~');
        if (tilde != std::string::npos) name = trim(rhs.substr(tilde + 1));
        else {
            const size_t at = rhs.find('@');
            name = trim(at == std::string::npos ? rhs : rhs.substr(0, at));
        }
        if (!name.empty() && name != "MOOSDB") apps.push_back(name);
    }
    return apps;
}

// Apps in `required` missing from a DB_CLIENTS list ("a,b,c").
inline std::vector<std::string> MissingApps(const std::vector<std::string>& required,
                                            const std::string& db_clients) {
    std::vector<std::string> present = SplitCsv(db_clients), missing;
    for (const auto& r : required) {
        bool found = false;
        for (const auto& p : present) if (trim(p) == r) found = true;
        if (!found) missing.push_back(r);
    }
    return missing;
}

// Vessel constants for the readiness checks (grafana_bridge ProcessConfig).
// Zero / empty disables the check that needs it (row reads N/A).
struct ReadinessConfig {
    double fuel_tank_l = 0.0;       // usable fuel
    double cruise_fuel_lph = 0.0;   // consumption at cruise
    double cruise_speed_kn = 0.0;   // planning speed for the route time
    double battery_kwh = 0.0;       // usable battery energy
    double cruise_power_kw = 0.0;   // electrical load under way
    double endurance_margin = 1.5;  // required = route time x margin (return leg, weather)
    // From VESSEL_* in /etc/usv/usv.env (cpp-common/vessel_profile.hpp):
    // the tank size when real.moos has no fuel_tank_l, and the hull's
    // maximum speed for the Speed cap check (0 = not configured).
    double vessel_fuel_l = 0.0;
    double vessel_max_speed_kn = 0.0;
    std::vector<std::string> required_apps;   // MOOS apps that must be connected
    std::vector<std::string> optional_apps;   // reported when absent, never a fault (GUI viewers)
    std::vector<ChecklistItem> checklist_items;
    // Console-fed rows. A vessel without an ENC chart system, or whose
    // console does not publish the pre-departure checklist, reads N/A on
    // those rows instead of waiting for the console (CONSOLE blocks READY).
    bool enc_fitted = true;
    bool console_checklist = true;
    // The mission runs the navigation guard (pNavGuard in the app list):
    // its silence then fails the Navigation integrity row, since the boat
    // refuses autonomy starts without it (nav_guard_required).
    bool nav_guard_expected = false;
};

// strict (vessel profile): a check whose sensor data is missing FAILS
// instead of reading N/A. In simulation there is no engine, CAN or camera
// data, so the simulation profile reports those as N/A.
inline std::vector<ReadinessCheck> EvaluateReadiness(const LatestStore& s, double now,
                                                     bool strict,
                                                     const ReadinessConfig& cfg = {}) {
    std::vector<ReadinessCheck> out;
    const int missing = strict ? READY_FAIL : READY_NA;
    auto add = [&](const char* id, const char* name, const std::string& ev, int st) {
        ReadinessCheck c;
        c.seq = static_cast<int>(out.size()) + 1;
        c.id = id; c.name = name; c.evidence = ev; c.status = st;
        out.push_back(c);
    };
    auto num = [&](const char* m, const char* f, double age = 120.0) {
        return s.get(m, f, now, age);
    };

    // 1 GPS position fix
    {
        const Sample* lat = num("nav", "lat", 15.0);
        const Sample* sats = num("nav", "satellites", 30.0);
        const Sample* hdop = num("nav", "hdop", 30.0);
        const Sample* fix = num("nav", "fix_type", 30.0);
        std::string ev;
        int st;
        if (!lat) { ev = "no position"; st = READY_FAIL; }
        else {
            bool ok = true;
            if (fix) {
                const std::string f = lower(fix->str);
                ev += fix->str;
                // "none" / "no fix" (GNSS), "INS aligning" / "GNSS lost"
                // (VectorNav INS via Signal K) are not a usable fix.
                ok = ok && f != "none" && f != "no fix"
                        && f != "ins aligning" && f != "gnss lost";
            }
            if (sats) { ev += (ev.empty() ? "" : " · ") + fmt0(sats->num) + " sats"; ok = ok && sats->num >= 6.0; }
            if (hdop) { ev += " · HDOP " + fmt1(hdop->num); ok = ok && hdop->num <= 2.5; }
            if (!sats && !fix) {
                ev = "position available, no GPS quality data";
                st = strict ? READY_FAIL : READY_PASS;
            } else st = ok ? READY_PASS : READY_FAIL;
        }
        add("gps", "GPS position fix", ev, st);
    }
    // 1b Navigation integrity: the boat's navigation guard compares heading,
    // speed and course with what the GPS positions show (autonomy.nav_guard
    // 0 ok, 1 warning, 2 fault, written once a second while it reports). In
    // a fault the boat ends autonomy and refuses starts.
    {
        const Sample* g = num("autonomy", "nav_guard", 10.0);
        const Sample* m = num("mission_state", "nav_status_msg", 1e9);
        const std::string why = (m && m->is_str && !m->str.empty() && m->str != "-") ? m->str : "";
        if (!g) {
            if (cfg.nav_guard_expected)
                add("nav", "Navigation integrity", "navigation guard not reporting · autonomy starts refused", READY_FAIL);
            else
                add("nav", "Navigation integrity", "no navigation guard on this vessel", READY_NA);
        } else if (g->num >= 2.0) {
            add("nav", "Navigation integrity",
                "FAULT" + (why.empty() ? std::string() : ": " + why) + " · autonomy starts refused", READY_FAIL);
        } else if (g->num >= 1.0) {
            add("nav", "Navigation integrity",
                "warning" + (why.empty() ? std::string() : ": " + why), READY_PASS);
        } else {
            add("nav", "Navigation integrity", "heading, speed and track consistent with GPS", READY_PASS);
        }
    }
    // A route planned with Swiftships-UI's safe-route tool is checked
    // against the ENC (depth, land, hazards) and saved with "(safe)" in its
    // name; "(unsafe)" marks one that failed the check. A safe route stands
    // in for the operating region on the Course row, and shows that an ENC
    // chart is in use on the Chart / ENC row.
    const Sample* route = num("mission_state", "route_name", 1e9);
    const bool have_route = route && !route->str.empty() && route->str != "-";
    const std::string route_lc = have_route ? lower(route->str) : "";
    // Since Swiftships-UI updates the selected route in place, the check
    // is recorded in the route's description instead of its name; the
    // bridge reads that note with the route (mission_state.route_plan).
    const Sample* plan = num("mission_state", "route_plan", 1e9);
    const bool route_planned = have_route && plan && plan->is_str &&
                               !plan->str.empty() && plan->str != "-";
    const bool route_enc_safe = route_lc.find("(safe)") != std::string::npos || route_planned;
    const bool route_enc_unsafe = route_lc.find("(unsafe)") != std::string::npos;
    // 2 Course planning: route, waypoints, operating region (or a route
    // checked safe on the ENC)
    {
        // Last known (also read back from InfluxDB after a community
        // restart, like the Route tile); flagged when older than 10 min.
        const Sample* wpts = num("mission", "waypoints_total", 1e9);
        const Sample* region = num("mission", "region_vertices", 1e9);
        const bool have_wpts = wpts && wpts->num > 0.0;
        const bool have_region = region && region->num >= 3.0;
        const Sample* inside = num("mission", "route_in_region", 1e9);
        const Sample* len = num("mission", "route_length_m", 1e9);
        std::string ev = have_route ? route->str : "no route loaded";
        if (have_route && now - route->t > 600.0) ev += " (last known)";
        ev += have_wpts ? " · " + fmt0(wpts->num) + " waypoints" : " · no waypoints";
        if (len && len->num > 0.0) ev += " · " + fmt1(len->num / 1852.0) + " nm";
        if (have_region) ev += " · region " + fmt0(region->num) + " vertices";
        else if (route_planned) ev += " · checked safe on ENC (" + plan->str + ")";
        else if (route_enc_safe) ev += " · checked safe on ENC";
        else ev += " · no operating region or ENC safe route";
        bool ok = have_route && have_wpts && (have_region || route_enc_safe);
        if (ok && have_region && inside) {
            if (inside->num == 1.0) ev += " · route inside region";
            else { ev += " · ROUTE OUTSIDE REGION"; ok = false; }
        }
        if (route_enc_unsafe) { ev += " · ROUTE NOT SAFE ON ENC"; ok = false; }
        add("course", "Course planning", ev, ok ? READY_PASS : READY_FAIL);
    }
    // 3 Chart / ENC selected. Sources in order: the console's ENC_STATUS
    // when it publishes one ("selected:<chart>" / "none"); the ENC cells
    // the onboard chart server lists (chart.enc_* from the bridge's Signal
    // K poll), checked against the boat's position; a route checked safe
    // on the ENC; otherwise N/A on a vessel without an ENC (enc_fitted =
    // false) or CONSOLE.
    {
        const Sample* enc = num("mission_state", "enc_status", 1e9);
        const Sample* cells = num("chart", "enc_cells", 300.0);
        const Sample* covers = num("chart", "enc_covers", 300.0);
        const Sample* cell = num("chart", "enc_cell", 300.0);
        if (enc)
            add("enc", "Chart / ENC selected", enc->str,
                enc->str.rfind("selected", 0) == 0 ? READY_PASS : READY_FAIL);
        else if (cells && cells->num >= 1.0) {
            const std::string n = fmt0(cells->num) + (cells->num == 1.0 ? " cell" : " cells") + " loaded";
            if (covers && covers->num == 1.0 && cell && cell->is_str && !cell->str.empty())
                add("enc", "Chart / ENC selected", "ENC " + cell->str + " · covers position · " + n, READY_PASS);
            else if (covers && covers->num == 0.0)
                add("enc", "Chart / ENC selected", "ENC " + n + " · NO CELL COVERS POSITION", READY_FAIL);
            else
                add("enc", "Chart / ENC selected", "ENC " + n + " · position unknown", READY_PASS);
        }
        else if (route_enc_safe)
            add("enc", "Chart / ENC selected", "ENC in use · route checked safe on chart", READY_PASS);
        else if (!cfg.enc_fitted) add("enc", "Chart / ENC selected", "no ENC chart system on this vessel", READY_NA);
        else add("enc", "Chart / ENC selected", "console not publishing ENC_STATUS", READY_CONSOLE);
    }
    // 4 Autonomy stack
    {
        const Sample* rate = num("comms", "moos_msg_rate", 20.0);
        const Sample* helm = num("mission_state", "helm_state", 1e9);
        const Sample* allstop = num("autonomy", "allstop", 10.0);
        std::string ev;
        bool ok = rate && rate->num >= 1.0;
        ev = rate ? fmt0(rate->num) + " msg/s" : "no autonomy traffic";
        if (helm) { ev += " · helm " + helm->str; ok = ok && helm->str != "MALCONFIG"; }
        else { ev += " · helm state unknown"; ok = false; }
        if (allstop && allstop->num != 0.0) { ev += " · ALL-STOP"; ok = false; }
        // Autopilot service mode: manual tuning gains replace the schedule.
        const Sample* svc = num("autopilot", "service_mode", 1e9);
        if (svc && svc->num != 0.0) ev += " · SERVICE MODE (tuning gains)";
        add("autonomy", "Autonomy stack", ev, ok ? READY_PASS : READY_FAIL);
    }
    // 5 MOOS community complete: every app the Antler block starts (or
    // required_apps) is connected to MOOSDB (DB_CLIENTS).
    {
        const Sample* clients = num("moos", "clients", 30.0);
        if (cfg.required_apps.empty())
            add("apps", "Apps", "no app list (Antler block / required_apps)", READY_NA);
        else if (!clients)
            add("apps", "Apps", "DB_CLIENTS not received", READY_FAIL);
        else {
            std::vector<std::string> must, opt_missing;
            for (const auto& a : cfg.required_apps) {
                bool optional = false;
                for (const auto& o : cfg.optional_apps) if (o == a) optional = true;
                if (!optional) must.push_back(a);
                else if (!MissingApps({a}, clients->str).empty()) opt_missing.push_back(a);
            }
            const auto missing = MissingApps(must, clients->str);
            std::string ev = std::to_string(must.size() - missing.size()) + " / " +
                             std::to_string(must.size()) + " running";
            if (!missing.empty()) {
                ev += " · missing: ";
                for (size_t i = 0; i < missing.size(); ++i) ev += (i ? ", " : "") + missing[i];
            }
            if (!opt_missing.empty()) {
                ev += " · optional not running: ";
                for (size_t i = 0; i < opt_missing.size(); ++i) ev += (i ? ", " : "") + opt_missing[i];
            }
            add("apps", "Apps", ev, missing.empty() ? READY_PASS : READY_FAIL);
        }
    }
    // 6 Steering / actuators: rudder feedback (ANGULO_LEME) and command
    {
        const Sample* act = num("autopilot", "rudder_actual", 15.0);
        const Sample* cmd = num("autopilot", "rudder_cmd", 15.0);
        // The drive interface's own check: rudder feedback differs from
        // what it sends for longer than its alarm time.
        const Sample* mis = num("can_drive", "rudder_mismatch", 1e9);
        const bool mismatch = mis && mis->num != 0.0;
        if (!act && !cmd)
            add("steering", "Steering / actuators",
                mismatch ? "RUDDER NOT FOLLOWING DRIVE COMMAND" : "no rudder feedback or command",
                mismatch ? READY_FAIL : missing);
        else {
            bool ok = true;
            std::string ev;
            if (act) { ev += "rudder " + fmt1(act->num) + "°"; ok = ok && std::fabs(act->num) <= 35.0; }
            if (cmd) { ev += std::string(ev.empty() ? "" : " · ") + "cmd " + fmt1(cmd->num) + "°"; ok = ok && std::fabs(cmd->num) <= 35.0; }
            if (act && cmd && std::fabs(act->num - cmd->num) > 10.0) { ev += " · NOT FOLLOWING"; ok = false; }
            if (!act) { ev += " · no rudder feedback"; if (strict) ok = false; }
            if (mismatch) { ev += " · RUDDER NOT FOLLOWING DRIVE COMMAND"; ok = false; }
            add("steering", "Steering / actuators", ev, ok ? READY_PASS : READY_FAIL);
        }
    }
    // 7 Operator link and comms
    {
        const Sample* sk = num("comms", "signalk_ws_up", 60.0);
        const Sample* inet = num("ping", "result_code", 120.0);
        const Sample* lat = num("ping", "average_response_ms", 120.0);
        const Sample* phase = num("autonomy", "comms_phase", 10.0);
        std::string ev = sk ? std::string("Signal K ") + (sk->num == 1.0 ? "UP" : "DOWN") : "Signal K unknown";
        bool ok = sk && sk->num == 1.0;
        if (inet) { ev += std::string(" · Internet ") + (inet->num == 0.0 ? "UP" : "DOWN"); ok = ok && inet->num == 0.0; }
        if (lat && inet && inet->num == 0.0) ev += " · " + fmt0(lat->num) + " ms";
        if (phase && phase->num != 0.0) { ev += " · operator link LOST"; ok = false; }
        add("comms", "Operator link & comms", ev, ok ? READY_PASS : READY_FAIL);
    }
    // 8 CAN bus (NMEA 2000 / J1939 interface)
    {
        const Sample* up = num("canbus", "interface_up", 60.0);
        const Sample* rate = num("canbus", "msg_rate", 60.0);
        // The drive interface (what sends rudder, thrust and gear on the
        // bus): its status, and IDs it is not sending because another node
        // transmits them.
        const Sample* dst = num("can_drive", "status", 1e9);
        const Sample* dcf = num("can_drive", "conflict", 1e9);
        std::string drive;
        bool drive_ok = true;
        if (dst && dst->is_str && !dst->str.empty()) {
            const std::string st = dst->str;
            if (st == "INHIBITED") { drive = "DRIVE PROFILE NOT LOADED"; drive_ok = false; }
            else if (st == "NO_BUS") { drive = "DRIVE INTERFACE: NO BUS"; drive_ok = false; }
            else drive = "drive " + lower(st);
            if (dcf && dcf->is_str && !dcf->str.empty() && dcf->str != "none" && dcf->str != "-") {
                drive += " · BLOCKED, ANOTHER NODE SENDS " + dcf->str;
                drive_ok = false;
            }
        }
        if (!up) {
            if (drive.empty()) add("can", "CAN bus", "no CAN bridge data", missing);
            else add("can", "CAN bus", "no CAN bridge data · " + drive, strict || !drive_ok ? READY_FAIL : READY_NA);
        } else if (up->num == 1.0) {
            std::string ev = "interface UP" + (rate ? " · " + fmt0(rate->num) + " msg/s" : std::string());
            if (!drive.empty()) ev += " · " + drive;
            add("can", "CAN bus", ev, drive_ok ? READY_PASS : READY_FAIL);
        } else   // simulation host: can_bridge runs with no CAN hardware
            add("can", "CAN bus", strict ? "interface DOWN" : "interface DOWN (no CAN hardware in simulation)",
                strict ? READY_FAIL : READY_NA);
    }
    // 9 Engine pre-start (J1939)
    {
        const Sample* rpm = num("engine", "rpm", 60.0);
        const Sample* cool = num("engine", "coolant_temp", 60.0);
        const Sample* oil = num("engine", "oil_pressure", 60.0);
        const Sample* fuel = num("engine", "fuel_level", 120.0);
        const Sample* dtcs = num("engine", "active_dtcs", 120.0);
        // Helm / drive state from the CAN bus (usv-722: key switch, gear and
        // thrust sent to the engine; its engine data arrives through Signal K
        // once the Yamaha gateway is fitted). On usv-725 the J1939 engine
        // (iCANRec) supplies rpm, coolant, oil pressure and faults.
        const Sample* key = num("engine", "key_state", 60.0);
        const Sample* gear = num("engine", "gear", 60.0);
        const Sample* thr = num("engine", "throttle_pct", 60.0);
        if (!cool && !oil && !fuel && !key && !rpm) add("engine", "Engine pre-start", "no engine data", missing);
        else {
            bool ok = true;
            std::string ev;
            auto sep = [&ev]() { return std::string(ev.empty() ? "" : " · "); };
            if (key) {
                const char* k = key->num >= 2.0 ? "START" : (key->num >= 1.0 ? "RUN" : "OFF");
                ev += std::string("key ") + k;
                ok = ok && key->num >= 1.0;
            }
            if (gear) ev += sep() + "gear " + (gear->num > 0.5 ? "FORWARD" : (gear->num < -0.5 ? "REVERSE" : "NEUTRAL"));
            if (thr) ev += sep() + "thrust " + fmt0(thr->num) + " %";
            if (rpm) ev += sep() + "rpm " + fmt0(rpm->num);
            if (cool) { ev += sep() + "coolant " + fmt0(cool->num) + " °C"; ok = ok && cool->num < 105.0; }
            // Oil pressure is only meaningful with the engine running: a
            // stopped engine with the key on reports 0 kPa.
            const bool running = rpm && rpm->num > 700.0;
            if (oil && running && oil->num < 150.0) { ev += sep() + "oil " + fmt0(oil->num) + " kPa LOW"; ok = false; }
            else if (oil) ev += sep() + "oil " + fmt0(oil->num) + " kPa";
            if (fuel) { ev += sep() + "fuel " + fmt0(fuel->num) + " %"; ok = ok && fuel->num > 15.0; }
            if (dtcs && dtcs->num > 0.0) { ev += " · " + fmt0(dtcs->num) + " ACTIVE DTC"; ok = false; }
            else if (dtcs) ev += " · no DTC";
            add("engine", "Engine pre-start", ev, ok ? READY_PASS : READY_FAIL);
        }
    }
    // 10 Electrical
    {
        const Sample* volt = num("electrical", "battery_voltage", 60.0);
        const Sample* soc = num("electrical", "soc", 120.0);
        if (!volt && !soc) add("electrical", "Electrical", "no electrical data", missing);
        else {
            bool ok = true;
            std::string ev;
            if (volt) {
                ev += fmt1(volt->num) + " V";
                // 12 V or 24 V system: low below 11.5 V, or 16..23 V
                const double v = volt->num;
                ok = ok && !(v < 11.5 || (v > 16.0 && v < 23.0));
            }
            if (soc) { ev += std::string(ev.empty() ? "" : " · ") + "SOC " + fmt0(soc->num) + " %"; ok = ok && soc->num >= 30.0; }
            add("electrical", "Electrical", ev, ok ? READY_PASS : READY_FAIL);
        }
    }
    // 11 Endurance: fuel and battery hours against the route time (with the
    // margin for the return leg). Needs the vessel constants and a route.
    {
        const Sample* len = num("mission", "route_length_m", 1e9);
        const Sample* fuel = num("engine", "fuel_level", 120.0);
        const Sample* soc = num("electrical", "soc", 120.0);
        const bool have_route = len && len->num > 0.0 && cfg.cruise_speed_kn > 0.0;
        // Tank size: fuel_tank_l in real.moos, else VESSEL_FUEL_L in usv.env.
        const double tank_l = cfg.fuel_tank_l > 0.0 ? cfg.fuel_tank_l : cfg.vessel_fuel_l;
        const bool fuel_cfg = tank_l > 0.0 && cfg.cruise_fuel_lph > 0.0;
        const bool batt_cfg = cfg.battery_kwh > 0.0 && cfg.cruise_power_kw > 0.0;
        // "fuel 94 % = 1,067 L of 1,135 L" whenever the tank size is known.
        auto litres = [&]() -> std::string {
            if (!fuel || tank_l <= 0.0) return "";
            return "fuel " + fmt0(fuel->num) + " % = " + fmt_thousands(fuel->num / 100.0 * tank_l) +
                   " L of " + fmt_thousands(tank_l) + " L";
        };
        if (!fuel_cfg && !batt_cfg) {
            const std::string l = litres();
            add("endurance", "Endurance",
                l.empty() ? "no vessel constants (fuel_tank_l / battery_kwh)"
                          : l + " · set cruise_fuel_lph for hours", READY_NA);
        } else if (!have_route)
            add("endurance", "Endurance",
                (litres().empty() ? "" : litres() + " · ") + "no route to plan against",
                strict ? READY_FAIL : READY_NA);
        else {
            const double need_h = (len->num / 1852.0) / cfg.cruise_speed_kn * cfg.endurance_margin;
            std::string ev = "route " + fmt1(len->num / 1852.0) + " nm needs " + fmt1(need_h) +
                             " h (x" + fmt1(cfg.endurance_margin) + ")";
            bool ok = true, any = false;
            if (fuel_cfg) {
                if (fuel) {
                    const double h = fuel->num / 100.0 * tank_l / cfg.cruise_fuel_lph;
                    ev += " · " + litres() + " = " + fmt1(h) + " h"; ok = ok && h >= need_h; any = true;
                } else ev += " · no fuel level";
            }
            if (batt_cfg) {
                if (soc) {
                    const double h = soc->num / 100.0 * cfg.battery_kwh / cfg.cruise_power_kw;
                    ev += " · battery " + fmt1(h) + " h"; ok = ok && h >= need_h; any = true;
                } else ev += " · no SOC";
            }
            add("endurance", "Endurance", ev, !any ? missing : (ok ? READY_PASS : READY_FAIL));
        }
    }
    // 12 Speed cap: the helm's SPEED_CAP against the hull's maximum
    // (VESSEL_MAX_SPEED_KN in usv.env, from the GA drawing). A cap above
    // the hull maximum means the helm may command a speed the boat cannot
    // make. N/A without a configured maximum or a published cap.
    {
        const Sample* cap = num("autopilot", "speed_cap", 120.0);
        if (cfg.vessel_max_speed_kn <= 0.0)
            add("speed_cap", "Speed cap", "no VESSEL_MAX_SPEED_KN in usv.env", READY_NA);
        else if (!cap)
            add("speed_cap", "Speed cap",
                "no SPEED_CAP published · hull max " + fmt0(cfg.vessel_max_speed_kn) + " kn", READY_NA);
        else {
            const std::string ev = "cap " + fmt1(cap->num) + " kn set · hull max " +
                                   fmt0(cfg.vessel_max_speed_kn) + " kn";
            if (cap->num <= 0.0) add("speed_cap", "Speed cap", ev + " · CAP IS ZERO", READY_FAIL);
            else add("speed_cap", "Speed cap", ev,
                     cap->num <= cfg.vessel_max_speed_kn ? READY_PASS : READY_FAIL);
        }
    }
    // 13 AIS / radar. An empty harbour gives zero reports, so a zero is
    // "nothing to verify", never a fault; an offline radar is.
    {
        const Sample* rep = num("traffic", "reports", 300.0);
        const Sample* on = num("radar", "online", 60.0);
        std::string ev = (rep && rep->num > 0.0) ? fmt0(rep->num) + " reported contacts"
                                                 : "no contact reports (5 min)";
        int st = (rep && rep->num > 0.0) ? READY_PASS : READY_NA;
        if (!on) { ev += " · no radar data"; if (strict) st = READY_FAIL; }
        else if (on->num == 1.0) { ev += " · radar online"; if (st == READY_NA) st = READY_PASS; }
        else { ev += " · RADAR OFFLINE"; st = READY_FAIL; }
        add("ais", "AIS / radar", ev, st);
    }
    // 14 Cameras: frigate_bridge's summary of the cameras ENABLED in the
    // Frigate NVR (configured-but-unpowered cameras are disabled there and
    // do not count); a single camera.connected flag as the fallback.
    {
        const Sample* fup = num("camera_summary", "frigate_up", 60.0);
        const Sample* en = num("camera_summary", "enabled", 60.0);
        const Sample* str = num("camera_summary", "streaming", 60.0);
        const Sample* con = num("camera", "connected", 60.0);
        if (fup && fup->num != 1.0)
            add("cameras", "Cameras", "Frigate NVR not reachable", READY_FAIL);
        else if (en && str) {
            if (en->num < 1.0)
                add("cameras", "Cameras", "no cameras enabled in Frigate", missing);
            else
                add("cameras", "Cameras",
                    fmt0(str->num) + " / " + fmt0(en->num) + " cameras streaming",
                    str->num >= en->num ? READY_PASS : READY_FAIL);
        } else if (!con) add("cameras", "Cameras", "no camera data", missing);
        else add("cameras", "Cameras", con->num == 1.0 ? "connected" : "DISCONNECTED",
                 con->num == 1.0 ? READY_PASS : READY_FAIL);
    }
    // 15 Onboard computer (Telegraf host metrics)
    {
        const Sample* cpu = num("cpu", "usage_user", 60.0);
        const Sample* mem = num("mem", "used_percent", 60.0);
        const Sample* dsk = num("disk", "used_percent", 120.0);
        if (!cpu && !mem) add("obc", "Onboard computer", "no host metrics", missing);
        else {
            bool ok = true;
            std::string ev;
            if (cpu) { ev += "CPU " + fmt0(cpu->num) + " %"; ok = ok && cpu->num < 90.0; }
            if (mem) { ev += std::string(ev.empty() ? "" : " · ") + "RAM " + fmt0(mem->num) + " %"; ok = ok && mem->num < 90.0; }
            if (dsk) { ev += " · disk " + fmt0(dsk->num) + " %"; ok = ok && dsk->num < 90.0; }
            add("obc", "Onboard computer", ev, ok ? READY_PASS : READY_FAIL);
        }
    }
    // 16 Safety flags
    {
        const Sample* geo = num("autonomy", "geofence_breached", 10.0);
        const Sample* risk = num("traffic", "risk_level", 10.0);
        const Sample* crash = num("autonomy", "crash_stop", 10.0);
        std::string ev;
        if (geo && geo->num != 0.0) ev += "GEOFENCE BREACH";
        if (risk && risk->num >= 2.0) ev += std::string(ev.empty() ? "" : " · ") + "COLLISION RISK HIGH";
        if (crash && crash->num != 0.0) ev += std::string(ev.empty() ? "" : " · ") + "CRASH STOP";
        // Operator ALL STOP latched (starts refused until reset) and the
        // operating region's own breach state.
        const Sample* astop = num("autonomy", "all_stop", 10.0);
        if (astop && astop->num != 0.0) ev += std::string(ev.empty() ? "" : " · ") + "ALL STOP LATCHED";
        const Sample* rst = num("mission_state", "region_state", 1e9);
        if (rst && rst->is_str && lower(rst->str).find("breach") != std::string::npos)
            ev += std::string(ev.empty() ? "" : " · ") + "REGION BREACHED";
        add("safety", "Safety flags",
            ev.empty() ? "no geofence breach, no HIGH collision risk, no crash stop, no ALL STOP" : ev,
            ev.empty() ? READY_PASS : READY_FAIL);
    }
    // 17 Console checklist (manual items ticked in Swiftships-UI)
    {
        const Sample* done = num("mission_state", "checklist_done", 1e9);
        const Sample* total = num("mission_state", "checklist_total", 1e9);
        const Sample* pend = num("mission_state", "checklist_pending", 1e9);
        if (!cfg.console_checklist) add("checklist", "Console checklist", "not published by this console", READY_NA);
        else if (!done || !total) add("checklist", "Console checklist", "console not publishing CHECKLIST_STATUS", READY_CONSOLE);
        else {
            const bool complete = done->num >= total->num;
            std::string ev = fmt0(done->num) + " / " + fmt0(total->num) + " ticked";
            if (!complete && pend && !pend->str.empty() && pend->str != "-") {
                std::string p = pend->str;
                for (char& c : p) if (c == '|') c = ',';
                ev += " · pending: " + p;
            }
            add("checklist", "Console checklist", ev, complete ? READY_PASS : READY_FAIL);
        }
    }
    return out;
}

inline ReadinessSummary SummarizeReadiness(const std::vector<ReadinessCheck>& checks) {
    ReadinessSummary r;
    for (const auto& c : checks) {
        switch (c.status) {
            case READY_PASS: ++r.passed; break;
            case READY_FAIL: ++r.failed; if (!r.blocking.empty()) r.blocking += "; "; r.blocking += c.name; break;
            case READY_NA: ++r.na; break;
            default: ++r.console; break;
        }
    }
    // READY needs every check decided: a CONSOLE item is unknown, not passed.
    r.ready = r.failed == 0 && r.console == 0;
    return r;
}

}  // namespace usv
