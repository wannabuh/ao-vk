#include "rvk.h"

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vma/vk_mem_alloc.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace rvk {
namespace {

using namespace vk;

const uint32_t kVertSpirv[] = {
#include "ffp.vert.inc"
};
const uint32_t kFragSpirv[] = {
#include "ffp.frag.inc"
};

// Per-draw constants, std140; must match shaders/constants.glsl.
struct GpuLight {
    float diffuse[4], specular[4], ambient[4], position[4], direction[4], atten[4], spot[4];
};
struct DrawConstants {
    d3d::Matrix world, view, proj, texMatrix[2];
    float viewport[4];
    float matDiffuse[4], matAmbient[4], matSpecular[4], matEmissive[4];
    float ambient[4];
    float fogColor[4];
    float fogParams[4];
    float tfactor[4];
    float misc[4];
    float eyePos[4];
    float eyeDir[4];
    uint32_t vtx[4];
    uint32_t flags[4];
    uint32_t matSources[4];
    uint32_t stageA[2][4];
    uint32_t stageB[2][4];
    uint32_t lightInfo[4];
    GpuLight lights[Device::kMaxLights];
};
static_assert(sizeof(DrawConstants) % 16 == 0, "std140 block size");

enum : uint32_t { F_LIGHTING = 1, F_COLORVERTEX = 2, F_SPECULAR = 4, F_NORMALIZE = 8, F_FOG = 16, F_RANGEFOG = 32,
                  F_LOCALVIEWER = 64, F_TEX0 = 128, F_TEX1 = 256, F_ALPHATEST = 512 };

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

d3d::Matrix Identity()
{
    d3d::Matrix m{};
    for (int i = 0; i < 4; ++i)
        m.m[i][i] = 1.0f;
    return m;
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

VkSamplerAddressMode AddressMode(uint32_t a)
{
    switch (a) {
    case d3d::TADDRESS_MIRROR: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case d3d::TADDRESS_CLAMP: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case d3d::TADDRESS_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
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

// Vertex layout of a D3D flexible vertex format.
struct FvfLayout {
    uint32_t stride = 0;
    int offset[6] = {-1, -1, -1, -1, -1, -1};   // position, normal, diffuse, specular, tex0, tex1
    VkFormat format[6] = {};
};

FvfLayout DecodeFvf(uint32_t fvf)
{
    FvfLayout l;
    uint32_t pos = fvf & d3d::FVF_POSITION_MASK;
    l.offset[0] = 0;
    if (pos == d3d::FVF_XYZRHW) {
        l.format[0] = VK_FORMAT_R32G32B32A32_SFLOAT;
        l.stride = 16;
    } else {
        l.format[0] = VK_FORMAT_R32G32B32_SFLOAT;
        l.stride = 12 + (pos >= 0x6 ? ((pos - 0x4) / 2) * 4 : 0);   // XYZB1..5 blend weights
    }
    if (fvf & d3d::FVF_NORMAL) { l.offset[1] = l.stride; l.format[1] = VK_FORMAT_R32G32B32_SFLOAT; l.stride += 12; }
    if (fvf & d3d::FVF_PSIZE) l.stride += 4;
    if (fvf & d3d::FVF_DIFFUSE) { l.offset[2] = l.stride; l.format[2] = VK_FORMAT_B8G8R8A8_UNORM; l.stride += 4; }
    if (fvf & d3d::FVF_SPECULAR) { l.offset[3] = l.stride; l.format[3] = VK_FORMAT_B8G8R8A8_UNORM; l.stride += 4; }
    uint32_t texCount = (fvf & d3d::FVF_TEXCOUNT_MASK) >> d3d::FVF_TEXCOUNT_SHIFT;
    static const VkFormat kTexFormats[4] = {VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT,
                                            VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32_SFLOAT};
    static const uint32_t kTexSizes[4] = {8, 12, 16, 4};
    for (uint32_t i = 0; i < texCount; ++i) {
        uint32_t code = (fvf >> (16 + 2 * i)) & 3;
        if (i < 2) { l.offset[4 + i] = l.stride; l.format[4 + i] = kTexFormats[code]; }
        l.stride += kTexSizes[code];
    }
    return l;
}

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

}  // namespace

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
    for (auto& f : m_frames) {
        for (Texture* t : f.pendingDestroy)
            DestroyTextureNow(t);
        if (f.ring) vmaDestroyBuffer(m_allocator, f.ring, f.ringAllocation);
        if (f.fence) vkDestroyFence(m_device, f.fence, nullptr);
        if (f.imageAvailable) vkDestroySemaphore(m_device, f.imageAvailable, nullptr);
    }
    for (VkSemaphore s : m_renderDone) vkDestroySemaphore(m_device, s, nullptr);
    if (m_blackTexture) DestroyTextureNow(m_blackTexture);
    for (auto& [key, sampler] : m_samplers) vkDestroySampler(m_device, sampler, nullptr);
    for (VkPipeline p : m_pipelines) if (p) vkDestroyPipeline(m_device, p, nullptr);
    if (m_pipelineLayout) vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
    if (m_setLayout) vkDestroyDescriptorSetLayout(m_device, m_setLayout, nullptr);
    if (m_nullBuffer) vmaDestroyBuffer(m_allocator, m_nullBuffer, m_nullAllocation);
    if (m_readback) vmaDestroyBuffer(m_allocator, m_readback, m_readbackAllocation);
    if (m_colorView) vkDestroyImageView(m_device, m_colorView, nullptr);
    if (m_depthView) vkDestroyImageView(m_device, m_depthView, nullptr);
    if (m_color) vmaDestroyImage(m_allocator, m_color, m_colorAllocation);
    if (m_depth) vmaDestroyImage(m_allocator, m_depth, m_depthAllocation);
    if (m_pool) vkDestroyCommandPool(m_device, m_pool, nullptr);
    if (m_swapchain) vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
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
    return CreateInstance(error) && PickDevice(error) && CreateLogicalDevice(error) && CreateTargets(error) &&
           (!window || CreateSwapchain(error)) && CreatePipelines(error) && CreateFrames(error);
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
    if (m_window)
        extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
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

bool Device::CreateTargets(std::string* error)
{
    auto makeImage = [&](VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect, VkImage* image,
                         VmaAllocation* allocation, VkImageView* view) {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = format;
        ci.extent = {m_width, m_height, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = usage;
        VmaAllocationCreateInfo ac{};
        ac.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        if (!Check(vmaCreateImage(m_allocator, &ci, &ac, image, allocation, nullptr), "vmaCreateImage", error))
            return false;
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = *image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format;
        vi.subresourceRange = {aspect, 0, 1, 0, 1};
        return Check(vkCreateImageView(m_device, &vi, nullptr, view), "vkCreateImageView", error);
    };
    return makeImage(kColorFormat,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT, &m_color, &m_colorAllocation, &m_colorView) &&
           makeImage(kDepthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, &m_depth,
                     &m_depthAllocation, &m_depthView);
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
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
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
        ac.usage = VMA_MEMORY_USAGE_AUTO;
        ac.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo info;
        if (!Check(vmaCreateBuffer(m_allocator, &bi, &ac, &f.ring, &f.ringAllocation, &info), "ring buffer", error))
            return false;
        f.ringData = static_cast<uint8_t*>(info.pMappedData);
    }

    // Readback buffer for screenshots.
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = VkDeviceSize(m_width) * m_height * 4;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo ac{};
    ac.usage = VMA_MEMORY_USAGE_AUTO;
    ac.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info;
    if (!Check(vmaCreateBuffer(m_allocator, &bi, &ac, &m_readback, &m_readbackAllocation, &info), "readback", error))
        return false;
    m_readbackData = info.pMappedData;

    // The texture an unbound stage samples; uploaded with the first frame.
    m_frameIndex = 0;
    uint32_t black = 0xFF000000;
    m_blackTexture = CreateTexture(1, 1, &black);
    return m_blackTexture != nullptr;
}

// ---------------------------------------------------------------------------------------------------
// Frames

VkDeviceSize Device::Allocate(VkDeviceSize size, VkDeviceSize alignment, void** cpu)
{
    Frame& f = m_frames[m_frameIndex];
    VkDeviceSize offset = (f.ringOffset + alignment - 1) & ~(alignment - 1);
    if (offset + size > kRingSize) {
        std::fprintf(stderr, "rvk: per-frame ring buffer full (%llu bytes)\n", (unsigned long long)kRingSize);
        offset = 0;     // overwrites this frame's data; visible corruption rather than a crash
    }
    f.ringOffset = offset + size;
    *cpu = f.ringData + offset;
    return offset;
}

VkCommandBuffer Device::UploadCommands()
{
    Frame& f = m_frames[m_frameIndex];
    if (!f.uploadsRecorded) {
        if (!m_inFrame) {
            // Between frames this slot's previous submission may still be running: wait for it before
            // reusing its command buffer and ring buffer. BeginFrame keeps what is recorded here.
            vkWaitForFences(m_device, 1, &f.fence, VK_TRUE, UINT64_MAX);
            f.ringOffset = 0;
        }
        VkCommandBufferBeginInfo b{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        b.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(f.upload, &b);
        f.uploadsRecorded = true;
    }
    return f.upload;
}

void Device::BeginFrame()
{
    Frame& f = m_frames[m_frameIndex];
    bool uploadsPending = f.uploadsRecorded;          // set by Init for the very first frame
    vkWaitForFences(m_device, 1, &f.fence, VK_TRUE, UINT64_MAX);
    vkResetFences(m_device, 1, &f.fence);
    for (Texture* t : f.pendingDestroy)
        DestroyTextureNow(t);
    f.pendingDestroy.clear();
    if (!uploadsPending)
        f.ringOffset = 0;
    f.uploadsRecorded = uploadsPending;

    VkCommandBufferBeginInfo b{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    b.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f.main, &b);
    ImageBarrier(f.main, m_color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    ImageBarrier(f.main, m_depth, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
                 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);

    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = m_colorView;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;      // contents are undefined at frame start, like a D3D flip
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = m_depthView;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.clearValue.depthStencil = {1.0f, 0};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {m_width, m_height}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    ri.pDepthAttachment = &depth;
    vkCmdBeginRendering(f.main, &ri);
    m_inFrame = true;
}

void Device::EndFrame()
{
    Frame& f = m_frames[m_frameIndex];
    vkCmdEndRendering(f.main);
    ImageBarrier(f.main, m_color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);

    bool screenshot = !m_screenshotPath.empty();
    if (screenshot) {
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {m_width, m_height, 1};
        vkCmdCopyImageToBuffer(f.main, m_color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_readback, 1, &region);
    }

    uint32_t imageIndex = 0;
    bool present = false;
    if (m_swapchain) {
        VkResult r = vkAcquireNextImageKHR(m_device, m_swapchain, UINT64_MAX, f.imageAvailable, VK_NULL_HANDLE, &imageIndex);
        present = r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR;
    }
    if (present) {
        VkImage dst = m_swapImages[imageIndex];
        ImageBarrier(f.main, dst, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkImageBlit blit{};
        blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {int32_t(m_width), int32_t(m_height), 1};
        blit.dstOffsets[1] = {int32_t(m_swapExtent.width), int32_t(m_swapExtent.height), 1};
        vkCmdBlitImage(f.main, m_color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &blit, VK_FILTER_LINEAR);
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
    vkQueueSubmit(m_queue, 1, &si, f.fence);
    f.uploadsRecorded = false;
    if (present) {
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &m_renderDone[imageIndex];
        pi.swapchainCount = 1;
        pi.pSwapchains = &m_swapchain;
        pi.pImageIndices = &imageIndex;
        vkQueuePresentKHR(m_queue, &pi);
    }
    if (screenshot) {
        vkWaitForFences(m_device, 1, &f.fence, VK_TRUE, UINT64_MAX);
        SaveScreenshot();
        m_screenshotPath.clear();
    }
    m_inFrame = false;
    m_frameIndex = (m_frameIndex + 1) % kFramesInFlight;
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
    for (int32_t y = int32_t(m_height) - 1; y >= 0; --y) {      // BMP rows are bottom-up
        const uint8_t* src = pixels + size_t(y) * m_width * 4;
        for (uint32_t x = 0; x < m_width; ++x) {
            row[x * 3 + 0] = src[x * 4 + 2];
            row[x * 3 + 1] = src[x * 4 + 1];
            row[x * 3 + 2] = src[x * 4 + 0];
        }
        std::fwrite(row.data(), 1, row.size(), file);
    }
    std::fclose(file);
}

// ---------------------------------------------------------------------------------------------------
// Textures and samplers

Texture* Device::CreateTexture(uint32_t width, uint32_t height, const void* argbPixels)
{
    auto* t = new Texture;
    t->m_width = width;
    t->m_height = height;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_B8G8R8A8_UNORM;        // D3DCOLOR byte order
    ci.extent = {width, height, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo ac{};
    ac.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateImage(m_allocator, &ci, &ac, &t->m_image, &t->m_allocation, nullptr) != VK_SUCCESS) {
        delete t;
        return nullptr;
    }
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = t->m_image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = ci.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(m_device, &vi, nullptr, &t->m_view);

    // Stage through this frame's ring buffer; the upload command buffer runs before the frame's draws.
    VkCommandBuffer cmd = UploadCommands();
    void* cpu;
    VkDeviceSize bytes = VkDeviceSize(width) * height * 4;
    VkDeviceSize offset = Allocate(bytes, 16, &cpu);
    std::memcpy(cpu, argbPixels, bytes);
    ImageBarrier(cmd, t->m_image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                 VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkBufferImageCopy region{};
    region.bufferOffset = offset;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(cmd, m_frames[m_frameIndex].ring, t->m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    ImageBarrier(cmd, t->m_image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                 VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    return t;
}

void Device::DestroyTexture(Texture* texture)
{
    if (!texture)
        return;
    for (auto& t : m_textures)
        if (t == texture) t = nullptr;
    m_frames[m_frameIndex].pendingDestroy.push_back(texture);   // freed once this frame's GPU work is done
}

void Device::DestroyTextureNow(Texture* t)
{
    vkDestroyImageView(m_device, t->m_view, nullptr);
    vmaDestroyImage(m_allocator, t->m_image, t->m_allocation);
    delete t;
}

VkSampler Device::SamplerFor(uint32_t stage)
{
    const auto& t = m_tss[stage];
    uint32_t addrU = t[d3d::TSS_ADDRESSU], addrV = t[d3d::TSS_ADDRESSV];
    uint64_t key = uint64_t(t[d3d::TSS_MAGFILTER] & 7) | uint64_t(t[d3d::TSS_MINFILTER] & 7) << 3 |
                   uint64_t(t[d3d::TSS_MIPFILTER] & 7) << 6 | uint64_t(addrU & 7) << 9 | uint64_t(addrV & 7) << 12 |
                   uint64_t(t[d3d::TSS_BORDERCOLOR]) << 32;
    auto it = m_samplers.find(key);
    if (it != m_samplers.end())
        return it->second;
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter = t[d3d::TSS_MAGFILTER] == d3d::TFG_POINT ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    ci.minFilter = t[d3d::TSS_MINFILTER] == d3d::TFN_POINT ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    ci.mipmapMode = t[d3d::TSS_MIPFILTER] == d3d::TFP_LINEAR ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.maxLod = t[d3d::TSS_MIPFILTER] == d3d::TFP_NONE ? 0.0f : VK_LOD_CLAMP_NONE;
    ci.addressModeU = AddressMode(addrU);
    ci.addressModeV = AddressMode(addrV);
    ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    ci.borderColor = (t[d3d::TSS_BORDERCOLOR] >> 24) ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK
                                                     : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    VkSampler sampler = VK_NULL_HANDLE;
    vkCreateSampler(m_device, &ci, nullptr, &sampler);
    m_samplers.emplace(key, sampler);
    return sampler;
}

// ---------------------------------------------------------------------------------------------------
// D3D7 state

void Device::SetRenderState(uint32_t state, uint32_t value)
{
    if (state < m_rs.size())
        m_rs[state] = value;
}

void Device::SetTextureStageState(uint32_t stage, uint32_t type, uint32_t value)
{
    if (stage >= 2 || type >= d3d::TSS_COUNT)
        return;
    m_tss[stage][type] = value;
    if (type == d3d::TSS_ADDRESS)                      // ADDRESS sets both U and V
        m_tss[stage][d3d::TSS_ADDRESSU] = m_tss[stage][d3d::TSS_ADDRESSV] = value;
}

void Device::SetTransform(uint32_t type, const d3d::Matrix& m)
{
    switch (type) {
    case d3d::World: m_world = m; break;
    case d3d::View: m_view = m; break;
    case d3d::Projection: m_proj = m; break;
    case d3d::Texture0: m_texMatrix[0] = m; break;
    case d3d::Texture1: m_texMatrix[1] = m; break;
    default: break;
    }
}

void Device::SetMaterial(const d3d::Material& m) { m_material = m; }

void Device::SetLight(uint32_t index, const d3d::Light& light)
{
    if (index >= m_lights.size())
        m_lights.resize(index + 1);
    m_lights[index].light = light;
}

void Device::LightEnable(uint32_t index, bool enable)
{
    if (index >= m_lights.size())
        m_lights.resize(index + 1);
    m_lights[index].enabled = enable;
}

void Device::SetTexture(uint32_t stage, Texture* texture)
{
    if (stage < 2)
        m_textures[stage] = texture;
}

void Device::SetViewport(const d3d::Viewport& vp) { m_viewport = vp; }

void Device::Clear(uint32_t flags, uint32_t argb, float z)
{
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
    // D3D clears the viewport rectangle.
    uint32_t x = std::min(m_viewport.x, m_width), y = std::min(m_viewport.y, m_height);
    VkClearRect rect{{{int32_t(x), int32_t(y)}, {std::min(m_viewport.width, m_width - x), std::min(m_viewport.height, m_height - y)}}, 0, 1};
    vkCmdClearAttachments(m_frames[m_frameIndex].main, n, att, 1, &rect);
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

void Device::ApplyDynamicState(uint32_t primitive, uint32_t fvf)
{
    VkCommandBuffer cmd = m_frames[m_frameIndex].main;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelines[TopologyClass(primitive)]);
    vkCmdSetPrimitiveTopology(cmd, Topology(primitive));

    // Negative height flips Y so D3D's clip space maps the same way; +0.5 matches D3D pixel centres.
    VkViewport vp;
    vp.x = float(m_viewport.x) + 0.5f;
    vp.y = float(m_viewport.y + m_viewport.height) + 0.5f;
    vp.width = float(m_viewport.width);
    vp.height = -float(m_viewport.height);
    vp.minDepth = m_viewport.minZ;
    vp.maxDepth = m_viewport.maxZ;
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D scissor{{int32_t(m_viewport.x), int32_t(m_viewport.y)}, {m_viewport.width, m_viewport.height}};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // D3D front faces are clockwise on screen; D3DCULL_CCW culls the counter-clockwise (back) ones.
    vkCmdSetFrontFace(cmd, VK_FRONT_FACE_CLOCKWISE);
    uint32_t cull = m_rs[d3d::RS_CULLMODE];
    vkCmdSetCullMode(cmd, cull == d3d::CULL_CCW ? VK_CULL_MODE_BACK_BIT
                          : cull == d3d::CULL_CW ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_NONE);
    bool zEnable = m_rs[d3d::RS_ZENABLE] != 0;
    vkCmdSetDepthTestEnable(cmd, zEnable);
    vkCmdSetDepthWriteEnable(cmd, zEnable && m_rs[d3d::RS_ZWRITEENABLE] != 0);
    vkCmdSetDepthCompareOp(cmd, CompareOp(m_rs[d3d::RS_ZFUNC]));

    VkBool32 blend = m_rs[d3d::RS_ALPHABLENDENABLE] != 0;
    vkCmdSetColorBlendEnableEXT(cmd, 0, 1, &blend);
    VkColorBlendEquationEXT eq{};
    uint32_t src = m_rs[d3d::RS_SRCBLEND], dst = m_rs[d3d::RS_DESTBLEND];
    if (src == d3d::BLEND_BOTHSRCALPHA) { src = d3d::BLEND_SRCALPHA; dst = d3d::BLEND_INVSRCALPHA; }
    if (src == d3d::BLEND_BOTHINVSRCALPHA) { src = d3d::BLEND_INVSRCALPHA; dst = d3d::BLEND_SRCALPHA; }
    eq.srcColorBlendFactor = eq.srcAlphaBlendFactor = BlendFactor(src);
    eq.dstColorBlendFactor = eq.dstAlphaBlendFactor = BlendFactor(dst);
    eq.colorBlendOp = eq.alphaBlendOp = VK_BLEND_OP_ADD;
    vkCmdSetColorBlendEquationEXT(cmd, 0, 1, &eq);

    // Vertex layout: binding 0 = the draw's vertices, binding 1 = zeros for missing attributes.
    FvfLayout layout = DecodeFvf(fvf);
    VkVertexInputBindingDescription2EXT bindings[2] = {
        {VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT, nullptr, 0, layout.stride, VK_VERTEX_INPUT_RATE_VERTEX, 1},
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
}

void Device::Draw(uint32_t primitive, uint32_t fvf, const void* vertices, uint32_t vertexCount,
                  const uint16_t* indices, uint32_t indexCount)
{
    if (!m_inFrame || !vertexCount)
        return;
    Frame& f = m_frames[m_frameIndex];
    VkCommandBuffer cmd = f.main;
    FvfLayout layout = DecodeFvf(fvf);

    // Per-draw constants.
    void* cpu;
    VkDeviceSize uboOffset = Allocate(sizeof(DrawConstants), m_props.limits.minUniformBufferOffsetAlignment, &cpu);
    auto* c = static_cast<DrawConstants*>(cpu);
    c->world = m_world;
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
            g.spot[0] = std::cos(l.theta * 0.5f);
            g.spot[1] = std::cos(l.phi * 0.5f);
            g.spot[2] = g.spot[3] = 0.0f;
        }
    c->lightInfo[0] = lightCount;
    c->lightInfo[1] = c->lightInfo[2] = c->lightInfo[3] = 0;

    // Geometry.
    VkDeviceSize vbBytes = VkDeviceSize(layout.stride) * vertexCount;
    VkDeviceSize vbOffset = Allocate(vbBytes, 16, &cpu);
    std::memcpy(cpu, vertices, vbBytes);
    VkDeviceSize ibOffset = 0;
    if (indices) {
        ibOffset = Allocate(VkDeviceSize(indexCount) * 2, 4, &cpu);
        std::memcpy(cpu, indices, size_t(indexCount) * 2);
    }

    ApplyDynamicState(primitive, fvf);

    VkDescriptorBufferInfo ubo{f.ring, uboOffset, sizeof(DrawConstants)};
    VkDescriptorImageInfo images[2];
    for (uint32_t s = 0; s < 2; ++s) {
        Texture* t = m_textures[s] ? m_textures[s] : m_blackTexture;
        images[s] = {SamplerFor(s), t->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }
    VkWriteDescriptorSet writes[3] = {};
    for (int i = 0; i < 3; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstBinding = uint32_t(i);
        writes[i].descriptorCount = 1;
    }
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].pBufferInfo = &ubo;
    writes[1].descriptorType = writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo = &images[0];
    writes[2].pImageInfo = &images[1];
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 3, writes);

    VkBuffer buffers[2] = {f.ring, m_nullBuffer};
    VkDeviceSize offsets[2] = {vbOffset, 0};
    vkCmdBindVertexBuffers(cmd, 0, 2, buffers, offsets);
    if (indices) {
        vkCmdBindIndexBuffer(cmd, f.ring, ibOffset, VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexed(cmd, indexCount, 1, 0, 0, 0);
    } else {
        vkCmdDraw(cmd, vertexCount, 1, 0, 0);
    }
}

}  // namespace rvk
