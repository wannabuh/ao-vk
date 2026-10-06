// Procedural ground grass (RVK_GrassOn): a camera-centred field of blades over the terrain, an addition of our own on
// top of the game's dated foliage cards. The ground heights are sampled from the game's own terrain draws
// (CaptureTerrain) into a world-space grid; the blades are generated on the CPU each frame and drawn into the still-open
// scene rendering at the end of the scene (RenderGrassField, called from Device::EndScene), before the post passes, so
// they light and depth-test with the scene. Off by default: with RVK_GrassOn off nothing is captured and nothing is
// drawn, and the renderer is exactly as without this file.
#include "internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>
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

// One blade vertex: world position, normal, and how far up the blade it is (0 root, 1 tip).
struct GrassVertex {
    float pos[3];
    float normal[3];
    float shade;
};
static_assert(sizeof(GrassVertex) == 28, "grass vertex");

// The pass's frame block (grass.vert / grass.frag GrassFrame): the current and previous camera, one uniform buffer.
struct GrassFrame {
    d3d::Matrix viewProj;
    d3d::Matrix prevViewProj;
    float viewport[4];
};
static_assert(sizeof(GrassFrame) == 144, "grass frame block");



// A cheap, position-stable hash so a blade stays put frame to frame (needed by the temporal anti-aliasing): a cell's
// blades depend on the cell, not on the frame number.
uint32_t HashCell(int32_t x, int32_t z)
{
    uint64_t h = uint64_t(uint32_t(x)) * 0x9E3779B97F4A7C15ull ^ uint64_t(uint32_t(z)) * 0xC2B2AE3D27D4EB4Full;
    h ^= h >> 29;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 32;
    return uint32_t(h);
}

float Unit(uint32_t h) { return float(h & 0xFFFFFFu) / float(0x1000000u); }

// Smooth 1 -> 0 over the outer third of the field, so the blades shrink away instead of stopping at a hard circle.
float EdgeFade(float distance, float radius)
{
    if (radius <= 0.0f)
        return 0.0f;
    float t = (radius - distance) / (radius * 0.35f);
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

void SetVertex(GrassVertex& v, float x, float y, float z, const float n[3], float shade)
{
    v.pos[0] = x; v.pos[1] = y; v.pos[2] = z;
    v.normal[0] = n[0]; v.normal[1] = n[1]; v.normal[2] = n[2];
    v.shade = shade;
}

}  // namespace

// Whether the texture's colour at (u, v) is green enough to be grass: green dominant by ~12% and not nearly black.
// Samples the small grid taken at upload (resources.cpp PixelsThumbnail), so a tile atlas holding grass, sand and
// brick classifies each one; a texture that couldn't be sampled (an unsupported format) is never grass.
bool Texture::GrassTexel(float u, float v) const
{
    if (!m_thumbValid)
        return false;
    const float fu = u - std::floor(u), fv = v - std::floor(v);   // wrap
    const uint32_t i = uint32_t(std::clamp(int(fu * float(kThumb)), 0, int(kThumb) - 1));
    const uint32_t j = uint32_t(std::clamp(int(fv * float(kThumb)), 0, int(kThumb) - 1));
    const uint8_t* c = m_thumb + (size_t(j) * kThumb + i) * 3;
    return c[1] > 24 && int(c[1]) * 100 > int(c[0]) * 112 && int(c[1]) * 100 > int(c[2]) * 112;
}

