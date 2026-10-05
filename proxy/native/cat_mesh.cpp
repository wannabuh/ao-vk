// A character's per-frame upkeep (RCATMesh_t), native: randy-vk.ini [Native] CatMesh=on.
//   Process (RVisual_t vtable slot 8, FUN_1005798b): visibility (a sphere around the animated pose), attachments.
//   UpdateBones (FUN_10055d52) / UpdateSkin (FUN_10055c1c): bones and skinning redone only when the animation's
//   version changed (FUN_10055a23: the CATRender_t's change counter + the animation's own).
// The bone hierarchy and the skinning loop themselves are their own replacements (Anim, Skin).
#include "native/cat_mesh.h"
#include "native/cat.h"
#include "native/orig_api.gen.h"

namespace rnative::catmesh {

namespace {

HMODULE g_orig;

template <typename F>
F Internal(uint32_t rva) { return reinterpret_cast<F>(AddressFor(rva)); }

constexpr uint32_t kBoneState = 0x218;            // < 0: both need redoing; > 0: bones are set from outside
constexpr uint32_t kBonesVersion = 0x1C0, kSkinVersion = 0x1C4;
constexpr uint32_t kBoxMin = 0x1FC, kBoxMax = 0x208;

void* Anim(uint8_t* mesh) { return At<void*>(mesh, cat::kRenderAnim); }

template <typename R>
R AnimCall(void* anim, uint32_t slot)
{
    return reinterpret_cast<R(__fastcall*)(void*)>((*static_cast<void***>(anim))[slot / 4])(anim);
}

int32_t __fastcall Version(uint8_t* mesh)          // FUN_10055a23
{
    void* anim = Anim(mesh);
    return At<int32_t>(mesh, 0x24) + (anim ? AnimCall<int32_t>(anim, 0x20) : 0);
}

void RootBones(uint8_t* mesh)                       // FUN_10054d16: the hierarchy from the root bone
{
    void* anim = Anim(mesh);
    if (!anim || !AnimCall<bool>(anim, 0x28))
        return;
    float identity[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    Internal<void(__fastcall*)(void*, void*, float*, int32_t, float)>(0x540A5)(mesh, nullptr, identity, 0, 1.0f);
}

void __fastcall UpdateSkin(uint8_t* mesh);

void __fastcall UpdateBones(uint8_t* mesh)          // FUN_10055d52
{
    const int32_t state = At<int32_t>(mesh, kBoneState);
    if (state < 1) {
        int32_t v = Version(mesh);
        if (v != At<int32_t>(mesh, kBonesVersion)) {
            RootBones(mesh);
            At<int32_t>(mesh, kBonesVersion) = Version(mesh);
        }
    }
    if (state < 0) {
        At<int32_t>(mesh, kBoneState) = 0;
        UpdateSkin(mesh);
    }
}

void __fastcall UpdateSkin(uint8_t* mesh)           // FUN_10055c1c
{
    int32_t state = At<int32_t>(mesh, kBoneState);
    if (state < 0) {
        At<int32_t>(mesh, kBoneState) = 0;
        UpdateBones(mesh);
        state = At<int32_t>(mesh, kBoneState);
    }
    if (state == 0 && Version(mesh) != At<int32_t>(mesh, kSkinVersion)) {
        Internal<void(__fastcall*)(void*, void*, float*, float*)>(0x5470D)(mesh, nullptr, &At<float>(mesh, kBoxMin),
                                                                          &At<float>(mesh, kBoxMax));
        At<int32_t>(mesh, kSkinVersion) = Version(mesh);
    }
}

// RVisual_t vtable slot 8 (Process), on the RVisual_t part (RCATMesh_t +0x3C).
void __fastcall Process(uint8_t* visual)
{
    uint8_t* mesh = visual - 0x3C;
    if (visual[0x1E4]) {                            // drawn last frame: a countdown restarts when it runs out
        int32_t& countdown = At<int32_t>(visual, 0x1DC);
        if (--countdown < 0) countdown = At<int32_t>(visual, 0x1E0);
    }
    orig::RVisual_t_Process(visual);
    if (At<void*>(mesh, cat::kRenderMesh) && Anim(mesh)) {
        const float* world = static_cast<const float*>(orig::RRefFrame_t_GetWorldMatrix(visual));
        float animRadius = AnimCall<float>(Anim(mesh), 0x1C);
        float frameScale = Internal<float(__fastcall*)(void*)>(0x2FD23)(visual);
        float radius = (animRadius + At<float>(visual, 0x190)) * At<float>(visual, 0x194);
        visual[0x18C] = orig::RRefFrame_t_CullBySphere_145(visual, world + 12, frameScale * radius) ? 1 : 0;
    }
    visual[0x1E4] = visual[0x18C];
    if (visual[0x18C])
        orig::RCATMesh_t_ProcessAttractorChilds(mesh, 1, At<float>(visual, 0x3F8));
}

}  // namespace

void Install(HMODULE orig)
{
    if (GetMode("CatMesh", Mode::Off) != Mode::On)
        return;
    g_orig = orig;
    static const uint8_t kBones[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x40};
    static const uint8_t kSkin[] = {0x56, 0x57, 0x8B, 0xF9, 0x83, 0xBF, 0x18, 0x02, 0x00, 0x00, 0x00};
    static const uint8_t kVersion[] = {0x56, 0x8B, 0xF1, 0x33, 0xC9};
    bool ok = HookEntry(orig, 0x55D52, kBones, sizeof(kBones), reinterpret_cast<void*>(&UpdateBones),
                        "RCATMesh_t bones (FUN_10055d52)") &&
              HookEntry(orig, 0x55C1C, kSkin, sizeof(kSkin), reinterpret_cast<void*>(&UpdateSkin),
                        "RCATMesh_t skinning (FUN_10055c1c)") &&
              HookEntry(orig, 0x55A23, kVersion, sizeof(kVersion), reinterpret_cast<void*>(&Version),
                        "RCATMesh_t animation version (FUN_10055a23)") &&
              HookSlot(orig, 0x95EDC, 8, 0x5798B, reinterpret_cast<void*>(&Process), "RCATMesh_t::Process");
    Log("character upkeep: %s", ok ? "on" : "partly installed (unknown client build)");
}

}  // namespace rnative::catmesh
