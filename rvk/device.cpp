// Device setup, main targets, swapchain, frames, render target switching, screenshots.
#include "internal.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace rvk {

namespace {
void (*g_logSink)(const char*) = nullptr;
}

void SetLogSink(void (*sink)(const char*)) { g_logSink = sink; }

void Log(const char* fmt, ...)
{
    char line[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    size_t n = std::strlen(line);
    if (n && line[n - 1] == '\n') line[n - 1] = 0;
    if (g_logSink) {
        g_logSink(line);
    } else {
        std::fprintf(stderr, "rvk: %s\n", line);
        std::fflush(stderr);
    }
}

using namespace vk;
using namespace detail;

namespace {

const uint32_t kVertSpirv[] = {
#include "ffp.vert.inc"
};
const uint32_t kTescSpirv[] = {                 // characters' Phong tessellation
#include "ffp.tesc.inc"
};
const uint32_t kTeseSpirv[] = {
#include "ffp.tese.inc"
};
const uint32_t kFragSpirv[] = {
#include "ffp.frag.inc"
};
const uint32_t kFragGlowSpirv[] = {             // the HDR scene's: also writes the glow attachment
#include "ffp_glow.frag.inc"
};
const uint32_t kFragNoCutSpirv[] = {            // no discard: a draw that never cuts out keeps early-Z
#include "ffp_nocut.frag.inc"
};
const uint32_t kFragGlowNoCutSpirv[] = {
#include "ffp_glow_nocut.frag.inc"
};

// Access/stage masks to go with an image layout, for barriers.
void LayoutUse(VkImageLayout layout, VkPipelineStageFlags2* stage, VkAccessFlags2* access)
{
    switch (layout) {
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        *access = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        break;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        *access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        break;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        *access = VK_ACCESS_2_TRANSFER_READ_BIT;
        break;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        *access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        break;
    default:
        *stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        *access = 0;
        break;
    }
}

}  // namespace

namespace detail {

bool Check(VkResult r, const char* what, std::string* error)
{
    if (r == VK_SUCCESS)
        return true;
    if (error)
        *error = std::string(what) + " failed: VkResult " + std::to_string(r);
    return false;
}

void ImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                  VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                  VkAccessFlags2 dstAccess)
{
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

d3d::Matrix Identity()
{
    d3d::Matrix m{};
    for (int i = 0; i < 4; ++i)
        m.m[i][i] = 1.0f;
    return m;
}

}  // namespace detail

Device::Device()
{
    // D3D7 defaults (render states not listed default to 0).
    m_rs[d3d::RS_ZENABLE] = 1;
    m_rs[d3d::RS_FILLMODE] = 3;
    m_rs[d3d::RS_SHADEMODE] = 2;
    m_rs[d3d::RS_ZWRITEENABLE] = 1;
    m_rs[d3d::RS_SRCBLEND] = d3d::BLEND_ONE;
    m_rs[d3d::RS_DESTBLEND] = d3d::BLEND_ZERO;
    m_rs[d3d::RS_CULLMODE] = d3d::CULL_CCW;
    m_rs[d3d::RS_ZFUNC] = d3d::CMP_LESSEQUAL;
    m_rs[d3d::RS_ALPHAFUNC] = d3d::CMP_ALWAYS;
    m_rs[d3d::RS_FOGEND] = 0x3F800000;        // 1.0f
    m_rs[d3d::RS_FOGDENSITY] = 0x3F800000;
    m_rs[d3d::RS_TEXTUREFACTOR] = 0xFFFFFFFF;
    m_rs[d3d::RS_CLIPPING] = 1;
    m_rs[d3d::RS_LIGHTING] = 1;
    m_rs[d3d::RS_COLORVERTEX] = 1;
    m_rs[d3d::RS_LOCALVIEWER] = 1;
    m_rs[d3d::RS_DIFFUSEMATERIALSOURCE] = d3d::MCS_COLOR1;
    m_rs[d3d::RS_SPECULARMATERIALSOURCE] = d3d::MCS_COLOR2;
    for (uint32_t s = 0; s < 2; ++s) {
        auto& t = m_tss[s];
        t[d3d::TSS_COLOROP] = s == 0 ? d3d::TOP_MODULATE : d3d::TOP_DISABLE;
        t[d3d::TSS_COLORARG1] = d3d::TA_TEXTURE;
        t[d3d::TSS_COLORARG2] = d3d::TA_CURRENT;
        t[d3d::TSS_ALPHAOP] = s == 0 ? d3d::TOP_SELECTARG1 : d3d::TOP_DISABLE;
        t[d3d::TSS_ALPHAARG1] = d3d::TA_TEXTURE;
        t[d3d::TSS_ALPHAARG2] = d3d::TA_CURRENT;
        t[d3d::TSS_TEXCOORDINDEX] = s;
        t[d3d::TSS_ADDRESS] = t[d3d::TSS_ADDRESSU] = t[d3d::TSS_ADDRESSV] = d3d::TADDRESS_WRAP;
        t[d3d::TSS_MAGFILTER] = d3d::TFG_POINT;
        t[d3d::TSS_MINFILTER] = d3d::TFN_POINT;
        t[d3d::TSS_MIPFILTER] = d3d::TFP_NONE;
    }
    m_world = m_view = m_proj = m_texMatrix[0] = m_texMatrix[1] = Identity();
}

Device::~Device()
{
    if (!m_device)
        return;
    vkDeviceWaitIdle(m_device);
    ReleaseParticleTextures(true);
    m_completed = UINT64_MAX;                   // the device is idle: everything deferred can go
    CollectGarbage();
    for (auto& f : m_frames) {
        if (f.ring) vmaDestroyBuffer(m_allocator, f.ring, f.ringAllocation);
        if (f.post) vmaDestroyBuffer(m_allocator, f.post, f.postAllocation);
        if (f.fence) vkDestroyFence(m_device, f.fence, nullptr);
        if (f.imageAvailable) vkDestroySemaphore(m_device, f.imageAvailable, nullptr);
    }
    if (m_blackTexture) DestroyTextureNow(m_blackTexture);
    if (m_flatNormal) DestroyTextureNow(m_flatNormal);
    for (auto& [key, sampler] : m_samplers) vkDestroySampler(m_device, sampler, nullptr);
    for (VkPipeline p : m_pipelines) if (p) vkDestroyPipeline(m_device, p, nullptr);
    for (VkPipeline p : m_pipelinesHdr) if (p) vkDestroyPipeline(m_device, p, nullptr);
    for (VkPipeline p : m_pipelinesNoCut) if (p) vkDestroyPipeline(m_device, p, nullptr);
    for (VkPipeline p : m_pipelinesHdrNoCut) if (p) vkDestroyPipeline(m_device, p, nullptr);
    for (VkPipeline p : m_tessPipelines) if (p) vkDestroyPipeline(m_device, p, nullptr);
    for (VkPipeline p : m_tessPipelinesNoCut) if (p) vkDestroyPipeline(m_device, p, nullptr);
    if (m_prepassPipeline) vkDestroyPipeline(m_device, m_prepassPipeline, nullptr);
    if (m_prepassCutoutPipeline) vkDestroyPipeline(m_device, m_prepassCutoutPipeline, nullptr);
    DestroyShadowResources();
    DestroyPointShadowResources();
    DestroyHdrResources();
    DestroyProfiler();
    DestroyParticleResources();
    DestroySkinResources();
    DestroyGrassResources();
    DestroyWaterResources();
    DestroyStaticGeometry();
    if (m_pipelineLayout) vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
    if (m_setLayout) vkDestroyDescriptorSetLayout(m_device, m_setLayout, nullptr);
    if (m_bindlessPool) vkDestroyDescriptorPool(m_device, m_bindlessPool, nullptr);
    if (m_bindlessSetLayout) vkDestroyDescriptorSetLayout(m_device, m_bindlessSetLayout, nullptr);
    if (m_nullBuffer) vmaDestroyBuffer(m_allocator, m_nullBuffer, m_nullAllocation);
    DestroyMainTargets();
    if (m_pool) vkDestroyCommandPool(m_device, m_pool, nullptr);
    DestroySwapchain();
    if (m_allocator) vmaDestroyAllocator(m_allocator);
    vkDestroyDevice(m_device, nullptr);
    if (m_surface) vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
    if (m_instance) vkDestroyInstance(m_instance, nullptr);
}

// ---------------------------------------------------------------------------------------------------
// Initialisation

bool Device::Init(HWND window, uint32_t width, uint32_t height, std::string* error)
{
    m_window = window;
    m_width = width;
    m_height = height;
    m_viewport = {0, 0, width, height, 0.0f, 1.0f};
    if (!CreateInstance(error) || !PickDevice(error) || !CreateLogicalDevice(error) || !CreateFrames(error) || !CreateProfiler(error) ||
        !CreateMainTargets(error) || (window && !CreateSwapchain(error)) || !CreatePipelines(error))
        return false;
    uint32_t black = 0xFF000000;
    m_blackTexture = CreateTexture(1, 1, &black);
    uint32_t flat = 0xFF8080FF;                  // (0.5, 0.5, 1): the unbent normal
    m_flatNormal = CreateTexture(1, 1, &flat);
    return m_blackTexture != nullptr && m_flatNormal != nullptr;
}

bool Device::CreateInstance(std::string* error)
{
    if (!LoadGlobal()) {
        if (error) *error = "vulkan-1.dll not found";
        return false;
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "randy-vk";
    app.pEngineName = "rvk";
    app.apiVersion = VK_API_VERSION_1_3;
    const char* extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = 2;
    ci.ppEnabledExtensionNames = extensions;
    if (!Check(vkCreateInstance(&ci, nullptr, &m_instance), "vkCreateInstance", error))
        return false;
    LoadInstance(m_instance);
    if (m_window) {
        VkWin32SurfaceCreateInfoKHR si{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        si.hinstance = GetModuleHandleA(nullptr);
        si.hwnd = m_window;
        if (!Check(vkCreateWin32SurfaceKHR(m_instance, &si, nullptr, &m_surface), "vkCreateWin32SurfaceKHR", error))
            return false;
    }
    return true;
}

bool Device::PickDevice(std::string* error)
{
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(m_instance, &count, devices.data());
    // Prefer a discrete GPU with a graphics queue that can present.
    int best = -1;
    for (uint32_t i = 0; i < count; ++i) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devices[i], &p);
        if (p.apiVersion < VK_API_VERSION_1_3)
            continue;
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qf(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &qn, qf.data());
        for (uint32_t q = 0; q < qn; ++q) {
            VkBool32 present = VK_TRUE;
            if (m_surface)
                vkGetPhysicalDeviceSurfaceSupportKHR(devices[i], q, m_surface, &present);
            if (!(qf[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present)
                continue;
            bool discrete = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
            if (best < 0 || discrete) {
                best = static_cast<int>(i);
                m_queueFamily = q;
            }
            break;
        }
    }
    if (best < 0) {
        if (error) *error = "no Vulkan 1.3 GPU with a graphics queue";
        return false;
    }
    m_physical = devices[best];
    vkGetPhysicalDeviceProperties(m_physical, &m_props);
    m_info.gpu = m_props.deviceName;
    m_info.apiVersion = m_props.apiVersion;
    VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &driver;
    vkGetPhysicalDeviceProperties2(m_physical, &p2);
    m_info.driver = std::string(driver.driverName) + " " + driver.driverInfo;
    for (uint32_t f = 0; f < uint32_t(Format::Count); ++f) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(m_physical, GetFormatInfo(Format(f)).vk, &fp);
        m_formatSupported[f] = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
    }
    return true;
}

bool Device::CreateLogicalDevice(std::string* error)
{
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(m_physical, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    vkEnumerateDeviceExtensionProperties(m_physical, nullptr, &count, available.data());
    auto has = [&](const char* name) {
        return std::any_of(available.begin(), available.end(),
                           [&](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; });
    };
    std::vector<const char*> extensions = {VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
                                           VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME,
                                           VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME,
                                           VK_EXT_ROBUSTNESS_2_EXTENSION_NAME};   // nullDescriptor: empty bindless slots
    // Swapchain support even without a window yet: SetWindow() can attach one later.
    m_swapchainSupported = has(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    if (m_swapchainSupported)
        extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    else if (m_window) {
        if (error) *error = "device extension missing: " VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        return false;
    }
    for (const char* e : extensions)
        if (!has(e)) {
            if (error) *error = std::string("device extension missing: ") + e;
            return false;
        }

    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT vertexInput{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT};
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceDescriptorIndexingFeatures descIdx{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES};
    VkPhysicalDeviceRobustness2FeaturesEXT rob2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &v13;
    v13.pNext = &descIdx;
    descIdx.pNext = &rob2;
    rob2.pNext = &eds3;
    eds3.pNext = &vertexInput;
    vkGetPhysicalDeviceFeatures2(m_physical, &features);
    // Quad operations in fragment shaders (ffp_main.glsl: a 2x2 block of cut-out pixels stops before the lighting).
    VkPhysicalDeviceVulkan11Properties p11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES};
    VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    props2.pNext = &p11;
    vkGetPhysicalDeviceProperties2(m_physical, &props2);
    bool quad = (p11.subgroupSupportedStages & VK_SHADER_STAGE_FRAGMENT_BIT) &&
                (p11.subgroupSupportedOperations & VK_SUBGROUP_FEATURE_QUAD_BIT);
    if (!quad || !v13.dynamicRendering || !v13.synchronization2 || !eds3.extendedDynamicState3ColorBlendEnable ||
        !eds3.extendedDynamicState3ColorBlendEquation || !vertexInput.vertexInputDynamicState ||
        !v13.shaderDemoteToHelperInvocation || !features.features.imageCubeArray) {
        if (error) *error = "required Vulkan features missing (dynamic rendering, sync2, dynamic blend/vertex input, "
                          "fragment quad operations)";
        return false;
    }
    // Bindless textures (M1): textures and samplers in descriptor arrays, indexed per draw.
    m_bindless = descIdx.shaderSampledImageArrayNonUniformIndexing && descIdx.descriptorBindingSampledImageUpdateAfterBind &&
                 descIdx.descriptorBindingPartiallyBound && rob2.nullDescriptor;
    Log("bindless textures: %s", m_bindless ? "yes" : "no");
    if (!m_bindless) {
        // The scene shaders read their textures through set 1 now; there is no non-bindless path.
        if (error) *error = "required Vulkan features missing (descriptor indexing, robustness2 nullDescriptor)";
        return false;
    }
    // Enable only what is used.
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT enVertexInput{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT};
    enVertexInput.vertexInputDynamicState = VK_TRUE;
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT enEds3{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    enEds3.pNext = &enVertexInput;
    enEds3.extendedDynamicState3ColorBlendEnable = VK_TRUE;
    enEds3.extendedDynamicState3ColorBlendEquation = VK_TRUE;
    // Write masks per draw (optional): attachments a draw leaves as they are aren't read and written back.
    m_dynamicWriteMask = eds3.extendedDynamicState3ColorWriteMask == VK_TRUE;
    enEds3.extendedDynamicState3ColorWriteMask = m_dynamicWriteMask ? VK_TRUE : VK_FALSE;
    Log("dynamic colour write masks: %s", m_dynamicWriteMask ? "yes" : "no");
    VkPhysicalDeviceDescriptorIndexingFeatures enDescIdx{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES};
    enDescIdx.shaderSampledImageArrayNonUniformIndexing = m_bindless ? VK_TRUE : VK_FALSE;
    enDescIdx.descriptorBindingSampledImageUpdateAfterBind = m_bindless ? VK_TRUE : VK_FALSE;
    enDescIdx.descriptorBindingPartiallyBound = m_bindless ? VK_TRUE : VK_FALSE;
    VkPhysicalDeviceRobustness2FeaturesEXT enRob2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
    enRob2.nullDescriptor = m_bindless ? VK_TRUE : VK_FALSE;
    enRob2.pNext = &enEds3;
    enDescIdx.pNext = &enRob2;
    VkPhysicalDeviceVulkan13Features enV13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    enV13.pNext = &enDescIdx;
    enV13.dynamicRendering = VK_TRUE;
    enV13.synchronization2 = VK_TRUE;
    enV13.shaderDemoteToHelperInvocation = VK_TRUE;     // glslc turns `discard` into OpDemoteToHelperInvocation
    VkPhysicalDeviceFeatures2 enabled{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    enabled.pNext = &enV13;
    enabled.features.samplerAnisotropy = features.features.samplerAnisotropy;
    m_maxAnisotropy = features.features.samplerAnisotropy ? m_props.limits.maxSamplerAnisotropy : 0.0f;
    // Out-of-range vertex indices in game data read zeros instead of faulting the GPU.
    enabled.features.robustBufferAccess = features.features.robustBufferAccess;
    enabled.features.textureCompressionBC = features.features.textureCompressionBC;
    enabled.features.imageCubeArray = VK_TRUE;           // point light shadow maps
    enabled.features.tessellationShader = features.features.tessellationShader;   // characters' Phong tessellation
    m_tessSupported = features.features.tessellationShader;
    // M3: a batch's indirect commands carry the record index in firstInstance (the shader's gl_InstanceIndex),
    // and one call holds several commands (multiDrawIndirect).
    m_groupIndirect = features.features.drawIndirectFirstInstance && features.features.multiDrawIndirect;
    enabled.features.drawIndirectFirstInstance = features.features.drawIndirectFirstInstance;
    enabled.features.multiDrawIndirect = features.features.multiDrawIndirect;
    // The depth pre-pass's backdrop cull (prepass.cpp): a background drawn before the scene's depth is rejected where
    // the pre-pass found an opaque surface - the depth bounds test, early, before shading.
    m_depthBounds = features.features.depthBounds;
    enabled.features.depthBounds = features.features.depthBounds;
    // Profiling: fragment shader invocations per scene class (profile.cpp, "scene shading" - the overdraw).
    m_shadeQueries = features.features.pipelineStatisticsQuery;
    enabled.features.pipelineStatisticsQuery = features.features.pipelineStatisticsQuery;
    Log("draw grouping (indirect firstInstance + multiDrawIndirect): %s", m_groupIndirect ? "yes" : "no");

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = m_queueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    ci.pNext = &enabled;
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qci;
    ci.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    ci.ppEnabledExtensionNames = extensions.data();
    if (!Check(vkCreateDevice(m_physical, &ci, nullptr, &m_device), "vkCreateDevice", error))
        return false;
    LoadDevice(m_device);
    vkGetDeviceQueue(m_device, m_queueFamily, 0, &m_queue);

    VmaVulkanFunctions fns{};
    fns.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    fns.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo ai{};
    ai.vulkanApiVersion = VK_API_VERSION_1_3;
    ai.physicalDevice = m_physical;
    ai.device = m_device;
    ai.instance = m_instance;
    ai.pVulkanFunctions = &fns;
    return Check(vmaCreateAllocator(&ai, &m_allocator), "vmaCreateAllocator", error);
}

bool Device::CreateMainTargets(std::string* error)
{
    m_ldrMain = CreateImage(m_width, m_height, Format::A8R8G8B8, 1, true);
    m_scene = CreateImage(m_width, m_height, Format::RGBA16F, 1, true);
    m_glow = CreateImage(m_width, m_height, Format::RGBA16F, 1, true);
    m_localFraction = CreateImage(m_width, m_height, Format::RGBA8, 1, true);
    m_motionVectors = CreateImage(m_width, m_height, Format::RGBA16F, 1, true);   // zw: PBR shading normal (ssr.frag)
    m_albedo = CreateImage(m_width, m_height, Format::A8R8G8B8, 1, true);
    if (!m_ldrMain || !m_scene || !m_glow || !m_localFraction || !m_motionVectors || !m_albedo) {
        if (error) *error = "main colour target";
        return false;
    }
    m_main = m_ldrMain;
    m_target = m_main;

    m_depthWidth = m_depthHeight = 0;
    if (!EnsureDepth(m_width, m_height)) {
        if (error) *error = "depth buffer";
        return false;
    }

    // Readback buffer for screenshots.
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = VkDeviceSize(m_width) * m_height * 4;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo rc{};
    rc.usage = VMA_MEMORY_USAGE_AUTO;
    rc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info;
    if (!Check(vmaCreateBuffer(m_allocator, &bi, &rc, &m_readback, &m_readbackAllocation, &info), "readback", error))
        return false;
    m_readbackData = info.pMappedData;
    return true;
}

bool Device::EnsureDepth(uint32_t width, uint32_t height)
{
    if (m_depth && width <= m_depthWidth && height <= m_depthHeight)
        return true;
    width = std::max(width, m_depthWidth);
    height = std::max(height, m_depthHeight);
    if (m_depth)          // may still be in use by submitted or recorded work: free with this frame slot
        m_deadImages.push_back({DeathTag(), {m_depth, m_depthView, m_depthAllocation}});
    m_depth = VK_NULL_HANDLE;
    m_depthView = VK_NULL_HANDLE;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = kDepthFormat;
    ci.extent = {width, height, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    // sampled: ambient occlusion; copied: the water's view of the scene
    ci.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo ac{};
    ac.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    ac.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    if (vmaCreateImage(m_allocator, &ci, &ac, &m_depth, &m_depthAllocation, nullptr) != VK_SUCCESS)
        return false;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = m_depth;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = kDepthFormat;
    vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    vkCreateImageView(m_device, &vi, nullptr, &m_depthView);
    m_depthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    m_depthWidth = width;
    m_depthHeight = height;
    return true;
}

void Device::DestroyMainTargets()
{
    if (m_ldrMain) { DestroyTextureNow(m_ldrMain); m_ldrMain = nullptr; }
    if (m_scene) { DestroyTextureNow(m_scene); m_scene = nullptr; }
    if (m_glow) { DestroyTextureNow(m_glow); m_glow = nullptr; }
    if (m_localFraction) { DestroyTextureNow(m_localFraction); m_localFraction = nullptr; }
    if (m_motionVectors) { DestroyTextureNow(m_motionVectors); m_motionVectors = nullptr; }
    if (m_albedo) { DestroyTextureNow(m_albedo); m_albedo = nullptr; }
    m_main = nullptr;
    for (auto& [tag, d] : m_deadImages) { vkDestroyImageView(m_device, d.view, nullptr); vmaDestroyImage(m_allocator, d.image, d.allocation); }
    m_deadImages.clear();
    if (m_depthView) { vkDestroyImageView(m_device, m_depthView, nullptr); m_depthView = VK_NULL_HANDLE; }
    if (m_depth) { vmaDestroyImage(m_allocator, m_depth, m_depthAllocation); m_depth = VK_NULL_HANDLE; }
    if (m_readback) { vmaDestroyBuffer(m_allocator, m_readback, m_readbackAllocation); m_readback = VK_NULL_HANDLE; }
}

bool Device::Resize(uint32_t width, uint32_t height)
{
    if (m_inFrame || !width || !height)
        return false;
    vkDeviceWaitIdle(m_device);
    m_completed = m_submitted;
    bool wasMain = m_target == m_main;
    DestroyMainTargets();
    m_width = width;
    m_height = height;
    std::string error;
    if (!CreateMainTargets(&error))
        return false;
    if (!wasMain)
        m_target = m_main;          // the old target may be bigger than the new depth buffer
    m_viewport = {0, 0, width, height, 0.0f, 1.0f};
    if (m_swapchain)
        m_swapchainStale = true;
    return true;
}

bool Device::SetWindow(HWND window)
{
    if (window == m_window && m_surface)
        return true;
    if (!m_swapchainSupported || m_inFrame)
        return false;
    vkDeviceWaitIdle(m_device);
    DestroySwapchain();
    if (m_surface) {
        vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
        m_surface = VK_NULL_HANDLE;
    }
    m_window = window;
    if (!window)
        return true;
    VkWin32SurfaceCreateInfoKHR si{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    si.hinstance = GetModuleHandleA(nullptr);
    si.hwnd = window;
    std::string error;
    if (!Check(vkCreateWin32SurfaceKHR(m_instance, &si, nullptr, &m_surface), "vkCreateWin32SurfaceKHR", &error))
        return false;
    VkBool32 present = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(m_physical, m_queueFamily, m_surface, &present);
    if (!present || !CreateSwapchain(&error)) {
        Log("SetWindow failed: %s\n", present ? error.c_str() : "queue cannot present");
        return false;
    }
    return true;
}

bool Device::CreateSwapchain(std::string* error)
{
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physical, m_surface, &caps);
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_physical, m_surface, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_physical, m_surface, &n, formats.data());
    VkSurfaceFormatKHR format = formats[0];
    for (auto& f : formats)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) { format = f; break; }
    m_swapExtent = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : VkExtent2D{m_width, m_height};
    if (!m_swapExtent.width || !m_swapExtent.height)
        return true;                // minimised: keep rendering offscreen, present nothing
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = m_surface;
    ci.minImageCount = std::max(caps.minImageCount, 2u);
    if (caps.maxImageCount) ci.minImageCount = std::min(ci.minImageCount + 1, caps.maxImageCount);
    ci.imageFormat = format.format;
    ci.imageColorSpace = format.colorSpace;
    ci.imageExtent = m_swapExtent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    // MAILBOX or IMMEDIATE like D7VK: a FIFO swapchain can block indefinitely on Wayland when the
    // compositor stops sending frame callbacks (e.g. while the window is hidden or not focused).
    uint32_t modeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(m_physical, m_surface, &modeCount, nullptr);
    std::vector<VkPresentModeKHR> modes(modeCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(m_physical, m_surface, &modeCount, modes.data());
    auto hasMode = [&](VkPresentModeKHR m) { return std::find(modes.begin(), modes.end(), m) != modes.end(); };
    ci.presentMode = hasMode(VK_PRESENT_MODE_MAILBOX_KHR)     ? VK_PRESENT_MODE_MAILBOX_KHR
                     : hasMode(VK_PRESENT_MODE_IMMEDIATE_KHR) ? VK_PRESENT_MODE_IMMEDIATE_KHR
                                                              : VK_PRESENT_MODE_FIFO_KHR;
    Log("swapchain %ux%u, present mode %d\n", m_swapExtent.width, m_swapExtent.height, ci.presentMode);
    ci.clipped = VK_TRUE;
    if (!Check(vkCreateSwapchainKHR(m_device, &ci, nullptr, &m_swapchain), "vkCreateSwapchainKHR", error))
        return false;
    vkGetSwapchainImagesKHR(m_device, m_swapchain, &n, nullptr);
    m_swapImages.resize(n);
    vkGetSwapchainImagesKHR(m_device, m_swapchain, &n, m_swapImages.data());
    m_renderDone.resize(n);
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (auto& s : m_renderDone)
        if (!Check(vkCreateSemaphore(m_device, &si, nullptr, &s), "vkCreateSemaphore", error))
            return false;
    return true;
}

void Device::DestroySwapchain()
{
    for (VkSemaphore s : m_renderDone) vkDestroySemaphore(m_device, s, nullptr);
    m_renderDone.clear();
    m_swapImages.clear();
    if (m_swapchain) vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
    m_swapchain = VK_NULL_HANDLE;
}

bool Device::CreatePipelines(std::string* error)
{
    // Bindings 0, 3, 4 also for the tessellation stages (characters' Phong tessellation: camera, draw, frame).
    const VkShaderStageFlags vsfs = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    const VkShaderStageFlags tess = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    const VkShaderStageFlags vsfstess = vsfs | tess;
    VkDescriptorSetLayoutBinding bindings[12] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, vsfstess, nullptr},   // the deduplicated DrawConstants array
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, vsfstess, nullptr},
        {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr},
        {9, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},   // shadow depths
        {10, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr},     // tessellation's normals
        {11, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},  // normal map
        {12, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, vsfstess, nullptr},  // the per-draw DrawRecords
    };
    if (!m_tessSupported)
        for (auto& b : bindings) b.stageFlags &= ~tess;
    VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    sl.bindingCount = 12;
    sl.pBindings = bindings;
    if (!Check(vkCreateDescriptorSetLayout(m_device, &sl, nullptr, &m_setLayout), "vkCreateDescriptorSetLayout", error))
        return false;
    if (m_bindless) {
        VkDescriptorSetLayoutBinding bb[2] = {
            {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kMaxBindlessImages, vsfs, nullptr},
            {1, VK_DESCRIPTOR_TYPE_SAMPLER, kMaxBindlessSamplers, vsfs, nullptr},
        };
        VkDescriptorBindingFlags bflags[2] = {
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT,
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT};
        VkDescriptorSetLayoutBindingFlagsCreateInfo bf{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
        bf.bindingCount = 2;
        bf.pBindingFlags = bflags;
        VkDescriptorSetLayoutCreateInfo sl2{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        sl2.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        sl2.pNext = &bf;
        sl2.bindingCount = 2;
        sl2.pBindings = bb;
        if (!Check(vkCreateDescriptorSetLayout(m_device, &sl2, nullptr, &m_bindlessSetLayout), "bindless set layout", error))
            return false;
        VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kMaxBindlessImages},
                                      {VK_DESCRIPTOR_TYPE_SAMPLER, kMaxBindlessSamplers}};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        pi.maxSets = 1;
        pi.poolSizeCount = 2;
        pi.pPoolSizes = ps;
        if (!Check(vkCreateDescriptorPool(m_device, &pi, nullptr, &m_bindlessPool), "bindless descriptor pool", error))
            return false;
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = m_bindlessPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &m_bindlessSetLayout;
        if (!Check(vkAllocateDescriptorSets(m_device, &ai, &m_bindlessSet), "bindless descriptor set", error))
            return false;
    }
    VkDescriptorSetLayout setLayouts[2] = {m_setLayout, m_bindlessSetLayout};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = m_bindless ? 2 : 1;
    pl.pSetLayouts = setLayouts;
    if (!Check(vkCreatePipelineLayout(m_device, &pl, nullptr, &m_pipelineLayout), "vkCreatePipelineLayout", error))
        return false;

    auto module = [&](const uint32_t* code, size_t size, VkShaderModule* out) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = size;
        ci.pCode = code;
        return Check(vkCreateShaderModule(m_device, &ci, nullptr, out), "vkCreateShaderModule", error);
    };
    VkShaderModule vert, frag, fragGlow, fragNoCut, fragGlowNoCut;
    if (!module(kVertSpirv, sizeof(kVertSpirv), &vert) || !module(kFragSpirv, sizeof(kFragSpirv), &frag) ||
        !module(kFragGlowSpirv, sizeof(kFragGlowSpirv), &fragGlow) ||
        !module(kFragNoCutSpirv, sizeof(kFragNoCutSpirv), &fragNoCut) ||
        !module(kFragGlowNoCutSpirv, sizeof(kFragGlowNoCutSpirv), &fragGlowNoCut))
        return false;

    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vert, "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main", nullptr},
    };
    VkDynamicState dynamic[] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE,
        VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_VERTEX_INPUT_EXT, VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT,
        VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT};
    std::vector<VkDynamicState> dynamicStates(dynamic, dynamic + sizeof(dynamic) / sizeof(dynamic[0]));
    if (m_dynamicWriteMask) dynamicStates.push_back(VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT);
    if (m_depthBounds) {                         // the backdrop cull (prepass.cpp)
        dynamicStates.push_back(VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE);
        dynamicStates.push_back(VK_DYNAMIC_STATE_DEPTH_BOUNDS);
    }
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = uint32_t(dynamicStates.size());
    ds.pDynamicStates = dynamicStates.data();
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo dss{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState att[5] = {};
    for (auto& a : att) a.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = att;
    // 8-bit targets: one colour attachment. HDR scene: the float scene, the glow (additive effects, for the bloom) and
    // the local-light fraction (for the ambient occlusion), the motion vectors and the surface colour (indirect light).
    VkFormat colorFormats[5] = {kColorFormat, GetFormatInfo(Format::RGBA16F).vk, GetFormatInfo(Format::RGBA8).vk,
                                GetFormatInfo(Format::RGBA16F).vk, GetFormatInfo(Format::A8R8G8B8).vk};
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = colorFormats;
    rendering.depthAttachmentFormat = kDepthFormat;

    static const VkPrimitiveTopology kClassTopology[3] = {VK_PRIMITIVE_TOPOLOGY_POINT_LIST,
                                                          VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
                                                          VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    bool ok = true;
    // One set for the 8-bit targets, one for the HDR scene (float); each also as a no-discard variant (early-Z for
    // draws that never cut out).
    for (int set = 0; set < 2 && ok; ++set)
    for (int nc = 0; nc < 2 && ok; ++nc)
    for (int c = 0; c < 3 && ok; ++c) {
        colorFormats[0] = set ? GetFormatInfo(Format::RGBA16F).vk : kColorFormat;
        rendering.colorAttachmentCount = cb.attachmentCount = set ? 5 : 1;
        stages[1].module = nc ? (set ? fragGlowNoCut : fragNoCut) : (set ? fragGlow : frag);
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = kClassTopology[c];
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
        ci.layout = m_pipelineLayout;
        VkPipeline* dst = set ? (nc ? &m_pipelinesHdrNoCut[c] : &m_pipelinesHdr[c])
                              : (nc ? &m_pipelinesNoCut[c] : &m_pipelines[c]);
        ok = Check(vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &ci, nullptr, dst),
                   "vkCreateGraphicsPipelines", error);
    }
    // Characters' Phong tessellation: triangles as 3-point patches, through ffp.tesc / ffp.tese (both targets).
    VkShaderModule tesc = VK_NULL_HANDLE, tese = VK_NULL_HANDLE;
    if (ok && m_tessSupported && module(kTescSpirv, sizeof(kTescSpirv), &tesc) && module(kTeseSpirv, sizeof(kTeseSpirv), &tese)) {
        VkPipelineShaderStageCreateInfo tstages[4] = {
            stages[0],
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, tesc, "main", nullptr},
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, tese, "main", nullptr},
            stages[1],
        };
        VkPipelineTessellationDomainOriginStateCreateInfo origin{
            VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_DOMAIN_ORIGIN_STATE_CREATE_INFO};
        origin.domainOrigin = VK_TESSELLATION_DOMAIN_ORIGIN_LOWER_LEFT;
        VkPipelineTessellationStateCreateInfo ts{VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO};
        ts.pNext = &origin;
        ts.patchControlPoints = 3;
        for (int set = 0; set < 2 && ok; ++set)
        for (int nc = 0; nc < 2 && ok; ++nc) {
            colorFormats[0] = set ? GetFormatInfo(Format::RGBA16F).vk : kColorFormat;
            rendering.colorAttachmentCount = cb.attachmentCount = set ? 5 : 1;
            tstages[3].module = nc ? (set ? fragGlowNoCut : fragNoCut) : (set ? fragGlow : frag);
            VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
            ia.topology = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
            VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
            ci.pNext = &rendering;
            ci.stageCount = 4;
            ci.pStages = tstages;
            ci.pInputAssemblyState = &ia;
            ci.pTessellationState = &ts;
            ci.pViewportState = &vp;
            ci.pRasterizationState = &rs;
            ci.pMultisampleState = &ms;
            ci.pDepthStencilState = &dss;
            ci.pColorBlendState = &cb;
            ci.pDynamicState = &ds;
            ci.layout = m_pipelineLayout;
            VkPipeline* dst = nc ? &m_tessPipelinesNoCut[set] : &m_tessPipelines[set];
            ok = Check(vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &ci, nullptr, dst),
                       "tessellation pipeline", error);
        }
    }
    if (tesc) vkDestroyShaderModule(m_device, tesc, nullptr);
    if (tese) vkDestroyShaderModule(m_device, tese, nullptr);
    if (ok)
        CreatePrepassPipeline(vert);             // not fatal: without it the scene draws as before
    vkDestroyShaderModule(m_device, vert, nullptr);
    vkDestroyShaderModule(m_device, fragGlow, nullptr);
    vkDestroyShaderModule(m_device, frag, nullptr);
    if (!ok || !CreateShadowResources(error) || !CreatePointShadowResources(error) || !CreateHdrResources(error) ||
        !CreateParticleResources(error) || !CreateSkinResources(error))
        return false;
    CreateGrassResources(error);                 // not fatal: without it the ground grass is simply unavailable
    {
        std::string waterError;                  // not fatal either: the game's own water stays
        if (!CreateWaterResources(&waterError)) {
            Log("water: unavailable (%s)", waterError.c_str());
            DestroyWaterResources();
        }
        UpdateWaterReady();
    }
    char gpuSkin[8] = "";
    if (GetEnvironmentVariableA("RANDYVK_GPU_SKIN", gpuSkin, sizeof(gpuSkin)) && gpuSkin[0] == '0')
        m_gpuSkin = false;
    skin::Job::SetPrefetch(!m_gpuSkin);          // skinned on the GPU: no CPU skinning ahead of draws
    char staticGpu[8] = "";
    if (GetEnvironmentVariableA("RANDYVK_STATIC_GPU", staticGpu, sizeof(staticGpu)) && staticGpu[0] == '0')
        m_staticResident = false;

    // Zero vertex data for attributes a format doesn't have (bound with stride 0).
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = 64;
    bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    VmaAllocationCreateInfo ac{};
    ac.usage = VMA_MEMORY_USAGE_AUTO;
    ac.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info;
    if (!Check(vmaCreateBuffer(m_allocator, &bi, &ac, &m_nullBuffer, &m_nullAllocation, &info), "null buffer", error))
        return false;
    std::memset(info.pMappedData, 0, 64);
    return true;
}

