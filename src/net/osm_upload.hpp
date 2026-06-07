#pragma once
#include "../osm/types.hpp"
#include <string>
#include <functional>

namespace net {

struct UploadResult {
    bool        ok{false};
    std::string message;
    long long   changeset_id{0};
};

// Upload a ChangeSet to an OSM API server using an OAuth2 bearer token, running
// the full create → upload → close flow. Runs on a detached worker thread; the
// callback fires on that thread (marshal to the UI thread yourself).
//
// api_base examples:
//   https://master.apis.dev.openstreetmap.org   (dev sandbox — safe for testing)
//   https://api.openstreetmap.org               (LIVE — real edits!)
void upload_changeset_async(
    const std::string& api_base,
    const std::string& token,
    const std::string& comment,
    const osm::ChangeSet& cs,
    std::function<void(UploadResult)> cb);

} // namespace net
