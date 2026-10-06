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
const char* const kSceneClassNames[] = {"", "scene statics", "scene terrain base", "scene terrain light",
                                        "scene foliage", "scene characters", "scene effects", "scene sky",
                                        "scene water", "scene rooms", "scene other"};
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
    if (m_shadeQueries) {
        VkQueryPoolCreateInfo si{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        si.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
        si.queryCount = kShadeQueries;
        si.pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
        for (ProfileFrame& p : m_profile)
            if (vkCreateQueryPool(m_device, &si, nullptr, &p.shadePool) != VK_SUCCESS) {
                Log("profiling: no pipeline statistics pool; scene shading not measured");
                for (ProfileFrame& q : m_profile)
                    if (q.shadePool) { vkDestroyQueryPool(m_device, q.shadePool, nullptr); q.shadePool = VK_NULL_HANDLE; }
                break;
            }
    }
    return true;
}

void Device::DestroyProfiler()
{
    for (ProfileFrame& p : m_profile) {
        if (p.pool) { vkDestroyQueryPool(m_device, p.pool, nullptr); p.pool = VK_NULL_HANDLE; }
        if (p.shadePool) { vkDestroyQueryPool(m_device, p.shadePool, nullptr); p.shadePool = VK_NULL_HANDLE; }
    }
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
    if (p.shadePool) {
        if (p.shadeCount) {
            uint64_t v[kShadeQueries];
            if (vkGetQueryPoolResults(m_device, p.shadePool, 0, p.shadeCount, sizeof(v), v, sizeof(uint64_t),
                                      VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
                for (uint32_t i = 0; i < p.shadeCount; ++i)
                    m_shadeSum[p.shadeClass[i]] += v[i];
                m_shadePixels += p.shadePixels;
                if (p.shadeOverflow) ++m_shadeOverflowFrames;
            }
        }
        p.shadeCount = 0;
        p.shadeActive = false;
        p.shadePixels = 0;
        p.shadeOverflow = false;
        vkCmdResetQueryPool(cmd, p.shadePool, 0, kShadeQueries);
    }
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

// The scene's draws split by class (statics, terrain base / light, foliage, characters, effects, sky, water, rooms,
// other) for the GPU profile: the interval ending at a mark is named after the class it closes. Bounded so the pool
// isn't exhausted mid-frame; what comes after the last mark stays in "scene".
void Device::ProfileSceneClass(int cls)
{
    if (cls == m_sceneClass)
        return;
    // The closing class's batched draws (M3) are still pending: issue them first, or the timestamp and the shading
    // count land before them and they are counted in the next class. Only while profiling (it can split a group).
    if (m_profile[m_frameIndex].pool)
        FlushGroup();
    ShadeQueryEnd();
    if (m_sceneClass > 0 && m_sceneClass < kSceneClasses && m_sceneClassMarks < kSceneClassMarks) {
        ProfileMark(kSceneClassNames[m_sceneClass]);
        ++m_sceneClassMarks;
    }
    m_sceneClass = cls;
    ShadeQueryBegin();
}

// Scene shading: the fragment shader invocations of the draws of one class, counted from here to ShadeQueryEnd (the
// class changes or the rendering ends). Early-Z-rejected fragments aren't invoked, so invocations / pixels is the
// overdraw the shading actually pays for (helper invocations at triangle edges may be counted too).
void Device::ShadeQueryBegin()
{
    ProfileFrame& p = m_profile[m_frameIndex];
    if (!p.shadePool || p.shadeActive || !m_rendering || !m_scenePhase || m_target != m_scene || m_sceneClass <= 0 ||
        m_sceneClass >= kSceneClasses)
        return;
    if (p.shadeCount >= kShadeQueries) {
        p.shadeOverflow = true;
        return;
    }
    vkCmdBeginQuery(m_frames[m_frameIndex].main, p.shadePool, p.shadeCount, 0);
    p.shadeClass[p.shadeCount] = uint8_t(m_sceneClass);
    p.shadeActive = true;
    if (!p.shadePixels && m_scene)
        p.shadePixels = uint64_t(m_scene->m_width) * m_scene->m_height;
}

void Device::ShadeQueryEnd()
{
    ProfileFrame& p = m_profile[m_frameIndex];
    if (!p.shadeActive)
        return;
    vkCmdEndQuery(m_frames[m_frameIndex].main, p.shadePool, p.shadeCount);
    ++p.shadeCount;
    p.shadeActive = false;
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
        std::memset(m_shadeSum, 0, sizeof(m_shadeSum));
        m_shadePixels = 0;
        m_shadeOverflowFrames = 0;
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
    if (m_shadePixels) {                          // fragment shader invocations per scene pixel, by class
        std::string out;
        char buf[64];
        uint64_t all = 0;
        for (int c = 1; c < kSceneClasses; ++c) {
            all += m_shadeSum[c];
            if (!m_shadeSum[c]) continue;
            std::snprintf(buf, sizeof(buf), " %s %.2f |", kSceneClassNames[c] + 6, double(m_shadeSum[c]) / double(m_shadePixels));
            out += buf;
        }
        std::snprintf(buf, sizeof(buf), " all %.2f", double(all) / double(m_shadePixels));
        out += buf;
        if (m_shadeOverflowFrames) {
            std::snprintf(buf, sizeof(buf), " (%u frames ran out of queries: low)", m_shadeOverflowFrames);
            out += buf;
        }
        Log("%sscene shading (pixel-shader runs per screen pixel):%s", label, out.c_str());
    }
    std::memset(m_shadeSum, 0, sizeof(m_shadeSum));
    m_shadePixels = 0;
    m_shadeOverflowFrames = 0;
    m_profileFrames = 0;
}

}  // namespace rvk
