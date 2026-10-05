// Characters being made, changed and destroyed natively (randy-vk.ini [Native] Scene=on): CATRender_t (a mesh and an
// animation turned into vertex buffers per piece of each mesh group) - construction, destruction, SetMesh, SetAnim,
// vertex process callbacks, the buffers rebuilt on the CPU after a lost device; RCATMesh_t (a CATRender_t and an
// RVisual_t) - construction, destruction, attractor children (a std::map<std::string, std::vector<RRefFrame_t*>>),
// its RestoreData and RenderDepth.
//
// CATRender_t (0x3C bytes, vtable 0x95DC8; cat.h): +0x04 CATMesh_t*, +0x08 CATAnim_t*, +0x0C groups {count, array of
// {CATMesh_t*, slot count, slots}} (array new[] with its count before it), +0x14 bones {count, 0x30 each}, +0x1C
// {count, 0x10 each}, +0x24 changes, +0x28 rest pose, +0x2C std::vector of {user, callback}.
// RCATMesh_t (0x438 bytes; vtables 0x95F34, +0x3C 0x95EDC, +0xE0 0x95ECC): +0x3C its RVisual_t, +0x1B8 0.03,
// +0x1BC 1, +0x1C0 / +0x1C4 -1, +0x1C8 1, +0x1CC the largest group extents, +0x1D8 std::vector of substitute
// materials, +0x1E8 the attractor children (map: +0x1EC head, +0x1F0 size), +0x224 / +0x2A8 / +0x32C / +0x3B0
// StateBlob_c for its effects.
#include "native/cat_life.h"
#include "native/cat.h"
#include "native/orig_api.gen.h"
#include "native/vc10.h"

#include <cstring>

namespace rnative::catlife {

namespace {

HMODULE g_orig;

template <typename T>
T& Field(void* object, uint32_t offset) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(object) + offset); }
template <typename T>
T& Global(uint32_t rva) { return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(g_orig) + rva); }
template <typename F>
F Internal(uint32_t rva) { return reinterpret_cast<F>(AddressFor(rva)); }

template <typename R = void, typename... A>
R Virtual(void* object, uint32_t slot, A... args)
{
    return reinterpret_cast<R(__fastcall*)(void*, void*, A...)>((*static_cast<void***>(object))[slot])(object, nullptr,
                                                                                                      args...);
}
void SetVtable(void* o, uint32_t rva) { Field<uintptr_t>(o, 0) = reinterpret_cast<uintptr_t>(g_orig) + rva; }

using cat::RenderGroup;
using Slot = RenderGroup::Slot;

constexpr uint32_t kRenderVtable = 0x95DC8;
constexpr uint32_t kMesh = 0x04, kAnim = 0x08, kGroupCount = 0x0C, kGroups = 0x10, kBoneCount = 0x14, kBones = 0x18,
                   kPairCount = 0x1C, kPairs = 0x20, kChanges = 0x24, kRest = 0x28, kCallbacks = 0x2C;
constexpr uint32_t kVertexFvf = 0x112;              // position, normal, one texture coordinate (0x20 bytes)

// ---- arrays made with new[] (their count before them) ----

template <typename T>
T* NewArray(uint32_t n)
{
    uint32_t* block = static_cast<uint32_t*>(vc10::AllocateArray(n * sizeof(T) + 4));
    block[0] = n;
    return reinterpret_cast<T*>(block + 1);
}
template <typename T>
uint32_t ArrayCount(T* a) { return reinterpret_cast<uint32_t*>(a)[-1]; }
template <typename T>
void DeleteArray(T* a) { vc10::FreeArray(reinterpret_cast<uint32_t*>(a) - 1); }

// ---- slots: one vertex buffer per piece ----

void* __fastcall SlotConstruct(Slot* s) { return s->vertexCount = 0, s->buffer = nullptr, s; }   // (0x53F36)
void __fastcall SlotDestroy(Slot* s)                 // FUN_10053f40
{
    if (s->buffer) orig::VertexBuffer_c_Release(s->buffer);
}

void* __fastcall SlotArrayDelete(Slot* s, void*, uint8_t flags)   // FUN_10055434 (vector deleting destructor)
{
    if (!(flags & 2)) {
        SlotDestroy(s);
        if (flags & 1) vc10::Free(s);
        return s;
    }
    for (uint32_t i = ArrayCount(s); i-- > 0;) SlotDestroy(s + i);
    if (flags & 1) DeleteArray(s);
    return reinterpret_cast<uint32_t*>(s) - 1;
}

