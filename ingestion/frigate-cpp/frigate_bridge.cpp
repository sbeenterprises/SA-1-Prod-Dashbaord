// frigate_bridge - camera status from Frigate (NVR) -> InfluxDB (FR-7).
//
// Every FRIGATE_POLL_S seconds, for each camera ENABLED in Frigate:
//   camera,cam=<name> connected   1 = the camera answers on its RTSP port
//                     stream_up   1 = Frigate receives frames (>= 1 fps)
//                     fps         frames per second arriving from the camera
//                     recording   1 = Frigate records this camera
// and one summary row for the Readiness check:
//   camera_summary    enabled, streaming, connected (counts), frigate_up (0/1)
//
// Disabled cameras (configured but not yet powered) are not written, so they
// never raise the Camera Offline alert. The camera address comes from
// Frigate's own config (go2rtc / ffmpeg RTSP URLs); credentials in those URLs
// are never logged or stored.
//
// Env: FRIGATE_URL (e.g. http://127.0.0.1:5000; empty = service disabled),
//      FRIGATE_POLL_S (5), INFLUX_URL, INFLUX_TOKEN, INFLUX_ORG,
//      INFLUX_BUCKET, VESSEL_ID

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "../cpp-common/http_get.hpp"
#include "../cpp-common/influx_writer.hpp"
#include "frigate_logic.hpp"

namespace {

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop = true; }

// TCP connect with a timeout: does anything answer on host:port?
bool TcpOpen(const std::string& host, int port, int timeout_ms) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0)
        return false;
    bool ok = false;
    for (addrinfo* a = res; a && !ok; a = a->ai_next) {
        const int fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        int rc = connect(fd, a->ai_addr, a->ai_addrlen);
        if (rc == 0) {
            ok = true;
        } else if (errno == EINPROGRESS) {
            pollfd p{fd, POLLOUT, 0};
            if (poll(&p, 1, timeout_ms) == 1) {
                int err = 0;
                socklen_t len = sizeof(err);
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
                ok = (err == 0);
            }
        }
        close(fd);
    }
    freeaddrinfo(res);
    return ok;
}

}  // namespace

int main() {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    std::string base = usv::env_or("FRIGATE_URL", "http://127.0.0.1:5000");
    while (!base.empty() && base.back() == '/') base.pop_back();
    int poll_s = 5;
    try { poll_s = std::stoi(usv::env_or("FRIGATE_POLL_S", "5")); } catch (...) {}
    if (poll_s < 1) poll_s = 1;
    if (poll_s > 60) poll_s = 60;

    curl_global_init(CURL_GLOBAL_DEFAULT);
    usv::InfluxWriter writer{"telemetry"};
    std::cerr << "frigate_bridge: frigate=" << base << " poll=" << poll_s << "s\n";

    nlohmann::json config;                       // last good /api/config
    auto next_config = std::chrono::steady_clock::now();
    bool was_up = true;
    size_t last_enabled = static_cast<size_t>(-1), last_streaming = last_enabled;

    while (!g_stop) {
        const auto t0 = std::chrono::steady_clock::now();
        long st = 0;

        if (t0 >= next_config || config.is_null()) {
            const std::string body = usv::HttpGet(base + "/api/config", 3000, "", &st);
            auto j = nlohmann::json::parse(body, nullptr, false);
            if (!body.empty() && !j.is_discarded()) {
                config = std::move(j);
                next_config = t0 + std::chrono::seconds(60);   // config rarely changes
            }
        }
        const std::string sbody = usv::HttpGet(base + "/api/stats", 3000, "", &st);
        auto stats = nlohmann::json::parse(sbody, nullptr, false);
        const bool up = !sbody.empty() && !stats.is_discarded();
        if (!up) stats = nullptr;

        const auto cams = usv::frigate::CameraStatus(config, stats);
        size_t streaming = 0, connected = 0;
        for (const auto& c : cams) {
            const bool conn = c.host.empty() ? c.stream_up : TcpOpen(c.host, c.port, 1000);
            const std::string tags = "cam=" + c.name;
            writer.field("camera", tags, "connected", conn ? 1.0 : 0.0);
            writer.field("camera", tags, "stream_up", c.stream_up ? 1.0 : 0.0);
            writer.field("camera", tags, "fps", c.fps);
            writer.field("camera", tags, "recording", c.recording ? 1.0 : 0.0);
            streaming += c.stream_up;
            connected += conn;
        }
        writer.field("camera_summary", "", "frigate_up", up ? 1.0 : 0.0);
        if (!config.is_null()) {
            writer.field("camera_summary", "", "enabled", static_cast<double>(cams.size()));
            writer.field("camera_summary", "", "streaming", static_cast<double>(streaming));
            writer.field("camera_summary", "", "connected", static_cast<double>(connected));
        }
        writer.flush();

        // Log changes only.
        if (up != was_up) {
            std::cerr << "frigate_bridge: Frigate " << (up ? "reachable" : "unreachable (retrying)")
                      << " at " << base << "\n";
            was_up = up;
        }
        if (cams.size() != last_enabled || streaming != last_streaming) {
            std::cerr << "frigate_bridge: " << streaming << "/" << cams.size()
                      << " enabled camera(s) streaming";
            for (const auto& c : cams)
                std::cerr << " " << c.name << (c.stream_up ? "=up" : "=DOWN");
            std::cerr << "\n";
            last_enabled = cams.size();
            last_streaming = streaming;
        }

        std::this_thread::sleep_until(t0 + std::chrono::seconds(poll_s));
    }
    writer.flush();
    curl_global_cleanup();
    return 0;
}
