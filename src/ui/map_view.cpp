#include "map_view.hpp"
#include "detection_panel.hpp"
#include "../osm/diff.hpp"
#include "../osm/spatial_index.hpp"
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <mapbox/earcut.hpp>
#include <cmath>
#include <cstring>
#include <numbers>
#include <future>
#include <string_view>
#include <array>

namespace ui {

constexpr double PI = std::numbers::pi;

// Triangulate a simple polygon with earcut and submit it as ImGui triangles.
// Handles convex, concave, and arbitrary CW/CCW winding — no GL state changes needed.
static void fill_polygon_earcut(ImDrawList* dl,
                                const std::vector<ImVec2>& pts, ImU32 col) {
    if (pts.size() < 3) return;
    using Pt = std::array<float, 2>;
    std::vector<std::vector<Pt>> poly(1);
    poly[0].reserve(pts.size());
    for (const auto& p : pts) poly[0].push_back({p.x, p.y});
    auto idx = mapbox::earcut<uint32_t>(poly);
    for (size_t i = 0; i + 2 < idx.size(); i += 3)
        dl->AddTriangleFilled(pts[idx[i]], pts[idx[i+1]], pts[idx[i+2]], col);
}
constexpr int TILE_PX = 256;

static double wrap_lon(double lon) {
    lon = std::fmod(lon + 180.0, 360.0);
    if (lon < 0) lon += 360.0;
    return lon - 180.0;
}

MapView::MapView() {
    tiles_ = std::make_unique<map::TileCache>(map::esri_satellite());
}

void MapView::set_tile_source(map::TileSource src) {
    tiles_ = std::make_unique<map::TileCache>(std::move(src));
}

void MapView::set_dataset(const osm::Dataset* ds) {
    dataset_ = ds;
    if (ds) {
        // Build spatial index asynchronously so UI doesn't block
        auto fut = std::async(std::launch::async, [ds] {
            return osm::SpatialIndex::build(*ds);
        });
        spatial_index_ = std::make_unique<osm::SpatialIndex>(fut.get());
    } else {
        spatial_index_.reset();
    }
}

void MapView::center_on(double lat, double lon) {
    center_lat_ = std::clamp(lat, -85.0, 85.0);
    center_lon_ = wrap_lon(lon);
    view_dirty_ = true;
}

void MapView::set_view(double lat, double lon, int z) {
    center_lat_ = std::clamp(lat, -85.0, 85.0);
    center_lon_ = wrap_lon(lon);
    zoom_ = std::clamp(z, 2, 21);
    view_dirty_ = true;
}

void MapView::set_tile_source_index(int idx) {
    const auto& catalog = map::tile_source_catalog();
    if (idx < 0 || idx >= (int)catalog.size()) return;
    if (catalog[idx].category == map::TileCategory::Overlay) return; // not a base layer
    selected_zoom_source_ = idx;
    map::TileSource src = catalog[idx];
    if (idx < (int)tile_keys_.size()) src.key = tile_keys_[idx];
    set_tile_source(std::move(src));
    view_dirty_ = true;
}

void MapView::set_overlay_source_index(int idx) {
    selected_overlay_ = idx;
    rebuild_overlay();
    view_dirty_ = true;
}

void MapView::set_tile_key(int catalog_idx, std::string key) {
    if (catalog_idx < 0 || catalog_idx >= (int)tile_keys_.size()) return;
    tile_keys_[catalog_idx] = std::move(key);
    // Re-apply if this is the currently active base or overlay source
    if (catalog_idx == selected_zoom_source_) {
        const auto& catalog = map::tile_source_catalog();
        map::TileSource src = catalog[catalog_idx];
        src.key = tile_keys_[catalog_idx];
        set_tile_source(std::move(src));
    }
    if (catalog_idx == selected_overlay_) rebuild_overlay();
}

std::string MapView::tile_key(int catalog_idx) const {
    if (catalog_idx < 0 || catalog_idx >= (int)tile_keys_.size()) return {};
    return tile_keys_[catalog_idx];
}

std::string MapView::current_base_url() const {
    const auto& catalog = map::tile_source_catalog();
    if (selected_zoom_source_ < 0 || selected_zoom_source_ >= (int)catalog.size())
        return {};
    std::string url = catalog[selected_zoom_source_].url_template;
    std::string key = (selected_zoom_source_ < (int)tile_keys_.size())
        ? tile_keys_[selected_zoom_source_] : std::string();
    for (size_t p; (p = url.find("{key}")) != std::string::npos;)
        url.replace(p, 5, key);
    return url;
}

void MapView::rebuild_overlay() {
    if (selected_overlay_ <= 0) { overlay_tiles_.reset(); return; }
    const auto& catalog = map::tile_source_catalog();
    if (selected_overlay_ >= (int)catalog.size()) { overlay_tiles_.reset(); return; }
    map::TileSource src = catalog[selected_overlay_];
    if (selected_overlay_ < (int)tile_keys_.size()) src.key = tile_keys_[selected_overlay_];
    overlay_tiles_ = std::make_unique<map::TileCache>(std::move(src));
}

void MapView::get_view_bbox(double& min_lat, double& min_lon,
                            double& max_lat, double& max_lon) const {
    double vp_half_lon = last_size_.x * 0.5 / TILE_PX * (360.0 / (1 << zoom_));
    double lat_r       = center_lat_ * PI / 180.0;
    // Latitude degrees per pixel shrink with cos(lat) in Web Mercator.
    double vp_half_lat = last_size_.y * 0.5 / TILE_PX * (360.0 / (1 << zoom_))
                         * std::cos(lat_r);
    min_lat = std::clamp(center_lat_ - vp_half_lat, -85.0, 85.0);
    max_lat = std::clamp(center_lat_ + vp_half_lat, -85.0, 85.0);
    min_lon = std::clamp(center_lon_ - vp_half_lon, -180.0, 180.0);
    max_lon = std::clamp(center_lon_ + vp_half_lon, -180.0, 180.0);
}

void MapView::center_on_dataset(const osm::Dataset& ds) {
    if (ds.nodes.empty()) return;
    center_lat_ = std::clamp((ds.min_lat + ds.max_lat) / 2.0, -85.0, 85.0);
    center_lon_ = wrap_lon((ds.min_lon + ds.max_lon) / 2.0);
    // Rough zoom fit
    double dlat = ds.max_lat - ds.min_lat;
    double dlon = ds.max_lon - ds.min_lon;
    double deg = std::max(dlat, dlon);
    zoom_ = std::clamp((int)(std::log2(360.0 / deg)), 3, 18);
}

ImVec2 MapView::geo_to_screen(double lat, double lon,
                               ImVec2 origin, ImVec2 size) const {
    int n = 1 << zoom_;
    // Center tile
    double cx_tile = (center_lon_ + 180.0) / 360.0 * n;
    double lat_r   = center_lat_ * PI / 180.0;
    double cy_tile = (1.0 - std::log(std::tan(lat_r) + 1.0/std::cos(lat_r)) / PI) / 2.0 * n;

    double px_tile = (lon + 180.0) / 360.0 * n;
    double py_lat  = lat * PI / 180.0;
    double py_tile = (1.0 - std::log(std::tan(py_lat) + 1.0/std::cos(py_lat)) / PI) / 2.0 * n;

    float sx = origin.x + size.x * 0.5f + (float)((px_tile - cx_tile) * TILE_PX);
    float sy = origin.y + size.y * 0.5f + (float)((py_tile - cy_tile) * TILE_PX);
    return {sx, sy};
}

void MapView::screen_to_geo(ImVec2 px, ImVec2 origin, ImVec2 size,
                            double& lat, double& lon) const {
    int n = 1 << zoom_;
    double cx_tile = (center_lon_ + 180.0) / 360.0 * n;
    double lat_r   = center_lat_ * PI / 180.0;
    double cy_tile = (1.0 - std::log(std::tan(lat_r) + 1.0/std::cos(lat_r)) / PI) / 2.0 * n;

    double px_tile = cx_tile + (px.x - origin.x - size.x * 0.5f) / TILE_PX;
    double py_tile = cy_tile + (px.y - origin.y - size.y * 0.5f) / TILE_PX;

    lon = px_tile / n * 360.0 - 180.0;
    lat = std::atan(std::sinh(PI * (1.0 - 2.0 * py_tile / n))) * 180.0 / PI;
}

// Distance² from point p to segment ab, in pixels.
static float dist2_to_segment(ImVec2 p, ImVec2 a, ImVec2 b) {
    float vx = b.x - a.x, vy = b.y - a.y;
    float wx = p.x - a.x, wy = p.y - a.y;
    float len2 = vx*vx + vy*vy;
    float t = len2 > 0 ? (wx*vx + wy*vy) / len2 : 0.f;
    t = std::clamp(t, 0.f, 1.f);
    float dx = a.x + t*vx - p.x, dy = a.y + t*vy - p.y;
    return dx*dx + dy*dy;
}

int64_t MapView::pick_node(ImVec2 m, ImVec2 origin, ImVec2 size) const {
    if (!dataset_) return 0;
    constexpr float R2 = 10.f * 10.f; // pick radius²
    int64_t best = 0; float best_d2 = R2;
    for (const auto& [id, n] : dataset_->nodes) {
        ImVec2 p = geo_to_screen(n.lat, n.lon, origin, size);
        float dx = p.x - m.x, dy = p.y - m.y, d2 = dx*dx + dy*dy;
        if (d2 < best_d2) { best_d2 = d2; best = id; }
    }
    return best;
}

// Ray-casting point-in-polygon test for closed way (area) selection.
static bool point_in_polygon(ImVec2 p, const std::vector<ImVec2>& pts) {
    bool inside = false;
    for (size_t i = 0, j = pts.size() - 1; i < pts.size(); j = i++) {
        float xi = pts[i].x, yi = pts[i].y;
        float xj = pts[j].x, yj = pts[j].y;
        if (((yi > p.y) != (yj > p.y)) &&
            (p.x < (xj - xi) * (p.y - yi) / (yj - yi) + xi))
            inside = !inside;
    }
    return inside;
}

int64_t MapView::pick_way(ImVec2 m, ImVec2 origin, ImVec2 size) const {
    if (!dataset_) return 0;
    constexpr float R2 = 8.f * 8.f;
    // Segment hit (outline click) and area interior hit are tracked separately.
    // Segment hit takes priority; area interior is a fallback.
    int64_t best_seg = 0;  float best_d2 = R2;
    int64_t best_area = 0;

    auto is_closed_way = [](const osm::Way& w) {
        return w.nodes.size() >= 4 && w.nodes.front().ref == w.nodes.back().ref;
    };

    for (const auto& [id, w] : dataset_->ways) {
        // Segment hit-test (outline of way or area).
        for (size_t i = 0; i + 1 < w.nodes.size(); ++i) {
            auto a = dataset_->nodes.find(w.nodes[i].ref);
            auto b = dataset_->nodes.find(w.nodes[i+1].ref);
            if (a == dataset_->nodes.end() || b == dataset_->nodes.end()) continue;
            ImVec2 pa = geo_to_screen(a->second.lat, a->second.lon, origin, size);
            ImVec2 pb = geo_to_screen(b->second.lat, b->second.lon, origin, size);
            float d2 = dist2_to_segment(m, pa, pb);
            if (d2 < best_d2) { best_d2 = d2; best_seg = id; }
        }
        // For closed ways: also test whether the click is inside the polygon.
        if (!best_area && is_closed_way(w)) {
            std::vector<ImVec2> pts;
            for (const auto& nd : w.nodes) {
                auto it = dataset_->nodes.find(nd.ref);
                if (it == dataset_->nodes.end()) continue;
                pts.push_back(geo_to_screen(it->second.lat, it->second.lon, origin, size));
            }
            if (!pts.empty() && point_in_polygon(m, pts))
                best_area = id;
        }
    }
    // Prefer the nearest outline click; fall back to interior hit.
    return best_seg ? best_seg : best_area;
}

void MapView::insert_node_into_way(int64_t way_id, ImVec2 m, ImVec2 origin, ImVec2 size) {
    if (!edit_ds_) return;
    auto wit = edit_ds_->ways.find(way_id);
    if (wit == edit_ds_->ways.end()) return;
    auto& w = wit->second;

    // Find the segment closest to the click; insert the new node after node i.
    size_t best_i = 0; float best_d2 = 1e30f; bool found = false;
    for (size_t i = 0; i + 1 < w.nodes.size(); ++i) {
        auto a = edit_ds_->nodes.find(w.nodes[i].ref);
        auto b = edit_ds_->nodes.find(w.nodes[i+1].ref);
        if (a == edit_ds_->nodes.end() || b == edit_ds_->nodes.end()) continue;
        ImVec2 pa = geo_to_screen(a->second.lat, a->second.lon, origin, size);
        ImVec2 pb = geo_to_screen(b->second.lat, b->second.lon, origin, size);
        float d2 = dist2_to_segment(m, pa, pb);
        if (d2 < best_d2) { best_d2 = d2; best_i = i; found = true; }
    }
    if (!found) return;

    double lat, lon; screen_to_geo(m, origin, size, lat, lon);
    osm::Node nn; nn.id = next_new_id_--; nn.lat = lat; nn.lon = lon;
    nn.version = 0; nn.visible = true;
    edit_ds_->nodes[nn.id] = nn;
    w.nodes.insert(w.nodes.begin() + best_i + 1, osm::WayNode{nn.id});
    sel_nodes_ = {nn.id}; sel_ways_.clear();
    if (on_edit_) on_edit_();
}

void MapView::delete_selection() {
    if (!edit_ds_ || !has_selection()) return;
    push_undo();

    // Delete selected nodes
    for (int64_t nid : sel_nodes_) {
        edit_ds_->nodes.erase(nid);
        for (auto& [id, w] : edit_ds_->ways)
            std::erase_if(w.nodes, [nid](const osm::WayNode& wn){ return wn.ref == nid; });
    }

    // Delete selected ways (and their orphaned untagged nodes)
    for (int64_t wid : sel_ways_) {
        std::vector<int64_t> way_nodes;
        if (auto wit = edit_ds_->ways.find(wid); wit != edit_ds_->ways.end())
            for (const auto& wn : wit->second.nodes) way_nodes.push_back(wn.ref);
        edit_ds_->ways.erase(wid);
        for (int64_t nid : way_nodes) {
            bool shared = false;
            for (const auto& [wid2, w] : edit_ds_->ways)
                for (const auto& wn : w.nodes)
                    if (wn.ref == nid) { shared = true; break; }
            if (!shared) {
                auto nit = edit_ds_->nodes.find(nid);
                if (nit != edit_ds_->nodes.end() && nit->second.tags.empty())
                    edit_ds_->nodes.erase(nit);
            }
        }
    }

    sel_nodes_.clear();
    sel_ways_.clear();
    if (on_edit_) on_edit_();
}

void MapView::push_undo() {
    if (!edit_ds_) return;
    undo_.push_back(*edit_ds_);
    if (undo_.size() > 64) undo_.erase(undo_.begin());
    redo_.clear();
}

void MapView::undo() {
    if (undo_.empty() || !edit_ds_) return;
    redo_.push_back(*edit_ds_);
    *edit_ds_ = std::move(undo_.back());
    undo_.pop_back();
    clear_selection();
    draw_nodes_.clear();
    if (on_edit_) on_edit_();
}

void MapView::redo() {
    if (redo_.empty() || !edit_ds_) return;
    undo_.push_back(*edit_ds_);
    *edit_ds_ = std::move(redo_.back());
    redo_.pop_back();
    clear_selection();
    if (on_edit_) on_edit_();
}

void MapView::finish_way() {
    if (!edit_ds_ || draw_nodes_.size() < 2) { draw_nodes_.clear(); return; }
    int64_t wid = next_new_id_--;
    osm::Way w;
    w.id = wid;
    w.version = 0; w.visible = true;
    for (int64_t nid : draw_nodes_) w.nodes.push_back(osm::WayNode{nid});
    edit_ds_->ways[wid] = std::move(w);
    sel_ways_ = {wid}; sel_nodes_.clear();
    draw_nodes_.clear();
    if (on_edit_) on_edit_();
}

bool MapView::can_split() const {
    if (!edit_ds_ || sel_ways_.empty() || sel_nodes_.empty()) return false;
    for (int64_t wid : sel_ways_) {
        auto wit = edit_ds_->ways.find(wid);
        if (wit == edit_ds_->ways.end()) continue;
        const auto& nodes = wit->second.nodes;
        // Need at least one selected node that is an *intermediate* node of the way.
        for (size_t i = 1; i + 1 < nodes.size(); ++i)
            if (sel_nodes_.contains(nodes[i].ref)) return true;
    }
    return false;
}

// ── Tile mosaic assembly for CV detection ─────────────────────────────────────
static DetectionPanel::Mosaic assemble_mosaic_helper(
    map::TileCache& cache, double min_lat, double min_lon,
    double max_lat, double max_lon, int zoom)
{
    auto nw = map::TileCache::lat_lon_to_tile(max_lat, min_lon, zoom);
    auto se = map::TileCache::lat_lon_to_tile(min_lat, max_lon, zoom);
    int tx0 = nw.x, ty0 = nw.y, tx1 = se.x, ty1 = se.y;
    if (tx0 > tx1) std::swap(tx0, tx1);
    if (ty0 > ty1) std::swap(ty0, ty1);

    int cols = tx1 - tx0 + 1, rows = ty1 - ty0 + 1;
    if (cols * rows > 64) return {};

    int W = cols * TILE_PX, H = rows * TILE_PX;
    std::vector<uint8_t> rgb((size_t)W * H * 3, 0);
    int ok = 0;

    for (int ty = ty0; ty <= ty1; ++ty) {
        for (int tx = tx0; tx <= tx1; ++tx) {
            auto tp = cache.read_tile_rgb({zoom, tx, ty});
            if (tp.rgb.empty()) continue;
            ++ok;
            int ox = (tx - tx0) * TILE_PX, oy = (ty - ty0) * TILE_PX;
            int cw = std::min(tp.w, TILE_PX), ch = std::min(tp.h, TILE_PX);
            for (int y = 0; y < ch; ++y)
                std::memcpy(&rgb[((size_t)(oy + y) * W + ox) * 3],
                            &tp.rgb[(size_t)y * tp.w * 3], (size_t)cw * 3);
        }
    }
    if (ok == 0) return {};
    return {std::move(rgb), W, H};
}

void MapView::set_detection_panel(DetectionPanel* dp) {
    detection_panel_ = dp;
    if (!dp) return;
    dp->set_mosaic_callback(
        [this](double a, double b, double c, double d, int z) {
            return assemble_mosaic_helper(*tiles_, a, b, c, d, z);
        });
    dp->set_dsm_mosaic_callback(
        [this](double a, double b, double c, double d, int z) -> DetectionPanel::Mosaic {
            if (!overlay_tiles_) return {};
            return assemble_mosaic_helper(*overlay_tiles_, a, b, c, d, z);
        });
}

void MapView::add_detected_features(const std::vector<ai::DetectedFeature>& feats) {
    if (!edit_ds_) return;
    push_undo();
    for (const auto& f : feats) {
        if (f.coords.empty()) continue;
        if (f.coords.size() == 1) {
            // POI node
            osm::Node n;
            n.id = next_new_id_--; n.lat = f.coords[0].first; n.lon = f.coords[0].second;
            n.version = 0; n.visible = true; n.tags = f.suggested_tags;
            edit_ds_->nodes[n.id] = n;
        } else {
            // Way (road or building polygon)
            std::vector<int64_t> node_ids;
            for (const auto& [lat, lon] : f.coords) {
                osm::Node n;
                n.id = next_new_id_--; n.lat = lat; n.lon = lon;
                n.version = 0; n.visible = true;
                edit_ds_->nodes[n.id] = n;
                node_ids.push_back(n.id);
            }
            osm::Way w;
            w.id = next_new_id_--; w.version = 0; w.visible = true;
            w.tags = f.suggested_tags;
            for (int64_t nid : node_ids) w.nodes.push_back({nid});
            if (f.is_area && !node_ids.empty() &&
                node_ids.front() != node_ids.back())
                w.nodes.push_back({node_ids.front()}); // close ring
            edit_ds_->ways[w.id] = std::move(w);
        }
    }
    if (on_edit_) on_edit_();
}

void MapView::draw_ai_overlay(ImDrawList* dl, ImVec2 origin, ImVec2 size) {
    // Prefer the panel's renderer — it respects the show_overlay checkbox and
    // uses its internal mutex to read the result safely.
    if (detection_panel_) {
        detection_panel_->draw_map_overlay(dl,
            [&](double lat, double lon) { return geo_to_screen(lat, lon, origin, size); });
        return;
    }
    // Fallback: render directly from the pointer set via set_ai_detections().
    if (!ai_detections_) return;
    for (const auto& f : ai_detections_->features) {
        if (f.status == ai::DetectedFeature::Status::Rejected) continue;
        ImU32 col;
        switch (f.status) {
        case ai::DetectedFeature::Status::Accepted: col = IM_COL32( 80,220, 80,210); break;
        default:
            switch (f.type) {
            case ai::FeatureType::Building: col = IM_COL32(255,160, 60,200); break;
            case ai::FeatureType::Road:     col = IM_COL32( 60,200,255,200); break;
            case ai::FeatureType::Waterway: col = IM_COL32( 60,120,255,200); break;
            case ai::FeatureType::Landuse:  col = IM_COL32(140,220,100,180); break;
            default:                        col = IM_COL32(200,200, 60,200); break;
            }
        }
        std::vector<ImVec2> pts;
        pts.reserve(f.coords.size());
        for (const auto& [lat, lon] : f.coords)
            pts.push_back(geo_to_screen(lat, lon, origin, size));

        if (pts.size() == 1) {
            dl->AddCircleFilled(pts[0], 6.f, col);
            dl->AddCircle(pts[0], 8.f, IM_COL32(255,255,255,180), 0, 1.5f);
        } else if (pts.size() >= 2) {
            if (f.is_area) {
                ImU32 fill = (col & 0x00FFFFFF) | 0x40000000;
                for (size_t i = 1; i + 1 < pts.size(); ++i)
                    dl->AddTriangleFilled(pts[0], pts[i], pts[i+1], fill);
                dl->AddPolyline(pts.data(), (int)pts.size(), col,
                                ImDrawFlags_Closed, 2.5f);
            } else {
                dl->AddPolyline(pts.data(), (int)pts.size(), col,
                                ImDrawFlags_None, 2.5f);
                dl->AddCircleFilled(pts.front(), 3.f, col);
                dl->AddCircleFilled(pts.back(),  3.f, col);
            }
        }
    }
}

void MapView::split_way() {
    if (!edit_ds_ || !can_split()) return;
    push_undo();

    std::unordered_set<int64_t> new_ways;

    for (int64_t wid : sel_ways_) {
        auto wit = edit_ds_->ways.find(wid);
        if (wit == edit_ds_->ways.end()) continue;
        osm::Way& orig = wit->second;

        // Collect split points: indices of intermediate nodes that are selected.
        std::vector<size_t> splits;
        for (size_t i = 1; i + 1 < orig.nodes.size(); ++i)
            if (sel_nodes_.contains(orig.nodes[i].ref)) splits.push_back(i);
        if (splits.empty()) continue;

        // Build segment slices: [0..s0], [s0..s1], ... [sN..end]
        std::vector<std::vector<osm::WayNode>> segments;
        size_t start = 0;
        for (size_t sp : splits) {
            segments.push_back({orig.nodes.begin() + start,
                                orig.nodes.begin() + sp + 1});
            start = sp;
        }
        segments.push_back({orig.nodes.begin() + start, orig.nodes.end()});

        // First segment reuses the original way id; rest get new ids.
        orig.nodes = segments[0];
        new_ways.insert(wid);
        for (size_t k = 1; k < segments.size(); ++k) {
            int64_t new_id = next_new_id_--;
            osm::Way nw;
            nw.id      = new_id;
            nw.version = 0;
            nw.visible = true;
            nw.tags    = orig.tags; // inherit tags
            nw.nodes   = segments[k];
            edit_ds_->ways[new_id] = std::move(nw);
            new_ways.insert(new_id);
        }
    }

    // Select all resulting ways, deselect split nodes.
    sel_ways_ = std::move(new_ways);
    sel_nodes_.clear();
    if (on_edit_) on_edit_();
}

void MapView::handle_click(ImVec2 m, ImVec2 origin, ImVec2 size) {
    switch (tool_) {
    case Tool::Select: {
        bool shift = ImGui::GetIO().KeyShift;
        if (int64_t n = pick_node(m, origin, size)) {
            if (!shift) { sel_nodes_.clear(); sel_ways_.clear(); }
            if (sel_nodes_.contains(n)) sel_nodes_.erase(n); // toggle off
            else                        sel_nodes_.insert(n);
        } else if (int64_t w = pick_way(m, origin, size)) {
            if (!shift) { sel_nodes_.clear(); sel_ways_.clear(); }
            if (sel_ways_.contains(w)) sel_ways_.erase(w);
            else                       sel_ways_.insert(w);
        } else if (!shift) {
            // Click on empty space without Shift → deselect all
            sel_nodes_.clear(); sel_ways_.clear();
        }
        break;
    }
    case Tool::AddNode: {
        if (!edit_ds_) break;
        push_undo();
        double lat, lon; screen_to_geo(m, origin, size, lat, lon);
        osm::Node nn; nn.id = next_new_id_--; nn.lat = lat; nn.lon = lon;
        nn.version = 0; nn.visible = true;
        edit_ds_->nodes[nn.id] = nn;
        sel_nodes_ = {nn.id}; sel_ways_.clear();
        if (on_edit_) on_edit_();
        break;
    }
    case Tool::AddNodeToWay: {
        int64_t wid = sel_ways_.empty() ? 0 : *sel_ways_.begin();
        if (!wid) wid = pick_way(m, origin, size);
        if (wid) { push_undo(); insert_node_into_way(wid, m, origin, size); }
        break;
    }
    case Tool::DrawWay: {
        if (!edit_ds_) break;
        if (draw_nodes_.empty()) push_undo();
        int64_t nid = pick_node(m, origin, size);
        if (!nid) {
            double lat, lon; screen_to_geo(m, origin, size, lat, lon);
            osm::Node nn; nn.id = next_new_id_--; nn.lat = lat; nn.lon = lon;
            nn.version = 0; nn.visible = true;
            edit_ds_->nodes[nn.id] = nn;
            nid = nn.id;
        }
        draw_nodes_.push_back(nid);
        if (on_edit_) on_edit_();
        break;
    }
    }
}

// Draw one TileCache layer into dl.  fill_missing: draw dark placeholder tiles.
static void draw_tile_cache(map::TileCache& cache,
                            ImDrawList* dl, ImVec2 origin, ImVec2 size,
                            double center_lat, double center_lon, int zoom,
                            bool fill_missing) {
    cache.upload_pending();

    // Overlay layers may request a blend alpha (<1) so e.g. hillshade lets the
    // base imagery show through.  Opaque (1.0) tiles use plain white tint.
    float op = std::clamp(cache.source().opacity, 0.0f, 1.0f);
    ImU32 tint = IM_COL32(255, 255, 255, (int)(op * 255.0f + 0.5f));

    // Overzoom: tile services only publish tiles up to their max_zoom. Past that
    // we fetch tiles at max_zoom and draw them upscaled, so the imagery keeps
    // showing (just softer) instead of vanishing into "missing tile" gaps. The
    // larger on-screen tile size keeps the world scale identical to `zoom`, so
    // the data overlay (which uses zoom_) stays perfectly aligned.
    int   ez  = std::min(zoom, cache.source().max_zoom);
    if (ez < 2) ez = 2;
    float tpx = TILE_PX * (float)(1 << (zoom - ez)); // on-screen size of one tile

    int n = 1 << ez;
    double cx_tile = (center_lon + 180.0) / 360.0 * n;
    double lat_r   = center_lat * PI / 180.0;
    double cy_tile = (1.0 - std::log(std::tan(lat_r) + 1.0/std::cos(lat_r)) / PI) / 2.0 * n;

    int cx = (int)std::floor(cx_tile);
    int cy = (int)std::floor(cy_tile);
    int half_x = (int)(size.x / 2 / tpx) + 2;
    int half_y = (int)(size.y / 2 / tpx) + 2;

    for (int dy = -half_y; dy <= half_y; ++dy) {
        for (int dx = -half_x; dx <= half_x; ++dx) {
            int tx = ((cx + dx) % n + n) % n;
            int ty = cy + dy;
            if (ty < 0 || ty >= n) continue;

            map::TileCoord coord{ez, tx, ty};
            auto state = cache.request(coord);
            auto tex   = cache.texture(coord);

            float px = origin.x + size.x * 0.5f
                       + (float)((cx + dx - cx_tile) * tpx);
            float py = origin.y + size.y * 0.5f
                       + (float)((cy + dy - cy_tile) * tpx);

            if (tex) {
                dl->AddImage((ImTextureID)(uintptr_t)tex,
                             ImVec2(px, py),
                             ImVec2(px + tpx, py + tpx),
                             ImVec2(0, 0), ImVec2(1, 1), tint);
            } else if (fill_missing) {
                dl->AddRectFilled(ImVec2(px, py),
                                  ImVec2(px + tpx, py + tpx),
                                  IM_COL32(40, 40, 40, 255));
                if (state == map::TileState::Loading)
                    dl->AddText(ImVec2(px + 4, py + 4), IM_COL32(120,120,120,255), "...");
            }
        }
    }
}

void MapView::draw_tiles(ImDrawList* dl, ImVec2 origin, ImVec2 size) {
    draw_tile_cache(*tiles_, dl, origin, size,
                   center_lat_, center_lon_, zoom_, /*fill_missing=*/true);
    // Overlay layer (transparent PNG tiles drawn on top).
    if (overlay_tiles_)
        draw_tile_cache(*overlay_tiles_, dl, origin, size,
                        center_lat_, center_lon_, zoom_, /*fill_missing=*/false);
}

static ImU32 color_for_diff(osm::DiffState s) {
    switch (s) {
    case osm::DiffState::Added:    return IM_COL32(60, 220, 60, 200);
    case osm::DiffState::Modified: return IM_COL32(255, 200, 0, 200);
    case osm::DiffState::Deleted:  return IM_COL32(230, 60, 60, 200);
    default:                       return IM_COL32(180,180,180,100);
    }
}

void MapView::draw_diff_overlay(ImDrawList* dl, ImVec2 origin, ImVec2 size) {
    if (!changeset_) return;

    for (const auto& d : changeset_->nodes) {
        const osm::Node* n = d.after ? &*d.after : (d.before ? &*d.before : nullptr);
        if (!n) continue;
        ImVec2 p = geo_to_screen(n->lat, n->lon, origin, size);
        bool hi = (n->id == highlight_id_);
        float r = hi ? 8.f : 5.f;
        dl->AddCircleFilled(p, r, color_for_diff(d.state));
        if (hi) dl->AddCircle(p, r + 2, IM_COL32(255,255,255,255), 0, 2.f);
    }

    if (!dataset_) return;

    // Ways: draw polyline with diff color
    for (const auto& d : changeset_->ways) {
        const osm::Way* w = d.after ? &*d.after : (d.before ? &*d.before : nullptr);
        if (!w || w->nodes.empty()) continue;

        std::vector<ImVec2> pts;
        for (auto& nd : w->nodes) {
            auto it = dataset_->nodes.find(nd.ref);
            // For added ways nodes might be in the changeset nodes list
            if (it == dataset_->nodes.end()) continue;
            pts.push_back(geo_to_screen(it->second.lat, it->second.lon, origin, size));
        }
        if (pts.size() >= 2)
            dl->AddPolyline(pts.data(), (int)pts.size(),
                            color_for_diff(d.state), ImDrawFlags_None, 2.f);
    }
}

// Returns a tag-based fill color for an OSM area way.
// Known key=value pairs map to semantic colors; unknowns get a stable hash color.
static ImU32 area_color_for_tags(const osm::TagMap& tags, float opacity = 1.0f) {
    struct KV { const char* kv; uint8_t r, g, b, a; };
    static constexpr KV table[] = {
        // water
        {"natural=water",            60,140,220,180},
        {"natural=lake",             60,140,220,180},
        {"natural=pond",             60,140,220,180},
        {"natural=reservoir",        60,140,220,180},
        {"leisure=swimming_pool",    80,180,220,180},
        // dense vegetation
        {"landuse=forest",           40,120, 40,160},
        {"landuse=wood",             40,120, 40,160},
        {"natural=wood",             40,110, 50,160},
        {"natural=forest",           40,110, 50,160},
        // open vegetation
        {"landuse=grass",           120,200, 80,160},
        {"landuse=meadow",          120,200, 80,160},
        {"landuse=orchard",         140,210,120,130},
        {"landuse=vineyard",        150,200,130,130},
        {"landuse=allotments",      170,210,130,130},
        {"natural=grassland",       140,200, 90,150},
        {"natural=scrub",           140,190,120,150},
        {"natural=heath",           180,170,130,150},
        {"leisure=park",            100,200,120,150},
        {"leisure=garden",          100,210,110,150},
        {"leisure=nature_reserve",   80,180,100,150},
        {"leisure=pitch",           130,210,160,150},
        {"leisure=golf_course",     130,210,150,150},
        // farmland
        {"landuse=farmland",        195,210,150,160},
        {"landuse=farmyard",        195,210,150,160},
        // sand / rock
        {"natural=sand",            240,220,160,150},
        {"natural=beach",           240,220,160,150},
        {"natural=bare_rock",       180,180,170,150},
        // wetland
        {"natural=wetland",          80,170,150,150},
        // urban land use — kept translucent so buildings/roads stay visible
        {"landuse=residential",     224,223,223, 70},
        {"landuse=commercial",      235,205,160, 80},
        {"landuse=retail",          235,190,170, 80},
        {"landuse=industrial",      200,190,205, 80},
        {"landuse=cemetery",        170,200,165,110},
        // amenities
        {"amenity=parking",         205,205,215,120},
        {"amenity=school",          235,230,150,110},
        {"amenity=university",      235,230,150,110},
        {"amenity=hospital",        250,200,200,120},
        {"amenity=cemetery",        170,200,165,110},
        // leisure
        {"leisure=playground",      225,200,120,140},
        {"leisure=sports_centre",   150,180,225,140},
        // wildcard: any building value (drawn on top, fairly opaque)
        {"building",                206,178,148,205},
    };
    static constexpr const char* prio[] = {
        "building","landuse","natural","leisure","amenity",nullptr
    };

    const char* key = nullptr;
    std::string_view val;
    for (int i = 0; prio[i]; ++i) {
        auto it = tags.find(prio[i]);
        if (it != tags.end()) { key = prio[i]; val = it->second; break; }
    }
    if (!key && !tags.empty()) { key = tags.begin()->first.c_str(); val = tags.begin()->second; }
    if (!key) return IM_COL32(160,160,160,(uint8_t)(100*opacity));

    std::string kv = std::string(key) + "=" + std::string(val);

    // Exact key=value match
    for (const auto& e : table)
        if (kv == e.kv) return IM_COL32(e.r,e.g,e.b,(uint8_t)(e.a*opacity));
    // Wildcard key match (bare "key" entry like "building")
    for (const auto& e : table)
        if (std::string_view(e.kv) == key) return IM_COL32(e.r,e.g,e.b,(uint8_t)(e.a*opacity));

    // FNV-1a hash → muted HSV color for unknown tag combinations
    uint32_t h = 2166136261u;
    for (char c : kv) { h ^= (uint8_t)c; h *= 16777619u; }
    float hue    = (h & 0xFFFF) / 65535.0f * 360.0f;
    float sat    = 0.35f + ((h >> 16) & 0x1F) / 31.0f * 0.25f;
    float bright = 0.55f + ((h >> 21) & 0x0F) / 15.0f * 0.25f;
    float chroma = bright * sat;
    float x = chroma * (1.0f - std::abs(std::fmod(hue / 60.0f, 2.0f) - 1.0f));
    float m = bright - chroma;
    float r = m, g = m, b = m;
    switch ((int)(hue / 60.0f) % 6) {
    case 0: r+=chroma; g+=x;       break;
    case 1: r+=x;      g+=chroma;  break;
    case 2: g+=chroma; b+=x;       break;
    case 3: g+=x;      b+=chroma;  break;
    case 4: r+=x;      b+=chroma;  break;
    default:r+=chroma; b+=x;       break;
    }
    return IM_COL32((uint8_t)(r*255),(uint8_t)(g*255),(uint8_t)(b*255),(uint8_t)(140*opacity));
}

// FNV-1a hash → a distinct but muted color from an arbitrary 64-bit seed.
static ImU32 hash_color(uint64_t seed, uint8_t alpha) {
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < 8; ++i) { h ^= (seed >> (i * 8)) & 0xFF; h *= 1099511628211ull; }
    float hue = (h & 0xFFFF) / 65535.0f * 360.0f;
    float sat = 0.45f + ((h >> 16) & 0x1F) / 31.0f * 0.30f;
    float val = 0.65f + ((h >> 21) & 0x0F) / 15.0f * 0.25f;
    float c = val * sat, x = c * (1 - std::abs(std::fmod(hue / 60.f, 2.f) - 1)), m = val - c;
    float r = m, g = m, b = m;
    switch ((int)(hue / 60.f) % 6) {
    case 0: r += c; g += x; break;  case 1: r += x; g += c; break;
    case 2: g += c; b += x; break;  case 3: g += x; b += c; break;
    case 4: r += x; b += c; break;  default: r += c; b += x; break;
    }
    return IM_COL32((int)(r * 255), (int)(g * 255), (int)(b * 255), alpha);
}

