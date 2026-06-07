#pragma once
#include "../osm/types.hpp"
#include <functional>
#include <string>

namespace ui {

// In-place tag editor widget (used in both the inspector and diff panel).
// Returns true if tags were modified.
class TagEditor {
public:
    // Renders inside a pre-opened ImGui window / child
    bool draw(osm::TagMap& tags, bool read_only = false);

private:
    std::string new_key_;
    std::string new_val_;
    int         edit_row_{-1};
    std::string edit_val_;
    std::string preset_filter_; // filter text inside the preset dropdowns
};

} // namespace ui
