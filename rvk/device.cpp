// Device setup, main targets, swapchain, frames, render target switching, screenshots.
#include "internal.h"
#include "dynemu.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

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
const uint32_t kLeafVertSpirv[] = {             // ffp.vert built with RVK_LEAF: a canopy's leaves (leaves.cpp)
#include "leaf.vert.inc"
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
// The same four without the ground's code (RVK_NO_GROUND, terrain.glsl): every draw but the ground's.
const uint32_t kFragNgSpirv[] = {
#include "ffp_ng.frag.inc"
};
const uint32_t kFragGlowNgSpirv[] = {
#include "ffp_glow_ng.frag.inc"
};
const uint32_t kFragNoCutNgSpirv[] = {
#include "ffp_nocut_ng.frag.inc"
};
const uint32_t kFragGlowNoCutNgSpirv[] = {
#include "ffp_glow_nocut_ng.frag.inc"
};
// ... and without the frame lights' loop either (RVK_NO_FRAME_LIGHTS, lighting.glsl): the draws none reaches.
const uint32_t kFragNfSpirv[] = {
#include "ffp_nf.frag.inc"
};
const uint32_t kFragGlowNfSpirv[] = {
#include "ffp_glow_nf.frag.inc"
};
const uint32_t kFragNoCutNfSpirv[] = {
#include "ffp_nocut_nf.frag.inc"
};
const uint32_t kFragGlowNoCutNfSpirv[] = {
#include "ffp_glow_nocut_nf.frag.inc"
};
// The 8-bit targets' 2D draws (the interface): the lean main() alone (RVK_LITE, RVK_LiteShader), as cut-out and not.
const uint32_t kFragLiteSpirv[] = {
#include "ffp_lite.frag.inc"
};
const uint32_t kFragLiteNoCutSpirv[] = {
#include "ffp_lite_nocut.frag.inc"
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
    case VK_IMAGE_LAYOUT_GENERAL:                // (storage images: FSR 2's output, written by compute)
        *stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        *access = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        break;
    default:
        *stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        *access = 0;
        break;
    }
}

}  // namespace

AddressSpace QueryAddressSpace()
{
    AddressSpace a;
    MEMORY_BASIC_INFORMATION mbi;
    for (uintptr_t p = 0x10000; VirtualQuery(reinterpret_cast<const void*>(p), &mbi, sizeof(mbi)) == sizeof(mbi);) {
        if (mbi.State == MEM_FREE) {
            a.free += mbi.RegionSize;
            a.largest = std::max<uint64_t>(a.largest, mbi.RegionSize);
        }
        const uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (next <= p) break;                    // (the top of the address space)
        p = next;
    }
    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem)) a.total = mem.ullTotalVirtual;   // (the whole is right everywhere)
    return a;
}

AddressSpace LatestAddressSpace()
{
    static std::atomic<uint64_t> free{0}, largest{0}, total{0};
    static std::once_flag started;
    std::call_once(started, [] {
        std::thread([] {
            for (;;) {
                const AddressSpace a = QueryAddressSpace();
                free.store(a.free, std::memory_order_relaxed);
                largest.store(a.largest, std::memory_order_relaxed);
                total.store(a.total, std::memory_order_relaxed);
                Sleep(1000);
            }
        }).detach();
    });
    return {free.load(std::memory_order_relaxed), largest.load(std::memory_order_relaxed),
            total.load(std::memory_order_relaxed)};
}

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
    WaitIdle();
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
    if (m_blueNoise) DestroyTextureNow(m_blueNoise);
    if (m_flatNormal) DestroyTextureNow(m_flatNormal);
    if (m_rock.pending.valid()) m_rock.pending.wait();   // (the cliffs' rock: a decode still running finishes first)
    for (Texture* t : {m_rock.albedo, m_rock.normal, m_rock.orm, m_native.codes, m_native.maps, m_native.means, m_native.lightmap, m_native.lightmapGrey, m_native.lightmapPrev, m_native.sky})
        if (t) DestroyTextureNow(t);
    for (auto& [key, sampler] : m_samplers) vkDestroySampler(m_device, sampler, nullptr);
    for (VkPipeline p : m_pipelines) if (p) vkDestroyPipeline(m_device, p, nullptr);
    for (VkPipeline p : m_pipelinesHdr) if (p) vkDestroyPipeline(m_device, p, nullptr);
    for (VkPipeline p : m_pipelinesNoCut) if (p) vkDestroyPipeline(m_device, p, nullptr);
    for (VkPipeline p : m_pipelinesHdrNoCut) if (p) vkDestroyPipeline(m_device, p, nullptr);
    for (VkPipeline p : m_tessPipelines) if (p) vkDestroyPipeline(m_device, p, nullptr);
    for (VkPipeline p : m_tessPipelinesNoCut) if (p) vkDestroyPipeline(m_device, p, nullptr);
    for (VkPipeline* set : {m_groundPipelines, m_groundPipelinesHdr, m_groundPipelinesNoCut, m_groundPipelinesHdrNoCut,
                            m_noFramePipelines, m_noFramePipelinesHdr, m_noFramePipelinesNoCut, m_noFramePipelinesHdrNoCut,
                            m_pipelinesLite, m_pipelinesLiteNoCut})
        for (int c = 0; c < 3; ++c) if (set[c]) vkDestroyPipeline(m_device, set[c], nullptr);
    for (VkPipeline* set : {m_groundTessPipelines, m_groundTessPipelinesNoCut, m_noFrameTessPipelines, m_noFrameTessPipelinesNoCut})
        for (int k = 0; k < 2; ++k) if (set[k]) vkDestroyPipeline(m_device, set[k], nullptr);
    if (m_prepassPipeline) vkDestroyPipeline(m_device, m_prepassPipeline, nullptr);
    if (m_prepassCutoutPipeline) vkDestroyPipeline(m_device, m_prepassCutoutPipeline, nullptr);
    if (m_leafPrepassPipeline) vkDestroyPipeline(m_device, m_leafPrepassPipeline, nullptr);
    DestroyShadowResources();
    DestroyPointShadowResources();
    DestroyHdrResources();
    DestroyDarknessResources();
    DestroyProfiler();
    DestroyParticleResources();
    DestroyFxResources();
    DestroySkinResources();
    DestroyGrassResources();
    DestroyLeafResources();
    DestroyWaterResources();
    DestroyRainResources();
    DestroySmokeResources();
    DestroyCloudResources();
    DestroyStaticGeometry();
    DestroyStatels();
    DestroyHandover();
    DestroyPortals();
    CollectGarbage();                            // (what those let go of: DestroyTexture defers - the device is idle)
    SavePipelineCache("at exit");
    if (m_pipelineCache) vkDestroyPipelineCache(m_device, m_pipelineCache, nullptr);
    if (m_pipelineLayout) vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
    if (m_setLayout) vkDestroyDescriptorSetLayout(m_device, m_setLayout, nullptr);
    if (m_bindlessPool) vkDestroyDescriptorPool(m_device, m_bindlessPool, nullptr);
    m_bindlessSet = VK_NULL_HANDLE;              // (freed with its pool: the targets below were made after it - their
                                                 // slots aren't cleared)
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
    // Each step timed, so the log says where a slow start goes (one line, below).
    using Clock = std::chrono::steady_clock;
    const Clock::time_point begin = Clock::now();
    Clock::time_point mark = begin;
    std::string timings;
    bool ok = true;
    auto step = [&](const char* name, auto&& run) {
        if (!ok)
            return;
        ok = run();
        char part[64];
        std::snprintf(part, sizeof(part), "%s%s %d", timings.empty() ? "" : ", ", name,
                      int(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - mark).count()));
        timings += part;
        mark = Clock::now();
    };
    step("instance", [&] { return CreateInstance(error); });
    step("gpu", [&] { return PickDevice(error); });
    step("device", [&] { return CreateLogicalDevice(error); });
    step("cache", [&] { CreatePipelineCache(); return true; });
    step("frames", [&] { return CreateFrames(error); });
    step("profiler", [&] { return CreateProfiler(error); });
    step("targets", [&] { return CreateMainTargets(error); });
    step("swapchain", [&] { return !window || CreateSwapchain(error); });
    step("pipelines", [&] { return CreatePipelines(error); });
    if (ok)
        SavePipelineCache("after the pipelines were made");
    Log("startup: %s%s; total %d ms", timings.c_str(), ok ? "" : " (failed)",
        int(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - begin).count()));
    if (!ok)
        return false;
    uint32_t black = 0xFF000000;
    m_blackTexture = CreateTexture(1, 1, &black);
    uint32_t flat = 0xFF8080FF;                  // (0.5, 0.5, 1): the unbent normal
    m_flatNormal = CreateTexture(1, 1, &flat);
    m_blueNoise = CreateBlueNoise();
    return m_blackTexture != nullptr && m_flatNormal != nullptr && m_blueNoise != nullptr;
}

