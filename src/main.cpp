#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <misc/cpp/imgui_stdlib.h>
#define GLFW_INCLUDE_NONE          // let glad provide the GL headers
#include <glad/gl.h>
#include <GLFW/glfw3.h>

#include "osm/parser.hpp"
#include "osm/diff.hpp"
#include "ui/map_view.hpp"
#include "ui/diff_panel.hpp"
#include "ui/tag_editor.hpp"
#include "ui/overpass_panel.hpp"
#include "ui/detection_panel.hpp"
#include "net/http.hpp"
#include "net/overpass.hpp"
#include "net/osm_upload.hpp"
#include "net/oauth.hpp"
#include "osm/data_layer.hpp"
#include "log.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cctype>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <fstream>
#include <filesystem>

// ── Persistent view settings ──────────────────────────────────────────────────
// Default center: Łódź, Poland.
// Łódź — the application's built-in default view.
static constexpr double LODZ_LAT  = 51.7592;
static constexpr double LODZ_LON  = 19.4560;
static constexpr int    LODZ_ZOOM = 13;

struct ViewConfig {
    double lat{LODZ_LAT};
    double lon{LODZ_LON};
    int    zoom{LODZ_ZOOM};
    int    source{0};   // base layer index (tile_source_catalog)
    int    overlay{0};  // overlay layer index (0 = none)
    // Per-catalog-index API keys (sparse; only store non-empty ones)
    std::unordered_map<int, std::string> tile_keys;
};

// Persisted upload/login settings.
struct UploadPrefs {
    int         server{0};       // 0 = dev sandbox, 1 = live OSM
    std::string client_id;
    std::string client_secret;   // empty for public apps
    std::string token;           // OAuth2 access token (stays logged in)
    std::string comment{"Edited with cpposmui"};
};

static std::filesystem::path config_path() {
    namespace fs = std::filesystem;
    const char* appdata = std::getenv("APPDATA");
    fs::path dir = appdata ? fs::path(appdata) / "cpposmui"
                           : fs::current_path();
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir / "config.json";
}

static nlohmann::json j_vec4(const ImVec4& v) { return nlohmann::json::array({v.x, v.y, v.z, v.w}); }
static ImVec4 r_vec4(const nlohmann::json& j, const ImVec4& def) {
    if (j.is_array() && j.size() == 4)
        return ImVec4(j[0].get<float>(), j[1].get<float>(), j[2].get<float>(), j[3].get<float>());
    return def;
}

static ViewConfig load_config(ui::MapStyle& style, UploadPrefs& up) {
    ViewConfig c;
    std::ifstream f(config_path());
    if (!f) return c;
    try {
        nlohmann::json j; f >> j;
        c.lat     = j.value("lat",     c.lat);
        c.lon     = j.value("lon",     c.lon);
        c.zoom    = j.value("zoom",    c.zoom);
        c.source  = j.value("source",  c.source);
        c.overlay = j.value("overlay", c.overlay);
        if (j.contains("tile_keys") && j["tile_keys"].is_object()) {
            for (const auto& [k, v] : j["tile_keys"].items())
                if (v.is_string()) c.tile_keys[std::stoi(k)] = v.get<std::string>();
        }
        if (j.contains("upload")) {
            const auto& u = j["upload"];
            up.server    = u.value("server",    up.server);
            up.client_id     = u.value("client_id",     up.client_id);
            up.client_secret = u.value("client_secret", up.client_secret);
            up.token     = u.value("token",     up.token);
            up.comment   = u.value("comment",   up.comment);
        }
        if (j.contains("style")) {
            const auto& s = j["style"];
            style.node_color    = r_vec4(s.value("node_color",   nlohmann::json{}), style.node_color);
            style.node_tagged   = r_vec4(s.value("node_tagged",  nlohmann::json{}), style.node_tagged);
            style.node_radius   = s.value("node_radius",   style.node_radius);
            style.node_min_zoom = s.value("node_min_zoom", style.node_min_zoom);
            style.way_color     = r_vec4(s.value("way_color",    nlohmann::json{}), style.way_color);
            style.way_width     = s.value("way_width",     style.way_width);
            style.area_fill     = r_vec4(s.value("area_fill",    nlohmann::json{}), style.area_fill);
            style.area_outline  = r_vec4(s.value("area_outline", nlohmann::json{}), style.area_outline);
            style.area_width    = s.value("area_width",    style.area_width);
            style.area_opacity  = s.value("area_opacity",  style.area_opacity);
            style.show_poi_icons= s.value("show_poi_icons",style.show_poi_icons);
            style.show_labels   = s.value("show_labels",   style.show_labels);
            style.label_min_zoom= s.value("label_min_zoom",style.label_min_zoom);
            style.random_colors = s.value("random_colors", style.random_colors);
            style.road_casing   = s.value("road_casing",   style.road_casing);
        }
    } catch (...) { /* keep defaults on malformed config */ }
    return c;
}

static void save_config(const ViewConfig& c, const ui::MapStyle& style, const UploadPrefs& up) {
    nlohmann::json tile_keys_j = nlohmann::json::object();
    for (const auto& [idx, key] : c.tile_keys)
        if (!key.empty()) tile_keys_j[std::to_string(idx)] = key;

    nlohmann::json j{
        {"lat", c.lat}, {"lon", c.lon}, {"zoom", c.zoom},
        {"source", c.source}, {"overlay", c.overlay},
        {"tile_keys", tile_keys_j},
        {"upload", {
            {"server", up.server}, {"client_id", up.client_id},
            {"client_secret", up.client_secret},
            {"token", up.token}, {"comment", up.comment},
        }},
        {"style", {
            {"node_color",    j_vec4(style.node_color)},
            {"node_tagged",   j_vec4(style.node_tagged)},
            {"node_radius",   style.node_radius},
            {"node_min_zoom", style.node_min_zoom},
            {"way_color",     j_vec4(style.way_color)},
            {"way_width",     style.way_width},
            {"area_fill",     j_vec4(style.area_fill)},
            {"area_outline",  j_vec4(style.area_outline)},
            {"area_width",    style.area_width},
            {"area_opacity",  style.area_opacity},
            {"show_poi_icons",style.show_poi_icons},
            {"show_labels",   style.show_labels},
            {"label_min_zoom",style.label_min_zoom},
            {"random_colors", style.random_colors},
            {"road_casing",   style.road_casing},
        }},
    };
    std::ofstream f(config_path());
    if (f) f << j.dump(2);
}

