#include "detection_panel.hpp"
#include "../ai/detection.hpp"
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <cmath>
#include <format>
#include <algorithm>

namespace ui {

// Source indices that shift depending on whether ONNX is compiled in.
#ifdef CPPOSMUI_HAVE_ONNXRUNTIME
static constexpr int SRC_CV = 4, SRC_CV_LIDAR = 5, N_SOURCES = 6;
#else
static constexpr int SRC_CV = 3, SRC_CV_LIDAR = 4, N_SOURCES = 5;
#endif

// ── Dashed polyline helper ───────────────────────────────────────────────────
static void draw_dashed_polyline(ImDrawList* dl, const ImVec2* pts, int count,
                                 ImU32 col, float width, float dash, float gap) {
    for (int i = 0; i + 1 < count; ++i) {
        float dx = pts[i+1].x - pts[i].x, dy = pts[i+1].y - pts[i].y;
        float len = std::sqrt(dx*dx + dy*dy);
        if (len < 0.5f) continue;
        float ux = dx / len, uy = dy / len;
        float t = 0;
        while (t < len) {
            float end = std::min(t + dash, len);
            ImVec2 a{pts[i].x + ux*t, pts[i].y + uy*t};
            ImVec2 b{pts[i].x + ux*end, pts[i].y + uy*end};
            dl->AddLine(a, b, col, width);
            t = end + gap;
        }
    }
}

// ── Color coding ─────────────────────────────────────────────────────────────
static ImU32 feature_color(const ai::DetectedFeature& f) {
    switch (f.status) {
    case ai::DetectedFeature::Status::Accepted: return IM_COL32( 80, 220,  80, 210);
    case ai::DetectedFeature::Status::Rejected: return IM_COL32(120, 120, 120,  80);
    default: break;
    }
    switch (f.type) {
    case ai::FeatureType::Building:  return IM_COL32(255, 160,  60, 200);
    case ai::FeatureType::Road:      return IM_COL32( 60, 200, 255, 200);
    case ai::FeatureType::Waterway:  return IM_COL32( 60, 120, 255, 200);
    case ai::FeatureType::Landuse:   return IM_COL32(140, 220, 100, 180);
    default:                         return IM_COL32(200, 200,  60, 200);
    }
}

// ── draw_map_overlay ──────────────────────────────────────────────────────────
void DetectionPanel::draw_map_overlay(
    ImDrawList* dl,
    const std::function<ImVec2(double lat, double lon)>& geo_to_screen) const
{
    if (!show_overlay_) return;
    std::scoped_lock lk(result_mu_);
    if (!result_) return;

    for (size_t fi = 0; fi < result_->features.size(); ++fi) {
        const auto& f = result_->features[fi];
        if (f.status == ai::DetectedFeature::Status::Rejected) continue;
        ImU32 col = feature_color(f);
        bool pending = (f.status == ai::DetectedFeature::Status::Pending);
        bool selected = ((int)fi == selected_idx_);

        std::vector<ImVec2> pts;
        pts.reserve(f.coords.size());
        for (const auto& [lat, lon] : f.coords)
            pts.push_back(geo_to_screen(lat, lon));

        float lw = selected ? 3.5f : 2.5f;

        if (pts.size() == 1) {
            dl->AddCircleFilled(pts[0], selected ? 8.f : 6.f, col);
            dl->AddCircle(pts[0], selected ? 10.f : 8.f,
                          IM_COL32(255,255,255,180), 0, 1.5f);
        } else if (pts.size() >= 2) {
            if (f.is_area) {
                ImU32 fill = (col & 0xFFFFFF) | (pending ? 0x28000000u : 0x50000000u);
                for (size_t i = 1; i + 1 < pts.size(); ++i)
                    dl->AddTriangleFilled(pts[0], pts[i], pts[i+1], fill);
                if (pending)
                    draw_dashed_polyline(dl, pts.data(), (int)pts.size(),
                                         col, lw, 8.f, 5.f);
                else
                    dl->AddPolyline(pts.data(), (int)pts.size(), col,
                                    ImDrawFlags_Closed, lw);
            } else {
                if (pending)
                    draw_dashed_polyline(dl, pts.data(), (int)pts.size(),
                                         col, lw, 8.f, 5.f);
                else
                    dl->AddPolyline(pts.data(), (int)pts.size(), col,
                                    ImDrawFlags_None, lw);
                dl->AddCircleFilled(pts.front(), 3.f, col);
                dl->AddCircleFilled(pts.back(),  3.f, col);
            }
        }
        if (selected && pts.size() >= 2) {
            ImU32 hi = IM_COL32(255, 255, 100, 140);
            dl->AddPolyline(pts.data(), (int)pts.size(), hi,
                            f.is_area ? ImDrawFlags_Closed : ImDrawFlags_None, lw + 2.f);
        }
    }
}

// ── run_detection ─────────────────────────────────────────────────────────────
void DetectionPanel::run_detection() {
    if (running_) return;
    running_ = true;
    status_  = "Running detection…";

    auto on_done = [this](ai::DetectionResult res) {
        auto sp = std::make_shared<ai::DetectionResult>(std::move(res));
        std::scoped_lock lk(result_mu_);
        result_   = std::move(sp);
        checked_.assign(result_->features.size(), true); // default: all checked
        running_  = false;
        status_   = result_->ok()
            ? std::format("{} feature(s) found  ({})",
                           result_->features.size(), result_->source_name)
            : std::format("Error: {}", result_->error);
    };

    switch (source_) {
    case 0:
        ai::detect_ms_buildings(min_lat_, min_lon_, max_lat_, max_lon_,
                                 std::move(on_done));
        break;
    case 1:
        ai::detect_mapwithai_roads(min_lat_, min_lon_, max_lat_, max_lon_,
                                    std::move(on_done));
        break;
    case 2:
        ai::detect_custom_rest(custom_url_, min_lat_, min_lon_, max_lat_, max_lon_,
                                std::move(on_done));
        break;
#ifdef CPPOSMUI_HAVE_ONNXRUNTIME
    case 3:
        if (model_path_.empty()) {
            running_ = false; status_ = "Set the ONNX model path first"; break;
        }
        if (imagery_url_.empty()) {
            running_ = false;
            status_ = "Select a slippy/WMTS imagery base layer first"; break;
        }
        ai::detect_via_onnx_bbox(model_path_, imagery_url_, imagery_zoom_,
                                 min_lat_, min_lon_, max_lat_, max_lon_,
                                 std::move(on_done));
        break;
#endif
    default:
        if (source_ == SRC_CV) {
            if (!mosaic_fn_) {
                running_ = false; status_ = "No imagery source available"; break;
            }
            auto m = mosaic_fn_(min_lat_, min_lon_, max_lat_, max_lon_, imagery_zoom_);
            if (m.rgb.empty()) {
                running_ = false; status_ = "No cached tiles in view — pan around first"; break;
            }
            ai::detect_cv_on_imagery(m.rgb, m.w, m.h,
                                     min_lat_, min_lon_, max_lat_, max_lon_,
                                     std::move(on_done));
        } else if (source_ == SRC_CV_LIDAR) {
            if (!mosaic_fn_ || !dsm_mosaic_fn_) {
                running_ = false;
                status_ = "Enable the Geoportal LiDAR NMPT overlay first"; break;
            }
            auto m = mosaic_fn_(min_lat_, min_lon_, max_lat_, max_lon_, imagery_zoom_);
            auto d = dsm_mosaic_fn_(min_lat_, min_lon_, max_lat_, max_lon_, imagery_zoom_);
            if (m.rgb.empty()) {
                running_ = false; status_ = "No cached imagery tiles in view"; break;
            }
            if (d.rgb.empty()) {
                running_ = false;
                status_ = "No cached LiDAR DSM tiles — enable and pan first"; break;
            }
            // Convert DSM mosaic to single-channel grayscale.
            std::vector<uint8_t> dsm_gray((size_t)d.w * d.h);
            for (size_t i = 0; i < dsm_gray.size(); ++i)
                dsm_gray[i] = (uint8_t)(0.299 * d.rgb[i*3]
                                      + 0.587 * d.rgb[i*3+1]
                                      + 0.114 * d.rgb[i*3+2]);
            ai::detect_cv_with_dsm(m.rgb, dsm_gray, m.w, m.h,
                                   min_lat_, min_lon_, max_lat_, max_lon_,
                                   std::move(on_done));
        } else {
            running_ = false; status_ = "Unknown source";
        }
        break;
    }
}

// ── accept_checked ────────────────────────────────────────────────────────────
void DetectionPanel::accept_checked() {
    if (!on_accept_) return;
    std::scoped_lock lk(result_mu_);
    if (!result_) return;

    std::vector<ai::DetectedFeature> accepted;
    for (size_t i = 0; i < result_->features.size(); ++i) {
        if (i < checked_.size() && checked_[i] &&
            result_->features[i].status != ai::DetectedFeature::Status::Rejected) {
            result_->features[i].status = ai::DetectedFeature::Status::Accepted;
            accepted.push_back(result_->features[i]);
        }
    }
    if (!accepted.empty()) on_accept_(std::move(accepted));
}

// ── draw ──────────────────────────────────────────────────────────────────────
void DetectionPanel::draw(bool* p_open) {
    ImGui::SetNextWindowSize(ImVec2(380, 560), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("AI Feature Detection", p_open)) { ImGui::End(); return; }

    // ── Source selector ───────────────────────────────────────────────────────
    ImGui::SeparatorText("Detection source");
    static const char* src_names[] = {
        "Microsoft Building Footprints",
        "MapWithAI Roads (Facebook/Meta)",
        "Custom REST endpoint",
#ifdef CPPOSMUI_HAVE_ONNXRUNTIME
        "Local ONNX model",
#endif
        "CV on imagery (Sobel)",
        "CV + LiDAR DSM",
    };
    ImGui::SetNextItemWidth(-1);
    ImGui::Combo("##src", &source_, src_names, N_SOURCES);

    // Source-specific sub-settings
    if (source_ == 0) {
        ImGui::TextDisabled("Free dataset — no API key required.");
        ImGui::TextDisabled("Coverage: global (varies by region).");
    } else if (source_ == 1) {
        ImGui::TextDisabled("Free public endpoint — rate limits apply.");
        ImGui::TextDisabled("Best results in USA, Africa, Southeast Asia.");
    } else if (source_ == 2) {
        ImGui::TextDisabled("Your endpoint must return GeoJSON FeatureCollection.");
        ImGui::TextDisabled("{bbox} is replaced with min_lon,min_lat,max_lon,max_lat");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##url", &custom_url_);
#ifdef CPPOSMUI_HAVE_ONNXRUNTIME
    } else if (source_ == 3) {
        ImGui::TextDisabled("Segmentation model: float32[1,3,H,W] input, [1,C,H,W] output.");
        ImGui::TextDisabled("Classes: 0=bg  1=road  2=building  3=water");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("Model path##onnx", &model_path_);
        ImGui::TextDisabled("Imagery: %s @ z%d",
                            imagery_url_.empty() ? "(none)" : "active base layer",
                            imagery_zoom_);
#endif
    } else if (source_ == SRC_CV) {
        ImGui::TextDisabled("Sobel edge detection + colour segmentation on");
        ImGui::TextDisabled("tile imagery from the active base layer.");
        ImGui::TextDisabled("Works best on high-res satellite imagery.");
    } else if (source_ == SRC_CV_LIDAR) {
        ImGui::TextDisabled("Edge detection + LiDAR DSM (Geoportal NMPT).");
        ImGui::TextDisabled("Elevated surfaces in DSM boost building confidence.");
        ImGui::TextDisabled("Requires Geoportal LiDAR NMPT overlay active.");
    }

    // ── Bbox display ──────────────────────────────────────────────────────────
    ImGui::Spacing();
    ImGui::TextDisabled("Bbox: %.5f,%.5f → %.5f,%.5f",
                        min_lat_, min_lon_, max_lat_, max_lon_);

    ImGui::Spacing();
    ImGui::BeginDisabled(running_);
    if (ImGui::Button("Detect", ImVec2(-1, 0))) run_detection();
    ImGui::EndDisabled();
    if (running_) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1,.7f,0,1), "(running…)");
    }

    if (!status_.empty()) {
        ImVec4 col = result_ && !result_->ok()
            ? ImVec4(1,.4f,.4f,1) : ImVec4(.6f,1,.6f,1);
        ImGui::TextColored(col, "%s", status_.c_str());
    }

    // ── Results ───────────────────────────────────────────────────────────────
    {
        std::scoped_lock lk(result_mu_);
        if (result_ && !result_->features.empty()) {
            ImGui::SeparatorText("Candidates");
            ImGui::Checkbox("Show overlay on map", &show_overlay_);
            ImGui::SameLine();

            // Accept / reject all buttons
            if (ImGui::SmallButton("All")) {
                std::fill(checked_.begin(), checked_.end(), true);
                for (auto& f : result_->features)
                    if (f.status == ai::DetectedFeature::Status::Rejected)
                        f.status = ai::DetectedFeature::Status::Pending;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("None")) {
                std::fill(checked_.begin(), checked_.end(), false);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Reject checked")) {
                for (size_t i = 0; i < result_->features.size(); ++i)
                    if (i < checked_.size() && checked_[i])
                        result_->features[i].status =
                            ai::DetectedFeature::Status::Rejected;
            }

            // Feature list
            ImGui::BeginChild("feat_list", ImVec2(0, -40),
                              ImGuiChildFlags_Borders);
            for (size_t i = 0; i < result_->features.size(); ++i) {
                auto& f = result_->features[i];
                ImGui::PushID((int)i);

                // Status indicator
                ImU32 dot_col = feature_color(f);
                ImVec2 cp = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddCircleFilled(
                    {cp.x + 6, cp.y + 8}, 5.f, dot_col);
                ImGui::Dummy({14, 0});
                ImGui::SameLine();

                bool chk = (i < checked_.size()) && checked_[i];
                if (ImGui::Checkbox("##c", &chk)) {
                    if (i < checked_.size()) checked_[i] = chk;
                }
                ImGui::SameLine();

                // Label
                bool rejected = (f.status == ai::DetectedFeature::Status::Rejected);
                bool accepted = (f.status == ai::DetectedFeature::Status::Accepted);
                std::string label = std::format("{} ({:.0f}%)",
                    ai::feature_type_name(f.type), f.confidence * 100.f);
                if (accepted) label += " ✓";
                if (rejected) label += " ✗";

                // Source provenance
                if (!f.source.empty())
                    label += "  [" + f.source + "]";

                // First suggested tag as a hint
                if (!f.suggested_tags.empty()) {
                    const auto& [k, v] = *f.suggested_tags.begin();
                    label += "  " + k + "=" + v;
                }

                if (rejected)
                    ImGui::TextDisabled("%s", label.c_str());
                else if (accepted)
                    ImGui::TextColored(ImVec4(.6f,1,.6f,1), "%s", label.c_str());
                else
                    ImGui::TextUnformatted(label.c_str());

                // Tooltip with coordinates
                if (ImGui::IsItemHovered() && !f.coords.empty()) {
                    ImGui::BeginTooltip();
                    ImGui::Text("%.6f, %.6f", f.coords[0].first, f.coords[0].second);
                    ImGui::Text("%zu vertices, %s",
                                f.coords.size(), f.is_area ? "polygon" : "polyline");
                    ImGui::EndTooltip();
                }

                ImGui::PopID();
            }
            ImGui::EndChild();

            // Accept button
            int n_checked = (int)std::count(checked_.begin(), checked_.end(), true);
            ImGui::BeginDisabled(n_checked == 0 || !on_accept_);
            std::string accept_label = std::format("Add {} feature(s) to layer", n_checked);
            if (ImGui::Button(accept_label.c_str(), ImVec2(-1, 0)))
                accept_checked();
            ImGui::EndDisabled();
        }
    }

    ImGui::End();
}

