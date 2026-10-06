#include "vk.h"

#include <algorithm>
#include <cstring>

namespace rvk::vk {

#define RVK_DEFINE(name) PFN_##name name;
RVK_VK_GLOBAL_FUNCS(RVK_DEFINE)
RVK_VK_INSTANCE_FUNCS(RVK_DEFINE)
RVK_VK_DEVICE_FUNCS(RVK_DEFINE)
#undef RVK_DEFINE
PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;

bool LoadGlobal()
{
    HMODULE lib = LoadLibraryA("vulkan-1.dll");
    if (!lib)
        return false;
    vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(lib, "vkGetInstanceProcAddr"));
    if (!vkGetInstanceProcAddr)
        return false;
#define RVK_LOAD(name) name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(nullptr, #name));
    RVK_VK_GLOBAL_FUNCS(RVK_LOAD)
#undef RVK_LOAD
    return vkCreateInstance != nullptr;
}

void LoadInstance(VkInstance instance)
{
#define RVK_LOAD(name) name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(instance, #name));
    RVK_VK_INSTANCE_FUNCS(RVK_LOAD)
#undef RVK_LOAD
}

namespace {

constexpr int kMaxCounted = 160;
const char* g_cmdNames[kMaxCounted];
uint64_t g_cmdCounts[kMaxCounted];                 // the render thread's (statistics: no atomics)
bool g_countOn;

// The counting wrapper of one entry point: Index is its slot, Pfn its function pointer type.
template <int Index, typename Pfn>
struct Counted;
template <int Index, typename R, typename... A>
struct Counted<Index, R(VKAPI_PTR*)(A...)> {
    static inline R(VKAPI_PTR* real)(A...) = nullptr;
    static R VKAPI_PTR Call(A... args)
    {
        ++g_cmdCounts[Index];
        return real(args...);
    }
};

}  // namespace

bool CmdCountsOn() { return g_countOn; }

int TakeCmdCounts(const char** names, uint64_t* counts, int max)
{
    int order[kMaxCounted], n = 0;
    for (int i = 0; i < kMaxCounted; ++i)
        if (g_cmdNames[i] && g_cmdCounts[i]) order[n++] = i;
    std::sort(order, order + n, [](int a, int b) { return g_cmdCounts[a] > g_cmdCounts[b]; });
    int out = std::min(n, max);
    for (int i = 0; i < out; ++i) {
        names[i] = g_cmdNames[order[i]];
        counts[i] = g_cmdCounts[order[i]];
    }
    std::memset(g_cmdCounts, 0, sizeof(g_cmdCounts));
    return out;
}

void LoadDevice(VkDevice device)
{
#define RVK_LOAD(name) name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name));
    RVK_VK_DEVICE_FUNCS(RVK_LOAD)
#undef RVK_LOAD
    char value[8] = "";
    g_countOn = GetEnvironmentVariableA("RANDYVK_VKCOUNT", value, sizeof(value)) && value[0] == '1';
    if (!g_countOn)
        return;
    constexpr int base = __COUNTER__;
#define RVK_COUNT(name)                                                                          \
    if (name && std::strncmp(#name, "vkCmd", 5) == 0) {                                          \
        constexpr int slot = __COUNTER__ - base - 1;                                             \
        static_assert(slot < kMaxCounted, "kMaxCounted");                                        \
        using W = Counted<slot, PFN_##name>;                                                     \
        W::real = name;                                                                          \
        name = &W::Call;                                                                         \
        g_cmdNames[slot] = #name;                                                                \
    }
    RVK_VK_DEVICE_FUNCS(RVK_COUNT)
#undef RVK_COUNT
}

}  // namespace rvk::vk
