// Depth pre-pass: the scene's opaque draws draw their depth first, so the expensive scene shader runs about once per
// pixel instead of for every surface the game draws over another (the statics shaded ~3.5 screens' worth a frame).
//
// The game draws in its own order, one call at a time, so the pre-pass can't simply come first. Instead the frame's
// main commands are split where the scene's depth is cleared (PrepassArm, from Clear): everything recorded before runs
// as before (mainA); then the pre-pass command buffer - the depth clear, then the depth of every opaque draw that
// follows (PrepassDraw, recorded as each draw comes); then the main commands from the clear on (mainB), whose draws
// now find the nearest opaque surface's depth already there. An opaque draw passes its own depth test on equal (the
// same vertex shader, gl_Position invariant); whatever is behind it is rejected before shading.
//
// Only a contiguous stretch of the scene's rendering is pre-passed - the segment ends (PrepassEnd) when that rendering
// ends (a target switch, a copy of the scene, a flush, the end of the scene) or at a second depth clear - because
// within it an opaque surface drawn later only hides what was drawn earlier where it covers it anyway. "Opaque": no
// discard, depth written, and either not blended or blended by its alpha with that alpha provably 1 (AlphaOneCheck).
#include "internal.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

namespace rvk {

using namespace vk;
using namespace detail;

bool Device::CreatePrepassPipeline(VkShaderModule vert)
{
    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                                          VK_SHADER_STAGE_VERTEX_BIT, vert, "main", nullptr};
    VkDynamicState dynamic[] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE,
        VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_VERTEX_INPUT_EXT};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = uint32_t(sizeof(dynamic) / sizeof(dynamic[0]));
    ds.pDynamicStates = dynamic;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo dss{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.depthAttachmentFormat = kDepthFormat;   // depth only: no colour attachments, no fragment shader
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.pNext = &rendering;
    ci.stageCount = 1;
    ci.pStages = &stage;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &dss;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &ds;
    ci.layout = m_pipelineLayout;
    if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &ci, nullptr, &m_prepassPipeline) != VK_SUCCESS) {
        m_prepassPipeline = VK_NULL_HANDLE;
        Log("depth pre-pass: pipeline creation failed; off");
        return false;
    }
    return true;
}