static bool is_building_tags(const osm::TagMap& t) { return t.contains("building"); }

// ── Line (road / rail / waterway) styling ──
struct LineStyle { ImU32 core; ImU32 casing; float w; float casing_w; bool dashed; bool known; };
static LineStyle line_style_for_tags(const osm::TagMap& tags) {
    auto get = [&](const char* k) -> std::string_view {
        auto it = tags.find(k);
        return it == tags.end() ? std::string_view{} : std::string_view(it->second);
    };
    LineStyle s{ IM_COL32(190, 190, 200, 230), 0, 2.0f, 0.f, false, true };

    std::string_view hw = get("highway");
    if (!hw.empty()) {
        auto set = [&](int r, int g, int b, float w, bool casing) {
            s.core = IM_COL32(r, g, b, 255); s.w = w;
            if (casing) { s.casing = IM_COL32(60, 60, 70, 255); s.casing_w = w + 2.f; }
        };
        if      (hw == "motorway" || hw == "motorway_link")   set(235, 130, 150, 6.f, true);
        else if (hw == "trunk"    || hw == "trunk_link")      set(245, 160, 120, 5.5f, true);
        else if (hw == "primary"  || hw == "primary_link")    set(250, 205, 120, 5.f, true);
        else if (hw == "secondary"|| hw == "secondary_link")  set(245, 245, 140, 4.f, true);
        else if (hw == "tertiary" || hw == "tertiary_link")   set(255, 255, 255, 3.5f, true);
        else if (hw == "residential" || hw == "living_street" || hw == "unclassified")
                                                              set(255, 255, 255, 3.f, true);
        else if (hw == "service")                             set(255, 255, 255, 2.f, true);
        else if (hw == "pedestrian")                          set(220, 210, 225, 3.f, true);
        else if (hw == "footway" || hw == "path" || hw == "steps")
            { s.core = IM_COL32(240, 130, 110, 255); s.w = 1.6f; s.dashed = true; }
        else if (hw == "cycleway")
            { s.core = IM_COL32(70, 110, 230, 255);  s.w = 1.6f; s.dashed = true; }
        else if (hw == "track")
            { s.core = IM_COL32(170, 120, 70, 255);  s.w = 1.8f; s.dashed = true; }
        else                                                  set(255, 255, 255, 2.5f, true);
        return s;
    }
    std::string_view ww = get("waterway");
    if (!ww.empty()) {
        s.core = IM_COL32(120, 170, 225, 235);
        s.w = (ww == "river" || ww == "canal") ? 3.f : 1.6f; return s;
    }
    std::string_view rw = get("railway");
    if (!rw.empty()) {
        s.core = IM_COL32(110, 110, 120, 235); s.w = 2.f; s.dashed = true;
        s.casing = IM_COL32(70, 70, 80, 255);  s.casing_w = 3.f; return s;
    }
    if (tags.contains("power"))   { s.core = IM_COL32(150, 150, 160, 170); s.w = 1.2f; return s; }
    if (tags.contains("barrier")) { s.core = IM_COL32(140, 120, 120, 200); s.w = 1.4f; s.dashed = true; return s; }
    s.known = false; // unknown line — caller may apply random / default color
    return s;
}

