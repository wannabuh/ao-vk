// Sun shadows (an enhancement; D3D7 has none). The frame's opaque 3D draws are recorded as shadow casters while
// they are drawn (their geometry is already in the frame's ring buffer); at the end of the frame they are drawn
// again from the sun into a depth map, which the next frame samples. Shadows therefore lag one frame.
//
// The sun is the brightest directional light the game used during the frame. The map is a set of cascades: squares
// around the camera, each three times as wide as the one before (the nearest sharpest, the last reaching the shadow
// distance), oriented with the sun and snapped to whole texels so they don't shimmer while the camera moves.
#include "internal.h"

#include <algorithm>
#include <chrono>
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
    float sway[4];                       // plants (shadow.vert, sway.glsl)
    float windModel[4];                  // the wind in model space
    float origin[4];                     // world x, z of the object; wind time
};
static_assert(sizeof(ShadowPush) <= 128, "push constant range");

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

}  // namespace

int detail::BoxInClip(const float mn[3], const float mx[3], const d3d::Matrix& vp, bool depth)
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

namespace {

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

double Device::SwayClock()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// The pose of a remembered caster while out of view: its recorded sway played back and forth, starting from the
// latest sample (backwards first), interpolated between samples. Without a recording: the last pose seen.
d3d::Matrix Device::SwayPose(const CachedCaster& e) const
{
    size_t n = e.sway.size();
    if (n < 8)
        return e.world;
    double span = double(n - 1);
    double p = std::fmod((SwayClock() - e.lastSeenTime) / kSwayStep, 2.0 * span);
    double pos = p < span ? span - p : p - span;      // n-1 ... 0 ... n-1
    size_t i = std::min(size_t(pos), n - 2);
    float f = float(pos - double(i));
    d3d::Matrix r;
    for (int a = 0; a < 4; ++a)
        for (int b = 0; b < 4; ++b)
            r.m[a][b] = e.sway[i].m[a][b] * (1.0f - f) + e.sway[i + 1].m[a][b] * f;
    return r;
}

// General 4x4 inverse (cofactors); false if singular.
bool detail::InvertMatrix(const d3d::Matrix& m, d3d::Matrix* out)
{
    const float* a = &m.m[0][0];
    float inv[16];
    inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
    inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
    inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
    inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
    inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
    inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
    inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
    inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
    inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
    inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
    inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
    inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
    inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
    inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
    inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
    inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
    float det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
    if (std::fabs(det) < 1e-20f)
        return false;
    float* o = &out->m[0][0];
    for (int i = 0; i < 16; ++i) o[i] = inv[i] / det;
    return true;
}

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
    ci.arrayLayers = kShadowCascades;
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
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vi.format = kDepthFormat;
    vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, kShadowCascades};
    if (!Check(vkCreateImageView(m_device, &vi, nullptr, &m_shadowView), "shadow map view", error))
        return false;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    for (uint32_t i = 0; i < kShadowCascades; ++i) {
        vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, i, 1};
        if (!Check(vkCreateImageView(m_device, &vi, nullptr, &m_shadowLayerViews[i]), "shadow cascade view", error))
            return false;
    }
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
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;              // depths as stored (soft shadows' blocker search)
    si.compareEnable = VK_FALSE;
    if (!Check(vkCreateSampler(m_device, &si, nullptr, &m_shadowDepthSampler), "shadow depth sampler", error))
        return false;

    // Pass pipelines: depth only (opaque casters) and with an alpha-testing fragment shader.
    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                         VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
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
    if (m_shadowDepthSampler) vkDestroySampler(m_device, m_shadowDepthSampler, nullptr);
    m_shadowDepthSampler = VK_NULL_HANDLE;
    if (m_shadowView) vkDestroyImageView(m_device, m_shadowView, nullptr);
    for (VkImageView& v : m_shadowLayerViews)
        if (v) { vkDestroyImageView(m_device, v, nullptr); v = VK_NULL_HANDLE; }
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
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, kShadowCascades};
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
// Name labels over characters: unlit, alpha-blended quads (4 vertices, position + colour + one texture set)
// with a wide text texture - drawn with depth writes, but neither casting nor taking shadows.
bool Device::IsLabel(uint32_t primitive, uint32_t fvf, uint32_t vertexCount) const
{
    const Texture* t = m_textures[0];
    return fvf == (d3d::FVF_XYZ | d3d::FVF_DIFFUSE | (1u << 8)) && vertexCount == 4 && TopologyClassOf(primitive) == 2 &&
           !m_rs[d3d::RS_LIGHTING] && m_rs[d3d::RS_ALPHABLENDENABLE] && t && t->Width() >= 2 * t->Height();
}

