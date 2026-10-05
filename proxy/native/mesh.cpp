// Static meshes natively (randy-vk.ini [Native] Scene=on): how the scene draws everything made of SimpleMeshes - an
// RVisual_t's Rasterize (texture animation, its observers, the sub-meshes back to front when asked) and
// RenderWithTransparency (the visual's transparency, emissive and specular overrides on the material, the environment
// map and statel light passes), SimpleMesh::Render, and RTriMesh_t (a static mesh of the playfield: buildings, trees,
// props) - its Process (culling against the camera with its bounding volume), Render, RenderDepth, RenderShadow.
//
// SimpleMesh: +0x14 RMaterial_t*, +0x18 BVolume_t* (made when first asked), +0x1E flags (bit 0: its vertex buffer is
// the client's), +0x20 its data: +0x30 the indices (+8 begin, +0xC end), +0x34 vertex count, +0x38 triangle count,
// +0x3C flags (bit 1: a hardware copy), +0x40 FVF, +0x44 the hardware copy, +0x48 the vertex buffer.
// RTriMeshData_t (an RVisualData_t): +0x4C std::vector<SimpleMesh*>, +0x60 the frame it was last drawn, +0x6C
// BVolume_t* (+8 centre, +0x14 radius, +0x18 / +0x24 box corners); RVisualData_t itself stops at 0x68.
// RTriMesh_t (an RVisual_t): +0x178 16-bit vertex colours (or null), +0x182 drawn transparent, +0x184 RTriMeshData_t*,
// +0x188 visible this frame, +0x1B8 lights lighting it, +0x1BC sub-meshes sorted, +0x1BD registered with the occluder,
// +0x1BE an occluder itself ("[OCC]..."), +0x1BF culled by its bounding volume.
#include "native/mesh.h"
#include "native/mesh_data.h"
#include "native/orig_api.gen.h"
#include "native/serialize.h"
#include "native/vc10.h"
#include "native/xmath.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace rnative::mesh {

namespace {

HMODULE g_orig;
void* const* g_randy;

template <typename T>
T& Field(void* object, uint32_t offset) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(object) + offset); }
template <typename T>
T& Global(uint32_t rva) { return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(g_orig) + rva); }
template <typename F>
F Internal(uint32_t rva) { return reinterpret_cast<F>(reinterpret_cast<uint8_t*>(g_orig) + rva); }

template <typename R = void, typename... A>
R Virtual(void* object, uint32_t slot, A... args)
{
    return reinterpret_cast<R(__fastcall*)(void*, void*, A...)>((*static_cast<void***>(object))[slot])(object, nullptr,
                                                                                                      args...);
}

using Visual = uint8_t;

constexpr uint32_t kDebuggerMode = 0xB7500, kRender = 0x16BED0, kCurrentCamera = 0x17D338;
// Randy_t
constexpr uint32_t kFrame = 0x274, kDeviceState = 0x27C;
// DeviceState: the render states / texture stage states / textures it wants
constexpr uint32_t kWantedRs = 0x4C8, kWantedTss = 0xD84, kTssStage = 0x64, kWantedTextures = 0x1128;
// RViewPort_t
constexpr uint32_t kVpState = 0x08, kVpCamera = 0x0C, kVpMaterials = 0xD8, kVpMaterialIndex = 0x160;
// RRefFrame_t / RVisual_t
constexpr uint32_t kFade = 0x0C, kRotation = 0x2C, kWorldScale = 0x84, kTransparency = 0x88, kEmissive = 0x8C,
                   kSpecular = 0x90, kWorldDirty = 0x9E, kHook = 0xB0, kKeepPriority = 0xBC, kEnvAllowed = 0xE1,
                   kBlob = 0xF4, kObservers = 0xAC;
// RTriMesh_t
constexpr uint32_t kColours = 0x178, kTransparentFlag = 0x182, kData = 0x184, kVisibleNow = 0x188, kLitBy = 0x1B8,
                   kSorted = 0x1BC, kRegistered = 0x1BD, kIsOccluder = 0x1BE, kCullByVolume = 0x1BF;
constexpr uint32_t kTriVtable = 0x957F4, kTriSubjectVtable = 0x957E0, kSubjectPart = 0xA4, kTimer = 0x190,
                   kRestoredAt = 0xF0, kDataRestored = 0x5C, kTriSize = 0x1C0;
// SimpleMesh / its data / RVisualData_t
constexpr uint32_t kMaterial = 0x14, kVolume = 0x18, kMeshFlags = 0x1E, kMeshData = 0x20;
constexpr uint32_t kIndices = 0x30, kVertexCount = 0x34, kTriangleCount = 0x38, kDataFlags = 0x3C, kFvf = 0x40,
                   kHardware = 0x44, kVertexBuffer = 0x48;
constexpr uint32_t kMeshes = 0x4C, kLastDrawn = 0x60, kDataVolume = 0x6C;

enum : int32_t {   // D3DRENDERSTATETYPE / D3DTEXTURESTAGESTATETYPE / D3DTRANSFORMSTATETYPE
    kFillMode = 8, kAlphaBlend = 0x1B, kAmbient = 0x8B, kLighting = 0x89, kSpecularSource = 0x92,
    kEmissiveSource = 0x94, kColorOp = 1, kTexCoordIndex = 0xB, kTransformFlags = 0x18, kWorld = 1,
    kTexture0 = 0x10, kTexture1 = 0x11
};

void* Randy() { return *g_randy; }
void* State() { return Field<void*>(Randy(), kDeviceState); }
uint32_t DebuggerMode() { return Global<uint32_t>(kDebuggerMode); }
void* Render() { return Global<void*>(kRender); }
const float* World(void* f) { return static_cast<const float*>(orig::RRefFrame_t_GetWorldMatrix(f)); }
vc10::Vector<uint8_t*>& Meshes(void* data) { return Field<vc10::Vector<uint8_t*>>(data, kMeshes); }
uint32_t& WantedTss(void* state, uint32_t stage, uint32_t type)
{
    return Field<uint32_t>(state, kWantedTss + stage * kTssStage + type * 4);
}