// ── POI marker styling for a tagged node ──
struct PoiStyle { bool show; ImU32 color; char glyph; bool circle_only; };
static PoiStyle poi_style_for_tags(const osm::TagMap& tags) {
    auto v = [&](const char* k) -> std::string_view {
        auto it = tags.find(k);
        return it == tags.end() ? std::string_view{} : std::string_view(it->second);
    };
    std::string_view am = v("amenity"), shop = v("shop"), tour = v("tourism"),
                     leis = v("leisure"), nat = v("natural"), hist = v("historic"),
                     pt = v("public_transport"), rail = v("railway"),
                     off = v("office"), man = v("man_made"), em = v("emergency");
    PoiStyle p{ true, IM_COL32(120, 120, 130, 255), 'i', false };
    auto set = [&](int r, int g, int b, char gl) { p.color = IM_COL32(r, g, b, 255); p.glyph = gl; };

    if (nat == "tree")  { p.color = IM_COL32(70, 150, 70, 255); p.circle_only = true; return p; }
    if (nat == "peak")  { set(150, 120, 90, '^'); return p; }
    if (am == "restaurant" || am == "cafe" || am == "fast_food" || am == "bar" ||
        am == "pub" || am == "food_court")                 { set(230, 130, 60, 'F'); return p; }
    if (am == "parking")                                   { set(70, 110, 200, 'P'); return p; }
    if (am == "fuel")                                      { set(70, 110, 200, 'G'); return p; }
    if (am == "bank" || am == "atm")                       { set(80, 150, 90, '$'); return p; }
    if (am == "pharmacy" || am == "hospital" || am == "clinic" ||
        am == "doctors" || em == "yes")                    { set(220, 70, 70, '+'); return p; }
    if (am == "school" || am == "university" || am == "college" ||
        am == "kindergarten")                              { set(220, 180, 60, 'E'); return p; }
    if (am == "place_of_worship")                          { set(150, 130, 180, 'W'); return p; }
    if (am == "toilets")                                   { set(120, 140, 160, 'T'); return p; }
    if (!shop.empty())                                     { set(180, 90, 180, 'S'); return p; }
    if (tour == "hotel" || tour == "hostel" || tour == "guest_house" ||
        tour == "motel")                                   { set(90, 120, 200, 'H'); return p; }
    if (tour == "museum" || tour == "gallery" || tour == "attraction" ||
        !hist.empty())                                     { set(170, 120, 90, 'M'); return p; }
    if (am == "bus_station" || pt == "stop_position" || pt == "platform" ||
        v("highway") == "bus_stop" || rail == "tram_stop") { set(60, 140, 160, 'B'); return p; }
    if (rail == "station" || rail == "halt")               { set(80, 90, 110, 'R'); return p; }
    if (!leis.empty())                                     { set(90, 170, 110, 'L'); return p; }
    if (!off.empty())                                      { set(120, 120, 170, 'O'); return p; }
    if (!man.empty())                                      { set(130, 130, 140, 'm'); return p; }
    if (!am.empty())                                       { set(120, 120, 170, 'i'); return p; }
    p.show = false; // not a recognized POI (e.g. a plain vertex)
    return p;
}

