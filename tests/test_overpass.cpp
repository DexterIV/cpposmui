#include <catch2/catch_test_macros.hpp>
#include "net/overpass.hpp"
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// Overpass query builder – unit tests (no network)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("bbox_query: contains all three element types", "[overpass]") {
    auto q = net::bbox_query(52.22, 21.01, 52.23, 21.02);
    CHECK(q.find("node")     != std::string::npos);
    CHECK(q.find("way")      != std::string::npos);
    CHECK(q.find("relation") != std::string::npos);
}

TEST_CASE("bbox_query: contains the bbox coordinates", "[overpass]") {
    auto q = net::bbox_query(52.22, 21.01, 52.23, 21.02);
    CHECK(q.find("52.220000") != std::string::npos);
    CHECK(q.find("21.010000") != std::string::npos);
    CHECK(q.find("52.230000") != std::string::npos);
    CHECK(q.find("21.020000") != std::string::npos);
}

TEST_CASE("bbox_query: includes optional filter", "[overpass]") {
    auto q = net::bbox_query(52.22, 21.01, 52.23, 21.02, "[amenity=cafe]");
    CHECK(q.find("[amenity=cafe]") != std::string::npos);
}

TEST_CASE("bbox_query: is valid Overpass QL (has out body)", "[overpass]") {
    auto q = net::bbox_query(0, 0, 1, 1);
    CHECK(q.find("out body") != std::string::npos);
    CHECK(q.find("[out:xml]") != std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// Integration test (network) – skipped unless CPPOSMUI_NETWORK_TESTS is set
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdlib>

TEST_CASE("query_overpass_async: fetches real data [network]",
          "[overpass][integration][.]")
{
    if (!std::getenv("CPPOSMUI_NETWORK_TESTS"))
        SKIP("Set CPPOSMUI_NETWORK_TESTS=1 to run network tests");

    std::atomic<bool> done{false};
    std::expected<osm::Dataset, net::OverpassError> result;

    // Small area in Warsaw
    auto q = net::bbox_query(52.228, 21.012, 52.230, 21.015, "[highway]");
    net::query_overpass_async(q, [&](auto r) {
        result = std::move(r);
        done   = true;
        done.notify_one();
    });

    done.wait(false);

    REQUIRE(result.has_value());
    CHECK_FALSE(result->ways.empty());
}
