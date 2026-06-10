#include "http.hpp"
#include "../log.hpp"
#include <curl/curl.h>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/post.hpp>
#include <algorithm>
#include <thread>

namespace net {

namespace {

// Boost.Asio thread pool – sized to hardware concurrency, min 4
boost::asio::thread_pool& pool() {
    static boost::asio::thread_pool tp(
        std::max(4u, std::thread::hardware_concurrency()));
    return tp;
}

size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* buf = static_cast<std::vector<uint8_t>*>(userdata);
    buf->insert(buf->end(), ptr, ptr + size * nmemb);
    return size * nmemb;
}

} // anonymous

std::expected<Response, HttpError>
get(const std::string& url, int timeout_sec,
    const std::vector<std::pair<std::string,std::string>>& extra_headers) {
    CURL* curl = curl_easy_init();
    if (!curl) return std::unexpected(HttpError::CurlInit);

    std::vector<uint8_t> buf;
    const long total      = (long)timeout_sec;
    const long connect_to = std::min(total, 20L);          // fail dead connects fast
    const long stall_win  = std::max(10L, total * 2 / 3);  // genuine-stall window
    curl_easy_setopt(curl, CURLOPT_URL,             url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        total);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, connect_to);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL,       1L);    // thread-safe timeouts
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE,  1L);    // survive flaky NAT/idle
    // Stall detection for thin / high-latency links (e.g. mobile data in Africa):
    // abort only when throughput stays below LOW_SPEED_LIMIT for LOW_SPEED_TIME
    // seconds. Unlike the hard TIMEOUT this TOLERATES a slow-but-progressing
    // download (which a flat 30s cap would wrongly kill) yet still drops a truly
    // stalled connection quickly, so the retry layer can re-issue it instead of
    // burning the full timeout. Scales with the caller's timeout.
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 64L);  // bytes/sec
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME,  stall_win);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,      "cpposmui/0.1 (OSM editor)");
    // Accept gzip/deflate – libcurl decompresses transparently (needed for
    // services like MS Building Footprints that serve .geojson.gz payloads).
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

    struct curl_slist* hdrs = nullptr;
    for (const auto& [k, v] : extra_headers)
        hdrs = curl_slist_append(hdrs, (k + ": " + v).c_str());
    if (hdrs) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    double total_time = 0.0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &total_time);
    if (hdrs) curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    // These are logged at DEBUG, not WARN: get() is the low-level primitive and
    // its callers (tile fetch with retry, detection) decide what's worth a
    // warning. In particular the tile path retries transient resets/timeouts, so
    // a per-attempt WARN here would spam the log even when the tile then loads.
    if (res != CURLE_OK) {
        // strerror gives a legible cause ("Timeout was reached", "Couldn't
        // connect", "Failure when receiving data") — essential for diagnosing
        // poor-connectivity behaviour from the log alone.
        LOG_DEBUG("HTTP GET {}: {} (curl {}) after {:.1f}s",
                  url, curl_easy_strerror(res), (int)res, total_time);
        return std::unexpected(res == CURLE_OPERATION_TIMEDOUT
                               ? HttpError::Timeout : HttpError::NetworkError);
    }
    // A slow-but-successful fetch is the signature of a thin link — make it
    // visible (DEBUG) so "tiles are crawling" is diagnosable, not invisible.
    if (total_time >= 5.0)
        LOG_DEBUG("HTTP GET {}: slow {:.1f}s ({} bytes)", url, total_time, buf.size());
    if (http_code >= 500) {
        LOG_DEBUG("HTTP GET {} -> {}", url, http_code);
        return std::unexpected(HttpError::HttpError5xx);
    }
    if (http_code >= 400) {
        LOG_DEBUG("HTTP GET {} -> {}", url, http_code);
        return std::unexpected(HttpError::HttpError4xx);
    }

    Response r;
    r.status_code = (int)http_code;
    r.bytes = std::move(buf);
    r.body.assign(r.bytes.begin(), r.bytes.end());
    return r;
}

std::expected<Response, HttpError>
request(const std::string& method, const std::string& url, const std::string& body,
        const std::vector<std::pair<std::string, std::string>>& headers, int timeout_sec) {
    CURL* curl = curl_easy_init();
    if (!curl) return std::unexpected(HttpError::CurlInit);

    std::vector<uint8_t> buf;
    curl_easy_setopt(curl, CURLOPT_URL,           url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,       (long)timeout_sec);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, std::min((long)timeout_sec, 20L));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL,      1L);   // thread-safe timeouts
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);   // survive flaky NAT/idle
    // NOTE: no LOW_SPEED_* here — Overpass/POST endpoints can sit silent for tens
    // of seconds doing server-side work before the first byte; a stall detector
    // would abort legitimate long queries. The hard TIMEOUT is the only cap.
    curl_easy_setopt(curl, CURLOPT_USERAGENT,     "cpposmui/0.1 (OSM editor)");
    if (!body.empty()) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
        curl_easy_setopt(curl, CURLOPT_COPYPOSTFIELDS, body.c_str());
    }

    struct curl_slist* hdrs = nullptr;
    for (const auto& [k, v] : headers)
        hdrs = curl_slist_append(hdrs, (k + ": " + v).c_str());
    if (hdrs) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (hdrs) curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        LOG_WARN("HTTP {} {}: {} (curl {})", method, url,
                 curl_easy_strerror(res), (int)res);
        return std::unexpected(res == CURLE_OPERATION_TIMEDOUT
                               ? HttpError::Timeout : HttpError::NetworkError);
    }

    Response r;
    r.status_code = (int)http_code;
    r.bytes = std::move(buf);
    r.body.assign(r.bytes.begin(), r.bytes.end());
    if (http_code >= 400)
        LOG_WARN("HTTP {} {} -> {}", method, url, http_code);
    return r; // caller inspects status_code (4xx/5xx are NOT errors here)
}

void fetch_tile_async(const std::string& url,
                      std::function<void(std::expected<Response, HttpError>)> cb,
                      int timeout_sec,
                      std::vector<std::pair<std::string,std::string>> extra_headers) {
    boost::asio::post(pool(), [url, cb = std::move(cb), timeout_sec,
                               hdrs = std::move(extra_headers)] {
        cb(get(url, timeout_sec, hdrs));
    });
}

void post_async(std::function<void()> fn) {
    boost::asio::post(pool(), std::move(fn));
}

void shutdown_thread_pool() {
    pool().join();
}

} // namespace net
