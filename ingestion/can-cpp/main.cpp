// USV CAN / NMEA 2000 -> InfluxDB bridge (FR-5, parts of FR-2/FR-4)
//
// Reads raw frames from a SocketCAN interface (e.g. can0 via canable/PICAN),
// decodes a core set of single-frame NMEA 2000 PGNs, and tracks bus health:
// utilization, msgs/sec, error frames, per-PGN frequency.
//
// Writes InfluxDB 2.x line protocol over HTTP using libcurl.
//
// Build: cmake -B build && cmake --build build
// Also decodes the usv-722 helm / drive frames (steering actuator 0x210,
// Glendinning helm, J1939 thrust / gear, key switch, DM1): usv722_can.hpp;
// the usv-725 ones: usv725_can.hpp; and, when ENGINE_SOURCE=can, the J1939
// engine ECU itself (rpm, temperatures, pressures, faults): j1939_engine.hpp.
//
// Env:   CAN_INTERFACE, INFLUX_URL, INFLUX_TOKEN, INFLUX_ORG, INFLUX_BUCKET, VESSEL_ID
//        STEER_MAX_DEG (50), STEER_RAW_PORT (0), STEER_RAW_CENTER (511),
//        STEER_RAW_STBD (1012): steering calibration of the drive (defaults
//        for usv-722; HELM_PROFILE=usv725 defaults to 17, 10, 511, 1022)
//        ENGINE_SOURCE / ENGINE_TYPE / HELM_PROFILE (cpp-common/engine_profile.hpp):
//        N2K engine PGNs are decoded only when ENGINE_SOURCE=can, the J1939
//        DM1 faults unless ENGINE_SOURCE=moos, the helm / drive frames when
//        HELM_PROFILE=usv722 (default) or usv725; with ENGINE_SOURCE=can the
//        J1939 engine ECU at J1939_ENGINE_SA (0) is decoded and
//        CAN_ENGINE_HOURS_REQUEST=1 asks it for engine hours every 5 s
//        (from J1939_REQUEST_SA, 249). Engine points carry instance and
//        engine_type tags. Bus statistics are always written.
//        CAN_REPLAY=<candump -l log> decodes a recorded log and exits.

#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <string>

#include "../cpp-common/engine_profile.hpp"
#include "../cpp-common/influx_writer.hpp"
#include "j1939_engine.hpp"
#include "usv722_can.hpp"
#include "usv725_can.hpp"

using usv::InfluxWriter;
using usv::env_or;

