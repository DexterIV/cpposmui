#pragma once
#include "../osm/types.hpp"
#include "../osm/spatial_index.hpp"
#include "../map/tile_cache.hpp"
#include "../ai/detection.hpp"
#include <imgui.h>
#include <memory>
#include <optional>
#include <functional>
#include <string>
#include <unordered_set>
#include <array>

namespace ui {

// Configurable rendering style for the OSM data layer.
struct MapStyle {
    ImVec4 node_color   {0.86f, 0.86f, 1.00f, 0.70f};
    ImVec4 node_tagged  {1.00f, 0.67f, 0.24f, 0.90f}; // tagged nodes / POIs
    float  node_radius  {3.0f};
    int    node_min_zoom{14};                          // draw nodes at/above this zoom

    ImVec4 way_color    {0.70f, 0.70f, 1.00f, 0.55f};
    float  way_width    {1.5f};

    ImVec4 area_fill    {0.30f, 0.55f, 1.00f, 0.18f};
    ImVec4 area_outline {0.45f, 0.70f, 1.00f, 0.65f};
    float  area_width   {1.5f};

    // ── Prettier-rendering options ──
    bool   show_poi_icons {true};   // draw category icons for tagged POI nodes
    bool   show_labels    {true};   // draw POI name labels when zoomed in
    int    label_min_zoom {16};     // labels appear at/above this zoom
    float  area_opacity   {1.0f};   // global multiplier on area fill alpha
    bool   random_colors  {false};  // color every way/area by a hash of its id
    bool   road_casing    {true};   // draw darker casing under roads
};

class MapView {
public:
    MapView();

    void set_dataset(const osm::Dataset* ds);
    void set_changeset(const osm::ChangeSet* cs) { changeset_ = cs; }
    void set_highlight_id(int64_t id)          { highlight_id_ = id; }

    void center_on(double lat, double lon);
    void center_on_dataset(const osm::Dataset& ds);

    // Renders the map as an ImGui child region (fills available space)
    void draw();

    // Switch tile source
    void set_tile_source(map::TileSource src);

    // ── View state accessors (for persistence) ──
    double center_lat() const { return center_lat_; }
    double center_lon() const { return center_lon_; }
    int    zoom()       const { return zoom_; }
    int    tile_source_index()  const { return selected_zoom_source_; }
    int    overlay_source_index() const { return selected_overlay_; }
    void   set_view(double lat, double lon, int z);
    // idx into tile_source_catalog() (base layers only; overlays ignored here)
    void   set_tile_source_index(int idx);
    // idx into tile_source_catalog(); 0 = no overlay
    void   set_overlay_source_index(int idx);

    // Active base layer's url_template with {key} substituted (still contains
    // {z}/{x}/{y} or, for WMS layers, {bbox}).  Used by the ONNX detector to
    // fetch imagery for inference.
    std::string current_base_url() const;

    // Per-source API key storage (index matches tile_source_catalog())
    void        set_tile_key(int catalog_idx, std::string key);
    std::string tile_key(int catalog_idx) const;
    // Return all stored keys so they can be persisted
    const std::array<std::string, 32>& tile_keys() const { return tile_keys_; }

    // Geographic bbox currently visible (min_lat, min_lon, max_lat, max_lon)
    void   get_view_bbox(double& min_lat, double& min_lon,
                         double& max_lat, double& max_lon) const;

    // Fired when the user clicks "Download OSM data (this view)".
    // Args: min_lat, min_lon, max_lat, max_lon.
    using DownloadFn = std::function<void(double, double, double, double)>;
    void   set_download_callback(DownloadFn fn) { on_download_ = std::move(fn); }
    void   set_status(std::string s) { status_ = std::move(s); }

    // ── Editing (JOSM-like) ──
    enum class Tool { Select, AddNode, AddNodeToWay, DrawWay };

