// Profiling (diagnostics): GPU timestamps around the frame's passes and CPU times of the expensive parts recorded at
// its end, averaged and logged every kProfileFrames frames - "gpu ms: scene 4.10 | bloom 0.21 | ...". Each GPU
// interval is named by the mark that ends it. Results are read when the frame slot comes round again (its fence
// waited), so they never stall.
#include "internal.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace rvk {

using namespace vk;
using namespace detail;

namespace {
constexpr uint32_t kProfileFrames = 600;
double CpuNow()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}
}  // namespace

bool Device::CreateProfiler(std::string* error)
{
    uint32_t n = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(m_physical, &n, nullptr);
    std::vector<VkQueueFamilyProperties> families(n);
    vkGetPhysicalDeviceQueueFamilyProperties(m_physical, &n, families.data());
    if (m_queueFamily >= n || families[m_queueFamily].timestampValidBits == 0) {
        Log("profiling: the queue has no timestamps");
        return true;
    }
    m_timestampPeriod = m_props.limits.timestampPeriod;
    // What one clock read costs (under Wine a QueryPerformanceCounter): the per-draw sections subtract it.
    double start = CpuNow();
    for (int i = 0; i < 2000; ++i) (void)CpuNow();
    m_timerCost = (CpuNow() - start) / 2001.0;
    Log("profiling: a clock read costs %.0f ns", m_timerCost * 1e6);
    VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qi.queryCount = kProfileMarks;
    for (ProfileFrame& p : m_profile)
        if (!Check(vkCreateQueryPool(m_device, &qi, nullptr, &p.pool), "timestamp query pool", error))
            return false;
    return true;
}

void Device::DestroyProfiler()
{
    for (ProfileFrame& p : m_profile)
        if (p.pool) { vkDestroyQueryPool(m_device, p.pool, nullptr); p.pool = VK_NULL_HANDLE; }
}

// Frame start (this slot's fence waited, its command buffer begun): take in its last results, start anew.
void Device::ProfileBeginFrame(VkCommandBuffer cmd)
{
    ProfileFrame& p = m_profile[m_frameIndex];
    if (!p.pool)
        return;
    if (p.count > 1) {
        uint64_t t[kProfileMarks];
        if (vkGetQueryPoolResults(m_device, p.pool, 0, p.count, sizeof(t), t, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) ==
            VK_SUCCESS) {
            for (uint32_t i = 1; i < p.count; ++i)
                ProfileAdd(m_profileGpu, p.names[i], double(t[i] - t[i - 1]) * m_timestampPeriod * 1e-6);
            ProfileAdd(m_profileGpu, "total", double(t[p.count - 1] - t[0]) * m_timestampPeriod * 1e-6);
            ++m_profileFrames;
        }
    }
    p.count = 0;
    vkCmdResetQueryPool(cmd, p.pool, 0, kProfileMarks);
    m_profileCpuStart = CpuNow();
    ProfileMark("start");
    if (m_profileFrames >= kProfileFrames && !m_profileManual)
        ProfileLog("");
}

// A GPU timestamp after everything recorded so far; the interval up to it is named `name` (a string literal).
void Device::ProfileMark(const char* name)
{
    ProfileFrame& p = m_profile[m_frameIndex];
    if (!p.pool || p.count >= kProfileMarks)
        return;
    vkCmdWriteTimestamp2(m_frames[m_frameIndex].main, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, p.pool, p.count);
    p.names[p.count++] = name;
}

// The scene's draws split by class (opaque, terrain base, terrain light, foliage) for the GPU profile: the interval
// ending at a mark is named after the class it closes. Bounded so the pool isn't exhausted mid-frame.
void Device::ProfileSceneClass(int cls)
{
    if (cls == m_sceneClass || m_sceneClassMarks >= 20)
        return;
    static const char* names[] = {"", "scene opaque", "scene terrain base", "scene terrain light", "scene foliage"};
    if (m_sceneClass > 0 && m_sceneClass < int(sizeof(names) / sizeof(names[0]))) {
        ProfileMark(names[m_sceneClass]);
        ++m_sceneClassMarks;
    }
    m_sceneClass = cls;
}

// CPU time of a part of the frame's recording (milliseconds since `since`, from ProfileCpu()).
double Device::ProfileCpu() const { return CpuNow(); }
void Device::ProfileCpuAdd(const char* name, double since) { ProfileAdd(m_profileCpu, name, CpuNow() - since); }

void Device::ProfileAdd(std::vector<std::pair<const char*, double>>& sums, const char* name, double ms)
{
    for (auto& s : sums)
        if (s.first == name || std::strcmp(s.first, name) == 0) { s.second += ms; return; }
    sums.push_back({name, ms});
}

// A measurement window for the settings sweep (rvk_ddraw.cpp): automatic logging off while one runs.
void Device::ProfileWindow(bool start, const char* label)
{
    m_profileManual = true;
    if (start) {
        m_profileGpu.clear();
        m_profileCpu.clear();
        m_profileFrames = 0;
    } else {
        ProfileLog(label);
    }
}

void Device::ProfileManualEnd() { m_profileManual = false; }

void Device::ProfileDrawSection(const char* name, double& since)
{
    if ((m_frameNumber & 15) != 0)
        return;
    double now = CpuNow();
    ProfileAdd(m_profileCpu, name, std::max(0.0, now - since - m_timerCost) * 16.0);
    since = now;
}

void Device::ProfileLog(const char* label)
{
    if (!m_profileFrames) return;
    auto line = [&](const char* what, std::vector<std::pair<const char*, double>>& sums) {
        std::string out = what;
        char buf[64];
        for (auto& s : sums) {
            std::snprintf(buf, sizeof(buf), " %s %.2f |", s.first, s.second / double(m_profileFrames));
            out += buf;
        }
        if (!out.empty() && out.back() == '|') out.pop_back();
        Log("%s%s (avg of %u frames)", label, out.c_str(), m_profileFrames);
        sums.clear();
    };
    line("gpu ms:", m_profileGpu);
    ProfileAdd(m_profileCpu, "render thread idle", m_profileIdleMs);
    ProfileAdd(m_profileCpu, "game thread in rvk (submit)", m_profileCallerMs);
    ProfileAdd(m_profileCpu, "game thread waiting", double(m_profileGameWaitUs.exchange(0)) * 1e-3);
    m_profileIdleMs = 0.0;
    m_profileCallerMs = 0.0;
    line("cpu ms (frame recording):", m_profileCpu);
    m_profileFrames = 0;
}

}  // namespace rvk
