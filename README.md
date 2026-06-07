# cpposmui

A cross-platform C++26 GUI tool for reviewing and editing [OpenStreetMap](https://www.openstreetmap.org/) data, built on [Dear ImGui](https://github.com/ocornut/imgui). Inspired by JOSM, optimised for performance and changeset review workflows: load a base `.osm` dataset and an `.osc` changeset, and instantly see what changed — tag by tag, object by object — over a live satellite or OSM tile background.

---

## Features

- **Visual diff panel** — side-by-side before/after tag values, color-coded `[+] added` / `[~] modified` / `[-] deleted` per object
- **Interactive slippy map** — pan, scroll-to-zoom, click to select; ESRI World Imagery and OSM Standard tiles switchable at runtime
- **Async tile cache** — tiles fetched on a Boost.Asio thread pool; map stays responsive while loading
- **Overpass API integration** — built-in query editor, results auto-diffed against the loaded base dataset
- **Tag editor** — inline editable key/value table for any selected node, way, or relation
- **OSM upload** — OAuth2 login flow, changeset creation and upload to the live or dev API
- **AI detection panel** — optional ONNX Runtime backend for local feature-detection inference
- **Spatial index** — Boost.Geometry R-tree for viewport-culled rendering of large datasets

---

## Building

### Prerequisites

| Requirement | Version |
|---|---|
| CMake | 3.25+ |
| C++ compiler | GCC 14+, Clang 18+, or MSVC 2022 17.8+ |
| libcurl | system install (see below) |
| OpenGL | system (included on all platforms) |

All other dependencies (Dear ImGui, GLFW, glad, pugixml, Boost, nlohmann/json, Catch2, earcut) are fetched automatically by CMake via `FetchContent` at configure time — no manual installation needed.

**Install libcurl:**

```sh
# Debian/Ubuntu
sudo apt install libcurl4-openssl-dev

# macOS
brew install curl

# Windows (vcpkg)
vcpkg install curl:x64-windows
```

### Linux / macOS

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### Windows (Visual Studio 2022)

```sh
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

### Running the tests

```sh
cmake --build build --target cpposmui_tests
ctest --test-dir build --output-on-failure
```

---

## Usage

```
cpposmui [base.osm] [changes.osc]
```

Both arguments are optional — files can also be loaded from the **File** menu at runtime.

| Action | How |
|---|---|
| Load base dataset | **File → Open .osm** |
| Load changeset | **File → Import .osc / diff** |
| Query live OSM data | **Overpass panel** — edit QL, press Run |
| Browse changes | **Diff panel** — click any entry to highlight on map |
| Edit tags | Select an object on the map, edit in **Tag editor** |
| Upload to OSM | **File → Upload** — OAuth2 login required |
| Switch tile source | Combo box at bottom-left of the map |

Config (API keys, OAuth tokens, last viewport) is saved automatically to `%APPDATA%/cpposmui/config.json` on Windows and `~/.config/cpposmui/config.json` on Linux/macOS.

---

## Source layout

```
src/
  main.cpp              — GLFW + ImGui entry point, dockspace, menus, config I/O
  osm/
    types.hpp           — Node, Way, Relation, TagMap, ChangeSet, DiffState
    parser.cpp          — .osm / .osc XML parser (pugixml)
    diff.cpp            — tag diff, dataset diff, changeset enrichment & apply
    spatial_index.hpp   — Boost.Geometry R-tree for viewport culling
    tag_presets.hpp     — common OSM tag presets
    data_layer.hpp      — layered dataset abstraction
  map/
    tile_cache.cpp      — async tile fetcher + OpenGL texture upload
  net/
    http.cpp            — libcurl wrapper (Boost.Asio thread pool)
    overpass.cpp        — Overpass QL builder + async query runner
    oauth.cpp           — OAuth2 PKCE flow for OSM API
    osm_upload.cpp      — changeset create / upload
  ui/
    map_view.cpp        — slippy map rendering, OSM + diff overlay
    diff_panel.cpp      — collapsible diff tree with tag before/after tables
    tag_editor.cpp      — inline editable tag table
    overpass_panel.cpp  — Overpass query editor
    detection_panel.cpp — AI detection results viewer
  ai/
    detection.cpp       — ONNX Runtime inference wrapper
tests/
  test_parser.cpp       — OSM/OSC parse unit tests
  test_diff.cpp         — tag diff, changeset enrichment, apply
  test_spatial.cpp      — R-tree bbox query tests
  test_overpass.cpp     — Overpass QL builder tests
  data/
    simple.osm          — fixture: 4 nodes, 2 ways
    changes.osc         — fixture: create/modify/delete changeset
```

---

## Dependencies

| Library | Version | Purpose |
|---|---|---|
| [Dear ImGui](https://github.com/ocornut/imgui) | docking branch | All UI rendering |
| [GLFW](https://www.glfw.org/) | 3.4 | Window + OpenGL context |
| [glad2](https://gen.glad.sh/) | GL 3.3 core | OpenGL loader |
| [pugixml](https://pugixml.org/) | 1.15 | OSM/OSC XML parsing |
| [Boost.Asio + Geometry](https://www.boost.org/) | 1.86 | Thread pool, R-tree spatial index |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.11.3 | Overpass JSON, config I/O |
| [earcut.hpp](https://github.com/mapbox/earcut.hpp) | 2.2.4 | Polygon triangulation |
| [stb_image](https://github.com/nothings/stb) | — | PNG tile decoding |
| [Catch2](https://github.com/catchorg/Catch2) | 3.x | Unit tests |
| [ONNX Runtime](https://onnxruntime.ai/) | 1.20.1 | Optional local AI inference |
| libcurl | system | HTTP for tiles + Overpass + OSM API |

---

## Tile sources

| Source | URL template |
|---|---|
| ESRI World Imagery (default) | `https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}` |
| OSM Standard | `https://tile.openstreetmap.org/{z}/{x}/{y}.png` |

---

## License

[GNU General Public License v3.0](LICENSE)
