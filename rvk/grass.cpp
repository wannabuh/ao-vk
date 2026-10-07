// Procedural ground grass (RVK_GrassOn): our own blades over the game's terrain, an addition on top of its dated
// foliage cards.
//
// The ground: the terrain's own draws are captured (CaptureTerrain) into world-aligned tiles of quarter-unit cells near
// the camera - from the base pass each cell's height and its texture's colour (grass or not), from the light pass its
// lightmap texel (the baked light that makes the ground bright or dark). A cell keeps the finest terrain triangle that
// covered it, and a capture block whose cells are all known at least that finely skips the triangle, so once the
// ground near the camera is known a frame's capture costs a bounding-box test per terrain triangle.
//
// The blades: once a tile's ground has settled its blades are baked (BuildGrassTile) as 32-byte records into one GPU
// pool; a tile is rebuilt only when its ground changes (a finer level of detail, a newly seen part, a re-uploaded
// lightmap). The vertex shader (grass.vert) expands each record into a tapered strip - five triangles near, three far -
// bends it with the wind and the characters walking through, and lights it per vertex the way the terrain is lit
// (lightmap + global ambient, the sun's shadow, local lights), so the grass is as bright as its ground by day and by
// night. Drawn into the scene before the game's blended draws (DrawGrassTiles). Off by default: with RVK_GrassOn off
// nothing is captured, built or drawn.
#include "internal.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace rvk {

using namespace vk;
using namespace detail;