// FUN_10053f4d: a slot's buffer for `n` vertices (none for 0).
void __fastcall SlotAllocate(Slot* s, void*, uint32_t n)
{
    if (s->buffer) {
        orig::VertexBuffer_c_Release(s->buffer);
        s->buffer = nullptr;
    }
    s->vertexCount = n;
    if (n)
        s->buffer = orig::VertexBuffer_c_VertexBuffer_c_61(vc10::Allocate(4), kVertexFvf, 0, 2,
                                                           orig::VertexBuffer_c_GetFormatSize(kVertexFvf) * n);
}

void* __fastcall SlotLock(Slot* s) { return orig::VertexBuffer_c_Lock(s->buffer, 0, 0); }   // FUN_10053fc0

// ---- groups ----

struct SlotArray {                                   // a group's {count, slots}
    uint32_t count;
    Slot* slots;
};

void __fastcall ResizeSlots(SlotArray* a, void*, uint32_t n)   // FUN_1005548d
{
    if (a->count == n) return;
    if (a->slots) SlotArrayDelete(a->slots, nullptr, 3);
    a->count = n;
    a->slots = nullptr;
    if (n) {
        Slot* s = NewArray<Slot>(n);
        for (uint32_t i = 0; i < n; ++i) SlotConstruct(s + i);
        a->slots = s;
    }
}

void* __fastcall GroupConstruct(RenderGroup* g)      // (0x5555C) the mesh left as it is
{
    auto* slots = reinterpret_cast<SlotArray*>(&g->unknown);
    slots->count = 0;
    slots->slots = nullptr;
    ResizeSlots(slots, nullptr, 0);
    return g;
}

void __fastcall GroupDestroy(RenderGroup* g)          // FUN_10054b77
{
    orig::RResource_t_ReleaseRResource(g->mesh);
    if (g->slots) SlotArrayDelete(g->slots, nullptr, 3);
}

void* __fastcall GroupArrayDelete(RenderGroup* g, void*, uint8_t flags)   // FUN_10055512
{
    if (!(flags & 2)) {
        GroupDestroy(g);
        if (flags & 1) vc10::Free(g);
        return g;
    }
    for (uint32_t i = ArrayCount(g); i-- > 0;) GroupDestroy(g + i);
    if (flags & 1) DeleteArray(g);
    return reinterpret_cast<uint32_t*>(g) - 1;
}

struct GroupArray {
    uint32_t count;
    RenderGroup* groups;
};

void __fastcall ResizeGroups(GroupArray* a, void*, uint32_t n)   // FUN_1005559b
{
    if (a->count == n) return;
    RenderGroup* groups = nullptr;
    if (a->groups) GroupArrayDelete(a->groups, nullptr, 3);
    a->count = n;
    if (n) {
        groups = NewArray<RenderGroup>(n);
        for (uint32_t i = 0; i < n; ++i) GroupConstruct(groups + i);
    }
    a->groups = groups;
}

// ---- bones ----

struct Sized {
    uint32_t count;
    void* data;
};

void __fastcall ResizeBones(Sized* a, void*, uint32_t n)   // FUN_1005538f: 0x30 each, as they are
{
    if (a->count == n) return;
    vc10::FreeArray(a->data);
    a->count = n;
    a->data = n ? vc10::AllocateArray(size_t(n) * 0x30) : nullptr;
}

void __fastcall ResizePairs(Sized* a, void*, uint32_t n)   // FUN_100553d2: 0x10 each, zeroed
{
    if (a->count == n) return;
    vc10::FreeArray(a->data);
    a->count = n;
    void* p = nullptr;
    if (n && (p = vc10::AllocateArray(size_t(n) * 0x10)) != nullptr) std::memset(p, 0, size_t(n) * 0x10);
    a->data = p;
}

void __fastcall BoneIdentity(float* b)               // FUN_10053edd: a 4x3 identity
{
    std::memset(b, 0, 0x30);
    b[0] = b[4] = b[8] = 1.0f;
}

// ---- CATRender_t ----

void* __fastcall RenderConstruct(uint8_t* r)          // FUN_10055243
{
    SetVtable(r, kRenderVtable);
    auto* groups = reinterpret_cast<GroupArray*>(r + kGroupCount);
    groups->count = 0;
    groups->groups = nullptr;
    ResizeGroups(groups, nullptr, 0);
    Field<uint32_t>(r, kBoneCount) = 0;
    Field<void*>(r, kBones) = nullptr;
    Field<uint32_t>(r, kPairCount) = 0;
    Field<void*>(r, kPairs) = nullptr;
    Field<vc10::Vector<cat::Callback>>(r, kCallbacks) = {};
    Field<void*>(r, kMesh) = nullptr;
    Field<void*>(r, kAnim) = nullptr;
    Field<uint32_t>(r, kChanges) = 0;
    r[kRest] = 0;
    return r;
}

