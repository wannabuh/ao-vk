// Character data natively (randy-vk.ini [Native] Scene=on): CATMesh_t loaded from the game's data stream (materials,
// collision spheres, bones, groups of pieces of skinned vertices and triangles, attractors), destroyed, and asked;
// CATGroup_t and CATTriPolyList_t, its parts; CATError_t, what a bad file throws.
//
// CATMesh_t (an RResource_t, vtable 0x95D44): +0x2C / +0x30 two values of the file,
// +0x34 the farthest vertex (squared while loading), +0x38 materials {count, RMaterial_t*[]}, +0x40 bones {count,
// 0x28 each: name, +0x1C a value, +0x20 {count, int[]}}, +0x48 groups {count, CATGroup_t[]}, +0x50 collision spheres
// {count, 0x14 each: 4 floats, -1}, +0x58 the skeleton's bone count, +0x5C BVolume_t* of the rest pose, +0x60 0.
// CATGroup_t (0x34): name, +0x1C pieces {count, CATTriPolyList_t[]}, +0x24 spheres {count, 0x14 each}, +0x2C
// attractors {count, 0x40 each: name, 6 floats + 1, an int}.
// CATTriPolyList_t (0x34): +0x00 RMaterial_t*, +0x08 vertices {count, CATTriVertex_t[] (0x44)}, +0x10 indices
// {count, u16[]}, +0x18 an object (deleted with it), +0x1C neighbours (3 ints a triangle), +0x20 an array, +0x28 -1,
// +0x2C active vertices, +0x30 active triangles.
#include "native/cat_data.h"
#include "native/orig_api.gen.h"
#include "native/vc10.h"
#include "native/xmath.h"

#include <cmath>
#include <cstring>

namespace rnative::catdata {

namespace {

HMODULE g_orig;

template <typename T>
T& Field(void* object, uint32_t offset) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(object) + offset); }
template <typename F>
F Internal(uint32_t rva) { return reinterpret_cast<F>(AddressFor(rva)); }
template <typename R = void, typename... A>
R Virtual(void* object, uint32_t slot, A... args)
{
    return reinterpret_cast<R(__fastcall*)(void*, void*, A...)>((*static_cast<void***>(object))[slot])(object, nullptr,
                                                                                                      args...);
}
void SetVtable(void* o, uint32_t rva) { Field<uintptr_t>(o, 0) = reinterpret_cast<uintptr_t>(g_orig) + rva; }

using vc10::String;

// ---- arrays as the original keeps them: {count, pointer}; with element destructors: new[] with the count before ----

struct Array {
    int32_t count;
    void* data;
};

template <size_t Size, typename Init>
void ResizePlain(Array* a, int32_t n, Init init)   // delete[] / new[] of plain elements (FUN_10052816 and kin)
{
    if (a->count == n) return;
    vc10::FreeArray(a->data);
    a->count = n;
    if (!n) {
        a->data = nullptr;
        return;
    }
    auto* p = static_cast<uint8_t*>(vc10::AllocateArray(size_t(uint32_t(n)) * Size));
    if (p)
        for (int32_t i = 0; i < n; ++i) init(p + i * Size);
    a->data = p;
}

void* NewCounted(int32_t n, size_t size)
{
    uint32_t* block = static_cast<uint32_t*>(vc10::AllocateArray(size_t(uint32_t(n)) * size + 4));
    block[0] = uint32_t(n);
    return block + 1;
}
uint32_t CountOf(void* a) { return static_cast<uint32_t*>(a)[-1]; }
void FreeCounted(void* a) { vc10::FreeArray(static_cast<uint32_t*>(a) - 1); }

template <size_t Size, typename Destroy>
void* DeleteCounted(void* a, uint8_t flags, Destroy destroy)   // a vector deleting destructor
{
    if (!(flags & 2)) {
        destroy(static_cast<uint8_t*>(a));
        if (flags & 1) vc10::Free(a);
        return a;
    }
    for (uint32_t i = CountOf(a); i-- > 0;) destroy(static_cast<uint8_t*>(a) + i * Size);
    if (flags & 1) FreeCounted(a);
    return static_cast<uint32_t*>(a) - 1;
}

