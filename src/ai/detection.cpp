#include "detection.hpp"
#include "../net/http.hpp"
#include "../osm/parser.hpp"
#include "../log.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <numbers>
#include <filesystem>
#include <fstream>
#include <format>
#include <algorithm>
#include <unordered_set>
#include <string>
#include <cstring>
#include <expected>
#include <thread>
#include <chrono>

#include <stb_image.h>   // declarations only; implementation lives in tile_cache.cpp

#ifdef CPPOSMUI_HAVE_ONNXRUNTIME
#include <onnxruntime_cxx_api.h>
#ifdef _WIN32
#include <windows.h>     // MultiByteToWideChar — ORT wants wchar_t paths on Windows
#endif
#endif

namespace ai {

using json = nlohmann::json;

// ── Bing / Slippy quadkey helpers ────────────────────────────────────────────
namespace {

constexpr double QUAD_PI = std::numbers::pi;

// Standard slippy-map lat/lon → tile x,y at zoom z.
static void lat_lon_to_tile(double lat, double lon, int z, int& tx, int& ty) {
    int n = 1 << z;
    double lat_r = lat * QUAD_PI / 180.0;
    tx = (int)std::floor((lon + 180.0) / 360.0 * n);
    ty = (int)std::floor((1.0 - std::log(std::tan(lat_r) + 1.0/std::cos(lat_r))
                          / QUAD_PI) / 2.0 * n);
    tx = std::max(0, std::min(tx, n - 1));
    ty = std::max(0, std::min(ty, n - 1));
}

// Tile x,y,z → Bing Maps quadkey string.
static std::string tile_to_quadkey(int tx, int ty, int z) {
    std::string qk;
    qk.reserve(z);
    for (int i = z; i > 0; --i) {
        char digit = '0';
        int  mask  = 1 << (i - 1);
        if (tx & mask) digit += 1;
        if (ty & mask) digit += 2;
        qk += digit;
    }
    return qk;
}

// Collect all quadkeys at zoom level z whose tiles overlap the bbox.
static std::vector<std::string>
bbox_to_quadkeys(double min_lat, double min_lon,
                 double max_lat, double max_lon, int z)
{
    int tx0, ty0, tx1, ty1;
    lat_lon_to_tile(max_lat, min_lon, z, tx0, ty0); // NW corner
    lat_lon_to_tile(min_lat, max_lon, z, tx1, ty1); // SE corner
    if (tx0 > tx1) std::swap(tx0, tx1);
    if (ty0 > ty1) std::swap(ty0, ty1);

    std::vector<std::string> keys;
    for (int ty = ty0; ty <= ty1; ++ty)
        for (int tx = tx0; tx <= tx1; ++tx)
            keys.push_back(tile_to_quadkey(tx, ty, z));
    return keys;
}

} // anon

// ── GeoJSON helpers ───────────────────────────────────────────────────────────
namespace {

static FeatureType tag_map_type(const osm::TagMap& tags) {
    if (tags.contains("building"))  return FeatureType::Building;
    if (tags.contains("highway"))   return FeatureType::Road;
    if (tags.contains("waterway"))  return FeatureType::Waterway;
    if (tags.contains("landuse"))   return FeatureType::Landuse;
    if (tags.contains("amenity") || tags.contains("shop") || tags.contains("tourism"))
        return FeatureType::POI;
    return FeatureType::Unknown;
}

// Parse a GeoJSON ring [[lon,lat], ...] into (lat,lon) pairs.
static std::vector<std::pair<double,double>> parse_ring(const json& ring) {
    std::vector<std::pair<double,double>> out;
    out.reserve(ring.size());
    for (const auto& pt : ring)
        if (pt.is_array() && pt.size() >= 2)
            out.emplace_back(pt[1].get<double>(), pt[0].get<double>()); // lat, lon
    return out;
}

// Parse a single GeoJSON Feature into zero or more DetectedFeatures.
static void parse_geojson_feature(const json& feat,
                                   std::vector<DetectedFeature>& out,
                                   FeatureType default_type,
                                   float default_confidence)
{
    if (!feat.contains("geometry")) return;
    const auto& geom = feat["geometry"];
    std::string geom_type = geom.value("type", "");

    // Extract confidence from properties if present.
    float conf = default_confidence;
    if (feat.contains("properties") && feat["properties"].is_object())
        conf = feat["properties"].value("confidence", conf);

    // Build suggested tags.
    osm::TagMap tags;
    if (feat.contains("properties") && feat["properties"].is_object()) {
        for (const auto& [k, v] : feat["properties"].items()) {
            if (k == "confidence" || k == "height" || k == "area") continue;
            if (v.is_string()) tags[k] = v.get<std::string>();
        }
    }
    if (default_type == FeatureType::Building && !tags.contains("building"))
        tags["building"] = "yes";

    auto emit_coords = [&](std::vector<std::pair<double,double>> coords, bool area) {
        if (coords.size() < 2) return;
        DetectedFeature f;
        f.type           = tags.empty() ? default_type : tag_map_type(tags);
        if (f.type == FeatureType::Unknown) f.type = default_type;
        f.confidence     = std::clamp(conf, 0.0f, 1.0f);
        f.coords         = std::move(coords);
        f.is_area        = area;
        f.suggested_tags = tags;
        out.push_back(std::move(f));
    };

    if (geom_type == "Polygon" && geom.contains("coordinates")) {
        emit_coords(parse_ring(geom["coordinates"][0]), true);
    } else if (geom_type == "MultiPolygon" && geom.contains("coordinates")) {
        for (const auto& poly : geom["coordinates"])
            if (!poly.empty()) emit_coords(parse_ring(poly[0]), true);
    } else if (geom_type == "LineString" && geom.contains("coordinates")) {
        emit_coords(parse_ring(geom["coordinates"]), false);
    } else if (geom_type == "MultiLineString" && geom.contains("coordinates")) {
        for (const auto& line : geom["coordinates"])
            emit_coords(parse_ring(line), false);
    } else if (geom_type == "Point" && geom.contains("coordinates")) {
        const auto& c = geom["coordinates"];
        if (c.is_array() && c.size() >= 2) {
            DetectedFeature f;
            f.type           = default_type;
            f.confidence     = conf;
            f.is_area        = false;
            f.coords.emplace_back(c[1].get<double>(), c[0].get<double>());
            f.suggested_tags = tags;
            out.push_back(std::move(f));
        }
    }
}

// Parse a full GeoJSON FeatureCollection string.
static void parse_geojson_collection(const std::string& body,
                                      std::vector<DetectedFeature>& out,
                                      FeatureType default_type,
                                      float default_confidence)
{
    auto j = json::parse(body, nullptr, /*exceptions=*/false);
    if (j.is_discarded()) return;

    // Some services return a bare array instead of a FeatureCollection.
    const json* features_ptr = nullptr;
    if (j.contains("features"))
        features_ptr = &j["features"];
    else if (j.is_array())
        features_ptr = &j;
    if (!features_ptr) return;

    for (const auto& feat : *features_ptr)
        parse_geojson_feature(feat, out, default_type, default_confidence);
}

// Convert an osm::Dataset (e.g. from MapWithAI XML response) to DetectedFeatures.
static void osm_dataset_to_features(const osm::Dataset& ds,
                                     std::vector<DetectedFeature>& out)
{
    // Tagged standalone nodes → POIs
    for (const auto& [id, n] : ds.nodes) {
        if (n.tags.empty()) continue;
        DetectedFeature f;
        f.type           = tag_map_type(n.tags);
        f.confidence     = 1.0f;
        f.is_area        = false;
        f.suggested_tags = n.tags;
        f.coords.emplace_back(n.lat, n.lon);
        out.push_back(std::move(f));
    }
    // Ways → roads, buildings, waterways, landuse
    for (const auto& [id, w] : ds.ways) {
        if (w.nodes.empty()) continue;
        DetectedFeature f;
        f.type           = tag_map_type(w.tags);
        f.confidence     = 1.0f;
        f.suggested_tags = w.tags;
        bool closed = w.nodes.size() >= 4 &&
                      w.nodes.front().ref == w.nodes.back().ref;
        f.is_area = closed && (f.type == FeatureType::Building ||
                                f.type == FeatureType::Landuse ||
                                w.tags.contains("area"));
        for (const auto& wn : w.nodes) {
            auto it = ds.nodes.find(wn.ref);
            if (it != ds.nodes.end())
                f.coords.emplace_back(it->second.lat, it->second.lon);
        }
        if (f.coords.size() >= 2) out.push_back(std::move(f));
    }
}

} // anon

// ── Provider implementations ──────────────────────────────────────────────────

// Microsoft Global Building Footprints
// Data: https://github.com/microsoft/GlobalMLBuildingFootprints
// Blob: https://minedbuildings.blob.core.windows.net/globalbuildings/{quadkey}.geojson.gz
// libcurl's Accept-Encoding: "" handles the gzip decompression transparently.
void detect_ms_buildings(double min_lat, double min_lon,
                          double max_lat, double max_lon,
                          Callback cb)
{
    LOG_INFO("detect_ms_buildings: bbox {:.5f},{:.5f}..{:.5f},{:.5f}",
             min_lat, min_lon, max_lat, max_lon);
    net::post_async([=, cb = std::move(cb)] mutable {
        // Clamp area: refuse requests > ~1° × 1°  (zoom 9 → ~150×150 km tiles)
        constexpr int QUAD_ZOOM = 9;
        auto keys = bbox_to_quadkeys(min_lat, min_lon, max_lat, max_lon, QUAD_ZOOM);
        constexpr int MAX_TILES = 9; // 3×3 grid max
        if (keys.size() > MAX_TILES) {
            LOG_WARN("detect_ms_buildings: {} quadkeys exceeds cap", keys.size());
            cb({{}, "MS Buildings", "Zoom in — requested area spans too many tiles"});
            return;
        }

        DetectionResult result;
        result.source_name = "Microsoft Building Footprints";
        LOG_INFO("detect_ms_buildings: fetching {} quadkey blob(s)", keys.size());

        for (const auto& qk : keys) {
            std::string url =
                "https://minedbuildings.blob.core.windows.net/globalbuildings/"
                + qk + ".geojson.gz";
            auto resp = net::get(url, 30);
            if (!resp) {
                // A 404 just means no buildings for this quadkey — skip silently.
                if (resp.error() == net::HttpError::HttpError4xx) continue;
                result.error = "Network error fetching quadkey " + qk;
                break;
            }
            parse_geojson_collection(resp->body, result.features,
                                     FeatureType::Building, 1.0f);
        }

        // Clip results to the actual requested bbox (quadkey tiles are larger).
        result.features.erase(
            std::remove_if(result.features.begin(), result.features.end(),
                [&](const DetectedFeature& f) {
                    if (f.coords.empty()) return true;
                    // Keep if any vertex falls inside the bbox.
                    for (const auto& [lat, lon] : f.coords)
                        if (lat >= min_lat && lat <= max_lat &&
                            lon >= min_lon && lon <= max_lon) return false;
                    return true;
                }),
            result.features.end());

        for (auto& f : result.features) f.source = "msft-buildings";
        if (result.features.empty() && result.error.empty())
            result.error = "No building footprints found for this area "
                           "(dataset may not cover this region yet)";
        if (result.error.empty())
            LOG_INFO("detect_ms_buildings: {} building(s) in bbox", result.features.size());
        else
            LOG_WARN("detect_ms_buildings: {}", result.error);
        cb(std::move(result));
    });
}

// Facebook / Meta MapWithAI roads
// Endpoint: https://mapwith.ai/maps/ml_roads (public, no key required)
// Returns OSM XML which we parse with the existing osm::load_osm path.
void detect_mapwithai_roads(double min_lat, double min_lon,
                             double max_lat, double max_lon,
                             Callback cb)
{
    LOG_INFO("detect_mapwithai_roads: bbox {:.5f},{:.5f}..{:.5f},{:.5f}",
             min_lat, min_lon, max_lat, max_lon);
    net::post_async([=, cb = std::move(cb)] mutable {
        std::string url = std::format(
            "https://mapwith.ai/maps/ml_roads"
            "?bbox={:.6f},{:.6f},{:.6f},{:.6f}"
            "&result_type=road_vector_xml"
            "&sources=ESRI+imagery",
            min_lon, min_lat, max_lon, max_lat);

        auto resp = net::get(url, 45);
        DetectionResult result;
        result.source_name = "MapWithAI Roads";

        if (!resp) {
            result.error = "Network error contacting MapWithAI";
            LOG_WARN("detect_mapwithai_roads: network error");
            cb(std::move(result)); return;
        }
        if (resp->status_code == 400) {
            result.error = "MapWithAI: bad request (bbox too large or unsupported region?)";
            LOG_WARN("detect_mapwithai_roads: HTTP 400 (bbox/region)");
            cb(std::move(result)); return;
        }
        if (resp->status_code != 200) {
            result.error = std::format("MapWithAI returned HTTP {}", resp->status_code);
            LOG_WARN("detect_mapwithai_roads: HTTP {}", resp->status_code);
            cb(std::move(result)); return;
        }

        // Write to temp file and parse as OSM XML.
        auto tmp = std::filesystem::temp_directory_path() / "cpposmui_mapwithai.osm";
        {
            std::ofstream f(tmp, std::ios::binary);
            f.write(reinterpret_cast<const char*>(resp->bytes.data()),
                    (std::streamsize)resp->bytes.size());
        }
        auto ds = osm::load_osm(tmp);
        std::filesystem::remove(tmp);

        if (!ds) {
            result.error = "Failed to parse MapWithAI response";
            LOG_WARN("detect_mapwithai_roads: parse failed ({} bytes)", resp->bytes.size());
            cb(std::move(result)); return;
        }
        osm_dataset_to_features(*ds, result.features);
        for (auto& f : result.features) f.source = "mapwithai";
        if (result.features.empty())
            result.error = "MapWithAI returned no features for this area";
        LOG_INFO("detect_mapwithai_roads: {} feature(s)", result.features.size());
        cb(std::move(result));
    });
}

// Generic configurable REST endpoint → GeoJSON FeatureCollection
void detect_custom_rest(const std::string& url_template,
                         double min_lat, double min_lon,
                         double max_lat, double max_lon,
                         Callback cb)
{
    LOG_INFO("detect_custom_rest: bbox {:.5f},{:.5f}..{:.5f},{:.5f}",
             min_lat, min_lon, max_lat, max_lon);
    net::post_async([=, cb = std::move(cb)] mutable {
        std::string bbox = std::format("{:.6f},{:.6f},{:.6f},{:.6f}",
                                       min_lon, min_lat, max_lon, max_lat);
        std::string url = url_template;
        // Replace {bbox} placeholder
        for (size_t pos; (pos = url.find("{bbox}")) != std::string::npos;)
            url.replace(pos, 6, bbox);

        auto resp = net::get(url, 60);
        DetectionResult result;
        result.source_name = "Custom REST";

        if (!resp) {
            result.error = "Network error";
            LOG_WARN("detect_custom_rest: network error for {}", url);
            cb(std::move(result)); return;
        }
        if (resp->status_code != 200) {
            result.error = std::format("HTTP {}", resp->status_code);
            LOG_WARN("detect_custom_rest: HTTP {} for {}", resp->status_code, url);
            cb(std::move(result)); return;
        }
        parse_geojson_collection(resp->body, result.features,
                                  FeatureType::Unknown, 1.0f);
        for (auto& f : result.features) f.source = "custom-rest";
        if (result.features.empty() && result.error.empty())
            result.error = "No features returned";
        LOG_INFO("detect_custom_rest: {} feature(s)", result.features.size());
        cb(std::move(result));
    });
}

// ── Image processing helpers (shared by CV and ONNX pipelines) ───────────────
namespace {

// Pixel (col x, row y) in a W×H raster that covers a Web-Mercator-uniform tile
// extent → (lat, lon).  Longitude is linear in Mercator; latitude is not, so we
// interpolate in the projected northing and invert.  Using the linear-in-degrees
// shortcut here (as the old code did) skews every detection northward.
static std::pair<double,double> merc_px_to_geo(
        double x, double y, int W, int H,
        double min_lat, double min_lon, double max_lat, double max_lon) {
    auto ymerc = [](double lat) {
        double r = lat * QUAD_PI / 180.0;
        return std::log(std::tan(r) + 1.0 / std::cos(r));
    };
    double lon = min_lon + (x / W) * (max_lon - min_lon);
    double yt  = ymerc(max_lat), yb = ymerc(min_lat);
    double ym  = yt + (y / H) * (yb - yt);
    double lat = (2.0 * std::atan(std::exp(ym)) - QUAD_PI / 2.0) * 180.0 / QUAD_PI;
    return {lat, lon};
}

// 8-connected component labelling.  Fills `labels` (0 = background) and returns
// the number of components found.
static int connected_components(const std::vector<uint8_t>& mask, int W, int H,
                                std::vector<int>& labels) {
    labels.assign((size_t)W * H, 0);
    int next = 0;
    std::vector<int> stack;
    for (int i = 0; i < W * H; ++i) {
        if (mask[i] <= 127 || labels[i] != 0) continue;
        ++next;
        stack.clear(); stack.push_back(i); labels[i] = next;
        while (!stack.empty()) {
            int p = stack.back(); stack.pop_back();
            int px = p % W, py = p / W;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    if (!dx && !dy) continue;
                    int nx = px + dx, ny = py + dy;
                    if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
                    int np = ny * W + nx;
                    if (mask[np] > 127 && labels[np] == 0) {
                        labels[np] = next; stack.push_back(np);
                    }
                }
        }
    }
    return next;
}

// Moore-neighbour boundary trace of one labelled component → ordered pixel ring.
static std::vector<std::pair<int,int>> trace_contour(
        const std::vector<int>& labels, int W, int H, int target) {
    std::vector<std::pair<int,int>> ring;
    int start = -1;
    for (int i = 0; i < W * H && start < 0; ++i) if (labels[i] == target) start = i;
    if (start < 0) return ring;

    // 8 directions, clockwise, starting from "west".
    const int dx8[8] = {-1,-1, 0, 1, 1, 1, 0,-1};
    const int dy8[8] = { 0,-1,-1,-1, 0, 1, 1, 1};
    auto is = [&](int x, int y) {
        return x >= 0 && y >= 0 && x < W && y < H && labels[y * W + x] == target;
    };
    int sx = start % W, sy = start / W;
    int cx = sx, cy = sy, bdir = 0;
    int guard = 0, maxsteps = 8 * W * H;
    do {
        ring.emplace_back(cx, cy);
        int found = -1;
        for (int k = 0; k < 8; ++k) {
            int d  = (bdir + k) % 8;
            int nx = cx + dx8[d], ny = cy + dy8[d];
            if (is(nx, ny)) { found = d; cx = nx; cy = ny; break; }
        }
        if (found < 0) break;          // isolated pixel
        bdir = (found + 6) % 8;        // back up to keep hugging the boundary
    } while ((cx != sx || cy != sy) && ++guard < maxsteps);
    return ring;
}

// Douglas-Peucker polyline simplification (epsilon in pixels).
static void dp_simplify(const std::vector<std::pair<int,int>>& in, double eps,
                        std::vector<std::pair<int,int>>& out) {
    if (in.size() < 3) { out = in; return; }
    std::vector<char> keep(in.size(), 0);
    keep.front() = keep.back() = 1;
    std::vector<std::pair<int,int>> st = {{0, (int)in.size() - 1}};
    while (!st.empty()) {
        auto [a, b] = st.back(); st.pop_back();
        double ax = in[a].first, ay = in[a].second;
        double dxl = (double)in[b].first - ax, dyl = (double)in[b].second - ay;
        double len = std::hypot(dxl, dyl);
        double maxd = -1; int idx = -1;
        for (int i = a + 1; i < b; ++i) {
            double px = in[i].first, py = in[i].second, d;
            if (len < 1e-9) d = std::hypot(px - ax, py - ay);
            else d = std::fabs((px - ax) * dyl - (py - ay) * dxl) / len;
            if (d > maxd) { maxd = d; idx = i; }
        }
        if (maxd > eps && idx > 0) {
            keep[idx] = 1; st.push_back({a, idx}); st.push_back({idx, b});
        }
    }
    for (size_t i = 0; i < in.size(); ++i) if (keep[i]) out.push_back(in[i]);
}

// Zhang-Suen thinning: reduce a binary mask to a 1-pixel skeleton in place.
static void zhang_suen(std::vector<uint8_t>& img, int W, int H) {
    auto val = [&](int x, int y) {
        return (x < 0 || y < 0 || x >= W || y >= H) ? 0 : (img[y * W + x] > 127 ? 1 : 0);
    };
    std::vector<std::pair<int,int>> rm;
    bool changed = true;
    while (changed) {
        changed = false;
        for (int step = 0; step < 2; ++step) {
            rm.clear();
            for (int y = 1; y < H - 1; ++y)
                for (int x = 1; x < W - 1; ++x) {
                    if (!val(x, y)) continue;
                    int p2 = val(x, y-1), p3 = val(x+1, y-1), p4 = val(x+1, y),
                        p5 = val(x+1, y+1), p6 = val(x, y+1), p7 = val(x-1, y+1),
                        p8 = val(x-1, y), p9 = val(x-1, y-1);
                    int B = p2+p3+p4+p5+p6+p7+p8+p9;
                    if (B < 2 || B > 6) continue;
                    int seq[9] = {p2,p3,p4,p5,p6,p7,p8,p9,p2};
                    int A = 0;
                    for (int i = 0; i < 8; ++i) if (seq[i] == 0 && seq[i+1] == 1) ++A;
                    if (A != 1) continue;
                    if (step == 0) { if (p2*p4*p6) continue; if (p4*p6*p8) continue; }
                    else           { if (p2*p4*p8) continue; if (p2*p6*p8) continue; }
                    rm.emplace_back(x, y);
                }
            for (auto& [x, y] : rm) img[y * W + x] = 0;
            if (!rm.empty()) changed = true;
        }
    }
}

// Walk a 1-px skeleton into ordered polylines (junctions split into segments).
static void skeleton_to_polylines(
        const std::vector<uint8_t>& sk, int W, int H,
        std::vector<std::vector<std::pair<int,int>>>& paths) {
    auto on = [&](int x, int y) {
        return x >= 0 && y >= 0 && x < W && y < H && sk[y * W + x] > 127;
    };
    const int dx8[8] = {-1, 0, 1,-1, 1,-1, 0, 1};
    const int dy8[8] = {-1,-1,-1, 0, 0, 1, 1, 1};
    auto deg = [&](int x, int y) {
        int d = 0; for (int k = 0; k < 8; ++k) if (on(x+dx8[k], y+dy8[k])) ++d; return d;
    };
    std::vector<uint8_t> visited((size_t)W * H, 0);
    std::vector<std::pair<int,int>> starts;
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x)
        if (on(x, y) && deg(x, y) == 1) starts.emplace_back(x, y); // endpoints first
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x)
        if (on(x, y)) starts.emplace_back(x, y);                   // then loops
    for (auto& [sx, sy] : starts) {
        if (visited[sy * W + sx]) continue;
        std::vector<std::pair<int,int>> path;
        int cx = sx, cy = sy;
        while (true) {
            visited[cy * W + cx] = 1; path.emplace_back(cx, cy);
            int nx = -1, ny = -1;
            for (int k = 0; k < 8; ++k) {
                int x = cx + dx8[k], y = cy + dy8[k];
                if (on(x, y) && !visited[y * W + x]) { nx = x; ny = y; break; }
            }
            if (nx < 0) break;
            cx = nx; cy = ny;
        }
        if (path.size() >= 2) paths.push_back(std::move(path));
    }
}

