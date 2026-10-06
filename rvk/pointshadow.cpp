// Point light shadows (an enhancement, like the sun's; shadow.cpp). At the end of a frame the point / spot lights
// the game used that are nearest the camera get a cube shadow map each, rendered from the frame's shadow casters
// (the same list as the sun's, remembered off-screen statics included). The next frame lights with exactly those
// lights (the light override uses the previous frame's captured set), so a light's shadow is found by its data.
//
// All cubes live in one cube-array depth image: cube i = layers 6i .. 6i+5 in Vulkan's face order (+X -X +Y -Y +Z -Z).
// The depth stored is the perspective depth along the face's axis, near kPointShadowNear and far the light's range;
// the shader recomputes it from the largest component of (point - light), which picks the same face.
#include "internal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace rvk {

using namespace vk;
using namespace detail;

namespace {

// Per face: major axis, s axis, t axis (Vulkan's cube map face selection: s = sc/|ma|, t = tc/|ma|, mapped to 0..1).
const float kFaceAxes[6][3][3] = {
    {{1, 0, 0}, {0, 0, -1}, {0, -1, 0}},    // +X: sc = -z, tc = -y
    {{-1, 0, 0}, {0, 0, 1}, {0, -1, 0}},    // -X: sc = +z, tc = -y
    {{0, 1, 0}, {1, 0, 0}, {0, 0, 1}},      // +Y: sc = +x, tc = +z
    {{0, -1, 0}, {1, 0, 0}, {0, 0, -1}},    // -Y: sc = +x, tc = -z
    {{0, 0, 1}, {1, 0, 0}, {0, -1, 0}},     // +Z: sc = +x, tc = -y
    {{0, 0, -1}, {-1, 0, 0}, {0, -1, 0}},   // -Z: sc = -x, tc = -y
};

// World -> clip for one face (D3D row-vector matrix). With d = p - light: clip = (s.d, t.d, a*w + b, w), w = ma.d,
// so NDC x, y = the face's texture coordinates (Vulkan's y points down the image, as t does) and
// NDC z = a + b / w = 0 at the near plane, 1 at the light's range.
d3d::Matrix FaceViewProj(int face, const float light[3], float nearPlane, float farPlane)
{
    const float(*ax)[3] = kFaceAxes[face];
    float a = farPlane / (farPlane - nearPlane), b = -farPlane * nearPlane / (farPlane - nearPlane);
    d3d::Matrix m{};
    for (int i = 0; i < 3; ++i) {
        m.m[i][0] = ax[1][i];
        m.m[i][1] = ax[2][i];
        m.m[i][2] = a * ax[0][i];
        m.m[i][3] = ax[0][i];
    }
    for (int j = 0; j < 4; ++j)
        m.m[3][j] = -(light[0] * m.m[0][j] + light[1] * m.m[1][j] + light[2] * m.m[2][j]);
    m.m[3][2] += b;
    return m;
}

// Squared distance from a point to a box (0 inside).
float BoxDistance2(const float mn[3], const float mx[3], const float p[3])
{
    float d2 = 0.0f;
    for (int j = 0; j < 3; ++j) {
        float d = std::max({mn[j] - p[j], 0.0f, p[j] - mx[j]});
        d2 += d * d;
    }
    return d2;
}

}  // namespace

// The point lights' cube array: m_cubeCount cubes of m_pointShadowSize faces.
bool Device::CreatePointShadowMaps(std::string* error)
{
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = kDepthFormat;
    ci.extent = {m_pointShadowSize, m_pointShadowSize, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = m_cubeCount * 6;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo ac{};
    ac.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    ac.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    if (!Check(vmaCreateImage(m_allocator, &ci, &ac, &m_cubeImage, &m_cubeAllocation, nullptr), "point shadow maps", error))
        return false;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = m_cubeImage;
    vi.viewType = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    vi.format = kDepthFormat;
    vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, m_cubeCount * 6};
    if (!Check(vkCreateImageView(m_device, &vi, nullptr, &m_cubeArrayView), "point shadow view", error))
        return false;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.subresourceRange.layerCount = 1;
    for (uint32_t layer = 0; layer < m_cubeCount * 6; ++layer) {
        vi.subresourceRange.baseArrayLayer = layer;
        if (!Check(vkCreateImageView(m_device, &vi, nullptr, &m_cubeFaceViews[layer]), "point shadow face view", error))
            return false;
    }
    m_cubeLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    return true;
}

