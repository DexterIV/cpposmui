#pragma once
#include "../osm/types.hpp"
#include <vector>
#include <string>
#include <functional>
#include <utility>

namespace ai {

// Semantic type of an AI-detected map feature.
enum class FeatureType { Road, Building, Waterway, Landuse, POI, Unknown };

inline const char* feature_type_name(FeatureType t) {
    switch (t) {
    case FeatureType::Road:      return "Road";
    case FeatureType::Building:  return "Building";
    case FeatureType::Waterway:  return "Waterway";
    case FeatureType::Landuse:   return "Land use";
    case FeatureType::POI:       return "POI";
    default:                     return "Unknown";
    }
}

struct DetectedFeature {
    FeatureType  type{FeatureType::Unknown};
    float        confidence{1.0f};  // 0..1
    // Geometry as (lat, lon) pairs.  is_area=true → closed polygon.
    std::vector<std::pair<double,double>> coords;
    bool         is_area{false};
    // Suggested OSM tags for this feature.
    osm::TagMap  suggested_tags;
    // Provenance label (e.g. "msft-buildings", "mapwithai", "cv-edge", "onnx-local").
    std::string  source;

    // User review state (set by the detection panel).
    enum class Status { Pending, Accepted, Rejected } status{Status::Pending};
};

struct DetectionResult {
    std::vector<DetectedFeature> features;
    std::string source_name;
    std::string error;
    bool ok() const { return error.empty(); }
};

// Async callback — invoked on a background thread.
using Callback = std::function<void(DetectionResult)>;

// ── Detection providers ────────────────────────────────────────────────────

// Microsoft Global Building Footprints (public, no API key required).
// Fetches the quadkey-indexed GeoJSON blobs from Azure storage.
void detect_ms_buildings(double min_lat, double min_lon,
                          double max_lat, double max_lon,
                          Callback cb);

// Facebook / Meta MapWithAI — AI-detected roads returned as OSM XML.
// The API is public but may be rate-limited; results vary by region.
void detect_mapwithai_roads(double min_lat, double min_lon,
                             double max_lat, double max_lon,
                             Callback cb);

// Generic REST endpoint.  {bbox} in url_template is replaced with
// "min_lon,min_lat,max_lon,max_lat".  Response must be a GeoJSON
// FeatureCollection.  geometry types Polygon/MultiPolygon → is_area=true,
// LineString/MultiLineString → road/waterway polylines.
void detect_custom_rest(const std::string& url_template,
                         double min_lat, double min_lon,
                         double max_lat, double max_lon,
                         Callback cb);

// Classical CV on pre-assembled tile imagery: Sobel edge detection + color
// segmentation to extract building footprints and road centre-lines.
// rgb_pixels: H*W*3 row-major RGB bytes.  bbox: geographic extent of the image.
// simplify_px: Douglas-Peucker tolerance in pixels for building outlines
// (roads use 1.4×). Higher = fewer nodes per feature.
void detect_cv_on_imagery(const std::vector<uint8_t>& rgb_pixels,
                          int img_w, int img_h,
                          double min_lat, double min_lon,
                          double max_lat, double max_lon,
                          Callback cb, double simplify_px = 3.5);

// Classical CV enhanced with LiDAR-derived DSM (NMPT) shaded relief.
// dsm_gray: H*W single-channel grayscale raster from the DSM WMS (same
// dimensions as rgb_pixels).  Elevated pixels in the DSM independently
// flag building candidates, boosting confidence when both edge and height
// evidence agree.
void detect_cv_with_dsm(const std::vector<uint8_t>& rgb_pixels,
                         const std::vector<uint8_t>& dsm_gray,
                         int img_w, int img_h,
                         double min_lat, double min_lon,
                         double max_lat, double max_lon,
                         Callback cb, double simplify_px = 3.5);

// BBox-driven CV: fetches the imagery tiles for the bbox from a slippy/WMTS
// imagery layer ({z}/{x}/{y}, key substituted) at `zoom`, stitches them, and
// runs the classical CV pipeline.  Self-contained — no GL tile cache needed.
void detect_cv_bbox(const std::string& imagery_url, int zoom,
                    double min_lat, double min_lon,
                    double max_lat, double max_lon,
                    Callback cb, double simplify_px = 3.5);

// BBox-driven CV + LiDAR: as above, plus fetches a co-registered DSM mosaic from
// a WMS shaded-relief layer (dsm_wms_url uses {bbox}/{width}/{height}/{proj},
// e.g. the Geoportal NMPT LiDAR layer) and cross-references elevation evidence.
// The DSM always comes from this layer regardless of which overlay is displayed.
void detect_cv_lidar_bbox(const std::string& imagery_url,
                          const std::string& dsm_wms_url, int zoom,
                          double min_lat, double min_lon,
                          double max_lat, double max_lon,
                          Callback cb, double simplify_px = 3.5);

#ifdef CPPOSMUI_HAVE_ONNXRUNTIME
// Local ONNX model inference on the assembled tile imagery.
// model_path: path to an ONNX segmentation model.
//   Expected input:  float32[1, 3, H, W]  (RGB, normalised 0-1)
//   Expected output: float32[1, C, H, W]  (per-class softmax scores)
//                    Class order: 0=background, 1=road, 2=building, 3=water
// rgb_pixels: H*W*3 row-major RGB bytes.
// bbox: geographic extent of the image.
void detect_via_onnx(const std::string& model_path,
                      const std::vector<uint8_t>& rgb_pixels,
                      int img_w, int img_h,
                      double min_lat, double min_lon,
                      double max_lat, double max_lon,
                      Callback cb);

// Convenience driver: fetch the imagery tiles covering the bbox at `zoom` from a
// slippy/WMTS url_template (placeholders {z}/{x}/{y}, key already substituted),
// stitch them into one RGB mosaic, and run the model over it.
void detect_via_onnx_bbox(const std::string& model_path,
                          const std::string& url_template, int zoom,
                          double min_lat, double min_lon,
                          double max_lat, double max_lon,
                          Callback cb);
#endif

} // namespace ai