void Move(String& to, String& from)                 // FUN_10017a6f: std::string move assignment
{
    if (&to == &from) return;
    to.release();
    if (from.capacity < 16) {
        std::memmove(to.buffer, from.buffer, from.size + 1);
    } else {
        to.pointer = from.pointer;
        from.pointer = nullptr;
    }
    to.size = from.size;
    to.capacity = from.capacity;
    from.size = 0;
    from.capacity = 15;
    from.buffer[0] = 0;
}

void InitString(String& s)
{
    s.init();
    s.allocator = 0;
}

// ---- CATTriPolyList_t ----

constexpr uint32_t kPieceSize = 0x34, kVertexSize = 0x44;

void* __fastcall PieceConstruct(uint8_t* p)
{
    std::memset(p, 0, kPieceSize);
    Field<int32_t>(p, 0x28) = -1;
    return p;
}

void __fastcall PieceDestroy(uint8_t* p)
{
    orig::RResource_t_ReleaseRResource(Field<void*>(p, 0));
    vc10::FreeArray(Field<void*>(p, 0x1C));
    vc10::FreeArray(Field<void*>(p, 0x20));
    if (void* o = Field<void*>(p, 0x18)) Virtual<void*>(o, 0, uint32_t(1));
    vc10::FreeArray(Field<void*>(p, 0x14));
    vc10::FreeArray(Field<void*>(p, 0x0C));
}

void* __fastcall PieceAssign(uint8_t* p, void*, const uint8_t* from)
{
    std::memcpy(p, from, kPieceSize);
    return p;
}

int32_t __fastcall GetNeighbour(uint8_t* p, void*, int32_t tri, uint8_t edge)
{
    return Field<int32_t*>(p, 0x1C)[tri * 3 + edge];
}
void __fastcall SetNeighbour(uint8_t* p, void*, int32_t tri, uint8_t edge, int32_t n)
{
    Field<int32_t*>(p, 0x1C)[tri * 3 + edge] = n;
}
void* __fastcall GetTriVertex(uint8_t* p, void*, int32_t i) { return Field<uint8_t*>(p, 0x0C) + i * kVertexSize; }

uint8_t __fastcall FindTriangleIndex(uint8_t* p, void*, int32_t tri, int32_t n)
{
    const int32_t* t = Field<int32_t*>(p, 0x1C) + tri * 3;
    return t[0] == n ? 0 : t[1] == n ? 1 : t[2] == n ? 2 : 0xFF;
}
uint8_t __fastcall FindVertexIndex(uint8_t* p, void*, int32_t tri, int32_t v)
{
    const int16_t* t = Field<int16_t*>(p, 0x14) + tri * 3;
    const int16_t s = int16_t(v);
    return t[0] == s ? 0 : t[1] == s ? 1 : t[2] == s ? 2 : 0xFF;
}
int32_t __fastcall GetVertexIndex(uint8_t* p, void*, int32_t tri, uint8_t i)
{
    return Field<uint16_t*>(p, 0x14)[tri * 3 + i];
}

void* __fastcall PiecesDelete(void* a, void*, uint8_t flags)   // FUN_10011c58
{
    return DeleteCounted<kPieceSize>(a, flags, [](uint8_t* p) { PieceDestroy(p); });
}

void __fastcall ResizePieces(Array* a, void*, int32_t n)   // FUN_10011ca2
{
    if (a->count == n) return;
    if (a->data) PiecesDelete(a->data, nullptr, 3);
    a->count = n;
    a->data = nullptr;
    if (n) {
        auto* p = static_cast<uint8_t*>(NewCounted(n, kPieceSize));
        for (int32_t i = 0; i < n; ++i) PieceConstruct(p + i * kPieceSize);
        a->data = p;
    }
}

void __fastcall ResizeVertices(Array* a, void*, int32_t n)   // FUN_10053a87: zeroed (the first 0x30 bytes)
{
    ResizePlain<kVertexSize>(a, n, [](uint8_t* v) { std::memset(v, 0, 0x30); });
}
void __fastcall ResizeIndices(Array* a, void*, int32_t n)    // FUN_10053a1c
{
    ResizePlain<2>(a, n, [](uint8_t*) {});
}
void __fastcall ResizeInts(Array* a, void*, int32_t n)       // FUN_10052816
{
    ResizePlain<4>(a, n, [](uint8_t*) {});
}
void __fastcall ResizeSpheres(Array* a, void*, int32_t n)    // FUN_10011da1
{
    ResizePlain<0x14>(a, n, [](uint8_t* s) { std::memset(s, 0, 12); });
}

