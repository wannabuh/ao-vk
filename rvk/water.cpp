// Enhancement: the water (RVK_Water). The game's water - VisualLiquid_t: a world-space triangle mesh per liquid type
// (DisplaySystem's global arrays, FVF 0x152) that it transforms to the screen with ProcessVertices and draws flat,
// with a scrolled texture, in two or three blended passes - is replaced by our own surface: the proxy keeps the world
// vertices from the ProcessVertices call (rvk_device.cpp) and, at the game's first draw of them in the frame, hands
// them here instead (DrawWater); the game's draws of them are skipped.
//
// The game's water triangles are huge (it subdivides only edges over 10000 units), so they can't carry waves. We draw
// a polar grid around the camera instead (water.vert: rings growing geometrically, as dense as their distance needs;
// one mesh, no cracks), lifted onto the water by a water map - the game's flat triangles rasterised top-down into an
// image (R: the surface's height, G: water here) - and moved by Gerstner waves. Steep triangles (falls) are drawn as
// they are (mesh mode).
//
// Drawn where the game draws its water, in the scene's rendering: what is under and behind the water must be known,
// so the scene's colour and depth are copied first (once a frame) and the water drawn opaque over them (water.frag:
// refraction, absorption, caustics, reflections, foam, fog), writing depth as a surface does.
#include "internal.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace rvk {

using namespace vk;
using namespace detail;