void __fastcall RenderDestroy(uint8_t* r)             // FUN_10055299: the callbacks told (all null) on the way
{
    SetVtable(r, kRenderVtable);
    if (void* m = Field<void*>(r, kMesh)) orig::RResource_t_ReleaseRResource(m);
    if (void* a = Field<void*>(r, kAnim)) orig::RResource_t_ReleaseRResource(a);
    auto& callbacks = Field<vc10::Vector<cat::Callback>>(r, kCallbacks);
    for (cat::Callback* c = callbacks.first; c != callbacks.last; ++c)
        if (c->fn) c->fn(nullptr, 0, nullptr, nullptr, 0, nullptr, 0, 0, c->user);
    callbacks.release();
    vc10::FreeArray(Field<void*>(r, kPairs));
    vc10::FreeArray(Field<void*>(r, kBones));
    if (RenderGroup* g = Field<RenderGroup*>(r, kGroups)) GroupArrayDelete(g, nullptr, 3);
}

void* __fastcall RenderDelete(uint8_t* r, void*, uint8_t flags)   // vtable slot 0 (FUN_1005564d)
{
    RenderDestroy(r);
    if (flags & 1) vc10::Free(r);
    return r;
}

int32_t BoneCountOf(void* resource) { return Virtual<int32_t>(resource, 9); }   // (vtable +0x24)

// CATRender_t::SetMesh: a vertex buffer per piece of every group, its texture coordinates filled in.
void __fastcall SetMesh(uint8_t* r, void*, uint8_t* mesh)
{
    ++Field<int32_t>(r, kChanges);
    if (void* anim = Field<void*>(r, kAnim))
        if (Field<int32_t>(mesh, 0x58) != BoneCountOf(anim)) {
            orig::RResource_t_ReleaseRResource(anim);
            Field<void*>(r, kAnim) = nullptr;
        }
    orig::RResource_t_AddRefRResource(mesh);
    if (void* old = Field<void*>(r, kMesh)) orig::RResource_t_ReleaseRResource(old);
    Field<void*>(r, kMesh) = mesh;
    ResizeGroups(reinterpret_cast<GroupArray*>(r + kGroupCount), nullptr, Field<uint32_t>(mesh, 0x48));
    for (int32_t gi = 0; gi < Field<int32_t>(r, kGroupCount); ++gi) {
        RenderGroup* g = Field<RenderGroup*>(r, kGroups) + gi;
        uint8_t* group = Field<uint8_t*>(mesh, cat::kMeshGroups) + gi * cat::kGroupSize;
        g->mesh = mesh;
        orig::RResource_t_AddRefRResource(mesh);
        ResizeSlots(reinterpret_cast<SlotArray*>(&g->unknown), nullptr, Field<uint32_t>(group, cat::kGroupPieceCount));
        for (int32_t pi = 0; pi < Field<int32_t>(group, cat::kGroupPieceCount); ++pi) {
            uint8_t* piece = Field<uint8_t*>(group, cat::kGroupPieces) + pi * cat::kPieceSize;
            SlotAllocate(g->slots + pi, nullptr, Field<uint32_t>(piece, 8));
            float* out = static_cast<float*>(SlotLock(g->slots + pi));
            const uint8_t* in = Field<uint8_t*>(piece, cat::kPieceVertices);
            for (int32_t v = 0; v < Field<int32_t>(piece, 8); ++v, out += 8, in += 0x44) {
                out[6] = Field<float>(const_cast<uint8_t*>(in), 0x30);
                out[7] = Field<float>(const_cast<uint8_t*>(in), 0x34);
            }
            orig::VertexBuffer_c_Unlock(g->slots[pi].buffer);
        }
    }
    Virtual(r, 2);
}

// CATRender_t::SetAnim: kept when it fits the mesh's skeleton (or there's no mesh yet); bones made for it.
void __fastcall SetAnim(uint8_t* r, void*, void* anim)
{
    void* old = Field<void*>(r, kAnim);
    if (anim == old) return;
    ++Field<int32_t>(r, kChanges);
    if (!anim) {
        if (old) orig::RResource_t_ReleaseRResource(old);
        Field<void*>(r, kAnim) = nullptr;
        return;
    }
    if (void* mesh = Field<void*>(r, kMesh))
        if (BoneCountOf(anim) != Field<int32_t>(mesh, 0x58)) return;
    orig::RResource_t_AddRefRResource(anim);
    if (void* previous = Field<void*>(r, kAnim)) orig::RResource_t_ReleaseRResource(previous);
    int32_t had = Field<int32_t>(r, kBoneCount);
    Field<void*>(r, kAnim) = anim;
    if (had < Virtual<int32_t>(anim, 3)) {
        had = Field<int32_t>(r, kBoneCount);
        ResizeBones(reinterpret_cast<Sized*>(r + kBoneCount), nullptr, Virtual<uint32_t>(Field<void*>(r, kAnim), 3));
        ResizePairs(reinterpret_cast<Sized*>(r + kPairCount), nullptr, Virtual<uint32_t>(Field<void*>(r, kAnim), 3));
        for (int32_t b = had; b != Field<int32_t>(r, kBoneCount); ++b)
            BoneIdentity(Field<float*>(r, kBones) + b * 12);
    }
}

