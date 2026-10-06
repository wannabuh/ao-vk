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

// The pass's push constant (grass.vert / grass.frag Grass). Must match the shaders' block.
struct GrassPush {
    float sunDir[4];
    float sunColor[4];
    float params[4];
};
static_assert(sizeof(GrassPush) == 48, "grass push constant");

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

void SetVertex(GrassVertex& v, float x, float y, float z, const float n[3], float shade)
{
    v.pos[0] = x; v.pos[1] = y; v.pos[2] = z;
    v.normal[0] = n[0]; v.normal[1] = n[1]; v.normal[2] = n[2];
    v.shade = shade;
}

}  // namespace

// Records the terrain's ground heights into a world grid (m_groundHeights), for the grass blades to sit on. Called for
// every terrain draw (IsTerrain) while the grass is on; only cells within reach of the camera are kept, so a long
// session roaming a zone can't grow the grid without bound.
void Device::CaptureTerrain(const void* vertices, const FvfLayout& layout, uint32_t vertexCount)
{
    if (!m_grassOn || !vertices || !m_frameEyeValid || layout.offset[0] < 0)
        return;
    const uint8_t* src = static_cast<const uint8_t*>(vertices);
    const float reach = m_grassDistance + 24.0f;
    const size_t posOff = size_t(layout.offset[0]);
    for (uint32_t i = 0; i < vertexCount; ++i) {
        float p[3];
        std::memcpy(p, src + size_t(i) * layout.stride + posOff, sizeof(p));
        float dx = p[0] - m_frameEye[0], dz = p[2] - m_frameEye[2];
        if (dx * dx + dz * dz > reach * reach)
            continue;
        int32_t cx = int32_t(std::floor(p[0] / kGroundCell));
        int32_t cz = int32_t(std::floor(p[2] / kGroundCell));
        uint64_t key = (uint64_t(uint32_t(cx)) << 32) | uint32_t(cz);
        m_groundHeights[key] = p[1];             // the base and the light pass agree; the latest wins
    }
    if (m_groundHeights.size() > 4000000)
        m_groundHeights.clear();                 // a guard only; the reach test above keeps it near the camera
}

// The ground height under a world x, z, if the terrain has been seen there. Searches the neighbouring cells too: the
// terrain's own vertex spacing is coarser than kGroundCell, so the exact cell is often empty.
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
                if (it != m_groundHeights.end()) {
                    *y = it->second;
                    return true;
                }
            }
    }
    return false;
}

bool Device::CreateGrassResources(std::string* error)
{
    DestroyGrassResources();
    if (!m_independentBlend)
        return true;                             // the blade pipeline's per-attachment write masks need it: no grass
    VkPushConstantRange range{};
    range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    range.offset = 0;
    range.size = sizeof(GrassPush);
    VkDescriptorSetLayoutBinding b{};
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    sl.bindingCount = 1;
    sl.pBindings = &b;
    if (!Check(vkCreateDescriptorSetLayout(m_device, &sl, nullptr, &m_grassSetLayout),
               "vkCreateDescriptorSetLayout", error))
        return false;
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
    if (!m_grassOn || !m_grassPipeline || !m_scene || !m_frameEyeValid)
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
    std::vector<GrassVertex> verts;
    verts.reserve(size_t(3) * 64);
    for (int32_t cz = cz0 - r; cz <= cz0 + r; ++cz)
        for (int32_t cx = cx0 - r; cx <= cx0 + r; ++cx) {
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
                const uint32_t h = HashCell(cx * 73856093 ^ cz * 19349663 ^ (b * 83492791), b * 2654435761u + cx);
                const float u1 = Unit(h), u2 = Unit(h * 2246822519u), u3 = Unit(h * 3266489917u);
                const float px = bx + (u1 - 0.5f) * spacing, pz = bz + (u2 - 0.5f) * spacing;
                float py;
                if (!GroundHeight(px, pz, &py))
                    py = gy;
                const float height = m_grassHeight * (0.7f + 0.6f * u3);
                const float yaw = u1 * 6.2831853f;
                const float rx = std::cos(yaw), rz = std::sin(yaw);
                const float half = 0.5f * (0.02f + 0.03f * u2) * (0.5f + height);
                const float phase = px * 0.3f + pz * 0.25f;
                const float wind = 0.12f * height *
                                   (std::sin(time * 1.7f + phase) + 0.4f * std::sin(time * 3.3f + phase * 1.7f));
                const float tipX = px + wind * rx, tipZ = pz + wind * rz;
                float n[3] = {rz, 0.5f, -rx};
                const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                n[0] /= len; n[1] /= len; n[2] /= len;
                GrassVertex v0, v1, v2;
                SetVertex(v0, px - rx * half, py, pz - rz * half, n, 0.0f);
                SetVertex(v1, px + rx * half, py, pz + rz * half, n, 0.0f);
                SetVertex(v2, tipX, py + height, tipZ, n, 1.0f);
                verts.push_back(v0);
                verts.push_back(v1);
                verts.push_back(v2);
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
    VkWriteDescriptorSet frameWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    frameWrite.dstBinding = 0;
    frameWrite.descriptorCount = 1;
    frameWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    frameWrite.pBufferInfo = &frameInfo;
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_grassLayout, 0, 1, &frameWrite);
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
    GrassPush pc{};
    const float* sd = m_shadowValid ? m_shadowSunDir : m_frameSunDir;
    const float* sc = m_shadowValid ? m_shadowSunColor : m_frameSunColor;
    pc.sunDir[0] = sd[0]; pc.sunDir[1] = sd[1]; pc.sunDir[2] = sd[2]; pc.sunDir[3] = 1.0f;
    pc.sunColor[0] = sc[0]; pc.sunColor[1] = sc[1]; pc.sunColor[2] = sc[2]; pc.sunColor[3] = 0.45f;
    pc.params[0] = m_frameEye[0]; pc.params[1] = m_frameEye[1]; pc.params[2] = m_frameEye[2]; pc.params[3] = time;
    vkCmdPushConstants(cmd, m_grassLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
    vkCmdDraw(cmd, uint32_t(verts.size()), 1, 0, 0);
    EndRendering();
    m_cache = StateCache{};                      // this pipeline and its vertex input are not the scene's
    m_grassBlades += verts.size() / 3;
    ++m_grassDraws;
}

}  // namespace rvk