bool Device::IsShadowCaster(uint32_t primitive, uint32_t fvf) const
{
    if (m_drawIsLabel)
        return false;
    if (!(m_shadows || m_pointShadows) || m_target != m_main || TopologyClassOf(primitive) != 2 ||
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
    uint32_t view = 0;
    while (view < m_casterViews.size() && (std::memcmp(&m_casterViews[view].view, &m_view, sizeof(m_view)) != 0 ||
                                           std::memcmp(&m_casterViews[view].proj, &m_proj, sizeof(m_proj)) != 0))
        ++view;
    if (view == m_casterViews.size()) {
        if (view == 8)
            return;                              // too many cameras; not the world
        m_casterViews.push_back({m_view, m_proj, 0});
    }
    ++m_casterViews[view].count;
    ShadowCaster c;
    c.view = view;
    c.primitive = primitive;
    c.stride = stride;
    c.vertexCount = vertexCount;
    c.indexCount = indexCount;
    c.vbOffset = vbOffset;
    c.ibOffset = ibOffset;
    c.world = m_world;
    c.generation = m_ringGeneration;
    ShadowCutout(fvf, &c.texture, &c.texOffset, &c.alphaRef);
    uint64_t key = CasterKey(primitive, fvf, stride, vertices, vertexCount, indices, indexCount, c.boundsMin, c.boundsMax);
    c.key = key;
    std::memcpy(c.sway, m_drawSway, sizeof(c.sway));
    c.draw = m_frameDraw;
    m_casters.push_back(c);
    auto cached = m_casterCache.find(key);
    if (cached != m_casterCache.end()) {
        CachedCaster& e = cached->second;
        double now = SwayClock();
        e.lastSeen = m_frameNumber;
        e.lastSeenTime = now;
        if (std::memcmp(&e.world, &m_world, sizeof(m_world)) != 0) {
            e.world = m_world;                   // the latest pose: where an out-of-view playback starts
            if (e.sway.size() < kSwaySamples && now - e.lastSwaySample >= kSwayStep) {
                e.sway.push_back(m_world);
                e.lastSwaySample = now;
            }
        }
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
                           const uint16_t* indices, uint32_t indexCount, float boundsMin[3], float boundsMax[3]) const
{
    Texture* texture;
    int texOffset;
    float alphaRef;
    ShadowCutout(fvf, &texture, &texOffset, &alphaRef);
    for (int j = 0; j < 3; ++j) { boundsMin[j] = 1e30f; boundsMax[j] = -1e30f; }
    const uint8_t* v = static_cast<const uint8_t*>(vertices);
    for (uint32_t i = 0; i < vertexCount; ++i) {
        float p[3];
        std::memcpy(p, v + size_t(i) * stride, sizeof(p));
        for (int j = 0; j < 3; ++j) {
            boundsMin[j] = std::min(boundsMin[j], p[j]);
            boundsMax[j] = std::max(boundsMax[j], p[j]);
        }
    }
    int32_t bounds[6];
    for (int j = 0; j < 3; ++j) {
        bounds[j] = vertexCount ? int32_t(std::floor(boundsMin[j] * 0.25f)) : INT32_MAX;
        bounds[3 + j] = vertexCount ? int32_t(std::floor(boundsMax[j] * 0.25f)) : INT32_MIN;
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
    std::memcpy(e.plantSway, c.sway, sizeof(e.plantSway));
    e.lastSeen = m_frameNumber;
    e.lastSeenTime = SwayClock();
    e.lastSwaySample = -1.0;
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
    if (d2 > 9.0f * kCasterCacheRange * kCasterCacheRange)
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
            if (d2 > 16.0f * kCasterCacheRange * kCasterCacheRange) {
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
// Whether the current draw uses the world camera's projection (3D interface previews use their own).
bool Device::WorldCamera() const
{
    return std::memcmp(&m_proj, &m_shadowWorldProj, sizeof(m_proj)) == 0;
}

bool Device::ShadowReceiver(uint32_t fvf) const
{
    if (m_drawIsLabel || !WorldCamera() || !m_shadows || !m_shadowValid || m_target != m_main || (fvf & d3d::FVF_POSITION_MASK) != d3d::FVF_XYZ ||
        !m_rs[d3d::RS_ZENABLE])
        return false;
    if (!m_rs[d3d::RS_ALPHABLENDENABLE])
        // The ground's base pass, where a lightmap + lights pass follows (it did last frame): that pass takes the
        // shadow instead (ShadowInLightmap), so local lights can fill shadows in.
        return !(IsTerrain(fvf) && m_terrainLitPassPrev);
    uint32_t src = m_rs[d3d::RS_SRCBLEND], dst = m_rs[d3d::RS_DESTBLEND];
    return (src == d3d::BLEND_SRCALPHA && dst == d3d::BLEND_INVSRCALPHA) || (src == d3d::BLEND_ONE && dst == d3d::BLEND_ZERO);
}

bool Device::IsMultiplyPass() const
{
    uint32_t src = m_rs[d3d::RS_SRCBLEND], dst = m_rs[d3d::RS_DESTBLEND];
    return m_rs[d3d::RS_ALPHABLENDENABLE] &&
           ((src == d3d::BLEND_ZERO && dst == d3d::BLEND_SRCCOLOR) || (src == d3d::BLEND_DESTCOLOR && dst == d3d::BLEND_ZERO));
}

// The ground's lightmap + lights pass (multiplying the base pass): the shadow darkens its lightmap - the baked
// sunlight - before the lights are added and the sum is clamped, so a light (the player's) fills shadows in:
// ground = base * min(1, lightmap * shadow + ambient + lights).
bool Device::ShadowInLightmap(uint32_t fvf) const
{
    return !m_drawIsLabel && WorldCamera() && m_shadows && m_shadowValid && m_target == m_main && m_rs[d3d::RS_ZENABLE] &&
           IsTerrain(fvf) && IsMultiplyPass();
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
    return !m_drawIsLabel && WorldCamera() && m_shadows && m_shadowValid && m_target == m_main &&
           (fvf & d3d::FVF_POSITION_MASK) == d3d::FVF_XYZ && m_rs[d3d::RS_ZENABLE] && IsMultiplyPass();
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
    m_sunColor[0] = l.diffuse.r; m_sunColor[1] = l.diffuse.g; m_sunColor[2] = l.diffuse.b;
}

// A model-space box through a matrix: the world-space box around it.
static void TransformBounds(const float mn[3], const float mx[3], const d3d::Matrix& m, float outMin[3], float outMax[3])
{
    for (int j = 0; j < 3; ++j) {
        float c = m.m[3][j], e = 0.0f;
        for (int i = 0; i < 3; ++i) {
            float centre = 0.5f * (mn[i] + mx[i]), half = 0.5f * (mx[i] - mn[i]);
            c += centre * m.m[i][j];
            e += half * std::fabs(m.m[i][j]);
        }
        outMin[j] = c - e;
        outMax[j] = c + e;
    }
}

// End of frame: what the shadow passes draw - this frame's casters (the world camera's, still in the ring) and the
// remembered static casters the game didn't draw this frame (out of view, or overwritten by a mid-frame flush).
void Device::CollectShadowItems()
{
    m_shadowItems.clear();
    uint32_t world = 0;
    for (uint32_t i = 1; i < m_casterViews.size(); ++i)
        if (m_casterViews[i].count > m_casterViews[world].count) world = i;
    std::vector<uint64_t> staleKeys;
    std::vector<uint32_t> drawOf;                // per item: its draw number
    for (const ShadowCaster& c : m_casters) {
        if (c.view != world)
            continue;
        if (c.generation != m_ringGeneration) {  // the ring restarted since (mid-frame flush): data overwritten
            staleKeys.push_back(c.key);          // drawn from the caster cache instead, if remembered
            continue;
        }
        ShadowItem it;
        it.buffer = VK_NULL_HANDLE;
        it.vbOffset = c.vbOffset;
        it.ibOffset = c.ibOffset;
        it.primitive = c.primitive;
        it.stride = c.stride;
        it.vertexCount = c.vertexCount;
        it.indexCount = c.indexCount;
        it.texture = c.texture;
        it.texOffset = c.texOffset;
        it.alphaRef = c.alphaRef;
        it.world = c.world;
        TransformBounds(c.boundsMin, c.boundsMax, c.world, it.boundsMin, it.boundsMax);
        it.cached = false;
        it.group = 0;
        std::memcpy(it.sway, c.sway, sizeof(it.sway));
        m_shadowItems.push_back(it);
        drawOf.push_back(c.draw);
    }
    GroupShadowItems(drawOf);
    for (auto& [key, e] : m_casterCache) {
        if (e.lastSeen == m_frameNumber && std::find(staleKeys.begin(), staleKeys.end(), key) == staleKeys.end())
            continue;                            // drawn this frame: in the list above
        ShadowItem it;
        it.buffer = e.buffer;
        it.vbOffset = 0;
        it.ibOffset = e.indexOffset;
        it.primitive = e.primitive;
        it.stride = e.stride;
        it.vertexCount = e.vertexCount;
        it.indexCount = e.indexCount;
        it.texture = e.texture;
        it.texOffset = e.texOffset;
        it.alphaRef = e.alphaRef;
        it.world = SwayPose(e);
        std::memcpy(it.boundsMin, e.boundsMin, sizeof(it.boundsMin));
        std::memcpy(it.boundsMax, e.boundsMax, sizeof(it.boundsMax));
        it.cached = true;
        it.group = ~0u;
        std::memcpy(it.sway, e.plantSway, sizeof(it.sway));
        m_shadowItems.push_back(it);
    }
}

// The game draws each character's parts (body pieces, head, held weapon) one after another, so a run of casters
// close together is one object. In the dumps a character's parts lie within 0.7 sideways of each other and another
// character starts a new run. Other draws may come between the parts (effects on a weapon, particles), so only a
// long stretch without casters ends a run. Characters drawn back to back stay apart unless their origins are
// within 1 of each other: only when one walks right through the other.
constexpr uint32_t kGroupMaxGap = 32;
void Device::GroupShadowItems(const std::vector<uint32_t>& drawOf)
{
    uint32_t group = 0;
    size_t first = 0;
    for (size_t i = 0; i < drawOf.size(); ++i) {
        if (i > 0) {
            const auto& a = m_shadowItems[first].world.m[3];
            const auto& b = m_shadowItems[i].world.m[3];
            float dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
            if (drawOf[i] - drawOf[i - 1] > kGroupMaxGap || dx * dx + dz * dz > 1.0f || std::fabs(dy) > 2.6f) {
                ++group;
                first = i;
            }
        }
        m_shadowItems[i].group = group;
    }
}

// Draws one caster into the shadow map being rendered (the shadow pipelines, depth only).
void Device::DrawShadowItem(VkCommandBuffer cmd, ShadowBind& bind, const ShadowItem& item, const d3d::Matrix& lightViewProj)
{
    int pipeline = item.texture ? 1 : 0;
    if (pipeline != bind.pipeline) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPipelines[pipeline]);
        bind.pipeline = pipeline;
    }
    if (item.primitive != bind.primitive) {
        vkCmdSetPrimitiveTopology(cmd, TopologyOf(item.primitive));
        bind.primitive = item.primitive;
    }
    if (item.stride != bind.stride || item.texOffset != bind.texOffset) {
        VkVertexInputBindingDescription2EXT bindings[2] = {
            {VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT, nullptr, 0, item.stride, VK_VERTEX_INPUT_RATE_VERTEX, 1},
            {VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT, nullptr, 1, 0, VK_VERTEX_INPUT_RATE_VERTEX, 1},
        };
        VkVertexInputAttributeDescription2EXT attrs[2] = {
            {VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT, nullptr, 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
            {VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT, nullptr, 1, 1, VK_FORMAT_R32G32_SFLOAT, 0},
        };
        if (item.texOffset >= 0) { attrs[1].binding = 0; attrs[1].offset = uint32_t(item.texOffset); }
        vkCmdSetVertexInputEXT(cmd, 2, bindings, 2, attrs);
        bind.stride = item.stride;
        bind.texOffset = item.texOffset;
    }
    // Every caster binds a texture: the vertex shader may read it (sway.glsl); casters without one get a stand-in.
    Texture* texture = item.texture ? item.texture : m_blackTexture;
    if (texture != bind.texture) {
        VkDescriptorImageInfo image{SamplerFor(0), texture->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &image;
        vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPipelineLayout, 0, 1, &w);
        bind.texture = texture;
    }
    // Ring casters address their data inside the ring bound at offset 0; cached ones have their own buffer.
    VkBuffer vb = item.buffer ? item.buffer : m_frames[m_frameIndex].ring;
    if (vb != bind.vb) {
        VkBuffer buffers[2] = {vb, m_nullBuffer};
        VkDeviceSize offsets[2] = {0, 0};
        vkCmdBindVertexBuffers(cmd, 0, 2, buffers, offsets);
        bind.vb = vb;
    }
    ShadowPush push;
    push.worldLightViewProj = Mul(item.world, lightViewProj);
    push.alpha[0] = item.alphaRef;
    push.alpha[1] = push.alpha[2] = push.alpha[3] = 0.0f;
    // A swaying plant (only with its texture bound - sway.glsl reads it): the wind in model space, so the combined
    // matrix can stay; world displacement d = m * W (3x3), so m = d * inverse(W).
    std::memset(push.sway, 0, sizeof(push.sway) + sizeof(push.windModel) + sizeof(push.origin));
    d3d::Matrix inverse;
    if (item.sway[3] > 0.5f && item.texture && m_sway > 0.0f && InvertMatrix(item.world, &inverse)) {
        float wind[4];
        Wind(wind);
        std::memcpy(push.sway, item.sway, sizeof(push.sway));
        for (int j = 0; j < 3; ++j) push.windModel[j] = wind[0] * inverse.m[0][j] + wind[1] * inverse.m[2][j];
        push.origin[0] = item.world.m[3][0];
        push.origin[1] = item.world.m[3][2];
        push.origin[2] = wind[2];
    }
    vkCmdPushConstants(cmd, m_shadowPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(push), &push);
    if (item.indexCount) {
        VkDeviceSize ibOffset = item.buffer ? item.ibOffset : 0;
        if (vb != bind.ib || ibOffset != bind.ibOffset) {
            vkCmdBindIndexBuffer(cmd, vb, ibOffset, VK_INDEX_TYPE_UINT16);
            bind.ib = vb;
            bind.ibOffset = ibOffset;
        }
        if (item.buffer)
            vkCmdDrawIndexed(cmd, item.indexCount, 1, 0, 0, 0);
        else
            vkCmdDrawIndexed(cmd, item.indexCount, 1, uint32_t(item.ibOffset / 2), int32_t(item.vbOffset / item.stride), 0);
    } else {
        vkCmdDraw(cmd, item.vertexCount, 1, item.buffer ? 0 : uint32_t(item.vbOffset / item.stride), 0);
    }
}

// End of frame, after the main pass: render this frame's casters from the sun into the map for the next frame.
void Device::RenderShadowMap(VkCommandBuffer cmd)
{
    bool haveSun = m_sunLuminance > 0.0f && m_sunDir[1] < -0.05f;   // below the horizon / grazing: none
    m_shadowValid = false;
    // The sun for effects that don't need the shadow map (light through leaves, contact shadows).
    if (haveSun) std::memcpy(m_frameSunDir, m_sunDir, sizeof(m_sunDir));
    for (int i = 0; i < 3; ++i) m_frameSunColor[i] = haveSun ? m_sunColor[i] : 0.0f;
    m_frameLightsDirty = true;
    // The world camera: the one most casters were drawn with.
    uint32_t world = 0;
    for (uint32_t i = 1; i < m_casterViews.size(); ++i)
        if (m_casterViews[i].count > m_casterViews[world].count) world = i;
    float eye[3], forward[3];
    m_frameViewProjValid = !m_casterViews.empty();
    if (m_frameViewProjValid) {
        const auto& v = m_casterViews[world].view.m;
        for (int i = 0; i < 3; ++i) {
            eye[i] = -(v[3][0] * v[i][0] + v[3][1] * v[i][1] + v[3][2] * v[i][2]);
            forward[i] = v[i][2];
        }
        m_frameViewProj = MulMatrix(m_casterViews[world].view, m_casterViews[world].proj);
        m_shadowWorldProj = m_casterViews[world].proj;
        std::memcpy(m_frameEye, eye, sizeof(eye));             // also what caster cache eviction measures from
    }
    if (!m_frameViewProjValid || m_casters.empty()) {
        m_shadowItems.clear();
        return;
    }
    CollectShadowItems();
    if (!m_shadows || !haveSun)
        return;

    // Light view: z along the sun's direction; each cascade centred ahead of the camera, snapped to its texels.
    float z[3] = {m_sunDir[0], m_sunDir[1], m_sunDir[2]};
    float up[3] = {0.0f, 1.0f, 0.0f};
    if (std::fabs(z[1]) > 0.99f) { up[1] = 0.0f; up[2] = 1.0f; }
    float x[3], y[3];
    Cross(up, z, x);
    Normalize(x);
    Cross(z, x, y);
    bool sunMoved = Dot(z, m_shadowSunDir) < 0.99999f;
    uint32_t count = m_shadowCascades;
    if (count != m_cascadeCount)                 // cascades changed: draw them all this frame
        sunMoved = true;

    ImageBarrier(cmd, m_shadowImage, VK_IMAGE_ASPECT_DEPTH_BIT, m_shadowImageLayout,
                 VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    VkViewport viewport{0.0f, 0.0f, float(kShadowSize), float(kShadowSize), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {kShadowSize, kShadowSize}};
    m_cachedCastersDrawn = 0;
    ++m_cascadeFrame;
    for (uint32_t c = 0; c < count; ++c) {
        // The two widest cascades take turns (their shadows are far away, a frame's lag doesn't show).
        bool alternate = count >= 3 && c >= count - 2;
        if (alternate && !sunMoved && ((m_cascadeFrame & 1) != ((count - 1 - c) & 1)))
            continue;
        float range = m_shadowRange / std::pow(3.0f, float(count - 1 - c));   // half the cascade's width
        float center[3];
        for (int i = 0; i < 3; ++i) center[i] = eye[i] + forward[i] * range * 0.4f;
        float texel = 2.0f * range / float(kShadowSize);
        float cx = std::round(Dot(x, center) / texel) * texel, cy = std::round(Dot(y, center) / texel) * texel;
        float cz = Dot(z, center);
        d3d::Matrix view{};
        for (int i = 0; i < 3; ++i) { view.m[i][0] = x[i]; view.m[i][1] = y[i]; view.m[i][2] = z[i]; }
        view.m[3][0] = -cx; view.m[3][1] = -cy; view.m[3][2] = -cz; view.m[3][3] = 1.0f;
        const float depth = std::max(600.0f, 2.0f * range);      // light-space z range around the centre
        d3d::Matrix proj{};
        proj.m[0][0] = 1.0f / range;
        proj.m[1][1] = 1.0f / range;
        proj.m[2][2] = 1.0f / (2.0f * depth);
        proj.m[3][2] = 0.5f;
        proj.m[3][3] = 1.0f;
        d3d::Matrix lightViewProj = Mul(view, proj);

        VkRenderingAttachmentInfo depthAtt{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depthAtt.imageView = m_shadowLayerViews[c];
        depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depthAtt.clearValue.depthStencil = {1.0f, 0};
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {kShadowSize, kShadowSize}};
        ri.layerCount = 1;
        ri.pDepthAttachment = &depthAtt;
        vkCmdBeginRendering(cmd, &ri);
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        // Bias in depth units grows with the cascade's depth range; slope bias per texel stays the same.
        vkCmdSetDepthBias(cmd, 2.0f * 600.0f / depth, 0.0f, 2.5f);
        ShadowBind bind;
        for (const ShadowItem& item : m_shadowItems) {
            // Only casters reaching into this cascade's square (the widest takes every one the game drew).
            bool last = c == count - 1;
            if ((item.cached || !last) && BoxInClip(item.boundsMin, item.boundsMax, lightViewProj, false) == -1)
                continue;
            if (item.cached) ++m_cachedCastersDrawn;
            DrawShadowItem(cmd, bind, item, lightViewProj);
        }
        vkCmdEndRendering(cmd);
        m_cascadeViewProj[c] = lightViewProj;
        m_cascadeTexel[c] = texel;
        m_cascadeDepth[c] = 2.0f * depth;
    }
    ImageBarrier(cmd, m_shadowImage, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    m_shadowImageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    m_cascadeCount = count;
    std::memcpy(m_shadowSunDir, m_sunDir, sizeof(m_sunDir));
    std::memcpy(m_shadowSunColor, m_sunColor, sizeof(m_sunColor));
    m_shadowValid = true;
}

// After all shadow passes of the frame.
void Device::FinishShadowFrame()
{
    // Daylight for point shadows: the sun's brightness on flat ground, smoothed so a frame without it doesn't pop.
    float daylight = m_sunLuminance * std::max(0.0f, -m_sunDir[1]);
    m_daylight += (daylight - m_daylight) * 0.1f;
    m_casters.clear();
    m_shadowItems.clear();
    UpdateCasterCache();
    if ((m_frameNumber & 63) == 0)               // plants' static check: forget meshes not drawn lately
        for (auto it = m_swayStatic.begin(); it != m_swayStatic.end();)
            it = it->second.frame + 2 < m_frameNumber ? m_swayStatic.erase(it) : std::next(it);
}

}  // namespace rvk
