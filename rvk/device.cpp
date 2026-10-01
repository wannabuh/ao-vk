// Device setup, main targets, swapchain, frames, render target switching, screenshots.
#include "internal.h"

#include <algorithm>
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
const uint32_t kFragSpirv[] = {
#include "ffp.frag.inc"
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
    b.subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, 1};
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
    m_completed = UINT64_MAX;                   // the device is idle: everything deferred can go
    CollectGarbage();
    for (auto& f : m_frames) {
        if (f.ring) vmaDestroyBuffer(m_allocator, f.ring, f.ringAllocation);
        if (f.fence) vkDestroyFence(m_device, f.fence, nullptr);
        if (f.imageAvailable) vkDestroySemaphore(m_device, f.imageAvailable, nullptr);
    }
    if (m_blackTexture) DestroyTextureNow(m_blackTexture);
    for (auto& [key, sampler] : m_samplers) vkDestroySampler(m_device, sampler, nullptr);
    for (VkPipeline p : m_pipelines) if (p) vkDestroyPipeline(m_device, p, nullptr);
    if (m_pipelineLayout) vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
    if (m_setLayout) vkDestroyDescriptorSetLayout(m_device, m_setLayout, nullptr);
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
    if (!CreateInstance(error) || !PickDevice(error) || !CreateLogicalDevice(error) || !CreateFrames(error) ||
        !CreateMainTargets(error) || (window && !CreateSwapchain(error)) || !CreatePipelines(error))
        return false;
    uint32_t black = 0xFF000000;
    m_blackTexture = CreateTexture(1, 1, &black);
    return m_blackTexture != nullptr;
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
                                           VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME};
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
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &v13;
    v13.pNext = &eds3;
    eds3.pNext = &vertexInput;
    vkGetPhysicalDeviceFeatures2(m_physical, &features);
    if (!v13.dynamicRendering || !v13.synchronization2 || !eds3.extendedDynamicState3ColorBlendEnable ||
        !eds3.extendedDynamicState3ColorBlendEquation || !vertexInput.vertexInputDynamicState ||
        !v13.shaderDemoteToHelperInvocation) {
        if (error) *error = "required Vulkan features missing (dynamic rendering, sync2, dynamic blend/vertex input)";
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
    VkPhysicalDeviceVulkan13Features enV13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    enV13.pNext = &enEds3;
    enV13.dynamicRendering = VK_TRUE;
    enV13.synchronization2 = VK_TRUE;
    enV13.shaderDemoteToHelperInvocation = VK_TRUE;     // glslc turns `discard` into OpDemoteToHelperInvocation
    VkPhysicalDeviceFeatures2 enabled{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    enabled.pNext = &enV13;
    enabled.features.samplerAnisotropy = features.features.samplerAnisotropy;
    // Out-of-range vertex indices in game data read zeros instead of faulting the GPU.
    enabled.features.robustBufferAccess = features.features.robustBufferAccess;
    enabled.features.textureCompressionBC = features.features.textureCompressionBC;

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
    m_main = CreateImage(m_width, m_height, Format::A8R8G8B8, 1, true);
    if (!m_main) {
        if (error) *error = "main colour target";
        return false;
    }
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
    ci.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
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
    if (m_main) { DestroyTextureNow(m_main); m_main = nullptr; }
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
    VkDescriptorSetLayoutBinding bindings[3] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    };
    VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    sl.bindingCount = 3;
    sl.pBindings = bindings;
    if (!Check(vkCreateDescriptorSetLayout(m_device, &sl, nullptr, &m_setLayout), "vkCreateDescriptorSetLayout", error))
        return false;
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &m_setLayout;
    if (!Check(vkCreatePipelineLayout(m_device, &pl, nullptr, &m_pipelineLayout), "vkCreatePipelineLayout", error))
        return false;

    auto module = [&](const uint32_t* code, size_t size, VkShaderModule* out) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = size;
        ci.pCode = code;
        return Check(vkCreateShaderModule(m_device, &ci, nullptr, out), "vkCreateShaderModule", error);
    };
    VkShaderModule vert, frag;
    if (!module(kVertSpirv, sizeof(kVertSpirv), &vert) || !module(kFragSpirv, sizeof(kFragSpirv), &frag))
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
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = sizeof(dynamic) / sizeof(dynamic[0]);
    ds.pDynamicStates = dynamic;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
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
    rendering.depthAttachmentFormat = kDepthFormat;

    static const VkPrimitiveTopology kClassTopology[3] = {VK_PRIMITIVE_TOPOLOGY_POINT_LIST,
                                                          VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
                                                          VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    bool ok = true;
    for (int c = 0; c < 3 && ok; ++c) {
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
        ok = Check(vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &ci, nullptr, &m_pipelines[c]),
                   "vkCreateGraphicsPipelines", error);
    }
    vkDestroyShaderModule(m_device, vert, nullptr);
    vkDestroyShaderModule(m_device, frag, nullptr);
    if (!ok)
        return false;

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
        ai.commandBufferCount = 2;
        VkCommandBuffer cmds[2];
        if (!Check(vkAllocateCommandBuffers(m_device, &ai, cmds), "vkAllocateCommandBuffers", error))
            return false;
        f.upload = cmds[0];
        f.main = cmds[1];
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (!Check(vkCreateFence(m_device, &fi, nullptr, &f.fence), "vkCreateFence", error) ||
            !Check(vkCreateSemaphore(m_device, &si, nullptr, &f.imageAvailable), "vkCreateSemaphore", error))
            return false;
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = kRingSize;
        bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                   VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo ac{};
        // System memory, never the CPU-visible VRAM window: without resizable BAR that window is ~256 MB,
        // shared with the driver and other programs, and running it out makes allocations fail.
        ac.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        ac.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo info;
        if (!Check(vmaCreateBuffer(m_allocator, &bi, &ac, &f.ring, &f.ringAllocation, &info), "ring buffer", error))
            return false;
        f.ringData = static_cast<uint8_t*>(info.pMappedData);
        VkMemoryPropertyFlags props;
        vmaGetMemoryTypeProperties(m_allocator, info.memoryType, &props);
        Log("ring buffer: memory type %u (flags 0x%x%s)", info.memoryType, props,
            (props & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? ", device-local = BAR window" : "");
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------
// Frames

VkDeviceSize Device::Allocate(VkDeviceSize size, VkDeviceSize alignment, void** cpu)
{
    Frame& f = m_frames[m_frameIndex];
    VkDeviceSize offset = (f.ringOffset + alignment - 1) / alignment * alignment;   // any alignment (vertex strides)
    if (offset + size > kRingSize) {
        Log("per-frame ring buffer full (%llu bytes)\n", (unsigned long long)kRingSize);
        offset = 0;     // overwrites this frame's data; visible corruption rather than a crash
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
}

void Device::EnsureRingSpace(VkDeviceSize bytes)
{
    Frame& f = m_frames[m_frameIndex];
    VkDeviceSize needed = bytes + 1024;                      // alignment slack for a few allocations
    if (f.ringOffset + needed <= kRingSize)
        return;
    if (needed > kRingSize) {
        Log("%llu bytes do not fit in the %llu-byte ring buffer\n",
                     (unsigned long long)bytes, (unsigned long long)kRingSize);
        return;
    }
    if (m_inFrame) {
        bool wasRendering = m_rendering;
        SubmitAndWait();                                     // also submits pending uploads
        if (wasRendering)
            BeginRenderingOn(m_target);
    } else if (f.uploadsRecorded) {
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
        vkCmdEndRendering(m_frames[m_frameIndex].main);
        m_rendering = false;
    }
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
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {target->m_width, target->m_height}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    ri.pDepthAttachment = &depth;
    vkCmdBeginRendering(cmd, &ri);
    m_rendering = true;
}

void Device::SetRenderTarget(Texture* target)
{
    if (!target)
        target = m_main;
    if (!target->m_renderTarget) {
        Log("SetRenderTarget: not a render target\n");
        return;
    }
    if (target != m_target && m_inFrame) {
        EndRendering();
        m_target = target;
        BeginRenderingOn(target);
    }
    m_target = target;
    for (auto& t : m_textures)                  // a texture can't be sampled while it is being drawn into
        if (t == target) t = nullptr;
    m_viewport = {0, 0, target->m_width, target->m_height, 0.0f, 1.0f};
}

void Device::BeginFrame()
{
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
    WaitFrame(f, "GPU frame work");
    vkResetFences(m_device, 1, &f.fence);
    CollectGarbage();
    if (!uploadsPending)
        f.ringOffset = 0;

    VkCommandBufferBeginInfo b{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    b.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f.main, &b);
    m_cache = StateCache{};
    m_inFrame = true;
    BeginRenderingOn(m_target);
}

void Device::EndFrame()
{
    Frame& f = m_frames[m_frameIndex];
    EndRendering();
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

    VkCommandBuffer cmds[2];
    uint32_t cmdCount = 0;
    if (f.uploadsRecorded) {
        vkEndCommandBuffer(f.upload);
        cmds[cmdCount++] = f.upload;
    }
    cmds[cmdCount++] = f.main;
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
    EndRendering();
    vkEndCommandBuffer(f.main);
    VkCommandBuffer cmds[2];
    uint32_t n = 0;
    if (f.uploadsRecorded) {
        vkEndCommandBuffer(f.upload);
        cmds[n++] = f.upload;
        f.uploadsRecorded = false;
    }
    cmds[n++] = f.main;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = n;
    si.pCommandBuffers = cmds;
    f.serial = ++m_submitted;
    vkQueueSubmit(m_queue, 1, &si, f.fence);
    WaitFrame(f, "mid-frame flush");
    vkResetFences(m_device, 1, &f.fence);
    VkCommandBufferBeginInfo b{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    b.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f.main, &b);          // the frame goes on; the caller resumes rendering
    m_cache = StateCache{};
}

bool Device::ReadPixels(Texture* target, void* out)
{
    Texture* t = target ? target : m_main;
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