// ---- CATGroup_t ----

constexpr uint32_t kGroupSize = 0x34, kAttractorSize = 0x40, kBoneSize = 0x28;

void* __fastcall AttractorConstruct(uint8_t* a)        // (0x10011fb9)
{
    InitString(Field<String>(a, 0));
    for (uint32_t o = 0x1C; o <= 0x30; o += 4) Field<float>(a, o) = 0.0f;
    Field<float>(a, 0x34) = 1.0f;
    return a;
}
void __fastcall AttractorDestroy(uint8_t* a) { Field<String>(a, 0).release(); }   // FUN_100178fe

void* __fastcall AttractorsDelete(void* a, void*, uint8_t flags)   // FUN_10012048
{
    return DeleteCounted<kAttractorSize>(a, flags, [](uint8_t* p) { AttractorDestroy(p); });
}

void __fastcall ResizeAttractors(Array* a, void*, int32_t n)   // FUN_10012096
{
    if (a->count == n) return;
    if (a->data) AttractorsDelete(a->data, nullptr, 3);
    a->count = n;
    a->data = nullptr;
    if (n) {
        auto* p = static_cast<uint8_t*>(NewCounted(n, kAttractorSize));
        for (int32_t i = 0; i < n; ++i) AttractorConstruct(p + i * kAttractorSize);
        a->data = p;
    }
}

void* __fastcall GroupConstruct(uint8_t* g)
{
    InitString(Field<String>(g, 0));
    Field<Array>(g, 0x1C) = {0, nullptr};
    ResizePieces(&Field<Array>(g, 0x1C), nullptr, 0);
    Field<Array>(g, 0x24) = {0, nullptr};
    Field<Array>(g, 0x2C) = {0, nullptr};
    ResizeAttractors(&Field<Array>(g, 0x2C), nullptr, 0);
    return g;
}

void __fastcall GroupDestroy(uint8_t* g)
{
    if (void* a = Field<void*>(g, 0x30)) AttractorsDelete(a, nullptr, 3);
    vc10::FreeArray(Field<void*>(g, 0x28));
    if (void* p = Field<void*>(g, 0x20)) PiecesDelete(p, nullptr, 3);
    Field<String>(g, 0).release();
}

void* __fastcall GroupCopy(uint8_t* g, void*, const uint8_t* from)   // (shallow, as the original)
{
    const String& s = Field<String>(const_cast<uint8_t*>(from), 0);
    InitString(Field<String>(g, 0));
    Field<String>(g, 0).assign(s.c_str(), s.size);
    std::memcpy(g + 0x1C, from + 0x1C, 0x18);
    return g;
}

void* __fastcall GroupAssign(uint8_t* g, void*, const uint8_t* from)
{
    if (g != from) {
        const String& s = Field<String>(const_cast<uint8_t*>(from), 0);
        Field<String>(g, 0).assign(s.c_str(), s.size);
    }
    std::memcpy(g + 0x1C, from + 0x1C, 0x18);
    return g;
}

int32_t __fastcall GroupColSphereCount(uint8_t* g) { return Field<int32_t>(g, 0x24); }
void* __fastcall GroupColSphere(uint8_t* g, void*, int32_t i) { return Field<uint8_t*>(g, 0x28) + i * 0x14; }
void* __fastcall GroupAttractor(uint8_t* g, void*, int32_t i) { return Field<uint8_t*>(g, 0x30) + i * kAttractorSize; }
int32_t __fastcall GroupPieceCount(uint8_t* g) { return Field<int32_t>(g, 0x1C); }
void* __fastcall GroupPiece(uint8_t* g, void*, int32_t i) { return Field<uint8_t*>(g, 0x20) + i * kPieceSize; }

template <uint32_t Offset, bool Thirds = false>
uint32_t SumPieces(uint8_t* g)
{
    uint32_t n = 0;
    uint8_t* p = Field<uint8_t*>(g, 0x20);
    for (int32_t i = Field<int32_t>(g, 0x1C); i > 0; --i, p += kPieceSize)
        n += Thirds ? uint32_t(Field<int32_t>(p, Offset) / 3) : Field<uint32_t>(p, Offset);
    return n;
}
uint32_t __fastcall GroupTotalVertices(uint8_t* g) { return SumPieces<0x08>(g); }
uint32_t __fastcall GroupTotalTriangles(uint8_t* g) { return SumPieces<0x10, true>(g); }
uint32_t __fastcall GroupActiveVertices(uint8_t* g) { return SumPieces<0x2C>(g); }
uint32_t __fastcall GroupActiveTriangles(uint8_t* g) { return SumPieces<0x30>(g); }

