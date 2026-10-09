// Leaves on trees, shrubs and palms (RVK_LeafOn): an addition on top of the game's canopies, which are a few big cards
// with a painted cluster of leaves and twigs on each (a 20-unit jungle crown is 54 triangles).
//
// A canopy is a lit, static mesh drawn into the scene with a texture that is mostly holes (CanopyKind). Its leaves are
// baked once per mesh and texture (BakeLeaves): sprigs scattered over its cards where the texture is leaf, each a small
// card of its own cut from the same texture - the painted leaves around that spot - tilted and turned its own way and
// lifted off the card a little, so the crown gets depth. They go into one GPU pool, shuffled so that any first part of
// a canopy's leaves is an even thinning of all of them (the distance level of detail draws a first part).
//
// Drawn just before the canopy's own draw (DrawLeaves), with the canopy's record: the same world matrix, render state,
// lights and texture; leaf.vert (ffp.vert built with RVK_LEAF) makes each leaf's vertices from its record and passes
// them through the canopy's own vertex path. The canopy's cards and its leaves sway together with the branches
// (ffp.vert BranchSway, D.leaf), and each leaf flutters on its own.
#include "internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace rvk {

using namespace vk;
using namespace detail;

namespace {

// One leaf in the pool (ffp.vert RVK_LEAF: Leaves).
struct LeafRecord {
    float centre[3];
    uint32_t halfUxy;            // half its width along the texture's u, model space (half floats x, y)
    uint32_t halfUzVx;           // ... z, half its height along v x
    uint32_t halfVyz;            // ... y, z
    uint32_t uv;                 // its centre in the texture (unorm16 x 2)
    uint32_t uvHalfPhase;        // its half size in the texture (unorm12 x 0.25, u then v), its phase (unorm8)
};
static_assert(sizeof(LeafRecord) == 32, "leaf record (two uvec4)");

constexpr uint32_t kMaxLeavesPerSet = 16384;
constexpr uint32_t kBakesPerFrame = 4;           // canopies baked a frame at most (the rest wait for the next)
constexpr uint64_t kLeafSetIdleFrames = 3600;    // a set not drawn for this long leaves the pool

uint16_t ToHalf(float f)
{
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    int32_t e = int32_t((x >> 23) & 0xFF) - 127 + 15;
    uint32_t m = x & 0x7FFFFFu;
    if (e <= 0) {                                // tiny: zero (a leaf's extent is never that small)
        return uint16_t(sign);
    }
    if (e >= 31)
        return uint16_t(sign | 0x7BFFu);         // the largest finite half
    uint32_t h = sign | (uint32_t(e) << 10) | (m >> 13);
    if (m & 0x1000u)                             // round to nearest
        ++h;
    return uint16_t(h);
}
uint32_t Halves(float a, float b) { return uint32_t(ToHalf(a)) | uint32_t(ToHalf(b)) << 16; }

// A small, stable random generator: the same mesh bakes the same leaves (the temporal anti-aliasing and the distance
// thinning rely on a leaf staying put).
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull) {}
    uint32_t Next()
    {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return uint32_t((s * 0x2545F4914F6CDD1Dull) >> 32);
    }
    float Unit() { return float(Next() >> 8) * (1.0f / 16777216.0f); }
};

struct V3 { float x, y, z; };
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float Length(V3 a) { return std::sqrt(Dot(a, a)); }
V3 Normalize(V3 a) { float l = Length(a); return l > 1e-12f ? a * (1.0f / l) : V3{0, 1, 0}; }
// v turned by `angle` about the unit axis k (Rodrigues).
V3 Rotate(V3 v, V3 k, float angle)
{
    float c = std::cos(angle), s = std::sin(angle);
    return v * c + Cross(k, v) * s + k * (Dot(k, v) * (1.0f - c));
}

// The texture's alpha (0..1) at (u, v), filtered between the four nearest cells of its mask (wrapping).
float MaskAlpha(const std::vector<uint8_t>& mask, uint32_t w, uint32_t h, float u, float v)
{
    float x = (u - std::floor(u)) * float(w) - 0.5f, y = (v - std::floor(v)) * float(h) - 0.5f;
    float fx = std::floor(x), fy = std::floor(y);
    int x0 = int(fx), y0 = int(fy);
    float tx = x - fx, ty = y - fy;
    auto at = [&](int i, int j) {
        i = ((i % int(w)) + int(w)) % int(w);
        j = ((j % int(h)) + int(h)) % int(h);
        return float(mask[size_t(j) * w + size_t(i)]);
    };
    float a = at(x0, y0) * (1 - tx) + at(x0 + 1, y0) * tx;
    float b = at(x0, y0 + 1) * (1 - tx) + at(x0 + 1, y0 + 1) * tx;
    return (a * (1 - ty) + b * ty) / 255.0f;
}

}  // namespace

