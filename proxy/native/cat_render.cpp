// Drawing a character (RCATMesh_t's RVisual_t::Render, vtable slot 13; the original FUN_10056ed6 with FUN_10055dba):
// randy-vk.ini [Native] CatRender=on. Calls the same functions in the same order as the original (the call log of a
// frame is identical), only native. docs/native.md.
#include "native/cat_render.h"
#include "native/cat.h"
#include "native/orig_api.gen.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace rnative::catrender {

namespace {

HMODULE g_orig;
void* const* g_randy;                             // Randy_t::s_pcRandy (data export): -> the Randy_t
void* const* g_renderInstance;                    // render_t::m_pcInstance: -> the render_t
uint32_t* g_debuggerMode;                         // Debugger_t::m_nDebuggerMode
float* g_sfxPhase;                                // 0x101E238C: the pulsing effect's phase

template <typename F>
F Internal(uint32_t rva) { return reinterpret_cast<F>(reinterpret_cast<uint8_t*>(g_orig) + rva); }

// The visual's members (RVisual_t at RCATMesh_t +0x3C; offsets from there).
constexpr uint32_t kInView = 0x18C, kDrawn = 0x1E4, kTransparentLast = 0x1D8;
constexpr uint32_t kAlpha = 0x88, kColorA = 0x8C, kColorB = 0x90, kMaterialHook = 0xB0, kObservers = 0xAC;
constexpr uint32_t kSfxType = 0x180, kSfxSpeed = 0x17C;
constexpr uint32_t kEnvBlob = 0x1E8, kCatLightBlob = 0x2F0, kSfxBlob = 0x374;
constexpr uint32_t kMaterialTransparent = 0xBD;   // RMaterial_t: drawn in the second (transparent) pass

void* Devicestate()                                // the Randy_t's DeviceState
{
    return At<void*>(*g_randy, 0x27C);
}

void* Renderer()
{
    return *g_renderInstance;
}

// FUN_1001237d / FUN_100123c0: a render state set for a while (the old one back afterwards).
struct ScopedState {
    uint32_t state, old;
    bool set;
    ScopedState(uint32_t s, uint32_t value, int32_t priority) : state(s)
    {
        void* ds = Devicestate();
        old = At<uint32_t>(ds, 0x4C8 + s * 4);
        set = orig::DeviceState_SetRenderState(ds, int32_t(s), value, priority);
    }
    ~ScopedState()
    {
        if (set) orig::DeviceState_SetRenderState(Devicestate(), int32_t(state), old, 2);
    }
};

// FUN_1002be44 / FUN_1002be97: the same for a texture stage state.
struct ScopedStageState {
    uint32_t stage, type, old;
    bool set;
    ScopedStageState(uint32_t st, uint32_t t, uint32_t value, int32_t priority) : stage(st), type(t)
    {
        void* ds = Devicestate();
        old = At<uint32_t>(ds, 0xD84 + (st * 0x19 + t) * 4);
        set = orig::DeviceState_SetTextureStageState(ds, st, int32_t(t), value, priority);
    }
    ~ScopedStageState()
    {
        if (set) orig::DeviceState_SetTextureStageState(Devicestate(), stage, int32_t(type), old, 2);
    }
};

// RVisual_t's observers (a VS2010 std::set at +0xAC): FUN_10013e2e (before drawing: any true skips it),
// FUN_10013e71 (a material's D3D material: all true = they set it), FUN_10013ebb (after a material),
// FUN_10013f00 (after drawing).
struct Node {
    Node* left;
    Node* parent;
    Node* right;
    void* value;
    char color, isNil;
};

Node* NextNode(Node* n)                            // FUN_1005b04f
{
    if (n->isNil) return n;
    if (!n->right->isNil) {
        n = n->right;
        while (!n->left->isNil) n = n->left;
        return n;
    }
    Node* p = n->parent;
    while (!p->isNil && n == p->right) {
        n = p;
        p = p->parent;
    }
    return p;
}

template <typename Fn>
void ForEachObserver(void* visual, Fn&& fn)
{
    Node* head = At<Node*>(visual, kObservers);
    for (Node* n = head->left; n != head; n = NextNode(n))
        fn(n->value, *static_cast<void***>(n->value));
}

bool ObserversSkipImpl(void* visual, void* viewport)
{
    bool skip = false;
    ForEachObserver(visual, [&](void* o, void** vt) {
        skip |= reinterpret_cast<bool(__fastcall*)(void*, void*, void*, void*)>(vt[0])(o, nullptr, visual, viewport);
    });
    return skip;
}

bool ObserversMaterialImpl(void* visual, void* material, void* viewport, void* d3dMaterial)
{
    bool all = true;
    ForEachObserver(visual, [&](void* o, void** vt) {
        all &= reinterpret_cast<bool(__fastcall*)(void*, void*, void*, void*, void*, void*)>(vt[1])(
            o, nullptr, visual, material, viewport, d3dMaterial);
    });
    return all;
}

void ObserversAfterMaterialImpl(void* visual, void* material, void* viewport)
{
    ForEachObserver(visual, [&](void* o, void** vt) {
        reinterpret_cast<void(__fastcall*)(void*, void*, void*, void*, void*, void*)>(vt[2])(
            o, nullptr, visual, material, viewport, At<void*>(viewport, 8));
    });
}

void ObserversAfterImpl(void* visual, void* viewport)
{
    ForEachObserver(visual, [&](void* o, void** vt) {
        reinterpret_cast<void(__fastcall*)(void*, void*, void*, void*)>(vt[3])(o, nullptr, visual, viewport);
    });
}

void VirtualViewport(void* visual, uint32_t slot, void* viewport)   // StoreStateChanges / Apply / Restore
{
    void** vt = *static_cast<void***>(visual);
    reinterpret_cast<void(__fastcall*)(void*, void*, void*)>(vt[slot / 4])(visual, nullptr, viewport);
}

void SetViewportTexture(void* viewport, void* texture, uint32_t stage)   // FUN_10048d4f
{
    void* surface = texture ? At<void*>(texture, 0x30) : nullptr;
    orig::DeviceState_SetTexture(At<void*>(viewport, 8), surface, stage, 10);
}

float FrameScale(void* frame)                       // FUN_1002fd23: the frame's scale (its world matrix up to date)
{
    return Internal<float(__fastcall*)(void*)>(0x2FD23)(frame);
}

// FUN_10055dba: world matrix, bones, skinning; the bounding radius; whether lighting and normalising apply.
bool Prepare(uint8_t* mesh, void* viewport, float* radius, uint8_t* lighting, uint8_t* normalize)
{
    (void)viewport;
    if (!At<void*>(mesh, cat::kRenderMesh) || !At<void*>(mesh, cat::kRenderAnim))
        return false;
    void* frame = mesh + 0x3C;
    orig::render_t_SetTransformMatrix(Renderer(), 1, const_cast<void*>(static_cast<const void*>(
                                                         orig::RRefFrame_t_GetWorldMatrix(frame))));
    *lighting = 1;
    const float localScale = At<float>(mesh, 0x78);
    bool off = localScale < 0.95f || 1.05f < localScale;
    if (!off) {
        float s = FrameScale(frame);
        off = s < 0.95f;
        if (!off) {
            s = FrameScale(frame);
            off = (s < 1.05f) == (s == 1.05f);     // the original's test: over 1.05, or not comparable
        }
    }
    *normalize = off ? 1 : 0;
    Internal<void(__fastcall*)(void*)>(0x55D52)(mesh);   // bones
    Internal<void(__fastcall*)(void*)>(0x55C1C)(mesh);   // skinning
    void* anim = At<void*>(mesh, cat::kRenderAnim);
    float animRadius = reinterpret_cast<float(__fastcall*)(void*)>((*static_cast<void***>(anim))[0x1C / 4])(anim);
    *radius = (animRadius + At<float>(mesh, 0x1CC)) * At<float>(mesh, 0x1D0);
    return true;
}

// Each piece of each group: (group, piece, vertex buffer).
template <typename Fn>
void ForEachPiece(uint8_t* mesh, Fn&& fn)
{
    const int32_t groupCount = At<int32_t>(mesh, cat::kRenderGroupCount);
    auto* groups = At<cat::RenderGroup*>(mesh, cat::kRenderGroups);
    for (int32_t g = 0; g < groupCount; ++g) {
        uint8_t* meshGroup = At<uint8_t*>(groups[g].mesh, cat::kMeshGroups) + g * cat::kGroupSize;
        uint8_t* piece = At<uint8_t*>(meshGroup, cat::kGroupPieces);
        for (int32_t p = 0; p < At<int32_t>(meshGroup, cat::kGroupPieceCount); ++p, piece += cat::kPieceSize)
            fn(piece, groups[g].slots[p].buffer);
    }
}

void DrawPiece(uint8_t* piece, void* buffer)
{
    if (At<int32_t>(piece, cat::kPieceActiveTris) > 0 && At<uint32_t>(piece, cat::kPieceTriCount) != 0)
        orig::render_t_RenderTriangleList_408(Renderer(), buffer, 0, At<uint32_t>(piece, cat::kPieceVertexCount),
                                              At<void*>(piece, cat::kPieceIndices),
                                              At<uint32_t>(piece, cat::kPieceTriCount) * 3, 0);
}

void* MaterialOf(uint8_t* mesh, uint8_t* piece)
{
    const char* name = orig::RResource_t_GetName(At<void*>(piece, 0));
    return orig::RCATMesh_t_GetSubstMaterial(mesh, orig::RCATMesh_t_GetMaterialIndex(mesh, name));
}

void __fastcall Render(uint8_t* visual, void*, void* viewport)
{
    uint8_t* mesh = visual - 0x3C;
    visual[kDrawn] = visual[kInView];
    if (!visual[kInView])
        return;
    uint8_t transparent = visual[kTransparentLast];
    float radius = 0.0f;
    uint8_t lighting = 0, normalize = 0;
    if (Prepare(mesh, viewport, &radius, &lighting, &normalize)) {
        ScopedState fill(8, (~(*g_debuggerMode >> 7) & 1) | 2, 10);        // D3DRENDERSTATE_FILLMODE
        ScopedState light(0x88, lighting, 10);                                // LIGHTING
        ScopedState norm(0x8F, normalize, 10);                                // NORMALIZENORMALS
        orig::RVisual_t_CullLights(visual, viewport, radius, 0xFFFFFFFFu);
        if (ObserversSkipImpl(visual, viewport))
            return;                                                          // (no render priority change)
        VirtualViewport(visual, 0x40, viewport);                             // StoreStateChanges
        {
            ScopedStageState magFilter(0, 0x10, 2, 10), minFilter(0, 0x11, 2, 10), mipFilter(0, 0x12, 2, 10);   // linear filtering
            // Colour overrides (+0x90 specular, +0x8C emissive, +0x88 alpha < 1): one D3D material for the pieces.
            float d3dMaterial[17] = {};                                      // D3DMATERIAL7
            const float* colorB = At<const float*>(visual, kColorB);
            const float* colorA = At<const float*>(visual, kColorA);
            const float alpha = At<float>(visual, kAlpha);
            bool overrides = false;
            if (colorB || colorA || alpha < 1.0f) {
                overrides = true;
                if (colorB) d3dMaterial[8] = colorB[0], d3dMaterial[9] = colorB[1], d3dMaterial[10] = colorB[2],
                            d3dMaterial[11] = alpha;
                if (colorA) d3dMaterial[12] = colorA[0], d3dMaterial[13] = colorA[1], d3dMaterial[14] = colorA[2],
                            d3dMaterial[15] = alpha;
                transparent = alpha < 1.0f ? 1 : 0;
                d3dMaterial[3] = alpha;
            } else {
                transparent = 0;
            }
            for (int pass = 0; pass < 2; ++pass)
                ForEachPiece(mesh, [&](uint8_t* piece, void* buffer) {
                    void* material = MaterialOf(mesh, piece);
                    if (!material || static_cast<uint8_t*>(material)[kMaterialTransparent] != (pass != 0 ? 1 : 0))
                        return;
                    VirtualViewport(visual, 0x44, viewport);                 // ApplyStateChanges
                    orig::RViewPort_t_SetMaterial(viewport, material);
                    if (At<int32_t>(visual, kMaterialHook) != 0) {
                        orig::RMaterial_t_InitD3DMaterial(material, d3dMaterial);
                        if (!ObserversMaterialImpl(visual, material, viewport, d3dMaterial))
                            orig::RViewPort_t_SetD3DMaterial(viewport, d3dMaterial);
                        ObserversAfterMaterialImpl(visual, material, viewport);
                    }
                    if (!overrides) {
                        orig::RViewPort_t_RealizeRenderStates(viewport);
                        transparent = (transparent || static_cast<uint8_t*>(material)[kMaterialTransparent]) ? 1 : 0;
                    } else {
                        ScopedState blend(0x1B, 1, 10), diffuseSource(0x92, 1, 10), ambientSource(0x94, 1, 10);
                        orig::RViewPort_t_SetD3DMaterial(viewport, d3dMaterial);
                        orig::RViewPort_t_RealizeRenderStates(viewport);
                    }
                    if (At<int32_t>(piece, cat::kPieceActiveTris) > 0 && At<uint32_t>(piece, cat::kPieceTriCount) != 0) {
                        DrawPiece(piece, buffer);
                        void* env = orig::RMaterial_t_GetEnvTexture(material);
                        if (env && orig::StateBlob_c_Validate(visual + kEnvBlob)) {   // the environment map, added
                            orig::StateBlob_c_Set(visual + kEnvBlob);
                            float m[16] = {0.5f, 0, 0, 0, 0, 0.5f, 0, 0, 0, 0, 1.0f, 0, 0.5f, 0.5f, 0.0f, 1.0f};
                            orig::render_t_SetTransformMatrix(Renderer(), 0x10, m);  // D3DTRANSFORMSTATE_TEXTURE0
                            SetViewportTexture(viewport, env, 0);
                            orig::RViewPort_t_RealizeRenderStates(viewport);
                            DrawPiece(piece, buffer);
                            orig::StateBlob_c_Reset(visual + kEnvBlob);
                        }
                    }
                    orig::RViewPort_t_ResetMaterial(viewport);
                    VirtualViewport(visual, 0x48, viewport);                 // RestoreStateChanges
                });
            // The special light (SfxType 1): the character once more with its texture lit by it, ambient by strength.
            if (At<int32_t>(visual, kSfxType) == 1 && orig::RandyShadowlandsData_s_IsCATLightUsed() &&
                0.01f < orig::RandyShadowlandsData_s_GetCATLightIntensity() &&
                orig::StateBlob_c_Validate(visual + kCatLightBlob)) {
                orig::RVisual_t_CullLights(visual, viewport, radius, 0);
                float strength = At<float>(visual, kAlpha);
                uint32_t v = uint32_t(int32_t(double(orig::RandyShadowlandsData_s_GetCATLightIntensity() * strength) * 255.0));
                orig::StateBlob_c_SetRenderState(visual + kCatLightBlob, 0x8B, v | ((v << 8 | v) << 8));   // AMBIENT
                orig::StateBlob_c_Set(visual + kCatLightBlob);
                float m[16];
                std::memcpy(m, orig::RandyShadowlandsData_s_GetCATLightMatrix(), sizeof(m));
                orig::RViewPort_t_RealizeRenderStates(viewport);
                orig::render_t_SetTransformMatrix(Renderer(), 0x10, m);
                void* ds = At<void*>(viewport, 8);
                void*& saved = *Internal<void**>(0x1E2390);                  // the stage's texture, put back after
                saved = At<void*>(ds, 0x1128);
                void* lightSurface = At<void*>(orig::RandyShadowlandsData_s_GetCATLightTexture(), 0x30);
                orig::surface_t_AddRefDXSurface(lightSurface);
                orig::DeviceState_SetTexture(ds, lightSurface, 0, 10);
                orig::DeviceState_UpdateDevice(ds);
                ForEachPiece(mesh, [&](uint8_t* piece, void* buffer) { DrawPiece(piece, buffer); });
                orig::DeviceState_SetTexture(ds, saved, 0, 10);
                orig::DeviceState_UpdateDevice(ds);
                orig::surface_t_ReleaseDXSurface(lightSurface);
                orig::StateBlob_c_Reset(visual + kCatLightBlob);
            }
            // A pulsing glow (SfxType 2): each piece again with its texture, texture factor by |sin(phase)|.
            if (At<int32_t>(visual, kSfxType) == 2 && orig::StateBlob_c_Validate(visual + kSfxBlob)) {
                *g_sfxPhase = At<float>(visual, kSfxSpeed) + *g_sfxPhase;
                float s = std::fabs(float(std::sin(double(*g_sfxPhase))));
                uint32_t v = uint32_t(int32_t(double(s) * 255.0));
                ScopedState factor(0x3C, (v << 8 | v) << 8 | v, 10);           // TEXTUREFACTOR
                orig::StateBlob_c_Set(visual + kSfxBlob);
                orig::RViewPort_t_RealizeRenderStates(viewport);
                ForEachPiece(mesh, [&](uint8_t* piece, void* buffer) {
                    void* material = MaterialOf(mesh, piece);
                    if (!material || At<int32_t>(material, 0x70) == 0)
                        std::printf("missing material\n");
                    else
                        SetViewportTexture(viewport, orig::RMaterial_t_GetTexture(material, 0), 0);
                    orig::RViewPort_t_RealizeRenderStates(viewport);
                    DrawPiece(piece, buffer);
                });
                orig::StateBlob_c_Reset(visual + kSfxBlob);
            }
            VirtualViewport(visual, 0x48, viewport);                         // RestoreStateChanges
            ObserversAfterImpl(visual, viewport);
        }
    }
    orig::RVisual_t_SetRenderPriority(visual, transparent ? 6 : 3);
}

// RVisual_t vtable slot 20 (FUN_1005604b): the character flattened by an RShadow (its matrix, its material), after
// the children; every piece's active triangles from what the skinning last wrote.
void __fastcall RenderShadow(uint8_t* visual, void*, void* viewport, uint8_t* shadow)
{
    orig::RVisual_t_RenderShadow(visual, viewport, shadow);
    uint8_t* mesh = visual - 0x3C;
    if (!At<void*>(mesh, cat::kRenderMesh) || !At<void*>(mesh, cat::kRenderAnim))
        return;
    orig::RVisual_t_CullLights(visual, viewport, 0.0f, 0xFFFFFFFFu);
    void* matrix = Internal<void*(__fastcall*)(void*)>(0x46D5F)(shadow);    // RShadow's matrix, brought up to date
    orig::render_t_SetTransformMatrix(Renderer(), 1, matrix);               // D3DTRANSFORMSTATE_WORLD
    orig::RViewPort_t_SetMaterial(viewport, At<void*>(shadow, 0x178));
    orig::RViewPort_t_RealizeRenderStates(viewport);
    Internal<void(__fastcall*)(void*)>(0x55D52)(mesh);                      // bones
    Internal<void(__fastcall*)(void*)>(0x55C1C)(mesh);                      // skinning
    const int32_t groupCount = At<int32_t>(mesh, cat::kRenderGroupCount);
    auto* groups = At<cat::RenderGroup*>(mesh, cat::kRenderGroups);
    for (int32_t g = 0; g < groupCount; ++g) {
        uint8_t* meshGroup = At<uint8_t*>(groups[g].mesh, cat::kMeshGroups) + g * cat::kGroupSize;
        uint8_t* piece = At<uint8_t*>(meshGroup, cat::kGroupPieces);
        for (int32_t p = 0; p < At<int32_t>(meshGroup, cat::kGroupPieceCount); ++p, piece += cat::kPieceSize) {
            const int32_t active = At<int32_t>(piece, cat::kPieceActiveTris);
            if (active > 0)
                orig::render_t_RenderTriangleList_408(Renderer(), groups[g].slots[p].buffer, 0,
                                                      groups[g].slots[p].vertexCount, At<void*>(piece, cat::kPieceIndices),
                                                      uint32_t(active), 8);
        }
    }
}

}  // namespace