// ── Native "open file" dialog ─────────────────────────────────────────────────
#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
// `filter` is a Win32 double-null-terminated pair list, e.g.
//   "OSM data (*.osm)\0*.osm\0All files (*.*)\0*.*\0"
static std::optional<std::string> open_file_dialog(const char* title, const char* filter) {
    char buf[1024] = {0};
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = nullptr;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = sizeof(buf);
    ofn.lpstrTitle  = title;
    ofn.Flags       = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&ofn)) return std::string(buf);
    return std::nullopt;
}
static std::optional<std::string> save_file_dialog(const char* title, const char* filter,
                                                   const char* default_ext) {
    char buf[1024] = {0};
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = nullptr;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = sizeof(buf);
    ofn.lpstrTitle  = title;
    ofn.lpstrDefExt = default_ext;
    ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetSaveFileNameA(&ofn)) return std::string(buf);
    return std::nullopt;
}
#else
static std::optional<std::string> open_file_dialog(const char*, const char*) {
    return std::nullopt; // no native dialog; the text-input popup is used instead
}
static std::optional<std::string> save_file_dialog(const char*, const char*, const char*) {
    return std::nullopt;
}
#endif

// ── Global state ──────────────────────────────────────────────────────────────
struct AppState {
    // ── Layers (JOSM-style) ───────────────────────────────────────────────────
    std::vector<osm::DataLayer> layers_;
    int                         active_{-1};   // index into layers_

    // Changeset for the Diff panel (Overpass diff or .osc import).
    osm::ChangeSet changeset_;
    bool           has_changeset_{false};

    ui::MapView       map_view;
    ui::DiffPanel     diff_panel;
    ui::TagEditor     tag_editor;
    std::unique_ptr<ui::OverpassPanel> overpass_panel;
    ui::DetectionPanel detection_panel;

    // Panel visibility.
    bool show_selection_{true};
    bool show_layers_{true};
    bool show_diff_{false};
    bool show_overpass_{false};
    bool show_upload_{false};
    bool show_style_{false};
    bool show_detection_{false};
    bool show_goto_{false};
    bool show_settings_{false};

    // Upload UI state.
    int  upload_server_{0};
    std::string upload_comment_{"Edited with cpposmui"};
    std::string upload_token_{};
    std::string upload_client_id_{};
    std::string upload_client_secret_{}; // empty for public apps
    std::string upload_status_;
    std::optional<std::string> pending_token_;

    // Async download slot.
    std::mutex                  pending_mu_;
    std::optional<osm::Dataset> pending_download_;
    std::string                 pending_layer_name_;
    std::string                 download_status_;

    // ── Helpers ───────────────────────────────────────────────────────────────
    bool        has_active() const { return active_ >= 0 && active_ < (int)layers_.size(); }
    osm::DataLayer& active_layer()  { return layers_[active_]; }

    // Commit a freshly-loaded dataset as a new layer and make it active.
    void push_layer(osm::Dataset ds, std::string name) {
        osm::DataLayer L;
        L.name     = std::move(name);
        L.data     = ds;
        L.baseline = std::move(ds);
        L.visible  = true;
        L.modified = false;
        layers_.push_back(std::move(L));
        active_ = (int)layers_.size() - 1;
        sync_map_view();
    }

    // Sync MapView after any layer change.
    void sync_map_view() {
        if (!has_active()) {
            map_view.set_dataset(nullptr);
            map_view.set_editable_dataset(nullptr);
            map_view.set_extra_layers({});
            return;
        }
        auto& L = active_layer();
        map_view.set_dataset(&L.data);
        map_view.set_editable_dataset(&L.data);
        // Extra layers = all visible non-active layers.
        std::vector<const osm::Dataset*> extras;
        for (int i = 0; i < (int)layers_.size(); ++i)
            if (i != active_ && layers_[i].visible)
                extras.push_back(&layers_[i].data);
        map_view.set_extra_layers(std::move(extras));
        if (!L.data.nodes.empty()) {
            overpass_panel->set_bbox(L.data.min_lat, L.data.min_lon,
                                     L.data.max_lat, L.data.max_lon);
            detection_panel.set_bbox(L.data.min_lat, L.data.min_lon,
                                     L.data.max_lat, L.data.max_lon);
        }
    }

    // Layer operations.
    void new_layer(std::string name = "New layer") {
        osm::DataLayer L;
        L.name = std::move(name);
        layers_.push_back(std::move(L));
        active_ = (int)layers_.size() - 1;
        sync_map_view();
    }
    void delete_layer(int idx) {
        if (idx < 0 || idx >= (int)layers_.size()) return;
        layers_.erase(layers_.begin() + idx);
        active_ = layers_.empty() ? -1 : std::min(active_, (int)layers_.size() - 1);
        sync_map_view();
    }
    // Merge src into dst (union of nodes/ways/relations, src baseline is discarded).
    void merge_layers(int dst, int src) {
        if (dst == src || dst < 0 || src < 0 ||
            dst >= (int)layers_.size() || src >= (int)layers_.size()) return;
        auto& D = layers_[dst];
        auto& S = layers_[src];
        for (auto& [id, n] : S.data.nodes)    D.data.nodes[id] = n;
        for (auto& [id, w] : S.data.ways)     D.data.ways[id]  = w;
        for (auto& [id, r] : S.data.relations) D.data.relations[id] = r;
        D.modified = true;
        layers_.erase(layers_.begin() + src);
        if (active_ >= src) --active_;
        active_ = std::clamp(active_, 0, (int)layers_.size() - 1);
        sync_map_view();
    }
    void set_active(int idx) {
        if (idx < 0 || idx >= (int)layers_.size()) return;
        active_ = idx;
        sync_map_view();
    }