bool Device::CreateFrames(std::string* error)
{
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = m_queueFamily;
    if (!Check(vkCreateCommandPool(m_device, &pci, nullptr, &m_pool), "vkCreateCommandPool", error))
        return false;
    for (auto& f : m_frames) {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = m_pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 4;
        VkCommandBuffer cmds[4];
        if (!Check(vkAllocateCommandBuffers(m_device, &ai, cmds), "vkAllocateCommandBuffers", error))
            return false;
        f.upload = cmds[0];
        f.main = f.mainA = cmds[1];
        f.prepass = cmds[2];                     // the depth pre-pass (prepass.cpp)
        f.mainB = cmds[3];
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (!Check(vkCreateFence(m_device, &fi, nullptr, &f.fence), "vkCreateFence", error) ||
            !Check(vkCreateSemaphore(m_device, &si, nullptr, &f.imageAvailable), "vkCreateSemaphore", error))
            return false;
        if (!CreateRing(f, kRingSize, error))
            return false;
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        VmaAllocationCreateInfo ac{};
        ac.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        ac.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo info;
        bi.size = sizeof(FrameLights);
        bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        if (!Check(vmaCreateBuffer(m_allocator, &bi, &ac, &f.post, &f.postAllocation, &info), "post buffer", error))
            return false;
        f.postData = info.pMappedData;
        vmaGetAllocationInfo(m_allocator, f.ringAllocation, &info);
        VkMemoryPropertyFlags props;
        vmaGetMemoryTypeProperties(m_allocator, info.memoryType, &props);
        Log("ring buffer: memory type %u (flags 0x%x%s)", info.memoryType, props,
            (props & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? ", device-local = BAR window" : "");
    }
    return true;
}

// A frame's ring buffer (vertices, indices, per-draw blocks): `size` bytes, replacing any it had (the slot idle).
bool Device::CreateRing(Frame& f, VkDeviceSize size, std::string* error)
{
    if (f.ring) vmaDestroyBuffer(m_allocator, f.ring, f.ringAllocation);
    f.ring = VK_NULL_HANDLE;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
               VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |   // last frame's vertex positions (motion vectors)
               VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;   // M3: a frame's batched draw commands
    VmaAllocationCreateInfo ac{};
    // System memory, never the CPU-visible VRAM window: without resizable BAR that window is ~256 MB,
    // shared with the driver and other programs, and running it out makes allocations fail.
    ac.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    ac.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info;
    if (!Check(vmaCreateBuffer(m_allocator, &bi, &ac, &f.ring, &f.ringAllocation, &info), "ring buffer", error))
        return false;
    f.ringData = static_cast<uint8_t*>(info.pMappedData);
    f.ringSize = size;
    f.ringOffset = 0;
    return true;
}

// ---------------------------------------------------------------------------------------------------
// Frames

VkDeviceSize Device::Allocate(VkDeviceSize size, VkDeviceSize alignment, void** cpu)
{
    Frame& f = m_frames[m_frameIndex];
    // Any alignment (vertex strides); 32-bit math, the ring is far below 4 GB (64-bit division is a libcall on x86).
    uint32_t a = uint32_t(alignment), o = uint32_t(f.ringOffset);
    VkDeviceSize offset = (o + a - 1) / a * a;
    if (offset + size > f.ringSize) {
        Log("per-frame ring buffer full (%llu bytes)\n", (unsigned long long)f.ringSize);
        offset = 0;     // overwrites this frame's data; visible corruption rather than a crash
        ++m_ringGeneration;
    }
    f.ringOffset = offset + size;
    *cpu = f.ringData + offset;
    return offset;
}

void Device::WaitFrame(Frame& f, const char* what)
{
    // Reports a stuck GPU instead of hanging silently; gives up on a lost device.
    for (int seconds = 1; !m_deviceLost; ++seconds) {
        VkResult r = vkWaitForFences(m_device, 1, &f.fence, VK_TRUE, 1000000000ull);
        if (r == VK_SUCCESS) {
            if (seconds > 1)
                Log("%s finished after %d s", what, seconds);
            m_completed = std::max(m_completed, f.serial);
            return;
        }
        if (r == VK_ERROR_DEVICE_LOST) {
            m_deviceLost = true;
            Log("GPU device lost (while waiting for %s); rendering stops", what);
            return;
        }
        Log("still waiting for %s after %d s", what, seconds);
    }
}

void Device::CollectGarbage()
{
    auto textures = std::remove_if(m_deadTextures.begin(), m_deadTextures.end(), [&](auto& e) {
        if (e.first > m_completed) return false;
        DestroyTextureNow(e.second);
        return true;
    });
    m_deadTextures.erase(textures, m_deadTextures.end());
    auto images = std::remove_if(m_deadImages.begin(), m_deadImages.end(), [&](auto& e) {
        if (e.first > m_completed) return false;
        vkDestroyImageView(m_device, e.second.view, nullptr);
        vmaDestroyImage(m_allocator, e.second.image, e.second.allocation);
        return true;
    });
    m_deadImages.erase(images, m_deadImages.end());
    auto buffers = std::remove_if(m_deadBuffers.begin(), m_deadBuffers.end(), [&](auto& e) {
        if (e.first > m_completed) return false;
        vmaDestroyBuffer(m_allocator, e.second.first, e.second.second);
        return true;
    });
    m_deadBuffers.erase(buffers, m_deadBuffers.end());
    CollectArenaSlots();
}

void Device::EnsureRingSpace(VkDeviceSize bytes)
{
    Frame& f = m_frames[m_frameIndex];
    VkDeviceSize needed = bytes + 1024;                      // alignment slack for a few allocations
    if (f.ringOffset + needed <= f.ringSize)
        return;
    if (needed > f.ringSize) {
        Log("%llu bytes do not fit in the %llu-byte ring buffer\n",
                     (unsigned long long)bytes, (unsigned long long)f.ringSize);
        return;
    }
    if (m_inFrame) {
        // A flush waits for the GPU, and the frame's shadow casters recorded before it (their data overwritten)
        // drop out of this frame's shadows: the next frames get a bigger ring (BeginFrame).
        ++m_midFrameFlushes;
        m_ringPeak = std::max(m_ringPeak, f.ringOffset);
        m_ringWanted = std::min(std::max(m_ringWanted, f.ringSize * 2), kRingMaxSize);
        if (m_ringFlushesLogged++ < 20)
            Log("ring buffer full mid-frame (%llu bytes wanted, ring %llu MB): flushing\n", (unsigned long long)bytes,
                (unsigned long long)(f.ringSize >> 20));
        bool wasRendering = m_rendering;
        SubmitAndWait();                                     // also submits pending uploads
        if (wasRendering)
            BeginRenderingOn(m_target);
    } else if (f.uploadsRecorded) {
        FinishSkinUploads(f.upload);
        vkEndCommandBuffer(f.upload);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &f.upload;
        f.serial = ++m_submitted;
        // Between frames the slot's fence is signalled (UploadCommands waited on it); a fence must be
        // unsignalled to be submitted, or waits return early while the GPU still runs the commands.
        vkResetFences(m_device, 1, &f.fence);
        vkQueueSubmit(m_queue, 1, &si, f.fence);
        WaitFrame(f, "GPU frame work");
        // Leave the fence signalled: BeginFrame waits on it before using this slot.
        f.uploadsRecorded = false;
    } else {
        WaitFrame(f, "GPU frame work");
    }
    f.ringOffset = 0;
    ++m_ringGeneration;
}

// ---------------------------------------------------------------------------------------------------
// GPU-driven M2: a frame's draw arenas, slices of its ring

// Reserve this frame's deduplicated-constants and draw-record arrays in the ring (the space was ensured by the
// caller). Called when the ring was (re)started.
void Device::PrepareDrawArenas()
{
    if (m_arenaGeneration == m_ringGeneration)
        return;
    m_constCapacity = m_constWanted;
    m_recordCapacity = m_recordWanted;
    VkDeviceSize align = std::max<VkDeviceSize>(16, m_props.limits.minStorageBufferOffsetAlignment);
    VkDeviceSize shadowBytes = (m_shadows || m_pointShadows) ? VkDeviceSize(kShadowRecordCapacity) *
                                               (sizeof(ShadowRecord) + sizeof(VkDrawIndexedIndirectCommand))
                                         : 0;
    VkDeviceSize bytes = VkDeviceSize(m_constCapacity) * sizeof(DrawConstants) +
                         VkDeviceSize(m_recordCapacity) * sizeof(DrawRecord) +
                         VkDeviceSize(m_recordCapacity) * sizeof(VkDrawIndexedIndirectCommand) + shadowBytes + 64;
    EnsureRingSpace(bytes);          // the caller reserved for it, but a flush may have restarted the ring since
    void* cpu;
    m_constsBase = Allocate(VkDeviceSize(m_constCapacity) * sizeof(DrawConstants), align, &cpu);
    m_recordsBase = Allocate(VkDeviceSize(m_recordCapacity) * sizeof(DrawRecord), align, &cpu);
    m_indirectBase = Allocate(VkDeviceSize(m_recordCapacity) * sizeof(VkDrawIndexedIndirectCommand), 4, &cpu);
    m_shadowArenaCapacity = (m_shadows || m_pointShadows) ? kShadowRecordCapacity : 0;
    if (m_shadowArenaCapacity) {
        m_shadowRecordBase = Allocate(VkDeviceSize(m_shadowArenaCapacity) * sizeof(ShadowRecord), align, &cpu);
        m_shadowCmdBase = Allocate(VkDeviceSize(m_shadowArenaCapacity) * sizeof(VkDrawIndexedIndirectCommand), 4, &cpu);
    }
    m_constCount = m_recordCount = m_indirectCount = 0;
    m_shadowRecordCount = m_shadowCmdCount = 0;
    m_shadowGroup = ShadowGroup{};
    m_shadowArenaBound = false;
    m_group = DrawGroup{};
    m_constIndex = 0;
    m_constantsDirty = true;
    m_constantsGeneration = ~0ull;
    m_arenaGeneration = m_ringGeneration;
    m_arenaBound = false;            // a new base: bindings 0 and 12 must be pushed again
    m_bindlessBound = false;
}

// The arrays are full: submit what the frame has so far, wait, and start them over (the GPU has consumed them).
// The base and capacity stay; only the raw data is stale.
void Device::FlushDrawArenas()
{
    bool wasRendering = m_rendering;
    if (m_arenaFlushes++ < 20)
        Log("draw arenas full mid-frame (constants %u/%u, records %u/%u): flushing\n", m_constCount, m_constCapacity,
            m_recordCount, m_recordCapacity);
    SubmitAndWait();
    if (wasRendering)
        BeginRenderingOn(m_target);
    m_arenaBound = false;
    m_bindlessBound = false;
    m_constCount = m_recordCount = m_indirectCount = 0;
    m_group = DrawGroup{};
    m_constIndex = 0;
    m_constantsDirty = true;
    m_constantsGeneration = ~0ull;
}

// M3 stats: one indirect call issued for a group.
void Device::NoteGroup(uint32_t count)
{
    ++m_groupCalls;
    m_groupDraws += count;
    if (count == 1)
        ++m_singleDraws;
}

// The shader reads the lights only up to lightInfo.x, so only those are copied (the slot keeps its full size).
uint32_t Device::AppendConstant(const DrawConstants& c, size_t bytes)
{
    uint32_t index = m_constCount++;
    std::memcpy(m_frames[m_frameIndex].ringData + m_constsBase + VkDeviceSize(index) * sizeof(DrawConstants), &c,
                std::min(bytes, sizeof(c)));
    return index;
}

uint32_t Device::AppendRecord(uint32_t constIndex, const DrawTransform& d)
{
    uint32_t index = m_recordCount++;
    DrawRecord rec{};
    rec.constIndex = constIndex;
    rec.d = d;
    std::memcpy(m_frames[m_frameIndex].ringData + m_recordsBase + VkDeviceSize(index) * sizeof(DrawRecord), &rec,
                sizeof(rec));
    return index;
}

VkCommandBuffer Device::UploadCommands()
{
    Frame& f = m_frames[m_frameIndex];
    if (!f.uploadsRecorded) {
        if (!m_inFrame) {
            // Between frames this slot's previous submission may still be running: wait for it before
            // reusing its command buffer and ring buffer. BeginFrame keeps what is recorded here.
            WaitFrame(f, "GPU frame work");
            f.ringOffset = 0;
        }
        VkCommandBufferBeginInfo b{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        b.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(f.upload, &b);
        f.uploadsRecorded = true;
    }
    return f.upload;
}

void Device::Transition(VkCommandBuffer cmd, Texture* t, VkImageLayout to)
{
    if (t->m_layout == to)
        return;
    VkPipelineStageFlags2 srcStage, dstStage;
    VkAccessFlags2 srcAccess, dstAccess;
    LayoutUse(t->m_layout, &srcStage, &srcAccess);
    LayoutUse(to, &dstStage, &dstAccess);
    ImageBarrier(cmd, t->m_image, VK_IMAGE_ASPECT_COLOR_BIT, t->m_layout, to, srcStage, srcAccess, dstStage, dstAccess);
    t->m_layout = to;
}

void Device::EndRendering()
{
    if (m_rendering) {
        FlushEdges();                                // two-pass foliage's queued edges: over this rendering's scene
        FlushGroup();                                // the pending batched draws belong to this rendering
        ShadeQueryEnd();                             // a query begun in a rendering ends in it
        PrepassEnd(m_renderEndCause);                // the scene's rendering ends: so does a depth pre-pass segment
        vkCmdEndRendering(m_frames[m_frameIndex].main);
        m_rendering = false;
    }
    m_renderEndCause = kEndOther;
}

void Device::BeginRenderingOn(Texture* target)
{
    VkCommandBuffer cmd = m_frames[m_frameIndex].main;
    EnsureDepth(target->m_width, target->m_height);
    Transition(cmd, target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (m_depthLayout != VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL) {
        ImageBarrier(cmd, m_depth, VK_IMAGE_ASPECT_DEPTH_BIT, m_depthLayout, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
                     VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
        m_depthLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    }
    // Contents persist across target switches and frames, as in D3D (LOAD/STORE everywhere).
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = target->m_view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = m_depthView;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    // The HDR scene has the glow, the local-light fraction, the motion vectors and the surface colour as further
    // attachments, cleared at their first use in the frame.
    VkRenderingAttachmentInfo colors[5] = {color, color, color, color, color};
    uint32_t colorCount = 1;
    if (target == m_scene && m_glow && m_localFraction && m_motionVectors && m_albedo) {
        Texture* extra[4] = {m_glow, m_localFraction, m_motionVectors, m_albedo};
        for (int i = 0; i < 4; ++i) {
            Transition(cmd, extra[i], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            colors[1 + i].imageView = extra[i]->m_view;
            colors[1 + i].loadOp = m_glowCleared ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
            colors[1 + i].clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
        }
        m_glowCleared = true;
        colorCount = 5;
    }
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {target->m_width, target->m_height}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = colorCount;
    ri.pColorAttachments = colors;
    ri.pDepthAttachment = &depth;
    vkCmdBeginRendering(cmd, &ri);
    m_rendering = true;
    if (target == m_scene)
        ShadeQueryBegin();                       // the current scene class goes on counting (profile.cpp)
}

void Device::SetAnisotropy(uint32_t level)
{
    if (level < 1) level = 1;
    if (float(level) > m_maxAnisotropy) level = m_maxAnisotropy >= 1.0f ? uint32_t(m_maxAnisotropy) : 1u;
    m_anisotropy = level;                        // new samplers pick it up (SamplerFor keys on it)
}

void Device::SetRenderTarget(Texture* target)
{
    if (!target)
        target = m_main;
    if (m_uiLayerActive && target == m_main && m_uiLayer)
        target = m_uiLayer;                      // the interface is being drawn into its layer
    if (!target->m_renderTarget || !target->m_image) {
        Log("SetRenderTarget: not a (usable) render target\n");
        return;
    }
    if (target != m_target && m_inFrame) {
        m_renderEndCause = kEndTarget;
        EndRendering();
        m_target = target;
        BeginRenderingOn(target);
    }
    m_target = target;
    for (auto& t : m_textures)                  // a texture can't be sampled while it is being drawn into
        if (t == target) t = nullptr;
    m_viewport = {0, 0, target->m_width, target->m_height, 0.0f, 1.0f};
    m_constantsDirty = true;
}

void Device::BeginFrame()
{
    m_pickActive = m_pickArmed;                  // texture picking (RequestPick): this frame's draws
    m_pickArmed = false;
    m_pickBest = nullptr;
    m_pickBestVisual = m_pickBestOwner = m_pickBestKind = m_pickBestVertices = m_pickBestIndices = 0;
    m_pickDepth = 2.0f;
    m_batchRuns = m_batchMerged = m_batchMaxRun = 0;
    m_batchKey = 0;
    m_batchRun = 0;
    m_meshStaticDraws = m_meshHashedDraws = 0;
    m_bindlessBound = false;                     // the frame's command buffer starts with set 1 unbound
    m_arenaBound = false;                        // ... and with the draw arenas unbound
    m_shadowArenaBound = false;                  // ... and the shadow record array unbound
    if (m_swapchainStale) {
        vkDeviceWaitIdle(m_device);
        DestroySwapchain();
        std::string error;
        if (!CreateSwapchain(&error))
            Log("swapchain recreation failed: %s\n", error.c_str());
        m_swapchainStale = false;
    }
    Frame& f = m_frames[m_frameIndex];
    bool uploadsPending = f.uploadsRecorded;          // recorded between frames, already waited for
    double waitStart = ProfileCpu();
    WaitFrame(f, "GPU frame work");
    // The render thread waiting for the GPU to finish this slot's last frame: large means GPU-bound (profile).
    double waitMs = ProfileCpu() - waitStart;
    vkResetFences(m_device, 1, &f.fence);
    CollectGarbage();
    ApplyShadowResolution();
    if (!uploadsPending) {
        if (f.ringSize != m_ringWanted) {        // the slot is idle (waited for): a bigger (or again smaller) ring
            std::string error;
            VkDeviceSize old = f.ringSize;
            if (CreateRing(f, m_ringWanted, &error))
                Log("ring buffer: %llu -> %llu MB (%s)", (unsigned long long)(old >> 20),
                    (unsigned long long)(m_ringWanted >> 20),
                    m_ringWanted > old ? "frames overflowed it" : "frames have used little of it for a while");
            else if (!CreateRing(f, old, &error))
                Log("ring buffer: %s", error.c_str());
            else
                m_ringWanted = old;              // no memory for more: stay
        }
        m_ringPeak = std::max(m_ringPeak, f.ringOffset);
        f.ringOffset = 0;
    }
    if (m_frameNumber % 600 == 599) {            // how full frames get (logged with the profiler's numbers)
        Log("ring buffer: peak %.1f of %llu MB a frame, %u mid-frame flushes (last 600 frames)",
            double(m_ringPeak) / 1048576.0, (unsigned long long)(f.ringSize >> 20), m_midFrameFlushes);
        // The rings are mapped into the game's (32-bit) address space, two of them: grown for a burst (a zone's
        // uploads), they give it back once frames have used under a quarter of them for three windows (~30 s), so
        // a long session doesn't keep 512 MB of it - the game, short of address space, falls back to its smallest
        // ground textures.
        m_ringQuietWindows = m_ringPeak < f.ringSize / 4 && m_midFrameFlushes == 0 ? m_ringQuietWindows + 1 : 0;
        if (m_ringQuietWindows >= 3 && m_ringWanted > kRingSize) {
            m_ringWanted = std::max(kRingSize, m_ringWanted / 2);
            m_ringQuietWindows = 0;
        }
        MEMORYSTATUSEX mem{};
        mem.dwLength = sizeof(mem);
        if (GlobalMemoryStatusEx(&mem))
            Log("address space: %llu of %llu MB free (the game's process)", (unsigned long long)(mem.ullAvailVirtual >> 20),
                (unsigned long long)(mem.ullTotalVirtual >> 20));
        Log("opaque static fast path: %llu draws (last 600 frames)", (unsigned long long)m_opaqueDraws);
        Log("foliage: %llu draws, %llu of them far (LOD) (last 600 frames)", (unsigned long long)m_foliageDraws,
            (unsigned long long)m_foliageLodDraws);
        Log("foliage survey: %.0f draws a frame (%.0f static, %.0f changing), %.0f vertices; box height %.2f .. %.2f",
            double(m_foliageDraws) / 600.0, double(m_foliageStatic) / 600.0, double(m_foliageDynamic) / 600.0,
            double(m_foliageVerts) / 600.0, m_foliageMinH, m_foliageMaxH);
        Log("no-discard pipeline (early-Z): %llu draws (last 600 frames)", (unsigned long long)m_noCutDraws);
        if (m_grassOn)
            Log("ground grass: %llu frames drawn, %.0f blades a frame; tiles %llu, %llu built (%.2f ms each, %.0f%% of "
                "their blades lit by the ground's lightmap), capture %.3f ms a frame; terrain ambient %.2f %.2f %.2f; "
                "pushers now %llu max %u; trail cells %u; candidates left out: %llu clumps, %llu no grass ground, %llu steep, %llu "
                "edge; %llu tiles waiting to build; %.0f blades a frame cast the sun's shadow (last 600 frames)",
                (unsigned long long)m_grassDraws, double(m_grassBlades) / std::max<uint64_t>(m_grassDraws, 1),
                (unsigned long long)m_grassTiles.size(), (unsigned long long)m_grassBuilds,
                m_grassBuildMs / double(std::max<uint64_t>(m_grassBuilds, 1)),
                100.0 * double(m_grassLitBlades) / double(std::max<uint64_t>(m_grassBuiltBlades, 1)),
                m_grassCaptureMs / 600.0, m_terrainAmbient[0], m_terrainAmbient[1], m_terrainAmbient[2],
                (unsigned long long)m_framePushers.size(), m_pusherSeenMax, m_trailActive, (unsigned long long)m_grassLeftOut[0],
                (unsigned long long)m_grassLeftOut[1], (unsigned long long)m_grassLeftOut[2],
                (unsigned long long)m_grassLeftOut[3], (unsigned long long)GrassTilesWaiting(),
                double(m_grassShadowBlades) / 600.0);
        m_grassShadowBlades = 0;
        m_grassLitBlades = m_grassBuiltBlades = 0;
        for (uint64_t& n : m_grassLeftOut)
            n = 0;
        m_pusherSeenMax = 0;
        m_grassBuilds = 0;
        m_grassBuildMs = m_grassCaptureMs = 0.0;
        m_grassDraws = m_grassBlades = 0;
        PrepassLog();
        m_ringPeak = 0;
        m_midFrameFlushes = 0;
        m_opaqueDraws = 0;
        m_foliageDraws = m_foliageLodDraws = 0;
        m_foliageStatic = m_foliageDynamic = m_foliageVerts = 0;
        m_foliageMinH = m_foliageMaxH = 0.0f;
        m_noCutDraws = 0;
    }
    ++m_ringGeneration;                          // a different slot's ring: cached offsets are invalid
    BeginSkinFrame();
    BeginStaticFrame();

    VkCommandBufferBeginInfo b{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    b.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f.main, &b);
    m_cache = StateCache{};
    ++m_frameNumber;
    ProfileBeginFrame(f.main);
    ProfileCpuAddMs("wait for the GPU (frame slot)", waitMs);
    UpdatePushTrail();
    m_tessCharsPrev.swap(m_tessChars);           // characters drawn last frame (their rigid parts: TessellateDraw)
    m_tessChars.clear();
    m_windTimePrev = m_windTime;
    m_frameClock = SwayClock();
    m_windTime = std::fmod(m_frameClock, 3600.0);
    if (m_windTimePrev > m_windTime || m_windTime - m_windTimePrev > 0.25)
        m_windTimePrev = m_windTime;             // wrapped, or the first frame after a pause
    m_lightsPrev.swap(m_lightsCur);              // last frame's complete light set lights this frame
    m_lightsCur.clear();
    m_sunLuminance = 0.0f;
    m_casters.clear();
    m_blobShadowsReplaced.store(m_shadows && m_shadowValid, std::memory_order_relaxed);
    m_edges.clear();                             // (two-pass foliage: flushed with every rendering's end already)
    m_profileDraws += m_frameDraw;               // last frame's batching (the profile's log)
    m_frameDraw = 0;
    m_prepassEndedThisFrame = false;
    m_grassDrawnThisFrame = false;
    m_profileGroupCalls += m_groupCalls;
    m_profileGroupDraws += m_groupDraws;
    m_profileConstBlocks += m_constCount;
    m_profileConstReused += m_constantsReused;
    m_groupCalls = m_groupDraws = m_singleDraws = m_constantsReused = 0;
    m_shadowGroupCalls = m_shadowGroupDraws = 0;
    m_casterViews.clear();
    m_frameLightsDirty = true;
    m_constantsDirty = true;                     // shadow receiving depends on last frame's map
    m_frameEyeValid = false;
    m_frameViewProjValid = false;
    m_terrainLitPassPrev = m_terrainLitPassCur;
    m_terrainLitPassCur = false;
    m_terrainBases.clear();
    m_particlePending = nullptr;
    m_particleDraws = 0;
    m_particleOrphansDone = false;
    m_particleOrphanTrigger = "none";
    m_particleSaw3D = false;
    SimulateParticles(f.main);                   // last frame's effects; reads last frame's world camera
    ProfileMark("particle simulation");
    m_inFrame = true;
    BeginScene();
    PrepareShadowMap(f.main);
    PreparePointShadowMaps(f.main);
    BeginRenderingOn(m_target);
    if (!m_dumpPath.empty())
        BeginFrameDump();
}

void Device::EndFrame()
{
    if (m_pickActive) {                          // texture picking: the nearest draw's texture, for LastPick
        m_pickActive = false;
        m_pickPublished.store(m_pickBest, std::memory_order_relaxed);
        m_pickPublishedVisual.store(m_pickBestVisual, std::memory_order_relaxed);
        m_pickPublishedOwner.store(m_pickBestOwner, std::memory_order_relaxed);
        m_pickPublishedKind.store(m_pickBestKind, std::memory_order_relaxed);
        m_pickPublishedVertices.store(m_pickBestVertices, std::memory_order_relaxed);
        m_pickPublishedIndices.store(m_pickBestIndices, std::memory_order_relaxed);
        m_pickSerial.fetch_add(1, std::memory_order_release);
    }
    if (m_rendering && !m_particleOrphansDone) {
        m_particleOrphanTrigger = "end of the frame";   // no interface draw after the 3D
        DrawOrphanParticles();
    }
    EndScene();                                  // if the interface didn't end it (no interface drawn)
    Frame& f = m_frames[m_frameIndex];
    m_renderEndCause = kEndScene;
    EndRendering();
    ProfileMark("interface");
    double cpu = ProfileCpu();
    RenderShadowMap(f.main);
    ProfileCpuAdd("sun shadows", cpu);
    ProfileMark("sun shadows");
    FindCarriers();
    EndFrameDump();                              // after the carriers: it lists them
    cpu = ProfileCpu();
    RenderPointShadowMaps(f.main);
    ProfileCpuAdd("point shadows", cpu);
    ProfileMark("point shadows");
    FinishShadowFrame();
    Transition(f.main, m_main, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    bool screenshot = !m_screenshotPath.empty();
    if (screenshot) {
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {m_width, m_height, 1};
        vkCmdCopyImageToBuffer(f.main, m_main->m_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_readback, 1, &region);
    }

    uint32_t imageIndex = 0;
    bool present = false;
    double presentStart = ProfileCpu();              // acquire + submit + present: driver time (profile)
    if (m_swapchain) {
        // Bounded wait: if no image comes within 250 ms, skip presenting this frame instead of freezing.
        VkResult r = vkAcquireNextImageKHR(m_device, m_swapchain, 250000000ull, f.imageAvailable, VK_NULL_HANDLE, &imageIndex);
        present = r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR;
        if (r == VK_TIMEOUT || r == VK_NOT_READY) {
            static int timeouts;
            if (timeouts++ < 20)
                Log("no swapchain image within 250 ms, frame not presented\n");
        }
        if (r == VK_SUBOPTIMAL_KHR || r == VK_ERROR_OUT_OF_DATE_KHR)
            m_swapchainStale = true;
    }
    if (present) {
        VkImage dst = m_swapImages[imageIndex];
        ImageBarrier(f.main, dst, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkImageBlit blit{};
        blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {int32_t(m_width), int32_t(m_height), 1};
        blit.dstOffsets[1] = {int32_t(m_swapExtent.width), int32_t(m_swapExtent.height), 1};
        vkCmdBlitImage(f.main, m_main->m_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        ImageBarrier(f.main, dst, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0);
    }
    vkEndCommandBuffer(f.main);

    VkCommandBuffer cmds[5];
    uint32_t cmdCount = 0;
    if (f.uploadsRecorded) {
        FinishSkinUploads(f.upload);
        vkEndCommandBuffer(f.upload);
        cmds[cmdCount++] = f.upload;
    }
    cmdCount += FrameCommands(f, cmds + cmdCount);   // main, or split around the depth pre-pass
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = cmdCount;
    si.pCommandBuffers = cmds;
    if (present) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &f.imageAvailable;
        si.pWaitDstStageMask = &waitStage;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &m_renderDone[imageIndex];
    }
    f.serial = ++m_submitted;
    vkQueueSubmit(m_queue, 1, &si, f.fence);
    f.uploadsRecorded = false;
    FrameCommandsSubmitted(f);
    if (present) {
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &m_renderDone[imageIndex];
        pi.swapchainCount = 1;
        pi.pSwapchains = &m_swapchain;
        pi.pImageIndices = &imageIndex;
        VkResult r = vkQueuePresentKHR(m_queue, &pi);
        if (r == VK_SUBOPTIMAL_KHR || r == VK_ERROR_OUT_OF_DATE_KHR)
            m_swapchainStale = true;
    }
    ProfileCpuAdd("acquire + submit + present", presentStart);
    if (screenshot) {
        WaitFrame(f, "GPU frame work");
        SaveScreenshot();
        m_screenshotPath.clear();
    }
    m_inFrame = false;
    m_frameIndex = (m_frameIndex + 1) % kFramesInFlight;
}

void Device::SubmitAndWait()
{
    Frame& f = m_frames[m_frameIndex];
    m_renderEndCause = kEndFlush;
    EndRendering();
    vkEndCommandBuffer(f.main);
    VkCommandBuffer cmds[5];
    uint32_t n = 0;
    if (f.uploadsRecorded) {
        FinishSkinUploads(f.upload);
        vkEndCommandBuffer(f.upload);
        cmds[n++] = f.upload;
        f.uploadsRecorded = false;
    }
    n += FrameCommands(f, cmds + n);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = n;
    si.pCommandBuffers = cmds;
    f.serial = ++m_submitted;
    vkQueueSubmit(m_queue, 1, &si, f.fence);
    WaitFrame(f, "mid-frame flush");
    vkResetFences(m_device, 1, &f.fence);
    FrameCommandsSubmitted(f);                 // f.main stays the buffer being recorded
    m_arenaBound = m_bindlessBound = m_shadowArenaBound = false;   // a re-begun command buffer has nothing pushed
    VkCommandBufferBeginInfo b{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    b.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f.main, &b);          // the frame goes on; the caller resumes rendering
    m_cache = StateCache{};
}

bool Device::ReadPixels(Texture* target, void* out)
{
    if (!target || target == m_scene)
        EndScene();                              // the main target as the game sees it: 8-bit, tone mapped
    Texture* t = target && target != m_scene ? target : m_main;
    if (!t->m_renderTarget)
        return false;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = VkDeviceSize(t->m_width) * t->m_height * 4;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo ac{};
    ac.usage = VMA_MEMORY_USAGE_AUTO;
    ac.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VkBuffer buffer;
    VmaAllocation allocation;
    VmaAllocationInfo info;
    if (vmaCreateBuffer(m_allocator, &bi, &ac, &buffer, &allocation, &info) != VK_SUCCESS)
        return false;
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {t->m_width, t->m_height, 1};
    if (m_inFrame) {
        VkCommandBuffer cmd = m_frames[m_frameIndex].main;
        m_renderEndCause = kEndReadback;
        EndRendering();
        Transition(cmd, t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        vkCmdCopyImageToBuffer(cmd, t->m_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
        SubmitAndWait();
        BeginRenderingOn(m_target);
    } else {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = m_pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer cmd;
        vkAllocateCommandBuffers(m_device, &ai, &cmd);
        VkCommandBufferBeginInfo b{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        b.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &b);
        Transition(cmd, t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        vkCmdCopyImageToBuffer(cmd, t->m_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
        vkEndCommandBuffer(cmd);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        vkQueueSubmit(m_queue, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(m_queue);
        m_completed = m_submitted;
        vkFreeCommandBuffers(m_device, m_pool, 1, &cmd);
    }
    std::memcpy(out, info.pMappedData, size_t(bi.size));
    vmaDestroyBuffer(m_allocator, buffer, allocation);
    return true;
}

void Device::RequestScreenshot(const std::string& bmpPath)
{
    m_screenshotPath = bmpPath;
}

void Device::SaveScreenshot()
{
    FILE* file = std::fopen(m_screenshotPath.c_str(), "wb");
    if (!file)
        return;
    uint32_t rowBytes = m_width * 3, pad = (4 - rowBytes % 4) % 4, imageSize = (rowBytes + pad) * m_height;
    uint8_t header[54] = {'B', 'M'};
    auto put32 = [&](int at, uint32_t v) { std::memcpy(header + at, &v, 4); };
    put32(2, 54 + imageSize);
    put32(10, 54);
    put32(14, 40);
    put32(18, m_width);
    put32(22, m_height);
    header[26] = 1;
    header[28] = 24;
    put32(34, imageSize);
    std::fwrite(header, 1, 54, file);
    const uint8_t* pixels = static_cast<const uint8_t*>(m_readbackData);
    std::vector<uint8_t> row(rowBytes + pad, 0);
    for (int32_t y = int32_t(m_height) - 1; y >= 0; --y) {      // BMP rows are bottom-up, BGR like the target
        const uint8_t* src = pixels + size_t(y) * m_width * 4;
        for (uint32_t x = 0; x < m_width; ++x)
            std::memcpy(&row[x * 3], src + x * 4, 3);
        std::fwrite(row.data(), 1, row.size(), file);
    }
    std::fclose(file);
}

}  // namespace rvk
