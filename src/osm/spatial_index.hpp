#pragma once
#include "types.hpp"
#include <boost/geometry.hpp>
#include <boost/geometry/geometries/point.hpp>
#include <boost/geometry/index/rtree.hpp>
#include <vector>
#include <span>

namespace osm {

namespace bg  = boost::geometry;
namespace bgi = boost::geometry::index;

// Cartesian (planar) lon/lat degree space: the R-tree is used only for
// axis-aligned bbox viewport culling, so we don't need geodesic distances.
// Cartesian also keeps model::point/box satisfying Boost's is_indexable trait
// (geographic cs trips it up under MSVC, breaking rtree pair-indexable lookup).
using GeoPoint = bg::model::point<double, 2, bg::cs::cartesian>;
using NodeEntry = std::pair<GeoPoint, NodeId>;
using WayEntry  = std::pair<bg::model::box<GeoPoint>, WayId>;

using NodeRTree = bgi::rtree<NodeEntry, bgi::rstar<16>>;
using WayRTree  = bgi::rtree<WayEntry,  bgi::rstar<16>>;

struct SpatialIndex {
    NodeRTree nodes;
    WayRTree  ways;

    // Build from a dataset (call after loading / applying changeset)
    static SpatialIndex build(const Dataset& ds);

    // Query nodes inside a lat/lon bounding box
    std::vector<NodeId> query_nodes(double min_lat, double min_lon,
                                     double max_lat, double max_lon) const;

    // Query ways whose bbox overlaps the given region
    std::vector<WayId>  query_ways(double min_lat, double min_lon,
                                    double max_lat, double max_lon) const;
};

} // namespace osm
