cmake_minimum_required(VERSION 3.25)
project(cpposmui VERSION 0.1.0 LANGUAGES CXX C)  # C needed for glad (OpenGL loader) + Threads checks

# C++26 everywhere it's supported. MSVC 19.44 + CMake 3.31 doesn't yet map
# CXX_STANDARD 26 to a flag, but on MSVC C++23 already selects /std:c++latest,
# which exposes the same in-flight C++26 features we use.
if(MSVC)
    set(CMAKE_CXX_STANDARD 23)
else()
    set(CMAKE_CXX_STANDARD 26)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Emit build/compile_commands.json for clangd / IDE tooling.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

# ── glad (OpenGL 3.3 core loader) ─────────────────────────────────────────────
FetchContent_Declare(glad
    GIT_REPOSITORY https://github.com/Dav1dde/glad.git
    GIT_TAG        v2.0.8
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  cmake
)
FetchContent_MakeAvailable(glad)
glad_add_library(glad_gl33 REPRODUCIBLE API gl:core=3.3)

# ── GLFW ──────────────────────────────────────────────────────────────────────
FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
    GIT_SHALLOW    TRUE
)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

# ── Dear ImGui ────────────────────────────────────────────────────────────────
# docking branch: required for DockSpaceOverViewport + multi-viewport APIs used in main.cpp
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        docking
    GIT_SHALLOW    TRUE
)
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
    FetchContent_MakeAvailable(imgui)
endif()

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp   # ImGui::InputText(std::string*) overload
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui PUBLIC glfw glad_gl33)