// Draw a polyline as dashes (ImGui has no native dashed-line primitive).
static void draw_dashed(ImDrawList* dl, const std::vector<ImVec2>& pts, ImU32 col,
                        float w, float dash = 8.f, float gap = 6.f) {
    for (size_t i = 1; i < pts.size(); ++i) {
        ImVec2 a = pts[i - 1], b = pts[i];
        float dx = b.x - a.x, dy = b.y - a.y, len = std::sqrt(dx * dx + dy * dy);
        if (len < 0.01f) continue;
        float ux = dx / len, uy = dy / len, pos = 0;
        while (pos < len) {
            float e = std::min(pos + dash, len);
            dl->AddLine({a.x + ux * pos, a.y + uy * pos},
                        {a.x + ux * e,   a.y + uy * e}, col, w);
            pos = e + gap;
        }
    }
}

// Draw a POI marker (category-colored disc + glyph) with an optional name label.
static void draw_poi_marker(ImDrawList* dl, ImVec2 p, const PoiStyle& s, const char* label) {
    const float r = 7.f;
    if (s.circle_only) {
        dl->AddCircleFilled(p, 5.f, s.color);
        dl->AddCircle(p, 5.f, IM_COL32(255, 255, 255, 180), 0, 1.2f);
    } else {
        dl->AddCircleFilled(p, r, s.color);
        dl->AddCircle(p, r, IM_COL32(255, 255, 255, 220), 0, 1.5f);
        char g[2] = { s.glyph, 0 };
        ImVec2 ts = ImGui::CalcTextSize(g);
        dl->AddText({p.x - ts.x * 0.5f, p.y - ts.y * 0.5f}, IM_COL32(255, 255, 255, 255), g);
    }
    if (label && label[0]) {
        ImVec2 tp{ p.x + r + 2.f, p.y - ImGui::GetFontSize() * 0.5f };
        dl->AddText({tp.x + 1, tp.y + 1}, IM_COL32(0, 0, 0, 170), label); // shadow
        dl->AddText(tp, IM_COL32(245, 245, 245, 255), label);
    }
}