namespace {

const uint32_t kGrassVertSpirv[] = {
#include "grass.vert.inc"
};
const uint32_t kGrassFragSpirv[] = {
#include "grass.frag.inc"
};

// The pass's frame block (grass.vert / grass.frag GrassFrame), one UBO.
struct GrassFrame {
    d3d::Matrix viewProj;
    d3d::Matrix prevViewProj;
    float viewport[4];   // xy: target size in pixels; z: the field radius; w: the wind clock last frame
    float wind[4];       // x: time (s); yz: the wind's direction; w: strength
    float camera[4];     // xyz: the camera; w: 1 = the TAA's jitter applies
    float ambient[4];    // rgb: the terrain's global ambient; w: 1 = captured this frame
    float look[4];       // x: brightness; y: local light scale
    float sunColour[4];  // rgb: the directional light the terrain's light pass takes (none with the light override)
    float sunDir[4];     // xyz: the direction it travels
};
static_assert(sizeof(GrassFrame) == 240, "grass frame block");

// One blade in the pool (grass.vert Blade).
struct GrassBlade {
    float x, y, z;       // the root
    uint32_t up;         // the blade's axis: x, z as snorm16 (y = the rest)
    uint32_t shape;      // height (unorm16 x 4 units) | half width (unorm8 x 0.25) << 16 | droop (unorm8) << 24
    uint32_t yawPhase;   // yaw (unorm16 x 2 pi) | wind phase (unorm16 x 20 pi) << 16
    uint32_t tint;       // RGB (0xRRGGBB)
    uint32_t light;      // the lightmap's RGB at the root; A nonzero = captured
};
static_assert(sizeof(GrassBlade) == 32, "grass blade record");

// The shared index pattern: vertex v of blade b is 8 b + v (cross section v >> 1, edge v & 1; 7 is unused). The far
// pattern skips the first third's cross section: the same blade, its curve in two pieces.
constexpr uint32_t kNearIndices = 15;    // three segments: 2 + 2 triangles and the tip
constexpr uint32_t kFarIndices = 9;      // two segments: root, two thirds up, the tip
constexpr uint32_t kNearPattern[kNearIndices] = {0, 1, 2, 1, 3, 2, 2, 3, 4, 3, 5, 4, 4, 5, 6};
constexpr uint32_t kFarPattern[kFarIndices] = {0, 1, 4, 1, 5, 4, 4, 5, 6};

// A cheap, position-stable hash so a blade stays put frame to frame (needed by the temporal anti-aliasing).
uint32_t HashCell(int32_t x, int32_t z)
{
    uint64_t h = uint64_t(uint32_t(x)) * 0x9E3779B97F4A7C15ull ^ uint64_t(uint32_t(z)) * 0xC2B2AE3D27D4EB4Full;
    h ^= h >> 29;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 32;
    return uint32_t(h);
}

float Unit(uint32_t h) { return float(h & 0xFFFFFFu) / float(0x1000000u); }

// Smooth value noise in [0, 1] over the world.
float HashF(int32_t x, int32_t z) { return Unit(HashCell(x, z)); }
float ValueNoise(float x, float z)
{
    const int32_t xi = int32_t(std::floor(x)), zi = int32_t(std::floor(z));
    float fx = x - float(xi), fz = z - float(zi);
    fx = fx * fx * (3.0f - 2.0f * fx);
    fz = fz * fz * (3.0f - 2.0f * fz);
    const float a = HashF(xi, zi), b = HashF(xi + 1, zi), c = HashF(xi, zi + 1), d = HashF(xi + 1, zi + 1);
    return (a * (1.0f - fx) + b * fx) * (1.0f - fz) + (c * (1.0f - fx) + d * fx) * fz;
}

// The clump noise for the grass's density. Two octaves, each sampled in a rotated frame: value noise alone is aligned
// to the world axes, so its dense and sparse bands line up with them and read as rows at a grazing angle.
float ClumpNoise(float x, float z)
{
    constexpr float c = 0.86602540f, s = 0.5f;   // 30 degrees
    const float a = ValueNoise((x * c - z * s) * 0.075f, (x * s + z * c) * 0.075f);
    const float b = ValueNoise((x * s + z * c) * 0.19f + 11.3f, (x * c - z * s) * 0.19f + 7.1f);
    return a * 0.7f + b * 0.3f;
}

// A terrain triangle's size class from its area on the ground: smaller = a finer level of detail. Half octaves.
uint8_t TriangleRank(float area)
{
    const int r = int(std::floor(std::log2(std::max(area, 1e-6f)) * 2.0f)) + 60;
    return uint8_t(std::clamp(r, 0, 254));
}

bool GreenRgb(const uint8_t c[3])
{
    return c[1] > 24 && int(c[1]) * 100 > int(c[0]) * 112 && int(c[1]) * 100 > int(c[2]) * 112;
}

int32_t FloorDiv(int32_t a, int32_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

uint64_t TileKey(int32_t tx, int32_t tz) { return (uint64_t(uint32_t(tx)) << 32) | uint32_t(tz); }

// VmaVirtualAllocation is a 64-bit handle (a pointer on 64-bit builds), kept as uint64_t in rvk.h.
uint64_t FromVa(VmaVirtualAllocation a)
{
    uint64_t v = 0;
    static_assert(sizeof(a) <= sizeof(v), "virtual allocation handle");
    std::memcpy(&v, &a, sizeof(a));
    return v;
}
VmaVirtualAllocation ToVa(uint64_t v)
{
    VmaVirtualAllocation a{};
    std::memcpy(&a, &v, sizeof(a));
    return a;
}

}  // namespace

// The texture's colour at (u, v), RGB 0..255, from the copy taken at upload (resources.cpp PixelsThumbnail): nearest.
bool Texture::TexelRgbAt(float u, float v, uint8_t rgb[3]) const
{
    if (!m_thumbValid || !m_thumbW || !m_thumbH)
        return false;
    const float fu = u - std::floor(u), fv = v - std::floor(v);   // wrap
    const uint32_t i = uint32_t(std::clamp(int(fu * float(m_thumbW)), 0, int(m_thumbW) - 1));
    const uint32_t j = uint32_t(std::clamp(int(fv * float(m_thumbH)), 0, int(m_thumbH) - 1));
    const uint8_t* c = m_thumb.data() + (size_t(j) * m_thumbW + i) * 3;
    rgb[0] = c[0]; rgb[1] = c[1]; rgb[2] = c[2];
    return true;
}

// ... filtered between the four nearest samples (wrapping), as the GPU samples the lightmap.
bool Texture::TexelRgbBilinear(float u, float v, uint8_t rgb[3]) const
{
    if (!m_thumbValid || !m_thumbW || !m_thumbH)
        return false;
    const float fu = (u - std::floor(u)) * float(m_thumbW) - 0.5f, fv = (v - std::floor(v)) * float(m_thumbH) - 0.5f;
    const float bu = std::floor(fu), bv = std::floor(fv), tu = fu - bu, tv = fv - bv;
    const int W = int(m_thumbW), H = int(m_thumbH);
    const int i0 = ((int(bu) % W) + W) % W, j0 = ((int(bv) % H) + H) % H;
    const int i1 = (i0 + 1) % W, j1 = (j0 + 1) % H;
    const uint8_t* a = m_thumb.data() + (size_t(j0) * W + i0) * 3;
    const uint8_t* b = m_thumb.data() + (size_t(j0) * W + i1) * 3;
    const uint8_t* c = m_thumb.data() + (size_t(j1) * W + i0) * 3;
    const uint8_t* d = m_thumb.data() + (size_t(j1) * W + i1) * 3;
    for (int k = 0; k < 3; ++k) {
        const float top = float(a[k]) * (1.0f - tu) + float(b[k]) * tu;
        const float bottom = float(c[k]) * (1.0f - tu) + float(d[k]) * tu;
        rgb[k] = uint8_t(std::clamp(top * (1.0f - tv) + bottom * tv + 0.5f, 0.0f, 255.0f));
    }
    return true;
}

Device::GrassTile* Device::GrassTileAt(int32_t tx, int32_t tz) const
{
    auto it = m_grassTiles.find(TileKey(tx, tz));
    return it == m_grassTiles.end() ? nullptr : it->second.get();
}

// Records the terrain's ground into the grass tiles near the camera (UpdateGrassTiles makes them). Called for every
// terrain draw (IsTerrain); takes the base pass (unlit, opaque: its stage-0 texture is the ground's own, coordinate set
// 0) and the light pass (lit, multiplying: stage 0 is the lightmap, coordinate set 1). Each cell is filled from the
// triangle that contains its centre - height and texture coordinate interpolated - if that triangle is finer than what
// the cell has (the ground's coarse levels of detail overlap its fine ones). With RVK_GrassTex off, every terrain cell
// counts as grass.
void Device::CaptureTerrain(uint32_t primitive, const FvfLayout& layout, const void* vertices, uint32_t vertexCount,
                            const uint16_t* indices, uint32_t indexCount)
{
    if (!m_grassOn || !vertices || layout.offset[0] < 0)
        return;
    if (primitive < d3d::TriangleList || primitive > d3d::TriangleFan)
        return;
    const bool lightPass = m_rs[d3d::RS_LIGHTING] && IsMultiplyPass();
    const bool basePass = !m_rs[d3d::RS_LIGHTING] && !m_rs[d3d::RS_ALPHABLENDENABLE];
    if (!lightPass && !basePass)
        return;
    const int uvSet = lightPass ? 5 : 4;
    if (layout.offset[uvSet] < 0)
        return;
    Texture* tex = m_textures[0];
    if (lightPass) {
        // The light pass's global ambient (D3D: RS_AMBIENT x the material's ambient), which the grass adds to the
        // lightmap as the ground does - it carries much of the time of day.
        float amb[4];
        const uint32_t c = m_rs[d3d::RS_AMBIENT];
        amb[0] = float((c >> 16) & 0xFF) / 255.0f;
        amb[1] = float((c >> 8) & 0xFF) / 255.0f;
        amb[2] = float(c & 0xFF) / 255.0f;
        m_terrainAmbient[0] = amb[0] * m_material.ambient.r;
        m_terrainAmbient[1] = amb[1] * m_material.ambient.g;
        m_terrainAmbient[2] = amb[2] * m_material.ambient.b;
        m_terrainAmbient[3] = 1.0f;
        // ... and its directional light: the scene keeps it off the ground under the light override (draw.cpp: the
        // lightmap has the sun baked in), otherwise D3D lights the ground with it as with any light.
        std::memset(m_terrainSun, 0, sizeof(m_terrainSun));
        if (!(m_pixelLighting && m_lightOverride))
            for (const LightSlot& slot : m_lights)
                if (slot.enabled && slot.light.type == d3d::LIGHT_DIRECTIONAL) {
                    const float k = LightScale(slot.light);
                    m_terrainSun[0] += slot.light.diffuse.r * k;
                    m_terrainSun[1] += slot.light.diffuse.g * k;
                    m_terrainSun[2] += slot.light.diffuse.b * k;
                    m_terrainSun[4] = slot.light.direction.x;
                    m_terrainSun[5] = slot.light.direction.y;
                    m_terrainSun[6] = slot.light.direction.z;
                }
        if (!tex || !tex->m_thumbValid)
            return;                              // no lightmap to read (an unsupported format): the shader's fallback
        tex->m_lightmap = true;
    }
    if (m_grassTiles.empty())
        return;
    const double since = ProfileCpu();
    // A chunk already captured is skipped whole unless a tile under it is newer than that capture (made since, or its
    // light reset): drawing the same triangles again would add nothing - a cell only ever takes a finer one. Chunks are
    // known by their contents (the ground's level of detail changes them), the pass and the texture.
    const uint64_t chunkKey = TerrainChunkKey(vertices, vertexCount, uint32_t(layout.stride), indexCount) ^
                              (lightPass ? 0x9E3779B97F4A7C15ull : 0) ^
                              uint64_t(reinterpret_cast<uintptr_t>(tex)) * 0x100000001B3ull;
    GrassChunk& chunk = m_grassChunks[chunkKey];
    chunk.lastSeen = m_frameNumber;
    if (chunk.processed && chunk.processed > m_grassTileEpoch) {
        m_grassCaptureMs += ProfileCpu() - since;
        return;
    }
    if (!chunk.boxed) {
        chunk.box[0] = chunk.box[1] = 1e30f;
        chunk.box[2] = chunk.box[3] = -1e30f;
        for (uint32_t i = 0; i < vertexCount; ++i) {
            float p[3];
            std::memcpy(p, static_cast<const uint8_t*>(vertices) + size_t(i) * layout.stride + layout.offset[0], 12);
            chunk.box[0] = std::min(chunk.box[0], p[0]);
            chunk.box[1] = std::min(chunk.box[1], p[2]);
            chunk.box[2] = std::max(chunk.box[2], p[0]);
            chunk.box[3] = std::max(chunk.box[3], p[2]);
        }
        chunk.boxed = true;
    }
    // The tiles under the chunk, in a small grid (no map lookups per triangle); only those made or reset since the
    // chunk was last captured take its triangles.
    const int32_t gx0 = std::max(int32_t(std::floor(chunk.box[0] / kGrassTileSize)), m_grassTileBox[0]);
    const int32_t gz0 = std::max(int32_t(std::floor(chunk.box[1] / kGrassTileSize)), m_grassTileBox[1]);
    const int32_t gx1 = std::min(int32_t(std::floor(chunk.box[2] / kGrassTileSize)), m_grassTileBox[2]);
    const int32_t gz1 = std::min(int32_t(std::floor(chunk.box[3] / kGrassTileSize)), m_grassTileBox[3]);
    const uint64_t capturedAt = chunk.processed;
    chunk.processed = m_frameNumber + 1;         // (the tiles' clock: a tile made later this frame gets it next frame)
    if (gx0 > gx1 || gz0 > gz1) {
        m_grassCaptureMs += ProfileCpu() - since;
        return;
    }
    const int32_t gw = gx1 - gx0 + 1, gh = gz1 - gz0 + 1;
    std::vector<GrassTile*>& grid = m_grassChunkGrid;
    grid.assign(size_t(gw) * gh, nullptr);
    bool any = false;
    for (int32_t tz = gz0; tz <= gz1; ++tz)
        for (int32_t tx = gx0; tx <= gx1; ++tx) {
            GrassTile* t = GrassTileAt(tx, tz);
            if (t && (capturedAt == 0 || std::max(t->created, lightPass ? t->lightReset : 0) >= capturedAt)) {
                grid[size_t(tz - gz0) * gw + (tx - gx0)] = t;
                any = true;
            }
        }
    if (!any) {
        m_grassCaptureMs += ProfileCpu() - since;
        return;
    }
    const float gridX0 = float(gx0) * kGrassTileSize, gridZ0 = float(gz0) * kGrassTileSize;
    const float gridX1 = float(gx1 + 1) * kGrassTileSize, gridZ1 = float(gz1 + 1) * kGrassTileSize;
    const bool filter = basePass && m_grassTex && tex;
    const uint8_t* src = static_cast<const uint8_t*>(vertices);
    const size_t stride = layout.stride, posOff = size_t(layout.offset[0]), uvOff = size_t(layout.offset[uvSet]);
    const uint32_t count = indices ? indexCount : vertexCount;
    auto at = [&](uint32_t i) -> uint32_t { return indices ? indices[i] : i; };
    auto tileAt = [&](int32_t tx, int32_t tz) -> GrassTile* {
        if (tx < gx0 || tx > gx1 || tz < gz0 || tz > gz1)
            return nullptr;
        return grid[size_t(tz - gz0) * gw + (tx - gx0)];
    };
    constexpr int kBlocksPerSide = kGroundN / kGroundBlock;
    auto triangle = [&](uint32_t a, uint32_t b, uint32_t c) {
        if (a >= vertexCount || b >= vertexCount || c >= vertexCount)
            return;
        float pa[3], pb[3], pc[3];
        std::memcpy(pa, src + size_t(a) * stride + posOff, 12);
        std::memcpy(pb, src + size_t(b) * stride + posOff, 12);
        std::memcpy(pc, src + size_t(c) * stride + posOff, 12);
        const float minX = std::min({pa[0], pb[0], pc[0]}), maxX = std::max({pa[0], pb[0], pc[0]});
        const float minZ = std::min({pa[2], pb[2], pc[2]}), maxZ = std::max({pa[2], pb[2], pc[2]});
        if (maxX < gridX0 || minX >= gridX1 || maxZ < gridZ0 || minZ >= gridZ1)
            return;                              // not over a tile that wants it
        const float d = (pb[2] - pc[2]) * (pa[0] - pc[0]) + (pc[0] - pb[0]) * (pa[2] - pc[2]);
        if (std::fabs(d) < 1e-6f)
            return;                              // degenerate in x, z (nothing to cover)
        const int32_t tx0 = int32_t(std::floor(minX / kGrassTileSize)), tx1 = int32_t(std::floor(maxX / kGrassTileSize));
        const int32_t tz0 = int32_t(std::floor(minZ / kGrassTileSize)), tz1 = int32_t(std::floor(maxZ / kGrassTileSize));
        uint8_t rank = 0;
        bool ranked = false;
        float ua[2] = {}, ub[2] = {}, uc[2] = {};
        bool uvs = false;
        for (int32_t tz = tz0; tz <= tz1; ++tz)
            for (int32_t tx = tx0; tx <= tx1; ++tx) {
                GrassTile* tile = tileAt(tx, tz);
                if (!tile)
                    continue;
                const float x0 = float(tx) * kGrassTileSize, z0 = float(tz) * kGrassTileSize;
                const int cx0 = std::max(0, int(std::floor((minX - x0) / kGroundCell)));
                const int cx1 = std::min(kGroundN - 1, int(std::floor((maxX - x0) / kGroundCell)));
                const int cz0 = std::max(0, int(std::floor((minZ - z0) / kGroundCell)));
                const int cz1 = std::min(kGroundN - 1, int(std::floor((maxZ - z0) / kGroundCell)));
                if (cx0 > cx1 || cz0 > cz1)
                    continue;
                if (!ranked) {
                    rank = TriangleRank(0.5f * std::fabs(d));
                    ranked = true;
                }
                // Skip the triangle if every block it touches already knows its cells at least this finely.
                const uint8_t* blockRank = lightPass ? tile->blockLightRank : tile->blockRank;
                bool useful = false;
                for (int bz = cz0 / kGroundBlock; bz <= cz1 / kGroundBlock && !useful; ++bz)
                    for (int bx = cx0 / kGroundBlock; bx <= cx1 / kGroundBlock; ++bx)
                        if (blockRank[bz * kBlocksPerSide + bx] > rank) {
                            useful = true;
                            break;
                        }
                if (!useful)
                    continue;
                if (!uvs) {
                    std::memcpy(ua, src + size_t(a) * stride + uvOff, 8);
                    std::memcpy(ub, src + size_t(b) * stride + uvOff, 8);
                    std::memcpy(uc, src + size_t(c) * stride + uvOff, 8);
                    uvs = true;
                }
                uint8_t* ranks = lightPass ? tile->lightRank : tile->rank;
                bool changed = false;
                for (int cz = cz0; cz <= cz1; ++cz)
                    for (int cx = cx0; cx <= cx1; ++cx) {
                        const int idx = cz * kGroundN + cx;
                        const uint8_t cur = ranks[idx];
                        if (cur != 255 && rank >= cur)
                            continue;                // known as finely already
                        const float qx = x0 + (float(cx) + 0.5f) * kGroundCell, qz = z0 + (float(cz) + 0.5f) * kGroundCell;
                        const float l0 = ((pb[2] - pc[2]) * (qx - pc[0]) + (pc[0] - pb[0]) * (qz - pc[2])) / d;
                        const float l1 = ((pc[2] - pa[2]) * (qx - pc[0]) + (pa[0] - pc[0]) * (qz - pc[2])) / d;
                        const float l2 = 1.0f - l0 - l1;
                        if (l0 < -0.001f || l1 < -0.001f || l2 < -0.001f)
                            continue;
                        const float u = l0 * ua[0] + l1 * ub[0] + l2 * uc[0];
                        const float v = l0 * ua[1] + l1 * ub[1] + l2 * uc[1];
                        ranks[idx] = rank;
                        if (lightPass) {
                            uint8_t l[3] = {0, 0, 0};
                            tex->TexelRgbBilinear(u, v, l);   // (readable: checked above)
                            const uint32_t light = uint32_t(l[0]) << 16 | uint32_t(l[1]) << 8 | l[2] | 0xFF000000u;
                            const uint32_t old = tile->light[idx];
                            tile->light[idx] = light;
                            if (cur == 255 || std::abs(int(old >> 16 & 0xFF) - int(l[0])) > 3 ||
                                std::abs(int(old >> 8 & 0xFF) - int(l[1])) > 3 || std::abs(int(old & 0xFF) - int(l[2])) > 3)
                                changed = true;
                            continue;
                        }
                        const float y = l0 * pa[1] + l1 * pb[1] + l2 * pc[1];
                        uint8_t rgb[3] = {0x3C, 0x6A, 0x2A};      // a default grass green (no filter, or no texel read)
                        bool have = false;
                        if (filter)
                            have = tex->TexelRgbAt(u, v, rgb);
                        const bool grass = !filter || (have && GreenRgb(rgb));
                        const uint32_t colour = uint32_t(rgb[0]) << 16 | uint32_t(rgb[1]) << 8 | rgb[2] |
                                                (grass ? 1u << 24 : 0u);
                        if (cur == 255) {
                            ++tile->seen;
                            changed = true;
                        } else if (std::fabs(tile->y[idx] - y) > 0.02f || ((tile->colour[idx] ^ colour) >> 24)) {
                            changed = true;
                        }
                        tile->y[idx] = y;
                        tile->colour[idx] = colour;
                    }
                // The blocks' coarsest rank again (only those this triangle touched).
                uint8_t* blocks = lightPass ? tile->blockLightRank : tile->blockRank;
                for (int bz = cz0 / kGroundBlock; bz <= cz1 / kGroundBlock; ++bz)
                    for (int bx = cx0 / kGroundBlock; bx <= cx1 / kGroundBlock; ++bx) {
                        uint8_t worst = 0;
                        for (int z = 0; z < kGroundBlock && worst != 255; ++z) {
                            const uint8_t* row = ranks + (bz * kGroundBlock + z) * kGroundN + bx * kGroundBlock;
                            for (int x = 0; x < kGroundBlock; ++x)
                                worst = std::max(worst, row[x]);
                        }
                        blocks[bz * kBlocksPerSide + bx] = worst;
                    }
                if (changed) {
                    ++tile->version;
                    tile->lastChange = m_frameNumber;
                }
                if (lightPass) {                 // which lightmaps it holds (a re-upload recaptures the light)
                    bool known = false;
                    for (const void* p : tile->lightmaps)
                        known = known || p == tex;
                    for (const void*& p : tile->lightmaps)
                        if (!known && !p) {
                            p = tex;
                            known = true;
                        }
                }
            }
    };
    if (primitive == d3d::TriangleList)
        for (uint32_t i = 0; i + 2 < count; i += 3)
            triangle(at(i), at(i + 1), at(i + 2));
    else if (primitive == d3d::TriangleStrip)
        for (uint32_t i = 0; i + 2 < count; ++i)
            (i & 1) ? triangle(at(i + 1), at(i), at(i + 2)) : triangle(at(i), at(i + 1), at(i + 2));
    else
        for (uint32_t i = 1; i + 1 < count; ++i)
            triangle(at(0), at(i), at(i + 1));
    m_grassCaptureMs += ProfileCpu() - since;
}

// The ground at a world x, z: the nearest captured cell within two cells (a stray gap at the captured region's edge).
// False where no ground was captured or (with `grassOnly`) where the nearest ground isn't grass.
bool Device::GroundAt(float x, float z, float* y, uint32_t* colour, uint32_t* light) const
{
    const int32_t gx = int32_t(std::floor(x / kGroundCell)), gz = int32_t(std::floor(z / kGroundCell));
    const GrassTile* tile = nullptr;
    int32_t tileX = INT32_MIN, tileZ = INT32_MIN;
    for (int r = 0; r <= 2; ++r)
        for (int dz = -r; dz <= r; ++dz)
            for (int dx = -r; dx <= r; ++dx) {
                if (r > 0 && std::abs(dx) != r && std::abs(dz) != r)
                    continue;                    // only the ring at this radius
                const int32_t cx = gx + dx, cz = gz + dz;
                const int32_t tx = FloorDiv(cx, kGroundN), tz = FloorDiv(cz, kGroundN);
                if (tx != tileX || tz != tileZ) {
                    tileX = tx;
                    tileZ = tz;
                    tile = GrassTileAt(tx, tz);
                }
                if (!tile)
                    continue;
                const int idx = (cz - tz * kGroundN) * kGroundN + (cx - tx * kGroundN);
                if (tile->rank[idx] == 255)
                    continue;
                *y = tile->y[idx];
                if (colour)
                    *colour = tile->colour[idx];
                if (light)
                    *light = tile->lightRank[idx] != 255 ? tile->light[idx] : 0u;
                return true;
            }
    return false;
}

// The ground's upward normal at x, z, from the captured heights around it: blades grow along the slope, not straight
// up. Falls back to straight up where the ground is unknown.
void Device::GroundNormal(float x, float z, float out[3]) const
{
    constexpr float d = 0.6f;
    float yl, yr, yd, yu;
    const bool ox = GroundAt(x - d, z, &yl, nullptr, nullptr) && GroundAt(x + d, z, &yr, nullptr, nullptr);
    const bool oz = GroundAt(x, z - d, &yd, nullptr, nullptr) && GroundAt(x, z + d, &yu, nullptr, nullptr);
    out[0] = ox ? -(yr - yl) / (2.0f * d) : 0.0f;
    out[1] = 1.0f;
    out[2] = oz ? -(yu - yd) / (2.0f * d) : 0.0f;
    const float l = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
    out[0] /= l; out[1] /= l; out[2] /= l;
}

bool Device::CreateGrassResources(std::string* error)
{
    DestroyGrassResources();
    // One set: 0 the camera (uniform), 2 the blades, 4 the frame lights, 5 the sun's shadows, 6 the point lights'.
    VkDescriptorSetLayoutBinding b[5] = {};
    const uint32_t numbers[5] = {0, 2, 4, 5, 6};
    const VkDescriptorType types[5] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                       VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                       VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER};
    for (int i = 0; i < 5; ++i) {
        b[i].binding = numbers[i];
        b[i].descriptorType = types[i];
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | (i == 0 ? VK_SHADER_STAGE_FRAGMENT_BIT : 0);
    }
    VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    sl.bindingCount = 5;
    sl.pBindings = b;
    if (!Check(vkCreateDescriptorSetLayout(m_device, &sl, nullptr, &m_grassSetLayout),
               "vkCreateDescriptorSetLayout", error))
        return false;
    VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT, 0, 8};   // the per-tile frame-light mask (uvec2)
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &m_grassSetLayout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &range;
    if (!Check(vkCreatePipelineLayout(m_device, &pl, nullptr, &m_grassLayout), "vkCreatePipelineLayout", error))
        return false;
    auto module = [&](const uint32_t* code, size_t size, VkShaderModule* out) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = size;
        ci.pCode = code;
        return Check(vkCreateShaderModule(m_device, &ci, nullptr, out), "vkCreateShaderModule", error);
    };
    VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE;
    bool ok = module(kGrassVertSpirv, sizeof(kGrassVertSpirv), &vert) &&
              module(kGrassFragSpirv, sizeof(kGrassFragSpirv), &frag);
    if (ok) {
        VkPipelineShaderStageCreateInfo stages[2] = {
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vert, "main", nullptr},
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main", nullptr},
        };
        VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        ds.dynamicStateCount = 2;
        ds.pDynamicStates = dyn;
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;         // thin double-sided blades
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo dss{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        dss.depthTestEnable = VK_TRUE;
        dss.depthWriteEnable = VK_TRUE;
        dss.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        VkPipelineColorBlendAttachmentState att[5] = {};
        for (auto& a : att)
            a.colorWriteMask = 0xF;              // colour, glow, local fraction, motion, albedo: a solid surface
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 5;
        cb.pAttachments = att;
        VkFormat colorFormats[5] = {GetFormatInfo(Format::RGBA16F).vk, GetFormatInfo(Format::RGBA16F).vk,
                                    GetFormatInfo(Format::RGBA8).vk, GetFormatInfo(Format::RG16F).vk,
                                    GetFormatInfo(Format::A8R8G8B8).vk};
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = 5;
        rendering.pColorAttachmentFormats = colorFormats;
        rendering.depthAttachmentFormat = kDepthFormat;
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        ci.pNext = &rendering;
        ci.stageCount = 2;
        ci.pStages = stages;
        ci.pVertexInputState = &vi;
        ci.pInputAssemblyState = &ia;
        ci.pViewportState = &vp;
        ci.pRasterizationState = &rs;
        ci.pMultisampleState = &ms;
        ci.pDepthStencilState = &dss;
        ci.pColorBlendState = &cb;
        ci.pDynamicState = &ds;
        ci.layout = m_grassLayout;
        ok = Check(vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &ci, nullptr, &m_grassPipeline),
                   "vkCreateGraphicsPipelines", error);
    }
    if (vert) vkDestroyShaderModule(m_device, vert, nullptr);
    if (frag) vkDestroyShaderModule(m_device, frag, nullptr);
    return ok;
}

