// stb_image, one implementation for the whole DLL: the native bitmap loaders (lbitmap.cpp) and the side-loaded
// material images (proxy/ddraw/rvk_materials.cpp) both decode through it. Only the formats they need.
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"
