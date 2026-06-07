#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include "osm/parser.hpp"
#include "osm/diff.hpp"

using namespace Catch::Matchers;

// ─────────────────────────────────────────────────────────────────────────────
// OSM parser – unit tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("load_osm: parses nodes", "[parser][osm]") {
    auto ds = osm::load_osm(TEST_DATA_DIR "/simple.osm");
    REQUIRE(ds.has_value());
    REQUIRE(ds->nodes.size() == 4);

    auto& n1 = ds->nodes.at(1);
    CHECK(n1.id      == 1);
    CHECK(n1.lat     == Catch::Approx(52.2250));
    CHECK(n1.lon     == Catch::Approx(21.0150));
    CHECK(n1.version == 1);
    REQUIRE(n1.tags.contains("amenity"));
    CHECK(n1.tags.at("amenity") == "cafe");
    CHECK(n1.tags.at("name")    == "Test Node A");
}

TEST_CASE("load_osm: parses ways", "[parser][osm]") {
    auto ds = osm::load_osm(TEST_DATA_DIR "/simple.osm");
    REQUIRE(ds.has_value());
    REQUIRE(ds->ways.size() == 2);

    auto& w100 = ds->ways.at(100);
    CHECK(w100.nodes.size() == 4);  // closed ring includes start node twice
    CHECK(w100.tags.at("building") == "yes");

    auto& w101 = ds->ways.at(101);
    CHECK(w101.version == 2);
    CHECK(w101.tags.at("maxspeed") == "50");
}

TEST_CASE("load_osm: computes bounds", "[parser][osm]") {
    auto ds = osm::load_osm(TEST_DATA_DIR "/simple.osm");
    REQUIRE(ds.has_value());
    CHECK(ds->min_lat == Catch::Approx(52.22));
    CHECK(ds->max_lat == Catch::Approx(52.23));
}

TEST_CASE("load_osm: returns error for non-existent file", "[parser][osm]") {
    auto ds = osm::load_osm("nonexistent.osm");
    REQUIRE_FALSE(ds.has_value());
    CHECK(ds.error() == osm::ParseError::XmlError);
}

// ─────────────────────────────────────────────────────────────────────────────
// OSC parser – unit tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("load_osc: parses create/modify/delete blocks", "[parser][osc]") {
    auto cs = osm::load_osc(TEST_DATA_DIR "/changes.osc");
    REQUIRE(cs.has_value());

    // count by state
    int adds = 0, mods = 0, dels = 0;
    for (auto& d : cs->nodes) {
        if (d.state == osm::DiffState::Added)    ++adds;
        if (d.state == osm::DiffState::Modified) ++mods;
        if (d.state == osm::DiffState::Deleted)  ++dels;
    }
    CHECK(adds == 1);  // node 999
    CHECK(mods == 1);  // node 1
    CHECK(dels == 1);  // node 4

    // ways
    int wmods = 0;
    for (auto& d : cs->ways) if (d.state == osm::DiffState::Modified) ++wmods;
    CHECK(wmods == 1);  // way 101
}

TEST_CASE("load_osc: added node has correct tags", "[parser][osc]") {
    auto cs = osm::load_osc(TEST_DATA_DIR "/changes.osc");
    REQUIRE(cs.has_value());

    auto it = std::ranges::find_if(cs->nodes, [](const auto& d) {
        return d.state == osm::DiffState::Added && d.after && d.after->id == 999;
    });
    REQUIRE(it != cs->nodes.end());
    CHECK(it->after->tags.at("amenity") == "bench");
}
