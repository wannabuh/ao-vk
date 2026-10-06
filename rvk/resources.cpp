// Texture formats, textures and render targets, samplers, vertex buffers and FVF layouts.
#include "internal.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace rvk {

using namespace vk;
using namespace detail;

namespace {

constexpr VkComponentMapping kIdentity = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                          VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
constexpr VkComponentMapping kOpaque = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_ONE};
constexpr VkComponentMapping kLuminance = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R,
                                           VK_COMPONENT_SWIZZLE_ONE};
constexpr VkComponentMapping kAlphaOnly = {VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO,
                                           VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_R};
constexpr VkComponentMapping kLuminanceAlpha = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R,
                                                VK_COMPONENT_SWIZZLE_G};

// Indexed by Format. D3D's 16-bit formats have the same bit layout as Vulkan's *_PACK16 formats.
const FormatInfo kFormats[] = {
    {VK_FORMAT_B8G8R8A8_UNORM, kIdentity, 4, false},          // A8R8G8B8
    {VK_FORMAT_B8G8R8A8_UNORM, kOpaque, 4, false},            // X8R8G8B8
    {VK_FORMAT_R5G6B5_UNORM_PACK16, kIdentity, 2, false},     // R5G6B5
    {VK_FORMAT_A1R5G5B5_UNORM_PACK16, kIdentity, 2, false},   // A1R5G5B5
    {VK_FORMAT_A1R5G5B5_UNORM_PACK16, kOpaque, 2, false},     // X1R5G5B5
    {VK_FORMAT_A4R4G4B4_UNORM_PACK16, kIdentity, 2, false},   // A4R4G4B4
    {VK_FORMAT_R8_UNORM, kLuminance, 1, false},               // L8
    {VK_FORMAT_R8_UNORM, kAlphaOnly, 1, false},               // A8
    {VK_FORMAT_R8G8_UNORM, kLuminanceAlpha, 2, false},        // A8L8
    {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, kIdentity, 8, true},     // DXT1 (1-bit alpha allowed)
    {VK_FORMAT_BC2_UNORM_BLOCK, kIdentity, 16, true},         // DXT2 (premultiplied; same storage)
    {VK_FORMAT_BC2_UNORM_BLOCK, kIdentity, 16, true},         // DXT3
    {VK_FORMAT_BC3_UNORM_BLOCK, kIdentity, 16, true},         // DXT4 (premultiplied; same storage)
    {VK_FORMAT_BC3_UNORM_BLOCK, kIdentity, 16, true},         // DXT5
    {VK_FORMAT_R16G16B16A16_SFLOAT, kIdentity, 8, false},     // RGBA16F (HDR scene)
    {VK_FORMAT_B10G11R11_UFLOAT_PACK32, kIdentity, 4, false}, // RG11B10F (HDR glow)
    {VK_FORMAT_R16G16_SFLOAT, kIdentity, 4, false},          // RG16F (ambient occlusion)
    {VK_FORMAT_R8G8B8A8_UNORM, kIdentity, 4, false},         // RGBA8 (local-light fraction, reflectivity, sun share)
};
static_assert(sizeof(kFormats) / sizeof(kFormats[0]) == size_t(Format::Count), "format table");

VkSamplerAddressMode AddressMode(uint32_t a)
{
    switch (a) {
    case d3d::TADDRESS_MIRROR: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case d3d::TADDRESS_CLAMP: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case d3d::TADDRESS_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

}  // namespace

namespace detail {

const FormatInfo& GetFormatInfo(Format format) { return kFormats[size_t(format)]; }

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

}  // namespace detail

bool FormatIsCompressed(Format format) { return kFormats[size_t(format)].compressed; }

uint32_t FormatRowBytes(Format format, uint32_t width)
{
    const FormatInfo& f = kFormats[size_t(format)];
    return f.compressed ? std::max(1u, (width + 3) / 4) * f.blockBytes : width * f.blockBytes;
}

uint32_t FormatRows(Format format, uint32_t height)
{
    return kFormats[size_t(format)].compressed ? std::max(1u, (height + 3) / 4) : height;
}

uint32_t Device::FvfStride(uint32_t fvf) { return DecodeFvf(fvf).stride; }

// ---------------------------------------------------------------------------------------------------
// Textures

Texture* Device::NewTexture(uint32_t width, uint32_t height, Format format, uint32_t levels, bool renderTarget)
{
    if (!width || !height)
        return nullptr;
    uint32_t maxLevels = 1;
    for (uint32_t s = std::max(width, height); s > 1; s >>= 1)
        ++maxLevels;
    auto* t = new Texture;
    t->m_width = width;
    t->m_height = height;
    t->m_levels = std::clamp(levels, 1u, maxLevels);
    t->m_format = format;
    t->m_renderTarget = renderTarget;
    return t;
}

bool Device::RealizeTexture(Texture* t)
{
    const FormatInfo& info = kFormats[size_t(t->m_format)];
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = info.vk;
    ci.extent = {t->m_width, t->m_height, 1};
    ci.mipLevels = t->m_levels;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (t->m_renderTarget)
        ci.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo ac{};
    ac.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (t->m_renderTarget)
        ac.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    if (vmaCreateImage(m_allocator, &ci, &ac, &t->m_image, &t->m_allocation, nullptr) != VK_SUCCESS) {
        t->m_image = VK_NULL_HANDLE;
        Log("texture creation failed (%ux%u, format %u)", t->m_width, t->m_height, uint32_t(t->m_format));
        return false;
    }
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = t->m_image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = info.vk;
    vi.components = info.swizzle;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, t->m_levels, 0, 1};
    vkCreateImageView(m_device, &vi, nullptr, &t->m_view);
    RegisterBindlessTexture(t);
    return true;
}

