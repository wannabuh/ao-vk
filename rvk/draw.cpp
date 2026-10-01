// D3D7 state, Clear, and drawing.
#include "internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace rvk {

using namespace vk;
using namespace detail;

namespace {

void ArgbToFloat(uint32_t c, float out[4])
{
    out[0] = ((c >> 16) & 0xFF) / 255.0f;
    out[1] = ((c >> 8) & 0xFF) / 255.0f;
    out[2] = (c & 0xFF) / 255.0f;
    out[3] = (c >> 24) / 255.0f;
}

void Copy4(float out[4], const d3d::Color& c)
{
    out[0] = c.r; out[1] = c.g; out[2] = c.b; out[3] = c.a;
}

float AsFloat(uint32_t v)
{
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

VkBlendFactor BlendFactor(uint32_t b)
{
    switch (b) {
    case d3d::BLEND_ZERO: return VK_BLEND_FACTOR_ZERO;
    case d3d::BLEND_ONE: return VK_BLEND_FACTOR_ONE;
    case d3d::BLEND_SRCCOLOR: return VK_BLEND_FACTOR_SRC_COLOR;
    case d3d::BLEND_INVSRCCOLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case d3d::BLEND_SRCALPHA: return VK_BLEND_FACTOR_SRC_ALPHA;
    case d3d::BLEND_INVSRCALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case d3d::BLEND_DESTALPHA: return VK_BLEND_FACTOR_DST_ALPHA;
    case d3d::BLEND_INVDESTALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case d3d::BLEND_DESTCOLOR: return VK_BLEND_FACTOR_DST_COLOR;
    case d3d::BLEND_INVDESTCOLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case d3d::BLEND_SRCALPHASAT: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default: return VK_BLEND_FACTOR_ONE;
    }
}

VkCompareOp CompareOp(uint32_t c)
{
    switch (c) {
    case d3d::CMP_NEVER: return VK_COMPARE_OP_NEVER;
    case d3d::CMP_LESS: return VK_COMPARE_OP_LESS;
    case d3d::CMP_EQUAL: return VK_COMPARE_OP_EQUAL;
    case d3d::CMP_LESSEQUAL: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case d3d::CMP_GREATER: return VK_COMPARE_OP_GREATER;
    case d3d::CMP_NOTEQUAL: return VK_COMPARE_OP_NOT_EQUAL;
    case d3d::CMP_GREATEREQUAL: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}

VkPrimitiveTopology Topology(uint32_t p)
{
    switch (p) {
    case d3d::PointList: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case d3d::LineList: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case d3d::LineStrip: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case d3d::TriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case d3d::TriangleFan: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

uint32_t TopologyClass(uint32_t p)
{
    return p == d3d::PointList ? 0 : (p == d3d::LineList || p == d3d::LineStrip) ? 1 : 2;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------
// State

void Device::SetRenderState(uint32_t state, uint32_t value)
{
    if (state < m_rs.size() && m_rs[state] != value) {
        m_rs[state] = value;
        m_constantsDirty = true;
    }
}

void Device::SetTextureStageState(uint32_t stage, uint32_t type, uint32_t value)
{
    if (stage >= 2 || type >= d3d::TSS_COUNT)
        return;
    if (m_tss[stage][type] == value && type != d3d::TSS_ADDRESS)
        return;
    m_tss[stage][type] = value;
    if (type == d3d::TSS_ADDRESS)                      // ADDRESS sets both U and V
        m_tss[stage][d3d::TSS_ADDRESSU] = m_tss[stage][d3d::TSS_ADDRESSV] = value;
    m_constantsDirty = true;
}

void Device::SetTransform(uint32_t type, const d3d::Matrix& m)
{
    // The world matrix has its own per-draw block; the others live in the cached constant block.
    d3d::Matrix* target = nullptr;
    switch (type) {
    case d3d::World: m_world = m; return;
    case d3d::View: target = &m_view; break;
    case d3d::Projection: target = &m_proj; break;
    case d3d::Texture0: target = &m_texMatrix[0]; break;
    case d3d::Texture1: target = &m_texMatrix[1]; break;
    default: return;
    }
    if (std::memcmp(target, &m, sizeof(m)) != 0) {
        *target = m;
        m_constantsDirty = true;
    }
}

void Device::SetMaterial(const d3d::Material& m)
{
    if (std::memcmp(&m_material, &m, sizeof(m)) != 0) {
        m_material = m;
        m_constantsDirty = true;
    }
}

void Device::SetLight(uint32_t index, const d3d::Light& light)
{
    if (index >= m_lights.size())
        m_lights.resize(index + 1);
    m_lights[index].light = light;
    if (m_lights[index].enabled)
        m_constantsDirty = true;
    m_lights[index].cosHalfTheta = std::cos(light.theta * 0.5f);
    m_lights[index].cosHalfPhi = std::cos(light.phi * 0.5f);
}

void Device::LightEnable(uint32_t index, bool enable)
{
    if (index >= m_lights.size())
        m_lights.resize(index + 1);
    if (m_lights[index].enabled != enable) {
        m_lights[index].enabled = enable;
        m_constantsDirty = true;
    }
}

void Device::SetTexture(uint32_t stage, Texture* texture)
{
    if (texture && !texture->m_view)            // failed creation: draw untextured
        texture = nullptr;
    if (stage < 2)
        m_textures[stage] = texture == m_target ? nullptr : texture;
}

void Device::SetViewport(const d3d::Viewport& vp)
{
    if (std::memcmp(&m_viewport, &vp, sizeof(vp)) != 0) {
        m_viewport = vp;
        m_constantsDirty = true;
    }
}

void Device::Clear(uint32_t flags, uint32_t argb, float z)
{
    Clear(0, nullptr, flags, argb, z);
}

void Device::Clear(uint32_t count, const Rect* rects, uint32_t flags, uint32_t argb, float z)
{
    if (!m_inFrame)
        return;
    VkClearAttachment att[2];
    uint32_t n = 0;
    if (flags & d3d::CLEAR_TARGET) {
        att[n] = {VK_IMAGE_ASPECT_COLOR_BIT, 0, {}};
        ArgbToFloat(argb, att[n].clearValue.color.float32);
        ++n;
    }
    if (flags & d3d::CLEAR_ZBUFFER) {
        att[n] = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, {}};
        att[n].clearValue.depthStencil = {z, 0};
        ++n;
    }
    if (!n)
        return;
    // D3D clears the given rectangles (or the whole viewport), always clipped to the viewport.
    int32_t vx0 = int32_t(std::min(m_viewport.x, m_target->m_width));
    int32_t vy0 = int32_t(std::min(m_viewport.y, m_target->m_height));
    int32_t vx1 = int32_t(std::min(m_viewport.x + m_viewport.width, m_target->m_width));
    int32_t vy1 = int32_t(std::min(m_viewport.y + m_viewport.height, m_target->m_height));
    Rect whole{vx0, vy0, vx1, vy1};
    if (!count) {
        count = 1;
        rects = &whole;
    }
    std::vector<VkClearRect> clears;
    for (uint32_t i = 0; i < count; ++i) {
        int32_t x0 = std::max(rects[i].left, vx0), y0 = std::max(rects[i].top, vy0);
        int32_t x1 = std::min(rects[i].right, vx1), y1 = std::min(rects[i].bottom, vy1);
        if (x1 > x0 && y1 > y0)
            clears.push_back({{{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}}, 0, 1});
    }
    if (!clears.empty())
        vkCmdClearAttachments(m_frames[m_frameIndex].main, n, att, uint32_t(clears.size()), clears.data());
}

void Device::CopyTexture(Texture* dst, const Rect* dstRect, Texture* src, const Rect* srcRect, bool linear)
{
    if (!m_inFrame)
        return;
    if (!dst) dst = m_main;
    if (!src) src = m_main;
    if (!dst->m_renderTarget || src == dst || FormatIsCompressed(src->m_format) || !dst->m_image || !src->m_image) {
        Log("CopyTexture: unsupported source/destination combination\n");
        return;
    }
    VkCommandBuffer cmd = m_frames[m_frameIndex].main;
    EndRendering();
    VkImageLayout srcRestore = src->m_layout;
    Transition(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    Transition(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit blit{};
    blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    Rect s = srcRect ? *srcRect : Rect{0, 0, int32_t(src->m_width), int32_t(src->m_height)};
    Rect d = dstRect ? *dstRect : Rect{0, 0, int32_t(dst->m_width), int32_t(dst->m_height)};
    blit.srcOffsets[0] = {s.left, s.top, 0};
    blit.srcOffsets[1] = {s.right, s.bottom, 1};
    blit.dstOffsets[0] = {d.left, d.top, 0};
    blit.dstOffsets[1] = {d.right, d.bottom, 1};
    vkCmdBlitImage(cmd, src->m_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &blit, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    // Plain textures stay sampleable between uploads (their layout is otherwise only changed by uploads).
    if (!src->m_renderTarget)
        Transition(cmd, src, srcRestore);
    BeginRenderingOn(m_target);
}

// ---------------------------------------------------------------------------------------------------
// Drawing

void Device::DrawPrimitive(uint32_t primitive, uint32_t fvf, const void* vertices, uint32_t vertexCount)
{
    Draw(primitive, fvf, vertices, vertexCount, nullptr, 0);
}

void Device::DrawIndexedPrimitive(uint32_t primitive, uint32_t fvf, const void* vertices, uint32_t vertexCount,
                                  const uint16_t* indices, uint32_t indexCount)
{
    Draw(primitive, fvf, vertices, vertexCount, indices, indexCount);
}

void Device::DrawPrimitiveVB(uint32_t primitive, VertexBuffer* vb, uint32_t startVertex, uint32_t vertexCount)
{
    if (!vb || startVertex + vertexCount > vb->m_count)
        return;
    Draw(primitive, vb->m_fvf, vb->m_data.data() + size_t(startVertex) * vb->m_stride, vertexCount, nullptr, 0);
}

void Device::DrawIndexedPrimitiveVB(uint32_t primitive, VertexBuffer* vb, uint32_t startVertex, uint32_t vertexCount,
                                    const uint16_t* indices, uint32_t indexCount)
{
    // D3D7: indices are relative to startVertex; vertexCount vertices from there are referenced.
    if (!vb || startVertex + vertexCount > vb->m_count)
        return;
    Draw(primitive, vb->m_fvf, vb->m_data.data() + size_t(startVertex) * vb->m_stride, vertexCount, indices, indexCount);
}

void Device::ApplyDynamicState(uint32_t primitive, uint32_t fvf, uint32_t stride)
{
    VkCommandBuffer cmd = m_frames[m_frameIndex].main;
    StateCache& c = m_cache;
    uint32_t topoClass = TopologyClass(primitive);
    if (c.topologyClass != topoClass) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelines[topoClass]);
        c.topologyClass = topoClass;
    }
    if (c.topology != primitive) {
        vkCmdSetPrimitiveTopology(cmd, Topology(primitive));
        c.topology = primitive;
    }
    if (!c.valid) {
        // D3D front faces are clockwise on screen; D3DCULL_CCW culls the counter-clockwise (back) ones.
        vkCmdSetFrontFace(cmd, VK_FRONT_FACE_CLOCKWISE);
        c.valid = true;
    }

    // Negative height flips Y so D3D's clip space maps the same way; +0.5 matches D3D pixel centres.
    VkViewport vp;
    vp.x = float(m_viewport.x) + 0.5f;
    vp.y = float(m_viewport.y + m_viewport.height) + 0.5f;
    vp.width = float(m_viewport.width);
    vp.height = -float(m_viewport.height);
    vp.minDepth = m_viewport.minZ;
    vp.maxDepth = m_viewport.maxZ;
    if (std::memcmp(&vp, &c.viewport, sizeof(vp)) != 0) {
        vkCmdSetViewport(cmd, 0, 1, &vp);
        c.viewport = vp;
    }
    uint32_t w = m_target->m_width, h = m_target->m_height;
    uint32_t sx = std::min(m_viewport.x, w), sy = std::min(m_viewport.y, h);
    VkRect2D scissor{{int32_t(sx), int32_t(sy)}, {std::min(m_viewport.width, w - sx), std::min(m_viewport.height, h - sy)}};
    if (std::memcmp(&scissor, &c.scissor, sizeof(scissor)) != 0) {
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        c.scissor = scissor;
    }

    uint32_t cull = m_rs[d3d::RS_CULLMODE];
    if (c.cull != cull) {
        vkCmdSetCullMode(cmd, cull == d3d::CULL_CCW ? VK_CULL_MODE_BACK_BIT
                              : cull == d3d::CULL_CW ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_NONE);
        c.cull = cull;
    }
    uint32_t zEnable = m_rs[d3d::RS_ZENABLE] != 0;
    uint32_t zWrite = zEnable && m_rs[d3d::RS_ZWRITEENABLE] != 0;
    uint32_t zFunc = m_rs[d3d::RS_ZFUNC];
    if (c.depthTest != zEnable) { vkCmdSetDepthTestEnable(cmd, zEnable); c.depthTest = zEnable; }
    if (c.depthWrite != zWrite) { vkCmdSetDepthWriteEnable(cmd, zWrite); c.depthWrite = zWrite; }
    if (c.depthOp != zFunc) { vkCmdSetDepthCompareOp(cmd, CompareOp(zFunc)); c.depthOp = zFunc; }

    VkBool32 blend = m_rs[d3d::RS_ALPHABLENDENABLE] != 0;
    if (c.blendEnable != blend) {
        vkCmdSetColorBlendEnableEXT(cmd, 0, 1, &blend);
        c.blendEnable = blend;
    }
    uint32_t src = m_rs[d3d::RS_SRCBLEND], dst = m_rs[d3d::RS_DESTBLEND];
    if (src == d3d::BLEND_BOTHSRCALPHA) { src = d3d::BLEND_SRCALPHA; dst = d3d::BLEND_INVSRCALPHA; }
    if (src == d3d::BLEND_BOTHINVSRCALPHA) { src = d3d::BLEND_INVSRCALPHA; dst = d3d::BLEND_SRCALPHA; }
    // The equation only matters while blending, but Vulkan wants it set once per command buffer regardless.
    if (c.src == ~0u || (blend && (c.src != src || c.dst != dst))) {
        VkColorBlendEquationEXT eq{};
        eq.srcColorBlendFactor = eq.srcAlphaBlendFactor = BlendFactor(src);
        eq.dstColorBlendFactor = eq.dstAlphaBlendFactor = BlendFactor(dst);
        eq.colorBlendOp = eq.alphaBlendOp = VK_BLEND_OP_ADD;
        vkCmdSetColorBlendEquationEXT(cmd, 0, 1, &eq);
        c.src = src;
        c.dst = dst;
    }

    if (c.fvf != fvf) {
        // Vertex layout: binding 0 = the frame's ring buffer (draws address it through their vertex offset),
        // binding 1 = zeros for attributes the format lacks.
        FvfLayout layout = DecodeFvf(fvf);
        VkVertexInputBindingDescription2EXT bindings[2] = {
            {VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT, nullptr, 0, stride, VK_VERTEX_INPUT_RATE_VERTEX, 1},
            {VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT, nullptr, 1, 0, VK_VERTEX_INPUT_RATE_VERTEX, 1},
        };
        VkVertexInputAttributeDescription2EXT attrs[6];
        for (uint32_t i = 0; i < 6; ++i) {
            attrs[i] = {VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT};
            attrs[i].location = i;
            if (layout.offset[i] >= 0) {
                attrs[i].binding = 0;
                attrs[i].format = layout.format[i];
                attrs[i].offset = uint32_t(layout.offset[i]);
            } else {
                attrs[i].binding = 1;
                attrs[i].format = VK_FORMAT_R32G32B32A32_SFLOAT;
                attrs[i].offset = 0;
            }
        }
        vkCmdSetVertexInputEXT(cmd, 2, bindings, 6, attrs);
        c.fvf = fvf;
    }
    if (!c.buffersBound) {
        Frame& f = m_frames[m_frameIndex];
        VkBuffer buffers[2] = {f.ring, m_nullBuffer};
        VkDeviceSize offsets[2] = {0, 0};
        vkCmdBindVertexBuffers(cmd, 0, 2, buffers, offsets);
        vkCmdBindIndexBuffer(cmd, f.ring, 0, VK_INDEX_TYPE_UINT16);
        c.buffersBound = true;
    }
}

void Device::Draw(uint32_t primitive, uint32_t fvf, const void* vertices, uint32_t vertexCount,
                  const uint16_t* indices, uint32_t indexCount)
{
    if (!m_inFrame || !vertexCount)
        return;
    Frame& f = m_frames[m_frameIndex];
    VkCommandBuffer cmd = f.main;
    FvfLayout layout = DecodeFvf(fvf);

    // Render targets bound as textures must be readable; layout changes can't happen inside rendering.
    bool needTransition = false;
    for (Texture* t : m_textures)
        if (t && t->m_renderTarget && t->m_layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
            needTransition = true;
    if (needTransition) {
        vkCmdEndRendering(cmd);
        for (Texture* t : m_textures)
            if (t && t->m_renderTarget)
                Transition(cmd, t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        BeginRenderingOn(m_target);
    }

    // Everything this draw puts in the ring buffer, reserved together so a flush can't split it.
    const VkDeviceSize uboAlign = m_props.limits.minUniformBufferOffsetAlignment;
    EnsureRingSpace(sizeof(DrawConstants) + sizeof(DrawTransform) + VkDeviceSize(layout.stride) * vertexCount +
                    VkDeviceSize(indexCount) * 2 + 2 * uboAlign + layout.stride + 32);

    // Per-draw world matrix (small block).
    void* cpu;
    VkDeviceSize transformOffset = Allocate(sizeof(DrawTransform), uboAlign, &cpu);
    std::memcpy(cpu, &m_world, sizeof(m_world));

    // The big constant block: reused unless something feeding it changed since it was written.
    uint32_t texMask = (m_textures[0] ? 1u : 0u) | (m_textures[1] ? 2u : 0u);
    bool rewrite = m_constantsDirty || m_constantsGeneration != m_ringGeneration || m_constantsFvf != fvf ||
                   m_constantsTexMask != texMask;
    VkDeviceSize uboOffset = m_constantsOffset;
    if (rewrite) {
    uboOffset = Allocate(sizeof(DrawConstants), uboAlign, &cpu);
    m_constantsOffset = uboOffset;
    m_constantsGeneration = m_ringGeneration;   // after Allocate: a wrap would bump the generation
    m_constantsDirty = false;
    m_constantsFvf = fvf;
    m_constantsTexMask = texMask;
    auto* c = static_cast<DrawConstants*>(cpu);
    c->view = m_view;
    c->proj = m_proj;
    c->texMatrix[0] = m_texMatrix[0];
    c->texMatrix[1] = m_texMatrix[1];
    c->viewport[0] = float(m_viewport.x);
    c->viewport[1] = float(m_viewport.y);
    c->viewport[2] = float(m_viewport.width);
    c->viewport[3] = float(m_viewport.height);
    Copy4(c->matDiffuse, m_material.diffuse);
    Copy4(c->matAmbient, m_material.ambient);
    Copy4(c->matSpecular, m_material.specular);
    Copy4(c->matEmissive, m_material.emissive);
    ArgbToFloat(m_rs[d3d::RS_AMBIENT], c->ambient);
    ArgbToFloat(m_rs[d3d::RS_FOGCOLOR], c->fogColor);
    c->fogParams[0] = AsFloat(m_rs[d3d::RS_FOGSTART]);
    c->fogParams[1] = AsFloat(m_rs[d3d::RS_FOGEND]);
    c->fogParams[2] = AsFloat(m_rs[d3d::RS_FOGDENSITY]);
    c->fogParams[3] = 0.0f;
    ArgbToFloat(m_rs[d3d::RS_TEXTUREFACTOR], c->tfactor);
    c->misc[0] = m_material.power;
    c->misc[1] = float(m_rs[d3d::RS_ALPHAREF] & 0xFF);
    c->misc[2] = c->misc[3] = 0.0f;
    // Camera position/forward in world space from the view matrix (columns 0-2 = camera axes for an
    // orthonormal D3D view matrix; row 3 = -eye expressed in those axes).
    const auto& v = m_view.m;
    for (int i = 0; i < 3; ++i) {
        c->eyePos[i] = -(v[3][0] * v[i][0] + v[3][1] * v[i][1] + v[3][2] * v[i][2]);
        c->eyeDir[i] = v[i][2];
    }
    c->eyePos[3] = 1.0f;
    c->eyeDir[3] = 0.0f;
    c->vtx[0] = fvf;
    c->vtx[1] = c->vtx[2] = c->vtx[3] = 0;
    uint32_t flags = 0;
    if (m_rs[d3d::RS_LIGHTING] && (fvf & d3d::FVF_POSITION_MASK) != d3d::FVF_XYZRHW) flags |= F_LIGHTING;
    if (m_rs[d3d::RS_COLORVERTEX]) flags |= F_COLORVERTEX;
    if (m_rs[d3d::RS_SPECULARENABLE]) flags |= F_SPECULAR;
    if (m_rs[d3d::RS_NORMALIZENORMALS]) flags |= F_NORMALIZE;
    if (m_rs[d3d::RS_FOGENABLE]) flags |= F_FOG;
    if (m_rs[d3d::RS_RANGEFOGENABLE]) flags |= F_RANGEFOG;
    if (m_rs[d3d::RS_LOCALVIEWER]) flags |= F_LOCALVIEWER;
    if (m_textures[0]) flags |= F_TEX0;
    if (m_textures[1]) flags |= F_TEX1;
    if (m_rs[d3d::RS_ALPHATESTENABLE]) flags |= F_ALPHATEST;
    c->flags[0] = flags;
    c->flags[1] = m_rs[d3d::RS_FOGVERTEXMODE];
    c->flags[2] = m_rs[d3d::RS_FOGTABLEMODE];
    c->flags[3] = m_rs[d3d::RS_ALPHAFUNC];
    c->matSources[0] = m_rs[d3d::RS_DIFFUSEMATERIALSOURCE];
    c->matSources[1] = m_rs[d3d::RS_AMBIENTMATERIALSOURCE];
    c->matSources[2] = m_rs[d3d::RS_SPECULARMATERIALSOURCE];
    c->matSources[3] = m_rs[d3d::RS_EMISSIVEMATERIALSOURCE];
    for (int s = 0; s < 2; ++s) {
        const auto& t = m_tss[s];
        c->stageA[s][0] = t[d3d::TSS_COLOROP];
        c->stageA[s][1] = t[d3d::TSS_COLORARG1];
        c->stageA[s][2] = t[d3d::TSS_COLORARG2];
        c->stageA[s][3] = t[d3d::TSS_ALPHAOP];
        c->stageB[s][0] = t[d3d::TSS_ALPHAARG1];
        c->stageB[s][1] = t[d3d::TSS_ALPHAARG2];
        c->stageB[s][2] = t[d3d::TSS_TEXCOORDINDEX];
        c->stageB[s][3] = t[d3d::TSS_TEXTURETRANSFORMFLAGS];
    }
    uint32_t lightCount = 0;
    if (flags & F_LIGHTING)
        for (const LightSlot& slot : m_lights) {
            if (!slot.enabled || lightCount == kMaxLights)
                continue;
            const d3d::Light& l = slot.light;
            GpuLight& g = c->lights[lightCount++];
            Copy4(g.diffuse, l.diffuse);
            Copy4(g.specular, l.specular);
            Copy4(g.ambient, l.ambient);
            g.position[0] = l.position.x; g.position[1] = l.position.y; g.position[2] = l.position.z;
            g.position[3] = float(l.type);
            g.direction[0] = l.direction.x; g.direction[1] = l.direction.y; g.direction[2] = l.direction.z;
            g.direction[3] = l.range;
            g.atten[0] = l.attenuation0; g.atten[1] = l.attenuation1; g.atten[2] = l.attenuation2; g.atten[3] = l.falloff;
            g.spot[0] = slot.cosHalfTheta;
            g.spot[1] = slot.cosHalfPhi;
            g.spot[2] = g.spot[3] = 0.0f;
        }
    c->lightInfo[0] = lightCount;
    c->lightInfo[1] = c->lightInfo[2] = c->lightInfo[3] = 0;
    }

    // Geometry: vertices aligned to their stride and indices to 2 bytes, so the draw can address them inside
    // the ring buffer bound once (vertexOffset / firstIndex) instead of rebinding buffers per draw.
    VkDeviceSize vbBytes = VkDeviceSize(layout.stride) * vertexCount;
    VkDeviceSize vbOffset = Allocate(vbBytes, layout.stride, &cpu);
    std::memcpy(cpu, vertices, vbBytes);
    VkDeviceSize ibOffset = 0;
    if (indices) {
        ibOffset = Allocate(VkDeviceSize(indexCount) * 2, 2, &cpu);
        std::memcpy(cpu, indices, size_t(indexCount) * 2);
    }

    ApplyDynamicState(primitive, fvf, layout.stride);

    VkDescriptorBufferInfo ubo{f.ring, uboOffset, sizeof(DrawConstants)};
    VkDescriptorBufferInfo transform{f.ring, transformOffset, sizeof(DrawTransform)};
    VkDescriptorImageInfo images[2];
    for (uint32_t s = 0; s < 2; ++s) {
        Texture* t = m_textures[s] ? m_textures[s] : m_blackTexture;
        images[s] = {SamplerFor(s), t->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }
    VkWriteDescriptorSet writes[4] = {};
    for (int i = 0; i < 4; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstBinding = uint32_t(i);
        writes[i].descriptorCount = 1;
    }
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].pBufferInfo = &ubo;
    writes[1].descriptorType = writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo = &images[0];
    writes[2].pImageInfo = &images[1];
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[3].pBufferInfo = &transform;
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 4, writes);

    if (indices)
        vkCmdDrawIndexed(cmd, indexCount, 1, uint32_t(ibOffset / 2), int32_t(vbOffset / layout.stride), 0);
    else
        vkCmdDraw(cmd, vertexCount, 1, uint32_t(vbOffset / layout.stride), 0);
}

}  // namespace rvk