void* __fastcall GroupsDelete(void* a, void*, uint8_t flags)   // FUN_10053c0e
{
    return DeleteCounted<kGroupSize>(a, flags, [](uint8_t* p) { GroupDestroy(p); });
}

void __fastcall ResizeGroups(Array* a, void*, int32_t n)   // FUN_10053c67
{
    if (a->count == n) return;
    void* fresh = nullptr;
    if (a->data) GroupsDelete(a->data, nullptr, 3);
    a->count = n;
    if (n) {
        auto* p = static_cast<uint8_t*>(NewCounted(n, kGroupSize));
        for (int32_t i = 0; i < n; ++i) GroupConstruct(p + i * kGroupSize);
        fresh = p;
    }
    a->data = fresh;
}

// ---- bones ----

void* __fastcall BoneConstruct(uint8_t* b)              // FUN_10053aea
{
    InitString(Field<String>(b, 0));
    Field<Array>(b, 0x20) = {0, nullptr};
    return b;
}
void __fastcall BoneDestroy(uint8_t* b)                 // FUN_10053b17
{
    vc10::FreeArray(Field<void*>(b, 0x24));
    Field<String>(b, 0).release();
}
void* __fastcall BonesDelete(void* a, void*, uint8_t flags)   // FUN_10053b30
{
    return DeleteCounted<kBoneSize>(a, flags, [](uint8_t* p) { BoneDestroy(p); });
}
void __fastcall ResizeBones(Array* a, void*, int32_t n)   // FUN_10053b89
{
    if (a->count == n) return;
    void* fresh = nullptr;
    if (a->data) BonesDelete(a->data, nullptr, 3);
    a->count = n;
    if (n) {
        auto* p = static_cast<uint8_t*>(NewCounted(n, kBoneSize));
        for (int32_t i = 0; i < n; ++i) BoneConstruct(p + i * kBoneSize);
        fresh = p;
    }
    a->data = fresh;
}

// ---- CATError_t ----

[[noreturn]] void ThrowCatError(const char* text)
{
    String message;
    InitString(message);
    message.assign(text, std::strlen(text));
    alignas(8) uint8_t error[0x10];
    using CtorFn = void*(__fastcall*)(void*, void*, const String*);
    reinterpret_cast<CtorFn>(GetProcAddress(
        g_orig, "??0CATError_t@@QAE@ABV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z"))(
        error, nullptr, &message);
    message.release();
    using ThrowFn = void(__stdcall*)(void*, void*);
    static const auto cxxThrow =
        reinterpret_cast<ThrowFn>(GetProcAddress(GetModuleHandleA("msvcr100.dll"), "_CxxThrowException"));
    cxxThrow(error, reinterpret_cast<uint8_t*>(g_orig) + 0xA7530);
    __builtin_unreachable();
}

// ---- CATMesh_t ----

constexpr uint32_t g_meshVtable = 0x95D44;
constexpr uint32_t kAnimRadius = 0x2C, kMaxRadius = 0x34, kMaterials = 0x38, kBones = 0x40, kGroups = 0x48,
                   kSpheres = 0x50, kSkeleton = 0x58, kVolume = 0x5C;

// FUN_10052ea8: a texture by name from the texture paths - stubbed out in this build: none.
void* TextureFor(const void* paths)
{
    (void)paths;
    return nullptr;
}

struct Reader {
    void* io;
    int32_t Int() { return Virtual<int32_t>(io, 5); }
    uint16_t Short() { return Virtual<uint16_t>(io, 7); }
    float Float() { return Virtual<float>(io, 11); }
    void Str(String* out) { Virtual<void*>(io, 13, out); }
};

void __fastcall MeshDestroy(uint8_t* m);

