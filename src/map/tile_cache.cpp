#include "tile_cache.hpp"
#include "../net/http.hpp"
#include "../log.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <glad/gl.h>   // glad2 header – must come before any other GL include

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <semaphore>
#include <thread>

namespace map {

namespace {
namespace fs = std::filesystem;
constexpr double PI = std::numbers::pi;

int clamp_tile_x(int v, int z) {
    int n = 1 << z;
    return (v % n + n) % n;
}

// Cap concurrent tile network requests across ALL TileCache instances. WMS
// servers (e.g. Geoportal) render each tile on the fly and reset connections
// (curl error 56) when slammed by the whole thread pool at once. 6 mirrors the
// classic per-host browser connection limit — polite to the server, still fast.
constexpr int kMaxConcurrentFetches = 6;
std::counting_semaphore<kMaxConcurrentFetches>& fetch_gate() {
    static std::counting_semaphore<kMaxConcurrentFetches> sem(kMaxConcurrentFetches);
    return sem;
}
struct GateGuard {
    GateGuard()  { fetch_gate().acquire(); }
    ~GateGuard() { fetch_gate().release(); }
    GateGuard(const GateGuard&) = delete;
    GateGuard& operator=(const GateGuard&) = delete;
};

// Fetch a tile with bounded retries on transient failures. A 4xx is permanent
// (auth/not-found) so it returns immediately; timeouts, connection resets and
// 5xx are retried with exponential backoff plus a per-tile offset so a wave of
// failed tiles doesn't retry in lockstep and re-trigger the same overload.
//
// Geoportal's WMS resets connections (curl error 56) intermittently even at low
// concurrency — measured ~20-30% per request — so the retries here are the real
// fix, not the throttle.  5 attempts drives the residual failure rate to ~0.1%.
// min_bytes > 0 enables blank-tile detection: a successful 200 whose body is
// smaller than min_bytes is the server's load-shed "blank" tile (e.g. Geoportal
// returns a fixed ~2419-byte all-white JPEG under load). It is treated as a
// transient failure — retried, and on exhaustion reported as an error so the
// caller does NOT cache it.
std::expected<net::Response, net::HttpError>
fetch_with_retry(const std::string& url,
                 const std::vector<std::pair<std::string,std::string>>& hdrs,
                 const TileCoord& c, int min_bytes) {
    constexpr int kMaxAttempts = 5;
    std::expected<net::Response, net::HttpError> resp;
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        resp = net::get(url, 30, hdrs);
        bool blank = resp && min_bytes > 0 && (int)resp->bytes.size() < min_bytes;
        if (resp && !blank) {
            if (attempt > 1)
                LOG_DEBUG("tile ok after {} attempts z={} x={} y={}",
                          attempt, c.z, c.x, c.y);
            return resp;
        }
        // Past here the attempt failed: either a network/timeout/5xx error or a
        // blank load-shed tile.
        if (blank)
            LOG_DEBUG("blank tile {}B (<{}) z={} x={} y={} attempt {}",
                      resp->bytes.size(), min_bytes, c.z, c.x, c.y, attempt);
        else if (resp.error() == net::HttpError::HttpError4xx)
            break;             // permanent, won't change
        if (attempt == kMaxAttempts) {
            // Exhausted. A blank tile carries a 200, so surface a transient error
            // instead so the caller marks Error and skips the disk cache.
            if (blank) return std::unexpected(net::HttpError::NetworkError);
            break;             // return the underlying network/5xx error
        }
        int jitter  = (c.x * 7 + c.y * 13) % 150;               // 0–149 ms desync
        int backoff = 150 * (1 << (attempt - 1)) + jitter;      // 150,300,600,1200ms
        std::this_thread::sleep_for(std::chrono::milliseconds(backoff));
    }
    return resp;
}
} // anonymous

fs::path TileCache::disk_cache_root() {
    const char* appdata = std::getenv("APPDATA");
    fs::path base = appdata ? fs::path(appdata) / "cpposmui" : fs::current_path();
    return base / "tile_cache";
}

