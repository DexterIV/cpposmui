# cpposmui — build & run skills

Concrete, validated commands for building, running, and testing on **this machine**
(Windows 11, native MSVC + Ninja). For architecture/source layout see
[CLAUDE.md](CLAUDE.md).

## TL;DR

```powershell
# 1. Enter a VS dev environment (puts cmake, ninja, cl on PATH)
& "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"

# 2. Configure once (Ninja + the VS-bundled vcpkg toolchain — supplies libcurl)
cmake -B build -G Ninja

# 3. Build the app / tests
cmake --build build --target cpposmui       -j8   # the GUI app
cmake --build build --target cpposmui_tests -j8   # the Catch2 test binary

# 4. Run
build\cpposmui.exe
build\tests\cpposmui_tests.exe
```

## Toolchain (where things actually live)

`cmake` and `ninja` are **not** on the global PATH — they ship with Visual Studio
2022 and are added to PATH by `vcvars64.bat`:

| Tool | Path |
|------|------|
| vcvars | `C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat` |
| cmake | `…\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe` |
| ninja | `…\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe` |
| vcpkg toolchain | `…\VC\vcpkg\scripts\buildsystems\vcpkg.cmake` (auto-selected; provides `curl:x64-windows`) |

Easiest interactive route: launch **"x64 Native Tools Command Prompt for VS 2022"**
from the Start menu — `cmake`/`ninja`/`cl` are already on PATH there, skip step 1.

### Non-interactive build (one shot, e.g. from a script or agent)

`vcvars64.bat` only mutates the calling shell, so chain it with the build in a
single `cmd` invocation:

```bat
@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cmake --build C:\dev\cpposmui\build --target cpposmui_tests -j8
```

From PowerShell, run that via `& cmd /c "...\that.bat"`. Chaining `vcvars && cmake`
inline through `cmd /c` from PowerShell is fragile (quoting) — prefer a `.bat`.

## Build targets

| Target | Output | Notes |
|--------|--------|-------|
| `cpposmui` | `build\cpposmui.exe` | GUI app. Links imgui/glfw/glad/OpenGL via `cpposmui_lib`. |
| `cpposmui_tests` | `build\tests\cpposmui_tests.exe` | Catch2. Links **`cpposmui_core`** (osm + net + **ai**, no GL). |
| `cpposmui_lib` | static lib | All of `src/{osm,net,map,ui,ai}` for the app. |
| `cpposmui_core` | static lib | Headless subset for tests: `osm/`, `net/`, `ai/detection.cpp`. |

First configure takes **~3 min** — it FetchContents Boost/ImGui/GLFW/Catch2 and
downloads a prebuilt ONNX Runtime (win-x64). Subsequent configures are seconds.

## Running the app

```powershell
build\cpposmui.exe                      # empty session
build\cpposmui.exe base.osm changes.osc # load a base dataset + changeset
```

- File → Open .osm / Import .osc to load interactively.
- Map source combo (bottom-left of the map) switches ESRI/OSM/Geoportal layers.
- Logs go to `%APPDATA%\cpposmui\cpposmui.log` (also a live ring buffer in-app).

## Running tests

```powershell
build\tests\cpposmui_tests.exe                       # all
build\tests\cpposmui_tests.exe "[detection]"         # one tag
build\tests\cpposmui_tests.exe --reporter compact    # terse output
ctest --test-dir build                               # via CTest (catch_discover_tests)
```

Tag overview: `[parser]`, `[diff]`, `[spatial]`, `[overpass]`, `[detection]`.
The detection tests synthesize in-memory imagery — fully offline, no tiles/GL.

## Gotchas (learned the hard way)

- **LNK1168 "cannot open cpposmui.exe for writing"** — the app is still running.
  `Get-Process cpposmui -ErrorAction SilentlyContinue | Stop-Process -Force`
  before re-linking.
- **Adding a file to `cpposmui_core`** (a non-glob source list in
  `tests/CMakeLists.txt`) requires a **re-configure** — Ninja's `CONFIGURE_DEPENDS`
  re-globs the test `*.cpp` automatically, but explicit source lists don't.
- **stb_image in tests** — `detection.cpp` only *declares* `stbi_*`; the
  implementation lives in `src/map/tile_cache.cpp` (GL-linked, not in
  `cpposmui_core`). `tests/stb_impl.cpp` provides `STB_IMAGE_IMPLEMENTATION` for
  the test binary.
- **PowerShell + Polish locale** — float formatting uses commas; prefer integer
  args/bboxes when scripting. `2>&1` on native exes wraps stderr as error records
  (sets `$?` false even on exit 0) — don't redirect; stderr is captured already.
- **Geoportal tiles flake** (~20-30% curl-56 resets + load-shed white JPEGs);
  the tile cache retries with backoff and refuses to cache sub-`min_valid_bytes`
  blanks — expected, not a build problem.

## WSL fallback (only if VS toolchain is unavailable)

The repo now lives at a normal path (`C:\dev\cpposmui`), reachable from WSL via
`/mnt/c/dev/cpposmui` — no copy step needed.

```bash
sudo apt install -y cmake ninja-build g++ libcurl4-openssl-dev libboost-all-dev \
  libglfw3-dev libgl1-mesa-dev xorg-dev
cmake -B build-wsl -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-wsl -j$(nproc)
```

Use a **separate build dir** (`build-wsl`) so it doesn't clobber the MSVC `build/`.