// A render state for a scope (FUN_1001237d / FUN_100123c0): set, and put back when it went through.
struct StateGuard {
    int32_t state;
    uint32_t before;
    bool changed;
};
StateGuard* __fastcall GuardOpen(StateGuard* g, void*, int32_t state, uint32_t value, int32_t priority)
{
    void* ds = State();
    g->state = state;
    g->before = Field<uint32_t>(ds, kWantedRs + uint32_t(state) * 4);
    g->changed = orig::DeviceState_SetRenderState(ds, state, value, priority);
    return g;
}
void __fastcall GuardClose(StateGuard* g)
{
    if (g->changed) orig::DeviceState_SetRenderState(State(), g->state, g->before, 2);
}
struct ScopedState {
    StateGuard g;
    ScopedState(int32_t state, uint32_t value, int32_t priority) { GuardOpen(&g, nullptr, state, value, priority); }
    ~ScopedState() { GuardClose(&g); }
};
uint32_t FillMode() { return (~(DebuggerMode() >> 4) & 1) | 2; }   // solid, wireframe in the debugger's mode 0x10

// ---- the visual's observers (its SubjectImpl's std::set, VS2010 tree: left, parent, right, value, ..., +0x11 head) --

struct SetNode {
    SetNode* left;
    SetNode* parent;
    SetNode* right;
    void* value;
};
bool IsHead(const SetNode* n) { return reinterpret_cast<const uint8_t*>(n)[0x11] != 0; }
SetNode* Next(SetNode* n)                           // FUN_1005b04f
{
    if (IsHead(n)) return n;
    if (!IsHead(n->right)) {
        n = n->right;
        while (!IsHead(n->left)) n = n->left;
        return n;
    }
    SetNode* p = n->parent;
    while (!IsHead(p) && n == p->right) {
        n = p;
        p = p->parent;
    }
    return p;
}
template <typename F>
void EachObserver(Visual* v, F f)
{
    SetNode* head = Field<SetNode*>(v, kObservers);
    for (SetNode* n = head->left; n != head; n = Next(n)) f(n->value);
}
bool ObserversPreRender(Visual* v, void* vp)        // FUN_10013e2e: any of them says "don't draw"
{
    uint8_t skip = 0;
    EachObserver(v, [&](void* o) { skip |= Virtual<uint8_t>(o, 0, v, vp); });
    return skip != 0;
}
void ObserversPostRender(Visual* v, void* vp)       // FUN_10013f00
{
    EachObserver(v, [&](void* o) { Virtual(o, 3, v, vp); });
}
void ObserversMaterial(Visual* v, void* material, void* vp, void* d3dMaterial)   // FUN_10013e71, FUN_10013ebb
{
    EachObserver(v, [&](void* o) { Virtual<uint8_t>(o, 1, v, material, vp, d3dMaterial); });
    EachObserver(v, [&](void* o) { Virtual(o, 2, v, material, vp, Field<void*>(vp, 8)); });
}

// ---- SimpleMesh ----

void* __fastcall MeshVertexBuffer(uint8_t* m)        // SimpleMesh::GetVertexBuffer
{
    uint8_t* d = Field<uint8_t*>(m, kMeshData);
    void* vb = Field<void*>(d, kVertexBuffer);
    return vb ? vb : Field<void*>(d, kHardware);
}
uint32_t __fastcall MeshFormat(uint8_t* m) { return Field<uint32_t>(Field<uint8_t*>(m, kMeshData), kFvf); }
uint32_t __fastcall MeshVertexCount(uint8_t* m) { return Field<uint32_t>(Field<uint8_t*>(m, kMeshData), kVertexCount); }
uint32_t __fastcall MeshTriangleCount(uint8_t* m)
{
    return Field<uint32_t>(Field<uint8_t*>(m, kMeshData), kTriangleCount);
}

// FUN_1004872b: a new hardware copy of the vertex buffer, when the data keeps one.
void* __fastcall RestoreHardware(uint8_t* d)
{
    if (Field<uint32_t>(d, kDataFlags) & 2) {
        if (void* old = Field<void*>(d, kHardware)) {
            orig::VertexBuffer_c_Release(old);
            Field<void*>(d, kHardware) = nullptr;
        }
        Field<void*>(d, kHardware) =
            orig::VertexBuffer_c_VertexBuffer_c_59(vc10::Allocate(4), Field<void*>(d, kVertexBuffer), 8, 0);
    }
    return Field<void*>(d, kHardware);
}
void* __fastcall MeshRestoreHardware(uint8_t* m) { return RestoreHardware(Field<uint8_t*>(m, kMeshData)); }

void __fastcall MeshSetMaterial(uint8_t* m, void*, void* material)
{
    if (material) orig::RResource_t_AddRefRResource(material);
    if (void* old = Field<void*>(m, kMaterial)) orig::RResource_t_ReleaseRResource(old);
    Field<void*>(m, kMaterial) = material;
}

const void* __fastcall MeshVolume(uint8_t* m)        // SimpleMesh::GetBoundingVolume
{
    if (!Field<void*>(m, kVolume)) Internal<void(__fastcall*)(void*)>(0x4F820)(m);   // made from its vertices
    return Field<void*>(m, kVolume);
}

// FUN_1001fdba: a triangle list from the index range (none for fewer than 4 vertices or 2 triangles); always 0.
uint32_t __fastcall DrawIndexed(uint8_t* indices, void*, void* vb, uint32_t vertices, uint32_t triangles,
                                uint32_t flags, int32_t start)
{
    uint8_t* begin = Field<uint8_t*>(indices, 8);
    if (vertices > 3 && triangles > 1 && ((Field<uint8_t*>(indices, 0xC) - begin) & ~1) != 0)
        orig::render_t_RenderTriangleList_408(Render(), vb, flags, vertices, begin + start * 6, triangles * 3, 0);
    return 0;
}