// Records the terrain's ground into a world grid (m_groundHeights), for the grass blades to sit on, and whether the
// ground there is grass. Called for the terrain's base pass only (IsTerrain): its stage-0 texture is the ground's own,
// while the lighting pass multiplies a lightmap. Each covered cell is filled from the triangle that contains its
// centre - the height and the texture coordinate are interpolated - so the grass/not boundary follows the texture, not
// the coarser terrain vertices (a vertex sampled alone would spill grass half a cell past the edge). Only cells within
// reach of the camera are kept. With RVK_GrassTex off, every terrain cell counts as grass.
void Device::CaptureTerrain(uint32_t primitive, const FvfLayout& layout, const void* vertices, uint32_t vertexCount,
                            const uint16_t* indices, uint32_t indexCount)
{
    if (!m_grassOn || !vertices || !m_frameEyeValid || layout.offset[0] < 0 || layout.offset[4] < 0)
        return;
    if (m_rs[d3d::RS_LIGHTING] || m_rs[d3d::RS_ALPHABLENDENABLE])
        return;                                  // the ground's base pass
    if (primitive < d3d::TriangleList || primitive > d3d::TriangleFan)
        return;
    const Texture* tex = m_textures[0];
    const bool filter = m_grassTex && tex;
    const uint8_t* src = static_cast<const uint8_t*>(vertices);
    const size_t stride = layout.stride, posOff = size_t(layout.offset[0]), uvOff = size_t(layout.offset[4]);
    auto vertexIn = [&](uint32_t i, float p[3], float uv[2]) {
        const uint8_t* v = src + size_t(i) * stride;
        std::memcpy(p, v + posOff, 12);
        std::memcpy(uv, v + uvOff, 8);
    };
    const float reach = m_grassDistance + 24.0f, reach2 = reach * reach;
    const uint32_t count = indices ? indexCount : vertexCount;
    auto at = [&](uint32_t i) -> uint32_t { return indices ? indices[i] : i; };
    auto triangle = [&](uint32_t a, uint32_t b, uint32_t c) {
        if (a >= vertexCount || b >= vertexCount || c >= vertexCount)
            return;
        float pa[3], pb[3], pc[3], ua[2], ub[2], uc[2];
        vertexIn(a, pa, ua);
        vertexIn(b, pb, ub);
        vertexIn(c, pc, uc);
        const float d = (pb[2] - pc[2]) * (pa[0] - pc[0]) + (pc[0] - pb[0]) * (pa[2] - pc[2]);
        if (std::fabs(d) < 1e-6f)
            return;                              // degenerate in x, z (nothing to cover)
        const float minX = std::min({pa[0], pb[0], pc[0]}), maxX = std::max({pa[0], pb[0], pc[0]});
        const float minZ = std::min({pa[2], pb[2], pc[2]}), maxZ = std::max({pa[2], pb[2], pc[2]});
        if (maxX < m_frameEye[0] - reach || minX > m_frameEye[0] + reach ||
            maxZ < m_frameEye[2] - reach || minZ > m_frameEye[2] + reach)
            return;
        const int32_t cx0 = int32_t(std::floor(minX / kGroundCell)), cx1 = int32_t(std::floor(maxX / kGroundCell));
        const int32_t cz0 = int32_t(std::floor(minZ / kGroundCell)), cz1 = int32_t(std::floor(maxZ / kGroundCell));
        for (int32_t cz = cz0; cz <= cz1; ++cz)
            for (int32_t cx = cx0; cx <= cx1; ++cx) {
                const float qx = (float(cx) + 0.5f) * kGroundCell, qz = (float(cz) + 0.5f) * kGroundCell;
                const float dx = qx - m_frameEye[0], dz = qz - m_frameEye[2];
                if (dx * dx + dz * dz > reach2)
                    continue;
                const float l0 = ((pb[2] - pc[2]) * (qx - pc[0]) + (pc[0] - pb[0]) * (qz - pc[2])) / d;
                const float l1 = ((pc[2] - pa[2]) * (qx - pc[0]) + (pa[0] - pc[0]) * (qz - pc[2])) / d;
                const float l2 = 1.0f - l0 - l1;
                if (l0 < -0.001f || l1 < -0.001f || l2 < -0.001f)
                    continue;
                GroundCell& cell = m_groundHeights[(uint64_t(uint32_t(cx)) << 32) | uint32_t(cz)];
                cell.y = l0 * pa[1] + l1 * pb[1] + l2 * pc[1];
                if (filter) {
                    const float u = l0 * ua[0] + l1 * ub[0] + l2 * uc[0];
                    const float v = l0 * ua[1] + l1 * ub[1] + l2 * uc[1];
                    cell.grass = tex->GrassTexel(u, v);
                } else {
                    cell.grass = true;
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
    if (m_groundHeights.size() > 8000000)
        m_groundHeights.clear();                 // a guard only; the reach test above keeps it near the camera
}

// The ground height under a world x, z, if grass grows there. Searches the neighbouring cells too (a stray gap at the
// captured region's edge); the nearest ground found decides (a non-grass ground there means no grass).
bool Device::GroundHeight(float x, float z, float* y) const
{
    int32_t cx = int32_t(std::floor(x / kGroundCell));
    int32_t cz = int32_t(std::floor(z / kGroundCell));
    for (int r = 0; r <= 2; ++r) {
        for (int dz = -r; dz <= r; ++dz)
            for (int dx = -r; dx <= r; ++dx) {
                if (r > 0 && std::abs(dx) != r && std::abs(dz) != r)
                    continue;                    // only the ring at this radius
                uint64_t key = (uint64_t(uint32_t(cx + dx)) << 32) | uint32_t(cz + dz);
                auto it = m_groundHeights.find(key);
                if (it == m_groundHeights.end())
                    continue;
                if (!it->second.grass)
                    return false;                // the nearest ground here isn't grass
                *y = it->second.y;
                return true;
            }
    }
    return false;
}

bool Device::CreateGrassResources(std::string* error)
{
    DestroyGrassResources();
    if (!m_independentBlend)
        return true;                             // the blade pipeline's per-attachment write masks need it: no grass
    // One set: 0 the camera (uniform), 4 the frame lights (the sun and its shadow cascades), 5 the sun's shadow map.
    VkDescriptorSetLayoutBinding b[3] = {};
    b[0].binding = 0;
    b[1].binding = 4;
    b[2].binding = 5;
    b[0].descriptorType = b[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    for (auto& e : b)
        e.descriptorCount = 1;
    b[0].stageFlags = b[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    b[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    sl.bindingCount = 3;
    sl.pBindings = b;
    if (!Check(vkCreateDescriptorSetLayout(m_device, &sl, nullptr, &m_grassSetLayout),
               "vkCreateDescriptorSetLayout", error))
        return false;
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &m_grassSetLayout;
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
        VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_VERTEX_INPUT_EXT};
        VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        ds.dynamicStateCount = 3;
        ds.pDynamicStates = dyn;
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
        att[0].colorWriteMask = 0xF;             // the scene colour
        att[3].colorWriteMask = 0xF;             // and the motion vectors (the grass is static: the camera's motion)
        att[1].colorWriteMask = att[2].colorWriteMask = att[4].colorWriteMask = 0;   // glow, fraction, albedo: kept
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

// Draws the frame's grass into the scene rendering that is still open (Device::EndScene calls this right after the 3D
// scene, before the post passes). Blades are generated afresh each frame from the ground grid; the same world position
// yields the same blades, so nothing crawls.
void Device::RenderGrassField(VkCommandBuffer cmd)
{
    if (!m_grassOn || !m_grassPipeline || !m_scene || !m_shadowView || !m_frameLightsOffset)
        return;
    UpdateFrameEye();
    if (!m_frameEyeValid)
        return;
    // A patch of ground every `spacing` units, out to the grass distance, each with a few blades.
    const float spacing = std::max(0.6f, m_grassHeight * 1.6f);
    const int32_t r = int32_t(std::ceil(m_grassDistance / spacing));
    const int32_t cx0 = int32_t(std::floor(m_frameEye[0] / spacing));
    const int32_t cz0 = int32_t(std::floor(m_frameEye[2] / spacing));
    const float time = float(SwayClock());
    // A cap on the frame's blades so an extreme radius or density can't blow the ring (or the CPU) up: the nearest
    // cells are visited first (the loops run outward from the camera's cell), so what is kept is the nearest grass.
    constexpr size_t kMaxVerts = 400000;         // ~33k blades, ~11 MB a frame
    std::vector<GrassVertex> verts;
    verts.reserve(4096);
    bool full = false;
    for (int32_t cz = cz0 - r; cz <= cz0 + r && !full; ++cz)
        for (int32_t cx = cx0 - r; cx <= cx0 + r && !full; ++cx) {
            const float bx = (float(cx) + 0.5f) * spacing, bz = (float(cz) + 0.5f) * spacing;
            const float ddx = bx - m_frameEye[0], ddz = bz - m_frameEye[2];
            if (ddx * ddx + ddz * ddz > m_grassDistance * m_grassDistance)
                continue;
            float gy;
            if (!GroundHeight(bx, bz, &gy))
                continue;                        // no terrain seen here (a building, the sky): no grass
            const uint32_t cellHash = HashCell(cx, cz);
            int blades = int(m_grassDensity);
            if (Unit(cellHash) < m_grassDensity - float(blades))
                ++blades;
            for (int b = 0; b < blades; ++b) {
                if (verts.size() + 12 > kMaxVerts) {
                    full = true;
                    break;
                }
                const uint32_t h = HashCell(cx * 73856093 ^ cz * 19349663 ^ (b * 83492791), b * 2654435761u + cx);
                const float u1 = Unit(h), u2 = Unit(h * 2246822519u), u3 = Unit(h * 3266489917u);
                const float px = bx + (u1 - 0.5f) * spacing, pz = bz + (u2 - 0.5f) * spacing;
                float py;
                if (!GroundHeight(px, pz, &py))
                    py = gy;
                const float dist = std::sqrt(ddx * ddx + ddz * ddz);
                float height = m_grassHeight * (0.7f + 0.6f * u3) * EdgeFade(dist, m_grassDistance);
                if (height < 0.02f)
                    continue;
                const float yaw = u1 * 6.2831853f;
                const float rx = std::cos(yaw), rz = std::sin(yaw);
                const float half = 0.5f * (0.02f + 0.03f * u2) * (0.5f + height);
                const float phase = px * 0.3f + pz * 0.25f;
                const float wind = 0.12f * height *
                                   (std::sin(time * 1.7f + phase) + 0.4f * std::sin(time * 3.3f + phase * 1.7f));
                float n[3] = {rz, 0.5f, -rx};
                const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                n[0] /= len; n[1] /= len; n[2] /= len;
                // A tapered, bending strip: a few cross sections from the base to the tip, each half as wide as the
                // one below, the wind bending the upper ones more (so a breeze ripples through the field).
                constexpr int kSeg = 2;
                GrassVertex row[2][kSeg + 1];
                for (int s = 0; s <= kSeg; ++s) {
                    const float t = float(s) / float(kSeg);
                    const float bend = wind * t * t;
                    const float cx = px + bend * rx, cz = pz + bend * rz;
                    const float cy = py + height * t;
                    const float w = half * (1.0f - t);
                    SetVertex(row[0][s], cx - rx * w, cy, cz - rz * w, n, t);
                    SetVertex(row[1][s], cx + rx * w, cy, cz + rz * w, n, t);
                }
                for (int s = 0; s < kSeg; ++s) {
                    verts.push_back(row[0][s]); verts.push_back(row[1][s]); verts.push_back(row[0][s + 1]);
                    verts.push_back(row[1][s]); verts.push_back(row[1][s + 1]); verts.push_back(row[0][s + 1]);
                }
            }
        }
    if (verts.empty())
        return;
    const VkDeviceSize bytes = VkDeviceSize(verts.size()) * sizeof(GrassVertex);
    EnsureRingSpace(bytes + sizeof(GrassFrame));
    void* cpu = nullptr;
    const VkDeviceSize offset = Allocate(bytes, 4, &cpu);
    std::memcpy(cpu, verts.data(), size_t(bytes));
    GrassFrame gf{};
    gf.viewProj = MulMatrix(m_view, m_proj);
    gf.prevViewProj = m_prevViewProjValid ? MulMatrix(m_prevView, m_prevProj) : gf.viewProj;
    gf.viewport[0] = float(m_scene->m_width);
    gf.viewport[1] = float(m_scene->m_height);
    gf.viewport[2] = gf.viewport[3] = 0.0f;
    void* frameCpu = nullptr;
    const VkDeviceSize frameOffset = Allocate(sizeof(GrassFrame), 64, &frameCpu);   // minUniformBufferOffsetAlignment
    std::memcpy(frameCpu, &gf, sizeof(gf));
    Frame& f = m_frames[m_frameIndex];

    // The scene's rendering has just closed (EndScene's EndRendering flushed the batched draws): begin it again to add
    // the blades, depth-testing against the opaque scene, and leave the attachments as the post passes expect.
    BeginRenderingOn(m_scene);
    // The scene draws into a flipped viewport (Device::ViewportState): y down, height negative. Match it, or the
    // blades land mirrored in the sky.
    const float sw = float(m_scene->m_width), sh = float(m_scene->m_height);
    VkViewport viewport{0.5f, sh + 0.5f, sw, -sh, 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {m_scene->m_width, m_scene->m_height}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_grassPipeline);
    VkDescriptorBufferInfo frameInfo{f.ring, frameOffset, sizeof(GrassFrame)};
    VkDescriptorBufferInfo lightsInfo{f.ring, m_frameLightsOffset, sizeof(FrameLights)};
    VkDescriptorImageInfo shadowInfo{m_shadowSampler, m_shadowView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet writes[3] = {};
    writes[0].sType = writes[1].sType = writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstBinding = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].pBufferInfo = &frameInfo;
    writes[1].dstBinding = 4;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[1].pBufferInfo = &lightsInfo;
    writes[2].dstBinding = 5;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[2].pImageInfo = &shadowInfo;
    for (auto& w : writes)
        w.descriptorCount = 1;
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_grassLayout, 0, 3, writes);
    VkVertexInputBindingDescription2EXT binding{VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT};
    binding.binding = 0;
    binding.stride = sizeof(GrassVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    binding.divisor = 1;
    VkVertexInputAttributeDescription2EXT attrs[3] = {};
    for (auto& a : attrs)
        a.sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT;
    attrs[0].location = 0; attrs[0].binding = 0; attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT; attrs[0].offset = 0;
    attrs[1].location = 1; attrs[1].binding = 0; attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT; attrs[1].offset = 12;
    attrs[2].location = 2; attrs[2].binding = 0; attrs[2].format = VK_FORMAT_R32_SFLOAT; attrs[2].offset = 24;
    vkCmdSetVertexInputEXT(cmd, 1, &binding, 3, attrs);
    VkBuffer vb = f.ring;
    VkDeviceSize vbOffset = offset;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &vbOffset);
    vkCmdDraw(cmd, uint32_t(verts.size()), 1, 0, 0);
    EndRendering();
    m_cache = StateCache{};                      // this pipeline and its vertex input are not the scene's
    m_grassBlades += verts.size() / 3;
    ++m_grassDraws;
}

}  // namespace rvk
