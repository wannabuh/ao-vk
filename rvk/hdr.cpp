// HDR (an enhancement): the 3D scene is drawn into a 16-bit float target, so light the game would clip at 1 - local
// lights given headroom, additive effects piling up - is kept, then tone mapped into the 8-bit main target. The
// interface is drawn after that, into the 8-bit target as before, so it never goes through the tone mapping.
//
// The switch: during the scene phase m_main *is* the float target (every "is this the main target" test keeps
// working); the first pre-transformed (interface) draw after the frame's first 3D draw ends the phase.
#include "internal.h"

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

}  // namespace

bool Device::CreateHdrResources(std::string* error)
{
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (!Check(vkCreateSampler(m_device, &si, nullptr, &m_pointSampler), "point sampler", error))
        return false;
    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    sl.bindingCount = 1;
    sl.pBindings = &binding;
    if (!Check(vkCreateDescriptorSetLayout(m_device, &sl, nullptr, &m_tonemapSetLayout), "tonemap set layout", error))
        return false;
    VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &m_tonemapSetLayout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &push;
    if (!Check(vkCreatePipelineLayout(m_device, &pl, nullptr, &m_tonemapLayout), "tonemap pipeline layout", error))
        return false;

    auto module = [&](const uint32_t* code, size_t size, VkShaderModule* out) {
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = size;
        mi.pCode = code;
        return Check(vkCreateShaderModule(m_device, &mi, nullptr, out), "tonemap shader module", error);
    };
    VkShaderModule vert, frag;
    if (!module(kFullscreenVertSpirv, sizeof(kFullscreenVertSpirv), &vert) ||
        !module(kTonemapFragSpirv, sizeof(kTonemapFragSpirv), &frag))
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
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &att;
    VkFormat colorFormat = kColorFormat;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &colorFormat;
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
    gi.layout = m_tonemapLayout;
    bool ok = Check(vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &gi, nullptr, &m_tonemapPipeline),
                    "tonemap pipeline", error);
    vkDestroyShaderModule(m_device, vert, nullptr);
    vkDestroyShaderModule(m_device, frag, nullptr);
    return ok;
}

void Device::DestroyHdrResources()
{
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
    VkDescriptorImageInfo image{m_pointSampler, m_scene->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &image;
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_tonemapLayout, 0, 1, &w);
    float params[4] = {m_tonemapKnee, m_exposure, 0.0f, 0.0f};
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

}  // namespace rvk