// SimpleMesh::Render. (Its retry and lost-device paths only follow a failed draw, which FUN_1001fdba never reports.)
void __fastcall MeshRender(uint8_t* m, void*, void*)
{
    uint8_t* d = Field<uint8_t*>(m, kMeshData);
    void* vb = Field<void*>(d, kVertexBuffer);
    if (!vb) vb = Field<void*>(d, kHardware);
    if (!(m[kMeshFlags] & 1) && !vb) vb = RestoreHardware(d);
    DrawIndexed(Field<uint8_t*>(d, kIndices), nullptr, vb, Field<uint32_t>(d, kVertexCount),
                Field<uint32_t>(d, kTriangleCount), 0, 0);
}

bool __fastcall DataIsTransparent(uint8_t* data)    // RVisualData_t::IsTransparent
{
    for (uint8_t** m = Meshes(data).first; m != Meshes(data).last; ++m) {
        void* material = Field<void*>(*m, kMaterial);
        orig::RMaterial_t_GetTexture(material, 0);
        if (Field<uint8_t>(material, 0xBD)) return true;
    }
    return false;
}

// ---- RVisual_t drawing ----

// FUN_1004d076: the statel light's pass - the mesh again with the light's texture projected (Shadowlands).
void __fastcall StatelPass(Visual* v, void*, void* vp, uint8_t* mesh)
{
    if (!orig::RandyShadowlandsData_s_IsStatelLightUsed()) return;
    const float intensity = orig::RandyShadowlandsData_s_GetStatelLightIntensity();
    if (!(0.01f < intensity)) return;
    void* blob = v + kBlob;
    if (!orig::StateBlob_c_Validate(blob)) return;
    orig::RVisual_t_CullLights(v, vp, 1.0f, 0);
    const uint32_t grey =
        uint32_t(int64_t(double(orig::RandyShadowlandsData_s_GetStatelLightIntensity()) * 255.0)) & 0xFFFFFFFFu;
    orig::StateBlob_c_SetRenderState(blob, kAmbient, (grey << 8 | grey) << 8 | grey);
    orig::StateBlob_c_Set(blob);
    xm::M4 m = xm::Load(orig::RandyShadowlandsData_s_GetStatelLightMatrix());
    orig::RViewPort_t_RealizeRenderStates(vp);
    void* ds = Field<void*>(vp, kVpState);
    void* before = Field<void*>(ds, kWantedTextures);
    orig::DeviceState_SetTexture(ds, Field<void*>(orig::RandyShadowlandsData_s_GetStatelLightTexture(), 0x30), 0, 10);
    orig::DeviceState_UpdateDevice(ds);
    orig::render_t_SetTransformMatrix(Render(), kTexture0, m.m);
    MeshRender(mesh, nullptr, vp);
    orig::DeviceState_SetTexture(ds, before, 0, 10);
    orig::DeviceState_UpdateDevice(ds);
    orig::StateBlob_c_Reset(blob);
}

// FUN_1004d18a: with the material's environment map on stage 1 (camera-space normals, scaled into 0..1, added).
void __fastcall EnvironmentPass(Visual* v, void*, void* vp, uint8_t* mesh)
{
    xm::M4 m = xm::Identity();
    xm::ScaleColumns(m, 0.5f, 0.5f, 1.0f);
    m.m[12] = float(double(m.m[12]) + 0.5);
    m.m[13] = float(0.5 + double(m.m[13]));
    m.m[14] = float(double(m.m[14]) + 0.0);
    orig::render_t_SetTransformMatrix(Render(), kTexture1, m.m);
    void* ds = Field<void*>(vp, kVpState);
    const uint32_t texCoords = WantedTss(ds, 1, kTexCoordIndex), flags = WantedTss(ds, 1, kTransformFlags),
                   colorOp = WantedTss(ds, 1, kColorOp);
    orig::DeviceState_SetTextureStageState(ds, 1, kTexCoordIndex, 0x10000, 10);
    orig::DeviceState_SetTextureStageState(ds, 1, kTransformFlags, 2, 10);
    orig::DeviceState_SetTextureStageState(ds, 1, kColorOp, 7, 10);
    void* before = Field<void*>(ds, kWantedTextures + 4);
    void* env = orig::RMaterial_t_GetEnvTexture(Field<void*>(mesh, kMaterial));
    orig::DeviceState_SetTexture(ds, Field<void*>(env, 0x30), 1, 10);
    orig::DeviceState_UpdateDevice(ds);
    MeshRender(mesh, nullptr, vp);
    if (orig::RandyShadowlandsData_s_IsStatelLightUsed()) StatelPass(v, nullptr, vp, mesh);
    orig::DeviceState_SetTexture(ds, before, 1, 10);
    orig::DeviceState_SetTextureStageState(ds, 1, kTexCoordIndex, texCoords, 10);
    orig::DeviceState_SetTextureStageState(ds, 1, kTransformFlags, flags, 10);
    orig::DeviceState_SetTextureStageState(ds, 1, kColorOp, colorOp, 10);
}

