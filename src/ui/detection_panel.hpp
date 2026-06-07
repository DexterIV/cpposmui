#pragma once
#include "../ai/detection.hpp"
#include "../osm/types.hpp"
#include <imgui.h>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <functional>

namespace ui {

// Panel for AI/CV-assisted feature detection.
// Workflow:
//   1. User selects source (MS Buildings, MapWithAI, or custom REST).
//   2. "Detect" button fires an async request for the current view bbox.
//   3. Results appear as a list; each feature can be accepted or rejected.
//   4. "Accept selected" adds chosen features to the active editing layer.
//
// The panel also exposes draw_map_overlay() so the calling code can render
// candidate features on top of the map every frame.
class DetectionPanel {
public:
    // Called once to wire up the callback that adds accepted features to the
    // active OSM dataset.  Callback receives the list of accepted features; the
    // panel clears them after calling the callback.
    using AcceptFn = std::function<void(std::vector<ai::DetectedFeature>)>;
    void set_accept_callback(AcceptFn fn) { on_accept_ = std::move(fn); }

    // Push current view bbox (used when the user hits Detect).
    void set_bbox(double min_lat, double min_lon,
                  double max_lat, double max_lon)
    {
        min_lat_ = min_lat; min_lon_ = min_lon;
        max_lat_ = max_lat; max_lon_ = max_lon;
    }

    // Push the active base imagery layer so the local ONNX model can fetch and
    // stitch its tiles.  url_template uses {z}/{x}/{y} with the API key already
    // substituted; zoom is the level the mosaic is assembled at.
    void set_imagery(std::string url_template, int zoom) {
        imagery_url_  = std::move(url_template);
        imagery_zoom_ = zoom;
    }

    // Callback that assembles a stitched RGB mosaic from the tile cache for the
    // current view bbox.  Returns (pixels, width, height); empty on failure.
    // Must be called from the GL thread.
    struct Mosaic { std::vector<uint8_t> rgb; int w{0}, h{0}; };
    using MosaicFn = std::function<Mosaic(double min_lat, double min_lon,
                                          double max_lat, double max_lon, int zoom)>;
    void set_mosaic_callback(MosaicFn fn) { mosaic_fn_ = std::move(fn); }

    // Callback that assembles a grayscale mosaic from a specific overlay tile
    // cache (e.g. the NMPT LiDAR layer).  Same bbox/zoom contract as MosaicFn.
    using DsmMosaicFn = std::function<Mosaic(double min_lat, double min_lon,
                                             double max_lat, double max_lon, int zoom)>;
    void set_dsm_mosaic_callback(DsmMosaicFn fn) { dsm_mosaic_fn_ = std::move(fn); }

    // Map click interaction: select/approve/reject features by clicking on the map.
    void select_feature(int idx);
    int  selected_feature() const { return selected_idx_; }
    void approve_selected();
    void reject_selected();

    // Render the dockable panel window.
    void draw(bool* p_open);

    // Render candidate feature overlays into the map DrawList.
    // geo_to_screen: converts (lat, lon) → screen pixel.
    void draw_map_overlay(
        ImDrawList* dl,
        const std::function<ImVec2(double lat, double lon)>& geo_to_screen) const;

    // True while a detection request is in flight.
    bool is_running() const { return running_; }

private:
    double min_lat_{0}, min_lon_{0}, max_lat_{0}, max_lon_{0};

    // UI state
    // 0=MS Buildings, 1=MapWithAI Roads, 2=Custom REST, 3=ONNX,
    // 4=CV on imagery, 5=CV + LiDAR DSM
    int  source_{0};
    std::string custom_url_{"https://example.com/detect?bbox={bbox}"};
    std::string model_path_;        // ONNX model file
    std::string imagery_url_;       // active base tile template (key substituted)
    int         imagery_zoom_{17};  // zoom the ONNX mosaic is assembled at

    MosaicFn    mosaic_fn_;         // assembles RGB mosaic from base tile cache
    DsmMosaicFn dsm_mosaic_fn_;     // assembles grayscale mosaic from DSM overlay

    // Detection state
    bool running_{false};
    std::string status_;

    mutable std::mutex result_mu_;
    // Updated on a background thread; shared_ptr lets main thread safely hold a
    // frame-local copy while the panel may swap in a new result concurrently.
    std::shared_ptr<ai::DetectionResult> result_;

public:
    // Returns a shared_ptr snapshot safe to read on the main thread for one frame.
    std::shared_ptr<const ai::DetectionResult> result_sp() const {
        std::scoped_lock lk(result_mu_);
        return result_;
    }
private:

    // Per-feature checkboxes (parallel to result_->features)
    std::vector<bool> checked_;
    bool show_overlay_{true};
    int  selected_idx_{-1};  // map-clicked feature index, or -1

    AcceptFn on_accept_;

    void run_detection();
    void accept_checked();
};

} // namespace ui