# ── pugixml ───────────────────────────────────────────────────────────────────
FetchContent_Declare(pugixml
    GIT_REPOSITORY https://github.com/zeux/pugixml.git
    GIT_TAG        v1.15
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(pugixml)

# ── stb (header-only) ─────────────────────────────────────────────────────────
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(stb)
add_library(stb INTERFACE)
target_include_directories(stb INTERFACE ${stb_SOURCE_DIR})

# ── nlohmann/json ─────────────────────────────────────────────────────────────
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# ── Catch2 (tests) ────────────────────────────────────────────────────────────
FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.15.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(Catch2)

# ── Boost ─────────────────────────────────────────────────────────────────────
# We use header-only components: Asio (networking/thread pool), Geometry (R-tree)
# and Boost.Beast is NOT needed – we drive libcurl via Asio strand dispatch only.
# System Boost is preferred; if not found we fetch it.
# asio + geometry are header-only but must be requested as components so their
# Boost::asio / Boost::geometry imported targets are created (vcpkg modular Boost).
find_package(Boost 1.86 QUIET COMPONENTS system thread asio geometry)
if(NOT Boost_FOUND)
    message(STATUS "System Boost not found – fetching Boost 1.86 headers via FetchContent")
    FetchContent_Declare(boost_src
        URL      https://github.com/boostorg/boost/releases/download/boost-1.86.0/boost-1.86.0-cmake.tar.xz
        URL_HASH SHA256=a4d99d037b9b6a7f4d85e4887a1559c5ad0cf4a5a1d5fddb0b0d8c4b0c1e7c34
    )
    set(BOOST_INCLUDE_LIBRARIES asio geometry system thread)
    set(BOOST_ENABLE_CMAKE ON)
    FetchContent_MakeAvailable(boost_src)
endif()

# ── earcut.hpp (header-only polygon triangulation) ───────────────────────────
# Use Populate (not MakeAvailable) to avoid earcut's CMakeLists.txt, which
# would try to add a "glfw" target and conflict with our own GLFW.
FetchContent_Declare(earcut_hpp
    GIT_REPOSITORY https://github.com/mapbox/earcut.hpp.git
    GIT_TAG        v2.2.4
    GIT_SHALLOW    TRUE
)
FetchContent_GetProperties(earcut_hpp)
if(NOT earcut_hpp_POPULATED)
    FetchContent_Populate(earcut_hpp)
    add_library(earcut INTERFACE)
    target_include_directories(earcut INTERFACE ${earcut_hpp_SOURCE_DIR}/include)
endif()

# ── libcurl ───────────────────────────────────────────────────────────────────
find_package(CURL REQUIRED)

# ── OpenGL ────────────────────────────────────────────────────────────────────
find_package(OpenGL REQUIRED)

# ── ONNX Runtime (optional, for local AI inference) ───────────────────────────
# Resolution order:
#   1. A system / vcpkg install exposing the onnxruntime CMake config.
#   2. (Windows) Microsoft's official prebuilt binaries, fetched on demand.
# Either way we end up with the imported target onnxruntime::onnxruntime and the
# CPPOSMUI_HAVE_ONNXRUNTIME compile definition.  If neither is available the
# feature is silently disabled.
set(CPPOSMUI_ONNX FALSE)
find_package(onnxruntime QUIET)
if(onnxruntime_FOUND)
    set(CPPOSMUI_ONNX TRUE)
    message(STATUS "ONNX Runtime found (package) — local AI inference enabled")
elseif(WIN32)
    set(ORT_VERSION 1.20.1)
    message(STATUS "Fetching prebuilt ONNX Runtime ${ORT_VERSION} (win-x64)…")
    FetchContent_Declare(onnxruntime_bin
        URL https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/onnxruntime-win-x64-${ORT_VERSION}.zip
    )
    FetchContent_MakeAvailable(onnxruntime_bin)
    add_library(onnxruntime::onnxruntime SHARED IMPORTED)
    set_target_properties(onnxruntime::onnxruntime PROPERTIES
        IMPORTED_LOCATION             "${onnxruntime_bin_SOURCE_DIR}/lib/onnxruntime.dll"
        IMPORTED_IMPLIB               "${onnxruntime_bin_SOURCE_DIR}/lib/onnxruntime.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${onnxruntime_bin_SOURCE_DIR}/include")
    set(ORT_RUNTIME_DLL "${onnxruntime_bin_SOURCE_DIR}/lib/onnxruntime.dll")
    set(CPPOSMUI_ONNX TRUE)
    message(STATUS "ONNX Runtime (prebuilt) — local AI inference enabled")
else()
    message(STATUS "ONNX Runtime not found — local AI inference disabled "
                   "(install onnxruntime and re-run cmake to enable)")
endif()

# ── Shared library sources (used by both the app and tests) ───────────────────
file(GLOB_RECURSE LIB_SOURCES CONFIGURE_DEPENDS
    src/osm/*.cpp
    src/net/*.cpp
    src/map/*.cpp
    src/ui/*.cpp
    src/ai/*.cpp
)

add_library(cpposmui_lib STATIC ${LIB_SOURCES})
target_include_directories(cpposmui_lib PUBLIC src)
target_link_libraries(cpposmui_lib PUBLIC
    imgui
    glfw
    glad_gl33
    OpenGL::GL
    pugixml
    stb
    earcut
    nlohmann_json::nlohmann_json
    CURL::libcurl
)
if(Boost_FOUND)
    target_link_libraries(cpposmui_lib PUBLIC Boost::asio Boost::geometry Boost::system)
else()
    target_link_libraries(cpposmui_lib PUBLIC boost_asio boost_geometry boost_system)
endif()
if(WIN32)
    target_link_libraries(cpposmui_lib PUBLIC ws2_32 mswsock)
endif()
if(CPPOSMUI_ONNX)
    target_link_libraries(cpposmui_lib PUBLIC onnxruntime::onnxruntime)
    target_compile_definitions(cpposmui_lib PUBLIC CPPOSMUI_HAVE_ONNXRUNTIME)
endif()

target_compile_definitions(cpposmui_lib PUBLIC
    BOOST_ASIO_NO_DEPRECATED
    _WIN32_WINNT=0x0A00   # Windows 10+ for Asio
)
if(WIN32)
    # Stop <windows.h> (pulled in by curl/asio) from defining min/max macros
    # that collide with std::min/std::max.
    # _CRT_SECURE_NO_WARNINGS silences MSVC deprecation of getenv/strncpy/etc.
    target_compile_definitions(cpposmui_lib PUBLIC
        NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()

target_compile_options(cpposmui_lib PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:/W3>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra>
)

# ── Executable ────────────────────────────────────────────────────────────────
add_executable(cpposmui src/main.cpp)
target_link_libraries(cpposmui PRIVATE cpposmui_lib)

# Copy the prebuilt ONNX Runtime DLL next to the executable so it loads at runtime.
if(DEFINED ORT_RUNTIME_DLL)
    add_custom_command(TARGET cpposmui POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ORT_RUNTIME_DLL}" "$<TARGET_FILE_DIR:cpposmui>"
        COMMENT "Copying onnxruntime.dll next to cpposmui.exe")
endif()

# ── Tests ─────────────────────────────────────────────────────────────────────
enable_testing()
add_subdirectory(tests)