// RVisual_t::RenderWithTransparency: one sub-mesh with the visual's own transparency / emissive / specular.
void __fastcall RenderWithTransparency(Visual* v, void*, void* vp, uint8_t* mesh)
{
    void* material = Field<void*>(mesh, kMaterial);
    const float* emissive = Field<const float*>(v, kEmissive);
    const float* specular = Field<const float*>(v, kSpecular);
    const float t = Field<float>(v, kTransparency);
    void* ds = State();
    uint32_t specularWas = 1, emissiveWas = 1, blendWas = 1;
    static const uint16_t stages = orig::Randy_t_GetTextureStageCount(Randy());
    const bool env = orig::RMaterial_t_GetEnvTexture(material) && v[kEnvAllowed] && stages >= 2;
    const bool hook = Field<void*>(v, kHook) != nullptr;
    orig::RViewPort_t_SetMaterial(vp, material);
    if (specular || emissive || t < 1.0f || hook) {
        uint8_t d3d[0x44];                           // D3DMATERIAL7: the viewport's current one (FUN_1004b5fb)
        std::memcpy(d3d, static_cast<uint8_t*>(vp) + kVpMaterials + Field<uint32_t>(vp, kVpMaterialIndex) * 0x44,
                    sizeof(d3d));
        float* f = reinterpret_cast<float*>(d3d);    // diffuse 0, ambient 4, specular 8, emissive 12, power 16
        if (specular) {
            f[8] = specular[0], f[9] = specular[1], f[10] = specular[2], f[11] = t;
            f[16] = specular[0];
            specularWas = Field<uint32_t>(ds, kWantedRs + kSpecularSource * 4);
            if (!specularWas) orig::DeviceState_SetRenderState(ds, kSpecularSource, 1, 10);
        }
        if (emissive) {
            emissiveWas = Field<uint32_t>(ds, kWantedRs + kEmissiveSource * 4);
            f[12] = emissive[0], f[13] = emissive[1], f[14] = emissive[2], f[15] = t;
            if (!emissiveWas) orig::DeviceState_SetRenderState(ds, kEmissiveSource, 1, 10);
        }
        if (t < 1.0f) {
            blendWas = Field<uint32_t>(ds, kWantedRs + kAlphaBlend * 4);
            if (!blendWas) orig::DeviceState_SetRenderState(ds, kAlphaBlend, 1, 10);
        }
        f[3] = t;
        if (hook) ObserversMaterial(v, material, vp, d3d);
        orig::RViewPort_t_SetD3DMaterial(vp, d3d);
        orig::RViewPort_t_RealizeRenderStates(vp);
        if (env) EnvironmentPass(v, nullptr, vp, mesh);
    } else if (env) {
        EnvironmentPass(v, nullptr, vp, mesh);
    } else {
        orig::DeviceState_UpdateDevice(Field<void*>(vp, kVpState));
    }
    if (!env) {
        MeshRender(mesh, nullptr, vp);
        if (orig::RandyShadowlandsData_s_IsStatelLightUsed()) StatelPass(v, nullptr, vp, mesh);
    }
    if (!blendWas) orig::DeviceState_SetRenderState(ds, kAlphaBlend, 0, 2);
    if (!emissiveWas) orig::DeviceState_SetRenderState(ds, kEmissiveSource, 0, 2);
    if (!specularWas) orig::DeviceState_SetRenderState(ds, kSpecularSource, 0, 2);
    orig::RViewPort_t_ResetMaterial(vp);
}

// RVisual_t::Rasterize: all sub-meshes of `data` (lit or not; sorted far to near when asked).
void __fastcall Rasterize(Visual* v, void*, void* vp, uint8_t* data, bool lit, bool sorted)
{
    ScopedState fill(kFillMode, FillMode(), 10);
    Field<uint32_t>(data, kLastDrawn) = Field<uint32_t>(Randy(), kFrame);
    const float t = Field<float>(v, kTransparency);
    if (!(1e-5f < t && 1e-5f < Field<float>(v, kFade))) return;
    if (Field<float>(v, kFade) < t) orig::RRefFrame_t_SetTransparency(v, Field<float>(v, kFade));
    void* anim = const_cast<void*>(orig::RRefFrame_t_GetAnimation(v));
    float su = 0, sv = 0, tu = 0, tv = 0;
    bool texAnim = anim && Virtual<uint8_t>(anim, 4, &su, &sv, &tu, &tv) != 0;
    uint32_t stage0Was = 0, stage1Was = 0;
    if (texAnim) {                                   // the texture's own animation: scale and scroll
        void* ds = Field<void*>(vp, kVpState);
        xm::M4 m{{su, 0, 0, 0, 0, sv, 0, 0, tu, tv, 1, 0, 0, 0, 0, 1}};
        stage0Was = WantedTss(ds, 0, kTransformFlags);
        orig::DeviceState_SetTextureStageState(ds, 0, kTransformFlags, 2, 10);
        orig::render_t_SetTransformMatrix(Render(), kTexture0, m.m);
        if (orig::Randy_t_GetTextureStageCount(Randy()) > 1) {
            ds = Field<void*>(vp, kVpState);
            stage1Was = WantedTss(ds, 1, kTransformFlags);
            orig::DeviceState_SetTextureStageState(ds, 1, kTransformFlags, 2, 10);
            orig::render_t_SetTransformMatrix(Render(), kTexture1, m.m);
        }
    }
    if (ObserversPreRender(v, vp)) {
        if (Field<float>(v, kFade) < t) orig::RRefFrame_t_SetTransparency(v, t);
        return;
    }
    Virtual(v, 16, vp);
    Virtual(v, 17, vp);
    {
        ScopedState lighting(kLighting, lit, 10);
        auto& meshes = Meshes(data);
        if (sorted) {
            struct Far {
                int32_t index;
                float distance;
            };
            std::vector<Far> order(meshes.size());
            const float* eye = World(Field<void*>(vp, kVpCamera)) + 12;
            const float camera[3] = {eye[0], eye[1], eye[2]};
            for (uint32_t i = 0; i < meshes.size(); ++i) {
                order[i].index = int32_t(i);
                const float* volume = static_cast<const float*>(MeshVolume(meshes.first[i]));
                float c[3], d[3];
                xm::Transform(volume + 2, World(v), c);
                d[0] = c[0] - camera[0], d[1] = c[1] - camera[1], d[2] = c[2] - camera[2];
                order[i].distance = float(std::sqrt(double(xm::LengthSquared(d))));
            }
            std::sort(order.begin(), order.end(), [](const Far& a, const Far& b) { return b.distance < a.distance; });
            for (const Far& o : order) RenderWithTransparency(v, nullptr, vp, Meshes(data).first[o.index]);
        } else {
            for (uint8_t** m = meshes.first; m != Meshes(data).last; ++m) RenderWithTransparency(v, nullptr, vp, *m);
        }
        Virtual(v, 18, vp);
        ObserversPostRender(v, vp);
        if (texAnim) {
            orig::DeviceState_SetTextureStageState(Field<void*>(vp, kVpState), 0, kTransformFlags, stage0Was, 10);
            if (orig::Randy_t_GetTextureStageCount(Randy()) > 1)
                orig::DeviceState_SetTextureStageState(Field<void*>(vp, kVpState), 1, kTransformFlags, stage1Was, 10);
        }
        if (Field<float>(v, kFade) < t) orig::RRefFrame_t_SetTransparency(v, t);
    }
}