void MapView::draw_features(ImDrawList* dl, ImVec2 origin, ImVec2 size,
                            const osm::Dataset* ds, float opacity, bool use_index) {
    if (!ds) return;

    double vp_half_lon = size.x * 0.5 / TILE_PX * (360.0 / (1 << zoom_));
    double vp_half_lat = size.y * 0.5 / TILE_PX * (360.0 / (1 << zoom_));
    double vmin_lat = center_lat_ - vp_half_lat, vmax_lat = center_lat_ + vp_half_lat;
    double vmin_lon = center_lon_ - vp_half_lon, vmax_lon = center_lon_ + vp_half_lon;

    auto is_area = [](const osm::Way& w) {
        if (w.nodes.size() < 4 || w.nodes.front().ref != w.nodes.back().ref) return false;
        auto has = [&](const char* k){ return w.tags.contains(k); };
        if (has("building") || has("landuse") || has("leisure") ||
            has("natural")  || has("amenity")) return true;
        auto a = w.tags.find("area");
        return a != w.tags.end() && a->second == "yes";
    };
    auto pts_for = [&](const osm::Way& w, bool drop_dup) {
        size_t nc = w.nodes.size();
        if (drop_dup && nc > 1 && w.nodes.front().ref == w.nodes.back().ref) nc--;
        std::vector<ImVec2> pts; pts.reserve(nc);
        for (size_t i = 0; i < nc; ++i) {
            auto it = ds->nodes.find(w.nodes[i].ref);
            if (it != ds->nodes.end())
                pts.push_back(geo_to_screen(it->second.lat, it->second.lon, origin, size));
        }
        return pts;
    };
    auto darken = [](ImU32 c, float f, uint8_t a) -> ImU32 {
        int r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
        return IM_COL32((int)(r * f), (int)(g * f), (int)(b * f), a);
    };

    struct AreaDraw { std::vector<ImVec2> pts; ImU32 fill, outline; float ow; };
    struct LineDraw { std::vector<ImVec2> pts; LineStyle st; };
    std::vector<AreaDraw> landcover, buildings;
    std::vector<LineDraw> lines;

    auto handle_way = [&](int64_t wid, const osm::Way& way) {
        if (way.nodes.size() < 2) return;
        if (is_area(way)) {
            auto pts = pts_for(way, true);
            if (pts.size() < 3) return;
            ImU32 fill = style_.random_colors
                ? hash_color((uint64_t)wid, (uint8_t)(150 * opacity * style_.area_opacity))
                : area_color_for_tags(way.tags, opacity * style_.area_opacity);
            ImU32 outline = darken(fill, 0.55f, (uint8_t)(200 * opacity));
            AreaDraw ad{ std::move(pts), fill, outline, style_.area_width };
            if (is_building_tags(way.tags)) buildings.push_back(std::move(ad));
            else                            landcover.push_back(std::move(ad));
        } else {
            auto pts = pts_for(way, false);
            if (pts.size() < 2) return;
            LineStyle st = line_style_for_tags(way.tags);
            if (style_.random_colors)
                st.core = hash_color((uint64_t)wid, 255);
            else if (!st.known)
                st.core = ImGui::GetColorU32(style_.way_color);
            lines.push_back({ std::move(pts), st });
        }
    };

    if (use_index && spatial_index_) {
        for (auto wid : spatial_index_->query_ways(vmin_lat, vmin_lon, vmax_lat, vmax_lon)) {
            auto it = ds->ways.find(wid);
            if (it != ds->ways.end()) handle_way(wid, it->second);
        }
    } else {
        for (const auto& [id, way] : ds->ways) handle_way(id, way);
    }

    // 1) Land-cover fills + outlines (bottom).
    for (auto& a : landcover) {
        fill_polygon_earcut(dl, a.pts, a.fill);
        dl->AddPolyline(a.pts.data(), (int)a.pts.size(), a.outline, ImDrawFlags_Closed, a.ow);
    }
    // 2) Road / rail casings (drawn under the cores so junctions merge cleanly).
    if (style_.road_casing)
        for (auto& l : lines)
            if (l.st.casing_w > 0)
                dl->AddPolyline(l.pts.data(), (int)l.pts.size(),
                                l.st.casing, ImDrawFlags_None, l.st.casing_w);
    // 3) Line cores.
    for (auto& l : lines) {
        if (l.st.dashed) draw_dashed(dl, l.pts, l.st.core, l.st.w);
        else dl->AddPolyline(l.pts.data(), (int)l.pts.size(),
                             l.st.core, ImDrawFlags_None, l.st.w);
    }
    // 4) Buildings on top of land cover and roads.
    for (auto& b : buildings) {
        fill_polygon_earcut(dl, b.pts, b.fill);
        dl->AddPolyline(b.pts.data(), (int)b.pts.size(), b.outline, ImDrawFlags_Closed, b.ow);
    }

    // 5) POIs / node vertices (top).
    bool labels = style_.show_labels && zoom_ >= style_.label_min_zoom;
    bool show_vertices = zoom_ >= style_.node_min_zoom;
    ImVec4 nc = style_.node_color;  nc.w *= opacity;
    ImVec4 tc = style_.node_tagged; tc.w *= opacity;
    const ImU32 ncol = ImGui::GetColorU32(nc);
    const ImU32 tcol = ImGui::GetColorU32(tc);
    for (const auto& [id, n] : ds->nodes) {
        if (n.lat < vmin_lat || n.lat > vmax_lat ||
            n.lon < vmin_lon || n.lon > vmax_lon) continue;
        ImVec2 p = geo_to_screen(n.lat, n.lon, origin, size);
        PoiStyle ps = n.tags.empty() ? PoiStyle{false, 0, 0, false}
                                     : poi_style_for_tags(n.tags);
        if (style_.show_poi_icons && ps.show) {
            std::string namebuf;
            const char* lbl = nullptr;
            if (labels) {
                auto it = n.tags.find("name");
                if (it != n.tags.end()) { namebuf = it->second; lbl = namebuf.c_str(); }
            }
            draw_poi_marker(dl, p, ps, lbl);
        } else if (show_vertices) {
            dl->AddCircleFilled(p, style_.node_radius, n.tags.empty() ? ncol : tcol);
        }
    }
}

