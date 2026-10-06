// Unit tests for frigate_logic.hpp:  g++ -std=c++17 frigate_logic_test.cpp -o t && ./t
#include "frigate_logic.hpp"

#include <cstdio>
#include <string>

static int g_pass = 0, g_fail = 0;
#define CHECK(e) do { if (e) ++g_pass; else { ++g_fail; \
  std::printf("  [FAIL] %s:%d  %s\n", __FILE__, __LINE__, #e); } } while (0)

using namespace usv::frigate;
using json = nlohmann::json;

// Shape of usv-722's Frigate 0.16: two live Hikvision cameras via go2rtc,
// camera_3 configured but disabled, legacy flir_camera disabled.
static const char* kConfig = R"({
  "go2rtc": {"streams": {
    "bullet_camera": ["rtsp://admin:p%40ss@192.168.22.14:554/Streaming/Channels/101"],
    "bullet_camera_sub": ["rtsp://admin:p%40ss@192.168.22.14:554/Streaming/Channels/102"],
    "camera_2": "rtsp://admin:x@192.168.22.15/Streaming/Channels/101",
    "camera_3": ["rtsp://admin:x@192.168.22.16:554/Streaming/Channels/101"]}},
  "cameras": {
    "bullet_camera": {"enabled": true, "record": {"enabled": false},
      "ffmpeg": {"inputs": [{"path": "rtsp://127.0.0.1:8554/bullet_camera_sub", "roles": ["detect"]}]}},
    "camera_2": {"enabled": true, "record": {"enabled": true},
      "ffmpeg": {"inputs": [{"path": "rtsp://127.0.0.1:8554/camera_2", "roles": ["detect"]}]}},
    "camera_3": {"enabled": false},
    "flir_camera": {"enabled": false,
      "ffmpeg": {"inputs": [{"path": "rtsp://192.168.143.20:554/stream"}]}},
    "direct_cam": {"ffmpeg": {"inputs": [{"path": "rtsp://u:p@10.0.0.9:8555/live"}]}}
  }})";

static const char* kStats = R"({"cameras": {
    "bullet_camera": {"camera_fps": 5.1, "process_fps": 5.0},
    "camera_2": {"camera_fps": 0.0},
    "camera_3": {"camera_fps": 0.0}},
  "service": {"version": "0.16.0"}})";

int main() {
    const auto cams = CameraStatus(json::parse(kConfig), json::parse(kStats));
    // enabled only (camera_3, flir_camera disabled); direct_cam has no
    // "enabled" key -> Frigate's default is enabled
    CHECK(cams.size() == 3);
    if (cams.size() == 3) {
        CHECK(cams[0].name == "bullet_camera");
        CHECK(cams[0].host == "192.168.22.14" && cams[0].port == 554);
        CHECK(cams[0].stream_up && cams[0].fps > 5.0);
        CHECK(!cams[0].recording);
        CHECK(cams[1].name == "camera_2");
        CHECK(cams[1].host == "192.168.22.15" && cams[1].port == 554);  // default port
        CHECK(!cams[1].stream_up);
        CHECK(cams[1].recording);
        CHECK(cams[2].name == "direct_cam");
        CHECK(cams[2].host == "10.0.0.9" && cams[2].port == 8555);   // from ffmpeg input
        CHECK(!cams[2].stream_up);                                    // no stats row
    }

    // Frigate unreachable for stats: cameras known, all down
    const auto nostats = CameraStatus(json::parse(kConfig), json());
    CHECK(nostats.size() == 3 && !nostats[0].stream_up);

    // No config yet / junk
    CHECK(CameraStatus(json(), json::parse(kStats)).empty());
    CHECK(CameraStatus(json::parse(R"({"cameras": []})"), json()).empty());

    // Only a loopback restream known: no host (connected falls back to stream)
    const auto lo = CameraStatus(json::parse(R"({"cameras": {"c": {"ffmpeg":
        {"inputs": [{"path": "rtsp://127.0.0.1:8554/c"}]}}}})"), json());
    CHECK(lo.size() == 1 && lo[0].host.empty());

    // URL parsing
    std::string h; int p = 0;
    CHECK(detail::RtspHost("rtsp://a:b@cam.local:10554/x", &h, &p) && h == "cam.local" && p == 10554);
    CHECK(!detail::RtspHost("http://192.168.22.14/x", &h, &p));
    CHECK(!detail::RtspHost("rtsp://localhost:8554/x", &h, &p));

    std::printf("frigate_logic_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