void* __fastcall MeshConstruct(uint8_t* m, void*, void* io, const vc10::Vector<String>* paths)
{
    Internal<void*(__fastcall*)(void*, void*, const char*)>(0x46311)(m, nullptr, "CATMesh_t");
    SetVtable(m, g_meshVtable);
    Field<Array>(m, kMaterials) = {0, nullptr};
    Field<Array>(m, kBones) = {0, nullptr};
    ResizeBones(&Field<Array>(m, kBones), nullptr, 0);
    Field<Array>(m, kGroups) = {0, nullptr};
    ResizeGroups(&Field<Array>(m, kGroups), nullptr, 0);
    Field<Array>(m, kSpheres) = {0, nullptr};
    Field<void*>(m, kVolume) = nullptr;
    Field<float>(m, kMaxRadius) = 0.0f;
    m[0x60] = 0;
    Reader in{io};
    if (in.Int() != 4) {
        MeshDestroy(m);
        ThrowCatError("file is not a CATMesh");
    }
    const int32_t version = in.Int();
    if (version != 0x104 && version != 0x67) {
        MeshDestroy(m);
        ThrowCatError("file is not a supported version");
    }
    Field<int32_t>(m, kSkeleton) = in.Int();
    Field<float>(m, kAnimRadius) = in.Float();
    Field<float>(m, kAnimRadius + 4) = in.Float();
    ResizeInts(&Field<Array>(m, kMaterials), nullptr, in.Int());
    for (int32_t i = 0; i < Field<int32_t>(m, kMaterials); ++i) {
        String name, texture, env;
        in.Str(&name);
        const uint32_t flags = uint32_t(in.Int());
        in.Str(&texture);
        InitString(env);
        if (flags & 2) {
            String e;
            in.Str(&e);
            Move(env, e);
            e.release();
        }
        float c[15];
        for (float& v : c) v = in.Float();
        // diffuse, specular, ambient, emissive, then power and two values (the file's order).
        const float diffuse[3] = {c[0], c[1], c[2]}, specular[3] = {c[3], c[4], c[5]}, ambient[3] = {c[6], c[7], c[8]},
                    emissive[3] = {c[9], c[10], c[11]};
        void* tex = nullptr;
        if (texture.size != 0 && paths->size() != 0) tex = TextureFor(paths);
        void* material = orig::RMaterial_t_RMaterial_t_31(vc10::Allocate(0xC0), name.c_str(), tex, diffuse, ambient,
                                                          specular, emissive, c[12], c[13], c[14], (flags & 1) != 0,
                                                          ((flags >> 3) & 1) == 0);
        static_cast<void**>(Field<Array>(m, kMaterials).data)[i] = material;
        if (tex) orig::RResource_t_ReleaseRResource(tex);   // (RTexture_t::ReleaseRTexture)
        env.release();
        texture.release();
        name.release();
    }
    ResizeSpheres(&Field<Array>(m, kSpheres), nullptr, in.Int());
    for (int32_t i = 0; i < Field<int32_t>(m, kSpheres); ++i) {
        float* s = static_cast<float*>(Field<Array>(m, kSpheres).data) + i * 5;
        s[0] = in.Float();
        s[1] = in.Float();
        s[2] = in.Float();
        const float r = in.Float();
        reinterpret_cast<int32_t*>(s)[4] = -1;
        s[3] = r;
    }
    ResizeBones(&Field<Array>(m, kBones), nullptr, in.Int());
    for (int32_t i = 0; i < Field<int32_t>(m, kBones); ++i) {
        uint8_t* b = static_cast<uint8_t*>(Field<Array>(m, kBones).data) + i * kBoneSize;
        String name;
        in.Str(&name);
        Move(Field<String>(b, 0), name);
        name.release();
        Field<float>(b, 0x1C) = in.Float();
        ResizeInts(&Field<Array>(b, 0x20), nullptr, in.Int());
        for (int32_t k = 0; k < Field<int32_t>(b, 0x20); ++k) static_cast<int32_t*>(Field<void*>(b, 0x24))[k] = in.Int();
    }
    ResizeGroups(&Field<Array>(m, kGroups), nullptr, in.Int());
    float& farthest = Field<float>(m, kMaxRadius);
    for (int32_t gi = 0; gi < Field<int32_t>(m, kGroups); ++gi) {
        uint8_t* g = static_cast<uint8_t*>(Field<Array>(m, kGroups).data) + gi * kGroupSize;
        String name;
        in.Str(&name);
        Move(Field<String>(g, 0), name);
        name.release();
        ResizePieces(&Field<Array>(g, 0x1C), nullptr, in.Int());
        for (int32_t pi = 0; pi < Field<int32_t>(g, 0x1C); ++pi) {
            uint8_t* p = Field<uint8_t*>(g, 0x20) + pi * kPieceSize;
            const int32_t mi = in.Int();
            if (mi < 0 || Field<int32_t>(m, kMaterials) < mi) {
                MeshDestroy(m);
                ThrowCatError("invalid material index");
            }
            void* material = static_cast<void**>(Field<Array>(m, kMaterials).data)[mi];
            Field<void*>(p, 0) = material;
            orig::RResource_t_AddRefRResource(material);
            Field<float>(material, 0x60) = Field<float>(material, 0x60) * 100.0f;
            orig::RMaterial_t_UpdateSpecular(material);
            Field<uint8_t>(material, 0x6C) = 0;
            ResizeVertices(&Field<Array>(p, 0x08), nullptr, in.Int());
            Field<int32_t>(p, 0x2C) = Field<int32_t>(p, 0x08);
            for (int32_t vi = 0; vi < Field<int32_t>(p, 0x08); ++vi) {
                float* v = reinterpret_cast<float*>(Field<uint8_t*>(p, 0x0C) + vi * kVertexSize);
                for (int k = 0; k < 6; ++k) v[k] = in.Float();
                // FUN_10016851's sum in the x87's precision, rounded once.
                auto lengthSq = [](const float* p) {
                    return float((double(p[1]) * p[1] + double(p[0]) * p[0]) + double(p[2]) * p[2]);
                };
                float d = lengthSq(v);
                if (farthest < d) farthest = d;
                d = lengthSq(v + 3);
                if (farthest < d) farthest = d;
                if (version < 0x68) {
                    v[6] = v[7] = v[8] = 0.0f;
                } else {
                    v[6] = in.Float();
                    v[7] = in.Float();
                    v[8] = in.Float();
                }
                for (int k = 9; k < 14; ++k) v[k] = in.Float();
                reinterpret_cast<int32_t*>(v)[14] = in.Int();
                reinterpret_cast<int32_t*>(v)[15] = in.Int();
                v[16] = in.Float();
            }
            ResizeIndices(&Field<Array>(p, 0x10), nullptr, in.Int());
            Field<int32_t>(p, 0x30) = Field<int32_t>(p, 0x10) / 3;
            for (int32_t k = 0; k < Field<int32_t>(p, 0x10); ++k) Field<uint16_t*>(p, 0x14)[k] = in.Short();
        }
        ResizeSpheres(&Field<Array>(g, 0x24), nullptr, in.Int());
        for (int32_t k = 0; k < Field<int32_t>(g, 0x24); ++k) {
            float* s = Field<float*>(g, 0x28) + k * 5;
            for (int j = 0; j < 4; ++j) s[j] = in.Float();
            reinterpret_cast<int32_t*>(s)[4] = in.Int();
        }
        ResizeAttractors(&Field<Array>(g, 0x2C), nullptr, in.Int());
        for (int32_t k = 0; k < Field<int32_t>(g, 0x2C); ++k) {
            uint8_t* a = Field<uint8_t*>(g, 0x30) + k * kAttractorSize;
            String aname;
            in.Str(&aname);
            Move(Field<String>(a, 0), aname);
            aname.release();
            for (uint32_t o = 0x1C; o <= 0x38; o += 4) Field<float>(a, o) = in.Float();
            Field<int32_t>(a, 0x3C) = in.Int();
        }
    }
    farthest = float(std::sqrt(double(farthest)));
    Internal<void(__fastcall*)(void*)>(0x52F16)(m);   // CalcBoundingVolume (ours)
    return m;
}

