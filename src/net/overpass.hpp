#pragma once
#include "../osm/types.hpp"
#include <string>
#include <functional>
#include <expected>

namespace net {

enum class OverpassError { NetworkError, ParseError, Timeout, QueryError };

// Build a bbox query string for Overpass QL
std::string bbox_query(double min_lat, double min_lon, double max_lat, double max_lon,
                       const std::string& filter = "");

// Async fetch – fires callback on worker thread with parsed Dataset
void query_overpass_async(
    const std::string& ql,
    std::function<void(std::expected<osm::Dataset, OverpassError>)> cb,
    const std::string& endpoint = "https://overpass-api.de/api/interpreter");

// Download an editable area via the official OSM API 0.6 /map call — the same
// fast, reliable endpoint JOSM uses. The bbox must be small (server rejects
// areas larger than 0.25 deg²). Fires the callback on a worker thread.
void download_osm_bbox_async(
    double min_lat, double min_lon, double max_lat, double max_lon,
    std::function<void(std::expected<osm::Dataset, OverpassError>)> cb,
    const std::string& api_base = "https://api.openstreetmap.org");

} // namespace net