// The draw's output alpha is exactly 1, so blending it by its alpha (SRCALPHA / INVSRCALPHA) is the same as drawing it
// opaque. Follows ffp_main.glsl Cascade: the running alpha starts as the diffuse colour's; each stage's alpha op of
// arguments that are all 1 gives 1 (clamped), except SUBTRACT and complemented arguments. Only the arguments the op
// reads count: D3D's default stage 0 alpha is SELECTARG1 (the texture) with CURRENT (the diffuse) as an unread arg 2.
// kPreIn when provable, else the input that may be below 1.
uint32_t Device::AlphaOneCheck(uint32_t fvf) const
{
    bool needDiffuse = false, needTfactor = false, currentIsDiffuse = true;
    for (int s = 0; s < 2; ++s) {
        if (m_tss[s][d3d::TSS_COLOROP] == d3d::TOP_DISABLE) break;   // the cascade ends with the running alpha
        uint32_t op = m_tss[s][d3d::TSS_ALPHAOP];
        if (op == d3d::TOP_DISABLE) continue;                       // keeps the running alpha
        if (op == d3d::TOP_SUBTRACT) return kPreAlphaArg;
        uint32_t args[2], n = 0;
        if (op != d3d::TOP_SELECTARG2) args[n++] = m_tss[s][d3d::TSS_ALPHAARG1];
        if (op != d3d::TOP_SELECTARG1) args[n++] = m_tss[s][d3d::TSS_ALPHAARG2];
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t arg = args[i];
            if (arg & 0x10u) return kPreAlphaArg;                   // a complemented argument turns 1 into 0
            switch (arg & 0xFu) {
            case d3d::TA_DIFFUSE: needDiffuse = true; break;
            case d3d::TA_CURRENT: if (currentIsDiffuse) needDiffuse = true; break;
            case d3d::TA_TFACTOR: needTfactor = true; break;
            case d3d::TA_TEXTURE: {
                // Unbound: the shader reads alpha 1. Bound: no transparent texel, and no border (its colour's alpha).
                Texture* t = m_textures[s];
                if (t && !t->m_opaque) return kPreAlphaTexture;
                if (t && (m_tss[s][d3d::TSS_ADDRESSU] == d3d::TADDRESS_BORDER ||
                          m_tss[s][d3d::TSS_ADDRESSV] == d3d::TADDRESS_BORDER))
                    return kPreAlphaTexture;
                break;
            }
            default: return kPreAlphaArg;                           // the specular alpha (0 without one): unknown
            }
        }
        currentIsDiffuse = false;
    }
    if (currentIsDiffuse) needDiffuse = true;                       // no stage replaced the diffuse alpha
    if (needTfactor && (m_rs[d3d::RS_TEXTUREFACTOR] >> 24) != 0xFF)
        return kPreAlphaTfactor;
    if (!needDiffuse)
        return kPreIn;
    // The diffuse alpha (ffp.vert): lit, the material's diffuse source's; unlit, the vertex colour's (1 without one).
    bool hasDiffuse = (fvf & d3d::FVF_DIFFUSE) != 0, hasSpecular = (fvf & d3d::FVF_SPECULAR) != 0;
    if (m_rs[d3d::RS_LIGHTING]) {
        uint32_t source = m_rs[d3d::RS_DIFFUSEMATERIALSOURCE];
        bool colorVertex = m_rs[d3d::RS_COLORVERTEX] != 0;
        if (colorVertex && source == 1 && hasDiffuse)
            return m_drawMesh && m_drawMesh->diffuseAlphaOne ? kPreIn : kPreAlphaDiffuse;
        if (colorVertex && source == 2 && hasSpecular)
            return m_drawMesh && m_drawMesh->specularAlphaOne ? kPreIn : kPreAlphaDiffuse;
        return m_material.diffuse.a >= 1.0f ? kPreIn : kPreAlphaMaterial;
    }
    return !hasDiffuse || (m_drawMesh && m_drawMesh->diffuseAlphaOne) ? kPreIn : kPreAlphaDiffuse;
}

// The backdrop cull (m_drawBackdrop): a draw that ignores depth and doesn't write it, while the armed segment's depth
// is still exactly the pre-pass's (no draw has written depth in the main pass since the clear). Its pixels survive in
// the final image only where no opaque draw of the segment comes after it, i.e. where the pre-pass left the clear
// value - which the depth bounds test [clear, clear] keeps, rejecting the rest before shading. Draws it would have
// painted over don't exist yet (nothing drawn there), and later non-opaque draws are not in the pre-pass depth, so
// what it shows under them is unchanged.
bool Device::BackdropCull(uint32_t fvf) const
{
    if (!m_depthBounds || !m_prepassArmed || m_prepassDepthTouched || !m_prepassFullClear || m_target != m_scene ||
        m_external || (fvf & d3d::FVF_POSITION_MASK) == d3d::FVF_XYZRHW)
        return false;
    if (m_rs[d3d::RS_ZENABLE] && (m_rs[d3d::RS_ZWRITEENABLE] || m_rs[d3d::RS_ZFUNC] != d3d::CMP_ALWAYS))
        return false;                            // tests (other than ALWAYS) or writes depth: not a backdrop
    return !WaterWritesDepth(fvf);
}