void Device::DestroyGrassResources()
{
    DestroyGrassTiles();
    if (m_grassPipeline) {
        vkDestroyPipeline(m_device, m_grassPipeline, nullptr);
        m_grassPipeline = VK_NULL_HANDLE;
    }
    if (m_grassLayout) {
        vkDestroyPipelineLayout(m_device, m_grassLayout, nullptr);
        m_grassLayout = VK_NULL_HANDLE;
    }
    if (m_grassSetLayout) {
        vkDestroyDescriptorSetLayout(m_device, m_grassSetLayout, nullptr);
        m_grassSetLayout = VK_NULL_HANDLE;
    }
}

// Everything the grass holds on the GPU and the ground it captured (the device is idle, or the grass is reset).
void Device::DestroyGrassTiles()
{
    m_grassTiles.clear();
    for (auto& t : m_grassTrash)
        if (t.buffer)
            vmaDestroyBuffer(m_allocator, t.buffer, t.allocation);
    m_grassTrash.clear();
    if (m_grassPoolBlock) {
        vmaClearVirtualBlock(m_grassPoolBlock);
        vmaDestroyVirtualBlock(m_grassPoolBlock);
        m_grassPoolBlock = nullptr;
    }
    if (m_grassPool) {
        vmaDestroyBuffer(m_allocator, m_grassPool, m_grassPoolAllocation);
        m_grassPool = VK_NULL_HANDLE;
        m_grassPoolAllocation = nullptr;
    }
    m_grassPoolBlades = 0;
    if (m_grassIndex) {
        vmaDestroyBuffer(m_allocator, m_grassIndex, m_grassIndexAllocation);
        m_grassIndex = VK_NULL_HANDLE;
        m_grassIndexAllocation = nullptr;
    }
    m_grassIndexBlades = 0;
}