    AppState() {
        map_view.set_edit_callback([this] {
            // Mark active layer modified; rebuild spatial index.
            if (has_active()) active_layer().modified = true;
            if (has_active()) map_view.set_dataset(&active_layer().data);
        });

        // Wire detection panel → map overlay + accept → add to active layer.
        map_view.set_detection_panel(&detection_panel);
        detection_panel.set_accept_callback(
            [this](std::vector<ai::DetectedFeature> feats) {
                if (!has_active()) return;
                active_layer().modified = true;
                map_view.add_detected_features(feats);
            });

        overpass_panel = std::make_unique<ui::OverpassPanel>(
            [this](osm::Dataset ds) {
                if (!has_active()) return;
                auto& L = active_layer();
                changeset_ = osm::diff::diff_datasets(L.data, ds);
                has_changeset_ = true;
                diff_panel.set_changeset(&changeset_);
                map_view.set_changeset(&changeset_);
                show_diff_ = true;
            });
    }

    void download_bbox(double min_lat, double min_lon, double max_lat, double max_lon) {
        double area = (max_lat - min_lat) * (max_lon - min_lon);
        if (area > 0.25) {
            std::scoped_lock lk(pending_mu_);
            download_status_ = "Area too large — zoom in (OSM API limit 0.25 deg²)";
            return;
        }
        {
            std::scoped_lock lk(pending_mu_);
            download_status_ = "Downloading OSM data…";
            pending_layer_name_ = std::format("OSM {:.4f},{:.4f}", min_lat, min_lon);
        }
        net::download_osm_bbox_async(min_lat, min_lon, max_lat, max_lon,
            [this](std::expected<osm::Dataset, net::OverpassError> res) {
                std::scoped_lock lk(pending_mu_);
                if (!res) { download_status_ = "Download failed"; return; }
                download_status_ = std::format("Loaded {} nodes, {} ways",
                    res->nodes.size(), res->ways.size());
                pending_download_ = std::move(*res);
            });
    }

    bool export_osc(const char* path) {
        if (!has_active()) return false;
        auto& L = active_layer();
        auto cs = osm::diff::diff_datasets(L.baseline, L.data);
        return osm::write_osc(cs, path);
    }

    void upload_layer(int idx) {
        if (idx < 0 || idx >= (int)layers_.size()) return;
        auto& L = layers_[idx];
        auto cs = osm::diff::diff_datasets(L.baseline, L.data);
        if (cs.nodes.empty() && cs.ways.empty() && cs.relations.empty()) {
            std::scoped_lock lk(pending_mu_); upload_status_ = "No edits to upload."; return;
        }
        const char* api = (upload_server_ == 1) ? "https://api.openstreetmap.org"
                                                : "https://master.apis.dev.openstreetmap.org";
        { std::scoped_lock lk(pending_mu_); upload_status_ = "Uploading…"; }
        net::upload_changeset_async(api, upload_token_, upload_comment_, cs,
            [this, idx](net::UploadResult res) {
                std::scoped_lock lk(pending_mu_);
                upload_status_ = res.message;
                if (res.ok && idx < (int)layers_.size())
                    layers_[idx].modified = false;
            });
    }

    void oauth_login() {
        net::OAuthConfig oc;
        oc.web_base  = (upload_server_ == 1) ? "https://www.openstreetmap.org"
                                             : "https://master.apis.dev.openstreetmap.org";
        oc.client_id     = upload_client_id_;
        oc.client_secret = upload_client_secret_;
        { std::scoped_lock lk(pending_mu_); upload_status_ = "Opening browser for OSM login…"; }
        net::oauth2_login_async(oc, [this](std::string token, std::string msg) {
            std::scoped_lock lk(pending_mu_);
            if (!token.empty()) pending_token_ = std::move(token);
            upload_status_ = msg;
        });
    }

    void apply_pending() {
        std::optional<osm::Dataset> ds;
        std::string layer_name, status;
        {
            std::scoped_lock lk(pending_mu_);
            status     = download_status_;
            layer_name = pending_layer_name_;
            if (pending_download_) { ds = std::move(pending_download_); pending_download_.reset(); }
            if (pending_token_) {
                upload_token_ = *pending_token_;
                pending_token_.reset();
            }
        }
        map_view.set_status(status);
        if (ds) push_layer(std::move(*ds), layer_name);
    }

    void load_osm_file(const char* path) {
        auto ds = osm::load_osm(path);
        if (!ds) { fprintf(stderr, "Failed to load OSM: %s\n", path); return; }
        std::filesystem::path fp(path);
        push_layer(std::move(*ds), fp.filename().string());
        if (has_active()) map_view.center_on_dataset(active_layer().data);
    }

    void load_osc_file(const char* path) {
        auto cs = osm::load_osc(path);
        if (!cs) { fprintf(stderr, "Failed to load OSC: %s\n", path); return; }
        changeset_ = std::move(*cs);
        if (has_active()) osm::diff::enrich_changeset(changeset_, active_layer().data);
        has_changeset_ = true;
        diff_panel.set_changeset(&changeset_);
        if (has_active()) map_view.set_changeset(&changeset_);
        show_diff_ = true;
    }
};

// Write a given view (lat/lon/zoom) plus the current layers/keys/style/login to
// config.json.  The non-view settings always come from the live app state.
static void persist_config_view(AppState& app, double lat, double lon, int zoom) {
    std::unordered_map<int, std::string> saved_keys;
    for (int i = 0; i < 32; ++i) {
        auto k = app.map_view.tile_key(i);
        if (!k.empty()) saved_keys[i] = std::move(k);
    }
    save_config(ViewConfig{lat, lon, zoom,
                    app.map_view.tile_source_index(),
                    app.map_view.overlay_source_index(),
                    std::move(saved_keys)},
                app.map_view.style(),
                UploadPrefs{app.upload_server_, app.upload_client_id_,
                            app.upload_client_secret_,
                            app.upload_token_, app.upload_comment_});
}

