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
#include <cstdio>
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
    if (f.returning)                             return "ReturnToBase";
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
    std::vector<std::string> required_apps;   // MOOS apps that must be connected
    std::vector<std::string> optional_apps;   // reported when absent, never a fault (GUI viewers)
    std::vector<ChecklistItem> checklist_items;
    // Console-fed rows. A vessel without an ENC chart system, or whose
    // console does not publish the pre-departure checklist, reads N/A on
    // those rows instead of waiting for the console (CONSOLE blocks READY).
    bool enc_fitted = true;
    bool console_checklist = true;
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
    // 2 Course planning: route, waypoints, operating region
    {
        // Last known (also read back from InfluxDB after a community
        // restart, like the Route tile); flagged when older than 10 min.
        const Sample* route = num("mission_state", "route_name", 1e9);
        const Sample* wpts = num("mission", "waypoints_total", 1e9);
        const Sample* region = num("mission", "region_vertices", 1e9);
        const bool have_route = route && !route->str.empty() && route->str != "-";
        const bool have_wpts = wpts && wpts->num > 0.0;
        const bool have_region = region && region->num >= 3.0;
        const Sample* inside = num("mission", "route_in_region", 1e9);
        const Sample* len = num("mission", "route_length_m", 1e9);
        std::string ev = have_route ? route->str : "no route loaded";
        if (have_route && now - route->t > 600.0) ev += " (last known)";
        ev += have_wpts ? " · " + fmt0(wpts->num) + " waypoints" : " · no waypoints";
        if (len && len->num > 0.0) ev += " · " + fmt1(len->num / 1852.0) + " nm";
        ev += have_region ? " · region " + fmt0(region->num) + " vertices" : " · no operating region";
        bool ok = have_route && have_wpts && have_region;
        if (ok && inside) {
            if (inside->num == 1.0) ev += " · route inside region";
            else { ev += " · ROUTE OUTSIDE REGION"; ok = false; }
        }
        add("course", "Course planning", ev, ok ? READY_PASS : READY_FAIL);
    }
    // 3 Chart / ENC selected (console publishes ENC_STATUS)
    {
        const Sample* enc = num("mission_state", "enc_status", 1e9);
        if (!cfg.enc_fitted) add("enc", "Chart / ENC selected", "no ENC chart system on this vessel", READY_NA);
        else if (!enc) add("enc", "Chart / ENC selected", "console not publishing ENC_STATUS", READY_CONSOLE);
        else add("enc", "Chart / ENC selected", enc->str,
                 enc->str.rfind("selected", 0) == 0 ? READY_PASS : READY_FAIL);
    }
    // 4 Autonomy stack
    {
        const Sample* rate = num("comms", "moos_msg_rate", 20.0);
        const Sample* helm = num("mission_state", "helm_state", 1e9);
        const Sample* allstop = num("autonomy", "allstop", 10.0);
        std::string ev;
        bool ok = rate && rate->num >= 1.0;
        ev = rate ? "MOOS " + fmt0(rate->num) + " msg/s" : "no MOOS traffic";
        if (helm) { ev += " · helm " + helm->str; ok = ok && helm->str != "MALCONFIG"; }
        else { ev += " · helm state unknown"; ok = false; }
        if (allstop && allstop->num != 0.0) { ev += " · ALL-STOP"; ok = false; }
        add("autonomy", "Autonomy stack", ev, ok ? READY_PASS : READY_FAIL);
    }
    // 5 MOOS community complete: every app the Antler block starts (or
    // required_apps) is connected to MOOSDB (DB_CLIENTS).
    {
        const Sample* clients = num("moos", "clients", 30.0);
        if (cfg.required_apps.empty())
            add("apps", "MOOS apps", "no app list (Antler block / required_apps)", READY_NA);
        else if (!clients)
            add("apps", "MOOS apps", "DB_CLIENTS not received", READY_FAIL);
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
            add("apps", "MOOS apps", ev, missing.empty() ? READY_PASS : READY_FAIL);
        }
    }
    // 6 Steering / actuators: rudder feedback (ANGULO_LEME) and command
    {
        const Sample* act = num("autopilot", "rudder_actual", 15.0);
        const Sample* cmd = num("autopilot", "rudder_cmd", 15.0);
        if (!act && !cmd) add("steering", "Steering / actuators", "no rudder feedback or command", missing);
        else {
            bool ok = true;
            std::string ev;
            if (act) { ev += "rudder " + fmt1(act->num) + "°"; ok = ok && std::fabs(act->num) <= 35.0; }
            if (cmd) { ev += std::string(ev.empty() ? "" : " · ") + "cmd " + fmt1(cmd->num) + "°"; ok = ok && std::fabs(cmd->num) <= 35.0; }
            if (act && cmd && std::fabs(act->num - cmd->num) > 10.0) { ev += " · NOT FOLLOWING"; ok = false; }
            if (!act) { ev += " · no rudder feedback"; if (strict) ok = false; }
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
        if (!up) add("can", "CAN bus", "no CAN bridge data", missing);
        else if (up->num == 1.0)
            add("can", "CAN bus", "interface UP" + (rate ? " · " + fmt0(rate->num) + " msg/s" : std::string()),
                READY_PASS);
        else   // simulation host: can_bridge runs with no CAN hardware
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
        const bool fuel_cfg = cfg.fuel_tank_l > 0.0 && cfg.cruise_fuel_lph > 0.0;
        const bool batt_cfg = cfg.battery_kwh > 0.0 && cfg.cruise_power_kw > 0.0;
        if (!fuel_cfg && !batt_cfg)
            add("endurance", "Endurance", "no vessel constants (fuel_tank_l / battery_kwh)", READY_NA);
        else if (!have_route)
            add("endurance", "Endurance", "no route to plan against", strict ? READY_FAIL : READY_NA);
        else {
            const double need_h = (len->num / 1852.0) / cfg.cruise_speed_kn * cfg.endurance_margin;
            std::string ev = "route " + fmt1(len->num / 1852.0) + " nm needs " + fmt1(need_h) +
                             " h (x" + fmt1(cfg.endurance_margin) + ")";
            bool ok = true, any = false;
            if (fuel_cfg) {
                if (fuel) {
                    const double h = fuel->num / 100.0 * cfg.fuel_tank_l / cfg.cruise_fuel_lph;
                    ev += " · fuel " + fmt1(h) + " h"; ok = ok && h >= need_h; any = true;
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
    // 12 AIS / radar. An empty harbour gives zero reports, so a zero is
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
    // 13 Cameras: frigate_bridge's summary of the cameras ENABLED in the
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
    // 14 Onboard computer (Telegraf host metrics)
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
    // 15 Safety flags
    {
        const Sample* geo = num("autonomy", "geofence_breached", 10.0);
        const Sample* risk = num("traffic", "risk_level", 10.0);
        const Sample* crash = num("autonomy", "crash_stop", 10.0);
        std::string ev;
        if (geo && geo->num != 0.0) ev += "GEOFENCE BREACH";
        if (risk && risk->num >= 2.0) ev += std::string(ev.empty() ? "" : " · ") + "COLLISION RISK HIGH";
        if (crash && crash->num != 0.0) ev += std::string(ev.empty() ? "" : " · ") + "CRASH STOP";
        add("safety", "Safety flags",
            ev.empty() ? "no geofence breach, no HIGH collision risk, no crash stop" : ev,
            ev.empty() ? READY_PASS : READY_FAIL);
    }
    // 16 Console checklist (manual items ticked in Swiftships-UI)
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