std::uintmax_t TileCache::disk_cache_size() {
    fs::path root = disk_cache_root();
    std::error_code ec;
    if (!fs::exists(root, ec)) return 0;
    std::uintmax_t total = 0;
    for (auto it = fs::recursive_directory_iterator(root, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        std::error_code fec;
        if (it->is_regular_file(fec)) total += it->file_size(fec);
    }
    return total;
}

std::uintmax_t TileCache::purge_disk_cache() {
    std::uintmax_t freed = disk_cache_size();
    fs::path root = disk_cache_root();
    std::error_code ec;
    fs::remove_all(root, ec);            // best-effort; skips files held open
    fs::create_directories(root, ec);   // leave an empty root for new writes
    LOG_INFO("purged tile cache: {} bytes freed", freed);
    return freed;
}

TileCache::TileCache(TileSource source) : source_(std::move(source)) {
    cache_dir_ = disk_cache_root() / source_slug(source_.name);
    std::error_code ec;
    fs::create_directories(cache_dir_, ec);
}

std::string TileCache::source_slug(const std::string& name) {
    std::string s;
    for (unsigned char c : name) {
        if (std::isalnum(c)) s += (char)std::tolower(c);
        else if (!s.empty() && s.back() != '_') s += '_';
    }
    while (!s.empty() && s.back() == '_') s.pop_back();
    return s;
}

fs::path TileCache::disk_cache_path(const TileCoord& c) const {
    return cache_dir_ / std::to_string(c.z) / std::to_string(c.x)
                      / (std::to_string(c.y) + ".dat");
}

int TileCache::cache_max_age_days() const {
    if (source_.cache_days != 0) return source_.cache_days;
    switch (source_.category) {
        case TileCategory::Satellite: return 30;
        case TileCategory::Topo:      return 14;
        case TileCategory::Street:    return 7;
        case TileCategory::Overlay:   return 7;
    }
    return 7;
}

bool TileCache::disk_cache_stale(const fs::path& p) const {
    std::error_code ec;
    auto mtime = fs::last_write_time(p, ec);
    if (ec) return true; // missing → must fetch
    int max_days = cache_max_age_days();
    if (max_days < 0) return false; // never expires
    auto age_hours = std::chrono::duration_cast<std::chrono::hours>(
        fs::file_time_type::clock::now() - mtime).count();
    return age_hours / 24 >= max_days;
}

TileCoord TileCache::lat_lon_to_tile(double lat, double lon, int zoom) {
    int n = 1 << zoom;
    double lat_r = lat * PI / 180.0;
    int x = (int)std::floor((lon + 180.0) / 360.0 * n);
    int y = (int)std::floor((1.0 - std::log(std::tan(lat_r) + 1.0/std::cos(lat_r)) / PI) / 2.0 * n);
    return {zoom, clamp_tile_x(x, zoom), std::clamp(y, 0, n - 1)};
}

void TileCache::tile_to_lat_lon(const TileCoord& t,
                                 double& top_lat, double& left_lon,
                                 double& bot_lat, double& right_lon) {
    int n = 1 << t.z;
    left_lon  = (double)t.x       / n * 360.0 - 180.0;
    right_lon = (double)(t.x + 1) / n * 360.0 - 180.0;

    auto tile_lat = [&](int ty) {
        double s = std::sinh(PI * (1.0 - 2.0 * ty / n));
        return std::atan(s) * 180.0 / PI;
    };
    top_lat = tile_lat(t.y);
    bot_lat = tile_lat(t.y + 1);
}

// Web Mercator (EPSG:3857) extent of a slippy tile, in metres.
static void tile_to_mercator(const TileCoord& c,
                             double& minx, double& miny,
                             double& maxx, double& maxy) {
    // Half the equatorial circumference: pi * R, R = 6378137 m.
    constexpr double ORIGIN = PI * 6378137.0;          // 20037508.342789244
    double world = 2.0 * ORIGIN;
    double span  = world / (double)(1 << c.z);          // metres per tile
    minx = -ORIGIN + c.x       * span;
    maxx = -ORIGIN + (c.x + 1) * span;
    // Slippy y grows southward; Mercator northing grows northward.
    maxy =  ORIGIN - c.y       * span;
    miny =  ORIGIN - (c.y + 1) * span;
}

std::string TileCache::build_url(const TileCoord& c) const {
    std::string url = source_.url_template;
    auto replace = [&](const std::string& token, const std::string& val) {
        for (size_t pos; (pos = url.find(token)) != std::string::npos;)
            url.replace(pos, token.size(), val);
    };

    if (source_.wms) {
        // WMS GetMap: substitute the tile's Mercator bbox + raster size + CRS.
        double minx, miny, maxx, maxy;
        tile_to_mercator(c, minx, miny, maxx, maxy);
        // WMS 1.3.0 with a projected CRS (EPSG:3857) uses x,y axis order:
        // BBOX = minx,miny,maxx,maxy.
        replace("{bbox}", std::format("{:.9f},{:.9f},{:.9f},{:.9f}",
                                      minx, miny, maxx, maxy));
        replace("{width}",  "256");
        replace("{height}", "256");
        replace("{proj}",   source_.crs);
    }

    replace("{z}", std::to_string(c.z));
    replace("{x}", std::to_string(c.x));
    replace("{y}", std::to_string(c.y));
    replace("{key}", source_.key); // empty if key not set
    return url;
}

TileState TileCache::request(const TileCoord& coord) {
    std::scoped_lock lk(mu_);
    auto it = tiles_.find(coord);
    if (it != tiles_.end()) {
        if (it->second.state == TileState::Error) {
            auto elapsed = std::chrono::steady_clock::now() - it->second.error_time;
            if (elapsed > std::chrono::seconds(10)) {
                it->second.state = TileState::Loading;
                fetch_async(coord);
            }
        }
        return it->second.state;
    }

    Tile t; t.coord = coord; t.state = TileState::Loading;
    tiles_.emplace(coord, std::move(t));
    fetch_async(coord);
    return TileState::Loading;
}

void TileCache::fetch_async(TileCoord coord) {
    std::string url = build_url(coord);
    fs::path cache_path = disk_cache_path(coord);
    std::vector<std::pair<std::string,std::string>> hdrs;
    if (!source_.referer.empty())
        hdrs.push_back({"Referer", source_.referer});

    net::post_async([this, coord, url, cache_path,
                     hdrs = std::move(hdrs)]() mutable {
        const int min_bytes = source_.min_valid_bytes;

        // 1. Try disk cache (no mutex, file I/O off main thread). A cached file
        //    smaller than min_bytes is a stale blank from before this guard
        //    existed — ignore it and re-fetch, which overwrites it with real
        //    imagery (auto-heals previously-cached load-shed tiles).
        if (!disk_cache_stale(cache_path)) {
            std::ifstream f(cache_path, std::ios::binary);
            if (f) {
                std::vector<uint8_t> bytes(
                    (std::istreambuf_iterator<char>(f)),
                    std::istreambuf_iterator<char>());
                if (!bytes.empty() && (int)bytes.size() >= min_bytes) {
                    std::scoped_lock lk(mu_);
                    auto it = tiles_.find(coord);
                    if (it != tiles_.end()) {
                        it->second.png   = std::move(bytes);
                        it->second.state = TileState::Ready;
                    }
                    return;
                }
            }
        }

        // 2. Fetch from network — throttled to kMaxConcurrentFetches and
        //    retried with backoff on transient failures. The gate is held only
        //    for the network round-trip (disk-cache hits above never wait).
        std::expected<net::Response, net::HttpError> resp;
        {
            GateGuard gate;
            resp = fetch_with_retry(url, hdrs, coord, min_bytes);
        }
        if (!resp) {
            const char* why = resp.error() == net::HttpError::Timeout     ? "timeout"
                            : resp.error() == net::HttpError::HttpError4xx ? "HTTP 4xx"
                            : resp.error() == net::HttpError::HttpError5xx ? "HTTP 5xx"
                            : "network error";
            LOG_WARN("tile fetch failed ({}) z={} x={} y={} url={}", why,
                     coord.z, coord.x, coord.y, url);
            std::scoped_lock lk(mu_);
            auto it = tiles_.find(coord);
            if (it != tiles_.end()) {
                it->second.state      = TileState::Error;
                it->second.error_time = std::chrono::steady_clock::now();
            }
            return;
        }

        // 3. Write to disk (still outside mutex — don't stall the GL thread)
        if (!resp->bytes.empty()) {
            std::error_code ec;
            fs::create_directories(cache_path.parent_path(), ec);
            if (!ec) {
                std::ofstream f(cache_path, std::ios::binary | std::ios::trunc);
                if (f)
                    f.write(reinterpret_cast<const char*>(resp->bytes.data()),
                            (std::streamsize)resp->bytes.size());
            }
        }

        // 4. Hand bytes to the in-memory cache
        std::scoped_lock lk(mu_);
        auto it = tiles_.find(coord);
        if (it == tiles_.end()) return;
        it->second.png   = std::move(resp->bytes);
        it->second.state = TileState::Ready;
    });
}

void TileCache::upload_pending() {
    // Must be called from the main (OpenGL) thread
    std::scoped_lock lk(mu_);
    for (auto& [coord, tile] : tiles_) {
        if (tile.state != TileState::Ready || tile.gl_texture != 0 || tile.png.empty())
            continue;

        int w, h, ch;
        uint8_t* data = stbi_load_from_memory(
            tile.png.data(), (int)tile.png.size(), &w, &h, &ch, 4);
        if (!data) {
            // Log the first ~80 bytes so we can tell if the server returned
            // HTML/XML instead of an image (e.g. wrong WMTS parameters).
            std::string hint;
            for (int k = 0; k < std::min((int)tile.png.size(), 80); ++k) {
                char c = (char)tile.png[k];
                hint += (c >= 0x20 && c < 0x7f) ? c : '.';
            }
            LOG_WARN("stbi decode failed z={} x={} y={} ({} bytes) head: {}",
                     coord.z, coord.x, coord.y, tile.png.size(), hint);
            tile.state = TileState::Error;
            tile.error_time = std::chrono::steady_clock::now();
            tile.png.clear();
            continue;
        }

        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
        stbi_image_free(data);

        tile.gl_texture = tex;
        tile.png.clear();
        tile.png.shrink_to_fit();
    }
}

uint32_t TileCache::texture(const TileCoord& coord) const {
    std::scoped_lock lk(mu_);
    auto it = tiles_.find(coord);
    return (it != tiles_.end()) ? it->second.gl_texture : 0u;
}

TileCache::TilePixels TileCache::read_tile_rgb(const TileCoord& coord) const {
    std::scoped_lock lk(mu_);
    auto it = tiles_.find(coord);
    if (it == tiles_.end()) return {};
    const auto& tile = it->second;

    // PNG still in memory (not yet uploaded to GL) — decode it.
    if (!tile.png.empty()) {
        int w, h, ch;
        uint8_t* px = stbi_load_from_memory(
            tile.png.data(), (int)tile.png.size(), &w, &h, &ch, 3);
        if (!px) return {};
        TilePixels tp;
        tp.rgb.assign(px, px + (size_t)w * h * 3);
        tp.w = w; tp.h = h;
        stbi_image_free(px);
        return tp;
    }

    // Already on the GPU — read back via glGetTexImage (requires GL thread).
    if (!tile.gl_texture) return {};
    glBindTexture(GL_TEXTURE_2D, tile.gl_texture);
    GLint w = 0, h = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
    if (w <= 0 || h <= 0) return {};
    TilePixels tp;
    tp.w = w; tp.h = h;
    tp.rgb.resize((size_t)w * h * 3);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGB, GL_UNSIGNED_BYTE, tp.rgb.data());
    return tp;
}

void TileCache::clear() {
    std::scoped_lock lk(mu_);
    for (auto& [coord, tile] : tiles_)
        if (tile.gl_texture) glDeleteTextures(1, &tile.gl_texture);
    tiles_.clear();
}

} // namespace map
