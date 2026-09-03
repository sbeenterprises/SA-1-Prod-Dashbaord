// Tiny blocking HTTP GET (libcurl) for one-off lookups such as the Signal K
// route name. Bounded by timeout_ms; returns "" on any failure.
#pragma once
#include <curl/curl.h>
#include <string>

namespace usv {

inline size_t http_get_sink(char* ptr, size_t size, size_t nmemb, void* ud) {
    static_cast<std::string*>(ud)->append(ptr, size * nmemb);
    return size * nmemb;
}

inline std::string HttpGet(const std::string& url, long timeout_ms = 1000,
                           const std::string& bearer = "", long* status = nullptr) {
    CURL* curl = curl_easy_init();
    if (!curl) return "";
    std::string body;
    struct curl_slist* headers = nullptr;
    if (!bearer.empty()) {
        headers = curl_slist_append(headers, ("Authorization: Bearer " + bearer).c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_get_sink);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, timeout_ms);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    const CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(curl);
    if (headers) curl_slist_free_all(headers);
    if (status) *status = (rc == CURLE_OK) ? code : -static_cast<long>(rc);
    if (rc != CURLE_OK || code < 200 || code >= 300) return "";
    return body;
}

}  // namespace usv