// May this draw go into the pre-pass (kPreIn), and if not, why. Opaque scene geometry drawn with the same vertex
// shader path the pre-pass takes - not characters (tessellated, skinned on the GPU), swaying plants, labels,
// particles or effects - while a segment is armed.
uint32_t Device::PrepassCheck(uint32_t primitive, uint32_t fvf, bool swaying) const
{
    if (!m_prepassArmed)
        return m_prepassEndedThisFrame ? kPreAfter : kPreBefore;
    if (m_drawTess || m_drawGpu || m_drawIsCharacter)
        return kPreCharacter;
    if (swaying)
        return kPreSway;
    if (m_target != m_scene || m_external || m_drawIsLabel || (fvf & d3d::FVF_POSITION_MASK) == d3d::FVF_XYZRHW ||
        TopologyClassOf(primitive) != 2 || (m_particlePending && fvf == kParticleFvf))
        return kPreKind;
    uint32_t zFunc = m_rs[d3d::RS_ZFUNC];
    if (!m_rs[d3d::RS_ZENABLE])
        return kPreNoZTest;
    if (!m_rs[d3d::RS_ZWRITEENABLE])
        return kPreNoZWrite;
    if (zFunc != d3d::CMP_LESS && zFunc != d3d::CMP_LESSEQUAL)
        return kPreZFunc;
    if (m_drawMayDiscard)
        return kPreDiscard;
    if (WaterWritesDepth(fvf))
        return kPreWater;
    if (!m_rs[d3d::RS_ALPHABLENDENABLE])
        return kPreIn;
    if (m_rs[d3d::RS_SRCBLEND] != d3d::BLEND_SRCALPHA || m_rs[d3d::RS_DESTBLEND] != d3d::BLEND_INVSRCALPHA)
        return kPreBlend;
    return AlphaOneCheck(fvf);
}

// Every 600 frames: how many draws went in, why the scene's other 3D draws didn't (a frame's average), and what ended
// the segments.
void Device::PrepassLog()
{
    static const char* const why[kPreWhyCount] = {
        "in", "before the depth clear", "after the segment", "characters", "swaying", "label/particle/other",
        "no depth test", "no depth write", "depth compare", "cut-out/alpha test", "water", "blend mode", "alpha: vertex colour", "alpha: material",
        "alpha: texture", "alpha: argument", "alpha: texture factor"};
    static const char* const end[kEndCount] = {"other", "scene end", "target switch", "copy", "read-back", "flush",
                                               "depth clear"};
    std::string out, ends;
    char buf[64];
    for (uint32_t i = 1; i < kPreWhyCount; ++i)
        if (m_prepassWhy[i]) {
            std::snprintf(buf, sizeof(buf), " %s %.0f |", why[i], double(m_prepassWhy[i]) / 600.0);
            out += buf;
        }
    for (uint32_t i = 0; i < kEndCount; ++i)
        if (m_prepassEndCause[i]) {
            std::snprintf(buf, sizeof(buf), " %s %llu |", end[i], (unsigned long long)m_prepassEndCause[i]);
            ends += buf;
        }
    if (!out.empty()) out.pop_back();
    if (!ends.empty()) ends.pop_back();
    Log("depth pre-pass: %.0f draws a frame in %llu segments (last 600 frames)%s", double(m_prepassDraws) / 600.0,
        (unsigned long long)m_prepassSegments, m_prepassPipeline ? "" : " - no pipeline");
    Log("depth pre-pass: draws ignoring depth, a frame: %.1f culled as backdrops | %.1f before the depth clear | %.1f "
        "after the segment | %.1f after depth was written | %.1f other%s",
        double(m_backdropDraws[0]) / 600.0, double(m_backdropDraws[1]) / 600.0, double(m_backdropDraws[2]) / 600.0,
        double(m_backdropDraws[3]) / 600.0, double(m_backdropDraws[4]) / 600.0,
        m_depthBounds ? "" : " (no depth bounds test on this GPU)");
    Log("depth pre-pass: scene draws kept out, a frame:%s; segments ended by:%s", out.c_str(), ends.c_str());
    m_prepassDraws = m_prepassSegments = 0;
    std::memset(m_backdropDraws, 0, sizeof(m_backdropDraws));
    std::memset(m_prepassWhy, 0, sizeof(m_prepassWhy));
    std::memset(m_prepassEndCause, 0, sizeof(m_prepassEndCause));
}

