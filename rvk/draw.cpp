// D3D7 state, Clear, and drawing.
#include "internal.h"

#include <algorithm>
#include <bit>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
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

// scale: the light's intensity (RVK_PtLight / RVK_CharLight for point and spot lights).
void FillGpuLight(const d3d::Light& l, float cosHalfTheta, float cosHalfPhi, GpuLight& g, float scale = 1.0f)
{
    Copy4(g.diffuse, l.diffuse);
    Copy4(g.specular, l.specular);
    Copy4(g.ambient, l.ambient);
    for (int i = 0; i < 3; ++i) {
        g.diffuse[i] *= scale;
        g.specular[i] *= scale;
        g.ambient[i] *= scale;
    }
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
    case d3d::View: {
        m_gameView = m;
        if (m.m[3][0] != 0.0f || m.m[3][1] != 0.0f || m.m[3][2] != 0.0f) {
            m_gameMainView = m;
            m_gameMainValid = true;
        }
        if (m_freeCam) {
            ApplyFreeCam();
            return;
        }
        target = &m_view;
        break;
    }
    case d3d::Projection: target = &m_proj; break;
    case d3d::Texture0: target = &m_texMatrix[0]; break;
    case d3d::Texture1: target = &m_texMatrix[1]; break;
    default: return;
    }
    if (std::memcmp(target, &m, sizeof(m)) != 0) {
        *target = m;
        m_constantsDirty = true;
        if (type == d3d::View || type == d3d::Projection) ++m_cameraGen;
    }
}

// The free camera's view: each view the game sets taken back out of its main camera's and into the free one's - the main
// view becomes the free camera's own, a view without position (the sky's) keeps its relation to it.
void Device::ApplyFreeCam()
{
    const float cy = std::cos(m_freeYaw), sy = std::sin(m_freeYaw), cp = std::cos(m_freePitch), sp = std::sin(m_freePitch);
    const float f[3] = {cp * sy, sp, cp * cy};
    float r[3] = {f[2], 0.0f, -f[0]};            // (left-handed: the world's up x forward)
    const float rl = std::hypot(r[0], r[2]);
    r[0] /= rl; r[2] /= rl;
    const float u[3] = {f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0]};
    d3d::Matrix F = Identity();
    for (int i = 0; i < 3; ++i) {
        F.m[i][0] = r[i];
        F.m[i][1] = u[i];
        F.m[i][2] = f[i];
    }
    const float* e = m_freePos;
    F.m[3][0] = -(r[0] * e[0] + r[1] * e[1] + r[2] * e[2]);
    F.m[3][1] = -(u[0] * e[0] + u[1] * e[1] + u[2] * e[2]);
    F.m[3][2] = -(f[0] * e[0] + f[1] * e[1] + f[2] * e[2]);
    d3d::Matrix inv;
    if (!InvertMatrix(m_gameMainView, &inv))
        return;
    m_view = MulMatrix(MulMatrix(m_gameView, inv), F);
    m_constantsDirty = true;
    ++m_cameraGen;
}

void Device::SetFreeCam(const FreeCamInput& in)
{
    if (!in.on) {
        if (m_freeCam) {                         // (back to the game's)
            m_freeCam = false;
            m_view = m_gameView;
            m_constantsDirty = true;
            ++m_cameraGen;
            Log("free camera: off");
        }
        return;
    }
    if (!m_freeCam) {                            // on: where the game's camera is, looking its way
        d3d::Matrix inv;
        if (!m_gameMainValid || !InvertMatrix(m_gameMainView, &inv))
            return;
        for (int i = 0; i < 3; ++i) m_freePos[i] = inv.m[3][i];
        m_freeYaw = std::atan2(inv.m[2][0], inv.m[2][2]);
        m_freePitch = std::asin(std::clamp(inv.m[2][1], -1.0f, 1.0f));
        m_freeCam = true;
        Log("free camera: on at (%.1f %.1f %.1f)", double(m_freePos[0]), double(m_freePos[1]), double(m_freePos[2]));
    }
    m_freeYaw += in.look[0];
    m_freePitch = std::clamp(m_freePitch + in.look[1], -1.55f, 1.55f);
    const float cy = std::cos(m_freeYaw), sy = std::sin(m_freeYaw), cp = std::cos(m_freePitch), sp = std::sin(m_freePitch);
    const float f[3] = {cp * sy, sp, cp * cy}, r[3] = {cy, 0.0f, -sy};
    const float step = in.speed * std::clamp(in.dt, 0.0f, 0.1f);
    for (int i = 0; i < 3; ++i)
        m_freePos[i] += (r[i] * in.move[0] + (i == 1 ? in.move[1] : 0.0f) + f[i] * in.move[2]) * step;
    ApplyFreeCam();
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
    // The game sets its lights again for every object; the same light again changes nothing (no constant block
    // rewrite, which would also end the draw's indirect batch).
    if (std::memcmp(&slot.light, &light, sizeof(light)) == 0) {
        if (slot.enabled)
            CaptureLight(slot);                  // this frame's light set still needs it (once per frame)
        return;
    }
    ++slot.version;
    ++m_lightsGen;
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
        ++m_lightsGen;
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
    // Night: the sun's light on flat ground (smoothed) below a tenth of full day; lifted by the game's dawn ambient.
    float night = NightAmount();
    fl->effects[0] = m_leafLight;
    fl->effects[1] = m_hdr ? m_nightGlow * night : 0.0f;
    fl->effects[2] = m_sunSoftness;
    fl->effects[3] = m_grassPush;
    Wind(fl->wind);
    WindFrame(fl->windFrame);
    Lightning(fl->flash, fl->flashView, fl->flashProj, fl->flashWorld);
    fl->rainNear[0] = m_rainNearFrame == m_frameNumber && m_rainNear ? 1.0f : 0.0f;
    fl->rainNear[1] = StormOvercast();
    fl->rainNear[2] = std::clamp(m_storm * std::max(m_windStormScale, 0.0f), 0.0f, 1.0f) * std::clamp(m_stormDebris, 0.0f, 2.0f);
    fl->rainNear[3] = m_ssr > 0.0f ? 1.0f : 0.0f;
    // The wet ground and the rain's cover map (its bindless slot packed as the terrain's sun map's).
    // (Deferred - the user, 2026-10-16: the wet look flickered at an even beat in game; off whatever RVK_RainWet says
    // until that is found. Suspects: the environment probe's ping-pong update, the puddles' ripple clock.)
    constexpr bool kWetGround = false;
    fl->wet[0] = kWetGround && !m_indoors ? m_wetness : 0.0f;
    fl->wet[1] = kWetGround && !m_indoors ? m_puddles : 0.0f;
    fl->wet[2] = float(std::fmod(m_rainClock, 3600.0));
    fl->wet[3] = m_rainCoverOut[3];
    for (int i = 0; i < 4; ++i) fl->cover[i] = 0.0f;
    if (m_rainCover && m_rainCoverOut[2] > 0.0f && m_rainCover->m_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        const uint32_t image = BindlessImage(m_rainCover), sampler = FixedSamplerSlot(2, m_linearSampler);
        if (image != ~0u && image < (1u << 20) - 1 && sampler < 256) {
            const uint32_t slot = (image + 1) | sampler << 20;
            fl->cover[0] = m_rainCoverOut[0];
            fl->cover[1] = m_rainCoverOut[1];
            fl->cover[2] = m_rainCoverOut[2];
            std::memcpy(&fl->cover[3], &slot, 4);
        }
    }
    fl->neon[0] = m_hdr ? m_neonSat : 0.0f;
    // (A rainbow - tonemap.frag Rainbow: yzw towards the point opposite the sun, in the world camera's view, its
    // length how strong.)
    fl->neon[1] = fl->neon[2] = fl->neon[3] = 0.0f;
    if (m_rainbow > 0.001f && m_hdr) {
        const float l = std::max(std::sqrt(m_trueSunDir[0] * m_trueSunDir[0] + m_trueSunDir[1] * m_trueSunDir[1] +
                                           m_trueSunDir[2] * m_trueSunDir[2]), 1e-6f);
        for (int k = 0; k < 3; ++k)
            fl->neon[1 + k] = (m_aoView.m[0][k] * m_trueSunDir[0] + m_aoView.m[1][k] * m_trueSunDir[1] +
                               m_aoView.m[2][k] * m_trueSunDir[2]) / l * m_rainbow;
    }
    // The sky's model (sky.cpp: the haze and the far fog in its colours) and the world camera's rotation (the tone
    // mapping works in view space; the model in the world's).
    std::memcpy(fl->sky, m_skyModel, sizeof(fl->sky));
    std::memcpy(fl->dawn, m_dawnTint, sizeof(fl->dawn));
    for (int i = 0; i < 3; ++i) {
        for (int k = 0; k < 3; ++k) fl->skyView[i][k] = m_aoView.m[i][k];
        fl->skyView[i][3] = 0.0f;
    }
    FillPushers(fl, eye);
    fl->leaves[0] = m_leaf.on ? m_leaf.wind : 0.0f;
    fl->leaves[1] = m_leaf.on ? m_leaf.flutter : 0.0f;
    fl->leaves[2] = Gusts();
    fl->leaves[3] = m_leaf.on ? m_leaf.core : 0.0f;
    fl->leafView[0] = std::max(m_leaf.distance, 1.0f);
    fl->leafView[1] = m_leaf.on ? 1.0f : 0.0f;
    fl->leafView[2] = m_leaf.on ? m_leaf.fall : 0.0f;
    fl->leafView[3] = std::clamp(m_crownShade, 0.0f, 1.0f);   // (the crown shading, ffp.vert)
    fl->taa[0] = TaaActive() ? m_taaJitter[0] : 0.0f;
    fl->taa[1] = TaaActive() ? m_taaJitter[1] : 0.0f;
    fl->taa[2] = FrameNoise();                   // noise patterns move on each frame (averaged by the TAA)
    fl->taa[3] = float(m_windTimePrev);         // the wind's time last frame (plants' motion vectors)
    fl->shadowParams[0] = m_shadowValid ? 1.0f : 0.0f;
    // (The moon's shadows come in with its light: at full strength from the first frame they switched on at dusk.)
    fl->shadowParams[1] = m_shadowValid && m_shadowIsMoon ? 0.8f * m_shadowStrength * std::min(2.0f * m_moonFade, 1.0f)
                                                          : m_shadowStrength;
    // (Unfaded: as the light nears the horizon they fade out by distance from the eye - frame_lights.glsl ShadowStrength.)
    fl->shadowParams[2] = m_shadowValid ? float(m_cascadeCount) : 0.0f;   // (none: no map - the terrain's shade alone)
    fl->shadowParams[3] = PointShadowStrength();
    // The sun the shadow map was drawn with (shadows lag a frame), or without shadows this frame's.
    const float* sunDir = m_shadowValid ? m_shadowSunDir : m_frameSunDir;
    fl->sunDir[0] = sunDir[0]; fl->sunDir[1] = sunDir[1]; fl->sunDir[2] = sunDir[2];
    fl->sunDir[3] = m_hdr ? m_hdrHeadroom : m_lightHeadroom;
    for (int i = 0; i < 3; ++i) fl->sunColor[i] = m_shadowValid ? m_shadowSunColor[i] : m_frameSunColor[i];
    fl->sunColor[3] = m_frameSunLuma;            // (the clouds' light: not dimmed by the clouds hiding the sun)
    // A thunderstorm's deck hides the sun: its light on the grass, the clouds, the air and the water mostly gone
    // (lighting.glsl and the ground's shade take the rest - FL.rainNear.y).
    const float overcast = StormOvercast();
    // (0.9 of it: with 0.65 taken, rocks facing the hidden sun stood out light under a black storm sky at noon - user,
    // 2026-10-19; an overcast leaves a tenth or so of the light, and soft - lighting.glsl.)
    for (int i = 0; i < 3; ++i) fl->sunColor[i] *= (1.0f - kStormSunTaken * overcast) * m_dawnTint[0][i];   // (a low sun's: warm)
    // The moon's light on the ground and grass (night.cpp); whether the shadow map is its. The sun's true direction
    // (below the horizon too) for the clouds' light.
    for (int i = 0; i < 3; ++i) fl->moon[i] = m_moonColour[i];
    fl->moon[3] = m_shadowValid && m_shadowIsMoon ? 1.0f : 0.0f;
    {
        // (The sun's clock's, eased - never the shadow light's: by night that's the moon.)
        const float* d = m_trueSunDir;
        const float l = std::max(std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), 1e-6f);
        for (int i = 0; i < 3; ++i) fl->sunTrue[i] = -d[i] / l;
        fl->sunTrue[3] = night;
    }
    const float phase[4] = {m_cond.golden, m_cond.afterglow, m_cond.blueHour, m_cond.night};   // (the dusk's phases)
    std::memcpy(fl->phase, phase, sizeof(fl->phase));
    fl->cond[0] = m_cond.cloudNight;             // (cloud_light.glsl Night())
    fl->cond[1] = m_rig.sunShare;                // (ffp_main RigOn, FlatShade; grass.vert)
    fl->cond[2] = m_rig.on ? 1.0f : 0.0f;
    for (int i = 0; i < 3; ++i) fl->rigSky[i] = m_rig.on ? m_rig.groundSky[i] : 0.0f;
    fl->rigSky[3] = 0.0f;
    fl->cond[3] = 0.0f;
    fl->screen[0] = SceneScaleX();
    fl->screen[1] = SceneScaleY();
    fl->screen[2] = SceneNormalsWanted() ? 1.0f : 0.0f;
    fl->screen[3] = 0.0f;
    {
        const uint32_t mask = RtShadowMaskBits();
        std::memcpy(&fl->rtShadow[0], &mask, 4);
        fl->rtShadow[1] = m_aoProjValid ? m_aoProj.m[2][2] : 0.0f;
        fl->rtShadow[2] = m_aoProjValid ? m_aoProj.m[3][2] : 0.0f;
        fl->rtShadow[3] = 0.0f;
    }
    fl->volume2[0] = std::clamp(m_volLamps, 0.0f, 4.0f);
    // A lamp's light in the air at RVK_PtLight, indoors too: in a room the lamps light things at the game's strength
    // (LampScale), but a shop's big ceiling lamps at that strength filled it with a glowing haze.
    fl->volume2[1] = LampScale() > 0.0f ? m_pointLightScale / LampScale() : 1.0f;
    fl->volume2[2] = fl->volume2[3] = 0.0f;
    fl->foliage[0] = std::clamp(m_crownSky, 0.0f, 1.0f);
    fl->foliage[1] = std::clamp(m_crownDepth, 0.0f, 1.0f);
    fl->foliage[2] = fl->foliage[3] = 0.0f;
    std::memcpy(fl->skyRing, m_skyRing, sizeof(fl->skyRing));
    std::memcpy(fl->skyUp, m_skyUp, sizeof(fl->skyUp));
    if (!OwnSkyFog()) {                          // (indoors too: the zone's fog colour, not our sky's horizon)
        for (auto& r : fl->skyRing) r[3] = 0.0f;
        fl->skyUp[3] = 0.0f;
    }
    fl->terrain[0] = std::clamp(m_terrainSettings.cliff, 0.0f, 1.0f);
    fl->terrain[1] = std::clamp(m_terrainSettings.tiling, 0.0f, 1.0f);
    fl->terrain[2] = std::clamp(m_terrainSettings.variation, 0.0f, 2.0f);
    fl->terrain[3] = m_normalMaps && m_pixelLighting ? m_normalStrength : 0.0f;
    fl->terrain2[0] = std::clamp(m_native.ao, 0.0f, 1.0f);
    fl->terrain2[1] = fl->terrain2[2] = 0.0f;
    fl->terrain2[3] = std::clamp(m_terrainSettings.grain, 0.0f, 1.0f);
    fl->terrain3[0] = std::clamp(m_terrainSettings.blend, 0.0f, 1.0f);
    fl->terrain3[1] = m_normalMaps && m_pixelLighting ? 0.04f * std::clamp(m_terrainSettings.parallax, 0.0f, 2.0f) : 0.0f;
    fl->terrain3[2] = std::clamp(m_terrainSettings.parallaxSteps, 0.25f, 4.0f);
    fl->terrain3[3] = m_native.anyHeight && fl->terrain3[1] > 0.0f ? 1.0f : 0.0f;
    fl->sand[0] = std::clamp(m_terrainSettings.ripple, 0.0f, 1.0f);
    fl->sand[1] = std::clamp(m_terrainSettings.dune, 0.0f, 1.0f);
    fl->sand[2] = std::clamp(m_terrainSettings.glint, 0.0f, 1.0f);
    fl->sand[3] = SandWindBearing();
    fl->sand2[0] = std::clamp(m_terrainSettings.smooth, 0.0f, 1.0f);
    SandPrintLights(&fl->sand2[1]);
    fl->sand3[0] = m_normalMaps && m_pixelLighting ? std::clamp(m_terrainSettings.rippleDepth, 0.0f, 3.0f) : 0.0f;
    // (y: the sun the clouds let through towards it - MakeCloudSunRay -: the glints, the sun's own image, go with it.)
    fl->sand3[1] = m_cloudParams.enable ? std::clamp(m_sunThrough, 0.0f, 1.0f) : 1.0f;
    fl->sand3[2] = fl->sand3[3] = 0.0f;
    FogBankParams(fl->fogBanks);
    fl->pick[0] = m_pickHighlight ? float(m_pickHighlightTile) : 0.0f;
    fl->pick[1] = fl->pick[2] = fl->pick[3] = 0.0f;
    fl->lightGrid[0] = fl->lightGrid[1] = fl->lightGrid[2] = fl->lightGrid[3] = 0.0f;
    DesertAir(fl->desert);
    fl->desert[2] = m_aoProjValid ? m_aoProj.m[0][0] : 0.0f;
    fl->desert[3] = m_aoProjValid ? m_aoProj.m[1][1] : 0.0f;
    FillNativeLights(fl);
    FillTerrainRockLights(fl->terrainRock, fl->terrainRock2);
    fl->dusk[0] = std::clamp(m_afterglow, 0.0f, 2.0f);
    fl->dusk[1] = m_hdr ? m_blueGrade : 0.0f;
    // (The sun on the world camera's screen, NDC - the sunset rays fan from it: volume.frag. Behind the camera: far off.)
    fl->dusk[2] = fl->dusk[3] = 1e3f;
    if (m_aoProjValid) {
        const d3d::Matrix vp = MulMatrix(m_aoView, m_aoProj);
        float c[4];
        for (int k = 0; k < 4; ++k)
            c[k] = -m_trueSunDir[0] * vp.m[0][k] - m_trueSunDir[1] * vp.m[1][k] - m_trueSunDir[2] * vp.m[2][k];
        if (c[3] > 1e-4f) {
            fl->dusk[2] = std::clamp(c[0] / c[3], -1e3f, 1e3f);
            fl->dusk[3] = std::clamp(c[1] / c[3], -1e3f, 1e3f);
        }
    }
    {
        float ts[8];
        TerrainSunLights(ts);
        std::memcpy(fl->terrainSun, ts, sizeof(fl->terrainSun));
        std::memcpy(fl->terrainSun2, ts + 4, sizeof(fl->terrainSun2));
    }
    fl->pbr[0] = m_pbr.specular;
    fl->pbr[1] = m_pbr.ambient;
    fl->pbr[2] = m_ssr > 0.0f ? m_pbr.reflections : 0.0f;
    fl->pbr[3] = m_pbr.occlusion;
    fl->pbr2[0] = float(m_pbr.debug);
    fl->pbr2[1] = m_pbr.enabled ? 1.0f : 0.0f;
    fl->pbr2[2] = m_pbr.enabled ? m_pbr.emissive : 0.0f;
    // The environment probe's atlas (hdr.cpp RenderEnvProbe): bindless image slot + 1 (0 = none) in bits 0-19, the
    // linear sampler's slot in 20-27, its strength (0..1, 1/15 steps) in 28-31 - as the float's bits.
    uint32_t env = 0;
    if (m_envReady && m_envAtlas && m_pbr.enabled && m_pbr.probe > 0.0f) {
        const uint32_t image = BindlessImage(m_envAtlas), sampler = FixedSamplerSlot(2, m_linearSampler);
        if (image != ~0u && image < (1u << 20) - 1 && sampler < 256)
            env = (image + 1) | sampler << 20 | uint32_t(std::lround(std::clamp(m_pbr.probe, 0.0f, 1.0f) * 15.0f)) << 28;
    }
    std::memcpy(&fl->pbr2[3], &env, 4);
    // The clouds' shadow map (clouds.cpp), when made this frame or the last.
    uint32_t clouds = 0;
    std::memset(fl->clouds, 0, sizeof(fl->clouds) + sizeof(fl->clouds2));
    if (m_cloudShadowMap && m_cloudShadowFrame + 1 >= m_frameNumber && m_cloudParams.enable && m_cloudParams.shadow > 0.0f) {
        const uint32_t image = BindlessImage(m_cloudShadowMap), sampler = FixedSamplerSlot(2, m_linearSampler);
        if (image != ~0u && image < (1u << 20) - 1 && sampler < 256) {
            clouds = (image + 1) | sampler << 20;
            fl->clouds[0] = m_cloudShadowArea[0];
            fl->clouds[1] = m_cloudShadowArea[1];
            fl->clouds[2] = 1.0f / m_cloudShadowArea[2];
            fl->clouds2[0] = m_cloudShadowArea[3];
            fl->clouds2[1] = std::clamp(m_cloudParams.shadow, 0.0f, 1.0f);
        }
    }
    std::memcpy(&fl->clouds[3], &clouds, 4);
    // The game's sunlight on its clouds: its hue (cloud_light.glsl SunLight), when seen lately.
    if (m_cloudGameHueFrame != ~0ull && m_cloudGameHueFrame + 600 >= m_frameNumber) {
        uint32_t hue = 0;
        for (int j = 0; j < 3; ++j)
            hue |= uint32_t(std::lround(std::clamp(m_cloudGameHue[j], 0.0f, 1.0f) * 255.0f)) << (8 * j);
        std::memcpy(&fl->clouds2[2], &hue, 4);
        fl->clouds2[3] = 1.0f;
    }
    // The game's cloud sheets (RVK_CloudSheet), coloured as the layer's clouds.
    std::memcpy(fl->cloudTint, m_sheetTint, sizeof(fl->cloudTint));
    std::memcpy(fl->cloudSky, m_sheetSky, sizeof(fl->cloudSky));
    std::memcpy(fl->cloudAir, m_sheetAir, sizeof(fl->cloudAir));
    fl->cloudSky[3] = CloudSheetsOn() && m_sheetTint[3] > 0.0f ? std::min(m_cloudParams.sheet, 2.0f) * m_skySheets : 0.0f;
    fl->prevView = m_prevView;                   // the world camera last frame (motion vectors)
    fl->prevProj = m_prevProj;
    m_frameLightIndices.clear();
    m_frameLightSpheres.clear();
    // (RANDYVK_SPOT_CONE=0: spot lights by their range alone, as for their cubes - comparisons.)
    static const bool spotConeOff = [] { const char* v = std::getenv("RANDYVK_SPOT_CONE"); return v && *v == '0'; }();
    for (uint32_t k = 0; k < used; ++k) {
        const CapturedLight& c = m_lightsPrev[candidates[k].index];
        m_frameLightIndices.push_back(candidates[k].index);
        LightSphere sphere{c.light.position.x, c.light.position.y, c.light.position.z, c.light.range * c.light.range};
        if (const Cone cone = SpotCone(c.light); cone.on && !spotConeOff) {
            std::memcpy(sphere.axis, cone.axis, sizeof(sphere.axis));
            sphere.cosHalf = cone.cosHalf;
            sphere.sinHalf = cone.sinHalf;
            sphere.spot = true;
        }
        m_frameLightSpheres.push_back(sphere);
        FillGpuLight(c.light, c.cosHalfTheta, c.cosHalfPhi, fl->lights[k],
                     LampScale() + (m_charLightScale - LampScale()) * c.carried);
        fl->lights[k].ambient[3] = c.carried > 0.5f ? 1.0f : 0.0f;   // carried by a character (F_CHARACTER draws)
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
    BuildLightGrid();
    LightGridLights(fl);
}

void Device::LightGridLights(FrameLights* fl)
{
    if (m_lightGridTexFrame != m_frameNumber) {   // (once a frame: the frame's lights are the last frame's, fixed)
        m_lightGridTexFrame = m_frameNumber;
        m_lightGridTexValid = false;
        const uint32_t cells = m_lightGridW * m_lightGridH;
        if (cells && cells <= kLightGridMaxCells) {
            const uint32_t rows = (cells + kLightGridTexCells - 1) / kLightGridTexCells;
            if (!m_lightGridTex)
                m_lightGridTex = CreateTexture(2 * kLightGridTexCells, kLightGridMaxCells / kLightGridTexCells,
                                               Format::A8R8G8B8, 1);
            if (m_lightGridTex) {
                std::vector<uint32_t> texels(size_t(rows) * 2 * kLightGridTexCells, 0u);
                for (uint32_t i = 0; i < cells; ++i) {
                    texels[size_t(i) * 2] = uint32_t(m_lightGrid[i]);
                    texels[size_t(i) * 2 + 1] = uint32_t(m_lightGrid[i] >> 32);
                }
                UpdateTexture(m_lightGridTex, 0, 0, 0, 2 * kLightGridTexCells, rows, texels.data(),
                              2 * kLightGridTexCells * 4);
                m_lightGridTexValid = true;
            }
        }
    }
    if (!m_lightGridTexValid || !m_lightGridTex)
        return;
    const uint32_t image = BindlessImage(m_lightGridTex), sampler = FixedSamplerSlot(2, m_linearSampler);   // (texelFetch)
    if (image == ~0u || image >= (1u << 20) - 1 || sampler >= 256)
        return;
    const uint32_t slot = (image + 1) | sampler << 20, lo = uint32_t(m_lightGridAlways), hi = uint32_t(m_lightGridAlways >> 32);
    std::memcpy(&fl->pick[1], &slot, 4);
    std::memcpy(&fl->pick[2], &lo, 4);
    std::memcpy(&fl->pick[3], &hi, 4);
    fl->lightGrid[0] = float(m_lightGridX0) * kLightGridCell;
    fl->lightGrid[1] = float(m_lightGridZ0) * kLightGridCell;
    fl->lightGrid[2] = float(m_lightGridW);
    fl->lightGrid[3] = float(m_lightGridH);
}

// Identity of a ground chunk within a frame: its sizes, its vertices - a static snapshot's serial and place, else a
// sample of them - and a sample of its indices (base and lighting passes draw the same). The game draws its ground as
// small index lists into big static vertex buffers: by sizes and vertices alone, the chunks of one buffer with the
// same index count were one chunk (one base texture; the grass captured under the first of them only).
uint64_t Device::TerrainChunkKey(const void* vertices, uint32_t vertexCount, uint32_t stride, const uint16_t* indices,
                                 uint32_t indexCount) const
{
    uint64_t h = 1469598103934665603ull ^ (uint64_t(vertexCount) << 32) ^ indexCount;
    if (m_drawStaticBuffer) {
        h = (h ^ m_drawStaticSerial) * 1099511628211ull;
        h = (h ^ m_drawStaticOffset) * 1099511628211ull;
    } else {
        const uint8_t* v = static_cast<const uint8_t*>(vertices);
        uint32_t step = vertexCount > 32 ? vertexCount / 32 : 1;
        for (uint32_t i = 0; i < vertexCount; i += step) {   // the position as three words, not twelve bytes
            uint32_t w[3];
            std::memcpy(w, v + size_t(i) * stride, sizeof(w));
            h = (h ^ w[0]) * 1099511628211ull;
            h = (h ^ w[1]) * 1099511628211ull;
            h = (h ^ w[2]) * 1099511628211ull;
        }
    }
    if (indices) {
        uint32_t step = indexCount > 16 ? indexCount / 16 : 1;
        for (uint32_t i = 0; i < indexCount; i += step)
            h = (h ^ (indices[i] | uint64_t(i) << 16)) * 1099511628211ull;
    }
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
           AoCamera();
}

bool Device::AoCamera() const
{
    if (m_aoCameraGen != m_cameraGen) {
        m_aoCameraSame = std::memcmp(&m_proj, &m_aoProj, sizeof(m_proj)) == 0 &&
                         std::memcmp(&m_view, &m_aoView, sizeof(m_view)) == 0;
        m_aoCameraGen = m_cameraGen;
    }
    return m_aoCameraSame;
}

// A mesh's identity across frames, independent of where it is and of its (CPU-skinned) vertices: format, sizes,
// a sample of its indices, its texture.
uint64_t Device::MotionKey(uint32_t primitive, uint32_t fvf, uint32_t vertexCount, const uint16_t* indices,
                           uint32_t indexCount) const
{
    // A static (never-changing) mesh's identity is already known - DrawMeshInfo's key is stable across frames - so
    // hashing a sample of its indices here again is wasted work. Animated ones (their vertices change) hash here.
    if (m_drawMeshStatic && m_drawMesh) {
        uintptr_t texture = reinterpret_cast<uintptr_t>(m_textures[0]);
        return (m_drawMeshKey ^ uint64_t(texture)) * 1099511628211ull;
    }
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
bool Device::IsInterfaceDraw(uint32_t fvf) const
{
    return (fvf & d3d::FVF_POSITION_MASK) == d3d::FVF_XYZRHW && !IsWater(fvf);
}

// Whether a mesh is one plane: 95% of its triangles' area faces one way (either side) and its vertices lie within 6%
// of its size from that plane - a sign, a banner, a poster, a window's frame. Positions first in the vertex (FVF_XYZ).
bool detail::OnePlane(uint32_t primitive, uint32_t stride, const void* vertices, uint32_t vertexCount,
                      const uint16_t* indices, uint32_t indexCount)
{
    const auto* base = static_cast<const uint8_t*>(vertices);
    auto pos = [&](uint32_t i, float p[3]) { std::memcpy(p, base + size_t(i) * stride, 12); };
    const uint32_t n = indices ? indexCount : vertexCount;
    const uint32_t triangles = primitive == d3d::TriangleList ? n / 3 : n >= 3 ? n - 2 : 0;
    if ((primitive != d3d::TriangleList && primitive != d3d::TriangleStrip && primitive != d3d::TriangleFan) || !triangles)
        return false;
    auto corner = [&](uint32_t t, int k) {
        uint32_t j = primitive == d3d::TriangleList ? t * 3 + k : primitive == d3d::TriangleStrip ? t + k : k == 0 ? 0 : t + k;
        return indices ? uint32_t(indices[j]) : j;
    };
    // The normal of the largest triangle, then the share of the area facing the same way.
    float axis[3] = {}, best = 0.0f, total = 0.0f;
    std::vector<float> normals(size_t(triangles) * 4, 0.0f);
    for (uint32_t t = 0; t < triangles; ++t) {
        uint32_t i0 = corner(t, 0), i1 = corner(t, 1), i2 = corner(t, 2);
        if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount)
            continue;
        float a[3], b[3], c[3];
        pos(i0, a); pos(i1, b); pos(i2, c);
        const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        float nx = e1[1] * e2[2] - e1[2] * e2[1], ny = e1[2] * e2[0] - e1[0] * e2[2], nz = e1[0] * e2[1] - e1[1] * e2[0];
        const float area = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (area <= 1e-12f)
            continue;
        float* o = &normals[size_t(t) * 4];
        o[0] = nx / area; o[1] = ny / area; o[2] = nz / area; o[3] = area;
        total += area;
        if (area > best) { best = area; axis[0] = o[0]; axis[1] = o[1]; axis[2] = o[2]; }
    }
    if (total <= 0.0f)
        return false;
    float facing = 0.0f;
    for (uint32_t t = 0; t < triangles; ++t) {
        const float* o = &normals[size_t(t) * 4];
        if (std::fabs(o[0] * axis[0] + o[1] * axis[1] + o[2] * axis[2]) > 0.9f) facing += o[3];
    }
    if (facing < 0.95f * total)
        return false;
    float lo = 1e30f, hi = -1e30f, mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
    for (uint32_t i = 0; i < vertexCount; ++i) {
        float p[3];
        pos(i, p);
        const float d = p[0] * axis[0] + p[1] * axis[1] + p[2] * axis[2];
        lo = std::min(lo, d); hi = std::max(hi, d);
        for (int k = 0; k < 3; ++k) { mn[k] = std::min(mn[k], p[k]); mx[k] = std::max(mx[k], p[k]); }
    }
    const float size = std::sqrt((mx[0] - mn[0]) * (mx[0] - mn[0]) + (mx[1] - mn[1]) * (mx[1] - mn[1]) +
                                 (mx[2] - mn[2]) * (mx[2] - mn[2]));
    return hi - lo < 0.06f * size;
}