// Drop vertices closer than min_d px to their predecessor (collapses the
// "multiple points on the same node" clusters the contour tracer leaves behind).
// For closed rings, also remove a near-coincident closing vertex.
static void dedupe_consecutive(std::vector<std::pair<int,int>>& r,
                               double min_d, bool closed) {
    if (r.size() < 2) return;
    std::vector<std::pair<int,int>> out;
    out.reserve(r.size());
    for (auto& p : r)
        if (out.empty() ||
            std::hypot(p.first - out.back().first,
                       p.second - out.back().second) >= min_d)
            out.push_back(p);
    if (closed)
        while (out.size() >= 2 &&
               std::hypot(out.front().first - out.back().first,
                          out.front().second - out.back().second) < min_d)
            out.pop_back();
    r.swap(out);
}

} // anon (image processing helpers)

// ── Classical CV core (synchronous; fills result) ────────────────────────────

static void run_cv_sync(const std::vector<uint8_t>& rgb_pixels,
                        int img_w, int img_h,
                        double min_lat, double min_lon,
                        double max_lat, double max_lon,
                        DetectionResult& result,
                        double simplify_px = 3.5)
{
        const int W = img_w, H = img_h;
        const int64_t HW = (int64_t)H * W;
        if (HW == 0 || (int64_t)rgb_pixels.size() < HW * 3) {
            result.error = "Invalid imagery dimensions";
            LOG_WARN("run_cv_sync: invalid imagery {}x{} ({} bytes)",
                     W, H, rgb_pixels.size());
            return;
        }
        LOG_INFO("run_cv_sync: {}x{} px, simplify={:.1f}px, bbox {:.5f},{:.5f}..{:.5f},{:.5f}",
                 W, H, simplify_px, min_lat, min_lon, max_lat, max_lon);
        const size_t feat_before = result.features.size();

        // Acceptance thresholds — tuned to suppress the false positives classical
        // edge detection throws off on real aerial imagery (vegetation, shadows,
        // driveways traced as "buildings"; every grey blob skeletonised as a
        // "road"). The rejection breakdown is logged at the end of the function.
        constexpr int    BLDG_MIN_AREA   = 60;     // px² — drop tiny fragments
        constexpr double BLDG_MAX_ASPECT = 8.0;    // long:short side ratio
        constexpr double BLDG_MIN_EXTENT = 0.55;   // pixel area / bbox area (solidity)
        constexpr double BLDG_MAX_VAR    = 3000.0; // colour variance (also drives confidence)
        constexpr size_t BLDG_MAX_VERTS  = 14;     // simple footprints have few corners
        constexpr double ROAD_MIN_LEN    = 45.0;   // px — drop short skeleton spurs
        int rej_area = 0, rej_aspect = 0, rej_extent = 0, rej_var = 0, rej_shape = 0;

        std::vector<uint8_t> gray((size_t)HW);
        for (int64_t i = 0; i < HW; ++i)
            gray[i] = (uint8_t)(0.299 * rgb_pixels[i*3]
                              + 0.587 * rgb_pixels[i*3+1]
                              + 0.114 * rgb_pixels[i*3+2]);

        std::vector<uint8_t> edges((size_t)HW, 0);
        for (int y = 1; y < H - 1; ++y)
            for (int x = 1; x < W - 1; ++x) {
                auto g = [&](int dx, int dy) -> int {
                    return gray[(y+dy)*W + (x+dx)]; };
                int gx = -g(-1,-1) - 2*g(-1,0) - g(-1,1)
                       +  g( 1,-1) + 2*g( 1,0) + g( 1,1);
                int gy = -g(-1,-1) - 2*g(0,-1) - g( 1,-1)
                       +  g(-1, 1) + 2*g(0, 1) + g( 1, 1);
                edges[y*W + x] = (uint8_t)std::min(
                    (int)std::sqrt((double)(gx*gx + gy*gy)), 255);
            }

        constexpr uint8_t EDGE_THRESH = 60;
        std::vector<uint8_t> dilated((size_t)HW, 0);
        for (int y = 1; y < H - 1; ++y)
            for (int x = 1; x < W - 1; ++x) {
                bool any = false;
                for (int dy = -1; dy <= 1 && !any; ++dy)
                    for (int dx = -1; dx <= 1 && !any; ++dx)
                        if (edges[(y+dy)*W + (x+dx)] > EDGE_THRESH) any = true;
                if (any) dilated[y*W + x] = 255;
            }

        std::vector<uint8_t> inv((size_t)HW);
        for (int64_t i = 0; i < HW; ++i) inv[i] = dilated[i] ? 0 : 255;
        for (int x = 0; x < W; ++x) { inv[x] = 0; inv[(H-1)*W + x] = 0; }
        for (int y = 0; y < H; ++y) { inv[y*W] = 0; inv[y*W + W - 1] = 0; }

        std::vector<int> labels;
        int ncomp = connected_components(inv, W, H, labels);

        auto to_geo = [&](const std::vector<std::pair<int,int>>& pts) {
            std::vector<std::pair<double,double>> out; out.reserve(pts.size());
            for (auto& [px, py] : pts)
                out.push_back(merc_px_to_geo(px + 0.5, py + 0.5, W, H,
                                             min_lat, min_lon, max_lat, max_lon));
            return out;
        };

        for (int lab = 1; lab <= ncomp; ++lab) {
            std::vector<std::pair<int,int>> pix;
            int bx0 = W, bx1 = 0, by0 = H, by1 = 0;
            for (int64_t i = 0; i < HW; ++i) {
                if (labels[i] != lab) continue;
                int px = (int)(i % W), py = (int)(i / W);
                pix.emplace_back(px, py);
                bx0 = std::min(bx0, px); bx1 = std::max(bx1, px);
                by0 = std::min(by0, py); by1 = std::max(by1, py);
            }
            int area = (int)pix.size();
            if (area < BLDG_MIN_AREA || area > HW / 4) { ++rej_area; continue; }
            int bw = bx1 - bx0 + 1, bh = by1 - by0 + 1;
            if ((double)std::max(bw, bh) / std::max(1, std::min(bw, bh)) > BLDG_MAX_ASPECT) {
                ++rej_aspect; continue;
            }
            // Solidity: a real footprint fills most of its bounding box. Sprawling
            // vegetation / shadow / driveway blobs cover a big box with few pixels
            // — this is the single biggest false-positive killer on real imagery.
            double extent = (double)area / ((double)bw * bh);
            if (extent < BLDG_MIN_EXTENT) { ++rej_extent; continue; }

            double sr = 0, sg = 0, sb = 0;
            for (auto& [x, y] : pix) {
                size_t idx = ((size_t)y * W + x) * 3;
                sr += rgb_pixels[idx]; sg += rgb_pixels[idx+1]; sb += rgb_pixels[idx+2];
            }
            double mr = sr / area, mg = sg / area, mb = sb / area;
            double var = 0;
            for (auto& [x, y] : pix) {
                size_t idx = ((size_t)y * W + x) * 3;
                double dr = rgb_pixels[idx] - mr, dg = rgb_pixels[idx+1] - mg,
                       db = rgb_pixels[idx+2] - mb;
                var += dr*dr + dg*dg + db*db;
            }
            var /= area;
            if (var > BLDG_MAX_VAR) { ++rej_var; continue; }

            auto ring = trace_contour(labels, W, H, lab);
            std::vector<std::pair<int,int>> simp;
            dp_simplify(ring, simplify_px, simp);
            dedupe_consecutive(simp, 1.5, /*closed=*/true);
            // A footprint is a simple polygon: a handful of corners. A ring that is
            // still many-sided after simplification is a noisy/concave blob, not a
            // building — reject it rather than emit a spider-shaped polygon.
            if (simp.size() < 4 || simp.size() > BLDG_MAX_VERTS) { ++rej_shape; continue; }

            DetectedFeature f;
            f.type           = FeatureType::Building;
            f.confidence     = std::clamp(1.0f - (float)(var / BLDG_MAX_VAR), 0.3f, 0.95f);
            f.is_area        = true;
            f.suggested_tags["building"] = "yes";
            f.source         = "cv-edge";
            f.coords         = to_geo(simp);
            result.features.push_back(std::move(f));
        }

        std::vector<uint8_t> road_mask((size_t)HW, 0);
        for (int64_t i = 0; i < HW; ++i) {
            uint8_t r = rgb_pixels[i*3], g = rgb_pixels[i*3+1], b = rgb_pixels[i*3+2];
            int mean = (r + g + b) / 3;
            int sat  = std::max({r, g, b}) - std::min({r, g, b});
            if (mean > 40 && mean < 180 && sat < 40) road_mask[i] = 255;
        }
        zhang_suen(road_mask, W, H);
        std::vector<std::vector<std::pair<int,int>>> paths;
        skeleton_to_polylines(road_mask, W, H, paths);

        int rej_road_len = 0;
        for (auto& path : paths) {
            std::vector<std::pair<int,int>> simp;
            dp_simplify(path, simplify_px * 1.4, simp); // roads tolerate coarser
            dedupe_consecutive(simp, 1.5, /*closed=*/false);
            if (simp.size() < 3) continue;
            double len = 0;
            for (size_t i = 1; i < simp.size(); ++i)
                len += std::hypot(simp[i].first - simp[i-1].first,
                                  simp[i].second - simp[i-1].second);
            if (len < ROAD_MIN_LEN) { ++rej_road_len; continue; }

            DetectedFeature f;
            f.type           = FeatureType::Road;
            f.confidence     = 0.6f;
            f.is_area        = false;
            f.suggested_tags["highway"] = "unclassified";
            f.source         = "cv-edge";
            f.coords         = to_geo(simp);
            result.features.push_back(std::move(f));
        }

        {
            size_t added = result.features.size() - feat_before;
            int n_bldg = 0, n_road = 0;
            for (size_t i = feat_before; i < result.features.size(); ++i)
                (result.features[i].type == FeatureType::Building ? n_bldg : n_road)++;
            LOG_INFO("run_cv_sync: {} feature(s) — {} building(s), {} road(s); "
                     "rejected buildings[area:{} aspect:{} extent:{} var:{} shape:{}] "
                     "roads[short:{}]",
                     added, n_bldg, n_road,
                     rej_area, rej_aspect, rej_extent, rej_var, rej_shape, rej_road_len);
        }

        if (result.features.empty() && result.error.empty()) {
            result.error = "CV detected no features in this imagery";
            LOG_WARN("run_cv_sync: no features detected");
        }
}