Texture* Device::CreateImage(uint32_t width, uint32_t height, Format format, uint32_t levels, bool renderTarget)
{
    Texture* t = NewTexture(width, height, format, levels, renderTarget);
    if (t && !RealizeTexture(t)) {
        delete t;
        return nullptr;
    }
    return t;
}

Texture* Device::CreateTexture(uint32_t width, uint32_t height, Format format, uint32_t levels)
{
    if (!m_formatSupported[size_t(format)]) {
        Log("texture format %u not supported by the GPU\n", uint32_t(format));
        return nullptr;
    }
    return CreateImage(width, height, format, levels, false);
}

Texture* Device::CreateTexture(uint32_t width, uint32_t height, const void* argbPixels)
{
    Texture* t = CreateTexture(width, height, Format::A8R8G8B8, 1);
    if (t)
        UpdateTexture(t, 0, 0, 0, width, height, argbPixels, width * 4);
    return t;
}

Texture* Device::CreateRenderTarget(uint32_t width, uint32_t height)
{
    return CreateImage(width, height, Format::A8R8G8B8, 1, true);
}

// Whether the pixels prove a texture fully opaque (alpha 1 everywhere), so a draw that blends it as a static (the
// game's cut-out idiom) can take the opaque path instead: no blend and no discard, so early-Z works and the
// overdraw of a screen full of blended statics collapses. Conservative: false means "not known to be opaque".
static bool PixelsOpaque(Format fmt, const void* data, uint32_t width, uint32_t height, uint32_t pitch)
{
    const uint8_t* base = static_cast<const uint8_t*>(data);
    auto rows = [&](auto check) {               // check(row, x) for every pixel
        for (uint32_t y = 0; y < height; ++y)
            if (!check(base + size_t(y) * pitch)) return false;
        return true;
    };
    switch (fmt) {
    case Format::X8R8G8B8: case Format::R5G6B5: case Format::X1R5G5B5: case Format::L8:
        return true;                            // no alpha channel: the shader reads alpha 1
    case Format::A8R8G8B8:
        return rows([&](const uint8_t* row) {
            for (uint32_t x = 0; x < width; ++x) if (row[size_t(x) * 4 + 3] != 0xFF) return false;
            return true; });
    case Format::A1R5G5B5:
        return rows([&](const uint8_t* row) {
            auto r = reinterpret_cast<const uint16_t*>(row);
            for (uint32_t x = 0; x < width; ++x) if ((r[x] & 0x8000u) == 0) return false;
            return true; });
    case Format::A4R4G4B4:
        return rows([&](const uint8_t* row) {
            auto r = reinterpret_cast<const uint16_t*>(row);
            for (uint32_t x = 0; x < width; ++x) if (((r[x] >> 12) & 0xF) != 0xF) return false;
            return true; });
    case Format::A8:
        return rows([&](const uint8_t* row) {
            for (uint32_t x = 0; x < width; ++x) if (row[x] != 0xFF) return false;
            return true; });
    case Format::A8L8:
        return rows([&](const uint8_t* row) {
            for (uint32_t x = 0; x < width; ++x) if (row[size_t(x) * 2 + 1] != 0xFF) return false;
            return true; });
    case Format::DXT1: {
        uint32_t bx = (width + 3) / 4, by = (height + 3) / 4;
        for (uint32_t y = 0; y < by; ++y) {
            const uint8_t* row = base + size_t(y) * pitch;
            for (uint32_t x = 0; x < bx; ++x) {
                const uint8_t* blk = row + size_t(x) * 8;
                uint16_t c0 = uint16_t(blk[0] | blk[1] << 8), c1 = uint16_t(blk[2] | blk[3] << 8);
                if (c0 <= c1) {                 // 3-colour mode: the index-3 pixels are transparent
                    uint32_t bits;
                    std::memcpy(&bits, blk + 4, 4);
                    for (int p = 0; p < 16; ++p) if (((bits >> (2 * p)) & 3u) == 3u) return false;
                }
            }
        }
        return true;
    }
    case Format::DXT2: case Format::DXT3: {
        uint32_t bx = (width + 3) / 4, by = (height + 3) / 4;
        for (uint32_t y = 0; y < by; ++y) {
            const uint8_t* row = base + size_t(y) * pitch;
            for (uint32_t x = 0; x < bx; ++x)
                for (int b = 0; b < 8; ++b)      // explicit 4-bit alpha: opaque iff every nibble is 0xF
                    if (row[size_t(x) * 16 + b] != 0xFF) return false;
        }
        return true;
    }
    case Format::DXT4: case Format::DXT5: {
        uint32_t bx = (width + 3) / 4, by = (height + 3) / 4;
        for (uint32_t y = 0; y < by; ++y) {
            const uint8_t* row = base + size_t(y) * pitch;
            for (uint32_t x = 0; x < bx; ++x) {
                const uint8_t* blk = row + size_t(x) * 16;
                if (blk[0] != 0xFF || blk[1] != 0xFF) return false;   // both ends 255: every interpolated alpha is 255
            }
        }
        return true;
    }
    default:
        return false;
    }
}

