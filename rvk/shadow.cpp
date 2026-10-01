// Sun shadows (an enhancement; D3D7 has none). The frame's opaque 3D draws are recorded as shadow casters while
// they are drawn (their geometry is already in the frame's ring buffer); at the end of the frame they are drawn
// again from the sun into a depth map, which the next frame samples. Shadows therefore lag one frame.
//
// The sun is the brightest directional light the game used during the frame. The map covers a square around
// the camera, oriented with the sun and snapped to whole texels so it doesn't shimmer while the camera moves.
#include "internal.h"

#include <algorithm>
#include <cmath>
#include <climits>
#include <cstring>

namespace rvk {

using namespace vk;
using namespace detail;

namespace {

const uint32_t kShadowVertSpirv[] = {
#include "shadow.vert.inc"
};
const uint32_t kShadowFragSpirv[] = {
#include "shadow.frag.inc"
};

struct ShadowPush {
    d3d::Matrix worldLightViewProj;
    float alpha[4];
};

d3d::Matrix Mul(const d3d::Matrix& a, const d3d::Matrix& b) { return MulMatrix(a, b); }

// Fast 64-bit hash of a byte range (identity of a caster's geometry).
uint64_t HashBytes(const void* data, size_t size, uint64_t h)
{
    const uint8_t* p = static_cast<const uint8_t*>(data);
    auto mix = [&](uint64_t w) { h ^= w; h *= 0x9E3779B97F4A7C15ull; h ^= h >> 29; };
    for (; size >= 8; size -= 8, p += 8) { uint64_t w; std::memcpy(&w, p, 8); mix(w); }
    uint64_t tail = 0;
    std::memcpy(&tail, p, size);
    mix(tail ^ (uint64_t(size) << 56));
    return h;
}

// Clip-space test of a world-space box's corners against a view-projection: +1 all inside, -1 all outside one
// plane, 0 otherwise.
int BoxInClip(const float mn[3], const float mx[3], const d3d::Matrix& vp, bool depth)
{
    int outside[6] = {}, inside = 0;
    for (int k = 0; k < 8; ++k) {
        float p[3] = {(k & 1) ? mx[0] : mn[0], (k & 2) ? mx[1] : mn[1], (k & 4) ? mx[2] : mn[2]}, c[4];
        for (int j = 0; j < 4; ++j)
            c[j] = p[0] * vp.m[0][j] + p[1] * vp.m[1][j] + p[2] * vp.m[2][j] + vp.m[3][j];
        bool in = c[3] > 0.0f && std::fabs(c[0]) <= c[3] && std::fabs(c[1]) <= c[3] &&
                  (!depth || (c[2] >= 0.0f && c[2] <= c[3]));
        inside += in;
        outside[0] += c[0] < -c[3]; outside[1] += c[0] > c[3];
        outside[2] += c[1] < -c[3]; outside[3] += c[1] > c[3];
        outside[4] += depth && c[2] < 0.0f; outside[5] += depth && c[2] > c[3];
    }
    if (inside == 8) return 1;
    for (int o : outside) if (o == 8) return -1;
    return 0;
}

void Normalize(float v[3])
{
    float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 0.0f) { v[0] /= l; v[1] /= l; v[2] /= l; }
}

void Cross(const float a[3], const float b[3], float out[3])
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

float Dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

}  // namespace

d3d::Matrix detail::MulMatrix(const d3d::Matrix& a, const d3d::Matrix& b)
{
    d3d::Matrix r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
    return r;
}