// ── Classical CV + LiDAR DSM core (synchronous; fills result) ────────────────

static void run_cv_dsm_sync(const std::vector<uint8_t>& rgb_pixels,
                            const std::vector<uint8_t>& dsm_gray,
                            int img_w, int img_h,
                            double min_lat, double min_lon,
                            double max_lat, double max_lon,
                            DetectionResult& result,
                            double simplify_px = 3.5)
{
        const int W = img_w, H = img_h;
        const int64_t HW = (int64_t)H * W;
        if (HW == 0 || (int64_t)rgb_pixels.size() < HW * 3 ||
            (int64_t)dsm_gray.size() < HW) {
            result.error = "Invalid imagery / DSM dimensions";
            LOG_WARN("run_cv_dsm_sync: invalid input {}x{} (rgb {} / dsm {} bytes)",
                     W, H, rgb_pixels.size(), dsm_gray.size());
            return;
        }
        LOG_INFO("run_cv_dsm_sync: {}x{} px + DSM, simplify={:.1f}px", W, H, simplify_px);
        const size_t feat_before = result.features.size();

        // Same acceptance thresholds as run_cv_sync (see notes there). The DSM path
        // is permitted slightly higher colour variance when height evidence agrees.
        constexpr int    BLDG_MIN_AREA   = 60;
        constexpr double BLDG_MAX_ASPECT = 8.0;
        constexpr double BLDG_MIN_EXTENT = 0.55;
        constexpr size_t BLDG_MAX_VERTS  = 14;
        constexpr double ROAD_MIN_LEN    = 45.0;
        int rej_area = 0, rej_aspect = 0, rej_extent = 0, rej_var = 0, rej_shape = 0;

        // Edge-based building mask from RGB.
        std::vector<uint8_t> gray((size_t)HW);
        for (int64_t i = 0; i < HW; ++i)
            gray[i] = (uint8_t)(0.299 * rgb_pixels[i*3]
                              + 0.587 * rgb_pixels[i*3+1]
                              + 0.114 * rgb_pixels[i*3+2]);

        std::vector<uint8_t> edges((size_t)HW, 0);
        for (int y = 1; y < H - 1; ++y)
            for (int x = 1; x < W - 1; ++x) {
                auto g = [&](int dx, int dy) -> int {
                    return gray[(y+dy)*W + (x+dx)]; };
                int gx = -g(-1,-1) - 2*g(-1,0) - g(-1,1)
                       +  g( 1,-1) + 2*g( 1,0) + g( 1,1);
                int gy = -g(-1,-1) - 2*g(0,-1) - g( 1,-1)
                       +  g(-1, 1) + 2*g(0, 1) + g( 1, 1);
                edges[y*W + x] = (uint8_t)std::min(
                    (int)std::sqrt((double)(gx*gx + gy*gy)), 255);
            }

        std::vector<uint8_t> dilated((size_t)HW, 0);
        for (int y = 1; y < H - 1; ++y)
            for (int x = 1; x < W - 1; ++x) {
                bool any = false;
                for (int dy = -1; dy <= 1 && !any; ++dy)
                    for (int dx = -1; dx <= 1 && !any; ++dx)
                        if (edges[(y+dy)*W + (x+dx)] > 60) any = true;
                if (any) dilated[y*W + x] = 255;
            }

        std::vector<uint8_t> inv((size_t)HW);
        for (int64_t i = 0; i < HW; ++i) inv[i] = dilated[i] ? 0 : 255;
        for (int x = 0; x < W; ++x) { inv[x] = 0; inv[(H-1)*W + x] = 0; }
        for (int y = 0; y < H; ++y) { inv[y*W] = 0; inv[y*W + W - 1] = 0; }

        std::vector<int> edge_labels;
        int edge_ncomp = connected_components(inv, W, H, edge_labels);

        // DSM Sobel edges — elevation boundaries from the shaded-relief raster.
        std::vector<uint8_t> dsm_edges((size_t)HW, 0);
        for (int y = 1; y < H - 1; ++y)
            for (int x = 1; x < W - 1; ++x) {
                auto d = [&](int dx, int dy) -> int {
                    return dsm_gray[(y+dy)*W + (x+dx)]; };
                int gx = -d(-1,-1) - 2*d(-1,0) - d(-1,1)
                       +  d( 1,-1) + 2*d( 1,0) + d( 1,1);
                int gy = -d(-1,-1) - 2*d(0,-1) - d( 1,-1)
                       +  d(-1, 1) + 2*d(0, 1) + d( 1, 1);
                dsm_edges[y*W + x] = (uint8_t)std::min(
                    (int)std::sqrt((double)(gx*gx + gy*gy)), 255);
            }

        std::vector<uint8_t> dsm_dilated((size_t)HW, 0);
        for (int y = 1; y < H - 1; ++y)
            for (int x = 1; x < W - 1; ++x) {
                bool any = false;
                for (int dy = -1; dy <= 1 && !any; ++dy)
                    for (int dx = -1; dx <= 1 && !any; ++dx)
                        if (dsm_edges[(y+dy)*W + (x+dx)] > 50) any = true;
                if (any) dsm_dilated[y*W + x] = 255;
            }

        std::vector<uint8_t> dsm_inv((size_t)HW);
        for (int64_t i = 0; i < HW; ++i) dsm_inv[i] = dsm_dilated[i] ? 0 : 255;
        for (int x = 0; x < W; ++x) { dsm_inv[x] = 0; dsm_inv[(H-1)*W + x] = 0; }
        for (int y = 0; y < H; ++y) { dsm_inv[y*W] = 0; dsm_inv[y*W + W - 1] = 0; }

        std::vector<int> dsm_labels;
        int dsm_ncomp = connected_components(dsm_inv, W, H, dsm_labels);

        std::vector<int> dsm_area((size_t)(dsm_ncomp + 1), 0);
        for (int64_t i = 0; i < HW; ++i)
            if (dsm_labels[i] > 0) dsm_area[dsm_labels[i]]++;

        auto to_geo = [&](const std::vector<std::pair<int,int>>& pts) {
            std::vector<std::pair<double,double>> out; out.reserve(pts.size());
            for (auto& [px, py] : pts)
                out.push_back(merc_px_to_geo(px + 0.5, py + 0.5, W, H,
                                             min_lat, min_lon, max_lat, max_lon));
            return out;
        };

        // Merge edge + DSM evidence for buildings.
        for (int lab = 1; lab <= edge_ncomp; ++lab) {
            std::vector<std::pair<int,int>> pix;
            int bx0 = W, bx1 = 0, by0 = H, by1 = 0;
            for (int64_t i = 0; i < HW; ++i) {
                if (edge_labels[i] != lab) continue;
                int px = (int)(i % W), py = (int)(i / W);
                pix.emplace_back(px, py);
                bx0 = std::min(bx0, px); bx1 = std::max(bx1, px);
                by0 = std::min(by0, py); by1 = std::max(by1, py);
            }
            int area = (int)pix.size();
            if (area < BLDG_MIN_AREA || area > HW / 4) { ++rej_area; continue; }
            int bw = bx1 - bx0 + 1, bh = by1 - by0 + 1;
            if ((double)std::max(bw,bh) / std::max(1, std::min(bw,bh)) > BLDG_MAX_ASPECT) {
                ++rej_aspect; continue;
            }
            double extent = (double)area / ((double)bw * bh);
            if (extent < BLDG_MIN_EXTENT) { ++rej_extent; continue; }

            int dsm_hits = 0;
            for (auto& [x, y] : pix) {
                int dl = dsm_labels[(size_t)y * W + x];
                if (dl > 0 && dsm_area[dl] >= 12) ++dsm_hits;
            }
            float dsm_ratio = (float)dsm_hits / (float)area;

            double sr = 0, sg = 0, sb = 0;
            for (auto& [x, y] : pix) {
                size_t idx = ((size_t)y * W + x) * 3;
                sr += rgb_pixels[idx]; sg += rgb_pixels[idx+1]; sb += rgb_pixels[idx+2];
            }
            double mr = sr/area, mg = sg/area, mb = sb/area;
            double var = 0;
            for (auto& [x, y] : pix) {
                size_t idx = ((size_t)y * W + x) * 3;
                double dr = rgb_pixels[idx]-mr, dg = rgb_pixels[idx+1]-mg,
                       db = rgb_pixels[idx+2]-mb;
                var += dr*dr + dg*dg + db*db;
            }
            var /= area;
            double var_limit = dsm_ratio > 0.3 ? 5000.0 : 3000.0;
            if (var > var_limit) { ++rej_var; continue; }

            auto ring = trace_contour(edge_labels, W, H, lab);
            std::vector<std::pair<int,int>> simp;
            dp_simplify(ring, simplify_px, simp);
            dedupe_consecutive(simp, 1.5, /*closed=*/true);
            if (simp.size() < 4 || simp.size() > BLDG_MAX_VERTS) { ++rej_shape; continue; }

            float base_conf = std::clamp(1.0f - (float)(var / var_limit), 0.2f, 0.9f);
            float conf = std::clamp(base_conf + dsm_ratio * 0.15f, 0.2f, 0.98f);

            DetectedFeature f;
            f.type           = FeatureType::Building;
            f.confidence     = conf;
            f.is_area        = true;
            f.suggested_tags["building"] = "yes";
            f.source         = "cv-lidar";
            f.coords         = to_geo(simp);
            result.features.push_back(std::move(f));
        }

        // DSM-only buildings: elevated in DSM but missed by edge detection.
        for (int lab = 1; lab <= dsm_ncomp; ++lab) {
            if (dsm_area[lab] < 25 || dsm_area[lab] > HW / 4) { ++rej_area; continue; }
            int covered = 0;
            std::vector<std::pair<int,int>> pix;
            int bx0 = W, bx1 = 0, by0 = H, by1 = 0;
            for (int64_t i = 0; i < HW; ++i) {
                if (dsm_labels[i] != lab) continue;
                int px = (int)(i % W), py = (int)(i / W);
                pix.emplace_back(px, py);
                bx0 = std::min(bx0, px); bx1 = std::max(bx1, px);
                by0 = std::min(by0, py); by1 = std::max(by1, py);
                if (edge_labels[i] > 0) ++covered;
            }
            if ((float)covered / pix.size() > 0.4f) continue;
            int bw = bx1 - bx0 + 1, bh = by1 - by0 + 1;
            if ((double)std::max(bw,bh) / std::max(1, std::min(bw,bh)) > BLDG_MAX_ASPECT) {
                ++rej_aspect; continue;
            }
            double extent = (double)dsm_area[lab] / ((double)bw * bh);
            if (extent < BLDG_MIN_EXTENT) { ++rej_extent; continue; }

            double sg = 0, sr = 0, sb = 0;
            for (auto& [x, y] : pix) {
                size_t idx = ((size_t)y * W + x) * 3;
                sr += rgb_pixels[idx]; sg += rgb_pixels[idx+1]; sb += rgb_pixels[idx+2];
            }
            double mg = sg / pix.size(), mr = sr / pix.size(), mb = sb / pix.size();
            if (mg > mr * 1.15 && mg > mb * 1.15) continue;

            auto ring = trace_contour(dsm_labels, W, H, lab);
            std::vector<std::pair<int,int>> simp;
            dp_simplify(ring, simplify_px, simp);
            dedupe_consecutive(simp, 1.5, /*closed=*/true);
            if (simp.size() < 4 || simp.size() > BLDG_MAX_VERTS) { ++rej_shape; continue; }

            DetectedFeature f;
            f.type           = FeatureType::Building;
            f.confidence     = 0.45f;
            f.is_area        = true;
            f.suggested_tags["building"] = "yes";
            f.source         = "cv-lidar";
            f.coords         = to_geo(simp);
            result.features.push_back(std::move(f));
        }

        // Road candidates from RGB (same as plain CV).
        std::vector<uint8_t> road_mask((size_t)HW, 0);
        for (int64_t i = 0; i < HW; ++i) {
            uint8_t r = rgb_pixels[i*3], g = rgb_pixels[i*3+1], b = rgb_pixels[i*3+2];
            int mean = (r + g + b) / 3;
            int sat  = std::max({r, g, b}) - std::min({r, g, b});
            if (mean > 40 && mean < 180 && sat < 40) road_mask[i] = 255;
        }
        zhang_suen(road_mask, W, H);
        std::vector<std::vector<std::pair<int,int>>> paths;
        skeleton_to_polylines(road_mask, W, H, paths);

        int rej_road_len = 0;
        for (auto& path : paths) {
            std::vector<std::pair<int,int>> simp;
            dp_simplify(path, simplify_px * 1.4, simp); // roads tolerate coarser
            dedupe_consecutive(simp, 1.5, /*closed=*/false);
            if (simp.size() < 3) continue;
            double len = 0;
            for (size_t i = 1; i < simp.size(); ++i)
                len += std::hypot(simp[i].first - simp[i-1].first,
                                  simp[i].second - simp[i-1].second);
            if (len < ROAD_MIN_LEN) { ++rej_road_len; continue; }

            DetectedFeature f;
            f.type           = FeatureType::Road;
            f.confidence     = 0.6f;
            f.is_area        = false;
            f.suggested_tags["highway"] = "unclassified";
            f.source         = "cv-lidar";
            f.coords         = to_geo(simp);
            result.features.push_back(std::move(f));
        }

        {
            size_t added = result.features.size() - feat_before;
            int n_bldg = 0, n_road = 0;
            for (size_t i = feat_before; i < result.features.size(); ++i)
                (result.features[i].type == FeatureType::Building ? n_bldg : n_road)++;
            LOG_INFO("run_cv_dsm_sync: {} feature(s) — {} building(s), {} road(s); "
                     "rejected buildings[area:{} aspect:{} extent:{} var:{} shape:{}] "
                     "roads[short:{}]",
                     added, n_bldg, n_road,
                     rej_area, rej_aspect, rej_extent, rej_var, rej_shape, rej_road_len);
        }

        if (result.features.empty() && result.error.empty()) {
            result.error = "CV+LiDAR detected no features in this imagery";
            LOG_WARN("run_cv_dsm_sync: no features detected");
        }
}