namespace {

const uint32_t kWaterVertSpirv[] = {
#include "water.vert.inc"
};
const uint32_t kWaterFragSpirv[] = {
#include "water.frag.inc"
};

// The per-draw block (water_common.glsl WaterFrame, std140).
struct WaterFrame {
    d3d::Matrix viewProj, prevViewProj, view;
    float proj[4], camera[4], viewport[4], time[4], wind[4], map[4], mapInfo[4], grid[4];
    float sunColour[4], sunDir[4], ambient[4], tint[4], look[4], look2[4], fogColour[4], fogParams[4], sky[4];
    float env[4];
};

constexpr uint32_t kDetailSize = 256;
constexpr uint32_t kMapMax = 1024;              // water map side (texels)
constexpr float kGridFirstRing = 0.25f;         // world units
constexpr uint32_t kGridMaxRings = 720;

void ArgbTo4(uint32_t c, float out[4])
{
    out[0] = ((c >> 16) & 0xFF) / 255.0f;
    out[1] = ((c >> 8) & 0xFF) / 255.0f;
    out[2] = (c & 0xFF) / 255.0f;
    out[3] = (c >> 24) / 255.0f;
}

float AsFloatBits(uint32_t v)
{
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

uint16_t ToHalf(float f)
{
    uint32_t x;
    std::memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000u;
    int32_t exp = int32_t((x >> 23) & 0xFF) - 127 + 15;
    uint32_t mant = x & 0x7FFFFFu;
    if (exp <= 0)
        return uint16_t(sign);                  // tiny: zero (the map's heights are never that small and non-zero)
    if (exp >= 31)
        return uint16_t(sign | 0x7BFFu);        // the largest finite half
    uint32_t h = sign | (uint32_t(exp) << 10) | (mant >> 13);
    if (mant & 0x1000u)
        ++h;                                    // round half up
    return uint16_t(h);
}

// A small deterministic random sequence (the detail texture is the same every run).
struct Rng {
    uint32_t s;
    float Next() { s = s * 1664525u + 1013904223u; return float(s >> 8) / 16777216.0f; }
};

// Each level of the detail texture (RGBA8): RG the ripples' slope (a sum of waves with whole wave numbers, so it
// tiles), B a soft noise (the foam's breakup), A the cells' edges (F2 - F1 of a tiled cell noise: the caustics'
// web). Then box-filtered mip levels.
std::vector<std::vector<uint8_t>> MakeDetail()
{
    const uint32_t n = kDetailSize;
    std::vector<float> sx(n * n), sz(n * n), noise(n * n), cells(n * n);
    Rng rng{12345u};
    const float twoPi = 6.2831853f;
    struct Wave { float kx, kz, amp, phase; };
    std::vector<Wave> ripples, soft;
    for (int i = 0; i < 48; ++i) {
        int kx = 0, kz = 0;
        while (kx == 0 && kz == 0) {
            kx = int(rng.Next() * 25.0f) - 12;
            kz = int(rng.Next() * 25.0f) - 12;
        }
        float k = std::sqrt(float(kx * kx + kz * kz));
        ripples.push_back({float(kx), float(kz), 1.0f / std::pow(k, 1.6f), rng.Next() * twoPi});
    }
    for (int i = 0; i < 24; ++i) {
        int kx = int(rng.Next() * 13.0f) - 6, kz = int(rng.Next() * 13.0f) - 6;
        if (kx == 0 && kz == 0) kx = 1;
        float k = std::sqrt(float(kx * kx + kz * kz));
        soft.push_back({float(kx), float(kz), 1.0f / k, rng.Next() * twoPi});
    }
    // Cell points: 6 x 6 cells, one point each (tiled by wrapping).
    const int cellsPerSide = 6;
    std::vector<float> px(cellsPerSide * cellsPerSide), pz(px.size());
    for (size_t i = 0; i < px.size(); ++i) {
        px[i] = rng.Next();
        pz[i] = rng.Next();
    }
    float maxSlope = 0.0f, minNoise = 1e9f, maxNoise = -1e9f, maxCell = 0.0f;
    for (uint32_t y = 0; y < n; ++y)
        for (uint32_t x = 0; x < n; ++x) {
            float u = float(x) / float(n), v = float(y) / float(n);
            float gx = 0.0f, gz = 0.0f, h = 0.0f;
            for (const Wave& w : ripples) {
                float th = twoPi * (w.kx * u + w.kz * v) + w.phase;
                float c = std::cos(th);
                gx += w.amp * w.kx * c;
                gz += w.amp * w.kz * c;
            }
            for (const Wave& w : soft)
                h += w.amp * std::sin(twoPi * (w.kx * u + w.kz * v) + w.phase);
            // The cells' domain warped (whole frequencies: it still tiles), so their edges curve like caustics do.
            float wu = u + 0.045f * std::sin(twoPi * (2.0f * v + 0.3f)) + 0.03f * std::sin(twoPi * (3.0f * u + 5.0f * v));
            float wv = v + 0.045f * std::sin(twoPi * (2.0f * u + 0.7f)) + 0.03f * std::sin(twoPi * (4.0f * u - 3.0f * v));
            float cu = wu * cellsPerSide, cv = wv * cellsPerSide;
            int ci = int(std::floor(cu)), cj = int(std::floor(cv));
            float f1 = 1e9f, f2 = 1e9f;
            for (int dj = -1; dj <= 1; ++dj)
                for (int di = -1; di <= 1; ++di) {
                    int i = ci + di, j = cj + dj;
                    int wi = ((i % cellsPerSide) + cellsPerSide) % cellsPerSide;
                    int wj = ((j % cellsPerSide) + cellsPerSide) % cellsPerSide;
                    float ex = float(i) + px[size_t(wj * cellsPerSide + wi)] - cu;
                    float ez = float(j) + pz[size_t(wj * cellsPerSide + wi)] - cv;
                    float d = std::sqrt(ex * ex + ez * ez);
                    if (d < f1) { f2 = f1; f1 = d; } else if (d < f2) f2 = d;
                }
            size_t idx = size_t(y) * n + x;
            sx[idx] = gx;
            sz[idx] = gz;
            noise[idx] = h;
            cells[idx] = f2 - f1;
            maxSlope = std::max({maxSlope, std::fabs(gx), std::fabs(gz)});
            minNoise = std::min(minNoise, h);
            maxNoise = std::max(maxNoise, h);
            maxCell = std::max(maxCell, f2 - f1);
        }
    std::vector<std::vector<uint8_t>> levels;
    std::vector<float> rgba(size_t(n) * n * 4);
    auto q = [](float v) { return uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    for (size_t i = 0; i < size_t(n) * n; ++i) {
        rgba[i * 4 + 0] = 0.5f + 0.5f * sx[i] / maxSlope;
        rgba[i * 4 + 1] = 0.5f + 0.5f * sz[i] / maxSlope;
        rgba[i * 4 + 2] = (noise[i] - minNoise) / std::max(maxNoise - minNoise, 1e-6f);
        rgba[i * 4 + 3] = std::min(cells[i] / (maxCell * 0.6f), 1.0f);
    }
    uint32_t size = n;
    for (;;) {
        std::vector<uint8_t> level(size_t(size) * size * 4);
        for (size_t i = 0; i < level.size(); ++i)
            level[i] = q(rgba[i]);
        levels.push_back(std::move(level));
        if (size == 1)
            break;
        uint32_t half = size / 2;
        std::vector<float> next(size_t(half) * half * 4);
        for (uint32_t y = 0; y < half; ++y)
            for (uint32_t x = 0; x < half; ++x)
                for (int c = 0; c < 4; ++c) {
                    auto at = [&](uint32_t xx, uint32_t yy) { return rgba[(size_t(yy) * size + xx) * 4 + c]; };
                    next[(size_t(y) * half + x) * 4 + c] =
                        0.25f * (at(2 * x, 2 * y) + at(2 * x + 1, 2 * y) + at(2 * x, 2 * y + 1) + at(2 * x + 1, 2 * y + 1));
                }
        rgba = std::move(next);
        size = half;
    }
    return levels;
}

uint64_t HashWater(const Device::WaterVertex* v, uint32_t count, const uint16_t* idx, uint32_t icount)
{
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const void* data, size_t bytes) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < bytes; ++i)
            h = (h ^ p[i]) * 1099511628211ull;
    };
    for (uint32_t i = 0; i < count; ++i)
        mix(v[i].pos, 12);
    mix(idx, size_t(icount) * 2);
    return h ^ count;
}

}  // namespace

