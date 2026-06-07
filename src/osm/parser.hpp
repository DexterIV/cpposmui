#pragma once
#include "types.hpp"
#include <filesystem>
#include <expected>
#include <string>

namespace osm {

enum class ParseError {
    FileNotFound,
    XmlError,
    UnknownFormat,
};

// Load a full .osm file into a Dataset
std::expected<Dataset, ParseError>
load_osm(const std::filesystem::path& path);

// Load a .osc (change) file; produces a ChangeSet whose before states are empty
// (caller must populate them from an existing Dataset via diff::apply_changeset)
std::expected<ChangeSet, ParseError>
load_osc(const std::filesystem::path& path);

// Serialize a ChangeSet to an OsmChange (.osc) file. Returns false on failure.
bool write_osc(const ChangeSet& cs, const std::filesystem::path& path);

// Serialize a ChangeSet to an OsmChange XML string. When changeset_id != 0 it is
// stamped onto every element (required by the OSM API upload endpoint).
std::string changeset_xml(const ChangeSet& cs, long long changeset_id);

} // namespace osm
