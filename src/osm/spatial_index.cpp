#include "spatial_index.hpp"
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/post.hpp>
#include <future>
#include <algorithm>

namespace osm {

SpatialIndex SpatialIndex::build(const Dataset& ds) {
    SpatialIndex idx;

    // Build node R-tree in parallel with way R-tree using Boost.Asio thread pool
    boost::asio::thread_pool pool(2);

    std::vector<NodeEntry> node_entries;
    node_entries.reserve(ds.nodes.size());
    std::vector<WayEntry> way_entries;
    way_entries.reserve(ds.ways.size());

    std::promise<void> np, wp;

    boost::asio::post(pool, [&] {
        for (const auto& [id, n] : ds.nodes)
            node_entries.emplace_back(GeoPoint{n.lon, n.lat}, id);
        np.set_value();
    });

    boost::asio::post(pool, [&] {
        for (const auto& [id, w] : ds.ways) {
            if (w.nodes.empty()) continue;
            double min_lat =  90, max_lat = -90;
            double min_lon = 180, max_lon = -180;
            for (const auto& nd : w.nodes) {
                auto it = ds.nodes.find(nd.ref);
                if (it == ds.nodes.end()) continue;
                min_lat = std::min(min_lat, it->second.lat);
                max_lat = std::max(max_lat, it->second.lat);
                min_lon = std::min(min_lon, it->second.lon);
                max_lon = std::max(max_lon, it->second.lon);
            }
            if (min_lat > max_lat) continue;
            way_entries.emplace_back(
                bg::model::box<GeoPoint>{{min_lon, min_lat}, {max_lon, max_lat}}, id);
        }
        wp.set_value();
    });

    np.get_future().wait();
    wp.get_future().wait();
    pool.join();

    idx.nodes = NodeRTree(node_entries.begin(), node_entries.end());
    idx.ways  = WayRTree (way_entries.begin(),  way_entries.end());
    return idx;
}

std::vector<NodeId> SpatialIndex::query_nodes(
    double min_lat, double min_lon, double max_lat, double max_lon) const
{
    bg::model::box<GeoPoint> box{{min_lon, min_lat}, {max_lon, max_lat}};
    std::vector<NodeEntry> results;
    nodes.query(bgi::intersects(box), std::back_inserter(results));
    std::vector<NodeId> ids;
    ids.reserve(results.size());
    for (auto& [pt, id] : results) ids.push_back(id);
    return ids;
}

std::vector<WayId> SpatialIndex::query_ways(
    double min_lat, double min_lon, double max_lat, double max_lon) const
{
    bg::model::box<GeoPoint> box{{min_lon, min_lat}, {max_lon, max_lat}};
    std::vector<WayEntry> results;
    ways.query(bgi::intersects(box), std::back_inserter(results));
    std::vector<WayId> ids;
    ids.reserve(results.size());
    for (auto& [box2, id] : results) ids.push_back(id);
    return ids;
}

} // namespace osm
