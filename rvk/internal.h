// Shared between rvk's implementation files.
#pragma once

#include "rvk.h"

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vma/vk_mem_alloc.h>

#include <string>

namespace rvk::detail {

// Per-draw constants, std140; must match shaders/constants.glsl.
struct GpuLight {
    float diffuse[4], specular[4], ambient[4], position[4], direction[4], atten[4], spot[4];
};
struct DrawConstants {
    d3d::Matrix view, proj, texMatrix[2];
    float viewport[4];
    float matDiffuse[4], matAmbient[4], matSpecular[4], matEmissive[4];
    float ambient[4];
    float fogColor[4];
    float fogParams[4];
    float tfactor[4];
    float misc[4];
    float eyePos[4];
    float eyeDir[4];
    uint32_t vtx[4];
    uint32_t flags[4];
    uint32_t matSources[4];
    uint32_t stageA[2][4];
    uint32_t stageB[2][4];
    uint32_t lightInfo[4];
    GpuLight lights[Device::kMaxLights];
};
static_assert(sizeof(DrawConstants) % 16 == 0, "std140 block size");
struct DrawTransform {
    d3d::Matrix world;
    d3d::Matrix prevWorld;         // motion vectors: the object's world matrix last frame
    float motion[4];               // x: 1 = world camera; y: 1 = previous positions (binding 8); z: base vertex
    float sway[4];                 // plants: model y of the base, 1 / model height, tip sway (world units), 1 = on
    uint32_t lightMask[4];         // frame lights (bits 0-63 of x, y) that reach the draw's bounding box; z: pushers
                                   // (FrameLights.pushers) near it
};

enum : uint32_t { F_LIGHTING = 1, F_COLORVERTEX = 2, F_SPECULAR = 4, F_NORMALIZE = 8, F_FOG = 16, F_RANGEFOG = 32,
                  F_LOCALVIEWER = 64, F_TEX0 = 128, F_TEX1 = 256, F_ALPHATEST = 512,
                  F_PERPIXEL = 1024, F_DEBUGLIGHT = 2048, F_LIGHTOVERRIDE = 4096,
                  F_SHADOW = 8192, F_SHADOWCOMP = 16384,
                  F_SHADOWTEX = 32768, F_OVERBRIGHT = 65536, F_OVERBRIGHT2X = 131072,
                  F_HDR = 262144, F_GLOW = 524288, F_GLOWALPHA = 1048576,
                  F_BUMP = 2097152, F_BUMPBASE = 4194304, F_FOLIAGE = 8388608, F_EMISSIVE = 16777216,
                  F_CUTOUT = 33554432, F_SHADOWCHEAP = 67108864, F_VERTEXSUN = 134217728 };

constexpr uint32_t kFrameLights = 64;
constexpr uint32_t kPushers = 16;            // info.y of them used
struct FrameLights {               // binding 4: per-frame data (constants.glsl FrameLights)
    uint32_t info[4];
    d3d::Matrix shadowViewProj[4]; // world -> each sun shadow cascade
    float cascadeTexel[4];         // world size of a texel of each cascade
    float cascadeDepth[4];         // world units per unit of each cascade's depth (soft shadows)
    float effects[4];              // light through leaves, night glow (x darkness), sun shadow softness, plant push
    float wind[4];                 // plants' sway: direction x, z, time (s), strength
    float taa[4];                  // temporal anti-aliasing: this frame's jitter (clip x, y per w), noise offset; wind time last frame
    float shadowParams[4];         // enabled, strength, cascade count, point light shadow strength
    float sunDir[4];               // w: light headroom (F_OVERBRIGHT)
    float sunColor[4];             // the shadow-casting sun's colour (0 = none)
    d3d::Matrix prevViewProj;      // motion vectors: the world camera last frame
    float pushers[kPushers][4];    // what plants bend away from (characters' feet and trails): world x, y, z, seconds
                                   // since a character was there
    float pusherBorn[kPushers];    // seconds since each point was made (a character walking on makes new ones)
    GpuLight lights[kFrameLights];
};

// Vertex layout of a D3D flexible vertex format.
struct FvfLayout {
    uint32_t stride = 0;
    int offset[6] = {-1, -1, -1, -1, -1, -1};   // position, normal, diffuse, specular, tex0, tex1
    VkFormat format[6] = {};
};
FvfLayout DecodeFvf(uint32_t fvf);

// Vulkan format and view swizzle for a texture format.
struct FormatInfo {
    VkFormat vk;
    VkComponentMapping swizzle;
    uint32_t blockBytes;    // bytes per pixel, or per 4x4 block for compressed formats
    bool compressed;
};
const FormatInfo& GetFormatInfo(Format format);

bool Check(VkResult r, const char* what, std::string* error);
uint64_t HashBytes(const void* data, size_t size, uint64_t h);   // fast 64-bit hash of a byte range
void ImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                  VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                  VkAccessFlags2 dstAccess);
d3d::Matrix Identity();
d3d::Matrix MulMatrix(const d3d::Matrix& a, const d3d::Matrix& b);   // D3D order: v * a * b
bool InvertMatrix(const d3d::Matrix& m, d3d::Matrix* out);
VkPrimitiveTopology TopologyOf(uint32_t d3dPrimitive);
uint32_t TopologyClassOf(uint32_t d3dPrimitive);   // 0 points, 1 lines, 2 triangles
// Clip-space test of a world-space box's corners against a view-projection: +1 all inside, -1 all outside one
// plane, 0 otherwise. depth: also test the near and far planes.
int BoxInClip(const float mn[3], const float mx[3], const d3d::Matrix& vp, bool depth);

}  // namespace rvk::detail
