// The interface layer (docs/rvk.md "Interface refresh rate"): the game's interface (GUI.dll's windows) drawn into an
// image of its own, premultiplied by its coverage, and blended over the main target every frame - so frames that
// don't redraw it (the game skips GUI.dll's drawing, rnative gui.cpp) still show it.
//
// In the layer every draw blends, its colour as the game asked and its alpha as coverage: what a sequence of blended
// draws over a transparent image leaves is exactly what they would have left over the frame, once the layer is put
// over the frame with (ONE, INV_SRC_ALPHA) - for the interface's alpha-blended and additive draws. (An unblended
// draw keeps its texture's alpha as coverage; one multiplying the frame - DESTCOLOR - is approximated.)
#include "internal.h"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstring>

namespace rvk {

using namespace vk;
using namespace detail;

namespace {

const uint32_t kUiFullscreenVertSpirv[] = {
#include "fullscreen.vert.inc"
};
const uint32_t kUiShapeFragSpirv[] = {
#include "ui_shape.frag.inc"
};
const uint32_t kUiImageFragSpirv[] = {
#include "ui_image.frag.inc"
};
const uint32_t kUiMeshVertSpirv[] = {
#include "ui_mesh.vert.inc"
};
const uint32_t kUiMeshFragSpirv[] = {
#include "ui_mesh.frag.inc"
};

}  // namespace

void Device::InterfaceSceneEnd()
{
    // The interface's first draw ends the 3D scene (Draw): particles the game no longer draws go first, then (HDR) the
    // scene is tone mapped into the 8-bit target the interface goes over. A frame that doesn't redraw the interface
    // has no such draw.
    if (!m_external && m_target == m_main && m_particleSaw3D && !m_particleOrphansDone) {
        m_particleOrphanTrigger = "the interface";
        DrawOrphanParticles();
    }
    if (m_scenePhase && m_target == m_scene && m_sceneSaw3D) {
        m_sceneEndDraw = m_dumpDraw;
        m_sceneEndFvf = 0;
        EndScene();
    }
}

void Device::InterfaceBegin(bool redraw)
{
    if (!m_inFrame || !m_main)
        return;
    InterfaceSceneEnd();
    // Only over the 8-bit main target (the layer's format; HDR: after the scene's tone mapping). Otherwise - a frame
    // without 3D still in the float scene - the interface is drawn straight into the target, and is redrawn each
    // frame (not ready).
    if (m_main->m_format != Format::A8R8G8B8) {
        m_uiLayerReady.store(false, std::memory_order_relaxed);
        return;
    }
    m_uiLayerOpen = true;
    if (!redraw)
        return;
    if (!m_uiLayer || m_uiLayer->m_width != m_main->m_width || m_uiLayer->m_height != m_main->m_height) {
        if (m_uiLayer) DestroyTexture(m_uiLayer);
        m_uiLayer = CreateImage(m_main->m_width, m_main->m_height, Format::A8R8G8B8, 1, true);
        m_uiLayerReady.store(false, std::memory_order_relaxed);
        if (!m_uiLayer)
            return;
    }
    m_glassRects.clear();                        // this redraw's windows follow (InterfaceWindow)
    m_uiLayerReturn = m_target;
    m_uiLayerActive = true;
    SetRenderTarget(m_uiLayer);                  // (ends the rendering on the main target, begins one on the layer)
    m_cache = StateCache{};                      // the layer's blend equations differ (ApplyDynamicState)
    VkClearAttachment clear{VK_IMAGE_ASPECT_COLOR_BIT, 0, {}};
    clear.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    VkClearRect rect{{{0, 0}, {m_uiLayer->m_width, m_uiLayer->m_height}}, 0, 1};
    vkCmdClearAttachments(m_frames[m_frameIndex].main, 1, &clear, 1, &rect);
}

void Device::InterfaceEnd()
{
    if (!m_inFrame || !m_uiLayerOpen)
        return;
    m_uiLayerOpen = false;
    VkCommandBuffer cmd = m_frames[m_frameIndex].main;
    if (m_uiLayerActive) {
        m_uiLayerActive = false;
        m_renderEndCause = kEndTarget;
        EndRendering();
        Texture* back = m_uiLayerReturn && m_uiLayerReturn != m_uiLayer ? m_uiLayerReturn : m_main;
        m_target = back;
        m_viewport = {0, 0, back->m_width, back->m_height, 0.0f, 1.0f};
        m_uiLayerReady.store(true, std::memory_order_relaxed);
    } else {
        m_renderEndCause = kEndTarget;
        EndRendering();
    }
    // The layer over the main target - this frame's interface, or the last one drawn - with the glass under it.
    if (m_uiLayer && m_uiCompositePipeline && m_uiLayerReady.load(std::memory_order_relaxed) && !m_hideInterface &&
        m_main->m_format == Format::A8R8G8B8 && m_uiLayer->m_width == m_main->m_width &&
        m_uiLayer->m_height == m_main->m_height) {
        RenderGlass(cmd);
        Transition(cmd, m_uiLayer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        const float params[4] = {};
        FullscreenPass(cmd, m_main, m_uiCompositePipeline, m_uiLayer->m_view, m_uiLayer->m_view, m_pointSampler, params,
                       sizeof(params), true);
    }
    // The pass pushed set 0 with its own layout and changed the pipeline: the next draw binds again (as EndScene).
    m_cache = StateCache{};
    m_arenaBound = false;
    m_bindlessBound = false;
    m_constantsDirty = true;
    BeginRenderingOn(m_target);
}

void Device::InterfaceWindow(float x0, float y0, float x1, float y1)
{
    if (!m_uiLayerActive || !m_main)
        return;
    x0 = std::max(x0, 0.0f), y0 = std::max(y0, 0.0f);
    x1 = std::min(x1, float(m_main->m_width)), y1 = std::min(y1, float(m_main->m_height));
    if (x1 - x0 >= 2.0f && y1 - y0 >= 2.0f)
        m_glassRects.push_back({x0, y0, x1, y1});
}

// The interface back end's shape pipelines: a full-target triangle cut down by the scissor, ui_shape.frag, drawn inside
// the interface's rendering (one colour target and the depth attachment) - the colour pass (depth test LESS, no write,
// premultiplied colour and coverage: ONE, INV_SRC_ALPHA), the depth pass (kDepthOnly: depth written, no colour) and
// the glow pass (as the colour pass, LESS_OR_EQUAL).
bool Device::CreateUiShapePipeline()
{
    if (m_uiShapePipeline && m_uiShapeDepthPipeline && m_uiShapeGlowPipeline && m_uiImagePipeline && m_uiMeshPipeline)
        return true;
    if (m_uiShapeFailed)
        return false;
    m_uiShapeFailed = true;                      // (until they are made)
    std::string error;
    VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 96};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &push;
    if (!m_uiShapeLayout &&
        !Check(vkCreatePipelineLayout(m_device, &pl, nullptr, &m_uiShapeLayout), "interface shape layout", &error))
        return false;
    VkShaderModule modules[2] = {};
    const uint32_t* code[2] = {kUiFullscreenVertSpirv, kUiShapeFragSpirv};
    size_t sizes[2] = {sizeof(kUiFullscreenVertSpirv), sizeof(kUiShapeFragSpirv)};
    for (int i = 0; i < 2; ++i) {
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = sizes[i];
        mi.pCode = code[i];
        if (!Check(vkCreateShaderModule(m_device, &mi, nullptr, &modules[i]), "interface shape shader", &error)) {
            for (VkShaderModule m : modules)
                if (m) vkDestroyShaderModule(m_device, m, nullptr);
            return false;
        }
    }
    bool ok = true;
    for (int kind = 0; kind < 3 && ok; ++kind) {             // colour, depth, glow
        const int depthOnly = kind == 1;
        const VkBool32 spec = depthOnly ? VK_TRUE : VK_FALSE;
        VkSpecializationMapEntry entry{0, 0, sizeof(VkBool32)};
        VkSpecializationInfo si{1, &entry, sizeof(spec), &spec};
        VkPipelineShaderStageCreateInfo stages[2] = {
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, modules[0],
             "main", nullptr},
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, modules[1],
             "main", &si},
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
        dss.depthTestEnable = VK_TRUE;
        dss.depthWriteEnable = depthOnly ? VK_TRUE : VK_FALSE;
        dss.depthCompareOp = kind == 2 ? VK_COMPARE_OP_LESS_OR_EQUAL : VK_COMPARE_OP_LESS;
        VkPipelineColorBlendAttachmentState att{};
        att.colorWriteMask = depthOnly ? 0u : 0xFu;
        att.blendEnable = depthOnly ? VK_FALSE : VK_TRUE;
        att.srcColorBlendFactor = att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        att.dstColorBlendFactor = att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        att.colorBlendOp = att.alphaBlendOp = VK_BLEND_OP_ADD;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1;
        cb.pAttachments = &att;
        VkFormat colorFormat = kColorFormat;
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &colorFormat;
        rendering.depthAttachmentFormat = kDepthFormat;
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
        gi.layout = m_uiShapeLayout;
        VkPipeline* out = kind == 0 ? &m_uiShapePipeline : kind == 1 ? &m_uiShapeDepthPipeline : &m_uiShapeGlowPipeline;
        ok = Check(vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &gi, nullptr, out),
                   "interface shape pipeline", &error);
    }
    // The image pipeline: its own layout (one combined image sampler by push descriptor, 64 bytes of push constants),
    // ui_image.frag, as the glow pass (depth tested LESS_OR_EQUAL - over the window's shapes - not written,
    // premultiplied over; the caller keeps it off windows in front).
    VkShaderModule imageFrag = VK_NULL_HANDLE;
    if (ok) {
        VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                                       nullptr};
        VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        sl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        sl.bindingCount = 1;
        sl.pBindings = &b;
        VkPushConstantRange range{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64};
        VkPipelineLayoutCreateInfo il{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        il.setLayoutCount = 1;
        il.pSetLayouts = &m_uiImageSetLayout;
        il.pushConstantRangeCount = 1;
        il.pPushConstantRanges = &range;
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = sizeof(kUiImageFragSpirv);
        mi.pCode = kUiImageFragSpirv;
        ok = (m_uiImageSetLayout ||
              Check(vkCreateDescriptorSetLayout(m_device, &sl, nullptr, &m_uiImageSetLayout), "interface image set",
                    &error)) &&
             (m_uiImageLayout ||
              Check(vkCreatePipelineLayout(m_device, &il, nullptr, &m_uiImageLayout), "interface image layout", &error)) &&
             Check(vkCreateShaderModule(m_device, &mi, nullptr, &imageFrag), "interface image shader", &error);
    }
    if (ok) {
        VkPipelineShaderStageCreateInfo stages[2] = {
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, modules[0],
             "main", nullptr},
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, imageFrag,
             "main", nullptr},
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
        dss.depthTestEnable = VK_TRUE;
        dss.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        VkPipelineColorBlendAttachmentState att{};
        att.colorWriteMask = 0xF;
        att.blendEnable = VK_TRUE;
        att.srcColorBlendFactor = att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        att.dstColorBlendFactor = att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        att.colorBlendOp = att.alphaBlendOp = VK_BLEND_OP_ADD;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1;
        cb.pAttachments = &att;
        VkFormat colorFormat = kColorFormat;
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &colorFormat;
        rendering.depthAttachmentFormat = kDepthFormat;
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
        gi.layout = m_uiImageLayout;
        ok = Check(vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &gi, nullptr, &m_uiImagePipeline),
                   "interface image pipeline", &error);
    }
    if (imageFrag)
        vkDestroyShaderModule(m_device, imageFrag, nullptr);
    // The mesh pipeline: RmlUi's vertices (UiVertex), the image's set layout, 32 bytes of vertex push constants;
    // depth tested LESS (behind windows in front, over the window's shapes - their depth comes after), not written.
    if (ok && !m_uiMeshPipeline) {
        VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT, 0, 32};
        VkPipelineLayoutCreateInfo ml{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        ml.setLayoutCount = 1;
        ml.pSetLayouts = &m_uiImageSetLayout;
        ml.pushConstantRangeCount = 1;
        ml.pPushConstantRanges = &range;
        ok = m_uiMeshLayout ||
             Check(vkCreatePipelineLayout(m_device, &ml, nullptr, &m_uiMeshLayout), "interface mesh layout", &error);
        VkShaderModule meshModules[2] = {};
        const uint32_t* meshCode[2] = {kUiMeshVertSpirv, kUiMeshFragSpirv};
        size_t meshSizes[2] = {sizeof(kUiMeshVertSpirv), sizeof(kUiMeshFragSpirv)};
        for (int i = 0; i < 2 && ok; ++i) {
            VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            mi.codeSize = meshSizes[i];
            mi.pCode = meshCode[i];
            ok = Check(vkCreateShaderModule(m_device, &mi, nullptr, &meshModules[i]), "interface mesh shader", &error);
        }
        if (ok) {
            VkPipelineShaderStageCreateInfo stages[2] = {
                {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,
                 meshModules[0], "main", nullptr},
                {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT,
                 meshModules[1], "main", nullptr},
            };
            VkDynamicState dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
            VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
            ds.dynamicStateCount = 2;
            ds.pDynamicStates = dynamic;
            VkVertexInputBindingDescription binding{0, sizeof(UiVertex), VK_VERTEX_INPUT_RATE_VERTEX};
            VkVertexInputAttributeDescription attrs[3] = {
                {0, 0, VK_FORMAT_R32G32_SFLOAT, uint32_t(offsetof(UiVertex, x))},
                {1, 0, VK_FORMAT_R8G8B8A8_UNORM, uint32_t(offsetof(UiVertex, rgba))},
                {2, 0, VK_FORMAT_R32G32_SFLOAT, uint32_t(offsetof(UiVertex, u))},
            };
            VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            vi.vertexBindingDescriptionCount = 1;
            vi.pVertexBindingDescriptions = &binding;
            vi.vertexAttributeDescriptionCount = 3;
            vi.pVertexAttributeDescriptions = attrs;
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
            dss.depthTestEnable = VK_TRUE;
            dss.depthCompareOp = VK_COMPARE_OP_LESS;
            VkPipelineColorBlendAttachmentState att{};
            att.colorWriteMask = 0xF;
            att.blendEnable = VK_TRUE;
            att.srcColorBlendFactor = att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            att.dstColorBlendFactor = att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            att.colorBlendOp = att.alphaBlendOp = VK_BLEND_OP_ADD;
            VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
            cb.attachmentCount = 1;
            cb.pAttachments = &att;
            VkFormat colorFormat = kColorFormat;
            VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
            rendering.colorAttachmentCount = 1;
            rendering.pColorAttachmentFormats = &colorFormat;
            rendering.depthAttachmentFormat = kDepthFormat;
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
            gi.layout = m_uiMeshLayout;
            ok = Check(vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &gi, nullptr, &m_uiMeshPipeline),
                       "interface mesh pipeline", &error);
        }
        for (VkShaderModule m : meshModules)
            if (m) vkDestroyShaderModule(m_device, m, nullptr);
    }
    for (VkShaderModule m : modules)
        vkDestroyShaderModule(m_device, m, nullptr);
    if (!ok)
        Log("interface back end: %s", error.c_str());
    m_uiShapeFailed = !ok;
    return ok;
}

