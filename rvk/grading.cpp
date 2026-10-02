// Colour grading (an enhancement): the tone mapping's last step (tonemap.frag Grade) - white balance, a cooler and
// paler look at night, contrast, saturation, vignette - and optional 3D lookup tables, one for the day and one for
// the night, blended by daylight. The tables are .cube files the proxy loads (rvk_settings.cpp); without them both
// are the identity.
#include "internal.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace rvk {

using namespace vk;
using namespace detail;

// A size^3 RGBA8 lookup table (red fastest, then green, then blue) as a 3D texture; null data: the identity.
Texture* Device::CreateLut(uint32_t size, const uint8_t* rgba)
{
    size = std::clamp(size, 2u, 64u);
    std::vector<uint8_t> identity;
    if (!rgba) {
        identity.resize(size_t(size) * size * size * 4);
        for (uint32_t b = 0; b < size; ++b)
            for (uint32_t g = 0; g < size; ++g)
                for (uint32_t r = 0; r < size; ++r) {
                    uint8_t* p = &identity[((size_t(b) * size + g) * size + r) * 4];
                    p[0] = uint8_t(r * 255 / (size - 1));
                    p[1] = uint8_t(g * 255 / (size - 1));
                    p[2] = uint8_t(b * 255 / (size - 1));
                    p[3] = 255;
                }
        rgba = identity.data();
    }
    Texture* t = NewTexture(size, size, Format::RGBA8, 1, false);
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_3D;
    ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ci.extent = {size, size, size};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo ac{};
    ac.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateImage(m_allocator, &ci, &ac, &t->m_image, &t->m_allocation, nullptr) != VK_SUCCESS) {
        Log("colour lookup table creation failed (%u^3)", size);
        delete t;
        return nullptr;
    }
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = t->m_image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_3D;
    vi.format = ci.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(m_device, &vi, nullptr, &t->m_view);

    VkDeviceSize bytes = VkDeviceSize(size) * size * size * 4;
    EnsureRingSpace(bytes);
    VkCommandBuffer cmd = UploadCommands();
    void* cpu;
    VkDeviceSize offset = Allocate(bytes, 16, &cpu);
    std::memcpy(cpu, rgba, size_t(bytes));
    ImageBarrier(cmd, t->m_image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkBufferImageCopy region{};
    region.bufferOffset = offset;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {size, size, size};
    vkCmdCopyBufferToImage(cmd, m_frames[m_frameIndex].ring, t->m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    ImageBarrier(cmd, t->m_image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    t->m_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return t;
}

// slot 0: day, 1: night. size 0 or no data: the identity.
void Device::SetColorLut(uint32_t slot, uint32_t size, const uint8_t* rgba)
{
    if (slot > 1)
        return;
    Texture* t = CreateLut(size ? size : 16, size ? rgba : nullptr);
    if (!t)
        return;
    if (m_lut[slot]) DestroyTexture(m_lut[slot]);
    m_lut[slot] = t;
    Log("colour lookup table %s: %s", slot ? "night" : "day", size ? "loaded" : "identity");
}

// The grading part of the tone mapping's parameters (params[8..15], tonemap.frag Grade).
void Device::GradingParams(float out[8]) const
{
    float night = 1.0f - std::clamp(m_daylight / 0.35f, 0.0f, 1.0f);
    out[0] = m_saturation;
    out[1] = m_contrast;
    out[2] = m_warmth;
    out[3] = m_lutAmount;
    out[4] = night;
    out[5] = m_nightTint;
    out[6] = m_vignette;
    out[7] = 0.0f;
}

}  // namespace rvk