void MapView::draw_osm_layer_ds(ImDrawList* dl, ImVec2 origin, ImVec2 size,
                                const osm::Dataset* ds, float opacity) {
    draw_features(dl, origin, size, ds, opacity, /*use_index=*/false);
}

void MapView::draw_osm_layer(ImDrawList* dl, ImVec2 origin, ImVec2 size) {
    if (!dataset_) return;

    draw_features(dl, origin, size, dataset_, 1.0f, /*use_index=*/true);

    // ── Selection highlight (multi-select aware) ──
    for (int64_t wid : sel_ways_) {
        auto it = dataset_->ways.find(wid);
        if (it == dataset_->ways.end()) continue;
        std::vector<ImVec2> pts;
        for (auto& nd : it->second.nodes) {
            auto nit = dataset_->nodes.find(nd.ref);
            if (nit != dataset_->nodes.end())
                pts.push_back(geo_to_screen(nit->second.lat, nit->second.lon, origin, size));
        }
        if (pts.size() >= 2)
            dl->AddPolyline(pts.data(), (int)pts.size(),
                            IM_COL32(255, 80, 80, 220), ImDrawFlags_None, 3.f);
        for (auto& p : pts) dl->AddCircleFilled(p, 4.f, IM_COL32(255, 120, 40, 220));
    }
    for (int64_t nid : sel_nodes_) {
        auto it = dataset_->nodes.find(nid);
        if (it == dataset_->nodes.end()) continue;
        ImVec2 p = geo_to_screen(it->second.lat, it->second.lon, origin, size);
        dl->AddCircleFilled(p, 6.f, IM_COL32(255, 60, 60, 255));
        dl->AddCircle(p, 8.f, IM_COL32(255, 255, 255, 200), 0, 2.f);
    }
}

