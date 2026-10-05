// mayara_bridge — mayara-server ARPA/MARPA targets -> MOOSDB RADAR_TARGET.
//
// Polls the mayara Signal K Radar API and posts every tracked target as the
// RADAR_TARGET string grafana_bridge already consumes, so radar targets show
// on the Traffic dashboard (Radar Only contacts, CPA/TCPA, risk, alerts):
//
//   RADAR_TARGET   "id=R3,range=1067.0,bearing=133.8,spd=5.70,cog=232.6,cpa=350.1,tcpa=138"
//   RADAR_TTM_RATE target updates per second (radar liveness on the dashboard)
//   MAYARA_BRIDGE_STATUS "radar=nav6959,power=transmit,targets=3,mayara=ok"
//
// DISPLAY ONLY: nothing here posts NODE_REPORT, so pContactMgrV20 / the
// helm never see these targets and collision avoidance is unchanged.
//
// Targets are posted only while the radar transmits (or when its power
// state cannot be read). A target mayara stops reporting simply stops being
// posted; grafana_bridge expires it after contact_ttl_s.
//
// Runs as a plain MOOS client (systemd: usv-mayara-bridge.service), not from
// the mission's Antler block. The async client reconnects by itself, so the
// service can start before or after the mission.
//
// Environment (/etc/usv/usv.env):
//   MAYARA_URL               http://localhost:3001  (empty = service disabled)
//   MAYARA_RADAR             radar id, e.g. nav6959 (default: first found)
//   MAYARA_POLL_MS           1000     (200..10000)
//   MAYARA_ID_PREFIX         R        contact id = prefix + mayara target id
//   MAYARA_INCLUDE_ACQUIRING 0        1 = also post targets still acquiring
//   MOOS_HOST / MOOS_PORT    localhost / 9000
//   MAYARA_MOOS_NAME         mayara_bridge (client name in the MOOSDB)

#include "MOOS/libMOOS/Comms/MOOSAsyncCommClient.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "../cpp-common/http_get.hpp"
#include "mayara_logic.hpp"

namespace {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop = true; }

std::string Env(const char* k, const std::string& def) {
    const char* v = std::getenv(k);
    return (v && *v) ? std::string(v) : def;
}

long EnvLong(const char* k, long def, long lo, long hi) {
    const char* v = std::getenv(k);
    if (!v || !*v) return def;
    char* end = nullptr;
    const long n = std::strtol(v, &end, 10);
    if (!end || *end != '\0') return def;
    return n < lo ? lo : (n > hi ? hi : n);
}

bool EnvBool(const char* k, bool def) {
    const std::string v = Env(k, "");
    if (v.empty()) return def;
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

__attribute__((format(printf, 1, 2))) void Log(const char* fmt, ...) {
    std::fprintf(stderr, "[mayara_bridge] ");
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
}

const char* PowerName(int p) {
    switch (p) {
        case usv::mayara::kPowerTransmit: return "transmit";
        case usv::mayara::kPowerStandby: return "standby";
        case usv::mayara::kPowerOff: return "off";
        default: return "unknown";
    }
}

// GET + parse. Returns false (with *status set) on HTTP or JSON failure.
bool GetJson(const std::string& url, json* out, long* status) {
    const std::string body = usv::HttpGet(url, 800, "", status);
    if (body.empty()) return false;
    *out = json::parse(body, nullptr, false);
    return !out->is_discarded();
}

}  // namespace

