// Shared InfluxDB 2.x line-protocol writer for the C++ bridges.
// Batched HTTP writes via libcurl on a worker thread: the caller never blocks
// on the network. Bounded queue, automatic retry, drop-oldest when full.
//
// Public API: field(), str_field(), flush(), pending(), dropped().
#pragma once

#include <curl/curl.h>

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <mutex>
#include <sstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace usv {

inline std::string env_or(const char* name, const std::string& def) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : def;
}

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
        delete_url_ = opts.url + "/api/v2/delete?org=" + opts.org +
                      "&bucket=" + opts.bucket;
        token_ = "Authorization: Token " + opts.token;
        vessel_ = opts.vessel;
        static const bool curl_ready = [] {
            return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
        }();
        (void)curl_ready;
        worker_ = std::thread([this] { Run(); });
    }

    ~InfluxWriter() {
        flush();
        {
            std::lock_guard<std::mutex> l(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }
    InfluxWriter(const InfluxWriter&) = delete;
    InfluxWriter& operator=(const InfluxWriter&) = delete;

    // ts_ns > 0 pins the point to an explicit timestamp (geometry sets
    // written as one consistent snapshot); 0 lets the server stamp it.
    // Observers see every field as it is written (the MOOS bridge keeps the
    // latest value of each for the onboard readiness checks).
    using FieldObserver = std::function<void(const std::string&, const std::string&, double)>;
    using StrObserver = std::function<void(const std::string&, const std::string&,
                                           const std::string&)>;
    void observe(FieldObserver on_field, StrObserver on_str) {
        on_field_ = std::move(on_field);
        on_str_ = std::move(on_str);
    }

    void field(const std::string& measurement, const std::string& tags,
               const std::string& name, double value, long long ts_ns = 0) {
        if (std::isnan(value) || std::isinf(value)) return;
        if (on_field_) on_field_(measurement, name, value);
        std::ostringstream line;
        line << measurement << ",vessel=" << vessel_;
        if (!tags.empty()) line << "," << tags;
        line << " " << name << "=" << value;
        if (ts_ns > 0) line << " " << ts_ns;
        push(line.str());
    }

    // Synchronous delete of every point matching `predicate` (Flux delete
    // predicate syntax, e.g. _measurement="geometry" AND kind="route") in
    // this writer's bucket; the vessel tag is always added. Used before
    // rewriting a geometry set so stale vertices never linger. Bounded
    // by timeout; returns false on any failure (the caller writes anyway).
    bool erase(const std::string& predicate, long timeout_ms = 1500) {
        CURL* curl = curl_easy_init();
        if (!curl) return false;
        std::string pred = predicate + " AND vessel=\"" + vessel_ + "\"", esc;
        for (char c : pred) {           // JSON-escape the predicate string
            if (c == '"' || c == '\\') esc += '\\';
            esc += c;
        }
        const std::string body =
            "{\"start\":\"1970-01-01T00:00:00Z\",\"stop\":\"2100-01-01T00:00:00Z\","
            "\"predicate\":\"" + esc + "\"}";
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, token_.c_str());
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_URL, delete_url_.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_body);
        const CURLcode rc = curl_easy_perform(curl);
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return rc == CURLE_OK && code >= 200 && code < 300;
    }

    void str_field(const std::string& measurement, const std::string& tags,
                   const std::string& name, const std::string& value) {
        if (on_str_) on_str_(measurement, name, value);
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

    // Hand the current batch to the worker. Never blocks on the network.
    void flush() {
        std::lock_guard<std::mutex> l(mu_);
        if (batch_.empty()) return;
        std::string body;
        for (const auto& s : batch_) { body += s; body += '\n'; }
        batch_.clear();
        queue_.push_back(std::move(body));
        while (queue_.size() > kMaxQueuedBodies) {   // bound memory while DB is down
            queue_.pop_front();
            ++dropped_bodies_;
        }
        cv_.notify_one();
    }

    // Lines not yet accepted by the database (batch + queued bodies, approx).
    size_t pending() const {
        std::lock_guard<std::mutex> l(mu_);
        return batch_.size() + queue_.size() * kBatchLines;
    }
    // Bodies (of up to kBatchLines lines) discarded because the queue overflowed.
    size_t dropped() const { return dropped_bodies_.load(); }

private:
    static constexpr size_t kBatchLines = 100;
    static constexpr size_t kMaxQueuedBodies = 200;   // ~20 000 lines

    void push(std::string line) {
        bool full = false;
        {
            std::lock_guard<std::mutex> l(mu_);
            batch_.push_back(std::move(line));
            full = batch_.size() >= kBatchLines;
        }
        if (full) flush();
    }

    // Worker thread: owns the curl handle for its whole life.
    void Run() {
        CURL* curl = curl_easy_init();
        std::unique_lock<std::mutex> l(mu_);
        while (true) {
            cv_.wait(l, [&] { return stop_ || !queue_.empty(); });
            if (queue_.empty() && stop_) break;
            std::string body = queue_.front();
            l.unlock();
            const bool ok = Post(curl, body);
            l.lock();
            if (ok) {
                if (!queue_.empty()) queue_.pop_front();
            } else if (stop_) {
                break;                    // shutting down: do not spin on a dead DB
            } else {
                l.unlock();
                std::this_thread::sleep_for(std::chrono::seconds(1));
                l.lock();
            }
        }
        if (curl) curl_easy_cleanup(curl);
    }

    bool Post(CURL* curl, const std::string& body) {
        if (!curl) return false;
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, token_.c_str());
        headers = curl_slist_append(headers,
                                    "Content-Type: text/plain; charset=utf-8");
        curl_easy_setopt(curl, CURLOPT_URL, url_.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 500L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 2000L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        CURLcode rc = curl_easy_perform(curl);
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        curl_slist_free_all(headers);
        const bool ok = (rc == CURLE_OK && http_code >= 200 && http_code < 300);
        if (!ok)
            std::cerr << "influx write failed rc=" << rc << " http=" << http_code
                      << " (will retry)\n";
        return ok;
    }

    static size_t discard_body(char*, size_t size, size_t nmemb, void*) {
        return size * nmemb;
    }

    std::string url_, delete_url_, token_, vessel_;
    FieldObserver on_field_;
    StrObserver on_str_;
    std::vector<std::string> batch_;
    std::deque<std::string> queue_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::thread worker_;
    bool stop_ = false;
    std::atomic<size_t> dropped_bodies_{0};
};

}  // namespace usv
