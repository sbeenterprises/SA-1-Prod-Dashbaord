// frigate_logic.hpp - camera status from Frigate's HTTP API, no I/O.
//
// Frigate (NVR, Docker on usv-722, API on port 5000) already pulls every
// camera stream through its built-in go2rtc. Its API says which cameras are
// enabled and how many frames each one is delivering:
//
//   GET /api/config -> {"cameras": {"<name>": {"enabled": true,
//                         "ffmpeg": {"inputs": [{"path": "rtsp://..."}]},
//                         "record": {"enabled": false}, ...}},
//                       "go2rtc": {"streams": {"<name>": ["rtsp://u:p@IP:554/..."]}}}
//   GET /api/stats  -> {"cameras": {"<name>": {"camera_fps": 5.1,
//                         "process_fps": 5.1, ...}}, ...}
//
// CameraStatus() joins the two into one row per ENABLED camera. A disabled
// camera (cameras 3 to 6 until they have PoE power) is left out, so it never
// reads as a fault. Unit-tested by frigate_logic_test.cpp.
#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace usv {
namespace frigate {

using json = nlohmann::json;

struct Camera {
    std::string name;
    double fps = 0.0;          // camera_fps: frames arriving from the camera
    bool stream_up = false;    // fps above the threshold
    bool recording = false;    // record.enabled in Frigate's config
    std::string host;          // camera IP (from go2rtc / ffmpeg URLs), may be ""
    int port = 554;            // its RTSP port
};

namespace detail {

// "rtsp://user:pass@192.168.22.14:554/Streaming/Channels/101" -> host, port.
// Returns false for anything that is not rtsp/rtsps or that points at the
// loopback restream (127.0.0.1:8554), which says nothing about the camera.
inline bool RtspHost(const std::string& url, std::string* host, int* port) {
    size_t p = url.find("://");
    if (p == std::string::npos) return false;
    const std::string scheme = url.substr(0, p);
    if (scheme != "rtsp" && scheme != "rtsps") return false;
    std::string rest = url.substr(p + 3);
    const size_t slash = rest.find('/');
    if (slash != std::string::npos) rest.erase(slash);
    const size_t at = rest.rfind('@');
    if (at != std::string::npos) rest.erase(0, at + 1);
    int prt = scheme == "rtsps" ? 322 : 554;
    const size_t colon = rest.rfind(':');
    if (colon != std::string::npos) {
        try {
            prt = std::stoi(rest.substr(colon + 1));
        } catch (...) {
            return false;
        }
        rest.erase(colon);
    }
    if (rest.empty() || rest == "127.0.0.1" || rest == "localhost") return false;
    *host = rest;
    *port = prt;
    return true;
}

inline void CollectUrls(const json& v, std::vector<std::string>* out) {
    if (v.is_string()) {
        out->push_back(v.get<std::string>());
    } else if (v.is_array()) {
        for (const auto& e : v) CollectUrls(e, out);
    } else if (v.is_object()) {
        if (v.contains("path")) CollectUrls(v["path"], out);
        if (v.contains("url")) CollectUrls(v["url"], out);
    }
}

}  // namespace detail

// One Camera per camera Frigate has enabled, in config order (json objects
// iterate sorted by name). stats may be null (Frigate up but no stats yet).
inline std::vector<Camera> CameraStatus(const json& config, const json& stats,
                                        double min_fps = 1.0) {
    std::vector<Camera> out;
    if (!config.is_object() || !config.contains("cameras") ||
        !config["cameras"].is_object())
        return out;
    const json* streams = nullptr;
    if (config.contains("go2rtc") && config["go2rtc"].is_object() &&
        config["go2rtc"].contains("streams"))
        streams = &config["go2rtc"]["streams"];

    for (auto it = config["cameras"].begin(); it != config["cameras"].end(); ++it) {
        const json& c = it.value();
        if (!c.is_object()) continue;
        if (c.contains("enabled") && c["enabled"].is_boolean() &&
            !c["enabled"].get<bool>())
            continue;

        Camera cam;
        cam.name = it.key();
        if (c.contains("record") && c["record"].is_object() &&
            c["record"].contains("enabled") && c["record"]["enabled"].is_boolean())
            cam.recording = c["record"]["enabled"].get<bool>();

        // Camera address: its go2rtc stream(s) first (same name, then
        // name_sub), else the ffmpeg inputs if they point at the camera.
        std::vector<std::string> urls;
        if (streams && streams->is_object()) {
            for (const std::string& key : {cam.name, cam.name + "_sub"})
                if (streams->contains(key)) detail::CollectUrls((*streams)[key], &urls);
        }
        if (c.contains("ffmpeg") && c["ffmpeg"].is_object() &&
            c["ffmpeg"].contains("inputs"))
            detail::CollectUrls(c["ffmpeg"]["inputs"], &urls);
        for (const auto& u : urls)
            if (detail::RtspHost(u, &cam.host, &cam.port)) break;

        if (stats.is_object() && stats.contains("cameras") &&
            stats["cameras"].is_object() && stats["cameras"].contains(cam.name)) {
            const json& s = stats["cameras"][cam.name];
            if (s.is_object() && s.contains("camera_fps") && s["camera_fps"].is_number())
                cam.fps = s["camera_fps"].get<double>();
        }
        cam.stream_up = cam.fps >= min_fps;
        out.push_back(cam);
    }
    return out;
}

}  // namespace frigate
}  // namespace usv
