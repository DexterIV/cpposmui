#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "ai/detection.hpp"

#include <future>
#include <vector>
#include <cstdint>
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
// Detection — classical-CV unit tests.
//
// These run the real Sobel/colour-segmentation pipeline (ai::detect_cv_on_imagery
// and ai::detect_cv_with_dsm) over *synthetic* imagery built in-memory, so the
// whole suite is offline and deterministic — no tiles fetched, no GL, no app.
//
// The synthetic scenes mimic what the detector is tuned for:
//   • Buildings — solid, uniform-colour rectangles (low colour variance → high
//     confidence; the confidence is clamp(1 - var/3000, 0.3, 0.95), so a flat
//     roof lands at the 0.95 ceiling).
//   • Roads — a low-saturation grey band (mean 40-180, sat < 40) that the colour
//     mask + skeletoniser turn into a centre-line polyline.
//   • Background — a saturated grass-green field that is neither (high sat, so it
//     is not road; one big region, so it is filtered out as a building by area).
// ─────────────────────────────────────────────────────────────────────────────

namespace {

struct RGBImage {
    int W, H;
    std::vector<uint8_t> px; // H*W*3, row-major RGB
    RGBImage(int w, int h, uint8_t r, uint8_t g, uint8_t b)
        : W(w), H(h), px((size_t)w * h * 3) {
        for (size_t i = 0; i < (size_t)W * H; ++i) {
            px[i*3] = r; px[i*3+1] = g; px[i*3+2] = b;
        }
    }
    // Fill the axis-aligned rectangle [x0,x1) × [y0,y1) with a solid colour.
    void rect(int x0, int y0, int x1, int y1, uint8_t r, uint8_t g, uint8_t b) {
        for (int y = std::max(0, y0); y < std::min(H, y1); ++y)
            for (int x = std::max(0, x0); x < std::min(W, x1); ++x) {
                size_t i = ((size_t)y * W + x) * 3;
                px[i] = r; px[i+1] = g; px[i+2] = b;
            }
    }
};

// Drive the async detector synchronously: post the work, block on the result.
ai::DetectionResult run_cv(const RGBImage& img,
                           double min_lat, double min_lon,
                           double max_lat, double max_lon,
                           double simplify_px = 3.5) {
    std::promise<ai::DetectionResult> prom;
    auto fut = prom.get_future();
    ai::detect_cv_on_imagery(img.px, img.W, img.H,
                             min_lat, min_lon, max_lat, max_lon,
                             [&prom](ai::DetectionResult r) {
                                 prom.set_value(std::move(r));
                             },
                             simplify_px);
    return fut.get();
}

ai::DetectionResult run_cv_dsm(const RGBImage& img,
                               const std::vector<uint8_t>& dsm,
                               double min_lat, double min_lon,
                               double max_lat, double max_lon,
                               double simplify_px = 3.5) {
    std::promise<ai::DetectionResult> prom;
    auto fut = prom.get_future();
    ai::detect_cv_with_dsm(img.px, dsm, img.W, img.H,
                           min_lat, min_lon, max_lat, max_lon,
                           [&prom](ai::DetectionResult r) {
                               prom.set_value(std::move(r));
                           },
                           simplify_px);
    return fut.get();
}

// A small geographic window over Łódź, PL (matches the manual-test region).
constexpr double MIN_LAT = 51.7400, MIN_LON = 19.4500;
constexpr double MAX_LAT = 51.7500, MAX_LON = 19.4600;

int count_type(const ai::DetectionResult& r, ai::FeatureType t) {
    return (int)std::count_if(r.features.begin(), r.features.end(),
                              [t](const ai::DetectedFeature& f) { return f.type == t; });
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("CV detects uniform rectangular buildings with high confidence",
          "[detection][cv][buildings]") {
    // 256×256 grass-green field with three solid reddish-roof rectangles.
    // Green (90,140,80): sat 60 ≥ 40 → not a road; one big region → not a
    // building (area filter). Roof (120,72,60): sat 60, uniform → var ≈ 0.
    RGBImage img(256, 256, 90, 140, 80);
    img.rect( 30,  30,  72,  62, 120, 72, 60); // 42×32
    img.rect(120,  40, 172,  92, 120, 72, 60); // 52×52
    img.rect( 60, 150, 104, 188, 120, 72, 60); // 44×38

    auto res = run_cv(img, MIN_LAT, MIN_LON, MAX_LAT, MAX_LON);

    INFO("error: " << res.error);
    REQUIRE(res.ok());

    int n_bldg = count_type(res, ai::FeatureType::Building);
    CHECK(n_bldg == 3);

    int high_conf_areas = 0;
    for (const auto& f : res.features) {
        if (f.type != ai::FeatureType::Building) continue;

        // Buildings are closed polygons tagged building=yes.
        CHECK(f.is_area);
        REQUIRE(f.suggested_tags.count("building") == 1);
        CHECK(f.suggested_tags.at("building") == "yes");
        CHECK(f.source == "cv-edge");

        // Uniform roof → very high confidence (clamped ceiling 0.95).
        CHECK(f.confidence >= 0.9f);

        // A simplified rectangle is a handful of nodes, not a noisy blob.
        CHECK(f.coords.size() >= 4);
        CHECK(f.coords.size() <= 12);

        // Every vertex must land inside the requested bbox — regression guard
        // for the tile-aligned-extent drift fix (features used to skew north).
        for (const auto& [lat, lon] : f.coords) {
            CHECK(lat >= MIN_LAT - 1e-6);
            CHECK(lat <= MAX_LAT + 1e-6);
            CHECK(lon >= MIN_LON - 1e-6);
            CHECK(lon <= MAX_LON + 1e-6);
        }
        ++high_conf_areas;
    }
    CHECK(high_conf_areas == 3);
}

TEST_CASE("CV detects a grey road as a polyline", "[detection][cv][roads]") {
    // Grass-green field with one grey L-shaped road. Grey (120,120,120): sat 0,
    // mean 120 → road mask; ≈ same luma as the green background, so it makes no
    // Sobel edge and is not mistaken for a building. The road has a bend: a
    // dead-straight band simplifies to 2 points and is rejected (roads need ≥3),
    // and real roads turn anyway. Inset from the borders so the Zhang-Suen thinner
    // (which skips the image edge) yields a clean centre-line.
    RGBImage img(256, 256, 90, 140, 80);
    img.rect( 20, 122, 180, 128, 120, 120, 120); // horizontal leg
    img.rect(174, 122, 180, 200, 120, 120, 120); // vertical leg → "L"

    auto res = run_cv(img, MIN_LAT, MIN_LON, MAX_LAT, MAX_LON);

    INFO("error: " << res.error);
    REQUIRE(res.ok());

    int n_road = count_type(res, ai::FeatureType::Road);
    CHECK(n_road >= 1);

    bool found_spanning_road = false;
    for (const auto& f : res.features) {
        if (f.type != ai::FeatureType::Road) continue;
        CHECK_FALSE(f.is_area);
        REQUIRE(f.suggested_tags.count("highway") == 1);
        CHECK(f.coords.size() >= 2);

        // The centre-line should track the band's latitude (image row ~125 of
        // 256) and span a good fraction of the width.
        double lon_lo = 1e9, lon_hi = -1e9;
        for (const auto& [lat, lon] : f.coords) {
            CHECK(lat >= MIN_LAT - 1e-6);
            CHECK(lat <= MAX_LAT + 1e-6);
            lon_lo = std::min(lon_lo, lon);
            lon_hi = std::max(lon_hi, lon);
        }
        if (lon_hi - lon_lo > 0.5 * (MAX_LON - MIN_LON))
            found_spanning_road = true;
    }
    CHECK(found_spanning_road);
}

TEST_CASE("CV finds both buildings and a road in one scene",
          "[detection][cv][mixed]") {
    RGBImage img(256, 256, 90, 140, 80);
    img.rect( 30,  30,  72,  62, 120, 72, 60);
    img.rect(150,  40, 200,  90, 120, 72, 60);
    img.rect( 20, 122, 180, 128, 120, 120, 120); // L-shaped road, horizontal leg
    img.rect(174, 122, 180, 200, 120, 120, 120); // vertical leg
    img.rect( 50, 200, 110, 240, 120, 72, 60);

    auto res = run_cv(img, MIN_LAT, MIN_LON, MAX_LAT, MAX_LON);

    INFO("error: " << res.error);
    REQUIRE(res.ok());
    CHECK(count_type(res, ai::FeatureType::Building) == 3);
    CHECK(count_type(res, ai::FeatureType::Road) >= 1);
}

TEST_CASE("CV rejects a sprawling low-solidity blob (not a building)",
          "[detection][cv][falsepos]") {
    // A reddish plus/cross fills only ~14% of its bounding box. This is the shape
    // class that used to be mis-traced as a spider-shaped "building" on real
    // imagery (driveways, hedges, shadows). The solidity filter must reject it.
    RGBImage img(256, 256, 90, 140, 80);
    img.rect(120,  40, 132, 200, 120, 72, 60); // vertical bar
    img.rect( 40, 120, 200, 132, 120, 72, 60); // horizontal bar → "+"

    auto res = run_cv(img, MIN_LAT, MIN_LON, MAX_LAT, MAX_LON);
    // It is large, low-variance and within the aspect limit, so only the solidity
    // (extent) filter stops it — exactly the regression we care about.
    CHECK(count_type(res, ai::FeatureType::Building) == 0);
}

TEST_CASE("Higher simplify tolerance yields fewer or equal nodes",
          "[detection][cv][simplify]") {
    RGBImage img(256, 256, 90, 140, 80);
    img.rect(60, 60, 180, 180, 120, 72, 60); // one large building

    auto fine   = run_cv(img, MIN_LAT, MIN_LON, MAX_LAT, MAX_LON, /*simplify*/ 1.5);
    auto coarse = run_cv(img, MIN_LAT, MIN_LON, MAX_LAT, MAX_LON, /*simplify*/ 10.0);

    REQUIRE(fine.ok());
    REQUIRE(coarse.ok());
    REQUIRE(count_type(fine,   ai::FeatureType::Building) >= 1);
    REQUIRE(count_type(coarse, ai::FeatureType::Building) >= 1);

    auto max_nodes = [](const ai::DetectionResult& r) {
        size_t m = 0;
        for (const auto& f : r.features)
            if (f.type == ai::FeatureType::Building) m = std::max(m, f.coords.size());
        return m;
    };
    CHECK(max_nodes(coarse) <= max_nodes(fine));
}

TEST_CASE("CV+LiDAR boosts confidence when DSM elevation agrees",
          "[detection][cv][lidar]") {
    // Same building scene; the DSM marks the building footprints as elevated
    // (bright) over flat (dark) ground, so edge + height evidence agree and the
    // confidence climbs above the RGB-only ceiling.
    RGBImage img(256, 256, 90, 140, 80);
    struct Box { int x0, y0, x1, y1; };
    std::vector<Box> boxes = {{30,30,72,62}, {120,40,172,92}, {60,150,104,188}};
    for (auto& b : boxes) img.rect(b.x0, b.y0, b.x1, b.y1, 120, 72, 60);

    std::vector<uint8_t> dsm((size_t)img.W * img.H, 30); // flat ground = dark
    for (auto& b : boxes)
        for (int y = b.y0; y < b.y1; ++y)
            for (int x = b.x0; x < b.x1; ++x)
                dsm[(size_t)y * img.W + x] = 210;        // elevated = bright

    auto res = run_cv_dsm(img, dsm, MIN_LAT, MIN_LON, MAX_LAT, MAX_LON);

    INFO("error: " << res.error);
    REQUIRE(res.ok());
    REQUIRE(count_type(res, ai::FeatureType::Building) >= 1);

    bool any_very_high = false;
    for (const auto& f : res.features) {
        if (f.type != ai::FeatureType::Building) continue;
        CHECK(f.is_area);
        CHECK(f.source == "cv-lidar");
        if (f.confidence >= 0.95f) any_very_high = true;
    }
    CHECK(any_very_high);
}

TEST_CASE("CV reports an error on blank imagery", "[detection][cv][empty]") {
    RGBImage img(128, 128, 90, 140, 80); // uniform field, nothing to find
    auto res = run_cv(img, MIN_LAT, MIN_LON, MAX_LAT, MAX_LON);
    CHECK_FALSE(res.ok());
    CHECK(res.features.empty());
}