// Whether the current draw is a canopy, and which kind: lit, static vertices, drawn into the scene with depth writes and
// a texture that is mostly holes (its mean alpha well under the walls' and signs'), standing upright (the model axis
// most aligned with the world's up), not one flat plane (a sign, a poster). Shrubs are plant-sized (the plants that
// sway: 0.2 - 3 units tall), trees up to 45 units.
Device::Canopy Device::CanopyKind(uint32_t primitive, uint32_t fvf, uint32_t stride, const void* vertices,
                                  uint32_t vertexCount, const uint16_t* indices, uint32_t indexCount)
{
    Texture* tex = m_textures[0];
    if (m_target != m_scene || !tex || tex->m_alphaMask.empty() || !m_rs[d3d::RS_LIGHTING] || m_drawIsLabel ||
        (fvf & d3d::FVF_POSITION_MASK) != d3d::FVF_XYZ || IsTerrain(fvf) || !m_rs[d3d::RS_ZWRITEENABLE] ||
        !(m_rs[d3d::RS_ALPHATESTENABLE] || m_rs[d3d::RS_ALPHABLENDENABLE]) || vertexCount < 3 || vertexCount > 4096 ||
        !m_drawMesh || primitive < d3d::TriangleList || primitive > d3d::TriangleFan || !vertices ||
        DecodeFvf(fvf).offset[4] < 0)
        return Canopy::None;
    // Mostly holes (leaf textures: mean alpha 0.3 - 0.45 in the game's trees).
    uint32_t sum = 0;
    for (uint8_t a : tex->m_alphaMask) sum += a;
    const float mean = float(sum) / (255.0f * float(tex->m_alphaMask.size()));
    if (mean < 0.05f || mean > 0.65f)
        return Canopy::None;
    const auto& w = m_world.m;
    int axis = 0;
    float best = -1.0f, scale = 0.0f;
    for (int i = 0; i < 3; ++i) {
        float len = std::sqrt(w[i][0] * w[i][0] + w[i][1] * w[i][1] + w[i][2] * w[i][2]);
        float up = len > 0.0f ? std::fabs(w[i][1]) / len : 0.0f;
        if (up > best) { best = up; axis = i; scale = len; }
    }
    if (best < 0.7f)
        return Canopy::None;
    const float height = (m_drawMesh->boundsMax[axis] - m_drawMesh->boundsMin[axis]) * scale;
    float across = 0.0f;
    for (int i = 0; i < 3; ++i) {
        if (i == axis)
            continue;
        const float len = std::sqrt(w[i][0] * w[i][0] + w[i][1] * w[i][1] + w[i][2] * w[i][2]);
        across = std::max(across, (m_drawMesh->boundsMax[i] - m_drawMesh->boundsMin[i]) * len);
    }
    if (height < 0.2f || height > 45.0f || across > 60.0f || (height <= 3.0f && across > 6.0f))
        return Canopy::None;
    if (m_drawMesh->flatShape < 0)
        m_drawMesh->flatShape = OnePlane(primitive, stride, vertices, vertexCount, indices, indexCount) ? 1 : 0;
    if (m_drawMesh->flatShape == 1)
        return Canopy::None;
    // A palm's (or a fern's) fronds: each card one long frond, its texture at least 2.5 times as long as wide.
    const uint32_t lo = std::min(tex->m_width, tex->m_height), hi = std::max(tex->m_width, tex->m_height);
    if (height > 1.0f && hi >= 5 * lo / 2)
        return Canopy::Palm;
    return height <= 3.0f ? Canopy::Shrub : Canopy::Tree;
}