// Plants that sway in the wind: small (0.2 - 3 units tall, at most 6 across), lit, drawn into the scene with a
// cut-out texture (the vertex shader keeps those whose texture is mostly holes - leaves, grass), not one flat plane
// (signs, banners, posters - from a survey of the game's 10,600 static meshes: a third of the non-plants that passed
// the rest, a few single-card plants), and static: their vertices are the same as last frame (a character's hair or
// cloak is CPU-skinned, its vertices change every frame). Never a character or what it carries: a held weapon is
// rigid (moved by its world matrix), blended with holes and plant-sized - hanging upright (sitting down) it passed as
// a plant, and the grass push bent it away from its own carrier. out: DrawTransform sway.
bool Device::SwayParams(uint32_t primitive, uint32_t fvf, uint32_t stride, const void* vertices, uint32_t vertexCount,
                        const uint16_t* indices, uint32_t indexCount, float out[4])
{
    const auto kind = VisualKind(m_drawVisualKind);
    if (kind == VisualKind::Character || kind == VisualKind::CharacterPart)
        return false;
    if ((m_sway <= 0.0f && m_grassPush <= 0.0f) || m_target != m_scene || !m_textures[0] || !m_rs[d3d::RS_LIGHTING] || m_drawIsLabel ||
        (fvf & d3d::FVF_POSITION_MASK) != d3d::FVF_XYZ || IsTerrain(fvf) || !m_rs[d3d::RS_ZWRITEENABLE] ||
        !(m_rs[d3d::RS_ALPHATESTENABLE] || m_rs[d3d::RS_ALPHABLENDENABLE]) || vertexCount < 3 || vertexCount > 4096 ||
        !m_drawMesh)
        return false;
    // A texture with no transparent texel never moves: the shaders scale sway and push by its holes (ffp.vert
    // SwayHoles(mean alpha) = 0). Most blended statics of plant size are such props - not split, kept on
    // the GPU and in the depth pre-pass.
    if (m_textures[0]->m_opaque)
        return false;
    // Nor a soft card - a lamp's glow or beam, smoke, a halo: no hard edges between leaf and hole (leaves.cpp).
    if (!m_textures[0]->m_alphaMask.empty() && m_textures[0]->m_alphaHardEdges < 0.002f)
        return false;
    // Static: the same vertices for the last two frames at least (the mesh cache's fingerprint).
    if (!m_drawMeshStatic || m_drawMesh->firstFrame + 2 > m_frameNumber)
        return false;
    // The model axis that points most nearly up in the world (not always y), and which way.
    const auto& w = m_world.m;
    int axis = 0;
    float best = -1.0f, scale = 0.0f;
    for (int i = 0; i < 3; ++i) {
        float len = std::sqrt(w[i][0] * w[i][0] + w[i][1] * w[i][1] + w[i][2] * w[i][2]);
        float up = len > 0.0f ? std::fabs(w[i][1]) / len : 0.0f;
        if (up > best) { best = up; axis = i; scale = len; }
    }
    if (best < 0.7f)
        return false;                            // lying on its side
    float lo = m_drawMesh->boundsMin[axis], hi = m_drawMesh->boundsMax[axis];
    float height = (hi - lo) * scale;
    if (height < 0.2f || height > 3.0f)
        return false;
    // Across (the other two model axes, scaled): a plant this short isn't 6 units wide - a bridge, a railing, a rope
    // between posts is.
    for (int i = 0; i < 3; ++i) {
        if (i == axis)
            continue;
        const float len = std::sqrt(w[i][0] * w[i][0] + w[i][1] * w[i][1] + w[i][2] * w[i][2]);
        if ((m_drawMesh->boundsMax[i] - m_drawMesh->boundsMin[i]) * len > 6.0f)
            return false;
    }
    if (m_drawMesh->flatShape < 0)
        m_drawMesh->flatShape = OnePlane(primitive, stride, vertices, vertexCount, indices, indexCount) ? 1 : 0;
    if (m_drawMesh->flatShape == 1)
        return false;
    bool down = w[axis][1] < 0.0f;
    out[0] = down ? hi : lo;
    out[1] = (down ? -1.0f : 1.0f) / (hi - lo);
    out[2] = 0.06f * m_sway * height;
    out[3] = 1.0f + float(axis);
    return true;
}

// Plants pushed aside by characters. A character is drawn as CPU-skinned parts: lit world meshes, depth-writing,
// whose vertices are new this frame. A static mesh is new on the frame it first appears too, so a candidate counts
// only if its fingerprint isn't drawn again the next frame (UpdatePushTrail) - the feet lag two frames.
// A game light's intensity setting: directional ones as they are; a point / spot light carried by a character
// (the captured lights' carriers, matched by position and range) at m_charLightScale, others at LampScale (a room's
// lamps at the game's strength: a shop's props had a quarter of the light its walls and floor have baked in).
float Device::LightScale(const d3d::Light& l) const
{
    if (l.type == d3d::LIGHT_DIRECTIONAL)
        return 1.0f;
    const float lamp = LampScale();
    if (m_charLightScale != lamp)
        for (const CapturedLight& c : m_lightsPrev) {
            if (c.carried <= 0.0f || c.light.range != l.range) continue;
            float dx = c.light.position.x - l.position.x, dy = c.light.position.y - l.position.y,
                  dz = c.light.position.z - l.position.z;
            if (dx * dx + dy * dy + dz * dz < 1.0f)
                return lamp + (m_charLightScale - lamp) * c.carried;
        }
    return lamp;
}

// Phong tessellation of characters (RVK_Tess): the level for this draw, 0 for none. Characters are drawn as
// CPU-skinned meshes, new vertices every frame; a static mesh is new on the frame it appears too, so a mesh's topology
// (its indices) must have been drawn animated the frame before as well - a building doesn't round for a frame. Indexed
// or not, triangle lists only (3-point patches), lit and with normals (the shape follows them), near the camera.
// Is the current draw a character's (m_drawIsCharacter)? Characters are drawn as CPU-skinned meshes, new vertices
// every frame; a static mesh is new on the frame it appears too. A character's topology (its indices) is drawn
// animated on most frames, but in crowds the game skips some characters' animation now and then (the same vertices
// again: a static mesh for that frame). So a topology counts as a character's once it was animated on 3 frames within
// the last 30, and stays one for 10 frames after its last: a building is only "new" the frame it appears. (Characters
// sharing a model share this.) Lit world meshes, character-sized; plus a character's rigid parts - its head, hair, a
// helmet: the game moves those whole by their world matrix (same vertices every frame) and draws them before the
// body - small, in a body's column (last frame's characters) from its feet to a little above its top.
// Two-pass foliage isn't dithered far off when its texture is translucent all over: most of its visible cells partly
// see-through (Texture::m_alphaSoftShare) and hardly a hard edge between leaf and hole (m_alphaHardEdges) - the alien
// ships' door membranes (0.72 soft, 0.000 edges), panels of glass. Leaves have hard edges (a tree's 1024 x 1024 atlas:
// 0.68 soft on the coarse grid, but 0.226 edges).
constexpr float kDitherSoftShare = 0.5f, kDitherHardEdges = 0.01f;

bool Device::CharacterDraw(uint32_t fvf, uint32_t vertexCount)
{
    if (m_external || !m_drawMesh || m_drawIsLabel || !m_rs[d3d::RS_LIGHTING] ||
        (fvf & d3d::FVF_POSITION_MASK) != d3d::FVF_XYZ || (m_target != m_scene && m_target != m_main))
        return false;
    // Known from the game's visual: a character's body or a part attached to it; or known not to be one.
    const auto kind = VisualKind(m_drawVisualKind);
    if (kind == VisualKind::CharacterPart)
        return true;
    if (kind != VisualKind::Unknown && kind != VisualKind::Character && kind != VisualKind::Other)
        return false;
    if (m_drawSkin || kind == VisualKind::Character) {   // known to be a character: any size
        float c[3], e[3];
        DrawWorldBox(c, e);
        if (m_tessChars.size() < 1024)
            m_tessChars.push_back({c[0], c[2], c[1] - e[1], c[1] + e[1]});
        return true;
    }
    uint64_t key = m_drawMesh->indexHash ^ (uint64_t(fvf) << 40) ^ (uint64_t(vertexCount) * 0x9E3779B97F4A7C15ull);
    auto found = m_tessTopologies.find(key);
    if (!m_drawMeshStatic) {
        TessTopology& t = found != m_tessTopologies.end() ? found->second : m_tessTopologies[key];
        if (t.frames[0] != m_frameNumber) {              // newest first
            t.frames[2] = t.frames[1];
            t.frames[1] = t.frames[0];
            t.frames[0] = m_frameNumber;
        }
        found = m_tessTopologies.find(key);
    }
    bool animated = found != m_tessTopologies.end() && found->second.frames[2] != 0 &&
                    found->second.frames[2] + 30 >= m_frameNumber && found->second.frames[0] + 10 >= m_frameNumber;
    float c[3], e[3];
    DrawWorldBox(c, e);
    if (animated) {
        if (e[1] > 2.5f || e[0] > 2.0f || e[2] > 2.0f)    // character-sized (as the grass push's)
            return false;
        if (m_tessChars.size() < 1024)                   // where characters are, for their rigid parts next frame
            m_tessChars.push_back({c[0], c[2], c[1] - e[1], c[1] + e[1]});
        return true;
    }
    if (e[0] > 0.5f || e[1] > 0.5f || e[2] > 0.5f)
        return false;
    for (const TessCharacter& ch : m_tessCharsPrev) {
        float dx = c[0] - ch.x, dz = c[2] - ch.z;
        if (dx * dx + dz * dz < 0.6f * 0.6f && c[1] > ch.minY - 0.2f && c[1] < ch.maxY + 0.8f)
            return true;
    }
    return false;
}

// Phong tessellation of characters (RVK_Tess): the level for this draw, 0 for none. Characters (CharacterDraw) drawn
// as triangle lists (3-point patches), indexed or not, with normals (the shape follows them), near the camera -
// depth-writing, or a second pass over one with depth EQUAL (the Kyr'Ozch armour's additive shine): left flat, it met
// the rounded surface under it only here and there, and its shine came out as speckle. The same mesh at the same
// distance gets the same level and shape.
float Device::TessellateDraw(uint32_t primitive, uint32_t fvf, uint32_t vertexCount)
{
    const bool overPass = !m_rs[d3d::RS_ZWRITEENABLE] && m_rs[d3d::RS_ZFUNC] == d3d::CMP_EQUAL;
    if (m_tessShape <= 0.0f || !m_tessSupported || !m_drawIsCharacter || primitive != d3d::TriangleList ||
        !(m_rs[d3d::RS_ZWRITEENABLE] || overPass) || !(fvf & d3d::FVF_NORMAL) || vertexCount > 20000)
        return 0.0f;
    float c[3], e[3];
    DrawWorldBox(c, e);
    UpdateFrameEye();
    float d2 = 0.0f;
    for (int j = 0; j < 3; ++j) {
        float d = std::max(std::fabs(m_frameEye[j] - c[j]) - e[j], 0.0f);
        d2 += d * d;
    }
    float closeness = 1.0f - std::sqrt(d2) / std::max(m_tessDistance, 1.0f);
    float level = 1.0f + float(m_tessLevel - 1) * closeness;
    return level >= 1.5f ? level : 0.0f;
}

// The current draw's normals averaged over the vertices sharing a position (m_smoothNormals, model space): a hard
// edge or a seam has a vertex per side with its own normal, and the Phong shape following those would tear the
// surface open there. The average of unit normals is shorter the more they differ: its length says how smooth the
// surface is meant to be there (the shader rounds only smooth corners - a box, a blade's edge stay sharp).
// False without normals.
bool Device::SmoothNormals(const void* vertices, uint32_t vertexCount, const FvfLayout& layout)
{
    if (layout.offset[1] < 0 || !vertexCount)
        return false;
    const uint8_t* v = static_cast<const uint8_t*>(vertices);
    uint32_t size = 64;
    while (size < vertexCount * 2) size *= 2;
    m_smoothTable.assign(size, -1);
    m_smoothNormals.assign(size_t(vertexCount) * 3, 0.0f);
    std::vector<int32_t> owner(vertexCount);       // the first vertex at each one's position
    std::vector<uint16_t> count(vertexCount, 0);
    auto quant = [](float f) { return int32_t(std::lround(f * 2048.0f)); };   // positions within 1/2048 are one
    for (uint32_t i = 0; i < vertexCount; ++i) {
        float p[3], n[3];
        std::memcpy(p, v + size_t(i) * layout.stride, 12);
        std::memcpy(n, v + size_t(i) * layout.stride + layout.offset[1], 12);
        int32_t q[3] = {quant(p[0]), quant(p[1]), quant(p[2])};
        uint32_t h = (uint32_t(q[0]) * 73856093u) ^ (uint32_t(q[1]) * 19349663u) ^ (uint32_t(q[2]) * 83492791u);
        int32_t found = -1;
        for (uint32_t slot = h & (size - 1);; slot = (slot + 1) & (size - 1)) {
            int32_t o = m_smoothTable[slot];
            if (o < 0) { m_smoothTable[slot] = int32_t(i); break; }
            float op[3];
            std::memcpy(op, v + size_t(o) * layout.stride, 12);
            if (quant(op[0]) == q[0] && quant(op[1]) == q[1] && quant(op[2]) == q[2]) { found = o; break; }
        }
        owner[i] = found < 0 ? int32_t(i) : found;
        float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (len > 0.0f) {
            for (int j = 0; j < 3; ++j) m_smoothNormals[size_t(owner[i]) * 3 + j] += n[j] / len;
            ++count[owner[i]];
        }
    }
    for (uint32_t i = 0; i < vertexCount; ++i)           // the owners' sums -> means (unit vectors averaged)
        if (owner[i] == int32_t(i) && count[i] > 1)
            for (int j = 0; j < 3; ++j) m_smoothNormals[size_t(i) * 3 + j] /= float(count[i]);
    for (uint32_t i = 0; i < vertexCount; ++i)
        if (owner[i] != int32_t(i))
            for (int j = 0; j < 3; ++j) m_smoothNormals[size_t(i) * 3 + j] = m_smoothNormals[size_t(owner[i]) * 3 + j];
    return true;
}

// SmoothNormals for a skinned piece (m_drawSkin): the vertices sharing a position are the same in every pose, so they
// are found once per mesh, in its rest pose; each draw only averages its normals over them.
bool Device::SmoothNormalsSkinned(const void* vertices, uint32_t startVertex, uint32_t vertexCount)
{
    const auto& source = m_drawSkin->source;
    if (!source || size_t(startVertex) + vertexCount > source->vertices.size())
        return false;
    SmoothOwners& so = m_smoothOwners[source.get()];
    if (so.source != source) {                           // new (or another mesh at the same address)
        so.source = source;
        const std::vector<skin::TriVertex>& in = source->vertices;
        uint32_t n = uint32_t(in.size()), size = 64;
        while (size < n * 2) size *= 2;
        std::vector<int32_t> table(size, -1);
        so.owner.assign(n, 0);
        auto quant = [](float f) { return int32_t(std::lround(f * 2048.0f)); };
        for (uint32_t i = 0; i < n; ++i) {
            int32_t q[3] = {quant(in[i].bind[0]), quant(in[i].bind[1]), quant(in[i].bind[2])};
            uint32_t h = (uint32_t(q[0]) * 73856093u) ^ (uint32_t(q[1]) * 19349663u) ^ (uint32_t(q[2]) * 83492791u);
            int32_t found = -1;
            for (uint32_t slot = h & (size - 1);; slot = (slot + 1) & (size - 1)) {
                int32_t o = table[slot];
                if (o < 0) { table[slot] = int32_t(i); break; }
                if (quant(in[o].bind[0]) == q[0] && quant(in[o].bind[1]) == q[1] && quant(in[o].bind[2]) == q[2]) {
                    found = o;
                    break;
                }
            }
            so.owner[i] = found < 0 ? int32_t(i) : found;
        }
    }
    so.lastFrame = m_frameNumber;
    // Sums per owner, then means, then copied to the others (as SmoothNormals). Owners before startVertex (a partial
    // draw) count as their own group.
    const auto* v = static_cast<const skin::Vertex*>(vertices);
    m_smoothNormals.assign(size_t(vertexCount) * 3, 0.0f);
    static thread_local std::vector<uint16_t> count;
    count.assign(vertexCount, 0);
    auto ownerOf = [&](uint32_t i) {
        int32_t o = so.owner[startVertex + i] - int32_t(startVertex);
        return o >= 0 ? uint32_t(o) : i;
    };
    for (uint32_t i = 0; i < vertexCount; ++i) {
        const float* n = v[i].normal;
        float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (len > 0.0f) {
            uint32_t o = ownerOf(i);
            for (int j = 0; j < 3; ++j) m_smoothNormals[size_t(o) * 3 + j] += n[j] / len;
            ++count[o];
        }
    }
    for (uint32_t i = 0; i < vertexCount; ++i)
        if (ownerOf(i) == i && count[i] > 1)
            for (int j = 0; j < 3; ++j) m_smoothNormals[size_t(i) * 3 + j] /= float(count[i]);
    for (uint32_t i = 0; i < vertexCount; ++i) {
        uint32_t o = ownerOf(i);
        if (o != i)
            for (int j = 0; j < 3; ++j) m_smoothNormals[size_t(i) * 3 + j] = m_smoothNormals[size_t(o) * 3 + j];
    }
    if ((m_frameNumber & 255) == 0)                      // meshes not drawn for a while
        for (auto it = m_smoothOwners.begin(); it != m_smoothOwners.end();)
            it = it->second.lastFrame + 600 < m_frameNumber ? m_smoothOwners.erase(it) : std::next(it);
    return true;
}

// The current draw is further from the camera than the foliage level of detail's distance (its box's nearest point).
bool Device::FoliageFar() const
{
    return m_foliageLod > 0.0f && DrawFartherThan(m_foliageLod);
}

// The current draw's mesh box (its nearest point) is further than `distance` from the camera; false when unknown.
bool Device::DrawFartherThan(float distance) const
{
    if (!m_drawMesh || !m_frameEyeValid || m_target != m_scene)
        return false;
    float c[3], e[3], d2 = 0.0f;
    DrawWorldBox(c, e);
    for (int j = 0; j < 3; ++j) {
        float d = std::max(std::fabs(m_frameEye[j] - c[j]) - e[j], 0.0f);
        d2 += d * d;
    }
    return d2 > distance * distance;
}

// The current draw's mesh box in the world: centre and half extents (the model box's centre moved, its half extents
// through the matrix's magnitudes).
void Device::DrawWorldBox(float c[3], float e[3]) const
{
    if (m_drawObject) {                          // (kept with the object: objects.cpp)
        std::memcpy(c, m_drawObject->boxCentre, 12);
        std::memcpy(e, m_drawObject->boxExtent, 12);
        return;
    }
    const auto& w = m_world.m;
    float mc[3], me[3];
    for (int i = 0; i < 3; ++i) {
        mc[i] = 0.5f * (m_drawMesh->boundsMin[i] + m_drawMesh->boundsMax[i]);
        me[i] = 0.5f * (m_drawMesh->boundsMax[i] - m_drawMesh->boundsMin[i]);
    }
    for (int j = 0; j < 3; ++j) {
        c[j] = mc[0] * w[0][j] + mc[1] * w[1][j] + mc[2] * w[2][j] + w[3][j];
        e[j] = me[0] * std::fabs(w[0][j]) + me[1] * std::fabs(w[1][j]) + me[2] * std::fabs(w[2][j]);
    }
}

// The frame's pushers (FrameLights order) within `margin` of the current draw's box, as a bit mask - a plant far from
// every character skips the push entirely (ffp.vert), and needs no splitting (SubdividePlant).
uint32_t Device::PusherMask(float margin) const
{
    if (!m_drawMesh || m_framePushers.empty())
        return 0;
    float c[3], e[3];
    DrawWorldBox(c, e);
    uint32_t mask = 0;
    for (size_t i = 0; i < m_framePushers.size(); ++i) {
        const float* p = m_framePushers[i].p;
        float dx = std::max(std::fabs(p[0] - c[0]) - e[0], 0.0f), dz = std::max(std::fabs(p[2] - c[2]) - e[2], 0.0f);
        float dy = p[1] - c[1];
        if (dx * dx + dz * dz < margin * margin && dy > -e[1] - 3.0f && dy < e[1] + 1.0f)
            mask |= 1u << i;
    }
    return mask;
}

void Device::PushCandidateDraw(uint32_t fvf)
{
    if ((m_grassPush <= 0.0f && !SandPrintsOn()) || !m_drawMesh || m_drawMeshStatic || m_target != m_scene || m_drawIsLabel ||
        !m_rs[d3d::RS_LIGHTING] || !m_rs[d3d::RS_ZWRITEENABLE] || IsTerrain(fvf) || m_pushNew.size() >= 4096)
        return;
    float c[3], e[3];
    DrawWorldBox(c, e);
    // Character-sized: not a whole animated scene (a flag, water), not a speck.
    if (e[1] > 2.5f || e[0] > 2.0f || e[2] > 2.0f || e[0] + e[1] + e[2] < 0.05f)
        return;
    m_pushNew.push_back({m_drawMeshKey, c[0], c[1] - e[1], c[2]});
}

void Device::UpdatePushTrail()
{
    constexpr float kMerge = 1.0f;               // parts this close (x, z) are one character
    constexpr float kSpacing = 0.35f;            // a new trail point once a character is this far from the last
    constexpr double kRecover = 1.6;             // seconds for a pushed plant to settle again (ffp.vert PushSpring)
    double now = SwayClock();
    // The frame before last's candidates: animated if their mesh wasn't drawn again last frame.
    struct Character { float x, y, z; int parts; };
    std::vector<Character> characters;
    for (const PushCandidate& p : m_pushOld) {
        auto it = m_meshInfo.find(p.mesh);
        if (it == m_meshInfo.end() || it->second.lastFrame + 2 != m_frameNumber)
            continue;
        Character* into = nullptr;
        for (Character& c : characters) {
            float dx = c.x / c.parts - p.x, dz = c.z / c.parts - p.z;
            if (dx * dx + dz * dz < kMerge * kMerge && std::fabs(c.y - p.y) < 2.0f) { into = &c; break; }
        }
        if (!into) { characters.push_back({p.x, p.y, p.z, 1}); continue; }
        into->x += p.x; into->z += p.z; into->y = std::min(into->y, p.y); ++into->parts;
    }
    m_pushOld.swap(m_pushNew);
    m_pushNew.clear();
    // Each character moves its head point (the nearest head within a step) to where it is now, every frame - the push
    // follows it smoothly - and drops a trail point behind each time it has gone kSpacing further. No head near: a
    // new one. A head left behind (its character gone) ages like a trail point.
    constexpr float kFollow = 1.0f;              // furthest a character moves between two frames it is seen in
    size_t heads = m_pushTrail.size();
    std::vector<bool> claimed(heads, false);
    for (const Character& c : characters) {
        float x = c.x / c.parts, z = c.z / c.parts;
        size_t nearest = heads;
        float best = kFollow * kFollow;
        for (size_t i = 0; i < heads; ++i) {
            const PushPoint& t = m_pushTrail[i];
            float dx = t.x - x, dz = t.z - z, d = dx * dx + dz * dz;
            if (t.head && !claimed[i] && d < best && std::fabs(t.y - c.y) < 1.5f) { best = d; nearest = i; }
        }
        if (nearest == heads) {
            m_pushTrail.push_back({x, c.y, z, now, now, x, z, true, x, z});
            continue;
        }
        claimed[nearest] = true;
        PushPoint& h = m_pushTrail[nearest];
        if (std::sqrt(best) > 0.01f)
            h.born = now;                        // moving: the plants it touches rustle
        h.x = x; h.y = c.y; h.z = z; h.time = now;
        // Its footprints (sand_prints.cpp): a step on from the last, beside its way, left and right by turns. (Only its
        // body's centre is known, not its feet - a print a step, under where it was; a jump far away starts afresh.)
        if (SandPrintsOn()) {
            constexpr float kStep = 0.7f, kApart = 0.11f;
            const float px = x - h.printX, pz = z - h.printZ, d = std::sqrt(px * px + pz * pz);
            if (d > 4.0f) {
                h.printX = x; h.printZ = z;
            } else if (d >= kStep) {
                const float ux = px / d, uz = pz / d;
                m_printsNew.push_back({x - uz * kApart * h.printSide, z + ux * kApart * h.printSide, ux, uz});
                h.printX = x; h.printZ = z;
                h.printSide = -h.printSide;
            }
        }
        float dx = x - h.dropX, dz = z - h.dropZ;
        if (dx * dx + dz * dz >= kSpacing * kSpacing) {
            m_pushTrail.push_back({x, c.y, z, now, now, x, z, false});
            m_pushTrail[nearest].dropX = x;      // (push_back may have moved the head)
            m_pushTrail[nearest].dropZ = z;
        }
    }
    m_pushTrail.erase(std::remove_if(m_pushTrail.begin(), m_pushTrail.end(),
                                     [&](const PushPoint& t) { return now - t.time > kRecover || now < t.time; }),
                      m_pushTrail.end());
    if (m_pushTrail.size() > 512)                // drop the oldest trail points, keep the heads
        for (auto it = m_pushTrail.begin(); m_pushTrail.size() > 512 && it != m_pushTrail.end();)
            it = it->head ? std::next(it) : m_pushTrail.erase(it);
}

// The trail points for this frame's plants: those nearest the camera, fresher ones first. The shader turns a point's
// age into how far the plants still bend (springing back behind a character).
void Device::FillPushers(detail::FrameLights* fl, const float eye[3])
{
    fl->info[1] = 0;
    m_framePushers.clear();
    if (m_grassPush <= 0.0f || m_pushTrail.empty())
        return;
    double now = SwayClock();
    struct Item { float key; const PushPoint* p; };
    std::vector<Item> items;
    items.reserve(m_pushTrail.size());
    // The characters where they are now (heads seen this frame) first, nearest the camera first; then the trail points,
    // fresher and nearer first. Ranked together, a few walkers near the camera dropped enough trail points to take
    // every slot from the player's own character a dozen units away (zoomed out): the grass stood up through it.
    for (const PushPoint& t : m_pushTrail) {
        float dx = t.x - eye[0], dy = t.y - eye[1], dz = t.z - eye[2];
        const bool here = t.head && now - t.time < 0.1;
        items.push_back({(here ? 0.0f : 1e4f) + std::sqrt(dx * dx + dy * dy + dz * dz) + 4.0f * float(now - t.time), &t});
    }
    uint32_t used = std::min<uint32_t>(uint32_t(items.size()), detail::kPushers);
    std::partial_sort(items.begin(), items.begin() + used, items.end(),
                      [](const Item& a, const Item& b) { return a.key < b.key; });
    for (uint32_t i = 0; i < used; ++i) {
        fl->pushers[i][0] = items[i].p->x;
        fl->pushers[i][1] = items[i].p->y;
        fl->pushers[i][2] = items[i].p->z;
        fl->pushers[i][3] = float(now - items[i].p->time);
        fl->pusherBorn[i] = float(now - items[i].p->born);
        m_framePushers.push_back({{items[i].p->x, items[i].p->y, items[i].p->z}});
    }
    fl->info[1] = used;
    if (used > m_pusherSeenMax)
        m_pusherSeenMax = used;
}

