# cpposmui — project context for Claude

## What this is

A cross-platform C++26 Dear ImGui GUI tool for OpenStreetMap editing, inspired by JOSM but focused on:
- **Visual diff** of OSM/OSC changesets (tag-level added/modified/deleted, color-coded)
- **Overpass API** integration for live data verification
- **ESRI satellite + OSM tile** background (async tile cache)
- **JOSM-like editing** — tag editor, object inspector
- Importing `.osm` (base dataset) and `.osc` (changeset) files generated from Geoportal/imagery

## Build system

- **CMake 3.25+**, C++26, Ninja preferred
- All dependencies fetched via `FetchContent` at configure time (no manual installs except libcurl + Boost + OpenGL); libcurl comes from the VS-bundled vcpkg
- **Build & run commands, toolchain paths, test tags, and gotchas live in [SKILLS.md](SKILLS.md)** — read that first when building/running.

### Quick reference (Windows, native MSVC + Ninja)

The VS2022-bundled CMake + Ninja **are** available here; `vcvars64.bat` puts
`cmake`/`ninja`/`cl` on PATH. From an "x64 Native Tools" prompt (or after running
vcvars):

```powershell
cmake -B build -G Ninja                            # configure (first run ~3 min)
cmake --build build --target cpposmui       -j8    # GUI app  → build\cpposmui.exe
cmake --build build --target cpposmui_tests -j8    # tests    → build\tests\cpposmui_tests.exe
```

Stop a running `cpposmui.exe` before re-linking (LNK1168). WSL is a fallback only —
see SKILLS.md. The repo lives at a normal path (`C:\dev\cpposmui`), so the old
AppData copy-step is obsolete.

## Source layout

```
src/
  main.cpp              — GLFW + ImGui app entry point, dockspace, menus, Settings
  log.hpp               — applog logger (LOG_DEBUG/INFO/WARN/ERR), ring buffer + file
  osm/
    types.hpp           — Node, Way, Relation, TagMap, ChangeSet, Dataset, DiffState
    parser.cpp          — .osm and .osc XML parser (pugixml)
    diff.cpp            — tag diffs, dataset diff, changeset enrichment + apply
    spatial_index.hpp   — Boost.Geometry R-tree for viewport culling
  map/
    tile_cache.cpp      — async tile fetcher (Boost.Asio pool), disk cache, GL upload
                          (also hosts STB_IMAGE_IMPLEMENTATION for the app)
  net/
    http.cpp            — libcurl wrapper, Boost.Asio thread pool, post_async
    overpass.cpp        — Overpass QL builder + async query
  ai/
    detection.cpp/.hpp  — feature detection: MS Buildings, MapWithAI, custom REST,
                          classical CV (Sobel + colour seg), CV+LiDAR DSM, ONNX
  ui/
    map_view.cpp        — slippy map (pan/zoom/scroll), OSM + diff + detection overlay
    diff_panel.cpp      — +/~/- tree with per-tag before/after tables
    tag_editor.cpp      — inline editable tag table
    overpass_panel.cpp  — Overpass query editor + async run
    detection_panel.cpp — AI/CV detection source picker, candidate list, accept/reject/remove
tests/
  test_parser.cpp       — OSM/OSC parse unit tests (Catch2)
  test_diff.cpp         — tag diff, changeset enrichment, apply (Catch2)
  test_spatial.cpp      — R-tree bbox query tests (Catch2)
  test_overpass.cpp     — Overpass QL builder tests (Catch2)
  test_detection.cpp    — classical-CV detection over synthetic imagery (Catch2, offline)
  stb_impl.cpp          — STB_IMAGE_IMPLEMENTATION for the test binary (see SKILLS.md)
  data/simple.osm       — fixture: 4 nodes, 2 ways
  data/changes.osc      — fixture: create/modify/delete changeset
```

## Key dependencies (all FetchContent unless noted)

| Lib | Purpose |
|-----|---------|
| Dear ImGui v1.91.6 | All UI rendering |
| GLFW 3.4 | Window + OpenGL context |
| glad2 (GL 3.3 core) | OpenGL loader |
| pugixml 1.15 | OSM/OSC XML parsing |
| Boost.Asio | Thread pool for tile/HTTP fetches |
| Boost.Geometry | R-tree spatial index |
| stb_image | PNG/JPEG decode for tiles + detection imagery |
| earcut.hpp | Polygon triangulation (area fills) |
| nlohmann/json | GeoJSON (MS Buildings, custom REST), Overpass JSON |
| ONNX Runtime 1.20.1 | Optional local AI inference (auto-fetched on Windows) |
| Catch2 v3 | Unit + integration tests |
| libcurl (vcpkg) | HTTP for tiles + Overpass + detection |

## Known issues / next steps

- [ ] `glad` include: `tile_cache.cpp` uses `<glad/gl.h>` — make sure glad2 CMake target (`glad_gl33`) is linked
- [ ] `overpass.cpp` calls `curl_easy_escape(nullptr, ...)` — should use a live CURL handle
- [ ] Native file picker not yet integrated — currently uses a text-input popup for file paths
- [ ] Need to wire `SpatialIndex` rebuild after changeset apply in `AppState`
- [ ] Tests don't link `imgui`/`glfw`/`glad` — they use `cpposmui_core` (OSM + net + `ai/detection.cpp`, plus `tests/stb_impl.cpp` for the stb_image impl)
- [ ] Windows: `_WIN32_WINNT=0x0A00` required for Boost.Asio

## Tile sources

- **ESRI World Imagery** (default): `https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}`
- **OSM Standard**: `https://tile.openstreetmap.org/{z}/{x}/{y}.png`

Switch in-app via the map view combo box (bottom-left of map).

## Usage

```
cpposmui [base.osm] [changes.osc]
```

Or load via `File → Open .osm` / `File → Import .osc / diff`.
The diff panel updates automatically; clicking an entry highlights the object on the map.
The Overpass panel pre-fills the bbox from the loaded dataset; run any QL query and it auto-diffs against the base.
