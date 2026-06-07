#pragma once
#include "types.hpp"
#include <string>

namespace osm {

// A single editable OSM data layer (analogous to JOSM's OsmDataLayer).
struct DataLayer {
    std::string  name{"New layer"};
    Dataset      data;
    Dataset      baseline;    // snapshot at load/download time; diff against this for export/upload
    bool         visible{true};
    bool         modified{false}; // true if data differs from baseline
};

} // namespace osm
