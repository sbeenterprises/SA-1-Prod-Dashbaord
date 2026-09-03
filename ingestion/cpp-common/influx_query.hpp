// Background poller for the latest value of a few InfluxDB fields written
// by OTHER producers (Telegraf host metrics, the Signal K and CAN bridges,
// a camera manager). The MOOS bridge feeds them into its onboard
// readiness checks. One Flux query per period on a worker thread; the
// MOOS app never blocks on the network. libcurl, no JSON library: the
// query is sent as application/vnd.flux and the reply is read as plain
// CSV (dialect via the Accept-less default: annotated CSV, whose
// annotation lines start with '#').
#pragma once
#include <curl/curl.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "influx_writer.hpp"

namespace usv {

struct QueryField {
    std::string measurement, field;
    std::string where;   // extra Flux predicate on r, e.g. r.url == "8.8.8.8"
};

struct LatestValue {
    double value = 0.0;
    double age_s = 0.0;   // at the time of the poll
};

class LatestPoller {
public:
    LatestPoller() = default;
    ~LatestPoller() { stop(); }
    LatestPoller(const LatestPoller&) = delete;
    LatestPoller& operator=(const LatestPoller&) = delete;

    // fields: what to fetch; window: how far back last() may look.
    void start(const InfluxOptions& opts, std::vector<QueryField> fields,
               double period_s = 5.0, const std::string& window = "2m") {
        stop();
        opts_ = opts;
        fields_ = std::move(fields);
        period_s_ = period_s;
        query_ = BuildQuery(opts_.bucket, opts_.vessel, fields_, window);
        stop_ = false;
        worker_ = std::thread([this] { run(); });
    }

    void stop() {
        stop_ = true;
        if (worker_.joinable()) worker_.join();
    }

    // "measurement.field" -> latest value; empty until the first poll works.
    std::map<std::string, LatestValue> snapshot() const {
        std::lock_guard<std::mutex> lock(mu_);
        return latest_;
    }
    bool healthy() const { return ok_.load(); }

    // Flux: one last() per requested field, restricted to this vessel when
    // the series carries a vessel tag (Telegraf series do not).
    static std::string BuildQuery(const std::string& bucket, const std::string& vessel,
                                  const std::vector<QueryField>& fields,
                                  const std::string& window) {
        std::ostringstream q;
        q << "from(bucket: \"" << bucket << "\")\n  |> range(start: -" << window << ")\n"
          << "  |> filter(fn: (r) => ";
        for (size_t i = 0; i < fields.size(); ++i) {
            if (i) q << " or ";
            q << "(r._measurement == \"" << fields[i].measurement << "\" and r._field == \""
              << fields[i].field << "\"";
            if (!fields[i].where.empty()) q << " and " << fields[i].where;
            q << ")";
        }
        q << ")\n"
          << "  |> filter(fn: (r) => not exists r.vessel or r.vessel == \"" << vessel << "\")\n"
          << "  |> last()\n"
          << "  |> group(columns: [\"_measurement\", \"_field\"])\n"
          << "  |> min()\n"   // several series (cameras): the weakest one counts
          << "  |> keep(columns: [\"_measurement\", \"_field\", \"_value\", \"_time\"])\n";
        return q.str();
    }

    // Annotated-CSV reply -> (measurement.field, value, age). Exposed for
    // the unit test.
    static std::map<std::string, LatestValue> ParseCsv(const std::string& body,
                                                      std::time_t now_utc) {
        std::map<std::string, LatestValue> out;
        std::istringstream in(body);
        std::string line;
        std::vector<std::string> header;
        int im = -1, ifld = -1, iv = -1, it = -1;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) { header.clear(); continue; }   // table separator
            if (line[0] == '#') continue;                       // annotation
            std::vector<std::string> cols = SplitLine(line);
            if (header.empty()) {
                header = cols;
                im = ifld = iv = it = -1;
                for (size_t i = 0; i < header.size(); ++i) {
                    if (header[i] == "_measurement") im = static_cast<int>(i);
                    else if (header[i] == "_field") ifld = static_cast<int>(i);
                    else if (header[i] == "_value") iv = static_cast<int>(i);
                    else if (header[i] == "_time") it = static_cast<int>(i);
                }
                continue;
            }
            if (im < 0 || ifld < 0 || iv < 0) continue;
            const int need = std::max(std::max(im, ifld), std::max(iv, it));
            if (static_cast<int>(cols.size()) <= need) continue;
            LatestValue lv;
            try { lv.value = std::stod(cols[iv]); } catch (...) { continue; }
            if (it >= 0) lv.age_s = static_cast<double>(now_utc - ParseRfc3339(cols[it]));
            out[cols[im] + "." + cols[ifld]] = lv;
        }
        return out;
    }

private:
    static std::vector<std::string> SplitLine(const std::string& line) {
        std::vector<std::string> cols;
        std::string cur;
        bool quoted = false;
        for (char c : line) {
            if (c == '"') { quoted = !quoted; continue; }
            if (c == ',' && !quoted) { cols.push_back(cur); cur.clear(); continue; }
            cur += c;
        }
        cols.push_back(cur);
        return cols;
    }

    static std::time_t ParseRfc3339(const std::string& s) {
        std::tm tm{};
        if (s.size() < 19) return 0;
        tm.tm_year = std::atoi(s.substr(0, 4).c_str()) - 1900;
        tm.tm_mon = std::atoi(s.substr(5, 2).c_str()) - 1;
        tm.tm_mday = std::atoi(s.substr(8, 2).c_str());
        tm.tm_hour = std::atoi(s.substr(11, 2).c_str());
        tm.tm_min = std::atoi(s.substr(14, 2).c_str());
        tm.tm_sec = std::atoi(s.substr(17, 2).c_str());
        return timegm(&tm);
    }

    static size_t sink(char* ptr, size_t size, size_t nmemb, void* ud) {
        static_cast<std::string*>(ud)->append(ptr, size * nmemb);
        return size * nmemb;
    }

    bool poll_once() {
        CURL* curl = curl_easy_init();
        if (!curl) return false;
        std::string body;
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, ("Authorization: Token " + opts_.token).c_str());
        headers = curl_slist_append(headers, "Content-Type: application/vnd.flux");
        headers = curl_slist_append(headers, "Accept: application/csv");
        const std::string url = opts_.url + "/api/v2/query?org=" + opts_.org;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, query_.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sink);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 3000L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 1500L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        const CURLcode rc = curl_easy_perform(curl);
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        if (rc != CURLE_OK || code < 200 || code >= 300) return false;
        auto parsed = ParseCsv(body, std::time(nullptr));
        std::lock_guard<std::mutex> lock(mu_);
        latest_ = std::move(parsed);
        return true;
    }

    void run() {
        while (!stop_) {
            ok_ = poll_once();
            // Sleep in small steps so stop() returns promptly.
            const auto until = std::chrono::steady_clock::now() +
                               std::chrono::milliseconds(static_cast<int>(period_s_ * 1000));
            while (!stop_ && std::chrono::steady_clock::now() < until)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    InfluxOptions opts_;
    std::vector<QueryField> fields_;
    std::string query_;
    double period_s_ = 5.0;
    std::map<std::string, LatestValue> latest_;
    mutable std::mutex mu_;
    std::thread worker_;
    std::atomic<bool> stop_{true};
    std::atomic<bool> ok_{false};
};

}  // namespace usv