// FUN_1004cd31: the sub-meshes as they are, for the depth pass (nothing when fully transparent).
void __fastcall RasterizeDepth(Visual* v, void*, void* vp, uint8_t* data, bool lit)
{
    ScopedState fill(kFillMode, FillMode(), 10);
    if (Field<float>(v, kTransparency) == 0.0f) return;
    ScopedState lighting(kLighting, lit, 10);
    orig::RViewPort_t_RealizeRenderStates(vp);
    for (uint8_t** m = Meshes(data).first; m != Meshes(data).last; ++m) MeshRender(*m, nullptr, vp);
}

// FUN_1004cde6: the sub-meshes with the shadow's matrix and material, unlit, no z writes.
void __fastcall RasterizeShadow(Visual*, void*, void* vp, uint8_t* data, uint8_t* shadow)
{
    if (shadow[0x1EC]) Internal<void(__fastcall*)(void*)>(0x46788)(shadow);   // FUN_10046d5f: its matrix up to date
    orig::render_t_SetTransformMatrix(Render(), kWorld, shadow + 0x1AC);
    orig::RViewPort_t_SetMaterial(vp, Field<void*>(shadow, 0x178));
    ScopedState zWrite(0xE, 0, 10);
    ScopedState lighting(kLighting, 0, 10);
    for (uint8_t** m = Meshes(data).first; m != Meshes(data).last; ++m) MeshRender(*m, nullptr, vp);
    orig::RViewPort_t_ResetMaterial(vp);
}

// ---- RTriMesh_t ----

float WorldScale(Visual* v)                         // FUN_1002fd23
{
    if (v[kWorldDirty]) orig::RRefFrame_t_UpdateWorldMatrix(v);
    return Field<float>(v, kWorldScale);
}

// FUN_100457bd: can the current camera see this bounding volume (RCamera_t, FUN_1002b4a6).
bool VolumeSeen(Visual* v, void* volume)
{
    const float scale = WorldScale(v);
    void* camera = Global<void*>(kCurrentCamera);
    if (!camera) return true;                        // (the original would fault)
    return Internal<uint8_t(__fastcall*)(void*, void*, void*, const float*, float)>(0x2B4A6)(camera, nullptr, volume,
                                                                                            World(v), scale) != 0;
}

// FUN_1004941b: its 16-bit (5:6:5) vertex colours into the sub-meshes' vertex buffers.
void __fastcall CopyColours(Visual* v, void*, const uint16_t* colours)
{
    uint8_t* data = Field<uint8_t*>(v, kData);
    if (!data) return;
    for (uint32_t i = 0; i < Meshes(data).size(); ++i) {
        uint8_t* mesh = Meshes(data).first[i];
        const uint32_t count = MeshVertexCount(mesh);
        if (!count) continue;
        void* vb = MeshVertexBuffer(mesh);
        uint8_t* base = static_cast<uint8_t*>(orig::VertexBuffer_c_Lock(vb, 0, 0));
        void* c = base + orig::VertexBuffer_c_GetElementOffset(vb, 0x40);
        for (const uint16_t* end = colours + count; colours < end; ++colours) {
            const uint32_t x = *colours;
            *static_cast<uint32_t*>(c) = ((((x & 0x1F) + 0x1FE0) << 14) + (x & 0x7E0)) * 0x20 + ((x >> 8) & 0xF8);
            c = orig::VertexBuffer_c_NextVertex(vb, c);
        }
        orig::VertexBuffer_c_Unlock(vb);
    }
}

// typeid(*this) == typeid(RTriMesh_t): its own primary vtable (a subclass has its own). ConvertToLightmap's branch.
bool IsExactTriMesh(void* v) { return Field<void*>(v, 0) == reinterpret_cast<uint8_t*>(g_orig) + kTriVtable; }

// RTriMesh_t::ConvertToLightmap: a DisplaySystem subclass gets a private copy of its data and the colours written
// straight in; an RTriMesh_t keeps the colours for its Render and swaps in the shared lightmap copy of its data.
void __fastcall TriMeshConvertToLightmap(Visual* v, void*, const uint16_t* colours)
{
    if (!IsExactTriMesh(v)) {
        Field<void*>(v, kData) = meshdata::MakePrivate(Field<void*>(v, kData), false);
        CopyColours(v, nullptr, colours);
    } else {
        Field<const uint16_t*>(v, kColours) = colours;
        Field<void*>(v, kData) = meshdata::SharedCopy(Field<void*>(v, kData));
    }
}

void __fastcall TriMeshProcess(Visual* v)
{
    uint8_t* data = Field<uint8_t*>(v, kData);
    if (!data || (v[kIsOccluder] && !(DebuggerMode() & 2))) return;
    if (!v[kCullByVolume]) {
        v[kVisibleNow] = 1;
    } else if (void* volume = Field<void*>(data, kDataVolume)) {
        v[kVisibleNow] = VolumeSeen(v, volume);
    }
    if (!v[kRegistered]) {
        Internal<void(__fastcall*)(void*, void*, void*)>(0x3F78D)(orig::HMOccluder_t_Get(), nullptr, v);
        v[kRegistered] = 1;
        const char* name = orig::RResource_t_GetName(data);
        v[kIsOccluder] = std::strncmp(name, "[OCC]", 5) == 0;
        if (v[kIsOccluder] && !(DebuggerMode() & 2)) return;
    }
    // (Instancing of equal sub-meshes would follow here, behind a switch at 0x1017D4B1 that is never set.)
    orig::RVisual_t_Process(v);
}