// Per canopy draw (Draw, before its ring space is reserved): its leaf set, baked if it isn't yet (or its texture or the
// settings changed), and how many of its leaves to draw at this distance. Sets m_drawLeaves / m_drawLeafCount.
void Device::CanopyParams(uint32_t primitive, uint32_t fvf, const FvfLayout& layout, const void* vertices,
                          uint32_t vertexCount, const uint16_t* indices, uint32_t indexCount)
{
    m_drawLeaves = nullptr;
    m_drawLeafKey = 0;
    m_drawLeafCount = 0;
    if (!m_leaf.on || m_external || m_drawGpu || !m_drawMesh || !m_leafPipelines[0] || !m_drawMeshStatic ||
        m_drawMesh->firstFrame + 2 > m_frameNumber || m_target != m_scene)
        return;
    Texture* tex = m_textures[0];
    if (!tex || tex->m_alphaMask.empty() || !m_rs[d3d::RS_LIGHTING] || !m_rs[d3d::RS_ZWRITEENABLE])
        return;                                  // (most draws: not worth a look-up)
    const uint64_t key = m_drawMeshKey ^ (uint64_t(reinterpret_cast<uintptr_t>(tex)) * 0x9E3779B97F4A7C15ull);
    auto it = m_leafSets.find(key);
    if (it == m_leafSets.end()) {
        LeafSet set;
        set.kind = CanopyKind(primitive, fvf, layout.stride, vertices, vertexCount, indices, indexCount);
        it = m_leafSets.emplace(key, set).first;
    }
    LeafSet& set = it->second;
    set.lastFrame = m_frameNumber;
    if (set.kind == Canopy::None)
        return;
    ++m_canopyCount[uint32_t(set.kind)];
    if ((set.kind == Canopy::Tree && !m_leaf.trees) || (set.kind == Canopy::Shrub && !m_leaf.shrubs) ||
        (set.kind == Canopy::Palm && !m_leaf.palms))
        return;
    if (set.kind == Canopy::Palm) {              // no leaves: its fronds bend (ffp.vert BranchSway, D.leaf.w < 0)
        if (!set.baked) {
            CrownOf(set);
            set.baked = true;
        }
        m_drawLeaves = &set;
        m_drawLeafKey = key;
        return;
    }
    if (!set.baked || set.texVersion != tex->m_alphaVersion || set.settingsVersion != m_leafVersion) {
        if (m_leafBakesThisFrame >= kBakesPerFrame)
            return;                              // next frame
        ++m_leafBakesThisFrame;
        if (!BakeLeaves(set, primitive, layout, vertices, vertexCount, indices, indexCount))
            return;
    }
    if (!set.count)
        return;
    // The distance: full leaves up to half of RVK_LeafDist, then thinning out (the shuffled pool's first part) to none.
    float c[3], e[3], d2 = 0.0f;
    DrawWorldBox(c, e);
    if (m_frameEyeValid)
        for (int j = 0; j < 3; ++j) {
            float d = std::max(std::fabs(m_frameEye[j] - c[j]) - e[j], 0.0f);
            d2 += d * d;
        }
    const float dist = std::sqrt(d2), reach = std::max(m_leaf.distance, 1.0f);
    const float keep = std::clamp((reach - dist) / (0.5f * reach), 0.0f, 1.0f);
    m_drawLeaves = &set;
    m_drawLeafKey = key;
    m_drawLeafCount = uint32_t(float(set.count) * keep * keep);
}

// The crown: the canopy's box, its centre and half its largest extent (model space).
void Device::CrownOf(LeafSet& set) const
{
    const float* lo = m_drawMesh->boundsMin;
    const float* hi = m_drawMesh->boundsMax;
    for (int i = 0; i < 3; ++i)
        set.centre[i] = 0.5f * (lo[i] + hi[i]);
    set.radius = 0.5f * std::max(hi[0] - lo[0], std::max(hi[1] - lo[1], hi[2] - lo[2]));
}

