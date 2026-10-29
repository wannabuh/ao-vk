// HDR (an enhancement): the 3D scene is drawn into a 16-bit float target, so light the game would clip at 1 - local
// lights given headroom, additive effects piling up - is kept, then tone mapped into the 8-bit main target. The
// interface is drawn after that, into the 8-bit target as before, so it never goes through the tone mapping.
//
// The switch: during the scene phase m_main *is* the float target (every "is this the main target" test keeps
// working); the first pre-transformed (interface) draw after the frame's first 3D draw ends the phase.
#include "internal.h"

#include <algorithm>
#include <cmath>
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
const uint32_t kEnvAccumSpirv[] = {
#include "env_accum.frag.inc"
};
const uint32_t kEnvFilterSpirv[] = {
#include "env_filter.frag.inc"
};
const uint32_t kVolumeSpirv[] = {
#include "volume.frag.inc"
};
const uint32_t kAtmosTransSpirv[] = {
#include "atmos_transmittance.frag.inc"
};
const uint32_t kAtmosMultiSpirv[] = {
#include "atmos_multi.frag.inc"
};
const uint32_t kAtmosViewSpirv[] = {
#include "atmos_view.frag.inc"
};
const uint32_t kVolumeBlurSpirv[] = {
#include "volume_blur.frag.inc"
};
const uint32_t kSandFogSpirv[] = {
#include "sand_fog.frag.inc"
};
const uint32_t kSandFogBlurSpirv[] = {
#include "sand_fog_blur.frag.inc"
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
const uint32_t kDepthUpSpirv[] = {
#include "depth_up.frag.inc"
};
const uint32_t kCacaoPackSpirv[] = {
#include "cacao_pack.frag.inc"
};
const uint32_t kBrixPackSpirv[] = {
#include "brix_pack.frag.inc"
};
const uint32_t kSssrEnvSpirv[] = {
#include "sssr_env.frag.inc"
};
const uint32_t kGbufferSpirv[] = {
#include "gbuffer.frag.inc"
};
const uint32_t kSssrResolveSpirv[] = {
#include "sssr_resolve.frag.inc"
};
const uint32_t kMotionFillSpirv[] = {
#include "motion_fill.frag.inc"
};
const uint32_t kReactiveMaskSpirv[] = {
#include "reactive_mask.frag.inc"
};
const uint32_t kSharpenSpirv[] = {
#include "sharpen.frag.inc"
};
const uint32_t kFxaaSpirv[] = {
#include "fxaa.frag.inc"
};
const uint32_t kUiCompositeSpirv[] = {
#include "ui_composite.frag.inc"
};
const uint32_t kPortalOpenSpirv[] = {
#include "portal_open.frag.inc"
};
const uint32_t kPortalMarkSpirv[] = {
#include "portal_mark.frag.inc"
};
const uint32_t kHandoverSpirv[] = {
#include "handover.frag.inc"
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
    // Normal maps (binding 9): tiled, mipmapped, anisotropic (their detail matters most at grazing angles).
    if (m_maxAnisotropy >= 1.0f) {
        si.anisotropyEnable = VK_TRUE;
        si.maxAnisotropy = m_maxAnisotropy < 16.0f ? m_maxAnisotropy : 16.0f;
    }
    if (!Check(vkCreateSampler(m_device, &si, nullptr, &m_normalSampler), "normal map sampler", error))
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
        // (256: the tone mapping's air (RVK_Atmos), sand haze and gusts - past Vulkan's guaranteed 128; desktop GPUs
        // give 256.)
        VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 256};   // (256: + the storm's distant gusts)
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &set;
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges = &push;
        return Check(vkCreatePipelineLayout(m_device, &pl, nullptr, out), "post pipeline layout", error);
    };
    if (m_props.limits.maxPushConstantsSize < 192)
        Log("post: this GPU allows %u bytes of push constants, the tone mapping needs 192 (RVK_Atmos, sand): expect errors",
            m_props.limits.maxPushConstantsSize);
    // (The tone mapping: its images and, at kTonemapInputs, the frame lights - the lightning's flash.)
    auto tonemapSetLayout = [&](VkDescriptorSetLayout* out) {
        VkDescriptorSetLayoutBinding b[kTonemapInputs + 2];
        for (uint32_t i = 0; i < kTonemapInputs; ++i)
            b[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        b[kTonemapInputs] = {kTonemapInputs, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        // (16: the rain layer - rain.cpp m_rainNear.)
        b[kTonemapInputs + 1] = {kTonemapInputs + 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        sl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        sl.bindingCount = kTonemapInputs + 2;
        sl.pBindings = b;
        return Check(vkCreateDescriptorSetLayout(m_device, &sl, nullptr, out), "tone mapping set layout", error);
    };
    if (!tonemapSetLayout(&m_tonemapSetLayout) || !pipelineLayout(m_tonemapSetLayout, &m_tonemapLayout) ||
        !setLayout(4, &m_bloomSetLayout) || !pipelineLayout(m_bloomSetLayout, &m_bloomLayout))
        return false;
    {
        // Volumetric light: depth, the sun shadow cascades, the clouds' shadow map (clouds.cpp), the terrain's shade
        // from a low sun (terrain_sun.cpp), the frame lights (binding 4, as in the scene shaders); the sand fog's
        // shelter (binding 5, sand_map.cpp SandFogShelter); the point lights' shadow cubes (binding 6: the lamps' light
        // in the air, its shafts).
        VkDescriptorSetLayoutBinding b[7] = {
            {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
        VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        sl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        sl.bindingCount = 7;
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
    // A full-target triangle with one fragment shader, no depth. blend: 0 none, 1 additive (the bloom upsampling), 2
    // a premultiplied image over the target (the interface layer); 3 no colour, the depth written (UpsampleDepth); 5 the
    // same, only where it is greater (a shop portal's box given its depth back, the room drawn keeping its own).
    auto fullscreen = [&](const uint32_t* code, size_t size, VkFormat format, int blend, VkPipelineLayout layout,
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
        if (blend == 1) {
            att.blendEnable = VK_TRUE;
            att.srcColorBlendFactor = att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            att.srcAlphaBlendFactor = att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            att.colorBlendOp = att.alphaBlendOp = VK_BLEND_OP_ADD;
        } else if (blend == 2) {
            att.blendEnable = VK_TRUE;
            att.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;   // the target's alpha stays
            att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            att.colorBlendOp = att.alphaBlendOp = VK_BLEND_OP_ADD;
        } else if (blend == 4) {
            // Premultiplied over, its coverage too: layers put together into one laid over later (frame generation's
            // overlay - with blend 2 it stayed see-through, and the swapchain added the interface onto the frame).
            att.blendEnable = VK_TRUE;
            att.srcColorBlendFactor = att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            att.dstColorBlendFactor = att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            att.colorBlendOp = att.alphaBlendOp = VK_BLEND_OP_ADD;
        }
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        const bool depthOnly = blend == 3 || blend == 5;
        cb.attachmentCount = depthOnly ? 0 : 1;
        cb.pAttachments = &att;
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = depthOnly ? 0 : 1;
        rendering.pColorAttachmentFormats = &format;
        if (depthOnly) {
            rendering.depthAttachmentFormat = kDepthFormat;
            dss.depthTestEnable = dss.depthWriteEnable = VK_TRUE;
            dss.depthCompareOp = blend == 5 ? VK_COMPARE_OP_GREATER : VK_COMPARE_OP_ALWAYS;
        }
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
        bool ok = Check(vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &gi, nullptr, out), "post pipeline", error);
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
              fullscreen(kEnvAccumSpirv, sizeof(kEnvAccumSpirv), hdrFormat, false, m_bloomLayout, &m_envAccumPipeline) &&
              fullscreen(kEnvFilterSpirv, sizeof(kEnvFilterSpirv), hdrFormat, false, m_bloomLayout, &m_envFilterPipeline) &&
              fullscreen(kVolumeSpirv, sizeof(kVolumeSpirv), hdrFormat, false, m_volumeLayout, &m_volumePipeline) &&
              fullscreen(kAtmosTransSpirv, sizeof(kAtmosTransSpirv), hdrFormat, false, m_bloomLayout, &m_atmosTransPipeline) &&
              fullscreen(kAtmosMultiSpirv, sizeof(kAtmosMultiSpirv), hdrFormat, false, m_bloomLayout, &m_atmosMultiPipeline) &&
              fullscreen(kAtmosViewSpirv, sizeof(kAtmosViewSpirv), hdrFormat, false, m_bloomLayout, &m_atmosViewPipeline) &&
              fullscreen(kVolumeBlurSpirv, sizeof(kVolumeBlurSpirv), hdrFormat, false, m_bloomLayout, &m_volumeBlurPipeline) &&
              fullscreen(kSandFogSpirv, sizeof(kSandFogSpirv), hdrFormat, false, m_volumeLayout, &m_sandFogPipeline) &&
              fullscreen(kSandFogBlurSpirv, sizeof(kSandFogBlurSpirv), hdrFormat, false, m_bloomLayout, &m_sandFogBlurPipeline) &&
              fullscreen(kSsrSpirv, sizeof(kSsrSpirv), hdrFormat, false, m_bloomLayout, &m_ssrPipeline) &&
              fullscreen(kGbufferSpirv, sizeof(kGbufferSpirv), hdrFormat, false, m_bloomLayout, &m_gbufferPipeline) &&
              fullscreen(kSssrEnvSpirv, sizeof(kSssrEnvSpirv), hdrFormat, false, m_bloomLayout, &m_sssrEnvPipeline) &&
              fullscreen(kCacaoPackSpirv, sizeof(kCacaoPackSpirv), GetFormatInfo(Format::RG16F).vk, false, m_bloomLayout,
                         &m_cacaoPackPipeline) &&
              fullscreen(kBrixPackSpirv, sizeof(kBrixPackSpirv), hdrFormat, false, m_bloomLayout, &m_brixPackPipeline) &&
              fullscreen(kSssrResolveSpirv, sizeof(kSssrResolveSpirv), hdrFormat, false, m_bloomLayout,
                         &m_sssrResolvePipeline) &&
              fullscreen(kContactSpirv, sizeof(kContactSpirv), GetFormatInfo(Format::RG16F).vk, false, m_bloomLayout,
                         &m_contactPipeline) &&
              fullscreen(kTaaSpirv, sizeof(kTaaSpirv), hdrFormat, false, m_bloomLayout, &m_taaPipeline) &&
              fullscreen(kSharpenSpirv, sizeof(kSharpenSpirv), kColorFormat, false, m_bloomLayout, &m_sharpenPipeline) &&
              fullscreen(kFxaaSpirv, sizeof(kFxaaSpirv), kColorFormat, false, m_bloomLayout, &m_fxaaPipeline) &&
              fullscreen(kUiCompositeSpirv, sizeof(kUiCompositeSpirv), kColorFormat, 2, m_bloomLayout,
                         &m_uiCompositePipeline) &&
              fullscreen(kUiCompositeSpirv, sizeof(kUiCompositeSpirv), kColorFormat, 4, m_bloomLayout,
                         &m_uiOverlayPipeline) &&
              fullscreen(kHandoverSpirv, sizeof(kHandoverSpirv), kColorFormat, 2, m_bloomLayout, &m_handoverPipeline) &&
              fullscreen(kPortalOpenSpirv, sizeof(kPortalOpenSpirv), kColorFormat, 3, m_bloomLayout, &m_portalOpenPipeline) &&
              fullscreen(kPortalOpenSpirv, sizeof(kPortalOpenSpirv), kColorFormat, 5, m_bloomLayout,
                         &m_portalRestorePipeline) &&
              fullscreen(kPortalMarkSpirv, sizeof(kPortalMarkSpirv), GetFormatInfo(Format::RGBA8).vk, false, m_bloomLayout,
                         &m_portalMarkPipeline) &&
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
              fullscreen(kDofFinalSpirv, sizeof(kDofFinalSpirv), hdrFormat, false, m_bloomLayout, &m_dofFinalPipeline) &&
              fullscreen(kDepthUpSpirv, sizeof(kDepthUpSpirv), VK_FORMAT_UNDEFINED, 3, m_bloomLayout, &m_depthUpPipeline) &&
              fullscreen(kMotionFillSpirv, sizeof(kMotionFillSpirv), GetFormatInfo(Format::RG16F).vk, false, m_bloomLayout,
                         &m_motionFillPipeline) &&
              fullscreen(kReactiveMaskSpirv, sizeof(kReactiveMaskSpirv), GetFormatInfo(Format::RGBA8).vk, false,
                         m_bloomLayout, &m_reactiveMaskPipeline);
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
    for (Texture** t : {&m_envProbe[0], &m_envProbe[1], &m_envAtlas})
        if (*t) { DestroyTextureNow(*t); *t = nullptr; }
    m_envReady = m_envEyeValid = false;
    for (Texture*& t : m_volumeTex)
        if (t) { DestroyTextureNow(t); t = nullptr; }
    for (Texture*& t : m_sandFogTex)
        if (t) { DestroyTextureNow(t); t = nullptr; }
    if (m_sandFogShelter) { DestroyTextureNow(m_sandFogShelter); m_sandFogShelter = nullptr; }
    if (m_rainCover) { DestroyTextureNow(m_rainCover); m_rainCover = nullptr; }
    if (m_lightGridTex) { DestroyTextureNow(m_lightGridTex); m_lightGridTex = nullptr; }
    m_rainCoverGen = ~0u;
    if (m_rainNear) { DestroyTextureNow(m_rainNear); m_rainNear = nullptr; }
    m_sandFogShelterGen = ~0u;
    if (m_ssrTex) { DestroyTextureNow(m_ssrTex); m_ssrTex = nullptr; }
    for (Texture*& t : m_lut)
        if (t) { DestroyTextureNow(t); t = nullptr; }
    for (Texture*& t : m_taaHistory)
        if (t) { DestroyTextureNow(t); t = nullptr; }
    for (Texture*& t : m_contactTex)
        if (t) { DestroyTextureNow(t); t = nullptr; }
    if (m_tonemapped) { DestroyTextureNow(m_tonemapped); m_tonemapped = nullptr; }
    if (m_fxaaOut) { DestroyTextureNow(m_fxaaOut); m_fxaaOut = nullptr; }
    if (m_taaOut) { DestroyTextureNow(m_taaOut); m_taaOut = nullptr; }
    DestroyFsr2();
    DestroyFsr3();
    DestroySssr();
    DestroyCacao();
    DestroyVrs();
    DestroyBrix();
    DestroyRt();
    DestroySceneSurfaces();
    if (m_brixGi) { DestroyTextureNow(m_brixGi); m_brixGi = nullptr; }
    if (m_brixGiLast) { DestroyTextureNow(m_brixGiLast); m_brixGiLast = nullptr; }
    if (m_uiLayer) { DestroyTextureNow(m_uiLayer); m_uiLayer = nullptr; }
    if (m_fgLabels) { DestroyTextureNow(m_fgLabels); m_fgLabels = nullptr; }
    if (m_fgOverlay) { DestroyTextureNow(m_fgOverlay); m_fgOverlay = nullptr; }
    if (m_fgShot) { DestroyTextureNow(m_fgShot); m_fgShot = nullptr; }
    if (m_fgTop) { DestroyTextureNow(m_fgTop); m_fgTop = nullptr; }
    m_uiLayerReady.store(false, std::memory_order_relaxed);
    for (Texture*& t : m_motionTiles)
        if (t) { DestroyTextureNow(t); t = nullptr; }
    for (Texture** t : {&m_dofIn, &m_dofOut, &m_dofHalf, &m_dofBlur, &m_dofTiles[0], &m_dofTiles[1], &m_dofFocus[0], &m_dofFocus[1]})
        if (*t) { DestroyTextureNow(*t); *t = nullptr; }
    for (VkPipeline* p : {&m_bloomDown, &m_bloomUp, &m_aoPipeline, &m_aoBlurPipeline, &m_giPipeline, &m_giBlurPipeline,
                          &m_envAccumPipeline, &m_envFilterPipeline, &m_atmosTransPipeline, &m_atmosMultiPipeline,
                          &m_atmosViewPipeline, &m_volumePipeline, &m_volumeBlurPipeline, &m_sandFogPipeline, &m_sandFogBlurPipeline, &m_ssrPipeline,
                          &m_gbufferPipeline, &m_sssrResolvePipeline, &m_cacaoPackPipeline, &m_sssrEnvPipeline, &m_brixPackPipeline,
                          &m_contactPipeline, &m_taaPipeline, &m_sharpenPipeline, &m_fxaaPipeline, &m_uiCompositePipeline,
                          &m_uiOverlayPipeline, &m_handoverPipeline, &m_portalOpenPipeline, &m_portalMarkPipeline,
                          &m_portalRestorePipeline, &m_motionPipeline,
                          &m_tileMaxPipeline, &m_neighbourMaxPipeline, &m_objectBlurPipeline, &m_dofCompositePipeline,
                          &m_dofFocusPipeline, &m_dofPrefilterPipeline, &m_dofTilesPipeline, &m_dofGatherPipeline,
                          &m_dofFinalPipeline, &m_depthUpPipeline, &m_motionFillPipeline,
                          &m_reactiveMaskPipeline})
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
    if (m_normalSampler) vkDestroySampler(m_device, m_normalSampler, nullptr);
    m_normalSampler = VK_NULL_HANDLE;
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
    if (m_depthSwapped) {                        // (RVK_RenderScale: the scene's own depth again)
        SwapDepth();
        m_depthSwapped = false;
    }
    // Temporal anti-aliasing: the scene moves by a sub-pixel offset each frame (Halton 2, 3 over 8 frames; with
    // RVK_RenderScale over 8 / scale^2, FSR 2's rule, so each output pixel still sees about 8 different samples).
    auto halton = [](uint32_t i, uint32_t base) {
        float f = 1.0f, r = 0.0f;
        for (; i; i /= base) { f /= float(base); r += f * float(i % base); }
        return r;
    };
    const float scale = SceneScaleX();
    const uint32_t phases = SceneScaled() ? uint32_t(std::ceil(8.0f / (scale * scale))) : 8u;
    uint32_t k = uint32_t(m_frameNumber % phases) + 1;
    float w = m_scene ? float(m_scene->m_width) : 1.0f, h = m_scene ? float(m_scene->m_height) : 1.0f;
    m_taaJitter[0] = (halton(k, 2) - 0.5f) * 2.0f / w;
    m_taaJitter[1] = (halton(k, 3) - 0.5f) * 2.0f / h;
    m_sceneSaw3D = false;
    m_aoProjValid = false;
    m_sceneClass = 0;
    m_shadeClass = 0;
    m_sceneClassMarks = 0;
    std::swap(m_motionPrev, m_motionCur);        // last frame's objects, for matching this frame's
    m_motionCur.Clear();
    MotionFrame& prev = m_motionPrev;
    prev.index.resize(prev.entries.size());
    for (uint32_t i = 0; i < prev.entries.size(); ++i)
        prev.index[i] = {prev.keys[i], prev.entries[i].world.m[3][0], i};
    std::sort(prev.index.begin(), prev.index.end(), [](const MotionFrame::Index& a, const MotionFrame::Index& b) {
        return a.key != b.key ? a.key < b.key : a.x < b.x;
    });
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
    if (m_native.basePending && m_scenePhase)
        DrawPendingGroundBase();                 // (RVK_GroundOne: no lighting pass came after the ground's base pass)
    if (!m_scenePhase)
        return;
    DrawFx();                                    // our own effects' particles (fx.cpp)
    DrawPortals();                               // RVK_Portals: shops' rooms through their doors (portal.cpp)
    DrawStreetPortal();                          // RVK_StreetView: the street from inside a shop (portal.cpp)
    m_scenePhase = false;
    VkCommandBuffer cmd = m_frames[m_frameIndex].main;
    bool wasMain = m_target == m_scene;
    m_renderEndCause = kEndScene;
    EndRendering();
    ProfileSceneClass(0);                        // close the last scene class for the GPU profile
    m_shadeClass = 0;                            // (its shading query ended with the rendering)
    ProfileMark("scene");
    RenderGrassField(cmd);                       // RVK_GrassOn: our ground grass, over the scene (its own block)
    ProfileMark("ground grass");
    RenderSmoke(cmd);                            // RVK_SmokeOn: the smoke's simulation, and it over the scene
    RenderDebris(cmd);                           // RVK_StormDebris: a storm's blown leaves, straw and twigs
    RenderRain(cmd);                             // RVK_WeatherOn: the rain, over all of it
    double cpu = ProfileCpu();
    Transition(cmd, m_scene, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    bool bloom = m_bloomStrength > 0.0f;
    if (bloom)
        RenderBloom(cmd);
    ProfileMark("bloom");
    if (m_rt && !RtWanted())
        DestroyRt();
    const bool rays = RtWanted() && RtUpdate(cmd);   // (the ray tracing's structures, for the passes after)
    ProfileMark("ray tracing");
    // RVK_AoMode: 1 AMD's, 2 ray traced (ours where they can't run).
    m_aoFull = m_aoMode >= 1.5f ? rays && RenderRtAo(cmd) : m_aoMode >= 0.5f && RenderCacao(cmd);
    bool ao = m_aoFull || RenderAo(cmd);
    ProfileMark("ao");
    if (m_brix && !BrixWanted())
        DestroyBrix();                           // (RVK_GiMode back to 0: its field's memory goes)
    m_giFull = BrixWanted() && RenderBrixGi(cmd);   // (RVK_GiMode 1: AMD's; ours where it can't run)
    bool gi = m_giFull || RenderGi(cmd);
    ProfileMark("gi");
    if (rays && m_rtDebug >= 0.5f)
        RenderRtDebug(cmd);                      // (RVK_RtDebug: over the scene)
    RenderEnvProbe(cmd);
    ProfileMark("environment probe");
    bool volume = RenderVolume(cmd);
    ProfileMark("volumetric");
    m_sandFogValid = RenderSandFog(cmd);
    ProfileMark("sand fog");
    Transition(cmd, m_localFraction, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    // (RVK_SsrMode 1: AMD's; ours where they can't run. Only then: the scene's normals are made for AMD's GI and the
    // traced AO too, and with them on AMD's reflections had taken over from ours.)
    bool ssr = m_ssrMode >= 0.5f && SceneNormalsWanted() && RenderSssr(cmd);
    if (!ssr) {
        const bool traced = rays && RtReflectWanted() && RtReflectReady();   // (RVK_RtReflect: rays where ours miss)
        ssr = RenderSsr(cmd, traced);
        if (ssr && traced)
            RenderRtReflect(cmd);
    }
    ProfileMark("reflections");
    bool contact = RenderContactShadows(cmd);
    ProfileMark("contact shadows");
    MeasureSceneLuma(cmd);                       // (darkness.cpp: the land's brightness, for RVK_Darkness's floor)
    Transition(cmd, m_albedo, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    // The upsampled bloom chain sums every level's light (weighted: RenderBloom): averaged here.
    // params[3]: how much bloom reaches objects nearer than its light (-1: the bloom carries no depth).
    // params[8..15]: colour grading (identity lookup tables until the proxy loads others).
    for (Texture*& lut : m_lut)
        if (!lut) lut = CreateLut(16, nullptr);
    // (With the light rig its land exposure takes the darkness floor's place: tonemap.frag.)
    float params[16] = {m_tonemapKnee, m_exposure * (m_rig.on ? m_rig.meter : m_darkGain), bloom ? m_bloomStrength / m_bloomWeightSum : 0.0f,
                        m_bloomDepth ? m_bloomOverNearer : -1.0f, m_aoProj.m[2][2], m_aoProj.m[3][2],
                        gi ? m_giStrength : 0.0f,
                        float((volume ? 1 : 0) | (ssr ? 2 : 0) | (ao ? 4 : 0) | (contact ? 8 : 0) | (m_aoFull ? 32 : 0) |
                              (gi && m_giFull ? 64 : 0))};
    GradingParams(params + 8);
    // Depth of field blurs the scene with its ambient occlusion and indirect light applied (they belong to its
    // surfaces); the tone mapping then leaves them out. Not the volumetric light: it is in the air in front, and the
    // tone mapping adds it after the haze, the mist and the storm's air - in the scene before them, a lamp's glow near
    // the camera was hazed away as if it lay at the land behind it (the user: the trees behind a character's own light
    // stood dark in its glow, sharp-edged, with RVK_Dof on).
    float dofParams[16];
    std::memcpy(dofParams, params, sizeof(dofParams));
    dofParams[7] = float(int(dofParams[7]) & ~1);
    bool dof = RenderDof(cmd, bloom, ao, gi, false, dofParams);
    ProfileMark("dof");
    Texture* scene = dof ? m_dofOut : m_scene;
    if (dof) {
        params[6] = 0.0f;
        params[7] = float(volume ? 1 : 0);
    }
    // With motion blur or anti-aliasing the tone mapping goes to an intermediate image: the anti-aliasing resolves
    // it (and sharpens it back into it, or into the main target), the blur reads it into the main target.
    // RVK_RenderScale: the tone mapping at the scene's size, the TAA bringing it up to the output's (with motion
    // blur into m_taaOut, which the blur then reads).
    const bool scaled = SceneScaled();
    float taaParams[28];
    bool taa = TaaParams(taaParams);
    float motion[28];
    bool blur = MotionBlurParams(motion);
    bool fxaa = m_fxaa && !taa && m_fxaaPipeline;    // (TAA anti-aliases already)
    if ((blur || taa || fxaa) &&
        (!m_tonemapped || m_tonemapped->m_width != m_scene->m_width || m_tonemapped->m_height != m_scene->m_height)) {
        if (m_tonemapped) DestroyTexture(m_tonemapped);
        m_tonemapped = CreateImage(m_scene->m_width, m_scene->m_height, Format::A8R8G8B8, 1, true);
        blur = blur && m_tonemapped;
        taa = taa && m_tonemapped;
        fxaa = fxaa && m_tonemapped;
    }
    if (scaled && blur &&
        (!m_taaOut || m_taaOut->m_width != m_ldrMain->m_width || m_taaOut->m_height != m_ldrMain->m_height)) {
        if (m_taaOut) DestroyTexture(m_taaOut);
        m_taaOut = CreateImage(m_ldrMain->m_width, m_ldrMain->m_height, Format::A8R8G8B8, 1, true);
        blur = blur && m_taaOut;
    }
    Texture* blurSrc = scaled ? m_taaOut : m_tonemapped;   // what the motion blur reads
    if (fxaa && blur &&
        (!m_fxaaOut || m_fxaaOut->m_width != m_ldrMain->m_width || m_fxaaOut->m_height != m_ldrMain->m_height)) {
        if (m_fxaaOut) DestroyTexture(m_fxaaOut);
        m_fxaaOut = CreateImage(m_ldrMain->m_width, m_ldrMain->m_height, Format::A8R8G8B8, 1, true);
        fxaa = fxaa && m_fxaaOut;
    }
    Texture* toned = blur || taa || fxaa ? m_tonemapped : m_ldrMain;
    // With TAA the tone mapping's alpha says how much of each pixel additive effects made (taa.frag: those pixels
    // take more of the new frame - sparks and spell effects, new each frame, were averaged away).
    if (taa) params[7] = float(int(params[7]) | 16);
    TonemapInputsPass(cmd, toned, m_tonemapPipeline, scene, bloom, ao, gi, volume, params);
    ProfileMark("tonemap");
    // (RVK_Upscaler 1: FSR 2 upscales the scaled scene - fsr2.cpp; 2: FSR 3.1 - fsr3.cpp; the TAA if they can't.)
    Texture* upscaled = blur ? blurSrc : m_ldrMain;
    if (taa && !(scaled && m_upscaler == 1 && RenderFsr2(cmd, upscaled, taaParams)) &&
        !(scaled && m_upscaler == 2 && RenderFsr3(cmd, upscaled, taaParams)))
        RenderTaa(cmd, upscaled, taaParams);
    if (taa && m_fg && !HandoverShown())
        FgPrepare(cmd, taaParams);               // (RVK_FrameGen: the scene's depth and motion for the generated frame;
                                                 // none under a zone change's held picture - handover.cpp)
    if (fxaa) {
        // Into the main target, or - with motion blur - into an image of its own that the blur then reads (the two
        // swap: m_tonemapped is what the blur reads).
        Transition(cmd, m_tonemapped, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        const float size[4] = {1.0f / float(m_tonemapped->m_width), 1.0f / float(m_tonemapped->m_height), 0.0f, 0.0f};
        FullscreenPass(cmd, blur ? m_fxaaOut : m_ldrMain, m_fxaaPipeline, m_tonemapped->m_view, m_tonemapped->m_view,
                       m_linearSampler, size, sizeof(size), false);
        if (blur) std::swap(m_tonemapped, m_fxaaOut);
    }
    ProfileMark("taa + sharpen");
    if (scaled)
        UpsampleDepth(cmd);                      // (the blur, name labels and the interface: the output's depth)
    ProfileMark("depth upsampling");
    if (blur) {
        Transition(cmd, blurSrc, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        MakeDepthReadable(cmd);
        if (m_motionMode == 1)
            RenderObjectMotionBlur(cmd, blurSrc, motion);
        else
            FullscreenPass(cmd, m_ldrMain, m_motionPipeline, blurSrc->m_view, m_depthView, m_pointSampler, motion,
                           sizeof(motion), false);
    }
    ProfileMark("motion blur");
    HandoverScene(cmd);                          // RVK_ZoneFade: a zone change's held picture over the scene
    // (A frame dump: the in-between images too - not every frame of a run.)
    if (m_dumpFile && m_dumpBurstIndex == 0 && !m_dumpOpenPath.empty())
        DumpTargets(m_dumpOpenPath.substr(0, m_dumpOpenPath.rfind('.')));
    ProfileCpuAdd("post passes", cpu);
    m_cache = StateCache{};                      // pipeline, viewport and scissor changed
    // The post passes pushed set 0 with their own pipeline layouts, so the scene's pushed draw bindings are no
    // longer valid for the interface draws that follow: the first one pushes them again (M2: bindings 0 and 12).
    m_arenaBound = false;
    m_bindlessBound = false;
    m_main = m_ldrMain;
    if (wasMain)
        m_target = m_ldrMain;
    m_constantsDirty = true;
    if (m_inFrame)
        BeginRenderingOn(m_target);
    if (m_inFrame && m_target == m_ldrMain && !HandoverOpaque()) {   // (not the new zone's labels on the old picture)
        // (RVK_FrameGen: into a layer the swapchain lays over the frames - interpolated, they smeared.)
        if (m_fgUiLayer && !m_heldLabels.empty() && FgLabelsBegin()) {
            DrawHeldLabels();
            FgLabelsEnd();
        } else {
            DrawHeldLabels();
        }
    }
    m_heldLabels.clear();                        // (the scene ended elsewhere: not drawn this frame)
}

// The name labels held back during the scene (Draw), onto the final picture: each drawn as the game drew it - its
// state put back for the draw, the game's current state after.
void Device::DrawHeldLabels()
{
    if (m_heldLabels.empty())
        return;
    std::vector<HeldLabel> labels;
    labels.swap(m_heldLabels);
    const auto rs = m_rs;
    const auto tss = m_tss;
    const auto textures = m_textures;
    const d3d::Matrix world = m_world, view = m_view, proj = m_proj;
    const auto texMatrix = m_texMatrix;
    const d3d::Viewport viewport = m_viewport;
    m_drawingHeldLabels = true;
    for (const HeldLabel& l : labels) {
        if (!l.textures[0])
            continue;                            // (its text freed since: nothing to draw)
        m_rs = l.rs;
        m_tss = l.tss;
        m_textures = l.textures;
        m_world = l.world;
        m_view = l.view;
        m_proj = l.proj;
        m_texMatrix = l.texMatrix;
        m_viewport = l.viewport;
        m_constantsDirty = true;
        Draw(l.primitive, l.fvf, l.vertices.data(), l.vertexCount, nullptr, 0);
    }
    m_drawingHeldLabels = false;
    m_rs = rs;
    m_tss = tss;
    m_textures = textures;
    m_world = world;
    m_view = view;
    m_proj = proj;
    m_texMatrix = texMatrix;
    m_viewport = viewport;
    m_constantsDirty = true;
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
    Transition(cmd, m_glow, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);   // (binding 12: the effects' share, for TAA)
    if (m_rainNear && m_rainNearFrame == m_frameNumber)
        Transition(cmd, m_rainNear, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);   // (binding 16: the rain layer)
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
    Texture* occlusion = ao ? (m_aoFull ? m_aoFullTex : m_aoTex[1]) : m_scene;   // unread when off
    Texture* indirect = gi ? (m_giFull ? m_brixGi : m_giTex[1]) : m_scene;                // unread when off
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
                                       {m_linearSampler, m_lut[1]->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                       {m_pointSampler, m_glow->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                       {m_linearSampler, SmokeCoverValid() ? m_smokeHalf->m_view : m_scene->m_view,
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                       {m_pointSampler, m_sandFogValid ? m_sandFogTex[1]->m_view : m_scene->m_view,
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    static_assert(sizeof(images) / sizeof(images[0]) == kTonemapInputs, "occlusion.glsl bindings");
    VkWriteDescriptorSet w[kTonemapInputs + 2] = {};
    for (uint32_t i = 0; i < kTonemapInputs; ++i) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[i].pImageInfo = &images[i];
    }
    // The frame lights (tonemap.frag: the lightning's flash).
    Frame& frame = m_frames[m_frameIndex];
    FillFrameLights(static_cast<FrameLights*>(frame.postData), false);
    VkDescriptorBufferInfo lights{frame.post, 0, sizeof(FrameLights)};
    w[kTonemapInputs].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[kTonemapInputs].dstBinding = kTonemapInputs;
    w[kTonemapInputs].descriptorCount = 1;
    w[kTonemapInputs].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w[kTonemapInputs].pBufferInfo = &lights;
    const bool rainNear = m_rainNear && m_rainNearFrame == m_frameNumber;   // (made readable before the rendering)
    VkDescriptorImageInfo rainInfo{m_pointSampler, rainNear ? m_rainNear->m_view : m_scene->m_view,   // (unread without)
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    w[kTonemapInputs + 1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[kTonemapInputs + 1].dstBinding = kTonemapInputs + 1;
    w[kTonemapInputs + 1].descriptorCount = 1;
    w[kTonemapInputs + 1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[kTonemapInputs + 1].pImageInfo = &rainInfo;
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_tonemapLayout, 0, kTonemapInputs + 2, w);
    float push[64] = {};
    std::memcpy(push, params, 16 * sizeof(float));
    if (depthView == m_depthView && m_aoProjValid) {
        push[7] = float(int(push[7]) | 16);      // (the depth is the scene's: the grading's distance, the land's haze)
        LandHaze(push + 16);
    }
    SandHaze(push + 40);                         // (wind.cpp: a sand or ash storm's air)
    SandGusts(push + 48);                        // (... its distant gusts)
    vkCmdPushConstants(cmd, m_tonemapLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

// The haze over the distant land (RVK_VolLand; tonemap.frag): out[0..3] its colour - the game's fog colour (what the
// far land fades into) cooled and a little lighter, as the air's blue - and how much; out[4..7] the game's fog start
// and end (view depth: from there the game's own fog takes over), 0 0 if none. Off (strength 0) without the fog.
void Device::LandHaze(float out[24]) const
{
    for (int k = 0; k < 24; ++k) out[k] = 0.0f;
    float fog[4], fogParams[4];
    SceneFog(fog, fogParams);
    const bool linear = fog[3] > 0.5f && uint32_t(fogParams[3]) == 3 && fogParams[1] > fogParams[0] && fogParams[1] > 1.0f;
    if (linear) {                                // (also without the haze: the ambient occlusion fades into the fog)
        out[4] = fogParams[0];
        out[5] = fogParams[1];
    }
    // (The view rays' scale also without the haze: the tone mapping's far fog takes the twilight's colour along them.
    // Left 0 - the sun unknown outdoors, after leaving a shop - its rays were inf / inf, and the land went black.)
    out[8] = m_aoProj.m[0][0];
    out[9] = m_aoProj.m[1][1];
    if (m_landHaze <= 0.0f || !linear || !SunKnown())   // (indoors: no distant land, no outdoor air - the zone's fog)
        return;
    const float l = 0.3f * fog[0] + 0.59f * fog[1] + 0.11f * fog[2];
    // (A desert's dust, RVK_DesertAir: the haze warm and pale instead of cool and blue, and thicker.)
    float desert[2];
    DesertAir(desert);
    const float cool[3] = {0.78f, 0.95f, 1.25f}, dusty[3] = {1.1f, 1.0f, 0.82f};
    for (int k = 0; k < 3; ++k) {
        const float tint = cool[k] + (dusty[k] - cool[k]) * desert[0];
        out[k] = std::max(0.0f, fog[k] + (l * tint - fog[k]) * (0.45f + 0.2f * desert[0])) * (1.08f + 0.06f * desert[0]);
    }
    out[3] = std::clamp(m_landHaze, 0.0f, 1.0f);
    out[4] = fogParams[0];
    out[5] = fogParams[1];
    MistParams(out + 6);                         // (the valley mist: its top over the eye, its density)
    // The air (RVK_Atmos; tonemap.frag Air): out[8..23]. Its density at the eye: about 45% hazed at the game's fog
    // end on the level, more with the sky's cloud and rain (sky.cpp); its scale height; the sun's glow.
    if (m_atmos) {
        const auto& v = m_aoView.m;
        const float vary = std::clamp(m_cloudParams.vary, 0.0f, 1.0f);
        const float weather = (1.0f + (0.7f + 0.7f * SkyCloudiness() - 1.0f) * vary) * (1.0f + StormSky());
        // The morning's haze (RVK_MornHaze): thicker round sunrise - the sun from ~6 degrees under the horizon to
        // ~15 up -, half as much round sunset (docs/twilight-plan.md).
        float morning = 0.0f;
        {
            const float up = SunUp();
            morning = Smoothstep(-0.1f, -0.02f, up) * (1.0f - Smoothstep(0.1f, 0.26f, up)) *
                      (0.5f + 0.5f * m_sunRising) * std::max(m_mornHaze, 0.0f);
        }
        out[10] = 0.6f / fogParams[1] * weather * (1.0f + morning) * (1.0f + 0.8f * desert[0]);
        out[11] = 150.0f;
        // (D3D row vectors: a world direction's view-space form is d.x * row 0 + d.y * row 1 + d.z * row 2.)
        for (int k = 0; k < 3; ++k) out[12 + k] = v[1][k];
        out[15] = 0.35f;                         // (0.6 washed the far land out round a rising sun - user)
        // Towards the sun, or by night the moon; their light through the air.
        const bool moon = MoonOn() && m_frameSunColor[0] + m_frameSunColor[1] + m_frameSunColor[2] <= 0.0f;
        float to[3], light[3];
        for (int k = 0; k < 3; ++k) {
            to[k] = moon ? -m_moonDir[k] : -m_trueSunDir[k];
            light[k] = moon ? m_moonColour[k] : m_frameSunColor[k];
        }
        const float tl = std::max(std::sqrt(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]), 1e-6f);
        for (int k = 0; k < 3; ++k) to[k] /= tl;
        for (int k = 0; k < 3; ++k) out[16 + k] = to[0] * v[0][k] + to[1] * v[1][k] + to[2] * v[2][k];
        out[19] = 0.75f;
        const float low = 1.0f - std::clamp(to[1] / 0.3f, 0.0f, 1.0f), low2 = low * low;
        const float tint[3] = {1.0f, 1.0f - 0.35f * low2, 1.0f - 0.6f * low2};
        // (A low sun's light through much air: a third of it at the horizon, all from ~17 degrees.)
        const float through = 0.35f + 0.65f * std::clamp(to[1] / 0.3f, 0.0f, 1.0f);
        // (The sun's glow in the air, as the eye stands: none under the mountains hiding a low sun - terrain_sun.cpp.)
        const float terrain = moon ? 1.0f : 1.0f + (m_tsEye - 1.0f) * m_tsWeight;
        // (Nor with the clouds hiding the sun, at any height - the light they let through towards it (MakeCloudSunRay)
        // and a storm's deck: the frame's sun is only dimmed by them low, and behind an overcast at ~11 degrees the far
        // land and the valley mist glowed white towards it - user, 2026-10-18.)
        const float clouds = moon ? 1.0f : std::clamp(m_sunThrough, 0.0f, 1.0f) * (1.0f - StormOvercast());
        for (int k = 0; k < 3; ++k)
            out[20 + k] = light[k] * tint[k] * (moon ? 1.0f : 0.5f * through) * terrain * clouds;
    }
    if (m_frameNumber % 600 == 3)
        Log("land haze: colour %.2f %.2f %.2f x %.2f, the game's fog %.0f..%.0f; desert air %.2f, shimmer %.2f (sun %.2f up, "
            "clouds %.2f)", double(out[0]), double(out[1]), double(out[2]), double(out[3]), double(out[4]), double(out[5]),
            double(desert[0]), double(desert[1]), double(SunUp()), double(m_cloudParams.enable ? SkyCloudiness() : 0.0f));
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
                            VkImageView src4, const VkRect2D* scissorRect)
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
    VkRect2D scissor = scissorRect ? *scissorRect : VkRect2D{{0, 0}, {dst->m_width, dst->m_height}};
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

// Indirect light (gi.frag): one bounce of the lit HDR scene, gathered at quarter resolution from the depth buffer and the
// scene, blurred both ways (depth-aware); m_giTex[1] holds it. Like the AO, needs the world camera's projection.
bool Device::RenderGi(VkCommandBuffer cmd)
{
    if (m_giStrength <= 0.0f || !m_aoProjValid || m_aoProj.m[2][3] != 1.0f || m_aoProj.m[3][3] != 0.0f || !m_depth)
        return false;
    uint32_t w = std::max(1u, (m_scene->m_width + 3) / 4), h = std::max(1u, (m_scene->m_height + 3) / 4);
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
                       m_giRadius, FrameNoise(), float(m_scene->m_width), float(m_scene->m_height)};
    // (The surface colours too: their alpha marks the native ground's pebbles, which the gathering steps round.)
    if (m_albedo) Transition(cmd, m_albedo, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    FullscreenPass(cmd, m_giTex[0], m_giPipeline, m_depthView, m_scene->m_view, m_pointSampler, params, sizeof(params), false,
                   m_albedo ? m_albedo->m_view : VK_NULL_HANDLE);
    // Two blurs each way (quarter resolution): wide taps, then the gaps between them.
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

// The environment probe (env_accum.frag, env_filter.frag; rvk.h m_envProbe): the scene just drawn goes into the
// direction map around the camera, which is then filtered into the atlas the next frame's PBR materials reflect.
bool Device::RenderEnvProbe(VkCommandBuffer cmd)
{
    bool perspective = m_aoProjValid && m_aoProj.m[2][3] == 1.0f && m_aoProj.m[3][3] == 0.0f;
    if (!m_pbr.enabled || m_pbr.probe <= 0.0f || !perspective || !m_depth || !m_envAccumPipeline) {
        m_envEyeValid = false;
        return false;
    }
    if (!m_envAtlas) {
        for (Texture*& t : m_envProbe)
            t = CreateImage(128, 128, Format::RGBA16F, 1, true);
        m_envAtlas = CreateImage(192, 128, Format::RGBA16F, 1, true);
        if (!m_envProbe[0] || !m_envProbe[1] || !m_envAtlas)
            return false;
    }
    const auto& vm = m_aoView.m;
    float eye[3];
    for (int i = 0; i < 3; ++i)
        eye[i] = -(vm[3][0] * vm[i][0] + vm[3][1] * vm[i][1] + vm[3][2] * vm[i][2]);
    // Sureness fades as the camera moves away from where it saw things (half of it in ~140 units) and slowly with
    // time (half in ~25 s at 60 fps: the light changes); a jump (a teleport, a zone) forgets everything.
    float moved = 0.0f;
    if (m_envEyeValid)
        moved = std::sqrt((eye[0] - m_envEye[0]) * (eye[0] - m_envEye[0]) + (eye[1] - m_envEye[1]) * (eye[1] - m_envEye[1]) +
                          (eye[2] - m_envEye[2]) * (eye[2] - m_envEye[2]));
    const bool forget = !m_envEyeValid || moved > 40.0f;
    std::memcpy(m_envEye, eye, sizeof(eye));
    m_envEyeValid = true;
    // A PBR debug view colours the scene: the probe would learn those colours and show them back. It keeps what it had.
    if (m_pbr.debug != 0 && m_envReady)
        return true;
    MakeDepthReadable(cmd);
    struct {
        d3d::Matrix viewProj;
        float proj[4];
        float params[4];
    } accum{MulMatrix(m_aoView, m_aoProj),
            {m_aoProj.m[2][2], m_aoProj.m[3][2], float(m_scene->m_width), float(m_scene->m_height)},
            {0.12f, 6.0f, std::exp(-moved / 200.0f) * 0.99954f, forget ? 1.0f : 0.0f}};
    Texture* prev = m_envProbe[m_envCurrent];
    Texture* next = m_envProbe[m_envCurrent ^ 1];
    if (prev->m_layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        if (!forget)
            return false;                        // (never written: only a forgetting pass may read it)
        Transition(cmd, prev, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    FullscreenPass(cmd, next, m_envAccumPipeline, prev->m_view, m_scene->m_view, m_linearSampler, &accum.viewProj.m[0][0],
                   sizeof(accum), false, m_depthView);
    Transition(cmd, next, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    m_envCurrent ^= 1;
    float filter[4] = {FrameNoise(), 0.0f, 0.0f, 0.0f};
    FullscreenPass(cmd, m_envAtlas, m_envFilterPipeline, next->m_view, next->m_view, m_linearSampler, filter,
                   sizeof(filter), false);
    Transition(cmd, m_envAtlas, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    m_envReady = true;
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
    push.shafts[1] = CloudShadowView() != m_blackTexture->m_view ? m_volumeRays : 0.0f;   // (clouds casting shadows)
    push.shafts[2] = 3000.0f;                                     // the rays' reach
    // The sunset rays (RVK_DuskRays): their strength - negative without this frame's clouds (binding 5: none then).
    const bool cover = m_cloudCover && m_cloudCoverFrame == m_frameNumber &&
                       m_cloudCover->m_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    push.shafts[3] = cover ? DuskRaysNow() : -DuskRaysNow();
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
    VkDescriptorImageInfo clouds{m_linearSampler, CloudShadowView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const bool terrainMap = m_terrainSunMap && m_terrainSunMap->m_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo terrain{m_linearSampler, terrainMap ? m_terrainSunMap->m_view : m_blackTexture->m_view,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo coverInfo{m_linearSampler, cover ? m_cloudCover->m_view : m_blackTexture->m_view,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo cubes{m_cubeSampler, m_cubeArrayView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr[7] = {};
    for (auto& x : wr) { x.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; x.descriptorCount = 1; }
    wr[0].dstBinding = 0; wr[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr[0].pImageInfo = &depth;
    wr[1].dstBinding = 1; wr[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr[1].pImageInfo = &shadow;
    wr[2].dstBinding = 4; wr[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; wr[2].pBufferInfo = &lights;
    wr[3].dstBinding = 2; wr[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr[3].pImageInfo = &clouds;
    wr[4].dstBinding = 3; wr[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr[4].pImageInfo = &terrain;
    wr[5].dstBinding = 5; wr[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr[5].pImageInfo = &coverInfo;
    wr[6].dstBinding = 6; wr[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr[6].pImageInfo = &cubes;
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_volumeLayout, 0, m_cubeArrayView ? 7 : 6, wr);
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

// A sand or ash storm's air as a lit volume (sand_fog.frag, RVK_SandFog; docs/sand-fog-plan.md): marched at half
// resolution, blurred both ways (sand_fog_blur.frag, depth-aware); m_sandFogTex[1] holds its light and transmittance
// for the tone mapping (occlusion.glsl binding 14), in place of the flat haze.
bool Device::RenderSandFog(VkCommandBuffer cmd)
{
    bool perspective = m_aoProjValid && m_aoProj.m[2][3] == 1.0f && m_aoProj.m[3][3] == 0.0f;
    d3d::Matrix inverse;
    if (!SandFogOn() || !perspective || !m_depth || !InvertMatrix(MulMatrix(m_aoView, m_aoProj), &inverse))
        return false;
    uint32_t w = std::max(1u, m_scene->m_width / 2), h = std::max(1u, m_scene->m_height / 2);
    if (!m_sandFogTex[0] || m_sandFogTex[0]->m_width != w || m_sandFogTex[0]->m_height != h) {
        for (Texture*& t : m_sandFogTex) {
            if (t) DestroyTexture(t);
            t = CreateImage(w, h, Format::RGBA16F, 1, true);
        }
        if (!m_sandFogTex[0] || !m_sandFogTex[1])
            return false;
    }
    MakeDepthReadable(cmd);
    Frame& frame = m_frames[m_frameIndex];
    FillFrameLights(static_cast<FrameLights*>(frame.postData), false);

    float gusts[16];
    SandGusts(gusts);                            // (the eye's ground, the wind's frame and time; zeros without gusts)
    struct {
        d3d::Matrix invViewProj;
        float eye[4], proj[4], dens[4], albedo[4], sky[4], terrain[4], wind[4], gusts[4], sun[4], shelter[4];
    } push{};
    static_assert(sizeof(push) <= 256, "push constant range");
    push.invViewProj = inverse;
    const auto& v = m_aoView.m;
    for (int i = 0; i < 3; ++i) push.eye[i] = -(v[3][0] * v[i][0] + v[3][1] * v[i][1] + v[3][2] * v[i][2]);
    push.eye[3] = 400.0f;                                         // the march's reach
    push.proj[0] = m_aoProj.m[2][2];
    push.proj[1] = m_aoProj.m[3][2];
    push.proj[2] = float(m_scene->m_width);
    push.proj[3] = float(m_scene->m_height);
    // (Its density at the ground: the haze's - tonemap.frag's dust.w - so the distance is as buried. The dust: a storm
    // wall 120 units high, the sky buried overhead too; a thunderstorm's rain mist deeper and with little high up.)
    const StormAir air = StormAirNow();
    const float rainShare = 1.0f - air.sandShare;
    push.dens[0] = air.sigma;
    push.dens[1] = air.height;
    float ground;
    push.dens[2] = gusts[3] != 0.0f ? gusts[3] : (GroundAt(push.eye[0], push.eye[2], &ground, nullptr, nullptr) ? ground : push.eye[1] - 2.0f);
    push.dens[3] = std::max(m_sandClear, 0.0f);
    if (m_terrainAmbient[3] > 0.5f) {
        for (int i = 0; i < 3; ++i) push.sky[i] = m_terrainAmbient[i];
    } else {
        const uint32_t a = m_rs[d3d::RS_AMBIENT];
        push.sky[0] = float((a >> 16) & 0xFF) / 255.0f;
        push.sky[1] = float((a >> 8) & 0xFF) / 255.0f;
        push.sky[2] = float(a & 0xFF) / 255.0f;
    }
    // The dust's colour: such that, as typically lit (sand_fog.frag: the sky's light, the moon's, the sun's through
    // the dust above halfway up the thick layer and what it lost spread round), it is the haze's - the storm's
    // colour, by night the haze's dimmer one. Its light and shade - the glow towards the sun, the dust shaded by
    // dunes, a dusk - stay as variation about it. (Else, as the sim sand before storms40: a darker, greyer dust,
    // and by night dark grey where the land is sandy - the user.)
    float haze[8];
    SandHaze(haze);
    const bool moonShadow = m_shadowValid && m_shadowIsMoon;
    const float sunUp = std::max(-m_frameSunDir[1], 0.02f);
    const float kRedden[3] = {0.85f, 1.0f, 1.25f};               // (sand_fog.frag's: the dust takes blue first)
    for (int i = 0; i < 3; ++i) {
        const float through = std::exp(-kRedden[i] * push.dens[0] * (1.0f - air.upper) * push.dens[1] * 0.5f / sunUp);   // (the thick layer)
        const float sun = m_shadowValid ? m_shadowSunColor[i] : m_frameSunColor[i];
        const float moon = moonShadow ? 0.0f : m_moonColour[i] * 0.6f;
        const float typical = std::max(push.sky[i] + moon + sun * (through + 0.8f * std::max(-m_frameSunDir[1], 0.0f) * (1.0f - through)), 0.02f);
        push.albedo[i] = std::min(haze[i] / typical, 4.0f);
    }
    // (The rain mist: a light grey-blue as lit - its own light, not calibrated to a tint; the dust's share as above.)
    const float kMist[3] = {0.5f, 0.53f, 0.6f};   // (a storm's distance dark grey, not a bright veil)
    if (rainShare > 0.0f) {
        float green[3], dust[3];
        StormGreen(green);                       // (the mist cast green with the storm: RVK_StormGreen)
        float mist[3];
        // (Dimmed with the air under the storm's deck - StormAirColour: the far land through it stood pale under the sky.)
        const float dim = StormAirDim();
        for (int i = 0; i < 3; ++i) mist[i] = kMist[i] * green[i] * dim;
        for (int i = 0; i < 3; ++i) dust[i] = air.sandShare > 0.0f ? push.albedo[i] : mist[i];
        for (int i = 0; i < 3; ++i) push.albedo[i] = mist[i] + (dust[i] - mist[i]) * air.sandShare;
    }
    push.albedo[3] = float(m_frameNumber % 64) * 0.618034f;      // the march's offsets move on every frame
    // Steps (the blur and TAA average the jitter): the sand 24, the rain mist - smooth, thin - 12.
    push.sky[3] = std::round(12.0f + 12.0f * std::clamp(air.sandShare * 2.0f, 0.0f, 1.0f));
    const bool terrainMap = m_terrainSunMap && m_terrainSunMap->m_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
                            m_tsMadeFrame != ~0ull;
    if (terrainMap) {
        push.terrain[0] = m_tsMapOrigin[0];
        push.terrain[1] = m_tsMapOrigin[1];
        push.terrain[2] = 1.0f / (16.0f * float(m_terrainSunMap->m_width));   // (terrain_sun.cpp: 16 units a cell)
        const float sunNow[3] = {-m_trueSunDir[0], -m_trueSunDir[1], -m_trueSunDir[2]};
        const float l = std::sqrt(sunNow[0] * sunNow[0] + sunNow[1] * sunNow[1] + sunNow[2] * sunNow[2]);
        const float dot = l > 1e-6f ? (sunNow[0] * m_tsSunUsed[0] + sunNow[1] * m_tsSunUsed[1] + sunNow[2] * m_tsSunUsed[2]) / l : 0.0f;
        push.terrain[3] = dot > 0.999f ? 2.0f : 1.0f;             // (its sun shade only as made for this sun)
    }
    for (int i = 0; i < 4; ++i) push.wind[i] = gusts[12 + i];
    if (m_frameNumber % 600 == 0)
        Log("sand fog: density %.4f, scale height %.0f, ground %.1f (eye y %.1f), sky light %.2f %.2f %.2f, dust colour %.2f %.2f "
            "%.2f (x the light), terrain map %.0f", double(push.dens[0]), double(push.dens[1]), double(push.dens[2]), double(push.eye[1]),
            double(push.sky[0]), double(push.sky[1]), double(push.sky[2]), double(push.albedo[0]), double(push.albedo[1]),
            double(push.albedo[2]), double(push.terrain[3]));
    push.gusts[0] = gusts[10];
    push.gusts[1] = gusts[11];
    push.gusts[2] = m_sandEyeShelter;
    // An ash storm (no glow towards the sun): forced so (RVK_SandAsh), or the game's storm grey rather than sand's
    // orange (its colour's saturation: sand ~0.45, ash ~0.05).
    const float hi = std::max({m_sandColour[0], m_sandColour[1], m_sandColour[2]});
    const float lo = std::min({m_sandColour[0], m_sandColour[1], m_sandColour[2]});
    const float saturation = hi > 1e-4f ? (hi - lo) / hi : 0.0f;
    const float grey = 1.0f - std::clamp((saturation - 0.12f) / 0.18f, 0.0f, 1.0f);
    push.gusts[3] = m_sandForce > 0.0f ? (m_sandAsh ? 1.0f : 0.0f) : grey * grey * (3.0f - 2.0f * grey);
    push.gusts[3] = std::max(push.gusts[3] * air.sandShare, rainShare);   // (the rain mist: dull too, under cloud)
    // RVK_SandSun: the sky round the sun seen through the dust's depth ^ this (1: as the rest of the sky - hidden).
    push.sun[0] = 1.0f - 0.95f * std::clamp(m_sandSun, 0.0f, 1.0f);
    push.sun[1] = air.upper;                                      // the tall column's share, its scale height
    push.sun[2] = air.upperHeight;
    push.sun[3] = rainShare;                                      // (sand_fog.frag: rain shafts, not billows; no reddening)
    // The lee of walls and hills near the eye (the sand map's shelter): the dust there held off.
    const bool shelterMap = SandFogShelter(push.shelter);

    Texture* dst = m_sandFogTex[0];
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
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_sandFogPipeline);
    VkDescriptorImageInfo depth{m_pointSampler, m_depthView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo shadow{m_shadowSampler, m_shadowView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo lights{frame.post, 0, sizeof(FrameLights)};
    VkDescriptorImageInfo clouds{m_linearSampler, CloudShadowView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo terrain{m_linearSampler, terrainMap ? m_terrainSunMap->m_view : m_blackTexture->m_view,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo shelter{m_linearSampler, shelterMap ? m_sandFogShelter->m_view : m_blackTexture->m_view,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr[6] = {};
    for (auto& x : wr) { x.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; x.descriptorCount = 1; }
    wr[0].dstBinding = 0; wr[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr[0].pImageInfo = &depth;
    wr[1].dstBinding = 1; wr[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr[1].pImageInfo = &shadow;
    wr[2].dstBinding = 4; wr[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; wr[2].pBufferInfo = &lights;
    wr[3].dstBinding = 2; wr[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr[3].pImageInfo = &clouds;
    wr[4].dstBinding = 3; wr[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr[4].pImageInfo = &terrain;
    wr[5].dstBinding = 5; wr[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr[5].pImageInfo = &shelter;
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_volumeLayout, 0, 6, wr);
    vkCmdPushConstants(cmd, m_volumeLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    ProfileMark("sand fog march");               // (the blur: the rest, "sand fog")

    for (int pass = 0; pass < 2; ++pass) {
        Texture* src = m_sandFogTex[pass];
        Texture* out = m_sandFogTex[pass ^ 1];
        Transition(cmd, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        float p[8] = {pass == 0 ? 1.0f : 0.0f, pass == 0 ? 0.0f : 1.0f, 2.0f, 0.0f, m_aoProj.m[2][2], m_aoProj.m[3][2], 0.0f, 0.0f};
        FullscreenPass(cmd, out, m_sandFogBlurPipeline, src->m_view, m_depthView, m_pointSampler, p, sizeof(p), false);
    }
    std::swap(m_sandFogTex[0], m_sandFogTex[1]);    // the second pass wrote [0]
    Transition(cmd, m_sandFogTex[1], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return true;
}

// Screen-space reflections (ssr.frag) at full resolution into m_ssrTex: needs the scene, the depth buffer and the
// reflectivity (the local-fraction attachment's G), all readable by now.
bool Device::RenderSsr(VkCommandBuffer cmd, bool traced)
{
    bool perspective = m_aoProjValid && m_aoProj.m[2][3] == 1.0f && m_aoProj.m[3][3] == 0.0f;
    if (m_ssr <= 0.0f || !perspective || !m_depth)
        return false;
    if (!m_ssrTex || m_ssrTex->m_width != m_scene->m_width || m_ssrTex->m_height != m_scene->m_height) {
        if (m_ssrTex) DestroyTexture(m_ssrTex);
        // (Storage too with ray tracing: RVK_RtReflect fills its misses in place.)
        m_ssrTex = CreateImage(m_scene->m_width, m_scene->m_height, Format::RGBA16F, 1, true, m_rayQuery);
        if (!m_ssrTex)
            return false;
    }
    MakeDepthReadable(cmd);
    Transition(cmd, m_motionVectors, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);   // zw: PBR shading normals
    float params[12] = {m_aoProj.m[2][2], m_aoProj.m[3][2], m_aoProj.m[0][0], m_aoProj.m[1][1],
                        float(m_scene->m_width), float(m_scene->m_height), 48.0f, 80.0f,
                        m_ssr, traced ? 1.0f : 0.0f, 0.0f, 0.0f};
    FullscreenPass(cmd, m_ssrTex, m_ssrPipeline, m_depthView, m_scene->m_view, m_pointSampler, params, sizeof(params), false,
                   m_localFraction->m_view, m_motionVectors->m_view);
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

// This frame's clip -> last frame's clip for a still world (TAA and motion blur reprojection), in double: the view
// holds the camera's world position (thousands of units), and a float inverse of view x projection is off by a
// sub-pixel amount that differs with where the camera is - the TAA then resamples its history by it every frame and
// the still image drifts and blurs. A camera that didn't change reprojects to exactly itself.
void detail::CameraReprojection(const d3d::Matrix& view, const d3d::Matrix& proj, const d3d::Matrix& prevView,
                                const d3d::Matrix& prevProj, d3d::Matrix* out)
{
    if (std::memcmp(&view, &prevView, sizeof(view)) == 0 && std::memcmp(&proj, &prevProj, sizeof(proj)) == 0) {
        *out = Identity();
        return;
    }
    auto mul = [](const double (&a)[4][4], const double (&b)[4][4], double (&r)[4][4]) {
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                r[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + a[i][3] * b[3][j];
    };
    double v[4][4], p[4][4], pv[4][4], pp[4][4], now[4][4], before[4][4];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            v[i][j] = view.m[i][j];
            p[i][j] = proj.m[i][j];
            pv[i][j] = prevView.m[i][j];
            pp[i][j] = prevProj.m[i][j];
        }
    mul(v, p, now);
    mul(pv, pp, before);
    // Gauss-Jordan with partial pivoting: inverse of now.
    double a[4][8];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            a[i][j] = now[i][j];
            a[i][j + 4] = i == j ? 1.0 : 0.0;
        }
    for (int c = 0; c < 4; ++c) {
        int pivot = c;
        for (int r = c + 1; r < 4; ++r)
            if (std::fabs(a[r][c]) > std::fabs(a[pivot][c])) pivot = r;
        if (a[pivot][c] == 0.0) {     // singular (never for a camera): no reprojection
            *out = Identity();
            return;
        }
        if (pivot != c)
            for (int j = 0; j < 8; ++j) std::swap(a[c][j], a[pivot][j]);
        double inv = 1.0 / a[c][c];
        for (int j = 0; j < 8; ++j) a[c][j] *= inv;
        for (int r = 0; r < 4; ++r)
            if (r != c && a[r][c] != 0.0) {
                double f = a[r][c];
                for (int j = 0; j < 8; ++j) a[r][j] -= f * a[c][j];
            }
    }
    double inverse[4][4], r[4][4];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) inverse[i][j] = a[i][j + 4];
    mul(inverse, before, r);
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) out->m[i][j] = float(r[i][j]);
}

// Temporal anti-aliasing: this frame's camera against the last one's (reprojection for pixels without object motion,
// the sky) and whether the history can be used - the last frame resolved, same size, no teleport. RVK_RenderScale:
// also the scene's scale and this frame's jitter in its pixels (taa.frag upscales), and a frame the TAA couldn't
// otherwise resolve (no perspective camera) is still brought up to the output's size, without history.
bool Device::TaaParams(float out[28])
{
    bool perspective = m_aoProjValid && m_aoProj.m[2][3] == 1.0f && m_aoProj.m[3][3] == 0.0f;
    const bool scaled = SceneScaled();
    if (!TaaActive() || ((!perspective || !m_depth) && !scaled))
        return false;
    d3d::Matrix reproject{};
    const auto& v = m_aoView.m;
    float jump2 = 0.0f;
    for (int i = 0; i < 3; ++i) {
        float eye = -(v[3][0] * v[i][0] + v[3][1] * v[i][1] + v[3][2] * v[i][2]);
        jump2 += (eye - m_prevEye[i]) * (eye - m_prevEye[i]);
    }
    // (Continuous: the camera's last frame known, no teleport - FSR 2 keeps its own history on it, fsr2.cpp.)
    const bool continuous = perspective && m_depth && m_prevViewProjValid && jump2 < 25.0f;
    bool history = continuous && m_taaFrame + 1 == m_frameNumber && m_taaHistory[0] &&
                   m_taaHistory[0]->m_width == m_ldrMain->m_width && m_taaHistory[0]->m_height == m_ldrMain->m_height;
    if (continuous)
        CameraReprojection(m_aoView, m_aoProj, m_prevView, m_prevProj, &reproject);
    std::memcpy(out, &reproject, 64);
    out[16] = float(m_ldrMain->m_width);              // (the output's size: the history's)
    out[17] = float(m_ldrMain->m_height);
    out[18] = history ? 1.0f : 0.0f;
    out[19] = m_prevViewProjValid ? 1.0f : 0.0f;      // object motion written this frame (MotionVectorDraw)
    out[20] = 0.1f;                                    // weight of the new frame
    out[21] = m_sharpen;
    out[22] = scaled ? 1.0f : 0.0f;                    // upscaling
    out[23] = continuous ? 1.0f : 0.0f;
    // Scene pixels per output pixel, and the jitter in scene pixels (the scene moved by it: clip x right, y up).
    out[24] = SceneScaleX();
    out[25] = SceneScaleY();
    out[26] = m_taaJitter[0] * 0.5f * float(m_scene->m_width);
    out[27] = -m_taaJitter[1] * 0.5f * float(m_scene->m_height);
    return true;
}

// Resolve m_tonemapped against the history into the next history image, then sharpen that into dst. The history
// is at the output's size (RVK_RenderScale: m_tonemapped is the scene's).
void Device::RenderTaa(VkCommandBuffer cmd, Texture* dst, const float params[28])
{
    for (Texture*& t : m_taaHistory)
        if (!t || t->m_width != m_ldrMain->m_width || t->m_height != m_ldrMain->m_height) {
            if (t) DestroyTexture(t);
            t = CreateImage(m_ldrMain->m_width, m_ldrMain->m_height, Format::RGBA16F, 1, true);
        }
    if (!m_taaHistory[0] || !m_taaHistory[1])
        return;
    Texture* prev = m_taaHistory[m_taaIndex];
    Texture* next = m_taaHistory[m_taaIndex ^ 1];
    Transition(cmd, m_tonemapped, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Transition(cmd, prev, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Transition(cmd, m_motionVectors, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    MakeDepthReadable(cmd);
    FullscreenPass(cmd, next, m_taaPipeline, m_tonemapped->m_view, prev->m_view, m_linearSampler, params, 112, false,
                   m_motionVectors->m_view, m_depthView);
    Transition(cmd, next, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    float sharpen[4] = {params[21], 0.0f, 0.0f, 0.0f};
    FullscreenPass(cmd, dst, m_sharpenPipeline, next->m_view, next->m_view, m_pointSampler, sharpen, sizeof(sharpen), false);
    m_taaIndex ^= 1;
    m_taaFrame = m_frameNumber;
}

// RVK_RenderScale: the scene's depth (nearest sample) drawn into the output-size depth image, which then becomes
// the depth (SwapDepth) - for the motion blur, the name labels drawn after the scene and the interface.
void Device::UpsampleDepth(VkCommandBuffer cmd)
{
    if (m_depthSwapped || !m_depthUpPipeline || !m_depth)
        return;
    MakeDepthReadable(cmd);
    const VkImageView sceneDepth = m_depthView;
    SwapDepth();
    m_depthSwapped = true;
    const uint32_t w = m_ldrMain->m_width, h = m_ldrMain->m_height;
    if (!EnsureDepth(w, h)) {
        SwapDepth();
        m_depthSwapped = false;
        return;
    }
    ImageBarrier(cmd, m_depth, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                 VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
                 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    m_depthLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = m_depthView;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {w, h}};
    ri.layerCount = 1;
    ri.pDepthAttachment = &depth;
    vkCmdBeginRendering(cmd, &ri);
    VkViewport viewport{0.0f, 0.0f, float(w), float(h), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {w, h}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_depthUpPipeline);
    VkDescriptorImageInfo image{m_pointSampler, sceneDepth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wr.dstBinding = 0;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &image;
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_bloomLayout, 0, 1, &wr);
    const float push[4] = {SceneScaleX(), SceneScaleY(), float(m_scene->m_width), float(m_scene->m_height)};
    vkCmdPushConstants(cmd, m_bloomLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), push);
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
    // The levels carry, in alpha, their light's brightness x 1 / view depth: divided by the brightness, how far away
    // the light is - so the tone mapping can keep the bloom off objects in front of it (foliage before a lamp).
    m_bloomDepth = m_aoProjValid && m_aoProj.m[2][3] == 1.0f && m_aoProj.m[3][3] == 0.0f && m_depth;
    if (m_bloomDepth)
        MakeDepthReadable(cmd);
    Texture* src = m_scene;
    for (size_t i = 0; i < m_bloomLevels.size(); ++i) {
        // (By night - as dark as RVK_Darkness makes it - the threshold lower: lamps and windows glow out of the dark.)
        const float nightGlow = NightAmount() * std::min(m_dark.tone, 1.0f);
        float params[8] = {1.0f / float(src->m_width), 1.0f / float(src->m_height),
                           m_bloomThreshold * (1.0f - 0.25f * nightGlow), i == 0 ? 1.0f : 0.0f,
                           m_aoProj.m[2][2], m_aoProj.m[3][2], m_bloomDepth ? 1.0f : 0.0f, 0.0f};
        // The first pass adds the glow (additive effects) to the scene's light above the threshold.
        Texture* src2 = i == 0 ? m_glow : src;
        Transition(cmd, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        Transition(cmd, src2, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        FullscreenPass(cmd, m_bloomLevels[i], m_bloomDown, src->m_view, src2->m_view, m_linearSampler, params, 32, false,
                       m_bloomDepth ? m_depthView : VK_NULL_HANDLE);
        src = m_bloomLevels[i];
    }
    // Each smaller level is added to the next larger one with this weight: level 0 ends up with level i's light x weight^i.
    // RVK_BloomWide raises it (a wide haze round a tight core), more in rain or a storm by night - lamps' halos spread
    // in wet air. The tone mapping divides by the weights' sum, so only the shape changes, not how bright.
    const float wetAir = NightAmount() * std::max(StormOvercast(), std::clamp(m_wetness, 0.0f, 1.0f));
    const float weight = 1.0f + 0.6f * std::clamp(m_bloomWide, 0.0f, 1.0f) * (1.0f + 0.5f * wetAir);
    m_bloomWeightSum = 0.0f;
    for (size_t i = 0; i < m_bloomLevels.size(); ++i) m_bloomWeightSum += std::pow(weight, float(i));
    for (size_t i = m_bloomLevels.size() - 1; i-- > 0;) {
        Texture* smaller = m_bloomLevels[i + 1];
        float params[4] = {1.0f / float(smaller->m_width), 1.0f / float(smaller->m_height), weight, 0.0f};
        Transition(cmd, smaller, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        FullscreenPass(cmd, m_bloomLevels[i], m_bloomUp, smaller->m_view, smaller->m_view, m_linearSampler, params, 16, true);
    }
    Transition(cmd, m_bloomLevels[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

// Per-object motion blur from the motion vectors: strongest motion per tile, of each tile's neighbourhood, then the
// reconstruction filter from m_tonemapped into the main target. The depth buffer is readable (EndScene).
void Device::RenderObjectMotionBlur(VkCommandBuffer cmd, Texture* src, const float params[28])
{
    uint32_t tw = (m_ldrMain->m_width + kMotionTile - 1) / kMotionTile, th = (m_ldrMain->m_height + kMotionTile - 1) / kMotionTile;
    if (!m_motionTiles[0] || m_motionTiles[0]->m_width != tw || m_motionTiles[0]->m_height != th) {
        for (Texture*& t : m_motionTiles) {
            if (t) DestroyTexture(t);
            t = CreateImage(tw, th, Format::RG16F, 1, true);
        }
    }
    if (!m_motionTiles[0] || !m_motionTiles[1]) {
        FullscreenPass(cmd, m_ldrMain, m_motionPipeline, src->m_view, m_depthView, m_pointSampler, params, 112, false);
        return;
    }
    // The blur reaches at most a tile beyond its own: cap the motion at two tiles.
    float p[28];
    std::memcpy(p, params, sizeof(p));
    p[17] = std::min(p[17], float(2 * kMotionTile));
    Transition(cmd, m_motionVectors, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    FullscreenPass(cmd, m_motionTiles[0], m_tileMaxPipeline, m_motionVectors->m_view, m_depthView, m_pointSampler, p, 112, false);
    Transition(cmd, m_motionTiles[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    FullscreenPass(cmd, m_motionTiles[1], m_neighbourMaxPipeline, m_motionTiles[0]->m_view, m_motionTiles[0]->m_view,
                   m_pointSampler, p, 112, false);
    Transition(cmd, m_motionTiles[1], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    FullscreenPass(cmd, m_ldrMain, m_objectBlurPipeline, src->m_view, m_depthView, m_pointSampler, p, 112, false,
                   m_motionVectors->m_view, m_motionTiles[1]->m_view);
}

// This frame's camera against the last one's: the reprojection matrix (current clip -> previous clip) and the blur
// parameters for motion_blur.frag. Remembers this frame's camera for the next.
bool Device::MotionBlurParams(float out[28])
{
    double now = SwayClock(), dt = now - m_prevSceneTime;
    m_prevSceneTime = now;
    bool perspective = m_aoProjValid && m_aoProj.m[2][3] == 1.0f && m_aoProj.m[3][3] == 0.0f;
    if (!perspective) {
        m_prevViewProjValid = false;
        return false;
    }
    const auto& v = m_aoView.m;
    float eye[3];
    for (int i = 0; i < 3; ++i) eye[i] = -(v[3][0] * v[i][0] + v[3][1] * v[i][1] + v[3][2] * v[i][2]);
    bool have = m_prevViewProjValid && m_motionBlur > 0.0f && m_depth;
    float jump2 = 0.0f;
    for (int i = 0; i < 3; ++i) jump2 += (eye[i] - m_prevEye[i]) * (eye[i] - m_prevEye[i]);
    if (have && jump2 > 25.0f)   // a teleport / zone change: no blur
        have = false;
    if (have) {
        d3d::Matrix reproject;
        CameraReprojection(m_aoView, m_aoProj, m_prevView, m_prevProj, &reproject);
        std::memcpy(out, &reproject, 64);
        dt = std::clamp(dt, 1.0 / 500.0, 0.25);
        out[16] = float(m_motionBlur / 60.0 / dt);        // per-frame motion -> motion during the exposure
        out[17] = 0.04f * float(m_ldrMain->m_width);      // at most 4% of the screen width
        out[18] = m_motionNear;
        out[19] = m_motionNear * 1.5f;
        out[20] = m_aoProj.m[2][2];
        out[21] = m_aoProj.m[3][2];
        out[22] = float(m_ldrMain->m_width);              // (it runs at the output's size, after the TAA)
        out[23] = float(m_ldrMain->m_height);
        out[24] = SceneScaleX();                          // (RVK_RenderScale: the motion vectors are the scene's)
        out[25] = SceneScaleY();
        out[26] = out[27] = 0.0f;
    }
    m_prevView = m_aoView;
    m_prevProj = m_aoProj;
    std::memcpy(m_prevEye, eye, sizeof(eye));
    m_prevViewProjValid = true;
    return have;
}

}  // namespace rvk
