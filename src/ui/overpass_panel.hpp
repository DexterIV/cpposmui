#pragma once
#include "../osm/types.hpp"
#include <functional>
#include <string>
#include <optional>

namespace ui {

class OverpassPanel {
public:
    using OnResult = std::function<void(osm::Dataset)>;

    explicit OverpassPanel(OnResult cb) : on_result_(std::move(cb)) {}

    // Set the current map bbox so it can be pre-filled
    void set_bbox(double min_lat, double min_lon, double max_lat, double max_lon);

    void draw(bool* p_open = nullptr);

private:
    OnResult on_result_;
    std::string ql_buf_;
    std::string endpoint_buf_{"https://overpass-api.de/api/interpreter"};
    bool loading_{false};
    std::string status_;

    double min_lat_{}, min_lon_{}, max_lat_{}, max_lon_{};

    void fire_query();
};

} // namespace ui