bool __fastcall RegisterCallback(uint8_t* r, void*, cat::CallbackFn fn, void* user)
{
    Field<vc10::Vector<cat::Callback>>(r, kCallbacks).push_back(cat::Callback{user, fn});
    return true;
}

bool __fastcall UnregisterCallback(uint8_t* r, void*, cat::CallbackFn fn, void* user)
{
    auto& callbacks = Field<vc10::Vector<cat::Callback>>(r, kCallbacks);
    for (cat::Callback* c = callbacks.first; c != callbacks.last; ++c)
        if (c->user == user && c->fn == fn) {
            callbacks.erase(c);
            return true;
        }
    return false;
}

// Vtable slot 1 (FUN_10054d16): the rest pose, when the animation says so.
void __fastcall RestPose(uint8_t* r)
{
    void* anim = Field<void*>(r, kAnim);
    if (!anim || !Virtual<uint8_t>(anim, 10)) return;
    float identity[12];
    BoneIdentity(identity);
    Internal<void(__fastcall*)(void*, void*, float*, int32_t, float)>(0x540A5)(r, nullptr, identity, 0, 1.0f);
}

// FUN_100543bc: every piece skinned on the CPU into a new buffer (after a lost device): the rest pose, one bone, or
// two blended; then the vertex process callbacks.
void __fastcall RebuildBuffers(uint8_t* r)
{
    static bool logged = false;
    if (!logged) {
        logged = true;
        Log("characters: vertex buffers rebuilt on the CPU (a lost device's restore)");
    }
    uint32_t vertexBase = 0, triBase = 0;
    for (int32_t gi = 0; gi < Field<int32_t>(r, kGroupCount); ++gi) {
        RenderGroup* g = Field<RenderGroup*>(r, kGroups) + gi;
        uint8_t* group = Field<uint8_t*>(g->mesh, cat::kMeshGroups) + gi * cat::kGroupSize;
        for (int32_t pi = 0; pi < Field<int32_t>(group, cat::kGroupPieceCount); ++pi) {
            uint8_t* piece = Field<uint8_t*>(group, cat::kGroupPieces) + pi * cat::kPieceSize;
            SlotAllocate(g->slots + pi, nullptr, g->slots[pi].vertexCount);   // a new buffer, the same size
            float* base = static_cast<float*>(SlotLock(g->slots + pi));
            float* o = base + 3;
            const float* bones = Field<float*>(r, kBones);
            for (int32_t vi = 0; vi < Field<int32_t>(piece, 8); ++vi, o += 8) {
                const float* v = reinterpret_cast<const float*>(Field<uint8_t*>(piece, cat::kPieceVertices) + vi * 0x44);
                o[3] = v[12];
                o[4] = v[13];
                if (r[kRest] == 1) {
                    o[-3] = v[6], o[-2] = v[7], o[-1] = v[8];
                    o[0] = v[9], o[1] = v[10], o[2] = v[11];
                } else if (v[16] <= 0.99f) {
                    const float w = v[16], w1 = 1.0f - w;
                    const float* a = bones + reinterpret_cast<const int32_t*>(v)[14] * 12;
                    const float* b = bones + reinterpret_cast<const int32_t*>(v)[15] * 12;
                    o[-3] = w * (a[6] * v[2] + v[1] * a[3] + a[0] * v[0] + a[9]) +
                            (b[6] * v[5] + b[0] * v[3] + b[3] * v[4] + b[9]) * w1;
                    o[0] = a[6] * v[11] + a[3] * v[10] + a[0] * v[9];
                    o[-2] = w * (v[2] * a[7] + v[1] * a[4] + a[1] * v[0] + a[10]) +
                            (b[7] * v[5] + b[4] * v[4] + b[1] * v[3] + b[10]) * w1;
                    o[1] = v[11] * a[7] + a[4] * v[10] + a[1] * v[9];
                    o[-1] = (b[8] * v[5] + b[5] * v[4] + b[2] * v[3] + b[11]) * w1 +
                            (v[2] * a[8] + v[1] * a[5] + a[2] * v[0] + a[11]) * w;
                    o[2] = v[11] * a[8] + a[5] * v[10] + a[2] * v[9];
                } else {
                    const float* a = bones + reinterpret_cast<const int32_t*>(v)[14] * 12;
                    o[-3] = a[6] * v[2] + v[1] * a[3] + v[0] * a[0] + a[9];
                    o[0] = a[6] * v[11] + a[0] * v[9] + v[10] * a[3];
                    o[-2] = v[2] * a[7] + v[1] * a[4] + a[1] * v[0] + a[10];
                    o[1] = v[11] * a[7] + v[10] * a[4] + a[1] * v[9];
                    o[-1] = a[8] * v[2] + v[0] * a[2] + a[5] * v[1] + a[11];
                    o[2] = a[8] * v[11] + a[2] * v[9] + a[5] * v[10];
                }
            }
            auto& callbacks = Field<vc10::Vector<cat::Callback>>(r, kCallbacks);
            for (uint32_t c = 0; c < callbacks.size(); ++c)
                callbacks.first[c].fn(r, Field<uint32_t>(piece, cat::kPieceVertexCount),
                                      Field<const cat::TriVertex*>(piece, cat::kPieceVertices),
                                      reinterpret_cast<cat::Vertex*>(base), Field<uint32_t>(piece, cat::kPieceTriCount),
                                      Field<const uint16_t*>(piece, cat::kPieceIndices), vertexBase, triBase,
                                      callbacks.first[c].user);
            vertexBase += Field<uint32_t>(piece, cat::kPieceVertexCount);
            triBase += Field<uint32_t>(piece, cat::kPieceTriCount);
            orig::VertexBuffer_c_Unlock(g->slots[pi].buffer);
        }
    }
}

