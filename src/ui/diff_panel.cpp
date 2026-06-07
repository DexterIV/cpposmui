#include "diff_panel.hpp"
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <format>
#include <string_view>

namespace ui {

static ImVec4 color_for_state(osm::DiffState s) {
    switch (s) {
    case osm::DiffState::Added:    return {0.2f, 0.8f, 0.2f, 1.f};
    case osm::DiffState::Modified: return {1.0f, 0.8f, 0.0f, 1.f};
    case osm::DiffState::Deleted:  return {0.9f, 0.3f, 0.3f, 1.f};
    default:                       return {0.7f, 0.7f, 0.7f, 1.f};
    }
}

static const char* label_for_state(osm::DiffState s) {
    switch (s) {
    case osm::DiffState::Added:    return "[+]";
    case osm::DiffState::Modified: return "[~]";
    case osm::DiffState::Deleted:  return "[-]";
    default:                       return "[ ]";
    }
}

void DiffPanel::draw_tag_diff_row(const osm::TagDiff& d) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted(d.key.c_str());

    ImGui::TableSetColumnIndex(1);
    if (d.old_val)
        ImGui::TextColored({0.9f, 0.5f, 0.5f, 1.f}, "%s", d.old_val->c_str());
    else
        ImGui::TextDisabled("—");

    ImGui::TableSetColumnIndex(2);
    if (d.new_val)
        ImGui::TextColored({0.5f, 0.9f, 0.5f, 1.f}, "%s", d.new_val->c_str());
    else
        ImGui::TextDisabled("—");
}

void DiffPanel::draw_object_section(
    const char* label, osm::DiffState state,
    const std::string& obj_type, int64_t id,
    const osm::TagMap* before_tags,
    const osm::TagMap* after_tags,
    const std::vector<osm::TagDiff>& tag_diffs)
{
    std::string header = std::format("{} {} #{}", label_for_state(state), obj_type, id);

    ImGui::PushStyleColor(ImGuiCol_Text, color_for_state(state));
    bool open = ImGui::TreeNodeEx(header.c_str(),
                                   ImGuiTreeNodeFlags_SpanAvailWidth |
                                   (id == selected_id_ ? ImGuiTreeNodeFlags_Selected : 0));
    ImGui::PopStyleColor();

    if (ImGui::IsItemClicked()) { selected_id_ = id; selected_type_ = osm::ObjType::Node; }

    if (open) {
        if (!tag_diffs.empty()) {
            if (ImGui::BeginTable("tags", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                              ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Key",  ImGuiTableColumnFlags_WidthStretch, 0.4f);
                ImGui::TableSetupColumn("Before", ImGuiTableColumnFlags_WidthStretch, 0.3f);
                ImGui::TableSetupColumn("After",  ImGuiTableColumnFlags_WidthStretch, 0.3f);
                ImGui::TableHeadersRow();

                for (const auto& d : tag_diffs)
                    draw_tag_diff_row(d);

                ImGui::EndTable();
            }
        } else if (state == osm::DiffState::Added && after_tags) {
            if (ImGui::BeginTable("tags_new", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Key"); ImGui::TableSetupColumn("Value");
                ImGui::TableHeadersRow();
                for (const auto& [k, v] : *after_tags) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(k.c_str());
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextColored({0.5f, 0.9f, 0.5f, 1.f}, "%s", v.c_str());
                }
                ImGui::EndTable();
            }
        } else if (state == osm::DiffState::Deleted && before_tags) {
            if (ImGui::BeginTable("tags_del", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Key"); ImGui::TableSetupColumn("Value");
                ImGui::TableHeadersRow();
                for (const auto& [k, v] : *before_tags) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(k.c_str());
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextColored({0.9f, 0.5f, 0.5f, 1.f}, "%s", v.c_str());
                }
                ImGui::EndTable();
            }
        } else {
            ImGui::TextDisabled("No tag changes");
        }
        ImGui::TreePop();
    }
}

void DiffPanel::draw(bool* p_open) {
    ImGui::Begin("Diff", p_open, ImGuiWindowFlags_NoCollapse);

    if (!cs_) { ImGui::TextDisabled("No changeset loaded."); ImGui::End(); return; }

    // Summary bar
    int n_add = 0, n_mod = 0, n_del = 0;
    for (auto& d : cs_->nodes) {
        if (d.state == osm::DiffState::Added)    ++n_add;
        if (d.state == osm::DiffState::Modified) ++n_mod;
        if (d.state == osm::DiffState::Deleted)  ++n_del;
    }
    for (auto& d : cs_->ways) {
        if (d.state == osm::DiffState::Added)    ++n_add;
        if (d.state == osm::DiffState::Modified) ++n_mod;
        if (d.state == osm::DiffState::Deleted)  ++n_del;
    }
    ImGui::TextColored({0.2f,0.8f,0.2f,1.f}, "+%d", n_add); ImGui::SameLine();
    ImGui::TextColored({1.0f,0.8f,0.0f,1.f}, "~%d", n_mod); ImGui::SameLine();
    ImGui::TextColored({0.9f,0.3f,0.3f,1.f}, "-%d", n_del);
    ImGui::Separator();

    // Filters
    ImGui::Checkbox("[+]", &show_added_); ImGui::SameLine();
    ImGui::Checkbox("[~]", &show_modified_); ImGui::SameLine();
    ImGui::Checkbox("[-]", &show_deleted_); ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##filter", &filter_buf_);
    ImGui::Separator();

    std::string_view filter = filter_buf_;

    ImGui::BeginChild("diff_scroll");

    // Nodes
    for (const auto& d : cs_->nodes) {
        if (!show_added_   && d.state == osm::DiffState::Added)    continue;
        if (!show_modified_&& d.state == osm::DiffState::Modified) continue;
        if (!show_deleted_ && d.state == osm::DiffState::Deleted)  continue;
        int64_t id = d.after ? d.after->id : (d.before ? d.before->id : 0);
        if (!filter.empty() && std::to_string(id).find(filter) == std::string::npos) continue;
        const osm::TagMap* bt = d.before ? &d.before->tags : nullptr;
        const osm::TagMap* at = d.after  ? &d.after->tags  : nullptr;
        draw_object_section("Node", d.state, "node", id, bt, at, d.tag_diffs);
    }

    // Ways
    for (const auto& d : cs_->ways) {
        if (!show_added_   && d.state == osm::DiffState::Added)    continue;
        if (!show_modified_&& d.state == osm::DiffState::Modified) continue;
        if (!show_deleted_ && d.state == osm::DiffState::Deleted)  continue;
        int64_t id = d.after ? d.after->id : (d.before ? d.before->id : 0);
        if (!filter.empty() && std::to_string(id).find(filter) == std::string::npos) continue;
        const osm::TagMap* bt = d.before ? &d.before->tags : nullptr;
        const osm::TagMap* at = d.after  ? &d.after->tags  : nullptr;
        draw_object_section("Way", d.state, "way", id, bt, at, d.tag_diffs);
    }

    ImGui::EndChild();
    ImGui::End();
}

} // namespace ui