void Device::UpdateTexture(Texture* t, uint32_t level, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                           const void* data, uint32_t pitch)
{
    if (!t || !t->m_image || level >= t->m_levels || !width || !height)
        return;
    uint32_t rowBytes = FormatRowBytes(t->m_format, width), rows = FormatRows(t->m_format, height);

    // Whether the texture is fully opaque, for the static-draw opaque fast path (Draw): only a full level-0 upload
    // proves it; a partial update makes it unknown again.
    if (level == 0) {
        if (x == 0 && y == 0 && width >= t->m_width && height >= t->m_height &&
            pitch >= FormatRowBytes(t->m_format, t->m_width))
            t->m_opaque = PixelsOpaque(t->m_format, data, t->m_width, t->m_height, pitch);
        else
            t->m_opaque = false;
    }

    // Stage tightly packed rows in this frame's ring buffer; the upload command buffer runs before the
    // frame's draws.
    EnsureRingSpace(VkDeviceSize(rowBytes) * rows);
    VkCommandBuffer cmd = UploadCommands();
    void* cpu;
    VkDeviceSize offset = Allocate(VkDeviceSize(rowBytes) * rows, 16, &cpu);
    for (uint32_t r = 0; r < rows; ++r)
        std::memcpy(static_cast<uint8_t*>(cpu) + size_t(r) * rowBytes, static_cast<const uint8_t*>(data) + size_t(r) * pitch,
                    rowBytes);

    // Render targets change layout in the main command buffer; uploads into them aren't supported.
    if (t->m_renderTarget)
        return;
    VkImageLayout from = t->m_layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_IMAGE_LAYOUT_UNDEFINED
                                                                   : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ImageBarrier(cmd, t->m_image, VK_IMAGE_ASPECT_COLOR_BIT, from, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkBufferImageCopy region{};
    region.bufferOffset = offset;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
    region.imageOffset = {int32_t(x), int32_t(y), 0};
    uint32_t levelW = std::max(1u, t->m_width >> level), levelH = std::max(1u, t->m_height >> level);
    region.imageExtent = {std::min(width, levelW - x), std::min(height, levelH - y), 1};
    vkCmdCopyBufferToImage(cmd, m_frames[m_frameIndex].ring, t->m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    ImageBarrier(cmd, t->m_image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                 VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    t->m_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

void Device::DestroyTexture(Texture* texture)
{
    if (!texture || texture == m_main)
        return;
    for (auto& t : m_textures)
        if (t == texture) t = nullptr;
    if (m_target == texture)
        SetRenderTarget(nullptr);
    ForgetCasterTexture(texture);
    if (texture->m_normalMap) {
        DestroyTexture(texture->m_normalMap);
        texture->m_normalMap = nullptr;
    }
    if (HoldParticleTexture(texture))
        return;                                  // freed once the particles drawn with it have faded out
    m_deadTextures.push_back({DeathTag(), texture});   // freed once every submission that may use it is done
}

void Device::SetNormalMap(Texture* texture, Texture* normal)
{
    if (!texture || texture->m_normalMap == normal)
        return;
    if (texture->m_normalMap)
        DestroyTexture(texture->m_normalMap);
    texture->m_normalMap = normal;
    m_constantsDirty = true;
}

void Device::DestroyTextureNow(Texture* t)
{
    UnregisterBindlessTexture(t);                // clears the slot to the black image before the view goes
    if (t->m_view) vkDestroyImageView(m_device, t->m_view, nullptr);
    if (t->m_image) vmaDestroyImage(m_allocator, t->m_image, t->m_allocation);
    delete t;
}

// Bindless textures (M1): a slot in set 1's image array, written when the texture appears and cleared to black when
// it goes. Update-after-bind lets these update while frames are in flight.
void Device::RegisterBindlessTexture(Texture* t)
{
    if (!m_bindless || !t || !t->m_view || m_bindlessSet == VK_NULL_HANDLE)
        return;
    uint32_t index;
    if (!m_freeBindlessImages.empty()) {
        index = m_freeBindlessImages.back();
        m_freeBindlessImages.pop_back();
    } else if (m_nextBindlessImage < kMaxBindlessImages) {
        index = m_nextBindlessImage++;
    } else {
        static bool logged;
        if (!logged) { logged = true; Log("bindless textures: out of slots (%u)", kMaxBindlessImages); }
        return;
    }
    t->m_bindless = index;
    VkDescriptorImageInfo img{VK_NULL_HANDLE, t->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = m_bindlessSet;
    w.dstBinding = 0;
    w.dstArrayElement = index;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w.pImageInfo = &img;
    vkUpdateDescriptorSets(m_device, 1, &w, 0, nullptr);
}

void Device::UnregisterBindlessTexture(Texture* t)
{
    if (!m_bindless || !t || t->m_bindless == ~0u)
        return;
    if (m_bindlessSet != VK_NULL_HANDLE) {
        // A null view is allowed for a sampled-image slot (and the binding is partially bound). It must not be
        // sampled, but no draw should still hold this index.
        VkDescriptorImageInfo img{VK_NULL_HANDLE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = m_bindlessSet;
        w.dstBinding = 0;
        w.dstArrayElement = t->m_bindless;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        w.pImageInfo = &img;
        vkUpdateDescriptorSets(m_device, 1, &w, 0, nullptr);
    }
    m_freeBindlessImages.push_back(t->m_bindless);
    t->m_bindless = ~0u;
}

uint32_t Device::BindlessImage(Texture* t)
{
    if (!m_bindless)
        return 0;
    if (!t)
        t = m_blackTexture;
    if (t && t->m_bindless == ~0u)
        RegisterBindlessTexture(t);
    if (t && t->m_bindless != ~0u)
        return t->m_bindless;
    return m_blackTexture ? m_blackTexture->m_bindless : 0u;
}

uint32_t Device::BindlessSampler(VkSampler s)
{
    if (!m_bindless)
        return 0;
    uint32_t index = RegisterBindlessSampler(s);
    return index == ~0u ? 0u : index;
}

uint32_t Device::RegisterBindlessSampler(VkSampler s)
{
    if (!m_bindless || !s || m_bindlessSet == VK_NULL_HANDLE)
        return ~0u;
    auto it = m_bindlessSamplerIndex.find(uint64_t(s));
    if (it != m_bindlessSamplerIndex.end())
        return it->second;
    uint32_t index;
    if (!m_freeBindlessSamplers.empty()) {
        index = m_freeBindlessSamplers.back();
        m_freeBindlessSamplers.pop_back();
    } else if (m_nextBindlessSampler < kMaxBindlessSamplers) {
        index = m_nextBindlessSampler++;
    } else {
        return ~0u;
    }
    m_bindlessSamplerIndex.emplace(uint64_t(s), index);
    VkDescriptorImageInfo img{s, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = m_bindlessSet;
    w.dstBinding = 1;
    w.dstArrayElement = index;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    w.pImageInfo = &img;
    vkUpdateDescriptorSets(m_device, 1, &w, 0, nullptr);
    return index;
}

uint64_t Device::SamplerKey(uint32_t stage) const
{
    const auto& t = m_tss[stage];
    return uint64_t(t[d3d::TSS_MAGFILTER] & 7) | uint64_t(t[d3d::TSS_MINFILTER] & 7) << 3 |
           uint64_t(t[d3d::TSS_MIPFILTER] & 7) << 6 | uint64_t(t[d3d::TSS_ADDRESSU] & 7) << 9 |
           uint64_t(t[d3d::TSS_ADDRESSV] & 7) << 12 | uint64_t(t[d3d::TSS_MAXMIPLEVEL] & 15) << 15 |
           uint64_t(t[d3d::TSS_BORDERCOLOR] >> 24 ? 1 : 0) << 19 |
           uint64_t(m_anisotropy) << 20;        // samplers made at another level stay valid for work in flight
}

// Samplers are never destroyed while the device lives (SamplerFor's map owns them), so a stage whose sampler state
// is unchanged keeps its slot. Only a successful registration is remembered.
uint32_t Device::StageSamplerSlot(uint32_t stage)
{
    if (!m_bindless)
        return 0;
    uint64_t key = SamplerKey(stage);
    if (key == m_stageSamplerKey[stage])
        return m_stageSamplerSlot[stage];
    uint32_t index = RegisterBindlessSampler(SamplerFor(stage));
    if (index == ~0u)
        return 0;
    m_stageSamplerKey[stage] = key;
    m_stageSamplerSlot[stage] = index;
    return index;
}

uint32_t Device::FixedSamplerSlot(uint32_t which, VkSampler s)
{
    if (!m_bindless)
        return 0;
    if (s && s == m_fixedSampler[which])
        return m_fixedSamplerSlot[which];
    uint32_t index = RegisterBindlessSampler(s);
    if (index == ~0u)
        return 0;
    m_fixedSampler[which] = s;
    m_fixedSamplerSlot[which] = index;
    return index;
}

VkSampler Device::SamplerFor(uint32_t stage)
{
    const auto& t = m_tss[stage];
    uint32_t addrU = t[d3d::TSS_ADDRESSU], addrV = t[d3d::TSS_ADDRESSV];
    uint64_t key = SamplerKey(stage);
    auto it = m_samplers.find(key);
    if (it != m_samplers.end())
        return it->second;
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter = t[d3d::TSS_MAGFILTER] == d3d::TFG_POINT ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    ci.minFilter = t[d3d::TSS_MINFILTER] == d3d::TFN_POINT ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    ci.mipmapMode = t[d3d::TSS_MIPFILTER] == d3d::TFP_LINEAR ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.minLod = float(t[d3d::TSS_MAXMIPLEVEL]);           // D3D MAXMIPLEVEL = most detailed level used
    ci.maxLod = t[d3d::TSS_MIPFILTER] == d3d::TFP_NONE ? ci.minLod : VK_LOD_CLAMP_NONE;
    ci.addressModeU = AddressMode(addrU);
    ci.addressModeV = AddressMode(addrV);
    ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    ci.borderColor = (t[d3d::TSS_BORDERCOLOR] >> 24) ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK
                                                     : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    // Enhancement: anisotropic filtering for textures the game filters linearly (point-filtered ones stay crisp), as
    // DXVK's forced anisotropy does.
    if (m_anisotropy > 1 && ci.minFilter == VK_FILTER_LINEAR && ci.magFilter == VK_FILTER_LINEAR) {
        ci.anisotropyEnable = VK_TRUE;
        ci.maxAnisotropy = float(m_anisotropy);
    }
    VkSampler sampler = VK_NULL_HANDLE;
    vkCreateSampler(m_device, &ci, nullptr, &sampler);
    m_samplers.emplace(key, sampler);
    RegisterBindlessSampler(sampler);
    return sampler;
}

// ---------------------------------------------------------------------------------------------------
// Vertex buffers

VertexBuffer* Device::CreateVertexBuffer(uint32_t fvf, uint32_t vertexCount)
{
    auto* vb = new VertexBuffer;
    vb->m_fvf = fvf;
    vb->m_stride = FvfStride(fvf);
    vb->m_count = vertexCount;
    vb->m_data.resize(size_t(vb->m_stride) * vertexCount);
    return vb;
}

void* Device::Lock(VertexBuffer* vb)
{
    vb->m_locked = true;
    return vb->m_data.data();
}

void Device::Unlock(VertexBuffer* vb) { vb->m_locked = false; }

void Device::DestroyVertexBuffer(VertexBuffer* vb) { delete vb; }   // draws already copied what they used

}  // namespace rvk