bool Device::CreateWaterResources(std::string* error)
{
    DestroyWaterResources();
    // One pushed set: 0 the block, 2 the mesh's vertices, 4 the frame lights, 5 the sun's shadows, 7 the water map, 8
    // the scene's depth, 9 the scene, 10 the detail texture, 11 the game's texture, 12 the environment probe's atlas.
    const uint32_t numbers[10] = {0, 2, 4, 5, 7, 8, 9, 10, 11, 12};
    VkDescriptorSetLayoutBinding b[10] = {};
    const VkShaderStageFlags both = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    for (int i = 0; i < 10; ++i) {
        b[i].binding = numbers[i];
        b[i].descriptorCount = 1;
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[i].stageFlags = both;
    }
    b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    sl.bindingCount = 10;
    sl.pBindings = b;
    if (!Check(vkCreateDescriptorSetLayout(m_device, &sl, nullptr, &m_waterSetLayout), "vkCreateDescriptorSetLayout",
               error))
        return false;
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &m_waterSetLayout;
    if (!Check(vkCreatePipelineLayout(m_device, &pl, nullptr, &m_waterLayout), "vkCreatePipelineLayout", error))
        return false;
    auto module = [&](const uint32_t* code, size_t size, VkShaderModule* out) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = size;
        ci.pCode = code;
        return Check(vkCreateShaderModule(m_device, &ci, nullptr, out), "vkCreateShaderModule", error);
    };
    VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE;
    bool ok = module(kWaterVertSpirv, sizeof(kWaterVertSpirv), &vert) &&
              module(kWaterFragSpirv, sizeof(kWaterFragSpirv), &frag);
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
        rs.cullMode = VK_CULL_MODE_NONE;         // seen from below too (the camera under the water)
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
                                    GetFormatInfo(Format::RGBA8).vk, GetFormatInfo(Format::RGBA16F).vk,
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
        ci.layout = m_waterLayout;
        ok = Check(vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &ci, nullptr, &m_waterPipeline),
                   "vkCreateGraphicsPipelines", error);
    }
    if (vert) vkDestroyShaderModule(m_device, vert, nullptr);
    if (frag) vkDestroyShaderModule(m_device, frag, nullptr);
    if (!ok)
        return false;
    // Samplers: the detail texture tiles (mipmapped, anisotropic); the game's texture tiles too.
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.maxLod = VK_LOD_CLAMP_NONE;
    if (m_maxAnisotropy >= 1.0f) {
        si.anisotropyEnable = VK_TRUE;
        si.maxAnisotropy = std::min(m_maxAnisotropy, 8.0f);
    }
    if (!Check(vkCreateSampler(m_device, &si, nullptr, &m_waterRepeatSampler), "water sampler", error))
        return false;
    // The detail texture.
    std::vector<std::vector<uint8_t>> levels = MakeDetail();
    m_waterDetail = CreateImage(kDetailSize, kDetailSize, Format::RGBA8, uint32_t(levels.size()), false);
    if (!m_waterDetail) {
        if (error) *error = "water detail texture";
        return false;
    }
    for (uint32_t l = 0; l < levels.size(); ++l) {
        uint32_t s = std::max(kDetailSize >> l, 1u);
        UpdateTexture(m_waterDetail, l, 0, 0, s, s, levels[l].data(), s * 4);
    }
    return true;
}

