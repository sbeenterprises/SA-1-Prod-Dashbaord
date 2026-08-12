// Shared InfluxDB 2.x line-protocol writer for the C++ bridges.
// Batched HTTP writes via libcurl, automatic retry, bounded buffer.
#pragma once

#include <curl/curl.h>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace usv {

inline std::string env_or(const char* name, const std::string& def) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : def;
}

// Explicit connection settings; FromEnv() gives env-var defaults so callers
// (e.g. a MOOS app) can override from their own config source.
struct InfluxOptions {
    std::string url;
    std::string token;
    std::string org;
    std::string bucket;
    std::string vessel;

    static InfluxOptions FromEnv(const std::string& bucket = "") {
        InfluxOptions o;
        o.url = env_or("INFLUX_URL", "http://localhost:8086");
        o.token = env_or("INFLUX_TOKEN", "");
        o.org = env_or("INFLUX_ORG", "usv");
        o.bucket = bucket.empty() ? env_or("INFLUX_BUCKET", "telemetry")
                                  : bucket;
        o.vessel = env_or("VESSEL_ID", "usv-01");
        return o;
    }
};

class InfluxWriter {
public:
    explicit InfluxWriter(const std::string& bucket = "")
        : InfluxWriter(InfluxOptions::FromEnv(bucket)) {}

    explicit InfluxWriter(const InfluxOptions& opts) {
        url_ = opts.url + "/api/v2/write?org=" + opts.org +
               "&bucket=" + opts.bucket + "&precision=ns";
        token_ = "Authorization: Token " + opts.token;
        vessel_ = opts.vessel;
        // curl global init exactly once per process; never cleaned up —
        // these writers live for the daemon's lifetime.
        static const bool curl_ready = [] {
            return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
        }();
        (void)curl_ready;
        curl_ = curl_easy_init();
    }
    ~InfluxWriter() {
        flush();
        if (curl_) curl_easy_cleanup(curl_);
    }
    InfluxWriter(const InfluxWriter&) = delete;
    InfluxWriter& operator=(const InfluxWriter&) = delete;

    void field(const std::string& measurement, const std::string& tags,
               const std::string& name, double value) {
        if (std::isnan(value) || std::isinf(value)) return;
        std::ostringstream line;
        line << measurement << ",vessel=" << vessel_;
        if (!tags.empty()) line << "," << tags;
        line << " " << name << "=" << value;
        push(line.str());
    }

    void str_field(const std::string& measurement, const std::string& tags,
                   const std::string& name, const std::string& value) {
        std::string esc;
        esc.reserve(value.size());
        for (char c : value) {
            if (c == '"' || c == '\\') esc += '\\';
            if (c == '\n') { esc += ' '; continue; }
            esc += c;
        }
        std::ostringstream line;
        line << measurement << ",vessel=" << vessel_;
        if (!tags.empty()) line << "," << tags;
        line << " " << name << "=\"" << esc << "\"";
        push(line.str());
    }

    void flush() {
        if (batch_.empty() || !curl_) return;
        std::string body;
        for (const auto& l : batch_) { body += l; body += '\n'; }

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, token_.c_str());
        headers = curl_slist_append(headers,
                                    "Content-Type: text/plain; charset=utf-8");
        curl_easy_setopt(curl_, CURLOPT_URL, url_.c_str());
        curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl_, CURLOPT_TIMEOUT_MS, 5000L);

        CURLcode rc = curl_easy_perform(curl_);
        long http_code = 0;
        curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &http_code);
        curl_slist_free_all(headers);

        if (rc == CURLE_OK && http_code >= 200 && http_code < 300) {
            batch_.clear();
        } else {
            std::cerr << "influx write failed rc=" << rc << " http="
                      << http_code << " (will retry)\n";
            if (batch_.size() > 10000)   // bound memory while DB is down
                batch_.erase(batch_.begin(), batch_.begin() + 5000);
        }
    }

    size_t pending() const { return batch_.size(); }

private:
    void push(std::string line) {
        batch_.push_back(std::move(line));
        if (batch_.size() >= 100) flush();
    }

    CURL* curl_ = nullptr;
    std::string url_, token_, vessel_;
    std::vector<std::string> batch_;
};

}  // namespace usv