void __fastcall MeshDestroy(uint8_t* m)                // FUN_10052e15
{
    SetVtable(m, g_meshVtable);
    Array& materials = Field<Array>(m, kMaterials);
    for (int32_t i = 0; i < materials.count; ++i)
        orig::RResource_t_ReleaseRResource(static_cast<void**>(materials.data)[i]);
    if (void* v = Field<void*>(m, kVolume)) Virtual<void*>(v, 0, uint32_t(1));
    vc10::FreeArray(Field<void*>(m, kSpheres + 4));
    if (void* g = Field<void*>(m, kGroups + 4)) GroupsDelete(g, nullptr, 3);
    if (void* b = Field<void*>(m, kBones + 4)) BonesDelete(b, nullptr, 3);
    vc10::FreeArray(materials.data);
    Internal<void(__fastcall*)(void*)>(0x46283)(m);   // ~RResource_t
}

void* __fastcall MeshDelete(uint8_t* m, void*, uint8_t flags)   // vtable slot 0 (FUN_10053cec)
{
    MeshDestroy(m);
    if (flags & 1) vc10::Free(m);
    return m;
}

void __fastcall SetMaterial(uint8_t* m, void*, int32_t i, void* material)
{
    orig::RResource_t_AddRefRResource(material);
    void*& slot = static_cast<void**>(Field<Array>(m, kMaterials).data)[i];
    orig::RResource_t_ReleaseRResource(slot);
    slot = material;
    Field<float>(material, 0x60) = Field<float>(material, 0x60) * 100.0f;
    orig::RMaterial_t_UpdateSpecular(material);
    Field<uint8_t>(material, 0x6C) = 0;
}

