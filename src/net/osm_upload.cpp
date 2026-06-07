#include "osm_upload.hpp"
#include "http.hpp"
#include "../osm/parser.hpp"

#include <format>
#include <string>
#include <thread>
#include <utility>

namespace net {

namespace {

std::string xml_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '&':  out += "&amp;";  break;
        case '<':  out += "&lt;";   break;
        case '>':  out += "&gt;";   break;
        case '"':  out += "&quot;"; break;
        case '\'': out += "&apos;"; break;
        default:   out += c;        break;
        }
    }
    return out;
}

} // anonymous namespace

void upload_changeset_async(
    const std::string& api_base, const std::string& token, const std::string& comment,
    const osm::ChangeSet& cs, std::function<void(UploadResult)> cb)
{
    std::thread([api_base, token, comment, cs, cb = std::move(cb)]() mutable {
        UploadResult r;
        // Trim leading/trailing whitespace from the token — copy-paste often adds newlines.
        std::string tok = token;
        auto is_ws = [](char c){ return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
        while (!tok.empty() && is_ws(tok.front())) tok.erase(tok.begin());
        while (!tok.empty() && is_ws(tok.back()))  tok.pop_back();
        if (tok.empty()) { r.message = "Token is empty — log in first."; cb(r); return; }

        const std::pair<std::string, std::string> auth{"Authorization", "Bearer " + tok};
        const std::pair<std::string, std::string> ctype{"Content-Type", "text/xml"};

        // 1. Open a changeset.
        std::string create_body = std::format(
            "<osm><changeset>"
            "<tag k=\"created_by\" v=\"cpposmui/0.1\"/>"
            "<tag k=\"comment\" v=\"{}\"/>"
            "</changeset></osm>",
            xml_escape(comment));
        auto created = net::request("PUT", api_base + "/api/0.6/changeset/create",
                                    create_body, {auth, ctype});
        if (!created) { r.message = "Network error creating changeset."; cb(r); return; }
        if (created->status_code != 200) {
            r.message = std::format("Create failed (HTTP {}). {}",
                                    created->status_code, created->body);
            cb(r); return;
        }
        long long cs_id = 0;
        try { cs_id = std::stoll(created->body); }
        catch (...) { r.message = "Could not parse changeset id from server."; cb(r); return; }
        r.changeset_id = cs_id;

        // 2. Upload the diff (changeset id stamped on every element).
        std::string osc = osm::changeset_xml(cs, cs_id);
        auto uploaded = net::request(
            "POST", std::format("{}/api/0.6/changeset/{}/upload", api_base, cs_id),
            osc, {auth, ctype});

        if (!uploaded || uploaded->status_code != 200) {
            // Best-effort close so we don't leave an open changeset behind.
            net::request("PUT", std::format("{}/api/0.6/changeset/{}/close", api_base, cs_id),
                         "", {auth});
            r.message = uploaded
                ? std::format("Upload rejected (HTTP {}). {}", uploaded->status_code, uploaded->body)
                : std::string("Network error during upload.");
            cb(r); return;
        }

        // 3. Close the changeset.
        net::request("PUT", std::format("{}/api/0.6/changeset/{}/close", api_base, cs_id),
                     "", {auth});

        r.ok = true;
        r.message = std::format("Uploaded successfully — changeset #{}.", cs_id);
        cb(r);
    }).detach();
}

} // namespace net