// The scene's depth clear: end the main commands here (mainA), start the pre-pass with the clear, and go on recording
// the main commands in mainB. False (nothing changed) when the pre-pass can't start: the caller clears as usual.
bool Device::PrepassArm(const VkClearRect* rects, uint32_t count, float z)
{
    Frame& f = m_frames[m_frameIndex];
    if (!m_prepassOn || !m_prepassPipeline || !f.prepass || !f.mainB || f.split || !m_inFrame || !m_rendering ||
        !m_scenePhase || m_target != m_scene || !m_depth || !count)
        return false;
    EndRendering();                              // mainA's pending draws, its rendering and queries end here
    vkEndCommandBuffer(f.main);
    VkCommandBufferBeginInfo b{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    b.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f.prepass, &b);
    vkBeginCommandBuffer(f.mainB, &b);
    f.main = f.mainB;
    f.split = true;
    // Fresh command buffers: no bound state, nothing pushed.
    m_cache = StateCache{};
    m_arenaBound = m_bindlessBound = m_shadowArenaBound = false;

    // The pre-pass: after mainA's depth work (the depth stays an attachment), the clear.
    VkCommandBuffer cmd = f.prepass;
    const VkPipelineStageFlags2 tests =
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    const VkAccessFlags2 depthRW =
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    ImageBarrier(cmd, m_depth, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, tests, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, tests,
                 depthRW);
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = m_depthView;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {m_scene->m_width, m_scene->m_height}};
    ri.layerCount = 1;
    ri.pDepthAttachment = &depth;
    vkCmdBeginRendering(cmd, &ri);
    VkClearAttachment att{VK_IMAGE_ASPECT_DEPTH_BIT, 0, {}};
    att.clearValue.depthStencil = {z, 0};
    vkCmdClearAttachments(cmd, 1, &att, count, rects);
    m_pre = PrepassCache{};
    m_pre.depth = m_depth;
    m_prepassArmed = true;
    m_prepassDepthTouched = false;
    m_prepassClearZ = z;
    m_prepassFullClear = count == 1 && rects[0].rect.offset.x == 0 && rects[0].rect.offset.y == 0 &&
                         rects[0].rect.extent.width >= m_scene->m_width && rects[0].rect.extent.height >= m_scene->m_height;
    ++m_prepassSegments;

    // mainB: the profile's interval ending here holds the pre-pass (it runs right before); the scene goes on.
    ProfileMark("depth pre-pass");
    BeginRenderingOn(m_scene);
    return true;
}