void Device::DestroyPointShadowMaps()
{
    for (VkImageView& v : m_cubeFaceViews)
        if (v) { vkDestroyImageView(m_device, v, nullptr); v = VK_NULL_HANDLE; }
    if (m_cubeArrayView) vkDestroyImageView(m_device, m_cubeArrayView, nullptr);
    if (m_cubeImage) vmaDestroyImage(m_allocator, m_cubeImage, m_cubeAllocation);
    m_cubeArrayView = VK_NULL_HANDLE;
    m_cubeImage = VK_NULL_HANDLE;
    m_cubeLayout = VK_IMAGE_LAYOUT_UNDEFINED;
}

bool Device::CreatePointShadowResources(std::string* error)
{
    if (!CreatePointShadowMaps(error))
        return false;
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;               // 2x2 comparison filtering per tap
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.compareEnable = VK_TRUE;
    si.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    si.maxLod = 0.0f;
    return Check(vkCreateSampler(m_device, &si, nullptr, &m_cubeSampler), "point shadow sampler", error);
}

void Device::DestroyPointShadowResources()
{
    if (m_cubeSampler) vkDestroySampler(m_device, m_cubeSampler, nullptr);
    DestroyPointShadowMaps();
    m_cubeSampler = VK_NULL_HANDLE;
}