// ── Public CV entry points (raw pixels) ──────────────────────────────────────

void detect_cv_on_imagery(const std::vector<uint8_t>& rgb_pixels,
                          int img_w, int img_h,
                          double min_lat, double min_lon,
                          double max_lat, double max_lon,
                          Callback cb, double simplify_px)
{
    net::post_async([=, cb = std::move(cb)] mutable {
        DetectionResult result;
        result.source_name = "CV Edge Detection";
        run_cv_sync(rgb_pixels, img_w, img_h,
                    min_lat, min_lon, max_lat, max_lon, result, simplify_px);
        cb(std::move(result));
    });
}

void detect_cv_with_dsm(const std::vector<uint8_t>& rgb_pixels,
                         const std::vector<uint8_t>& dsm_gray,
                         int img_w, int img_h,
                         double min_lat, double min_lon,
                         double max_lat, double max_lon,
                         Callback cb, double simplify_px)
{
    net::post_async([=, cb = std::move(cb)] mutable {
        DetectionResult result;
        result.source_name = "CV + LiDAR DSM";
        run_cv_dsm_sync(rgb_pixels, dsm_gray, img_w, img_h,
                        min_lat, min_lon, max_lat, max_lon, result, simplify_px);
        cb(std::move(result));
    });
}

// ── Imagery / DSM mosaic fetch helpers (bbox → stitched raster) ───────────────
namespace {

// Web-Mercator (EPSG:3857) extent of a slippy tile, in metres.
static void tile_mercator_bbox(int z, int x, int y,
                               double& minx, double& miny,
                               double& maxx, double& maxy) {
    constexpr double ORIGIN = QUAD_PI * 6378137.0;
    double span = 2.0 * ORIGIN / (double)(1 << z);
    minx = -ORIGIN + x * span;
    maxx = -ORIGIN + (x + 1) * span;
    maxy =  ORIGIN - y * span;
    miny =  ORIGIN - (y + 1) * span;
}

struct FetchedMosaic {
    std::vector<uint8_t> rgb;            // W*H*3
    int W{0}, H{0};
    double min_lat{0}, min_lon{0}, max_lat{0}, max_lon{0}; // tile-aligned extent
    int tx0{0}, ty0{0}, cols{0}, rows{0};
    std::string error;
};

// Tile range covering the bbox at zoom + the tile-aligned geo extent.
static void mosaic_tile_range(double min_lat, double min_lon,
                              double max_lat, double max_lon, int zoom,
                              int& tx0, int& ty0, int& tx1, int& ty1,
                              double& m_min_lat, double& m_min_lon,
                              double& m_max_lat, double& m_max_lon) {
    int n = 1 << zoom;
    lat_lon_to_tile(max_lat, min_lon, zoom, tx0, ty0); // NW
    lat_lon_to_tile(min_lat, max_lon, zoom, tx1, ty1); // SE
    if (tx0 > tx1) std::swap(tx0, tx1);
    if (ty0 > ty1) std::swap(ty0, ty1);
    auto tile_lon = [&](int x) { return (double)x / n * 360.0 - 180.0; };
    auto tile_lat = [&](int y) {
        double t = QUAD_PI * (1.0 - 2.0 * (double)y / n);
        return std::atan(std::sinh(t)) * 180.0 / QUAD_PI;
    };
    m_min_lon = tile_lon(tx0);  m_max_lon = tile_lon(tx1 + 1);
    m_max_lat = tile_lat(ty0);  m_min_lat = tile_lat(ty1 + 1);
}

// Geoportal services check the Referer on some layers; send it whenever the
// URL targets geoportal.gov.pl so detection fetches behave like the tile cache.
static std::vector<std::pair<std::string,std::string>>
detect_headers(const std::string& url) {
    std::vector<std::pair<std::string,std::string>> h;
    if (url.find("geoportal.gov.pl") != std::string::npos)
        h.push_back({"Referer", "https://mapy.geoportal.gov.pl"});
    return h;
}

static const char* http_err_name(net::HttpError e) {
    switch (e) {
        case net::HttpError::CurlInit:     return "curl-init";
        case net::HttpError::NetworkError: return "network/reset";
        case net::HttpError::Timeout:      return "timeout";
        case net::HttpError::HttpError4xx: return "http-4xx";
        case net::HttpError::HttpError5xx: return "http-5xx";
    }
    return "unknown";
}

// Fetch one URL with bounded retries on transient failures, mirroring the tile
// cache's fetch_with_retry. 4xx is permanent (returns immediately); timeouts,
// connection resets (Geoportal throws curl-56/28 even at low concurrency) and
// 5xx are retried with exponential backoff + per-call jitter so a wave of
// failures doesn't retry in lockstep. min_bytes>0 also rejects load-shed "blank"
// bodies (server returns a fixed tiny image under load) as transient. `tag`
// labels the logs so a stuck/failed fetch is VISIBLE per-tile, never silent.
static std::expected<net::Response, net::HttpError>
fetch_retry(const std::string& url,
            const std::vector<std::pair<std::string,std::string>>& hdrs,
            int timeout_s, int min_bytes, int jitter_seed, const std::string& tag) {
    constexpr int kMaxAttempts = 4;
    std::expected<net::Response, net::HttpError> resp;
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        resp = net::get(url, timeout_s, hdrs);
        bool blank = resp && min_bytes > 0 && (int)resp->bytes.size() < min_bytes;
        if (resp && !blank) {
            if (attempt > 1)
                LOG_DEBUG("{}: ok after {} attempt(s), {} B", tag, attempt,
                          resp->bytes.size());
            return resp;
        }
        if (blank) {
            LOG_DEBUG("{}: blank/load-shed {} B (<{}) attempt {}/{}",
                      tag, resp->bytes.size(), min_bytes, attempt, kMaxAttempts);
        } else if (resp.error() == net::HttpError::HttpError4xx) {
            LOG_WARN("{}: http-4xx (permanent) — not retrying", tag);
            return resp;
        } else {
            LOG_DEBUG("{}: {} attempt {}/{}", tag, http_err_name(resp.error()),
                      attempt, kMaxAttempts);
        }
        if (attempt == kMaxAttempts) {
            if (blank) resp = std::unexpected(net::HttpError::NetworkError);
            break;
        }
        int jitter  = (jitter_seed * 7 + attempt * 13) % 150;
        int backoff = 150 * (1 << (attempt - 1)) + jitter;   // 150,300,600 ms +jit
        std::this_thread::sleep_for(std::chrono::milliseconds(backoff));
    }
    LOG_WARN("{}: failed after {} attempts ({})", tag, kMaxAttempts,
             resp ? "blank" : http_err_name(resp.error()));
    return resp;
}