bool Device::CreateShadowResources(std::string* error)
{
    // Depth map, sampled with comparison.
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = kDepthFormat;
    ci.extent = {kShadowSize, kShadowSize, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo ac{};
    ac.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    ac.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    if (!Check(vmaCreateImage(m_allocator, &ci, &ac, &m_shadowImage, &m_shadowAllocation, nullptr), "shadow map", error))
        return false;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = m_shadowImage;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = kDepthFormat;
    vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    if (!Check(vkCreateImageView(m_device, &vi, nullptr, &m_shadowView), "shadow map view", error))
        return false;
    m_shadowImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;               // 2x2 comparison filtering per tap
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;          // outside the map: lit
    si.compareEnable = VK_TRUE;
    si.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    si.maxLod = 0.0f;
    if (!Check(vkCreateSampler(m_device, &si, nullptr, &m_shadowSampler), "shadow sampler", error))
        return false;

    // Pass pipelines: depth only (opaque casters) and with an alpha-testing fragment shader.
    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    sl.bindingCount = 1;
    sl.pBindings = &binding;
    if (!Check(vkCreateDescriptorSetLayout(m_device, &sl, nullptr, &m_shadowSetLayout), "shadow set layout", error))
        return false;
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ShadowPush)};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &m_shadowSetLayout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &push;
    if (!Check(vkCreatePipelineLayout(m_device, &pl, nullptr, &m_shadowPipelineLayout), "shadow pipeline layout", error))
        return false;

    auto module = [&](const uint32_t* code, size_t size, VkShaderModule* out) {
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = size;
        mi.pCode = code;
        return Check(vkCreateShaderModule(m_device, &mi, nullptr, out), "shadow shader module", error);
    };
    VkShaderModule vert, frag;
    if (!module(kShadowVertSpirv, sizeof(kShadowVertSpirv), &vert) || !module(kShadowFragSpirv, sizeof(kShadowFragSpirv), &frag))
        return false;
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vert, "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main", nullptr},
    };
    VkDynamicState dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
                                VK_DYNAMIC_STATE_VERTEX_INPUT_EXT, VK_DYNAMIC_STATE_DEPTH_BIAS};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = sizeof(dynamic) / sizeof(dynamic[0]);
    ds.pDynamicStates = dynamic;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;                               // both sides: no light leaks through thin walls
    rs.depthBiasEnable = VK_TRUE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo dss{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    dss.depthTestEnable = dss.depthWriteEnable = VK_TRUE;
    dss.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.depthAttachmentFormat = kDepthFormat;
    bool ok = true;
    for (int alphaTest = 0; alphaTest < 2 && ok; ++alphaTest) {
        VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gi.pNext = &rendering;
        gi.stageCount = alphaTest ? 2 : 1;
        gi.pStages = stages;
        gi.pInputAssemblyState = &ia;
        gi.pViewportState = &vp;
        gi.pRasterizationState = &rs;
        gi.pMultisampleState = &ms;
        gi.pDepthStencilState = &dss;
        gi.pColorBlendState = &cb;
        gi.pDynamicState = &ds;
        gi.layout = m_shadowPipelineLayout;
        ok = Check(vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &gi, nullptr, &m_shadowPipelines[alphaTest]),
                   "shadow pipeline", error);
    }
    vkDestroyShaderModule(m_device, vert, nullptr);
    vkDestroyShaderModule(m_device, frag, nullptr);
    return ok;
}

void Device::DestroyShadowResources()
{
    for (auto& [key, e] : m_casterCache) vmaDestroyBuffer(m_allocator, e.buffer, e.allocation);
    m_casterCache.clear();
    for (auto& [tag, b] : m_deadBuffers) vmaDestroyBuffer(m_allocator, b.first, b.second);
    m_deadBuffers.clear();
    for (VkPipeline& p : m_shadowPipelines) if (p) { vkDestroyPipeline(m_device, p, nullptr); p = VK_NULL_HANDLE; }
    if (m_shadowPipelineLayout) vkDestroyPipelineLayout(m_device, m_shadowPipelineLayout, nullptr);
    if (m_shadowSetLayout) vkDestroyDescriptorSetLayout(m_device, m_shadowSetLayout, nullptr);
    if (m_shadowSampler) vkDestroySampler(m_device, m_shadowSampler, nullptr);
    if (m_shadowView) vkDestroyImageView(m_device, m_shadowView, nullptr);
    if (m_shadowImage) vmaDestroyImage(m_allocator, m_shadowImage, m_shadowAllocation);
    m_shadowPipelineLayout = VK_NULL_HANDLE;
    m_shadowSetLayout = VK_NULL_HANDLE;
    m_shadowSampler = VK_NULL_HANDLE;
    m_shadowView = VK_NULL_HANDLE;
    m_shadowImage = VK_NULL_HANDLE;
}