// ---- RCATMesh_t ----

constexpr uint32_t kCatVtable = 0x95F34, kCatVisualVtable = 0x95EDC, kCatThirdVtable = 0x95ECC;
constexpr uint32_t kVisual = 0x3C, kThird = 0xE0, kSubstMaterials = 0x1D8, kAttached = 0x1E8, kExtents = 0x1CC;
constexpr uint32_t kBlobs[4] = {0x224, 0x2A8, 0x32C, 0x3B0};

// The attractor children: VS2010 std::map<std::string, std::vector<RRefFrame_t*>> (node: left, parent, right, the
// name, the frames, colour (0 red, 1 black), +0x39 head).
struct MapNode {
    MapNode* left;
    MapNode* parent;
    MapNode* right;
    vc10::String name;
    vc10::Vector<void*> frames;
    uint8_t black;
    uint8_t head;
};
static_assert(sizeof(MapNode) == 0x3C, "map node");
struct Map {
    uint32_t allocator;
    MapNode* head;
    uint32_t size;
};

void* __fastcall MapInit(Map* m)                       // FUN_100583aa
{
    m->size = 0;
    MapNode* h = static_cast<MapNode*>(vc10::Allocate(sizeof(MapNode)));
    m->head = h;
    h->left = h->parent = h->right = h;
    h->black = 1;
    h->head = 1;
    return m;
}

void EraseSubtree(MapNode* n)
{
    while (!n->head) {
        EraseSubtree(n->right);
        MapNode* left = n->left;
        n->frames.release();
        n->name.release();
        vc10::Free(n);
        n = left;
    }
}

void __fastcall MapDestroy(Map* m)                     // FUN_10058f11
{
    EraseSubtree(m->head->parent);
    vc10::Free(m->head);
}

int Compare(const vc10::String& a, const char* b, size_t bn)   // std::string::compare
{
    const size_t an = a.size;
    const int c = std::memcmp(a.c_str(), b, an < bn ? an : bn);
    if (c) return c;
    return an < bn ? -1 : an != bn ? 1 : 0;
}

void RotateLeft(Map* m, MapNode* x)
{
    MapNode* y = x->right;
    x->right = y->left;
    if (!y->left->head) y->left->parent = x;
    y->parent = x->parent;
    if (x == m->head->parent) m->head->parent = y;
    else if (x == x->parent->left) x->parent->left = y;
    else x->parent->right = y;
    y->left = x;
    x->parent = y;
}

void RotateRight(Map* m, MapNode* x)
{
    MapNode* y = x->left;
    x->left = y->right;
    if (!y->right->head) y->right->parent = x;
    y->parent = x->parent;
    if (x == m->head->parent) m->head->parent = y;
    else if (x == x->parent->right) x->parent->right = y;
    else x->parent->left = y;
    y->right = x;
    x->parent = y;
}