// Bakes a canopy's leaves (see the top of the file) into the pool.
bool Device::BakeLeaves(LeafSet& set, uint32_t primitive, const FvfLayout& layout, const void* vertices,
                        uint32_t vertexCount, const uint16_t* indices, uint32_t indexCount)
{
    const double since = ProfileCpu();
    Texture* tex = m_textures[0];
    FreeLeaves(set);
    set.baked = true;
    set.texVersion = tex->m_alphaVersion;
    set.settingsVersion = m_leafVersion;
    const auto& mask = tex->m_alphaMask;
    const uint32_t mw = tex->m_alphaW, mh = tex->m_alphaH;
    const uint8_t* base = static_cast<const uint8_t*>(vertices);
    const uint32_t stride = layout.stride, uvOffset = uint32_t(layout.offset[4]);
    auto pos = [&](uint32_t i) {
        float p[3];
        std::memcpy(p, base + size_t(i) * stride, 12);
        return V3{p[0], p[1], p[2]};
    };
    auto uvAt = [&](uint32_t i, float out[2]) { std::memcpy(out, base + size_t(i) * stride + uvOffset, 8); };
    CrownOf(set);
    const V3 centre{set.centre[0], set.centre[1], set.centre[2]};

    const float win = 0.0625f * std::clamp(m_leaf.size, 0.25f, 3.5f);   // a sprig's half size in the texture
    const float perArea = 2.2f * std::clamp(m_leaf.density, 0.1f, 4.0f) / (4.0f * win * win);
    std::vector<LeafRecord> leaves;
    Rng rng(m_drawMeshKey ^ 0x5EEDull);
    const uint32_t count = indices ? indexCount : vertexCount;
    const uint32_t triangles = primitive == d3d::TriangleList ? count / 3 : (count >= 3 ? count - 2 : 0);
    for (uint32_t t = 0; t < triangles; ++t) {
        uint32_t k[3];
        if (primitive == d3d::TriangleList) { k[0] = 3 * t; k[1] = 3 * t + 1; k[2] = 3 * t + 2; }
        else if (primitive == d3d::TriangleStrip) { k[0] = t; k[1] = t + 1; k[2] = t + 2; }
        else { k[0] = 0; k[1] = t + 1; k[2] = t + 2; }
        uint32_t vi[3];
        for (int j = 0; j < 3; ++j) vi[j] = indices ? indices[k[j]] : k[j];
        if (vi[0] >= vertexCount || vi[1] >= vertexCount || vi[2] >= vertexCount)
            continue;
        V3 p0 = pos(vi[0]), p1 = pos(vi[1]), p2 = pos(vi[2]);
        float t0[2], t1[2], t2[2];
        uvAt(vi[0], t0);
        uvAt(vi[1], t1);
        uvAt(vi[2], t2);
        V3 e1 = p1 - p0, e2 = p2 - p0;
        float du1 = t1[0] - t0[0], dv1 = t1[1] - t0[1], du2 = t2[0] - t0[0], dv2 = t2[1] - t0[1];
        float det = du1 * dv2 - du2 * dv1;
        float uvArea = 0.5f * std::fabs(det);
        if (uvArea < 1e-6f || Length(Cross(e1, e2)) < 1e-8f)
            continue;
        // The card's own directions of u and v (model units per unit of the texture): a sprig cut from the texture
        // keeps its painted proportions.
        V3 du = (e1 * dv2 - e2 * dv1) * (1.0f / det), dv = (e2 * du1 - e1 * du2) * (1.0f / det);
        V3 n = Normalize(Cross(e1, e2));
        float want = uvArea * perArea;
        uint32_t tries = uint32_t(want) + (rng.Unit() < want - std::floor(want) ? 1u : 0u);
        tries = std::min<uint32_t>(tries, 4096);
        for (uint32_t c = 0; c < tries; ++c) {
            float r1 = rng.Unit(), r2 = rng.Unit();
            if (r1 + r2 > 1.0f) { r1 = 1.0f - r1; r2 = 1.0f - r2; }
            float u = t0[0] + du1 * r1 + du2 * r2, v = t0[1] + dv1 * r1 + dv2 * r2;
            // Leaf there, and leafy around it (not a lone twig at the cluster's edge).
            if (MaskAlpha(mask, mw, mh, u, v) < 0.5f)
                continue;
            float around = 0.0f;
            const float o = 0.6f * win;
            around += MaskAlpha(mask, mw, mh, u - o, v) + MaskAlpha(mask, mw, mh, u + o, v);
            around += MaskAlpha(mask, mw, mh, u, v - o) + MaskAlpha(mask, mw, mh, u, v + o);
            if (around < 4.0f * 0.35f)
                continue;
            V3 p = p0 + e1 * r1 + e2 * r2;
            V3 hu = du * win, hv = dv * win;
            // Its own way: turned about the card's normal, then tilted off it (up to ~40 degrees).
            float turn = (rng.Unit() - 0.5f) * 6.2831853f;
            hu = Rotate(hu, n, turn);
            hv = Rotate(hv, n, turn);
            V3 tiltAxis = Normalize(Rotate(Normalize(du), n, rng.Unit() * 6.2831853f));
            float tilt = (rng.Unit() - 0.5f) * 1.4f;
            hu = Rotate(hu, tiltAxis, tilt);
            hv = Rotate(hv, tiltAxis, tilt);
            // Lifted off the card (either side), and a little out of the crown: depth.
            float size = Length(hv);
            p = p + n * ((rng.Unit() - 0.5f) * 1.2f * size);
            V3 out = p - centre;
            p = p + Normalize(out) * (rng.Unit() * 0.5f * size);
            LeafRecord r{};
            r.centre[0] = p.x;
            r.centre[1] = p.y;
            r.centre[2] = p.z;
            r.halfUxy = Halves(hu.x, hu.y);
            r.halfUzVx = Halves(hu.z, hv.x);
            r.halfVyz = Halves(hv.y, hv.z);
            auto unorm16 = [](float x) { return uint32_t(std::clamp(x, 0.0f, 1.0f) * 65535.0f + 0.5f); };
            r.uv = unorm16(u - std::floor(u)) | unorm16(v - std::floor(v)) << 16;
            uint32_t uh = uint32_t(std::min(win / 0.25f, 1.0f) * 4095.0f + 0.5f);
            r.uvHalfPhase = uh | uh << 12 | (rng.Next() >> 24) << 24;
            leaves.push_back(r);
        }
    }
    // Shuffled: any first part of the set is an even thinning (the distance level of detail draws a first part).
    for (size_t i = leaves.size(); i > 1; --i)
        std::swap(leaves[i - 1], leaves[rng.Next() % i]);
    if (leaves.size() > kMaxLeavesPerSet)
        leaves.resize(kMaxLeavesPerSet);
    const uint32_t n = uint32_t(leaves.size());
    if (n) {
        VmaVirtualAllocationCreateInfo ai{};
        ai.size = n;                             // in leaves
        VmaVirtualAllocation a{};
        VkDeviceSize offset = 0;
        if (!m_leafPoolBlock || vmaVirtualAllocate(m_leafPoolBlock, &ai, &a, &offset) != VK_SUCCESS) {
            if (!GrowLeafPool(m_leafPoolLeaves + n) || vmaVirtualAllocate(m_leafPoolBlock, &ai, &a, &offset) != VK_SUCCESS)
                return false;
        }
        const VkDeviceSize bytes = VkDeviceSize(n) * sizeof(LeafRecord);
        EnsureRingSpace(bytes + 64);
        void* cpu;
        const VkDeviceSize staging = Allocate(bytes, 16, &cpu);
        std::memcpy(cpu, leaves.data(), bytes);
        VkBufferCopy region{staging, offset * sizeof(LeafRecord), bytes};
        vkCmdCopyBuffer(UploadCommands(), m_frames[m_frameIndex].ring, m_leafPool, 1, &region);
        m_skinUploadsPending = true;             // the upload buffer's closing barrier covers it
        uint64_t handle = 0;
        static_assert(sizeof(a) <= sizeof(handle), "virtual allocation handle");
        std::memcpy(&handle, &a, sizeof(a));
        set.alloc = handle;
        set.first = uint32_t(offset);
        set.count = n;
    }
    ++m_leafBakes;
    m_leafBakedLeaves += n;
    m_leafBakeMs += ProfileCpu() - since;
    return true;
}