// A tile's blades leave the pool once no frame in flight can still draw them.
void Device::FreeGrassBlades(GrassTile& tile)
{
    if (tile.alloc)
        m_grassTrash.push_back({tile.alloc, VK_NULL_HANDLE, nullptr, m_frameNumber});
    tile.alloc = 0;
    tile.first = tile.blades = tile.sparse = 0;
}

// The blade pool, at least `minBlades` big: a new, bigger buffer that the built tiles are copied into (the old one is
// freed once no frame in flight reads it).
bool Device::GrowGrassPool(uint32_t minBlades)
{
    uint32_t blades = std::max<uint32_t>(m_grassPoolBlades * 2, 1u << 17);   // 4 MB to start
    while (blades < minBlades)
        blades *= 2;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = VkDeviceSize(blades) * sizeof(GrassBlade);
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo ac{};
    ac.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    std::string err;
    if (!Check(vmaCreateBuffer(m_allocator, &bi, &ac, &buffer, &allocation, nullptr), "grass blade pool", &err))
        return false;
    VmaVirtualBlockCreateInfo vb{};
    vb.size = bi.size;
    VmaVirtualBlock block = nullptr;
    if (vmaCreateVirtualBlock(&vb, &block) != VK_SUCCESS) {
        vmaDestroyBuffer(m_allocator, buffer, allocation);
        return false;
    }
    // Copy the built tiles across (after the staging copies already recorded into the old pool).
    std::vector<VkBufferCopy> regions;
    for (auto& [key, tp] : m_grassTiles) {
        GrassTile& tile = *tp;
        if (!tile.alloc)
            continue;
        VmaVirtualAllocationCreateInfo ai{};
        ai.size = VkDeviceSize(tile.blades) * sizeof(GrassBlade);
        ai.alignment = sizeof(GrassBlade);       // offsets are in whole blades
        VmaVirtualAllocation a{};
        VkDeviceSize offset = 0;
        if (vmaVirtualAllocate(block, &ai, &a, &offset) != VK_SUCCESS) {   // can't happen: the new pool is bigger
            tile.alloc = 0;
            tile.built = false;
            continue;
        }
        regions.push_back({VkDeviceSize(tile.first) * sizeof(GrassBlade), offset, ai.size});
        tile.alloc = FromVa(a);
        tile.first = uint32_t(offset / sizeof(GrassBlade));
    }
    if (m_grassPool) {
        VkCommandBuffer cmd = UploadCommands();
        if (!regions.empty()) {
            VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            mb.srcStageMask = mb.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.memoryBarrierCount = 1;
            dep.pMemoryBarriers = &mb;
            vkCmdPipelineBarrier2(cmd, &dep);
            vkCmdCopyBuffer(cmd, m_grassPool, buffer, uint32_t(regions.size()), regions.data());
            m_skinUploadsPending = true;             // the upload buffer's closing barrier covers it
        }
        m_grassTrash.push_back({0, m_grassPool, m_grassPoolAllocation, m_frameNumber});
    }
    // The old block's ranges (and those waiting in the trash) are gone with it.
    for (auto& t : m_grassTrash)
        t.alloc = 0;
    if (m_grassPoolBlock) {
        vmaClearVirtualBlock(m_grassPoolBlock);
        vmaDestroyVirtualBlock(m_grassPoolBlock);
    }
    m_grassPool = buffer;
    m_grassPoolAllocation = allocation;
    m_grassPoolBlock = block;
    m_grassPoolBlades = blades;
    Log("ground grass: blade pool %u blades (%.1f MB)", blades, double(bi.size) / (1024.0 * 1024.0));
    return true;
}

