// D3D7 state, Clear, and drawing.
#include "internal.h"

#include <algorithm>
#include <cmath>
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
    fl->effects[3] = m_grassPush;
    Wind(fl->wind);
    FillPushers(fl, eye);
    fl->taa[0] = TaaActive() ? m_taaJitter[0] : 0.0f;
    fl->taa[1] = TaaActive() ? m_taaJitter[1] : 0.0f;
    fl->taa[2] = FrameNoise();                   // noise patterns move on each frame (averaged by the TAA)
    fl->taa[3] = float(m_windTimePrev);         // the wind's time last frame (plants' motion vectors)
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
    m_frameLightSpheres.clear();
    for (uint32_t k = 0; k < used; ++k) {
        const CapturedLight& c = m_lightsPrev[candidates[k].index];
        m_frameLightIndices.push_back(candidates[k].index);
        m_frameLightSpheres.push_back({c.light.position.x, c.light.position.y, c.light.position.z, c.light.range * c.light.range});
        FillGpuLight(c.light, c.cosHalfTheta, c.cosHalfPhi, fl->lights[k],
                     m_pointLightScale + (m_charLightScale - m_pointLightScale) * c.carried);
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
}

// Identity of a ground chunk within a frame: its sizes and a sample of its vertices (base and lighting passes draw
// the same vertices).
uint64_t Device::TerrainChunkKey(const void* vertices, uint32_t vertexCount, uint32_t stride, uint32_t indexCount)
{
    uint64_t h = 1469598103934665603ull ^ (uint64_t(vertexCount) << 32) ^ indexCount;
    const uint8_t* v = static_cast<const uint8_t*>(vertices);
    uint32_t step = vertexCount > 32 ? vertexCount / 32 : 1;
    for (uint32_t i = 0; i < vertexCount; i += step) {   // the position as three words, not twelve bytes
        uint32_t w[3];
        std::memcpy(w, v + size_t(i) * stride, sizeof(w));
        h = (h ^ w[0]) * 1099511628211ull;
        h = (h ^ w[1]) * 1099511628211ull;
        h = (h ^ w[2]) * 1099511628211ull;
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
           std::memcmp(&m_proj, &m_aoProj, sizeof(m_proj)) == 0 && std::memcmp(&m_view, &m_aoView, sizeof(m_view)) == 0;
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
bool Device::IsInterfaceDraw(uint32_t fvf)
{
    return (fvf & d3d::FVF_POSITION_MASK) == d3d::FVF_XYZRHW && !IsWater(fvf);
}

// Plants that sway in the wind: small (0.2 - 3 units tall), lit, drawn into the scene with a cut-out texture (the
// vertex shader keeps those whose texture is mostly holes - leaves, grass), and static: their vertices are the same as
// last frame (a character's hair or cloak is CPU-skinned, its vertices change every frame). out: DrawTransform sway.
bool Device::SwayParams(uint32_t fvf, uint32_t stride, const void* vertices, uint32_t vertexCount, float out[4])
{
    if ((m_sway <= 0.0f && m_grassPush <= 0.0f) || m_target != m_scene || !m_textures[0] || !m_rs[d3d::RS_LIGHTING] || m_drawIsLabel ||
        (fvf & d3d::FVF_POSITION_MASK) != d3d::FVF_XYZ || IsTerrain(fvf) || !m_rs[d3d::RS_ZWRITEENABLE] ||
        !(m_rs[d3d::RS_ALPHATESTENABLE] || m_rs[d3d::RS_ALPHABLENDENABLE]) || vertexCount < 3 || vertexCount > 4096 ||
        !m_drawMesh)
        return false;
    // A texture with no transparent texel never moves: the shaders scale sway and push by its holes (ffp.vert
    // smoothstep(0.92, 0.7, mean alpha) = 0). Most blended statics of plant size are such props - not split, kept on
    // the GPU and in the depth pre-pass.
    if (m_textures[0]->m_opaque)
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
// (the captured lights' carriers, matched by position and range) at m_charLightScale, others at m_pointLightScale.
float Device::LightScale(const d3d::Light& l) const
{
    if (l.type == d3d::LIGHT_DIRECTIONAL)
        return 1.0f;
    if (m_charLightScale != m_pointLightScale)
        for (const CapturedLight& c : m_lightsPrev) {
            if (c.carried <= 0.0f || c.light.range != l.range) continue;
            float dx = c.light.position.x - l.position.x, dy = c.light.position.y - l.position.y,
                  dz = c.light.position.z - l.position.z;
            if (dx * dx + dy * dy + dz * dz < 1.0f)
                return m_pointLightScale + (m_charLightScale - m_pointLightScale) * c.carried;
        }
    return m_pointLightScale;
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
// as triangle lists (3-point patches), indexed or not, depth-writing, with normals (the shape follows them), near the
// camera.
float Device::TessellateDraw(uint32_t primitive, uint32_t fvf, uint32_t vertexCount)
{
    if (m_tessShape <= 0.0f || !m_tessSupported || !m_drawIsCharacter || primitive != d3d::TriangleList ||
        !m_rs[d3d::RS_ZWRITEENABLE] || !(fvf & d3d::FVF_NORMAL) || vertexCount > 20000)
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
    if (m_foliageLod <= 0.0f || !m_drawMesh || !m_frameEyeValid || m_target != m_scene)
        return false;
    float c[3], e[3], d2 = 0.0f;
    DrawWorldBox(c, e);
    for (int j = 0; j < 3; ++j) {
        float d = std::max(std::fabs(m_frameEye[j] - c[j]) - e[j], 0.0f);
        d2 += d * d;
    }
    return d2 > m_foliageLod * m_foliageLod;
}

// The current draw's mesh box in the world: centre and half extents (the model box's centre moved, its half extents
// through the matrix's magnitudes).
void Device::DrawWorldBox(float c[3], float e[3]) const
{
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
    if (m_grassPush <= 0.0f || !m_drawMesh || m_drawMeshStatic || m_target != m_scene || m_drawIsLabel ||
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
            m_pushTrail.push_back({x, c.y, z, now, now, x, z, true});
            continue;
        }
        claimed[nearest] = true;
        PushPoint& h = m_pushTrail[nearest];
        if (std::sqrt(best) > 0.01f)
            h.born = now;                        // moving: the plants it touches rustle
        h.x = x; h.y = c.y; h.z = z; h.time = now;
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
    for (const PushPoint& t : m_pushTrail) {
        float dx = t.x - eye[0], dy = t.y - eye[1], dz = t.z - eye[2];
        items.push_back({std::sqrt(dx * dx + dy * dy + dz * dz) + 4.0f * float(now - t.time), &t});
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
    double t = m_windTime;
    float angle = 0.6f + 0.4f * float(std::sin(t * 0.013));
    out[0] = std::cos(angle);
    out[1] = std::sin(angle);
    out[2] = float(t);
    out[3] = m_sway;
}

namespace {
uint64_t LightCellKey(int32_t cx, int32_t cz)
{
    return (uint64_t(uint32_t(cx)) << 32) | uint64_t(uint32_t(cz));
}
}  // namespace

// The frame lights into a coarse x/z grid: each light's slot into every cell its sphere covers. FrameLightMask then
// tests a draw's few cells instead of all kFrameLights (a busy scene captures up to 40). A light covering more than
// 256 cells (a very large range) is tested by every draw instead (m_lightGridAlways).
void Device::BuildLightGrid()
{
    m_lightGrid.clear();
    m_lightGridAlways.clear();
    for (uint32_t slot = 0; slot < m_frameLightSpheres.size(); ++slot) {
        const LightSphere& s = m_frameLightSpheres[slot];
        float r = std::sqrt(std::max(0.0f, s.r2));
        int32_t cx0 = int32_t(std::floor((s.x - r) / kLightGridCell));
        int32_t cx1 = int32_t(std::floor((s.x + r) / kLightGridCell));
        int32_t cz0 = int32_t(std::floor((s.z - r) / kLightGridCell));
        int32_t cz1 = int32_t(std::floor((s.z + r) / kLightGridCell));
        if (int64_t(cx1 - cx0 + 1) * int64_t(cz1 - cz0 + 1) > 256) {
            m_lightGridAlways.push_back(slot);
            continue;
        }
        for (int32_t cz = cz0; cz <= cz1; ++cz)
            for (int32_t cx = cx0; cx <= cx1; ++cx)
                m_lightGrid.push_back({LightCellKey(cx, cz), slot});
    }
    std::sort(m_lightGrid.begin(), m_lightGrid.end());
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
    const LightSphere* spheres = m_frameLightSpheres.data();
    count = std::min<uint32_t>(count, uint32_t(m_frameLightSpheres.size()));
    uint64_t hits = 0;
    auto Test = [&](uint32_t k) {
        if (hits >> k & 1)
            return;
        const LightSphere& l = spheres[k];
        float dx = std::max(std::max(wmn[0] - l.x, l.x - wmx[0]), 0.0f);
        float dy = std::max(std::max(wmn[1] - l.y, l.y - wmx[1]), 0.0f);
        float dz = std::max(std::max(wmn[2] - l.z, l.z - wmx[2]), 0.0f);
        if (dx * dx + dy * dy + dz * dz <= l.r2)
            hits |= uint64_t(1) << k;
    };
    int32_t cx0 = int32_t(std::floor(wmn[0] / kLightGridCell)), cx1 = int32_t(std::floor(wmx[0] / kLightGridCell));
    int32_t cz0 = int32_t(std::floor(wmn[2] / kLightGridCell)), cz1 = int32_t(std::floor(wmx[2] / kLightGridCell));
    if (int64_t(cx1 - cx0 + 1) * int64_t(cz1 - cz0 + 1) > 256) {
        for (uint32_t k = 0; k < count; ++k) Test(k);        // a box wider than the grid's cells: all of them
    } else {
        for (uint32_t k : m_lightGridAlways)
            if (k < count) Test(k);
        for (int32_t cz = cz0; cz <= cz1; ++cz)
            for (int32_t cx = cx0; cx <= cx1; ++cx) {
                uint64_t key = LightCellKey(cx, cz);
                auto it = std::lower_bound(m_lightGrid.begin(), m_lightGrid.end(), std::make_pair(key, 0u));
                for (; it != m_lightGrid.end() && it->first == key; ++it)
                    if (it->second < count) Test(it->second);
            }
    }
    if (m_lightMaskVerify < 0) {
        const char* v = std::getenv("RANDYVK_LIGHTMASK_VERIFY");
        m_lightMaskVerify = v && *v == '1' ? 1 : 0;
    }
    if (m_lightMaskVerify) {                     // the same mask from every light: a check while bringing the grid up
        uint64_t brute = 0;
        for (uint32_t k = 0; k < count; ++k) {
            const LightSphere& l = spheres[k];
            float dx = std::max(std::max(wmn[0] - l.x, l.x - wmx[0]), 0.0f);
            float dy = std::max(std::max(wmn[1] - l.y, l.y - wmx[1]), 0.0f);
            float dz = std::max(std::max(wmn[2] - l.z, l.z - wmx[2]), 0.0f);
            if (dx * dx + dy * dy + dz * dz <= l.r2) brute |= uint64_t(1) << k;
        }
        if (brute != hits) ++m_lightMaskDiff;
        hits = brute;
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
    m_drawMesh = &m_meshInfo.emplace(key, info).first->second;
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
        if (m_drawOwner && (c.owner || m_sceneLightsFrame + 1 >= m_frameNumber)) {   // known: exactly
            if (c.owner == m_drawOwner)
                return k + 1;
            continue;
        }
        if (IsCarrierPart(c, m_world, mn, mx))
            return k + 1;
    }
    return 0;
}

void Device::SetDrawVisual(uint32_t kind, const char* className, uint32_t owner)
{
    m_drawVisualKind = kind;
    m_drawVisualName = className ? className : "";
    m_drawOwner = owner;
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
    if (!(flags & (d3d::CLEAR_TARGET | d3d::CLEAR_ZBUFFER)))
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
    m_drawSkin = &job;
    m_drawGpu = nullptr;
    // On the GPU: the whole piece with its own triangles (the game's draws of a character piece always are).
    if (m_gpuSkin && fvf == skin::kVertexFvf && startVertex == 0 && vertexCount == job.source->vertices.size() &&
        indices && indices == job.source->indices.data() && indexCount <= job.source->indices.size() && m_inFrame)
        m_drawGpu = SkinOnGpu(job);
    if (m_drawGpu) {
        Draw(primitive, fvf, nullptr, vertexCount, indices, indexCount);
    } else {
        const skin::Vertex* vertices = job.Skinned();
        m_drawSkinBase = vertices;
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
    vp->x = float(m_viewport.x) + 0.5f;
    vp->y = float(m_viewport.y + m_viewport.height) + 0.5f;
    vp->width = float(m_viewport.width);
    vp->height = -float(m_viewport.height);
    vp->minDepth = m_viewport.minZ;
    vp->maxDepth = m_viewport.maxZ;
    uint32_t w = m_target->m_width, h = m_target->m_height;
    uint32_t sx = std::min(m_viewport.x, w), sy = std::min(m_viewport.y, h);
    *scissor = {{int32_t(sx), int32_t(sy)}, {std::min(m_viewport.width, w - sx), std::min(m_viewport.height, h - sy)}};
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
    // for the no-discard variant (a draw that never cuts out keeps early-Z).
    uint32_t colorClass = m_drawTess ? 6u + (hdrTarget ? 1u : 0u) : topoClass + (hdrTarget ? 3u : 0u);
    uint32_t pipelineClass = colorClass + (m_drawMayDiscard ? 0u : 12u);
    if (c.topologyClass != pipelineClass) {
        VkPipeline pipeline;
        if (m_drawTess)
            pipeline = m_drawMayDiscard ? m_tessPipelines[hdrTarget ? 1 : 0] : m_tessPipelinesNoCut[hdrTarget ? 1 : 0];
        else if (hdrTarget)
            pipeline = m_drawMayDiscard ? m_pipelinesHdr[topoClass] : m_pipelinesHdrNoCut[topoClass];
        else
            pipeline = m_drawMayDiscard ? m_pipelines[topoClass] : m_pipelinesNoCut[topoClass];
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
    if (WaterWritesDepth(fvf))
        zWrite = 1;
    uint32_t zFunc = m_rs[d3d::RS_ZFUNC];
    if (m_drawPrepassed && zFunc == d3d::CMP_LESS)
        zFunc = d3d::CMP_LESSEQUAL;              // its own depth is already in the buffer (the pre-pass): pass equal
    if (c.depthTest != zEnable) { vkCmdSetDepthTestEnable(cmd, zEnable); c.depthTest = zEnable; }
    if (c.depthWrite != zWrite) { vkCmdSetDepthWriteEnable(cmd, zWrite); c.depthWrite = zWrite; }
    if (c.depthOp != zFunc) { vkCmdSetDepthCompareOp(cmd, CompareOp(zFunc)); c.depthOp = zFunc; }

    if (hdrTarget && !c.glowBlendSet) {
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

    if (c.fvf != fvf) {
        SetVertexInput(cmd, fvf, stride);
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
    c.vertexOffset = int32_t(vbOffset / stride);
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

void Device::Draw(uint32_t primitive, uint32_t fvf, const void* vertices, uint32_t vertexCount,
                  const uint16_t* indices, uint32_t indexCount)
{
    if (!m_inFrame || !vertexCount)
        return;
    ++m_frameDraw;
    // GPU profiling: which class of scene draw this is (splits the "scene" block; see ProfileSceneClass).
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
            }
        } else if (m_sceneSaw3D && IsInterfaceDraw(fvf)) {
            m_sceneEndDraw = m_dumpDraw;
            m_sceneEndFvf = fvf;
            EndScene();
        }
    }
    m_drawIsLabel = !m_external && IsLabel(primitive, fvf, vertexCount);
    if (m_dumpFile && !m_external)
        DumpDraw(primitive, fvf, m_drawGpu ? nullptr : vertices, vertexCount, indices, indexCount);
    if (!m_external && !m_drawGpu && IsBlobShadow(primitive, fvf, vertices, vertexCount, indexCount))
        return;                                  // replaced by sun shadows
    if (IsTerrain(fvf) && IsMultiplyPass())
        m_terrainLitPassCur = true;
    Frame& f = m_frames[m_frameIndex];
    VkCommandBuffer cmd = f.main;
    FvfLayout layout = DecodeFvf(fvf);
    double since = (m_frameNumber & 15) == 0 ? ProfileCpu() : 0.0;   // per-draw CPU sections (profiling)
    DrawMeshInfo(fvf, layout.stride, vertices, vertexCount, indices, indexCount);
    PushCandidateDraw(fvf);
    // A swaying plant: its big quads split into small ones (cached), so they bend rather than tilt as a whole.
    float sway[4] = {};
    bool swaying = !m_external && SwayParams(fvf, layout.stride, vertices, vertexCount, sway);
    if (swaying) {
        const void* before = vertices;
        SubdividePlant(primitive, fvf, layout, vertices, vertexCount, indices, indexCount);
        if (vertices != before)
            m_drawStaticBuffer = VK_NULL_HANDLE;     // split into new geometry: through the ring
    }

    // Render targets bound as textures must be readable; layout changes can't happen inside rendering.
    bool needTransition = false;
    for (Texture* t : m_textures)
        if (t && t->m_renderTarget && t->m_layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
            needTransition = true;
    if (needTransition) {
        FlushGroup();                                // the previous group's state is about to be invalidated
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
    bool reserveArenas = m_arenaGeneration != m_ringGeneration;
    VkDeviceSize arenaBytes = reserveArenas ? VkDeviceSize(m_constWanted) * sizeof(DrawConstants) +
                                                  VkDeviceSize(m_recordWanted) * sizeof(DrawRecord) +
                                                  VkDeviceSize(m_recordWanted) * sizeof(VkDrawIndexedIndirectCommand) +
                                                  ((m_shadows || m_pointShadows) ? VkDeviceSize(kShadowRecordCapacity) *
                                                                   (sizeof(ShadowRecord) +
                                                                    sizeof(VkDrawIndexedIndirectCommand))
                                                             : 0) + 64
                                            : 0;
    EnsureRingSpace(arenaBytes + sizeof(FrameLights) + geometryBytes + 3 * uboAlign +
                    layout.stride + 32 + (motion ? 12ull * vertexCount + 256 : 0) +
                    (tessLevel > 0.0f ? 12ull * vertexCount + 256 : 0));
    PrepareDrawArenas();
    // The arrays are full (a frame with more unique states or draws than reserved): submit what is recorded, wait,
    // and start over. Rare; the targets double for the next frames.
    if (m_constCount >= m_constCapacity || m_recordCount >= m_recordCapacity) {
        m_constWanted = std::min(m_constWanted * 2u, kMaxDrawConstCapacity);
        m_recordWanted = std::min(m_recordWanted * 2u, kMaxDrawRecordCapacity);
        FlushDrawArenas();
    }

    // The frame's light list (binding 4): rebuilt when lights changed; always bound, as layouts require.
    // Only lit draws read it; the others bind any in-range part of the ring.
    bool needLights = (m_lightOverride && m_pixelLighting && m_rs[d3d::RS_LIGHTING] &&
                       (fvf & d3d::FVF_POSITION_MASK) != d3d::FVF_XYZRHW) || ShadowReceiver(fvf) ||
                      ShadowCompensated(fvf) || motion ||
                      (TaaActive() && m_target == m_scene && (fvf & d3d::FVF_POSITION_MASK) != d3d::FVF_XYZRHW);
    VkDeviceSize frameLightsOffset = m_frameLightsGeneration == m_ringGeneration ? m_frameLightsOffset : 0;
    if (needLights && (m_frameLightsDirty || m_frameLightsGeneration != m_ringGeneration))   // once per frame
        frameLightsOffset = WriteFrameLights();

    ProfileDrawSection("draw: setup", since);
    // Per-draw world matrix: written into this draw's record.
    void* cpu;
    DrawTransform dt{};
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
    ProfileDrawSection("draw: sway", since);
    FrameLightMask(fvf, layout.stride, vertices, vertexCount, dt.lightMask);
    dt.lightMask[2] = swaying && m_grassPush > 0.0f ? PusherMask(1.4f * std::sqrt(m_grassPush) + 0.1f) : 0u;
    ProfileDrawSection("draw: light mask", since);
    if (motion) {
        // Motion vectors: the same object last frame - same mesh, nearest to where this one is (within 3 units). Not
        // found (new, or a different level of detail): its current matrix, i.e. it moved with the world.
        uint64_t key = MotionKey(primitive, fvf, vertexCount, indices, indexCount);
        // This frame's positions (for next frame's match) of a mesh small enough to be a character's part.
        // Static meshes (the mesh cache: same vertices as before) need none - only animated ones are compared.
        std::vector<float> positions;
        if (vertexCount <= kMotionMaxVertices && !m_drawMeshStatic && !m_drawGpu) {
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
                if (!positions.empty() && best->positions.size() == positions.size() &&
                    std::memcmp(best->positions.data(), positions.data(), positions.size() * 4) != 0) {
                    void* prevCpu;
                    prevPositionsOffset = Allocate(positions.size() * 4, m_props.limits.minStorageBufferOffsetAlignment, &prevCpu);
                    std::memcpy(prevCpu, best->positions.data(), positions.size() * 4);
                    prevPositionsBytes = positions.size() * 4;
                    dt.motion[1] = 1.0f;
                }
            }
        }
        m_motionCur[key].push_back({m_world, false, std::move(positions)});
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
    if (terrain && m_textures[0]) {
        if (!m_rs[d3d::RS_LIGHTING] && !m_rs[d3d::RS_ALPHABLENDENABLE]) {
            m_terrainBases[TerrainChunkKey(vertices, vertexCount, layout.stride, indexCount)] = m_textures[0];
        } else if (m_rs[d3d::RS_LIGHTING] && IsMultiplyPass()) {
            auto it = m_terrainBases.find(TerrainChunkKey(vertices, vertexCount, layout.stride, indexCount));
            if (it != m_terrainBases.end() &&
                (m_bump > 0.0f || (m_normalMaps && m_pixelLighting && it->second->m_normalMap)))
                m_drawBumpBase = it->second;
        }
    }
    uint32_t carrier = m_external ? 0 : CarriedLight(fvf, m_drawGpu ? nullptr : vertices, vertexCount, layout.stride);
    // The texture's own normal map (F_NORMALMAP), when it is the surface (stage 0, plain coordinates) of a per-pixel
    // lit draw; for the ground's lightmap pass, the normal map of its chunk's base texture (as the generated normals).
    Texture* normalMap = nullptr;
    if (m_normalMaps && m_pixelLighting && m_textures[0] && m_textures[0]->m_normalMap && !terrain &&
        m_rs[d3d::RS_LIGHTING] && m_tss[0][d3d::TSS_COLOROP] != d3d::TOP_DISABLE &&
        !(m_tss[0][d3d::TSS_TEXTURETRANSFORMFLAGS] & 256u) && (m_tss[0][d3d::TSS_TEXCOORDINDEX] & 0xFFFF0000u) == 0)
        normalMap = m_textures[0]->m_normalMap;
    else if (m_normalMaps && m_pixelLighting && m_drawBumpBase && m_drawBumpBase->m_normalMap)
        normalMap = m_drawBumpBase->m_normalMap;
    // The foliage level of detail depends on the draw's distance, not the render state: part of the block's key, or
    // a run of plants with the same state would all get the first one's (flickering as the camera moves).
    uint32_t foliageLod = FoliageFar() ? (m_drawSway[3] > 0.5f ? 2u : 1u) : 0u;
    bool rewrite = m_constantsDirty || m_constantsGeneration != m_ringGeneration || m_constantsFvf != fvf ||
                   m_constantsTexMask != texMask || m_constantsTerrain != terrain || m_constantsLabel != m_drawIsLabel ||
                   m_constantsCarrier != carrier || m_constantsBumpBase != m_drawBumpBase ||
                   m_constantsFoliageLod != foliageLod || m_constantsNormalMap != normalMap ||
                   m_constantsCharacter != m_drawIsCharacter || m_constantsOpaque != cutoutTexture ||
                   m_constantsHoles != textureHoles;
    uint32_t constIndex = m_constIndex;
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
    m_constantsCharacter = m_drawIsCharacter;
    m_constantsOpaque = cutoutTexture;
    m_constantsHoles = textureHoles;
    m_constantsNormalMap = normalMap;
    DrawConstants c{};
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
    ArgbToFloat(m_rs[d3d::RS_FOGCOLOR], c.fogColor);
    c.fogParams[0] = AsFloat(m_rs[d3d::RS_FOGSTART]);
    c.fogParams[1] = AsFloat(m_rs[d3d::RS_FOGEND]);
    c.fogParams[2] = AsFloat(m_rs[d3d::RS_FOGDENSITY]);
    c.fogParams[3] = 0.0f;
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
    std::memcpy(&c.vtx[1], &m_drawColorScale, 4);
    if (m_drawColorScale == 1.0f) c.vtx[1] = 0;
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
    std::memcpy(&c.vtx[2], &reflectivity, 4);
    std::memcpy(&c.vtx[3], &wet, 4);
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
    if ((flags & F_PERPIXEL) && normalMap)
        flags |= F_NORMALMAP | (m_drawBumpBase ? F_BUMPBASE : 0u);
    else if ((flags & F_PERPIXEL) && m_bump > 0.0f && m_textures[0] && !terrain && m_tss[0][d3d::TSS_COLOROP] != d3d::TOP_DISABLE &&
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
    m_drawMayDiscard = (flags & (F_ALPHATEST | F_CUTOUT)) != 0u;   // else the no-discard pipeline keeps early-Z
    // Night glow candidates: opaque 3D surfaces drawn unlit (self-lit, like windows and signs) or with an emissive
    // material - not effects, the sky, the ground or lighting passes.
    bool additive = m_rs[d3d::RS_ALPHABLENDENABLE] && m_rs[d3d::RS_DESTBLEND] == d3d::BLEND_ONE;
    float emissive = std::max({m_material.emissive.r, m_material.emissive.g, m_material.emissive.b});
    if (m_nightGlow > 0.0f && hdrTarget && solid3d && !additive && !IsMultiplyPass() &&
        (!(flags & F_LIGHTING) || emissive > 0.05f))
        flags |= F_EMISSIVE;
    // A character's body and parts: the lights characters carry don't shadow them (lighting.glsl).
    if (m_drawIsCharacter) flags |= F_CHARACTER;
    if (m_drawTerrainLight) flags |= F_NOALBEDO;   // its albedo write is masked (the base pass wrote it)
    c.flags[0] = flags;
    c.flags[1] = m_rs[d3d::RS_FOGVERTEXMODE];
    c.flags[2] = m_rs[d3d::RS_FOGTABLEMODE];
    c.flags[3] = m_rs[d3d::RS_ALPHAFUNC];
    c.matSources[0] = m_rs[d3d::RS_DIFFUSEMATERIALSOURCE];
    c.matSources[1] = m_rs[d3d::RS_AMBIENTMATERIALSOURCE];
    c.matSources[2] = m_rs[d3d::RS_SPECULARMATERIALSOURCE];
    c.matSources[3] = m_rs[d3d::RS_EMISSIVEMATERIALSOURCE];
    for (int s = 0; s < 2; ++s) {
        const auto& t = m_tss[s];
        c.stageA[s][0] = t[d3d::TSS_COLOROP];
        c.stageA[s][1] = t[d3d::TSS_COLORARG1];
        c.stageA[s][2] = t[d3d::TSS_COLORARG2];
        c.stageA[s][3] = t[d3d::TSS_ALPHAOP];
        c.stageB[s][0] = t[d3d::TSS_ALPHAARG1];
        c.stageB[s][1] = t[d3d::TSS_ALPHAARG2];
        c.stageB[s][2] = t[d3d::TSS_TEXCOORDINDEX];
        c.stageB[s][3] = t[d3d::TSS_TEXTURETRANSFORMFLAGS];
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
            FillGpuLight(slot.light, slot.cosHalfTheta, slot.cosHalfPhi, c.lights[lightCount++], LightScale(slot.light));
            if (slot.light.type != d3d::LIGHT_DIRECTIONAL) ++localLights;
        }
    c.lightInfo[0] = lightCount;
    c.lightInfo[1] = localLights;
    c.lightInfo[2] = override ? carrier : 0;   // frame light (index + 1) this draw carries: it doesn't light it
    c.lightInfo[3] = 0;
    constIndex = AppendConstant(c);            // shared with the next draws that keep this state
    m_constIndex = constIndex;
    }
    if (!m_drawMayDiscard) ++m_noCutDraws;       // per draw (the block above runs only when the constants change)
    if (m_drawFoliage) {
        ++m_foliageDraws;
        if (m_drawFoliage == 2u) ++m_foliageLodDraws;
    }

    ProfileDrawSection("draw: constants", since);
    // Geometry: vertices aligned to their stride and indices to 2 bytes, so the draw can address them inside
    // the ring buffer bound once (vertexOffset / firstIndex) instead of rebinding buffers per draw.
    VkDeviceSize vbOffset = 0, ibOffset = 0;
    if (m_drawGpu) {
        // Skinned on the GPU: the vertices in the frame's skin arena, the indices in the mesh's buffer.
        vbOffset = m_drawGpu->vertexOffset;
        ibOffset = m_drawGpu->mesh->indexOffset;
        dt.motion[2] = float(vbOffset / layout.stride);
        RecordShadowCaster(primitive, fvf, layout.stride, nullptr, vertexCount, vbOffset, indices, indices ? indexCount : 0,
                           ibOffset, m_frames[m_frameIndex].skinArena, m_drawGpu->mesh->buffer);
    } else if (m_drawStaticBuffer) {
        // Static geometry on the GPU: the vertices stay where they are, the indices come through the ring.
        vbOffset = m_drawStaticOffset;
        dt.motion[2] = float(vbOffset / layout.stride);
        if (indices) {
            ibOffset = Allocate(VkDeviceSize(indexCount) * 2, 2, &cpu);
            std::memcpy(cpu, indices, size_t(indexCount) * 2);
        }
        RecordShadowCaster(primitive, fvf, layout.stride, vertices, vertexCount, vbOffset, indices,
                           indices ? indexCount : 0, ibOffset, m_drawStaticBuffer, f.ring);
    } else if (!m_external) {
        VkDeviceSize vbBytes = VkDeviceSize(layout.stride) * vertexCount;
        vbOffset = Allocate(vbBytes, layout.stride, &cpu);
        std::memcpy(cpu, vertices, vbBytes);
        dt.motion[2] = float(vbOffset / layout.stride);   // the base vertex: gl_VertexIndex - it = vertex
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
            dt.tess[2] = float(vbOffset / layout.stride);
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
        dt.tess[2] = float(vbOffset / layout.stride);
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
    if (m_scenePhase && m_target == m_scene && !m_external && (fvf & d3d::FVF_POSITION_MASK) != d3d::FVF_XYZRHW)
        ++m_prepassWhy[prepassWhy];              // the scene's 3D draws, by why they are (not) pre-passed (log)
    uint64_t groupKey = 0;
    if (m_groupIndirect && indices && !m_external && !(m_particlePending && fvf == kParticleFvf)) {
        bool hdr = m_target->m_format == Format::RGBA16F;
        uint32_t pipelineClass = m_drawTess ? 6u + (hdr ? 1u : 0u)
                                            : TopologyClass(primitive) + (hdr ? 3u : 0u);
        VkBuffer vb = m_drawGpu ? m_frames[m_frameIndex].skinArena
                                : (m_drawStaticBuffer ? m_drawStaticBuffer : f.ring);
        VkBuffer ib = m_drawGpu ? m_drawGpu->mesh->buffer : f.ring;
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
        groupKey = k ? k : 1;
    }
    if (m_group.active && (groupKey == 0 || groupKey != m_group.key))
        FlushGroup();                                // the pending group's state is about to change
    // A draw that extends the pending group shares its pipeline, dynamic state, descriptors and buffers, so only
    // its record and indirect command are added - the expensive per-draw setup is skipped.
    bool extending = m_group.active && groupKey != 0 && groupKey == m_group.key;

    // Bindless textures (set 1, M1): the draw's four textures and four samplers by index, so nothing per-draw is
    // bound as an image descriptor. The indices live in the draw's record (constants.glsl D.texIdx/sampIdx).
    Texture* texStage0 = m_textures[0] ? m_textures[0] : m_blackTexture;
    Texture* texStage1 = m_textures[1] ? m_textures[1] : m_blackTexture;
    Texture* bumpBase = m_drawBumpBase ? m_drawBumpBase : m_blackTexture;
    Texture* normalTex = normalMap ? normalMap : m_flatNormal;
    dt.texIdx[0] = BindlessImage(texStage0);
    dt.texIdx[1] = BindlessImage(texStage1);
    dt.texIdx[2] = BindlessImage(bumpBase);
    dt.texIdx[3] = BindlessImage(normalTex);
    dt.sampIdx[0] = BindlessSampler(SamplerFor(0));
    dt.sampIdx[1] = BindlessSampler(SamplerFor(1));
    dt.sampIdx[2] = BindlessSampler(m_bumpSampler);
    dt.sampIdx[3] = BindlessSampler(m_normalSampler);
    // GPU-driven M2: this draw's record, and the frame's two arrays (bindings 0 = constants, 12 = records) pushed
    // once per frame's command buffer. The record index travels in firstInstance (gl_InstanceIndex).
    uint32_t recordIndex = AppendRecord(constIndex, dt);
    if (m_drawPrepassed)
        PrepassDraw(primitive, fvf, layout.stride, vertexCount, indices ? indexCount : 0, vbOffset, ibOffset,
                    m_drawStaticBuffer ? m_drawStaticBuffer : f.ring, f.ring, frameLightsOffset, prevPositionsBuffer,
                    prevPositionsOffset, prevPositionsBytes, smoothBuffer, smoothOffset, smoothBytes, recordIndex);
    if (!extending) {
    ApplyDynamicState(primitive, fvf, layout.stride);
    if (!m_arenaBound) {
        // The frame's pushed set: the two arrays (0 = constants, 12 = records) and the shadow maps (5, 6, 9) are
        // constant for the frame's command buffer, so they are pushed once, not on every non-extending draw.
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
    VkDescriptorBufferInfo frameLights{f.ring, frameLightsOffset, sizeof(FrameLights)};
    // Binding 8: an animated mesh's last positions, else any small part of the ring (unread).
    VkDescriptorBufferInfo prevPositions{prevPositionsBuffer, prevPositionsOffset, prevPositionsBytes ? prevPositionsBytes : 16};
    // Binding 10: a tessellated draw's averaged normals, else any small part of the ring (unread).
    VkDescriptorBufferInfo smoothNormals{smoothBuffer, smoothOffset, smoothBytes ? smoothBytes : 16};
    // The per-draw bindings: the frame lights (rebuilt when the lights change) and the two buffers that vary by draw.
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
    if (!m_bindlessBound) {                       // set 1, once per frame's command buffer
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 1, 1, &m_bindlessSet, 0, nullptr);
        m_bindlessBound = true;
    }
    }

    // Would an instanced batch cover this draw together with the one before it? Only static snapshots share their
    // vertex data across draws; ring copies are unique per draw, so those never merge.
    uint64_t batchKey = 0;
    if (m_drawStaticBuffer && !m_external && !m_drawGpu) {
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
        VkBuffer buffers[2] = {m_external->vertices, m_nullBuffer};
        VkDeviceSize offsets[2] = {0, 0};
        vkCmdBindVertexBuffers(cmd, 0, 2, buffers, offsets);
        vkCmdBindIndexBuffer(cmd, m_external->indices, 0, VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexed(cmd, indexCount, 1, 0, m_external->baseVertex, recordIndex);
        m_cache.buffersBound = false;            // the next draw binds the ring again
        return;
    }
    if (m_drawStaticBuffer) {
        if (!extending) {
            VkBuffer buffers[2] = {m_drawStaticBuffer, m_nullBuffer};
            VkDeviceSize offsets[2] = {0, 0};
            vkCmdBindVertexBuffers(cmd, 0, 2, buffers, offsets);
            if (indices)
                vkCmdBindIndexBuffer(cmd, f.ring, 0, VK_INDEX_TYPE_UINT16);
        }
        if (groupKey) {
            RecordIndirect(indexCount, ibOffset, vbOffset, layout.stride, recordIndex, groupKey);
        } else if (indices) {
            vkCmdDrawIndexed(cmd, indexCount, 1, uint32_t(ibOffset / 2), int32_t(vbOffset / layout.stride), recordIndex);
        } else {
            vkCmdDraw(cmd, vertexCount, 1, uint32_t(vbOffset / layout.stride), recordIndex);
        }
        m_cache.buffersBound = false;            // the next draw binds the ring again
        return;
    }
    if (m_drawGpu) {
        if (!extending) {
            VkBuffer buffers[2] = {m_frames[m_frameIndex].skinArena, m_nullBuffer};
            VkDeviceSize offsets[2] = {0, 0};
            vkCmdBindVertexBuffers(cmd, 0, 2, buffers, offsets);
            vkCmdBindIndexBuffer(cmd, m_drawGpu->mesh->buffer, 0, VK_INDEX_TYPE_UINT16);
        }
        if (groupKey)
            RecordIndirect(indexCount, ibOffset, vbOffset, layout.stride, recordIndex, groupKey);
        else
            vkCmdDrawIndexed(cmd, indexCount, 1, uint32_t(ibOffset / 2), int32_t(vbOffset / layout.stride), recordIndex);
        m_cache.buffersBound = false;            // the next draw binds the ring again
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