void Device::DestroyWaterResources()
{
    m_waterReady.store(false, std::memory_order_relaxed);
    for (WaterMap& m : m_waterMaps)
        if (m.texture) DestroyTextureNow(m.texture);
    m_waterMaps.clear();
    if (m_waterDetail) { DestroyTextureNow(m_waterDetail); m_waterDetail = nullptr; }
    if (m_waterSceneCopy) { DestroyTextureNow(m_waterSceneCopy); m_waterSceneCopy = nullptr; }
    if (m_waterDepthCopy) {
        vkDestroyImageView(m_device, m_waterDepthCopyView, nullptr);
        vmaDestroyImage(m_allocator, m_waterDepthCopy, m_waterDepthCopyAllocation);
        m_waterDepthCopy = VK_NULL_HANDLE;
        m_waterDepthCopyView = VK_NULL_HANDLE;
        m_waterDepthCopyAllocation = nullptr;
    }
    for (WaterGrid& g : m_waterGrids)
        if (g.buffer) vmaDestroyBuffer(m_allocator, g.buffer, g.allocation);
    m_waterGrids.clear();
    if (m_waterRepeatSampler) { vkDestroySampler(m_device, m_waterRepeatSampler, nullptr); m_waterRepeatSampler = VK_NULL_HANDLE; }
    if (m_waterPipeline) { vkDestroyPipeline(m_device, m_waterPipeline, nullptr); m_waterPipeline = VK_NULL_HANDLE; }
    if (m_waterLayout) { vkDestroyPipelineLayout(m_device, m_waterLayout, nullptr); m_waterLayout = VK_NULL_HANDLE; }
    if (m_waterSetLayout) { vkDestroyDescriptorSetLayout(m_device, m_waterSetLayout, nullptr); m_waterSetLayout = VK_NULL_HANDLE; }
    m_waterDepthW = m_waterDepthH = 0;
}

// Whether the water can be drawn now (the proxy hands the game's water over only then): at the frame's start.
void Device::UpdateWaterReady()
{
    m_waterReady.store(m_waterParams.enable && m_waterPipeline && m_waterDetail && m_hdr,
                       std::memory_order_relaxed);
}

// The grid's index buffer for a number of segments around (quads between ring i and i + 1, ring-major: the first k
// rings' triangles are a prefix).
const Device::WaterGrid* Device::WaterGridFor(uint32_t segments)
{
    for (const WaterGrid& g : m_waterGrids)
        if (g.segments == segments)
            return &g;
    const VkDeviceSize count = VkDeviceSize(segments) * kGridMaxRings * 6;
    const VkDeviceSize bytes = count * sizeof(uint32_t);
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = bytes;
    bi.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo ac{};
    ac.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    WaterGrid g;
    g.segments = segments;
    std::string err;
    if (!Check(vmaCreateBuffer(m_allocator, &bi, &ac, &g.buffer, &g.allocation, nullptr), "water grid", &err))
        return nullptr;
    EnsureRingSpace(bytes + 64);
    void* cpu;
    const VkDeviceSize staging = Allocate(bytes, 16, &cpu);
    uint32_t* out = static_cast<uint32_t*>(cpu);
    for (uint32_t r = 0; r < kGridMaxRings; ++r)
        for (uint32_t s = 0; s < segments; ++s) {
            uint32_t s1 = (s + 1) % segments;
            uint32_t a = r * segments + s, b = r * segments + s1;
            uint32_t c = (r + 1) * segments + s, d = (r + 1) * segments + s1;
            *out++ = a; *out++ = c; *out++ = d;
            *out++ = a; *out++ = d; *out++ = b;
        }
    VkBufferCopy region{staging, 0, bytes};
    vkCmdCopyBuffer(UploadCommands(), m_frames[m_frameIndex].ring, g.buffer, 1, &region);
    m_skinUploadsPending = true;
    m_waterGrids.push_back(g);
    return &m_waterGrids.back();
}

