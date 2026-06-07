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
- All dependencies fetched via `FetchContent` at configure time (no manual installs except libcurl + Boost + OpenGL)
- On Windows: needs libcurl (`vcpkg install curl:x64-windows`) and Boost, or let CMake fetch Boost
- **VS2022 CMake component** is NOT installed on this machine — use WSL or install it via VS Installer

## Building with WSL (recommended for this machine)

The project source lives in the Claude Cowork sandboxed AppData, which WSL **cannot** access via `/mnt/c/Users/...`.
You must copy it to a normal path first:

```powershell
# In PowerShell — copy to home folder
Copy-Item -Recurse -Force "$env:APPDATA\..\Local\Packages\Claude_pzs8sxrjxfjjc\LocalCache\Roaming\Claude\local-agent-mode-sessions\bfa67d03-fec1-4a99-a08f-d4b65d4d40a0\139179d5-8d98-4ad8-bbdb-36f6b61c1321\agent\local_ditto_139179d5-8d98-4ad8-bbdb-36f6b61c1321\outputs\cpposmui\*" "$env:USERPROFILE\cpposmui\"
```

Then in WSL:

```bash
# Install deps (one-time)
sudo apt update && sudo apt install -y cmake ninja-build g++ \
  libcurl4-openssl-dev libboost-all-dev \
  libglfw3-dev libgl1-mesa-dev xorg-dev

# Build
cd ~/cpposmui
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc) 2>&1 | tee build.log
```

## VS Code tasks

`.vscode/tasks.json` has a task **"WSL: copy + install deps + build"** that does the copy + WSL build in one click:
`Terminal → Run Task → WSL: copy + install deps + build`

## Source layout

```
src/
  main.cpp              — GLFW + ImGui app entry point, dockspace, menus
  osm/
    types.hpp           — Node, Way, Relation, TagMap, ChangeSet, Dataset, DiffState
    parser.cpp          — .osm and .osc XML parser (pugixml)
    diff.cpp            — tag diffs, dataset diff, changeset enrichment + apply
    spatial_index.hpp   — Boost.Geometry R-tree for viewport culling
  map/
    tile_cache.cpp      — async tile fetcher (Boost.Asio thread pool), OpenGL upload
  net/
    http.cpp            — libcurl wrapper, Boost.Asio thread pool
    overpass.cpp        — Overpass QL builder + async query
  ui/
    map_view.cpp        — slippy map (pan/zoom/scroll), OSM + diff overlay
    diff_panel.cpp      — +/~/- tree with per-tag before/after tables
    tag_editor.cpp      — inline editable tag table
    overpass_panel.cpp  — Overpass query editor + async run
tests/
  test_parser.cpp       — OSM/OSC parse unit tests (Catch2)
  test_diff.cpp         — tag diff, changeset enrichment, apply (Catch2)
  test_spatial.cpp      — R-tree bbox query tests (Catch2)
  test_overpass.cpp     — Overpass QL builder tests (Catch2)
  data/simple.osm       — fixture: 4 nodes, 2 ways
  data/changes.osc      — fixture: create/modify/delete changeset
```

## Key dependencies (all FetchContent unless noted)

| Lib | Purpose |
|-----|---------|
| Dear ImGui v1.91.6 | All UI rendering |
| GLFW 3.4 | Window + OpenGL context |
| glad2 (GL 3.3 core) | OpenGL loader |
| pugixml 1.14 | OSM/OSC XML parsing |
| Boost.Asio | Thread pool for tile/HTTP fetches |
| Boost.Geometry | R-tree spatial index |
| stb_image | PNG decode for tiles |
| nlohmann/json | Overpass JSON (future) |
| Catch2 v3 | Unit + integration tests |
| libcurl (system) | HTTP for tiles + Overpass |

## Known issues / next steps

- [ ] `glad` include: `tile_cache.cpp` uses `<glad/gl.h>` — make sure glad2 CMake target (`glad_gl33`) is linked
- [ ] `overpass.cpp` calls `curl_easy_escape(nullptr, ...)` — should use a live CURL handle
- [ ] Native file picker not yet integrated — currently uses a text-input popup for file paths
- [ ] Need to wire `SpatialIndex` rebuild after changeset apply in `AppState`
- [ ] Tests don't link `imgui`/`glfw`/`glad` — they use `cpposmui_core` (OSM + net only)
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