void Device::DestroyUiShapePipeline()
{
    if (m_uiShapePipeline) {
        vkDestroyPipeline(m_device, m_uiShapePipeline, nullptr);
        m_uiShapePipeline = VK_NULL_HANDLE;
    }
    if (m_uiShapeDepthPipeline) {
        vkDestroyPipeline(m_device, m_uiShapeDepthPipeline, nullptr);
        m_uiShapeDepthPipeline = VK_NULL_HANDLE;
    }
    if (m_uiShapeGlowPipeline) {
        vkDestroyPipeline(m_device, m_uiShapeGlowPipeline, nullptr);
        m_uiShapeGlowPipeline = VK_NULL_HANDLE;
    }
    if (m_uiShapeLayout) {
        vkDestroyPipelineLayout(m_device, m_uiShapeLayout, nullptr);
        m_uiShapeLayout = VK_NULL_HANDLE;
    }
    if (m_uiImagePipeline) {
        vkDestroyPipeline(m_device, m_uiImagePipeline, nullptr);
        m_uiImagePipeline = VK_NULL_HANDLE;
    }
    if (m_uiImageLayout) {
        vkDestroyPipelineLayout(m_device, m_uiImageLayout, nullptr);
        m_uiImageLayout = VK_NULL_HANDLE;
    }
    if (m_uiMeshPipeline) {
        vkDestroyPipeline(m_device, m_uiMeshPipeline, nullptr);
        m_uiMeshPipeline = VK_NULL_HANDLE;
    }
    if (m_uiMeshLayout) {
        vkDestroyPipelineLayout(m_device, m_uiMeshLayout, nullptr);
        m_uiMeshLayout = VK_NULL_HANDLE;
    }
    if (m_uiImageSetLayout) {
        vkDestroyDescriptorSetLayout(m_device, m_uiImageSetLayout, nullptr);
        m_uiImageSetLayout = VK_NULL_HANDLE;
    }
    m_uiShapeFailed = false;
}