// Fetch + stitch a slippy/WMTS imagery layer ({z}/{x}/{y}, key already
// substituted) into an RGB mosaic.  Rejects WMS ({bbox}) templates.
static FetchedMosaic fetch_slippy_rgb(const std::string& url_template, int zoom,
                                      double min_lat, double min_lon,
                                      double max_lat, double max_lon) {
    FetchedMosaic fm;
    if (url_template.find("{bbox}") != std::string::npos ||
        url_template.find("{x}") == std::string::npos) {
        fm.error = "Imagery base must be a slippy/WMTS layer (not WMS) for CV";
        LOG_WARN("fetch_slippy_rgb: rejected non-slippy template");
        return fm;
    }
    int tx0, ty0, tx1, ty1;
    mosaic_tile_range(min_lat, min_lon, max_lat, max_lon, zoom,
                      tx0, ty0, tx1, ty1,
                      fm.min_lat, fm.min_lon, fm.max_lat, fm.max_lon);
    int cols = tx1 - tx0 + 1, rows = ty1 - ty0 + 1;
    if (cols * rows > 64) {
        fm.error = "Zoom out — too many imagery tiles";
        LOG_WARN("fetch_slippy_rgb: {}x{} = {} tiles exceeds cap (z{})",
                 cols, rows, cols * rows, zoom);
        return fm;
    }
    LOG_INFO("fetch_slippy_rgb: z{} tiles x[{}..{}] y[{}..{}] = {}x{} ({} tiles)",
             zoom, tx0, tx1, ty0, ty1, cols, rows, cols * rows);

    const int TS = 256;
    fm.W = cols * TS; fm.H = rows * TS; fm.cols = cols; fm.rows = rows;
    fm.tx0 = tx0; fm.ty0 = ty0;
    fm.rgb.assign((size_t)fm.W * fm.H * 3, 0);
    auto hdrs = detect_headers(url_template);   // same host for every tile
    int ok = 0, net_fail = 0, decode_fail = 0;
    for (int ty = ty0; ty <= ty1; ++ty)
        for (int tx = tx0; tx <= tx1; ++tx) {
            std::string url = url_template;
            auto rep = [&](const char* tok, const std::string& v) {
                for (size_t p; (p = url.find(tok)) != std::string::npos;)
                    url.replace(p, std::strlen(tok), v);
            };
            rep("{z}", std::to_string(zoom));
            rep("{x}", std::to_string(tx));
            rep("{y}", std::to_string(ty));
            rep("{key}", std::string());
            auto tag = std::format("slippy z{} x{} y{}", zoom, tx, ty);
            auto resp = fetch_retry(url, hdrs, 30, /*min_bytes*/0,
                                    tx * 1000 + ty, tag);
            if (!resp) { ++net_fail; continue; }
            int tw, th, ch;
            uint8_t* px = stbi_load_from_memory(
                resp->bytes.data(), (int)resp->bytes.size(), &tw, &th, &ch, 3);
            if (!px) {
                ++decode_fail;
                LOG_DEBUG("{}: decode failed ({} B)", tag, resp->bytes.size());
                continue;
            }
            ++ok;
            int ox = (tx - tx0) * TS, oy = (ty - ty0) * TS;
            int cw = std::min(tw, TS), chh = std::min(th, TS);
            for (int y = 0; y < chh; ++y)
                std::memcpy(&fm.rgb[(((size_t)(oy + y)) * fm.W + ox) * 3],
                            &px[(size_t)y * tw * 3], (size_t)cw * 3);
            stbi_image_free(px);
        }
    if (ok == 0) {
        fm.error = "Could not fetch any imagery tiles";
        LOG_WARN("fetch_slippy_rgb: 0/{} tiles fetched (net_fail:{} decode_fail:{})",
                 cols * rows, net_fail, decode_fail);
    } else {
        LOG_INFO("fetch_slippy_rgb: {}/{} tiles ok (net_fail:{} decode_fail:{}), "
                 "mosaic {}x{}px", ok, cols * rows, net_fail, decode_fail, fm.W, fm.H);
        if (net_fail || decode_fail)
            LOG_WARN("fetch_slippy_rgb: {} tile(s) missing → holes in mosaic",
                     net_fail + decode_fail);
    }
    return fm;
}