// Save the current (live) map view as the startup default. Used by the exit-time
// save and the "Save current view" button.
static void persist_config(AppState& app) {
    persist_config_view(app, app.map_view.center_lat(),
                        app.map_view.center_lon(), app.map_view.zoom());
}

// Percent-encode a string for use in a URL query.
static std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out += (char)c;
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 0xF]; }
    }
    return out;
}

// A single geocoding candidate.
struct GeoMatch { double lat{0}, lon{0}; std::string name; };

// Thread-safe holder for an async geocode lookup (filled on a worker thread,
// consumed on the main/UI thread).
struct GeocodeResult {
    std::mutex             mu;
    bool                   running{false};
    bool                   ready{false};   // results (or error) are waiting
    std::vector<GeoMatch>  matches;
    std::string            error;          // non-empty → lookup failed / no hits
};

// Look up a place name via OSM Nominatim and store up to `limit` matches in `out`.
static void geocode_async(const std::string& query, GeocodeResult* out, int limit = 8) {
    {
        std::scoped_lock lk(out->mu);
        if (out->running) return;
        out->running = true; out->ready = false;
    }
    std::string url = "https://nominatim.openstreetmap.org/search?format=json"
                      "&limit=" + std::to_string(limit) + "&q=" + url_encode(query);
    net::post_async([url, out] {
        // Nominatim's usage policy requires an identifying User-Agent.
        auto resp = net::request("GET", url, "",
            {{"User-Agent", "cpposmui/0.1 (OSM editing tool)"}}, 20);
        std::scoped_lock lk(out->mu);
        out->running = false; out->ready = true;
        out->matches.clear(); out->error.clear();
        if (!resp || resp->status_code != 200) {
            out->error = "Geocoding request failed"; return;
        }
        try {
            auto j = nlohmann::json::parse(resp->body, nullptr, false);
            if (j.is_discarded() || !j.is_array() || j.empty()) {
                out->error = "No match found"; return;
            }
            for (const auto& e : j) {
                GeoMatch m;
                m.lat  = std::stod(e.value("lat", std::string("0")));
                m.lon  = std::stod(e.value("lon", std::string("0")));
                m.name = e.value("display_name", std::string("(match)"));
                out->matches.push_back(std::move(m));
            }
        } catch (...) {
            out->error = "Could not parse geocoding response";
        }
    });
}