// The shapes where the game's drawing of the interface has got to (its draws so far go first: the pending group is
// flushed), onto the interface's target - the layer, or the frame without one - never into the 3D scene: the colour
// pass, the meshes, then the depth pass, then the glow-only shapes, all at `depth`.
void Device::UiShapes(const UiShape* shapes, uint32_t count, float depth, const UiMeshes* meshes)
{
    const bool anyMesh = meshes && meshes->count && meshes->vertexCount && meshes->indexCount;
    if (!m_inFrame || !m_main || (!count && !anyMesh) || m_hideInterface)
        return;
    InterfaceSceneEnd();                         // (the interface's first draw ends the scene; this may be it)
    if (!m_target || m_target == m_scene || m_target->m_format != Format::A8R8G8B8 || !CreateUiShapePipeline())
        return;
    if (!m_rendering)
        BeginRenderingOn(m_target);
    FlushGroup();
    if (anyMesh)                                 // (before anything is recorded: a full ring flushes the frame so far)
        EnsureRingSpace(VkDeviceSize(meshes->vertexCount) * sizeof(UiVertex) +
                        VkDeviceSize(meshes->indexCount) * sizeof(uint32_t) + 64);
    VkCommandBuffer cmd = m_frames[m_frameIndex].main;
    const int32_t w = int32_t(m_target->m_width), h = int32_t(m_target->m_height);
    // What our shapes write (the depth pass), and what the glows and late images test against: a little in front of
    // the interface's depth. GUI.dll's quads come out of the vertex shader a rounding step either side of it (0.01 /
    // RHW 100): written exactly at it, the windows behind ours passed their LESS test against it and drew over ours.
    const float front = depth - std::max(depth * 1e-3f, 1e-6f);
    VkViewport viewport{0.0f, 0.0f, float(w), float(h), 0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    for (int pass = 0; pass < 3; ++pass) {
        VkPipeline pipeline = pass == 0   ? m_uiShapePipeline
                              : pass == 1 ? m_uiShapeDepthPipeline
                                          : m_uiShapeGlowPipeline;
        bool bound = false;
        for (uint32_t i = 0; i < count; ++i) {
            const UiShape& s = shapes[i];
            if ((s.glowOnly != 0.0f) != (pass == 2))
                continue;
            if (s.image) {                       // an image: in its place among the shapes (or the glows), no depth
                if (pass != 1 && DrawUiImage(cmd, s, pass == 0 ? depth : front, w, h))
                    bound = false;
                continue;
            }
            if (!bound) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                bound = true;
            }
            const float g = pass != 1 && s.glow > 0.0f ? s.glow * 4.0f : 0.0f;   // the glow's reach (colour only)
            int32_t x0 = std::max({0, int32_t(std::floor(s.rect[0] - g)), int32_t(std::floor(s.clip[0]))});
            int32_t y0 = std::max({0, int32_t(std::floor(s.rect[1] - g)), int32_t(std::floor(s.clip[1]))});
            int32_t x1 = std::min({w, int32_t(std::ceil(s.rect[2] + g)), int32_t(std::ceil(s.clip[2]))});
            int32_t y1 = std::min({h, int32_t(std::ceil(s.rect[3] + g)), int32_t(std::ceil(s.clip[3]))});
            if (x1 <= x0 || y1 <= y0)
                continue;
            VkRect2D scissor{{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};
            vkCmdSetScissor(cmd, 0, 1, &scissor);
            float push[24];
            std::memcpy(push, s.rect, 16);
            push[4] = s.radius, push[5] = s.border, push[6] = s.glow, push[7] = pass == 0 ? depth : front;
            std::memcpy(push + 8, s.fillTop, 64);   // fillTop, fillBottom, borderColor, glowColor
            vkCmdPushConstants(cmd, m_uiShapeLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), push);
            vkCmdDraw(cmd, 3, 1, 0, 0);
        }
        if (pass == 0 && anyMesh)
            DrawUiMeshes(cmd, *meshes, depth, w, h);
    }
    // The next draw binds everything again (as after EndScene).
    m_cache = StateCache{};
    m_arenaBound = false;
    m_bindlessBound = false;
    m_constantsDirty = true;
}