namespace {

std::string g_pipelineCachePath;

// Where the cache lives: what the proxy set, else RANDYVK_PIPELINE_CACHE ("off" = none).
std::string PipelineCacheFile()
{
    if (!g_pipelineCachePath.empty())
        return g_pipelineCachePath;
    char path[MAX_PATH] = {};
    if (GetEnvironmentVariableA("RANDYVK_PIPELINE_CACHE", path, sizeof(path)) == 0)
        return {};
    return _stricmp(path, "off") == 0 ? std::string() : std::string(path);
}

}  // namespace

void SetPipelineCachePath(const char* path) { g_pipelineCachePath = path ? path : ""; }

// The cache is a hint, never a requirement: no file, a file from another GPU or driver (the header says which), or any
// failure just means the pipelines are built from scratch, as before.
void Device::CreatePipelineCache()
{
    std::vector<uint8_t> data;
    const std::string file = PipelineCacheFile();
    if (!file.empty()) {
        if (FILE* f = std::fopen(file.c_str(), "rb")) {
            std::fseek(f, 0, SEEK_END);
            long size = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            if (size > 32 && size < (256l << 20)) {
                data.resize(size_t(size));
                if (std::fread(data.data(), 1, data.size(), f) != data.size())
                    data.clear();
            }
            std::fclose(f);
        }
        // The header (Vulkan spec): size, version 1, vendor, device, the driver's cache UUID. Anything else is not ours.
        uint32_t header[4] = {};
        if (data.size() >= 32)
            std::memcpy(header, data.data(), sizeof(header));
        if (!data.empty() && (header[0] < 32 || header[1] != VK_PIPELINE_CACHE_HEADER_VERSION_ONE ||
                              header[2] != m_props.vendorID || header[3] != m_props.deviceID ||
                              std::memcmp(data.data() + 16, m_props.pipelineCacheUUID, VK_UUID_SIZE) != 0)) {
            Log("pipeline cache %s is from another GPU or driver: starting afresh", file.c_str());
            data.clear();
        }
    }
    VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    ci.initialDataSize = data.size();
    ci.pInitialData = data.empty() ? nullptr : data.data();
    if (vkCreatePipelineCache(m_device, &ci, nullptr, &m_pipelineCache) != VK_SUCCESS) {
        m_pipelineCache = VK_NULL_HANDLE;        // (VK_NULL_HANDLE is also what the pipelines were made with before)
        return;
    }
    m_pipelineCacheSaved = data.size();
    if (!file.empty())
        Log("pipeline cache: %zu bytes loaded from %s", data.size(), file.c_str());
}