// The shared index pattern for at least `blades` blades a draw: the near pattern for each, then the far one.
bool Device::EnsureGrassIndices(uint32_t blades)
{
    if (blades <= m_grassIndexBlades && m_grassIndex)
        return true;
    uint32_t n = std::max<uint32_t>(m_grassIndexBlades * 2, 8192);
    while (n < blades)
        n *= 2;
    const VkDeviceSize bytes = VkDeviceSize(n) * (kNearIndices + kFarIndices) * sizeof(uint32_t);
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = bytes;
    bi.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo ac{};
    ac.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    std::string err;
    if (!Check(vmaCreateBuffer(m_allocator, &bi, &ac, &buffer, &allocation, nullptr), "grass indices", &err))
        return false;
    EnsureRingSpace(bytes + 64);
    void* cpu;
    const VkDeviceSize staging = Allocate(bytes, 16, &cpu);
    uint32_t* out = static_cast<uint32_t*>(cpu);
    for (uint32_t b = 0; b < n; ++b)
        for (uint32_t k = 0; k < kNearIndices; ++k)
            *out++ = b * 8 + kNearPattern[k];
    for (uint32_t b = 0; b < n; ++b)
        for (uint32_t k = 0; k < kFarIndices; ++k)
            *out++ = b * 8 + kFarPattern[k];
    VkBufferCopy region{staging, 0, bytes};
    vkCmdCopyBuffer(UploadCommands(), m_frames[m_frameIndex].ring, buffer, 1, &region);
    m_skinUploadsPending = true;
    if (m_grassIndex)
        m_grassTrash.push_back({0, m_grassIndex, m_grassIndexAllocation, m_frameNumber});
    m_grassIndex = buffer;
    m_grassIndexAllocation = allocation;
    m_grassIndexBlades = n;
    return true;
}

