#include "overpass_panel.hpp"
#include "../net/overpass.hpp"
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <format>
#include <mutex>

namespace ui {

void OverpassPanel::set_bbox(double min_lat, double min_lon, double max_lat, double max_lon) {
    min_lat_ = min_lat; min_lon_ = min_lon;
    max_lat_ = max_lat; max_lon_ = max_lon;
}

void OverpassPanel::fire_query() {
    if (loading_) return;
    loading_ = true;
    status_ = "Querying…";

    std::string ql = ql_buf_;
    std::string ep = endpoint_buf_;

    net::query_overpass_async(ql,
        [this](std::expected<osm::Dataset, net::OverpassError> res) {
            loading_ = false;
            if (!res) {
                status_ = "Error: query failed";
                return;
            }
            status_ = std::format("OK – {} nodes, {} ways, {} relations",
                res->nodes.size(), res->ways.size(), res->relations.size());
            on_result_(std::move(*res));
        }, ep);
}

void OverpassPanel::draw(bool* p_open) {
    ImGui::Begin("Overpass", p_open, ImGuiWindowFlags_NoCollapse);

    // Pre-fill bbox template button
    if (ImGui::Button("Use current bbox"))
        ql_buf_ = net::bbox_query(min_lat_, min_lon_, max_lat_, max_lon_);
    ImGui::SameLine();
    if (ImGui::Button("Clear")) ql_buf_.clear();

    ImGui::TextDisabled("Endpoint:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##ep", &endpoint_buf_);

    ImGui::TextDisabled("Query (Overpass QL):");
    ImGui::InputTextMultiline("##ql", &ql_buf_, ImVec2(-1, 200));

    ImGui::BeginDisabled(loading_);
    if (ImGui::Button("Run query")) fire_query();
    ImGui::EndDisabled();

    if (!status_.empty()) {
        ImGui::SameLine();
        ImGui::TextUnformatted(status_.c_str());
    }
    if (loading_) {
        ImGui::SameLine();
        ImGui::TextDisabled("...");
    }

    ImGui::End();
}

} // namespace ui