// Write the cache out if it holds more than the file does. Through a temporary file, so a crash can't leave half of one.
void Device::SavePipelineCache(const char* why)
{
    const std::string file = PipelineCacheFile();
    if (!m_pipelineCache || file.empty())
        return;
    size_t size = 0;
    if (vkGetPipelineCacheData(m_device, m_pipelineCache, &size, nullptr) != VK_SUCCESS || size == 0 || size == m_pipelineCacheSaved)
        return;
    std::vector<uint8_t> data(size);
    if (vkGetPipelineCacheData(m_device, m_pipelineCache, &size, data.data()) != VK_SUCCESS)
        return;
    data.resize(size);
    const std::string temp = file + "." + std::to_string(GetCurrentProcessId()) + ".tmp";   // (two clients in one folder)
    FILE* f = std::fopen(temp.c_str(), "wb");
    if (!f)
        return;
    const bool written = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    std::fclose(f);
    if (written && MoveFileExA(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        m_pipelineCacheSaved = data.size();
        Log("pipeline cache: %zu bytes saved (%s)", data.size(), why);
    } else {
        DeleteFileA(temp.c_str());
    }
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
                m_queueFamilyCount = qf[q].queueCount;
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
    // Dynamic vertex input and blending (extended dynamic state 3) where the driver has them, else emulated (dynemu.h:
    // macOS's MoltenVK and KosmicKrisp have neither) - below, with the features.
    std::vector<const char*> extensions = {VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
                                           VK_EXT_ROBUSTNESS_2_EXTENSION_NAME};   // nullDescriptor: empty bindless slots
    const bool eds3Ext = has(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
    const bool vertexInputExt = has(VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME);
    // The driver's own usage and budget per memory heap (the GPU memory report, diag.cpp GpuMemoryReport).
    m_memoryBudget = has(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    if (m_memoryBudget)
        extensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
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
    // Variable rate shading (RVK_Vrs, vrs.cpp): a shading rate image as an attachment, its tiles a size the GPU takes
    // (AMD's generator makes 8, 16 or 32).
    VkPhysicalDeviceFragmentShadingRateFeaturesKHR rate{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR};
    const bool rateExt = has(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME);
    // Ray queries (raytrace.cpp, docs/rt-plan.md): acceleration structures of the scene, rays from any shader.
    // RANDYVK_RAYTRACE=0 leaves them off.
    VkPhysicalDeviceRayQueryFeaturesKHR rayQuery{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR accel{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    VkPhysicalDeviceBufferDeviceAddressFeatures address{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
    const char* rtEnv = std::getenv("RANDYVK_RAYTRACE");
    const bool rtExt = !(rtEnv && *rtEnv == '0') && has(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) &&
                       has(VK_KHR_RAY_QUERY_EXTENSION_NAME) && has(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
    accel.pNext = &rayQuery;
    rayQuery.pNext = &address;

    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT vertexInput{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT};
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceDescriptorIndexingFeatures descIdx{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES};
    VkPhysicalDeviceRobustness2FeaturesEXT rob2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    VkPhysicalDeviceShaderFloat16Int8Features half{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES};
    // The driver's statistics of a compiled pipeline (registers, spills, instructions: ShaderRegisterReport).
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR exec{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR};
    const bool execExt = has(VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME);
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &half;
    half.pNext = execExt ? static_cast<void*>(&exec) : static_cast<void*>(&v13);
    exec.pNext = &v13;
    v13.pNext = &descIdx;
    descIdx.pNext = &rob2;
    rob2.pNext = &eds3;
    eds3.pNext = &vertexInput;
    vertexInput.pNext = &timeline;
    if (rateExt) timeline.pNext = &rate;
    if (rtExt) (rateExt ? rate.pNext : timeline.pNext) = &accel;
    vkGetPhysicalDeviceFeatures2(m_physical, &features);
    // Quad operations in fragment shaders (ffp_main.glsl: a 2x2 block of cut-out pixels stops before the lighting).
    VkPhysicalDeviceVulkan11Properties p11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES};
    VkPhysicalDeviceVulkan12Properties p12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    p11.pNext = &p12;
    VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    props2.pNext = &p11;
    vkGetPhysicalDeviceProperties2(m_physical, &props2);
    bool quad = (p11.subgroupSupportedStages & VK_SHADER_STAGE_FRAGMENT_BIT) &&
                (p11.subgroupSupportedOperations & VK_SUBGROUP_FEATURE_QUAD_BIT);
    if (!quad || !v13.dynamicRendering || !v13.synchronization2 || !v13.shaderDemoteToHelperInvocation ||
        !features.features.imageCubeArray) {
        if (error) *error = "required Vulkan features missing (dynamic rendering, sync2, demote to helper, cube arrays, "
                          "fragment quad operations)";
        return false;
    }
    // Dynamic vertex input and blend enable / equation: the driver's, or emulated by pipelines per state (dynemu.h).
    // RANDYVK_DYNAMIC_EMULATION=0 never emulates (the device fails without them, as before), =1 always does (a test of
    // the emulation on a GPU that has them).
    {
        const char* v = std::getenv("RANDYVK_DYNAMIC_EMULATION");
        const char mode = v && (*v == '0' || *v == '1') ? *v : 'a';
        const bool nativeVertexInput = vertexInputExt && vertexInput.vertexInputDynamicState;
        const bool nativeBlend = eds3Ext && eds3.extendedDynamicState3ColorBlendEnable &&
                                 eds3.extendedDynamicState3ColorBlendEquation;
        m_emulateVertexInput = mode == '1' || (mode == 'a' && !nativeVertexInput);
        m_emulateBlend = mode == '1' || (mode == 'a' && !nativeBlend);
        if (mode == '0' && (!nativeVertexInput || !nativeBlend)) {
            if (error) *error = "required Vulkan features missing (dynamic blend/vertex input; emulation off: "
                              "RANDYVK_DYNAMIC_EMULATION=0)";
            return false;
        }
        Log("dynamic vertex input: %s; dynamic blend enable and equation: %s",
            m_emulateVertexInput ? "emulated" : "the driver's", m_emulateBlend ? "emulated" : "the driver's");
    }
    // Bindless textures (M1): textures and samplers in descriptor arrays, indexed per draw.
    m_bindless = descIdx.shaderSampledImageArrayNonUniformIndexing && descIdx.descriptorBindingSampledImageUpdateAfterBind &&
                 descIdx.descriptorBindingPartiallyBound && rob2.nullDescriptor;
    // Its image slots (kMaxBindlessImages, every texture alive has one: a big zone's are over 4,096) within what the
    // GPU allows - the Vulkan minimum with these features is 500,000.
    const uint32_t imageLimit = std::min(p12.maxPerStageDescriptorUpdateAfterBindSampledImages,
                                         p12.maxDescriptorSetUpdateAfterBindSampledImages);
    if (m_bindless && imageLimit < kMaxBindlessImages) {
        Log("bindless textures: the GPU allows %u images, %u needed", imageLimit, kMaxBindlessImages);
        m_bindless = false;
    }
    Log("bindless textures: %s (%u image slots; the GPU allows %u)", m_bindless ? "yes" : "no", kMaxBindlessImages,
        imageLimit);
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
    enEds3.extendedDynamicState3ColorBlendEnable = m_emulateBlend ? VK_FALSE : VK_TRUE;
    enEds3.extendedDynamicState3ColorBlendEquation = m_emulateBlend ? VK_FALSE : VK_TRUE;
    // Write masks per draw (optional): attachments a draw leaves as they are aren't read and written back.
    m_dynamicWriteMask = eds3Ext && eds3.extendedDynamicState3ColorWriteMask == VK_TRUE;
    enEds3.extendedDynamicState3ColorWriteMask = m_dynamicWriteMask ? VK_TRUE : VK_FALSE;
    Log("dynamic colour write masks: %s", m_dynamicWriteMask ? "yes" : "no");
    // The two extensions only where something of theirs is the driver's.
    const bool useEds3 = !m_emulateBlend || m_dynamicWriteMask;
    const bool useVertexInput = !m_emulateVertexInput;
    if (useEds3) extensions.push_back(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
    if (useVertexInput) extensions.push_back(VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME);
    VkPhysicalDeviceDescriptorIndexingFeatures enDescIdx{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES};
    enDescIdx.shaderSampledImageArrayNonUniformIndexing = m_bindless ? VK_TRUE : VK_FALSE;
    enDescIdx.descriptorBindingSampledImageUpdateAfterBind = m_bindless ? VK_TRUE : VK_FALSE;
    enDescIdx.descriptorBindingPartiallyBound = m_bindless ? VK_TRUE : VK_FALSE;
    // AMD's effects' shaders (third_party/ffx-sdk): Brixelizer indexes its arrays of buffers per instance; the SDK picks
    // its 16-bit shaders where the GPU has half floats - both enabled where present.
    enDescIdx.shaderStorageBufferArrayNonUniformIndexing = descIdx.shaderStorageBufferArrayNonUniformIndexing;
    VkPhysicalDeviceRobustness2FeaturesEXT enRob2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
    enRob2.nullDescriptor = m_bindless ? VK_TRUE : VK_FALSE;
    enDescIdx.pNext = &enRob2;
    // Timeline semaphores: the frame interpolation swapchain's (RVK_FrameGen, framegen.cpp) - core in Vulkan 1.2.
    VkPhysicalDeviceTimelineSemaphoreFeatures enTimeline{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    enTimeline.timelineSemaphore = timeline.timelineSemaphore;
    m_timelineSemaphores = timeline.timelineSemaphore == VK_TRUE;
    {                                            // robustness 2 -> [extended dynamic state 3] -> [vertex input] -> timeline
        void** tail = &enRob2.pNext;
        if (useEds3) { *tail = &enEds3; tail = &enEds3.pNext; }
        if (useVertexInput) { *tail = &enVertexInput; tail = &enVertexInput.pNext; }
        *tail = &enTimeline;
    }
    VkPhysicalDeviceFragmentShadingRateFeaturesKHR enRate{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR};
    if (rateExt && rate.attachmentFragmentShadingRate && rate.pipelineFragmentShadingRate) {
        VkPhysicalDeviceFragmentShadingRatePropertiesKHR rp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &rp;
        vkGetPhysicalDeviceProperties2(m_physical, &p2);
        const VkExtent2D lo = rp.minFragmentShadingRateAttachmentTexelSize, hi = rp.maxFragmentShadingRateAttachmentTexelSize;
        m_vrsTexel = 0;
        for (uint32_t t : {16u, 8u, 32u})
            if (!m_vrsTexel && t >= lo.width && t <= hi.width && t >= lo.height && t <= hi.height) m_vrsTexel = t;
        if (m_vrsTexel) {
            enRate.pipelineFragmentShadingRate = VK_TRUE;
            enRate.attachmentFragmentShadingRate = VK_TRUE;
            enTimeline.pNext = &enRate;
            extensions.push_back(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME);
            m_sceneRateFlag = VK_PIPELINE_CREATE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR;
        }
    }
    Log("variable rate shading: %s", m_sceneRateFlag ? "yes" : "no (the GPU has no shading rate attachment of 8 .. 32)");
    VkPhysicalDeviceRayQueryFeaturesKHR enRayQuery{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR enAccel{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    VkPhysicalDeviceBufferDeviceAddressFeatures enAddress{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
    m_rayQuery = rtExt && accel.accelerationStructure && rayQuery.rayQuery && address.bufferDeviceAddress;
    if (m_rayQuery) {
        VkPhysicalDeviceAccelerationStructurePropertiesKHR ap{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &ap;
        vkGetPhysicalDeviceProperties2(m_physical, &p2);
        m_asScratchAlign = std::max<VkDeviceSize>(ap.minAccelerationStructureScratchOffsetAlignment, 16);
        enAccel.accelerationStructure = VK_TRUE;
        enRayQuery.rayQuery = VK_TRUE;
        enAddress.bufferDeviceAddress = VK_TRUE;
        enAccel.pNext = &enRayQuery;
        enRayQuery.pNext = &enAddress;
        enAddress.pNext = enTimeline.pNext;      // (after the shading rate's, if any)
        enTimeline.pNext = &enAccel;
        for (const char* e : {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_RAY_QUERY_EXTENSION_NAME,
                              VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME})
            extensions.push_back(e);
    }
    Log("ray queries: %s", m_rayQuery ? "yes"
                           : rtExt ? "no (the GPU lacks acceleration structures, ray queries or buffer addresses)"
                           : "no (no ray tracing extensions)");
    VkPhysicalDeviceShaderFloat16Int8Features enHalf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES};
    enHalf.shaderFloat16 = half.shaderFloat16;
    enHalf.pNext = &enDescIdx;
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR enExec{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR};
    // (Not with the emulation: its pipelines are templates, their statistics the variants'.)
    m_shaderStats = execExt && exec.pipelineExecutableInfo && !m_emulateVertexInput && !m_emulateBlend;
    if (m_shaderStats) {
        enExec.pipelineExecutableInfo = VK_TRUE;
        enExec.pNext = enHalf.pNext;
        enHalf.pNext = &enExec;
        extensions.push_back(VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME);
    }
    Log("shader statistics (registers per variant): %s", m_shaderStats ? "yes" : "no (the driver doesn't report them)");
    VkPhysicalDeviceVulkan13Features enV13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    enV13.pNext = &enHalf;
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
    // The HDR scene's attachments blend each their own way (draw.cpp ApplyDynamicState).
    enabled.features.independentBlend = features.features.independentBlend;
    // Gathers with offsets: AMD's ambient occlusion's shaders (RVK_AoMode 1, cacao.cpp).
    m_gatherExtended = features.features.shaderImageGatherExtended == VK_TRUE;
    enabled.features.shaderImageGatherExtended = features.features.shaderImageGatherExtended;
    enabled.features.tessellationShader = features.features.tessellationShader;   // characters' Phong tessellation
    m_tessSupported = features.features.tessellationShader;
    // KosmicKrisp (macOS, Vulkan on Metal): a character up close, tessellated, burst into spikes across the screen
    // (the character selection, 2026-11-04; docs/mac.md). Its tessellation stays off there - characters, ground and
    // water surface are drawn as without it - unless RANDYVK_TESS=1 (a test of a newer driver).
    {
        VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &driver;
        vkGetPhysicalDeviceProperties2(m_physical, &p2);
        const char* v = std::getenv("RANDYVK_TESS");
        if (m_tessSupported && driver.driverID == VK_DRIVER_ID_MESA_KOSMICKRISP && !(v && *v == '1')) {
            m_tessSupported = false;
            enabled.features.tessellationShader = VK_FALSE;
            Log("tessellation: off on KosmicKrisp (RANDYVK_TESS=1 turns it on)");
        }
    }
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
    // Only with RVK_SceneProf on (or RANDYVK_SCENE_PROFILE=1 from the start), as the scene's split by class
    // (ProfileSceneClass): every class change ended the pending indirect batch and began a query - ~350 a frame over a
    // city. The pools are made whenever the GPU can count, so the setting can switch it on in game.
    {
        const char* v = std::getenv("RANDYVK_SCENE_PROFILE");
        m_sceneProfile = v && *v == '1';
        const char* o = std::getenv("RANDYVK_OPAQUE_BLEND");
        m_opaqueBlend = !(o && *o == '0');
    }
    m_shadeQueries = features.features.pipelineStatisticsQuery;
    enabled.features.pipelineStatisticsQuery = features.features.pipelineStatisticsQuery;
    Log("draw grouping (indirect firstInstance + multiDrawIndirect): %s", m_groupIndirect ? "yes" : "no");

    // The engine's queue, and for frame generation (RVK_FrameGen, framegen.cpp) two more of the same family when it has
    // them: the frame interpolation swapchain's present queue - which the engine must not use - and its image
    // acquire queue.
    const float priorities[3] = {1.0f, 1.0f, 1.0f};
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = m_queueFamily;
    qci.queueCount = std::min(m_queueFamilyCount, 3u);
    qci.pQueuePriorities = priorities;
    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    ci.pNext = &enabled;
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qci;
    ci.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    ci.ppEnabledExtensionNames = extensions.data();
    if (!Check(vkCreateDevice(m_physical, &ci, nullptr, &m_device), "vkCreateDevice", error))
        return false;
    LoadDevice(m_device);
    InstallDynamicStateEmulation(m_device, m_emulateVertexInput, m_emulateBlend);   // (nothing, if neither)
    vkGetDeviceQueue(m_device, m_queueFamily, 0, &m_queue);
    if (qci.queueCount >= 2) vkGetDeviceQueue(m_device, m_queueFamily, 1, &m_fgPresentQueue);
    if (qci.queueCount >= 3) vkGetDeviceQueue(m_device, m_queueFamily, 2, &m_fgAcquireQueue);
    Log("queues: %u of family %u's %u (frame generation %s)", qci.queueCount, m_queueFamily, m_queueFamilyCount,
        m_fgPresentQueue ? "possible" : "not possible: one queue");

    VmaVulkanFunctions fns{};
    fns.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    fns.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo ai{};
    ai.vulkanApiVersion = VK_API_VERSION_1_3;
    ai.physicalDevice = m_physical;
    ai.device = m_device;
    ai.instance = m_instance;
    ai.pVulkanFunctions = &fns;
    if (m_memoryBudget)
        ai.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
    if (m_rayQuery)                              // (acceleration structures and their inputs by address)
        ai.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    return Check(vmaCreateAllocator(&ai, &m_allocator), "vmaCreateAllocator", error);
}

bool Device::CreateMainTargets(std::string* error)
{
    m_ldrMain = CreateImage(m_width, m_height, Format::A8R8G8B8, 1, true);
    if (!m_ldrMain || !CreateSceneTargets()) {
        if (error) *error = "main colour target";
        return false;
    }
    m_main = m_ldrMain;
    m_target = m_main;

    m_depthWidth = m_depthHeight = 0;
    // (At the scene's size: the post passes read it by the scene's coordinates. RVK_RenderScale: the output's own
    // depth is made by UpsampleDepth.)
    if (!EnsureDepth(m_scene->m_width, m_scene->m_height)) {
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

// The scene's targets: the float scene and its further attachments, at RVK_RenderScale's share of the output size.
bool Device::CreateSceneTargets()
{
    const uint32_t w = std::max(1u, (m_width * m_renderScale + 50) / 100);
    const uint32_t h = std::max(1u, (m_height * m_renderScale + 50) / 100);
    m_scene = CreateImage(w, h, Format::RGBA16F, 1, true);
    m_glow = CreateImage(w, h, Format::RGBA16F, 1, true);
    m_localFraction = CreateImage(w, h, Format::RGBA8, 1, true);
    m_motionVectors = CreateImage(w, h, Format::RGBA16F, 1, true);   // zw: PBR shading normal (ssr.frag)
    m_albedo = CreateImage(w, h, Format::A8R8G8B8, 1, true);
    return m_scene && m_glow && m_localFraction && m_motionVectors && m_albedo;
}

void Device::DestroySceneTargets()
{
    for (Texture** t : {&m_scene, &m_glow, &m_localFraction, &m_motionVectors, &m_albedo})
        if (*t) { DestroyTextureNow(*t); *t = nullptr; }
    if (m_depthSwapped) {
        SwapDepth();
        m_depthSwapped = false;
    }
    if (m_otherDepth.view) vkDestroyImageView(m_device, m_otherDepth.view, nullptr);
    if (m_otherDepth.image) vmaDestroyImage(m_allocator, m_otherDepth.image, m_otherDepth.allocation);
    m_otherDepth = DepthImage{};
    if (m_depthView) { vkDestroyImageView(m_device, m_depthView, nullptr); m_depthView = VK_NULL_HANDLE; }
    if (m_depth) { vmaDestroyImage(m_allocator, m_depth, m_depthAllocation); m_depth = VK_NULL_HANDLE; }
    m_depthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    m_depthWidth = m_depthHeight = 0;
}

// RVK_RenderScale changed (or the TAA it needs went on or off): between frames, the scene's targets and depth made
// again at the new size. Everything sized from the scene follows by itself; the TAA's history (output size) is
// started again.
void Device::ApplyRenderScale()
{
    const uint32_t want = RenderScaleNow();
    if (want == m_renderScale || !m_ldrMain || m_inFrame)
        return;
    WaitIdle();
    m_completed = m_submitted;
    const bool wasScene = m_target == m_scene, wasMain = m_main == m_scene;
    DestroySceneTargets();
    m_renderScale = want;
    if (!CreateSceneTargets() || !EnsureDepth(m_scene->m_width, m_scene->m_height)) {
        Log("render scale %u%%: the scene's targets could not be made", want);
        return;
    }
    if (wasScene) m_target = m_scene;
    if (wasMain) m_main = m_scene;
    m_taaFrame = ~0ull;
    m_frameLightsDirty = true;
    Log("render scale %u%%: the scene %ux%u of %ux%u", m_renderScale, m_scene->m_width, m_scene->m_height,
        m_width, m_height);
}

// The current depth image and the other one (m_otherDepth) trade places.
void Device::SwapDepth()
{
    std::swap(m_depth, m_otherDepth.image);
    std::swap(m_depthView, m_otherDepth.view);
    std::swap(m_depthAllocation, m_otherDepth.allocation);
    std::swap(m_depthLayout, m_otherDepth.layout);
    std::swap(m_depthWidth, m_otherDepth.width);
    std::swap(m_depthHeight, m_otherDepth.height);
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
    m_main = nullptr;
    for (auto& [tag, d] : m_deadImages) { vkDestroyImageView(m_device, d.view, nullptr); vmaDestroyImage(m_allocator, d.image, d.allocation); }
    m_deadImages.clear();
    DestroySceneTargets();
    if (m_readback) { vmaDestroyBuffer(m_allocator, m_readback, m_readbackAllocation); m_readback = VK_NULL_HANDLE; }
}

bool Device::Resize(uint32_t width, uint32_t height)
{
    if (m_inFrame || !width || !height)
        return false;
    WaitIdle();
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
    WaitIdle();
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
    // RVK_FrameGen: the frame interpolation swapchain (framegen.cpp); a plain one if it can't be made.
    bool fg = false;
    if (m_frameGenWanted && !m_frameGenFailed) {
        std::string why;
        fg = FgCreateSwapchain(ci, &why);
        if (!fg) {
            m_frameGenFailed = true;
            Log("frame generation: off - %s", why.empty() ? "no second queue for its presenting" : why.c_str());
        }
    }
    if (!fg && !Check(vkCreateSwapchainKHR(m_device, &ci, nullptr, &m_swapchain), "vkCreateSwapchainKHR", error))
        return false;
    if (fg) {
        FgGetSwapchainImages(&n, nullptr);
        m_swapImages.resize(n);
        FgGetSwapchainImages(&n, m_swapImages.data());
    } else {
        vkGetSwapchainImagesKHR(m_device, m_swapchain, &n, nullptr);
        m_swapImages.resize(n);
        vkGetSwapchainImagesKHR(m_device, m_swapchain, &n, m_swapImages.data());
    }
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
    if (m_fg)
        FgDestroySwapchain();                    // (frame generation's swapchain and context)
    else if (m_swapchain)
        vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
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
        // (Also the tessellation stages: the native ground's cliffs' rock pushed out of their faces, ffp.tese.)
        // (And compute with ray tracing: the rays test a cut-out's texture at their hits - raytrace.cpp.)
        const VkShaderStageFlags stages = (m_tessSupported ? vsfstess : vsfs) | (m_rayQuery ? VK_SHADER_STAGE_COMPUTE_BIT : 0);
        VkDescriptorSetLayoutBinding bb[2] = {
            {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kMaxBindlessImages, stages, nullptr},
            {1, VK_DESCRIPTOR_TYPE_SAMPLER, kMaxBindlessSamplers, stages, nullptr},
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
    // frags[g][set][nc]: the pixel shader by its build (g: 0 without the ground's code, 1 with it, 2 without the frame
    // lights' loop either), the target (8-bit, the HDR scene: glow) and whether it may discard (nc: no cut-out,
    // early-Z).
    VkShaderModule vert, frags[3][2][2];
    if (!module(kVertSpirv, sizeof(kVertSpirv), &vert) || !module(kFragSpirv, sizeof(kFragSpirv), &frags[1][0][0]) ||
        !module(kFragGlowSpirv, sizeof(kFragGlowSpirv), &frags[1][1][0]) ||
        !module(kFragNoCutSpirv, sizeof(kFragNoCutSpirv), &frags[1][0][1]) ||
        !module(kFragGlowNoCutSpirv, sizeof(kFragGlowNoCutSpirv), &frags[1][1][1]) ||
        !module(kFragNgSpirv, sizeof(kFragNgSpirv), &frags[0][0][0]) ||
        !module(kFragGlowNgSpirv, sizeof(kFragGlowNgSpirv), &frags[0][1][0]) ||
        !module(kFragNoCutNgSpirv, sizeof(kFragNoCutNgSpirv), &frags[0][0][1]) ||
        !module(kFragGlowNoCutNgSpirv, sizeof(kFragGlowNoCutNgSpirv), &frags[0][1][1]) ||
        !module(kFragNfSpirv, sizeof(kFragNfSpirv), &frags[2][0][0]) ||
        !module(kFragGlowNfSpirv, sizeof(kFragGlowNfSpirv), &frags[2][1][0]) ||
        !module(kFragNoCutNfSpirv, sizeof(kFragNoCutNfSpirv), &frags[2][0][1]) ||
        !module(kFragGlowNoCutNfSpirv, sizeof(kFragGlowNoCutNfSpirv), &frags[2][1][1]))
        return false;
    const VkShaderModule frag = frags[0][0][0], fragGlow = frags[0][1][0];   // (the leaves': never the ground)

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
    // RVK_Vrs: the scene shader shades at the shading rate image's rate (REPLACE) where the scene has one (vrs.cpp) -
    // its no-discard variants: a draw that cuts out (leaves, fences, grilles) shades every pixel (KEEP), or its holes
    // were cut 2 x 2 at a time - and two-pass foliage's core and edge layers, each cut at its own alpha, showed squares
    // of the tiles' size through dense leaves (user, 2026-10-21).
    VkPipelineFragmentShadingRateStateCreateInfoKHR rateState{
        VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR};
    rateState.fragmentSize = {1, 1};
    rateState.combinerOps[0] = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR;
    rateState.combinerOps[1] = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR;
    if (m_sceneRateFlag) rendering.pNext = &rateState;

    static const VkPrimitiveTopology kClassTopology[3] = {VK_PRIMITIVE_TOPOLOGY_POINT_LIST,
                                                          VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
                                                          VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    bool ok = true;
    // One set for the 8-bit targets, one for the HDR scene (float); each also as a no-discard variant (early-Z for
    // draws that never cut out); each without the ground's code (g 0), with it (g 1: the ground's draws), and without
    // the frame lights' loop (g 2: the draws none reaches).
    static const char* const kBuildName[3] = {"general", "ground", "no lamps"};
    for (int g = 0; g < 3 && ok; ++g)
    for (int set = 0; set < 2 && ok; ++set)
    for (int nc = 0; nc < 2 && ok; ++nc)
    for (int c = 0; c < 3 && ok; ++c) {
        colorFormats[0] = set ? GetFormatInfo(Format::RGBA16F).vk : kColorFormat;
        rendering.colorAttachmentCount = cb.attachmentCount = set ? 5 : 1;
        rateState.combinerOps[1] = nc ? VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR : VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR;
        stages[1].module = frags[g][set][nc];
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = kClassTopology[c];
        VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        ci.flags = m_sceneRateFlag;
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
        VkPipeline* const byBuild[3][2][2] = {
            {{&m_pipelines[c], &m_pipelinesNoCut[c]}, {&m_pipelinesHdr[c], &m_pipelinesHdrNoCut[c]}},
            {{&m_groundPipelines[c], &m_groundPipelinesNoCut[c]}, {&m_groundPipelinesHdr[c], &m_groundPipelinesHdrNoCut[c]}},
            {{&m_noFramePipelines[c], &m_noFramePipelinesNoCut[c]}, {&m_noFramePipelinesHdr[c], &m_noFramePipelinesHdrNoCut[c]}}};
        VkPipeline* dst = byBuild[g][set][nc];
        const bool stats = m_shaderStats && c == 2;   // (the points' and lines' builds share the triangles' shaders)
        if (stats) ci.flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
        ok = Check(vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &ci, nullptr, dst),
                   "vkCreateGraphicsPipelines", error);
        if (ok && stats)
            ShaderRegisterReport(*dst, kBuildName[g], set ? "HDR" : "8-bit", nc ? "no cut" : "cut-out");
    }
    // The interface's lean shader (RVK_LiteShader, m_drawLite): the 8-bit targets' set again with ffp_lite.frag.
    VkShaderModule lite[2] = {};
    ok = ok && module(kFragLiteSpirv, sizeof(kFragLiteSpirv), &lite[0]) &&
         module(kFragLiteNoCutSpirv, sizeof(kFragLiteNoCutSpirv), &lite[1]);
    colorFormats[0] = kColorFormat;
    rendering.colorAttachmentCount = cb.attachmentCount = 1;
    for (int nc = 0; nc < 2 && ok; ++nc)
    for (int c = 0; c < 3 && ok; ++c) {
        rateState.combinerOps[1] = nc ? VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR : VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR;
        stages[1].module = lite[nc];
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = kClassTopology[c];
        VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        ci.flags = m_sceneRateFlag;
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
        const bool stats = m_shaderStats && c == 2;
        if (stats) ci.flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
        VkPipeline* dst = nc ? &m_pipelinesLiteNoCut[c] : &m_pipelinesLite[c];
        ok = Check(vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &ci, nullptr, dst), "lean pipeline", error);
        if (ok && stats) ShaderRegisterReport(*dst, "interface (lean)", "8-bit", nc ? "no cut" : "cut-out");
    }
    for (VkShaderModule m : lite)
        if (m) vkDestroyShaderModule(m_device, m, nullptr);
    stages[1].module = frags[0][0][0];
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
        // (The characters take g 0 or 2, the native ground g 1.)
        for (int g = 0; g < 3 && ok; ++g)
        for (int set = 0; set < 2 && ok; ++set)
        for (int nc = 0; nc < 2 && ok; ++nc) {
            colorFormats[0] = set ? GetFormatInfo(Format::RGBA16F).vk : kColorFormat;
            rendering.colorAttachmentCount = cb.attachmentCount = set ? 5 : 1;
            rateState.combinerOps[1] = nc ? VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR : VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR;
            tstages[3].module = frags[g][set][nc];
            VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
            ia.topology = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
            VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
            ci.flags = m_sceneRateFlag;
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
            VkPipeline* const byBuild[3][2] = {{&m_tessPipelines[set], &m_tessPipelinesNoCut[set]},
                                               {&m_groundTessPipelines[set], &m_groundTessPipelinesNoCut[set]},
                                               {&m_noFrameTessPipelines[set], &m_noFrameTessPipelinesNoCut[set]}};
            VkPipeline* dst = byBuild[g][nc];
            if (m_shaderStats) ci.flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
            ok = Check(vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &ci, nullptr, dst),
                       "tessellation pipeline", error);
            if (ok && m_shaderStats)
                ShaderRegisterReport(*dst, kBuildName[g], set ? "HDR, tessellated" : "8-bit, tessellated",
                                     nc ? "no cut" : "cut-out");
        }
    }
    if (tesc) vkDestroyShaderModule(m_device, tesc, nullptr);
    if (tese) vkDestroyShaderModule(m_device, tese, nullptr);
    // A canopy's leaves (leaves.cpp): triangles through leaf.vert, cut out (both targets). Not fatal. Every pixel shaded
    // (RVK_Vrs), as the other cut-out draws: their sprigs showed squares as the camera turned (user, 2026-10-21).
    rateState.combinerOps[1] = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR;
    VkShaderModule leafVert = VK_NULL_HANDLE;
    if (ok && module(kLeafVertSpirv, sizeof(kLeafVertSpirv), &leafVert)) {
        VkPipelineShaderStageCreateInfo lstages[2] = {stages[0], stages[1]};
        lstages[0].module = leafVert;
        for (int set = 0; set < 2; ++set) {
            colorFormats[0] = set ? GetFormatInfo(Format::RGBA16F).vk : kColorFormat;
            rendering.colorAttachmentCount = cb.attachmentCount = set ? 5 : 1;
            lstages[1].module = set ? fragGlow : frag;
            VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
            ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
            ci.flags = m_sceneRateFlag;
            ci.pNext = &rendering;
            ci.stageCount = 2;
            ci.pStages = lstages;
            ci.pInputAssemblyState = &ia;
            ci.pViewportState = &vp;
            ci.pRasterizationState = &rs;
            ci.pMultisampleState = &ms;
            ci.pDepthStencilState = &dss;
            ci.pColorBlendState = &cb;
            ci.pDynamicState = &ds;
            ci.layout = m_pipelineLayout;
            std::string leafError;
            if (!Check(vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &ci, nullptr, &m_leafPipelines[set]),
                       "leaf pipeline", &leafError))
                m_leafPipelines[set] = VK_NULL_HANDLE;
        }
        if (!m_leafPipelines[0] || !m_leafPipelines[1])
            for (VkPipeline& p : m_leafPipelines) {
                if (p) vkDestroyPipeline(m_device, p, nullptr);
                p = VK_NULL_HANDLE;
            }
    }
    if (ok)
        CreatePrepassPipeline(vert, m_leafPipelines[0] ? leafVert : VK_NULL_HANDLE);   // not fatal: without it the
                                                                                      // scene draws as before
    if (leafVert)
        vkDestroyShaderModule(m_device, leafVert, nullptr);
    vkDestroyShaderModule(m_device, vert, nullptr);
    for (auto& byGround : frags)
        for (auto& bySet : byGround)
            for (VkShaderModule m : bySet) vkDestroyShaderModule(m_device, m, nullptr);
    if (!ok || !CreateShadowResources(error) || !CreatePointShadowResources(error) || !CreateHdrResources(error) ||
        !CreateDarknessResources(error) ||
        !CreateParticleResources(error) || !CreateFxResources(error) || !CreateSkinResources(error))
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
    {
        std::string rainError;                   // not fatal: no rain then (the game's stays)
        if (!CreateRainResources(&rainError)) {
            Log("rain: unavailable (%s)", rainError.c_str());
            DestroyRainResources();
        }
        std::string smokeError;                  // nor this: no smoke then
        if (!CreateSmokeResources(&smokeError)) {
            Log("smoke: unavailable (%s)", smokeError.c_str());
            DestroySmokeResources();
        }
        std::string cloudError;                  // nor this: the game's clouds then
        if (!CreateCloudResources(&cloudError)) {
            Log("clouds: unavailable (%s)", cloudError.c_str());
            DestroyCloudResources();
        }
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
        ai.commandBufferCount = 8;
        VkCommandBuffer cmds[8];
        if (!Check(vkAllocateCommandBuffers(m_device, &ai, cmds), "vkAllocateCommandBuffers", error))
            return false;
        f.upload = cmds[0];
        f.main = f.mainA = cmds[1];
        f.prepass = cmds[2];                     // the depth pre-pass (prepass.cpp)
        f.mainB = cmds[3];
        f.clouds = cmds[4];                      // the clouds' own pass (clouds.cpp)
        f.rays = cmds[5];                        // the ray-traced sun shadows (raytrace.cpp)
        f.prepass2 = cmds[6];                    // a second pre-pass segment (prepass.cpp PrepassContinue)
        f.mainC = cmds[7];
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
    if (size > f.ringSize) {
        // Bigger than the whole ring: written aside, so nothing past the ring's mapping is overwritten; the GPU reads
        // stale data for it this frame, and the next frames get a ring that holds it.
        if (m_ringTooSmallLogged++ < 20)
            Log("per-frame ring buffer too small: %llu bytes wanted, ring %llu MB - left out this frame\n",
                (unsigned long long)size, (unsigned long long)(f.ringSize >> 20));
        WantRing(size + size / 4);
        if (m_ringSpill.size() < size) m_ringSpill.resize(size_t(size));
        *cpu = m_ringSpill.data();
        return 0;
    }
    if (offset + size > f.ringSize) {
        Log("per-frame ring buffer full (%llu bytes)\n", (unsigned long long)f.ringSize);
        offset = 0;     // overwrites this frame's data; visible corruption rather than a crash
        ++m_ringGeneration;
        WantRing(f.ringSize * 2);
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
    if (m_rt)
        RtCollect();
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
        // Never fits this frame. It used to stay so: the ring grew only after a mid-frame flush, and a zone whose
        // frames wanted 71 MB of a 64 MB ring overflowed it every frame, writing past its end, until the game ended.
        if (m_ringTooSmallLogged++ < 20)
            Log("%llu bytes do not fit in the %llu-byte ring buffer - a bigger one for the next frames\n",
                (unsigned long long)bytes, (unsigned long long)f.ringSize);
        WantRing(needed + needed / 4);
        return;
    }
    if (m_inFrame) {
        // A flush waits for the GPU, and the frame's shadow casters recorded before it (their data overwritten)
        // drop out of this frame's shadows: the next frames get a bigger ring (BeginFrame).
        ++m_midFrameFlushes;
        m_ringPeak = std::max(m_ringPeak, f.ringOffset);
        WantRing(f.ringSize * 2);
        if (m_ringFlushesLogged++ < 20)
            Log("ring buffer full mid-frame (%llu bytes wanted, ring %llu MB): flushing\n", (unsigned long long)bytes,
                (unsigned long long)(f.ringSize >> 20));
        bool wasRendering = m_rendering;
        SubmitAndWait();                                     // also submits pending uploads
        if (wasRendering)
            BeginRenderingOn(m_target);
    } else if (f.uploadsRecorded) {
        SubmitUploadsBetweenFrames(f);
    } else {
        WaitFrame(f, "GPU frame work");
    }
    f.ringOffset = 0;
    ++m_ringGeneration;
}

// Between frames: the uploads recorded so far run now, and are waited for.
void Device::SubmitUploadsBetweenFrames(Frame& f)
{
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
}

void Device::WantRing(VkDeviceSize bytes)
{
    VkDeviceSize size = std::max(m_ringWanted, kRingSize);
    while (size < bytes && size < kRingMaxSize) size *= 2;
    size = std::min(size, kRingMaxSize);
    if (m_ringRefused && size >= m_ringRefused) size = std::max(m_ringWanted, m_ringRefused / 2);
    m_ringWanted = std::max(m_ringWanted, size);
    m_ringQuietWindows = 0;
}

// ---------------------------------------------------------------------------------------------------
// GPU-driven M2: a frame's draw arenas, slices of its ring

VkDeviceSize Device::ArenaBytes(uint32_t consts, uint32_t records) const
{
    VkDeviceSize shadowBytes = (m_shadows || m_pointShadows) ? VkDeviceSize(kShadowRecordCapacity) *
                                               (sizeof(ShadowRecord) + sizeof(VkDrawIndexedIndirectCommand))
                                         : 0;
    return VkDeviceSize(consts) * sizeof(DrawConstants) + VkDeviceSize(records) * sizeof(DrawRecord) +
           2 * VkDeviceSize(records) * sizeof(VkDrawIndexedIndirectCommand) + shadowBytes + 64;
}

void Device::FitArenas(uint32_t* consts, uint32_t* records)
{
    uint32_t c = m_constWanted, r = m_recordWanted;
    const VkDeviceSize limit = m_frames[m_frameIndex].ringSize / 2;
    if (ArenaBytes(c, r) > limit)
        WantRing(2 * ArenaBytes(c, r));
    // Until then the larger array gives way (each stays at least its starting size; a frame that fills it flushes).
    while (ArenaBytes(c, r) > limit && (c > kDrawConstCapacity || r > kDrawRecordCapacity)) {
        const bool recordsLarger = VkDeviceSize(r) * (sizeof(DrawRecord) + 2 * sizeof(VkDrawIndexedIndirectCommand)) >=
                                   VkDeviceSize(c) * sizeof(DrawConstants);
        if ((recordsLarger && r > kDrawRecordCapacity) || c <= kDrawConstCapacity)
            r /= 2;
        else
            c /= 2;
    }
    *consts = c;
    *records = r;
}

// Reserve this frame's deduplicated-constants and draw-record arrays in the ring (the space was ensured by the
// caller). Called when the ring was (re)started.
void Device::PrepareDrawArenas()
{
    if (m_arenaGeneration == m_ringGeneration)
        return;
    FitArenas(&m_constCapacity, &m_recordCapacity);
    VkDeviceSize align = std::max<VkDeviceSize>(16, m_props.limits.minStorageBufferOffsetAlignment);
    VkDeviceSize bytes = ArenaBytes(m_constCapacity, m_recordCapacity);
    EnsureRingSpace(bytes);          // the caller reserved for it, but a flush may have restarted the ring since
    void* cpu;
    m_constsBase = Allocate(VkDeviceSize(m_constCapacity) * sizeof(DrawConstants), align, &cpu);
    m_recordsBase = Allocate(VkDeviceSize(m_recordCapacity) * sizeof(DrawRecord), align, &cpu);
    m_indirectBase = Allocate(VkDeviceSize(m_recordCapacity) * sizeof(VkDrawIndexedIndirectCommand), 4, &cpu);
    m_prepassIndirectBase = Allocate(VkDeviceSize(m_recordCapacity) * sizeof(VkDrawIndexedIndirectCommand), 4, &cpu);
    m_prepassIndirectCount = 0;
    m_pre.groupCount = 0;                        // (flushed with the pre-pass before the ring restarted)
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
    // Straight into the ring (a record built and copied was two passes over ~250 bytes a draw).
    uint8_t* dst = m_frames[m_frameIndex].ringData + m_recordsBase + VkDeviceSize(index) * sizeof(DrawRecord);
    const uint32_t head[4] = {constIndex, m_recordExtra[0], m_recordExtra[1], m_recordExtra[2]};
    std::memcpy(dst, head, sizeof(head));
    std::memcpy(dst + offsetof(DrawRecord, d), &d, sizeof(d));
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
    if (target == m_scene && !m_glowCleared)
        RenderVrsImage(cmd);                     // (RVK_Vrs: the scene's first rendering this frame)
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
    // RVK_Vrs: the shading rate image (made before the scene's first rendering of the frame, above).
    VkRenderingFragmentShadingRateAttachmentInfoKHR rate{VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR};
    if (target == m_scene && (rate.imageView = VrsAttachment())) {
        rate.imageLayout = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
        rate.shadingRateAttachmentTexelSize = {m_vrsTexel, m_vrsTexel};
        ri.pNext = &rate;
    }
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
    else if (m_fgTopActive && target == m_main && m_fgTop)
        target = m_fgTop;                        // (frame generation: the overlay's top, to the frame's end)
    else if (m_fgUnderActive && target == m_main && m_fgLabels)
        target = m_fgLabels;                     // (... and under the interface, to its start: FgUnderBegin)
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
    PortalCaptureFrame();                        // (shop portals: textures read back, a room's capture saved; outside a frame)
    HeapCheck("the frame's start", 1);
    if (!m_heapBroken && HeapCheckLevel() > 0) m_heapLastOk = "the frame's start";
    // Indoors (a room's playfield: rooms drawn, no terrain) for 10 frames running, or out again: no weather in there
    // (a forced storm went on raining and striking inside - the user). The weather goes at once, not eased.
    {
        const bool inside = m_frameRoomVisuals > 0 && m_frameTerrainVisuals == 0;
        m_indoorRun = inside == m_indoors ? 0 : m_indoorRun + 1;
        if (m_indoorRun >= 10) {
            m_indoors = inside;
            m_indoorRun = 0;
            Log("weather: %s (rooms %u, terrain %u drawn)", inside ? "indoors - none" : "outdoors", m_frameRoomVisuals,
                m_frameTerrainVisuals);
            if (inside) m_rain = m_stormSky = m_sand = m_storm = 0.0f;
        }
        m_frameRoomVisuals = m_frameTerrainVisuals = 0;
    }
    m_fgUiLayer = m_fg != nullptr;               // (RVK_FrameGen: the interface's layer to the swapchain, framegen.cpp)
    m_pickActive = m_pickArmed;                  // texture picking (RequestPick): this frame's draws
    m_pickArmed = false;
    m_pickBest = nullptr;
    m_pickBestTerrain = false;
    m_pickBestVisual = m_pickBestOwner = m_pickBestKind = m_pickBestVertices = m_pickBestIndices = 0;
    m_pickBestTriangleCount = 0;
    m_pickDepth = 2.0f;
    m_batchRuns = m_batchMerged = m_batchMaxRun = 0;
    m_batchKey = 0;
    m_batchRun = 0;
    m_meshStaticDraws = m_meshHashedDraws = 0;
    m_bindlessBound = false;                     // the frame's command buffer starts with set 1 unbound
    m_arenaBound = false;                        // ... and with the draw arenas unbound
    m_shadowArenaBound = false;                  // ... and the shadow record array unbound
    if (m_swapchain && m_frameGenWanted != (m_fg != nullptr) && !(m_frameGenWanted && m_frameGenFailed))
        m_swapchainStale = true;                 // (RVK_FrameGen switched: the swapchain made again)
    if (m_swapchainStale) {
        WaitIdle();
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
    ApplyRenderScale();
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
            else {
                Log("ring buffer: no memory for %llu MB, stays %llu MB", (unsigned long long)(m_ringWanted >> 20),
                    (unsigned long long)(old >> 20));
                if (m_ringWanted > old) m_ringRefused = m_ringWanted;   // not asked for again every frame
                m_ringWanted = old;              // no memory for more: stay
            }
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
        const AddressSpace space = QueryAddressSpace();
        Log("address space: %u of %u MB free (the game's process), largest free block %u MB", unsigned(space.free >> 20),
            unsigned(space.total >> 20), unsigned(space.largest >> 20));
        Log("%s", GpuMemoryReport().c_str());
        Log("opaque static fast path: %llu draws; %llu alpha-1 blended draws drawn unblended (last 600 frames)",
            (unsigned long long)m_opaqueDraws, (unsigned long long)m_opaqueBlendDraws);
        m_opaqueBlendDraws = 0;
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
                "edge; %llu tiles waiting to build; %.0f blades a frame cast the sun's shadow, %.0f the point lights' (last 600 frames)",
                (unsigned long long)m_grassDraws, double(m_grassBlades) / std::max<uint64_t>(m_grassDraws, 1),
                (unsigned long long)m_grassTiles.size(), (unsigned long long)m_grassBuilds,
                m_grassBuildMs / double(std::max<uint64_t>(m_grassBuilds, 1)),
                100.0 * double(m_grassLitBlades) / double(std::max<uint64_t>(m_grassBuiltBlades, 1)),
                m_grassCaptureMs / 600.0, m_terrainAmbient[0], m_terrainAmbient[1], m_terrainAmbient[2],
                (unsigned long long)m_framePushers.size(), m_pusherSeenMax, m_trailActive, (unsigned long long)m_grassLeftOut[0],
                (unsigned long long)m_grassLeftOut[1], (unsigned long long)m_grassLeftOut[2],
                (unsigned long long)m_grassLeftOut[3], (unsigned long long)GrassTilesWaiting(),
                double(m_grassShadowBlades) / 600.0, double(m_grassPointShadowBlades) / 600.0);
        if (m_leaf.on)
            Log("leaves: canopy draws a frame %.1f shrubs, %.1f trees, %.1f palms; %.1f leaf draws a frame (%.1f in the "
                "depth pre-pass), %.0f leaves a frame; %zu sets, %u baked (%.2f ms each, %.0f leaves each); %.0f leaves a "
                "frame cast the sun's shadow (last 600 frames)",
                double(m_canopyCount[1]) / 600.0, double(m_canopyCount[2]) / 600.0, double(m_canopyCount[3]) / 600.0,
                double(m_leafDraws) / 600.0, double(m_leafPrepassed) / 600.0, double(m_leafDrawn) / 600.0,
                m_leafSets.size(), m_leafBakes,
                m_leafBakeMs / double(std::max<uint32_t>(m_leafBakes, 1)),
                double(m_leafBakedLeaves) / double(std::max<uint32_t>(m_leafBakes, 1)),
                double(m_leafShadowLeaves) / 600.0);
        for (uint64_t& n : m_canopyCount)
            n = 0;
        m_leafDraws = m_leafDrawn = m_leafShadowLeaves = m_leafPrepassed = 0;
        m_leafBakes = m_leafBakedLeaves = 0;
        m_leafBakeMs = 0.0;
        m_grassShadowBlades = 0;
        m_grassPointShadowBlades = 0;
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
    LiveSmooth(m_liveGpuWaitMs, waitMs);
    MakeCloudNoise(f.main);                      // (once: before any rendering opens)
    if (!m_cloudPuffs.empty()) {
        m_cloudPuffsLast.swap(m_cloudPuffs);
        m_cloudPuffsLastFrame = m_frameNumber - 1;
    }
    m_cloudPuffs.clear();
    m_cloudsDrawn = false;
    UpdatePushTrail();
    UpdateSandPrints(f.main);                    // (the footprints left since: sand_prints.cpp)
    UpdateLeaves();
    PruneObjects();                              // (the per-object cache: objects.cpp)
    m_tessCharsPrev.swap(m_tessChars);           // characters drawn last frame (their rigid parts: TessellateDraw)
    m_tessChars.clear();
    if (m_frameNumber == 1200)
        SavePipelineCache("after the first frames");   // (the pipelines made on demand since the start)
    TickFixedClock();                            // (tests: SetFixedClock)
    CollectWaterBodies();                        // the water's ripples follow the characters of the last frame
    m_frameClock = SwayClock();
    UpdateWind();                                // (m_windTime, m_windTimePrev: wind.cpp)
    UpdateLightning();                           // (rain.cpp: the game's glow last frame)
    m_lightsPrev.swap(m_lightsCur);              // last frame's complete light set lights this frame
    m_lightsCur.clear();
    m_sunLuminance = 0.0f;
    m_casters.clear();
    m_blobShadowsReplaced.store(m_shadows && m_shadowValid, std::memory_order_relaxed);
    m_edges.clear();                             // (two-pass foliage: flushed with every rendering's end already)
    m_profileDraws += m_frameDraw;               // last frame's batching (the profile's log)
    m_frameDraw = 0;
    m_prepassEndedThisFrame = false;
    m_waterEndedPrepass = false;
    m_grassDrawnThisFrame = false;
    m_profileGroupCalls += m_groupCalls;
    m_profileGroupDraws += m_groupDraws;
    m_profileConstBlocks += m_constCount;
    m_profileConstReused += m_constantsReused;
    m_groupCalls = m_groupDraws = m_singleDraws = m_constantsReused = 0;
    std::memset(m_rewriteWhy, 0, sizeof(m_rewriteWhy));
    std::memset(m_splitWhy, 0, sizeof(m_splitWhy));
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
    SimulateFx(f.main);                          // our own effects (fx.cpp): last frame's announcements
    ProfileMark("particle simulation");
    m_inFrame = true;
    BeginScene();
    PrepareShadowMap(f.main);
    PreparePointShadowMaps(f.main);
    BeginRenderingOn(m_target);
    if (!m_dumpPath.empty())
        BeginFrameDump();
    // (The cliffs' rock uploaded once decoded: inside the frame - its several MB of uploads may need the ring flushed,
    // which waits on frame work only an open frame submits.)
    PollTerrainRock();
}

void Device::EndFrame()
{
    if (m_pickActive) {                          // texture picking: the nearest draw's texture, for LastPick
        m_pickActive = false;
        // The ground drawn natively: its draws' stage 0 is a stand-in or the lightmap - the cell's own tile instead.
        m_pickTile = 0;
        if (m_pickBestTerrain && m_native.on && m_native.ground) {
            const NativeGround& g = *m_native.ground;
            const int cx = int(std::floor(m_pickBestWorld[0] / g.cellX)), cz = int(std::floor(m_pickBestWorld[2] / g.cellZ));
            if (cx >= 0 && cz >= 0 && uint32_t(cx) < g.cellsX && uint32_t(cz) < g.cellsZ) {
                const uint32_t tile = g.codes[size_t(cz) * g.cellsX + uint32_t(cx)] & 0x3FFFu;
                if (tile < g.tiles.size() && g.tiles[tile]) {
                    m_pickBest = g.tiles[tile];
                    m_pickTile = tile + 1;
                }
                Log("pick: the native ground's cell %d, %d (world %.1f %.1f): tile %u", cx, cz,
                    double(m_pickBestWorld[0]), double(m_pickBestWorld[2]), tile);
            }
        }
        m_pickPublished.store(m_pickBest, std::memory_order_relaxed);
        m_pickPublishedVisual.store(m_pickBestVisual, std::memory_order_relaxed);
        m_pickPublishedOwner.store(m_pickBestOwner, std::memory_order_relaxed);
        m_pickPublishedKind.store(m_pickBestKind, std::memory_order_relaxed);
        m_pickPublishedVertices.store(m_pickBestVertices, std::memory_order_relaxed);
        m_pickPublishedIndices.store(m_pickBestIndices, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(m_pickTriangleMutex);
            m_pickPublishedTriangleCount = m_pickBestTriangleCount;
            std::memcpy(m_pickPublishedTriangles, m_pickBestTriangles, sizeof(m_pickPublishedTriangles));
        }
        m_pickSerial.fetch_add(1, std::memory_order_release);
    }
    if (m_rendering && !m_particleOrphansDone) {
        m_particleOrphanTrigger = "end of the frame";   // no interface draw after the 3D
        DrawOrphanParticles();
    }
    EndScene();                                  // if the interface didn't end it (no interface drawn)
    m_framesWithout3D = m_sceneSaw3D ? 0u : std::min(m_framesWithout3D + 1u, 1000u);   // (RenderScaleNow)
    m_framesWithout3DShared.store(m_framesWithout3D, std::memory_order_relaxed);
    PortalDiagFrame();                           // (shop portals' phase 0: portal_diag.cpp)
    PortalStreetDiagFrame();
    FgUnderEnd();                                // (no interface drawn after it)
    Frame& f = m_frames[m_frameIndex];
    m_renderEndCause = kEndScene;
    EndRendering();
    if (m_fgTopActive) {                         // (frame generation's top layer: done, for the overlay)
        m_fgTopActive = false;
        m_fgTopFrame = m_frameNumber;
        m_target = m_main;
    }
    ProfileMark("interface");
    double cpu = ProfileCpu();
    UpdateEnvironment(f.main);                   // (the time of day and the weather's light: sky.cpp)
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
    static const bool consoleInShots = std::getenv("RANDYVK_CONSOLE_SHOT") != nullptr;   // (tests: the console in it)
    if (consoleInShots) DrawConsole(f.main);
    if (screenshot) {
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {m_width, m_height, 1};
        Texture* shot = FgComposedFrame(f.main);   // (RVK_FrameGen: the labels and interface aren't in the frame)
        vkCmdCopyImageToBuffer(f.main, (shot ? shot : m_main)->m_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_readback, 1,
                               &region);
    }
    if (!consoleInShots) DrawConsole(f.main);    // (after the screenshot's copy: not in it)

    uint32_t imageIndex = 0;
    bool present = false;
    double presentStart = ProfileCpu();              // acquire + submit + present: driver time (profile)
    if (m_swapchain) {
        // Bounded wait: if no image comes within 250 ms, skip presenting this frame instead of freezing.
        VkResult r = m_fg ? FgAcquire(250000000ull, f.imageAvailable, &imageIndex)
                          : vkAcquireNextImageKHR(m_device, m_swapchain, 250000000ull, f.imageAvailable, VK_NULL_HANDLE, &imageIndex);
        present = r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR;
        if (r == VK_TIMEOUT || r == VK_NOT_READY) {
            static int timeouts;
            if (timeouts++ < 20)
                Log("no swapchain image within 250 ms, frame not presented\n");
        }
        if (r == VK_SUBOPTIMAL_KHR || r == VK_ERROR_OUT_OF_DATE_KHR)
            m_swapchainStale = true;
    }
    // (Its parts too: a session that slowed to 15 fps after an hour had all the time here, with frame generation -
    // which of the three grew tells the swapchain's waits from the queue's (#20).)
    ProfileCpuAdd("  of which acquire", presentStart);
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
        // (The frame interpolation swapchain's images are read by its passes - optical flow, interpolation, the
        // interface laid over - and it expects them as shader-read textures, not to present.)
        ImageBarrier(f.main, dst, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     m_fg ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0);
    }
    if (m_fg && present)
        FgBeforePresent(f.main);                 // (frame generation: whether to generate, the interface's layer)
    vkEndCommandBuffer(f.main);

    VkCommandBuffer cmds[8];
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
    const double submitStart = ProfileCpu();
    vkQueueSubmit(m_queue, 1, &si, f.fence);
    ProfileCpuAdd("  of which submit", submitStart);
    f.uploadsRecorded = false;
    FrameCommandsSubmitted(f);
    if (present) {
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &m_renderDone[imageIndex];
        pi.swapchainCount = 1;
        pi.pSwapchains = &m_swapchain;
        pi.pImageIndices = &imageIndex;
        const double queueStart = ProfileCpu();
        VkResult r = m_fg ? FgPresent(&pi) : vkQueuePresentKHR(m_queue, &pi);
        ProfileCpuAdd("  of which present", queueStart);
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
    VkCommandBuffer cmds[8];
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
        Texture* src = t;
        if (t == m_main) {
            // (RVK_FrameGen: the labels and the interface laid over, as the player sees it - the game's screenshots.)
            if (Texture* composed = FgComposedFrame(cmd)) {
                src = composed;
                m_cache = StateCache{};          // (its passes changed the pipeline and pushed set 0: InterfaceEnd)
                m_arenaBound = false;
                m_bindlessBound = false;
                m_constantsDirty = true;
            }
        }
        Transition(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        vkCmdCopyImageToBuffer(cmd, src->m_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
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

bool Device::ReadTextureLevel(Texture* t, uint32_t level, void* out, size_t bytes)
{
    if (!t || !t->m_image || t->m_renderTarget || level >= t->m_levels || t->m_layout == VK_IMAGE_LAYOUT_UNDEFINED)
        return false;
    const uint32_t w = std::max(1u, t->m_width >> level), h = std::max(1u, t->m_height >> level);
    const VkDeviceSize size = VkDeviceSize(FormatRowBytes(t->m_format, w)) * FormatRows(t->m_format, h);
    if (size != bytes)
        return false;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo ac{};
    ac.usage = VMA_MEMORY_USAGE_AUTO;
    ac.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VkBuffer buffer;
    VmaAllocation allocation;
    VmaAllocationInfo info;
    if (vmaCreateBuffer(m_allocator, &bi, &ac, &buffer, &allocation, &info) != VK_SUCCESS)
        return false;
    // In the upload commands, after the uploads recorded so far (the level's last one among them), then run.
    VkCommandBuffer cmd = UploadCommands();
    Transition(cmd, t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
    region.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(cmd, t->m_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
    Transition(cmd, t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (m_inFrame) {
        const bool wasRendering = m_rendering;
        SubmitAndWait();                         // (the uploads first)
        if (wasRendering)
            BeginRenderingOn(m_target);
    } else {
        SubmitUploadsBetweenFrames(m_frames[m_frameIndex]);
    }
    std::memcpy(out, info.pMappedData, size_t(size));
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