// One WMS GetMap over an arbitrary mercator bbox at an arbitrary pixel size.
// WMS honours BBOX+WIDTH+HEIGHT directly, so a single request replaces a whole
// grid of per-tile fetches: latency is bounded to ONE request instead of N. A
// slow layer (e.g. the NMPT ShadedRelief DSM) otherwise stalls detection for
// MINUTES — N serial 30s-timeout GETs back to back. Returns RGB (w*h*3) or {}.
static std::vector<uint8_t>
wms_getmap_rgb(const std::string& wms_template,
               double minx, double miny, double maxx, double maxy,
               int w, int h, int timeout_s) {
    std::string url = wms_template;
    auto rep = [&](const char* tok, const std::string& v) {
        for (size_t p; (p = url.find(tok)) != std::string::npos;)
            url.replace(p, std::strlen(tok), v);
    };
    rep("{bbox}", std::format("{:.9f},{:.9f},{:.9f},{:.9f}", minx, miny, maxx, maxy));
    rep("{width}",  std::to_string(w));
    rep("{height}", std::to_string(h));
    rep("{proj}",   "EPSG:3857");
    rep("{key}",    std::string());
    auto tag = std::format("wms_getmap {}x{}px", w, h);
    auto resp = fetch_retry(url, detect_headers(wms_template), timeout_s,
                            /*min_bytes*/0, w + h, tag);
    if (!resp) { LOG_WARN("wms_getmap_rgb: fetch failed ({}x{}px)", w, h); return {}; }
    int tw, th, ch;
    uint8_t* px = stbi_load_from_memory(resp->bytes.data(), (int)resp->bytes.size(),
                                        &tw, &th, &ch, 3);
    if (!px) {
        LOG_WARN("wms_getmap_rgb: decode failed ({} bytes, {}x{}px)",
                 resp->bytes.size(), w, h);
        return {};
    }
    std::vector<uint8_t> out((size_t)w * h * 3, 0);
    int cw = std::min(tw, w), chh = std::min(th, h);
    for (int y = 0; y < chh; ++y)
        std::memcpy(&out[(size_t)y * w * 3], &px[(size_t)y * tw * 3], (size_t)cw * 3);
    stbi_image_free(px);
    return out;
}

