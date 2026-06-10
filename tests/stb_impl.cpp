// stb_image implementation for the test binary.
// In the app this lives in src/map/tile_cache.cpp, which links OpenGL and is
// therefore not part of cpposmui_core. detection.cpp only declares the stbi_*
// functions; this TU satisfies them at link time.
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