// Bakes one tile's grass: blades scattered over it where its ground is grass, each varying in height, width, lean,
// tilt and wind phase, with a low-frequency noise clumping them and leaving bare gaps; colour from the ground's texel,
// light from its lightmap. The records go to the pool (a rebuild replaces the tile's old ones).
bool Device::BuildGrassTile(GrassTile& tile)
{
    const double since = ProfileCpu();
    const float x0 = float(tile.tx) * kGrassTileSize, z0 = float(tile.tz) * kGrassTileSize;
    const float spacing = std::max(0.3f, m_grassHeight * 0.75f);
    // A pure random scatter, not a patch grid: a jittered lattice shows as rows at grazing angles (a moire). The
    // expected count sets the density; the clump noise drops about half.
    const float perM2 = m_grassDensity / (spacing * spacing);
    const int32_t total = int32_t(perM2 * kGrassTileSize * kGrassTileSize * 1.7f);
    // The ground's slope on a coarse grid over the tile (it varies slowly), bilinear per blade.
    constexpr int kNG = 9;
    const float ngCell = kGrassTileSize / float(kNG);
    float normals[kNG][kNG][3];
    for (int j = 0; j < kNG; ++j)
        for (int i = 0; i < kNG; ++i)
            GroundNormal(x0 + (float(i) + 0.5f) * ngCell, z0 + (float(j) + 0.5f) * ngCell, normals[j][i]);
    auto sampleNormal = [&](float px, float pz, float out[3]) {
        const float fi = std::clamp((px - x0) / ngCell - 0.5f, 0.0f, float(kNG - 1));
        const float fj = std::clamp((pz - z0) / ngCell - 0.5f, 0.0f, float(kNG - 1));
        const int i0 = int(fi), j0 = int(fj);
        const int i1 = std::min(i0 + 1, kNG - 1), j1 = std::min(j0 + 1, kNG - 1);
        const float ti = fi - float(i0), tj = fj - float(j0);
        for (int c = 0; c < 3; ++c) {
            const float a = normals[j0][i0][c] * (1.0f - ti) + normals[j0][i1][c] * ti;
            const float b = normals[j1][i0][c] * (1.0f - ti) + normals[j1][i1][c] * ti;
            out[c] = a * (1.0f - tj) + b * tj;
        }
    };
    // The ground under a blade: its own tile's cell, else the nearest known one around it (GroundAt).
    auto ground = [&](float px, float pz, float* y, uint32_t* colour, uint32_t* light) {
        const int cx = std::clamp(int((px - x0) / kGroundCell), 0, kGroundN - 1);
        const int cz = std::clamp(int((pz - z0) / kGroundCell), 0, kGroundN - 1);
        const int idx = cz * kGroundN + cx;
        if (tile.rank[idx] != 255) {
            *y = tile.y[idx];
            *colour = tile.colour[idx];
            *light = tile.lightRank[idx] != 255 ? tile.light[idx] : 0u;
            return true;
        }
        return GroundAt(px, pz, y, colour, light);
    };
    auto unorm = [](float v, float scale, uint32_t max) {
        return uint32_t(std::clamp(std::lround(v / scale * float(max)), 0L, long(max)));
    };
    // The slow noise fields - the clumps (density) and the lean (direction, strength) - on a coarse grid over the tile,
    // bilinear per blade: they vary over metres, and evaluated per candidate they were most of a build.
    constexpr int kFG = 17;                      // half-unit samples, edges included
    const float fgCell = kGrassTileSize / float(kFG - 1);
    float clumpF[kFG][kFG], leanX[kFG][kFG], leanZ[kFG][kFG];
    for (int j = 0; j < kFG; ++j)
        for (int i = 0; i < kFG; ++i) {
            const float x = x0 + float(i) * fgCell, z = z0 + float(j) * fgCell;
            clumpF[j][i] = ClumpNoise(x, z);
            if ((i & 1) == 0 && (j & 1) == 0) {  // the lean: one-unit samples are plenty
                const float f1 = ValueNoise(x * 0.05f + 31.1f, z * 0.05f - 7.3f) - 0.5f;
                const float f2 = ValueNoise(x * 0.05f - 12.7f, z * 0.05f + 19.3f) - 0.5f;
                const float fl = std::sqrt(f1 * f1 + f2 * f2) + 1e-6f;
                const float mag = 0.05f + 0.22f * ValueNoise(x * 0.028f + 4.7f, z * 0.028f - 2.2f);
                leanX[j][i] = f1 / fl * mag;
                leanZ[j][i] = f2 / fl * mag;
            }
        }
    for (int j = 0; j < kFG; ++j)                // the lean's odd samples: between their neighbours
        for (int i = 0; i < kFG; ++i)
            if ((i & 1) || (j & 1)) {
                const int i0 = i & ~1, j0 = j & ~1, i1 = std::min(i0 + 2, kFG - 1), j1 = std::min(j0 + 2, kFG - 1);
                const float ti = (i & 1) ? 0.5f : 0.0f, tj = (j & 1) ? 0.5f : 0.0f;
                auto mix = [&](float (&f)[kFG][kFG]) {
                    return (f[j0][i0] * (1 - ti) + f[j0][i1] * ti) * (1 - tj) + (f[j1][i0] * (1 - ti) + f[j1][i1] * ti) * tj;
                };
                leanX[j][i] = mix(leanX);
                leanZ[j][i] = mix(leanZ);
            }
    auto field = [&](const float (&f)[kFG][kFG], float px, float pz) {
        const float fi = std::clamp((px - x0) / fgCell, 0.0f, float(kFG - 1) - 1e-3f);
        const float fj = std::clamp((pz - z0) / fgCell, 0.0f, float(kFG - 1) - 1e-3f);
        const int i = int(fi), j = int(fj);
        const float ti = fi - float(i), tj = fj - float(j);
        return (f[j][i] * (1 - ti) + f[j][i + 1] * ti) * (1 - tj) + (f[j + 1][i] * (1 - ti) + f[j + 1][i + 1] * ti) * tj;
    };
    std::vector<GrassBlade> sparseB, denseB;   // the sparse subset is drawn alone for a distant tile (its LOD)
    sparseB.reserve(size_t(total) / 8);
    denseB.reserve(size_t(total) / 2);
    float minY = 1e30f, maxY = -1e30f;
    const int32_t tileSeed = int32_t(uint32_t(tile.tx) * 73856093u ^ uint32_t(tile.tz) * 19349663u);
    for (int32_t k = 0; k < total; ++k) {
        // The position and every other property come from separate hashes: from one, a band of x would share a
        // rotation (the "rows").
        const uint32_t hp = HashCell(tileSeed, int32_t(uint32_t(k) * 2654435761u));
        const uint32_t hv = HashCell(tileSeed ^ 0x5BF03635, int32_t(uint32_t(k) * 40503u));
        const float px = x0 + Unit(hp) * kGrassTileSize, pz = z0 + Unit(hp * 2246822519u) * kGrassTileSize;
        const float v0 = Unit(hv), v1 = Unit(hv * 2246822519u), v2 = Unit(hv * 3266489917u),
                    v3 = Unit(hv * 668265263u), v4 = Unit(hv * 40503u);
        // Clumps: a low-frequency noise drops blades and leaves bare gaps.
        if (v3 > 0.5f + 0.5f * field(clumpF, px, pz))
            continue;
        float py;
        uint32_t pcol, plight;
        if (!ground(px, pz, &py, &pcol, &plight) || !(pcol >> 24 & 1u))
            continue;                            // no grass ground here
        const float height = m_grassHeight * (0.45f + 1.2f * v0);
        const float yaw = v1 * 6.2831853f;       // the blade's droop direction
        const float rx = std::cos(yaw), rz = std::sin(yaw);
        // The lean: a low-frequency field (a patch leans one way together), a little per-blade scatter.
        const float scatter = (v2 - 0.5f) * 0.18f;
        float tn[3];
        sampleNormal(px, pz, tn);                // the blade grows along the ground's slope, not straight up
        float up[3] = {tn[0] + field(leanX, px, pz) + rx * scatter, tn[1], tn[2] + field(leanZ, px, pz) + rz * scatter};
        const float ul = std::sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
        up[0] /= ul; up[1] /= ul; up[2] /= ul;
        const float droop = 0.06f + 0.18f * v4;  // a little arc over towards the tip
        const float half = 0.5f * (0.03f + 0.03f * v2) * (0.5f + height) * m_grassWidth;   // RVK_GrassWidth
        float phase = std::fmod(px * 0.3f + pz * 0.25f + v1 * 6.2831853f, 62.831853f);
        if (phase < 0.0f)
            phase += 62.831853f;
        // The colour: the grass hue (mostly one green, a little of the ground's own) at the ground texel's brightness,
        // so a field is as light or dark as the ground it grows on; a tiny per-blade variation.
        const float gr = float(pcol >> 16 & 0xFF) / 255.0f, gg = float(pcol >> 8 & 0xFF) / 255.0f,
                    gb = float(pcol & 0xFF) / 255.0f;
        float hue[3] = {(0.44f * 0.45f + gr * 0.55f) * 0.88f, (0.62f * 0.45f + gg * 0.55f) * 0.92f,
                        (0.30f * 0.45f + gb * 0.55f) * 0.82f};
        const float groundLum = 0.299f * gr + 0.587f * gg + 0.114f * gb;
        const float hueLum = 0.299f * hue[0] + 0.587f * hue[1] + 0.114f * hue[2];
        const float scale = (hueLum > 1e-4f ? groundLum / hueLum : 1.0f) * (0.94f + 0.12f * v0);
        uint32_t tint = 0;
        for (int c = 0; c < 3; ++c)
            tint = tint << 8 | uint32_t(std::clamp(std::lround(hue[c] * scale * 255.0f), 0L, 255L));
        GrassBlade b;
        b.x = px;
        b.y = py;
        b.z = pz;
        auto snorm16 = [](float v) { return uint32_t(uint16_t(int16_t(std::clamp(std::lround(v * 32767.0f), -32767L, 32767L)))); };
        b.up = snorm16(up[0]) | snorm16(up[2]) << 16;
        b.shape = unorm(height, 4.0f, 65535) | unorm(half, 0.25f, 255) << 16 | unorm(droop, 1.0f, 255) << 24;
        b.yawPhase = (uint32_t(yaw / 6.2831853f * 65536.0f) & 0xFFFFu) |
                     (uint32_t(phase / 62.831853f * 65536.0f) & 0xFFFFu) << 16;
        b.tint = tint;
        b.light = plight;
        m_grassLitBlades += plight ? 1 : 0;
        ++m_grassBuiltBlades;
        minY = std::min(minY, py);
        maxY = std::max(maxY, py);
        ((hv % 4u == 0u) ? sparseB : denseB).push_back(b);
    }
    // Each subset near-to-far from the tile's centre: the camera is usually within a tile or so of it, so its nearest
    // blades go first and early-Z rejects what they hide.
    const float cx = x0 + 0.5f * kGrassTileSize, cz = z0 + 0.5f * kGrassTileSize;
    // (In 32 distance rings - near first is all early-Z needs, and a full sort cost as much as the rest of a build.)
    auto nearFirst = [&](std::vector<GrassBlade>& v) {
        constexpr int kRings = 32;
        const float scale = float(kRings) / (kGrassTileSize * 0.7072f);
        uint32_t counts[kRings + 1] = {};
        std::vector<uint8_t> ring(v.size());
        for (size_t i = 0; i < v.size(); ++i) {
            const float d = std::sqrt((v[i].x - cx) * (v[i].x - cx) + (v[i].z - cz) * (v[i].z - cz));
            ring[i] = uint8_t(std::min(int(d * scale), kRings - 1));
            ++counts[ring[i] + 1];
        }
        for (int r = 0; r < kRings; ++r)
            counts[r + 1] += counts[r];
        std::vector<GrassBlade> out(v.size());
        for (size_t i = 0; i < v.size(); ++i)
            out[counts[ring[i]]++] = v[i];
        v.swap(out);
    };
    nearFirst(sparseB);
    nearFirst(denseB);
    const uint32_t count = uint32_t(sparseB.size() + denseB.size());
    FreeGrassBlades(tile);                       // a rebuild: the old records go once no frame draws them
    tile.built = true;
    tile.builtVersion = tile.version;
    tile.minY = count ? minY : 0.0f;
    tile.maxY = count ? maxY : 0.0f;
    if (count) {
        if (!EnsureGrassIndices(count))
            return false;
        VmaVirtualAllocationCreateInfo ai{};
        ai.size = VkDeviceSize(count) * sizeof(GrassBlade);
        ai.alignment = sizeof(GrassBlade);       // offsets are in whole blades
        VmaVirtualAllocation a{};
        VkDeviceSize offset = 0;
        if (!m_grassPoolBlock || vmaVirtualAllocate(m_grassPoolBlock, &ai, &a, &offset) != VK_SUCCESS) {
            if (!GrowGrassPool(m_grassPoolBlades + count) || vmaVirtualAllocate(m_grassPoolBlock, &ai, &a, &offset) != VK_SUCCESS) {
                tile.built = false;
                return false;
            }
        }
        EnsureRingSpace(ai.size + 64);
        void* cpu;
        const VkDeviceSize staging = Allocate(ai.size, 16, &cpu);
        std::memcpy(cpu, sparseB.data(), sparseB.size() * sizeof(GrassBlade));
        std::memcpy(static_cast<uint8_t*>(cpu) + sparseB.size() * sizeof(GrassBlade), denseB.data(),
                    denseB.size() * sizeof(GrassBlade));
        VkBufferCopy region{staging, offset, ai.size};
        vkCmdCopyBuffer(UploadCommands(), m_frames[m_frameIndex].ring, m_grassPool, 1, &region);
        m_skinUploadsPending = true;             // the upload buffer's closing barrier covers it
        tile.alloc = FromVa(a);
        tile.first = uint32_t(offset / sizeof(GrassBlade));
        tile.blades = count;
        tile.sparse = uint32_t(sparseB.size());
    }
    ++m_grassBuilds;
    m_grassBuildMs += ProfileCpu() - since;
    return true;
}

