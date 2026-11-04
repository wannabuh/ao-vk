// Dynamic state emulation for drivers without VK_EXT_vertex_input_dynamic_state or extended dynamic state 3's colour
// blend enable and equation (macOS: MoltenVK, KosmicKrisp - docs/mac.md).
#pragma once

#include "vk.h"

#include <cstdint>

namespace rvk::vk {

// Wraps the device functions in rvk::vk (after LoadDevice): a graphics pipeline made with VK_DYNAMIC_STATE_VERTEX_INPUT_EXT
// (vertexInput) or VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT / _EQUATION_EXT (blend) dynamic becomes a template; the
// vkCmdSet* calls of those states are recorded per command buffer, and a draw binds the template's variant for what was
// recorded - made on first use through the template's pipeline cache, kept until the template is destroyed.
void InstallDynamicStateEmulation(VkDevice device, bool vertexInput, bool blend);

struct DynamicEmulationStats {
    uint32_t templates = 0;    // pipelines kept as templates
    uint32_t variants = 0;     // concrete pipelines made for them
    uint32_t failed = 0;       // variants the driver refused (their draws are skipped)
};
DynamicEmulationStats GetDynamicEmulationStats();

}  // namespace rvk::vk
