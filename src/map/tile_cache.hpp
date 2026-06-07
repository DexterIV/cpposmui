#pragma once
#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <mutex>
#include <functional>
#include <vector>

namespace map {

// Slippy map tile coordinate
struct TileCoord {
    int z{}, x{}, y{};
    bool operator==(const TileCoord&) const = default;
};

} // namespace map

template<>
struct std::hash<::map::TileCoord> {
    size_t operator()(const ::map::TileCoord& t) const noexcept {
        size_t h = (size_t)t.z;
        h ^= (size_t)t.x + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= (size_t)t.y + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

namespace map {

enum class TileState { Missing, Loading, Ready, Error };

struct Tile {
    TileCoord   coord;
    TileState   state{TileState::Missing};
    uint32_t    gl_texture{};    // 0 = not uploaded yet
    std::vector<uint8_t> png;    // raw PNG bytes before upload
    std::chrono::steady_clock::time_point error_time{}; // when Error was set
};

// Grouping for the layer-picker UI.
enum class TileCategory { Satellite, Street, Topo, Overlay };

// Tile sources — url_template may contain {z}/{x}/{y} (standard slippy-map) or
// {z}/{y}/{x} (ESRI convention), plus an optional {key} placeholder.
//
// WMS sources (wms == true) instead use a GetMap-style template with these
// placeholders, all substituted per requested tile:
//   {bbox}   → "minx,miny,maxx,maxy" in the source CRS (Web Mercator metres)
//   {width}  → tile pixel width  (always 256)
//   {height} → tile pixel height (always 256)
//   {proj}   → the CRS string (e.g. "EPSG:3857")
// The tile's geographic extent is derived from its slippy x/y/z, so WMS layers
// stay aligned with the slippy/WMTS layers in the same view.
struct TileSource {
    std::string  name;
    std::string  url_template;
    int          max_zoom{19};
    TileCategory category{TileCategory::Street};
    bool         requires_key{false}; // true ↔ url_template contains {key}
    std::string  key;                 // populated by the user; substituted into url
    bool         wms{false};          // true ↔ template uses {bbox}/{width}/{height}/{proj}
    std::string  crs{"EPSG:3857"};    // CRS substituted into {proj} for WMS layers
    float        opacity{1.0f};       // overlay blend alpha (1 = opaque; <1 for hillshade)
};

// Full built-in catalog.  Indices 0 and 1 are backward-compatible with the
// old "0 = ESRI satellite, 1 = OSM" convention stored in config.json.
// Entries with category == Overlay are meant to be layered on top of a base.
inline const std::vector<TileSource>& tile_source_catalog() {
    static const std::vector<TileSource> C = {
        // ── Satellite / Aerial ─────────────────────────────────────────── idx 0-4
        { "ESRI World Imagery",
          "https://server.arcgisonline.com/ArcGIS/rest/services/"
          "World_Imagery/MapServer/tile/{z}/{y}/{x}",
          19, TileCategory::Satellite },
        // idx 1 kept backward-compat (was "OSM Standard" at old index 1)
        { "OSM Standard",
          "https://tile.openstreetmap.org/{z}/{x}/{y}.png",
          19, TileCategory::Street },
        { "ESRI Imagery Clarity",
          "https://clarity.maptiles.arcgis.com/arcgis/rest/services/"
          "World_Imagery/MapServer/tile/{z}/{y}/{x}",
          19, TileCategory::Satellite },
        { "Mapbox Satellite",
          "https://api.mapbox.com/styles/v1/mapbox/satellite-v9/"
          "tiles/{z}/{x}/{y}?access_token={key}",
          22, TileCategory::Satellite, true },
        // Geoportal (Poland) high-resolution orthophoto via WMTS GoogleMaps grid.
        // ~25 cm/px — the best imagery available for PL, ideal for CV detection.
        { "Geoportal Ortofoto (PL)",
          "https://mapy.geoportal.gov.pl/wss/service/WMTS/guest/wmts/ORTO"
          "?SERVICE=WMTS&REQUEST=GetTile&VERSION=1.0.0&LAYER=ORTOFOTOMAPA"
          "&STYLE=default&FORMAT=image/jpeg&TILEMATRIXSET=EPSG:3857"
          "&TILEMATRIX={z}&TILEROW={y}&TILECOL={x}",
          21, TileCategory::Satellite },
        // ── Street ──────────────────────────────────────────────────────── idx 4-8
        { "CartoDB Positron",
          "https://a.basemaps.cartocdn.com/light_all/{z}/{x}/{y}.png",
          20, TileCategory::Street },
        { "CartoDB Dark Matter",
          "https://a.basemaps.cartocdn.com/dark_all/{z}/{x}/{y}.png",
          20, TileCategory::Street },
        { "CartoDB Voyager",
          "https://a.basemaps.cartocdn.com/rastertiles/voyager/{z}/{x}/{y}.png",
          20, TileCategory::Street },
        { "ESRI World Street Map",
          "https://services.arcgisonline.com/ArcGIS/rest/services/"
          "World_Street_Map/MapServer/tile/{z}/{y}/{x}",
          19, TileCategory::Street },
        { "Mapbox Streets",
          "https://api.mapbox.com/styles/v1/mapbox/streets-v12/"
          "tiles/{z}/{x}/{y}?access_token={key}",
          22, TileCategory::Street, true },
        // ── Topo / Terrain ───────────────────────────────────────────────── idx 9-13
        { "OpenTopoMap",
          "https://tile.opentopomap.org/{z}/{x}/{y}.png",
          17, TileCategory::Topo },
        { "ESRI World Topo",
          "https://services.arcgisonline.com/ArcGIS/rest/services/"
          "World_Topo_Map/MapServer/tile/{z}/{y}/{x}",
          19, TileCategory::Topo },
        { "CyclOSM",
          "https://tile-cyclosm.openstreetmap.fr/cyclosm/{z}/{x}/{y}.png",
          20, TileCategory::Topo },
        { "Thunderforest Outdoors",
          "https://tile.thunderforest.com/outdoors/{z}/{x}/{y}.png?apikey={key}",
          22, TileCategory::Topo, true },
        { "Thunderforest OpenCycleMap",
          "https://tile.thunderforest.com/cycle/{z}/{x}/{y}.png?apikey={key}",
          22, TileCategory::Topo, true },
        // ── Overlays (transparent, drawn on top of a base) ────────────── idx 14-17
        { "Waymarked Hiking Trails",
          "https://tile.waymarkedtrails.org/hiking/{z}/{x}/{y}.png",
          19, TileCategory::Overlay },
        { "Waymarked Cycling",
          "https://tile.waymarkedtrails.org/cycling/{z}/{x}/{y}.png",
          19, TileCategory::Overlay },
        { "OpenSnowMap Pistes",
          "https://www.opensnowmap.org/pistes/{z}/{x}/{y}.png",
          18, TileCategory::Overlay },
        { "Thunderforest Landscape",
          "https://tile.thunderforest.com/landscape/{z}/{x}/{y}.png?apikey={key}",
          22, TileCategory::Overlay, true },
        // Geoportal (Poland) shaded-relief (cieniowanie) from the 1 m NMT, served
        // as a WMS GetMap. Grayscale relief drawn semi-transparently over the base
        // so terrain shape shows through the imagery — helps spot embankments,
        // ditches, tracks and field boundaries the imagery alone hides.
        { "Geoportal Cieniowanie (PL)",
          "https://mapy.geoportal.gov.pl/wss/service/PZGIK/NMT/GRID1/WMS/ShadedRelief"
          "?SERVICE=WMS&VERSION=1.3.0&REQUEST=GetMap&LAYERS=Raster&STYLES="
          "&CRS={proj}&BBOX={bbox}&WIDTH={width}&HEIGHT={height}"
          "&FORMAT=image/png&TRANSPARENT=TRUE",
          18, TileCategory::Overlay, /*requires_key=*/false, /*key=*/"",
          /*wms=*/true, /*crs=*/"EPSG:3857", /*opacity=*/0.55f },
        // Geoportal (Poland) NMPT (Digital Surface Model) shaded-relief from the
        // LiDAR point cloud — includes buildings and vegetation canopy on top of
        // the bare ground.  Comparing visually against the NMT cieniowanie above
        // reveals man-made structures.  Also used as an input to the CV detection
        // pipeline: elevated surfaces in the DSM boost building confidence.
        { "Geoportal LiDAR NMPT (PL)",
          "https://mapy.geoportal.gov.pl/wss/service/PZGIK/NMPT/GRID1/WMS/ShadedRelief"
          "?SERVICE=WMS&VERSION=1.3.0&REQUEST=GetMap&LAYERS=Raster&STYLES="
          "&CRS={proj}&BBOX={bbox}&WIDTH={width}&HEIGHT={height}"
          "&FORMAT=image/png&TRANSPARENT=TRUE",
          18, TileCategory::Overlay, /*requires_key=*/false, /*key=*/"",
          /*wms=*/true, /*crs=*/"EPSG:3857", /*opacity=*/0.55f },
    };
    return C;
}

// Catalog index of the first overlay source.
inline int tile_overlay_start() {
    const auto& C = tile_source_catalog();
    for (int i = 0; i < (int)C.size(); ++i)
        if (C[i].category == TileCategory::Overlay) return i;
    return (int)C.size();
}

// Backward-compat helpers
inline TileSource esri_satellite() { return tile_source_catalog()[0]; }
inline TileSource osm_standard()   { return tile_source_catalog()[1]; }

class TileCache {
public:
    explicit TileCache(TileSource source) : source_(std::move(source)) {}

    // Request a tile; starts async fetch if not cached. Returns current state.
    TileState request(const TileCoord& coord);

    // Call from main (GL) thread to upload any pending PNG textures
    void upload_pending();

    // Returns gl texture id or 0 if not ready
    uint32_t texture(const TileCoord& coord) const;

    void clear();

    // Read-only access to the configured source (name, category, opacity, …).
    const TileSource& source() const { return source_; }

    // Read back a cached tile's pixels as 256×256×3 RGB.  Must be called from
    // the GL thread (reads from the GL texture if the PNG has already been
    // consumed by upload_pending()).  Returns empty on cache-miss.
    struct TilePixels { std::vector<uint8_t> rgb; int w{0}, h{0}; };
    TilePixels read_tile_rgb(const TileCoord& coord) const;

    // Convert lat/lon + zoom to tile coord
    static TileCoord lat_lon_to_tile(double lat, double lon, int zoom);

    // Tile bbox in degrees
    static void tile_to_lat_lon(const TileCoord& t,
                                 double& top_lat, double& left_lon,
                                 double& bot_lat, double& right_lon);

private:
    TileSource source_;
    mutable std::mutex mu_;
    std::unordered_map<TileCoord, Tile> tiles_;

    std::string build_url(const TileCoord& c) const;
    void fetch_async(TileCoord coord);
};

} // namespace map
