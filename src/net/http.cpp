#include "http.hpp"
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
get(const std::string& url, int timeout_sec) {
    CURL* curl = curl_easy_init();
    if (!curl) return std::unexpected(HttpError::CurlInit);

    std::vector<uint8_t> buf;
    curl_easy_setopt(curl, CURLOPT_URL,             url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        (long)timeout_sec);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,      "cpposmui/0.1 (OSM editor)");
    // Accept gzip/deflate – libcurl decompresses transparently (needed for
    // services like MS Building Footprints that serve .geojson.gz payloads).
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK)
        return std::unexpected(res == CURLE_OPERATION_TIMEDOUT
                               ? HttpError::Timeout : HttpError::NetworkError);
    if (http_code >= 500) return std::unexpected(HttpError::HttpError5xx);
    if (http_code >= 400) return std::unexpected(HttpError::HttpError4xx);

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

    if (res != CURLE_OK)
        return std::unexpected(res == CURLE_OPERATION_TIMEDOUT
                               ? HttpError::Timeout : HttpError::NetworkError);

    Response r;
    r.status_code = (int)http_code;
    r.bytes = std::move(buf);
    r.body.assign(r.bytes.begin(), r.bytes.end());
    return r; // caller inspects status_code (4xx/5xx are NOT errors here)
}

void fetch_tile_async(const std::string& url,
                      std::function<void(std::expected<Response, HttpError>)> cb,
                      int timeout_sec) {
    boost::asio::post(pool(), [url, cb = std::move(cb), timeout_sec] {
        cb(get(url, timeout_sec));
    });
}

void post_async(std::function<void()> fn) {
    boost::asio::post(pool(), std::move(fn));
}

void shutdown_thread_pool() {
    pool().join();
}

} // namespace net