// The segment ends: the pre-pass is complete, its depth made visible to the tests of the main commands after it.
void Device::PrepassEnd(uint32_t cause)
{
    if (!m_prepassArmed)
        return;
    m_prepassArmed = false;
    m_prepassEndedThisFrame = true;
    ++m_prepassEndCause[cause < kEndCount ? cause : kEndOther];
    VkCommandBuffer cmd = m_frames[m_frameIndex].prepass;
    vkCmdEndRendering(cmd);
    const VkPipelineStageFlags2 tests =
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    ImageBarrier(cmd, m_pre.depth, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, tests, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, tests,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    vkEndCommandBuffer(cmd);
}

// One draw's depth into the pre-pass: the same vertex shader, record, geometry and descriptors as its main draw.
void Device::PrepassDraw(uint32_t primitive, uint32_t fvf, uint32_t stride, uint32_t vertexCount, uint32_t indexCount,
                         VkDeviceSize vbOffset, VkDeviceSize ibOffset, VkBuffer vb, VkBuffer ib,
                         VkDeviceSize frameLightsOffset, VkBuffer prevBuffer, VkDeviceSize prevOffset,
                         VkDeviceSize prevBytes, VkBuffer smoothBuffer, VkDeviceSize smoothOffset, VkDeviceSize smoothBytes,
                         uint32_t recordIndex)
{
    Frame& f = m_frames[m_frameIndex];
    VkCommandBuffer cmd = f.prepass;
    PrepassCache& c = m_pre;
    if (!c.bound) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_prepassPipeline);
        vkCmdSetFrontFace(cmd, VK_FRONT_FACE_CLOCKWISE);
        vkCmdSetDepthTestEnable(cmd, VK_TRUE);
        vkCmdSetDepthWriteEnable(cmd, VK_TRUE);
        vkCmdSetDepthCompareOp(cmd, VK_COMPARE_OP_LESS_OR_EQUAL);
        // The frame's arrays (0 = constants, 12 = records) as the main pass has them, and set 1 (sway.glsl's texture).
        VkDescriptorBufferInfo consts{f.ring, m_constsBase, VkDeviceSize(m_constCapacity) * sizeof(DrawConstants)};
        VkDescriptorBufferInfo records{f.ring, m_recordsBase, VkDeviceSize(m_recordCapacity) * sizeof(DrawRecord)};
        VkWriteDescriptorSet w[2] = {};
        for (int i = 0; i < 2; ++i) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].descriptorCount = 1;
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        }
        w[0].dstBinding = 0;
        w[0].pBufferInfo = &consts;
        w[1].dstBinding = 12;
        w[1].pBufferInfo = &records;
        vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 2, w);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 1, 1, &m_bindlessSet, 0, nullptr);
        c.bound = true;
    }
    if (c.topology != primitive) {
        vkCmdSetPrimitiveTopology(cmd, TopologyOf(primitive));
        c.topology = primitive;
    }
    VkViewport vp;
    VkRect2D scissor;
    ViewportState(&vp, &scissor);
    if (std::memcmp(&vp, &c.viewport, sizeof(vp)) != 0) {
        vkCmdSetViewport(cmd, 0, 1, &vp);
        c.viewport = vp;
    }
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
    if (c.fvf != fvf) {
        SetVertexInput(cmd, fvf, stride);
        c.fvf = fvf;
    }
    if (c.vb != vb) {
        VkBuffer buffers[2] = {vb, m_nullBuffer};
        VkDeviceSize offsets[2] = {0, 0};
        vkCmdBindVertexBuffers(cmd, 0, 2, buffers, offsets);
        c.vb = vb;
    }
    if (indexCount && c.ib != ib) {
        vkCmdBindIndexBuffer(cmd, ib, 0, VK_INDEX_TYPE_UINT16);
        c.ib = ib;
    }
    // The per-draw bindings the vertex shader reads (4 frame lights, 8 last positions, 10 smooth normals), as Draw
    // pushes them; for most static draws they don't change from one to the next.
    if (c.frameLights != frameLightsOffset || c.prevBuffer != prevBuffer || c.prevOffset != prevOffset ||
        c.smoothBuffer != smoothBuffer || c.smoothOffset != smoothOffset) {
        VkDescriptorBufferInfo frameLights{f.ring, frameLightsOffset, sizeof(FrameLights)};
        VkDescriptorBufferInfo prev{prevBuffer, prevOffset, prevBytes ? prevBytes : 16};
        VkDescriptorBufferInfo smooth{smoothBuffer, smoothOffset, smoothBytes ? smoothBytes : 16};
        VkWriteDescriptorSet w[3] = {};
        for (int i = 0; i < 3; ++i) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].descriptorCount = 1;
        }
        w[0].dstBinding = 4;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[0].pBufferInfo = &frameLights;
        w[1].dstBinding = 8;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[1].pBufferInfo = &prev;
        w[2].dstBinding = 10;
        w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[2].pBufferInfo = &smooth;
        vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 3, w);
        c.frameLights = frameLightsOffset;
        c.prevBuffer = prevBuffer;
        c.prevOffset = prevOffset;
        c.smoothBuffer = smoothBuffer;
        c.smoothOffset = smoothOffset;
    }
    if (indexCount)
        vkCmdDrawIndexed(cmd, indexCount, 1, uint32_t(ibOffset / 2), int32_t(vbOffset / stride), recordIndex);
    else
        vkCmdDraw(cmd, vertexCount, 1, uint32_t(vbOffset / stride), recordIndex);
    ++m_prepassDraws;
}

// What a submission of this frame slot runs after its uploads: the main commands, or, split, mainA, the pre-pass and
// mainB. The current main command buffer must already be ended; an armed pre-pass is ended here.
uint32_t Device::FrameCommands(Frame& f, VkCommandBuffer out[4])
{
    PrepassEnd(kEndFlush);
    if (!f.split) {
        out[0] = f.main;
        return 1;
    }
    out[0] = f.mainA;
    out[1] = f.prepass;
    out[2] = f.mainB;
    return 3;
}

// After the submission: one main command buffer again. The one being recorded stays the main one (mainA from now on),
// so a caller that holds f.main across a flush keeps recording into the right buffer.
void Device::FrameCommandsSubmitted(Frame& f)
{
    if (!f.split)
        return;
    std::swap(f.mainA, f.mainB);
    f.split = false;
}

}  // namespace rvk
