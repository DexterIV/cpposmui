#include <catch2/catch_test_macros.hpp>
#include "osm/spatial_index.hpp"
#include "osm/parser.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Spatial index – unit tests
// ─────────────────────────────────────────────────────────────────────────────

static osm::Dataset make_dataset() {
    osm::Dataset ds;
    ds.nodes[1] = {1, 52.22, 21.01, {}, 1};  // inside typical query bbox
    ds.nodes[2] = {2, 52.23, 21.02, {}, 1};
    ds.nodes[3] = {3, 53.00, 22.00, {}, 1};  // far away
    // Way spanning nodes 1 and 2
    ds.ways[10] = {10, {{1}, {2}}, {{"highway","primary"}}, 1};
    return ds;
}

TEST_CASE("SpatialIndex: query_nodes returns nodes in bbox", "[spatial]") {
    auto ds  = make_dataset();
    auto idx = osm::SpatialIndex::build(ds);

    auto ids = idx.query_nodes(52.20, 21.00, 52.24, 21.05);
    REQUIRE(ids.size() == 2);
    CHECK(std::ranges::find(ids, osm::NodeId{1}) != ids.end());
    CHECK(std::ranges::find(ids, osm::NodeId{2}) != ids.end());
    CHECK(std::ranges::find(ids, osm::NodeId{3}) == ids.end());
}

TEST_CASE("SpatialIndex: query_nodes excludes out-of-bbox nodes", "[spatial]") {
    auto ds  = make_dataset();
    auto idx = osm::SpatialIndex::build(ds);

    auto ids = idx.query_nodes(53.0, 22.0, 53.1, 22.1);
    REQUIRE(ids.size() == 1);
    CHECK(ids[0] == 3);
}

TEST_CASE("SpatialIndex: query_ways returns overlapping ways", "[spatial]") {
    auto ds  = make_dataset();
    auto idx = osm::SpatialIndex::build(ds);

    auto ids = idx.query_ways(52.21, 21.00, 52.24, 21.05);
    REQUIRE(ids.size() == 1);
    CHECK(ids[0] == 10);
}

TEST_CASE("SpatialIndex: empty dataset returns empty results", "[spatial]") {
    osm::Dataset ds;
    auto idx = osm::SpatialIndex::build(ds);
    CHECK(idx.query_nodes(0, 0, 90, 180).empty());
    CHECK(idx.query_ways(0, 0, 90, 180).empty());
}

TEST_CASE("SpatialIndex: built from fixture file", "[spatial][integration]") {
    auto ds = osm::load_osm(TEST_DATA_DIR "/simple.osm");
    REQUIRE(ds.has_value());

    auto idx = osm::SpatialIndex::build(*ds);

    // All 4 nodes are in the bbox of the file
    auto nodes = idx.query_nodes(52.22, 21.01, 52.228, 21.018);
    CHECK(nodes.size() == 4);

    // Both ways should appear
    auto ways = idx.query_ways(52.22, 21.01, 52.228, 21.018);
    CHECK(ways.size() == 2);
}
