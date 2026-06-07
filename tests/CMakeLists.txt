file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS *.cpp)

# Tests don't need OpenGL/ImGui/GLFW – link only the data/net/osm parts
add_library(cpposmui_core STATIC
    ${CMAKE_SOURCE_DIR}/src/osm/parser.cpp
    ${CMAKE_SOURCE_DIR}/src/osm/diff.cpp
    ${CMAKE_SOURCE_DIR}/src/osm/spatial_index.cpp
    ${CMAKE_SOURCE_DIR}/src/net/http.cpp
    ${CMAKE_SOURCE_DIR}/src/net/overpass.cpp
)
target_include_directories(cpposmui_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(cpposmui_core PUBLIC
    pugixml
    nlohmann_json::nlohmann_json
    CURL::libcurl
)
if(Boost_FOUND)
    target_link_libraries(cpposmui_core PUBLIC Boost::asio Boost::geometry Boost::system)
else()
    target_link_libraries(cpposmui_core PUBLIC boost_asio boost_geometry boost_system)
endif()
if(WIN32)
    target_link_libraries(cpposmui_core PUBLIC ws2_32 mswsock)
endif()
target_compile_definitions(cpposmui_core PUBLIC BOOST_ASIO_NO_DEPRECATED _WIN32_WINNT=0x0A00)
if(WIN32)
    target_compile_definitions(cpposmui_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# Copy test fixture data
configure_file(data/simple.osm  ${CMAKE_CURRENT_BINARY_DIR}/data/simple.osm  COPYONLY)
configure_file(data/changes.osc ${CMAKE_CURRENT_BINARY_DIR}/data/changes.osc COPYONLY)

add_executable(cpposmui_tests ${TEST_SOURCES})
target_link_libraries(cpposmui_tests PRIVATE
    cpposmui_core
    Catch2::Catch2WithMain
)
target_compile_definitions(cpposmui_tests PRIVATE
    TEST_DATA_DIR="${CMAKE_CURRENT_BINARY_DIR}/data"
)

include(CTest)
include(Catch)
catch_discover_tests(cpposmui_tests)
