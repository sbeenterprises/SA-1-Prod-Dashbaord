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
// Glendinning helm, J1939 thrust / gear, key switch, DM1): usv722_can.hpp.
//
// Env:   CAN_INTERFACE, INFLUX_URL, INFLUX_TOKEN, INFLUX_ORG, INFLUX_BUCKET, VESSEL_ID
//        STEER_MAX_DEG (50), STEER_RAW_PORT (0), STEER_RAW_CENTER (511),
//        STEER_RAW_STBD (1012): steering calibration of the usv-722 drive
//        ENGINE_SOURCE / ENGINE_TYPE / HELM_PROFILE (cpp-common/engine_profile.hpp):
//        N2K engine PGNs are decoded only when ENGINE_SOURCE=can, the J1939
//        DM1 faults unless ENGINE_SOURCE=moos, the helm / drive frames when
//        HELM_PROFILE=usv722 (default); engine points carry instance and
//        engine_type tags. Bus statistics are always written.

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
#include <cstring>
#include <iostream>
#include <map>
#include <string>

#include "../cpp-common/engine_profile.hpp"
#include "../cpp-common/influx_writer.hpp"
#include "usv722_can.hpp"

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
            uint8_t type = d[0] & 0x0F;
            int16_t level_raw = s16(d + 1);
            if (type == 0 && level_raw != 0x7FFF)
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
    if (prof.helm_profile != "usv722" && prof.helm_profile != "none")
        std::cerr << "HELM_PROFILE=" << prof.helm_profile
                  << " has no decoder in this build; helm / drive tiles will read N/A\n";
    std::cerr << "can_bridge: " << prof.Describe()
              << " helm_decode=" << (helm_decode ? "1" : "0") << "\n";
    usv::usv722::SteerCal steer_cal;
    steer_cal.port = env_num("STEER_RAW_PORT", 0.0);
    steer_cal.centre = env_num("STEER_RAW_CENTER", 511.0);
    steer_cal.stbd = env_num("STEER_RAW_STBD", 1012.0);
    steer_cal.max_deg = env_num("STEER_MAX_DEG", 50.0);
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
                }
            } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                std::cerr << "read error: " << std::strerror(errno) << "\n";
                break;  // reopen socket
            }

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
        }
        close(sock);
    }
    writer.flush();
    return 0;
}
