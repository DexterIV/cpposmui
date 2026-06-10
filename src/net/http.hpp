#pragma once
#include <string>
#include <functional>
#include <expected>
#include <vector>
#include <cstdint>

namespace net {

enum class HttpError { CurlInit, NetworkError, Timeout, HttpError4xx, HttpError5xx };

struct Response {
    int         status_code{};
    std::string body;
    std::vector<uint8_t> bytes; // populated for binary responses
};

// Blocking GET – call from a worker thread, not the main thread
std::expected<Response, HttpError>
get(const std::string& url, int timeout_sec = 30,
    const std::vector<std::pair<std::string,std::string>>& extra_headers = {});

// Blocking generic request (GET/POST/PUT/...) with custom headers and body.
// Unlike get(), a 4xx/5xx response is returned (not an error) so the caller can
// read status_code and body for diagnostics. Call from a worker thread.
std::expected<Response, HttpError>
request(const std::string& method, const std::string& url, const std::string& body,
        const std::vector<std::pair<std::string, std::string>>& headers,
        int timeout_sec = 60);

// Async tile fetch – fires callback on a thread-pool thread.
// extra_headers: e.g. {{"Referer","https://example.com"}} for services that check origin.
void fetch_tile_async(const std::string& url,
                      std::function<void(std::expected<Response, HttpError>)> cb,
                      int timeout_sec = 30,
                      std::vector<std::pair<std::string,std::string>> extra_headers = {});

// Post any background task to the shared thread pool.
// Use this instead of creating a separate pool in other subsystems.
void post_async(std::function<void()> fn);

void shutdown_thread_pool();

} // namespace net