// A set's leaves leave the pool once no frame in flight can still draw them.
void Device::FreeLeaves(LeafSet& set)
{
    if (set.alloc)
        m_leafTrash.push_back({set.alloc, VK_NULL_HANDLE, nullptr, m_frameNumber});
    set.alloc = 0;
    set.first = set.count = 0;
}

// The leaf pool, at least `minLeaves` big: a new, bigger buffer that the baked sets are copied into (the old one is
// freed once no frame in flight reads it).
bool Device::GrowLeafPool(uint32_t minLeaves)
{
    uint32_t size = std::max<uint32_t>(m_leafPoolLeaves * 2, 1u << 16);   // 2 MB to start
    while (size < minLeaves)
        size *= 2;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = VkDeviceSize(size) * sizeof(LeafRecord);
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo ac{};
    ac.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    std::string err;
    if (!Check(vmaCreateBuffer(m_allocator, &bi, &ac, &buffer, &allocation, nullptr), "leaf pool", &err))
        return false;
    VmaVirtualBlockCreateInfo vb{};
    vb.size = size;                              // counted in leaves
    VmaVirtualBlock block = nullptr;
    if (vmaCreateVirtualBlock(&vb, &block) != VK_SUCCESS) {
        vmaDestroyBuffer(m_allocator, buffer, allocation);
        return false;
    }
    std::vector<VkBufferCopy> regions;
    for (auto& [key, set] : m_leafSets) {
        if (!set.alloc)
            continue;
        VmaVirtualAllocationCreateInfo ai{};
        ai.size = set.count;
        VmaVirtualAllocation a{};
        VkDeviceSize offset = 0;
        if (vmaVirtualAllocate(block, &ai, &a, &offset) != VK_SUCCESS) {   // can't happen: the new pool is bigger
            set.alloc = 0;
            set.count = 0;
            set.baked = false;
            continue;
        }
        regions.push_back({VkDeviceSize(set.first) * sizeof(LeafRecord), offset * sizeof(LeafRecord),
                           VkDeviceSize(set.count) * sizeof(LeafRecord)});
        uint64_t handle = 0;
        std::memcpy(&handle, &a, sizeof(a));
        set.alloc = handle;
        set.first = uint32_t(offset);
    }
    if (m_leafPool) {
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
            vkCmdCopyBuffer(cmd, m_leafPool, buffer, uint32_t(regions.size()), regions.data());
            m_skinUploadsPending = true;
        }
        m_leafTrash.push_back({0, m_leafPool, m_leafPoolAllocation, m_frameNumber});
    }
    for (auto& t : m_leafTrash)                  // the old block's ranges are gone with it
        t.alloc = 0;
    if (m_leafPoolBlock) {
        vmaClearVirtualBlock(m_leafPoolBlock);
        vmaDestroyVirtualBlock(m_leafPoolBlock);
    }
    m_leafPool = buffer;
    m_leafPoolAllocation = allocation;
    m_leafPoolBlock = block;
    m_leafPoolLeaves = size;
    Log("leaves: pool %u leaves (%.1f MB)", size, double(bi.size) / (1024.0 * 1024.0));
    return true;
}

