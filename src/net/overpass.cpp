#include "overpass.hpp"
#include "http.hpp"
#include "../osm/parser.hpp"

#include <curl/curl.h>
#include <pugixml.hpp>
#include <format>
#include <filesystem>
#include <fstream>

namespace net {

std::string bbox_query(double min_lat, double min_lon, double max_lat, double max_lon,
                       const std::string& filter) {
    // e.g. [out:xml][timeout:60]; (node(bbox); way(bbox); relation(bbox);); (._;>;); out body;
    std::string f = filter.empty() ? "" : filter;
    return std::format(
        "[out:xml][timeout:60];\n"
        "(\n"
        "  node{}({:.6f},{:.6f},{:.6f},{:.6f});\n"
        "  way{}({:.6f},{:.6f},{:.6f},{:.6f});\n"
        "  relation{}({:.6f},{:.6f},{:.6f},{:.6f});\n"
        ");\n"
        "(._;>;);\n"
        "out body;\n",
        f, min_lat, min_lon, max_lat, max_lon,
        f, min_lat, min_lon, max_lat, max_lon,
        f, min_lat, min_lon, max_lat, max_lon);
}

void query_overpass_async(
    const std::string& ql,
    std::function<void(std::expected<osm::Dataset, OverpassError>)> cb,
    const std::string& endpoint)
{
    CURL* eh = curl_easy_init();
    char* escaped = curl_easy_escape(eh, ql.c_str(), (int)ql.size());
    std::string url = endpoint + "?data=" + (escaped ? escaped : "");
    curl_free(escaped);
    if (eh) curl_easy_cleanup(eh);
    fetch_tile_async(
        url,
        [cb = std::move(cb)](std::expected<Response, HttpError> resp) {
            if (!resp) { cb(std::unexpected(OverpassError::NetworkError)); return; }

            // Write to a temp file and reuse osm::load_osm
            auto tmp = std::filesystem::temp_directory_path() / "cpposmui_overpass.osm";
            {
                std::ofstream f(tmp, std::ios::binary);
                f.write(reinterpret_cast<const char*>(resp->bytes.data()), (std::streamsize)resp->bytes.size());
            }
            auto ds = osm::load_osm(tmp);
            std::filesystem::remove(tmp);

            if (ds) cb(std::move(*ds));
            else    cb(std::unexpected(OverpassError::ParseError));
        },
        90);
}

void download_osm_bbox_async(
    double min_lat, double min_lon, double max_lat, double max_lon,
    std::function<void(std::expected<osm::Dataset, OverpassError>)> cb,
    const std::string& api_base)
{
    // OSM API expects bbox = left,bottom,right,top = min_lon,min_lat,max_lon,max_lat
    std::string url = std::format("{}/api/0.6/map?bbox={:.6f},{:.6f},{:.6f},{:.6f}",
                                  api_base, min_lon, min_lat, max_lon, max_lat);
    fetch_tile_async(url,
        [cb = std::move(cb)](std::expected<Response, HttpError> resp) {
            if (!resp) { cb(std::unexpected(OverpassError::NetworkError)); return; }

            auto tmp = std::filesystem::temp_directory_path() / "cpposmui_osmapi.osm";
            {
                std::ofstream f(tmp, std::ios::binary);
                f.write(reinterpret_cast<const char*>(resp->bytes.data()),
                        (std::streamsize)resp->bytes.size());
            }
            auto ds = osm::load_osm(tmp);
            std::filesystem::remove(tmp);

            if (ds) cb(std::move(*ds));
            else    cb(std::unexpected(OverpassError::ParseError));
        },
        90);
}

} // namespace net