// FUN_10058e75: map[name] - inserted (empty) when missing (a red-black insert as VS2010's _Insert).
vc10::Vector<void*>* __fastcall MapAt(Map* m, void*, const char* name)
{
    const size_t n = std::strlen(name);
    MapNode* h = m->head;
    MapNode* where = h;
    bool left = true;
    for (MapNode* p = h->parent; !p->head;) {
        where = p;
        const int c = Compare(p->name, name, n);
        if (c == 0) return &p->frames;
        left = c > 0;                                 // name < p->name
        p = left ? p->left : p->right;
    }
    MapNode* z = static_cast<MapNode*>(vc10::Allocate(sizeof(MapNode)));
    z->left = z->right = h;
    z->parent = where;
    z->name.init();
    z->name.allocator = 0;
    z->name.assign(name, n);
    z->frames = {};
    z->black = 0;
    z->head = 0;
    ++m->size;
    if (where == h) {
        h->parent = h->left = h->right = z;
    } else if (left) {
        where->left = z;
        if (where == h->left) h->left = z;
    } else {
        where->right = z;
        if (where == h->right) h->right = z;
    }
    for (MapNode* p = z; !p->parent->black;) {
        MapNode* parent = p->parent;
        MapNode* grand = parent->parent;
        if (parent == grand->left) {
            MapNode* uncle = grand->right;
            if (!uncle->black) {
                parent->black = 1, uncle->black = 1, grand->black = 0;
                p = grand;
            } else {
                if (p == parent->right) {
                    p = parent;
                    RotateLeft(m, p);
                }
                p->parent->black = 1;
                p->parent->parent->black = 0;
                RotateRight(m, p->parent->parent);
            }
        } else {
            MapNode* uncle = grand->left;
            if (!uncle->black) {
                parent->black = 1, uncle->black = 1, grand->black = 0;
                p = grand;
            } else {
                if (p == parent->left) {
                    p = parent;
                    RotateRight(m, p);
                }
                p->parent->black = 1;
                p->parent->parent->black = 0;
                RotateLeft(m, p->parent->parent);
            }
        }
    }
    h->parent->black = 1;
    return &z->frames;
}

void __fastcall AddAttractorChild(uint8_t* c, void*, const char* attractor, void* frame)
{
    auto* frames = MapAt(reinterpret_cast<Map*>(c + kAttached), nullptr, attractor);
    for (void** f = frames->first; f != frames->last; ++f)
        if (*f == frame) return;
    MapAt(reinterpret_cast<Map*>(c + kAttached), nullptr, attractor)->push_back(frame);
}

void __fastcall RemoveAttractorChild(uint8_t* c, void*, const char* attractor, const void* frame)
{
    auto* frames = MapAt(reinterpret_cast<Map*>(c + kAttached), nullptr, attractor);
    for (void** f = frames->first; f != frames->last; ++f)
        if (*f == frame) {
            frames->erase(f);
            return;
        }
}

void __fastcall BlobDefaults(uint8_t* c)              // FUN_10055a3e: the four effects' states
{
    struct State {
        bool stage;
        int32_t type;
        uint32_t value;
    };
    static const State kEnv[] = {{true, 0xB, 0x10000}, {true, 0x18, 2}, {true, 1, 2}, {true, 2, 2}, {true, 3, 0},
                                 {false, 0x13, 2},     {false, 0x14, 2}, {false, 0x1B, 1}};
    static const State kSphere[] = {{true, 0xB, 1}, {true, 1, 2},     {true, 2, 2},     {true, 3, 0},
                                    {false, 0x13, 2}, {false, 0x14, 2}, {false, 0x1B, 1}};
    static const State kCatLight[] = {{true, 1, 4},     {true, 2, 0},      {true, 3, 2},      {true, 0xB, 0x20000},
                                      {true, 0x18, 3},  {true, 0xC, 1},    {false, 0xE, 0},   {false, 0x17, 3},
                                      {false, 0x89, 1}, {false, 0x13, 2},  {false, 0x14, 2},  {false, 0x1B, 1},
                                      {false, 0x22, 0}, {false, 0x8B, 0}};
    static const State kSfx[] = {{true, 1, 4},     {true, 2, 3},     {true, 3, 2},      {true, 0xC, 1},
                                 {false, 0x89, 0}, {false, 0x13, 2}, {false, 0x14, 2},  {false, 0x1B, 1},
                                 {false, 0x22, 0}};
    auto apply = [&](uint32_t offset, const State* s, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            if (s[i].stage) orig::StateBlob_c_SetTextureStageState(c + offset, 0, s[i].type, s[i].value);
            else orig::StateBlob_c_SetRenderState(c + offset, s[i].type, s[i].value);
        }
    };
    apply(kBlobs[0], kEnv, sizeof(kEnv) / sizeof(kEnv[0]));
    apply(kBlobs[1], kSphere, sizeof(kSphere) / sizeof(kSphere[0]));
    apply(kBlobs[2], kCatLight, sizeof(kCatLight) / sizeof(kCatLight[0]));
    apply(kBlobs[3], kSfx, sizeof(kSfx) / sizeof(kSfx[0]));
}