// The water map of a mesh (made the first time it is seen; the last few kept): its flat triangles rasterised
// top-down. steep: the triangles left out (falls), as indices into the mesh.
Device::WaterMap* Device::WaterMapFor(const WaterVertex* v, uint32_t count, const uint16_t* idx, uint32_t icount,
                                      std::vector<uint32_t>& steep)
{
    steep.clear();
    struct Tri { uint32_t a, b, c; };
    std::vector<Tri> flat;
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    double heightSum = 0.0;
    for (uint32_t t = 0; t + 2 < icount; t += 3) {
        uint32_t i0 = idx[t], i1 = idx[t + 1], i2 = idx[t + 2];
        if (i0 >= count || i1 >= count || i2 >= count)
            continue;
        const float* p0 = v[i0].pos;
        const float* p1 = v[i1].pos;
        const float* p2 = v[i2].pos;
        float e1[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
        float e2[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
        float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (len <= 1e-8f)
            continue;
        if (std::fabs(n[1]) / len < 0.9f) {
            steep.insert(steep.end(), {i0, i1, i2});
            continue;
        }
        flat.push_back({i0, i1, i2});
        for (const float* p : {p0, p1, p2})
            for (int k = 0; k < 3; ++k) {
                lo[k] = std::min(lo[k], p[k]);
                hi[k] = std::max(hi[k], p[k]);
            }
        heightSum += double(p0[1] + p1[1] + p2[1]) / 3.0;
    }
    if (flat.empty())
        return nullptr;
    const uint64_t key = HashWater(v, count, idx, icount);
    for (WaterMap& m : m_waterMaps)
        if (m.key == key) {
            m.lastFrame = m_frameNumber;
            return &m;
        }
    // A new map: in place of the one not used the longest once a few are kept.
    if (m_waterMaps.size() >= 4) {
        auto oldest = std::min_element(m_waterMaps.begin(), m_waterMaps.end(),
                                       [](const WaterMap& a, const WaterMap& b) { return a.lastFrame < b.lastFrame; });
        if (oldest->texture) DestroyTexture(oldest->texture);
        m_waterMaps.erase(oldest);
    }
    WaterMap m;
    m.key = key;
    m.lastFrame = m_frameNumber;
    m.base = float(heightSum / double(flat.size()));
    const float ex = hi[0] - lo[0], ez = hi[2] - lo[2];
    float texel = std::max(std::max(ex, ez) / float(kMapMax - 2), 0.5f);
    const uint32_t w = std::min(uint32_t(std::ceil(ex / texel)) + 2, kMapMax);
    const uint32_t h = std::min(uint32_t(std::ceil(ez / texel)) + 2, kMapMax);
    m.origin[0] = lo[0] - texel;
    m.origin[1] = lo[2] - texel;
    m.size[0] = float(w) * texel;
    m.size[1] = float(h) * texel;
    m.texel = texel;
    m.extent = std::max(ex, ez);
    m.lo[0] = lo[0]; m.lo[1] = lo[2];
    m.hi[0] = hi[0]; m.hi[1] = hi[2];
    m.flatTriangles = uint32_t(flat.size());
    m.colour = v[flat.front().a].colour;
    std::vector<uint16_t> pixels(size_t(w) * h * 4, 0);
    const uint16_t one = ToHalf(1.0f);
    for (const Tri& t : flat) {
        const float* a = v[t.a].pos;
        const float* b = v[t.b].pos;
        const float* c = v[t.c].pos;
        float minX = std::min({a[0], b[0], c[0]}), maxX = std::max({a[0], b[0], c[0]});
        float minZ = std::min({a[2], b[2], c[2]}), maxZ = std::max({a[2], b[2], c[2]});
        int x0 = std::max(int(std::floor((minX - m.origin[0]) / texel)) - 1, 0);
        int x1 = std::min(int(std::ceil((maxX - m.origin[0]) / texel)) + 1, int(w) - 1);
        int z0 = std::max(int(std::floor((minZ - m.origin[1]) / texel)) - 1, 0);
        int z1 = std::min(int(std::ceil((maxZ - m.origin[1]) / texel)) + 1, int(h) - 1);
        float det = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2]);
        if (std::fabs(det) < 1e-12f)
            continue;
        // A texel counts as water when its centre is inside, with half a texel to spare (neighbouring triangles leave
        // no seam between them).
        const float slack = 0.5f * texel / std::sqrt(std::fabs(det)) * 2.0f;
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x) {
                float px = m.origin[0] + (float(x) + 0.5f) * texel, pz = m.origin[1] + (float(z) + 0.5f) * texel;
                float l0 = ((b[2] - c[2]) * (px - c[0]) + (c[0] - b[0]) * (pz - c[2])) / det;
                float l1 = ((c[2] - a[2]) * (px - c[0]) + (a[0] - c[0]) * (pz - c[2])) / det;
                float l2 = 1.0f - l0 - l1;
                if (l0 < -slack || l1 < -slack || l2 < -slack)
                    continue;
                float y = l0 * a[1] + l1 * b[1] + l2 * c[1];
                uint16_t* px4 = &pixels[(size_t(z) * w + x) * 4];
                px4[0] = ToHalf(y - m.base);
                px4[1] = one;
            }
    }
    m.texture = CreateImage(w, h, Format::RGBA16F, 1, false);
    if (!m.texture)
        return nullptr;
    UpdateTexture(m.texture, 0, 0, 0, w, h, pixels.data(), w * 8);
    m_waterMaps.push_back(m);
    WaterLog("water map %016llx: %u flat + %u steep triangles, %.0f x %.0f units at y %.1f, %u x %u texels of %.2f",
             (unsigned long long)key, m.flatTriangles, uint32_t(steep.size() / 3), double(ex), double(ez),
             double(m.base), w, h, double(texel));
    return &m_waterMaps.back();
}

void Device::WaterLog(const char* fmt, ...)
{
    if (m_waterLogCount >= 64)
        return;
    ++m_waterLogCount;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Log("%s", buf);
}

