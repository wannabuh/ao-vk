// What the game asks a character (RCATMesh_t / CATRender_t), native: randy-vk.ini [Native] CatQuery=on.
//   Attractors (named points on the mesh where weapons, effects and other things attach): HasAttractor,
//   GetAttractor (FUN_10054df1), the things attached to them (ProcessAttractorChilds).
//   Bones by name (GetBoneMatrix, FUN_10054f4f), materials by name and their per-character substitutes, the
//   bounding sphere, the effect type.
#include "native/cat_query.h"
#include "native/cat.h"
#include "native/orig_api.gen.h"
#include "native/vc10.h"
#include "native/xmath.h"

#include <cmath>
#include <cstring>

namespace rnative::catquery {

namespace {

HMODULE g_orig;
float g_noRadius;                                   // _DAT_10095e5c: a mesh without a bounding sphere

template <typename F>
F Internal(uint32_t rva) { return reinterpret_cast<F>(AddressFor(rva)); }

constexpr uint32_t kBoneState = 0x218;
constexpr uint32_t kSubstMaterials = 0x1D8;         // std::vector<RMaterial_t*>
constexpr uint32_t kSfxType = 0x1BC;
constexpr uint32_t kAttached = 0x1E8;               // std::map<std::string, std::vector<RRefFrame_t*>>
constexpr uint32_t kVisual = 0x3C;

// CATMesh_t.
constexpr uint32_t kMeshMaterialCount = 0x38, kMeshMaterials = 0x3C;
constexpr uint32_t kMeshBoneNameCount = 0x40, kMeshBoneNames = 0x44;   // 0x28 bytes each, the name first
constexpr uint32_t kMeshSphere = 0x5C;              // center +8, radius +0x14

// An attractor of a mesh group (0x40 bytes; the group's +0x2C count, +0x30 array).
struct Attractor {
    vc10::String name;
    float offset[3];
    float rotation[4];                              // quaternion
    float scale;
    int32_t bone;
};
static_assert(sizeof(Attractor) == 0x40, "attractor");
constexpr uint32_t kGroupAttractorCount = 0x2C, kGroupAttractors = 0x30;

void* Mesh(const void* render) { return At<void*>(render, cat::kRenderMesh); }

void UpdateBones(void* mesh) { Internal<void(__fastcall*)(void*, void*)>(0x55D52)(mesh, nullptr); }

float FrameScale(void* frame) { return Internal<float(__fastcall*)(void*)>(0x2FD23)(frame); }   // FUN_1002fd23

const Attractor* FindAttractor(const void* render, const char* name)
{
    if (!Mesh(render)) return nullptr;
    const int32_t groups = At<int32_t>(render, cat::kRenderGroupCount);
    const auto* group = At<cat::RenderGroup*>(render, cat::kRenderGroups);
    for (int32_t g = 0; g < groups; ++g) {
        const uint8_t* meshGroup = At<uint8_t*>(group[g].mesh, cat::kMeshGroups) + g * cat::kGroupSize;
        const int32_t count = At<int32_t>(meshGroup, kGroupAttractorCount);
        const auto* attractors = At<Attractor*>(meshGroup, kGroupAttractors);
        for (int32_t i = 0; i < count; ++i)
            if (std::strcmp(attractors[i].name.c_str(), name) == 0) return &attractors[i];
    }
    return nullptr;
}

// CATRender_t::HasAttractor (0x54D4F).
bool __fastcall HasAttractor(void* render, void*, const char* name)
{
    return FindAttractor(render, name) != nullptr;
}

}  // namespace

bool AttractorMatrix(const void* render, const char* name, float out[16])
{
    const Attractor* a = FindAttractor(render, name);
    if (!a) return false;
    xm::M4 m = xm::FromQuaternion(a->rotation);
    xm::ScaleColumns(m, a->scale, a->scale, a->scale);
    for (int i = 0; i < 3; ++i) m.m[12 + i] += a->offset[i];
    if (!At<void*>(render, cat::kRenderAnim)) {
        std::memcpy(out, m.m, sizeof(m.m));
        return false;
    }
    const xm::M4 bone = xm::From43(At<float*>(render, cat::kRenderBones) + a->bone * 12);
    float q[4];
    xm::ToQuaternion(bone, q);
    xm::M4 rigid = xm::FromQuaternion(q);
    for (int i = 0; i < 3; ++i) rigid.m[12 + i] += bone.m[12 + i];
    const xm::M4 r = xm::Mul(m, rigid);
    std::memcpy(out, r.m, sizeof(r.m));
    return true;
}

bool BoneMatrix(const void* render, const char* name, float out[16])
{
    const void* mesh = Mesh(render);
    if (!mesh || !At<void*>(render, cat::kRenderAnim)) return false;
    const int32_t count = At<int32_t>(mesh, kMeshBoneNameCount);
    const uint8_t* names = At<uint8_t*>(mesh, kMeshBoneNames);
    for (int32_t i = 0; i < count; ++i) {
        if (std::strcmp(reinterpret_cast<const vc10::String*>(names + i * 0x28)->c_str(), name) == 0) {
            const xm::M4 m = xm::From43(At<float*>(render, cat::kRenderBones) + i * 12);
            std::memcpy(out, m.m, sizeof(m.m));
            return true;
        }
    }
    return false;
}

namespace {

bool __fastcall AttractorMatrixHook(void* render, void*, const char* name, float* out)
{
    return AttractorMatrix(render, name, out);
}

bool __fastcall BoneMatrixHook(void* render, void*, const char* name, float* out)
{
    return BoneMatrix(render, name, out);
}

// RCATMesh_t::GetAttractor / GetBoneMatrix: the bones brought up to date first.
bool __fastcall GetAttractor(void* mesh, void*, const char* name, float* out)
{
    if (At<void*>(mesh, cat::kRenderAnim) && At<int32_t>(mesh, kBoneState) < 1) UpdateBones(mesh);
    return AttractorMatrix(mesh, name, out);
}

bool __fastcall GetBoneMatrix(void* mesh, void*, const char* name, float* out)
{
    if (At<int32_t>(mesh, kBoneState) < 1) UpdateBones(mesh);
    return BoneMatrix(mesh, name, out);
}

int32_t __fastcall GetMaterialIndex(void* mesh, void*, const char* name)
{
    void* catMesh = Mesh(mesh);
    const uint32_t count = At<uint32_t>(catMesh, kMeshMaterialCount);
    for (uint32_t i = 0; i < count; ++i) {
        const char* n = orig::RResource_t_GetName(At<void**>(catMesh, kMeshMaterials)[i]);
        if (n && std::strcmp(n, name) == 0) return int32_t(i);
    }
    return -1;
}

vc10::Vector<void*>& Substitutes(void* mesh) { return At<vc10::Vector<void*>>(mesh, kSubstMaterials); }

void* __fastcall GetSubstMaterial(void* mesh, void*, int32_t index)
{
    auto& subst = Substitutes(mesh);
    return index < 0 || size_t(index) >= subst.size() ? nullptr : subst[size_t(index)];
}

// FUN_10040594 as used here: the substitutes made as many as the mesh's materials (none yet), on first use.
vc10::Vector<void*>& SubstitutesFor(void* mesh)
{
    auto& subst = Substitutes(mesh);
    if (subst.first == subst.last) {
        const uint32_t n = At<uint32_t>(Mesh(mesh), kMeshMaterialCount);
        subst.reserve(n);
        while (subst.size() < n) subst.push_back(nullptr);
    }
    return subst;
}

void __fastcall SetSubstMaterial(void* mesh, void*, int32_t index, void* material)
{
    auto& subst = SubstitutesFor(mesh);
    if (subst[size_t(index)]) orig::RResource_t_ReleaseRResource(subst[size_t(index)]);
    subst[size_t(index)] = material;
}

void* __fastcall CreateSubstMaterial(void* mesh, void*, int32_t index)
{
    auto& subst = SubstitutesFor(mesh);
    void* source = At<void**>(Mesh(mesh), kMeshMaterials)[index];
    if (!source) return nullptr;
    if (subst[size_t(index)]) orig::RResource_t_ReleaseRResource(subst[size_t(index)]);
    void* copy = vc10::Allocate(0xC0);
    copy = orig::RMaterial_t_RMaterial_t_30(copy, source);
    subst[size_t(index)] = copy;
    return copy;
}

void __fastcall SetSfxType(void* mesh, void*, int32_t type)
{
    At<int32_t>(mesh, kSfxType) = type;
}

float __fastcall GetBoundingSphereRadius(void* mesh)
{
    void* catMesh = Mesh(mesh);
    if (!catMesh) return 0.0f;
    const uint8_t* sphere = At<uint8_t*>(catMesh, kMeshSphere);
    const float radius = sphere ? At<float>(sphere, 0x14) : g_noRadius;
    return FrameScale(static_cast<uint8_t*>(mesh) + kVisual) * radius;
}

float* __fastcall GetBoundingSpherePos(void* mesh, void*, float* out)
{
    void* catMesh = Mesh(mesh);
    if (!catMesh) {
        out[0] = out[1] = out[2] = 0.0f;
        return out;
    }
    const float* world = static_cast<const float*>(orig::RRefFrame_t_GetWorldMatrix(static_cast<uint8_t*>(mesh) + kVisual));
    const uint8_t* sphere = At<uint8_t*>(catMesh, kMeshSphere);
    float center[3] = {0, 0, 0};
    if (sphere) std::memcpy(center, sphere + 8, sizeof(center));
    xm::Transform(center, world, out);
    return out;
}

// The attached things' map (VS2010 std::map node: left, parent, right, then the key / value; +0x39: the head).
struct MapNode {
    MapNode* left;
    MapNode* parent;
    MapNode* right;
    vc10::String name;
    vc10::Vector<void*> frames;
};
bool IsHead(const MapNode* n) { return reinterpret_cast<const uint8_t*>(n)[0x39] != 0; }

MapNode* Next(MapNode* n)                           // FUN_100582ae
{
    if (IsHead(n)) return n;
    if (!IsHead(n->right)) {
        MapNode* m = n->right;
        while (!IsHead(m->left)) m = m->left;
        return m;
    }
    MapNode* p = n->parent;
    while (!IsHead(p) && n == p->right) {
        n = p;
        p = p->parent;
    }
    return p;
}

// RCATMesh_t::ProcessAttractorChilds: mode 0 puts the attached frames on their attractors (position, rotation,
// the character's scale, its visibility); mode 1 runs their animations at `time` and processes them.
void __fastcall ProcessAttractorChilds(uint8_t* mesh, void*, int32_t mode, float time)
{
    if (At<uint32_t>(mesh, kAttached + 8) == 0) return;
    uint8_t* visual = mesh + kVisual;
    const xm::M4 world = xm::Load(orig::RRefFrame_t_GetWorldMatrix(visual));
    const float inverseScale = 1.0f / FrameScale(visual);
    MapNode* head = At<MapNode*>(mesh, kAttached + 4);
    for (MapNode* node = head->left; node != head;) {
        MapNode* current = node;
        node = Next(node);
        xm::M4 local = xm::Identity();
        if (!GetAttractor(mesh, nullptr, current->name.c_str(), local.m)) continue;
        xm::M4 m = xm::Mul(local, world);
        const float position[3] = {m.m[12], m.m[13], m.m[14]};
        xm::ScaleColumns(m, inverseScale, inverseScale, inverseScale);
        float rotation[4];
        xm::ToQuaternion(m, rotation);
        for (void** f = current->frames.first; f != current->frames.last; ++f) {
            void* child = *f;
            if (mode == 0) {
                orig::RRefFrame_t_SetVisible(child, visual[0x94] != 0, false);
                Internal<void(__fastcall*)(void*, void*, const float*)>(0x16877)(child, nullptr, position);
                Internal<void(__fastcall*)(void*, void*, const float*)>(0x168A6)(child, nullptr, rotation);
                Internal<void(__fastcall*)(void*, void*, float)>(0x559D2)(child, nullptr, FrameScale(visual));
            } else if (mode == 1) {
                if (time > 0.0f) {
                    const float total = orig::RRefFrame_t_GetAnimationTreeTotalTime(child);
                    if (total > 0.0f) {
                        if (total < time) time = float(std::fmod(double(time), double(total)));
                        orig::RRefFrame_t_SetAnimationTime(child, time, true);
                    }
                }
                reinterpret_cast<void(__fastcall*)(void*, void*)>((*static_cast<void***>(child))[8])(child, nullptr);
            }
        }
    }
}

}  // namespace

void Install(HMODULE orig)
{
    if (GetMode("CatQuery", Mode::Off) != Mode::On)
        return;
    g_orig = orig;
    g_noRadius = *reinterpret_cast<const float*>(reinterpret_cast<uint8_t*>(orig) + 0x95E5C);
    struct Hook {
        uint32_t rva;
        uint8_t bytes[8];
        uint32_t count;
        void* target;
        const char* what;
    };
    const Hook hooks[] = {
        {0x54D4F, {0x55, 0x8B, 0xEC, 0x33, 0xD2}, 5, reinterpret_cast<void*>(&HasAttractor), "CATRender_t::HasAttractor"},
        {0x54DF1, {0x55, 0x8B, 0xEC, 0x81, 0xEC, 0xE4, 0x00, 0x00}, 8, reinterpret_cast<void*>(&AttractorMatrixHook),
         "CATRender_t attractor matrix (FUN_10054df1)"},
        {0x54F4F, {0x55, 0x8B, 0xEC, 0x8B, 0x51, 0x04}, 6, reinterpret_cast<void*>(&BoneMatrixHook),
         "CATRender_t bone matrix (FUN_10054f4f)"},
        {0x562B5, {0x55, 0x8B, 0xEC, 0x56, 0x8B, 0xF1}, 6, reinterpret_cast<void*>(&GetAttractor), "RCATMesh_t::GetAttractor"},
        {0x562E1, {0x55, 0x8B, 0xEC, 0x56, 0x8B, 0xF1}, 6, reinterpret_cast<void*>(&GetBoneMatrix), "RCATMesh_t::GetBoneMatrix"},
        {0x5622A, {0x55, 0x8B, 0xEC, 0x56, 0x57}, 5, reinterpret_cast<void*>(&GetMaterialIndex), "RCATMesh_t::GetMaterialIndex"},
        {0x5628C, {0x55, 0x8B, 0xEC, 0x8B, 0x55, 0x08}, 6, reinterpret_cast<void*>(&GetSubstMaterial), "RCATMesh_t::GetSubstMaterial"},
        {0x57A30, {0x55, 0x8B, 0xEC, 0x56, 0x8D, 0xB1, 0xD8, 0x01}, 8, reinterpret_cast<void*>(&SetSubstMaterial),
         "RCATMesh_t::SetSubstMaterial"},
        {0x55C86, {0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08}, 6, reinterpret_cast<void*>(&SetSfxType), "RCATMesh_t::SetSfxType"},
        {0x55C96, {0x55, 0x8B, 0xEC, 0x51, 0x8B, 0x41, 0x04}, 7, reinterpret_cast<void*>(&GetBoundingSphereRadius),
         "RCATMesh_t::GetBoundingSphereRadius"},
        {0x56741, {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C}, 6, reinterpret_cast<void*>(&GetBoundingSpherePos),
         "RCATMesh_t::GetBoundingSpherePos"},
        {0x577D3, {0x55, 0x8B, 0xEC, 0x81, 0xEC, 0xEC, 0x00, 0x00}, 8, reinterpret_cast<void*>(&ProcessAttractorChilds),
         "RCATMesh_t::ProcessAttractorChilds"},
    };
    int installed = 0;
    for (const Hook& h : hooks)
        installed += HookEntry(orig, h.rva, h.bytes, h.count, h.target, h.what) ? 1 : 0;
    // CreateSubstMaterial: mov eax, <handler> (absolute); call _EH_prolog (relative).
    static const uint8_t kCreate[] = {0xB8, 0xE5, 0x7E, 0x08, 0x10, 0xE8, 0x46, 0x0D, 0x02, 0x00};
    static const size_t kCreateAbs[] = {1}, kCreateRel[] = {6};
    HookFixups fixups;
    fixups.abs32 = kCreateAbs;
    fixups.abs32Count = 1;
    fixups.rel32 = kCreateRel;
    fixups.rel32Count = 1;
    installed += HookEntry(orig, 0x57A70, kCreate, sizeof(kCreate), reinterpret_cast<void*>(&CreateSubstMaterial),
                           "RCATMesh_t::CreateSubstMaterial", fixups) ? 1 : 0;
    Log("character queries: %d of %d on", installed, int(sizeof(hooks) / sizeof(hooks[0])) + 1);
}

}  // namespace rnative::catquery