void __fastcall TriMeshRender(Visual* v, void*, void* vp)
{
    if (!v[kVisibleNow]) return;
    if (const uint16_t* colours = Field<const uint16_t*>(v, kColours)) CopyColours(v, nullptr, colours);
    uint8_t* data = Field<uint8_t*>(v, kData);
    Field<uint32_t>(v, kLitBy) =
        orig::RVisual_t_CullLights(v, vp, Field<float>(Field<void*>(data, kDataVolume), 0x14), 0xFFFFFFFF);
    orig::render_t_SetTransformMatrix(Render(), kWorld, const_cast<float*>(World(v)));
    Rasterize(v, nullptr, vp, Field<uint8_t*>(v, kData), true, v[kSorted] != 0);
    if (!v[kKeepPriority])
        orig::RVisual_t_SetRenderPriority(v, (v[kTransparentFlag] || Field<float>(v, kTransparency) < 1.0f) ? 6 : 3);
}

// RTriMesh_t::RenderDepth: drawn when its nearest point (along the camera) is before `depth`.
void __fastcall TriMeshRenderDepth(Visual* v, void*, void* vp, float depth)
{
    if (!v[kVisibleNow]) return;
    const float* volume = &Field<float>(Field<void*>(Field<uint8_t*>(v, kData), kDataVolume), 0);
    const float radius = volume[5];
    float c[3];
    xm::Transform(volume + 2, World(v), c);
    void* camera = Field<void*>(vp, kVpCamera);
    const float* eye = World(camera);
    c[0] -= eye[12], c[1] -= eye[13], c[2] -= eye[14];
    const float* q = &Field<float>(camera, kRotation);   // its inverse (FUN_10049ea0)
    const float inv = float(1.0 / double(((q[3] * q[3] + q[2] * q[2]) + q[0] * q[0]) + q[1] * q[1]));
    const float qi[4] = {inv * -q[0], inv * -q[1], inv * -q[2], inv * q[3]};
    xm::RotateVector(c, qi);
    const double nearest = double(c[2]) - double(WorldScale(v)) * double(radius);
    if (!(double(depth) > nearest)) return;
    orig::render_t_SetTransformMatrix(Render(), kWorld, const_cast<float*>(World(v)));
    RasterizeDepth(v, nullptr, vp, Field<uint8_t*>(v, kData), true);
}

void __fastcall TriMeshRenderShadow(Visual* v, void*, void* vp, uint8_t* shadow)
{
    orig::RVisual_t_RenderShadow(v, vp, shadow);
    RasterizeShadow(v, nullptr, vp, Field<uint8_t*>(v, kData), shadow);
}

// ---- RTriMesh_t: being made, copied, archived, destroyed; its data; rays ----

void InitTriMeshPart(Visual* v)
{
    Field<uintptr_t>(v, 0) = reinterpret_cast<uintptr_t>(g_orig) + kTriVtable;
    Field<uintptr_t>(v, kSubjectPart) = reinterpret_cast<uintptr_t>(g_orig) + kTriSubjectVtable;
    Field<void*>(v, kColours) = nullptr;
    Field<void*>(v, kData) = nullptr;
    orig::Timer_Timer_57(v + kTimer);
}

void __fastcall SetTriMeshData(Visual* v, void*, uint8_t* data)
{
    if (data == Field<uint8_t*>(v, kData)) return;
    if (data) orig::RResource_t_AddRefRResource(data);
    if (void* old = Field<void*>(v, kData)) orig::RResource_t_ReleaseRResource(old);
    Field<uint8_t*>(v, kData) = data;
    if (data) Field<uint32_t>(v, kRestoredAt) = Field<uint32_t>(data, kDataRestored);
}

void PlaceFromData(Visual* v, uint8_t* data)        // its rotation (+0x38) and position (+0x2C) as the anim matrix
{
    xm::M4 m = xm::FromQuaternion(&Field<float>(data, 0x38));
    m.m[12] = Field<float>(data, 0x2C), m.m[13] = Field<float>(data, 0x30), m.m[14] = Field<float>(data, 0x34);
    orig::RRefFrame_t_SetAnimMatrix(v, m.m);
}

void __fastcall TriMeshInit(Visual* v, void*, uint8_t* data)
{
    SetTriMeshData(v, nullptr, data);
    uint8_t* d = Field<uint8_t*>(v, kData);
    if (!d) return;
    PlaceFromData(v, d);
    if (DataIsTransparent(d)) {
        orig::RVisual_t_SetRenderPriority(v, 6);
        v[kTransparentFlag] = 1;
    }
}

void* __fastcall TriMeshConstruct(Visual* v, void*, uint8_t* data, void* parent, void* animation)
{
    orig::RVisual_t_RVisual_t_49(v, parent, animation);
    InitTriMeshPart(v);
    v[kTransparentFlag] = 0;
    TriMeshInit(v, nullptr, data);
    Field<uint32_t>(v, 0x17C) = 0;
    Field<uint16_t>(v, 0x180) = 0;
    Field<uint32_t>(v, kSorted) = 0x1000000;         // sorted, registered, occluder: no; culled by its volume: yes
    return v;
}

void* __fastcall TriMeshCopy(Visual* v, void*, Visual* from)   // FUN_10049165
{
    orig::RVisual_t_RVisual_t_48(v, from);
    InitTriMeshPart(v);
    Field<void*>(v, kColours) = Field<void*>(from, kColours);
    v[kTransparentFlag] = 0;
    TriMeshInit(v, nullptr, Field<uint8_t*>(from, kData));
    std::memcpy(v + 0x17C, from + 0x17C, 6);
    v[kSorted] = from[kSorted];
    v[kRegistered] = v[kIsOccluder] = 0;
    v[kCullByVolume] = from[kCullByVolume];
    return v;
}

