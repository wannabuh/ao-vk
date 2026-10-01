// HDR (an enhancement): the 3D scene is drawn into a 16-bit float target, so light the game would clip at 1 - local
// lights given headroom, additive effects piling up - is kept, then tone mapped into the 8-bit main target. The
// interface is drawn after that, into the 8-bit target as before, so it never goes through the tone mapping.
//
// The switch: during the scene phase m_main *is* the float target (every "is this the main target" test keeps
// working); the first pre-transformed (interface) draw after the frame's first 3D draw ends the phase.
#include "internal.h"

#include <algorithm>
#include <cstring>

namespace rvk {

using namespace vk;
using namespace detail;

namespace {

const uint32_t kFullscreenVertSpirv[] = {
#include "fullscreen.vert.inc"
};
const uint32_t kTonemapFragSpirv[] = {
#include "tonemap.frag.inc"
};
const uint32_t kBloomDownSpirv[] = {
#include "bloom_down.frag.inc"
};
const uint32_t kBloomUpSpirv[] = {
#include "bloom_up.frag.inc"
};
const uint32_t kAoSpirv[] = {
#include "ao.frag.inc"
};
const uint32_t kAoBlurSpirv[] = {
#include "ao_blur.frag.inc"
};
const uint32_t kMotionBlurSpirv[] = {
#include "motion_blur.frag.inc"
};

}  // namespace

bool Device::CreateHdrResources(std::string* error)
{
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (!Check(vkCreateSampler(m_device, &si, nullptr, &m_pointSampler), "point sampler", error))
        return false;
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    if (!Check(vkCreateSampler(m_device, &si, nullptr, &m_linearSampler), "linear sampler", error))
        return false;
    // The ground's base texture read for its relief (F_BUMPBASE): tiled, mipmapped.
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.maxLod = VK_LOD_CLAMP_NONE;
    if (!Check(vkCreateSampler(m_device, &si, nullptr, &m_bumpSampler), "bump sampler", error))
        return false;

    // Layouts: tone mapping reads the scene and the bloom; bloom passes read one image. Both push 16 bytes.
    auto setLayout = [&](uint32_t count, VkDescriptorSetLayout* out) {
        VkDescriptorSetLayoutBinding b[5] = {
            {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
        VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        sl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        sl.bindingCount = count;
        sl.pBindings = b;
        return Check(vkCreateDescriptorSetLayout(m_device, &sl, nullptr, out), "post set layout", error);
    };
    auto pipelineLayout = [&](VkDescriptorSetLayout set, VkPipelineLayout* out) {
        VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &set;
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges = &push;
        return Check(vkCreatePipelineLayout(m_device, &pl, nullptr, out), "post pipeline layout", error);
    };
    if (!setLayout(5, &m_tonemapSetLayout) || !pipelineLayout(m_tonemapSetLayout, &m_tonemapLayout) ||
        !setLayout(2, &m_bloomSetLayout) || !pipelineLayout(m_bloomSetLayout, &m_bloomLayout))
        return false;

    auto module = [&](const uint32_t* code, size_t size, VkShaderModule* out) {
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = size;
        mi.pCode = code;
        return Check(vkCreateShaderModule(m_device, &mi, nullptr, out), "post shader module", error);
    };
    VkShaderModule vert;
    if (!module(kFullscreenVertSpirv, sizeof(kFullscreenVertSpirv), &vert))
        return false;
    // A full-target triangle with one fragment shader, no depth; additive blending for the bloom upsampling.
    auto fullscreen = [&](const uint32_t* code, size_t size, VkFormat format, bool additive, VkPipelineLayout layout,
                          VkPipeline* out) {
        VkShaderModule frag;
        if (!module(code, size, &frag))
            return false;
        VkPipelineShaderStageCreateInfo stages[2] = {
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vert, "main", nullptr},
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main", nullptr},
        };
        VkDynamicState dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        ds.dynamicStateCount = 2;
        ds.pDynamicStates = dynamic;
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo dss{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        VkPipelineColorBlendAttachmentState att{};
        att.colorWriteMask = 0xF;
        if (additive) {
            att.blendEnable = VK_TRUE;
            att.srcColorBlendFactor = att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            att.srcAlphaBlendFactor = att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            att.colorBlendOp = att.alphaBlendOp = VK_BLEND_OP_ADD;
        }
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1;
        cb.pAttachments = &att;
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &format;
        VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gi.pNext = &rendering;
        gi.stageCount = 2;
        gi.pStages = stages;
        gi.pVertexInputState = &vi;
        gi.pInputAssemblyState = &ia;
        gi.pViewportState = &vp;
        gi.pRasterizationState = &rs;
        gi.pMultisampleState = &ms;
        gi.pDepthStencilState = &dss;
        gi.pColorBlendState = &cb;
        gi.pDynamicState = &ds;
        gi.layout = layout;
        bool ok = Check(vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &gi, nullptr, out), "post pipeline", error);
        vkDestroyShaderModule(m_device, frag, nullptr);
        return ok;
    };
    VkFormat hdrFormat = GetFormatInfo(Format::RGBA16F).vk;
    bool ok = fullscreen(kTonemapFragSpirv, sizeof(kTonemapFragSpirv), kColorFormat, false, m_tonemapLayout, &m_tonemapPipeline) &&
              fullscreen(kBloomDownSpirv, sizeof(kBloomDownSpirv), hdrFormat, false, m_bloomLayout, &m_bloomDown) &&
              fullscreen(kBloomUpSpirv, sizeof(kBloomUpSpirv), hdrFormat, true, m_bloomLayout, &m_bloomUp) &&
              fullscreen(kAoSpirv, sizeof(kAoSpirv), GetFormatInfo(Format::RG16F).vk, false, m_bloomLayout, &m_aoPipeline) &&
              fullscreen(kAoBlurSpirv, sizeof(kAoBlurSpirv), GetFormatInfo(Format::RG16F).vk, false, m_bloomLayout,
                         &m_aoBlurPipeline) &&
              fullscreen(kMotionBlurSpirv, sizeof(kMotionBlurSpirv), kColorFormat, false, m_bloomLayout, &m_motionPipeline);
    vkDestroyShaderModule(m_device, vert, nullptr);
    return ok;
}

void Device::DestroyHdrResources()
{
    for (Texture* t : m_bloomLevels) DestroyTextureNow(t);
    m_bloomLevels.clear();
    for (Texture*& t : m_aoTex)
        if (t) { DestroyTextureNow(t); t = nullptr; }
    if (m_tonemapped) { DestroyTextureNow(m_tonemapped); m_tonemapped = nullptr; }
    for (VkPipeline* p : {&m_bloomDown, &m_bloomUp, &m_aoPipeline, &m_aoBlurPipeline, &m_motionPipeline})
        if (*p) { vkDestroyPipeline(m_device, *p, nullptr); *p = VK_NULL_HANDLE; }
    if (m_bloomLayout) vkDestroyPipelineLayout(m_device, m_bloomLayout, nullptr);
    if (m_bloomSetLayout) vkDestroyDescriptorSetLayout(m_device, m_bloomSetLayout, nullptr);
    if (m_linearSampler) vkDestroySampler(m_device, m_linearSampler, nullptr);
    if (m_bumpSampler) vkDestroySampler(m_device, m_bumpSampler, nullptr);
    m_bumpSampler = VK_NULL_HANDLE;
    m_bloomLayout = VK_NULL_HANDLE;
    m_bloomSetLayout = VK_NULL_HANDLE;
    m_linearSampler = VK_NULL_HANDLE;
    if (m_tonemapPipeline) vkDestroyPipeline(m_device, m_tonemapPipeline, nullptr);
    if (m_tonemapLayout) vkDestroyPipelineLayout(m_device, m_tonemapLayout, nullptr);
    if (m_tonemapSetLayout) vkDestroyDescriptorSetLayout(m_device, m_tonemapSetLayout, nullptr);
    if (m_pointSampler) vkDestroySampler(m_device, m_pointSampler, nullptr);
    m_tonemapPipeline = VK_NULL_HANDLE;
    m_tonemapLayout = VK_NULL_HANDLE;
    m_tonemapSetLayout = VK_NULL_HANDLE;
    m_pointSampler = VK_NULL_HANDLE;
}

// Frame start (before rendering begins): with HDR the main target is the float scene until EndScene.
void Device::BeginScene()
{
    m_sceneSaw3D = false;
    m_aoProjValid = false;
    m_glowCleared = false;
    m_glowDraws = 0;
    m_sceneEndDraw = 0;
    m_sceneEndFvf = 0;
    m_scenePhase = m_hdr && m_scene && m_ldrMain;
    Texture* main = m_scenePhase ? m_scene : m_ldrMain;
    if (m_target == m_main)
        m_target = main;
    m_main = main;
}

// Tone maps the scene into the 8-bit main target, which is the main target from here on. Called at the frame's
// first interface draw, before a read-back of the main target, and at the end of the frame.
void Device::EndScene()
{
    if (!m_scenePhase)
        return;
    m_scenePhase = false;
    VkCommandBuffer cmd = m_frames[m_frameIndex].main;
    bool wasMain = m_target == m_scene;
    EndRendering();
    Transition(cmd, m_scene, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    bool bloom = m_bloomStrength > 0.0f;
    if (bloom)
        RenderBloom(cmd);
    bool ao = RenderAo(cmd);
    Transition(cmd, m_localFraction, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    // With motion blur the tone mapping goes to an intermediate image, which the blur reads into the main target.
    float motion[24];
    bool blur = MotionBlurParams(motion);
    if (blur && (!m_tonemapped || m_tonemapped->m_width != m_ldrMain->m_width || m_tonemapped->m_height != m_ldrMain->m_height)) {
        if (m_tonemapped) DestroyTexture(m_tonemapped);
        m_tonemapped = CreateImage(m_ldrMain->m_width, m_ldrMain->m_height, Format::A8R8G8B8, 1, true);
        blur = m_tonemapped != nullptr;
    }
    Texture* toned = blur ? m_tonemapped : m_ldrMain;
    Transition(cmd, toned, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = toned->m_view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {m_ldrMain->m_width, m_ldrMain->m_height}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &ri);
    VkViewport viewport{0.0f, 0.0f, float(m_ldrMain->m_width), float(m_ldrMain->m_height), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {m_ldrMain->m_width, m_ldrMain->m_height}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_tonemapPipeline);
    Texture* glow = bloom ? m_bloomLevels[0] : m_scene;          // unread when the strength is 0
    Texture* occlusion = ao ? m_aoTex[1] : m_scene;               // unread when off
    // The depth buffer is readable after the AO pass; without AO, any readable image stands in (unread).
    VkImageView depthView = ao ? m_depthView : m_scene->m_view;
    VkDescriptorImageInfo images[5] = {{m_pointSampler, m_scene->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                       {m_linearSampler, glow->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                       {m_pointSampler, occlusion->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                       {m_pointSampler, depthView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                       {m_pointSampler, m_localFraction->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet w[5] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},
                                 {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},
                                 {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    for (uint32_t i = 0; i < 5; ++i) {
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[i].pImageInfo = &images[i];
    }
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_tonemapLayout, 0, 5, w);
    // The upsampled chain sums every level's light: averaged here.
    float params[8] = {m_tonemapKnee, m_exposure, bloom ? m_bloomStrength / float(m_bloomLevels.size()) : 0.0f,
                       ao ? 1.0f : 0.0f, m_aoProj.m[2][2], m_aoProj.m[3][2], 0.0f, 0.0f};
    vkCmdPushConstants(cmd, m_tonemapLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(params), params);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    if (blur) {
        Transition(cmd, m_tonemapped, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (m_depthLayout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {   // the AO pass didn't make it readable
            ImageBarrier(cmd, m_depth, VK_IMAGE_ASPECT_DEPTH_BIT, m_depthLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                         VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            m_depthLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        FullscreenPass(cmd, m_ldrMain, m_motionPipeline, m_tonemapped->m_view, m_depthView, m_pointSampler, motion,
                       sizeof(motion), false);
    }
    m_cache = StateCache{};                      // pipeline, viewport and scissor changed
    m_main = m_ldrMain;
    if (wasMain)
        m_target = m_ldrMain;
    m_constantsDirty = true;
    if (m_inFrame)
        BeginRenderingOn(m_target);
}

// One full-target pass: dst <- pipeline(src, src2). load: keep dst's contents (blended onto) instead of overwriting.
void Device::FullscreenPass(VkCommandBuffer cmd, Texture* dst, VkPipeline pipeline, VkImageView src, VkImageView src2,
                            VkSampler sampler, const float* params, uint32_t paramBytes, bool load)
{
    Transition(cmd, dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = dst->m_view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {dst->m_width, dst->m_height}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &ri);
    VkViewport viewport{0.0f, 0.0f, float(dst->m_width), float(dst->m_height), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {dst->m_width, dst->m_height}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    VkDescriptorImageInfo images[2] = {{sampler, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                       {sampler, src2, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    for (uint32_t i = 0; i < 2; ++i) {
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[i].pImageInfo = &images[i];
    }
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_bloomLayout, 0, 2, w);
    vkCmdPushConstants(cmd, m_bloomLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, paramBytes, params);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

// Ambient occlusion from the scene's depth buffer, at half resolution, blurred both ways; m_aoTex[1] holds it.
// Needs the world camera's (perspective) projection, seen at the frame's first depth-writing 3D draw.
bool Device::RenderAo(VkCommandBuffer cmd)
{
    if (m_aoStrength <= 0.0f || !m_aoProjValid || m_aoProj.m[2][3] != 1.0f || m_aoProj.m[3][3] != 0.0f || !m_depth)
        return false;
    uint32_t w = std::max(1u, m_scene->m_width / 2), h = std::max(1u, m_scene->m_height / 2);
    if (!m_aoTex[0] || m_aoTex[0]->m_width != w || m_aoTex[0]->m_height != h) {
        for (Texture*& t : m_aoTex) {
            if (t) DestroyTexture(t);
            t = CreateImage(w, h, Format::RG16F, 1, true);
        }
        if (!m_aoTex[0] || !m_aoTex[1])
            return false;
    }
    // The depth buffer, written by the scene, read by the AO pass; BeginRenderingOn makes it an attachment again.
    ImageBarrier(cmd, m_depth, VK_IMAGE_ASPECT_DEPTH_BIT, m_depthLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    m_depthLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    float params[8] = {m_aoProj.m[2][2], m_aoProj.m[3][2], m_aoProj.m[0][0], m_aoProj.m[1][1],
                       m_aoRadius, m_aoStrength, float(m_scene->m_width), float(m_scene->m_height)};
    FullscreenPass(cmd, m_aoTex[0], m_aoPipeline, m_depthView, m_depthView, m_pointSampler, params, sizeof(params), false);
    for (int pass = 0; pass < 2; ++pass) {
        Texture* src = m_aoTex[pass == 0 ? 0 : 1];
        Texture* dst = m_aoTex[pass == 0 ? 1 : 0];
        Transition(cmd, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        float dir[4] = {pass == 0 ? 1.0f : 0.0f, pass == 0 ? 0.0f : 1.0f, 0.0f, 0.0f};
        FullscreenPass(cmd, dst, m_aoBlurPipeline, src->m_view, src->m_view, m_pointSampler, dir, sizeof(dir), false);
    }
    // The second blur pass wrote m_aoTex[0]: swap so that [1] holds the result, as the tone mapping expects.
    std::swap(m_aoTex[0], m_aoTex[1]);
    Transition(cmd, m_aoTex[1], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return true;
}

// Bloom: the scene's light above the threshold, downsampled level by level from half resolution to ~16 pixels,
// then upsampled back, each level adding the blurred smaller one. m_bloomLevels[0] then holds the glow.
void Device::RenderBloom(VkCommandBuffer cmd)
{
    uint32_t w = std::max(1u, m_scene->m_width / 2), h = std::max(1u, m_scene->m_height / 2);
    if (m_bloomLevels.empty() || m_bloomLevels[0]->m_width != w || m_bloomLevels[0]->m_height != h) {
        for (Texture* t : m_bloomLevels) DestroyTexture(t);
        m_bloomLevels.clear();
        for (uint32_t lw = w, lh = h; m_bloomLevels.size() < 7 && lw >= 16 && lh >= 16; lw /= 2, lh /= 2) {
            Texture* t = CreateImage(lw, lh, Format::RGBA16F, 1, true);
            if (!t) break;
            m_bloomLevels.push_back(t);
        }
        if (m_bloomLevels.empty())
            return;
    }
    Texture* src = m_scene;
    for (size_t i = 0; i < m_bloomLevels.size(); ++i) {
        float params[4] = {1.0f / float(src->m_width), 1.0f / float(src->m_height), m_bloomThreshold, i == 0 ? 1.0f : 0.0f};
        // The first pass adds the glow (additive effects) to the scene's light above the threshold.
        Texture* src2 = i == 0 ? m_glow : src;
        Transition(cmd, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        Transition(cmd, src2, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        FullscreenPass(cmd, m_bloomLevels[i], m_bloomDown, src->m_view, src2->m_view, m_linearSampler, params, 16, false);
        src = m_bloomLevels[i];
    }
    for (size_t i = m_bloomLevels.size() - 1; i-- > 0;) {
        Texture* smaller = m_bloomLevels[i + 1];
        float params[4] = {1.0f / float(smaller->m_width), 1.0f / float(smaller->m_height), 0.0f, 0.0f};
        Transition(cmd, smaller, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        FullscreenPass(cmd, m_bloomLevels[i], m_bloomUp, smaller->m_view, smaller->m_view, m_linearSampler, params, 16, true);
    }
    Transition(cmd, m_bloomLevels[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

// This frame's camera against the last one's: the reprojection matrix (current clip -> previous clip) and the blur
// parameters for motion_blur.frag. Remembers this frame's camera for the next.
bool Device::MotionBlurParams(float out[24])
{
    double now = SwayClock(), dt = now - m_prevSceneTime;
    m_prevSceneTime = now;
    bool perspective = m_aoProjValid && m_aoProj.m[2][3] == 1.0f && m_aoProj.m[3][3] == 0.0f;
    if (!perspective) {
        m_prevViewProjValid = false;
        return false;
    }
    d3d::Matrix viewProj = MulMatrix(m_aoView, m_aoProj);
    const auto& v = m_aoView.m;
    float eye[3];
    for (int i = 0; i < 3; ++i) eye[i] = -(v[3][0] * v[i][0] + v[3][1] * v[i][1] + v[3][2] * v[i][2]);
    bool have = m_prevViewProjValid && m_motionBlur > 0.0f && m_depth;
    float jump2 = 0.0f;
    for (int i = 0; i < 3; ++i) jump2 += (eye[i] - m_prevEye[i]) * (eye[i] - m_prevEye[i]);
    d3d::Matrix inverse, reproject;
    if (have && (jump2 > 25.0f || !InvertMatrix(viewProj, &inverse)))   // a teleport / zone change: no blur
        have = false;
    if (have) {
        reproject = MulMatrix(inverse, m_prevViewProj);
        std::memcpy(out, &reproject, 64);
        dt = std::clamp(dt, 1.0 / 500.0, 0.25);
        out[16] = float(m_motionBlur / 60.0 / dt);        // per-frame motion -> motion during the exposure
        out[17] = 0.04f * float(m_scene->m_width);        // at most 4% of the screen width
        out[18] = m_motionNear;
        out[19] = m_motionNear * 1.5f;
        out[20] = m_aoProj.m[2][2];
        out[21] = m_aoProj.m[3][2];
        out[22] = float(m_scene->m_width);
        out[23] = float(m_scene->m_height);
    }
    m_prevViewProj = viewProj;
    std::memcpy(m_prevEye, eye, sizeof(eye));
    m_prevViewProjValid = true;
    return have;
}

}  // namespace rvk
