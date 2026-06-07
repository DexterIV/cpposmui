# Building cpposmui

## Prerequisites

- CMake 3.25+
- C++26-capable compiler: GCC 14+, Clang 18+, or MSVC 2022 17.8+
- libcurl (system)
  - Windows: `vcpkg install curl` or download pre-built
  - Linux: `sudo apt install libcurl4-openssl-dev`
  - macOS: `brew install curl`
- OpenGL (system — included on all platforms)

## Build

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

On Windows with Visual Studio 2022:
```sh
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

## Usage

```
cpposmui [base.osm] [changes.osc]
```

- **File > Open .osm** – load a base OSM dataset
- **File > Import .osc / diff** – load a changeset and see the visual diff
- **Overpass panel** – query live OSM data; auto-diffs against your base dataset
- **Diff panel** – browse added/modified/deleted objects; click to highlight on map
- Map supports scroll-to-zoom and drag-to-pan; switch between ESRI satellite and OSM standard tiles