void MapView::handle_input(ImVec2 origin, ImVec2 size) {
    ImGuiIO& io = ImGui::GetIO();

    // These refer to the "map_canvas" InvisibleButton submitted just before this
    // call. IsItemActive() stays true while the user holds & drags it — unlike
    // IsWindowHovered(), which goes false once an item becomes active.
    bool hovered = ImGui::IsItemHovered();
    bool active  = ImGui::IsItemActive();
    ImVec2 m = io.MousePos;

    // Zoom with scroll while hovering the map.
    if (hovered && io.MouseWheel != 0.f) {
        zoom_ = std::clamp(zoom_ + (io.MouseWheel > 0 ? 1 : -1), 2, 21);
        view_dirty_ = true;
    }

    // Delete selected object with the Delete key while the map is focused.
    if (hovered && ImGui::IsKeyPressed(ImGuiKey_Delete) && has_selection())
        delete_selection();

    // While drawing a way: Enter finishes, Escape cancels.
    if (tool_ == Tool::DrawWay) {
        if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))
            finish_way();
        else if (ImGui::IsKeyPressed(ImGuiKey_Escape))
            cancel_draw();
    }

    // ── Right-click pan (JOSM style) ─────────────────────────────────────────
    // Right button is tracked independently of the InvisibleButton active state.
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        panning_ = true;
    if (panning_) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            ImVec2 delta = io.MouseDelta;
            if (delta.x != 0.f || delta.y != 0.f) {
                int n_tiles = 1 << zoom_;
                double deg_per_px_lon = 360.0 / (n_tiles * TILE_PX);
                double lat_r = center_lat_ * PI / 180.0;
                double deg_per_px_lat = 360.0 / (n_tiles * TILE_PX) * std::cos(lat_r);
                center_lon_ = wrap_lon(center_lon_ - delta.x * deg_per_px_lon);
                center_lat_ = std::clamp(center_lat_ + delta.y * deg_per_px_lat, -85.0, 85.0);
                view_dirty_ = true;
            }
        } else {
            panning_ = false;
        }
    }

    // ── Left-click: select / draw / node-drag / box-select ───────────────────
    // Press: decide whether we're grabbing a node (to drag) or starting box-select.
    if (ImGui::IsItemActivated()) {
        press_pos_ = m;
        dragging_node_ = false;
        box_selecting_ = false;
        if (tool_ == Tool::Select) {
            int64_t n = pick_node(m, origin, size);
            if (n && (sel_nodes_.contains(n) || sel_nodes_.size() == 1)) {
                // Start a node drag only if clicking on already-selected node
                // or single newly-selected node.
                if (!sel_nodes_.contains(n)) {
                    sel_nodes_ = {n}; sel_ways_.clear();
                }
                dragging_node_ = true;
                push_undo();
            }
        }
    }

    if (active) {
        float dx = m.x - press_pos_.x, dy = m.y - press_pos_.y;
        bool moved = (dx*dx + dy*dy) > 25.f;

        if (dragging_node_ && edit_ds_ && !sel_nodes_.empty()) {
            double lat, lon; screen_to_geo(m, origin, size, lat, lon);
            auto it = edit_ds_->nodes.find(*sel_nodes_.begin());
            if (it != edit_ds_->nodes.end()) {
                it->second.lat = std::clamp(lat, -85.0, 85.0);
                it->second.lon = lon;
            }
        } else if (tool_ == Tool::Select && moved && !dragging_node_) {
            // Rubber-band box selection
            box_selecting_ = true;
            box_start_ = press_pos_;
            box_end_   = m;
        }
    }

    // Release
    if (ImGui::IsItemDeactivated()) {
        float dx = m.x - press_pos_.x, dy = m.y - press_pos_.y;
        bool was_click = (dx*dx + dy*dy) < 25.f;

        if (box_selecting_) {
            // Finalise box selection: select everything inside the screen rect.
            box_selecting_ = false;
            if (dataset_) {
                float x0 = std::min(box_start_.x, box_end_.x);
                float y0 = std::min(box_start_.y, box_end_.y);
                float x1 = std::max(box_start_.x, box_end_.x);
                float y1 = std::max(box_start_.y, box_end_.y);

                bool shift = io.KeyShift;
                if (!shift) { sel_nodes_.clear(); sel_ways_.clear(); }

                for (const auto& [id, n] : dataset_->nodes) {
                    ImVec2 p = geo_to_screen(n.lat, n.lon, origin, size);
                    if (p.x >= x0 && p.x <= x1 && p.y >= y0 && p.y <= y1)
                        sel_nodes_.insert(id);
                }
                for (const auto& [id, w] : dataset_->ways) {
                    for (const auto& wn : w.nodes) {
                        auto ni = dataset_->nodes.find(wn.ref);
                        if (ni == dataset_->nodes.end()) continue;
                        ImVec2 p = geo_to_screen(ni->second.lat, ni->second.lon, origin, size);
                        if (p.x >= x0 && p.x <= x1 && p.y >= y0 && p.y <= y1) {
                            sel_ways_.insert(id); break;
                        }
                    }
                }
            }
        } else if (dragging_node_) {
            if (was_click) {
                if (!undo_.empty()) undo_.pop_back(); // discard no-op move
            } else if (on_edit_) {
                on_edit_();
            }
        } else if (was_click) {
            handle_click(m, origin, size);
        }
        dragging_node_ = false;
    }
}