template <uint32_t(__fastcall* Count)(uint8_t*)>
uint32_t SumGroups(uint8_t* m)
{
    uint32_t n = 0;
    uint8_t* g = static_cast<uint8_t*>(Field<Array>(m, kGroups).data);
    for (int32_t i = Field<int32_t>(m, kGroups); i > 0; --i, g += kGroupSize) n += Count(g);
    return n;
}
uint32_t __fastcall MeshTotalVertices(uint8_t* m) { return SumGroups<GroupTotalVertices>(m); }
uint32_t __fastcall MeshActiveVertices(uint8_t* m) { return SumGroups<GroupActiveVertices>(m); }   // FUN_10052d29
uint32_t __fastcall MeshActiveTriangles(uint8_t* m) { return SumGroups<GroupActiveTriangles>(m); }   // FUN_10052d4f

// CATMesh_t::CalcBoundingVolume: of every vertex's rest position.
void __fastcall CalcBoundingVolume(uint8_t* m)
{
    struct V3 {
        float x, y, z;
    };
    vc10::Vector<V3> points{};
    void* volume = nullptr;
    if (0 < Field<int32_t>(m, kGroups)) {
        uint8_t* g = static_cast<uint8_t*>(Field<Array>(m, kGroups).data);
        for (int32_t gi = 0; gi < Field<int32_t>(m, kGroups); ++gi, g += kGroupSize)
            for (int32_t pi = 0; pi < Field<int32_t>(g, 0x1C); ++pi) {
                uint8_t* p = Field<uint8_t*>(g, 0x20) + pi * kPieceSize;
                for (int32_t vi = 0; vi < Field<int32_t>(p, 0x08); ++vi) {
                    V3 v;
                    std::memcpy(&v, Field<uint8_t*>(p, 0x0C) + vi * kVertexSize + 0x18, 12);
                    points.push_back(v);
                }
            }
        if (points.first != points.last)
            volume = Internal<void*(__fastcall*)(void*, void*, const void*, uint32_t, uint32_t)>(0x17FEE)(
                vc10::Allocate(0x30), nullptr, points.first, uint32_t(points.size()), 0xC);
    }
    Field<void*>(m, kVolume) = volume;
    points.release();
}