// Tile-aligned Web-Mercator extent spanning a cols×rows tile block (top-left
// tile tx0,ty0). The cols*256 × rows*256 raster maps onto it 1:1, so a single
// GetMap at that size is pixel-identical to stitching the individual tiles.
static void mosaic_mercator_extent(int zoom, int tx0, int ty0, int cols, int rows,
                                   double& minx, double& miny,
                                   double& maxx, double& maxy) {
    double ax0, ay0, ax1, ay1; tile_mercator_bbox(zoom, tx0, ty0, ax0, ay0, ax1, ay1);
    double bx0, by0, bx1, by1;
    tile_mercator_bbox(zoom, tx0 + cols - 1, ty0 + rows - 1, bx0, by0, bx1, by1);
    minx = ax0; maxx = bx1; maxy = ay1; miny = by0; // NW.west/SE.east, NW.north/SE.south
}

// Fetch a WMS GetMap RGB imagery layer ({bbox}/{width}/{height}/{proj}) into an
// RGB mosaic over the slippy tile grid — same grid fetch_slippy_rgb and
// fetch_wms_gray use, so imagery + DSM stay 1:1. Lets a WMS base (e.g. Geoportal
// Ortofoto) feed the CV pipeline. ONE GetMap over the full extent (no per-tile
// loop) — see wms_getmap_rgb for why.
static FetchedMosaic fetch_wms_rgb(const std::string& wms_template, int zoom,
                                   double min_lat, double min_lon,
                                   double max_lat, double max_lon) {
    FetchedMosaic fm;
    int tx0, ty0, tx1, ty1;
    mosaic_tile_range(min_lat, min_lon, max_lat, max_lon, zoom,
                      tx0, ty0, tx1, ty1,
                      fm.min_lat, fm.min_lon, fm.max_lat, fm.max_lon);
    int cols = tx1 - tx0 + 1, rows = ty1 - ty0 + 1;
    if (cols * rows > 64) {
        fm.error = "Zoom out — too many imagery tiles";
        LOG_WARN("fetch_wms_rgb: {}x{} = {} tiles exceeds cap (z{})",
                 cols, rows, cols * rows, zoom);
        return fm;
    }
    const int TS = 256;
    fm.W = cols * TS; fm.H = rows * TS; fm.cols = cols; fm.rows = rows;
    fm.tx0 = tx0; fm.ty0 = ty0;
    LOG_INFO("fetch_wms_rgb: z{} {}x{} tiles → single GetMap {}x{}px",
             zoom, cols, rows, fm.W, fm.H);

    double minx, miny, maxx, maxy;
    mosaic_mercator_extent(zoom, tx0, ty0, cols, rows, minx, miny, maxx, maxy);
    fm.rgb = wms_getmap_rgb(wms_template, minx, miny, maxx, maxy, fm.W, fm.H, 45);
    if (fm.rgb.empty()) {
        fm.error = "Could not fetch WMS imagery (GetMap failed/timeout)";
        LOG_WARN("fetch_wms_rgb: GetMap failed ({}x{}px, z{})", fm.W, fm.H, zoom);
        return fm;
    }
    LOG_INFO("fetch_wms_rgb: GetMap ok, mosaic {}x{}px ({} tiles equiv)",
             fm.W, fm.H, cols * rows);
    return fm;
}

// Dispatch to the right RGB fetcher based on the template: WMS ({bbox}) vs
// slippy/WMTS ({x}). Lets the CV pipeline accept either kind of base layer.
static FetchedMosaic fetch_imagery_rgb(const std::string& url_template, int zoom,
                                       double min_lat, double min_lon,
                                       double max_lat, double max_lon) {
    if (url_template.find("{bbox}") != std::string::npos)
        return fetch_wms_rgb(url_template, zoom, min_lat, min_lon, max_lat, max_lon);
    return fetch_slippy_rgb(url_template, zoom, min_lat, min_lon, max_lat, max_lon);
}

// Fetch a WMS shaded-relief DSM layer ({bbox}/{width}/{height}/{proj}) into a
// grayscale mosaic over the SAME tile grid as the imagery, so the DSM and RGB
// pixels correspond 1:1.  ONE GetMap over the full extent (no per-tile loop) —
// the old grid fetched N serial tiles, and a slow/overloaded DSM endpoint
// (NMPT ShadedRelief, curl error 28) hung detection for minutes. out_ok = 1 on
// success, 0 on failure (caller treats 0 as "no DSM"). Returns {} on failure.
static std::vector<uint8_t> fetch_wms_gray(const std::string& wms_template,
                                           int zoom, int tx0, int ty0,
                                           int cols, int rows, int& out_ok) {
    const int TS = 256;
    int W = cols * TS, H = rows * TS;
    out_ok = 0;
    double minx, miny, maxx, maxy;
    mosaic_mercator_extent(zoom, tx0, ty0, cols, rows, minx, miny, maxx, maxy);
    auto rgb = wms_getmap_rgb(wms_template, minx, miny, maxx, maxy, W, H, 45);
    if (rgb.empty()) {
        LOG_WARN("fetch_wms_gray: DSM GetMap failed ({}x{}px, z{})", W, H, zoom);
        return {};
    }
    std::vector<uint8_t> gray((size_t)W * H, 0);
    for (size_t i = 0, n = (size_t)W * H; i < n; ++i)
        gray[i] = (uint8_t)(0.299 * rgb[i*3] + 0.587 * rgb[i*3+1] + 0.114 * rgb[i*3+2]);
    out_ok = 1;
    LOG_INFO("fetch_wms_gray: DSM GetMap ok {}x{}px ({} tiles equiv, z{})",
             W, H, cols * rows, zoom);
    return gray;
}

} // anon (mosaic fetch helpers)

// ── BBox-driven CV: fetch imagery (+ DSM) then run the matcher ────────────────

void detect_cv_bbox(const std::string& imagery_url, int zoom,
                    double min_lat, double min_lon,
                    double max_lat, double max_lon,
                    Callback cb, double simplify_px)
{
    LOG_INFO("detect_cv_bbox: z{} bbox {:.5f},{:.5f}..{:.5f},{:.5f}",
             zoom, min_lat, min_lon, max_lat, max_lon);
    net::post_async([=, cb = std::move(cb)] mutable {
        DetectionResult result;
        result.source_name = "CV Edge Detection";
        auto img = fetch_imagery_rgb(imagery_url, zoom,
                                     min_lat, min_lon, max_lat, max_lon);
        if (!img.error.empty()) {
            result.error = img.error;
            LOG_WARN("detect_cv_bbox aborted: {}", img.error);
            cb(std::move(result)); return;
        }
        run_cv_sync(img.rgb, img.W, img.H,
                    img.min_lat, img.min_lon, img.max_lat, img.max_lon, result,
                    simplify_px);
        cb(std::move(result));
    });
}

void detect_cv_lidar_bbox(const std::string& imagery_url,
                          const std::string& dsm_wms_url, int zoom,
                          double min_lat, double min_lon,
                          double max_lat, double max_lon,
                          Callback cb, double simplify_px)
{
    LOG_INFO("detect_cv_lidar_bbox: z{} bbox {:.5f},{:.5f}..{:.5f},{:.5f}",
             zoom, min_lat, min_lon, max_lat, max_lon);
    net::post_async([=, cb = std::move(cb)] mutable {
        DetectionResult result;
        result.source_name = "CV + LiDAR DSM";
        if (dsm_wms_url.empty()) {
            result.error = "No LiDAR DSM (NMPT) layer configured";
            LOG_WARN("detect_cv_lidar_bbox aborted: no DSM URL");
            cb(std::move(result)); return;
        }
        auto img = fetch_imagery_rgb(imagery_url, zoom,
                                     min_lat, min_lon, max_lat, max_lon);
        if (!img.error.empty()) {
            result.error = img.error;
            LOG_WARN("detect_cv_lidar_bbox aborted: {}", img.error);
            cb(std::move(result)); return;
        }
        int dsm_ok = 0;
        auto dsm = fetch_wms_gray(dsm_wms_url, zoom, img.tx0, img.ty0,
                                  img.cols, img.rows, dsm_ok);
        if (dsm_ok == 0) {
            result.error = "Could not fetch LiDAR DSM (NMPT) tiles";
            LOG_WARN("detect_cv_lidar_bbox aborted: 0 DSM tiles fetched");
            cb(std::move(result)); return;
        }
        run_cv_dsm_sync(img.rgb, dsm, img.W, img.H,
                        img.min_lat, img.min_lon, img.max_lat, img.max_lon, result,
                        simplify_px);
        cb(std::move(result));
    });
}

// ── Optional: ONNX Runtime local inference ────────────────────────────────────
#ifdef CPPOSMUI_HAVE_ONNXRUNTIME

