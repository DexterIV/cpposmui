#include <catch2/catch_test_macros.hpp>
#include "osm/types.hpp"
#include "osm/diff.hpp"
#include "osm/parser.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Tag diff – unit tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("compute_tag_diffs: detects added tag", "[diff][tags]") {
    osm::TagMap before = {{"name", "Foo"}};
    osm::TagMap after  = {{"name", "Foo"}, {"amenity", "cafe"}};
    auto diffs = osm::diff::compute_tag_diffs(before, after);
    REQUIRE(diffs.size() == 1);
    CHECK(diffs[0].key     == "amenity");
    CHECK(!diffs[0].old_val.has_value());
    CHECK(diffs[0].new_val == "cafe");
}

TEST_CASE("compute_tag_diffs: detects removed tag", "[diff][tags]") {
    osm::TagMap before = {{"name", "Foo"}, {"amenity", "cafe"}};
    osm::TagMap after  = {{"name", "Foo"}};
    auto diffs = osm::diff::compute_tag_diffs(before, after);
    REQUIRE(diffs.size() == 1);
    CHECK(diffs[0].key     == "amenity");
    CHECK(diffs[0].old_val == "cafe");
    CHECK(!diffs[0].new_val.has_value());
}

TEST_CASE("compute_tag_diffs: detects modified tag", "[diff][tags]") {
    osm::TagMap before = {{"maxspeed", "50"}};
    osm::TagMap after  = {{"maxspeed", "30"}};
    auto diffs = osm::diff::compute_tag_diffs(before, after);
    REQUIRE(diffs.size() == 1);
    CHECK(diffs[0].old_val == "50");
    CHECK(diffs[0].new_val == "30");
}

TEST_CASE("compute_tag_diffs: no diffs when identical", "[diff][tags]") {
    osm::TagMap tags = {{"name", "Bar"}, {"highway", "residential"}};
    auto diffs = osm::diff::compute_tag_diffs(tags, tags);
    CHECK(diffs.empty());
}

TEST_CASE("compute_tag_diffs: result is sorted by key", "[diff][tags]") {
    osm::TagMap before = {};
    osm::TagMap after  = {{"z_key", "1"}, {"a_key", "2"}, {"m_key", "3"}};
    auto diffs = osm::diff::compute_tag_diffs(before, after);
    REQUIRE(diffs.size() == 3);
    CHECK(diffs[0].key == "a_key");
    CHECK(diffs[1].key == "m_key");
    CHECK(diffs[2].key == "z_key");
}

// ─────────────────────────────────────────────────────────────────────────────
// Changeset enrichment – integration test (loads real fixture files)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("enrich_changeset: fills before states from base dataset", "[diff][integration]") {
    auto base = osm::load_osm(TEST_DATA_DIR "/simple.osm");
    auto cs   = osm::load_osc(TEST_DATA_DIR "/changes.osc");
    REQUIRE(base); REQUIRE(cs);

    osm::diff::enrich_changeset(*cs, *base);

    // Modified node 1 should have before state
    auto it = std::ranges::find_if(cs->nodes, [](const auto& d) {
        return d.state == osm::DiffState::Modified && d.after && d.after->id == 1;
    });
    REQUIRE(it != cs->nodes.end());
    REQUIRE(it->before.has_value());
    CHECK(it->before->tags.at("name") == "Test Node A");

    // Tag diffs should be populated
    REQUIRE_FALSE(it->tag_diffs.empty());

    // "name" tag was changed
    auto name_diff = std::ranges::find_if(it->tag_diffs, [](const auto& d) {
        return d.key == "name";
    });
    REQUIRE(name_diff != it->tag_diffs.end());
    CHECK(name_diff->old_val == "Test Node A");
    CHECK(name_diff->new_val == "Test Node A (updated)");

    // "opening_hours" was added
    auto oh_diff = std::ranges::find_if(it->tag_diffs, [](const auto& d) {
        return d.key == "opening_hours";
    });
    REQUIRE(oh_diff != it->tag_diffs.end());
    CHECK(!oh_diff->old_val.has_value());
}

// ─────────────────────────────────────────────────────────────────────────────
// Dataset diff
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("diff_datasets: detects node added in incoming", "[diff][dataset]") {
    osm::Dataset base, incoming;
    base.nodes[1] = {1, 52.0, 21.0, {{"name", "A"}}, 1};
    incoming.nodes[1] = base.nodes[1];
    incoming.nodes[2] = {2, 52.1, 21.1, {{"name", "B"}}, 1};

    auto cs = osm::diff::diff_datasets(base, incoming);
    REQUIRE(cs.nodes.size() == 1);
    CHECK(cs.nodes[0].state == osm::DiffState::Added);
    CHECK(cs.nodes[0].after->id == 2);
}

TEST_CASE("diff_datasets: detects node deleted from base", "[diff][dataset]") {
    osm::Dataset base, incoming;
    base.nodes[1]     = {1, 52.0, 21.0, {}, 1};
    base.nodes[2]     = {2, 52.1, 21.1, {}, 1};
    incoming.nodes[1] = base.nodes[1];

    auto cs = osm::diff::diff_datasets(base, incoming);
    REQUIRE(cs.nodes.size() == 1);
    CHECK(cs.nodes[0].state == osm::DiffState::Deleted);
    CHECK(cs.nodes[0].before->id == 2);
}

TEST_CASE("diff_datasets: detects modified way tags", "[diff][dataset]") {
    osm::Dataset base, incoming;
    base.nodes[1] = incoming.nodes[1] = {1, 52.0, 21.0, {}, 1};
    base.nodes[2] = incoming.nodes[2] = {2, 52.1, 21.1, {}, 1};

    base.ways[10]     = {10, {{1},{2}}, {{"maxspeed","50"}}, 1};
    incoming.ways[10] = {10, {{1},{2}}, {{"maxspeed","30"}}, 2};

    auto cs = osm::diff::diff_datasets(base, incoming);
    REQUIRE(cs.ways.size() == 1);
    CHECK(cs.ways[0].state == osm::DiffState::Modified);
    REQUIRE(cs.ways[0].tag_diffs.size() == 1);
    CHECK(cs.ways[0].tag_diffs[0].old_val == "50");
    CHECK(cs.ways[0].tag_diffs[0].new_val == "30");
}

TEST_CASE("apply_changeset: correctly applies add/modify/delete", "[diff][apply]") {
    osm::Dataset base;
    base.nodes[1] = {1, 52.0, 21.0, {{"name","A"}}, 1};
    base.nodes[2] = {2, 52.1, 21.1, {}, 1};

    osm::ChangeSet cs;
    // Modify node 1
    osm::ObjectDiff<osm::Node> mod;
    mod.state = osm::DiffState::Modified;
    mod.after = {1, 52.0, 21.0, {{"name","A_new"}}, 2};
    cs.nodes.push_back(mod);
    // Delete node 2
    osm::ObjectDiff<osm::Node> del;
    del.state = osm::DiffState::Deleted;
    del.after = {2, 52.1, 21.1, {}, 1};
    cs.nodes.push_back(del);
    // Add node 3
    osm::ObjectDiff<osm::Node> add;
    add.state = osm::DiffState::Added;
    add.after = {3, 53.0, 22.0, {{"amenity","bench"}}, 1};
    cs.nodes.push_back(add);

    auto result = osm::diff::apply_changeset(base, cs);

    REQUIRE(result.nodes.size() == 2);
    CHECK(result.nodes.at(1).tags.at("name") == "A_new");
    CHECK_FALSE(result.nodes.contains(2));
    CHECK(result.nodes.at(3).tags.at("amenity") == "bench");
}
