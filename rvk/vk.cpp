#include "vk.h"

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

void LoadDevice(VkDevice device)
{
#define RVK_LOAD(name) name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name));
    RVK_VK_DEVICE_FUNCS(RVK_LOAD)
#undef RVK_LOAD
}

}  // namespace rvk::vk