// Splits a plant's triangles evenly (each into n x n, all its vertex data interpolated) so that the pieces are about
// kPlantCell / RVK_PlantDetail world units across: the wind and the push move vertices, and a big quad with only its
// four corners could only tilt and shear as a whole. One n for the whole mesh - shared edges split alike, no cracks.
// The result (a triangle list) is cached by the mesh and n; the draw continues with it.
void Device::SubdividePlant(uint32_t& primitive, uint32_t fvf, const FvfLayout& layout, const void*& vertices,
                            uint32_t& vertexCount, const uint16_t*& indices, uint32_t& indexCount)
{
    constexpr float kPlantCell = 0.3f;
    constexpr uint32_t kMaxSplit = 8, kMaxVertices = 30000;
    if (m_plantDetail <= 0.0f || !m_drawMesh || primitive < d3d::TriangleList || primitive > d3d::TriangleFan)
        return;
    // Only plants a character is near (the push's reach and then some): the wind alone bends a plant evenly enough.
    if (!PusherMask(1.4f * std::sqrt(std::max(m_grassPush, 0.0f)) + 1.0f))
        return;
    // The mesh's triangles as index triples (the game's list, strip or fan).
    uint32_t count = indices ? indexCount : vertexCount;
    auto at = [&](uint32_t i) -> uint32_t { return indices ? indices[i] : i; };
    auto forEachTriangle = [&](auto&& fn) {
        if (primitive == d3d::TriangleList)
            for (uint32_t i = 0; i + 2 < count; i += 3) fn(at(i), at(i + 1), at(i + 2));
        else if (primitive == d3d::TriangleStrip)
            for (uint32_t i = 0; i + 2 < count; ++i)
                (i & 1) ? fn(at(i + 1), at(i), at(i + 2)) : fn(at(i), at(i + 1), at(i + 2));
        else
            for (uint32_t i = 1; i + 1 < count; ++i) fn(at(0), at(i), at(i + 1));
    };
    const uint8_t* src = static_cast<const uint8_t*>(vertices);
    auto pos = [&](uint32_t v, float out[3]) { std::memcpy(out, src + size_t(v) * layout.stride, 12); };
    // The longest edge (model units), once per mesh.
    auto edge = m_plantMaxEdge.find(m_drawMeshKey);
    if (edge == m_plantMaxEdge.end()) {
        float longest = 0.0f;
        forEachTriangle([&](uint32_t a, uint32_t b, uint32_t c) {
            if (a >= vertexCount || b >= vertexCount || c >= vertexCount) return;
            float p[3][3];
            pos(a, p[0]); pos(b, p[1]); pos(c, p[2]);
            for (int e = 0; e < 3; ++e) {
                const float* u = p[e];
                const float* w = p[(e + 1) % 3];
                float dx = u[0] - w[0], dy = u[1] - w[1], dz = u[2] - w[2];
                longest = std::max(longest, dx * dx + dy * dy + dz * dz);
            }
        });
        edge = m_plantMaxEdge.emplace(m_drawMeshKey, std::sqrt(longest)).first;
    }
    const auto& w = m_world.m;
    float scale = 0.0f;
    for (int i = 0; i < 3; ++i) scale = std::max(scale, std::sqrt(w[i][0] * w[i][0] + w[i][1] * w[i][1] + w[i][2] * w[i][2]));
    uint32_t triangles = primitive == d3d::TriangleList ? count / 3 : count >= 3 ? count - 2 : 0;
    uint32_t n = uint32_t(std::ceil(edge->second * scale * m_plantDetail / kPlantCell));
    n = std::min(n, kMaxSplit);
    while (n > 1 && size_t(triangles) * (n + 1) * (n + 2) / 2 > kMaxVertices)
        --n;
    if (n <= 1)
        return;
    uint64_t key = m_drawMeshKey ^ (uint64_t(n) * 0x9E3779B97F4A7C15ull);
    auto it = m_plantMeshes.find(key);
    if (it == m_plantMeshes.end()) {
        PlantMesh mesh;
        const uint32_t stride = layout.stride, words = stride / 4;
        // Colours (4 bytes each) interpolate per byte; everything else is 32-bit floats.
        std::vector<bool> isColour(words, false);
        for (int a : {2, 3})
            if (layout.offset[a] >= 0 && uint32_t(layout.offset[a]) / 4 < words) isColour[layout.offset[a] / 4] = true;
        mesh.vertices.reserve(size_t(triangles) * (n + 1) * (n + 2) / 2 * stride);
        forEachTriangle([&](uint32_t a, uint32_t b, uint32_t c) {
            if (a >= vertexCount || b >= vertexCount || c >= vertexCount || a == b || b == c || a == c) return;
            const uint8_t* va = src + size_t(a) * stride;
            const uint8_t* vb = src + size_t(b) * stride;
            const uint8_t* vc = src + size_t(c) * stride;
            uint32_t base = uint32_t(mesh.vertices.size() / stride);
            // Vertex (i, j): a + (b - a) i/n + (c - a) j/n, row by row.
            for (uint32_t j = 0; j <= n; ++j)
                for (uint32_t i = 0; i + j <= n; ++i) {
                    float wb = float(i) / float(n), wc = float(j) / float(n), wa = 1.0f - wb - wc;
                    size_t o = mesh.vertices.size();
                    mesh.vertices.resize(o + stride);
                    uint8_t* out = &mesh.vertices[o];
                    for (uint32_t k = 0; k < words; ++k) {
                        if (isColour[k]) {
                            for (uint32_t ch = 0; ch < 4; ++ch)
                                out[k * 4 + ch] = uint8_t(std::lround(va[k * 4 + ch] * wa + vb[k * 4 + ch] * wb +
                                                                      vc[k * 4 + ch] * wc));
                        } else {
                            float fa, fb, fc;
                            std::memcpy(&fa, va + k * 4, 4); std::memcpy(&fb, vb + k * 4, 4); std::memcpy(&fc, vc + k * 4, 4);
                            float v = (i == 0 && j == 0) ? fa : (i == n) ? fb : (j == n) ? fc : fa * wa + fb * wb + fc * wc;
                            std::memcpy(out + k * 4, &v, 4);
                        }
                    }
                }
            auto index = [&](uint32_t i, uint32_t j) {
                // Rows shrink by one: row j starts after sum over rows < j of (n + 1 - r).
                return uint16_t(base + j * (n + 1) - j * (j - 1) / 2 + i);
            };
            for (uint32_t j = 0; j < n; ++j)
                for (uint32_t i = 0; i + j < n; ++i) {
                    mesh.indices.insert(mesh.indices.end(), {index(i, j), index(i + 1, j), index(i, j + 1)});
                    if (i + j + 1 < n)
                        mesh.indices.insert(mesh.indices.end(), {index(i + 1, j), index(i + 1, j + 1), index(i, j + 1)});
                }
        });
        if (mesh.indices.empty())
            return;
        it = m_plantMeshes.emplace(key, std::move(mesh)).first;
    }
    it->second.lastFrame = m_frameNumber;
    primitive = d3d::TriangleList;
    vertices = it->second.vertices.data();
    vertexCount = uint32_t(it->second.vertices.size() / layout.stride);
    indices = it->second.indices.data();
    indexCount = uint32_t(it->second.indices.size());
    (void)fvf;
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
    out[0] = m_windDir[0];                       // (the shared wind: wind.cpp UpdateWind)
    out[1] = m_windDir[1];
    out[2] = float(m_windTime);
    out[3] = m_sway * m_windStrength;
}

// The frame lights into a coarse x/z grid of light masks: each light's bit in every cell its sphere covers.
// FrameLightMask then ORs a draw's few cells and tests only those lights, not all kFrameLights (a busy scene captures
// up to 40). A light covering more than 256 cells (a very large range) is tested by every draw (m_lightGridAlways).
void Device::BuildLightGrid()
{
    m_lightGridAlways = 0;
    m_lightGridW = m_lightGridH = 0;
    struct Cells { int32_t x0, x1, z0, z1; };
    Cells cells[64];
    int32_t gx0 = INT32_MAX, gx1 = INT32_MIN, gz0 = INT32_MAX, gz1 = INT32_MIN;
    uint32_t n = std::min<uint32_t>(uint32_t(m_frameLightSpheres.size()), 64);
    for (uint32_t slot = 0; slot < n; ++slot) {
        const LightSphere& s = m_frameLightSpheres[slot];
        float r = std::sqrt(std::max(0.0f, s.r2));
        Cells& c = cells[slot];
        c.x0 = int32_t(std::floor((s.x - r) / kLightGridCell));
        c.x1 = int32_t(std::floor((s.x + r) / kLightGridCell));
        c.z0 = int32_t(std::floor((s.z - r) / kLightGridCell));
        c.z1 = int32_t(std::floor((s.z + r) / kLightGridCell));
        if (int64_t(c.x1 - c.x0 + 1) * int64_t(c.z1 - c.z0 + 1) > 256) {
            m_lightGridAlways |= uint64_t(1) << slot;
            continue;
        }
        gx0 = std::min(gx0, c.x0); gx1 = std::max(gx1, c.x1);
        gz0 = std::min(gz0, c.z0); gz1 = std::max(gz1, c.z1);
    }
    if (gx0 > gx1)
        return;                                  // no light in the grid
    int64_t w = int64_t(gx1) - gx0 + 1, h = int64_t(gz1) - gz0 + 1;
    if (w * h > kLightGridMaxCells) {            // lights spread too far apart: every draw tests them all
        for (uint32_t slot = 0; slot < n; ++slot) m_lightGridAlways |= uint64_t(1) << slot;
        return;
    }
    m_lightGridX0 = gx0;
    m_lightGridZ0 = gz0;
    m_lightGridW = uint32_t(w);
    m_lightGridH = uint32_t(h);
    m_lightGrid.assign(size_t(w * h), 0);
    for (uint32_t slot = 0; slot < n; ++slot) {
        if (m_lightGridAlways >> slot & 1)
            continue;
        const Cells& c = cells[slot];
        for (int32_t cz = c.z0; cz <= c.z1; ++cz) {
            uint64_t* row = m_lightGrid.data() + size_t(cz - gz0) * m_lightGridW;
            for (int32_t cx = c.x0; cx <= c.x1; ++cx)
                row[cx - gx0] |= uint64_t(1) << slot;
        }
    }
}

// Whether a frame light reaches a world box: its sphere does, and a spot light's cone does too (a floodlight's range -
// 1,041 units over Borealis's bridges - reached every draw: every pixel tried it, and no draw got the shader build
// without the lights' loop).
bool Device::LightReachesBox(const LightSphere& l, const float mn[3], const float mx[3]) const
{
    const float dx = std::max(std::max(mn[0] - l.x, l.x - mx[0]), 0.0f);
    const float dy = std::max(std::max(mn[1] - l.y, l.y - mx[1]), 0.0f);
    const float dz = std::max(std::max(mn[2] - l.z, l.z - mx[2]), 0.0f);
    if (dx * dx + dy * dy + dz * dz > l.r2)
        return false;
    if (!l.spot)
        return true;
    Cone cone{};
    std::memcpy(cone.axis, l.axis, sizeof(cone.axis));
    cone.cosHalf = l.cosHalf;
    cone.sinHalf = l.sinHalf;
    cone.on = true;
    const float at[3] = {l.x, l.y, l.z};
    return BoxInCone(mn, mx, at, cone);
}

// The frame lights (light override) whose sphere reaches the draw's world bounding box, as a 64-bit mask for the
// shader - instead of every pixel trying all of them. All set when the box is unknown (external geometry).
void Device::FrameLightMask(uint32_t fvf, uint32_t stride, const void* vertices, uint32_t vertexCount, uint32_t out[4])
{
    out[0] = out[1] = out[2] = out[3] = 0;
    if (m_drawingPortal || !m_lightOverride || !m_pixelLighting || !m_rs[d3d::RS_LIGHTING] ||
        (fvf & d3d::FVF_POSITION_MASK) == d3d::FVF_XYZRHW || m_frameLightIndices.empty())
        return;
    uint32_t count = std::min<uint32_t>(uint32_t(m_frameLightIndices.size()), 64);
    if (m_external || (!vertices && !m_drawMesh) || !vertexCount) {
        out[0] = count >= 32 ? ~0u : (1u << count) - 1u;
        out[1] = count > 32 ? (count >= 64 ? ~0u : (1u << (count - 32)) - 1u) : 0u;
        return;
    }
    float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
    if (m_drawMesh) {
        std::memcpy(mn, m_drawMesh->boundsMin, sizeof(mn));
        std::memcpy(mx, m_drawMesh->boundsMax, sizeof(mx));
    } else {
        const uint8_t* v = static_cast<const uint8_t*>(vertices);
        for (uint32_t i = 0; i < vertexCount; ++i) {
            float p[3];
            std::memcpy(p, v + size_t(i) * stride, 12);
            for (int j = 0; j < 3; ++j) { mn[j] = std::min(mn[j], p[j]); mx[j] = std::max(mx[j], p[j]); }
        }
    }
    // The model box through the world matrix (centre and half extents); kept with the object (objects.cpp).
    float wmn[3], wmx[3];
    const auto& m = m_world.m;
    if (m_drawObject) {
        std::memcpy(wmn, m_drawObject->lightMin, sizeof(wmn));
        std::memcpy(wmx, m_drawObject->lightMax, sizeof(wmx));
    } else for (int j = 0; j < 3; ++j) {
        float c = m[3][j], e = 0.0f;
        for (int i = 0; i < 3; ++i) {
            c += 0.5f * (mn[i] + mx[i]) * m[i][j];
            e += 0.5f * (mx[i] - mn[i]) * std::fabs(m[i][j]);
        }
        wmn[j] = c - e;
        wmx[j] = c + e;
    }
    const LightSphere* spheres = m_frameLightSpheres.data();
    count = std::min<uint32_t>(count, uint32_t(m_frameLightSpheres.size()));
    uint64_t hits = 0;
    auto Test = [&](uint32_t k) {
        if (hits >> k & 1)
            return;
        if (LightReachesBox(spheres[k], wmn, wmx))
            hits |= uint64_t(1) << k;
    };
    // The candidates: the lights of the grid cells the box covers (clipped to the grid), plus the ones too large for
    // it; each is then tested exactly.
    uint64_t all = count >= 64 ? ~uint64_t(0) : (uint64_t(1) << count) - 1;
    uint64_t candidates = m_lightGridAlways;
    if (m_lightGridW) {
        float fx0 = std::floor(wmn[0] / kLightGridCell), fx1 = std::floor(wmx[0] / kLightGridCell);
        float fz0 = std::floor(wmn[2] / kLightGridCell), fz1 = std::floor(wmx[2] / kLightGridCell);
        // In grid coordinates, clipped (in float first: a huge or broken box must not overflow the conversion).
        float gw = float(m_lightGridW) - 1.0f, gh = float(m_lightGridH) - 1.0f;
        float x0 = std::max(fx0 - float(m_lightGridX0), 0.0f), x1 = std::min(fx1 - float(m_lightGridX0), gw);
        float z0 = std::max(fz0 - float(m_lightGridZ0), 0.0f), z1 = std::min(fz1 - float(m_lightGridZ0), gh);
        if (x0 <= x1 && z0 <= z1) {
            if ((x1 - x0 + 1.0f) * (z1 - z0 + 1.0f) > 1024.0f) {
                candidates = all;                // a box covering much of the lights' area: test them all
            } else {
                for (uint32_t z = uint32_t(z0); z <= uint32_t(z1); ++z) {
                    const uint64_t* row = m_lightGrid.data() + size_t(z) * m_lightGridW;
                    for (uint32_t x = uint32_t(x0); x <= uint32_t(x1); ++x)
                        candidates |= row[x];
                }
            }
        }
    }
    candidates &= all;
    while (candidates) {
        uint32_t k = uint32_t(std::countr_zero(candidates));
        candidates &= candidates - 1;
        Test(k);
    }
    if (m_lightMaskVerify < 0) {
        const char* v = std::getenv("RANDYVK_LIGHTMASK_VERIFY");
        m_lightMaskVerify = v && *v == '1' ? 1 : 0;
    }
    if (m_lightMaskVerify) {                     // the same mask from every light: a check while bringing the grid up
        uint64_t brute = 0;
        for (uint32_t k = 0; k < count; ++k)
            if (LightReachesBox(spheres[k], wmn, wmx)) brute |= uint64_t(1) << k;
        if (brute != hits) ++m_lightMaskDiff;
        hits = brute;
    }
    // A dungeon room (a shop, a mission's rooms): only the lights the game lit it with. Its lamps are baked into its
    // lightmap (stage 1), so the game leaves them out and lights it with moving ones (a character's own light); with
    // every lamp in reach its walls and floor had them twice - a shop's room washed out once lamps indoors were at the
    // game's strength (LampScale).
    // A character's light isn't baked, though: it lights the room in its reach whichever of the room's sections the
    // game gave it to - the game gives it to the section the character stands in alone, and stepping through a door
    // the walls and floor beside it on the side just left went dark at once (the user, 2026-10-26).
    if (hits && VisualKind(m_drawVisualKind) == VisualKind::Room) {
        uint64_t kept = 0;
        for (uint64_t rest = hits; rest; rest &= rest - 1) {
            const uint32_t k = uint32_t(std::countr_zero(rest));
            if (m_lightsPrev[m_frameLightIndices[k]].carried > 0.5f) {
                kept |= uint64_t(1) << k;
                continue;
            }
            const d3d::Light& l = m_lightsPrev[m_frameLightIndices[k]].light;
            for (const LightSlot& slot : m_lights) {
                if (!slot.enabled || slot.light.type == d3d::LIGHT_DIRECTIONAL || slot.light.range != l.range) continue;
                const float dx = slot.light.position.x - l.position.x, dy = slot.light.position.y - l.position.y,
                            dz = slot.light.position.z - l.position.z;
                if (dx * dx + dy * dy + dz * dz < 1.0f) {
                    kept |= uint64_t(1) << k;
                    break;
                }
            }
        }
        hits = kept;
    }
    out[0] = uint32_t(hits);
    out[1] = uint32_t(hits >> 32);
}

// The current draw's mesh info (m_drawMesh): from the cache when its fingerprint was seen before, else one pass over
// its vertices (box) and indices (hash). World-space meshes (XYZ) only; others get none.
void Device::DrawMeshInfo(uint32_t fvf, uint32_t stride, const void* vertices, uint32_t vertexCount, const uint16_t* indices,
                          uint32_t indexCount)
{
    m_drawMesh = nullptr;
    m_drawMeshStatic = false;
    if (m_external || (!vertices && !m_drawSkin) || !vertexCount || (fvf & d3d::FVF_POSITION_MASK) != d3d::FVF_XYZ)
        return;
    if (m_drawSkin) {
        // A skinned piece: new vertices with every skinning (a key of its own, as new vertices would get), its box
        // and index hash from the skinning - no pass over the vertices.
        // (The frame number in it too: a job's memory may be reused by a later frame's.)
        uint64_t key = (reinterpret_cast<uintptr_t>(m_drawSkin) * 0x9E3779B97F4A7C15ull) ^ (uint64_t(fvf) << 40) ^
                       vertexCount ^ (uint64_t(indexCount) << 20) ^ (m_frameNumber * 0xC2B2AE3D27D4EB4Full);
        m_drawMeshKey = key;
        auto it = m_meshInfo.find(key);
        if (it == m_meshInfo.end()) {
            MeshInfo info{};
            info.diffuseAlphaOne = info.specularAlphaOne = false;   // a GPU-skinned piece: colours not scanned
            std::memcpy(info.boundsMin, m_drawSkin->boundsMin, sizeof(info.boundsMin));
            std::memcpy(info.boundsMax, m_drawSkin->boundsMax, sizeof(info.boundsMax));
            info.indexHash = indexCount == m_drawSkin->source->indices.size() ? m_drawSkin->source->indexHash
                             : indices && indexCount ? HashBytes(indices, size_t(indexCount) * 2, indexCount) : 0;
            info.firstFrame = m_frameNumber;
            it = m_meshInfo.emplace(key, info).first;
        }
        it->second.lastFrame = m_frameNumber;
        m_drawMeshStatic = it->second.firstFrame < m_frameNumber;
        m_drawMesh = &it->second;
        return;
    }
    // A registered mesh (DrawMesh) whose mesh info was found before: kept with it (used every draw, so not dropped).
    if (m_drawSlot && m_drawSlot->mesh && m_drawStaticBuffer) {
        ++m_meshStaticDraws;
        MeshInfo* info = m_drawSlot->mesh;
        info->lastFrame = m_frameNumber;
        m_drawMeshKey = m_drawSlot->meshKey;
        m_drawMeshStatic = info->firstFrame < m_frameNumber;
        m_drawMesh = info;
        return;
    }
    uint64_t key;
    if (m_drawStaticBuffer) {
        // A static snapshot's vertices never change, so identify the sub-mesh by its snapshot and where it sits in
        // it, and skip the pass over the vertices. The snapshot's serial, not its address: a freed snapshot's memory
        // can hold the next one, which must not inherit its bounds, alpha and caster key. (The ring path below must
        // hash the content: a dynamic buffer's pointer can stay while its data changes.) CachedMeshInfo then makes
        // CasterKey and FrameLightMask cheap too.
        ++m_meshStaticDraws;
        key = 0x51ED270F9E3779B9ull;
        auto mixStatic = [&key](uint64_t x) { key = (key ^ x) * 0xFF51AFD7ED558CCDull; key ^= key >> 32; };
        mixStatic(m_drawStaticSerial);
        mixStatic(m_drawStaticOffset);
        mixStatic(vertexCount);
        mixStatic(indexCount);
        mixStatic(fvf);
        mixStatic(stride);
    } else {
        ++m_meshHashedDraws;
        const uint8_t* v = static_cast<const uint8_t*>(vertices);
        // Word-wise mixing (positions as three 32-bit words), not byte by byte: this runs for every draw.
        key = 0x9E3779B97F4A7C15ull ^ (uint64_t(fvf) << 40) ^ (uint64_t(stride) << 32) ^ vertexCount;
        auto mix = [&key](uint64_t x) { key = (key ^ x) * 0xFF51AFD7ED558CCDull; key ^= key >> 32; };
        mix(indexCount);
        uint32_t step = vertexCount > 16 ? vertexCount / 16 : 1;
        for (uint32_t i = 0; i < vertexCount; i += step) {
            uint32_t p[3];
            std::memcpy(p, v + size_t(i) * stride, 12);
            mix(uint64_t(p[0]) | uint64_t(p[1]) << 32);
            mix(p[2]);
        }
        uint32_t last[3];
        std::memcpy(last, v + size_t(vertexCount - 1) * stride, 12);
        mix(uint64_t(last[0]) | uint64_t(last[1]) << 32);
        mix(last[2]);
        if (indices && indexCount) {
            uint32_t istep = indexCount > 16 ? indexCount / 16 : 1;
            for (uint32_t i = 0; i < indexCount; i += istep)
                mix(indices[i] | uint64_t(i) << 16);
        }
    }
    m_drawMeshKey = key;
    auto it = m_meshInfo.find(key);
    if (it != m_meshInfo.end()) {
        m_drawMeshStatic = it->second.firstFrame < m_frameNumber;
        it->second.lastFrame = m_frameNumber;
        m_drawMesh = &it->second;
        if (m_drawSlot && m_drawStaticBuffer) { m_drawSlot->mesh = &it->second; m_drawSlot->meshKey = key; }
        return;
    }
    MeshInfo info{};
    FvfLayout fl = DecodeFvf(fvf);
    int diffuseAlpha = fl.offset[2] >= 0 ? fl.offset[2] + 3 : -1;   // D3DCOLOR: B, G, R, A
    int specularAlpha = fl.offset[3] >= 0 ? fl.offset[3] + 3 : -1;
    for (int j = 0; j < 3; ++j) { info.boundsMin[j] = 1e30f; info.boundsMax[j] = -1e30f; }
    const uint8_t* verts = static_cast<const uint8_t*>(vertices);
    for (uint32_t i = 0; i < vertexCount; ++i) {
        float p[3];
        std::memcpy(p, verts + size_t(i) * stride, 12);
        for (int j = 0; j < 3; ++j) {
            info.boundsMin[j] = std::min(info.boundsMin[j], p[j]);
            info.boundsMax[j] = std::max(info.boundsMax[j], p[j]);
        }
        // Vertex colours' alpha (AlphaOneCheck: a blended draw whose alpha is 1 can go into the depth pre-pass).
        if (diffuseAlpha >= 0 && verts[size_t(i) * stride + diffuseAlpha] != 0xFF) info.diffuseAlphaOne = false;
        if (specularAlpha >= 0 && verts[size_t(i) * stride + specularAlpha] != 0xFF) info.specularAlphaOne = false;
    }
    info.indexHash = indices && indexCount ? HashBytes(indices, size_t(indexCount) * 2, indexCount) : 0;
    info.firstFrame = info.lastFrame = m_frameNumber;
    HashMeshId(info, fvf, stride, vertices, vertexCount, indices, indexCount);   // (mesh ids: a static piece's)
    MeshInfo* placed = &m_meshInfo.emplace(key, info).first->second;
    m_drawMesh = placed;
    if (m_drawSlot && m_drawStaticBuffer) { m_drawSlot->mesh = placed; m_drawSlot->meshKey = key; }
}

bool Device::IsWater(uint32_t fvf) const
{
    return m_drawProcessed &&
           fvf == (d3d::FVF_XYZRHW | d3d::FVF_DIFFUSE | d3d::FVF_SPECULAR | (1u << d3d::FVF_TEXCOUNT_SHIFT));
}

// A sheen laid on a surface drawn just before: added, depth EQUAL, no depth writes - the alien levels' shine on every
// wall, door and Kyr'Ozch (a 64 x 64 environment texture, lit by the game with no lights and a dim ambient). Not an
// effect: it neither feeds the glow nor takes the frame's lights and headroom (lit by our lamps - the player's carried
// light up close - and bloomed, a Kyr'Ozch came out a white blaze); the game's own light it keeps.
bool Device::SurfaceSheen(uint32_t fvf) const
{
    return m_rs[d3d::RS_ALPHABLENDENABLE] && m_rs[d3d::RS_DESTBLEND] == d3d::BLEND_ONE &&
           m_rs[d3d::RS_ZFUNC] == d3d::CMP_EQUAL && !m_rs[d3d::RS_ZWRITEENABLE] &&
           (fvf & d3d::FVF_POSITION_MASK) != d3d::FVF_XYZRHW;
}

bool Device::GlowDraw(uint32_t fvf) const
{
    if (m_effectGlow <= 0.0f || m_target != m_scene || !m_rs[d3d::RS_ALPHABLENDENABLE] ||
        (fvf & d3d::FVF_POSITION_MASK) == d3d::FVF_XYZRHW || m_rs[d3d::RS_ZFUNC] == d3d::CMP_ALWAYS || SurfaceSheen(fvf))
        return false;
    uint32_t src = m_rs[d3d::RS_SRCBLEND], dst = m_rs[d3d::RS_DESTBLEND];
    return dst == d3d::BLEND_ONE && (src == d3d::BLEND_ONE || src == d3d::BLEND_SRCALPHA);
}

// A solid surface drawn into the scene - depth-writing, not additive, not the ground's light pass: it covers the glow
// (additive effects' light, for the bloom) drawn before it behind it, as it covers their colour. Else an effect drawn
// before a wall in front of it bloomed through the wall (the Temple of the Three Winds portal through a rock).
bool Device::GlowOccluder(uint32_t fvf) const
{
    if (m_effectGlow <= 0.0f || m_target != m_scene || (fvf & d3d::FVF_POSITION_MASK) == d3d::FVF_XYZRHW ||
        !m_rs[d3d::RS_ZENABLE] || !m_rs[d3d::RS_ZWRITEENABLE] || m_rs[d3d::RS_ZFUNC] == d3d::CMP_ALWAYS ||
        m_drawIsLabel || ((m_drawTerrainLight || IsMultiplyPass()) && !m_groundOne))   // (the ground's one pass: its
        return false;                                                                // base pass's part, covering it)
    return !(m_rs[d3d::RS_ALPHABLENDENABLE] && m_rs[d3d::RS_DESTBLEND] == d3d::BLEND_ONE);
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
    if (!v && m_drawMesh) {                              // no vertices here (skinned on the GPU): its box
        float c[3], e[3];
        DrawWorldBox(c, e);
        for (int j = 0; j < 3; ++j) { mn[j] = c[j] - e[j]; mx[j] = c[j] + e[j]; }
        vertexCount = 0;
    }
    for (uint32_t i = 0; i < vertexCount; ++i) {
        float q[3];
        std::memcpy(q, v + size_t(i) * stride, sizeof(q));
        for (int j = 0; j < 3; ++j) {
            float wq = q[0] * w[0][j] + q[1] * w[1][j] + q[2] * w[2][j] + w[3][j];
            mn[j] = std::min(mn[j], wq);
            mx[j] = std::max(mx[j], wq);
        }
    }
    for (uint32_t k = 0; k < m_frameLightIndices.size(); ++k) {
        const CapturedLight& c = m_lightsPrev[m_frameLightIndices[k]];
        if (m_drawOwner && c.owner) {            // known: exactly (else found by where it stands: FindCarriers)
            if (c.owner == m_drawOwner)
                return k + 1;
            continue;
        }
        if (IsCarrierPart(c, m_world, mn, mx))
            return k + 1;
    }
    return 0;
}

void Device::SetDrawVisual(uint32_t kind, const char* className, uint32_t owner, uint32_t visual)
{
    m_drawVisualKind = kind;
    if (VisualKind(kind) == VisualKind::Terrain) ++m_frameTerrainVisuals;
    else if (VisualKind(kind) == VisualKind::Room) ++m_frameRoomVisuals;
    m_drawVisualName = className ? className : "";
    // (A sky visual is named by its class chain, "VisualSingleCloud_t<...".)
    m_drawIsCloudPuff = className && std::strncmp(className, "VisualSingleCloud_t", 19) == 0 &&
                        (className[19] == 0 || className[19] == '<');
    if (VisualKind(kind) == VisualKind::Sky)
        m_skySeenFrame = m_frameNumber;
    m_drawIsSkyMesh = VisualKind(kind) == VisualKind::Sky && className &&
                      std::strncmp(className, "GenericMeshObject_t", 19) == 0;
    m_drawOwner = owner;
    m_drawVisual = visual;
}

// The fog for a draw after the scene's: the ground's this frame (or lately), else the render states'.
void Device::SceneFog(float colour[4], float params[4]) const
{
    if (m_sceneFogFrame != ~0ull && m_sceneFogFrame + 30 >= m_frameNumber) {
        std::memcpy(colour, m_sceneFogColour, sizeof(m_sceneFogColour));
        std::memcpy(params, m_sceneFogParams, sizeof(m_sceneFogParams));
        return;
    }
    RenderStateFog(colour, params);
}

// The render states' fog: colour (w: on); start, end, density, mode.
void Device::RenderStateFog(float colour[4], float params[4]) const
{
    const uint32_t c = m_rs[d3d::RS_FOGCOLOR];
    colour[0] = float((c >> 16) & 0xFF) / 255.0f;
    colour[1] = float((c >> 8) & 0xFF) / 255.0f;
    colour[2] = float(c & 0xFF) / 255.0f;
    colour[3] = m_rs[d3d::RS_FOGENABLE] ? 1.0f : 0.0f;
    StormAirColour(colour);                            // (a thunderstorm's: its air dark yellow-green, dimmed)
    uint32_t bits[3] = {m_rs[d3d::RS_FOGSTART], m_rs[d3d::RS_FOGEND], m_rs[d3d::RS_FOGDENSITY]};
    std::memcpy(params, bits, sizeof(bits));
    params[0] *= m_viewRange;                    // (RVK_ViewRange: the fog out with the view)
    params[1] *= m_viewRange;
    params[3] = float(m_rs[d3d::RS_FOGTABLEMODE] ? m_rs[d3d::RS_FOGTABLEMODE] : m_rs[d3d::RS_FOGVERTEXMODE]);
}