// FUN_10049e12: the archive's "data" object, when it is an RTriMeshData_t.
int32_t FindTriMeshData(void* stream, uint8_t** out)
{
    using DynamicCastFn = void*(__cdecl*)(void*, long, void*, void*, int);
    static const auto dynamicCast =
        reinterpret_cast<DynamicCastFn>(GetProcAddress(GetModuleHandleA("msvcr100.dll"), "__RTDynamicCast"));
    void* object = nullptr;
    int32_t error = serialize::Get().findObject(stream, nullptr, "data", &object, 0);
    if (error) return error;
    void* data = object ? dynamicCast(object, 0, &Global<uint8_t>(0xB60D4), &Global<uint8_t>(0xB6580), 0) : nullptr;
    if (object && !data) return 1;
    *out = static_cast<uint8_t*>(data);
    return 0;
}

void* __fastcall TriMeshConstructFrom(Visual* v, void*, void* archive)   // FUN_1004927a
{
    Internal<void*(__fastcall*)(void*, void*, void*)>(0x4D6D3)(v, nullptr, archive);   // RVisual_t's part
    InitTriMeshPart(v);
    void* stream = serialize::Get().getStream(archive, nullptr);
    Field<uint32_t>(v, 0x17C) = 0;
    v[0x180] = 0;
    v[kSorted] = 0;
    v[kTransparentFlag] = 0;
    uint8_t* data = nullptr;
    if (FindTriMeshData(stream, &data) == 0) Field<uint8_t*>(v, kData) = data;
    if (Field<uint8_t*>(v, kData) && data) PlaceFromData(v, data);
    v[kRegistered] = v[kIsOccluder] = 0;
    v[kCullByVolume] = 1;
    return v;
}

void __fastcall TriMeshDestroy(Visual* v)
{
    Field<uintptr_t>(v, 0) = reinterpret_cast<uintptr_t>(g_orig) + kTriVtable;
    Field<uintptr_t>(v, kSubjectPart) = reinterpret_cast<uintptr_t>(g_orig) + kTriSubjectVtable;
    if (void* d = Field<void*>(v, kData)) orig::RResource_t_ReleaseRResource(d);
    Internal<void(__fastcall*)(void*)>(0x4D7D3)(v);   // ~RVisual_t
}

void* __fastcall TriMeshDelete(Visual* v, void*, uint8_t flags)   // vtable slot 0 (FUN_10049eca)
{
    if (!(flags & 2)) {
        TriMeshDestroy(v);
        if (flags & 1) vc10::Free(v);
        return v;
    }
    uint8_t* block = v - 4;
    for (uint32_t i = *reinterpret_cast<uint32_t*>(block); i-- > 0;) TriMeshDestroy(v + i * kTriSize);
    if (flags & 1) vc10::FreeArray(block);
    return block;
}

void* __cdecl TriMeshInstantiate(void* archive)
{
    Visual* v = static_cast<Visual*>(TriMeshConstructFrom(static_cast<Visual*>(vc10::Allocate(kTriSize)), nullptr,
                                                          archive));
    if (Field<void*>(v, kData)) return v;
    Virtual(v, 0, uint32_t(1));
    return nullptr;
}

void* __fastcall TriMeshClone(Visual* v) { return TriMeshCopy(static_cast<Visual*>(vc10::Allocate(kTriSize)), nullptr, v); }

void __fastcall TriMeshArchive(Visual* v, void*, void* archive)
{
    orig::RVisual_t_Archive(v, archive);
    const serialize::Api& s = serialize::Get();
    s.addObject(s.getStream(archive, nullptr), nullptr, "data", Field<void*>(v, kData));
}

void* __fastcall TriMeshVolume(Visual* v) { return Field<void*>(Field<uint8_t*>(v, kData), kDataVolume); }

void __fastcall CloneMeshData(Visual* v)
{
    void* data = Field<void*>(v, kData);
    void* copy = Virtual<void*>(data, 3);
    orig::RResource_t_ReleaseRResource(data);
    Field<void*>(v, kData) = copy;
}

const char* __fastcall TriMeshName(Visual* v) { return orig::RResource_t_GetName(Field<void*>(v, kData)); }

void __fastcall DataRestore(uint8_t* data)          // FUN_1004ebf7: new hardware copies after a lost device
{
    const uint32_t count = Global<uint32_t>(0x17D334);
    if (Field<uint32_t>(data, kDataRestored) == count) return;
    for (uint8_t** m = Meshes(data).first; m != Meshes(data).last; ++m) RestoreHardware(Field<uint8_t*>(*m, kMeshData));
    Field<uint32_t>(data, kDataRestored) = Global<uint32_t>(0x17D334);
}

void __fastcall TriMeshRestoreData(Visual* v)
{
    orig::RVisual_t_RestoreData(v);
    if (uint8_t* d = Field<uint8_t*>(v, kData)) DataRestore(d);
}

// RTriMesh_t::IsRayIntersecting: the ray into the mesh's space, then each sub-mesh (the nearest hit when asked).
bool __fastcall TriMeshRay(Visual* v, void*, const float* from, const float* along, float* at, bool nearest)
{
    float inverse[16];
    std::memcpy(inverse, World(v), sizeof(inverse));
    Internal<void(__fastcall*)(float*)>(0x6E108)(inverse);
    float p0[3], p1[3], end[3], d[3];
    xm::Transform(from, inverse, p0);
    end[0] = along[0] + from[0], end[1] = along[1] + from[1], end[2] = along[2] + from[2];
    xm::Transform(end, inverse, p1);
    d[0] = p1[0] - p0[0], d[1] = p1[1] - p0[1], d[2] = p1[2] - p0[2];
    const float none = 9.99999968e37f;
    float best = none;
    auto& meshes = Meshes(Field<uint8_t*>(v, kData));
    if (meshes.first == meshes.last) return false;
    for (uint8_t** m = meshes.first; m != meshes.last; ++m) {
        if (!orig::SimpleMesh_IsRayIntersecting(*m, p0, d, at, nearest)) continue;
        if (!nearest) return true;
        if (best <= *at) *at = best;
        else best = *at;
    }
    return best < none;
}