void MapView::draw() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("Map", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();

    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 size   = ImGui::GetContentRegionAvail();
    last_size_ = size; // remember for view-bbox computation

    // Clipping
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(origin, {origin.x + size.x, origin.y + size.y}, true);

    if (layer_imagery_) draw_tiles(dl, origin, size);
    // Extra (non-active) layers rendered at half opacity so the active layer stands out.
    if (layer_osm_) {
        for (const osm::Dataset* extra : extra_layers_) {
            if (!extra || extra == dataset_) continue;
            draw_osm_layer_ds(dl, origin, size, extra, 0.45f);
        }
        draw_osm_layer(dl, origin, size);
    }
    if (layer_diff_)    draw_diff_overlay(dl, origin, size);
    draw_ai_overlay(dl, origin, size);

    // Rubber-band box selection overlay
    if (box_selecting_) {
        float x0 = std::min(box_start_.x, box_end_.x);
        float y0 = std::min(box_start_.y, box_end_.y);
        float x1 = std::max(box_start_.x, box_end_.x);
        float y1 = std::max(box_start_.y, box_end_.y);
        dl->AddRectFilled({x0, y0}, {x1, y1}, IM_COL32(100, 160, 255, 40));
        dl->AddRect({x0, y0}, {x1, y1}, IM_COL32(100, 180, 255, 200), 0.f, 0, 1.5f);
    }

    // Show grab cursor while panning.
    if (panning_) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);

    // In-progress way preview (Draw way tool), with a rubber-band to the cursor.
    if (!draw_nodes_.empty() && edit_ds_) {
        std::vector<ImVec2> pts;
        for (int64_t nid : draw_nodes_) {
            auto it = edit_ds_->nodes.find(nid);
            if (it != edit_ds_->nodes.end())
                pts.push_back(geo_to_screen(it->second.lat, it->second.lon, origin, size));
        }
        if (!pts.empty()) {
            std::vector<ImVec2> rb = pts;
            rb.push_back(ImGui::GetIO().MousePos);
            dl->AddPolyline(rb.data(), (int)rb.size(), IM_COL32(80, 255, 120, 220),
                            ImDrawFlags_None, 2.f);
            for (auto& p : pts) dl->AddCircleFilled(p, 4.f, IM_COL32(80, 255, 120, 255));
        }
    }

    dl->PopClipRect();

    // Invisible button to capture pan/zoom over the full area. AllowOverlap so
    // the control buttons drawn on top of it (zoom, source, download) still get
    // their clicks.
    // Left button only: the item's active state drives node-drag and rubber-band
    // box selection, which must NOT engage during a right-button pan. Right-pan is
    // handled independently in handle_input() via IsMouseDown(Right).
    ImGui::InvisibleButton("map_canvas", size,
                           ImGuiButtonFlags_MouseButtonLeft |
                           ImGuiButtonFlags_AllowOverlap);
    handle_input(origin, size);

    // Edit-tool toolbar (top)
    ImGui::SetCursorScreenPos({origin.x + 40, origin.y + 8});
    auto tool_btn = [&](const char* label, Tool t) {
        bool act = (tool_ == t);
        if (act) ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(60, 120, 200, 255));
        if (ImGui::Button(label)) tool_ = t;
        if (act) ImGui::PopStyleColor();
        ImGui::SameLine();
    };
    tool_btn("Select",     Tool::Select);
    tool_btn("Add node",   Tool::AddNode);
    tool_btn("Add to way", Tool::AddNodeToWay);
    tool_btn("Draw way",   Tool::DrawWay);
    if (ImGui::Button("Delete")) delete_selection();
    ImGui::SameLine();
    ImGui::BeginDisabled(!can_split());
    if (ImGui::Button("Split")) split_way();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Select a way + an intermediate node on it, then split");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!can_undo());
    if (ImGui::Button("Undo")) undo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!can_redo());
    if (ImGui::Button("Redo")) redo();
    ImGui::EndDisabled();
    if (tool_ == Tool::DrawWay && drawing()) {
        ImGui::SameLine();
        if (ImGui::Button("Finish (Enter)")) finish_way();
        ImGui::SameLine();
        if (ImGui::Button("Cancel (Esc)"))   cancel_draw();
    }

    // Zoom controls overlay
    ImGui::SetCursorScreenPos({origin.x + 8, origin.y + 8});
    if (ImGui::SmallButton("+")) zoom_ = std::min(zoom_ + 1, 21);
    ImGui::SetCursorScreenPos({origin.x + 8, origin.y + 30});
    if (ImGui::SmallButton("-")) zoom_ = std::max(zoom_ - 1, 2);

    // ── Tile layer selector ───────────────────────────────────────────────────
    {
        const auto& catalog   = map::tile_source_catalog();
        const int   ov_start  = map::tile_overlay_start();

        // Base layer
        ImGui::SetCursorScreenPos({origin.x + 8, origin.y + 56});
        const auto& cur = catalog[selected_zoom_source_];
        ImGui::SetNextItemWidth(160);
        if (ImGui::BeginCombo("##base", cur.name.c_str())) {
            map::TileCategory last_cat = map::TileCategory(-1);
            for (int i = 0; i < ov_start; ++i) {
                const auto& src = catalog[i];
                if (src.category != last_cat) {
                    const char* cat_label =
                        src.category == map::TileCategory::Satellite ? "Satellite / Aerial" :
                        src.category == map::TileCategory::Street    ? "Street / Road"      :
                        src.category == map::TileCategory::Topo      ? "Topo / Terrain"     : "";
                    ImGui::SeparatorText(cat_label);
                    last_cat = src.category;
                }
                bool sel = (i == selected_zoom_source_);
                if (ImGui::Selectable(src.name.c_str(), sel))
                    set_tile_source_index(i);
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        // API-key input shown below combo when current source requires it
        if (cur.requires_key) {
            // Seed once per source switch so we don't clobber in-progress typing.
            static std::string key_buf;
            static int key_for = -1;
            if (key_for != selected_zoom_source_) {
                key_buf = tile_keys_[selected_zoom_source_];
                key_for = selected_zoom_source_;
            }
            ImGui::SetCursorScreenPos({origin.x + 8, origin.y + 82});
            ImGui::SetNextItemWidth(160);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(60,30,30,200));
            if (ImGui::InputText("##apikey", &key_buf,
                                 ImGuiInputTextFlags_Password |
                                 ImGuiInputTextFlags_EnterReturnsTrue))
                set_tile_key(selected_zoom_source_, key_buf);
            if (ImGui::IsItemDeactivatedAfterEdit())
                set_tile_key(selected_zoom_source_, key_buf);
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Enter API key and press Enter");
        }

        // Overlay layer (transparent tiles on top)
        float overlay_y = cur.requires_key ? origin.y + 108 : origin.y + 82;
        ImGui::SetCursorScreenPos({origin.x + 8, overlay_y});
        const char* overlay_label = (selected_overlay_ > 0 && selected_overlay_ < (int)catalog.size())
            ? catalog[selected_overlay_].name.c_str() : "No overlay";
        ImGui::SetNextItemWidth(160);
        if (ImGui::BeginCombo("##ovl", overlay_label)) {
            if (ImGui::Selectable("No overlay", selected_overlay_ == 0))
                set_overlay_source_index(0);
            if (selected_overlay_ == 0) ImGui::SetItemDefaultFocus();
            ImGui::SeparatorText("Overlays");
            for (int i = ov_start; i < (int)catalog.size(); ++i) {
                bool sel = (i == selected_overlay_);
                if (ImGui::Selectable(catalog[i].name.c_str(), sel))
                    set_overlay_source_index(i);
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        // Key input for keyed overlay
        if (selected_overlay_ > 0 && selected_overlay_ < (int)catalog.size()
            && catalog[selected_overlay_].requires_key) {
            static std::string ovl_key_buf;
            static int ovl_key_for = -1;
            if (ovl_key_for != selected_overlay_) {
                ovl_key_buf = tile_keys_[selected_overlay_];
                ovl_key_for = selected_overlay_;
            }
            ImGui::SetCursorScreenPos({origin.x + 8, overlay_y + 26});
            ImGui::SetNextItemWidth(160);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(60,30,30,200));
            if (ImGui::InputText("##ovlkey", &ovl_key_buf,
                                 ImGuiInputTextFlags_Password |
                                 ImGuiInputTextFlags_EnterReturnsTrue))
                set_tile_key(selected_overlay_, ovl_key_buf);
            if (ImGui::IsItemDeactivatedAfterEdit())
                set_tile_key(selected_overlay_, ovl_key_buf);
            ImGui::PopStyleColor();
        }

        // Push download button below the layer controls dynamically
        float dl_y = overlay_y + 26;
        if (selected_overlay_ > 0 && selected_overlay_ < (int)catalog.size()
            && catalog[selected_overlay_].requires_key) dl_y += 26;
        ImGui::SetCursorScreenPos({origin.x + 8, dl_y});
    }

    // Download OSM data for the current viewport (cursor already positioned above)
    if (ImGui::Button("Download OSM data (this view)") && on_download_) {
        double mn_lat, mn_lon, mx_lat, mx_lon;
        get_view_bbox(mn_lat, mn_lon, mx_lat, mx_lon);
        on_download_(mn_lat, mn_lon, mx_lat, mx_lon);
    }
    if (!status_.empty()) {
        ImGui::SameLine();
        ImGui::TextUnformatted(status_.c_str());
    }

    // Coords readout
    ImGui::SetCursorScreenPos({origin.x + size.x - 200, origin.y + size.y - 20});
    ImGui::TextDisabled("z%d  %.5f, %.5f", zoom_, center_lat_, center_lon_);

    ImGui::End();
}

} // namespace ui