// ── Map click interaction ────────────────────────────────────────────────────
void DetectionPanel::select_feature(int idx) {
    std::scoped_lock lk(result_mu_);
    if (!result_ || idx < 0 || idx >= (int)result_->features.size())
        selected_idx_ = -1;
    else
        selected_idx_ = idx;
}

void DetectionPanel::approve_selected() {
    std::scoped_lock lk(result_mu_);
    if (!result_ || selected_idx_ < 0 ||
        selected_idx_ >= (int)result_->features.size()) return;
    auto& f = result_->features[selected_idx_];
    if (f.status == ai::DetectedFeature::Status::Pending) {
        f.status = ai::DetectedFeature::Status::Accepted;
        if ((size_t)selected_idx_ < checked_.size())
            checked_[selected_idx_] = true;
        if (on_accept_) {
            std::vector<ai::DetectedFeature> v{f};
            on_accept_(std::move(v));
        }
    }
}

void DetectionPanel::reject_selected() {
    std::scoped_lock lk(result_mu_);
    if (!result_ || selected_idx_ < 0 ||
        selected_idx_ >= (int)result_->features.size()) return;
    result_->features[selected_idx_].status =
        ai::DetectedFeature::Status::Rejected;
    selected_idx_ = -1;
}

} // namespace ui