// One image of a UiShapes batch: its texture by push descriptor, its rectangle scissored to its clip. True when drawn
// (the shape pipeline must be bound again after it).
bool Device::DrawUiImage(VkCommandBuffer cmd, const UiShape& s, float depth, int32_t w, int32_t h)
{
    Texture* t = s.image;
    if (!t || !t->m_view || !m_uiImagePipeline)
        return false;
    int32_t x0 = std::max({0, int32_t(std::floor(s.rect[0])), int32_t(std::floor(s.clip[0]))});
    int32_t y0 = std::max({0, int32_t(std::floor(s.rect[1])), int32_t(std::floor(s.clip[1]))});
    int32_t x1 = std::min({w, int32_t(std::ceil(s.rect[2])), int32_t(std::ceil(s.clip[2]))});
    int32_t y1 = std::min({h, int32_t(std::ceil(s.rect[3])), int32_t(std::ceil(s.clip[3]))});
    if (x1 <= x0 || y1 <= y0)
        return false;
    if (t->m_layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        return false;                            // (its upload not done yet: the next frame)
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_uiImagePipeline);
    VkDescriptorImageInfo image{m_linearSampler, t->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wr.dstBinding = 0;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &image;
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_uiImageLayout, 0, 1, &wr);
    VkRect2D scissor{{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    float push[16];
    std::memcpy(push, s.rect, 16);
    std::memcpy(push + 4, s.uv, 16);
    std::memcpy(push + 8, s.fillTop, 16);        // the tint
    push[12] = depth, push[13] = s.additive, push[14] = 0.0f, push[15] = 0.0f;
    vkCmdPushConstants(cmd, m_uiImageLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    return true;
}

// A batch of meshes: its vertices and indices into the ring (space ensured by UiShapes), then each mesh with its
// scissor, translation, opacity and texture.
void Device::DrawUiMeshes(VkCommandBuffer cmd, const UiMeshes& m, float depth, int32_t w, int32_t h)
{
    if (!m_uiMeshPipeline)
        return;
    void* cpu;
    const VkDeviceSize vb = Allocate(VkDeviceSize(m.vertexCount) * sizeof(UiVertex), 16, &cpu);
    std::memcpy(cpu, m.vertices, size_t(m.vertexCount) * sizeof(UiVertex));
    const VkDeviceSize ib = Allocate(VkDeviceSize(m.indexCount) * sizeof(uint32_t), 4, &cpu);
    std::memcpy(cpu, m.indices, size_t(m.indexCount) * sizeof(uint32_t));
    VkBuffer ring = m_frames[m_frameIndex].ring;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_uiMeshPipeline);
    vkCmdBindVertexBuffers(cmd, 0, 1, &ring, &vb);
    vkCmdBindIndexBuffer(cmd, ring, ib, VK_INDEX_TYPE_UINT32);
    const Texture* bound = nullptr;
    for (uint32_t i = 0; i < m.count; ++i) {
        const UiMesh& mesh = m.meshes[i];
        if (!mesh.indexCount || mesh.firstIndex + mesh.indexCount > m.indexCount || mesh.opacity <= 0.0f)
            continue;
        const Texture* t = mesh.image;
        if (!t || !t->m_view || t->m_layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
            continue;                            // (no texture, or its upload not done yet: the next frame)
        int32_t x0 = std::max(0, int32_t(std::floor(mesh.clip[0])));
        int32_t y0 = std::max(0, int32_t(std::floor(mesh.clip[1])));
        int32_t x1 = std::min(w, int32_t(std::ceil(mesh.clip[2])));
        int32_t y1 = std::min(h, int32_t(std::ceil(mesh.clip[3])));
        if (x1 <= x0 || y1 <= y0)
            continue;
        if (t != bound) {
            VkDescriptorImageInfo image{m_linearSampler, t->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            wr.dstBinding = 0;
            wr.descriptorCount = 1;
            wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            wr.pImageInfo = &image;
            vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_uiMeshLayout, 0, 1, &wr);
            bound = t;
        }
        VkRect2D scissor{{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        const float push[8] = {2.0f / float(w), 2.0f / float(h), mesh.translate[0], mesh.translate[1],
                               depth, std::min(mesh.opacity, 1.0f), 0.0f, 0.0f};
        vkCmdPushConstants(cmd, m_uiMeshLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), push);
        vkCmdDrawIndexed(cmd, mesh.indexCount, 1, mesh.firstIndex, 0, 0);
    }
}

// GUI.dll's interface depth: every quad at Z 0.01 (pre-transformed), in the game's current viewport's depth range.
float Device::InterfaceDepth() const
{
    return m_viewport.minZ + 0.01f * (m_viewport.maxZ - m_viewport.minZ);
}

// Glass behind the interface's windows (docs/ui-dll.md phase 1): the finished frame blurred - dual-filter (Kawase)
// steps down to 1/2^blur of its size and back up to half - then, per window, the blurred frame dimmed and tinted inside
// its rounded rectangle with a rim light and a glow, onto the frame. The interface layer goes over it next.
void Device::RenderGlass(VkCommandBuffer cmd)
{
    if (!m_glass.enable || m_glassRects.empty() || !m_glassPipeline || !m_glassDownPipeline || !m_glassUpPipeline)
        return;
    const uint32_t levels = std::clamp(m_glass.blur, 1u, 4u);
    for (uint32_t i = 0; i < levels; ++i) {
        uint32_t w = std::max(1u, m_main->m_width >> (i + 1)), h = std::max(1u, m_main->m_height >> (i + 1));
        if (!m_glassTex[i] || m_glassTex[i]->m_width != w || m_glassTex[i]->m_height != h) {
            if (m_glassTex[i]) DestroyTexture(m_glassTex[i]);
            m_glassTex[i] = CreateImage(w, h, Format::A8R8G8B8, 1, true);
            if (!m_glassTex[i])
                return;
        }
    }
    Texture* src = m_main;
    for (uint32_t i = 0; i < levels; ++i) {
        const float params[4] = {1.0f / float(src->m_width), 1.0f / float(src->m_height), 0.0f, 0.0f};
        Transition(cmd, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        FullscreenPass(cmd, m_glassTex[i], m_glassDownPipeline, src->m_view, src->m_view, m_linearSampler, params,
                       sizeof(params), false);
        src = m_glassTex[i];
    }
    for (uint32_t i = levels - 1; i-- > 0;) {
        Texture* smaller = m_glassTex[i + 1];
        const float params[4] = {1.0f / float(smaller->m_width), 1.0f / float(smaller->m_height),
                                 1.0f / float(m_glassTex[i]->m_width), 1.0f / float(m_glassTex[i]->m_height)};
        Transition(cmd, smaller, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        FullscreenPass(cmd, m_glassTex[i], m_glassUpPipeline, smaller->m_view, smaller->m_view, m_linearSampler, params,
                       sizeof(params), false);
    }
    Texture* blurred = m_glassTex[0];
    Transition(cmd, blurred, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // The windows onto the frame, each with the scissor around it and its glow.
    Transition(cmd, m_main, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = m_main->m_view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {m_main->m_width, m_main->m_height}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &ri);
    VkViewport viewport{0.0f, 0.0f, float(m_main->m_width), float(m_main->m_height), 0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_glassPipeline);
    VkDescriptorImageInfo image{m_linearSampler, blurred->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w[4] = {};
    for (uint32_t i = 0; i < 4; ++i) {           // (the layout has four bindings; the shader reads the first)
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[i].pImageInfo = &image;
    }
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_bloomLayout, 0, 4, w);
    const float glow = 6.0f * float(m_main->m_height) / 1440.0f;   // the glow's width, by the screen's size
    const float margin = m_glass.edge > 0.0f ? std::ceil(glow * 4.0f) : 0.0f;
    for (const auto& r : m_glassRects) {
        const float params[20] = {r[0], r[1], r[2], r[3],
                                  m_glass.radius, m_glass.dim, m_glass.tint, m_glass.edge,
                                  1.0f / float(m_main->m_width), 1.0f / float(m_main->m_height), glow, 0.0f,
                                  m_glass.tintColor[0], m_glass.tintColor[1], m_glass.tintColor[2], 0.0f,
                                  m_glass.rimColor[0], m_glass.rimColor[1], m_glass.rimColor[2], 0.0f};
        int32_t x0 = std::max(0, int32_t(std::floor(r[0] - margin)));
        int32_t y0 = std::max(0, int32_t(std::floor(r[1] - margin)));
        int32_t x1 = std::min(int32_t(m_main->m_width), int32_t(std::ceil(r[2] + margin)));
        int32_t y1 = std::min(int32_t(m_main->m_height), int32_t(std::ceil(r[3] + margin)));
        if (x1 <= x0 || y1 <= y0)
            continue;
        VkRect2D scissor{{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdPushConstants(cmd, m_bloomLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(params), params);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }
    vkCmdEndRendering(cmd);
}

}  // namespace rvk
