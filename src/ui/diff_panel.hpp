#pragma once
#include "../osm/types.hpp"
#include <string>

namespace ui {

// Renders the diff side-panel showing all changes in a ChangeSet.
// Call from main ImGui render loop.
class DiffPanel {
public:
    void set_changeset(const osm::ChangeSet* cs) { cs_ = cs; }
    void draw(bool* p_open = nullptr);

    // Returns the ID of the currently selected object (for map highlight)
    int64_t selected_id() const { return selected_id_; }
    osm::ObjType selected_type() const { return selected_type_; }

private:
    const osm::ChangeSet* cs_{nullptr};
    int64_t selected_id_{0};
    osm::ObjType selected_type_{osm::ObjType::Node};

    // Filter state
    bool show_added_{true}, show_modified_{true}, show_deleted_{true};
    std::string filter_buf_;

    void draw_tag_diff_row(const osm::TagDiff& d);
    void draw_object_section(const char* label, osm::DiffState state,
                              const std::string& obj_type, int64_t id,
                              const osm::TagMap* before_tags,
                              const osm::TagMap* after_tags,
                              const std::vector<osm::TagDiff>& tag_diffs);
};

} // namespace ui