// Before the frame's first rendering: the map must be in a sampleable layout from the start (cleared = lit).
void Device::PrepareShadowMap(VkCommandBuffer cmd)
{
    if (m_shadowImageLayout != VK_IMAGE_LAYOUT_UNDEFINED)
        return;
    ImageBarrier(cmd, m_shadowImage, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkClearDepthStencilValue clear{1.0f, 0};
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    vkCmdClearDepthStencilImage(cmd, m_shadowImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
    ImageBarrier(cmd, m_shadowImage, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                 VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    m_shadowImageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

// AnarchyGround_t: world-space chunks (identity world matrix) of position, normal and two texture sets. The
// ground doesn't cast: its lightmap has the hills' shading baked in, and coarse chunks of the ground's levels of
// detail overlap finer ones and would cast blocky shadows onto them.
bool Device::IsTerrain(uint32_t fvf) const
{
    if (fvf != (d3d::FVF_XYZ | d3d::FVF_NORMAL | (2u << 8)))
        return false;
    const auto& w = m_world.m;
    return w[3][0] == 0.0f && w[3][1] == 0.0f && w[3][2] == 0.0f && w[0][0] == 1.0f && w[1][1] == 1.0f && w[2][2] == 1.0f;
}

// 3D triangles drawn into the main target with depth writes: opaque, alpha-tested, or alpha-blended the way
// Anarchy Online draws most static objects (blended, but writing depth - solid apart from the texture's cut-out
// parts). That leaves out the sky, effects, see-through surfaces and pre-transformed (XYZRHW) geometry.
bool Device::IsShadowCaster(uint32_t primitive, uint32_t fvf) const
{
    if (!m_shadows || m_target != m_main || TopologyClassOf(primitive) != 2 ||
        (fvf & d3d::FVF_POSITION_MASK) != d3d::FVF_XYZ || !m_rs[d3d::RS_ZENABLE] || !m_rs[d3d::RS_ZWRITEENABLE])
        return false;
    if (IsTerrain(fvf))
        return false;
    if (!m_rs[d3d::RS_ALPHABLENDENABLE])
        return true;
    bool alphaBlend = m_rs[d3d::RS_SRCBLEND] == d3d::BLEND_SRCALPHA && m_rs[d3d::RS_DESTBLEND] == d3d::BLEND_INVSRCALPHA;
    bool translucentMaterial = m_rs[d3d::RS_LIGHTING] && m_material.diffuse.a < 0.5f;     // glass and the like
    return alphaBlend && !translucentMaterial;
}

// Called by Draw for every draw, after its geometry went into the ring.
void Device::RecordShadowCaster(uint32_t primitive, uint32_t fvf, uint32_t stride, const void* vertices,
                                uint32_t vertexCount, VkDeviceSize vbOffset, const uint16_t* indices, uint32_t indexCount,
                                VkDeviceSize ibOffset)
{
    if (!IsShadowCaster(primitive, fvf))
        return;
    UpdateFrameEye();
    if (!m_frameViewProjValid) {                 // the world camera: taken at the first world object, not the sky
        m_frameViewProj = MulMatrix(m_view, m_proj);
        m_frameViewProjValid = true;
    }
    ShadowCaster c;
    c.primitive = primitive;
    c.stride = stride;
    c.vertexCount = vertexCount;
    c.indexCount = indexCount;
    c.vbOffset = vbOffset;
    c.ibOffset = ibOffset;
    c.world = m_world;
    c.generation = m_ringGeneration;
    ShadowCutout(fvf, &c.texture, &c.texOffset, &c.alphaRef);
    m_casters.push_back(c);

    uint64_t key = CasterKey(primitive, fvf, stride, vertices, vertexCount, indices, indexCount);
    auto cached = m_casterCache.find(key);
    if (cached != m_casterCache.end()) {
        cached->second.lastSeen = m_frameNumber;
        return;
    }
    CasterStreak& streak = m_casterStreaks[key];
    if (streak.lastFrame + 1 == m_frameNumber) ++streak.count;
    else if (streak.lastFrame != m_frameNumber) streak.count = 1;
    streak.lastFrame = m_frameNumber;
    if (streak.count >= kPromoteFrames)
        CacheCaster(key, c, vertices, indices);
}

// Alpha-tested and alpha-blended casters are cut out where texture 0 is transparent: the alpha test's reference,
// or half for blended surfaces.
void Device::ShadowCutout(uint32_t fvf, Texture** texture, int* texOffset, float* alphaRef) const
{
    *texture = nullptr;
    *texOffset = -1;
    *alphaRef = -1.0f;
    bool alphaTest = m_rs[d3d::RS_ALPHATESTENABLE] != 0, alphaBlend = m_rs[d3d::RS_ALPHABLENDENABLE] != 0;
    if (!(alphaTest || alphaBlend) || !m_textures[0])
        return;
    int offset = DecodeFvf(fvf).offset[4];
    if (offset < 0)
        return;
    *texture = m_textures[0];
    *texOffset = offset;
    float ref = alphaTest ? float(m_rs[d3d::RS_ALPHAREF] & 0xFF) / 255.0f : 0.0f;
    *alphaRef = alphaBlend ? std::max(ref, 0.5f) : ref;
}

// Identity of a caster across frames: mesh structure (format, counts, indices), where it stands, cut-out texture
// and its bounds rounded to 4 units - but not the exact vertices or world matrix, so plants swaying in the wind
// (by vertices or by a tilting world matrix) are still the same caster.
uint64_t Device::CasterKey(uint32_t primitive, uint32_t fvf, uint32_t stride, const void* vertices, uint32_t vertexCount,
                           const uint16_t* indices, uint32_t indexCount) const
{
    Texture* texture;
    int texOffset;
    float alphaRef;
    ShadowCutout(fvf, &texture, &texOffset, &alphaRef);
    int32_t bounds[6] = {INT32_MAX, INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN, INT32_MIN};
    const uint8_t* v = static_cast<const uint8_t*>(vertices);
    for (uint32_t i = 0; i < vertexCount; ++i) {
        float p[3];
        std::memcpy(p, v + size_t(i) * stride, sizeof(p));
        for (int j = 0; j < 3; ++j) {
            int32_t q = int32_t(std::floor(p[j] * 0.25f));
            bounds[j] = std::min(bounds[j], q);
            bounds[3 + j] = std::max(bounds[3 + j], q);
        }
    }
    uint64_t key = 0x5EEDull ^ (uint64_t(fvf) << 32) ^ (uint64_t(primitive) << 24) ^ vertexCount;
    key = HashBytes(bounds, sizeof(bounds), key);
    if (indexCount)
        key = HashBytes(indices, size_t(indexCount) * 2, key ^ indexCount);
    // Placement: where it stands (translation, to a quarter unit), not the exact matrix - plants sway by tilting
    // their world matrix a little every frame.
    int32_t at[3] = {int32_t(std::floor(m_world.m[3][0] * 4.0f)), int32_t(std::floor(m_world.m[3][1] * 4.0f)),
                     int32_t(std::floor(m_world.m[3][2] * 4.0f))};
    key = HashBytes(at, sizeof(at), key);
    return HashBytes(&texture, sizeof(texture), key ^ uint64_t(texOffset + 1));
}

void Device::CacheCaster(uint64_t key, const ShadowCaster& c, const void* vertices, const uint16_t* indices)
{
    if (m_casterCache.size() >= kMaxCachedCasters)
        return;
    CachedCaster e{};
    e.primitive = c.primitive;
    e.stride = c.stride;
    e.vertexCount = c.vertexCount;
    e.indexCount = c.indexCount;
    e.world = c.world;
    e.texture = c.texture;
    e.texOffset = c.texOffset;
    e.alphaRef = c.alphaRef;
    e.lastSeen = m_frameNumber;
    // World-space bounds.
    for (int i = 0; i < 3; ++i) { e.boundsMin[i] = 1e30f; e.boundsMax[i] = -1e30f; }
    const uint8_t* v = static_cast<const uint8_t*>(vertices);
    for (uint32_t i = 0; i < c.vertexCount; ++i) {
        float p[3];
        std::memcpy(p, v + size_t(i) * c.stride, sizeof(p));
        for (int j = 0; j < 3; ++j) {
            float w = p[0] * e.world.m[0][j] + p[1] * e.world.m[1][j] + p[2] * e.world.m[2][j] + e.world.m[3][j];
            e.boundsMin[j] = std::min(e.boundsMin[j], w);
            e.boundsMax[j] = std::max(e.boundsMax[j], w);
        }
    }
    // Too far from the camera to matter (distant scenery): not worth keeping.
    float d2 = 0.0f;
    for (int j = 0; j < 3; ++j) {
        float d = std::max({e.boundsMin[j] - m_frameEye[j], 0.0f, m_frameEye[j] - e.boundsMax[j]});
        d2 += d * d;
    }
    if (d2 > 9.0f * m_shadowRange * m_shadowRange)
        return;
    VkDeviceSize vbBytes = VkDeviceSize(c.stride) * c.vertexCount;
    e.indexOffset = (vbBytes + 3) & ~VkDeviceSize(3);
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = e.indexOffset + VkDeviceSize(c.indexCount) * 2 + 4;
    bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    VmaAllocationCreateInfo ac{};
    ac.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    ac.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info;
    if (vmaCreateBuffer(m_allocator, &bi, &ac, &e.buffer, &e.allocation, &info) != VK_SUCCESS)
        return;
    auto* dst = static_cast<uint8_t*>(info.pMappedData);
    std::memcpy(dst, vertices, vbBytes);
    if (c.indexCount)
        std::memcpy(dst + e.indexOffset, indices, size_t(c.indexCount) * 2);
    m_casterCache.emplace(key, e);
}

void Device::ForgetCachedCaster(std::unordered_map<uint64_t, CachedCaster>::iterator it)
{
    m_deadBuffers.push_back({DeathTag(), {it->second.buffer, it->second.allocation}});
    m_casterCache.erase(it);
}

void Device::ForgetCasterTexture(Texture* texture)
{
    for (auto it = m_casterCache.begin(); it != m_casterCache.end();)
        if (it->second.texture == texture) { auto next = std::next(it); ForgetCachedCaster(it); it = next; }
        else ++it;
}

// End of frame: drop streaks that were broken, and cached casters that are gone - ones in the middle of the view
// that the game didn't draw (removed, or replaced by another level of detail) - or that are far away.
void Device::UpdateCasterCache()
{
    for (auto it = m_casterStreaks.begin(); it != m_casterStreaks.end();)
        it = it->second.lastFrame == m_frameNumber ? std::next(it) : m_casterStreaks.erase(it);
    if (!m_frameEyeValid)
        return;
    for (auto it = m_casterCache.begin(); it != m_casterCache.end();) {
        CachedCaster& e = it->second;
        auto next = std::next(it);
        if (e.lastSeen != m_frameNumber) {
            float d2 = 0.0f;
            for (int j = 0; j < 3; ++j) {
                float d = std::max({e.boundsMin[j] - m_frameEye[j], 0.0f, m_frameEye[j] - e.boundsMax[j]});
                d2 += d * d;
            }
            // The game draws whatever reaches into the view. A box test against the view is loose near the edges
            // (it reports boxes just off screen as partly visible), so only the box's centre on screen counts as
            // "the game would draw it": if it then wasn't drawn, it's gone.
            float centre[3];
            for (int j = 0; j < 3; ++j) centre[j] = 0.5f * (e.boundsMin[j] + e.boundsMax[j]);
            if (d2 > 16.0f * m_shadowRange * m_shadowRange) {
                ++m_forgottenFar;
                ForgetCachedCaster(it);
            } else if (m_frameViewProjValid && BoxInClip(centre, centre, m_frameViewProj, true) == 1) {
                ++m_forgottenInView;
                ForgetCachedCaster(it);
            }
        }
        it = next;
    }
}

// Whether a draw takes sun shadows: opaque or alpha-blended 3D geometry on the main target. Additive and
// multiplying passes (lights, lightmaps) build on a surface that was already shadowed.
bool Device::ShadowReceiver(uint32_t fvf) const
{
    if (!m_shadows || !m_shadowValid || m_target != m_main || (fvf & d3d::FVF_POSITION_MASK) != d3d::FVF_XYZ ||
        !m_rs[d3d::RS_ZENABLE])
        return false;
    if (!m_rs[d3d::RS_ALPHABLENDENABLE])
        return true;
    uint32_t src = m_rs[d3d::RS_SRCBLEND], dst = m_rs[d3d::RS_DESTBLEND];
    return (src == d3d::BLEND_SRCALPHA && dst == d3d::BLEND_INVSRCALPHA) || (src == d3d::BLEND_ONE && dst == d3d::BLEND_ZERO);
}

// Anarchy Online's round blob shadow under characters (GfxVisualSimpleShadow_c): a black, alpha-blended disc of
// numSegs segments drawn as an indexed fan (2n+1 vertices, n+2 indices) and strip (2n+2 indices) with an 8x8
// texture and no depth writes. Redundant while real shadows are drawn.
bool Device::IsBlobShadow(uint32_t primitive, uint32_t fvf, const void* vertices, uint32_t vertexCount,
                          uint32_t indexCount) const
{
    if (!m_shadows || !m_shadowValid || fvf != (d3d::FVF_XYZ | d3d::FVF_DIFFUSE | (1u << 8)) || vertexCount % 2 == 0 ||
        vertexCount < 7 || vertexCount > 129)
        return false;
    uint32_t n = (vertexCount - 1) / 2;
    if (!((primitive == d3d::TriangleFan && indexCount == n + 2) || (primitive == d3d::TriangleStrip && indexCount == 2 * n + 2)))
        return false;
    if (!m_rs[d3d::RS_ALPHABLENDENABLE] || m_rs[d3d::RS_SRCBLEND] != d3d::BLEND_SRCALPHA ||
        m_rs[d3d::RS_DESTBLEND] != d3d::BLEND_INVSRCALPHA || m_rs[d3d::RS_ZWRITEENABLE])
        return false;
    const Texture* t = m_textures[0];
    if (!t || t->Width() != 8 || t->Height() != 8)
        return false;
    const uint8_t* v = static_cast<const uint8_t*>(vertices);
    for (uint32_t i = 0; i < vertexCount; ++i) {                // XYZ, diffuse, uv: 24 bytes
        uint32_t diffuse;
        std::memcpy(&diffuse, v + i * 24 + 12, 4);
        if (diffuse & 0x00FFFFFF)
            return false;
    }
    return true;
}

// A multiplying pass over a shadow receiver - the ground's lightmap + local lights pass (blend ZERO/SRCCOLOR).
// The surface under it was already darkened by the shadow, which would darken the local lights too; such a
// pass divides its local lights by the same shadow factor so only the (baked) sunlight ends up shadowed.
bool Device::ShadowCompensated(uint32_t fvf) const
{
    if (!m_shadows || !m_shadowValid || m_target != m_main || (fvf & d3d::FVF_POSITION_MASK) != d3d::FVF_XYZ ||
        !m_rs[d3d::RS_ZENABLE] || !m_rs[d3d::RS_ALPHABLENDENABLE])
        return false;
    uint32_t src = m_rs[d3d::RS_SRCBLEND], dst = m_rs[d3d::RS_DESTBLEND];
    return (src == d3d::BLEND_ZERO && dst == d3d::BLEND_SRCCOLOR) || (src == d3d::BLEND_DESTCOLOR && dst == d3d::BLEND_ZERO);
}

// The sun for the next frame's shadows: the brightest directional light used during this frame.
void Device::CaptureSun(const d3d::Light& l)
{
    float lum = l.diffuse.r * 0.3f + l.diffuse.g * 0.59f + l.diffuse.b * 0.11f;
    if (lum <= m_sunLuminance)
        return;
    float d[3] = {l.direction.x, l.direction.y, l.direction.z};
    Normalize(d);
    if (d[0] == 0.0f && d[1] == 0.0f && d[2] == 0.0f)
        return;
    m_sunLuminance = lum;
    std::memcpy(m_sunDir, d, sizeof(d));
}

// End of frame, after the main pass: render this frame's casters from the sun into the map for the next frame.
void Device::RenderShadowMap(VkCommandBuffer cmd)
{
    bool haveSun = m_sunLuminance > 0.0f && m_sunDir[1] < -0.05f;   // below the horizon / grazing: none
    m_shadowValid = false;
    if (!m_shadows || !haveSun || !m_frameEyeValid || m_casters.empty()) {
        m_casters.clear();
        UpdateCasterCache();
        return;
    }

    // Light view: z along the sun's direction, the map centred ahead of the camera, snapped to texels.
    float z[3] = {m_sunDir[0], m_sunDir[1], m_sunDir[2]};
    float up[3] = {0.0f, 1.0f, 0.0f};
    if (std::fabs(z[1]) > 0.99f) { up[1] = 0.0f; up[2] = 1.0f; }
    float x[3], y[3];
    Cross(up, z, x);
    Normalize(x);
    Cross(z, x, y);
    float center[3];
    for (int i = 0; i < 3; ++i) center[i] = m_frameEye[i] + m_frameForward[i] * m_shadowRange * 0.4f;
    float texel = 2.0f * m_shadowRange / float(kShadowSize);
    float cx = std::round(Dot(x, center) / texel) * texel, cy = std::round(Dot(y, center) / texel) * texel;
    float cz = Dot(z, center);
    d3d::Matrix view{};
    for (int i = 0; i < 3; ++i) { view.m[i][0] = x[i]; view.m[i][1] = y[i]; view.m[i][2] = z[i]; }
    view.m[3][0] = -cx; view.m[3][1] = -cy; view.m[3][2] = -cz; view.m[3][3] = 1.0f;
    const float depth = 600.0f;                                   // light-space z range around the centre
    d3d::Matrix proj{};
    proj.m[0][0] = 1.0f / m_shadowRange;
    proj.m[1][1] = 1.0f / m_shadowRange;
    proj.m[2][2] = 1.0f / (2.0f * depth);
    proj.m[3][2] = 0.5f;
    proj.m[3][3] = 1.0f;
    d3d::Matrix lightViewProj = Mul(view, proj);

    ImageBarrier(cmd, m_shadowImage, VK_IMAGE_ASPECT_DEPTH_BIT, m_shadowImageLayout,
                 VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo depthAtt{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depthAtt.imageView = m_shadowView;
    depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAtt.clearValue.depthStencil = {1.0f, 0};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {kShadowSize, kShadowSize}};
    ri.layerCount = 1;
    ri.pDepthAttachment = &depthAtt;
    vkCmdBeginRendering(cmd, &ri);

    VkViewport viewport{0.0f, 0.0f, float(kShadowSize), float(kShadowSize), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {kShadowSize, kShadowSize}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdSetDepthBias(cmd, 2.0f, 0.0f, 2.5f);
    Frame& f = m_frames[m_frameIndex];
    VkBuffer buffers[2] = {f.ring, m_nullBuffer};
    VkDeviceSize offsets[2] = {0, 0};
    vkCmdBindVertexBuffers(cmd, 0, 2, buffers, offsets);
    vkCmdBindIndexBuffer(cmd, f.ring, 0, VK_INDEX_TYPE_UINT16);

    int boundPipeline = -1;
    uint32_t boundStride = ~0u, boundPrimitive = ~0u;
    int boundTexOffset = -2;
    Texture* boundTexture = nullptr;
    for (const ShadowCaster& c : m_casters) {
        if (c.generation != m_ringGeneration)                    // the ring restarted since: data overwritten
            continue;
        int pipeline = c.texture ? 1 : 0;
        if (pipeline != boundPipeline) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPipelines[pipeline]);
            boundPipeline = pipeline;
        }
        if (c.primitive != boundPrimitive) {
            vkCmdSetPrimitiveTopology(cmd, TopologyOf(c.primitive));
            boundPrimitive = c.primitive;
        }
        if (c.stride != boundStride || c.texOffset != boundTexOffset) {
            VkVertexInputBindingDescription2EXT bindings[2] = {
                {VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT, nullptr, 0, c.stride, VK_VERTEX_INPUT_RATE_VERTEX, 1},
                {VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT, nullptr, 1, 0, VK_VERTEX_INPUT_RATE_VERTEX, 1},
            };
            VkVertexInputAttributeDescription2EXT attrs[2] = {
                {VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT, nullptr, 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
                {VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT, nullptr, 1, 1, VK_FORMAT_R32G32_SFLOAT, 0},
            };
            if (c.texOffset >= 0) { attrs[1].binding = 0; attrs[1].offset = uint32_t(c.texOffset); }
            vkCmdSetVertexInputEXT(cmd, 2, bindings, 2, attrs);
            boundStride = c.stride;
            boundTexOffset = c.texOffset;
        }
        if (c.texture && c.texture != boundTexture) {
            VkDescriptorImageInfo image{SamplerFor(0), c.texture->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstBinding = 0;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w.pImageInfo = &image;
            vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPipelineLayout, 0, 1, &w);
            boundTexture = c.texture;
        }
        ShadowPush push;
        push.worldLightViewProj = Mul(c.world, lightViewProj);
        push.alpha[0] = c.alphaRef;
        push.alpha[1] = push.alpha[2] = push.alpha[3] = 0.0f;
        vkCmdPushConstants(cmd, m_shadowPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(push), &push);
        if (c.indexCount)
            vkCmdDrawIndexed(cmd, c.indexCount, 1, uint32_t(c.ibOffset / 2), int32_t(c.vbOffset / c.stride), 0);
        else
            vkCmdDraw(cmd, c.vertexCount, 1, uint32_t(c.vbOffset / c.stride), 0);
    }
    // Remembered static casters the game didn't draw this frame (out of view), if they reach into the map.
    m_cachedCastersDrawn = 0;
    for (auto& [key, e] : m_casterCache) {
        if (e.lastSeen == m_frameNumber || BoxInClip(e.boundsMin, e.boundsMax, lightViewProj, false) == -1)
            continue;
        ++m_cachedCastersDrawn;
        int pipeline = e.texture ? 1 : 0;
        if (pipeline != boundPipeline) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPipelines[pipeline]);
            boundPipeline = pipeline;
        }
        if (e.primitive != boundPrimitive) {
            vkCmdSetPrimitiveTopology(cmd, TopologyOf(e.primitive));
            boundPrimitive = e.primitive;
        }
        if (e.stride != boundStride || e.texOffset != boundTexOffset) {
            VkVertexInputBindingDescription2EXT bindings[2] = {
                {VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT, nullptr, 0, e.stride, VK_VERTEX_INPUT_RATE_VERTEX, 1},
                {VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT, nullptr, 1, 0, VK_VERTEX_INPUT_RATE_VERTEX, 1},
            };
            VkVertexInputAttributeDescription2EXT attrs[2] = {
                {VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT, nullptr, 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
                {VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT, nullptr, 1, 1, VK_FORMAT_R32G32_SFLOAT, 0},
            };
            if (e.texOffset >= 0) { attrs[1].binding = 0; attrs[1].offset = uint32_t(e.texOffset); }
            vkCmdSetVertexInputEXT(cmd, 2, bindings, 2, attrs);
            boundStride = e.stride;
            boundTexOffset = e.texOffset;
        }
        if (e.texture && e.texture != boundTexture) {
            VkDescriptorImageInfo image{SamplerFor(0), e.texture->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstBinding = 0;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w.pImageInfo = &image;
            vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPipelineLayout, 0, 1, &w);
            boundTexture = e.texture;
        }
        VkDeviceSize zero = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &e.buffer, &zero);
        ShadowPush push;
        push.worldLightViewProj = Mul(e.world, lightViewProj);
        push.alpha[0] = e.alphaRef;
        push.alpha[1] = push.alpha[2] = push.alpha[3] = 0.0f;
        vkCmdPushConstants(cmd, m_shadowPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(push), &push);
        if (e.indexCount) {
            vkCmdBindIndexBuffer(cmd, e.buffer, e.indexOffset, VK_INDEX_TYPE_UINT16);
            vkCmdDrawIndexed(cmd, e.indexCount, 1, 0, 0, 0);
        } else {
            vkCmdDraw(cmd, e.vertexCount, 1, 0, 0);
        }
    }
    vkCmdEndRendering(cmd);
    ImageBarrier(cmd, m_shadowImage, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    m_shadowImageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    m_shadowViewProj = lightViewProj;
    std::memcpy(m_shadowSunDir, m_sunDir, sizeof(m_sunDir));
    m_shadowValid = true;
    m_casters.clear();
    UpdateCasterCache();
}

}  // namespace rvk