void Device::SetSceneLights(const SceneLight* lights, uint32_t count)
{
    m_sceneLights.assign(lights, lights + count);
    m_sceneLightsFrame = m_frameNumber;
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
    FlushGroup();                                // batched draws recorded before the clear must run before it
    FlushEdges();                                // ... and two-pass foliage's queued edges
    if (!(flags & (d3d::CLEAR_TARGET | d3d::CLEAR_ZBUFFER)))
        return;
    // D3D clears the given rectangles (or the whole viewport), always clipped to the viewport. (RVK_RenderScale: the
    // game's rectangles are in back-buffer pixels, brought onto the smaller scene.)
    const bool scaled = m_target == m_scene && SceneScaled();
    const uint32_t tw = scaled ? m_ldrMain->m_width : m_target->m_width, th = scaled ? m_ldrMain->m_height : m_target->m_height;
    int32_t vx0 = int32_t(std::min(m_viewport.x, tw));
    int32_t vy0 = int32_t(std::min(m_viewport.y, th));
    int32_t vx1 = int32_t(std::min(m_viewport.x + m_viewport.width, tw));
    int32_t vy1 = int32_t(std::min(m_viewport.y + m_viewport.height, th));
    Rect whole{vx0, vy0, vx1, vy1};
    if (!count) {
        count = 1;
        rects = &whole;
    }
    std::vector<VkClearRect> clears;
    for (uint32_t i = 0; i < count; ++i) {
        Rect c{std::max(rects[i].left, vx0), std::max(rects[i].top, vy0), std::min(rects[i].right, vx1),
               std::min(rects[i].bottom, vy1)};
        if (c.right <= c.left || c.bottom <= c.top)
            continue;
        if (scaled) {
            c = SceneRect(c);
            c.right = std::min(c.right, int32_t(m_target->m_width));
            c.bottom = std::min(c.bottom, int32_t(m_target->m_height));
        }
        if (c.right > c.left && c.bottom > c.top)
            clears.push_back({{{c.left, c.top}, {uint32_t(c.right - c.left), uint32_t(c.bottom - c.top)}}, 0, 1});
    }
    if (clears.empty())
        return;
    // The scene's depth clear starts the depth pre-pass, which then does the clear (it runs before everything
    // recorded from here on); a second one ends it, and clears here as usual.
    bool depth = (flags & d3d::CLEAR_ZBUFFER) != 0;
    if (depth && m_prepassArmed)
        PrepassEnd(kEndClear);
    else if (depth && PrepassArm(clears.data(), uint32_t(clears.size()), z))
        depth = false;
    VkClearAttachment att[2];
    uint32_t n = 0;
    if (flags & d3d::CLEAR_TARGET) {
        att[n] = {VK_IMAGE_ASPECT_COLOR_BIT, 0, {}};
        ArgbToFloat(argb, att[n].clearValue.color.float32);
        // In the interface layer a clear covers what is under it, as it would on the frame: alpha is coverage there,
        // and the PF map's black (0x00000000) would clear it see-through.
        if (InLayer())
            att[n].clearValue.color.float32[3] = 1.0f;
        ++n;
    }
    if (depth) {
        att[n] = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, {}};
        att[n].clearValue.depthStencil = {z, 0};
        ++n;
    }
    if (n)
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
    m_renderEndCause = kEndCopy;
    EndRendering();
    VkImageLayout srcRestore = src->m_layout;
    Transition(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    Transition(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit blit{};
    blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    Rect s = srcRect ? *srcRect : Rect{0, 0, int32_t(src->m_width), int32_t(src->m_height)};
    Rect d = dstRect ? *dstRect : Rect{0, 0, int32_t(dst->m_width), int32_t(dst->m_height)};
    // RVK_RenderScale: the game's rectangles on the back buffer are in its pixels; the scene standing in for it is
    // smaller (the blit scales).
    if (SceneScaled()) {
        if (src == m_scene && srcRect) s = SceneRect(s);
        if (dst == m_scene && dstRect) d = SceneRect(d);
    }
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

void Device::DrawSkinned(uint32_t primitive, uint32_t fvf, skin::Job& job, uint32_t startVertex, uint32_t vertexCount,
                         const uint16_t* indices, uint32_t indexCount)
{
    double since = (m_frameNumber & 15) == 0 ? ProfileCpu() : 0.0;   // profiling (as Draw's sections)
    m_drawSkin = &job;
    m_drawGpu = nullptr;
    // On the GPU: the whole piece with its own triangles (the game's draws of a character piece always are).
    if (m_gpuSkin && fvf == skin::kVertexFvf && startVertex == 0 && vertexCount == job.source->vertices.size() &&
        indices && indices == job.source->indices.data() && indexCount <= job.source->indices.size() && m_inFrame)
        m_drawGpu = SkinOnGpu(job);
    if (m_drawGpu) {
        ProfileDrawSection("draw: skinning", since);
        if (m_pickActive)                            // picking: this frame only, the piece skinned on the CPU too
            m_pickSkinned = job.Skinned() + startVertex;
        Draw(primitive, fvf, nullptr, vertexCount, indices, indexCount);
        m_pickSkinned = nullptr;
    } else {
        const skin::Vertex* vertices = job.Skinned();
        m_drawSkinBase = vertices;
        ProfileDrawSection("draw: skinning", since);
        Draw(primitive, fvf, vertices + startVertex, vertexCount, indices, indexCount);
    }
    m_drawSkin = nullptr;
    m_drawGpu = nullptr;
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

// The D3D viewport as Vulkan's: negative height flips Y so D3D's clip space maps the same way; +0.5 matches D3D pixel
// centres. The scissor is the viewport within the target.
void Device::ViewportState(VkViewport* vp, VkRect2D* scissor) const
{
    ViewportStateOf(vp, scissor);
    if (m_drawingPortal) {                       // (a shop's room: kept to its doorway's box - portal.cpp)
        const int32_t x0 = std::max(scissor->offset.x, m_portalScissor.offset.x);
        const int32_t y0 = std::max(scissor->offset.y, m_portalScissor.offset.y);
        const int32_t x1 = std::min(scissor->offset.x + int32_t(scissor->extent.width),
                                    m_portalScissor.offset.x + int32_t(m_portalScissor.extent.width));
        const int32_t y1 = std::min(scissor->offset.y + int32_t(scissor->extent.height),
                                    m_portalScissor.offset.y + int32_t(m_portalScissor.extent.height));
        *scissor = {{x0, y0}, {uint32_t(std::max(x1 - x0, 0)), uint32_t(std::max(y1 - y0, 0))}};
    }
}

void Device::ViewportStateOf(VkViewport* vp, VkRect2D* scissor) const
{
    // RVK_RenderScale: the game's viewport is in back-buffer pixels; the scene is smaller (the half-pixel shift of
    // D3D's pixel centres stays half a scene pixel, as in the passes that draw into the scene themselves).
    const bool scaled = m_target == m_scene && SceneScaled();
    const float kx = scaled ? SceneScaleX() : 1.0f, ky = scaled ? SceneScaleY() : 1.0f;
    vp->x = float(m_viewport.x) * kx + 0.5f;
    vp->y = float(m_viewport.y + m_viewport.height) * ky + 0.5f;
    vp->width = float(m_viewport.width) * kx;
    vp->height = -float(m_viewport.height) * ky;
    vp->minDepth = m_viewport.minZ;
    vp->maxDepth = m_viewport.maxZ;
    uint32_t w = m_target->m_width, h = m_target->m_height;
    if (scaled) {
        Rect r = SceneRect({int32_t(m_viewport.x), int32_t(m_viewport.y), int32_t(m_viewport.x + m_viewport.width),
                            int32_t(m_viewport.y + m_viewport.height)});
        uint32_t sx = std::min(uint32_t(r.left), w), sy = std::min(uint32_t(r.top), h);
        *scissor = {{int32_t(sx), int32_t(sy)}, {std::min(uint32_t(r.right), w) - sx, std::min(uint32_t(r.bottom), h) - sy}};
        return;
    }
    uint32_t sx = std::min(m_viewport.x, w), sy = std::min(m_viewport.y, h);
    *scissor = {{int32_t(sx), int32_t(sy)}, {std::min(m_viewport.width, w - sx), std::min(m_viewport.height, h - sy)}};
}

// A rectangle in back-buffer pixels on the scene's (RVK_RenderScale): every scene pixel it touches.
Device::Rect Device::SceneRect(const Rect& r) const
{
    const float kx = SceneScaleX(), ky = SceneScaleY();
    return {int32_t(std::floor(float(r.left) * kx)), int32_t(std::floor(float(r.top) * ky)),
            int32_t(std::ceil(float(r.right) * kx - 1e-3f)), int32_t(std::ceil(float(r.bottom) * ky - 1e-3f))};
}

// Vertex layout: binding 0 = the draw's vertex buffer (draws address it through their vertex offset), binding 1 =
// zeros for attributes the format lacks.
void Device::SetVertexInput(VkCommandBuffer cmd, uint32_t fvf, uint32_t stride)
{
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
}

void Device::ApplyDynamicState(uint32_t primitive, uint32_t fvf, uint32_t stride)
{
    VkCommandBuffer cmd = m_frames[m_frameIndex].main;
    StateCache& c = m_cache;
    uint32_t topoClass = TopologyClass(primitive);
    bool hdrTarget = m_target->m_format == Format::RGBA16F;
    // Pipeline classes: 0-2 points / lines / triangles, 3-5 the same for the float target, 6 / 7 tessellated; +12
    // for the no-discard variant (a draw that never cuts out keeps early-Z); +24 for the ground's (the shader's build
    // with the ground's code: m_drawGround), +48 for the draws no frame light reaches (the build without their loop);
    // 72-74 (+12) the interface's lean shader (m_drawLite: 8-bit targets only).
    const uint32_t build = m_drawGround ? 1u : m_drawNoFrameLights ? 2u : 0u;
    const bool lite = m_drawLite && !m_drawTess && !hdrTarget;
    uint32_t colorClass = m_drawTess ? 6u + (hdrTarget ? 1u : 0u) : topoClass + (hdrTarget ? 3u : 0u);
    uint32_t pipelineClass = lite ? 72u + topoClass + (m_drawMayDiscard ? 0u : 12u)
                                  : colorClass + (m_drawMayDiscard ? 0u : 12u) + 24u * build;
    if (c.topologyClass != pipelineClass) {
        VkPipeline pipeline;
        const uint32_t h = hdrTarget ? 1u : 0u, nc = m_drawMayDiscard ? 0u : 1u;
        if (lite) {
            pipeline = nc ? m_pipelinesLiteNoCut[topoClass] : m_pipelinesLite[topoClass];
        } else if (m_drawTess) {
            VkPipeline* const tess[3][2] = {{m_tessPipelines, m_tessPipelinesNoCut},
                                            {m_groundTessPipelines, m_groundTessPipelinesNoCut},
                                            {m_noFrameTessPipelines, m_noFrameTessPipelinesNoCut}};
            pipeline = tess[build][nc][h];
        } else {
            VkPipeline* const sets[3][2][2] = {
                {{m_pipelines.data(), m_pipelinesNoCut}, {m_pipelinesHdr, m_pipelinesHdrNoCut}},
                {{m_groundPipelines, m_groundPipelinesNoCut}, {m_groundPipelinesHdr, m_groundPipelinesHdrNoCut}},
                {{m_noFramePipelines, m_noFramePipelinesNoCut}, {m_noFramePipelinesHdr, m_noFramePipelinesHdrNoCut}}};
            pipeline = sets[build][h][nc][topoClass];
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        c.topologyClass = pipelineClass;
    }
    constexpr uint32_t kPatchTopology = 0x100;   // c.topology while a tessellated draw's patch list is set
    uint32_t topology = m_drawTess ? kPatchTopology : primitive;
    if (c.topology != topology) {
        vkCmdSetPrimitiveTopology(cmd, m_drawTess ? VK_PRIMITIVE_TOPOLOGY_PATCH_LIST : Topology(primitive));
        c.topology = topology;
    }
    if (!c.valid) {
        // D3D front faces are clockwise on screen; D3DCULL_CCW culls the counter-clockwise (back) ones.
        vkCmdSetFrontFace(cmd, VK_FRONT_FACE_CLOCKWISE);
        c.valid = true;
    }

    VkViewport vp;
    VkRect2D scissor;
    ViewportState(&vp, &scissor);
    if (std::memcmp(&vp, &c.viewport, sizeof(vp)) != 0) {
        vkCmdSetViewport(cmd, 0, 1, &vp);
        c.viewport = vp;
    }
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
    // An additive draw (blended onto ONE) only adds light - it hides nothing behind it - but some write depth (the
    // Temple of the Three Winds portal's discs): their whole quads, clear corners too, stood in the depth as walls,
    // and the passes reading it (ambient occlusion, contact shadows, haze and fog, reflections, volumetric light)
    // drew their outlines as squares. Their depth isn't written. (ao-vk #15)
    if (zWrite && m_target == m_scene && m_rs[d3d::RS_ALPHABLENDENABLE] && m_rs[d3d::RS_DESTBLEND] == d3d::BLEND_ONE &&
        (fvf & d3d::FVF_POSITION_MASK) != d3d::FVF_XYZRHW)
        zWrite = 0;
    // A blended overlay drawn over everything (ZFUNC ALWAYS) - a sandstorm's tint is a screen-sized sprite a few
    // units ahead of the camera - writes its depth over the whole frame: the post passes (ambient occlusion, depth
    // of field, contact shadows, volumetric light) would take the frame for a wall a few units away.
    if (zWrite && m_target == m_scene && m_rs[d3d::RS_ALPHABLENDENABLE] && m_rs[d3d::RS_ZFUNC] == d3d::CMP_ALWAYS &&
        (fvf & d3d::FVF_POSITION_MASK) != d3d::FVF_XYZRHW)
        zWrite = 0;
    if (WaterWritesDepth(fvf))
        zWrite = 1;
    uint32_t zFunc = m_rs[d3d::RS_ZFUNC];
    // Its own depth is already in the buffer (the pre-pass, same coverage, same positions: invariant): only where it
    // is equal is this draw seen, and nothing is left to write. Without a depth write the test runs before the shader
    // even when the shader can discard - with one it ran after, so every see-through fragment of a plant card in front
    // of the visible surface was shaded and then dropped (foliage ~6 shader runs a screen pixel at an outpost).
    if (m_drawPrepassed && (zFunc == d3d::CMP_LESS || zFunc == d3d::CMP_LESSEQUAL) && m_prepassEqual) {
        zFunc = d3d::CMP_EQUAL;
        zWrite = 0;
    } else if (m_drawPrepassed && zFunc == d3d::CMP_LESS) {
        zFunc = d3d::CMP_LESSEQUAL;
    }
    if (c.depthTest != zEnable) { vkCmdSetDepthTestEnable(cmd, zEnable); c.depthTest = zEnable; }
    if (c.depthWrite != zWrite) { vkCmdSetDepthWriteEnable(cmd, zWrite); c.depthWrite = zWrite; }
    if (c.depthOp != zFunc) { vkCmdSetDepthCompareOp(cmd, CompareOp(zFunc)); c.depthOp = zFunc; }
    if (m_depthBounds) {                         // the backdrop cull (prepass.cpp): only where nothing opaque is drawn
        uint32_t bounds = m_drawBackdrop ? 1u : 0u;
        if (c.depthBounds != bounds) { vkCmdSetDepthBoundsTestEnable(cmd, bounds); c.depthBounds = bounds; }
        if (bounds && c.depthBoundsZ != m_prepassClearZ) {
            vkCmdSetDepthBounds(cmd, m_prepassClearZ, m_prepassClearZ);
            c.depthBoundsZ = m_prepassClearZ;
        }
    }

    // The attachments' blend enables changed here are set together below (one call for the range, not one each: the
    // statics' blend toggling set three of them ~900 times a frame over a city).
    uint32_t blendDirty = 0;
    if (hdrTarget) {
        // The glow attachment: an additive effect adds what it contributes; a solid surface (GlowOccluder) scales what
        // is there by 1 - its coverage (it writes 0, 0, 0, coverage): what it covers no longer glows. Others don't
        // write it (masks below).
        if (!c.glowBlendSet) {
            blendDirty |= 2u;
            c.glowBlendSet = true;
        }
        const uint8_t mode = GlowOccluder(fvf) ? 2 : 1;
        if (c.glowBlendMode != mode) {
            VkColorBlendEquationEXT eq = mode == 1
                ? VkColorBlendEquationEXT{VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_ADD,
                                          VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_ADD}
                : VkColorBlendEquationEXT{VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD,
                                          VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD};
            vkCmdSetColorBlendEquationEXT(cmd, 1, 1, &eq);
            c.glowBlendMode = mode;
        }
    }
    // The interface layer (interface.cpp): every draw blends - its colour as asked (unblended: ONE, ZERO) - and its
    // alpha accumulates its coverage.
    const bool uiLayer = InLayer();
    VkBool32 blend = m_rs[d3d::RS_ALPHABLENDENABLE] != 0 || uiLayer;
    if (c.blendEnable != blend) {
        blendDirty |= 1u;
        c.blendEnable = blend;
    }
    uint32_t src = m_rs[d3d::RS_SRCBLEND], dst = m_rs[d3d::RS_DESTBLEND];
    if (uiLayer && !m_rs[d3d::RS_ALPHABLENDENABLE]) { src = d3d::BLEND_ONE; dst = d3d::BLEND_ZERO; }
    if (m_drawOverbright2x) { src = kBlendOverbright2x; dst = kBlendOverbright2x; }
    if (src == d3d::BLEND_BOTHSRCALPHA) { src = d3d::BLEND_SRCALPHA; dst = d3d::BLEND_INVSRCALPHA; }
    if (src == d3d::BLEND_BOTHINVSRCALPHA) { src = d3d::BLEND_INVSRCALPHA; dst = d3d::BLEND_SRCALPHA; }
    // The equation only matters while blending, but Vulkan wants it set once per command buffer regardless.
    if (c.src == ~0u || (blend && (c.src != src || c.dst != dst))) {
        VkColorBlendEquationEXT eq{};
        eq.srcColorBlendFactor = eq.srcAlphaBlendFactor = BlendFactor(src);
        eq.dstColorBlendFactor = eq.dstAlphaBlendFactor = BlendFactor(dst);
        if (uiLayer) {
            // Coverage: a draw over what is there (alpha blended, premultiplied, unblended) adds its alpha's; one adding
            // to it or multiplying it covers nothing.
            bool multiplies = src == d3d::BLEND_DESTCOLOR || src == d3d::BLEND_INVDESTCOLOR || dst == d3d::BLEND_SRCCOLOR ||
                              dst == d3d::BLEND_INVSRCCOLOR || src == kBlendOverbright2x;
            bool covers = !multiplies && (dst == d3d::BLEND_INVSRCALPHA || dst == d3d::BLEND_ZERO);
            eq.srcAlphaBlendFactor = covers ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ZERO;
            eq.dstAlphaBlendFactor = covers && dst == d3d::BLEND_INVSRCALPHA ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA
                                     : covers ? VK_BLEND_FACTOR_ZERO : VK_BLEND_FACTOR_ONE;
        }
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
            blendDirty |= 8u;
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
            blendDirty |= 4u;
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
            blendDirty |= 16u;
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
            uint32_t masks[5] = {all, GlowDraw(fvf) || GlowOccluder(fvf) ? all : 0u,
                                 blend && fSrc == d3d::BLEND_ZERO && fDst == d3d::BLEND_ONE ? 0u : all,
                                 keep ? 0u : all,
                                 blend && aSrc == d3d::BLEND_ZERO && aDst == d3d::BLEND_ONE ? 0u : all};
            if (m_drawTerrainBase) {              // the ground's light pass replaces these two (see Draw)
                masks[2] = 0u;
                masks[3] = 0u;
            } else if (m_drawTerrainLight) {      // its motion is zero either way (the base pass skipped it too)
                masks[3] = 0u;
            }
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
    if (blendDirty) {
        // From the first changed attachment to the last, when every one between is known; else each run of changed.
        const uint32_t now[5] = {c.blendEnable, c.glowBlendSet ? 1u : ~0u, c.fractionEnable, c.motionKeep, c.albedoEnable};
        const int lo = std::countr_zero(blendDirty), hi = 31 - std::countl_zero(blendDirty);
        bool known = true;
        for (int i = lo; i <= hi; ++i) known = known && now[i] != ~0u;
        VkBool32 e[5];
        for (int i = 0; i < 5; ++i) e[i] = now[i] == 1u ? VK_TRUE : VK_FALSE;
        if (known) {
            vkCmdSetColorBlendEnableEXT(cmd, uint32_t(lo), uint32_t(hi - lo + 1), e + lo);
        } else {
            for (int i = lo; i <= hi;) {
                if (!(blendDirty >> i & 1u)) { ++i; continue; }
                int j = i;
                while (j + 1 <= hi && (blendDirty >> (j + 1) & 1u)) ++j;
                vkCmdSetColorBlendEnableEXT(cmd, uint32_t(i), uint32_t(j - i + 1), e + i);
                i = j + 1;
            }
        }
    }

    if (c.fvf != fvf) {
        SetVertexInput(cmd, fvf, stride);
        c.fvf = fvf;
    }
}

void Device::BindGeometry(VkCommandBuffer cmd, VkBuffer vb, VkBuffer ib)
{
    StateCache& c = m_cache;
    if (c.vb != vb) {
        VkBuffer buffers[2] = {vb, m_nullBuffer};
        VkDeviceSize offsets[2] = {0, 0};
        vkCmdBindVertexBuffers(cmd, 0, 2, buffers, offsets);
        c.vb = vb;
    }
    if (ib && c.ib != ib) {
        vkCmdBindIndexBuffer(cmd, ib, 0, VK_INDEX_TYPE_UINT16);
        c.ib = ib;
    }
}

// The main pass's per-draw bindings: 4 the frame lights (rebuilt when the lights change), 8 an animated mesh's last
// positions, 10 a tessellated draw's averaged normals (else any small part of the ring, unread). Pushed only when one of
// them differs from the last push in this command buffer (most draws: the frame's lights and the unread stand-ins,
// again). A push updates only the bindings it names, so the others stay.
void Device::PushDrawSet(VkCommandBuffer cmd, VkDeviceSize frameLightsOffset, VkBuffer prevBuffer, VkDeviceSize prevOffset,
                         VkDeviceSize prevBytes, VkBuffer smoothBuffer, VkDeviceSize smoothOffset, VkDeviceSize smoothBytes)
{
    Frame& f = m_frames[m_frameIndex];
    VkDescriptorBufferInfo frameLights{f.ring, frameLightsOffset, sizeof(FrameLights)};
    VkDescriptorBufferInfo prevPositions{prevBuffer, prevOffset, prevBytes ? prevBytes : 16};
    VkDescriptorBufferInfo smoothNormals{smoothBuffer, smoothOffset, smoothBytes ? smoothBytes : 16};
    StateCache& pc = m_cache;
    bool pushDraw = !pc.drawSetValid || pc.frameLights != frameLightsOffset || pc.prevBuffer != prevPositions.buffer ||
                    pc.prevOffset != prevPositions.offset || pc.prevRange != prevPositions.range ||
                    pc.smoothBuffer != smoothNormals.buffer || pc.smoothOffset != smoothNormals.offset ||
                    pc.smoothRange != smoothNormals.range;
    if (!pushDraw)
        return;
    pc.drawSetValid = true;
    pc.frameLights = frameLightsOffset;
    pc.prevBuffer = prevPositions.buffer;
    pc.prevOffset = prevPositions.offset;
    pc.prevRange = prevPositions.range;
    pc.smoothBuffer = smoothNormals.buffer;
    pc.smoothOffset = smoothNormals.offset;
    pc.smoothRange = smoothNormals.range;
    VkWriteDescriptorSet writes[3] = {};
    auto write = [&](int i, uint32_t binding, VkDescriptorType type) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstBinding = binding;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = type;
    };
    write(0, 4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);          writes[0].pBufferInfo = &frameLights;
    write(1, 8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);          writes[1].pBufferInfo = &prevPositions;
    write(2, 10, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);         writes[2].pBufferInfo = &smoothNormals;
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 3, writes);
}

// Two-pass foliage's second pass: the queued soft edges, drawn now - blended, depth tested, no depth written - over
// what the scene has become since their cores were drawn. Each is replayed with the state it was drawn with (cull,
// viewport, its buffers and per-draw bindings; its own record, motion.w = 2) through ApplyDynamicState,
// which reads that state from the render states and the draw's flags: those are set for it and put back after.
// The scene's frame-wide bindings (rvk.h): the frame's pushed set - the two arrays (0 = constants, 12 = records) and the
// shadow maps (5, 6, 9), constant for the frame's command buffer: pushed once, not on every non-extending draw - and
// set 1, the bindless table. Again after any pass with its own layout (they clear m_arenaBound, m_bindlessBound).
void Device::BindSceneArena(VkCommandBuffer cmd)
{
    Frame& f = m_frames[m_frameIndex];
    if (!m_arenaBound) {
        VkDescriptorBufferInfo consts{f.ring, m_constsBase, VkDeviceSize(m_constCapacity) * sizeof(DrawConstants)};
        VkDescriptorBufferInfo records{f.ring, m_recordsBase, VkDeviceSize(m_recordCapacity) * sizeof(DrawRecord)};
        VkDescriptorImageInfo shadow{m_shadowSampler, m_shadowView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo cubes{m_cubeSampler, m_cubeArrayView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo shadowDepths{m_shadowDepthSampler, m_shadowView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet arena[5] = {};
        auto set = [&](int i, uint32_t binding, VkDescriptorType type, const void* info) {
            arena[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            arena[i].dstBinding = binding;
            arena[i].descriptorCount = 1;
            arena[i].descriptorType = type;
            if (type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
                arena[i].pBufferInfo = static_cast<const VkDescriptorBufferInfo*>(info);
            else
                arena[i].pImageInfo = static_cast<const VkDescriptorImageInfo*>(info);
        };
        set(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &consts);
        set(1, 12, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &records);
        set(2, 5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &shadow);
        set(3, 6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &cubes);
        set(4, 9, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &shadowDepths);
        vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 5, arena);
        m_arenaBound = true;
    }
    if (!m_bindlessBound) {                       // set 1, once per frame's command buffer
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 1, 1, &m_bindlessSet, 0, nullptr);
        m_bindlessBound = true;
    }
}

void Device::FlushEdges()
{
    if (m_edges.empty())
        return;
    if (!m_rendering) {                          // (nothing to draw into: dropped)
        m_edges.clear();
        return;
    }
    FlushGroup();                                // the batched draws before them go first
    VkCommandBuffer cmd = m_frames[m_frameIndex].main;
    const uint32_t keys[7] = {d3d::RS_CULLMODE, d3d::RS_ZENABLE, d3d::RS_ZWRITEENABLE, d3d::RS_ZFUNC,
                              d3d::RS_ALPHABLENDENABLE, d3d::RS_SRCBLEND, d3d::RS_DESTBLEND};
    uint32_t saved[7];
    for (int i = 0; i < 7; ++i) saved[i] = m_rs[keys[i]];
    const d3d::Viewport savedViewport = m_viewport;
    const bool mayDiscard = m_drawMayDiscard, tess = m_drawTess, label = m_drawIsLabel, prepassed = m_drawPrepassed,
               backdrop = m_drawBackdrop, overbright = m_drawOverbright2x, terrainBase = m_drawTerrainBase,
               terrainLight = m_drawTerrainLight, lite = m_drawLite;
    m_drawMayDiscard = true;
    m_drawLite = false;                          // (3D foliage: the full shader)
    m_drawTess = m_drawIsLabel = m_drawBackdrop = m_drawOverbright2x = m_drawTerrainBase = m_drawTerrainLight = false;
    m_drawGround = m_drawNoFrameLights = false;
    // Depth compared strictly (LESS, whatever the draw had): where the plant's own core wrote its depth the edge has
    // nothing to draw (alpha from the core up), and fails there before its shader runs rather than after.
    m_drawPrepassed = false;
    for (const DeferredEdge& e : m_edges) {
        // (Its record in another generation's arena - the ring re-reserved since it was queued: not drawn. And the
        // scene's bindings pushed again if a pass came between - the sky's, the clouds', the interface layer's: the
        // edges read whatever was bound - another draw's place and textures - blobs of wrong colour round the plants
        // and tables, changing with every interface update - user, 2026-10-19.)
        if (e.ringGeneration != m_ringGeneration)
            continue;
        const uint32_t values[7] = {e.cull, 1u, 0u, d3d::CMP_LESS, 1u, d3d::BLEND_SRCALPHA, d3d::BLEND_INVSRCALPHA};
        for (int i = 0; i < 7; ++i) m_rs[keys[i]] = values[i];
        m_viewport = e.viewport;
        ApplyDynamicState(e.primitive, e.fvf, e.stride);
        BindGeometry(cmd, e.vb, e.ib);
        BindSceneArena(cmd);
        PushDrawSet(cmd, e.frameLights, e.prevBuffer, e.prevOffset, e.prevBytes, e.smoothBuffer, e.smoothOffset,
                    e.smoothBytes);
        if (e.indexCount)
            vkCmdDrawIndexed(cmd, e.indexCount, 1, uint32_t(e.ibOffset / 2), int32_t(uint32_t(e.vbOffset) / e.stride), e.record);
        else
            vkCmdDraw(cmd, e.vertexCount, 1, uint32_t(e.vbOffset) / e.stride, e.record);
    }
    ++m_edgeFlushes;
    m_edgeDraws += uint32_t(m_edges.size());
    m_edges.clear();
    for (int i = 0; i < 7; ++i) m_rs[keys[i]] = saved[i];
    m_viewport = savedViewport;
    m_drawMayDiscard = mayDiscard;
    m_drawTess = tess;
    m_drawIsLabel = label;
    m_drawPrepassed = prepassed;
    m_drawBackdrop = backdrop;
    m_drawOverbright2x = overbright;
    m_drawTerrainBase = terrainBase;
    m_drawTerrainLight = terrainLight;
    m_drawLite = lite;
}

// A fingerprint of everything an instanced batch must share (geometry, format, textures, state); 0 = not mergeable.
// Consecutive equal keys are draws one instanced draw could cover (their world / per-draw block differs per instance).
void Device::NoteBatch(uint64_t key)
{
    if (key && key == m_batchKey) {
        ++m_batchRun;
        ++m_batchMerged;
        if (m_batchRun == 2) ++m_batchRuns;
        if (m_batchRun > m_batchMaxRun) m_batchMaxRun = m_batchRun;
    } else {
        m_batchKey = key;
        m_batchRun = key ? 1 : 0;
    }
}

// One draw's indirect command (M3): this draw's geometry, with the record index in firstInstance (the shader's
// gl_InstanceIndex). Consecutive draws with the same key become one vkCmdDrawIndexedIndirect (FlushGroup).
void Device::RecordIndirect(uint32_t indexCount, VkDeviceSize ibOffset, VkDeviceSize vbOffset, uint32_t stride,
                            uint32_t recordIndex, uint64_t key)
{
    Frame& f = m_frames[m_frameIndex];
    auto* cmds = reinterpret_cast<VkDrawIndexedIndirectCommand*>(f.ringData + m_indirectBase);
    VkDrawIndexedIndirectCommand& c = cmds[m_indirectCount++];
    c.indexCount = indexCount;
    c.instanceCount = 1;
    c.firstIndex = uint32_t(ibOffset / 2);
    c.vertexOffset = int32_t(uint32_t(vbOffset) / stride);   // (32-bit: the buffers are far below 4 GB, and a
                                                             // 64-bit division is a library call in 32-bit code)
    c.firstInstance = recordIndex;
    if (m_group.active && m_group.key == key) {
        ++m_group.count;
    } else {
        m_group.active = true;
        m_group.key = key;
        m_group.first = m_indirectCount - 1;
        m_group.count = 1;
    }
}

// Issues the pending group as one indirect draw; the pipeline, dynamic state, descriptors and buffers are the
// group's (nothing may have changed them since its last draw). Called before anything that would.
void Device::FlushGroup()
{
    if (!m_group.active)
        return;
    m_group.active = false;
    if (!m_group.count)
        return;
    Frame& f = m_frames[m_frameIndex];
    vkCmdDrawIndexedIndirect(f.main, f.ring,
                             m_indirectBase + VkDeviceSize(m_group.first) * sizeof(VkDrawIndexedIndirectCommand),
                             m_group.count, sizeof(VkDrawIndexedIndirectCommand));
    NoteGroup(m_group.count);
    m_group.count = 0;
}

// The scene's fog and ambient, from the ground's base pass (its lighting pass's fog is white): the sky's and the
// grading's references. Also taken when the base pass is deferred (RVK_GroundOne: native_ground.cpp).
void Device::NoteSceneFog()
{
    RenderStateFog(m_sceneFogColour, m_sceneFogParams);
    m_sceneFogFrame = m_frameNumber;
    if (m_sceneAmbientFrame != m_frameNumber) {   // (once a frame: eased - a zone's change shouldn't pop)
        const uint32_t a = m_rs[d3d::RS_AMBIENT];
        const float luma = (0.3f * float((a >> 16) & 0xFF) + 0.59f * float((a >> 8) & 0xFF) + 0.11f * float(a & 0xFF)) / 255.0f;
        m_sceneAmbient = m_sceneAmbient < 0.0f ? luma : m_sceneAmbient + (luma - m_sceneAmbient) * (1.0f - std::exp(-m_frameDt / 0.5f));
        m_sceneAmbientFrame = m_frameNumber;
    }
}

void Device::Draw(uint32_t primitive, uint32_t fvf, const void* vertices, uint32_t vertexCount,
                  const uint16_t* indices, uint32_t indexCount)
{
    if (!m_inFrame || !vertexCount)
        return;
    // A shop's room behind a doorway drawn this frame (portal.cpp): before the first draw that writes no depth - the
    // effects and particles, which then go over it (drawn at the scene's end, it covered them). Not sooner: solid
    // things drawn after the doorway, behind its plane (the building's own insides), showed in it over the room.
    if (m_portalArmed && !m_drawingPortal && m_scenePhase && m_target == m_scene &&
        (m_external || !m_rs[d3d::RS_ZENABLE] || !m_rs[d3d::RS_ZWRITEENABLE]))
        DrawPortals();
    // The game's ground, drawn by rvk from the playfield's data instead (native_ground.cpp): its own draws skipped.
    // (Its base pass deferred to the lighting pass - RVK_GroundOne - but something else came first: drawn now.)
    if (m_native.basePending && !m_native.drawing &&
        !(m_drawVisualKind == uint32_t(VisualKind::Terrain) && IsTerrain(fvf)))
        DrawPendingGroundBase();
    if (m_native.ground && NativeGroundReplaces(fvf))
        return;
    // The black texture's panel in a shop's entrance (1010004:6014), while the street is drawn through it
    // (portal.cpp DrawStreetPortal, the frame before): it stands in the door's plane, so the opening, set behind the
    // door's leaf to keep a shut door shut, would find it in front and stay black.
    if (m_streetShownFrame + 1 >= m_frameNumber && m_indoors && !m_drawingPortal && m_textures[0] &&
        m_textures[0]->m_rdbId == 6014 && (fvf & 0x00E) != 0x004) {   // (not a 2D draw)
        // (All of it in the entrance's plane, across the entrance's middle, from its base to a door's height. Not its
        // first corner near the entrance: in a last_ditch_building_medium3 the panel spans much of the wall, the door's
        // opening only a part of it - the user's dump, 2026-10-26.)
        const auto& w = m_world.m;
        const float* e = m_streetRoom.entrance;
        const float in[2] = {m_streetRoom.inward[0], m_streetRoom.inward[2]}, across[2] = {-in[1], in[0]};
        const uint32_t stride = FvfStride(fvf);
        float o0 = 1e30f, o1 = -1e30f, a0 = 1e30f, a1 = -1e30f, u0 = 1e30f, u1 = -1e30f;
        for (uint32_t v = 0; vertices && v < std::min(vertexCount, 64u); ++v) {
            float p[3], at[3];
            std::memcpy(p, static_cast<const uint8_t*>(vertices) + size_t(v) * stride, 12);
            for (int j = 0; j < 3; ++j) at[j] = p[0] * w[0][j] + p[1] * w[1][j] + p[2] * w[2][j] + w[3][j];
            const float dx = at[0] - e[0], dz = at[2] - e[2];
            const float o = dx * in[0] + dz * in[1], a = dx * across[0] + dz * across[1], u = at[1] - e[1];
            o0 = std::min(o0, o); o1 = std::max(o1, o);
            a0 = std::min(a0, a); a1 = std::max(a1, a);
            u0 = std::min(u0, u); u1 = std::max(u1, u);
        }
        if (o0 > -0.5f && o1 < 0.5f && a0 < 0.0f && a1 > 0.0f && a1 - a0 < 16.0f && u0 < 2.5f && u1 > 0.2f)
            return;
    }
    // RVK_FrameGen: a 2D draw on the frame after the interface (the mouse pointer) goes on top of the overlay.
    if (m_fgUiLayer && m_target == m_main && !m_fgTopActive && !m_uiLayerActive && !m_external &&
        m_interfaceEndFrame == m_frameNumber && IsInterfaceDraw(fvf))
        FgTopBegin();
    m_drawObject = nullptr;                      // (found below for a static mesh)
    struct ObjectScope {                         // ... and none once the draw is done (DrawWorldBox, FrameLightMask
        Device* d;                               // read it: outside a draw they must work from the matrix)
        ~ObjectScope() { d->m_drawObject = nullptr; }
    } objectScope{this};
    // The game's storm tint (a sandstorm's, an ash storm's): a screen-sized blended sprite a few units ahead, drawn over
    // everything with depth writes (an in-game dump: RSprite, 8 x 8 texture, ZFUNC ALWAYS). A flat sheet over the
    // frame: with the new weather it's left out (RVK_StormTint brings it back) - our storms draw the air themselves.
    std::vector<uint8_t> steadySheet;            // (the sheet drawn: its vertices with its alpha eased - below)
    if (m_rs[d3d::RS_ALPHABLENDENABLE] && m_rs[d3d::RS_ZFUNC] == d3d::CMP_ALWAYS && m_rs[d3d::RS_ZENABLE] &&
        m_rs[d3d::RS_ZWRITEENABLE] && (fvf & d3d::FVF_POSITION_MASK) != d3d::FVF_XYZRHW && m_textures[0] &&
        m_textures[0]->m_width <= 16 && m_textures[0]->m_height <= 16 && vertices && vertexCount <= 64) {
        const FvfLayout layout = DecodeFvf(fvf);  // (its colour and strength: our storm's air, wind.cpp SandHaze)
        uint32_t colour = 0;
        if (layout.offset[2] >= 0)
            std::memcpy(&colour, static_cast<const uint8_t*>(vertices) + layout.offset[2], 4);
        // A black one is no storm: the game's fade in from black after a load (the same sprite) - drawn as it is.
        const uint32_t brightest = std::max({(colour >> 16) & 0xFFu, (colour >> 8) & 0xFFu, colour & 0xFFu});
        if (brightest > 0x10) {
            if (m_weatherParams.enable && !m_stormTint && m_target == m_scene) {
                ++m_stormTintSkipped;
                m_stormTintColour = colour;
                m_stormTintSeen = m_frameClock;
                return;
            }
            // Drawn (RVK_StormTint, or the game's own look): the game sets its alpha afresh each frame at random about
            // its strength (a run of frame dumps: 26..63 frame to frame) - the whole screen flickers in its storms, a
            // native AO effect (the user). Its alpha eased (~1 s) instead: the storm's strength drifts, no flicker.
            const float target = float(colour >> 24);
            const double now = m_frameClock;
            const float dt = float(std::clamp(now - m_stormSheetTime, 0.0, 1.0));
            m_stormSheetAlpha = now - m_stormSheetTime > 2.0 ? target
                                                             : m_stormSheetAlpha + (target - m_stormSheetAlpha) * (1.0f - std::exp(-dt / 1.0f));
            m_stormSheetTime = now;
            const uint32_t stride = FvfStride(fvf);
            if (stride && layout.offset[2] >= 0) {
                steadySheet.assign(static_cast<const uint8_t*>(vertices), static_cast<const uint8_t*>(vertices) + size_t(stride) * vertexCount);
                const uint32_t a = uint32_t(std::clamp(m_stormSheetAlpha + 0.5f, 0.0f, 255.0f));
                for (uint32_t i = 0; i < vertexCount; ++i) {
                    uint32_t c;
                    std::memcpy(&c, steadySheet.data() + size_t(i) * stride + layout.offset[2], 4);
                    c = (c & 0x00FFFFFFu) | (a << 24);
                    std::memcpy(steadySheet.data() + size_t(i) * stride + layout.offset[2], &c, 4);
                }
                vertices = steadySheet.data();
                m_drawStaticBuffer = VK_NULL_HANDLE;     // (from these vertices, not a retained copy)
                m_drawStaticIndexBuffer = VK_NULL_HANDLE;
            }
        }
    }
    // The game's lightning glow (VisualCloudlight_t: a big additive mesh at the clouds' height, its vertex colours
    // pulsing with each strike - the whole sky flickers; an in-game run of frame dumps): with the new weather left out
    // (RVK_StormFlash brings it back). The bolts (VisualLightning_t) stay.
    if (m_weatherParams.enable && std::strncmp(m_drawVisualName, "VisualCloudlight_t", 18) == 0) {
        // (How bright it flares - additive: its colours x their alpha - and where: our flash's strike, rain.cpp.)
        const FvfLayout layout = DecodeFvf(fvf);
        if (vertices && layout.offset[0] >= 0 && layout.offset[2] >= 0) {
            const uint8_t* base = static_cast<const uint8_t*>(vertices);
            float bright = 0.0f, mid[3] = {0, 0, 0};
            for (uint32_t i = 0; i < vertexCount; ++i) {
                const uint8_t* vtx = base + size_t(i) * layout.stride;
                uint32_t c;
                std::memcpy(&c, vtx + layout.offset[2], 4);
                const float a = float(c >> 24) / 255.0f;
                const float l = (0.3f * float((c >> 16) & 0xFF) + 0.59f * float((c >> 8) & 0xFF) + 0.11f * float(c & 0xFF)) / 255.0f;
                bright = std::max(bright, a * l);
                float p[3];
                std::memcpy(p, vtx + layout.offset[0], 12);
                for (int k = 0; k < 3; ++k) mid[k] += p[k];
            }
            if (bright > m_glowSeen) {
                m_glowSeen = bright;
                const auto& w = m_world.m;
                for (int k = 0; k < 3; ++k) mid[k] /= float(vertexCount);
                for (int k = 0; k < 3; ++k)
                    m_glowPos[k] = mid[0] * w[0][k] + mid[1] * w[1][k] + mid[2] * w[2][k] + w[3][k];
            }
        }
        if (!m_stormFlash) {
            ++m_stormFlashSkipped;
            return;
        }
    }
    // The game's bolts (VisualLightning_t): left out while we draw our own (RVK_StormBolt; rain.cpp MakeBolt).
    if (m_weatherParams.enable && m_stormBolt > 0.0f && std::strncmp(m_drawVisualName, "VisualLightning_t", 17) == 0)
        return;
    ++m_frameDraw;
    // GPU profiling: which class of scene draw this is (splits the "scene" block; see ProfileSceneClass).
    m_drawSceneClass = 0;
    if (m_scenePhase && m_target == m_scene && !m_external &&
        (fvf & d3d::FVF_POSITION_MASK) != d3d::FVF_XYZRHW) {
        // Classes: ProfileSceneClass's names. Terrain and foliage by how they are drawn, the rest by the game's
        // visual kind (SetDrawVisual).
        int cls;
        VisualKind kind = VisualKind(m_drawVisualKind);
        if (IsTerrain(fvf))
            cls = IsMultiplyPass() ? 3 : 2;
        else if (m_rs[d3d::RS_LIGHTING] && (m_rs[d3d::RS_ALPHATESTENABLE] || m_rs[d3d::RS_ALPHABLENDENABLE]) &&
                 m_textures[0] && !m_textures[0]->m_opaque && (m_tss[0][d3d::TSS_TEXCOORDINDEX] & 0xFFFF0000u) == 0)
            cls = 4;                                 // as the foliage flag (Draw): a texture with holes
        else if (kind == VisualKind::Character || kind == VisualKind::CharacterPart)
            cls = 5;
        else if (kind == VisualKind::Effect || kind == VisualKind::BlobShadow)
            cls = 6;
        else if (kind == VisualKind::Sky)
            cls = 7;
        else if (kind == VisualKind::Water)
            cls = 8;
        else if (kind == VisualKind::Room)
            cls = 9;
        else if (kind == VisualKind::Static)
            cls = 1;
        else
            cls = 10;                                // Unknown / Other
        ProfileSceneClass(cls);
        m_drawSceneClass = cls;
    }
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
                m_aoCameraGen = 0;               // (AoCamera: asked again)
            }
        } else if (m_sceneSaw3D && IsInterfaceDraw(fvf)) {
            m_sceneEndDraw = m_dumpDraw;
            m_sceneEndFvf = fvf;
            EndScene();
        }
    }
    // RVK_FrameGen: a 2D draw on the frame between the 3D scene and the interface (AOReloaded's cast bars, the text
    // after zoning) goes into the labels' layer, under the interface - in the frame it was interpolated, smearing.
    if (m_fgUiLayer && !m_scenePhase && m_target == m_main && !m_fgUnderActive && !m_fgLabelsActive && !m_fgTopActive &&
        !m_uiLayerActive && !m_uiLayerOpen && !m_external && m_sceneSaw3D && m_interfaceEndFrame != m_frameNumber &&
        IsInterfaceDraw(fvf))
        FgUnderBegin();
    // The sky's twilight and glow (sky.cpp): drawn before the sky's second draw - after its dome, before the moons,
    // the suns and the clouds - or, a sky of one draw, at its end.
    if (m_scenePhase && m_target == m_scene && !m_external && (fvf & d3d::FVF_POSITION_MASK) != d3d::FVF_XYZRHW) {
        if (VisualKind(m_drawVisualKind) == VisualKind::Sky) {
            if (m_skyDrawFrame != m_frameNumber) { m_skyDrawFrame = m_frameNumber; m_skyDraws = 0; }
            // Our own sky (RVK_ProcSky; atmosphere.cpp): drawn before the sky's first draw, whatever it is (the stars
            // first, and ours later would have covered them); that draw, if it is the game's dome - a lit unblended
            // mesh - not drawn. The stars, moons, suns and sheets after it as the game draws them.
            const bool ours = ++m_skyDraws == 1 && ProcSkyReady();
            if (ours)
                DrawSkyGlow();
            if (ours && primitive == d3d::TriangleList && m_rs[d3d::RS_LIGHTING] && !m_rs[d3d::RS_ALPHABLENDENABLE] &&
                vertexCount >= 100) {
                const Texture* t = m_textures[0];
                const uint32_t id = t ? t->m_rdbId : 0u;
                if (vertexCount != m_domeSkipVerts || id != m_domeSkipTexture) {
                    m_domeSkipVerts = vertexCount;
                    m_domeSkipTexture = id;
                    Log("sky: the game's dome (%u vertices, texture %u) replaced by ours", vertexCount, id);
                }
                NoteDuskSky(primitive, vertexCount);
                if (m_dumpFile) std::fprintf(m_dumpFile, "  (the game's dome: not drawn - ours, RVK_ProcSky)\n");
                return;
            }
            // (... and the game's horizon band - a lit, blended 121-vertex ring in the zone's colour, drawn after the
            // moons: under our sky it stood as a teal strip along the horizon.)
            if (ProcSkyReady() && primitive == d3d::TriangleList && vertexCount == 121 && m_rs[d3d::RS_LIGHTING] &&
                m_rs[d3d::RS_ALPHABLENDENABLE]) {
                if (!m_bandSkipLogged) {
                    m_bandSkipLogged = true;
                    Log("sky: the game's horizon band (121 vertices) left out under ours");
                }
                if (m_dumpFile) std::fprintf(m_dumpFile, "  (the game's horizon band: not drawn - RVK_ProcSky)\n");
                return;
            }
            // (... and the game's sky tint: a screen-wide quad of 6 vertices, unlit and blended, in the time of day's
            // colour - teal, violet, grey-blue - laid over the whole sky: under ours it turned the sunset teal.)
            if (ProcSkyReady() && primitive == d3d::TriangleStrip && vertexCount == 6 && !m_rs[d3d::RS_LIGHTING] &&
                m_rs[d3d::RS_ALPHABLENDENABLE]) {
                if (!m_tintSkipLogged) {
                    m_tintSkipLogged = true;
                    Log("sky: the game's sky tint (6 vertices) left out under ours");
                }
                if (m_dumpFile) std::fprintf(m_dumpFile, "  (the game's sky tint: not drawn - RVK_ProcSky)\n");
                return;
            }
            // (... and its four textured sky layers round the eye - lit, blended 584-vertex meshes: hidden by our cloud
            // layer, but with it off they lay teal streaks over our sky - user, 2026-10-18.)
            if (ProcSkyReady() && m_drawIsSkyMesh && primitive == d3d::TriangleList && vertexCount == 584 &&
                m_rs[d3d::RS_ALPHABLENDENABLE]) {
                if (!m_layersSkipLogged) {
                    m_layersSkipLogged = true;
                    Log("sky: the game's sky layers (584 vertices) left out under ours");
                }
                if (m_dumpFile) std::fprintf(m_dumpFile, "  (the game's sky layer: not drawn - RVK_ProcSky)\n");
                return;
            }
            // (... and its stars - its point lists, 500 points of a pixel - under our own (RVK_Stars; sky_glow.frag).
            // Still counted for the dusk log.)
            if (ProcSkyReady() && m_nightSky.stars && primitive == d3d::PointList) {
                NoteDuskSky(primitive, vertexCount);
                if (!m_starsSkipLogged) {
                    m_starsSkipLogged = true;
                    Log("sky: the game's stars (%u points) left out under ours", vertexCount);
                }
                if (m_dumpFile) std::fprintf(m_dumpFile, "  (the game's stars: not drawn - RVK_Stars)\n");
                return;
            }
            if (m_skyDraws == 2) DrawSkyGlow();
        } else if (m_skyDrawFrame == m_frameNumber && m_skyGlowFrame != m_frameNumber) {
            DrawSkyGlow();
        }
    }
    // RVK_CloudsOn: the game's cloud puffs taken instead of drawn; drawn as volumes where the sky ends - before the
    // frame's first 3D draw after them that isn't the sky's.
    // Looking down or away the game draws no puffs: with the layer, the last ones seen (a few seconds), so its shadows
    // on the ground stay (they had gone, and the weather with them).
    if (m_cloudPuffs.empty() && !m_cloudsDrawn && !m_cloudPuffsLast.empty() && m_cloudParams.enable &&
        m_cloudParams.layer && m_cloudPuffsLastFrame + 300 >= m_frameNumber && m_scenePhase && m_target == m_scene &&
        !m_external && VisualKind(m_drawVisualKind) != VisualKind::Sky &&
        (fvf & d3d::FVF_POSITION_MASK) != d3d::FVF_XYZRHW)
        m_cloudPuffs = m_cloudPuffsLast;
    if (!m_cloudPuffs.empty() && !m_cloudsDrawn && m_scenePhase && m_target == m_scene && !m_external &&
        VisualKind(m_drawVisualKind) != VisualKind::Sky && (fvf & d3d::FVF_POSITION_MASK) != d3d::FVF_XYZRHW)
        DrawClouds(m_frames[m_frameIndex].main);
    // The suns' glow over the clouds (sun.cpp, RVK_SunBleed): at the sky's end, after the cloud layer.
    if (m_sunDrawnFrame == m_frameNumber && m_sunOverFrame != m_frameNumber && m_scenePhase && m_target == m_scene &&
        !m_external && VisualKind(m_drawVisualKind) != VisualKind::Sky && (fvf & d3d::FVF_POSITION_MASK) != d3d::FVF_XYZRHW)
        DrawSunOver();
    double since = (m_frameNumber & 15) == 0 ? ProfileCpu() : 0.0;   // per-draw CPU sections (profiling)
    m_drawIsLabel = !m_external && IsLabel(primitive, fvf, vertexCount);
    // The interface hidden (screenshots): its draws - pre-transformed, onto the frame or into the interface layer,
    // not while the 3D scene is still being drawn - and the names over characters.
    if (m_hideInterface && !m_external &&
        (m_drawIsLabel || (IsInterfaceDraw(fvf) && (m_target == m_main || m_target == m_uiLayer || m_target == m_fgTop ||
                                                  (m_fgUnderActive && m_target == m_fgLabels)) &&
                           !(m_scenePhase && m_target == m_scene))))
        return;
    // A name label in the scene: held back, drawn after the post passes (DrawHeldLabels).
    if (m_drawIsLabel && m_scenePhase && m_target == m_scene && vertices && !m_drawingHeldLabels) {
        const uint32_t stride = FvfStride(fvf);
        const auto* v = static_cast<const uint8_t*>(vertices);
        m_heldLabels.push_back({primitive, fvf, vertexCount, std::vector<uint8_t>(v, v + size_t(stride) * vertexCount), m_rs,
                                m_tss, m_textures, m_world, m_view, m_proj, m_texMatrix, m_viewport});
        return;
    }
    if (m_dumpFile && !m_external) {
        DumpDraw(primitive, fvf, m_drawGpu ? nullptr : vertices, vertexCount, indices, indexCount);
        // (Its vertex colours, averaged - what a draw-by-draw comparison of frames can't see otherwise: a colour
        // pulsing frame to frame.)
        const FvfLayout layout = DecodeFvf(fvf);
        const uint32_t stride = FvfStride(fvf);
        if (vertices && !m_drawGpu && layout.offset[2] >= 0 && stride && vertexCount) {
            double sum[4] = {};
            for (uint32_t i = 0; i < vertexCount; ++i) {
                uint32_t c;
                std::memcpy(&c, static_cast<const uint8_t*>(vertices) + size_t(i) * stride + layout.offset[2], 4);
                for (int k = 0; k < 4; ++k) sum[k] += double((c >> (24 - 8 * k)) & 0xFFu);
            }
            std::fprintf(m_dumpFile, "  vc %u: argb %.0f %.0f %.0f %.0f\n", m_dumpDraw - 1, sum[0] / vertexCount,
                         sum[1] / vertexCount, sum[2] / vertexCount, sum[3] / vertexCount);
        }
    }
    m_drawIsBlobShadow = !m_external && !m_drawGpu && BlobShadowShape(primitive, fvf, vertices, vertexCount, indexCount);
    if (m_drawIsBlobShadow && IsBlobShadow(primitive, fvf, vertices, vertexCount, indexCount))
        return;                                  // replaced by sun shadows
    if (m_drawIsCloudPuff && CaptureCloudPuff(fvf, vertices, vertexCount, indices, indexCount))
        return;                                  // replaced by a volume (DrawClouds)
    if (m_drawIsSkyMesh && !m_external && !m_drawGpu)
        NoteSkyMesh(fvf, vertices, vertexCount);   // the moons (night.cpp)
    if (VisualKind(m_drawVisualKind) == VisualKind::Sky && m_scenePhase && !m_external)
        NoteDuskSky(primitive, vertexCount);     // (the dusk log: sky.cpp)
    // The sun's disc (sun.cpp): where the game draws its sun's ray sprites - in the sky, before its cloud sheets.
    if (VisualKind(m_drawVisualKind) == VisualKind::Sky && std::strncmp(m_drawVisualName, "VisualSunRays_t", 15) == 0 &&
        m_scenePhase && !m_external) {
        DrawSun();                               // (once: the suns as the game drew them last frame)
        NoteSunSprite(fvf, m_drawGpu ? nullptr : vertices, vertexCount, m_drawGpu ? nullptr : indices, indexCount);
        if (!m_gameSun)
            return;                              // (RVK_GameSun off: our disc alone)
    }
    if (IsTerrain(fvf) && IsMultiplyPass() && !m_drawingPortal)
        m_terrainLitPassCur = true;
    // (The terrain's base pass: its light pass multiplies, its fog white - not the scene's fog colour.)
    // (Not the native ground's rocks: the ground's heights are the ground's.)
    const bool groundHeights = IsTerrain(fvf) && !m_native.clutter && (!IsMultiplyPass() || m_groundOne) && m_scenePhase &&
                               m_target == m_scene && !m_external && !m_drawGpu && !m_drawingPortal;
    if (groundHeights)
        NoteTerrainHeights(fvf, vertices, vertexCount);   // (the valley mist's low ground: mist.cpp)
    if (groundHeights)
        NoteTerrainSun(fvf, vertices, vertexCount);   // (the terrain's shade from a low sun: terrain_sun.cpp)
    if (IsTerrain(fvf) && !IsMultiplyPass() && m_scenePhase && m_target == m_scene && !m_external && !m_drawingPortal)
        NoteSceneFog();
    Frame& f = m_frames[m_frameIndex];
    VkCommandBuffer cmd = f.main;
    // Ground grass (RVK_GrassOn): the blades are opaque and write depth, so they must go in before the game's first
    // blended draw of the frame. The game's grass and foliage blend without writing depth; drawn after them (as at the
    // end of the scene), our grass would pass the depth test against the ground behind them and cover them however far
    // away it is. Before their pass, their fragments depth-test against ours and blend over us only where they are
    // nearer - the right order. (Not at the sky's blended draws - its stars, layers: the sky is drawn first, and what
    // is drawn where it ends, RVK_CloudsOn's clouds, would cover the blades.)
    if (m_grassOn && !m_grassDrawnThisFrame && m_scenePhase && m_target == m_scene && !m_external && vertexCount &&
        (fvf & d3d::FVF_POSITION_MASK) == d3d::FVF_XYZ && m_rs[d3d::RS_ALPHABLENDENABLE] && !IsTerrain(fvf) &&
        VisualKind(m_drawVisualKind) != VisualKind::Sky)
        DrawGrassTiles(cmd);
    ProfileDrawSection("draw: setup (sky, terrain notes, grass tiles)", since);
    FvfLayout layout = DecodeFvf(fvf);
    // RVK_GrassOn: the terrain's ground heights feed the procedural grass (grass.cpp CaptureTerrain / RenderGrassField).
    // The native ground in one pass (RVK_GroundOne) is its base pass too: the grass takes its heights and colours from
    // it as well - with no base pass drawn, no tile was ever given its ground, and no grass grew (user, 2026-10-20).
    if (m_grassOn && !m_external && !m_drawingPortal && vertices && IsTerrain(fvf) && !m_native.clutter) {   // (not the rocks on it)
        CaptureTerrain(primitive, layout, vertices, vertexCount, indices, indexCount);
        if (m_groundOne && m_native.drawing)
            CaptureTerrain(primitive, layout, vertices, vertexCount, indices, indexCount, true);
    }
    DrawMeshInfo(fvf, layout.stride, vertices, vertexCount, indices, indexCount);
    // (A shop's room drawn into its doorway, or its door drawn again over the street: none of the scene's notes.)
    if (!m_drawingPortal && !m_redrawingDoor) {
        PortalDiagDraw(fvf, vertexCount, indexCount);   // (shop portals' phase 0: portal_diag.cpp)
        PortalStreetDiagDraw(fvf, vertexCount, indexCount, layout.stride);   // (the reverse's phase 0: portal_diag.cpp)
        StreetCaptureDraw(primitive, fvf, layout.stride, vertices, vertexCount, indices, indexCount);   // (portal_street.cpp)
        PortalCaptureDraw(primitive, fvf, layout.stride, vertices, vertexCount, indices, indexCount);   // (portal_capture.cpp)
        PortalNoteBlack(primitive, fvf, layout.stride, vertices, vertexCount, indices, indexCount);     // (portal_links.cpp)
        // The room's door at the entrance, while the street shows through it: kept to be drawn again over the street
        // (portal.cpp). Here, after DrawMeshInfo: m_drawMesh is this draw's - before it, the previous draw's, and the
        // door's planks were passed over while the draw after them (its iron straps) was taken (the user, 2026-10-26).
        // Every frame indoors, not only after one the street was drawn in: a frame without it (no world camera, say)
        // left the next without the door - gone for a frame as it opened (the user, 2026-10-26).
        if (m_streetViewOn && m_portalsOn && m_indoors && m_enteredValid && m_streetRoom.known && vertices &&
            m_scenePhase && m_target == m_scene && (fvf & d3d::FVF_POSITION_MASK) == d3d::FVF_XYZ)
            NoteStreetDoorDraw(primitive, fvf, vertices, vertexCount, indices, indexCount);
    }
    ProfileDrawSection("draw: setup (mesh info)", since);
    if (!m_drawingPortal && !m_redrawingDoor)
        NoteMeshPiece();                         // (mesh ids: which mesh the object drawing is)
    // The object's entry in the per-object cache (objects.cpp): a static mesh at its world matrix.
    if (m_objectCache && m_drawMesh && m_drawMeshStatic && !m_external && !m_drawSkin && !m_drawGpu)
        m_drawObject = FindObject();
    ProfileDrawSection("draw: setup (mesh ids)", since);
    // (A GPU-skinned character has no vertices here: DrawSkinned hands the pick its CPU-skinned ones.)
    if (m_pickActive && !m_external && !m_drawingPortal && !m_redrawingDoor && (vertices || m_pickSkinned) && (m_textures[0] || m_drawVisual) &&
        (m_target == m_scene || m_target == m_main) && (fvf & d3d::FVF_POSITION_MASK) == d3d::FVF_XYZ &&
        m_rs[d3d::RS_ZENABLE])
        PickDraw(primitive, layout, vertices ? vertices : m_pickSkinned, vertexCount, indices, indexCount, IsTerrain(fvf));
    // ... and solid objects lying on the ground keep it off where they are (not characters, effects, the sky, plants
    // or anything see-through; grass.cpp CaptureCover).
    if (m_grassOn && !m_external && !m_drawingPortal && vertices && !IsTerrain(fvf) && m_target == m_scene &&
        (fvf & d3d::FVF_POSITION_MASK) == d3d::FVF_XYZ && m_rs[d3d::RS_ZENABLE] && m_rs[d3d::RS_ZWRITEENABLE] &&
        m_rs[d3d::RS_ZFUNC] != d3d::CMP_ALWAYS && !IsMultiplyPass() && !m_drawSkin && !m_drawIsLabel &&
        (!m_textures[0] || m_textures[0]->m_opaque) && !m_rs[d3d::RS_ALPHATESTENABLE]) {
        const auto kind = VisualKind(m_drawVisualKind);
        if ((kind == VisualKind::Static || kind == VisualKind::Room || kind == VisualKind::Unknown) &&
            !CharacterDraw(fvf, vertexCount))
            CaptureCover(primitive, layout, vertices, vertexCount, indices, indexCount);
    }
    if (!m_drawingPortal && !m_redrawingDoor)
        PushCandidateDraw(fvf);
    ProfileDrawSection("draw: setup (pick, grass cover)", since);
    // A canopy (leaves.cpp): its leaves, baked the first time it is seen, drawn just before it (DrawLeaves). A solid
    // draw may be a tree's trunk (where its fallen leaves lie).
    CanopyParams(primitive, fvf, layout, vertices, vertexCount, indices, indexCount);
    if (!m_drawLeaves && !m_external && !m_drawingPortal && !m_redrawingDoor)
        NoteSolidDraw(fvf);
    // A swaying plant: its big quads split into small ones (cached), so they bend rather than tilt as a whole.
    float sway[4] = {};
    bool swaying = !m_external && SwayParams(primitive, fvf, layout.stride, vertices, vertexCount, indices, indexCount, sway);
    if (swaying) {
        const void* before = vertices;
        const uint16_t* indicesBefore = indices;
        SubdividePlant(primitive, fvf, layout, vertices, vertexCount, indices, indexCount);
        if (vertices != before)
            m_drawStaticBuffer = VK_NULL_HANDLE;     // split into new geometry: through the ring
        if (vertices != before || indices != indicesBefore)
            m_drawStaticIndexBuffer = VK_NULL_HANDLE;
    }
    ProfileDrawSection("draw: setup (canopy, plants)", since);
    // Blended SRCALPHA / INVSRCALPHA with an alpha provably 1 (AlphaOneCheck) - most of the game's statics - blends to
    // exactly its own colour, in every attachment: drawn unblended from here (its constants, batch and blend state
    // an opaque draw's: blend toggling split the batches and set the blend state ~900 times a frame over a city). After
    // the grass tiles' trigger above (the first blended draw: where they go stays the game's order); not the sky, the
    // cloud sheets or a canopy with leaves (they change the alpha in the shader), nor a translucent material (not a
    // shadow caster). The render state is put back when the draw ends.
    struct BlendRestore {
        Device* d;
        bool on;
        ~BlendRestore() { if (on) d->m_rs[d3d::RS_ALPHABLENDENABLE] = 1; }
    } blendRestore{this, false};
    if (m_opaqueBlend && m_rs[d3d::RS_ALPHABLENDENABLE] == 1 && m_rs[d3d::RS_SRCBLEND] == d3d::BLEND_SRCALPHA &&
        m_rs[d3d::RS_DESTBLEND] == d3d::BLEND_INVSRCALPHA && m_rs[d3d::RS_ZENABLE] && m_rs[d3d::RS_ZWRITEENABLE] &&
        !m_rs[d3d::RS_ALPHATESTENABLE] && !m_external && !m_drawIsLabel && !m_drawLeaves && !m_drawIsCloudPuff &&
        VisualKind(m_drawVisualKind) != VisualKind::Sky && (m_target == m_scene || m_target == m_main) &&
        (fvf & d3d::FVF_POSITION_MASK) == d3d::FVF_XYZ && !IsTerrain(fvf) &&
        !(m_rs[d3d::RS_LIGHTING] && m_material.diffuse.a < 0.5f) && AlphaOneCheck(fvf) == kPreIn) {
        m_rs[d3d::RS_ALPHABLENDENABLE] = 0;
        blendRestore.on = true;
        ++m_opaqueBlendDraws;
    }
    // Retained indices only with the retained vertices (one static draw path).
    const VkBuffer staticIb = m_drawStaticBuffer && indices ? m_drawStaticIndexBuffer : VK_NULL_HANDLE;
    // An opaque static mesh kept on the GPU, as part of the models of the static world - AMD's distance field
    // (RVK_GiMode 1, brixelizer.cpp), the ray tracing's structures (raytrace.cpp): the native ground's patches, solid
    // statics and rooms; not plants, glass, characters or effects.
    m_drawRtPlace = 0;
    if ((m_brix || m_rt) && staticIb && m_target == m_scene && !m_external && !m_drawingPortal && !m_drawGpu && !m_drawLeaves && !m_drawSkin &&
        !m_drawIsLabel && (fvf & d3d::FVF_POSITION_MASK) == d3d::FVF_XYZ && m_rs[d3d::RS_ZENABLE] &&
        m_rs[d3d::RS_ZWRITEENABLE]) {
        const auto kind = VisualKind(m_drawVisualKind);
        // (The ground: the native ground's patches, either pass - the same mesh at the same place.)
        const bool solid = IsTerrain(fvf) ? m_native.drawing :
                           ((kind == VisualKind::Static || kind == VisualKind::Room || kind == VisualKind::Unknown) &&
                            !m_rs[d3d::RS_ALPHABLENDENABLE] && !m_rs[d3d::RS_ALPHATESTENABLE] &&
                            (!m_textures[0] || m_textures[0]->m_opaque) && !CharacterDraw(fvf, vertexCount));
        if (solid && m_brix)
            BrixNoteDraw(primitive, fvf, vertices, layout.stride, vertexCount, staticIb, indexCount);
        // A still cut-out (a fence's wire, a grille, a still plant): alpha-tested against its texture - the rays test
        // its texture's alpha at their hits (raytrace.cpp). Not one that sways (its vertex shader moves it).
        const uint32_t alphaFunc = m_rs[d3d::RS_ALPHAFUNC];
        const bool cutout = !solid && !IsTerrain(fvf) &&
                            (kind == VisualKind::Static || kind == VisualKind::Room || kind == VisualKind::Unknown) &&
                            m_rs[d3d::RS_ALPHATESTENABLE] && !m_rs[d3d::RS_ALPHABLENDENABLE] && m_textures[0] &&
                            !(m_drawSway[3] > 0.5f) && layout.offset[4] >= 0 && m_tss[0][d3d::TSS_TEXCOORDINDEX] == 0 &&
                            (alphaFunc == d3d::CMP_GREATER || alphaFunc == d3d::CMP_GREATEREQUAL) &&
                            !CharacterDraw(fvf, vertexCount);
        // (Its shadow caster: drawn into the untraced layers only if the rays lack it. A piece of a placed object the
        // rays already hold from the playfield's list - raytrace.cpp RtSyncStatels - isn't taken twice.)
        if ((solid || cutout) && m_rt && !(kind == VisualKind::Static && DrawStatelCasting(vertexCount, indexCount)))
            m_drawRtPlace = RtNoteDraw(primitive, vertices, layout.stride, vertexCount, staticIb, indexCount, IsTerrain(fvf),
                                       cutout ? m_textures[0] : nullptr, float(m_rs[d3d::RS_ALPHAREF] & 0xFF) / 255.0f,
                                       uint32_t(layout.offset[4]));
    }

    // Two-pass foliage: the queued edges go before a draw that writes no depth (the effects drawn over the scene came
    // after the foliage, edges included, before).
    if (!m_edges.empty() && !(m_rs[d3d::RS_ZENABLE] && m_rs[d3d::RS_ZWRITEENABLE]))
        FlushEdges();
    // Render targets bound as textures must be readable; layout changes can't happen inside rendering.
    bool needTransition = false;
    for (Texture* t : m_textures)
        if (t && t->m_renderTarget && t->m_layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
            needTransition = true;
    if (needTransition) {
        FlushGroup();                                // the previous group's state is about to be invalidated
        FlushEdges();                                // (they belong to this rendering)
        ShadeQueryEnd();                             // a query begun in this rendering ends in it
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
    VkDeviceSize geometryBytes = m_external || m_drawGpu ? 0
                                 : (m_drawStaticBuffer ? 0 : VkDeviceSize(layout.stride) * vertexCount) +
                                       VkDeviceSize(indexCount) * 2;
    m_drawIsCharacter = CharacterDraw(fvf, vertexCount);
    float tessLevel = TessellateDraw(primitive, fvf, vertexCount);
    // GPU-driven M2: reserve this frame's arrays (with the geometry) in the ring in one go; they are only
    // (re)reserved when the ring has been started. A frame that overflowed them grows the target below.
    VkDeviceSize arenaBytes = 0;
    if (m_arenaGeneration != m_ringGeneration) {
        uint32_t consts, records;
        FitArenas(&consts, &records);
        arenaBytes = ArenaBytes(consts, records);
    }
    EnsureRingSpace(arenaBytes + sizeof(FrameLights) + geometryBytes + 3 * uboAlign +
                    layout.stride + 32 + (motion ? 12ull * vertexCount + 256 : 0) +
                    (tessLevel > 0.0f ? 12ull * vertexCount + 256 : 0));
    PrepareDrawArenas();
    // The arrays are full (a frame with more unique states or draws than reserved): submit what is recorded, wait,
    // and start over. Rare; the target of the one that filled doubles for the next frames (both doubling, a zone short
    // of constants grew the records to 44 MB they never used, and the arenas outgrew the ring). A target already
    // above its capacity waits for the bigger ring FitArenas asked for.
    const bool constsFull = m_constCount >= m_constCapacity;
    const bool recordsFull = m_recordCount + 3 > m_recordCapacity;   // (two-pass foliage: 2 records; a canopy: 1 more)
    if (constsFull || recordsFull) {
        if (constsFull && m_constWanted <= m_constCapacity)
            m_constWanted = std::min(m_constWanted * 2u, kMaxDrawConstCapacity);
        if (recordsFull && m_recordWanted <= m_recordCapacity)
            m_recordWanted = std::min(m_recordWanted * 2u, kMaxDrawRecordCapacity);
        FlushDrawArenas();
    }

    // The frame's light list (binding 4): rebuilt when lights changed; always bound, as layouts require.
    // Only lit draws read it; the others bind any in-range part of the ring. Every 3D draw: the scene shader reads the
    // frame block on all of them (the PBR settings, debug view and probe in FL.pbr / FL.pbr2, the TAA jitter) - a draw
    // without one written this frame read whatever lay at offset 0 of the ring (displaced on screen; with enhancements
    // off, a stray debug view greyed the scene, flickering). It is written once a frame, so this costs nothing per draw.
    bool needLights = (fvf & d3d::FVF_POSITION_MASK) != d3d::FVF_XYZRHW || ShadowReceiver(fvf) ||
                      ShadowCompensated(fvf) || motion;
    VkDeviceSize frameLightsOffset = m_frameLightsGeneration == m_ringGeneration ? m_frameLightsOffset : 0;
    if (needLights && (m_frameLightsDirty || m_frameLightsGeneration != m_ringGeneration))   // once per frame
        frameLightsOffset = WriteFrameLights();

    ProfileDrawSection("draw: setup", since);
    // Per-draw world matrix: written into this draw's record.
    void* cpu;
    DrawTransform dt{};
    if (m_drawingPortal)
        std::memcpy(dt.clip, m_portalClip, sizeof(dt.clip));   // (portal.cpp)
    if (m_drawLeaves) {                          // a canopy with leaves: its branches sway (ffp.vert BranchSway)
        dt.leaf[0] = m_drawLeaves->centre[0];
        dt.leaf[1] = m_drawLeaves->centre[1];
        dt.leaf[2] = m_drawLeaves->centre[2];
        dt.leaf[3] = m_drawLeaves->kind == Canopy::Palm ? -m_drawLeaves->radius : m_drawLeaves->radius;
        dt.leafSet[0] = m_drawLeaves->first;
        dt.leafSet[1] = m_drawLeafCount | m_drawLeafFall << 16 | (m_drawLeaves->art ? 0x80000000u : 0u);   // (bit 31:
                                                 // our own canopy - its cards fade out, ffp.vert)
        dt.leafSet[2] = m_drawLeaves->count;
        std::memcpy(&dt.leafSet[3], &m_drawLeafGround, 4);
    }
    VkDeviceSize prevPositionsOffset = 0, prevPositionsBytes = 0;   // binding 8 (animated meshes' last positions)
    VkBuffer prevPositionsBuffer = f.ring;
    dt.world = m_world;
    dt.prevWorld = m_world;
    dt.motion[0] = motion ? 1.0f : 0.0f;
    dt.motion[1] = dt.motion[2] = dt.motion[3] = 0.0f;
    dt.tess[0] = dt.tess[1] = dt.tess[2] = dt.tess[3] = 0.0f;
    std::memcpy(dt.sway, sway, sizeof(sway));
    std::memcpy(m_drawSway, dt.sway, sizeof(m_drawSway));
    if (m_dumpFile && m_drawSway[3] > 0.5f) {    // frame dump: how the plant sways (after its D line)
        const auto& w = m_world.m;
        std::fprintf(m_dumpFile, "  sway: axis %d base %.3f 1/h %.3f tip %.3f | model box (%.2f %.2f %.2f)-(%.2f %.2f %.2f)"
                     " | world rows (%.2f %.2f %.2f) (%.2f %.2f %.2f) (%.2f %.2f %.2f)\n",
                     int(m_drawSway[3] + 0.5f) - 1, m_drawSway[0], m_drawSway[1], m_drawSway[2],
                     m_drawMesh->boundsMin[0], m_drawMesh->boundsMin[1], m_drawMesh->boundsMin[2],
                     m_drawMesh->boundsMax[0], m_drawMesh->boundsMax[1], m_drawMesh->boundsMax[2], w[0][0], w[0][1],
                     w[0][2], w[1][0], w[1][1], w[1][2], w[2][0], w[2][1], w[2][2]);
    }
    if (m_dumpFile && m_drawLeaves) {            // frame dump: the canopy's leaves (after its D line)
        static const char* const kKinds[] = {"none", "shrub", "tree", "palm"};
        std::fprintf(m_dumpFile, "  leaves: %s, %u baked, %u drawn, %u falling slots (ground y %.2f) | crown (%.2f %.2f %.2f) r %.2f\n",
                     kKinds[uint32_t(m_drawLeaves->kind)], m_drawLeaves->count, m_drawLeafCount, m_drawLeafFall,
                     m_drawLeafGround,
                     m_drawLeaves->centre[0], m_drawLeaves->centre[1], m_drawLeaves->centre[2], m_drawLeaves->radius);
    }
    ProfileDrawSection("draw: sway", since);
    FrameLightMask(fvf, layout.stride, vertices, vertexCount, dt.lightMask);
    dt.lightMask[2] = swaying && m_grassPush > 0.0f ? PusherMask(1.4f * std::sqrt(m_grassPush) + 0.1f) : 0u;
    ProfileDrawSection("draw: light mask", since);
    if (motion && m_drawObject && m_drawObject->seenLastFrame) {
        // An object drawn last frame at this very matrix (the per-object cache): it stood still - its last matrix is
        // this one (as below's search finds), and it needn't be looked up or kept for the next frame's search. (A
        // still object that starts moving is a new entry, which searches - and finds nothing of it for that first
        // frame: no motion vector for one frame.)
    } else if (motion) {
        // Motion vectors: the same object last frame - same mesh, nearest to where this one is (within 3 units). Not
        // found (new, or a different level of detail): its current matrix, i.e. it moved with the world.
        uint64_t key = MotionKey(primitive, fvf, vertexCount, indices, indexCount);
        // This frame's positions (for next frame's match) of a mesh small enough to be a character's part, into the
        // frame's pool. Static meshes (the mesh cache: same vertices as before) need none - only animated ones are
        // compared.
        MotionFrame& cur = m_motionCur;
        uint32_t posOffset = 0, posCount = 0;
        if (vertexCount <= kMotionMaxVertices && !m_drawMeshStatic && !m_drawGpu) {
            posOffset = uint32_t(cur.positions.size());
            posCount = vertexCount * 3;
            cur.positions.resize(size_t(posOffset) + posCount);
            float* out = cur.positions.data() + posOffset;
            const uint8_t* v = static_cast<const uint8_t*>(vertices);
            for (uint32_t i = 0; i < vertexCount; ++i)
                std::memcpy(out + size_t(i) * 3, v + size_t(i) * layout.stride, 12);
        }
        // Last frame's unused entry of this mesh nearest to where this one is (within 3 units): the index's run for
        // the key between x - 3 and x + 3.
        MotionFrame& prev = m_motionPrev;
        const float x = m_world.m[3][0];
        auto at = std::lower_bound(prev.index.begin(), prev.index.end(), MotionFrame::Index{key, x - 3.0f, 0},
                                   [](const MotionFrame::Index& a, const MotionFrame::Index& b) {
                                       return a.key != b.key ? a.key < b.key : a.x < b.x;
                                   });
        {
            MotionEntry* best = nullptr;
            float bestD2 = 9.0f;
            for (; at != prev.index.end() && at->key == key && at->x <= x + 3.0f; ++at) {
                MotionEntry& e = prev.entries[at->entry];
                if (e.used) continue;
                float dx = e.world.m[3][0] - m_world.m[3][0], dy = e.world.m[3][1] - m_world.m[3][1],
                      dz = e.world.m[3][2] - m_world.m[3][2], d2 = dx * dx + dy * dy + dz * dz;
                if (d2 < bestD2) { bestD2 = d2; best = &e; }
            }
            if (best) {
                dt.prevWorld = best->world;
                best->used = true;
                // Skinned on the GPU: last frame's positions came from the same dispatch (last frame's bones).
                if (m_drawGpu && m_drawGpu->moved) {
                    prevPositionsBuffer = m_frames[m_frameIndex].skinArena;
                    prevPositionsOffset = m_drawGpu->prevOffset;
                    prevPositionsBytes = VkDeviceSize(vertexCount) * 12;
                    dt.motion[1] = 1.0f;
                }
                // Animated (its vertices changed): last frame's positions for the vertex shader (binding 8).
                const float* last = prev.positions.data() + best->positions;
                if (posCount && best->positionCount == posCount &&
                    std::memcmp(last, cur.positions.data() + posOffset, size_t(posCount) * 4) != 0) {
                    void* prevCpu;
                    prevPositionsOffset = Allocate(VkDeviceSize(posCount) * 4, m_props.limits.minStorageBufferOffsetAlignment, &prevCpu);
                    std::memcpy(prevCpu, last, size_t(posCount) * 4);
                    prevPositionsBytes = VkDeviceSize(posCount) * 4;
                    dt.motion[1] = 1.0f;
                }
            }
        }
        cur.entries.push_back({m_world, posOffset, posCount, false});
        cur.keys.push_back(key);
    }

    ProfileDrawSection("draw: motion vectors", since);
    // The big constant block: reused unless something feeding it changed since it was written.
    uint32_t texMask = (m_textures[0] ? 1u : 0u) | (m_textures[1] ? 2u : 0u);
    bool terrain = IsTerrain(fvf);
    // The game draws most statics blended (SRCALPHA/INVSRCALPHA) with depth writes, so their texture's holes don't
    // write depth (F_CUTOUT). When the texture has no transparency at all, the cut-out discard can never drop a
    // fragment, and dropping it lets early-Z reject the draw's overdraw (the blend stays, so any partial vertex
    // alpha still blends as before).
    bool cutoutTexture = !terrain && !m_drawIsLabel && m_textures[0] && m_textures[0]->m_opaque &&
                         (!m_textures[1] || m_textures[1]->m_opaque) &&
                         (fvf & d3d::FVF_POSITION_MASK) != d3d::FVF_XYZRHW && m_rs[d3d::RS_ZENABLE] &&
                         m_rs[d3d::RS_ZWRITEENABLE] && m_rs[d3d::RS_ZFUNC] != d3d::CMP_ALWAYS &&
                         m_rs[d3d::RS_ALPHABLENDENABLE] && !m_rs[d3d::RS_ALPHATESTENABLE] &&
                         m_rs[d3d::RS_SRCBLEND] == d3d::BLEND_SRCALPHA && m_rs[d3d::RS_DESTBLEND] == d3d::BLEND_INVSRCALPHA;
    if (cutoutTexture) ++m_opaqueDraws;
    bool textureHoles = m_textures[0] && !m_textures[0]->m_opaque;   // foliage (below) needs a texture with holes
    // The ground's base pass (unlit, the texture the lighting pass multiplies): its local-light fraction and motion
    // attachments are replaced by that pass (depth-equal, right after) or are zero anyway, so don't write them here
    // - the ground covers much of the screen and is overdrawn, so those writes are pure bandwidth.
    m_drawTerrainBase = terrain && !m_rs[d3d::RS_LIGHTING] && !m_rs[d3d::RS_ALPHABLENDENABLE];
    m_drawTerrainLight = terrain && m_rs[d3d::RS_LIGHTING] && IsMultiplyPass();
    // The ground: base pass textures remembered by chunk; the lighting pass (lightmap + lights, multiplying the base,
    // drawn after all base passes) takes its relief from its chunk's.
    m_drawBumpBase = nullptr;
    if (terrain && m_native.clutter) {
        // The native ground's rocks (ground_clutter.cpp): laid with rock from every side, lit by the ground's light
        // where they lie - the native ground's record, marked (terrainAt.y 2: terrain.glsl TerrainClutter).
        if (TerrainTexturingOn() && m_native.codes && m_native.ground) {
            const bool base = !m_rs[d3d::RS_LIGHTING] && !m_rs[d3d::RS_ALPHABLENDENABLE];
            dt.terrain[0] = 1.0f / m_native.ground->cellX;
            dt.terrainAt[3] = float(base ? kTerrainBase : kTerrainLight);
            // (The pebbles 3, and the range they sink into the ground over the end of: ground_pebbles.cpp, ffp.vert.)
            dt.terrainAt[1] = m_native.pebbleDraw ? 3.0f : 2.0f;
            dt.terrainAt[2] = m_native.pebbleDraw ? m_native.pebbleRange : 0.0f;
            const uint32_t codes = BindlessImage(m_native.codes) + 1;
            std::memcpy(&dt.terrain[1], &codes, 4);
            if (base) {
                const uint32_t means = m_native.means ? BindlessImage(m_native.means) + 1 : 0u;
                std::memcpy(&dt.terrainAt[0], &means, 4);
            } else {
                const uint32_t sky = m_native.sky && m_native.ao > 0.0f ? BindlessImage(m_native.sky) + 1 : 0u;
                std::memcpy(&dt.terrain[3], &sky, 4);
                dt.terrainAt[0] = 0.0f;
                if (m_groundOne && m_native.means) {   // (the one pass lays the base's colour too: its tints' means)
                    const uint32_t means = BindlessImage(m_native.means) + 1;
                    std::memcpy(&dt.terrainAt[0], &means, 4);
                }
            }
        }
    } else if (terrain && m_textures[0]) {
        // (Each pass also carries how its chunk's base texture lies on the world: terrain.glsl lays it on cliffs.)
        const uint64_t chunk = TerrainChunkKey(vertices, vertexCount, layout.stride, indices, indexCount);
        if (!m_rs[d3d::RS_LIGHTING] && !m_rs[d3d::RS_ALPHABLENDENABLE]) {
            m_terrainBases[chunk] = m_textures[0];
            if (vertices && (m_tss[0][d3d::TSS_TEXCOORDINDEX] & 0xFFFFu) == 0 &&   // (stage 0 reads set 0 as it is)
                m_tss[0][d3d::TSS_TEXTURETRANSFORMFLAGS] == 0) {
                FillTerrainRecord(dt, TerrainFitFor(chunk, vertices, vertexCount, layout, indices, indexCount),
                                  kTerrainBase, m_textures[0]);
                // (The native ground's base pass: each cell's tile from the tile-code texture - terrain.glsl.)
                if (m_native.drawing && m_native.codes && dt.terrainAt[3] > 0.0f) {
                    const uint32_t codes = BindlessImage(m_native.codes) + 1;
                    const uint32_t means = m_native.means ? BindlessImage(m_native.means) + 1 : 0u;
                    std::memcpy(&dt.terrain[1], &codes, 4);
                    std::memcpy(&dt.terrainAt[0], &means, 4);   // (the native ground: the tiles' mean colours)
                    // (Its tiles' heights, when one has any: the parallax, marched alike in both passes.)
                    if (m_native.anyHeight && m_native.maps) {
                        const uint32_t maps = BindlessImage(m_native.maps) + 1;
                        std::memcpy(&dt.terrain[2], &maps, 4);
                    }
                }
            }
        } else if (m_rs[d3d::RS_LIGHTING] && IsMultiplyPass()) {
            auto it = m_terrainBases.find(chunk);
            if (it == m_terrainBases.end() && m_groundOne)   // (the one pass: its base pass's stand-in, not drawn)
                it = m_terrainBases.emplace(chunk, m_blackTexture).first;
            if (it != m_terrainBases.end() &&
                (m_bump > 0.0f || (m_normalMaps && m_pixelLighting && it->second->m_normalMap) ||
                 (m_pbr.enabled && m_pixelLighting && it->second->m_ormMap)))
                m_drawBumpBase = it->second;
            // (Laid too where the base has no maps of its own but the cliffs have rock - its relief - and on the
            // native ground: each cell's tile's normal map.)
            if (vertices && it != m_terrainBases.end() && (m_drawBumpBase || TerrainRockOn() || m_native.drawing)) {
                FillTerrainRecord(dt, TerrainFitFor(chunk, vertices, vertexCount, layout, indices, indexCount),
                                  kTerrainLight, it->second);
                if (m_native.drawing && m_native.codes && dt.terrainAt[3] > 0.0f) {
                    const uint32_t codes = BindlessImage(m_native.codes) + 1;
                    const uint32_t maps = m_native.maps ? BindlessImage(m_native.maps) + 1 : 0u;
                    const uint32_t sky = m_native.sky && m_native.ao > 0.0f ? BindlessImage(m_native.sky) + 1 : 0u;
                    const uint32_t means = m_native.means ? BindlessImage(m_native.means) + 1 : 0u;
                    std::memcpy(&dt.terrain[1], &codes, 4);
                    std::memcpy(&dt.terrain[2], &maps, 4);
                    std::memcpy(&dt.terrain[3], &sky, 4);
                    if (means) std::memcpy(&dt.terrainAt[0], &means, 4);   // (the means and the cells' sand)
                }
            }
        }
    }
    uint32_t carrier = m_external ? 0 : CarriedLight(fvf, m_drawGpu ? nullptr : vertices, vertexCount, layout.stride);
    // The texture's own normal map (F_NORMALMAP), when it is the surface (stage 0, plain coordinates) of a per-pixel
    // lit draw; for the ground's lightmap pass, the normal map of its chunk's base texture (as the generated normals).
    Texture* normalMap = nullptr;
    bool surfaceStage0 = m_pixelLighting && m_textures[0] && !terrain && m_rs[d3d::RS_LIGHTING] &&
                         m_tss[0][d3d::TSS_COLOROP] != d3d::TOP_DISABLE &&
                         !(m_tss[0][d3d::TSS_TEXTURETRANSFORMFLAGS] & 256u) &&
                         (m_tss[0][d3d::TSS_TEXCOORDINDEX] & 0xFFFF0000u) == 0;
    if (m_normalMaps && surfaceStage0 && m_textures[0]->m_normalMap)
        normalMap = m_textures[0]->m_normalMap;
    else if (m_normalMaps && m_pixelLighting && m_drawBumpBase && m_drawBumpBase->m_normalMap)
        normalMap = m_drawBumpBase->m_normalMap;
    // Its PBR material (occlusion / roughness / metallic map), on the same terms. In the record, not the constants.
    m_drawOrm = nullptr;
    m_drawOrmBase = false;
    if (m_pbr.enabled && surfaceStage0 && m_textures[0]->m_ormMap) {
        m_drawOrm = m_textures[0]->m_ormMap;
    } else if (m_pbr.enabled && m_pixelLighting && m_drawBumpBase && m_drawBumpBase->m_ormMap) {
        m_drawOrm = m_drawBumpBase->m_ormMap;
        m_drawOrmBase = true;
    }
    // The native ground's lighting pass: its cells' tiles' material maps (terrain.glsl NativeOrm, each cell's own from
    // the maps texture); one of them stands in for the record.
    if (!m_drawOrm && m_native.drawing && m_drawTerrainLight && m_native.anyOrm && m_native.maps && m_pbr.enabled &&
        m_pixelLighting) {
        m_drawOrm = m_native.anyOrm;
        m_drawOrmBase = true;
    }
    // Its emissive map: stage 0's, wherever that texture is drawn as the surface (lit or not; not the ground's passes).
    m_drawEmissive = nullptr;
    if (m_pbr.enabled && m_pbr.emissive > 0.0f && m_textures[0] && m_textures[0]->m_emissiveMap && !terrain &&
        (fvf & d3d::FVF_POSITION_MASK) != d3d::FVF_XYZRHW &&
        m_tss[0][d3d::TSS_COLOROP] != d3d::TOP_DISABLE && !(m_tss[0][d3d::TSS_TEXTURETRANSFORMFLAGS] & 256u) &&
        (m_tss[0][d3d::TSS_TEXCOORDINDEX] & 0xFFFF0000u) == 0)
        m_drawEmissive = m_textures[0]->m_emissiveMap;
    // The foliage level of detail depends on the draw's distance, not the render state: part of the block's key, or
    // a run of plants with the same state would all get the first one's (flickering as the camera moves).
    uint32_t foliageLod = FoliageFar() ? (m_drawSway[3] > 0.5f ? 2u : 1u) : 0u;
    const bool skyScenery = m_drawMesh && m_drawMesh->skyScenery;   // (mesh ids: the ships over the cities)
    bool rewrite = m_constantsDirty || m_constantsGeneration != m_ringGeneration || m_constantsFvf != fvf ||
                   m_constantsTexMask != texMask || m_constantsTerrain != terrain || m_constantsLabel != m_drawIsLabel ||
                   m_constantsCarrier != carrier || m_constantsBumpBase != m_drawBumpBase ||
                   m_constantsFoliageLod != foliageLod || m_constantsNormalMap != normalMap ||
                   m_constantsCharacter != m_drawIsCharacter || m_constantsOpaque != cutoutTexture ||
                   m_constantsHoles != textureHoles || m_constantsSkyScenery != skyScenery ||
                   m_constantsBlend != m_rs[d3d::RS_ALPHABLENDENABLE] || m_constantsDecal != m_drawIsBlobShadow;
    if (m_dumpFile && rewrite) {                 // (frame dump: what made it so)
        const bool why[kRewhy] = {m_constantsDirty, m_constantsGeneration != m_ringGeneration, m_constantsFvf != fvf,
                                  m_constantsTexMask != texMask, m_constantsTerrain != terrain,
                                  m_constantsLabel != m_drawIsLabel, m_constantsCarrier != carrier,
                                  m_constantsBumpBase != m_drawBumpBase, m_constantsFoliageLod != foliageLod,
                                  m_constantsNormalMap != normalMap, m_constantsCharacter != m_drawIsCharacter,
                                  m_constantsOpaque != cutoutTexture, m_constantsHoles != textureHoles,
                                  m_constantsSkyScenery != skyScenery,
                                  m_constantsBlend != m_rs[d3d::RS_ALPHABLENDENABLE]};
        for (int i = 0; i < kRewhy; ++i) m_rewriteWhy[i] += why[i];
    }
    uint32_t constIndex = m_constIndex;
    bool sameArena = m_constantsGeneration == m_ringGeneration && m_constIndex < m_constCount;   // m_constIndex valid
    if (rewrite) {
    m_constantsGeneration = m_ringGeneration;
    m_constantsDirty = false;
    m_constantsFvf = fvf;
    m_constantsTexMask = texMask;
    m_constantsTerrain = terrain;
    m_constantsLabel = m_drawIsLabel;
    m_constantsCarrier = carrier;
    m_constantsBumpBase = m_drawBumpBase;
    m_constantsFoliageLod = foliageLod;
    m_constantsSkyScenery = skyScenery;
    m_constantsDecal = m_drawIsBlobShadow;
    m_constantsBlend = m_rs[d3d::RS_ALPHABLENDENABLE];
    m_constantsCharacter = m_drawIsCharacter;
    m_constantsOpaque = cutoutTexture;
    m_constantsHoles = textureHoles;
    m_constantsNormalMap = normalMap;
    DrawConstants c;                             // every field is set below (the lights up to lightInfo[0])
    c.view = m_view;
    c.proj = m_proj;
    c.texMatrix[0] = m_texMatrix[0];
    c.texMatrix[1] = m_texMatrix[1];
    c.viewport[0] = float(m_viewport.x);
    c.viewport[1] = float(m_viewport.y);
    c.viewport[2] = float(m_viewport.width);
    c.viewport[3] = float(m_viewport.height);
    Copy4(c.matDiffuse, m_material.diffuse);
    Copy4(c.matAmbient, m_material.ambient);
    Copy4(c.matSpecular, m_material.specular);
    Copy4(c.matEmissive, m_material.emissive);
    ArgbToFloat(m_rs[d3d::RS_AMBIENT], c.ambient);
    // (A thunderstorm's sky: dimmer than the clear day's the ambient holds - the ground's light pass's too: its shade,
    // ffp FlatShade, takes the storm's sky with the lightmap only, and the pass adds the ambient to it - by night, under
    // the night's dark lightmap, nearly all its light: in a night storm the land stood lit under black characters,
    // user 2026-10-19.)
    // (And a low sun's shade the sky's cool light - sky.cpp; the ground's light pass tinted whole in ffp_main.)
    {
        // (Lit things - buildings, rocks, characters - a quarter of it under a full storm, as the ground keeps a quarter
        // of its light (FlatShade: half its sun's share, its sky's halved): at a half, with the storm's soft sunlight -
        // lighting.glsl - over it, they stood out light against the dark land - user, 2026-10-19.)
        // (By night the ground's share: no sun for the storm to spread over them, the ambient is nearly all their light,
        // and at a quarter of it the buildings and rocks stood near black on land lit by its lightmap and half its
        // ambient - user dumps rvk-frame-183522, -183647, a night storm in the desert.)
        const bool sky_ = VisualKind(m_drawVisualKind) == VisualKind::Sky;
        const bool groundLight = IsTerrain(fvf) && IsMultiplyPass();
        const float sky = groundLight || sky_ ? StormSkyLight()
                                              : 1.0f - 0.75f * StormOvercast() +
                                                    (StormSkyLight() - (1.0f - 0.75f * StormOvercast())) * m_cond.moonNight;
        for (int i = 0; i < 3; ++i) c.ambient[i] *= sky * (sky_ || groundLight ? 1.0f : m_dawnTint[1][i]);
    }
    ArgbToFloat(m_rs[d3d::RS_FOGCOLOR], c.fogColor);
    // (A thunderstorm's: its air dark yellow-green, dimmed. Not the ground's light pass's white fog: multiplying the
    // base pass, already in the storm's air, it took the air's colour twice - the far land darker than the trees and
    // things on it, which stood out pale - user, 2026-10-18.)
    if (!IsMultiplyPass()) StormAirColour(c.fogColor);
    // (Our own sky - RVK_ProcSky: the sky's scenery, the ships over the cities, hazed into its horizon - the ring's
    // mean - not the zone's fog colour, which stood out teal against it.)
    // The sky's own draws the same: the moons, a few hundred units out, fogged wholly into the zone's colour - a teal
    // moon by our sky (user, 2026-10-18) - fade into the sky round them instead (the horizon's for a low moon, the
    // upper sky's for a high one), as the game had them fade into its own by day; the stars, added, into black.
    const bool skyOwn = VisualKind(m_drawVisualKind) == VisualKind::Sky && !m_drawIsCloudPuff;
    if ((skyScenery || skyOwn) && ProcSkyReady() && m_skyRing[0][3] > 0.5f && m_skyUp[3] > 0.5f && !IsMultiplyPass()) {
        float mean[3] = {};
        for (const auto& r : m_skyRing)
            for (int i = 0; i < 3; ++i) mean[i] += r[i] * 0.125f;
        const float t = skyScenery ? 0.0f : std::clamp(m_moonPos[1] / 0.5f, 0.0f, 1.0f);
        const bool added = m_rs[d3d::RS_ALPHABLENDENABLE] && m_rs[d3d::RS_DESTBLEND] == d3d::BLEND_ONE;
        for (int i = 0; i < 3; ++i)
            c.fogColor[i] = added ? 0.0f : std::min(mean[i] + (m_skyUp[i] - mean[i]) * t, 1.0f);
    }
    c.fogParams[0] = AsFloat(m_rs[d3d::RS_FOGSTART]) * m_viewRange;   // (RVK_ViewRange: out with the view)
    c.fogParams[1] = AsFloat(m_rs[d3d::RS_FOGEND]) * m_viewRange;
    c.fogParams[2] = AsFloat(m_rs[d3d::RS_FOGDENSITY]);
    // (w 1: fade into our own sky's horizon towards the pixel - RVK_ProcSky, FL.skyRing - not the zone's fog colour:
    // the land, the buildings, the far trees (blended, writing no depth, so the tone mapping's correction by depth
    // never reached them: teal canopies on the horizon - user, 2026-10-18). Not the ground's light pass's white fog,
    // what is added, the sky's own draws and its scenery (their fog colour set above).)
    {
        const bool added = m_rs[d3d::RS_ALPHABLENDENABLE] && m_rs[d3d::RS_DESTBLEND] == d3d::BLEND_ONE;
        c.fogParams[3] = OwnSkyFog() && !IsMultiplyPass() && !added && !skyScenery &&
                                 VisualKind(m_drawVisualKind) != VisualKind::Sky
                             ? 1.0f
                             : 0.0f;
    }
    if (skyScenery)                              // (the far sky's haze: exponential below)
        c.fogParams[2] = 1.0f / kSkySceneryHaze;
    ArgbToFloat(m_rs[d3d::RS_TEXTUREFACTOR], c.tfactor);
    c.misc[0] = m_material.power;
    c.misc[1] = float(m_rs[d3d::RS_ALPHAREF] & 0xFF);
    c.misc[2] = m_effectGlow;                 // F_GLOW: how much the effect feeds the glow
    c.misc[3] = normalMap ? m_normalStrength : m_bump;   // F_NORMALMAP: slope scale; F_BUMP: height change per texel
                                                         // for a full brightness step
    // Camera position/forward in world space from the view matrix (columns 0-2 = camera axes for an
    // orthonormal D3D view matrix; row 3 = -eye expressed in those axes).
    const auto& v = m_view.m;
    for (int i = 0; i < 3; ++i) {
        c.eyePos[i] = -(v[3][0] * v[i][0] + v[3][1] * v[i][1] + v[3][2] * v[i][2]);
        c.eyeDir[i] = v[i][2];
    }
    c.eyePos[3] = 1.0f;
    c.eyeDir[3] = 0.0f;
    c.vtx[0] = fvf;
    // (The game's stars - the sky's point lists, added - faded in as the sky darkens: sky.cpp UpdateSunClock.)
    float colorScale = m_drawColorScale;
    if (primitive == d3d::PointList && VisualKind(m_drawVisualKind) == VisualKind::Sky && m_scenePhase && !m_external)
        colorScale *= std::max(m_starsShown, 1e-6f);   // (0's bits mean "as drawn")
    std::memcpy(&c.vtx[1], &colorScale, 4);
    if (colorScale == 1.0f) c.vtx[1] = 0;
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
    // (+ 2: this draw may get wet in the rain - the world's own surfaces drawn into the scene; not the interface, water,
    // the sky, characters, additive effects. ffp_main WetGround.)
    {
        const VisualKind kind = VisualKind(m_drawVisualKind);
        const bool additive = m_rs[d3d::RS_ALPHABLENDENABLE] && m_rs[d3d::RS_DESTBLEND] == d3d::BLEND_ONE;
        if (m_target == m_scene && !IsWater(fvf) && (fvf & d3d::FVF_POSITION_MASK) != d3d::FVF_XYZRHW && !skyScenery &&
            kind != VisualKind::Sky && kind != VisualKind::Character && kind != VisualKind::CharacterPart && !additive && !IsMultiplyPass())
            wet += 2.0f;
    }
    std::memcpy(&c.vtx[2], &reflectivity, 4);
    std::memcpy(&c.vtx[3], &wet, 4);
    uint32_t flags = 0;
    if (m_rs[d3d::RS_LIGHTING] && (fvf & d3d::FVF_POSITION_MASK) != d3d::FVF_XYZRHW) flags |= F_LIGHTING;
    if ((flags & F_LIGHTING) && m_pixelLighting) flags |= F_PERPIXEL;
    if (m_lightingDebug) flags |= F_DEBUGLIGHT;
    bool override = (flags & F_PERPIXEL) && m_lightOverride && !SurfaceSheen(fvf) && !m_drawingPortal;
    if (override) flags |= F_LIGHTOVERRIDE;
    m_constLightOverride = override;             // (the block's: kept while later draws reuse it)
    bool hdrTarget = m_target->m_format == Format::RGBA16F;
    if (hdrTarget) flags |= F_HDR;
    // Generated normals: per-pixel lit 3D drawn with a texture in stage 0 as its surface (not the ground's
    // lightmap pass, whose stage 0 is the lightmap), with plain coordinates.
    if ((flags & F_PERPIXEL) && normalMap)
        flags |= F_NORMALMAP | (m_drawBumpBase ? F_BUMPBASE : 0u);
    else if ((flags & F_PERPIXEL) && m_bump > 0.0f && m_textures[0] && !terrain && m_tss[0][d3d::TSS_COLOROP] != d3d::TOP_DISABLE &&
        !(m_tss[0][d3d::TSS_TEXTURETRANSFORMFLAGS] & 256u) && (m_tss[0][d3d::TSS_TEXCOORDINDEX] & 0xFFFF0000u) == 0)
        flags |= F_BUMP;
    else if ((flags & F_PERPIXEL) && m_drawBumpBase)
        flags |= F_BUMP | F_BUMPBASE;
    if (GlowDraw(fvf)) flags |= F_GLOW | (m_rs[d3d::RS_SRCBLEND] == d3d::BLEND_SRCALPHA ? F_GLOWALPHA : 0u);
    else if (GlowOccluder(fvf)) flags |= F_GLOWALPHA;   // (alone: covers the glow - ffp_main.glsl)
    if (override && (m_lightHeadroom > 1.0f || hdrTarget)) flags |= F_OVERBRIGHT;
    if (Overbright2x(fvf)) flags |= F_OVERBRIGHT2X;
    if (ShadowReceiver(fvf)) flags |= F_SHADOW;
    else if (ShadowInLightmap(fvf)) flags |= F_SHADOWTEX;
    else if (ShadowCompensated(fvf)) flags |= F_SHADOWCOMP;
    if (m_rs[d3d::RS_COLORVERTEX]) flags |= F_COLORVERTEX;
    if (m_rs[d3d::RS_SPECULARENABLE]) flags |= F_SPECULAR;
    if (m_rs[d3d::RS_NORMALIZENORMALS]) flags |= F_NORMALIZE;
    if (m_rs[d3d::RS_FOGENABLE]) flags |= F_FOG;
    // (RVK_ViewRange: by true distance, as the cull - the same fog all round, nothing fading in as the camera turns.)
    if (m_rs[d3d::RS_RANGEFOGENABLE] || m_viewRange != 1.0f) flags |= F_RANGEFOG;
    if (m_rs[d3d::RS_LOCALVIEWER]) flags |= F_LOCALVIEWER;
    if (m_textures[0]) flags |= F_TEX0;
    if (m_textures[1]) flags |= F_TEX1;
    if (m_rs[d3d::RS_ALPHATESTENABLE]) flags |= F_ALPHATEST;
    bool solid3d = (fvf & d3d::FVF_POSITION_MASK) != d3d::FVF_XYZRHW && !terrain && !m_drawIsLabel && m_textures[0] &&
                   m_rs[d3d::RS_ZENABLE] && m_rs[d3d::RS_ZWRITEENABLE] && m_rs[d3d::RS_ZFUNC] != d3d::CMP_ALWAYS;
    // Foliage: lit, cut out of a texture that actually has holes (alpha test, or blended with depth writes as most of
    // the game's statics are). Without the texture test the class matched ~96% of the statics - opaque-textured
    // buildings and props drawn blended - and the far LOD (cheap shadow, no relief) reached them.
    bool foliage = solid3d && textureHoles && (flags & F_LIGHTING) &&
                   (m_rs[d3d::RS_ALPHATESTENABLE] || m_rs[d3d::RS_ALPHABLENDENABLE]) &&
                   (m_tss[0][d3d::TSS_TEXCOORDINDEX] & 0xFFFF0000u) == 0;
    if (m_leafLight > 0.0f && foliage)
        flags |= F_FOLIAGE;
    // Distance level of detail (RVK_FoliageLod): foliage beyond it - many layers of it, a few pixels each - gets a
    // single-tap sun shadow and no relief; small plants (the swaying ones) are lit per vertex, the sun kept apart
    // (F_VERTEXSUN) so that its shadow darkens only the sunlight, as per pixel.
    if (foliage && foliageLod) {
        flags = (flags & ~(F_BUMP | F_BUMPBASE | F_NORMALMAP)) | F_SHADOWCHEAP;
        if (foliageLod == 2u && (flags & F_PERPIXEL))
            flags = (flags & ~F_PERPIXEL) | F_VERTEXSUN;
    }
    m_drawFoliage = foliage ? (foliageLod ? 2u : 1u) : 0u;   // counted per draw below (this block is per state)
    // Blended (not additive) with depth writes, as the game draws most statics: the see-through parts must not write
    // depth or motion - plants' quads would show in the ambient occlusion and smear in the motion blur. Fragments
    // nearly invisible anyway are dropped; a plant's (ffp.vert vCutout) below half, like its shadow.
    if (solid3d && !cutoutTexture && m_rs[d3d::RS_ALPHABLENDENABLE] && !m_rs[d3d::RS_ALPHATESTENABLE] &&
        m_rs[d3d::RS_SRCBLEND] == d3d::BLEND_SRCALPHA && m_rs[d3d::RS_DESTBLEND] == d3d::BLEND_INVSRCALPHA)
        flags |= F_CUTOUT;
    if (m_rs[d3d::RS_ALPHABLENDENABLE] && m_rs[d3d::RS_SRCBLEND] == d3d::BLEND_SRCALPHA &&
        m_rs[d3d::RS_DESTBLEND] == d3d::BLEND_INVSRCALPHA)
        flags |= F_BLENDED;                      // (the cut-out pre-pass: such a pixel hides what is behind at alpha 1)
    m_drawMayDiscard = (flags & (F_ALPHATEST | F_CUTOUT)) != 0u;   // else the no-discard pipeline keeps early-Z
    // Self-lit surfaces: opaque 3D surfaces drawn unlit (like windows and signs) or with an emissive material - not
    // effects, the sky, the ground or lighting passes. The night glow's (by night their bright texels glow), and an
    // emissive map on one only says where it glows (ffp_main: one glow per surface) - so marked with the night glow
    // off too.
    bool additive = m_rs[d3d::RS_ALPHABLENDENABLE] && m_rs[d3d::RS_DESTBLEND] == d3d::BLEND_ONE;
    float emissive = std::max(std::max(m_material.emissive.r, m_material.emissive.g), m_material.emissive.b);
    if (hdrTarget && solid3d && !additive && !IsMultiplyPass() &&
        (!(flags & F_LIGHTING) || emissive > 0.05f))
        flags |= F_EMISSIVE;
    // A character's body and parts: the lights characters carry don't shadow them (lighting.glsl).
    if (m_drawIsCharacter) flags |= F_CHARACTER;
    if (m_drawTerrainLight) flags |= F_NOALBEDO;   // its albedo write is masked (the base pass wrote it)
    c.flags[0] = flags;
    // The lean shader (RVK_LiteShader, ffp_lite.frag): a pre-transformed 2D draw on an 8-bit target - the interface -
    // unlit, unfogged, unshadowed, with no material maps. All the full shader does for such a draw is in its lean
    // main(); anything that would take the full one elsewhere keeps it.
    m_drawLite = m_liteShader && !hdrTarget && (fvf & d3d::FVF_POSITION_MASK) == d3d::FVF_XYZRHW &&
                 (flags & (F_LIGHTING | F_PERPIXEL | F_FOG | F_SHADOW | F_SHADOWCOMP | F_SHADOWTEX | F_GLOW |
                           F_GLOWALPHA | F_OVERBRIGHT2X | F_FOLIAGE | F_NOALBEDO | F_EMISSIVE | F_BUMP | F_BUMPBASE |
                           F_NORMALMAP | F_VERTEXSUN | F_CHARACTER)) == 0u &&
                 !m_drawOrm && !m_drawEmissive && !m_drawIsCloudPuff && !m_drawLeaves && !m_drawTerrainBase &&
                 !m_drawTerrainLight;   // (two-pass foliage, its dither and the ground are 3D: decided below, never 2D)
    c.flags[1] = m_rs[d3d::RS_FOGVERTEXMODE];
    c.flags[2] = m_rs[d3d::RS_FOGTABLEMODE];
    // The sky's scenery (mesh ids: the ships over the cities, kilometres out): hazed as the far sky - exponentially, in
    // the fog's colour, never wholly (the ground's linear fog made them flat fog-coloured shapes; none at all left
    // them hard and dark against a hazy sky): ~37% of their colour left at 2 km, 14% at 4 km, a trace at 9 km.
    if (skyScenery && m_rs[d3d::RS_FOGENABLE]) {
        c.flags[1] = 1;                          // D3DFOG_EXP
        c.flags[2] = 0;
    }
    // (Bit 8: in the interface layer, a draw replacing what is there - unblended, or blended ONE, ZERO - covers it
    // fully, whatever its alpha: on the frame it would. The PF map's light-map tiles have alpha 0; as coverage they
    // left the map's black parts see-through.)
    const bool layerReplace = InLayer() &&
                              (!m_rs[d3d::RS_ALPHABLENDENABLE] || (m_rs[d3d::RS_SRCBLEND] == d3d::BLEND_ONE &&
                                                                    m_rs[d3d::RS_DESTBLEND] == d3d::BLEND_ZERO));
    // (Bit 9: a PF map tile of an AI mission (RVK_AlienMap). That map is an untextured fill in transparent black, then
    // opaque black tiles (32 x 32, no alpha) over all that isn't a room: the rooms are the fill showing between them.
    // On the frame, black on black; in the interface layer the rooms showed only as long as the fill came out
    // see-through (player reports). The tiles are drawn green: the rooms black on it. Other maps' empty space is
    // tiles with alpha 0, not opaque black ones.)
    const bool alienMap = m_alienMap && (fvf & d3d::FVF_POSITION_MASK) == d3d::FVF_XYZRHW && m_textures[0] &&
                          m_textures[0]->m_blackRgb && m_textures[0]->m_opaque && m_textures[0]->Width() == 32 &&
                          m_textures[0]->Height() == 32 &&
                          (!m_rs[d3d::RS_ALPHABLENDENABLE] ||
                           (m_rs[d3d::RS_SRCBLEND] == d3d::BLEND_ONE && m_rs[d3d::RS_DESTBLEND] == d3d::BLEND_ZERO));
    // (1024: the game's blob shadow - ffp_main: it darkens the colour only, the surface's other targets kept.)
    c.flags[3] = m_rs[d3d::RS_ALPHAFUNC] | (layerReplace ? 256u : 0u) | (alienMap ? 512u : 0u) |
                 (m_drawIsBlobShadow ? 1024u : 0u);
    c.matSources[0] = m_rs[d3d::RS_DIFFUSEMATERIALSOURCE];
    c.matSources[1] = m_rs[d3d::RS_AMBIENTMATERIALSOURCE];
    c.matSources[2] = m_rs[d3d::RS_SPECULARMATERIALSOURCE];
    c.matSources[3] = m_rs[d3d::RS_EMISSIVEMATERIALSOURCE];
    // A stage without a texture whose colour operation reads the texture (an argument it uses is D3DTA_TEXTURE) is
    // disabled, and so the stages after it - as native Direct3D does (DXVK's fixed function does the same): the
    // PF map's white wash is drawn untextured with MODULATE(TEXTURE, DIFFUSE) and must come out its vertex colour,
    // not black. (Alpha arguments and implicit sampling don't count.)
    bool stagesOff = false;
    for (int s = 0; s < 2; ++s) {
        const auto& t = m_tss[s];
        uint32_t colorOp = t[d3d::TSS_COLOROP];
        if (!stagesOff && colorOp != d3d::TOP_DISABLE && !m_textures[s]) {
            const bool arg1 = colorOp != d3d::TOP_SELECTARG2, arg2 = colorOp != d3d::TOP_SELECTARG1;
            if ((arg1 && (t[d3d::TSS_COLORARG1] & 0xFu) == d3d::TA_TEXTURE) ||
                (arg2 && (t[d3d::TSS_COLORARG2] & 0xFu) == d3d::TA_TEXTURE))
                stagesOff = true;
        }
        if (stagesOff)
            colorOp = d3d::TOP_DISABLE;
        c.stageA[s][0] = colorOp;
        c.stageA[s][1] = t[d3d::TSS_COLORARG1];
        c.stageA[s][2] = t[d3d::TSS_COLORARG2];
        c.stageA[s][3] = t[d3d::TSS_ALPHAOP];
        c.stageB[s][0] = t[d3d::TSS_ALPHAARG1];
        c.stageB[s][1] = t[d3d::TSS_ALPHAARG2];
        c.stageB[s][2] = t[d3d::TSS_TEXCOORDINDEX];
        c.stageB[s][3] = t[d3d::TSS_TEXTURETRANSFORMFLAGS];
    }
    uint32_t lightCount = 0, localLights = 0;
    // The block's lights (FillGpuLight each): the last list made again while the lights (their generation), the frame
    // (the moon, the lights' scales) and what decides the list - lit, the override, the ground, the sky - are the same.
    const uint32_t lightKind = ((flags & F_LIGHTING) ? 1u : 0u) | (override ? 2u : 0u) | (terrain ? 4u : 0u) |
                               (VisualKind(m_drawVisualKind) == VisualKind::Sky ? 8u : 0u);
    if (m_lightListGen == m_lightsGen && m_lightListFrame == m_frameNumber && m_lightListKind == lightKind) {
        lightCount = m_lightListCount;
        localLights = m_lightListLocal;
        std::memcpy(c.lights, m_lightList.data(), size_t(lightCount) * sizeof(GpuLight));
    } else {
    if (flags & F_LIGHTING)
        for (const LightSlot& slot : m_lights) {
            if (!slot.enabled || lightCount == kMaxLights)
                continue;
            if (override && slot.light.type != d3d::LIGHT_DIRECTIONAL)
                continue;                        // replaced by the frame lights
            // The ground's lightmap has the sun baked in, and the game means to keep directional lights off it
            // (CullLights mask 0xfffffffe) - but skips that mask when there are 8 lights or fewer. The sun then
            // saturates the ground's lighting pass and local lights (the player's) vanish on it. Per vertex too:
            // turning the view changed how many lights the game gathered, and the ground lit up and darkened with
            // the sun coming and going (user, 2026-10-18).
            if (terrain && slot.light.type == d3d::LIGHT_DIRECTIONAL)
                continue;
            // By night the game's light from under the ground fades out as the moon's comes in (night.cpp).
            // (With the light rig: gone, and the sun faded over its last degrees - light_rig.cpp RigGameLight.)
            d3d::Light scaled;
            const bool sky = VisualKind(m_drawVisualKind) == VisualKind::Sky;
            const bool rigLit = override && !sky && RigGameLight(slot.light, &scaled);
            const bool night = rigLit || (override && slot.light.type == d3d::LIGHT_DIRECTIONAL && !sky &&
                                          !m_rig.on && MoonScaledGameLight(slot.light, &scaled));
            FillGpuLight(night ? scaled : slot.light, slot.cosHalfTheta, slot.cosHalfPhi, c.lights[lightCount++],
                         LightScale(slot.light));
            // The sun's light low in the sky warm (sky.cpp RVK_DawnLight): the game's directional light along the
            // sun's way - not other directional lights (effects' coloured ones), not the sky's own.
            if ((!night || rigLit) && slot.light.type == d3d::LIGHT_DIRECTIONAL && m_dawnTint[0][3] > 0.0f &&
                VisualKind(m_drawVisualKind) != VisualKind::Sky) {
                const d3d::Vector& d = slot.light.direction;
                const float l = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z) *
                                std::sqrt(m_sunDir[0] * m_sunDir[0] + m_sunDir[1] * m_sunDir[1] + m_sunDir[2] * m_sunDir[2]);
                if (l > 1e-6f && (d.x * m_sunDir[0] + d.y * m_sunDir[1] + d.z * m_sunDir[2]) / l > 0.995f) {
                    GpuLight& g = c.lights[lightCount - 1];
                    for (int i = 0; i < 3; ++i) { g.diffuse[i] *= m_dawnTint[0][i]; g.specular[i] *= m_dawnTint[0][i]; }
                }
            }
            if (slot.light.type != d3d::LIGHT_DIRECTIONAL) ++localLights;
        }
    // ... and the moon's light (the ground too, which the game keeps directional light off).
    if ((flags & F_LIGHTING) && override && MoonOn() && VisualKind(m_drawVisualKind) != VisualKind::Sky &&
        lightCount < kMaxLights)
        FillGpuLight(MoonAsLight(), 0.0f, 0.0f, c.lights[lightCount++], 1.0f);
    m_lightListGen = m_lightsGen;
    m_lightListFrame = m_frameNumber;
    m_lightListKind = lightKind;
    m_lightListCount = lightCount;
    m_lightListLocal = localLights;
    m_lightList.assign(reinterpret_cast<const uint8_t*>(c.lights),
                       reinterpret_cast<const uint8_t*>(c.lights) + size_t(lightCount) * sizeof(GpuLight));
    }
    c.lightInfo[0] = lightCount;
    c.lightInfo[1] = localLights;
    c.lightInfo[2] = override ? carrier : 0;   // frame light (index + 1) this draw carries: it doesn't light it
    c.lightInfo[3] = 0;
    // Shared with the next draws that keep this state. A state change that ends up writing the same block (a
    // state set and set back, a light set again) keeps the last one: fewer blocks, and the draw can still join the
    // pending indirect batch (its key includes the block's index).
    size_t constBytes = offsetof(DrawConstants, lights) + size_t(lightCount) * sizeof(GpuLight);
    if (sameArena && m_lastConstants.size() == constBytes && SameBytes(m_lastConstants.data(), &c, constBytes)) {
        ++m_constantsReused;
    } else {
        constIndex = AppendConstant(c, constBytes);
        m_lastConstants.assign(reinterpret_cast<const uint8_t*>(&c), reinterpret_cast<const uint8_t*>(&c) + constBytes);
    }
    m_constIndex = constIndex;
    }
    if (!m_drawMayDiscard) ++m_noCutDraws;       // per draw (the block above runs only when the constants change)
    // Two-pass foliage (RVK_FolEdges, see FlushEdges): a 3D blended cut-out that writes depth - not characters (their
    // own passes), labels, particles or water.
    {
        uint32_t zf = m_rs[d3d::RS_ZFUNC];
        m_drawSplit = m_foliageEdges && m_rendering && m_drawMayDiscard && !m_external && !m_drawIsLabel && !m_drawGpu &&
                      !m_drawIsCharacter && tessLevel <= 0.0f && (fvf & d3d::FVF_POSITION_MASK) != d3d::FVF_XYZRHW &&
                      TopologyClassOf(primitive) == 2 && m_rs[d3d::RS_ZENABLE] && m_rs[d3d::RS_ZWRITEENABLE] &&
                      (zf == d3d::CMP_LESS || zf == d3d::CMP_LESSEQUAL) && m_rs[d3d::RS_ALPHABLENDENABLE] &&
                      m_rs[d3d::RS_SRCBLEND] == d3d::BLEND_SRCALPHA && m_rs[d3d::RS_DESTBLEND] == d3d::BLEND_INVSRCALPHA &&
                      !WaterWritesDepth(fvf);
        // (Dithered: a cut-out's soft rims, not a surface translucent all over - Texture::m_alphaSoftShare.)
        m_drawDither = m_drawSplit && m_foliageDither > 0.0f && TaaActive() && DrawFartherThan(m_foliageDither);
        if (m_drawDither && m_textures[0] && m_textures[0]->m_alphaSoftShare >= kDitherSoftShare &&
            m_textures[0]->m_alphaHardEdges < kDitherHardEdges) {
            m_drawDither = false;
            static uint32_t logged;
            if (logged < 20 && m_ditherRefusedLogged.insert(m_textures[0]->m_rdbId).second) {
                ++logged;
                Log("foliage dither: texture %u (%ux%u) translucent all over (soft %.2f, edges %.3f) - blended, not "
                    "dithered", m_textures[0]->m_rdbId, m_textures[0]->Width(), m_textures[0]->Height(),
                    double(m_textures[0]->m_alphaSoftShare), double(m_textures[0]->m_alphaHardEdges));
            }
        }
        dt.motion[3] = m_drawDither ? 3.0f : m_drawSplit ? 1.0f : 0.0f;   // its core now (ffp_main.glsl); the edges'
                                                                          // record below
    }
    if (m_drawFoliage) {
        ++m_foliageDraws;
        if (m_drawFoliage == 2u) ++m_foliageLodDraws;
        // A survey of the game's own foliage, for deciding what to replace: whether its meshes are world-fixed (a
        // swaying plant) or change every frame (a camera-facing billboard or a skinned thing), and their size.
        if (m_drawMeshStatic)
            ++m_foliageStatic;
        else
            ++m_foliageDynamic;
        m_foliageVerts += vertexCount;
        if (m_drawObject || m_drawMesh) {        // a draw without a known mesh has no box to measure
            float c[3], e[3];
            DrawWorldBox(c, e);
            const float h = e[1] * 2.0f;
            if (m_foliageMinH == 0.0f || h < m_foliageMinH)
                m_foliageMinH = h;
            if (h > m_foliageMaxH)
                m_foliageMaxH = h;
        }
    }

    ProfileDrawSection("draw: constants", since);
    // Geometry: vertices aligned to their stride and indices to 2 bytes, so the draw can address them inside
    // the ring buffer bound once (vertexOffset / firstIndex) instead of rebinding buffers per draw.
    VkDeviceSize vbOffset = 0, ibOffset = 0;
    if (m_drawGpu) {
        // Skinned on the GPU: the vertices in the frame's skin arena, the indices in the mesh's buffer.
        vbOffset = m_drawGpu->vertexOffset;
        ibOffset = m_drawGpu->mesh->indexOffset;
        dt.motion[2] = float(uint32_t(vbOffset) / layout.stride);
        // A character's solid piece into the ray tracing (raytrace.cpp RtNoteSkinned; its shadow caster then only in
        // the map's untraced layers if the rays lack it). Cut-out pieces (hair, fringes) stay the map's.
        if (m_rt && m_drawSkin && primitive == d3d::TriangleList && indices && m_target == m_scene && !m_external &&
            m_rs[d3d::RS_ZENABLE] && m_rs[d3d::RS_ZWRITEENABLE] && !m_rs[d3d::RS_ALPHABLENDENABLE] &&
            (!m_rs[d3d::RS_ALPHATESTENABLE] || !m_textures[0] || m_textures[0]->m_opaque))
            m_drawRtPlace = RtNoteSkinned(m_drawSkin, m_frames[m_frameIndex].skinArena, vbOffset, vertexCount,
                                          m_drawGpu->mesh->buffer, ibOffset, indexCount);
        RecordShadowCaster(primitive, fvf, layout.stride, nullptr, vertexCount, vbOffset, indices, indices ? indexCount : 0,
                           ibOffset, m_frames[m_frameIndex].skinArena, m_drawGpu->mesh->buffer);
    } else if (m_drawStaticBuffer) {
        // Static geometry on the GPU: the vertices stay where they are; the indices are retained there too
        // (DrawSharedIndexed) or come through the ring.
        vbOffset = m_drawStaticOffset;
        dt.motion[2] = float(uint32_t(vbOffset) / layout.stride);
        if (staticIb) {
            ibOffset = m_drawStaticIndexOffset;
        } else if (indices) {
            ibOffset = Allocate(VkDeviceSize(indexCount) * 2, 2, &cpu);
            std::memcpy(cpu, indices, size_t(indexCount) * 2);
        }
        RecordShadowCaster(primitive, fvf, layout.stride, vertices, vertexCount, vbOffset, indices,
                           indices ? indexCount : 0, ibOffset, m_drawStaticBuffer, staticIb ? staticIb : f.ring);
    } else if (!m_external) {
        VkDeviceSize vbBytes = VkDeviceSize(layout.stride) * vertexCount;
        vbOffset = Allocate(vbBytes, layout.stride, &cpu);
        std::memcpy(cpu, vertices, vbBytes);
        dt.motion[2] = float(uint32_t(vbOffset) / layout.stride);   // the base vertex: gl_VertexIndex - it = vertex
        if (indices) {
            ibOffset = Allocate(VkDeviceSize(indexCount) * 2, 2, &cpu);
            std::memcpy(cpu, indices, size_t(indexCount) * 2);
        }
        RecordShadowCaster(primitive, fvf, layout.stride, vertices, vertexCount, vbOffset, indices,
                           indices ? indexCount : 0, ibOffset);
    }
    // Phong tessellation: the averaged normals (binding 10) and the draw's level and shape.
    VkDeviceSize smoothOffset = 0, smoothBytes = 0;
    VkBuffer smoothBuffer = f.ring;
    m_drawTess = false;
    if (tessLevel > 0.0f && m_drawGpu) {
        if (SkinSmoothOnGpu(*m_drawSkin, *m_drawGpu)) {
            smoothBuffer = m_frames[m_frameIndex].skinArena;
            smoothOffset = m_drawGpu->smoothOffset;
            smoothBytes = VkDeviceSize(vertexCount) * 12;
            dt.tess[0] = tessLevel;
            dt.tess[1] = m_tessShape;
            dt.tess[2] = float(uint32_t(vbOffset) / layout.stride);
            m_drawTess = true;
        }
    } else if (tessLevel > 0.0f &&
        (m_drawSkin && layout.stride == sizeof(skin::Vertex)
             ? SmoothNormalsSkinned(vertices, uint32_t(static_cast<const skin::Vertex*>(vertices) - m_drawSkinBase), vertexCount)
             : SmoothNormals(vertices, vertexCount, layout))) {
        void* smoothCpu;
        smoothBytes = m_smoothNormals.size() * 4;
        smoothOffset = Allocate(smoothBytes, m_props.limits.minStorageBufferOffsetAlignment, &smoothCpu);
        std::memcpy(smoothCpu, m_smoothNormals.data(), smoothBytes);
        dt.tess[0] = tessLevel;
        dt.tess[1] = m_tessShape;
        dt.tess[2] = float(uint32_t(vbOffset) / layout.stride);
        m_drawTess = true;
    } else if (m_native.drawing && !m_native.clutter && m_native.tessPatch && m_tessSupported && primitive == d3d::TriangleList &&
               (m_terrainSettings.round > 0.0f || (m_terrainSettings.depth > 0.0f && TerrainRockOn()))) {
        // The native ground (ffp.tesc): each edge split by its length and distance, the vertex normals its shape, the
        // rock pushed out of the cliffs - the same in both passes, so the lighting pass's equal depth test still meets
        // the base pass's.
        dt.tess[0] = 16.0f;
        dt.tess[1] = std::clamp(m_terrainSettings.round, 0.0f, 1.0f);
        dt.tess[2] = kNativeTessDistance;
        dt.tess[3] = TerrainRockOn() ? std::clamp(m_terrainSettings.depth, 0.0f, 1.0f) : 0.0f;
        m_drawTess = true;
    }
    ProfileDrawSection("draw: geometry + casters", since);
    m_drawOverbright2x = Overbright2x(fvf);
    if (GlowDraw(fvf)) ++m_glowDraws;
    // M3: can this draw join the pending batched group? A group shares the pipeline (tessellation, target), the
    // topology, the constants (render state, material, lights - any change makes a new constIndex), the vertex and
    // index buffers, and the per-draw descriptor blocks. Geometry offsets and the record index are per command, so
    // draws of different meshes with the same state still share one call.
    // Depth pre-pass (prepass.cpp): an opaque scene draw also draws its depth into the pre-pass, which runs before the
    // scene's draws; its own depth test then passes on equal.
    uint32_t prepassWhy = PrepassCheck(primitive, fvf, swaying);
    m_drawPrepassed = prepassWhy == kPreIn;
    m_drawPrepassCutout = m_drawPrepassed && m_drawMayDiscard;   // its depth only where fully opaque
    m_drawBackdrop = BackdropCull(fvf);
    if (m_scenePhase && m_target == m_scene && !m_external && (fvf & d3d::FVF_POSITION_MASK) != d3d::FVF_XYZRHW &&
        !m_rs[d3d::RS_ZWRITEENABLE] && (!m_rs[d3d::RS_ZENABLE] || m_rs[d3d::RS_ZFUNC] == d3d::CMP_ALWAYS)) {
        // A draw that ignores depth (log): culled as a backdrop, or why not.
        ++m_backdropDraws[m_drawBackdrop ? 0 : !m_prepassArmed ? (m_prepassEndedThisFrame ? 2 : 1)
                                         : m_prepassDepthTouched ? 3 : 4];
    }
    if (m_prepassArmed && m_rs[d3d::RS_ZENABLE] && (m_rs[d3d::RS_ZWRITEENABLE] || WaterWritesDepth(fvf)))
        m_prepassDepthTouched = true;            // this draw writes depth in the main pass: no backdrops after it
    if (m_scenePhase && m_target == m_scene && !m_external && (fvf & d3d::FVF_POSITION_MASK) != d3d::FVF_XYZRHW)
        ++m_prepassWhy[prepassWhy];              // the scene's 3D draws, by why they are (not) pre-passed (log)
    if (m_drawSceneClass) {                      // the shading count: statics split by the pre-pass
        int shade = m_drawSceneClass;
        if (shade == 1 && !m_drawPrepassed)
            shade = prepassWhy == kPreNoZTest || prepassWhy == kPreNoZWrite ? 11 : 12;
        else if (shade == 4 && prepassWhy == kPreAlphaTest)
            shade = 13;                          // foliage a cut-out pre-pass could take (vs blended cut-outs)
        ShadeClass(shade);
    }
    uint64_t groupKey = 0;
    // The shader's build with the ground's code for the ground's draws (ApplyDynamicState). RANDYVK_FULL_SHADER=1: for
    // every draw, as before the builds were split (comparisons).
    static const bool fullShader = [] { const char* v = std::getenv("RANDYVK_FULL_SHADER"); return v && *v == '1'; }();
    m_drawGround = fullShader || dt.terrainAt[3] > 0.5f;
    // ... and the build without the frame lights' loop for the draws none reaches (lighting.glsl): the override off
    // (the loop's test), or none in the draw's mask.
    m_drawNoFrameLights = !m_drawGround && (!m_constLightOverride || (dt.lightMask[0] | dt.lightMask[1]) == 0u);
    if (m_groupIndirect && indices && !m_external && !(m_particlePending && fvf == kParticleFvf) && !m_drawLeafCount) {
        bool hdr = m_target->m_format == Format::RGBA16F;
        uint32_t pipelineClass = (m_drawTess ? 6u + (hdr ? 1u : 0u)
                                             : TopologyClass(primitive) + (hdr ? 3u : 0u)) +
                                 24u * (m_drawGround ? 1u : m_drawNoFrameLights ? 2u : 0u);
        VkBuffer vb = m_drawGpu ? m_frames[m_frameIndex].skinArena
                                : (m_drawStaticBuffer ? m_drawStaticBuffer : f.ring);
        VkBuffer ib = m_drawGpu ? m_drawGpu->mesh->buffer : (staticIb ? staticIb : f.ring);
        uint64_t k = 0x9E3779B97F4A7C15ull;
        auto gmix = [&k](uint64_t x) { k = (k ^ x) * 0xFF51AFD7ED558CCDull; k ^= k >> 32; };
        gmix(constIndex);
        gmix(pipelineClass);
        gmix(primitive);
        gmix(fvf);
        gmix(layout.stride);
        gmix(uint64_t(vb));
        gmix(uint64_t(ib));
        gmix(uint64_t(prevPositionsBuffer));
        gmix(prevPositionsOffset);
        gmix(prevPositionsBytes);
        gmix(uint64_t(smoothBuffer));
        gmix(smoothOffset);
        gmix(smoothBytes);
        gmix(frameLightsOffset);
        gmix(m_drawPrepassed ? 1u : 0u);          // its depth compare (ApplyDynamicState)
        gmix(m_drawBackdrop ? 1u : 0u);           // its depth bounds
        // The rest of what ApplyDynamicState sets: render state that isn't in the constant block (a state change
        // that writes the same block keeps its index), and the draw's flags it reads.
        gmix(uint64_t(m_rs[d3d::RS_CULLMODE]) | uint64_t(m_rs[d3d::RS_ZENABLE] != 0) << 8 |
             uint64_t(m_rs[d3d::RS_ZWRITEENABLE] != 0) << 9 | uint64_t(m_rs[d3d::RS_ALPHABLENDENABLE] != 0) << 10 |
             uint64_t(m_drawMayDiscard) << 11 | uint64_t(m_drawIsLabel) << 12 | uint64_t(m_drawOverbright2x) << 13 |
             uint64_t(m_drawTerrainBase) << 14 | uint64_t(m_drawTerrainLight) << 15 |
             uint64_t(m_rs[d3d::RS_ZFUNC]) << 16 |   // (the blend factors only while blending: they aren't set else)
             (m_rs[d3d::RS_ALPHABLENDENABLE] ? uint64_t(m_rs[d3d::RS_SRCBLEND]) << 32 |
                                                   uint64_t(m_rs[d3d::RS_DESTBLEND]) << 48 : 0));
        groupKey = k ? k : 1;
        if (m_dumpFile) {                        // (frame dump: which parts ended the batch)
            const uint64_t parts[kSplitParts] = {
                constIndex, pipelineClass, primitive, fvf, uint64_t(vb), uint64_t(ib), uint64_t(prevPositionsBuffer) ^ prevPositionsOffset ^ prevPositionsBytes,
                uint64_t(smoothBuffer) ^ smoothOffset ^ smoothBytes, frameLightsOffset, m_drawPrepassed ? 1u : 0u,
                m_drawBackdrop ? 1u : 0u, m_rs[d3d::RS_CULLMODE], uint64_t(m_rs[d3d::RS_ZWRITEENABLE] != 0) | uint64_t(m_rs[d3d::RS_ZFUNC]) << 8,
                m_rs[d3d::RS_ALPHABLENDENABLE], uint64_t(m_drawMayDiscard) | uint64_t(m_drawIsLabel) << 1 | uint64_t(m_drawOverbright2x) << 2,
                uint64_t(m_drawTerrainBase) | uint64_t(m_drawTerrainLight) << 1};
            if (m_group.active && groupKey != m_group.key)
                for (int i = 0; i < kSplitParts; ++i) m_splitWhy[i] += parts[i] != m_lastGroupParts[i];
            else if (!m_group.active)
                ++m_splitWhy[kSplitParts];       // (the batch was ended elsewhere: a flush, edges, a pass)
            std::memcpy(m_lastGroupParts, parts, sizeof(parts));
        }
    } else if (m_dumpFile) {
        ++m_splitWhy[kSplitParts + 1];           // (not batchable)
    }
    if (m_group.active && (groupKey == 0 || groupKey != m_group.key))
        FlushGroup();                                // the pending group's state is about to change
    // A draw that extends the pending group shares its pipeline, dynamic state, descriptors and buffers, so only
    // its record and indirect command are added - the expensive per-draw setup is skipped.
    bool extending = m_group.active && groupKey != 0 && groupKey == m_group.key;

    // Bindless textures (set 1, M1): the draw's four textures and four samplers by index, so nothing per-draw is
    // bound as an image descriptor. The indices live in the draw's record (constants.glsl D.texIdx/sampIdx).
    // A texture's albedo map (SetMaterialMaps) is drawn in its place.
    auto drawn = [this](Texture* t) { return !t ? m_blackTexture : t->m_albedoMap && m_pbr.albedoMaps ? t->m_albedoMap : t; };
    Texture* texStage0 = drawn(m_textures[0]);
    Texture* texStage1 = drawn(m_textures[1]);
    Texture* bumpBase = drawn(m_drawBumpBase);
    Texture* normalTex = normalMap ? normalMap : m_flatNormal;
    dt.texIdx[0] = BindlessImage(texStage0);
    dt.texIdx[1] = BindlessImage(texStage1);
    dt.texIdx[2] = BindlessImage(bumpBase);
    dt.texIdx[3] = BindlessImage(normalTex);
    dt.sampIdx[0] = StageSamplerSlot(0);
    dt.sampIdx[1] = StageSamplerSlot(1);
    dt.sampIdx[2] = FixedSamplerSlot(0, m_bumpSampler);
    dt.sampIdx[3] = FixedSamplerSlot(1, m_normalSampler);
    // The PBR material (the shader uses it only where the draw is lit per pixel and isn't far foliage).
    if (m_drawOrm) {
        dt.mat[0] = BindlessImage(m_drawOrm);
        dt.mat[1] = FixedSamplerSlot(1, m_normalSampler);
        dt.mat[2] = kMatPbr | (m_drawOrmBase ? kMatBase : 0u);
    } else {
        dt.mat[0] = dt.texIdx[3];               // a valid slot even unused
        dt.mat[1] = dt.sampIdx[3];
    }
    if (m_textures[0] && texStage0 == m_textures[0]->m_albedoMap) dt.mat[2] |= kMatAlbedo;   // (debug view 7)
    if (m_pickHighlight && m_textures[0] == m_pickHighlight)
        dt.mat[2] |= kMatPicked;                 // (texture picking: flashes)
    // A height in its normal map's alpha (_h.png): drawn with parallax, where the normal map is the surface's own.
    if (normalMap && normalMap->m_alphaHeight && m_textures[0] && normalMap == m_textures[0]->m_normalMap &&
        m_terrainSettings.parallax > 0.0f)
        dt.mat[2] |= kMatHeight;
    if (VisualKind(m_drawVisualKind) == VisualKind::Sky || (m_drawMesh && m_drawMesh->skyScenery))
        dt.mat[2] |= kMatSky;                    // (the sky and its scenery: kept inside the far plane, ffp.vert)
    if (m_drawIsCloudPuff && CloudSheetsOn())
        dt.mat[2] |= kMatCloudSheet;             // (the game's cloud sheets, lit as the cloud layer: ffp_main.glsl)
    if (m_drawEmissive) {                        // (sampled with the ORM map's sampler, set either way)
        dt.mat[3] = BindlessImage(m_drawEmissive);
        dt.mat[2] |= kMatEmissive;
    } else {
        dt.mat[3] = dt.mat[0];
    }
    // GPU-driven M2: this draw's record, and the frame's two arrays (bindings 0 = constants, 12 = records) pushed
    // once per frame's command buffer. The record index travels in firstInstance (gl_InstanceIndex).
    uint32_t recordIndex = AppendRecord(constIndex, dt);
    // A tree's own canopy (leaves.cpp): its sprigs drawn with the type's atlas - their own record, the canopy's but for
    // the textures (its material maps are laid on the game's cards, not on the atlas).
    uint32_t leafRecord = recordIndex;
    if (m_drawLeafCount && m_drawLeaves->art) {
        DrawTransform ld = dt;
        ld.texIdx[0] = BindlessImage(m_drawLeaves->art);
        Texture* artNormal = m_textures[0] ? m_textures[0]->m_leafArtNormal : nullptr;   // (the canopy texture's)
        ld.texIdx[3] = BindlessImage(artNormal ? artNormal : m_flatNormal);
        ld.mat[2] &= ~(kMatPbr | kMatAlbedo | kMatEmissive);
        leafRecord = AppendRecord(constIndex, ld);
    }
    if (m_drawSplit && !m_drawDither) {          // the edges, queued: the same draw, its own record (motion.w = 2)
        DrawTransform edge = dt;
        edge.motion[3] = 2.0f;
        DeferredEdge e{};
        e.primitive = primitive;
        e.fvf = fvf;
        e.stride = layout.stride;
        e.vertexCount = vertexCount;
        e.indexCount = indices ? indexCount : 0;
        e.record = AppendRecord(constIndex, edge);
        e.vbOffset = vbOffset;
        e.ibOffset = ibOffset;
        e.vb = m_drawStaticBuffer ? m_drawStaticBuffer : f.ring;
        e.ib = indices ? (staticIb ? staticIb : f.ring) : VK_NULL_HANDLE;
        e.frameLights = frameLightsOffset;
        e.prevBuffer = prevPositionsBuffer;
        e.prevOffset = prevPositionsOffset;
        e.prevBytes = prevPositionsBytes;
        e.smoothBuffer = smoothBuffer;
        e.smoothOffset = smoothOffset;
        e.smoothBytes = smoothBytes;
        e.viewport = m_viewport;
        e.cull = m_rs[d3d::RS_CULLMODE];
        e.ringGeneration = m_ringGeneration;
        m_edges.push_back(e);
    }
    if (m_drawPrepassed)
        PrepassDraw(primitive, fvf, layout.stride, vertexCount, indices ? indexCount : 0, vbOffset, ibOffset,
                    m_drawStaticBuffer ? m_drawStaticBuffer : f.ring, staticIb ? staticIb : f.ring, frameLightsOffset,
                    prevPositionsBuffer,
                    prevPositionsOffset, prevPositionsBytes, smoothBuffer, smoothOffset, smoothBytes, recordIndex,
                    m_drawPrepassCutout);
    if (!extending) {
    ApplyDynamicState(primitive, fvf, layout.stride);
    // The draw's buffers: the particles' own, a static snapshot's arena chunk (its indices retained there or in the
    // ring), a skinned piece's arena and mesh, or the ring.
    if (m_external)
        BindGeometry(cmd, m_external->vertices, m_external->indices);
    else if (m_drawStaticBuffer)
        BindGeometry(cmd, m_drawStaticBuffer, indices ? (staticIb ? staticIb : f.ring) : VK_NULL_HANDLE);
    else if (m_drawGpu)
        BindGeometry(cmd, m_frames[m_frameIndex].skinArena, m_drawGpu->mesh->buffer);
    else
        BindGeometry(cmd, f.ring, f.ring);
    BindSceneArena(cmd);
    PushDrawSet(cmd, frameLightsOffset, prevPositionsBuffer, prevPositionsOffset, prevPositionsBytes, smoothBuffer,
                smoothOffset, smoothBytes);
    }

    // A canopy's leaves (leaves.cpp), before the canopy itself: its state is set up now; DrawLeaves puts it back.
    if (m_drawLeafCount)
        DrawLeaves(cmd, leafRecord, primitive, fvf, layout.stride, frameLightsOffset, prevPositionsBuffer,
                   prevPositionsOffset, prevPositionsBytes, smoothBuffer, smoothOffset, smoothBytes);

    // Would an instanced batch cover this draw together with the one before it? Only static snapshots share their
    // vertex data across draws; ring copies are unique per draw, so those never merge.
    // (A statistic of the frame dump's alone: worked out only while one is written.)
    uint64_t batchKey = 0;
    if (m_dumpFile && m_drawStaticBuffer && !m_external && !m_drawGpu) {
        uint64_t k = 0x9E3779B97F4A7C15ull;
        auto mix = [&k](uint64_t x) { k = (k ^ x) * 0xFF51AFD7ED558CCDull; k ^= k >> 32; };
        mix(uint64_t(m_drawStaticBuffer));
        mix(vbOffset); mix(vertexCount); mix(ibOffset); mix(indexCount);
        mix(fvf); mix(primitive); mix(layout.stride);
        mix(reinterpret_cast<uintptr_t>(m_textures[0]));
        mix(reinterpret_cast<uintptr_t>(m_textures[1]));
        batchKey = k;
    }
    NoteBatch(batchKey);

    if (m_external) {
        vkCmdDrawIndexed(cmd, indexCount, 1, 0, m_external->baseVertex, recordIndex);
        ProfileDrawSection("draw: state + descriptors + draw", since);
        return;
    }
    if (m_drawStaticBuffer) {
        if (groupKey) {
            RecordIndirect(indexCount, ibOffset, vbOffset, layout.stride, recordIndex, groupKey);
        } else if (indices) {
            vkCmdDrawIndexed(cmd, indexCount, 1, uint32_t(ibOffset / 2), int32_t(vbOffset / layout.stride), recordIndex);
        } else {
            vkCmdDraw(cmd, vertexCount, 1, uint32_t(vbOffset / layout.stride), recordIndex);
        }
        ProfileDrawSection("draw: state + descriptors + draw", since);
        return;
    }
    if (m_drawGpu) {
        if (groupKey)
            RecordIndirect(indexCount, ibOffset, vbOffset, layout.stride, recordIndex, groupKey);
        else
            vkCmdDrawIndexed(cmd, indexCount, 1, uint32_t(ibOffset / 2), int32_t(vbOffset / layout.stride), recordIndex);
        ProfileDrawSection("draw: state + descriptors + draw", since);
        return;
    }
    if (groupKey)
        RecordIndirect(indexCount, ibOffset, vbOffset, layout.stride, recordIndex, groupKey);
    else if (indices)
        vkCmdDrawIndexed(cmd, indexCount, 1, uint32_t(ibOffset / 2), int32_t(vbOffset / layout.stride), recordIndex);
    else
        vkCmdDraw(cmd, vertexCount, 1, uint32_t(vbOffset / layout.stride), recordIndex);

    // A particle effect's sprites (ParticleEmitter): its particles follow, with the same state.
    if (m_particlePending && fvf == kParticleFvf) {
        ParticleBlock* block = m_particlePending;
        m_particlePending = nullptr;
        DrawParticles(*block);
    }
    ProfileDrawSection("draw: state + descriptors + draw", since);
}

}  // namespace rvk

namespace rvk {

// Texture picking (RequestPick): the draw's triangles in clip space against the picked point, in the viewport's
// coordinates; the nearest hit (depth) so far keeps its stage 0 texture. One frame, on the CPU.
void Device::PickDraw(uint32_t primitive, const detail::FvfLayout& layout, const void* vertices, uint32_t vertexCount,
                      const uint16_t* indices, uint32_t indexCount, bool terrain)
{
    if ((primitive != d3d::TriangleList && primitive != d3d::TriangleStrip && primitive != d3d::TriangleFan) ||
        layout.offset[0] < 0 || !m_target || !m_viewport.width || !m_viewport.height)
        return;
    // The cursor in the viewport's pixels: the game's viewport is in back-buffer pixels, also on a scaled scene
    // (RVK_RenderScale) - the scene's own size put the pick up and left of the cursor, on whatever was there.
    const bool scaled = m_target == m_scene && SceneScaled();
    const float tx = m_pickX * float(m_target->m_width) / (scaled ? SceneScaleX() : 1.0f);
    const float ty = m_pickY * float(m_target->m_height) / (scaled ? SceneScaleY() : 1.0f);
    const float px = (tx - float(m_viewport.x)) / float(m_viewport.width) * 2.0f - 1.0f;
    const float py = 1.0f - (ty - float(m_viewport.y)) / float(m_viewport.height) * 2.0f;
    if (std::fabs(px) > 1.0f || std::fabs(py) > 1.0f)
        return;
    const d3d::Matrix m = MulMatrix(MulMatrix(m_world, m_view), m_proj);
    const auto* base = static_cast<const uint8_t*>(vertices);
    auto clip = [&](uint32_t i, float out[3]) {
        float p[3];
        std::memcpy(p, base + size_t(i) * layout.stride + layout.offset[0], 12);
        float c[4];
        for (int k = 0; k < 4; ++k)
            c[k] = p[0] * m.m[0][k] + p[1] * m.m[1][k] + p[2] * m.m[2][k] + m.m[3][k];
        if (c[3] <= 1e-6f)
            return false;
        out[0] = c[0] / c[3];
        out[1] = c[1] / c[3];
        out[2] = c[2] / c[3];
        return true;
    };
    const uint32_t n = indices ? indexCount : vertexCount;
    const uint32_t triangles = primitive == d3d::TriangleList ? n / 3 : n >= 3 ? n - 2 : 0;
    uint32_t hit = ~0u;                          // this draw's nearest hit, if it is the nearest so far
    for (uint32_t t = 0; t < triangles; ++t) {
        uint32_t k[3];
        if (primitive == d3d::TriangleList) {
            k[0] = t * 3; k[1] = t * 3 + 1; k[2] = t * 3 + 2;
        } else if (primitive == d3d::TriangleStrip) {
            k[0] = t; k[1] = t + 1; k[2] = t + 2;
        } else {
            k[0] = 0; k[1] = t + 1; k[2] = t + 2;
        }
        float a[3], b[3], c[3];
        uint32_t i0 = indices ? indices[k[0]] : k[0], i1 = indices ? indices[k[1]] : k[1], i2 = indices ? indices[k[2]] : k[2];
        if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount || !clip(i0, a) || !clip(i1, b) || !clip(i2, c))
            continue;
        const float det = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1]);
        if (std::fabs(det) < 1e-12f)
            continue;
        const float l0 = ((b[1] - c[1]) * (px - c[0]) + (c[0] - b[0]) * (py - c[1])) / det;
        const float l1 = ((c[1] - a[1]) * (px - c[0]) + (a[0] - c[0]) * (py - c[1])) / det;
        const float l2 = 1.0f - l0 - l1;
        if (l0 < 0.0f || l1 < 0.0f || l2 < 0.0f)
            continue;
        const float z = l0 * a[2] + l1 * b[2] + l2 * c[2];
        if (z < 0.0f || z > 1.0f || z >= m_pickDepth)
            continue;
        m_pickDepth = z;
        m_pickBest = m_textures[0];
        m_pickBestVisual = m_drawVisual;
        m_pickBestOwner = m_drawOwner;
        m_pickBestKind = m_drawVisualKind;
        m_pickBestVertices = vertexCount;
        m_pickBestIndices = indexCount;
        m_pickBestTriangleCount = 0;
        m_pickBestTerrain = terrain;
        hit = t;
    }
    // (The ground: where the point lies in the world, for its cell's tile - EndFrame.)
    if (hit != ~0u && terrain) {
        d3d::Matrix inverse;
        if (InvertMatrix(MulMatrix(m_view, m_proj), &inverse)) {
            float w[4];
            for (int k = 0; k < 4; ++k)
                w[k] = px * inverse.m[0][k] + py * inverse.m[1][k] + m_pickDepth * inverse.m[2][k] + inverse.m[3][k];
            if (std::fabs(w[3]) > 1e-9f)
                for (int k = 0; k < 3; ++k) m_pickBestWorld[k] = w[k] / w[3];
        }
    }
    // A dungeon room hit (VisualKind::Room): the hit triangle and those joined to it by shared vertices - the room's
    // vertices aren't shared between the tile pieces placed in it, so that is the piece's sub-mesh as placed.
    // The same for a draw of the black texture behind shop doorways (1010004:6014, shop portals' phase 0): the doorway
    // quad, logged in the world (portal_diag.cpp's numbers' counterpart outdoors).
    const bool doorway = m_textures[0] && m_textures[0]->m_rdbId == 6014;
    if (hit == ~0u || (VisualKind(m_drawVisualKind) != VisualKind::Room && !doorway) ||
        primitive != d3d::TriangleList || !indices)
        return;
    auto corner = [&](uint32_t tri, int j) { return uint32_t(indices[tri * 3 + j]); };
    auto put = [&](uint32_t tri) {
        float* out = &m_pickBestTriangles[m_pickBestTriangleCount++ * 15];
        for (int j = 0; j < 3; ++j) {
            const uint8_t* v = base + size_t(corner(tri, j)) * layout.stride;
            std::memcpy(&out[j * 5], v + layout.offset[0], 12);
            if (layout.offset[4] >= 0)
                std::memcpy(&out[j * 5 + 3], v + layout.offset[4], 8);
            else
                out[j * 5 + 3] = out[j * 5 + 4] = 0.0f;
        }
    };
    std::vector<uint32_t> vertices_{corner(hit, 0), corner(hit, 1), corner(hit, 2)};
    std::vector<bool> taken(triangles, false);
    taken[hit] = true;
    put(hit);
    for (bool grew = true; grew && m_pickBestTriangleCount < kPickTriangles;) {   // (until nothing more joins)
        grew = false;
        for (uint32_t t = 0; t < triangles && m_pickBestTriangleCount < kPickTriangles; ++t) {
            if (taken[t]) continue;
            const uint32_t a = corner(t, 0), b = corner(t, 1), c = corner(t, 2);
            if (a >= vertexCount || b >= vertexCount || c >= vertexCount) continue;
            bool joined = false;
            for (uint32_t v : vertices_)
                if (v == a || v == b || v == c) { joined = true; break; }
            if (!joined) continue;
            taken[t] = grew = true;
            put(t);
            for (uint32_t v : {a, b, c})
                if (std::find(vertices_.begin(), vertices_.end(), v) == vertices_.end()) vertices_.push_back(v);
        }
    }
    if (!doorway)
        return;
    const auto& w = m_world.m;
    auto world = [&](uint32_t v, float out[3]) {
        float p[3];
        std::memcpy(p, base + size_t(v) * layout.stride + layout.offset[0], 12);
        for (int j = 0; j < 3; ++j)
            out[j] = p[0] * w[0][j] + p[1] * w[1][j] + p[2] * w[2][j] + w[3][j];
    };
    float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
    std::string corners;
    for (uint32_t v : vertices_) {
        float p[3];
        world(v, p);
        for (int j = 0; j < 3; ++j) { mn[j] = std::min(mn[j], p[j]); mx[j] = std::max(mx[j], p[j]); }
        char buf[64];
        std::snprintf(buf, sizeof(buf), " (%.2f %.2f %.2f)", double(p[0]), double(p[1]), double(p[2]));
        corners += buf;
    }
    float a[3], b[3], c[3];
    world(corner(hit, 0), a);
    world(corner(hit, 1), b);
    world(corner(hit, 2), c);
    const float u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, t[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    float nrm[3] = {u[1] * t[2] - u[2] * t[1], u[2] * t[0] - u[0] * t[2], u[0] * t[1] - u[1] * t[0]};
    const float len = std::sqrt(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]);
    for (float& x : nrm) x = len > 0.0f ? x / len : 0.0f;
    Log("portal diag: picked a black doorway piece: %u triangles of %u in the draw (%u vertices), centre (%.2f %.2f %.2f), "
        "size (%.2f %.2f %.2f), normal (%.3f %.3f %.3f), corners%s",
        m_pickBestTriangleCount, triangles, vertexCount, double(0.5f * (mn[0] + mx[0])), double(0.5f * (mn[1] + mx[1])),
        double(0.5f * (mn[2] + mx[2])), double(mx[0] - mn[0]), double(mx[1] - mn[1]), double(mx[2] - mn[2]),
        double(nrm[0]), double(nrm[1]), double(nrm[2]), corners.c_str());
}

}  // namespace rvk