// FUN_10053e97: CATError_t's constructor (a std::runtime_error), installing the game class's own vtable.
void* __fastcall CATErrorConstruct(void* self, void*, void* str)
{
    Internal<void(__fastcall*)(void*, void*, void*)>(0x53EB2)(self, nullptr, str);   // std::runtime_error(string)
    SetVtable(self, 0x95D30);
    return self;
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
        {0x52D75, FN(PieceConstruct), "CATTriPolyList_t::CATTriPolyList_t"},
        {0x52B91, FN(PieceDestroy), "CATTriPolyList_t::~CATTriPolyList_t"},
        {0x11B40, FN(PieceAssign), "CATTriPolyList_t::operator="},
        {0x11B57, FN(GetNeighbour), "CATTriPolyList_t::GetNeighbour"},
        {0x11B70, FN(SetNeighbour), "CATTriPolyList_t::SetNeighbour"},
        {0x11D2B, FN(GetTriVertex), "CATTriPolyList_t::GetTriVertex"},
        {0x52B0F, FN(FindTriangleIndex), "CATTriPolyList_t::FindTriangleIndex"},
        {0x52B42, FN(FindVertexIndex), "CATTriPolyList_t::FindVertexIndex"},
        {0x52B77, FN(GetVertexIndex), "CATTriPolyList_t::GetVertexIndex"},
        {0x11C58, FN(PiecesDelete), "CATTriPolyList_t array deleting (FUN_10011c58)"},
        {0x11CA2, FN(ResizePieces), "CATGroup_t pieces resize (FUN_10011ca2)"},
        {0x53A87, FN(ResizeVertices), "CATTriPolyList_t vertices resize (FUN_10053a87)"},
        {0x53A1C, FN(ResizeIndices), "CATTriPolyList_t indices resize (FUN_10053a1c)"},
        {0x52816, FN(ResizeInts), "int array resize (FUN_10052816)"},
        {0x11DA1, FN(ResizeSpheres), "collision spheres resize (FUN_10011da1)"},
        {0x11FB9, FN(AttractorConstruct), "CATAttractor_t (0x10011fb9)"},
        {0x178FE, FN(AttractorDestroy), "CATAttractor_t destructor (FUN_100178fe)"},
        {0x12048, FN(AttractorsDelete), "CATAttractor_t array deleting (FUN_10012048)"},
        {0x12096, FN(ResizeAttractors), "CATGroup_t attractors resize (FUN_10012096)"},
        {0x121DB, FN(GroupConstruct), "CATGroup_t::CATGroup_t"},
        {0x12234, FN(GroupDestroy), "CATGroup_t::~CATGroup_t"},
        {0x1228D, FN(GroupCopy), "CATGroup_t::CATGroup_t(copy)"},
        {0x122C9, FN(GroupAssign), "CATGroup_t::operator="},
        {0x11D3F, FN(GroupColSphere), "CATGroup_t::GetColSphere"},
        {0x11D4F, FN(GroupAttractor), "CATGroup_t::GetAttractor"},
        {0x11D63, FN(GroupPiece), "CATGroup_t::GetTriPolyList"},
        {0x52BEE, FN(GroupTotalVertices), "CATGroup_t::GetTotalVertexCount"},
        {0x52C06, FN(GroupTotalTriangles), "CATGroup_t::GetTotalTriangleCount"},
        {0x52C30, FN(GroupActiveVertices), "CATGroup_t::GetActiveVertexCount"},
        {0x52C48, FN(GroupActiveTriangles), "CATGroup_t::GetActiveTriangleCount"},
        {0x53C0E, FN(GroupsDelete), "CATGroup_t array deleting (FUN_10053c0e)"},
        {0x53C67, FN(ResizeGroups), "CATMesh_t groups resize (FUN_10053c67)"},
        {0x53AEA, FN(BoneConstruct), "CATMesh_t bone (FUN_10053aea)"},
        {0x53B17, FN(BoneDestroy), "CATMesh_t bone destructor (FUN_10053b17)"},
        {0x53B30, FN(BonesDelete), "CATMesh_t bones deleting (FUN_10053b30)"},
        {0x53B89, FN(ResizeBones), "CATMesh_t bones resize (FUN_10053b89)"},
        {0x52FF8, FN(MeshConstruct), "CATMesh_t::CATMesh_t (the loader)"},
        {0x52E15, FN(MeshDestroy), "CATMesh_t::~CATMesh_t (FUN_10052e15)"},
        {0x53CEC, FN(MeshDelete), "CATMesh_t deleting destructor (FUN_10053cec)"},
        {0x52C99, FN(SetMaterial), "CATMesh_t::SetMaterial"},
        {0x52CDD, FN(MeshTotalVertices), "CATMesh_t::GetTotalVertexCount"},
        {0x52D29, FN(MeshActiveVertices), "CATMesh_t active vertices (FUN_10052d29)"},
        {0x52D4F, FN(MeshActiveTriangles), "CATMesh_t active triangles (FUN_10052d4f)"},
        {0x52F16, FN(CalcBoundingVolume), "CATMesh_t::CalcBoundingVolume"},
        {0x53E97, FN(CATErrorConstruct), "CATError_t::CATError_t"},
    };
#undef FN
    int installed = 0;
    for (const Entry& e : entries) installed += Replace(orig, e.rva, e.target, e.what) ? 1 : 0;
    Log("character data: %d of %d functions native", installed, int(sizeof(entries) / sizeof(entries[0])));
}

}  // namespace rnative::catdata