// Before the frame's first rendering: the cubes must be sampleable from the start (cleared = lit).
void Device::PreparePointShadowMaps(VkCommandBuffer cmd)
{
    if (m_cubeLayout != VK_IMAGE_LAYOUT_UNDEFINED)
        return;
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, m_cubeCount * 6};
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = m_cubeImage;
    b.subresourceRange = range;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
    VkClearDepthStencilValue clear{1.0f, 0};
    vkCmdClearDepthStencilImage(cmd, m_cubeImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
    b.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier2(cmd, &dep);
    m_cubeLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

// Point shadows fade from full strength (night, weak sun) to m_pointShadowDay of it in full daylight.
float Device::PointShadowStrength() const
{
    float t = std::clamp((m_daylight - 0.1f) / 0.35f, 0.0f, 1.0f);
    t = t * t * (3.0f - 2.0f * t);
    return m_pointShadowStrength * (1.0f + (m_pointShadowDay - 1.0f) * t);
}

bool Device::IsCarrierPart(const CapturedLight& c, const d3d::Matrix& world, const float boundsMin[3],
                           const float boundsMax[3])
{
    float extent = std::max({boundsMax[0] - boundsMin[0], boundsMax[1] - boundsMin[1], boundsMax[2] - boundsMin[2]});
    if (!c.hasCarrier || extent >= 3.0f)
        return false;
    // The carrier's parts: the body (origin 2.1-2.2 under the light in the dumps) and its attachments (0.5-1.3 under,
    // within ~0.3 of the body sideways).
    float dx = world.m[3][0] - c.carrier[0], dz = world.m[3][2] - c.carrier[2];
    float dy = world.m[3][1] - c.light.position.y;
    if (dx * dx + dz * dz < 0.5f * 0.5f && dy > -2.6f && dy < 0.3f)
        return true;
    // Held items (weapons): their origin is the hand, which swings further out while walking. Only small things, by
    // where they are rather than their origin, so a character standing next to the carrier still casts.
    if (extent >= 1.6f)
        return false;
    float cx = 0.5f * (boundsMin[0] + boundsMax[0]) - c.carrier[0], cz = 0.5f * (boundsMin[2] + boundsMax[2]) - c.carrier[2];
    float cy = 0.5f * (boundsMin[1] + boundsMax[1]) - c.light.position.y;
    return cx * cx + cz * cz < 1.0f && cy > -2.6f && cy < 0.5f;
}

// Drawn this frame: one of the carrier's runs of draws (FindCarriers), so a character walking through the carrier
// keeps casting. Remembered copies have no run; near the carrier they're copies of it from where it stood.
bool Device::IsCarrierItem(const CapturedLight& c, const ShadowItem& item)
{
    if (!item.cached)
        return c.hasCarrier && std::binary_search(c.carrierGroups.begin(), c.carrierGroups.end(), item.group);
    return IsCarrierPart(c, item.world, item.boundsMin, item.boundsMax);
}

void Device::FindCarriers()
{
    // Each run's box (union of its casters') and lowest origin, for the held-item test below.
    uint32_t groups = 0;
    for (const ShadowItem& it : m_shadowItems)
        if (!it.cached) groups = std::max(groups, it.group + 1);
    std::vector<std::array<float, 6>> groupBox(groups, {1e30f, 1e30f, 1e30f, -1e30f, -1e30f, -1e30f});
    std::vector<float> groupLowestOrigin(groups, 1e30f);
    for (const ShadowItem& it : m_shadowItems) {
        if (it.cached) continue;
        auto& b = groupBox[it.group];
        for (int j = 0; j < 3; ++j) {
            b[j] = std::min(b[j], it.boundsMin[j]);
            b[3 + j] = std::max(b[3 + j], it.boundsMax[j]);
        }
        groupLowestOrigin[it.group] = std::min(groupLowestOrigin[it.group], it.world.m[3][1]);
    }

    const bool exact = m_sceneLightsFrame == m_frameNumber;   // the game's own lights with their carriers
    for (CapturedLight& c : m_lightsCur) {
        c.hasCarrier = false;
        c.owner = 0;
        c.carrierGroup = ~0u;
        c.carrierGroups.clear();
        if (c.light.range < 1.0f)
            continue;
        const d3d::Vector& p = c.light.position;
        if (exact) {
            // The scene light it is (same range, where it stands), its character's pieces this frame.
            uint32_t owner = 0;
            float bestD2 = 0.3f * 0.3f;
            for (const SceneLight& s : m_sceneLights) {
                if (s.light.range != c.light.range) continue;
                float dx = s.light.position.x - p.x, dy = s.light.position.y - p.y, dz = s.light.position.z - p.z;
                float d2 = dx * dx + dy * dy + dz * dz;
                if (d2 < bestD2) { bestD2 = d2; owner = s.owner; }
            }
            c.owner = owner;
            if (!owner)
                continue;
            std::vector<uint8_t> linked(groups, 0);
            for (const ShadowItem& it : m_shadowItems) {
                if (it.cached || it.owner != owner) continue;
                linked[it.group] = 1;
                if (!c.hasCarrier || it.world.m[3][1] < c.carrier[1]) {   // its feet: the lowest origin
                    c.carrier[0] = it.world.m[3][0];
                    c.carrier[1] = it.world.m[3][1];
                    c.carrier[2] = it.world.m[3][2];
                    c.carrierGroup = it.group;
                }
                c.hasCarrier = true;
            }
            for (uint32_t g = 0; g < groups; ++g)
                if (linked[g]) c.carrierGroups.push_back(g);
            continue;
        }
        // The same light last frame (it moves with its character, a little per frame) and who carried it.
        const CapturedLight* prev = nullptr;
        float prevBest = 1.0f;
        for (const CapturedLight& q : m_lightsPrev) {
            if (!q.hasCarrier || q.light.range != c.light.range)
                continue;
            float dx = q.light.position.x - p.x, dy = q.light.position.y - p.y, dz = q.light.position.z - p.z;
            float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (d < prevBest) {
                prevBest = d;
                prev = &q;
            }
        }
        float best = 1e30f;
        for (const ShadowItem& it : m_shadowItems) {
            if (it.cached)                       // remembered copies may stand where the character was
                continue;
            float dy = it.world.m[3][1] - p.y;
            if (dy <= -2.6f || dy >= 0.3f)       // where IsCarrierPart looks for the carrier's parts
                continue;
            float extent = std::max({it.boundsMax[0] - it.boundsMin[0], it.boundsMax[1] - it.boundsMin[1],
                                     it.boundsMax[2] - it.boundsMin[2]});
            if (extent >= 3.0f)
                continue;
            float dx = it.world.m[3][0] - p.x, dz = it.world.m[3][2] - p.z, d2 = dx * dx + dz * dz;
            // Standing: the body origin 1.5-2.6 under the head-height light, up to ~0.4 sideways while moving.
            // Sitting (and other low poses) bring the head, and the light, down: right under it, closer.
            // Either way, the character that carried it last frame keeps it while its pose changes.
            float key = 1e30f;
            if (prev) {
                float px = it.world.m[3][0] - prev->carrier[0], pz = it.world.m[3][2] - prev->carrier[2];
                float p2 = px * px + pz * pz;
                if (p2 < 0.3f * 0.3f)
                    key = p2 - 100.0f;           // before any match by position alone
            }
            if (key > 0.0f && dy < -1.5f && d2 < 1.0f)
                key = d2;
            else if (key > 0.0f && dy < -0.5f && d2 < 0.3f * 0.3f)
                key = d2;
            if (key >= best)
                continue;
            best = key;
            c.hasCarrier = true;
            c.carrier[0] = it.world.m[3][0];
            c.carrier[1] = it.world.m[3][1];
            c.carrier[2] = it.world.m[3][2];
            c.carrierGroup = it.group;
        }
        if (!c.hasCarrier)
            continue;

        // The game doesn't draw a character in one go: its head, body and weapon pieces can each be a run of their
        // own, with other characters' pieces in between (dumps: player head, NPC body, player body). A run is the
        // carrier's if one of its pieces stands on the carrier's spot - a character's pieces share its origin to
        // within ~0.1 sideways, another character's body only gets that close walking right through it - or if
        // it's small and held (a weapon: its pieces sit at the hand, which swings up to ~0.8 out while walking,
        // 0.6 above the body's origin; a character's body pieces have theirs at its feet, like the carrier's).
        std::vector<uint8_t> linked(groups, 0);
        linked[c.carrierGroup] = 1;
        for (const ShadowItem& it : m_shadowItems) {
            if (it.cached || linked[it.group]) continue;
            float extent = std::max({it.boundsMax[0] - it.boundsMin[0], it.boundsMax[1] - it.boundsMin[1],
                                     it.boundsMax[2] - it.boundsMin[2]});
            if (extent >= 3.0f) continue;        // a floor or building whose origin happens to be there
            float dx = it.world.m[3][0] - c.carrier[0], dz = it.world.m[3][2] - c.carrier[2];
            float dy = it.world.m[3][1] - p.y;
            if (dx * dx + dz * dz < 0.25f * 0.25f && dy > -2.6f && dy < 0.3f)
                linked[it.group] = 1;
        }
        // The carrier's origin may be a head piece (sitting: right under the light); its feet are the lowest origin.
        for (const ShadowItem& it : m_shadowItems) {
            if (it.cached || !linked[it.group]) continue;
            float dx = it.world.m[3][0] - c.carrier[0], dz = it.world.m[3][2] - c.carrier[2];
            if (dx * dx + dz * dz < 0.25f * 0.25f)
                c.carrier[1] = std::min(c.carrier[1], it.world.m[3][1]);
        }
        for (uint32_t g = 0; g < groups; ++g) {
            const auto& b = groupBox[g];
            if (linked[g] || b[0] > b[3]) continue;
            float extent = std::max({b[3] - b[0], b[4] - b[1], b[5] - b[2]});
            float cx = 0.5f * (b[0] + b[3]) - c.carrier[0], cz = 0.5f * (b[2] + b[5]) - c.carrier[2];
            float cy = 0.5f * (b[1] + b[4]) - p.y;
            if (extent < 1.6f && cx * cx + cz * cz < 1.0f && cy > -2.6f && cy < 0.5f &&
                groupLowestOrigin[g] > c.carrier[1] + 0.3f)
                linked[g] = 1;
        }
        for (uint32_t g = 0; g < groups; ++g)
            if (linked[g]) c.carrierGroups.push_back(g);
    }
    // How much each light counts as a character's (its intensity setting, LightScale): the carrier search can miss
    // a frame in a crowd, and a light jumping between the two intensities for a frame flickers. It becomes a
    // character's within a few frames and stops being one over ~20. The same light last frame: same range, nearest.
    for (CapturedLight& c : m_lightsCur) {
        const CapturedLight* prev = nullptr;
        float prevBest = 1.0f;
        for (const CapturedLight& q : m_lightsPrev) {
            if (q.light.range != c.light.range || q.light.type != c.light.type) continue;
            float dx = q.light.position.x - c.light.position.x, dy = q.light.position.y - c.light.position.y,
                  dz = q.light.position.z - c.light.position.z, d = dx * dx + dy * dy + dz * dz;
            if (d < prevBest) { prevBest = d; prev = &q; }
        }
        float before = prev ? prev->carried : (c.hasCarrier ? 1.0f : 0.0f);
        c.carried = c.hasCarrier ? std::min(before + 0.34f, 1.0f) : std::max(before - 0.05f, 0.0f);
    }
}

// Diagnostics (log, once a second): how often the shadowed lights change - lights getting a cube, losing it, and
// losing it only to get it back within 3 frames (a shadow vanishing for a frame).
void Device::PointShadowChurn(const PointShadowLight* previous, uint32_t previousCount, size_t candidates,
                              const std::vector<const d3d::Light*>& chosen)
{
    uint32_t count = uint32_t(chosen.size());
    auto same = [](const float p[3], float range, const d3d::Light& l) {
        float dx = p[0] - l.position.x, dy = p[1] - l.position.y, dz = p[2] - l.position.z;
        return range == l.range && dx * dx + dy * dy + dz * dz < 0.5f * 0.5f;
    };
    for (uint32_t k = 0; k < count; ++k) {
        const d3d::Light& l = *chosen[k];
        bool was = false;
        for (uint32_t p = 0; p < previousCount && !was; ++p) was = same(previous[p].position, previous[p].range, l);
        if (was) continue;
        ++m_churnIn;
        for (const ChurnLeft& g : m_churnLeft)
            if (g.frame + 3 >= m_frameNumber && same(g.position, g.range, l)) { ++m_churnBack; break; }
    }
    for (uint32_t p = 0; p < previousCount; ++p) {
        if (previous[p].range <= 0.0f) continue;     // a free cube
        bool still = false;
        for (uint32_t k = 0; k < count && !still; ++k) still = same(previous[p].position, previous[p].range, *chosen[k]);
        if (still) continue;
        ++m_churnOut;
        m_churnLeft.push_back({{previous[p].position[0], previous[p].position[1], previous[p].position[2]},
                               previous[p].range, m_frameNumber});
    }
    m_churnLeft.erase(std::remove_if(m_churnLeft.begin(), m_churnLeft.end(),
                                     [&](const ChurnLeft& g) { return g.frame + 3 < m_frameNumber; }),
                      m_churnLeft.end());
    double now = SwayClock();
    if (now - m_churnTime >= 1.0) {
        if (m_churnIn || m_churnOut)
            Log("rvk: point shadows: %u of %zu candidate lights; last second %u got a cube, %u lost one, %u back "
                "within 3 frames", count, candidates, m_churnIn, m_churnOut, m_churnBack);
        m_churnIn = m_churnOut = m_churnBack = 0;
        m_churnTime = now;
    }
}

// Which cube (1-based; 0 = none) holds a frame light's shadow: lights are matched by their data, which the frame
// light list copies unchanged from the set the cubes were rendered for.
uint32_t Device::PointShadowLayer(const d3d::Light& l) const
{
    for (uint32_t i = 0; i < m_pointShadowCount; ++i) {
        const PointShadowLight& s = m_pointShadowLights[i];
        if (s.position[0] == l.position.x && s.position[1] == l.position.y && s.position[2] == l.position.z &&
            s.range == l.range)
            return i + 1;
    }
    return 0;
}

// End of frame, after the sun's pass: cube maps for the point / spot lights of this frame nearest the camera.
void Device::RenderPointShadowMaps(VkCommandBuffer cmd)
{
    PointShadowLight previous[kMaxPointShadows];   // the lights shadowed last frame: kept unless clearly beaten
    uint32_t previousCount = m_pointShadowCount;
    double now = SwayClock();
    float fadeStep = m_pointShadowFadeIn > 0.0 ? float(std::clamp((now - m_pointShadowClock) / m_pointShadowFadeIn, 0.0, 1.0)) : 1.0f;
    m_pointShadowClock = now;
    std::copy(m_pointShadowLights, m_pointShadowLights + kMaxPointShadows, previous);
    m_pointShadowCount = 0;
    m_pointShadowDraws = 0;
    if (!m_pointShadows || !m_lightOverride || !m_pixelLighting || !m_frameViewProjValid || m_shadowItems.empty())
        return;
    double pointScanMs = 0.0, pointDrawMs = 0.0;    // profile: caster selection vs recording the faces' draws

    // Candidates: lights whose sphere of influence reaches into the view, nearest the camera first - by the distance
    // to the light itself. (Not to its sphere: every light whose range covers the camera would tie at 0, and big
    // floodlights far away, whose shadows are coarse and faint here, took the slots of the lights around the
    // player depending on the camera angle.) A light shadowed last frame keeps its slot unless another is clearly
    // nearer, so lights at similar distances don't swap cubes back and forth.
    struct Candidate { float key; uint32_t index; float fade; int slot; };
    std::vector<Candidate> candidates;
    for (uint32_t i = 0; i < m_lightsCur.size(); ++i) {
        const d3d::Light& l = m_lightsCur[i].light;
        float lum = l.diffuse.r * 0.3f + l.diffuse.g * 0.59f + l.diffuse.b * 0.11f;
        if (l.range < 1.0f || lum < 0.05f)
            continue;
        float mn[3] = {l.position.x - l.range, l.position.y - l.range, l.position.z - l.range};
        float mx[3] = {l.position.x + l.range, l.position.y + l.range, l.position.z + l.range};
        if (BoxInClip(mn, mx, m_frameViewProj, true) == -1)
            continue;
        float dx = l.position.x - m_frameEye[0], dy = l.position.y - m_frameEye[1], dz = l.position.z - m_frameEye[2];
        float key = std::sqrt(dx * dx + dy * dy + dz * dz), fade = -1.0f;
        int slot = -1;
        for (uint32_t p = 0; p < previousCount; ++p) {
            const PointShadowLight& q = previous[p];
            float px = q.position[0] - l.position.x, py = q.position[1] - l.position.y, pz = q.position[2] - l.position.z;
            if (q.range > 0.0f && q.range == l.range && px * px + py * py + pz * pz < 0.5f * 0.5f) {   // same light
                key *= 0.8f;
                fade = q.fade;
                slot = int(p);
                break;
            }
        }
        candidates.push_back({key, i, fade, slot});
    }
    // The cubes' lights: the nearest m_pointShadows candidates are wanted. A light keeps its cube (slot) while it has
    // one; one no longer wanted fades its shadow out (still rendered) before giving the cube up, and a newly wanted
    // light takes a free cube and fades in - a shadow never disappears from one frame to the next, as it did when
    // lights at similar distances swapped places (crowds: every character carries a light) while moving.
    uint32_t limit = std::min<uint32_t>(m_pointShadows, m_cubeCount);
    uint32_t wanted = std::min<uint32_t>(uint32_t(candidates.size()), limit);
    std::partial_sort(candidates.begin(), candidates.begin() + wanted, candidates.end(),
                      [](const Candidate& a, const Candidate& b) { return a.key < b.key; });
    struct Slot { int candidate = -1; float fade = 0.0f; };
    Slot slots[kMaxPointShadows];
    for (uint32_t c = 0; c < candidates.size(); ++c) {
        const Candidate& cand = candidates[c];
        if (cand.slot < 0 || uint32_t(cand.slot) >= limit) continue;
        float fade = c < wanted ? std::min(1.0f, std::max(cand.fade, 0.0f) + fadeStep)
                                : (m_pointShadowFadeIn <= 0.0 ? 0.0f : cand.fade - fadeStep);
        if (fade > 0.0f || c < wanted)
            slots[cand.slot] = {int(c), fade};
    }
    for (uint32_t c = 0; c < wanted; ++c) {
        if (candidates[c].slot >= 0 && uint32_t(candidates[c].slot) < limit) continue;
        for (uint32_t k = 0; k < limit; ++k)
            if (slots[k].candidate < 0) {
                slots[k] = {int(c), m_pointShadowFadeIn <= 0.0 ? 1.0f : 0.0f};
                break;
            }
    }
    uint32_t count = 0;
    std::vector<const d3d::Light*> chosen;
    for (uint32_t k = 0; k < limit; ++k)
        if (slots[k].candidate >= 0) {
            count = k + 1;
            chosen.push_back(&m_lightsCur[candidates[slots[k].candidate].index].light);
        }
    PointShadowChurn(previous, previousCount, candidates.size(), chosen);
    if (count == 0)
        return;

    VkImageSubresourceRange all{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, m_cubeCount * 6};
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    b.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    b.oldLayout = m_cubeLayout;
    b.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = m_cubeImage;
    b.subresourceRange = all;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
    BindShadowRecords(cmd);                       // M4: the casters' records (binding 1)

    std::vector<uint32_t> inRange;               // casters within the current light's range
    ShadowBind bind;
    bool stateSet = false;                       // viewport, scissor and depth bias for the cube faces
    for (uint32_t k = 0; k < count; ++k) {
        PointShadowLight& s = m_pointShadowLights[k];
        if (slots[k].candidate < 0) {            // a free cube between used ones
            s = {};
            continue;
        }
        const d3d::Light& l = m_lightsCur[candidates[slots[k].candidate].index].light;
        float pos[3] = {l.position.x, l.position.y, l.position.z};
        float fade = slots[k].fade;
        // A cube still holding the same light where it was keeps last frame's map every other frame (half the cubes
        // each frame) - if only still things are around it. Moving lights (carried) render every frame.
        // Not while anything animated is in its range: a character's body moves every frame, and against last frame's
        // map its own limbs and torso shadow it in the wrong places - dark patches flickering on bodies in crowds.
        double scanStart = ProfileCpu();
        bool animatedNear = false;
        for (uint32_t i : m_animatedItems) {     // only animated casters can answer yes (m_animatedItems)
            const ShadowItem& it = m_shadowItems[i];
            if (BoxDistance2(it.boundsMin, it.boundsMax, pos) <= l.range * l.range) {
                animatedNear = true;
                break;
            }
        }
        if (!animatedNear && k < previousCount && previous[k].range == l.range && ((k + m_frameNumber) & 1)) {
            float dx = previous[k].position[0] - pos[0], dy = previous[k].position[1] - pos[1], dz = previous[k].position[2] - pos[2];
            if (dx * dx + dy * dy + dz * dz < 1e-4f) {
                // This frame's position (the game's lamps wobble in the last bits): PointShadowLayer matches exactly.
                std::memcpy(s.position, pos, sizeof(pos));
                s.fade = fade;
                pointScanMs += ProfileCpu() - scanStart;
                continue;
            }
        }
        inRange.clear();
        // Batch-key order (CollectShadowItems): longer batches per face. The boxes come packed in that order
        // (m_shadowOrderBox): this scan runs per light over every caster.
        const float* box = m_shadowOrderBox.data();
        for (size_t n = 0; n < m_shadowOrder.size(); ++n, box += 6) {
            float d2 = BoxDistance2(box, box + 3, pos);
            if (d2 > l.range * l.range)
                continue;
            uint32_t i = m_shadowOrder[n];
            // A lamp's housing (small, around the light) would shadow everything around it. Characters always
            // cast, the one carrying the light too (telling its pieces apart from others' proved unreliable;
            // FindCarriers now only serves CarriedLight). Big objects (buildings, platforms with their pillars) cast
            // even when their box contains the light; the near plane clips geometry right at the light.
            float extent = std::max({box[3] - box[0], box[4] - box[1], box[5] - box[2]});
            bool housing = d2 == 0.0f && extent < 1.5f;              // smaller than a character
            if (housing)
                continue;
            inRange.push_back(i);
        }
        pointScanMs += ProfileCpu() - scanStart;
        double drawStart = ProfileCpu();
        for (int face = 0; face < 6; ++face) {
            VkRenderingAttachmentInfo depthAtt{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            depthAtt.imageView = m_cubeFaceViews[k * 6 + face];
            depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            depthAtt.clearValue.depthStencil = {1.0f, 0};
            VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
            ri.renderArea = {{0, 0}, {m_pointShadowSize, m_pointShadowSize}};
            ri.layerCount = 1;
            ri.pDepthAttachment = &depthAtt;
            vkCmdBeginRendering(cmd, &ri);
            if (!stateSet) {                     // the first face drawn this frame (cubes may skip a frame)
                stateSet = true;
                VkViewport viewport{0.0f, 0.0f, float(m_pointShadowSize), float(m_pointShadowSize), 0.0f, 1.0f};
                VkRect2D scissor{{0, 0}, {m_pointShadowSize, m_pointShadowSize}};
                vkCmdSetViewport(cmd, 0, 1, &viewport);
                vkCmdSetScissor(cmd, 0, 1, &scissor);
                vkCmdSetDepthBias(cmd, 1.0f, 0.0f, 1.5f);
            }
            d3d::Matrix vp = FaceViewProj(face, pos, kPointShadowNear, l.range);
            ShadowPassMatrix(cmd, vp);
            for (uint32_t i : inRange) {
                ShadowItem& it = m_shadowItems[i];
                if (BoxInClip(it.boundsMin, it.boundsMax, vp, true) == -1)
                    continue;
                DrawShadowItem(cmd, bind, it);
                ++m_pointShadowDraws;
            }
            FlushShadowGroup(cmd);               // this face's batch (M4)
            vkCmdEndRendering(cmd);
        }
        pointDrawMs += ProfileCpu() - drawStart;
        std::memcpy(s.position, pos, sizeof(pos));
        s.range = l.range;
        s.fade = fade;                           // newly shadowed lights fade their shadow in instead of popping in
    }
    m_pointShadowCount = count;
    ProfileCpuAddMs("point scan", pointScanMs);
    ProfileCpuAddMs("point draw", pointDrawMs);

    b.srcStageMask = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    b.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier2(cmd, &dep);
    m_cubeLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

}  // namespace rvk