// ── Go-to-coordinate / save-default panel ───────────────────────────────────────
static void draw_goto(AppState& app, bool* p_open) {
    ImGui::SetNextWindowSize(ImVec2(340, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Go to coordinates", p_open)) { ImGui::End(); return; }

    static double lat  = LODZ_LAT;
    static double lon  = LODZ_LON;
    static int    zoom = LODZ_ZOOM;
    static std::string place;
    static std::string status;
    static GeocodeResult geo;
    static std::vector<GeoMatch> matches; // main-thread copy of the last results
    static int matches_sel = -1;

    // Apply a finished geocode lookup (filled on a worker thread).
    {
        std::scoped_lock lk(geo.mu);
        if (geo.ready) {
            geo.ready = false;
            matches = geo.matches;          // copy out for lock-free rendering
            matches_sel = -1;
            if (!geo.error.empty()) {
                status = geo.error;
            } else if (!matches.empty()) {
                status = std::format("{} match{} — pick one below",
                    matches.size(), matches.size() == 1 ? "" : "es");
            }
        }
    }

    // ── Manual entry ──────────────────────────────────────────────────────────
    ImGui::SeparatorText("Location");
    ImGui::InputDouble("Lat", &lat, 0.0, 0.0, "%.6f");
    ImGui::InputDouble("Lon", &lon, 0.0, 0.0, "%.6f");
    ImGui::SliderInt("Zoom", &zoom, 2, 21);

    // Place-name search (OSM Nominatim) — fills the lat/lon fields.
    bool searching;
    { std::scoped_lock lk(geo.mu); searching = geo.running; }
    ImGui::SetNextItemWidth(-72);
    bool submit = ImGui::InputText("Place", &place,
                                   ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    ImGui::BeginDisabled(searching || place.empty());
    if (ImGui::Button("Find", ImVec2(64, 0)) || submit) {
        status = "Searching…";
        geocode_async(place, &geo);
    }
    ImGui::EndDisabled();
    if (searching) { ImGui::SameLine(); ImGui::TextDisabled("(searching…)"); }

    // Matches list — shown when a search returned candidates.
    if (!matches.empty()) {
        float list_h = std::min((int)matches.size(), 6) * ImGui::GetTextLineHeightWithSpacing()
                       + ImGui::GetStyle().FramePadding.y * 2;
        if (ImGui::BeginListBox("##matches", ImVec2(-1, list_h))) {
            for (int i = 0; i < (int)matches.size(); ++i) {
                bool sel = (i == matches_sel);
                if (ImGui::Selectable(matches[i].name.c_str(), sel)) {
                    matches_sel = i;
                    lat = matches[i].lat;
                    lon = matches[i].lon;
                    status = "Selected: " + matches[i].name;
                }
                if (sel) ImGui::SetItemDefaultFocus();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%.6f, %.6f", matches[i].lat, matches[i].lon);
            }
            ImGui::EndListBox();
        }
    }

    if (ImGui::Button("Center here")) {
        app.map_view.set_view(lat, lon, zoom);
        status.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy current view")) {
        lat  = app.map_view.center_lat();
        lon  = app.map_view.center_lon();
        zoom = app.map_view.zoom();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Copy the live map position/zoom into the fields above");

    ImGui::Text("Current: %.6f, %.6f  z%d",
                app.map_view.center_lat(), app.map_view.center_lon(),
                app.map_view.zoom());

    // ── Startup default ─────────────────────────────────────────────────────────
    ImGui::SeparatorText("Startup default");
    if (ImGui::Button("Save current map view as default")) {
        persist_config(app);
        status = "Saved current view as default";
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Use the live map position/zoom as the startup view");

    if (ImGui::Button("Save entered coordinates as default")) {
        persist_config_view(app, lat, lon, zoom);
        status = std::format("Saved {:.5f}, {:.5f}  z{} as default", lat, lon, zoom);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Use the Lat/Lon/Zoom typed above as the startup view");

    if (ImGui::Button("Reset default to Łódź")) {
        persist_config_view(app, LODZ_LAT, LODZ_LON, LODZ_ZOOM);
        status = "Default reset to Łódź";
    }

    if (!status.empty())
        ImGui::TextColored(ImVec4(0.6f, 1.0f, 0.6f, 1.0f), "%s", status.c_str());

    ImGui::End();
}

// ── Menu bar ──────────────────────────────────────────────────────────────────
static void draw_menu(AppState& app) {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open .osm…")) {
                if (auto p = open_file_dialog("Open OSM dataset",
                        "OSM data (*.osm)\0*.osm\0XML (*.xml)\0*.xml\0All files (*.*)\0*.*\0"))
                    app.load_osm_file(p->c_str());
            }
            if (ImGui::MenuItem("Import .osc / diff…")) {
                if (auto p = open_file_dialog("Import OSM changeset",
                        "OSM change (*.osc)\0*.osc\0XML (*.xml)\0*.xml\0All files (*.*)\0*.*\0"))
                    app.load_osc_file(p->c_str());
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Export edits (.osc)…", nullptr, false, app.has_active())) {
                if (auto p = save_file_dialog("Export edits as OsmChange",
                        "OSM change (*.osc)\0*.osc\0All files (*.*)\0*.*\0", "osc")) {
                    bool ok = app.export_osc(p->c_str());
                    app.map_view.set_status(ok ? "Exported changes to .osc"
                                               : "Export failed");
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Quit")) glfwSetWindowShouldClose(glfwGetCurrentContext(), true);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false, app.map_view.can_undo()))
                app.map_view.undo();
            if (ImGui::MenuItem("Redo", "Ctrl+Y", false, app.map_view.can_redo()))
                app.map_view.redo();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Center on data") && app.has_active())
                app.map_view.center_on_dataset(app.active_layer().data);
            ImGui::MenuItem("Go to coordinates…", nullptr, &app.show_goto_);
            if (ImGui::MenuItem("Go to default (Łódź)"))
                app.map_view.set_view(LODZ_LAT, LODZ_LON, LODZ_ZOOM);
            if (ImGui::MenuItem("Save current view as default"))
                persist_config(app);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Windows")) {
            ImGui::MenuItem("Selection",        nullptr, &app.show_selection_);
            ImGui::MenuItem("Layers",           nullptr, &app.show_layers_);
            ImGui::MenuItem("Style",            nullptr, &app.show_style_);
            ImGui::MenuItem("Go to coordinates",nullptr, &app.show_goto_);
            ImGui::MenuItem("Diff",             nullptr, &app.show_diff_);
            ImGui::MenuItem("Overpass",         nullptr, &app.show_overpass_);
            ImGui::MenuItem("AI Detection",     nullptr, &app.show_detection_);
            ImGui::MenuItem("Upload",           nullptr, &app.show_upload_);
            ImGui::Separator();
            ImGui::MenuItem("Settings",         nullptr, &app.show_settings_);
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
}

// ── Selection inspector (editable tags for all selected nodes/ways) ─────────────
static void draw_inspector(AppState& app, bool* p_open) {
    ImGui::Begin("Selection", p_open);
    osm::Dataset* ds = app.has_active() ? &app.active_layer().data : nullptr;

    int sel_count = app.map_view.selection_count();

    if (!ds || sel_count == 0) {
        ImGui::TextDisabled("Nothing selected.");
        ImGui::TextWrapped("Pick the Select tool and click a node or way on the map. "
                           "Hold Shift to add to selection. Drag to box-select.");
        ImGui::End();
        return;
    }

    // Summary bar
    const auto& sel_nodes = app.map_view.selected_nodes();
    const auto& sel_ways  = app.map_view.selected_ways();
    if (sel_count == 1) {
        if (!sel_nodes.empty())
            ImGui::TextDisabled("1 node selected");
        else
            ImGui::TextDisabled("1 way selected");
    } else {
        ImGui::TextDisabled("%d selected  (%zu node%s, %zu way%s)",
            sel_count,
            sel_nodes.size(), sel_nodes.size() == 1 ? "" : "s",
            sel_ways.size(),  sel_ways.size()  == 1 ? "" : "s");
    }
    ImGui::Separator();

    // Scrollable list of selected items – each as a collapsible section.
    // For a single selection, skip the header and show tags directly.
    // When multiple items are selected, tags are shown read-only to avoid
    // shared TagEditor state confusion. Click a single item to edit.
    bool read_only = (sel_count > 1);
    if (read_only)
        ImGui::TextDisabled("(click a single feature to edit tags)");

    bool modified = false;
    ImGui::BeginChild("sel_scroll", ImVec2(0, 0), ImGuiChildFlags_None);

    int item_idx = 0;
    for (int64_t nid : sel_nodes) {
        ImGui::PushID(item_idx++);
        auto it = ds->nodes.find(nid);
        if (it == ds->nodes.end()) { ImGui::PopID(); continue; }
        osm::Node& n = it->second;

        char label[64];
        std::snprintf(label, sizeof(label), "Node %lld", (long long)nid);
        bool open = (sel_count == 1) || ImGui::CollapsingHeader(label,
                        ImGuiTreeNodeFlags_DefaultOpen);
        if (sel_count > 1 && ImGui::IsItemHovered())
            ImGui::SetTooltip("lat %.6f  lon %.6f  v%lld", n.lat, n.lon, (long long)n.version);

        if (open) {
            if (sel_count == 1) {
                ImGui::Text("Node %lld", (long long)nid);
                ImGui::SameLine(); ImGui::TextDisabled("(v%lld)", (long long)n.version);
                ImGui::Text("lat %.6f   lon %.6f", n.lat, n.lon);
                ImGui::Separator();
            }
            if (app.tag_editor.draw(n.tags, read_only)) modified = true;
            if (sel_count > 1) ImGui::Separator();
        }
        ImGui::PopID();
    }

    for (int64_t wid : sel_ways) {
        ImGui::PushID(item_idx++);
        auto it = ds->ways.find(wid);
        if (it == ds->ways.end()) { ImGui::PopID(); continue; }
        osm::Way& w = it->second;

        char label[64];
        std::snprintf(label, sizeof(label), "Way %lld  (%zu nd)",
                      (long long)wid, w.nodes.size());
        bool open = (sel_count == 1) || ImGui::CollapsingHeader(label,
                        ImGuiTreeNodeFlags_DefaultOpen);
        if (sel_count > 1 && ImGui::IsItemHovered())
            ImGui::SetTooltip("v%lld  %zu nodes", (long long)w.version, w.nodes.size());

        if (open) {
            if (sel_count == 1) {
                ImGui::Text("Way %lld", (long long)wid);
                ImGui::SameLine();
                ImGui::TextDisabled("(v%lld, %zu nodes)", (long long)w.version, w.nodes.size());
                ImGui::Separator();
            }
            if (app.tag_editor.draw(w.tags, read_only)) modified = true;
            if (sel_count > 1) ImGui::Separator();
        }
        ImGui::PopID();
    }

    ImGui::EndChild();

    if (modified && app.has_active())
        app.active_layer().modified = true;

    ImGui::End();
}

// ── Layers panel (JOSM-style: list of named data layers + imagery toggles) ────
static void draw_layers(AppState& app, bool* p_open) {
    ImGui::Begin("Layers", p_open);

    // ── Data layers list ──
    ImGui::SeparatorText("Data layers");
    static int  merge_target{-1};
    static std::string rename_buf;

    for (int i = (int)app.layers_.size() - 1; i >= 0; --i) {
        auto& L = app.layers_[i];
        bool is_active = (i == app.active_);

        ImGui::PushID(i);

        // Visibility checkbox
        ImGui::Checkbox("##vis", &L.visible);
        if (!is_active && ImGui::IsItemEdited()) app.sync_map_view();
        ImGui::SameLine();

        // Highlight active row
        if (is_active)
            ImGui::PushStyleColor(ImGuiCol_Header, ImGui::GetColorU32(ImGuiCol_HeaderActive));
        bool selected = ImGui::Selectable(L.name.c_str(), is_active,
                                         ImGuiSelectableFlags_SpanAllColumns);
        if (is_active) ImGui::PopStyleColor();
        if (selected && !is_active) app.set_active(i);

        // Modified marker
        if (L.modified) { ImGui::SameLine(); ImGui::TextDisabled(" *"); }

        // Right-click context menu
        if (ImGui::BeginPopupContextItem("layer_ctx")) {
            if (ImGui::MenuItem("Upload this layer", nullptr, false,
                                app.upload_token_[0] != '\0'))
                app.upload_layer(i);
            if (ImGui::MenuItem("Export this layer (.osc)…")) {
                if (auto p = save_file_dialog("Export edits",
                        "OSM change (*.osc)\0*.osc\0All files\0*.*\0", "osc")) {
                    auto cs = osm::diff::diff_datasets(L.baseline, L.data);
                    osm::write_osc(cs, p->c_str());
                }
            }
            ImGui::Separator();
            if (ImGui::BeginMenu("Merge into…")) {
                for (int j = 0; j < (int)app.layers_.size(); ++j) {
                    if (j == i) continue;
                    if (ImGui::MenuItem(app.layers_[j].name.c_str()))
                        app.merge_layers(j, i);
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Rename…")) {
                rename_buf = L.name;
                ImGui::OpenPopup("rename_layer");
            }
            if (ImGui::MenuItem("Delete layer")) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); ImGui::PopID(); app.delete_layer(i); continue; }
            if (ImGui::BeginPopupModal("rename_layer", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::InputText("Name", &rename_buf);
                if (ImGui::Button("OK") && !rename_buf.empty()) {
                    app.layers_[i].name = rename_buf;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
            ImGui::EndPopup();
        }

        // Tooltip with stats
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("%zu nodes  %zu ways  %zu relations",
                              L.data.nodes.size(), L.data.ways.size(), L.data.relations.size());

        ImGui::PopID();
    }

    // New layer button
    if (ImGui::Button("+ New layer")) app.new_layer();

    // ── Rendering toggles ──
    ImGui::SeparatorText("Rendering");
    ImGui::Checkbox("Imagery",      &app.map_view.layer_imagery());
    ImGui::Checkbox("OSM data",     &app.map_view.layer_osm());
    ImGui::Checkbox("Diff overlay", &app.map_view.layer_diff());

    ImGui::End();
}

// ── Style panel (configure how nodes / ways / areas are drawn) ────────────────
static void draw_style(AppState& app, bool* p_open) {
    ImGui::Begin("Style", p_open);
    ui::MapStyle& s = app.map_view.style();
    const ImGuiColorEditFlags cf = ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreview;

    ImGui::SeparatorText("Nodes");
    ImGui::ColorEdit4("Node",        &s.node_color.x,  cf);
    ImGui::ColorEdit4("Tagged node", &s.node_tagged.x, cf);
    ImGui::SliderFloat("Node radius", &s.node_radius, 1.f, 10.f, "%.1f");
    ImGui::SliderInt("Show nodes at zoom \xe2\x89\xa5", &s.node_min_zoom, 10, 19);

    ImGui::SeparatorText("Ways");
    ImGui::ColorEdit4("Line color", &s.way_color.x, cf);
    ImGui::SliderFloat("Line width", &s.way_width, 0.5f, 6.f, "%.1f");

    ImGui::SeparatorText("Areas");
    ImGui::ColorEdit4("Fill",    &s.area_fill.x,    cf);
    ImGui::ColorEdit4("Outline", &s.area_outline.x, cf);
    ImGui::SliderFloat("Outline width", &s.area_width, 0.5f, 6.f, "%.1f");
    ImGui::SliderFloat("Area opacity", &s.area_opacity, 0.1f, 1.5f, "%.2f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Global multiplier on area fills — lower it to see\n"
                          "buildings/roads through land-use polygons");

    ImGui::SeparatorText("Pretty rendering");
    ImGui::Checkbox("Road casing", &s.road_casing);
    ImGui::Checkbox("POI icons", &s.show_poi_icons);
    ImGui::SameLine();
    ImGui::Checkbox("Labels", &s.show_labels);
    ImGui::BeginDisabled(!s.show_labels);
    ImGui::SliderInt("Labels at zoom \xe2\x89\xa5", &s.label_min_zoom, 12, 19);
    ImGui::EndDisabled();
    ImGui::Checkbox("Random colors (by feature id)", &s.random_colors);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Color every way/area by a hash of its id so adjacent\n"
                          "features are easy to tell apart");

    ImGui::Separator();
    if (ImGui::Button("Reset to defaults")) s = ui::MapStyle{};
    ImGui::End();
}

// ── Settings panel ────────────────────────────────────────────────────────────
static void draw_settings(AppState& app, bool* p_open) {
    if (!ImGui::Begin("Settings", p_open)) { ImGui::End(); return; }

    ImGui::SeparatorText("Tile cache");

    // Cached size is expensive to compute (walks the dir), so do it lazily and
    // only refresh on demand or after a purge.
    static std::uintmax_t cache_bytes = UINTMAX_MAX; // UINTMAX_MAX = not computed
    if (cache_bytes == UINTMAX_MAX)
        cache_bytes = map::TileCache::disk_cache_size();

    ImGui::Text("On-disk cache: %.1f MB", cache_bytes / (1024.0 * 1024.0));
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh"))
        cache_bytes = map::TileCache::disk_cache_size();

    ImGui::TextDisabled("%s", map::TileCache::disk_cache_root().string().c_str());

    if (ImGui::Button("Purge tile cache\xe2\x80\xa6"))
        ImGui::OpenPopup("Purge tile cache?");

    if (ImGui::BeginPopupModal("Purge tile cache?", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Delete all cached map tiles from disk?\n"
                               "They will re-download as you browse.");
        ImGui::Separator();
        if (ImGui::Button("Purge", ImVec2(120, 0))) {
            std::uintmax_t freed = app.map_view.purge_tile_cache();
            cache_bytes = map::TileCache::disk_cache_size();
            char msg[96];
            std::snprintf(msg, sizeof msg, "Purged tile cache (%.1f MB freed)",
                          freed / (1024.0 * 1024.0));
            app.map_view.set_status(msg);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::End();
}

// ── Upload panel (push edits to an OSM API server) ────────────────────────────
static void draw_upload(AppState& app, bool* p_open) {
    ImGui::Begin("Upload", p_open);
    ImGui::TextWrapped("Upload your edits as an OSM changeset. Needs an OAuth2 "
                       "access token with the 'write_api' scope.");
    ImGui::Separator();

    const char* servers[] = {"Dev sandbox (master.apis.dev — safe to test)",
                             "LIVE OpenStreetMap (real edits!)"};
    ImGui::Combo("Server", &app.upload_server_, servers, 2);
    if (app.upload_server_ == 1)
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1),
                           "WARNING: this writes to the real OSM map.");

    // ── OAuth2 login ──
    ImGui::SeparatorText("Login");
    const char* web = (app.upload_server_ == 1) ? "https://www.openstreetmap.org"
                                                : "https://master.apis.dev.openstreetmap.org";
    std::string redirect = net::oauth_redirect_uri(8910);
    ImGui::TextWrapped(
        "One-time setup: register an OAuth2 app at %s/oauth2/applications/new\n"
        "  \xe2\x80\xa2 Scope: write_api\n"
        "  \xe2\x80\xa2 Redirect URI: exactly as shown below\n"
        "  \xe2\x80\xa2 If you check 'Confidential application', fill in the Client Secret too.",
        web);
    ImGui::InputText("Redirect URI##ru", &redirect, ImGuiInputTextFlags_ReadOnly);
    ImGui::InputText("Client ID",     &app.upload_client_id_, ImGuiInputTextFlags_None);
    ImGui::InputText("Client Secret", &app.upload_client_secret_, ImGuiInputTextFlags_Password);
    ImGui::TextDisabled("(leave Client Secret empty for public/native apps)");
    ImGui::BeginDisabled(app.upload_client_id_.empty());
    if (ImGui::Button("Log in with OSM")) app.oauth_login();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled(!app.upload_token_.empty() ? "(token set \xe2\x9c\x93)" : "(not logged in)");

    ImGui::SeparatorText("Changeset");
    ImGui::InputText("Comment", &app.upload_comment_, ImGuiInputTextFlags_None);
    ImGui::InputText("OAuth2 token", &app.upload_token_, ImGuiInputTextFlags_Password);

    bool can = app.has_active() && !app.upload_token_.empty();
    ImGui::BeginDisabled(!can);
    if (ImGui::Button("Upload active layer")) app.upload_layer(app.active_);
    ImGui::EndDisabled();
    if (!can)
        ImGui::TextDisabled("Download/load data and log in to enable upload.");

    {
        std::scoped_lock lk(app.pending_mu_);
        if (!app.upload_status_.empty())
            ImGui::TextWrapped("%s", app.upload_status_.c_str());
    }
    ImGui::End();
}

// ── Entry point ───────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    // Open log file in same APPDATA dir as config.json.
    {
        namespace fs = std::filesystem;
        const char* appdata = std::getenv("APPDATA");
        fs::path dir = appdata ? fs::path(appdata) / "cpposmui" : fs::current_path();
        std::error_code ec;
        fs::create_directories(dir, ec);
        applog::init((dir / "cpposmui.log").string());
    }
    LOG_INFO("cpposmui starting");

    if (!glfwInit()) return 1;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_STENCIL_BITS, 8);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

    GLFWwindow* win = glfwCreateWindow(1600, 900, "cpposmui", nullptr, nullptr);
    if (!win) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);

    // Load OpenGL function pointers via glad (tile_cache uploads textures
    // through these — without this they'd be null and crash on first tile).
    if (!gladLoadGL(reinterpret_cast<GLADloadfunc>(glfwGetProcAddress))) {
        fprintf(stderr, "Failed to initialize OpenGL loader (glad)\n");
        glfwTerminate();
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    ImGui::StyleColorsDark();

    // Load Segoe UI with Latin Extended-A so Polish chars (ł ó ą ę ź ż ć ń ś) render correctly.
    static const ImWchar latin_ext_ranges[] = { 0x0020, 0x017F, 0 };
    ImFontConfig font_cfg;
    font_cfg.OversampleH = 2; font_cfg.OversampleV = 2;
    if (!io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/segoeui.ttf", 15.0f, &font_cfg, latin_ext_ranges))
        io.Fonts->AddFontDefault(); // fallback if font file missing

    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    AppState app;

    // Restore the persisted view (defaults to Łódź) + style + login, wire downloads.
    UploadPrefs up;
    ViewConfig cfg = load_config(app.map_view.style(), up);
    app.upload_server_ = up.server;
    app.upload_client_id_ = up.client_id;
    app.upload_client_secret_ = up.client_secret;
    app.upload_token_ = up.token;
    app.upload_comment_ = up.comment;
    app.map_view.set_view(cfg.lat, cfg.lon, cfg.zoom);
    // Restore per-source API keys before applying source (so the key is baked in).
    for (const auto& [idx, key] : cfg.tile_keys)
        app.map_view.set_tile_key(idx, key);
    app.map_view.set_tile_source_index(cfg.source);
    app.map_view.set_overlay_source_index(cfg.overlay);
    app.map_view.clear_view_dirty();
    app.map_view.set_download_callback(
        [&app](double mn_lat, double mn_lon, double mx_lat, double mx_lon) {
            app.download_bbox(mn_lat, mn_lon, mx_lat, mx_lon);
        });

    // Optional: load files from command line (these recenter on the data)
    if (argc >= 2) app.load_osm_file(argv[1]);
    if (argc >= 3) app.load_osc_file(argv[2]);

    // Track the last view state pushed to the Overpass panel so we only recompute
    // the bbox when the map actually changes (pan / zoom), not every frame.
    double ov_last_lat = 0, ov_last_lon = 0;
    int    ov_last_zoom = -1;

    while (!glfwWindowShouldClose(win)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Apply any finished async download on the main (GL) thread.
        app.apply_pending();

        // Sync AI detection results to the map overlay (frame-local shared_ptr
        // keeps the result alive for the duration of map_view.draw()).
        auto det_sp = app.detection_panel.result_sp();
        app.map_view.set_ai_detections(det_sp.get());

        // Undo / redo shortcuts (ignored while typing in a text field).
        if (!io.WantTextInput) {
            if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z)) app.map_view.undo();
            if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y)) app.map_view.redo();
        }

        // Full-screen dockspace
        ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());

        draw_menu(app);
        app.map_view.draw();
        if (app.show_selection_) draw_inspector(app, &app.show_selection_);
        if (app.show_layers_)    draw_layers(app, &app.show_layers_);
        if (app.show_style_)     draw_style(app, &app.show_style_);
        if (app.show_settings_)  draw_settings(app, &app.show_settings_);
        if (app.show_goto_)      draw_goto(app, &app.show_goto_);
        if (app.show_diff_)      app.diff_panel.draw(&app.show_diff_);
        if (app.show_overpass_) {
            double lat = app.map_view.center_lat();
            double lon = app.map_view.center_lon();
            int    z   = app.map_view.zoom();
            if (lat != ov_last_lat || lon != ov_last_lon || z != ov_last_zoom) {
                double mn_lat, mn_lon, mx_lat, mx_lon;
                app.map_view.get_view_bbox(mn_lat, mn_lon, mx_lat, mx_lon);
                app.overpass_panel->set_bbox(mn_lat, mn_lon, mx_lat, mx_lon);
                ov_last_lat = lat; ov_last_lon = lon; ov_last_zoom = z;
            }
            app.overpass_panel->draw(&app.show_overpass_);
        }
        if (app.show_upload_)    draw_upload(app, &app.show_upload_);
        if (app.show_detection_) {
            // Keep detection panel bbox + imagery source in sync with the viewport.
            double mn_lat, mn_lon, mx_lat, mx_lon;
            app.map_view.get_view_bbox(mn_lat, mn_lon, mx_lat, mx_lon);
            app.detection_panel.set_bbox(mn_lat, mn_lon, mx_lat, mx_lon);
            app.detection_panel.set_imagery(app.map_view.current_base_url(),
                                            app.map_view.zoom());
            app.detection_panel.draw(&app.show_detection_);
        }

        // Sync map highlight from diff panel selection
        app.map_view.set_highlight_id(app.diff_panel.selected_id());

        ImGui::Render();
        int w, h;
        glfwGetFramebufferSize(win, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.1f, 0.1f, 0.1f, 1.f);
        glClearStencil(0);
        glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
            glfwMakeContextCurrent(win);
        }
        glfwSwapBuffers(win);
    }

    // Persist the final view + style + login for next launch.
    persist_config(app);

    LOG_INFO("cpposmui shutting down");
    net::shutdown_thread_pool();
    applog::shutdown();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