int main() {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    std::string base = Env("MAYARA_URL", "http://localhost:3001");
    while (!base.empty() && base.back() == '/') base.pop_back();
    const std::string api = base + "/signalk/v2/api/vessels/self/radars";
    const std::string want_radar = Env("MAYARA_RADAR", "");
    const long poll_ms = EnvLong("MAYARA_POLL_MS", 1000, 200, 10000);
    const std::string moos_host = Env("MOOS_HOST", "localhost");
    const int moos_port = static_cast<int>(EnvLong("MOOS_PORT", 9000, 1, 65535));
    const std::string moos_name = Env("MAYARA_MOOS_NAME", "mayara_bridge");

    usv::mayara::Options opts;
    opts.id_prefix = Env("MAYARA_ID_PREFIX", "R");
    opts.include_acquiring = EnvBool("MAYARA_INCLUDE_ACQUIRING", false);

    std::fprintf(stderr,
                 "[mayara_bridge] mayara=%s radar=%s poll=%ldms moos=%s:%d prefix=%s acquiring=%d\n",
                 base.c_str(), want_radar.empty() ? "(first found)" : want_radar.c_str(),
                 poll_ms, moos_host.c_str(), moos_port, opts.id_prefix.c_str(),
                 opts.include_acquiring ? 1 : 0);

    curl_global_init(CURL_GLOBAL_DEFAULT);

    MOOS::MOOSAsyncCommClient comms;
    comms.SetQuiet(true);
    comms.Run(moos_host, moos_port, moos_name);

    std::string radar;                 // radar in use
    auto next_discovery = Clock::now();
    bool was_reachable = true, was_moos = false, warned_tracking = false;
    int last_power = -2;
    size_t last_count = static_cast<size_t>(-1);

    while (!g_stop) {
        const auto t0 = Clock::now();
        bool reachable = true;
        int power = usv::mayara::kPowerUnknown;
        std::vector<std::string> specs;

        // 1. Find the radar (every 5 s until one is found).
        if (radar.empty() && t0 >= next_discovery) {
            next_discovery = t0 + std::chrono::seconds(5);
            json list;
            long st = 0;
            if (GetJson(api, &list, &st)) {
                const auto ids = usv::mayara::RadarIds(list);
                for (const auto& id : ids)
                    if (want_radar.empty() || id == want_radar) { radar = id; break; }
                if (!radar.empty())
                    Log("using radar %s", radar.c_str());
                else if (!want_radar.empty() && !ids.empty())
                    Log("radar %s not found (mayara reports %s)", want_radar.c_str(), ids.front().c_str());
            } else {
                reachable = false;
            }
        }

        // 2. Power state, then targets while transmitting.
        if (!radar.empty()) {
            json pj;
            long st = 0;
            if (GetJson(api + "/" + radar + "/controls/power", &pj, &st)) {
                power = usv::mayara::PowerState(pj);
            } else if (st == 404) {
                Log("radar %s gone, rediscovering", radar.c_str());
                radar.clear();
                next_discovery = t0;
            } else if (st <= 0) {
                reachable = false;
            }
        }
        const bool radiating = power == usv::mayara::kPowerTransmit ||
                               power == usv::mayara::kPowerUnknown;
        if (!radar.empty() && reachable && radiating) {
            json tj;
            long st = 0;
            if (GetJson(api + "/" + radar + "/targets", &tj, &st)) {
                specs = usv::mayara::TargetsToSpecs(tj, opts);
                warned_tracking = false;
            } else if (st == 400 && !warned_tracking) {
                warned_tracking = true;
                Log("mayara target tracking is off: remove --targets none/trails from mayara.service");
            } else if (st == 404) {
                radar.clear();
                next_discovery = t0;
            } else if (st <= 0) {
                reachable = false;
            }
        }

        // 3. Post.
        const bool moos = comms.IsConnected();
        if (moos) {
            if (reachable && !radar.empty() && radiating) {
                for (const auto& s : specs) comms.Notify("RADAR_TARGET", s);
                comms.Notify("RADAR_TTM_RATE", specs.size() * 1000.0 / poll_ms);
            }
            char status[160];
            std::snprintf(status, sizeof(status), "radar=%s,power=%s,targets=%zu,mayara=%s",
                          radar.empty() ? "none" : radar.c_str(), PowerName(power),
                          specs.size(), reachable ? "ok" : "unreachable");
            comms.Notify("MAYARA_BRIDGE_STATUS", std::string(status));
        }

        // 4. Log state changes only.
        if (moos != was_moos) {
            std::fprintf(stderr, "[mayara_bridge] MOOSDB %s:%d %s\n", moos_host.c_str(),
                         moos_port, moos ? "connected" : "not connected (retrying)");
            was_moos = moos;
        }
        if (reachable != was_reachable) {
            Log(reachable ? "mayara reachable at %s" : "mayara unreachable at %s (retrying)", base.c_str());
            was_reachable = reachable;
        }
        if (power != last_power && !radar.empty()) {
            Log("radar %s power: %s", radar.c_str(), PowerName(power));
            last_power = power;
        }
        if (specs.size() != last_count) {
            std::fprintf(stderr, "[mayara_bridge] tracking %zu target(s)\n", specs.size());
            last_count = specs.size();
        }

        std::this_thread::sleep_until(t0 + std::chrono::milliseconds(poll_ms));
    }

    Log("stopping");
    comms.Close(true);
    curl_global_cleanup();
    return 0;
}