// FUN_1004a64d: RTriMesh_t's SubjectImpl subobject (its +0xA4) deleting destructor, which adjusts to the mesh.
void* __fastcall TriMeshSubjectDelete(void* self, void*, uint8_t flags)
{
    return TriMeshDelete(static_cast<uint8_t*>(self) - 0xA4, nullptr, flags);
}

}  // namespace

void Install(HMODULE orig)
{
    if (GetMode("Scene", Mode::Off) != Mode::On) return;
    g_orig = orig;
    g_randy = reinterpret_cast<void* const*>(GetProcAddress(orig, "?s_pcRandy@Randy_t@@1PAV1@A"));
    const serialize::Api& s = serialize::Get();
    if (!g_randy || !s.complete || !s.findObject) {
        Log("meshes: serialize.dll / randy31 exports missing - not replaced");
        return;
    }
    struct Entry {
        uint32_t rva;
        void* target;
        const char* what;
    };
#define FN(f) reinterpret_cast<void*>(&f)
    const Entry entries[] = {
        {0x1237D, FN(GuardOpen), "render state guard (FUN_1001237d)"},
        {0x123C0, FN(GuardClose), "render state guard end (FUN_100123c0)"},
        {0x4E792, FN(MeshVertexBuffer), "SimpleMesh::GetVertexBuffer"},
        {0x4E7AB, FN(MeshFormat), "SimpleMesh::GetVertexBufferFormat"},
        {0x4E7F3, FN(MeshVertexCount), "SimpleMesh::GetVertexCount"},
        {0x4E7FA, FN(MeshTriangleCount), "SimpleMesh::GetTriangleCount"},
        {0x4872B, FN(RestoreHardware), "SimpleMesh data hardware copy (FUN_1004872b)"},
        {0x4E7A3, FN(MeshRestoreHardware), "SimpleMesh::RestoreHWBuffer"},
        {0x4E743, FN(MeshSetMaterial), "SimpleMesh::SetMaterial"},
        {0x4F898, FN(MeshVolume), "SimpleMesh::GetBoundingVolume"},
        {0x1FDBA, FN(DrawIndexed), "index range draw (FUN_1001fdba)"},
        {0x4F154, FN(MeshRender), "SimpleMesh::Render"},
        {0x4EBC4, FN(DataIsTransparent), "RVisualData_t::IsTransparent"},
        {0x4D076, FN(StatelPass), "RVisual_t statel light pass (FUN_1004d076)"},
        {0x4D18A, FN(EnvironmentPass), "RVisual_t environment map pass (FUN_1004d18a)"},
        {0x4D2D8, FN(RenderWithTransparency), "RVisual_t::RenderWithTransparency"},
        {0x4D84A, FN(Rasterize), "RVisual_t::Rasterize"},
        {0x4CD31, FN(RasterizeDepth), "RVisual_t depth pass (FUN_1004cd31)"},
        {0x4CDE6, FN(RasterizeShadow), "RVisual_t shadow pass (FUN_1004cde6)"},
        {0x49554, FN(TriMeshProcess), "RTriMesh_t::Process"},
        {0x49761, FN(TriMeshRender), "RTriMesh_t::Render"},
        {0x4941B, FN(CopyColours), "RTriMesh_t lightmap colours (FUN_1004941b)"},
        {0x4980F, FN(TriMeshConvertToLightmap), "RTriMesh_t::ConvertToLightmap"},
        {0x4934C, FN(TriMeshRenderDepth), "RTriMesh_t::RenderDepth"},
        {0x48FBE, FN(TriMeshRenderShadow), "RTriMesh_t::RenderShadow"},
        {0x49017, FN(SetTriMeshData), "RTriMesh_t::SetTriMeshData"},
        {0x4905C, FN(TriMeshInit), "RTriMesh_t::Init"},
        {0x490E3, FN(TriMeshConstruct), "RTriMesh_t::RTriMesh_t"},
        {0x49165, FN(TriMeshCopy), "RTriMesh_t::RTriMesh_t(copy) (FUN_10049165)"},
        {0x4927A, FN(TriMeshConstructFrom), "RTriMesh_t::RTriMesh_t(archive) (FUN_1004927a)"},
        {0x48F39, FN(TriMeshDestroy), "RTriMesh_t::~RTriMesh_t"},
        {0x49ECA, FN(TriMeshDelete), "RTriMesh_t deleting destructor (FUN_10049eca)"},
        {0x49502, FN(TriMeshInstantiate), "RTriMesh_t::Instantiate"},
        {0x49240, FN(TriMeshClone), "RTriMesh_t::Clone"},
        {0x48F85, FN(TriMeshArchive), "RTriMesh_t::Archive"},
        {0x48FB4, FN(TriMeshVolume), "RTriMesh_t::GetBoundingVolume"},
        {0x48FEF, FN(CloneMeshData), "RTriMesh_t::CloneMeshData"},
        {0x4900C, FN(TriMeshName), "RTriMesh_t::GetName"},
        {0x4EBF7, FN(DataRestore), "RVisualData_t restore (FUN_1004ebf7)"},
        {0x490CA, FN(TriMeshRestoreData), "RTriMesh_t::RestoreData"},
        {0x49867, FN(TriMeshRay), "RTriMesh_t::IsRayIntersecting"},
        {0x4A64D, FN(TriMeshSubjectDelete), "RTriMesh_t subject deleting destructor (FUN_1004a64d)"},
    };
#undef FN
    int installed = 0;
    for (const Entry& e : entries) installed += Replace(orig, e.rva, e.target, e.what) ? 1 : 0;
    Log("meshes: %d of %d functions native", installed, int(sizeof(entries) / sizeof(entries[0])));
}

}  // namespace rnative::mesh