// Synchronous inference core: runs the model on an RGB raster covering a
// Web-Mercator-uniform geo bbox and appends features to `result`.  Shared by the
// raw-pixel and bbox-fetching entry points.  Call from a worker thread.
static void run_onnx_sync(const std::string& model_path,
                          const std::vector<uint8_t>& rgb_pixels,
                          int img_w, int img_h,
                          double min_lat, double min_lon,
                          double max_lat, double max_lon,
                          DetectionResult& result)
{
        try {
            Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "cpposmui");
            Ort::SessionOptions opts;
            opts.SetIntraOpNumThreads(2);
#ifdef _WIN32
            // ORT uses wchar_t paths on Windows; convert from UTF-8.
            std::wstring wpath;
            if (!model_path.empty()) {
                int n = MultiByteToWideChar(CP_UTF8, 0, model_path.data(),
                                            (int)model_path.size(), nullptr, 0);
                wpath.resize(n);
                MultiByteToWideChar(CP_UTF8, 0, model_path.data(),
                                    (int)model_path.size(), wpath.data(), n);
            }
            Ort::Session session(env, wpath.c_str(), opts);
#else
            Ort::Session session(env, model_path.c_str(), opts);
#endif

            // Build float32 input [1, 3, H, W] — normalise to [0,1].
            int64_t H = img_h, W = img_w;
            std::vector<float> input(3 * H * W);
            for (int c = 0; c < 3; ++c)
                for (int y = 0; y < H; ++y)
                    for (int x = 0; x < W; ++x)
                        input[c * H * W + y * W + x] =
                            rgb_pixels[(y * W + x) * 3 + c] / 255.0f;

            std::array<int64_t, 4> in_shape{1, 3, H, W};
            auto mem = Ort::MemoryInfo::CreateCpu(
                OrtArenaAllocator, OrtMemTypeDefault);
            Ort::Value in_tensor = Ort::Value::CreateTensor<float>(
                mem, input.data(), input.size(), in_shape.data(), 4);

            const char* in_names[]  = {"input"};
            const char* out_names[] = {"output"};
            auto outputs = session.Run(Ort::RunOptions{nullptr},
                in_names, &in_tensor, 1, out_names, 1);

            const float* logits = outputs[0].GetTensorData<float>();
            auto out_shape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
            // Expected: [1, C, H, W] — C channels: 0=bg 1=road 2=building 3=water
            int64_t C  = out_shape.size() >= 2 ? out_shape[1] : 1;
            const int64_t HW = (int64_t)H * W;

            // Per-pixel argmax class + softmax probability of the winning class
            // (the latter feeds a real per-instance confidence score).
            std::vector<uint8_t> cls((size_t)HW, 0);
            std::vector<float>   prob((size_t)HW, 0.f);
            for (int64_t px = 0; px < HW; ++px) {
                int best_c = 0; float best_v = logits[px];
                for (int c = 1; c < (int)C; ++c) {
                    float v = logits[c * HW + px];
                    if (v > best_v) { best_v = v; best_c = c; }
                }
                float sum = 0.f;
                for (int c = 0; c < (int)C; ++c)
                    sum += std::exp(logits[c * HW + px] - best_v);
                cls[px]  = (uint8_t)best_c;
                prob[px] = sum > 0.f ? 1.0f / sum : 1.0f; // exp(0)/Σexp
            }

            auto mask_for = [&](int klass) {
                std::vector<uint8_t> m((size_t)HW, 0);
                for (int64_t px = 0; px < HW; ++px) if (cls[px] == klass) m[px] = 255;
                return m;
            };
            auto conf_of = [&](const std::vector<std::pair<int,int>>& pts) {
                if (pts.empty()) return 0.5f;
                double s = 0; for (auto& [x, y] : pts) s += prob[(size_t)y * W + x];
                return std::clamp((float)(s / pts.size()), 0.f, 1.f);
            };
            auto to_geo = [&](const std::vector<std::pair<int,int>>& pts) {
                std::vector<std::pair<double,double>> out; out.reserve(pts.size());
                for (auto& [x, y] : pts)
                    out.push_back(merc_px_to_geo(x + 0.5, y + 0.5, (int)W, (int)H,
                                                 min_lat, min_lon, max_lat, max_lon));
                return out;
            };

            // ── Area classes → one polygon per instance ────────────────────
            struct AreaClass { int klass; FeatureType type; const char* k; const char* v; };
            const AreaClass area_classes[] = {
                {2, FeatureType::Building, "building", "yes"},
                {3, FeatureType::Waterway, "natural",  "water"},
            };
            for (const auto& ac : area_classes) {
                if (ac.klass >= (int)C) continue;
                auto m = mask_for(ac.klass);
                std::vector<int> labels;
                int ncomp = connected_components(m, (int)W, (int)H, labels);
                for (int lab = 1; lab <= ncomp; ++lab) {
                    std::vector<std::pair<int,int>> pix;
                    for (int64_t i = 0; i < HW; ++i)
                        if (labels[i] == lab) pix.emplace_back((int)(i % W), (int)(i / W));
                    if ((int)pix.size() < 12) continue;            // drop speckle
                    auto ring = trace_contour(labels, (int)W, (int)H, lab);
                    std::vector<std::pair<int,int>> simp;
                    dp_simplify(ring, 2.0, simp);
                    if (simp.size() < 3) continue;
                    DetectedFeature f;
                    f.type           = ac.type;
                    f.confidence     = conf_of(pix);
                    f.is_area        = true;
                    f.suggested_tags[ac.k] = ac.v;
                    f.source         = "onnx-local";
                    f.coords         = to_geo(simp);
                    result.features.push_back(std::move(f));
                }
            }

            // ── Roads → skeletonise to centre-lines, then split into ways ──
            if ((int)C > 1) {
                auto road = mask_for(1);
                zhang_suen(road, (int)W, (int)H);
                std::vector<std::vector<std::pair<int,int>>> paths;
                skeleton_to_polylines(road, (int)W, (int)H, paths);
                for (auto& path : paths) {
                    std::vector<std::pair<int,int>> simp;
                    dp_simplify(path, 2.0, simp);
                    if (simp.size() < 2) continue;
                    DetectedFeature f;
                    f.type           = FeatureType::Road;
                    f.confidence     = conf_of(simp);
                    f.is_area        = false;
                    f.suggested_tags["highway"] = "unclassified";
                    f.source         = "onnx-local";
                    f.coords         = to_geo(simp);
                    result.features.push_back(std::move(f));
                }
            }

        } catch (const Ort::Exception& e) {
            result.error = std::string("ONNX error: ") + e.what();
        }
}

void detect_via_onnx(const std::string& model_path,
                      const std::vector<uint8_t>& rgb_pixels,
                      int img_w, int img_h,
                      double min_lat, double min_lon,
                      double max_lat, double max_lon,
                      Callback cb)
{
    net::post_async([=, cb = std::move(cb)] mutable {
        DetectionResult result;
        result.source_name = "ONNX Local Model";
        run_onnx_sync(model_path, rgb_pixels, img_w, img_h,
                      min_lat, min_lon, max_lat, max_lon, result);
        cb(std::move(result));
    });
}

// Fetch the slippy/WMTS imagery tiles covering the bbox at `zoom`, stitch them
// into one RGB mosaic, then run the segmentation model over it.  `url_template`
// must use {z}/{x}/{y} (key already substituted); WMS {bbox} layers are rejected.
void detect_via_onnx_bbox(const std::string& model_path,
                          const std::string& url_template, int zoom,
                          double min_lat, double min_lon,
                          double max_lat, double max_lon,
                          Callback cb)
{
    net::post_async([=, cb = std::move(cb)] mutable {
        DetectionResult result;
        result.source_name = "ONNX Local Model";

        if (url_template.find("{bbox}") != std::string::npos ||
            url_template.find("{x}")    == std::string::npos) {
            result.error = "ONNX needs a slippy/WMTS imagery base layer "
                           "(WMS {bbox} layers are not supported)";
            cb(std::move(result)); return;
        }

        int n = 1 << zoom;
        int tx0, ty0, tx1, ty1;
        lat_lon_to_tile(max_lat, min_lon, zoom, tx0, ty0); // NW
        lat_lon_to_tile(min_lat, max_lon, zoom, tx1, ty1); // SE
        if (tx0 > tx1) std::swap(tx0, tx1);
        if (ty0 > ty1) std::swap(ty0, ty1);
        int cols = tx1 - tx0 + 1, rows = ty1 - ty0 + 1;
        if (cols * rows > 64) {
            result.error = "Zoom out or narrow the view — too many imagery tiles "
                           "to assemble for inference";
            cb(std::move(result)); return;
        }

        const int TS = 256;
        int W = cols * TS, H = rows * TS;
        std::vector<uint8_t> rgb((size_t)W * H * 3, 0);
        int fetched = 0;
        for (int ty = ty0; ty <= ty1; ++ty) {
            for (int tx = tx0; tx <= tx1; ++tx) {
                std::string url = url_template;
                auto rep = [&](const char* tok, const std::string& v) {
                    for (size_t p; (p = url.find(tok)) != std::string::npos;)
                        url.replace(p, std::strlen(tok), v);
                };
                rep("{z}", std::to_string(zoom));
                rep("{x}", std::to_string(tx));
                rep("{y}", std::to_string(ty));
                rep("{key}", std::string());
                auto resp = net::get(url, 30);
                if (!resp) continue;
                int tw, th, ch;
                uint8_t* px = stbi_load_from_memory(
                    resp->bytes.data(), (int)resp->bytes.size(), &tw, &th, &ch, 3);
                if (!px) continue;
                ++fetched;
                int ox = (tx - tx0) * TS, oy = (ty - ty0) * TS;
                int cw = std::min(tw, TS), chh = std::min(th, TS);
                for (int y = 0; y < chh; ++y)
                    std::memcpy(&rgb[(((size_t)(oy + y)) * W + ox) * 3],
                                &px[(size_t)y * tw * 3], (size_t)cw * 3);
                stbi_image_free(px);
            }
        }
        if (fetched == 0) {
            result.error = "Could not fetch any imagery tiles for inference";
            cb(std::move(result)); return;
        }

        // Geo extent of the (tile-aligned) mosaic — Mercator-uniform by tile grid.
        auto tile_lon = [&](int x) { return (double)x / n * 360.0 - 180.0; };
        auto tile_lat = [&](int y) {
            double t = QUAD_PI * (1.0 - 2.0 * (double)y / n);
            return std::atan(std::sinh(t)) * 180.0 / QUAD_PI;
        };
        double m_min_lon = tile_lon(tx0),     m_max_lon = tile_lon(tx1 + 1);
        double m_max_lat = tile_lat(ty0),     m_min_lat = tile_lat(ty1 + 1);

        run_onnx_sync(model_path, rgb, W, H,
                      m_min_lat, m_min_lon, m_max_lat, m_max_lon, result);
        cb(std::move(result));
    });
}
#endif // CPPOSMUI_HAVE_ONNXRUNTIME

} // namespace ai