// The scene's colour and depth as they are now, into the water's copies (once a frame: a second water body sees the
// first one's background, which is what it would see through it anyway).
bool Device::CopySceneForWater(VkCommandBuffer cmd)
{
    if (m_waterCopyFrame == m_frameNumber)
        return true;
    const uint32_t w = m_scene->m_width, h = m_scene->m_height;
    if (!m_waterSceneCopy || m_waterSceneCopy->m_width != w || m_waterSceneCopy->m_height != h) {
        if (m_waterSceneCopy) DestroyTexture(m_waterSceneCopy);
        m_waterSceneCopy = CreateImage(w, h, Format::RGBA16F, 1, false);
        if (!m_waterSceneCopy)
            return false;
    }
    if (!m_waterDepthCopy || m_waterDepthW != w || m_waterDepthH != h) {
        if (m_waterDepthCopy)
            m_deadImages.push_back({DeathTag(), {m_waterDepthCopy, m_waterDepthCopyView, m_waterDepthCopyAllocation}});
        m_waterDepthCopy = VK_NULL_HANDLE;
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = kDepthFormat;
        ci.extent = {w, h, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        VmaAllocationCreateInfo ac{};
        ac.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        ac.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        if (vmaCreateImage(m_allocator, &ci, &ac, &m_waterDepthCopy, &m_waterDepthCopyAllocation, nullptr) != VK_SUCCESS) {
            m_waterDepthCopy = VK_NULL_HANDLE;
            return false;
        }
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = m_waterDepthCopy;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = kDepthFormat;
        vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        vkCreateImageView(m_device, &vi, nullptr, &m_waterDepthCopyView);
        m_waterDepthW = w;
        m_waterDepthH = h;
        m_waterDepthCopyLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    m_renderEndCause = kEndCopy;
    EndRendering();
    Transition(cmd, m_scene, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    Transition(cmd, m_waterSceneCopy, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    ImageBarrier(cmd, m_depth, VK_IMAGE_ASPECT_DEPTH_BIT, m_depthLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                 VK_ACCESS_2_TRANSFER_READ_BIT);
    m_depthLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    ImageBarrier(cmd, m_waterDepthCopy, VK_IMAGE_ASPECT_DEPTH_BIT, m_waterDepthCopyLayout,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                 VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                 VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = region.srcSubresource;
    region.extent = {w, h, 1};
    vkCmdCopyImage(cmd, m_scene->m_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_waterSceneCopy->m_image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    region.srcSubresource.aspectMask = region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    vkCmdCopyImage(cmd, m_depth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_waterDepthCopy,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    Transition(cmd, m_waterSceneCopy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    ImageBarrier(cmd, m_waterDepthCopy, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                 VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    m_waterDepthCopyLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    BeginRenderingOn(m_target);
    m_waterCopyFrame = m_frameNumber;
    return true;
}

void Device::DrawWater(const WaterVertex* vertices, uint32_t vertexCount, const uint16_t* indices, uint32_t indexCount)
{
    if (!m_inFrame || !vertexCount || indexCount < 3 || !m_waterParams.enable || !m_waterPipeline || !m_waterDetail ||
        !m_scenePhase || m_target != m_scene || !m_scene)
        return;
    ++m_frameDraw;
    std::vector<uint32_t>& steep = m_waterSteep;
    WaterMap* map = WaterMapFor(vertices, vertexCount, indices, indexCount, steep);
    if (!map && steep.empty())
        return;
    const WaterParams& P = m_waterParams;
    const uint32_t quality = std::clamp(P.quality, 1u, 3u);
    const uint32_t segments = quality == 1 ? 128u : quality == 2 ? 224u : 320u;
    const WaterGrid* grid = map ? WaterGridFor(segments) : nullptr;
    Frame& f = m_frames[m_frameIndex];
    VkCommandBuffer cmd = f.main;
    FlushGroup();                                // the scene's batched draws go first (and its profile class ends)
    ProfileSceneClass(8);
    if (!CopySceneForWater(cmd))
        return;

    // The block.
    const auto& vm = m_view.m;
    float eye[3];
    for (int i = 0; i < 3; ++i)
        eye[i] = -(vm[3][0] * vm[i][0] + vm[3][1] * vm[i][1] + vm[3][2] * vm[i][2]);
    WaterFrame wf{};
    wf.viewProj = MulMatrix(m_view, m_proj);
    wf.prevViewProj = m_prevViewProjValid ? MulMatrix(m_prevView, m_prevProj) : wf.viewProj;
    wf.view = m_view;
    wf.proj[0] = m_proj.m[2][2];
    wf.proj[1] = m_proj.m[3][2];
    wf.proj[2] = m_proj.m[0][0];
    wf.proj[3] = m_proj.m[1][1];
    wf.camera[0] = eye[0];
    wf.camera[1] = eye[1];
    wf.camera[2] = eye[2];
    wf.camera[3] = TaaActive() ? 1.0f : 0.0f;
    wf.viewport[0] = float(m_scene->m_width);
    wf.viewport[1] = float(m_scene->m_height);
    wf.viewport[2] = 1.0f / wf.viewport[0];
    wf.viewport[3] = 1.0f / wf.viewport[1];
    wf.time[0] = float(m_windTime);
    wf.time[1] = float(m_windTimePrev);
    wf.time[3] = float(quality == 1 ? 16 : quality == 2 ? 28 : 44);
    static const int debugView = [] { const char* v = std::getenv("RANDYVK_WATER_DEBUG"); return v ? std::atoi(v) : 0; }();
    wf.sky[3] = float(debugView);
    float wind[4];
    Wind(wind);
    wf.wind[0] = wind[0];
    wf.wind[1] = wind[1];
    // Wave height: the setting, and the body's size - a pond stays calm, the open sea has a swell.
    float body = map ? std::clamp((map->extent - 60.0f) / 700.0f, 0.0f, 1.0f) : 0.0f;
    wf.wind[2] = P.waves * (0.12f + 0.88f * body * body);
    wf.wind[3] = 0.35f + 0.65f * body;
    if (m_terrainSun[0] + m_terrainSun[1] + m_terrainSun[2] > 0.0f) {
        std::memcpy(wf.sunColour, m_terrainSun, 16);
        std::memcpy(wf.sunDir, m_terrainSun + 4, 16);
    }
    if (m_terrainAmbient[3] > 0.5f)
        std::memcpy(wf.ambient, m_terrainAmbient, 16);
    else
        ArgbTo4(m_rs[d3d::RS_AMBIENT], wf.ambient);
    wf.look[0] = std::clamp(P.style, 0.0f, 1.0f);
    wf.look[1] = P.reflections;
    wf.look[2] = P.refraction;
    wf.look[3] = P.clarity;
    wf.look2[0] = P.foam;
    wf.look2[1] = P.caustics;
    wf.look2[2] = P.ripples;
    wf.look2[3] = m_textures[0] ? P.gameTexture : 0.0f;
    ArgbTo4(m_rs[d3d::RS_FOGCOLOR], wf.fogColour);
    wf.fogColour[3] = m_rs[d3d::RS_FOGENABLE] ? 1.0f : 0.0f;
    wf.fogParams[0] = AsFloatBits(m_rs[d3d::RS_FOGSTART]);
    wf.fogParams[1] = AsFloatBits(m_rs[d3d::RS_FOGEND]);
    wf.fogParams[2] = AsFloatBits(m_rs[d3d::RS_FOGDENSITY]);
    wf.fogParams[3] = float(m_rs[d3d::RS_FOGTABLEMODE] ? m_rs[d3d::RS_FOGTABLEMODE] : m_rs[d3d::RS_FOGVERTEXMODE]);
    // The environment probe (hdr.cpp RenderEnvProbe): what the reflections that miss the screen show, where it has
    // seen (else the sky).
    const bool env = m_envReady && m_envAtlas && m_pbr.enabled && m_pbr.probe > 0.0f;
    wf.env[0] = env ? 1.0f : 0.0f;
    wf.env[1] = m_pbr.probe;
    // The game's water colour; a colourless one (white vertices) gets a water blue.
    uint32_t colour = map ? map->colour : vertices[0].colour;
    ArgbTo4(colour, wf.tint);
    if (wf.tint[0] > 0.9f && wf.tint[1] > 0.9f && wf.tint[2] > 0.9f) {
        wf.tint[0] = 0.05f; wf.tint[1] = 0.2f; wf.tint[2] = 0.35f;
    }

    EnsureRingSpace(2 * 512 + sizeof(FrameLights) + VkDeviceSize(vertexCount) * 16 + steep.size() * 4 + 1024);
    if (m_frameLightsDirty || m_frameLightsGeneration != m_ringGeneration)
        WriteFrameLights();
    // The mesh's vertices (mesh mode; bound in both modes - the grid doesn't read them).
    void* meshCpu = nullptr;
    const VkDeviceSize meshBytes = std::max<VkDeviceSize>(VkDeviceSize(vertexCount) * 16, 16);
    const VkDeviceSize meshOffset = Allocate(meshBytes, 256, &meshCpu);
    std::memcpy(meshCpu, vertices, size_t(vertexCount) * 16);

    const float sw = float(m_scene->m_width), sh = float(m_scene->m_height);
    VkViewport viewport{0.5f, sh + 0.5f, sw, -sh, 0.0f, 1.0f};   // the scene draws into a flipped viewport
    VkRect2D scissor{{0, 0}, {m_scene->m_width, m_scene->m_height}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_waterPipeline);

    auto draw = [&](bool meshMode) {
        WaterFrame block = wf;
        block.time[2] = meshMode ? 1.0f : 0.0f;
        if (!meshMode && map) {
            block.map[0] = map->origin[0];
            block.map[1] = map->origin[1];
            block.map[2] = 1.0f / map->size[0];
            block.map[3] = 1.0f / map->size[1];
            block.mapInfo[0] = map->base;
            block.mapInfo[1] = map->texel;
            block.mapInfo[2] = 1.0f;
        }
        const float ratio = 1.0f + 6.2831853f / float(segments);
        block.grid[0] = float(segments);
        block.grid[2] = kGridFirstRing;
        block.grid[3] = ratio;
        uint32_t rings = 0;
        if (!meshMode && map) {
            // Out to the map's farthest corner (or the fog's end, past which nothing shows).
            float reach = 0.0f;
            for (float x : {map->lo[0], map->hi[0]})
                for (float z : {map->lo[1], map->hi[1]})
                    reach = std::max(reach, std::hypot(x - eye[0], z - eye[2]));
            if (block.fogColour[3] > 0.5f && block.fogParams[3] == 3.0f && block.fogParams[1] > 0.0f)
                reach = std::min(reach, block.fogParams[1] * 1.05f);
            reach = std::clamp(reach, 1.0f, 20000.0f);
            rings = std::min(uint32_t(std::ceil(std::log(reach / kGridFirstRing) / std::log(ratio))) + 2, kGridMaxRings - 1);
            block.grid[1] = float(rings);
            if (m_frameNumber < 4)
                WaterLog("water grid: reach %.1f, %u rings x %u segments, eye %.1f %.1f %.1f", double(reach), rings,
                         segments, double(eye[0]), double(eye[1]), double(eye[2]));
        }
        void* blockCpu = nullptr;
        const VkDeviceSize blockOffset = Allocate(sizeof(WaterFrame), 256, &blockCpu);
        std::memcpy(blockCpu, &block, sizeof(block));
        Texture* mapTex = (!meshMode && map) ? map->texture : m_waterDetail;
        Texture* gameTex = m_textures[0] ? m_textures[0] : m_waterDetail;
        VkDescriptorBufferInfo blockInfo{f.ring, blockOffset, sizeof(WaterFrame)};
        VkDescriptorBufferInfo meshInfo{f.ring, meshOffset, meshBytes};
        VkDescriptorBufferInfo lightsInfo{f.ring, m_frameLightsOffset, sizeof(FrameLights)};
        VkDescriptorImageInfo shadowInfo{m_shadowSampler, m_shadowView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo mapInfo{m_linearSampler, mapTex->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo depthInfo{m_pointSampler, m_waterDepthCopyView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo sceneInfo{m_linearSampler, m_waterSceneCopy->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo detailInfo{m_waterRepeatSampler, m_waterDetail->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo gameInfo{m_waterRepeatSampler, gameTex->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo envInfo{m_linearSampler, (env ? m_envAtlas : m_waterDetail)->m_view,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet writes[10] = {};
        const uint32_t bindings[10] = {0, 2, 4, 5, 7, 8, 9, 10, 11, 12};
        for (int i = 0; i < 10; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].descriptorCount = 1;
            writes[i].dstBinding = bindings[i];
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        }
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo = &blockInfo;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].pBufferInfo = &meshInfo;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[2].pBufferInfo = &lightsInfo;
        writes[3].pImageInfo = &shadowInfo;
        writes[4].pImageInfo = &mapInfo;
        writes[5].pImageInfo = &depthInfo;
        writes[6].pImageInfo = &sceneInfo;
        writes[7].pImageInfo = &detailInfo;
        writes[8].pImageInfo = &gameInfo;
        writes[9].pImageInfo = &envInfo;
        vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_waterLayout, 0, 10, writes);
        if (meshMode) {
            void* idxCpu = nullptr;
            const VkDeviceSize idxOffset = Allocate(VkDeviceSize(steep.size()) * 4, 16, &idxCpu);
            std::memcpy(idxCpu, steep.data(), steep.size() * 4);
            vkCmdBindIndexBuffer(cmd, f.ring, idxOffset, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, uint32_t(steep.size()), 1, 0, 0, 0);
            m_waterTriangles += steep.size() / 3;
        } else {
            vkCmdBindIndexBuffer(cmd, grid->buffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, rings * segments * 6, 1, 0, 0, 0);
            m_waterTriangles += uint64_t(rings) * segments * 2;
        }
    };
    if (map && grid)
        draw(false);
    if (!steep.empty())
        draw(true);
    m_cache = StateCache{};                      // this pipeline is not the scene's
    m_arenaBound = m_bindlessBound = false;      // ... and its pushed set 0 replaced the scene's: rebind those too
    ProfileMark("water");
    ++m_waterDraws;
}

}  // namespace rvk
