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

    // Layouts: tone mapping reads the scene and the bloom; bloom passes read one image. Both push 16 bytes.
    auto setLayout = [&](uint32_t count, VkDescriptorSetLayout* out) {
        VkDescriptorSetLayoutBinding b[2] = {
            {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
        VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        sl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        sl.bindingCount = count;
        sl.pBindings = b;
        return Check(vkCreateDescriptorSetLayout(m_device, &sl, nullptr, out), "post set layout", error);
    };
    auto pipelineLayout = [&](VkDescriptorSetLayout set, VkPipelineLayout* out) {
        VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &set;
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges = &push;
        return Check(vkCreatePipelineLayout(m_device, &pl, nullptr, out), "post pipeline layout", error);
    };
    if (!setLayout(2, &m_tonemapSetLayout) || !pipelineLayout(m_tonemapSetLayout, &m_tonemapLayout) ||
        !setLayout(1, &m_bloomSetLayout) || !pipelineLayout(m_bloomSetLayout, &m_bloomLayout))
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
              fullscreen(kBloomUpSpirv, sizeof(kBloomUpSpirv), hdrFormat, true, m_bloomLayout, &m_bloomUp);
    vkDestroyShaderModule(m_device, vert, nullptr);
    return ok;
}

void Device::DestroyHdrResources()
{
    for (Texture* t : m_bloomLevels) DestroyTextureNow(t);
    m_bloomLevels.clear();
    for (VkPipeline* p : {&m_bloomDown, &m_bloomUp})
        if (*p) { vkDestroyPipeline(m_device, *p, nullptr); *p = VK_NULL_HANDLE; }
    if (m_bloomLayout) vkDestroyPipelineLayout(m_device, m_bloomLayout, nullptr);
    if (m_bloomSetLayout) vkDestroyDescriptorSetLayout(m_device, m_bloomSetLayout, nullptr);
    if (m_linearSampler) vkDestroySampler(m_device, m_linearSampler, nullptr);
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
    Transition(cmd, m_ldrMain, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = m_ldrMain->m_view;
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
    VkDescriptorImageInfo images[2] = {{m_pointSampler, m_scene->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                       {m_linearSampler, glow->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    for (uint32_t i = 0; i < 2; ++i) {
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[i].pImageInfo = &images[i];
    }
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_tonemapLayout, 0, 2, w);
    // The upsampled chain sums every level's light: averaged here.
    float params[4] = {m_tonemapKnee, m_exposure, bloom ? m_bloomStrength / float(m_bloomLevels.size()) : 0.0f, 0.0f};
    vkCmdPushConstants(cmd, m_tonemapLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(params), params);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    m_cache = StateCache{};                      // pipeline, viewport and scissor changed
    m_main = m_ldrMain;
    if (wasMain)
        m_target = m_ldrMain;
    m_constantsDirty = true;
    if (m_inFrame)
        BeginRenderingOn(m_target);
}

// One full-target pass: dst <- pipeline(src). load: keep dst's contents (blended onto) instead of overwriting them.
void Device::FullscreenPass(VkCommandBuffer cmd, Texture* dst, VkPipeline pipeline, VkPipelineLayout layout, Texture* src,
                            const float params[4], bool load)
{
    Transition(cmd, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
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
    VkDescriptorImageInfo image{m_linearSampler, src->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &image;
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &w);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, params);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
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
        FullscreenPass(cmd, m_bloomLevels[i], m_bloomDown, m_bloomLayout, src, params, false);
        src = m_bloomLevels[i];
    }
    for (size_t i = m_bloomLevels.size() - 1; i-- > 0;) {
        Texture* smaller = m_bloomLevels[i + 1];
        float params[4] = {1.0f / float(smaller->m_width), 1.0f / float(smaller->m_height), 0.0f, 0.0f};
        FullscreenPass(cmd, m_bloomLevels[i], m_bloomUp, m_bloomLayout, smaller, params, true);
    }
    Transition(cmd, m_bloomLevels[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

}  // namespace rvk