void* __fastcall CatConstruct(uint8_t* c, void*, void* parent)   // RCATMesh_t::RCATMesh_t
{
    RenderConstruct(c);
    orig::RVisual_t_RVisual_t_49(c + kVisual, parent, nullptr);
    Field<int32_t>(c, 0x1C0) = -1;
    Field<float>(c, 0x1B8) = 0.03f;
    Field<int32_t>(c, 0x1C4) = -1;
    Field<float>(c, kExtents) = 0.0f;
    SetVtable(c, kCatVtable);
    Field<float>(c, kExtents + 4) = 0.0f;
    SetVtable(c + kVisual, kCatVisualVtable);
    Field<float>(c, kExtents + 8) = 0.0f;
    SetVtable(c + kThird, kCatThirdVtable);
    c[0x1B4] = 0;
    Field<int32_t>(c, 0x1BC) = 1;
    c[0x1C8] = 1;
    Field<vc10::Vector<void*>>(c, kSubstMaterials) = {};
    MapInit(reinterpret_cast<Map*>(c + kAttached));
    std::memset(c + 0x1F8, 0, 0x1C);
    Field<int32_t>(c, 0x218) = -1;
    c[0x214] = 0;
    Field<int32_t>(c, 0x21C) = 0;
    c[0x220] = 0;
    for (uint32_t b : kBlobs) orig::StateBlob_c_StateBlob_c(c + b);
    Field<int32_t>(c, 0x434) = 0;
    BlobDefaults(c);
    return c;
}

void __fastcall CatDestroy(uint8_t* c)                 // FUN_10057f15
{
    SetVtable(c, kCatVtable);
    SetVtable(c + kVisual, kCatVisualVtable);
    SetVtable(c + kThird, kCatThirdVtable);
    auto& materials = Field<vc10::Vector<void*>>(c, kSubstMaterials);
    for (void** m = materials.first; m != materials.last; ++m)
        if (*m) orig::RResource_t_ReleaseRResource(*m);
    for (int i = 3; i >= 0; --i) Internal<void(__fastcall*)(void*)>(0x255B5)(c + kBlobs[i]);   // ~StateBlob_c
    MapDestroy(reinterpret_cast<Map*>(c + kAttached));
    materials.release();
    Internal<void(__fastcall*)(void*)>(0x4D7D3)(c + kVisual);   // ~RVisual_t
    RenderDestroy(c);
}

void* __fastcall CatDelete(uint8_t* c, void*, uint8_t flags)   // vtable slot 0 (FUN_10058f67)
{
    CatDestroy(c);
    if (flags & 1) vc10::Free(c);
    return c;
}
void* __fastcall CatDeleteFromVisual(uint8_t* v, void*, uint8_t flags) { return CatDelete(v - kVisual, nullptr, flags); }
void* __fastcall CatDeleteFromThird(uint8_t* t, void*, uint8_t flags) { return CatDelete(t - kThird, nullptr, flags); }

void __fastcall CatExtents(uint8_t* c)                 // vtable slot 2 (FUN_10055ccd): the largest group extents
{
    float x = 0.0f, y = 0.0f, z = 0.0f;
    RenderGroup* g = Field<RenderGroup*>(c, kGroups);
    for (int32_t n = Field<int32_t>(c, kGroupCount); n > 0; --n, ++g) {
        const uint8_t* mesh = static_cast<const uint8_t*>(g->mesh);
        if (x < Field<float>(const_cast<uint8_t*>(mesh), 0x2C)) x = Field<float>(const_cast<uint8_t*>(mesh), 0x2C);
        if (y < Field<float>(const_cast<uint8_t*>(mesh), 0x30)) y = Field<float>(const_cast<uint8_t*>(mesh), 0x30);
        if (z < Field<float>(const_cast<uint8_t*>(mesh), 0x34)) z = Field<float>(const_cast<uint8_t*>(mesh), 0x34);
    }
    Field<float>(c, kExtents) = x;
    Field<float>(c, kExtents + 4) = y;
    Field<float>(c, kExtents + 8) = z;
}