// Once a frame: the trash, the sets no longer drawn, a settings change (every set baked again as it is next drawn),
// and with the leaves off everything they hold.
void Device::UpdateLeaves()
{
    m_leafBakesThisFrame = 0;
    for (auto it = m_leafTrash.begin(); it != m_leafTrash.end();) {
        if (it->frame + kFramesInFlight + 1 <= m_frameNumber) {
            if (it->alloc && m_leafPoolBlock) {
                VmaVirtualAllocation a{};
                std::memcpy(&a, &it->alloc, sizeof(a));
                vmaVirtualFree(m_leafPoolBlock, a);
            }
            if (it->buffer)
                vmaDestroyBuffer(m_allocator, it->buffer, it->allocation);
            it = m_leafTrash.erase(it);
        } else {
            ++it;
        }
    }
    if (m_leafDirty) {
        m_leafDirty = false;
        ++m_leafVersion;
    }
    if (!m_leaf.on) {
        if (!m_leafSets.empty()) {
            for (auto& [key, set] : m_leafSets)
                FreeLeaves(set);
            m_leafSets.clear();
        }
        return;
    }
    if ((m_frameNumber & 255) == 0)
        for (auto it = m_leafSets.begin(); it != m_leafSets.end();) {
            if (it->second.lastFrame + kLeafSetIdleFrames < m_frameNumber) {
                FreeLeaves(it->second);
                it = m_leafSets.erase(it);
            } else {
                ++it;
            }
        }
}