// Once a frame: the tiles around the camera (made where the ground will be captured, dropped well out of range), the
// trash, and the settings' and lightmaps' changes.
void Device::UpdateGrassTiles()
{
    if (m_grassGroundReset) {                    // the ground filter changed: capture everything again
        m_grassGroundReset = false;
        for (auto& [key, tile] : m_grassTiles)
            FreeGrassBlades(*tile);
        m_grassTiles.clear();
    }
    if (m_grassDirty) {                          // a blade setting changed: rebuild every tile (old blades stay meanwhile)
        m_grassDirty = false;
        for (auto& [key, tile] : m_grassTiles)
            if (tile->built)
                tile->builtVersion = tile->version - 1;
    }
    if (!m_lightmapsUploaded.empty()) {          // a lightmap changed: its tiles take their light again
        for (auto& [key, tp] : m_grassTiles) {
            GrassTile& tile = *tp;
            bool hit = false;
            for (const void* p : tile.lightmaps)
                hit = hit || (p && std::find(m_lightmapsUploaded.begin(), m_lightmapsUploaded.end(), p) !=
                                       m_lightmapsUploaded.end());
            if (hit) {
                tile.lightReset = m_grassTileEpoch = m_frameNumber + 1;
                std::memset(tile.lightRank, 255, sizeof(tile.lightRank));
                std::memset(tile.blockLightRank, 255, sizeof(tile.blockLightRank));
                std::memset(tile.lightmaps, 0, sizeof(tile.lightmaps));
            }
        }
        m_lightmapsUploaded.clear();
    }
    for (auto it = m_grassTrash.begin(); it != m_grassTrash.end();) {
        if (it->frame + kFramesInFlight + 1 <= m_frameNumber) {
            if (it->alloc && m_grassPoolBlock)
                vmaVirtualFree(m_grassPoolBlock, ToVa(it->alloc));
            if (it->buffer)
                vmaDestroyBuffer(m_allocator, it->buffer, it->allocation);
            it = m_grassTrash.erase(it);
        } else {
            ++it;
        }
    }
    // Tiles reaching into the field (plus a margin, so their ground is known before their blades are needed).
    const float half = kGrassTileSize * 0.5f, diag = half * 1.4142136f;
    const float reach = m_grassDistance + 6.0f + diag;
    const int32_t tx0 = int32_t(std::floor((m_frameEye[0] - reach) / kGrassTileSize));
    const int32_t tx1 = int32_t(std::floor((m_frameEye[0] + reach) / kGrassTileSize));
    const int32_t tz0 = int32_t(std::floor((m_frameEye[2] - reach) / kGrassTileSize));
    const int32_t tz1 = int32_t(std::floor((m_frameEye[2] + reach) / kGrassTileSize));
    m_grassTileBox[0] = tx0;
    m_grassTileBox[1] = tz0;
    m_grassTileBox[2] = tx1;
    m_grassTileBox[3] = tz1;
    for (int32_t tz = tz0; tz <= tz1; ++tz)
        for (int32_t tx = tx0; tx <= tx1; ++tx) {
            const float dx = (float(tx) + 0.5f) * kGrassTileSize - m_frameEye[0];
            const float dz = (float(tz) + 0.5f) * kGrassTileSize - m_frameEye[2];
            if (dx * dx + dz * dz > reach * reach)
                continue;
            auto& slot = m_grassTiles[TileKey(tx, tz)];
            if (!slot) {
                slot = std::make_unique<GrassTile>();
                GrassTile& t = *slot;
                t.tx = tx;
                t.tz = tz;
                std::memset(t.rank, 255, sizeof(t.rank));
                std::memset(t.lightRank, 255, sizeof(t.lightRank));
                std::memset(t.blockRank, 255, sizeof(t.blockRank));
                std::memset(t.blockLightRank, 255, sizeof(t.blockLightRank));
                t.lastChange = m_frameNumber;
                t.created = m_grassTileEpoch = m_frameNumber + 1;
            }
            slot->lastUsed = m_frameNumber;
        }
    if ((m_frameNumber % 600) == 0)              // chunks not drawn for a while (the level of detail moved on)
        for (auto it = m_grassChunks.begin(); it != m_grassChunks.end();)
            it = it->second.lastSeen + 600 < m_frameNumber ? m_grassChunks.erase(it) : std::next(it);
    // Out of range for a while (a margin against churn when walking back and forth): dropped.
    if ((m_frameNumber & 15) == 0)
        for (auto it = m_grassTiles.begin(); it != m_grassTiles.end();) {
            GrassTile& t = *it->second;
            const float dx = (float(t.tx) + 0.5f) * kGrassTileSize - m_frameEye[0];
            const float dz = (float(t.tz) + 0.5f) * kGrassTileSize - m_frameEye[2];
            if (dx * dx + dz * dz > (reach + 16.0f) * (reach + 16.0f) && t.lastUsed + 60 < m_frameNumber) {
                FreeGrassBlades(t);
                it = m_grassTiles.erase(it);
            } else {
                ++it;
            }
        }
}