bool ObserversSkip(void* visual, void* viewport) { return ObserversSkipImpl(visual, viewport); }
bool ObserversMaterial(void* visual, void* material, void* viewport, void* d3dMaterial)
{
    return ObserversMaterialImpl(visual, material, viewport, d3dMaterial);
}
void ObserversAfterMaterial(void* visual, void* material, void* viewport)
{
    ObserversAfterMaterialImpl(visual, material, viewport);
}
void ObserversAfter(void* visual, void* viewport) { ObserversAfterImpl(visual, viewport); }

void Install(HMODULE orig)
{
    if (GetMode("CatRender", Mode::Off) != Mode::On)
        return;
    g_orig = orig;
    g_randy = reinterpret_cast<void* const*>(GetProcAddress(orig, "?s_pcRandy@Randy_t@@1PAV1@A"));
    g_renderInstance = reinterpret_cast<void* const*>(GetProcAddress(orig, "?m_pcInstance@render_t@@0PAV1@A"));
    g_debuggerMode = reinterpret_cast<uint32_t*>(GetProcAddress(orig, "?m_nDebuggerMode@Debugger_t@@2IA"));
    g_sfxPhase = Internal<float*>(0x1E238C);
    if (!g_randy || !g_renderInstance || !g_debuggerMode) {
        Log("character drawing: exports missing - not replaced");
        return;
    }
    // RCATMesh_t's RVisual_t vtable (0x10095EDC), slot 13 = FUN_10056ed6, slot 20 = FUN_1005604b.
    if (HookSlot(orig, 0x95EDC, 13, 0x56ED6, reinterpret_cast<void*>(&Render), "RCATMesh_t::Render") &&
        HookSlot(orig, 0x95EDC, 20, 0x5604B, reinterpret_cast<void*>(&RenderShadow), "RCATMesh_t::RenderShadow"))
        Log("character drawing: on");
}

}  // namespace rnative::catrender