// The current canopy's leaves, drawn before its own draw (Draw: its state, descriptors and geometry are already set up
// for it): through leaf.vert, with its record, every face (a leaf is seen from both sides), then the canopy's pipeline,
// culling and per-draw bindings back.
void Device::DrawLeaves(VkCommandBuffer cmd, uint32_t recordIndex, uint32_t primitive, uint32_t fvf, uint32_t stride,
                        VkDeviceSize frameLightsOffset, VkBuffer prevBuffer, VkDeviceSize prevOffset,
                        VkDeviceSize prevBytes, VkBuffer smoothBuffer, VkDeviceSize smoothOffset,
                        VkDeviceSize smoothBytes)
{
    const LeafSet& set = *m_drawLeaves;
    const bool hdr = m_target->m_format == Format::RGBA16F;
    StateCache& c = m_cache;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_leafPipelines[hdr ? 1 : 0]);
    vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE);
    vkCmdSetPrimitiveTopology(cmd, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    PushDrawSet(cmd, frameLightsOffset, prevBuffer, prevOffset, prevBytes, m_leafPool, 0,
                VkDeviceSize(m_leafPoolLeaves) * sizeof(LeafRecord));
    vkCmdDraw(cmd, m_drawLeafCount * 6u, 1, set.first * 6u, recordIndex);
    ++m_leafDraws;
    m_leafDrawn += m_drawLeafCount;
    // The canopy's own state again.
    c.topologyClass = ~0u;
    c.topology = ~0u;
    c.cull = ~0u;
    ApplyDynamicState(primitive, fvf, stride);
    PushDrawSet(cmd, frameLightsOffset, prevBuffer, prevOffset, prevBytes, smoothBuffer, smoothOffset, smoothBytes);
}

// The current sun cascade's canopies' leaves (RenderShadowMap, after the cascade's casters, with its rendering open and
// its light matrix pushed): each visible caster with a leaf set, with its record (drawn in already: the crown and the
// wind), through leaf_shadow.vert - as many leaves as the scene draws at its distance. Rebinds what it changes; the
// cascade's rendering ends right after.
void Device::DrawLeafShadows(VkCommandBuffer cmd)
{
    if (!m_leaf.on || !m_leaf.shadows || !m_leafShadowPipeline || !m_leafPool)
        return;
    bool bound = false;
    const float reach = std::max(m_leaf.distance, 1.0f);
    for (uint32_t i : m_cascadeVisible) {
        const ShadowItem& item = m_shadowItems[i];
        if (!item.leafKey || !item.texture || item.record >= kNoShadowRecord)
            continue;
        auto it = m_leafSets.find(item.leafKey);
        if (it == m_leafSets.end() || !it->second.count)
            continue;
        const LeafSet& set = it->second;
        float d2 = 0.0f;
        for (int j = 0; j < 3; ++j) {
            float d = std::max(std::max(item.boundsMin[j] - m_frameEye[j], m_frameEye[j] - item.boundsMax[j]), 0.0f);
            d2 += d * d;
        }
        const float keep = std::clamp((reach - std::sqrt(d2)) / (0.5f * reach), 0.0f, 1.0f);
        const uint32_t count = uint32_t(float(set.count) * keep * keep);
        if (!count)
            continue;
        if (!bound) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_leafShadowPipeline);
            vkCmdSetPrimitiveTopology(cmd, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
            vkCmdSetVertexInputEXT(cmd, 0, nullptr, 0, nullptr);
            VkDescriptorBufferInfo pool{m_leafPool, 0, VkDeviceSize(m_leafPoolLeaves) * sizeof(LeafRecord)};
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstBinding = 2;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w.pBufferInfo = &pool;
            vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPipelineLayout, 0, 1, &w);
            bound = true;
        }
        VkDescriptorImageInfo image{SamplerFor(0), item.texture->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &image;
        vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPipelineLayout, 0, 1, &w);
        vkCmdDraw(cmd, count * 6u, 1, set.first * 6u, item.record);
        m_leafShadowLeaves += count;
    }
}

void Device::DestroyLeafResources()
{
    for (auto& [key, set] : m_leafSets)
        set.alloc = 0;
    m_leafSets.clear();
    for (auto& t : m_leafTrash)
        if (t.buffer)
            vmaDestroyBuffer(m_allocator, t.buffer, t.allocation);
    m_leafTrash.clear();
    if (m_leafPoolBlock) {
        vmaClearVirtualBlock(m_leafPoolBlock);
        vmaDestroyVirtualBlock(m_leafPoolBlock);
        m_leafPoolBlock = nullptr;
    }
    if (m_leafPool)
        vmaDestroyBuffer(m_allocator, m_leafPool, m_leafPoolAllocation);
    m_leafPool = VK_NULL_HANDLE;
    m_leafPoolAllocation = nullptr;
    m_leafPoolLeaves = 0;
    for (VkPipeline& p : m_leafPipelines) {
        if (p) vkDestroyPipeline(m_device, p, nullptr);
        p = VK_NULL_HANDLE;
    }
}

}  // namespace rvk