namespace {

// Tags for the N2K / J1939 engine points this bridge writes (instance from
// the frame, engine_type from the vessel profile).
const usv::EngineProfile& profile() {
    static const usv::EngineProfile p = usv::EngineProfile::FromEnv();
    return p;
}

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

double now_s() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------------------
// NMEA 2000 helpers
// ---------------------------------------------------------------------------
uint32_t pgn_from_id(uint32_t can_id) {
    // 29-bit extended id: prio(3) | reserved(1) | DP(1) | PF(8) | PS(8) | SA(8)
    uint32_t pf = (can_id >> 16) & 0xFF;
    uint32_t ps = (can_id >> 8) & 0xFF;
    uint32_t dp = (can_id >> 24) & 0x01;
    if (pf < 240) return (dp << 16) | (pf << 8);        // PDU1: PS is address
    return (dp << 16) | (pf << 8) | ps;                  // PDU2: PS is group ext
}

uint16_t u16(const uint8_t* d) { return static_cast<uint16_t>(d[0] | (d[1] << 8)); }
int16_t s16(const uint8_t* d) { return static_cast<int16_t>(u16(d)); }
uint32_t u32(const uint8_t* d) {
    return static_cast<uint32_t>(d[0]) | (static_cast<uint32_t>(d[1]) << 8) |
           (static_cast<uint32_t>(d[2]) << 16) | (static_cast<uint32_t>(d[3]) << 24);
}
int32_t s32(const uint8_t* d) { return static_cast<int32_t>(u32(d)); }

constexpr double RAD = 57.29577951308232;

// Decode a core set of single-frame PGNs.
// NOTE: fast-packet (multi-frame) PGNs such as 129029 GNSS Position Data are
// counted in bus statistics but not decoded here — position is also available
// via PGN 129025 (single frame) and via the Signal K bridge.
void decode_pgn(InfluxWriter& w, uint32_t pgn, const uint8_t* d, uint8_t len) {
    switch (pgn) {
        case 127488: {  // Engine Parameters, Rapid Update
            if (len < 8 || !profile().can_n2k_engine) break;
            uint8_t instance = d[0];
            uint16_t rpm_raw = u16(d + 1);
            if (rpm_raw != 0xFFFF)
                w.field("engine", profile().Tags("instance=" + std::to_string(instance)),
                        "rpm", rpm_raw * 0.25);
            break;
        }
        case 127508: {  // Battery Status
            if (len < 8) break;
            uint8_t instance = d[0];
            std::string tag = "instance=" + std::to_string(instance);
            uint16_t v_raw = u16(d + 1);
            int16_t i_raw = s16(d + 3);
            if (v_raw != 0xFFFF) w.field("electrical", tag, "battery_voltage", v_raw * 0.01);
            if (i_raw != 0x7FFF) w.field("electrical", tag, "battery_current", i_raw * 0.1);
            break;
        }
        case 129025: {  // Position, Rapid Update
            if (len < 8) break;
            int32_t lat = s32(d), lon = s32(d + 4);
            if (lat != 0x7FFFFFFF && lon != 0x7FFFFFFF) {
                w.field("nav", "", "lat", lat * 1e-7);
                w.field("nav", "", "lon", lon * 1e-7);
            }
            break;
        }
        case 129026: {  // COG & SOG, Rapid Update
            if (len < 8) break;
            uint16_t cog_raw = u16(d + 2), sog_raw = u16(d + 4);
            if (cog_raw != 0xFFFF) w.field("nav", "", "cog", cog_raw * 0.0001 * RAD);
            if (sog_raw != 0xFFFF) w.field("nav", "", "sog", sog_raw * 0.01 * 1.94384);
            break;
        }
        case 127245: {  // Rudder
            if (len < 8) break;
            int16_t pos_raw = s16(d + 4);
            if (pos_raw != 0x7FFF)
                w.field("autopilot", "", "rudder_actual", pos_raw * 0.0001 * RAD);
            break;
        }
        case 127250: {  // Vessel Heading
            if (len < 8) break;
            uint16_t hdg_raw = u16(d + 1);
            if (hdg_raw != 0xFFFF) w.field("nav", "", "heading", hdg_raw * 0.0001 * RAD);
            break;
        }
        case 127505: {  // Fluid Level (instance 0 = fuel by convention)
            if (len < 8 || !profile().can_n2k_engine) break;
            // B0: low nibble instance, high nibble fluid type (0 = fuel).
            const uint8_t instance = d[0] & 0x0F, type = d[0] >> 4;
            int16_t level_raw = s16(d + 1);
            if (type == 0 && instance == 0 && level_raw != 0x7FFF)
                w.field("engine", profile().Tags("instance=0"), "fuel_level",
                        level_raw * 0.004);
            break;
        }
        default:
            break;
    }
}

// usv-722 helm / drive values arrive up to ~18 times a second. Each one is
// written when it changes or at least every 0.5 s, so a steady value still
// shows as live without flooding InfluxDB.
class ThrottledWriter {
public:
    void put(InfluxWriter& w, const usv::usv722::Value& v, double t) {
        const std::string key = std::string(v.measurement) + "." + v.field;
        auto it = last_.find(key);
        if (it != last_.end() && it->second.value == v.value && t - it->second.t < 0.5) return;
        if (it != last_.end() && t - it->second.t < 0.1) return;   // at most 10 Hz
        last_[key] = {v.value, t};
        w.field(v.measurement, EngineTags(v.measurement), v.field, v.value);
    }

    // The drive-bus values are the single engine's: engine points carry the
    // instance and engine_type tags, the autopilot (rudder) ones none.
    static std::string EngineTags(const std::string& measurement) {
        return measurement == "engine" ? profile().Tags("instance=0") : std::string();
    }
private:
    struct Last { double value; double t; };
    std::map<std::string, Last> last_;
};

double env_num(const char* name, double def) {
    try { return std::stod(env_or(name, "")); } catch (...) { return def; }
}

}  // namespace