void __fastcall CatRestoreData(uint8_t* visual)        // RVisual_t vtable slot 19 (FUN_10055c75)
{
    orig::RVisual_t_RestoreData(visual);
    RebuildBuffers(visual - kVisual);
}

}  // namespace

void Install(HMODULE orig)
{
    if (GetMode("Scene", Mode::Off) != Mode::On) return;
    g_orig = orig;
    struct Entry {
        uint32_t rva;
        void* target;
        const char* what;
    };
#define FN(f) reinterpret_cast<void*>(&f)
    const Entry entries[] = {
        {0x53F36, FN(SlotConstruct), "CATRender_t slot (0x10053f36)"},
        {0x53F40, FN(SlotDestroy), "CATRender_t slot destructor (FUN_10053f40)"},
        {0x55434, FN(SlotArrayDelete), "CATRender_t slots deleting (FUN_10055434)"},
        {0x53F4D, FN(SlotAllocate), "CATRender_t slot buffer (FUN_10053f4d)"},
        {0x53FC0, FN(SlotLock), "CATRender_t slot lock (FUN_10053fc0)"},
        {0x5548D, FN(ResizeSlots), "CATRender_t slots resize (FUN_1005548d)"},
        {0x5555C, FN(GroupConstruct), "CATRender_t group (0x1005555c)"},
        {0x54B77, FN(GroupDestroy), "CATRender_t group destructor (FUN_10054b77)"},
        {0x55512, FN(GroupArrayDelete), "CATRender_t groups deleting (FUN_10055512)"},
        {0x5559B, FN(ResizeGroups), "CATRender_t groups resize (FUN_1005559b)"},
        {0x5538F, FN(ResizeBones), "CATRender_t bones resize (FUN_1005538f)"},
        {0x553D2, FN(ResizePairs), "CATRender_t pairs resize (FUN_100553d2)"},
        {0x53EDD, FN(BoneIdentity), "CATRender_t bone identity (FUN_10053edd)"},
        {0x55243, FN(RenderConstruct), "CATRender_t::CATRender_t (FUN_10055243)"},
        {0x55299, FN(RenderDestroy), "CATRender_t::~CATRender_t (FUN_10055299)"},
        {0x5564D, FN(RenderDelete), "CATRender_t deleting destructor (FUN_1005564d)"},
        {0x55108, FN(SetMesh), "CATRender_t::SetMesh"},
        {0x53FE2, FN(SetAnim), "CATRender_t::SetAnim"},
        {0x5536C, FN(RegisterCallback), "CATRender_t::RegisterVertexProcessCallback"},
        {0x55331, FN(UnregisterCallback), "CATRender_t::UnregisterVertexProcessCallback"},
        {0x54D16, FN(RestPose), "CATRender_t rest pose (FUN_10054d16)"},
        {0x543BC, FN(RebuildBuffers), "CATRender_t buffers on the CPU (FUN_100543bc)"},
        {0x583AA, FN(MapInit), "RCATMesh_t attractor map (FUN_100583aa)"},
        {0x58F11, FN(MapDestroy), "RCATMesh_t attractor map destructor (FUN_10058f11)"},
        {0x58E75, FN(MapAt), "RCATMesh_t attractor map [] (FUN_10058e75)"},
        {0x57DE0, FN(AddAttractorChild), "RCATMesh_t::AddAttractorChild"},
        {0x57E96, FN(RemoveAttractorChild), "RCATMesh_t::RemoveAttractorChild"},
        {0x55A3E, FN(BlobDefaults), "RCATMesh_t effect states (FUN_10055a3e)"},
        {0x57FE0, FN(CatConstruct), "RCATMesh_t::RCATMesh_t"},
        {0x57F15, FN(CatDestroy), "RCATMesh_t::~RCATMesh_t (FUN_10057f15)"},
        {0x58F67, FN(CatDelete), "RCATMesh_t deleting destructor (FUN_10058f67)"},
        {0x58F54, FN(CatDeleteFromVisual), "RCATMesh_t deleting destructor, visual (FUN_10058f54)"},
        {0x58F5C, FN(CatDeleteFromThird), "RCATMesh_t deleting destructor, third base (FUN_10058f5c)"},
        {0x55CCD, FN(CatExtents), "RCATMesh_t group extents (FUN_10055ccd)"},
        {0x55C75, FN(CatRestoreData), "RCATMesh_t::RestoreData (FUN_10055c75)"},
    };
#undef FN
    int installed = 0;
    for (const Entry& e : entries) installed += Replace(orig, e.rva, e.target, e.what) ? 1 : 0;
    Log("characters: %d of %d functions native", installed, int(sizeof(entries) / sizeof(entries[0])));
}

}  // namespace rnative::catlife
