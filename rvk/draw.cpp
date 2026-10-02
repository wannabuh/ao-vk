// D3D7 state, Clear, and drawing.
#include "internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace rvk {

using namespace vk;
using namespace detail;

namespace {

void ArgbToFloat(uint32_t c, float out[4])
{
    out[0] = ((c >> 16) & 0xFF) / 255.0f;
    out[1] = ((c >> 8) & 0xFF) / 255.0f;
    out[2] = (c & 0xFF) / 255.0f;
    out[3] = (c >> 24) / 255.0f;
}

void Copy4(float out[4], const d3d::Color& c)
{
    out[0] = c.r; out[1] = c.g; out[2] = c.b; out[3] = c.a;
}

void FillGpuLight(const d3d::Light& l, float cosHalfTheta, float cosHalfPhi, GpuLight& g)
{
    Copy4(g.diffuse, l.diffuse);
    Copy4(g.specular, l.specular);
    Copy4(g.ambient, l.ambient);
    g.position[0] = l.position.x; g.position[1] = l.position.y; g.position[2] = l.position.z;
    g.position[3] = float(l.type);
    g.direction[0] = l.direction.x; g.direction[1] = l.direction.y; g.direction[2] = l.direction.z;
    g.direction[3] = l.range;
    g.atten[0] = l.attenuation0; g.atten[1] = l.attenuation1; g.atten[2] = l.attenuation2; g.atten[3] = l.falloff;
    g.spot[0] = cosHalfTheta;
    g.spot[1] = cosHalfPhi;
    g.spot[2] = g.spot[3] = 0.0f;
}

float AsFloat(uint32_t v)
{
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

constexpr uint32_t kBlendOverbright2x = 0x10000;   // pseudo D3D blend value: see Device::Overbright2x

VkBlendFactor BlendFactor(uint32_t b)
{
    switch (b) {
    case d3d::BLEND_ZERO: return VK_BLEND_FACTOR_ZERO;
    case d3d::BLEND_ONE: return VK_BLEND_FACTOR_ONE;
    case d3d::BLEND_SRCCOLOR: return VK_BLEND_FACTOR_SRC_COLOR;
    case d3d::BLEND_INVSRCCOLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case d3d::BLEND_SRCALPHA: return VK_BLEND_FACTOR_SRC_ALPHA;
    case d3d::BLEND_INVSRCALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case d3d::BLEND_DESTALPHA: return VK_BLEND_FACTOR_DST_ALPHA;
    case d3d::BLEND_INVDESTALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case d3d::BLEND_DESTCOLOR: return VK_BLEND_FACTOR_DST_COLOR;
    case d3d::BLEND_INVDESTCOLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case d3d::BLEND_SRCALPHASAT: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default: return VK_BLEND_FACTOR_ONE;
    }
}

VkCompareOp CompareOp(uint32_t c)
{
    switch (c) {
    case d3d::CMP_NEVER: return VK_COMPARE_OP_NEVER;
    case d3d::CMP_LESS: return VK_COMPARE_OP_LESS;
    case d3d::CMP_EQUAL: return VK_COMPARE_OP_EQUAL;
    case d3d::CMP_LESSEQUAL: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case d3d::CMP_GREATER: return VK_COMPARE_OP_GREATER;
    case d3d::CMP_NOTEQUAL: return VK_COMPARE_OP_NOT_EQUAL;
    case d3d::CMP_GREATEREQUAL: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}

VkPrimitiveTopology Topology(uint32_t p)
{
    switch (p) {
    case d3d::PointList: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case d3d::LineList: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case d3d::LineStrip: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case d3d::TriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case d3d::TriangleFan: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

uint32_t TopologyClass(uint32_t p)
{
    return p == d3d::PointList ? 0 : (p == d3d::LineList || p == d3d::LineStrip) ? 1 : 2;
}

}  // namespace

VkPrimitiveTopology detail::TopologyOf(uint32_t p) { return Topology(p); }
uint32_t detail::TopologyClassOf(uint32_t p) { return TopologyClass(p); }

// ---------------------------------------------------------------------------------------------------
// State

void Device::SetRenderState(uint32_t state, uint32_t value)
{
    if (state < m_rs.size() && m_rs[state] != value) {
        m_rs[state] = value;
        m_constantsDirty = true;
    }
}

void Device::SetTextureStageState(uint32_t stage, uint32_t type, uint32_t value)
{
    if (stage >= 2 || type >= d3d::TSS_COUNT)
        return;
    if (m_tss[stage][type] == value && type != d3d::TSS_ADDRESS)
        return;
    m_tss[stage][type] = value;
    if (type == d3d::TSS_ADDRESS)                      // ADDRESS sets both U and V
        m_tss[stage][d3d::TSS_ADDRESSU] = m_tss[stage][d3d::TSS_ADDRESSV] = value;
    m_constantsDirty = true;
}

void Device::SetTransform(uint32_t type, const d3d::Matrix& m)
{
    // The world matrix has its own per-draw block; the others live in the cached constant block.
    d3d::Matrix* target = nullptr;
    switch (type) {
    case d3d::World: m_world = m; return;
    case d3d::View: target = &m_view; break;
    case d3d::Projection: target = &m_proj; break;
    case d3d::Texture0: target = &m_texMatrix[0]; break;
    case d3d::Texture1: target = &m_texMatrix[1]; break;
    default: return;
    }
    if (std::memcmp(target, &m, sizeof(m)) != 0) {
        *target = m;
        m_constantsDirty = true;
    }
}

void Device::SetMaterial(const d3d::Material& m)
{
    if (std::memcmp(&m_material, &m, sizeof(m)) != 0) {
        m_material = m;
        m_constantsDirty = true;
    }
}

void Device::SetLight(uint32_t index, const d3d::Light& light)
{
    if (index >= m_lights.size())
        m_lights.resize(index + 1);
    LightSlot& slot = m_lights[index];
    if (std::memcmp(&slot.light, &light, sizeof(light)) != 0)
        ++slot.version;
    slot.light = light;
    slot.cosHalfTheta = std::cos(light.theta * 0.5f);
    slot.cosHalfPhi = std::cos(light.phi * 0.5f);
    if (slot.enabled) {
        m_constantsDirty = true;
        CaptureLight(slot);
    }
}

void Device::LightEnable(uint32_t index, bool enable)
{
    if (index >= m_lights.size())
        m_lights.resize(index + 1);
    LightSlot& slot = m_lights[index];
    if (slot.enabled != enable) {
        slot.enabled = enable;
        m_constantsDirty = true;
    }
    if (enable)
        CaptureLight(slot);
}

// Collects the point / spot lights the game uses during a frame, by what they are rather than by slot: Randy
// reassigns light slots while it draws (more so while the camera moves), so a slot's content at any one moment
// says little. Same colours / range / attenuation at nearly the same place = the same light (its latest data).
void Device::CaptureLight(LightSlot& slot)
{
    if (slot.capturedFrame == m_frameNumber && slot.capturedVersion == slot.version)
        return;
    slot.capturedFrame = m_frameNumber;
    slot.capturedVersion = slot.version;
    const d3d::Light& l = slot.light;
    if (l.type == d3d::LIGHT_DIRECTIONAL)
        CaptureSun(l);
    if (l.type == d3d::LIGHT_DIRECTIONAL || l.range <= 0.0f)
        return;
    auto sameKind = [](const d3d::Light& a, const d3d::Light& b) {
        d3d::Light x = a, y = b;
        x.position = y.position = {};
        return std::memcmp(&x, &y, sizeof(x)) == 0;
    };
    for (CapturedLight& c : m_lightsCur) {
        float dx = c.light.position.x - l.position.x, dy = c.light.position.y - l.position.y,
              dz = c.light.position.z - l.position.z;
        if (dx * dx + dy * dy + dz * dz < 0.25f && sameKind(c.light, l)) {
            c.light = l;
            return;
        }
    }
    if (m_lightsCur.size() < 512)
        m_lightsCur.push_back({l, slot.cosHalfTheta, slot.cosHalfPhi});
}

// The frame's camera (for choosing lights and placing the shadow map): the view at the frame's first lit draw
// or shadow caster, i.e. the world - not that of later draws under other views (sky, 3D interface elements).
void Device::UpdateFrameEye()
{
    if (m_frameEyeValid)
        return;
    const auto& v = m_view.m;
    for (int i = 0; i < 3; ++i) {
        m_frameEye[i] = -(v[3][0] * v[i][0] + v[3][1] * v[i][1] + v[3][2] * v[i][2]);
        m_frameForward[i] = v[i][2];
    }
    m_frameEyeValid = true;
}

// The point / spot lights the game used during the previous frame (complete, so every draw of this frame gets
// the same list), nearest first by the distance from the camera to their sphere of influence.
VkDeviceSize Device::WriteFrameLights()
{
    void* cpu;
    VkDeviceSize offset = Allocate(sizeof(FrameLights), m_props.limits.minUniformBufferOffsetAlignment, &cpu);
    m_frameLightsOffset = offset;
    m_frameLightsGeneration = m_ringGeneration;
    m_frameLightsDirty = false;
    FillFrameLights(static_cast<FrameLights*>(cpu), m_dumpFile != nullptr);
    return offset;
}

void Device::FillFrameLights(FrameLights* fl, bool dump)
{
    // One camera per frame: the view at the frame's first lit draw (the world), not that of later draws
    // under other views (sky, 3D interface elements).
    UpdateFrameEye();
    const float* eye = m_frameEye;
    struct Candidate { float key; uint32_t index; };
    Candidate candidates[512];
    uint32_t count = 0;
    for (uint32_t i = 0; i < m_lightsPrev.size(); ++i) {
        const d3d::Light& l = m_lightsPrev[i].light;
        float dx = l.position.x - eye[0], dy = l.position.y - eye[1], dz = l.position.z - eye[2];
        candidates[count++] = {std::max(0.0f, std::sqrt(dx * dx + dy * dy + dz * dz) - l.range), i};
    }
    uint32_t used = std::min(count, kFrameLights);
    std::partial_sort(candidates, candidates + used, candidates + count,
                      [](const Candidate& a, const Candidate& b) { return a.key < b.key; });
    fl->info[0] = used;
    fl->info[1] = fl->info[2] = fl->info[3] = 0;
    for (uint32_t i = 0; i < kShadowCascades; ++i) {
        fl->shadowViewProj[i] = m_cascadeViewProj[i];
        fl->cascadeTexel[i] = m_cascadeTexel[i];
        fl->cascadeDepth[i] = m_cascadeDepth[i];
    }
    // Night: the sun's light on flat ground (smoothed) below a tenth of full day.
    float night = 1.0f - std::clamp(m_daylight / 0.35f, 0.0f, 1.0f);
    fl->effects[0] = m_leafLight;
    fl->effects[1] = m_hdr ? m_nightGlow * night : 0.0f;
    fl->effects[2] = m_sunSoftness;
    fl->effects[3] = 0.0f;
    Wind(fl->wind);
    fl->taa[0] = TaaActive() ? m_taaJitter[0] : 0.0f;
    fl->taa[1] = TaaActive() ? m_taaJitter[1] : 0.0f;
    fl->taa[2] = FrameNoise();                   // noise patterns move on each frame (averaged by the TAA)
    fl->taa[3] = 0.0f;
    fl->shadowParams[0] = m_shadowValid ? 1.0f : 0.0f;
    fl->shadowParams[1] = m_shadowStrength;
    fl->shadowParams[2] = float(m_cascadeCount);
    fl->shadowParams[3] = PointShadowStrength();
    // The sun the shadow map was drawn with (shadows lag a frame), or without shadows this frame's.
    const float* sunDir = m_shadowValid ? m_shadowSunDir : m_frameSunDir;
    fl->sunDir[0] = sunDir[0]; fl->sunDir[1] = sunDir[1]; fl->sunDir[2] = sunDir[2];
    fl->sunDir[3] = m_hdr ? m_hdrHeadroom : m_lightHeadroom;
    for (int i = 0; i < 3; ++i) fl->sunColor[i] = m_shadowValid ? m_shadowSunColor[i] : m_frameSunColor[i];
    fl->sunColor[3] = 0.0f;
    fl->prevViewProj = m_prevViewProj;           // the world camera last frame (motion vectors)
    m_frameLightIndices.clear();
    for (uint32_t k = 0; k < used; ++k) {
        const CapturedLight& c = m_lightsPrev[candidates[k].index];
        m_frameLightIndices.push_back(candidates[k].index);
        FillGpuLight(c.light, c.cosHalfTheta, c.cosHalfPhi, fl->lights[k]);
        uint32_t cube = PointShadowLayer(c.light);
        fl->lights[k].spot[2] = float(cube);                        // its cube shadow map + 1, 0 = none
        fl->lights[k].spot[3] = cube ? m_pointShadowLights[cube - 1].fade : 0.0f;   // how far its shadow faded in
    }
    if (dump) {
        std::fprintf(m_dumpFile, "FL at draw %u: %u of %u lights captured last frame, eye (%.1f %.1f %.1f):", m_dumpDraw,
                     used, count, eye[0], eye[1], eye[2]);
        for (uint32_t k = 0; k < used; ++k) {
            const d3d::Light& l = m_lightsPrev[candidates[k].index].light;
            std::fprintf(m_dumpFile, " (%.1f %.1f %.1f r%.1f)", l.position.x, l.position.y, l.position.z, l.range);
        }
        std::fprintf(m_dumpFile, "\n");
    }
}

// Identity of a ground chunk within a frame: its sizes and a sample of its vertices (base and lighting passes draw
// the same vertices).
uint64_t Device::TerrainChunkKey(const void* vertices, uint32_t vertexCount, uint32_t stride, uint32_t indexCount)
{
    uint64_t h = 1469598103934665603ull ^ (uint64_t(vertexCount) << 32) ^ indexCount;
    const uint8_t* v = static_cast<const uint8_t*>(vertices);
    uint32_t step = vertexCount > 32 ? vertexCount / 32 : 1;
    for (uint32_t i = 0; i < vertexCount; i += step)
        for (uint32_t b = 0; b < 12; ++b)
            h = (h ^ v[size_t(i) * stride + b]) * 1099511628211ull;
    return h;
}

// Motion vectors (per-object motion blur): solid 3D geometry of the world camera in the HDR scene, given a camera last
// frame to compare with.
bool Device::MotionVectorDraw(uint32_t fvf) const
{
    // Object motion: for the per-object motion blur, and for the temporal anti-aliasing's history.
    return ((m_motionMode == 1 && m_motionBlur > 0.0f) || TaaActive()) && m_prevViewProjValid && m_target == m_scene &&
           m_aoProjValid &&
           (fvf & d3d::FVF_POSITION_MASK) != d3d::FVF_XYZRHW && m_rs[d3d::RS_ZENABLE] && m_rs[d3d::RS_ZWRITEENABLE] &&
           std::memcmp(&m_proj, &m_aoProj, sizeof(m_proj)) == 0 && std::memcmp(&m_view, &m_aoView, sizeof(m_view)) == 0;
}

// A mesh's identity across frames, independent of where it is and of its (CPU-skinned) vertices: format, sizes,
// a sample of its indices, its texture.
uint64_t Device::MotionKey(uint32_t primitive, uint32_t fvf, uint32_t vertexCount, const uint16_t* indices,
                           uint32_t indexCount) const
{
    uint64_t h = 1469598103934665603ull ^ (uint64_t(fvf) << 40) ^ (uint64_t(primitive) << 32) ^ vertexCount;
    h = (h ^ indexCount) * 1099511628211ull;
    if (indices) {
        uint32_t step = indexCount > 32 ? indexCount / 32 : 1;
        for (uint32_t i = 0; i < indexCount; i += step)
            h = (h ^ indices[i]) * 1099511628211ull;
    }
    uintptr_t texture = reinterpret_cast<uintptr_t>(m_textures[0]);
    return (h ^ uint64_t(texture)) * 1099511628211ull;
}

// An additive effect drawn into the HDR scene (light halos, spells, fire: blend ONE or SRCALPHA onto ONE) feeds the
// glow, and so the bloom. Not the sky's additive layers (clouds, stars), drawn at infinity with depth test ALWAYS.
// A pre-transformed draw that belongs to the interface, not the 3D scene. The water (VisualLiquid_t) is drawn from
// vertices the game transforms with ProcessVertices: pre-transformed too, but with a specular colour (FVF 0x1C4), in the
// middle of the scene - taking it for the interface ended the scene early wherever water was in view.
bool Device::IsInterfaceDraw(uint32_t fvf)
{
    return (fvf & d3d::FVF_POSITION_MASK) == d3d::FVF_XYZRHW && !IsWater(fvf);
}

// Plants that sway in the wind: small (0.2 - 3 units tall), lit, drawn into the scene with a cut-out texture (the
// vertex shader keeps those whose texture is mostly holes - leaves, grass), and static: their vertices are the same as
// last frame (a character's hair or cloak is CPU-skinned, its vertices change every frame). out: DrawTransform sway.
bool Device::SwayParams(uint32_t fvf, uint32_t stride, const void* vertices, uint32_t vertexCount, float out[4])
{
    if (m_sway <= 0.0f || m_target != m_scene || !m_textures[0] || !m_rs[d3d::RS_LIGHTING] || m_drawIsLabel ||
        (fvf & d3d::FVF_POSITION_MASK) != d3d::FVF_XYZ || IsTerrain(fvf) || !m_rs[d3d::RS_ZWRITEENABLE] ||
        !(m_rs[d3d::RS_ALPHATESTENABLE] || m_rs[d3d::RS_ALPHABLENDENABLE]) || vertexCount < 3 || vertexCount > 4096)
        return false;
    const uint8_t* v = static_cast<const uint8_t*>(vertices);
    float minY = 1e30f, maxY = -1e30f;
    uint64_t h = 1469598103934665603ull;
    uint32_t sampleStep = vertexCount > 16 ? vertexCount / 16 : 1;
    for (uint32_t i = 0; i < vertexCount; ++i) {
        float y;
        std::memcpy(&y, v + size_t(i) * stride + 4, 4);
        minY = std::min(minY, y);
        maxY = std::max(maxY, y);
        if (i % sampleStep == 0)
            for (uint32_t b = 0; b < 12; ++b) h = (h ^ v[size_t(i) * stride + b]) * 1099511628211ull;
    }
    const auto& w = m_world.m;
    float scaleY = std::sqrt(w[1][0] * w[1][0] + w[1][1] * w[1][1] + w[1][2] * w[1][2]);
    float height = (maxY - minY) * scaleY;
    if (height < 0.2f || height > 3.0f)
        return false;
    // Same mesh in the same place last frame: identity from its size, texture and position (rounded).
    uint64_t key = (uint64_t(vertexCount) << 40) ^ uint64_t(reinterpret_cast<uintptr_t>(m_textures[0]));
    for (int j = 0; j < 3; ++j) key = (key ^ uint64_t(int64_t(std::floor(w[3][j] * 4.0f)))) * 1099511628211ull;
    SwayEntry& e = m_swayStatic[key];
    e.stable = e.frame + 1 == m_frameNumber && e.positions == h ? e.stable + 1 : (e.frame == m_frameNumber ? e.stable : 0);
    e.positions = h;
    e.frame = m_frameNumber;
    if (e.stable < 2)
        return false;
    out[0] = minY;
    out[1] = 1.0f / (maxY - minY);
    out[2] = 0.06f * m_sway * height;
    out[3] = 1.0f;
    return true;
}

// The water doesn't write depth; the reflections need its surface (the depth buffer gives their position and
// normal), as do the ambient occlusion and the volumetric light, which then end at the water. Floating text (the
// "Entering ..." messages) comes out of ProcessVertices in the same format: drawn over everything (depth test ALWAYS),
// it must not write depth - the depth of field's focus would land on it.
bool Device::WaterWritesDepth(uint32_t fvf) const
{
    return IsWater(fvf) && m_rs[d3d::RS_ZENABLE] && m_rs[d3d::RS_ZFUNC] != d3d::CMP_ALWAYS && m_target == m_scene &&
           m_ssr > 0.0f;
}

// Wind: a slowly turning direction; the vertex shaders add waves and gusts (sway.glsl).
void Device::Wind(float out[4]) const
{
    double t = std::fmod(SwayClock(), 3600.0);
    float angle = 0.6f + 0.4f * float(std::sin(t * 0.013));
    out[0] = std::cos(angle);
    out[1] = std::sin(angle);
    out[2] = float(t);
    out[3] = m_sway;
}

// The frame lights (light override) whose sphere reaches the draw's world bounding box, as a 64-bit mask for the
// shader - instead of every pixel trying all of them. All set when the box is unknown (external geometry).
void Device::FrameLightMask(uint32_t fvf, uint32_t stride, const void* vertices, uint32_t vertexCount, uint32_t out[4])
{
    out[0] = out[1] = out[2] = out[3] = 0;
    if (!m_lightOverride || !m_pixelLighting || !m_rs[d3d::RS_LIGHTING] ||
        (fvf & d3d::FVF_POSITION_MASK) == d3d::FVF_XYZRHW || m_frameLightIndices.empty())
        return;
    uint32_t count = std::min<uint32_t>(uint32_t(m_frameLightIndices.size()), 64);
    if (m_external || !vertices || !vertexCount) {
        out[0] = count >= 32 ? ~0u : (1u << count) - 1u;
        out[1] = count > 32 ? (count >= 64 ? ~0u : (1u << (count - 32)) - 1u) : 0u;
        return;
    }
    float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
    const uint8_t* v = static_cast<const uint8_t*>(vertices);
    for (uint32_t i = 0; i < vertexCount; ++i) {
        float p[3];
        std::memcpy(p, v + size_t(i) * stride, 12);
        for (int j = 0; j < 3; ++j) { mn[j] = std::min(mn[j], p[j]); mx[j] = std::max(mx[j], p[j]); }
    }
    // The model box through the world matrix (centre and half extents).
    float wmn[3], wmx[3];
    const auto& m = m_world.m;
    for (int j = 0; j < 3; ++j) {
        float c = m[3][j], e = 0.0f;
        for (int i = 0; i < 3; ++i) {
            c += 0.5f * (mn[i] + mx[i]) * m[i][j];
            e += 0.5f * (mx[i] - mn[i]) * std::fabs(m[i][j]);
        }
        wmn[j] = c - e;
        wmx[j] = c + e;
    }
    for (uint32_t k = 0; k < count; ++k) {
        const d3d::Light& l = m_lightsPrev[m_frameLightIndices[k]].light;
        float p[3] = {l.position.x, l.position.y, l.position.z}, d2 = 0.0f;
        for (int j = 0; j < 3; ++j) {
            float d = std::max({wmn[j] - p[j], 0.0f, p[j] - wmx[j]});
            d2 += d * d;
        }
        if (d2 <= l.range * l.range)
            out[k >> 5] |= 1u << (k & 31);
    }
}

bool Device::IsWater(uint32_t fvf)
{
    return fvf == (d3d::FVF_XYZRHW | d3d::FVF_DIFFUSE | d3d::FVF_SPECULAR | (1u << d3d::FVF_TEXCOUNT_SHIFT));
}

bool Device::GlowDraw(uint32_t fvf) const
{
    if (m_effectGlow <= 0.0f || m_target != m_scene || !m_rs[d3d::RS_ALPHABLENDENABLE] ||
        (fvf & d3d::FVF_POSITION_MASK) == d3d::FVF_XYZRHW || m_rs[d3d::RS_ZFUNC] == d3d::CMP_ALWAYS)
        return false;
    uint32_t src = m_rs[d3d::RS_SRCBLEND], dst = m_rs[d3d::RS_DESTBLEND];
    return dst == d3d::BLEND_ONE && (src == d3d::BLEND_ONE || src == d3d::BLEND_SRCALPHA);
}

// A multiplying pass (the ground's lightmap + lights) lit by the frame lights with headroom: it outputs half its
// colour and is blended at 2x, so the lights can brighten the surface under it beyond the texture.
bool Device::Overbright2x(uint32_t fvf) const
{
    return m_lightHeadroom > 1.0f && m_target->m_format != Format::RGBA16F &&   // a float target takes > 1 as is
           m_lightOverride && m_pixelLighting && m_rs[d3d::RS_LIGHTING] &&
           (fvf & d3d::FVF_POSITION_MASK) != d3d::FVF_XYZRHW && IsMultiplyPass();
}

// A character with its own light (at head height): unless m_carrierLit, the light stays off the character itself,
// which it would otherwise light from the inside out. Its parts are those of the carrier found last frame
// (FindCarriers), as for the point shadows. Returns frame light index + 1.
uint32_t Device::CarriedLight(uint32_t fvf, const void* vertices, uint32_t vertexCount, uint32_t stride) const
{
    if (m_carrierLit || !m_lightOverride || !m_pixelLighting || !m_rs[d3d::RS_LIGHTING] || m_target != m_main ||
        (fvf & d3d::FVF_POSITION_MASK) == d3d::FVF_XYZRHW || !WorldCamera())
        return 0;
    bool anyCarrier = false;
    for (uint32_t k = 0; k < m_frameLightIndices.size() && !anyCarrier; ++k)
        anyCarrier = m_lightsPrev[m_frameLightIndices[k]].hasCarrier;
    if (!anyCarrier)
        return 0;
    const auto& w = m_world.m;
    float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
    const uint8_t* v = static_cast<const uint8_t*>(vertices);
    for (uint32_t i = 0; i < vertexCount; ++i) {
        float q[3];
        std::memcpy(q, v + size_t(i) * stride, sizeof(q));
        for (int j = 0; j < 3; ++j) {
            float wq = q[0] * w[0][j] + q[1] * w[1][j] + q[2] * w[2][j] + w[3][j];
            mn[j] = std::min(mn[j], wq);
            mx[j] = std::max(mx[j], wq);
        }
    }
    for (uint32_t k = 0; k < m_frameLightIndices.size(); ++k)
        if (IsCarrierPart(m_lightsPrev[m_frameLightIndices[k]], m_world, mn, mx))
            return k + 1;
    return 0;
}

void Device::SetTexture(uint32_t stage, Texture* texture)
{
    if (texture && !texture->m_view)            // failed creation: draw untextured
        texture = nullptr;
    if (stage < 2)
        m_textures[stage] = texture == m_target ? nullptr : texture;
}

void Device::SetViewport(const d3d::Viewport& vp)
{
    if (std::memcmp(&m_viewport, &vp, sizeof(vp)) != 0) {
        m_viewport = vp;
        m_constantsDirty = true;
    }
}

void Device::Clear(uint32_t flags, uint32_t argb, float z)
{
    Clear(0, nullptr, flags, argb, z);
}

void Device::Clear(uint32_t count, const Rect* rects, uint32_t flags, uint32_t argb, float z)
{
    if (!m_inFrame)
        return;
    VkClearAttachment att[2];
    uint32_t n = 0;
    if (flags & d3d::CLEAR_TARGET) {
        att[n] = {VK_IMAGE_ASPECT_COLOR_BIT, 0, {}};
        ArgbToFloat(argb, att[n].clearValue.color.float32);
        ++n;
    }
    if (flags & d3d::CLEAR_ZBUFFER) {
        att[n] = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, {}};
        att[n].clearValue.depthStencil = {z, 0};
        ++n;
    }
    if (!n)
        return;
    // D3D clears the given rectangles (or the whole viewport), always clipped to the viewport.
    int32_t vx0 = int32_t(std::min(m_viewport.x, m_target->m_width));
    int32_t vy0 = int32_t(std::min(m_viewport.y, m_target->m_height));
    int32_t vx1 = int32_t(std::min(m_viewport.x + m_viewport.width, m_target->m_width));
    int32_t vy1 = int32_t(std::min(m_viewport.y + m_viewport.height, m_target->m_height));
    Rect whole{vx0, vy0, vx1, vy1};
    if (!count) {
        count = 1;
        rects = &whole;
    }
    std::vector<VkClearRect> clears;
    for (uint32_t i = 0; i < count; ++i) {
        int32_t x0 = std::max(rects[i].left, vx0), y0 = std::max(rects[i].top, vy0);
        int32_t x1 = std::min(rects[i].right, vx1), y1 = std::min(rects[i].bottom, vy1);
        if (x1 > x0 && y1 > y0)
            clears.push_back({{{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}}, 0, 1});
    }
    if (!clears.empty())
        vkCmdClearAttachments(m_frames[m_frameIndex].main, n, att, uint32_t(clears.size()), clears.data());
}

void Device::CopyTexture(Texture* dst, const Rect* dstRect, Texture* src, const Rect* srcRect, bool linear)
{
    if (!m_inFrame)
        return;
    if (!dst) dst = m_main;
    if (!src) src = m_main;
    if (!dst->m_renderTarget || src == dst || FormatIsCompressed(src->m_format) || !dst->m_image || !src->m_image) {
        Log("CopyTexture: unsupported source/destination combination\n");
        return;
    }
    VkCommandBuffer cmd = m_frames[m_frameIndex].main;
    EndRendering();
    VkImageLayout srcRestore = src->m_layout;
    Transition(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    Transition(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit blit{};
    blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    Rect s = srcRect ? *srcRect : Rect{0, 0, int32_t(src->m_width), int32_t(src->m_height)};
    Rect d = dstRect ? *dstRect : Rect{0, 0, int32_t(dst->m_width), int32_t(dst->m_height)};
    blit.srcOffsets[0] = {s.left, s.top, 0};
    blit.srcOffsets[1] = {s.right, s.bottom, 1};
    blit.dstOffsets[0] = {d.left, d.top, 0};
    blit.dstOffsets[1] = {d.right, d.bottom, 1};
    vkCmdBlitImage(cmd, src->m_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &blit, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    // Plain textures stay sampleable between uploads (their layout is otherwise only changed by uploads).
    if (!src->m_renderTarget)
        Transition(cmd, src, srcRestore);
    BeginRenderingOn(m_target);
}

// ---------------------------------------------------------------------------------------------------
// Drawing

void Device::DrawPrimitive(uint32_t primitive, uint32_t fvf, const void* vertices, uint32_t vertexCount)
{
    Draw(primitive, fvf, vertices, vertexCount, nullptr, 0);
}

void Device::DrawIndexedPrimitive(uint32_t primitive, uint32_t fvf, const void* vertices, uint32_t vertexCount,
                                  const uint16_t* indices, uint32_t indexCount)
{
    Draw(primitive, fvf, vertices, vertexCount, indices, indexCount);
}

void Device::DrawPrimitiveVB(uint32_t primitive, VertexBuffer* vb, uint32_t startVertex, uint32_t vertexCount)
{
    if (!vb || startVertex + vertexCount > vb->m_count)
        return;
    Draw(primitive, vb->m_fvf, vb->m_data.data() + size_t(startVertex) * vb->m_stride, vertexCount, nullptr, 0);
}

void Device::DrawIndexedPrimitiveVB(uint32_t primitive, VertexBuffer* vb, uint32_t startVertex, uint32_t vertexCount,
                                    const uint16_t* indices, uint32_t indexCount)
{
    // D3D7: indices are relative to startVertex; vertexCount vertices from there are referenced.
    if (!vb || startVertex + vertexCount > vb->m_count)
        return;
    Draw(primitive, vb->m_fvf, vb->m_data.data() + size_t(startVertex) * vb->m_stride, vertexCount, indices, indexCount);
}

void Device::ApplyDynamicState(uint32_t primitive, uint32_t fvf, uint32_t stride)
{
    VkCommandBuffer cmd = m_frames[m_frameIndex].main;
    StateCache& c = m_cache;
    uint32_t topoClass = TopologyClass(primitive);
    uint32_t pipelineClass = topoClass + (m_target->m_format == Format::RGBA16F ? 3u : 0u);
    if (c.topologyClass != pipelineClass) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          pipelineClass >= 3 ? m_pipelinesHdr[topoClass] : m_pipelines[topoClass]);
        c.topologyClass = pipelineClass;
    }
    if (c.topology != primitive) {
        vkCmdSetPrimitiveTopology(cmd, Topology(primitive));
        c.topology = primitive;
    }
    if (!c.valid) {
        // D3D front faces are clockwise on screen; D3DCULL_CCW culls the counter-clockwise (back) ones.
        vkCmdSetFrontFace(cmd, VK_FRONT_FACE_CLOCKWISE);
        c.valid = true;
    }

    // Negative height flips Y so D3D's clip space maps the same way; +0.5 matches D3D pixel centres.
    VkViewport vp;
    vp.x = float(m_viewport.x) + 0.5f;
    vp.y = float(m_viewport.y + m_viewport.height) + 0.5f;
    vp.width = float(m_viewport.width);
    vp.height = -float(m_viewport.height);
    vp.minDepth = m_viewport.minZ;
    vp.maxDepth = m_viewport.maxZ;
    if (std::memcmp(&vp, &c.viewport, sizeof(vp)) != 0) {
        vkCmdSetViewport(cmd, 0, 1, &vp);
        c.viewport = vp;
    }
    uint32_t w = m_target->m_width, h = m_target->m_height;
    uint32_t sx = std::min(m_viewport.x, w), sy = std::min(m_viewport.y, h);
    VkRect2D scissor{{int32_t(sx), int32_t(sy)}, {std::min(m_viewport.width, w - sx), std::min(m_viewport.height, h - sy)}};
    if (std::memcmp(&scissor, &c.scissor, sizeof(scissor)) != 0) {
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        c.scissor = scissor;
    }

    uint32_t cull = m_rs[d3d::RS_CULLMODE];
    if (c.cull != cull) {
        vkCmdSetCullMode(cmd, cull == d3d::CULL_CCW ? VK_CULL_MODE_BACK_BIT
                              : cull == d3d::CULL_CW ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_NONE);
        c.cull = cull;
    }
    uint32_t zEnable = m_rs[d3d::RS_ZENABLE] != 0;
    uint32_t zWrite = zEnable && m_rs[d3d::RS_ZWRITEENABLE] != 0;
    // Name labels write depth, which the ambient occlusion would take for geometry. They come in the game's
    // back-to-front sorted list, so nothing drawn after them needs their depth.
    if (m_drawIsLabel && m_target == m_scene && m_aoStrength > 0.0f)
        zWrite = 0;
    if (WaterWritesDepth(fvf))
        zWrite = 1;
    uint32_t zFunc = m_rs[d3d::RS_ZFUNC];
    if (c.depthTest != zEnable) { vkCmdSetDepthTestEnable(cmd, zEnable); c.depthTest = zEnable; }
    if (c.depthWrite != zWrite) { vkCmdSetDepthWriteEnable(cmd, zWrite); c.depthWrite = zWrite; }
    if (c.depthOp != zFunc) { vkCmdSetDepthCompareOp(cmd, CompareOp(zFunc)); c.depthOp = zFunc; }

    if (pipelineClass >= 3 && !c.glowBlendSet) {
        // The glow attachment always adds: what each additive effect contributes (others write 0).
        VkBool32 on = VK_TRUE;
        vkCmdSetColorBlendEnableEXT(cmd, 1, 1, &on);
        VkColorBlendEquationEXT eq{VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_ADD,
                                   VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_ADD};
        vkCmdSetColorBlendEquationEXT(cmd, 1, 1, &eq);
        c.glowBlendSet = true;
    }
    VkBool32 blend = m_rs[d3d::RS_ALPHABLENDENABLE] != 0;
    if (c.blendEnable != blend) {
        vkCmdSetColorBlendEnableEXT(cmd, 0, 1, &blend);
        c.blendEnable = blend;
    }
    uint32_t src = m_rs[d3d::RS_SRCBLEND], dst = m_rs[d3d::RS_DESTBLEND];
    if (m_drawOverbright2x) { src = kBlendOverbright2x; dst = kBlendOverbright2x; }
    if (src == d3d::BLEND_BOTHSRCALPHA) { src = d3d::BLEND_SRCALPHA; dst = d3d::BLEND_INVSRCALPHA; }
    if (src == d3d::BLEND_BOTHINVSRCALPHA) { src = d3d::BLEND_INVSRCALPHA; dst = d3d::BLEND_SRCALPHA; }
    // The equation only matters while blending, but Vulkan wants it set once per command buffer regardless.
    if (c.src == ~0u || (blend && (c.src != src || c.dst != dst))) {
        VkColorBlendEquationEXT eq{};
        eq.srcColorBlendFactor = eq.srcAlphaBlendFactor = BlendFactor(src);
        eq.dstColorBlendFactor = eq.dstAlphaBlendFactor = BlendFactor(dst);
        if (src == kBlendOverbright2x) {
            // dst * src * 2 from a halved src: src * dst + dst * src. Alpha as the game's multiply: dst * src.
            eq.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
            eq.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR;
            eq.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            eq.dstAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        }
        eq.colorBlendOp = eq.alphaBlendOp = VK_BLEND_OP_ADD;
        vkCmdSetColorBlendEquationEXT(cmd, 0, 1, &eq);
        c.src = src;
        c.dst = dst;
    }
    if (pipelineClass >= 3) {
        // Attachment 3, the motion vectors: written by solid (depth-writing) geometry, kept by everything else.
        uint32_t keep = m_rs[d3d::RS_ZENABLE] && m_rs[d3d::RS_ZWRITEENABLE] ? 0u : 1u;
        if (c.motionKeep != keep) {
            VkBool32 e = keep;
            vkCmdSetColorBlendEnableEXT(cmd, 3, 1, &e);
            VkColorBlendEquationEXT eq{VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_ADD,
                                       VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_ADD};
            vkCmdSetColorBlendEquationEXT(cmd, 3, 1, &eq);
            c.motionKeep = keep;
        }
        // Attachment 2, the local-light fraction: blended like the colour, so it stays the fraction of the final
        // colour - except for multiplying passes (the ground's lightmap + lights pass: its fraction replaces the
        // unlit base pass's) and additive effects (they don't light the surface under them: kept).
        uint32_t fEnable = blend, fSrc = src, fDst = dst;
        if (blend && IsMultiplyPass()) { fSrc = d3d::BLEND_ONE; fDst = d3d::BLEND_ZERO; }
        else if (blend && dst == d3d::BLEND_ONE) { fSrc = d3d::BLEND_ZERO; fDst = d3d::BLEND_ONE; }
        if (c.fractionEnable != fEnable) {
            VkBool32 e = fEnable;
            vkCmdSetColorBlendEnableEXT(cmd, 2, 1, &e);
            c.fractionEnable = fEnable;
        }
        if (c.fractionSrc == ~0u || (fEnable && (c.fractionSrc != fSrc || c.fractionDst != fDst))) {
            VkColorBlendEquationEXT eq{BlendFactor(fSrc), BlendFactor(fDst), VK_BLEND_OP_ADD,
                                       BlendFactor(fSrc), BlendFactor(fDst), VK_BLEND_OP_ADD};
            vkCmdSetColorBlendEquationEXT(cmd, 2, 1, &eq);
            c.fractionSrc = fSrc;
            c.fractionDst = fDst;
        }
        // Attachment 4, the surface colour (indirect light): blended like the colour, kept by multiplying passes (the
        // ground's lightmap pass: the unlit base pass wrote its colour), additive effects and anything else blending
        // by the colour already there.
        uint32_t aSrc = src, aDst = dst;
        bool byTarget = src == kBlendOverbright2x || src == d3d::BLEND_DESTCOLOR || src == d3d::BLEND_INVDESTCOLOR ||
                        dst == d3d::BLEND_SRCCOLOR || dst == d3d::BLEND_INVSRCCOLOR;
        if (blend && (IsMultiplyPass() || byTarget || dst == d3d::BLEND_ONE)) { aSrc = d3d::BLEND_ZERO; aDst = d3d::BLEND_ONE; }
        if (c.albedoEnable != blend) {
            vkCmdSetColorBlendEnableEXT(cmd, 4, 1, &blend);
            c.albedoEnable = blend;
        }
        if (c.albedoSrc == ~0u || (blend && (c.albedoSrc != aSrc || c.albedoDst != aDst))) {
            VkColorBlendEquationEXT eq{BlendFactor(aSrc), BlendFactor(aDst), VK_BLEND_OP_ADD,
                                       BlendFactor(aSrc), BlendFactor(aDst), VK_BLEND_OP_ADD};
            vkCmdSetColorBlendEquationEXT(cmd, 4, 1, &eq);
            c.albedoSrc = aSrc;
            c.albedoDst = aDst;
        }
        // Write masks: an attachment the draw only keeps (blended ZERO/ONE, or adding nothing to the glow) isn't
        // written at all - not read and written back for every fragment of every draw.
        if (m_dynamicWriteMask) {
            const uint32_t all = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                                 VK_COLOR_COMPONENT_A_BIT;
            uint32_t masks[5] = {all, GlowDraw(fvf) ? all : 0u,
                                 blend && fSrc == d3d::BLEND_ZERO && fDst == d3d::BLEND_ONE ? 0u : all,
                                 keep ? 0u : all,
                                 blend && aSrc == d3d::BLEND_ZERO && aDst == d3d::BLEND_ONE ? 0u : all};
            if (std::memcmp(masks, c.writeMask, sizeof(masks)) != 0) {
                VkColorComponentFlags flags[5];
                for (int i = 0; i < 5; ++i) flags[i] = masks[i];
                vkCmdSetColorWriteMaskEXT(cmd, 0, 5, flags);
                std::memcpy(c.writeMask, masks, sizeof(masks));
            }
        }
    } else if (m_dynamicWriteMask && c.writeMask[0] == ~0u) {
        VkColorComponentFlags all = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                                    VK_COLOR_COMPONENT_A_BIT;
        vkCmdSetColorWriteMaskEXT(cmd, 0, 1, &all);          // 8-bit targets: their one attachment
        c.writeMask[0] = all;
    }

    if (c.fvf != fvf) {
        // Vertex layout: binding 0 = the frame's ring buffer (draws address it through their vertex offset),
        // binding 1 = zeros for attributes the format lacks.
        FvfLayout layout = DecodeFvf(fvf);
        VkVertexInputBindingDescription2EXT bindings[2] = {
            {VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT, nullptr, 0, stride, VK_VERTEX_INPUT_RATE_VERTEX, 1},
            {VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT, nullptr, 1, 0, VK_VERTEX_INPUT_RATE_VERTEX, 1},
        };
        VkVertexInputAttributeDescription2EXT attrs[6];
        for (uint32_t i = 0; i < 6; ++i) {
            attrs[i] = {VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT};
            attrs[i].location = i;
            if (layout.offset[i] >= 0) {
                attrs[i].binding = 0;
                attrs[i].format = layout.format[i];
                attrs[i].offset = uint32_t(layout.offset[i]);
            } else {
                attrs[i].binding = 1;
                attrs[i].format = VK_FORMAT_R32G32B32A32_SFLOAT;
                attrs[i].offset = 0;
            }
        }
        vkCmdSetVertexInputEXT(cmd, 2, bindings, 6, attrs);
        c.fvf = fvf;
    }
    if (!c.buffersBound) {
        Frame& f = m_frames[m_frameIndex];
        VkBuffer buffers[2] = {f.ring, m_nullBuffer};
        VkDeviceSize offsets[2] = {0, 0};
        vkCmdBindVertexBuffers(cmd, 0, 2, buffers, offsets);
        vkCmdBindIndexBuffer(cmd, f.ring, 0, VK_INDEX_TYPE_UINT16);
        c.buffersBound = true;
    }
}

void Device::Draw(uint32_t primitive, uint32_t fvf, const void* vertices, uint32_t vertexCount,
                  const uint16_t* indices, uint32_t indexCount)
{
    if (!m_inFrame || !vertexCount)
        return;
    ++m_frameDraw;
    // Particles whose effect the game no longer draws: at the end of the 3D scene - the first interface draw after 3D,
    // both into the main target (other targets - refraction, offscreen copies - have their own pre-transformed draws).
    if (!m_external && m_target == m_main) {
        if ((fvf & d3d::FVF_POSITION_MASK) != d3d::FVF_XYZRHW)
            m_particleSaw3D = true;
        else if (IsInterfaceDraw(fvf) && m_particleSaw3D && !m_particleOrphansDone) {
            m_particleOrphanTrigger = "end of the 3D scene";
            DrawOrphanParticles();
        }
    }
    // HDR: the frame's first interface draw after its 3D ends the scene phase - the scene is tone mapped and the
    // interface drawn over it into the 8-bit target. (Not every pre-transformed draw is interface: IsInterfaceDraw.)
    if (m_scenePhase && m_target == m_scene) {
        if ((fvf & d3d::FVF_POSITION_MASK) != d3d::FVF_XYZRHW) {
            m_sceneSaw3D = true;
            if (!m_aoProjValid && m_rs[d3d::RS_ZENABLE] && m_rs[d3d::RS_ZWRITEENABLE]) {   // the world camera
                m_aoProj = m_proj;
                m_aoView = m_view;
                m_aoProjValid = true;
            }
        } else if (m_sceneSaw3D && IsInterfaceDraw(fvf)) {
            m_sceneEndDraw = m_dumpDraw;
            m_sceneEndFvf = fvf;
            EndScene();
        }
    }
    m_drawIsLabel = !m_external && IsLabel(primitive, fvf, vertexCount);
    if (m_dumpFile && !m_external)
        DumpDraw(primitive, fvf, vertices, vertexCount, indices, indexCount);
    if (!m_external && IsBlobShadow(primitive, fvf, vertices, vertexCount, indexCount))
        return;                                  // replaced by sun shadows
    if (IsTerrain(fvf) && IsMultiplyPass())
        m_terrainLitPassCur = true;
    Frame& f = m_frames[m_frameIndex];
    VkCommandBuffer cmd = f.main;
    FvfLayout layout = DecodeFvf(fvf);

    // Render targets bound as textures must be readable; layout changes can't happen inside rendering.
    bool needTransition = false;
    for (Texture* t : m_textures)
        if (t && t->m_renderTarget && t->m_layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
            needTransition = true;
    if (needTransition) {
        vkCmdEndRendering(cmd);
        for (Texture* t : m_textures)
            if (t && t->m_renderTarget)
                Transition(cmd, t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        BeginRenderingOn(m_target);
    }

    // Everything this draw puts in the ring buffer, reserved together so a flush can't split it. (External geometry -
    // the particles - is already in a GPU buffer.)
    const VkDeviceSize uboAlign = m_props.limits.minUniformBufferOffsetAlignment;
    bool motion = !m_external && MotionVectorDraw(fvf);   // reads the last frame's camera from the frame block
    VkDeviceSize geometryBytes = m_external ? 0 : VkDeviceSize(layout.stride) * vertexCount + VkDeviceSize(indexCount) * 2;
    EnsureRingSpace(sizeof(DrawConstants) + sizeof(DrawTransform) + sizeof(FrameLights) + geometryBytes + 3 * uboAlign +
                    layout.stride + 32 + (motion ? 12ull * vertexCount + 256 : 0));

    // The frame's light list (binding 4): rebuilt when lights changed; always bound, as layouts require.
    // Only lit draws read it; the others bind any in-range part of the ring.
    bool needLights = (m_lightOverride && m_pixelLighting && m_rs[d3d::RS_LIGHTING] &&
                       (fvf & d3d::FVF_POSITION_MASK) != d3d::FVF_XYZRHW) || ShadowReceiver(fvf) ||
                      ShadowCompensated(fvf) || motion ||
                      (TaaActive() && m_target == m_scene && (fvf & d3d::FVF_POSITION_MASK) != d3d::FVF_XYZRHW);
    VkDeviceSize frameLightsOffset = m_frameLightsGeneration == m_ringGeneration ? m_frameLightsOffset : 0;
    if (needLights && (m_frameLightsDirty || m_frameLightsGeneration != m_ringGeneration))   // once per frame
        frameLightsOffset = WriteFrameLights();

    // Per-draw world matrix (small block).
    void* cpu;
    VkDeviceSize transformOffset = Allocate(sizeof(DrawTransform), uboAlign, &cpu);
    auto* drawTransform = static_cast<DrawTransform*>(cpu);
    VkDeviceSize prevPositionsOffset = 0, prevPositionsBytes = 0;   // binding 8 (animated meshes' last positions)
    drawTransform->world = m_world;
    drawTransform->prevWorld = m_world;
    drawTransform->motion[0] = motion ? 1.0f : 0.0f;
    drawTransform->motion[1] = drawTransform->motion[2] = drawTransform->motion[3] = 0.0f;
    if (m_external || !SwayParams(fvf, layout.stride, vertices, vertexCount, drawTransform->sway))
        drawTransform->sway[0] = drawTransform->sway[1] = drawTransform->sway[2] = drawTransform->sway[3] = 0.0f;
    std::memcpy(m_drawSway, drawTransform->sway, sizeof(m_drawSway));
    FrameLightMask(fvf, layout.stride, vertices, vertexCount, drawTransform->lightMask);
    if (motion) {
        // Motion vectors: the same object last frame - same mesh, nearest to where this one is (within 3 units). Not
        // found (new, or a different level of detail): its current matrix, i.e. it moved with the world.
        uint64_t key = MotionKey(primitive, fvf, vertexCount, indices, indexCount);
        // This frame's positions (for next frame's match) of a mesh small enough to be a character's part.
        std::vector<float> positions;
        if (vertexCount <= kMotionMaxVertices) {
            positions.resize(size_t(vertexCount) * 3);
            const uint8_t* v = static_cast<const uint8_t*>(vertices);
            for (uint32_t i = 0; i < vertexCount; ++i)
                std::memcpy(&positions[size_t(i) * 3], v + size_t(i) * layout.stride, 12);
        }
        auto it = m_motionPrev.find(key);
        if (it != m_motionPrev.end()) {
            MotionEntry* best = nullptr;
            float bestD2 = 9.0f;
            for (MotionEntry& e : it->second) {
                if (e.used) continue;
                float dx = e.world.m[3][0] - m_world.m[3][0], dy = e.world.m[3][1] - m_world.m[3][1],
                      dz = e.world.m[3][2] - m_world.m[3][2], d2 = dx * dx + dy * dy + dz * dz;
                if (d2 < bestD2) { bestD2 = d2; best = &e; }
            }
            if (best) {
                drawTransform->prevWorld = best->world;
                best->used = true;
                // Animated (its vertices changed): last frame's positions for the vertex shader (binding 8).
                if (!positions.empty() && best->positions.size() == positions.size() &&
                    std::memcmp(best->positions.data(), positions.data(), positions.size() * 4) != 0) {
                    void* prevCpu;
                    prevPositionsOffset = Allocate(positions.size() * 4, m_props.limits.minStorageBufferOffsetAlignment, &prevCpu);
                    std::memcpy(prevCpu, best->positions.data(), positions.size() * 4);
                    prevPositionsBytes = positions.size() * 4;
                    drawTransform->motion[1] = 1.0f;
                }
            }
        }
        m_motionCur[key].push_back({m_world, false, std::move(positions)});
    }

    // The big constant block: reused unless something feeding it changed since it was written.
    uint32_t texMask = (m_textures[0] ? 1u : 0u) | (m_textures[1] ? 2u : 0u);
    bool terrain = IsTerrain(fvf);
    // The ground: base pass textures remembered by chunk; the lighting pass (lightmap + lights, multiplying the base,
    // drawn after all base passes) takes its relief from its chunk's.
    m_drawBumpBase = nullptr;
    if (terrain && m_textures[0]) {
        if (!m_rs[d3d::RS_LIGHTING] && !m_rs[d3d::RS_ALPHABLENDENABLE]) {
            m_terrainBases[TerrainChunkKey(vertices, vertexCount, layout.stride, indexCount)] = m_textures[0];
        } else if (m_rs[d3d::RS_LIGHTING] && m_bump > 0.0f && IsMultiplyPass()) {
            auto it = m_terrainBases.find(TerrainChunkKey(vertices, vertexCount, layout.stride, indexCount));
            if (it != m_terrainBases.end()) m_drawBumpBase = it->second;
        }
    }
    uint32_t carrier = m_external ? 0 : CarriedLight(fvf, vertices, vertexCount, layout.stride);
    bool rewrite = m_constantsDirty || m_constantsGeneration != m_ringGeneration || m_constantsFvf != fvf ||
                   m_constantsTexMask != texMask || m_constantsTerrain != terrain || m_constantsLabel != m_drawIsLabel ||
                   m_constantsCarrier != carrier || m_constantsBumpBase != m_drawBumpBase;
    VkDeviceSize uboOffset = m_constantsOffset;
    if (rewrite) {
    uboOffset = Allocate(sizeof(DrawConstants), uboAlign, &cpu);
    m_constantsOffset = uboOffset;
    m_constantsGeneration = m_ringGeneration;   // after Allocate: a wrap would bump the generation
    m_constantsDirty = false;
    m_constantsFvf = fvf;
    m_constantsTexMask = texMask;
    m_constantsTerrain = terrain;
    m_constantsLabel = m_drawIsLabel;
    m_constantsCarrier = carrier;
    m_constantsBumpBase = m_drawBumpBase;
    auto* c = static_cast<DrawConstants*>(cpu);
    c->view = m_view;
    c->proj = m_proj;
    c->texMatrix[0] = m_texMatrix[0];
    c->texMatrix[1] = m_texMatrix[1];
    c->viewport[0] = float(m_viewport.x);
    c->viewport[1] = float(m_viewport.y);
    c->viewport[2] = float(m_viewport.width);
    c->viewport[3] = float(m_viewport.height);
    Copy4(c->matDiffuse, m_material.diffuse);
    Copy4(c->matAmbient, m_material.ambient);
    Copy4(c->matSpecular, m_material.specular);
    Copy4(c->matEmissive, m_material.emissive);
    ArgbToFloat(m_rs[d3d::RS_AMBIENT], c->ambient);
    ArgbToFloat(m_rs[d3d::RS_FOGCOLOR], c->fogColor);
    c->fogParams[0] = AsFloat(m_rs[d3d::RS_FOGSTART]);
    c->fogParams[1] = AsFloat(m_rs[d3d::RS_FOGEND]);
    c->fogParams[2] = AsFloat(m_rs[d3d::RS_FOGDENSITY]);
    c->fogParams[3] = 0.0f;
    ArgbToFloat(m_rs[d3d::RS_TEXTUREFACTOR], c->tfactor);
    c->misc[0] = m_material.power;
    c->misc[1] = float(m_rs[d3d::RS_ALPHAREF] & 0xFF);
    c->misc[2] = m_effectGlow;                 // F_GLOW: how much the effect feeds the glow
    c->misc[3] = m_bump;                       // F_BUMP: height change per texel for a full brightness step
    // Camera position/forward in world space from the view matrix (columns 0-2 = camera axes for an
    // orthonormal D3D view matrix; row 3 = -eye expressed in those axes).
    const auto& v = m_view.m;
    for (int i = 0; i < 3; ++i) {
        c->eyePos[i] = -(v[3][0] * v[i][0] + v[3][1] * v[i][1] + v[3][2] * v[i][2]);
        c->eyeDir[i] = v[i][2];
    }
    c->eyePos[3] = 1.0f;
    c->eyeDir[3] = 0.0f;
    c->vtx[0] = fvf;
    std::memcpy(&c->vtx[1], &m_drawColorScale, 4);
    if (m_drawColorScale == 1.0f) c->vtx[1] = 0;
    // Reflectivity (screen-space reflections, the scene's attachment 2 G): water; glossy (specular) surfaces; and a
    // wet look on surfaces facing up, which the shader scales by how much the surface faces up.
    float reflectivity = 0.0f, wet = 0.0f;
    if (m_ssr > 0.0f) {
        if (IsWater(fvf)) {
            // Floating text comes in the water's format, drawn over everything: not reflective.
            if (m_rs[d3d::RS_ZFUNC] != d3d::CMP_ALWAYS) reflectivity = m_ssrWater;
        } else if ((fvf & d3d::FVF_POSITION_MASK) != d3d::FVF_XYZRHW) {
            if (m_rs[d3d::RS_SPECULARENABLE] && m_material.power > 0.0f) reflectivity = m_ssrGloss;
            wet = m_ssrWet;
        }
    }
    std::memcpy(&c->vtx[2], &reflectivity, 4);
    std::memcpy(&c->vtx[3], &wet, 4);
    uint32_t flags = 0;
    if (m_rs[d3d::RS_LIGHTING] && (fvf & d3d::FVF_POSITION_MASK) != d3d::FVF_XYZRHW) flags |= F_LIGHTING;
    if ((flags & F_LIGHTING) && m_pixelLighting) flags |= F_PERPIXEL;
    if (m_lightingDebug) flags |= F_DEBUGLIGHT;
    bool override = (flags & F_PERPIXEL) && m_lightOverride;
    if (override) flags |= F_LIGHTOVERRIDE;
    bool hdrTarget = m_target->m_format == Format::RGBA16F;
    if (hdrTarget) flags |= F_HDR;
    // Generated normals: per-pixel lit 3D drawn with a texture in stage 0 as its surface (not the ground's
    // lightmap pass, whose stage 0 is the lightmap), with plain coordinates.
    if ((flags & F_PERPIXEL) && m_bump > 0.0f && m_textures[0] && !terrain && m_tss[0][d3d::TSS_COLOROP] != d3d::TOP_DISABLE &&
        !(m_tss[0][d3d::TSS_TEXTURETRANSFORMFLAGS] & 256u) && (m_tss[0][d3d::TSS_TEXCOORDINDEX] & 0xFFFF0000u) == 0)
        flags |= F_BUMP;
    else if ((flags & F_PERPIXEL) && m_drawBumpBase)
        flags |= F_BUMP | F_BUMPBASE;
    if (GlowDraw(fvf)) flags |= F_GLOW | (m_rs[d3d::RS_SRCBLEND] == d3d::BLEND_SRCALPHA ? F_GLOWALPHA : 0u);
    if (override && (m_lightHeadroom > 1.0f || hdrTarget)) flags |= F_OVERBRIGHT;
    if (Overbright2x(fvf)) flags |= F_OVERBRIGHT2X;
    if (ShadowReceiver(fvf)) flags |= F_SHADOW;
    else if (ShadowInLightmap(fvf)) flags |= F_SHADOWTEX;
    else if (ShadowCompensated(fvf)) flags |= F_SHADOWCOMP;
    if (m_rs[d3d::RS_COLORVERTEX]) flags |= F_COLORVERTEX;
    if (m_rs[d3d::RS_SPECULARENABLE]) flags |= F_SPECULAR;
    if (m_rs[d3d::RS_NORMALIZENORMALS]) flags |= F_NORMALIZE;
    if (m_rs[d3d::RS_FOGENABLE]) flags |= F_FOG;
    if (m_rs[d3d::RS_RANGEFOGENABLE]) flags |= F_RANGEFOG;
    if (m_rs[d3d::RS_LOCALVIEWER]) flags |= F_LOCALVIEWER;
    if (m_textures[0]) flags |= F_TEX0;
    if (m_textures[1]) flags |= F_TEX1;
    if (m_rs[d3d::RS_ALPHATESTENABLE]) flags |= F_ALPHATEST;
    bool solid3d = (fvf & d3d::FVF_POSITION_MASK) != d3d::FVF_XYZRHW && !terrain && !m_drawIsLabel && m_textures[0] &&
                   m_rs[d3d::RS_ZENABLE] && m_rs[d3d::RS_ZWRITEENABLE] && m_rs[d3d::RS_ZFUNC] != d3d::CMP_ALWAYS;
    // Foliage candidates: lit, cut out of their texture (alpha test, or blended with depth writes as most of the
    // game's statics are) - the shader keeps those whose texture actually has holes (leaves, not walls).
    if (m_leafLight > 0.0f && solid3d && (flags & F_LIGHTING) &&
        (m_rs[d3d::RS_ALPHATESTENABLE] || m_rs[d3d::RS_ALPHABLENDENABLE]) && (m_tss[0][d3d::TSS_TEXCOORDINDEX] & 0xFFFF0000u) == 0)
        flags |= F_FOLIAGE;
    // Night glow candidates: opaque 3D surfaces drawn unlit (self-lit, like windows and signs) or with an emissive
    // material - not effects, the sky, the ground or lighting passes.
    bool additive = m_rs[d3d::RS_ALPHABLENDENABLE] && m_rs[d3d::RS_DESTBLEND] == d3d::BLEND_ONE;
    float emissive = std::max({m_material.emissive.r, m_material.emissive.g, m_material.emissive.b});
    if (m_nightGlow > 0.0f && hdrTarget && solid3d && !additive && !IsMultiplyPass() &&
        (!(flags & F_LIGHTING) || emissive > 0.05f))
        flags |= F_EMISSIVE;
    c->flags[0] = flags;
    c->flags[1] = m_rs[d3d::RS_FOGVERTEXMODE];
    c->flags[2] = m_rs[d3d::RS_FOGTABLEMODE];
    c->flags[3] = m_rs[d3d::RS_ALPHAFUNC];
    c->matSources[0] = m_rs[d3d::RS_DIFFUSEMATERIALSOURCE];
    c->matSources[1] = m_rs[d3d::RS_AMBIENTMATERIALSOURCE];
    c->matSources[2] = m_rs[d3d::RS_SPECULARMATERIALSOURCE];
    c->matSources[3] = m_rs[d3d::RS_EMISSIVEMATERIALSOURCE];
    for (int s = 0; s < 2; ++s) {
        const auto& t = m_tss[s];
        c->stageA[s][0] = t[d3d::TSS_COLOROP];
        c->stageA[s][1] = t[d3d::TSS_COLORARG1];
        c->stageA[s][2] = t[d3d::TSS_COLORARG2];
        c->stageA[s][3] = t[d3d::TSS_ALPHAOP];
        c->stageB[s][0] = t[d3d::TSS_ALPHAARG1];
        c->stageB[s][1] = t[d3d::TSS_ALPHAARG2];
        c->stageB[s][2] = t[d3d::TSS_TEXCOORDINDEX];
        c->stageB[s][3] = t[d3d::TSS_TEXTURETRANSFORMFLAGS];
    }
    uint32_t lightCount = 0, localLights = 0;
    if (flags & F_LIGHTING)
        for (const LightSlot& slot : m_lights) {
            if (!slot.enabled || lightCount == kMaxLights)
                continue;
            if (override && slot.light.type != d3d::LIGHT_DIRECTIONAL)
                continue;                        // replaced by the frame lights
            // The ground's lightmap has the sun baked in, and the game means to keep directional lights off it
            // (CullLights mask 0xfffffffe) - but skips that mask when there are 8 lights or fewer. The sun then
            // saturates the ground's lighting pass and local lights (the player's) vanish on it.
            if (override && terrain && slot.light.type == d3d::LIGHT_DIRECTIONAL)
                continue;
            FillGpuLight(slot.light, slot.cosHalfTheta, slot.cosHalfPhi, c->lights[lightCount++]);
            if (slot.light.type != d3d::LIGHT_DIRECTIONAL) ++localLights;
        }
    c->lightInfo[0] = lightCount;
    c->lightInfo[1] = localLights;
    c->lightInfo[2] = override ? carrier : 0;   // frame light (index + 1) this draw carries: it doesn't light it
    c->lightInfo[3] = 0;
    }

    // Geometry: vertices aligned to their stride and indices to 2 bytes, so the draw can address them inside
    // the ring buffer bound once (vertexOffset / firstIndex) instead of rebinding buffers per draw.
    VkDeviceSize vbOffset = 0, ibOffset = 0;
    if (!m_external) {
        VkDeviceSize vbBytes = VkDeviceSize(layout.stride) * vertexCount;
        vbOffset = Allocate(vbBytes, layout.stride, &cpu);
        std::memcpy(cpu, vertices, vbBytes);
        drawTransform->motion[2] = float(vbOffset / layout.stride);   // the base vertex: gl_VertexIndex - it = vertex
        if (indices) {
            ibOffset = Allocate(VkDeviceSize(indexCount) * 2, 2, &cpu);
            std::memcpy(cpu, indices, size_t(indexCount) * 2);
        }
        RecordShadowCaster(primitive, fvf, layout.stride, vertices, vertexCount, vbOffset, indices,
                           indices ? indexCount : 0, ibOffset);
    }
    m_drawOverbright2x = Overbright2x(fvf);
    if (GlowDraw(fvf)) ++m_glowDraws;
    ApplyDynamicState(primitive, fvf, layout.stride);

    VkDescriptorBufferInfo ubo{f.ring, uboOffset, sizeof(DrawConstants)};
    VkDescriptorBufferInfo transform{f.ring, transformOffset, sizeof(DrawTransform)};
    VkDescriptorImageInfo images[2];
    for (uint32_t s = 0; s < 2; ++s) {
        Texture* t = m_textures[s] ? m_textures[s] : m_blackTexture;
        images[s] = {SamplerFor(s), t->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }
    VkDescriptorBufferInfo frameLights{f.ring, frameLightsOffset, sizeof(FrameLights)};
    VkDescriptorImageInfo shadow{m_shadowSampler, m_shadowView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo cubes{m_cubeSampler, m_cubeArrayView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    Texture* bumpBase = m_drawBumpBase ? m_drawBumpBase : m_blackTexture;
    VkDescriptorImageInfo bump{m_bumpSampler, bumpBase->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    // Binding 8: an animated mesh's last positions, else any small part of the ring (unread).
    VkDescriptorBufferInfo prevPositions{f.ring, prevPositionsOffset, prevPositionsBytes ? prevPositionsBytes : 16};
    // Binding 9: the sun shadow cascades' depths, read without comparison (soft shadows' blocker search).
    VkDescriptorImageInfo shadowDepths{m_shadowDepthSampler, m_shadowView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet writes[10] = {};
    for (int i = 0; i < 10; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstBinding = uint32_t(i);
        writes[i].descriptorCount = 1;
    }
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].pBufferInfo = &ubo;
    writes[1].descriptorType = writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo = &images[0];
    writes[2].pImageInfo = &images[1];
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[3].pBufferInfo = &transform;
    writes[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[4].pBufferInfo = &frameLights;
    writes[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[5].pImageInfo = &shadow;
    writes[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[6].pImageInfo = &cubes;
    writes[7].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[7].pImageInfo = &bump;
    writes[8].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[8].pBufferInfo = &prevPositions;
    writes[9].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[9].pImageInfo = &shadowDepths;
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 10, writes);

    if (m_external) {
        VkBuffer buffers[2] = {m_external->vertices, m_nullBuffer};
        VkDeviceSize offsets[2] = {0, 0};
        vkCmdBindVertexBuffers(cmd, 0, 2, buffers, offsets);
        vkCmdBindIndexBuffer(cmd, m_external->indices, 0, VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexed(cmd, indexCount, 1, 0, m_external->baseVertex, 0);
        m_cache.buffersBound = false;            // the next draw binds the ring again
        return;
    }
    if (indices)
        vkCmdDrawIndexed(cmd, indexCount, 1, uint32_t(ibOffset / 2), int32_t(vbOffset / layout.stride), 0);
    else
        vkCmdDraw(cmd, vertexCount, 1, uint32_t(vbOffset / layout.stride), 0);

    // A particle effect's sprites (ParticleEmitter): its particles follow, with the same state.
    if (m_particlePending && fvf == kParticleFvf) {
        ParticleBlock* block = m_particlePending;
        m_particlePending = nullptr;
        DrawParticles(*block);
    }
}

}  // namespace rvk
