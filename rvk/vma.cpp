// Vulkan Memory Allocator implementation unit. Function pointers come from rvk::vk at allocator creation.
#include "vk.h"
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#define VMA_IMPLEMENTATION
#include <vma/vk_mem_alloc.h>