int main() {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    const std::string ifname = env_or("CAN_INTERFACE", "can0");
    InfluxWriter writer;
    const usv::EngineProfile& prof = profile();
    // Which decoders run on this vessel (bus statistics always do).
    const bool helm_decode = prof.helm_profile == "usv722";
    const bool helm725 = prof.helm_profile == "usv725";
    if (prof.helm_profile != "usv722" && prof.helm_profile != "usv725" &&
        prof.helm_profile != "none")
        std::cerr << "HELM_PROFILE=" << prof.helm_profile
                  << " has no decoder in this build; helm / drive tiles will read N/A\n";
    std::cerr << "can_bridge: " << prof.Describe()
              << " helm_decode=" << (helm_decode ? "usv722" : (helm725 ? "usv725" : "0")) << "\n";
    usv::usv722::SteerCal steer_cal =
        helm725 ? usv::usv725::DefaultCal() : usv::usv722::SteerCal();
    steer_cal.port = env_num("STEER_RAW_PORT", steer_cal.port);
    steer_cal.centre = env_num("STEER_RAW_CENTER", steer_cal.centre);
    steer_cal.stbd = env_num("STEER_RAW_STBD", steer_cal.stbd);
    steer_cal.max_deg = env_num("STEER_MAX_DEG", steer_cal.max_deg);
    // J1939 engine ECU read from the bus (ENGINE_SOURCE=can): engine values
    // go through the same rate limit as the helm values; its DM1 text is
    // written when it changes and at least every 5 s.
    usv::j1939engine::Decoder engine(static_cast<uint8_t>(prof.j1939_engine_sa));
    const bool engine_decode = prof.can_j1939_engine;
    std::string eng_dm1_last_text;
    double eng_dm1_last_t = -1e9;
    const bool hours_request = engine_decode && env_or("CAN_ENGINE_HOURS_REQUEST", "0") == "1";
    const uint8_t request_sa =
        static_cast<uint8_t>(env_num("J1939_REQUEST_SA", 249.0));
    double last_hours_request = -1e9;
    // Bus signature check (log only, no data): each boat has frames the
    // other never sends. usv-725: 0x210 command frames (byte 5 = 00);
    // usv-722: the control processor at 226 (00F003E2, 00F005E2). Hearing
    // the other boat's frames means HELM_PROFILE does not match this bus,
    // and the rudder, gear and trim tiles would be decoded with the wrong
    // scale or not at all.
    bool profile_warned = false;
    ThrottledWriter helm_writer;
    // DM1 fault details from the Glendinning device (210): the summary text
    // is written when it changes and at least every 5 s.
    usv::usv722::Dm1Receiver dm1_rx;
    std::string dm1_last_text;
    double dm1_last_t = -1e9;

    // Stats state
    uint64_t frames = 0, error_frames = 0, dropped = 0, bits = 0;
    std::map<uint32_t, uint64_t> pgn_counts;
    double last_stats = now_s();

    // One received frame (the per-frame code of the read loop).
    auto on_frame = [&](const struct can_frame& frame, double t) {
        if (frame.can_id & CAN_ERR_FLAG) {
            ++error_frames;
        } else {
            ++frames;
            // 29-bit extended frame ≈ 64 + 8*dlc bits with stuffing avg
            bits += 64 + 8ULL * frame.can_dlc;
            const bool ext = frame.can_id & CAN_EFF_FLAG;
            const uint32_t id = frame.can_id & (ext ? CAN_EFF_MASK : CAN_SFF_MASK);
            if (ext) {
                uint32_t pgn = pgn_from_id(id);
                ++pgn_counts[pgn];
                decode_pgn(writer, pgn, frame.data, frame.can_dlc);
            }
            // J1939 DM1 from the drive bus: single frame 0CFECAD2 or the
            // BAM transport 1CECFFD2 / 1CEBFFD2. Skipped when the engine
            // reports its faults through MOOS (ENGINE_SOURCE=moos).
            const bool is_dm1 = ext && (id == 0x0CFECAD2 || id == 0x1CECFFD2 ||
                                        id == 0x1CEBFFD2);
            const bool want = is_dm1 ? prof.can_dm1 : helm_decode;
            if (want)
                for (const auto& v : usv::usv722::Decode(id, ext, frame.data,
                                                         frame.can_dlc, steer_cal))
                    helm_writer.put(writer, v, t);
            usv::usv722::Dm1 dm1;
            bool dm1_multi = false;
            if (is_dm1 && prof.can_dm1 &&
                dm1_rx.Feed(id, frame.data, frame.can_dlc, &dm1, &dm1_multi)) {
                // Single-frame values already came from Decode() above.
                if (dm1_multi)
                    for (const auto& v : usv::usv722::Dm1Values(dm1))
                        helm_writer.put(writer, v, t);
                const std::string text =
                    usv::usv722::Dm1Text(dm1, prof.type == "mercury_diesel");
                if (text != dm1_last_text || t - dm1_last_t >= 5.0) {
                    const auto& k = usv::usv722::kDm1TextField;
                    writer.str_field(k.measurement, prof.Tags("instance=0"),
                                     k.field, text);
                    dm1_last_text = text;
                    dm1_last_t = t;
                }
            }
            if (!profile_warned) {
                const bool bus725 = !ext && id == 0x210 && frame.can_dlc >= 7 && frame.data[5] == 0x00;
                const bool bus722 = ext && (id == 0x00F003E2 || id == 0x00F005E2);
                if ((bus725 && prof.helm_profile != "usv725") || (bus722 && prof.helm_profile != "usv722")) {
                    std::cerr << "WARNING: this CAN bus carries "
                              << (bus725 ? "usv-725 frames (0x210 with byte 5 = 00)"
                                         : "usv-722 frames (control processor 226)")
                              << " but HELM_PROFILE=" << prof.helm_profile << ": set HELM_PROFILE="
                              << (bus725 ? "usv725" : "usv722")
                              << " in /etc/usv/usv.env, else rudder, gear and trim read wrong\n";
                    profile_warned = true;
                }
            }
            // usv-725 helm / drive frames (HELM_PROFILE=usv725).
            if (helm725)
                for (const auto& v : usv::usv725::Decode(id, ext, frame.data,
                                                         frame.can_dlc, steer_cal))
                    helm_writer.put(writer, v, t);
            // J1939 engine ECU (ENGINE_SOURCE=can).
            if (engine_decode) {
                usv::usv722::Dm1 edm;
                bool got = false;
                for (const auto& v : engine.Feed(id, ext, frame.data, frame.can_dlc, &edm, &got))
                    helm_writer.put(writer, v, t);
                if (got) {
                    for (const auto& v : usv::usv722::Dm1Values(edm))
                        helm_writer.put(writer, v, t);
                    const std::string text =
                        usv::usv722::Dm1Text(edm, prof.type == "mercury_diesel");
                    if (text != eng_dm1_last_text || t - eng_dm1_last_t >= 5.0) {
                        const auto& k = usv::usv722::kDm1TextField;
                        writer.str_field(k.measurement, prof.Tags("instance=0"),
                                         k.field, text);
                        eng_dm1_last_text = text;
                        eng_dm1_last_t = t;
                    }
                }
            }
        }
    };

    // Bus statistics every 5 s.
    auto stats = [&](double t) {
        if (t - last_stats >= 5.0) {
            double dt = t - last_stats;
            double msg_rate = frames / dt;
            // Utilization vs 250 kbit/s (NMEA 2000 standard bitrate)
            double util = 100.0 * (bits / dt) / 250000.0;
            writer.field("canbus", "", "msg_rate", msg_rate);
            writer.field("canbus", "", "utilization_pct", util);
            writer.field("canbus", "", "error_frames", static_cast<double>(error_frames));
            writer.field("canbus", "", "dropped_frames", static_cast<double>(dropped));
            writer.field("canbus", "", "active_pgns", static_cast<double>(pgn_counts.size()));
            writer.field("canbus", "", "interface_up", 1);
            for (const auto& [pgn, count] : pgn_counts)
                writer.field("canbus_pgn", "pgn=" + std::to_string(pgn),
                             "rate", count / dt);
            frames = 0; bits = 0; pgn_counts.clear();
            last_stats = t;
            writer.flush();
        }
    };

    // Replay mode: CAN_REPLAY=<candump -l log> feeds the frames of a recorded
    // log through the same decoders (log timestamps drive the rate limits and
    // the statistics), writes the result and exits. Used by the smoke test to
    // compare decoder output between releases.
    if (const char* replay_file = std::getenv("CAN_REPLAY")) {
        std::ifstream in(replay_file);
        if (!in) { std::cerr << "CAN_REPLAY: cannot open " << replay_file << "\n"; return 1; }
        std::string line;
        bool first = true;
        while (std::getline(in, line)) {
            double ts = 0;
            char iface[32] = {0}, idtxt[16] = {0}, datatxt[40] = {0};
            if (line.empty() || line[0] != '(' ||
                std::sscanf(line.c_str(), "(%lf) %31s %15[0-9A-Fa-f]#%39[0-9A-Fa-f]",
                            &ts, iface, idtxt, datatxt) < 3) continue;
            struct can_frame frame {};
            const unsigned long rawid = std::strtoul(idtxt, nullptr, 16);
            frame.can_id = static_cast<canid_t>(rawid);
            if (std::strlen(idtxt) > 3) frame.can_id |= CAN_EFF_FLAG;
            const size_t nbytes = std::strlen(datatxt) / 2;
            frame.can_dlc = static_cast<uint8_t>(nbytes > 8 ? 8 : nbytes);
            for (size_t i = 0; i < frame.can_dlc; ++i) {
                char b[3] = {datatxt[2 * i], datatxt[2 * i + 1], 0};
                frame.data[i] = static_cast<uint8_t>(std::strtoul(b, nullptr, 16));
            }
            if (first) { last_stats = ts; first = false; }
            on_frame(frame, ts);
            stats(ts);
        }
        writer.flush();
        return 0;
    }

    while (!g_stop) {
        int sock = socket(PF_CAN, SOCK_RAW, CAN_RAW);
        if (sock < 0) {
            // Common causes: missing CAP_NET_RAW (see the systemd unit), or a
            // kernel without CAN support (`modprobe can can_raw vcan`).
            std::cerr << "socket(PF_CAN) failed: " << std::strerror(errno)
                      << " — retry in 5s\n";
            writer.field("canbus", "", "interface_up", 0);
            writer.flush();
            sleep(5);
            continue;
        }

        can_err_mask_t err_mask = CAN_ERR_MASK;
        setsockopt(sock, SOL_CAN_RAW, CAN_RAW_ERR_FILTER, &err_mask, sizeof(err_mask));

        struct ifreq ifr {};
        std::strncpy(ifr.ifr_name, ifname.c_str(), IFNAMSIZ - 1);
        if (ioctl(sock, SIOCGIFINDEX, &ifr) < 0) {
            std::cerr << "interface " << ifname << " not found ("
                      << std::strerror(errno) << ") — is it up? "
                      << "`ip link set " << ifname
                      << " up type can bitrate 250000` — retry in 5s\n";
            writer.field("canbus", "", "interface_up", 0);
            writer.flush();
            close(sock);
            sleep(5);
            continue;
        }
        struct sockaddr_can addr {};
        addr.can_family = AF_CAN;
        addr.can_ifindex = ifr.ifr_ifindex;
        if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            std::cerr << "bind(" << ifname << ") failed: "
                      << std::strerror(errno) << " — retry in 5s\n";
            writer.field("canbus", "", "interface_up", 0);
            writer.flush();
            close(sock); sleep(5); continue;
        }
        std::cerr << "listening on " << ifname << "\n";
        writer.field("canbus", "", "interface_up", 1);

        struct timeval tv {1, 0};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        while (!g_stop) {
            struct can_frame frame {};
            ssize_t n = read(sock, &frame, sizeof(frame));
            double t = now_s();

            if (n == sizeof(frame)) {
                on_frame(frame, t);
            } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                std::cerr << "read error: " << std::strerror(errno) << "\n";
                break;  // reopen socket
            }

            stats(t);
            if (hours_request && t - last_hours_request >= 5.0) {
                struct can_frame req {};
                req.can_id = engine.HoursRequestId(request_sa) | CAN_EFF_FLAG;
                req.can_dlc = 3;
                std::memcpy(req.data, usv::j1939engine::Decoder::kHoursRequest, 3);
                if (write(sock, &req, sizeof(req)) != sizeof(req) && last_hours_request < 0)
                    std::cerr << "engine hours request not sent: " << std::strerror(errno) << "\n";
                last_hours_request = t;
            }
        }
        close(sock);
    }
    writer.flush();
    return 0;
}