    // The mutable dataset that edits are applied to (the active layer).
    void set_editable_dataset(osm::Dataset* ds) { edit_ds_ = ds; }
    // Called after any edit so the owner can rebuild the spatial index, etc.
    void set_edit_callback(std::function<void()> fn) { on_edit_ = std::move(fn); }

    // Extra read-only layers rendered underneath the active layer (non-owning pointers).
    void set_extra_layers(std::vector<const osm::Dataset*> layers) { extra_layers_ = std::move(layers); }

    void  set_tool(Tool t) { tool_ = t; }
    Tool  tool() const     { return tool_; }

    // ── Single-object selection (legacy accessors – returns first selected) ──
    int64_t selected_node() const { return sel_nodes_.empty() ? 0 : *sel_nodes_.begin(); }
    int64_t selected_way()  const { return sel_ways_ .empty() ? 0 : *sel_ways_ .begin(); }
    bool    has_selection() const { return !sel_nodes_.empty() || !sel_ways_.empty(); }
    void    clear_selection()     { sel_nodes_.clear(); sel_ways_.clear(); }
    void    delete_selection();

    // ── Multi-selection accessors ──
    const std::unordered_set<int64_t>& selected_nodes() const { return sel_nodes_; }
    const std::unordered_set<int64_t>& selected_ways()  const { return sel_ways_; }
    int selection_count() const { return (int)(sel_nodes_.size() + sel_ways_.size()); }

    // Undo / redo (snapshot-based).
    void    undo();
    void    redo();
    bool    can_undo() const { return !undo_.empty(); }
    bool    can_redo() const { return !redo_.empty(); }

    // Draw-way tool: finish the in-progress way / cancel it.
    void    finish_way();
    void    cancel_draw() { draw_nodes_.clear(); }
    bool    drawing() const { return !draw_nodes_.empty(); }

    // Split: splits each selected way at every selected node that lies on it
    // (not the first/last). Result: N+1 ways per split point, each inheriting tags.
    void    split_way();
    // Returns true when the split operation can be performed (one or more ways
    // selected, with at least one intermediate node on them also selected).
    bool    can_split() const;

    // ── AI/CV detection results ──────────────────────────────────────────────
    // Set detection results to render as a map overlay each frame.
    // Pointer must remain valid for the lifetime of the MapView or until reset.
    void set_ai_detections(const ai::DetectionResult* res) { ai_detections_ = res; }

    // Wire the detection panel so draw_ai_overlay() delegates to its full
    // rendering logic (including the show_overlay checkbox and mutex).
    // Forward-declared here; map_view.cpp includes detection_panel.hpp.
    void set_detection_panel(class DetectionPanel* dp);

    // Convert accepted DetectedFeatures into nodes/ways and insert them into
    // the active editing dataset (uses the internal next_new_id_ counter).
    void add_detected_features(const std::vector<ai::DetectedFeature>& feats);

    // Layer visibility (bound directly to ImGui::Checkbox in the Layers panel).
    bool& layer_imagery() { return layer_imagery_; }
    bool& layer_osm()     { return layer_osm_; }
    bool& layer_diff()    { return layer_diff_; }

    // Rendering style (edited by the Style panel).
    MapStyle& style() { return style_; }

    // Has the view (center/zoom/source) changed since the last clear? Used to
    // know when to persist.
    bool   view_dirty() const { return view_dirty_; }
    void   clear_view_dirty()  { view_dirty_ = false; }

private:
    const osm::Dataset*    dataset_{nullptr};
    const osm::ChangeSet*  changeset_{nullptr};
    int64_t                highlight_id_{0};

    // Viewport state
    double center_lat_{52.0}, center_lon_{20.0};
    int    zoom_{14};
    float  pan_x_{0}, pan_y_{0}; // sub-tile pixel offset

    std::unique_ptr<map::TileCache> tiles_;

