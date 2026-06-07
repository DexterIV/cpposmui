#include "tile_cache.hpp"
#include "../net/http.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <glad/gl.h>   // glad2 header – must come before any other GL include

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>

namespace map {

namespace {
constexpr double PI = std::numbers::pi;

int clamp_tile_x(int v, int z) {
    int n = 1 << z;
    return (v % n + n) % n;
}
} // anonymous

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
    net::fetch_tile_async(url, [this, coord](auto resp) {
        std::scoped_lock lk(mu_);
        auto it = tiles_.find(coord);
        if (it == tiles_.end()) return;
        if (!resp) {
            it->second.state = TileState::Error;
            it->second.error_time = std::chrono::steady_clock::now();
            return;
        }
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
