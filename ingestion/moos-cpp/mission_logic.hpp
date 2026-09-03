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
#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
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
    std::string comms_lost = "false";  // COMMS_LOST
    std::string helm_state;            // IVPHELM_STATE (informational)
    std::string allstop = "clear";     // IVPHELM_ALLSTOP (informational)
};

// Operational mission state, priority-ordered (first match wins). Remote
// (operator driving through REMOTE_CONTROL passthrough) outranks the helm
// flags it sets underneath (manual override / park). Helm
// health is intentionally NOT folded in: a frozen helm while Paused must
// show as Paused + an all-stop alert, not mask one with the other.
inline const char* DeriveMissionState(const MissionFlags& f) {
    if (comms_lost_active(f.comms_lost))         return "CommsLost";
    if (f.remote_control)                        return "Remote";
    if (f.manual_override && f.survey_complete)  return "Completed";
    if (f.manual_override)                       return "Parked";
    if (f.pause)                                 return "Paused";
    if (f.stationing)                            return "StationKeeping";
    if (f.returning)                             return "ReturnToHome";
    if (f.waypoint && !gate_open(f.entry_gate))  return "EnteringRegion";
    if (f.waypoint && !f.armed)                  return "AwaitingRearm";
    if (f.waypoint)                              return "Autonomous";
    if (f.survey_complete)                       return "Completed";
    return "Idle";
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
    double tcpa_s = -1;
    double last_seen = -1e18;
};

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
    // Time of the most recent post of any contact.
    double last_seen() const {
        double t = -1e18;
        for (const auto& [id, c] : contacts_) t = std::max(t, c.last_seen);
        return t;
    }

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

}  // namespace usv