    // Convert geo <-> screen pixel (relative to canvas origin)
    ImVec2 geo_to_screen(double lat, double lon, ImVec2 canvas_origin, ImVec2 canvas_size) const;
    void   screen_to_geo(ImVec2 px, ImVec2 origin, ImVec2 size,
                         double& lat, double& lon) const;

    void draw_tiles(ImDrawList* dl, ImVec2 origin, ImVec2 size);
    void draw_osm_layer(ImDrawList* dl, ImVec2 origin, ImVec2 size);
    // Draw a specific dataset at a given opacity multiplier (for extra layers).
    void draw_osm_layer_ds(ImDrawList* dl, ImVec2 origin, ImVec2 size,
                           const osm::Dataset* ds, float opacity);
    // Unified, z-ordered feature renderer shared by the two above.
    // use_index: cull ways via the spatial index (only valid for the active dataset_).
    void draw_features(ImDrawList* dl, ImVec2 origin, ImVec2 size,
                       const osm::Dataset* ds, float opacity, bool use_index);
    void draw_diff_overlay(ImDrawList* dl, ImVec2 origin, ImVec2 size);
    void handle_input(ImVec2 origin, ImVec2 size);

    // Editing helpers
    int64_t pick_node(ImVec2 m, ImVec2 origin, ImVec2 size) const;
    int64_t pick_way (ImVec2 m, ImVec2 origin, ImVec2 size) const;
    void    handle_click(ImVec2 m, ImVec2 origin, ImVec2 size);
    void    insert_node_into_way(int64_t way_id, ImVec2 m, ImVec2 origin, ImVec2 size);
    void    push_undo();   // snapshot the dataset before a mutating edit

    int selected_zoom_source_{0}; // index into tile_source_catalog() (base only)
    int selected_overlay_{0};     // 0=none, else catalog index of overlay layer

    // Per-catalog-index API keys (max 32 entries — well above current catalog size)
    std::array<std::string, 32> tile_keys_{};

    std::unique_ptr<map::TileCache> overlay_tiles_; // nullptr if no overlay

    // Helper: rebuild the overlay TileCache from selected_overlay_ + keys
    void rebuild_overlay();

    // Spatial index – rebuilt whenever dataset changes
    std::unique_ptr<osm::SpatialIndex> spatial_index_;

    // Extra (read-only) layers rendered behind the active layer.
    std::vector<const osm::Dataset*> extra_layers_;

    // Last canvas size (set during draw) – needed to compute the visible bbox.
    ImVec2      last_size_{800, 600};
    DownloadFn  on_download_;
    std::string status_;
    bool        view_dirty_{false};

    // ── Editing state ──
    osm::Dataset*         edit_ds_{nullptr};
    std::function<void()> on_edit_;
    Tool     tool_{Tool::Select};

    // Multi-selection
    std::unordered_set<int64_t> sel_nodes_;
    std::unordered_set<int64_t> sel_ways_;

    int64_t  next_new_id_{-1};       // new objects get decreasing negative ids (JOSM convention)
    bool     dragging_node_{false};
    ImVec2   press_pos_{0, 0};

    // Right-click pan (JOSM style)
    bool   panning_{false};

    // Box (rubber-band) selection
    bool   box_selecting_{false};
    ImVec2 box_start_{0, 0};   // screen coords
    ImVec2 box_end_{0, 0};

    std::vector<osm::Dataset> undo_;   // snapshots for undo
    std::vector<osm::Dataset> redo_;
    std::vector<int64_t>      draw_nodes_; // node ids of the way being drawn

    // Layer visibility
    bool layer_imagery_{true};
    bool layer_osm_{true};
    bool layer_diff_{true};

    MapStyle style_;

    // AI/CV detection overlay.
    const ai::DetectionResult* ai_detections_{nullptr};
    class DetectionPanel* detection_panel_{nullptr}; // non-owning, forward-declared
    void draw_ai_overlay(ImDrawList* dl, ImVec2 origin, ImVec2 size);
};

} // namespace ui