// Draws the visible grass tiles into the scene rendering already active, building the tiles whose ground is ready.
// Called from Draw at the game's first blended draw - the blades are opaque and write depth, so they must go in
// before the game's blended grass/foliage (which writes no depth). Also from EndScene as a fallback, on its own.
void Device::DrawGrassTiles(VkCommandBuffer cmd)
{
    if (!m_grassOn || !m_grassPipeline || !m_scene || !m_shadowView || !m_frameLightsOffset)
        return;
    UpdateFrameEye();
    if (!m_frameEyeValid)
        return;
    m_grassDrawnThisFrame = true;
    UpdateGrassTiles();
    const uint64_t frame = m_frameNumber;
    const float half = kGrassTileSize * 0.5f, diag = half * 1.4142136f;
    const float drawReach = m_grassDistance + diag;
    // Builds: a tile whose ground has settled (fully seen, or nothing new for a few frames) and isn't built, or whose
    // ground changed since it was built. Nearest first, within a time budget (a burst of builds is a stutter).
    struct Pending { float d2; GrassTile* tile; };
    std::vector<Pending> pending;
    for (auto& [key, tp] : m_grassTiles) {
        GrassTile& t = *tp;
        if (!t.seen)
            continue;
        const bool settled = frame - t.lastChange >= 4 || (!t.built && t.seen == uint32_t(GrassTile::kCells));
        if (!settled || (t.built && t.builtVersion == t.version))
            continue;
        const float dx = (float(t.tx) + 0.5f) * kGrassTileSize - m_frameEye[0];
        const float dz = (float(t.tz) + 0.5f) * kGrassTileSize - m_frameEye[2];
        if (dx * dx + dz * dz > drawReach * drawReach)
            continue;
        pending.push_back({dx * dx + dz * dz, &t});
    }
    std::sort(pending.begin(), pending.end(), [](const Pending& a, const Pending& b) { return a.d2 < b.d2; });
    const double start = ProfileCpu();
    for (const Pending& p : pending) {
        if (p.tile != pending.front().tile && ProfileCpu() - start > 0.6)
            break;                               // ~0.6 ms of building a frame
        BuildGrassTile(*p.tile);
    }
    if (!m_grassPool || !m_grassIndex)
        return;
    // The visible tiles: in range and in the view frustum (the box of the roots, raised by the tallest blade).
    const d3d::Matrix viewProj = MulMatrix(m_view, m_proj);
    const auto& m = viewProj.m;
    float planes[5][4];                          // left, right, bottom, top, far: column 3 +- columns 0, 1, 2
    for (int j = 0; j < 4; ++j) {
        planes[0][j] = m[j][3] + m[j][0];
        planes[1][j] = m[j][3] - m[j][0];
        planes[2][j] = m[j][3] + m[j][1];
        planes[3][j] = m[j][3] - m[j][1];
        planes[4][j] = m[j][3] - m[j][2];
    }
    const float tall = m_grassHeight * 1.7f + 1.0f;
    struct Visible { float d2; const GrassTile* tile; uint32_t indices, firstIndex; };
    std::vector<Visible> visible;
    const float lodSparse = m_grassDistance * 0.9f, lodFar = m_grassDistance * 0.5f;
    for (auto& [key, tp] : m_grassTiles) {
        const GrassTile& t = *tp;
        if (!t.built || !t.blades)
            continue;
        const float x0 = float(t.tx) * kGrassTileSize, z0 = float(t.tz) * kGrassTileSize;
        const float dx = x0 + half - m_frameEye[0], dz = z0 + half - m_frameEye[2];
        const float d2 = dx * dx + dz * dz;
        if (d2 > drawReach * drawReach)
            continue;
        const float lo[3] = {x0 - 1.0f, t.minY - 1.0f, z0 - 1.0f};
        const float hi[3] = {x0 + kGrassTileSize + 1.0f, t.maxY + tall, z0 + kGrassTileSize + 1.0f};
        bool inside = true;
        for (int p = 0; p < 5 && inside; ++p) {
            const float* pl = planes[p];
            const float s = pl[0] * (pl[0] >= 0.0f ? hi[0] : lo[0]) + pl[1] * (pl[1] >= 0.0f ? hi[1] : lo[1]) +
                            pl[2] * (pl[2] >= 0.0f ? hi[2] : lo[2]) + pl[3];
            inside = s >= 0.0f;
        }
        if (!inside)
            continue;
        // Distance levels of detail, by the tile's nearest point: three triangles a blade, then only the sparse subset
        // (deep in the field's edge fade, where the blades are a fraction of their height).
        const float nx = std::max({x0 - m_frameEye[0], 0.0f, m_frameEye[0] - x0 - kGrassTileSize});
        const float nz = std::max({z0 - m_frameEye[2], 0.0f, m_frameEye[2] - z0 - kGrassTileSize});
        const float nearest = std::sqrt(nx * nx + nz * nz);
        const uint32_t blades = nearest > lodSparse && t.sparse ? t.sparse : t.blades;
        if (nearest > lodFar)
            visible.push_back({d2, &t, blades * kFarIndices, m_grassIndexBlades * kNearIndices});
        else
            visible.push_back({d2, &t, blades * kNearIndices, 0});
    }
    if (visible.empty())
        return;
    // Nearest first: with the depth write on, early-Z then rejects the blades they hide.
    std::sort(visible.begin(), visible.end(), [](const Visible& a, const Visible& b) { return a.d2 < b.d2; });
    FlushGroup();                                // the scene's batched draws go first (and its profile class ends)
    ProfileSceneClass(0);
    GrassFrame gf{};
    gf.viewProj = viewProj;
    gf.prevViewProj = m_prevViewProjValid ? MulMatrix(m_prevView, m_prevProj) : gf.viewProj;
    gf.viewport[0] = float(m_scene->m_width);
    gf.viewport[1] = float(m_scene->m_height);
    gf.viewport[2] = m_grassDistance;
    gf.wind[0] = float(SwayClock());
    gf.viewport[3] = m_grassPrevTime != 0.0f ? m_grassPrevTime : gf.wind[0];   // the wind clock last frame
    m_grassPrevTime = gf.wind[0];
    gf.wind[1] = 0.56f;
    gf.wind[2] = 0.35f;                          // a wind direction (normalised in the shader)
    gf.wind[3] = std::max(m_sway, 0.3f);         // the plants' sway strength scales the wind
    gf.camera[0] = m_frameEye[0];
    gf.camera[1] = m_frameEye[1];
    gf.camera[2] = m_frameEye[2];
    gf.camera[3] = TaaActive() ? 1.0f : 0.0f;
    std::memcpy(gf.ambient, m_terrainAmbient, sizeof(gf.ambient));
    gf.look[0] = 1.0f;
    gf.look[1] = 1.0f;
    std::memcpy(gf.sunColour, m_terrainSun, sizeof(gf.sunColour) + sizeof(gf.sunDir));
    // The frame lights (the sun, its cascades, the lights, the pushers) as of now, in this ring: the scene writes them
    // lazily, and an offset from an earlier ring would be another frame's data.
    EnsureRingSpace(sizeof(GrassFrame) + sizeof(FrameLights) + 512);
    if (m_frameLightsDirty || m_frameLightsGeneration != m_ringGeneration)
        WriteFrameLights();
    Frame& f = m_frames[m_frameIndex];
    void* frameCpu = nullptr;
    const VkDeviceSize frameOffset = Allocate(sizeof(GrassFrame), 256, &frameCpu);   // minUniformBufferOffsetAlignment
    std::memcpy(frameCpu, &gf, sizeof(gf));

    const float sw = float(m_scene->m_width), sh = float(m_scene->m_height);
    VkViewport viewport{0.5f, sh + 0.5f, sw, -sh, 0.0f, 1.0f};   // the scene draws into a flipped viewport
    VkRect2D scissor{{0, 0}, {m_scene->m_width, m_scene->m_height}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_grassPipeline);
    VkDescriptorBufferInfo frameInfo{f.ring, frameOffset, sizeof(GrassFrame)};
    VkDescriptorBufferInfo bladeInfo{m_grassPool, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo lightsInfo{f.ring, m_frameLightsOffset, sizeof(FrameLights)};
    VkDescriptorImageInfo shadowInfo{m_shadowSampler, m_shadowView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo cubeInfo{m_cubeSampler, m_cubeArrayView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet writes[5] = {};
    for (auto& w : writes) {
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.descriptorCount = 1;
    }
    writes[0].dstBinding = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].pBufferInfo = &frameInfo;
    writes[1].dstBinding = 2;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &bladeInfo;
    writes[2].dstBinding = 4;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[2].pBufferInfo = &lightsInfo;
    writes[3].dstBinding = 5;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[3].pImageInfo = &shadowInfo;
    writes[4].dstBinding = 6;
    writes[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[4].pImageInfo = &cubeInfo;
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_grassLayout, 0, 5, writes);
    vkCmdBindIndexBuffer(cmd, m_grassIndex, 0, VK_INDEX_TYPE_UINT32);
    // Which of the frame's 64 lights reach a tile, so a blade only tests the few that matter (the frame lights the
    // scene lights by too, so only with the light override on).
    const bool localLights = m_lightOverride && m_pixelLighting && !m_frameLightSpheres.empty();
    uint64_t drawn = 0;
    uint32_t lastMask[2] = {~0u, ~0u};
    for (const Visible& v : visible) {
        const GrassTile& t = *v.tile;
        uint64_t mask = 0;
        if (localLights) {
            const float x0 = float(t.tx) * kGrassTileSize, z0 = float(t.tz) * kGrassTileSize;
            const float x1 = x0 + kGrassTileSize, z1 = z0 + kGrassTileSize;
            const float y0 = t.minY - 1.0f, y1 = t.maxY + tall;
            const size_t n = std::min<size_t>(m_frameLightSpheres.size(), 64);
            for (size_t i = 0; i < n; ++i) {
                const LightSphere& s = m_frameLightSpheres[i];
                const float dx = std::max({x0 - s.x, 0.0f, s.x - x1});
                const float dz = std::max({z0 - s.z, 0.0f, s.z - z1});
                const float dy = std::max({y0 - s.y, 0.0f, s.y - y1});
                if (dx * dx + dy * dy + dz * dz <= s.r2)
                    mask |= uint64_t(1) << i;
            }
        }
        const uint32_t mask2[2] = {uint32_t(mask), uint32_t(mask >> 32)};
        if (mask2[0] != lastMask[0] || mask2[1] != lastMask[1]) {
            vkCmdPushConstants(cmd, m_grassLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, 8, mask2);
            lastMask[0] = mask2[0];
            lastMask[1] = mask2[1];
        }
        vkCmdDrawIndexed(cmd, v.indices, 1, v.firstIndex, int32_t(t.first * 8), 0);
        drawn += v.indices / (v.firstIndex ? kFarIndices : kNearIndices);
    }
    m_cache = StateCache{};                      // this pipeline and its vertex input are not the scene's
    m_arenaBound = m_bindlessBound = false;      // ... and its pushed set 0 replaced the scene's: rebind those too
    ProfileMark("ground grass");
    m_grassBlades += drawn;
    ++m_grassDraws;
}

// The fallback: if the frame never saw a blended draw to hook, the blades go in at the end of the scene, on their own
// rendering.
void Device::RenderGrassField(VkCommandBuffer cmd)
{
    if (!m_grassOn || !m_grassPipeline || !m_scene || !m_shadowView || !m_frameLightsOffset || m_grassDrawnThisFrame)
        return;
    UpdateFrameEye();
    if (!m_frameEyeValid)
        return;
    BeginRenderingOn(m_scene);
    DrawGrassTiles(cmd);
    EndRendering();
}

}  // namespace rvk
