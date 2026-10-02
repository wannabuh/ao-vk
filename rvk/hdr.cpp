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
const uint32_t kGiSpirv[] = {
#include "gi.frag.inc"
};
const uint32_t kGiBlurSpirv[] = {
#include "gi_blur.frag.inc"
};
const uint32_t kVolumeSpirv[] = {
#include "volume.frag.inc"
};
const uint32_t kVolumeBlurSpirv[] = {
#include "volume_blur.frag.inc"
};
const uint32_t kSsrSpirv[] = {
#include "ssr.frag.inc"
};
const uint32_t kContactSpirv[] = {
#include "contact.frag.inc"
};
const uint32_t kTaaSpirv[] = {
#include "taa.frag.inc"
};
const uint32_t kSharpenSpirv[] = {
#include "sharpen.frag.inc"
};
const uint32_t kMotionBlurSpirv[] = {
#include "motion_blur.frag.inc"
};
const uint32_t kTileMaxSpirv[] = {
#include "motion_tilemax.frag.inc"
};
const uint32_t kNeighbourMaxSpirv[] = {
#include "motion_neighbourmax.frag.inc"
};
const uint32_t kObjectBlurSpirv[] = {
#include "motion_object.frag.inc"
};
const uint32_t kDofCompositeSpirv[] = {
#include "dof_composite.frag.inc"
};
const uint32_t kDofFocusSpirv[] = {
#include "dof_focus.frag.inc"
};
const uint32_t kDofPrefilterSpirv[] = {
#include "dof_prefilter.frag.inc"
};
const uint32_t kDofTilesSpirv[] = {
#include "dof_tiles.frag.inc"
};
const uint32_t kDofGatherSpirv[] = {
#include "dof_gather.frag.inc"
};
const uint32_t kDofFinalSpirv[] = {
#include "dof_final.frag.inc"
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

    // Layouts: tone mapping reads its inputs (occlusion.glsl); the other passes read up to four images.
    auto setLayout = [&](uint32_t count, VkDescriptorSetLayout* out) {
        VkDescriptorSetLayoutBinding b[kTonemapInputs];
        for (uint32_t i = 0; i < kTonemapInputs; ++i)
            b[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
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
    if (!setLayout(kTonemapInputs, &m_tonemapSetLayout) || !pipelineLayout(m_tonemapSetLayout, &m_tonemapLayout) ||
        !setLayout(4, &m_bloomSetLayout) || !pipelineLayout(m_bloomSetLayout, &m_bloomLayout))
        return false;
    {
        // Volumetric light: depth, the sun shadow cascades, the frame lights (binding 4, as in the scene shaders).
        VkDescriptorSetLayoutBinding b[3] = {
            {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
        VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        sl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        sl.bindingCount = 3;
        sl.pBindings = b;
        if (!Check(vkCreateDescriptorSetLayout(m_device, &sl, nullptr, &m_volumeSetLayout), "volume set layout", error) ||
            !pipelineLayout(m_volumeSetLayout, &m_volumeLayout))
            return false;
    }

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
              fullscreen(kGiSpirv, sizeof(kGiSpirv), hdrFormat, false, m_bloomLayout, &m_giPipeline) &&
              fullscreen(kGiBlurSpirv, sizeof(kGiBlurSpirv), hdrFormat, false, m_bloomLayout, &m_giBlurPipeline) &&
              fullscreen(kVolumeSpirv, sizeof(kVolumeSpirv), hdrFormat, false, m_volumeLayout, &m_volumePipeline) &&
              fullscreen(kVolumeBlurSpirv, sizeof(kVolumeBlurSpirv), hdrFormat, false, m_bloomLayout, &m_volumeBlurPipeline) &&
              fullscreen(kSsrSpirv, sizeof(kSsrSpirv), hdrFormat, false, m_bloomLayout, &m_ssrPipeline) &&
              fullscreen(kContactSpirv, sizeof(kContactSpirv), GetFormatInfo(Format::RG16F).vk, false, m_bloomLayout,
                         &m_contactPipeline) &&
              fullscreen(kTaaSpirv, sizeof(kTaaSpirv), hdrFormat, false, m_bloomLayout, &m_taaPipeline) &&
              fullscreen(kSharpenSpirv, sizeof(kSharpenSpirv), kColorFormat, false, m_bloomLayout, &m_sharpenPipeline) &&
              fullscreen(kMotionBlurSpirv, sizeof(kMotionBlurSpirv), kColorFormat, false, m_bloomLayout, &m_motionPipeline) &&
              fullscreen(kTileMaxSpirv, sizeof(kTileMaxSpirv), GetFormatInfo(Format::RG16F).vk, false, m_bloomLayout,
                         &m_tileMaxPipeline) &&
              fullscreen(kNeighbourMaxSpirv, sizeof(kNeighbourMaxSpirv), GetFormatInfo(Format::RG16F).vk, false,
                         m_bloomLayout, &m_neighbourMaxPipeline) &&
              fullscreen(kObjectBlurSpirv, sizeof(kObjectBlurSpirv), kColorFormat, false, m_bloomLayout,
                         &m_objectBlurPipeline) &&
              fullscreen(kDofCompositeSpirv, sizeof(kDofCompositeSpirv), hdrFormat, false, m_tonemapLayout,
                         &m_dofCompositePipeline) &&
              fullscreen(kDofFocusSpirv, sizeof(kDofFocusSpirv), GetFormatInfo(Format::RG16F).vk, false, m_bloomLayout,
                         &m_dofFocusPipeline) &&
              fullscreen(kDofPrefilterSpirv, sizeof(kDofPrefilterSpirv), hdrFormat, false, m_bloomLayout,
                         &m_dofPrefilterPipeline) &&
              fullscreen(kDofTilesSpirv, sizeof(kDofTilesSpirv), GetFormatInfo(Format::RG16F).vk, false, m_bloomLayout,
                         &m_dofTilesPipeline) &&
              fullscreen(kDofGatherSpirv, sizeof(kDofGatherSpirv), hdrFormat, false, m_bloomLayout, &m_dofGatherPipeline) &&
              fullscreen(kDofFinalSpirv, sizeof(kDofFinalSpirv), hdrFormat, false, m_bloomLayout, &m_dofFinalPipeline);
    vkDestroyShaderModule(m_device, vert, nullptr);
    return ok;
}

void Device::DestroyHdrResources()
{
    for (Texture* t : m_bloomLevels) DestroyTextureNow(t);
    m_bloomLevels.clear();
    for (Texture*& t : m_aoTex)
        if (t) { DestroyTextureNow(t); t = nullptr; }
    for (Texture*& t : m_giTex)
        if (t) { DestroyTextureNow(t); t = nullptr; }
    for (Texture*& t : m_volumeTex)
        if (t) { DestroyTextureNow(t); t = nullptr; }
    if (m_ssrTex) { DestroyTextureNow(m_ssrTex); m_ssrTex = nullptr; }
    for (Texture*& t : m_lut)
        if (t) { DestroyTextureNow(t); t = nullptr; }
    for (Texture*& t : m_taaHistory)
        if (t) { DestroyTextureNow(t); t = nullptr; }
    for (Texture*& t : m_contactTex)
        if (t) { DestroyTextureNow(t); t = nullptr; }
    if (m_tonemapped) { DestroyTextureNow(m_tonemapped); m_tonemapped = nullptr; }
    for (Texture*& t : m_motionTiles)
        if (t) { DestroyTextureNow(t); t = nullptr; }
    for (Texture** t : {&m_dofIn, &m_dofOut, &m_dofHalf, &m_dofBlur, &m_dofTiles[0], &m_dofTiles[1], &m_dofFocus[0], &m_dofFocus[1]})
        if (*t) { DestroyTextureNow(*t); *t = nullptr; }
    for (VkPipeline* p : {&m_bloomDown, &m_bloomUp, &m_aoPipeline, &m_aoBlurPipeline, &m_giPipeline, &m_giBlurPipeline,
                          &m_volumePipeline, &m_volumeBlurPipeline, &m_ssrPipeline,
                          &m_contactPipeline, &m_taaPipeline, &m_sharpenPipeline,
                          &m_motionPipeline,
                          &m_tileMaxPipeline, &m_neighbourMaxPipeline, &m_objectBlurPipeline, &m_dofCompositePipeline,
                          &m_dofFocusPipeline, &m_dofPrefilterPipeline, &m_dofTilesPipeline, &m_dofGatherPipeline,
                          &m_dofFinalPipeline})
        if (*p) { vkDestroyPipeline(m_device, *p, nullptr); *p = VK_NULL_HANDLE; }
    if (m_bloomLayout) vkDestroyPipelineLayout(m_device, m_bloomLayout, nullptr);
    if (m_volumeLayout) vkDestroyPipelineLayout(m_device, m_volumeLayout, nullptr);
    if (m_volumeSetLayout) vkDestroyDescriptorSetLayout(m_device, m_volumeSetLayout, nullptr);
    m_volumeLayout = VK_NULL_HANDLE;
    m_volumeSetLayout = VK_NULL_HANDLE;
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
    // Temporal anti-aliasing: the scene moves by a sub-pixel offset each frame (Halton 2, 3 over 8 frames).
    auto halton = [](uint32_t i, uint32_t base) {
        float f = 1.0f, r = 0.0f;
        for (; i; i /= base) { f /= float(base); r += f * float(i % base); }
        return r;
    };
    uint32_t k = uint32_t(m_frameNumber % 8) + 1;
    float w = m_scene ? float(m_scene->m_width) : 1.0f, h = m_scene ? float(m_scene->m_height) : 1.0f;
    m_taaJitter[0] = (halton(k, 2) - 0.5f) * 2.0f / w;
    m_taaJitter[1] = (halton(k, 3) - 0.5f) * 2.0f / h;
    m_sceneSaw3D = false;
    m_aoProjValid = false;
    m_motionPrev.swap(m_motionCur);              // last frame's objects, for matching this frame's
    m_motionCur.clear();
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
    bool gi = RenderGi(cmd);
    bool volume = RenderVolume(cmd);
    Transition(cmd, m_localFraction, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    bool ssr = RenderSsr(cmd);
    bool contact = RenderContactShadows(cmd);
    Transition(cmd, m_albedo, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    // The upsampled bloom chain sums every level's light: averaged here.
    // params[3]: how much bloom reaches objects nearer than its light (-1: the bloom carries no depth).
    // params[8..15]: colour grading (identity lookup tables until the proxy loads others).
    for (Texture*& lut : m_lut)
        if (!lut) lut = CreateLut(16, nullptr);
    float params[16] = {m_tonemapKnee, m_exposure, bloom ? m_bloomStrength / float(m_bloomLevels.size()) : 0.0f,
                        m_bloomDepth ? m_bloomOverNearer : -1.0f, m_aoProj.m[2][2], m_aoProj.m[3][2],
                        gi ? m_giStrength : 0.0f,
                        float((volume ? 1 : 0) | (ssr ? 2 : 0) | (ao ? 4 : 0) | (contact ? 8 : 0))};
    GradingParams(params + 8);
    // Depth of field blurs the scene with its ambient occlusion, indirect and volumetric light applied; the tone
    // mapping then leaves them out.
    bool dof = RenderDof(cmd, bloom, ao, gi, volume, params);
    Texture* scene = dof ? m_dofOut : m_scene;
    if (dof) params[6] = params[7] = 0.0f;
    // With motion blur or anti-aliasing the tone mapping goes to an intermediate image: the anti-aliasing resolves
    // it (and sharpens it back into it, or into the main target), the blur reads it into the main target.
    float taaParams[24];
    bool taa = TaaParams(taaParams);
    float motion[24];
    bool blur = MotionBlurParams(motion);
    if ((blur || taa) &&
        (!m_tonemapped || m_tonemapped->m_width != m_ldrMain->m_width || m_tonemapped->m_height != m_ldrMain->m_height)) {
        if (m_tonemapped) DestroyTexture(m_tonemapped);
        m_tonemapped = CreateImage(m_ldrMain->m_width, m_ldrMain->m_height, Format::A8R8G8B8, 1, true);
        blur = blur && m_tonemapped;
        taa = taa && m_tonemapped;
    }
    Texture* toned = blur || taa ? m_tonemapped : m_ldrMain;
    TonemapInputsPass(cmd, toned, m_tonemapPipeline, scene, bloom, ao, gi, volume, params);
    if (taa)
        RenderTaa(cmd, blur ? m_tonemapped : m_ldrMain, taaParams);
    if (blur) {
        Transition(cmd, m_tonemapped, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        MakeDepthReadable(cmd);
        if (m_motionMode == 1)
            RenderObjectMotionBlur(cmd, motion);
        else
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

// The depth buffer, written by the scene, readable by the post passes (BeginRenderingOn makes it an attachment again).
void Device::MakeDepthReadable(VkCommandBuffer cmd)
{
    if (m_depthLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        return;
    ImageBarrier(cmd, m_depth, VK_IMAGE_ASPECT_DEPTH_BIT, m_depthLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    m_depthLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

// A full-target pass with the tone mapping's inputs (occlusion.glsl): the scene, the bloom, the occlusion, the depth,
// the local-light fraction, the indirect light and the surface colour - the tone mapping itself, or the depth of
// field's composite.
void Device::TonemapInputsPass(VkCommandBuffer cmd, Texture* dst, VkPipeline pipeline, Texture* scene, bool bloom, bool ao,
                               bool gi, bool volume, const float params[16])
{
    Transition(cmd, dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = dst->m_view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
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
    Texture* glow = bloom ? m_bloomLevels[0] : m_scene;          // unread when the strength is 0
    Texture* occlusion = ao ? m_aoTex[1] : m_scene;               // unread when off
    Texture* indirect = gi ? m_giTex[1] : m_scene;                // unread when off
    Texture* scattered = volume ? m_volumeTex[1] : m_scene;       // unread when off
    Texture* reflections = m_ssrTex && (int(params[7]) & 2) ? m_ssrTex : m_scene;   // unread when off
    Texture* contact = (int(params[7]) & 8) ? m_contactTex[1] : m_scene;              // unread when off
    // The depth buffer is readable after the AO pass; without AO, any readable image stands in (unread).
    VkImageView depthView = m_depthLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ? m_depthView : m_scene->m_view;
    VkDescriptorImageInfo images[] = {{m_pointSampler, scene->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                       {m_linearSampler, glow->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                       {m_pointSampler, occlusion->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                       {m_pointSampler, depthView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                       {m_pointSampler, m_localFraction->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                       {m_pointSampler, indirect->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                       {m_pointSampler, m_albedo->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                       {m_pointSampler, scattered->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                       {m_pointSampler, reflections->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                       {m_pointSampler, contact->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                       {m_linearSampler, m_lut[0]->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                       {m_linearSampler, m_lut[1]->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    static_assert(sizeof(images) / sizeof(images[0]) == kTonemapInputs, "occlusion.glsl bindings");
    VkWriteDescriptorSet w[kTonemapInputs] = {};
    for (uint32_t i = 0; i < kTonemapInputs; ++i) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[i].pImageInfo = &images[i];
    }
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_tonemapLayout, 0, kTonemapInputs, w);
    vkCmdPushConstants(cmd, m_tonemapLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64, params);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

// Depth of field (dof_*.frag): the scene with its occlusion -> focus distance (1x1, eased) -> half resolution colour
// and circle of confusion -> largest CoC per tile and neighbourhood -> depth-aware blur (normal or bokeh) -> blended
// with the sharp scene into m_dofOut, which the tone mapping reads. False: off (or no world camera this frame).
bool Device::RenderDof(VkCommandBuffer cmd, bool bloom, bool ao, bool gi, bool volume, const float tonemapParams[16])
{
    double now = SwayClock(), dt = std::clamp(now - m_dofPrevTime, 0.0, 0.25);
    m_dofPrevTime = now;
    bool perspective = m_aoProjValid && m_aoProj.m[2][3] == 1.0f && m_aoProj.m[3][3] == 0.0f;
    if (!m_dof || !perspective || !m_depth)
        return false;
    uint32_t w = m_scene->m_width, h = m_scene->m_height, hw = std::max(1u, w / 2), hh = std::max(1u, h / 2);
    uint32_t tw = (hw + 15) / 16, th = (hh + 15) / 16;
    auto ensure = [&](Texture*& t, uint32_t tw_, uint32_t th_, Format f) {
        if (t && t->m_width == tw_ && t->m_height == th_) return true;
        if (t) DestroyTexture(t);
        t = CreateImage(tw_, th_, f, 1, true);
        return t != nullptr;
    };
    bool focusNew = !m_dofFocus[0];
    if (!ensure(m_dofIn, w, h, Format::RGBA16F) || !ensure(m_dofOut, w, h, Format::RGBA16F) ||
        !ensure(m_dofHalf, hw, hh, Format::RGBA16F) || !ensure(m_dofBlur, hw, hh, Format::RGBA16F) ||
        !ensure(m_dofTiles[0], tw, th, Format::RG16F) || !ensure(m_dofTiles[1], tw, th, Format::RG16F) ||
        !ensure(m_dofFocus[0], 1, 1, Format::RG16F) || !ensure(m_dofFocus[1], 1, 1, Format::RG16F))
        return false;
    MakeDepthReadable(cmd);
    if (focusNew) {                              // no focus yet: the focus pass starts from "invalid"
        for (Texture* t : m_dofFocus) {
            Transition(cmd, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkClearColorValue zero{};
            VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(cmd, t->m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
        }
    }
    // 1. The scene with its ambient occlusion, indirect and volumetric light (the tone mapping's inputs).
    TonemapInputsPass(cmd, m_dofIn, m_dofCompositePipeline, m_scene, bloom, ao, gi, volume, tonemapParams);
    Transition(cmd, m_dofIn, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    // Parameters: projection and size; strength, max radius (scaled from 1440 lines), in-focus band, flags; frame
    // time, manual focus.
    float radius = m_dofRadius * float(h) / 1440.0f;
    float p[12] = {m_aoProj.m[2][2], m_aoProj.m[3][2], float(w), float(h),
                   m_dofStrength, radius, std::clamp(m_dofRange, 0.0f, 0.95f),
                   float((m_dofNear ? 1 : 0) | (m_dofBokeh ? 2 : 0) | (m_dofFar ? 4 : 0)),
                   float(dt), m_dofFocusDistance, std::max(m_dofCloseFocus, 0.01f), 0.0f};
    // 2. Focus distance, eased from last frame's.
    Texture* prevFocus = m_dofFocus[m_dofFocusIndex];
    Texture* focus = m_dofFocus[m_dofFocusIndex ^ 1];
    m_dofFocusIndex ^= 1;
    Transition(cmd, prevFocus, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    FullscreenPass(cmd, focus, m_dofFocusPipeline, m_depthView, prevFocus->m_view, m_pointSampler, p, sizeof(p), false);
    Transition(cmd, focus, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    // 3. Half resolution colour + circle of confusion.
    FullscreenPass(cmd, m_dofHalf, m_dofPrefilterPipeline, m_dofIn->m_view, m_depthView, m_pointSampler, p, sizeof(p), false,
                   focus->m_view);
    Transition(cmd, m_dofHalf, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    // 4. Largest CoC per tile, then per neighbourhood.
    FullscreenPass(cmd, m_dofTiles[0], m_dofTilesPipeline, m_dofHalf->m_view, m_dofHalf->m_view, m_pointSampler, p, sizeof(p), false);
    Transition(cmd, m_dofTiles[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    FullscreenPass(cmd, m_dofTiles[1], m_neighbourMaxPipeline, m_dofTiles[0]->m_view, m_dofTiles[0]->m_view, m_pointSampler,
                   p, sizeof(p), false);
    Transition(cmd, m_dofTiles[1], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    // 5. The blur.
    FullscreenPass(cmd, m_dofBlur, m_dofGatherPipeline, m_dofHalf->m_view, m_dofTiles[1]->m_view, m_pointSampler, p,
                   sizeof(p), false);
    Transition(cmd, m_dofBlur, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    // 6. Sharp and blurred blended by the circle of confusion (the blur read with a linear filter).
    FullscreenPass(cmd, m_dofOut, m_dofFinalPipeline, m_dofIn->m_view, m_dofBlur->m_view, m_linearSampler, p, sizeof(p), false,
                   m_depthView, focus->m_view);
    Transition(cmd, m_dofOut, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return true;
}

// One full-target pass: dst <- pipeline(src, src2). load: keep dst's contents (blended onto) instead of overwriting.
void Device::FullscreenPass(VkCommandBuffer cmd, Texture* dst, VkPipeline pipeline, VkImageView src, VkImageView src2,
                            VkSampler sampler, const float* params, uint32_t paramBytes, bool load, VkImageView src3,
                            VkImageView src4)
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
    // Unused bindings get the first image (the layouts have four).
    VkImageView views[4] = {src, src2, src3 ? src3 : src, src4 ? src4 : src};
    VkDescriptorImageInfo images[4];
    VkWriteDescriptorSet w[4] = {};
    for (uint32_t i = 0; i < 4; ++i) {
        images[i] = {sampler, views[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[i].pImageInfo = &images[i];
    }
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_bloomLayout, 0, 4, w);
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

// Indirect light (gi.frag): one bounce of the lit HDR scene, gathered at half resolution from the depth buffer and the
// scene, blurred both ways (depth-aware); m_giTex[1] holds it. Like the AO, needs the world camera's projection.
bool Device::RenderGi(VkCommandBuffer cmd)
{
    if (m_giStrength <= 0.0f || !m_aoProjValid || m_aoProj.m[2][3] != 1.0f || m_aoProj.m[3][3] != 0.0f || !m_depth)
        return false;
    uint32_t w = std::max(1u, m_scene->m_width / 2), h = std::max(1u, m_scene->m_height / 2);
    if (!m_giTex[0] || m_giTex[0]->m_width != w || m_giTex[0]->m_height != h) {
        for (Texture*& t : m_giTex) {
            if (t) DestroyTexture(t);
            t = CreateImage(w, h, Format::RGBA16F, 1, true);
        }
        if (!m_giTex[0] || !m_giTex[1])
            return false;
    }
    MakeDepthReadable(cmd);
    float params[8] = {m_aoProj.m[2][2], m_aoProj.m[3][2], m_aoProj.m[0][0], m_aoProj.m[1][1],
                       m_giRadius, 0.0f, float(m_scene->m_width), float(m_scene->m_height)};
    FullscreenPass(cmd, m_giTex[0], m_giPipeline, m_depthView, m_scene->m_view, m_pointSampler, params, sizeof(params), false);
    // Two blurs each way: wide taps, then the gaps between them.
    static const float kPasses[4][2] = {{1, 0}, {0, 1}, {1, 0}, {0, 1}};
    for (int pass = 0; pass < 4; ++pass) {
        Texture* src = m_giTex[pass & 1];
        Texture* dst = m_giTex[(pass & 1) ^ 1];
        Transition(cmd, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        float blur[12] = {kPasses[pass][0], kPasses[pass][1], pass < 2 ? 3.0f : 1.0f, 0.0f,
                          params[0], params[1], params[2], params[3], params[6], params[7], 0.0f, 0.0f};
        FullscreenPass(cmd, dst, m_giBlurPipeline, src->m_view, m_depthView, m_pointSampler, blur, sizeof(blur), false);
    }
    // Four passes end in m_giTex[0]: swap so that [1] holds the result.
    std::swap(m_giTex[0], m_giTex[1]);
    Transition(cmd, m_giTex[1], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return true;
}

// Volumetric light (volume.frag): sun shafts marched through the shadow cascades, at half resolution,
// blurred both ways; m_volumeTex[1] holds it. The frame lights are written again into the frame's post buffer (the
// scene's copy may be in an earlier ring generation).
bool Device::RenderVolume(VkCommandBuffer cmd)
{
    bool perspective = m_aoProjValid && m_aoProj.m[2][3] == 1.0f && m_aoProj.m[3][3] == 0.0f;
    d3d::Matrix inverse;
    if (m_volume <= 0.0f || !perspective || !m_depth || !InvertMatrix(MulMatrix(m_aoView, m_aoProj), &inverse))
        return false;
    uint32_t w = std::max(1u, m_scene->m_width / 2), h = std::max(1u, m_scene->m_height / 2);
    if (!m_volumeTex[0] || m_volumeTex[0]->m_width != w || m_volumeTex[0]->m_height != h) {
        for (Texture*& t : m_volumeTex) {
            if (t) DestroyTexture(t);
            t = CreateImage(w, h, Format::RGBA16F, 1, true);
        }
        if (!m_volumeTex[0] || !m_volumeTex[1])
            return false;
    }
    MakeDepthReadable(cmd);
    Frame& frame = m_frames[m_frameIndex];
    FillFrameLights(static_cast<FrameLights*>(frame.postData), false);

    struct {
        d3d::Matrix invViewProj;
        float eye[4], params[4], size[4], shafts[4];
    } push;
    push.invViewProj = inverse;
    const auto& v = m_aoView.m;
    for (int i = 0; i < 3; ++i) push.eye[i] = -(v[3][0] * v[i][0] + v[3][1] * v[i][1] + v[3][2] * v[i][2]);
    push.eye[3] = 120.0f;                                         // longest ray
    push.params[0] = 0.006f * m_volumeHaze;                       // scattering per world unit (at the ground)
    push.params[1] = m_volume;
    push.params[2] = float(m_frameNumber % 64) * 0.618034f;       // the march's offsets move on every frame
    push.params[3] = m_aoProj.m[2][2];
    push.size[0] = float(m_scene->m_width);
    push.size[1] = float(m_scene->m_height);
    push.size[2] = 32.0f;                                         // sun steps
    push.size[3] = m_aoProj.m[3][2];
    push.shafts[0] = 2.0f * m_volumeShafts;
    push.shafts[1] = push.shafts[2] = push.shafts[3] = 0.0f;
    static_assert(sizeof(push) <= 128, "push constant range");

    Texture* dst = m_volumeTex[0];
    Transition(cmd, dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = dst->m_view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {w, h}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &ri);
    VkViewport viewport{0.0f, 0.0f, float(w), float(h), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {w, h}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_volumePipeline);
    VkDescriptorImageInfo depth{m_pointSampler, m_depthView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo shadow{m_shadowSampler, m_shadowView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo lights{frame.post, 0, sizeof(FrameLights)};
    VkWriteDescriptorSet wr[3] = {};
    for (auto& x : wr) { x.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; x.descriptorCount = 1; }
    wr[0].dstBinding = 0; wr[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr[0].pImageInfo = &depth;
    wr[1].dstBinding = 1; wr[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr[1].pImageInfo = &shadow;
    wr[2].dstBinding = 4; wr[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; wr[2].pBufferInfo = &lights;
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_volumeLayout, 0, 3, wr);
    vkCmdPushConstants(cmd, m_volumeLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);

    for (int pass = 0; pass < 2; ++pass) {
        Texture* src = m_volumeTex[pass];
        Texture* out = m_volumeTex[pass ^ 1];
        Transition(cmd, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        float dir[4] = {pass == 0 ? 1.0f : 0.0f, pass == 0 ? 0.0f : 1.0f, 2.0f, 0.0f};
        FullscreenPass(cmd, out, m_volumeBlurPipeline, src->m_view, src->m_view, m_pointSampler, dir, sizeof(dir), false);
    }
    std::swap(m_volumeTex[0], m_volumeTex[1]);    // the second pass wrote [0]
    Transition(cmd, m_volumeTex[1], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return true;
}

// Screen-space reflections (ssr.frag) at full resolution into m_ssrTex: needs the scene, the depth buffer and the
// reflectivity (the local-fraction attachment's G), all readable by now.
bool Device::RenderSsr(VkCommandBuffer cmd)
{
    bool perspective = m_aoProjValid && m_aoProj.m[2][3] == 1.0f && m_aoProj.m[3][3] == 0.0f;
    if (m_ssr <= 0.0f || !perspective || !m_depth)
        return false;
    if (!m_ssrTex || m_ssrTex->m_width != m_scene->m_width || m_ssrTex->m_height != m_scene->m_height) {
        if (m_ssrTex) DestroyTexture(m_ssrTex);
        m_ssrTex = CreateImage(m_scene->m_width, m_scene->m_height, Format::RGBA16F, 1, true);
        if (!m_ssrTex)
            return false;
    }
    MakeDepthReadable(cmd);
    float params[12] = {m_aoProj.m[2][2], m_aoProj.m[3][2], m_aoProj.m[0][0], m_aoProj.m[1][1],
                        float(m_scene->m_width), float(m_scene->m_height), 48.0f, 80.0f,
                        m_ssr, 0.0f, 0.0f, 0.0f};
    FullscreenPass(cmd, m_ssrTex, m_ssrPipeline, m_depthView, m_scene->m_view, m_pointSampler, params, sizeof(params), false,
                   m_localFraction->m_view);
    Transition(cmd, m_ssrTex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return true;
}

// Contact shadows (contact.frag): a short march towards the sun through the depth buffer at half resolution,
// blurred both ways like the ambient occlusion; m_contactTex[1] holds it. Needs a sun and the world camera.
bool Device::RenderContactShadows(VkCommandBuffer cmd)
{
    bool perspective = m_aoProjValid && m_aoProj.m[2][3] == 1.0f && m_aoProj.m[3][3] == 0.0f;
    const float* sunDir = m_shadowValid ? m_shadowSunDir : m_frameSunDir;
    bool sun = m_frameSunColor[0] + m_frameSunColor[1] + m_frameSunColor[2] > 0.0f;
    if (m_contact <= 0.0f || !perspective || !m_depth || !sun)
        return false;
    uint32_t w = std::max(1u, m_scene->m_width / 2), h = std::max(1u, m_scene->m_height / 2);
    if (!m_contactTex[0] || m_contactTex[0]->m_width != w || m_contactTex[0]->m_height != h) {
        for (Texture*& t : m_contactTex) {
            if (t) DestroyTexture(t);
            t = CreateImage(w, h, Format::RG16F, 1, true);
        }
        if (!m_contactTex[0] || !m_contactTex[1])
            return false;
    }
    MakeDepthReadable(cmd);
    // Towards the sun in view space (the view matrix's rotation; D3D row vectors).
    const auto& v = m_aoView.m;
    float L[3];
    for (int j = 0; j < 3; ++j) L[j] = -(sunDir[0] * v[0][j] + sunDir[1] * v[1][j] + sunDir[2] * v[2][j]);
    float params[16] = {m_aoProj.m[2][2], m_aoProj.m[3][2], m_aoProj.m[0][0], m_aoProj.m[1][1],
                        float(m_scene->m_width), float(m_scene->m_height), 16.0f, 1.0f,
                        L[0], L[1], L[2], 40.0f,
                        m_contact, 0.0f, 0.0f, 0.0f};
    FullscreenPass(cmd, m_contactTex[0], m_contactPipeline, m_depthView, m_depthView, m_pointSampler, params, sizeof(params),
                   false);
    for (int pass = 0; pass < 2; ++pass) {
        Texture* src = m_contactTex[pass];
        Texture* dst = m_contactTex[pass ^ 1];
        Transition(cmd, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        float dir[4] = {pass == 0 ? 1.0f : 0.0f, pass == 0 ? 0.0f : 1.0f, 0.0f, 0.0f};
        FullscreenPass(cmd, dst, m_aoBlurPipeline, src->m_view, src->m_view, m_pointSampler, dir, sizeof(dir), false);
    }
    std::swap(m_contactTex[0], m_contactTex[1]);   // the second pass wrote [0]
    Transition(cmd, m_contactTex[1], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return true;
}

// Temporal anti-aliasing: this frame's camera against the last one's (reprojection for pixels without object motion,
// the sky) and whether the history can be used - the last frame resolved, same size, no teleport.
bool Device::TaaParams(float out[24])
{
    bool perspective = m_aoProjValid && m_aoProj.m[2][3] == 1.0f && m_aoProj.m[3][3] == 0.0f;
    if (!TaaActive() || !perspective || !m_depth)
        return false;
    d3d::Matrix viewProj = MulMatrix(m_aoView, m_aoProj), inverse, reproject{};
    const auto& v = m_aoView.m;
    float jump2 = 0.0f;
    for (int i = 0; i < 3; ++i) {
        float eye = -(v[3][0] * v[i][0] + v[3][1] * v[i][1] + v[3][2] * v[i][2]);
        jump2 += (eye - m_prevEye[i]) * (eye - m_prevEye[i]);
    }
    bool history = m_prevViewProjValid && m_taaFrame + 1 == m_frameNumber && jump2 < 25.0f && m_taaHistory[0] &&
                   m_taaHistory[0]->m_width == m_scene->m_width && m_taaHistory[0]->m_height == m_scene->m_height &&
                   InvertMatrix(viewProj, &inverse);
    if (history)
        reproject = MulMatrix(inverse, m_prevViewProj);
    std::memcpy(out, &reproject, 64);
    out[16] = float(m_scene->m_width);
    out[17] = float(m_scene->m_height);
    out[18] = history ? 1.0f : 0.0f;
    out[19] = m_prevViewProjValid ? 1.0f : 0.0f;      // object motion written this frame (MotionVectorDraw)
    out[20] = 0.1f;                                    // weight of the new frame
    out[21] = m_sharpen;
    out[22] = out[23] = 0.0f;
    return true;
}

// Resolve m_tonemapped against the history into the next history image, then sharpen that into dst.
void Device::RenderTaa(VkCommandBuffer cmd, Texture* dst, const float params[24])
{
    for (Texture*& t : m_taaHistory)
        if (!t || t->m_width != m_scene->m_width || t->m_height != m_scene->m_height) {
            if (t) DestroyTexture(t);
            t = CreateImage(m_scene->m_width, m_scene->m_height, Format::RGBA16F, 1, true);
        }
    if (!m_taaHistory[0] || !m_taaHistory[1])
        return;
    Texture* prev = m_taaHistory[m_taaIndex];
    Texture* next = m_taaHistory[m_taaIndex ^ 1];
    Transition(cmd, m_tonemapped, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Transition(cmd, prev, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Transition(cmd, m_motionVectors, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    MakeDepthReadable(cmd);
    FullscreenPass(cmd, next, m_taaPipeline, m_tonemapped->m_view, prev->m_view, m_linearSampler, params, 96, false,
                   m_motionVectors->m_view, m_depthView);
    Transition(cmd, next, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    float sharpen[4] = {params[21], 0.0f, 0.0f, 0.0f};
    FullscreenPass(cmd, dst, m_sharpenPipeline, next->m_view, next->m_view, m_pointSampler, sharpen, sizeof(sharpen), false);
    m_taaIndex ^= 1;
    m_taaFrame = m_frameNumber;
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
    // The levels carry, in alpha, their light's brightness x 1 / view depth: divided by the brightness, how far away
    // the light is - so the tone mapping can keep the bloom off objects in front of it (foliage before a lamp).
    m_bloomDepth = m_aoProjValid && m_aoProj.m[2][3] == 1.0f && m_aoProj.m[3][3] == 0.0f && m_depth;
    if (m_bloomDepth)
        MakeDepthReadable(cmd);
    Texture* src = m_scene;
    for (size_t i = 0; i < m_bloomLevels.size(); ++i) {
        float params[8] = {1.0f / float(src->m_width), 1.0f / float(src->m_height), m_bloomThreshold, i == 0 ? 1.0f : 0.0f,
                           m_aoProj.m[2][2], m_aoProj.m[3][2], m_bloomDepth ? 1.0f : 0.0f, 0.0f};
        // The first pass adds the glow (additive effects) to the scene's light above the threshold.
        Texture* src2 = i == 0 ? m_glow : src;
        Transition(cmd, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        Transition(cmd, src2, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        FullscreenPass(cmd, m_bloomLevels[i], m_bloomDown, src->m_view, src2->m_view, m_linearSampler, params, 32, false,
                       m_bloomDepth ? m_depthView : VK_NULL_HANDLE);
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

// Per-object motion blur from the motion vectors: strongest motion per tile, of each tile's neighbourhood, then the
// reconstruction filter from m_tonemapped into the main target. The depth buffer is readable (EndScene).
void Device::RenderObjectMotionBlur(VkCommandBuffer cmd, const float params[24])
{
    uint32_t tw = (m_scene->m_width + kMotionTile - 1) / kMotionTile, th = (m_scene->m_height + kMotionTile - 1) / kMotionTile;
    if (!m_motionTiles[0] || m_motionTiles[0]->m_width != tw || m_motionTiles[0]->m_height != th) {
        for (Texture*& t : m_motionTiles) {
            if (t) DestroyTexture(t);
            t = CreateImage(tw, th, Format::RG16F, 1, true);
        }
    }
    if (!m_motionTiles[0] || !m_motionTiles[1]) {
        FullscreenPass(cmd, m_ldrMain, m_motionPipeline, m_tonemapped->m_view, m_depthView, m_pointSampler, params, 96, false);
        return;
    }
    // The blur reaches at most a tile beyond its own: cap the motion at two tiles.
    float p[24];
    std::memcpy(p, params, sizeof(p));
    p[17] = std::min(p[17], float(2 * kMotionTile));
    Transition(cmd, m_motionVectors, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    FullscreenPass(cmd, m_motionTiles[0], m_tileMaxPipeline, m_motionVectors->m_view, m_depthView, m_pointSampler, p, 96, false);
    Transition(cmd, m_motionTiles[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    FullscreenPass(cmd, m_motionTiles[1], m_neighbourMaxPipeline, m_motionTiles[0]->m_view, m_motionTiles[0]->m_view,
                   m_pointSampler, p, 96, false);
    Transition(cmd, m_motionTiles[1], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    FullscreenPass(cmd, m_ldrMain, m_objectBlurPipeline, m_tonemapped->m_view, m_depthView, m_pointSampler, p, 96, false,
                   m_motionVectors->m_view, m_motionTiles[1]->m_view);
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
