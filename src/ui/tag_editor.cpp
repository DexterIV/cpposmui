#include "tag_editor.hpp"
#include "../osm/tag_presets.hpp"
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <algorithm>
#include <cctype>
#include <vector>
#include <string>
#include <string_view>

namespace {
bool icontains(std::string_view hay, std::string_view needle) {
    if (needle.empty()) return true;
    auto lower = [](char c){ return (char)std::tolower((unsigned char)c); };
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        bool ok = true;
        for (size_t j = 0; j < needle.size(); ++j)
            if (lower(hay[i + j]) != lower(needle[j])) { ok = false; break; }
        if (ok) return true;
    }
    return false;
}
} // namespace

namespace ui {

bool TagEditor::draw(osm::TagMap& tags, bool read_only) {
    bool changed = false;

    if (ImGui::BeginTable("tag_editor", read_only ? 2 : 3,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY,
                          ImVec2(0, 200))) {
        ImGui::TableSetupColumn("Key",   ImGuiTableColumnFlags_WidthStretch, 0.4f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.5f);
        if (!read_only)
            ImGui::TableSetupColumn("##del", ImGuiTableColumnFlags_WidthFixed, 24.f);
        ImGui::TableHeadersRow();

        // Collect keys for stable iteration
        std::vector<std::string> keys;
        keys.reserve(tags.size());
        for (const auto& [k, _] : tags) keys.push_back(k);
        std::ranges::sort(keys);

        std::string key_to_delete;
        int row = 0;
        for (const auto& k : keys) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(k.c_str());

            ImGui::TableSetColumnIndex(1);
            if (!read_only && edit_row_ == row) {
                ImGui::SetNextItemWidth(-1);
                ImGui::PushID(row);
                if (ImGui::InputText("##ev", &edit_val_,
                                     ImGuiInputTextFlags_EnterReturnsTrue)) {
                    tags[k] = edit_val_;
                    edit_row_ = -1;
                    changed = true;
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    tags[k] = edit_val_;
                    edit_row_ = -1;
                    changed = true;
                }
                ImGui::PopID();
            } else {
                ImGui::TextUnformatted(tags[k].c_str());
                if (!read_only && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                    edit_row_ = row;
                    edit_val_ = tags[k];
                }
            }

            if (!read_only) {
                ImGui::TableSetColumnIndex(2);
                ImGui::PushID(row + 10000);
                if (ImGui::SmallButton("x")) key_to_delete = k;
                ImGui::PopID();
            }
            ++row;
        }

        if (!key_to_delete.empty()) { tags.erase(key_to_delete); changed = true; }

        ImGui::EndTable();
    }

    if (!read_only) {
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.4f);
        ImGui::InputText("Key##nk",   &new_key_);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-60.f);
        ImGui::InputText("Value##nv", &new_val_);
        ImGui::SameLine();
        if (ImGui::Button("Add") && !new_key_.empty()) {
            tags[new_key_] = new_val_;
            new_key_.clear(); new_val_.clear();
            changed = true;
        }

        // ── Preset autocomplete / tag search ──
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
        if (ImGui::BeginCombo("##keypreset", "search keys…", ImGuiComboFlags_HeightLargest)) {
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##kf", "filter", &preset_filter_);
            for (auto key : osm::presets::common_keys) {
                if (!icontains(key, preset_filter_)) continue;
                std::string s(key);
                if (ImGui::Selectable(s.c_str())) {
                    new_key_ = s;
                    preset_filter_.clear();
                }
            }
            ImGui::EndCombo();
        }
        // Value suggestions for the current key.
        if (auto vit = osm::presets::key_values.find(new_key_);
            vit != osm::presets::key_values.end()) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##valpreset", "values for this key…",
                                  ImGuiComboFlags_HeightLargest)) {
                for (auto val : vit->second) {
                    std::string s(val);
                    if (ImGui::Selectable(s.c_str()))
                        new_val_ = s;
                }
                ImGui::EndCombo();
            }
        }
    }
    return changed;
}

} // namespace ui
