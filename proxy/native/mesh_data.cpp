// The data behind static meshes natively (randy-vk.ini [Native] Scene=on): TriList (a mesh's triangles), RTriList_t
// (its vertex buffers and triangle list), SimpleMesh (one material's part of a mesh) and RVisualData_t (a mesh: its
// SimpleMeshes, kept in a registry for freeing hardware buffers of meshes not drawn lately) - made, copied, loaded
// from and written to archives, destroyed; their vertex / triangle accessors; mirroring.
//
// TriList (0x18 bytes, a Serializable_c, vtable 0x1008AA00): +0x08 std::vector<unsigned short> (three a triangle).
// RTriList_t (0x4C bytes, an RResource_t, vtable 0x957A8): +0x30 TriList*, +0x34 vertex count, +0x38 triangle count,
// +0x3C flags (1: a system memory vertex buffer, 2: and a hardware copy), +0x40 FVF, +0x44 the hardware copy (or the
// only buffer), +0x48 the system memory one.
// SimpleMesh (0x24 bytes, a Serializable_c, vtable 0x95A14): +0x08 per-triangle vectors (or null), +0x0C a 4-byte
// array of +0x1C (u16) entries (or null), +0x10 -1, +0x14 RMaterial_t*, +0x18 BVolume_t*, +0x1E not on the
// hardware level 2 (only system memory), +0x20 RTriList_t*.
// RVisualData_t (0x68 bytes, an RResource_t, vtable 0x95A58): +0x2C anim position, +0x38 anim rotation, +0x48
// degenerate, +0x4C std::vector<SimpleMesh*>, +0x5C the restore count of its buffers, +0x60 the frame it was last
// drawn, +0x64 its slot in the registry (0x101E2368 std::vector, 0x101E2364 slots used).
// RTriMeshData_t (0x70 bytes, an RVisualData_t, vtable 0x958D0 - the type a static mesh's data really is): +0x68 a
// lazily-made private RVisualData_t (its meshes' materials turned into lightmap delta states), +0x6C its BVolume_t*
// (from the archive, or recomputed from the vertices). FAFTriMeshData_t is the scene reader's subclass (vtable
// 0x8A9EC, same layout).
#include "native/helpers.h"
#include "native/mesh_data.h"
#include "native/orig_api.gen.h"
#include "native/serialize.h"
#include "native/vc10.h"

#include <cmath>
#include <cstring>
#include <initializer_list>
#include <utility>

namespace rnative::meshdata {

namespace {

HMODULE g_orig;
void* const* g_randy;

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
void DeleteObject(void* o) { Virtual<void*>(o, 0, uint32_t(1)); }
void SetVtable(void* o, uint32_t rva) { Field<uintptr_t>(o, 0) = reinterpret_cast<uintptr_t>(g_orig) + rva; }

const serialize::Api& S() { return serialize::Get(); }

constexpr uint32_t kTriListVtable = 0x8AA00, kRTriListVtable = 0x957A8, kSimpleMeshVtable = 0x95A14,
                   kVisualDataVtable = 0x95A58;
constexpr uint32_t kHardwareLevel = 0xB772C, kRestoreCount = 0x17D334, kRegistry = 0x1E2368, kRegistryUsed = 0x1E2364;
constexpr uint32_t kFrame = 0x274;   // Randy_t
// TD addresses for __RTDynamicCast: Serializable_c to ...
constexpr uint32_t kSerializableTd = 0xB60D4, kTriListTd = 0xB65BC, kSimpleMeshTd = 0xB7A68, kMaterialTd = 0xB617C;

using Indices = vc10::Vector<uint16_t>;
struct V3 {
    float x, y, z;
};

void* DynamicCast(void* object, uint32_t td)
{
    using DynamicCastFn = void*(__cdecl*)(void*, long, void*, void*, int);
    static const auto cast =
        reinterpret_cast<DynamicCastFn>(GetProcAddress(GetModuleHandleA("msvcr100.dll"), "__RTDynamicCast"));
    return cast(object, 0, &Global<uint8_t>(kSerializableTd), &Global<uint8_t>(td), 0);
}

// ArchiveStream_c::FindObject<T> (FUN_10048bbe, FUN_1004f8ab, FUN_10013d19): the object `name` #index as a T.
int32_t FindObject(void* stream, const char* name, uint32_t td, void** out, int32_t index)
{
    void* object = nullptr;
    int32_t error = S().findObject(stream, nullptr, name, &object, index);
    if (error) return error;
    void* t = object ? DynamicCast(object, td) : nullptr;
    if (object && !t) return 1;
    *out = t;
    return 0;
}

void* NewVertexBuffer() { return vc10::Allocate(4); }   // VertexBuffer_c: a pointer to its implementation

// ---- TriList ----

Indices& Triangles(void* t) { return Field<Indices>(t, 8); }

uint64_t g_indexGeneration = 1;                     // IndexGeneration: bumped by every TriList writer below
bool g_indexTracked;                                // ... all of which are installed
void IndicesChanged() { ++g_indexGeneration; }

void CopyIndices(Indices& to, const uint16_t* from, size_t count)   // vector(range): exactly as many
{
    to.first = to.last = to.end = nullptr;
    to.allocator = 0;
    if (!count) return;
    to.reserve(count);
    std::memcpy(to.first, from, count * 2);
    to.last = to.first + count;
}

void* __fastcall TriListDelete(void* t, void*, uint8_t flags)   // vtable slot 0 (FUN_100188fb)
{
    IndicesChanged();                               // its address may hold another list next
    Triangles(t).release();
    S().destroy(t, nullptr);
    if (flags & 1) vc10::Free(t);
    return t;
}

void __fastcall TriListAddTriangle(void* t, void*, int32_t a, int32_t b, int32_t c)
{
    IndicesChanged();
    Triangles(t).push_back(uint16_t(a));
    Triangles(t).push_back(uint16_t(b));
    Triangles(t).push_back(uint16_t(c));
}

void* __fastcall TriListConstructFrom(void* t, void*, void* archive)   // FUN_1001fedf
{
    IndicesChanged();
    S().constructFrom(t, nullptr, archive);
    SetVtable(t, kTriListVtable);
    Indices& v = Triangles(t);
    v.first = v.last = v.end = nullptr;
    void* stream = S().getStream(archive, nullptr);
    const void* data = nullptr;
    int32_t bytes = 0;
    S().findData(stream, nullptr, "triangles", &data, &bytes, 0);
    CopyIndices(v, static_cast<const uint16_t*>(data), size_t(bytes) / 2);
    return t;
}

void* __cdecl TriListInstantiate(void* archive) { return TriListConstructFrom(vc10::Allocate(0x18), nullptr, archive); }

void* __fastcall TriListCopy(void* t, void*, void* from)   // FUN_10048cdf
{
    IndicesChanged();
    S().construct(t, nullptr);
    SetVtable(t, kTriListVtable);
    const Indices& v = Triangles(from);
    CopyIndices(Triangles(t), v.first, v.size());
    return t;
}

void __fastcall TriListFlip(void* t)                  // FUN_1001fe30: every triangle's winding reversed
{
    IndicesChanged();
    for (uint16_t* i = Triangles(t).first; i != Triangles(t).last; i += 3) std::swap(i[1], i[2]);
}

// FUN_1001fd66: triangle `index`'s three positions from vertices at `base`, `stride` apart.
void __fastcall TriListTriangle(void* t, void*, int32_t index, float* out, uint8_t* base, int32_t stride)
{
    const uint16_t* i = Triangles(t).first + index * 3;
    for (int k = 0; k < 3; ++k) std::memcpy(out + 3 * k, base + uint32_t(i[k]) * uint32_t(stride), 12);
}

// ---- RTriList_t ----

constexpr uint32_t kTriList = 0x30, kVertices = 0x34, kTriangleCount = 0x38, kFlags = 0x3C, kFvf = 0x40,
                   kHardware = 0x44, kSystem = 0x48;

void* Buffer(void* d)
{
    void* vb = Field<void*>(d, kSystem);
    return vb ? vb : Field<void*>(d, kHardware);
}
void RestoreHardware(void* d) { Internal<void*(__fastcall*)(void*)>(0x4872B)(d); }
void SetCounted(void* r, int32_t value) { Internal<void(__fastcall*)(void*, void*, int32_t)>(0x4624F)(r, nullptr, value); }

void* __fastcall RTriListConstruct(void* d, void*, uint32_t flags)   // FUN_1004866f
{
    Internal<void*(__fastcall*)(void*, void*, const char*)>(0x46311)(d, nullptr, "RTriList_t");
    Field<uint32_t>(d, kFlags) = flags;
    SetVtable(d, kRTriListVtable);
    Field<void*>(d, kTriList) = nullptr;
    Field<uint32_t>(d, kVertices) = 0;
    Field<uint32_t>(d, kTriangleCount) = 0;
    Field<uint32_t>(d, kFvf) = 0;
    Field<void*>(d, kHardware) = nullptr;
    Field<void*>(d, kSystem) = nullptr;
    SetCounted(d, 1);
    return d;
}

void* __fastcall RTriListCopy(void* d, void*, void* from)   // FUN_10048ad9
{
    Internal<void*(__fastcall*)(void*, void*, void*)>(0x462AD)(d, nullptr, from);
    SetVtable(d, kRTriListVtable);
    Field<void*>(d, kTriList) = nullptr;
    std::memcpy(static_cast<uint8_t*>(d) + kVertices, static_cast<uint8_t*>(from) + kVertices, 16);
    Field<void*>(d, kHardware) = nullptr;
    Field<void*>(d, kSystem) = nullptr;
    if (void* t = Field<void*>(from, kTriList)) Field<void*>(d, kTriList) = TriListCopy(vc10::Allocate(0x18), nullptr, t);
    SetCounted(d, 1);
    if (void* vb = Field<void*>(from, kSystem))
        Field<void*>(d, kSystem) = orig::VertexBuffer_c_VertexBuffer_c_58(NewVertexBuffer(), vb);
    if (void* vb = Field<void*>(from, kHardware))
        Field<void*>(d, kHardware) = orig::VertexBuffer_c_VertexBuffer_c_58(NewVertexBuffer(), vb);
    return d;
}

void __fastcall RTriListDestroy(void* d)              // FUN_100486c7
{
    SetVtable(d, kRTriListVtable);
    if (void* t = Field<void*>(d, kTriList)) {
        DeleteObject(t);
        Field<void*>(d, kTriList) = nullptr;
    }
    for (uint32_t at : {kHardware, kSystem})
        if (void* vb = Field<void*>(d, at)) {
            orig::VertexBuffer_c_Release(vb);
            Field<void*>(d, at) = nullptr;
        }
    Internal<void(__fastcall*)(void*)>(0x46283)(d);   // ~RResource_t
}

void* __fastcall RTriListDelete(void* d, void*, uint8_t flags)   // vtable slot 0 (FUN_10048c07)
{
    RTriListDestroy(d);
    if (flags & 1) vc10::Free(d);
    return d;
}

void __fastcall RTriListSetFlags(void* d, void*, uint32_t flags)   // FUN_10048787: make the buffers they ask for
{
    if ((flags & 2) && !Field<void*>(d, kHardware) && Field<void*>(d, kSystem))
        Field<void*>(d, kHardware) =
            orig::VertexBuffer_c_VertexBuffer_c_59(NewVertexBuffer(), Field<void*>(d, kSystem), 8, 0);
    if ((flags & 1) && !Field<void*>(d, kSystem))
        Field<void*>(d, kSystem) =
            orig::VertexBuffer_c_VertexBuffer_c_59(NewVertexBuffer(), Field<void*>(d, kHardware), 0, 2);
    Field<uint32_t>(d, kFlags) = flags;
}

bool __fastcall RTriListReleaseHardware(void* d)      // FUN_1004881d: the hardware copy, when there's another
{
    if (!Field<void*>(d, kSystem) || !Field<void*>(d, kHardware)) return false;
    orig::VertexBuffer_c_Release(Field<void*>(d, kHardware));
    Field<void*>(d, kHardware) = nullptr;
    return true;
}

void __fastcall RTriListArchiveVertices(void* d, void*, void* message)   // FUN_1004883e
{
    void* vb = Buffer(d);
    uint32_t desc[4] = {0x10, 0, 0, 0};
    desc[3] = orig::VertexBuffer_c_GetNumVertices(vb);
    desc[2] = orig::VertexBuffer_c_GetFormat(vb);
    S().addData(message, nullptr, "vb_desc", desc, 0x10, true, 1);
    void* vertices = orig::VertexBuffer_c_Lock(vb, 0, 0x10);
    S().addData(message, nullptr, "vertices", vertices, int32_t(orig::VertexBuffer_c_GetSize(vb)), false, 1);
    orig::VertexBuffer_c_Unlock(vb);
}

void __fastcall RTriListSetTriList(void* d, void*, void* t)   // FUN_100488bd
{
    if (void* old = Field<void*>(d, kTriList)) DeleteObject(old);
    Field<void*>(d, kTriList) = t;
    Field<uint32_t>(d, kTriangleCount) = uint32_t(Triangles(t).size()) / 3;
}

void __fastcall RTriListSetVertices(void* d, void*, uint32_t fvf, const void* data, uint32_t bytes)   // FUN_100488ed
{
    void* vb = orig::VertexBuffer_c_VertexBuffer_c_61(NewVertexBuffer(), fvf, 0, 2, bytes);
    const uint32_t size = orig::VertexBuffer_c_GetFormatSize(fvf);
    Field<uint32_t>(d, kFvf) = fvf;
    Field<uint32_t>(d, kVertices) = bytes / size;
    std::memcpy(orig::VertexBuffer_c_Lock(vb, 0, 0), data, bytes);
    orig::VertexBuffer_c_Unlock(vb);
    if (void* old = Buffer(d)) orig::VertexBuffer_c_Release(old);
    if (!(Field<uint32_t>(d, kFlags) & 1)) {
        Field<void*>(d, kHardware) = vb;
    } else {
        Field<void*>(d, kSystem) = vb;
        RestoreHardware(d);
    }
}

void __fastcall RTriListConvert(void* d, void*, uint32_t fvf)   // FUN_1004899a: the vertices in another format
{
    if (Field<uint32_t>(d, kFvf) == fvf) return;
    void* vb = orig::VertexBuffer_c_VertexBuffer_c_60(NewVertexBuffer(), Buffer(d), fvf, 0, 2);
    if (!(Field<uint32_t>(d, kFlags) & 1)) {
        if (void* old = Field<void*>(d, kHardware)) orig::VertexBuffer_c_Release(old);
        Field<void*>(d, kHardware) = vb;
    } else {
        if (void* old = Field<void*>(d, kSystem)) orig::VertexBuffer_c_Release(old);
        Field<void*>(d, kSystem) = vb;
        if (Field<uint32_t>(d, kFlags) & 2) RestoreHardware(d);
    }
    Field<uint32_t>(d, kFvf) = fvf;
}

void __fastcall RTriListSetBuffer(void* d, void*, void* vb, uint32_t vertices)   // FUN_10048a2e
{
    if (void* old = Field<void*>(d, kSystem)) {
        orig::VertexBuffer_c_Release(old);
        Field<void*>(d, kSystem) = nullptr;
    }
    Field<void*>(d, kSystem) = vb;
    Field<uint32_t>(d, kVertices) = vertices;
    RestoreHardware(d);
}

void __fastcall RTriListLoad(void* d, void*, void* message)   // FUN_10048a5c
{
    void* triList = nullptr;
    FindObject(message, "trilist", kTriListTd, &triList, 0);
    const void* data = nullptr;
    int32_t bytes = 0;
    S().findData(message, nullptr, "vb_desc", &data, &bytes, 0);
    uint32_t desc[4];
    std::memcpy(desc, data, sizeof(desc));
    S().findData(message, nullptr, "vertices", &data, &bytes, 0);
    RTriListSetTriList(d, nullptr, triList);
    RTriListSetVertices(d, nullptr, desc[2], data, uint32_t(bytes));
}

// ---- SimpleMesh ----

constexpr uint32_t kTriangleVectors = 0x08, kExtra = 0x0C, kMinusOne = 0x10, kMaterial = 0x14, kVolume = 0x18,
                   kExtraCount = 0x1C, kSystemOnly = 0x1E, kData = 0x20, kMeshSize = 0x24;

void* Data(void* m) { return Field<void*>(m, kData); }
void AddRef(void* r) { orig::RResource_t_AddRefRResource(r); }
void Release(void* r) { orig::RResource_t_ReleaseRResource(r); }
uint32_t BufferFlags(void* m) { return (Field<uint8_t>(m, kSystemOnly) & 1) ? 1 : 3; }

void InitMesh(void* m)
{
    S().construct(m, nullptr);
    Field<int32_t>(m, kMinusOne) = -1;
    Field<uint16_t>(m, kExtraCount) = 0;
    SetVtable(m, kSimpleMeshVtable);
    Field<void*>(m, kTriangleVectors) = nullptr;
    Field<void*>(m, kExtra) = nullptr;
    Field<void*>(m, kVolume) = nullptr;
    Field<uint8_t>(m, kSystemOnly) = Global<int32_t>(kHardwareLevel) != 2;
}

void* __fastcall MeshConstruct(void* m, void*, void* material, void* triList, uint32_t fvf, const void* vertices,
                               uint32_t count, uint32_t stride)
{
    InitMesh(m);
    Field<void*>(m, kMaterial) = material;
    void* d = RTriListConstruct(vc10::Allocate(0x4C), nullptr, BufferFlags(m));
    Field<void*>(m, kData) = d;
    RTriListSetTriList(d, nullptr, triList);
    RTriListSetVertices(d, nullptr, fvf, vertices, count * stride);
    if (void* mat = Field<void*>(m, kMaterial)) AddRef(mat);
    return m;
}

void* __fastcall MeshCopy(void* m, void*, void* from)   // FUN_1004e56f: shares the data and material
{
    S().construct(m, nullptr);
    SetVtable(m, kSimpleMeshVtable);
    Field<int32_t>(m, kMinusOne) = Field<int32_t>(from, kMinusOne);
    Field<void*>(m, kMaterial) = Field<void*>(from, kMaterial);
    Field<void*>(m, kVolume) = nullptr;
    Field<uint16_t>(m, kExtraCount) = Field<uint16_t>(from, kExtraCount);
    Field<uint8_t>(m, kSystemOnly) = Field<uint8_t>(from, kSystemOnly);
    Field<void*>(m, kData) = Data(from);
    if (void* volume = Field<void*>(from, kVolume))
        Field<void*>(m, kVolume) = Internal<void*(__fastcall*)(void*, void*, void*)>(0x29A13)(vc10::Allocate(0x30),
                                                                                             nullptr, volume);
    if (void* d = Data(m)) AddRef(d);
    if (void* mat = Field<void*>(m, kMaterial)) AddRef(mat);
    if (void* v = Field<void*>(from, kTriangleVectors)) {
        const uint32_t bytes = Field<uint32_t>(Data(m), kTriangleCount) * 12;
        Field<void*>(m, kTriangleVectors) = vc10::AllocateArray(bytes);
        std::memcpy(Field<void*>(m, kTriangleVectors), v, bytes);
    } else {
        Field<void*>(m, kTriangleVectors) = nullptr;
    }
    if (void* e = Field<void*>(from, kExtra)) {
        const uint32_t bytes = uint32_t(Field<uint16_t>(m, kExtraCount)) * 4;
        Field<void*>(m, kExtra) = vc10::AllocateArray(bytes);
        std::memcpy(Field<void*>(m, kExtra), e, bytes);
    } else {
        Field<void*>(m, kExtra) = nullptr;
    }
    return m;
}

void __fastcall MeshDestroy(void* m)                  // FUN_1004e689
{
    SetVtable(m, kSimpleMeshVtable);
    if (void* mat = Field<void*>(m, kMaterial)) Release(mat);
    if (void* d = Data(m)) Release(d);
    vc10::FreeArray(Field<void*>(m, kTriangleVectors));
    vc10::FreeArray(Field<void*>(m, kExtra));
    if (void* volume = Field<void*>(m, kVolume)) DeleteObject(volume);
    S().destroy(m, nullptr);
}

void* __fastcall MeshDelete(void* m, void*, uint8_t flags)   // vtable slot 0 (FUN_1004f990)
{
    MeshDestroy(m);
    if (flags & 1) vc10::Free(m);
    return m;
}

void __fastcall MeshArchive(void* m, void*, void* archive)   // vtable slot 1 (FUN_1004e6f4)
{
    S().getStream(archive, nullptr);
    void* stream = S().getStream(archive, nullptr);
    S().addObject(stream, nullptr, "material", Field<void*>(m, kMaterial));
    S().addObject(stream, nullptr, "trilist", Field<void*>(Data(m), kTriList));
    if (void* d = Data(m)) RTriListArchiveVertices(d, nullptr, stream);
}

void __fastcall MeshSetSystemOnly(void* m, void*, uint8_t systemOnly)   // FUN_1004e76d
{
    if (systemOnly == Field<uint8_t>(m, kSystemOnly)) return;
    Field<uint8_t>(m, kSystemOnly) = systemOnly;
    RTriListSetFlags(Data(m), nullptr, (systemOnly & 1) ? 1 : 3);
}

void __fastcall MeshAddColours(void* m, void*, bool diffuse, bool specular)   // FUN_1004e7be
{
    void* d = Data(m);
    RTriListConvert(d, nullptr, (specular ? 0x80u : 0u) | (diffuse ? 0x40u : 0u) | Field<uint32_t>(d, kFvf));
}

void* __fastcall MeshConstructFrom(void* m, void*, void* archive)   // FUN_1004e809
{
    InitMesh(m);
    Field<void*>(m, kMaterial) = nullptr;
    Field<void*>(m, kData) = nullptr;
    S().getStream(archive, nullptr);
    void* stream = S().getStream(archive, nullptr);
    FindObject(stream, "material", kMaterialTd, &Field<void*>(m, kMaterial), 0);
    void* d = RTriListConstruct(vc10::Allocate(0x4C), nullptr, BufferFlags(m));
    Field<void*>(m, kData) = d;
    RTriListLoad(d, nullptr, stream);
    return m;
}

void* __cdecl MeshInstantiate(void* archive) { return MeshConstructFrom(vc10::Allocate(kMeshSize), nullptr, archive); }

const uint16_t* MeshIndices(void* m) { return Triangles(Field<void*>(Data(m), kTriList)).first; }

void __fastcall MeshIndices32(void* m, void*, vc10::Vector<uint32_t>* out)   // SimpleMesh::GetTriangleIndices
{
    const uint32_t n = Field<uint32_t>(Data(m), kTriangleCount) * 3;
    const uint16_t* list = MeshIndices(m);
    out->reserve(n);
    for (uint32_t i = 0; i < n; ++i) out->push_back(list[i]);
}

void __fastcall MeshIndices16(void* m, void*, Indices* out)
{
    const uint32_t n = Field<uint32_t>(Data(m), kTriangleCount) * 3;
    const uint16_t* list = MeshIndices(m);
    out->reserve(n);
    for (uint32_t i = 0; i < n; ++i) out->push_back(list[i]);
}

void __fastcall MeshTriangles(void* m, void*, vc10::Vector<V3>* out)   // SimpleMesh::GetTriangles
{
    void* vb = Buffer(Data(m));
    uint8_t* base = static_cast<uint8_t*>(orig::VertexBuffer_c_Lock(vb, 0, 0x10));
    for (uint32_t i = 0; i < Field<uint32_t>(Data(m), kTriangleCount); ++i) {
        V3 t[3];
        TriListTriangle(Field<void*>(Data(m), kTriList), nullptr, int32_t(i), &t[0].x, base,
                        int32_t(orig::VertexBuffer_c_GetStride(vb)));
        for (const V3& p : t) out->push_back(p);
    }
    orig::VertexBuffer_c_Unlock(vb);
}

void __fastcall MeshPositions(void* m, void*, vc10::Vector<V3>* out)   // SimpleMesh::GetVertexPositions
{
    void* vb = Buffer(Data(m));
    const uint8_t* p = static_cast<const uint8_t*>(orig::VertexBuffer_c_Lock(vb, 0, 0x10));
    const uint32_t stride = orig::VertexBuffer_c_GetStride(vb);
    for (uint32_t n = Field<uint32_t>(Data(m), kVertices); n; --n, p += stride) {
        V3 v;
        std::memcpy(&v, p, 12);
        out->push_back(v);
    }
    orig::VertexBuffer_c_Unlock(vb);
}

void __fastcall MeshNormals(void* m, void*, vc10::Vector<V3>* out)   // (vertices of 32 bytes: the normal at 12)
{
    void* vb = Buffer(Data(m));
    const uint8_t* p = static_cast<const uint8_t*>(orig::VertexBuffer_c_Lock(vb, 0, 0x10)) + 12;
    for (uint32_t n = Field<uint32_t>(Data(m), kVertices); n; --n, p += 32) {
        V3 v;
        std::memcpy(&v, p, 12);
        out->push_back(v);
    }
    orig::VertexBuffer_c_Unlock(vb);
}

void __fastcall MeshUVs(void* m, void*, vc10::Vector<float>* out)   // (vertices of 32 bytes: u, v at 24)
{
    void* vb = Buffer(Data(m));
    const float* p = reinterpret_cast<const float*>(static_cast<uint8_t*>(orig::VertexBuffer_c_Lock(vb, 0, 0x10)) + 24);
    for (uint32_t n = Field<uint32_t>(Data(m), kVertices); n; --n, p += 8) {
        out->push_back(p[0]);
        out->push_back(p[1]);
    }
    orig::VertexBuffer_c_Unlock(vb);
}

// SimpleMesh::Mirror: its own copy of the data, positions and normals negated on the axes asked (1 x, 2 y, 4 z),
// triangles turned around. (The data it shared stays referenced, as in the original.)
void __fastcall MeshMirror(void* m, void*, uint32_t axes)
{
    if (!Data(m)) return;
    void* d = RTriListCopy(vc10::Allocate(0x4C), nullptr, Data(m));
    Field<void*>(m, kData) = d;
    void* vb = Buffer(d);
    float* p = static_cast<float*>(orig::VertexBuffer_c_Lock(vb, 0, 0));
    const float sz = (axes & 4) ? -1.0f : 1.0f, sy = (axes & 2) ? -1.0f : 1.0f, sx = (axes & 1) ? -1.0f : 1.0f;
    for (uint32_t n = Field<uint32_t>(Data(m), kVertices); n; --n, p += 8) {
        p[0] = p[0] * sx;
        p[1] = p[1] * sy;
        p[2] = p[2] * sz;
        p[3] = sx * p[3];
        p[4] = p[4] * sy;
        p[5] = p[5] * sz;
    }
    orig::VertexBuffer_c_Unlock(vb);
    RestoreHardware(d);
    TriListFlip(Field<void*>(d, kTriList));
}

void __fastcall MeshMakeVolume(void* m)               // FUN_1004f820: the bounding volume of its vertices
{
    vc10::Vector<V3> positions{};
    MeshPositions(m, nullptr, &positions);
    if (void* old = Field<void*>(m, kVolume)) DeleteObject(old);
    Field<void*>(m, kVolume) = Internal<void*(__fastcall*)(void*, void*, const void*, uint32_t, uint32_t)>(0x17FEE)(
        vc10::Allocate(0x30), nullptr, positions.first, Field<uint32_t>(Data(m), kVertices), 0xC);
    positions.release();
}

// SimpleMesh::IsRayIntersecting: the segment / ray `origin` along `dir` (t in [0, 1]) against its triangles, back
// facing only (the normal against the direction); `nearest` keeps the closest hit in *at, else the first. The same
// test as the character picking's (cat_pick.cpp), over this mesh's own triangles.
bool __fastcall MeshRay(void* m, void*, const float* origin, const float* dir, float* at, bool nearest)
{
    vc10::Vector<V3> triangles{};
    MeshTriangles(m, nullptr, &triangles);
    const float kMax = 3.4028234663852886e38f;   // FLT_MAX
    float best = kMax;
    for (size_t k = 0; k + 3 <= triangles.size(); k += 3) {
        const float* v0 = &triangles.first[k].x;
        const float* v1 = &triangles.first[k + 1].x;
        const float* v2 = &triangles.first[k + 2].x;
        float e0[3], e1[3], n[3];
        helpers::Subtract(v1, nullptr, e0, v0);        // e0 = v1 - v0
        helpers::Subtract(v2, nullptr, e1, v1);        // e1 = v2 - v1
        helpers::CrossProductTo(e0, nullptr, n, e1);   // n = e0 x e1
        const float det = dir[2] * n[2] + dir[0] * n[0] + dir[1] * n[1];
        if (!(det < 0.0f) && !std::isnan(det)) continue;   // facing away (or edge on)
        float w[3];
        helpers::Subtract(v0, nullptr, w, origin);     // w = v0 - origin
        const float num = w[2] * n[2] + w[0] * n[0] + w[1] * n[1];
        const float t = num / det;
        if (nearest && !(0.0f <= t && t <= best)) continue;
        float dt[3], p[3];
        helpers::ScaleVector(dir, nullptr, dt, t);
        p[0] = origin[0] + dt[0], p[1] = origin[1] + dt[1], p[2] = origin[2] + dt[2];
        float q[3], u[3], c0[3], c1[3], c2[3];
        helpers::Subtract(p, nullptr, q, v0);
        helpers::Subtract(v1, nullptr, u, v0);
        helpers::CrossProductTo(u, nullptr, c0, q);
        helpers::Subtract(p, nullptr, q, v1);
        helpers::Subtract(v2, nullptr, u, v1);
        helpers::CrossProductTo(u, nullptr, c1, q);
        if (!(0.0f <= c0[2] * c1[2] + c1[0] * c0[0] + c0[1] * c1[1])) continue;
        helpers::Subtract(p, nullptr, q, v2);
        helpers::Subtract(v0, nullptr, u, v2);
        helpers::CrossProductTo(u, nullptr, c2, q);
        if (!(0.0f <= c2[2] * c0[2] + c2[0] * c0[0] + c2[1] * c0[1])) continue;
        if (!(t <= 1.0f)) continue;
        best = t;
        if (at) *at = t;
        if (!nearest) {
            triangles.release();
            return true;
        }
    }
    triangles.release();
    return best < kMax;
}

// ---- RVisualData_t ----

constexpr uint32_t kAnimPosition = 0x2C, kAnimRotation = 0x38, kDegenerate = 0x48, kMeshes = 0x4C,
                   kRestored = 0x5C, kLastDrawn = 0x60, kSlot = 0x64, kVisualDataSize = 0x68;

vc10::Vector<void*>& Meshes(void* v) { return Field<vc10::Vector<void*>>(v, kMeshes); }
vc10::Vector<void*>& Registry() { return Global<vc10::Vector<void*>>(kRegistry); }

uint32_t __cdecl Register(void* v)                    // FUN_1004edff
{
    uint32_t& used = Global<uint32_t>(kRegistryUsed);
    if (used < Registry().size()) Registry().first[used] = v;
    else Registry().push_back(v);
    return used++;
}

void __fastcall Unregister(void*, void* v)            // FUN_1004e9c7 (ECX unused)
{
    uint32_t& used = Global<uint32_t>(kRegistryUsed);
    const uint32_t last = --used;
    const uint32_t slot = Field<uint32_t>(v, kSlot);
    if (slot != last) {
        Registry().first[slot] = Registry().first[last];
        Field<uint32_t>(Registry().first[slot], kSlot) = slot;
    }
    Registry().first[last] = nullptr;
}

void InitVisualData(void* v)
{
    SetVtable(v, kVisualDataVtable);
    std::memset(static_cast<uint8_t*>(v) + kAnimPosition, 0, 0x18);
    Field<float>(v, 0x44) = 1.0f;
    Meshes(v) = vc10::Vector<void*>{};
    Field<uint8_t>(v, kDegenerate) = 0;
}

void Registered(void* v)
{
    Field<uint32_t>(v, kRestored) = Global<uint32_t>(kRestoreCount);
    Field<uint32_t>(v, kLastDrawn) = Field<uint32_t>(*g_randy, kFrame);
    Field<uint32_t>(v, kSlot) = Register(v);
}

void* __fastcall VisualDataConstruct(void* v, void*, const char* name)   // FUN_1004ee3f
{
    Internal<void*(__fastcall*)(void*, void*, const char*)>(0x46311)(v, nullptr, name);
    InitVisualData(v);
    Meshes(v).reserve(16);
    Registered(v);
    return v;
}

void* __fastcall VisualDataCopy(void* v, void*, void* from)   // FUN_1004eec6: its own SimpleMeshes
{
    Internal<void*(__fastcall*)(void*, void*, void*)>(0x462AD)(v, nullptr, from);
    InitVisualData(v);
    Field<uint8_t>(v, kDegenerate) = Field<uint8_t>(from, kDegenerate);
    std::memcpy(static_cast<uint8_t*>(v) + kAnimPosition, static_cast<uint8_t*>(from) + kAnimPosition, 0x1C);
    for (void** m = Meshes(from).first; m != Meshes(from).last; ++m)
        Meshes(v).push_back(MeshCopy(vc10::Allocate(kMeshSize), nullptr, *m));
    Registered(v);
    return v;
}

void* __fastcall VisualDataConstructFrom(void* v, void*, void* archive)   // FUN_1004efa8
{
    Internal<void*(__fastcall*)(void*, void*, void*)>(0x46362)(v, nullptr, archive);
    InitVisualData(v);
    void* stream = S().getStream(archive, nullptr);
    S().findVector3(stream, nullptr, "anim_pos", &Field<float>(v, kAnimPosition), 0);
    S().findQuat(stream, nullptr, "anim_rot", &Field<float>(v, kAnimRotation), 0);
    S().findBool(stream, nullptr, "isdegen", reinterpret_cast<bool*>(static_cast<uint8_t*>(v) + kDegenerate), 0);
    int32_t count = 0;
    S().findInt32(stream, nullptr, "num_meshes", &count, 0);
    for (int32_t i = 0; i < count; ++i) {
        void* mesh = nullptr;
        if (FindObject(stream, "mesh", kSimpleMeshTd, &mesh, i) == 0) {
            Meshes(v).push_back(mesh);
        } else {
            char text[96];
            wsprintfA(text, "Error: RVisualData_t::RVisualData_t() Failed to load mesh %d\n", i);
            OutputDebugStringA(text);
        }
    }
    Registered(v);
    return v;
}

void* __cdecl VisualDataInstantiate(void* archive)
{
    return VisualDataConstructFrom(vc10::Allocate(kVisualDataSize), nullptr, archive);
}

void __fastcall VisualDataDestroy(void* v)            // FUN_1004ecdc
{
    SetVtable(v, kVisualDataVtable);
    for (void** m = Meshes(v).first; m != Meshes(v).last; ++m)
        if (*m) DeleteObject(*m);
    Unregister(nullptr, v);
    Meshes(v).release();
    Internal<void(__fastcall*)(void*)>(0x46283)(v);   // ~RResource_t
}

void* __fastcall VisualDataDelete(void* v, void*, uint8_t flags)   // vtable slot 0 (FUN_1004fbc1)
{
    VisualDataDestroy(v);
    if (flags & 1) vc10::Free(v);
    return v;
}

void* __fastcall VisualDataClone(void* v)             // vtable slot 3 (FUN_1004fd7b)
{
    return VisualDataCopy(vc10::Allocate(kVisualDataSize), nullptr, v);
}

void __fastcall VisualDataArchive(void* v, void*, void* archive)
{
    Internal<void(__fastcall*)(void*, void*, void*)>(0x4641A)(v, nullptr, archive);   // RResource_t::Archive
    void* stream = S().getStream(archive, nullptr);
    S().addVector3(stream, nullptr, "anim_pos", &Field<float>(v, kAnimPosition));
    S().addQuat(stream, nullptr, "anim_rot", &Field<float>(v, kAnimRotation));
    S().addInt32(stream, nullptr, "num_meshes", int32_t(Meshes(v).size()));
    S().addBool(stream, nullptr, "isdegen", Field<uint8_t>(v, kDegenerate) != 0);
    for (uint32_t i = 0; i < Meshes(v).size(); ++i) S().addObject(stream, nullptr, "mesh", Meshes(v).first[i]);
}

uint32_t __fastcall VisualDataVertexCount(void* v)    // FUN_1004eb88
{
    uint32_t n = 0;
    for (void** m = Meshes(v).first; m != Meshes(v).last; ++m) n += Field<uint32_t>(Data(*m), kVertices);
    return n;
}

uint32_t __fastcall VisualDataTriangleCount(void* v)  // FUN_1004eba6
{
    uint32_t n = 0;
    for (void** m = Meshes(v).first; m != Meshes(v).last; ++m) n += Field<uint32_t>(Data(*m), kTriangleCount);
    return n;
}

void __fastcall VisualDataPositions(void* v, void*, vc10::Vector<V3>* out)
{
    for (void** m = Meshes(v).first; m != Meshes(v).last; ++m) MeshPositions(*m, nullptr, out);
}

// FUN_1004ec2c: on hardware level 2, the hardware copies of its meshes freed; how many.
int32_t __fastcall VisualDataFree(void* v)
{
    if (Global<int32_t>(kHardwareLevel) != 2) return 0;
    int32_t n = 0;
    for (void** m = Meshes(v).first; m != Meshes(v).last; ++m) n += RTriListReleaseHardware(Data(*m)) ? 1 : 0;
    return n;
}

// FUN_1004ec5b: the hardware copies of meshes not drawn for `age` frames (at frame `now`), for up to 20 ms.
int32_t __cdecl FreeUnused(uint32_t now, uint32_t age)
{
    alignas(8) uint8_t timer[0x28];
    orig::Timer_Timer_57(timer);
    orig::Timer_Start(timer);
    int32_t n = 0;
    for (void** at = Registry().first; at != Registry().last;) {
        void* v = *at++;
        if (!v) break;
        if (age <= now - Field<uint32_t>(v, kLastDrawn)) {
            n += VisualDataFree(v);
            Field<uint32_t>(v, kLastDrawn) = Field<uint32_t>(*g_randy, kFrame);
            if (20.0f < orig::Timer_GetMilliSec(timer)) break;
        }
    }
    orig::Timer_Stop(timer);
    return n;
}

void __fastcall TriListArchiveImpl(void* t, void*, void* archive)
{
    static const auto base = reinterpret_cast<void(__fastcall*)(void*, void*, void*)>(GetProcAddress(
        GetModuleHandleA("serialize.dll"), "?Archive@Serializable_c@fun@@UBEXPAVObjectArchive_c@2@@Z"));
    base(t, nullptr, archive);
    void* stream = S().getStream(archive, nullptr);
    const Indices& v = Triangles(t);
    S().addData(stream, nullptr, "triangles", v.first, int32_t(v.size()) * 2, false, 1);
}

// ---- RTriMeshData_t (an RVisualData_t) ----

constexpr uint32_t kSharedCopy = 0x68, kBVolume = 0x6C, kTriMeshDataSize = 0x70;
constexpr uint32_t kTriMeshDataVtable = 0x958D0, kFafTriMeshDataVtable = 0x8A9EC, kBVolumeTd = 0xB6130,
                   kAutoVolume = 0xB79BA, kVector3Compare = 0x155C5, kMaterialPower = 0x4A8C0;

void*& SharedPtr(void* v) { return Field<void*>(v, kSharedCopy); }
void*& BVolumePtr(void* v) { return Field<void*>(v, kBVolume); }

// FUN_1004add9: the base archive constructor, this vtable, the "bvol" object, then (0xB79BA is 1) the bounding volume
// recomputed from the meshes' vertices - used, with the archived volume deleted, only if its centre differs.
void* __fastcall TriMeshDataConstructFrom(void* v, void*, void* archive)
{
    VisualDataConstructFrom(v, nullptr, archive);   // RVisualData_t::RVisualData_t(archive)
    SetVtable(v, kTriMeshDataVtable);
    SharedPtr(v) = nullptr;
    BVolumePtr(v) = nullptr;
    void* stream = S().getStream(archive, nullptr);
    FindObject(stream, "bvol", kBVolumeTd, &BVolumePtr(v), 0);
    vc10::Vector<V3> positions{};
    for (void** m = Meshes(v).first; m != Meshes(v).last; ++m) MeshPositions(*m, nullptr, &positions);
    if (Global<uint8_t>(kAutoVolume)) {
        const uint32_t count = uint32_t(positions.size());
        void* volume = count
            ? Internal<void*(__fastcall*)(void*, void*, const void*, uint32_t, uint32_t)>(0x17FEE)(
                  vc10::Allocate(0x30), nullptr, &positions.first[0].x, count, 0xC)
            : Internal<void*(__fastcall*)(void*, void*)>(0x299E0)(vc10::Allocate(0x30), nullptr);
        if (BVolumePtr(v) && Internal<uint8_t(__fastcall*)(void*, void*, const void*)>(kVector3Compare)(
                                  static_cast<uint8_t*>(volume) + 8, nullptr, static_cast<uint8_t*>(BVolumePtr(v)) + 8))
            std::swap(volume, BVolumePtr(v));
        if (volume) DeleteObject(volume);
    }
    positions.release();
    return v;
}

// FUN_1004ab7a: the base copy, this vtable, a copy of the bounding volume.
void* __fastcall TriMeshDataCopy(void* v, void*, const void* from)
{
    VisualDataCopy(v, nullptr, const_cast<void*>(from));   // RVisualData_t::RVisualData_t(copy)
    BVolumePtr(v) = nullptr;
    SetVtable(v, kTriMeshDataVtable);
    if (BVolumePtr(const_cast<void*>(from)))
        BVolumePtr(v) = Internal<void*(__fastcall*)(void*, void*, const void*)>(0x29A13)(
            vc10::Allocate(0x30), nullptr, BVolumePtr(const_cast<void*>(from)));
    SharedPtr(v) = nullptr;
    return v;
}

void __fastcall TriMeshDataDestroy(void* v)   // FUN_1004abe3
{
    SetVtable(v, kTriMeshDataVtable);
    if (void* volume = BVolumePtr(v)) DeleteObject(volume);
    if (void* shared = SharedPtr(v); shared && shared != v) Release(shared);
    VisualDataDestroy(v);   // RVisualData_t::~RVisualData_t
}

void __fastcall TriMeshDataArchive(void* v, void*, void* archive)   // FUN_1004ac33
{
    VisualDataArchive(v, nullptr, archive);
    void* stream = S().getStream(archive, nullptr);
    S().addObject(stream, nullptr, "bvol", BVolumePtr(v));
}

// FUN_1004ac5f: a private RVisualData_t (cloned unless it is unshared), its meshes in system memory when asked and
// their materials' lightmap delta states (no material sources, emissive by the material colour's luminance).
void* __fastcall TriMeshDataPrivate(void* v, void*, uint8_t systemOnly)
{
    void* data;
    if (Field<int32_t>(v, 0x24) == 1) {
        data = v;
    } else {
        data = Virtual<void*>(v, 3);   // Clone
        Release(v);
    }
    for (void** m = Meshes(data).first; m != Meshes(data).last; ++m) {
        if (systemOnly) MeshSetSystemOnly(*m, nullptr, 1);
        void* material = Field<void*>(*m, kMaterial);
        void* delta = Field<void*>(material, 0x70);
        if (!delta) {
            void* made = vc10::Allocate(0x16C);
            delta = made ? orig::RDeltaState_RDeltaState(made, "") : nullptr;
            orig::RMaterial_t_SetDeltaState(material, delta);
            if (made) Release(made);
        }
        orig::RDeltaState_SetRenderState(delta, 0x91, 0);
        orig::RDeltaState_SetRenderState(delta, 0x92, 0);
        orig::RDeltaState_SetRenderState(delta, 0x93, 0);
        const float power =
            Internal<float(__fastcall*)(const void*)>(kMaterialPower)(static_cast<const uint8_t*>(material) + 0x50);
        orig::RDeltaState_SetRenderState(delta, 0x94, power <= 0.0f ? 1u : 0u);
        MeshAddColours(*m, nullptr, true, false);
    }
    return data;
}

// FUN_1004ad93: its +0x68 private copy, made once (and all its copies share it).
void* __fastcall TriMeshDataSharedCopy(void* v)
{
    void* shared = SharedPtr(v);
    if (!shared) {
        AddRef(v);
        shared = TriMeshDataPrivate(v, nullptr, 1);
        SharedPtr(v) = shared;
        AddRef(shared);
        SharedPtr(shared) = shared;
    } else if (shared == v) {
        return shared;
    } else {
        AddRef(shared);
    }
    Release(v);
    return shared;
}

void* __cdecl TriMeshDataInstantiate(void* archive)
{
    void* v = vc10::Allocate(kTriMeshDataSize);
    return v ? TriMeshDataConstructFrom(v, nullptr, archive) : nullptr;
}

void* __fastcall TriMeshDataClone(void* v)   // FUN_100187fc (vtable slot 3)
{
    void* copy = vc10::Allocate(kTriMeshDataSize);
    return copy ? TriMeshDataCopy(copy, nullptr, v) : nullptr;
}

// ---- FAFTriMeshData_t (the scene reader's subclass) ----

void* __fastcall FafTriMeshDataConstructFrom(void* v, void*, void* archive)   // FUN_10018087
{
    TriMeshDataConstructFrom(v, nullptr, archive);
    SetVtable(v, kFafTriMeshDataVtable);
    S().getStream(archive, nullptr);
    return v;
}

void __fastcall FafTriMeshDataArchive(void* v, void*, void* archive)   // FUN_100180c4
{
    TriMeshDataArchive(v, nullptr, archive);
    S().getStream(archive, nullptr);
}

void* __cdecl FafTriMeshDataInstantiate(void* archive)
{
    void* v = vc10::Allocate(kTriMeshDataSize);
    return v ? FafTriMeshDataConstructFrom(v, nullptr, archive) : nullptr;
}

void* __fastcall FafTriMeshDataDelete(void* v, void*, uint8_t flags)   // FUN_10018893 (vtable slot 0)
{
    SetVtable(v, kFafTriMeshDataVtable);
    TriMeshDataDestroy(v);
    if (flags & 1) vc10::Free(v);
    return v;
}

// Triangle: three indices per triangle, and the static tables its other accessors hand back. RVisualData_t's
// SimpleMesh vector is at +0x4C.
uint32_t __cdecl TriangleIndexCount() { return 3; }                                             // Triangle::GetIndexCount
void* __cdecl TriangleTable0() { return reinterpret_cast<uint8_t*>(g_orig) + 0x8F490; }
void* __cdecl TriangleTable1() { return reinterpret_cast<uint8_t*>(g_orig) + 0x8F4C0; }
void* __cdecl TriangleTable2() { return reinterpret_cast<uint8_t*>(g_orig) + 0x8F4F8; }
void* __fastcall VisualDataSimpleMeshes(void* self, void*) { return static_cast<uint8_t*>(self) + 0x4C; }

}  // namespace

void* MakePrivate(void* data, bool systemOnly) { return TriMeshDataPrivate(data, nullptr, systemOnly ? 1 : 0); }

void* SharedCopy(void* data) { return TriMeshDataSharedCopy(data); }

void Install(HMODULE orig)
{
    if (GetMode("Scene", Mode::Off) != Mode::On) return;
    g_orig = orig;
    g_randy = reinterpret_cast<void* const*>(GetProcAddress(orig, "?s_pcRandy@Randy_t@@1PAV1@A"));
    const serialize::Api& s = S();
    const bool archiveBase = GetProcAddress(GetModuleHandleA("serialize.dll"),
                                            "?Archive@Serializable_c@fun@@UBEXPAVObjectArchive_c@2@@Z") != nullptr;
    if (!g_randy || !s.complete || !s.findObject || !s.findBool || !s.addData || !s.findData || !archiveBase) {
        Log("mesh data: serialize.dll / randy31 exports missing - not replaced");
        return;
    }
    struct Entry {
        uint32_t rva;
        void* target;
        const char* what;
    };
#define FN(f) reinterpret_cast<void*>(&f)
    const Entry entries[] = {
        {0x188FB, FN(TriListDelete), "TriList deleting destructor (FUN_100188fb)"},
        {0x1FD2A, FN(TriListArchiveImpl), "TriList::Archive"},
        {0x1FE9D, FN(TriListAddTriangle), "TriList::AddTriangle"},
        {0x1FEDF, FN(TriListConstructFrom), "TriList::TriList(archive) (FUN_1001fedf)"},
        {0x1FF60, FN(TriListInstantiate), "TriList::Instantiate"},
        {0x48CDF, FN(TriListCopy), "TriList::TriList(copy) (FUN_10048cdf)"},
        {0x1FE30, FN(TriListFlip), "TriList winding flip (FUN_1001fe30)"},
        {0x1FD66, FN(TriListTriangle), "TriList triangle positions (FUN_1001fd66)"},
        {0x4866F, FN(RTriListConstruct), "RTriList_t::RTriList_t (FUN_1004866f)"},
        {0x48AD9, FN(RTriListCopy), "RTriList_t::RTriList_t(copy) (FUN_10048ad9)"},
        {0x486C7, FN(RTriListDestroy), "RTriList_t::~RTriList_t (FUN_100486c7)"},
        {0x48C07, FN(RTriListDelete), "RTriList_t deleting destructor (FUN_10048c07)"},
        {0x48787, FN(RTriListSetFlags), "RTriList_t buffer flags (FUN_10048787)"},
        {0x4881D, FN(RTriListReleaseHardware), "RTriList_t hardware copy release (FUN_1004881d)"},
        {0x4883E, FN(RTriListArchiveVertices), "RTriList_t vertices to an archive (FUN_1004883e)"},
        {0x488BD, FN(RTriListSetTriList), "RTriList_t triangle list (FUN_100488bd)"},
        {0x488ED, FN(RTriListSetVertices), "RTriList_t vertices (FUN_100488ed)"},
        {0x4899A, FN(RTriListConvert), "RTriList_t vertex format (FUN_1004899a)"},
        {0x48A2E, FN(RTriListSetBuffer), "RTriList_t vertex buffer (FUN_10048a2e)"},
        {0x48A5C, FN(RTriListLoad), "RTriList_t from an archive (FUN_10048a5c)"},
        {0x4E4B7, FN(MeshConstruct), "SimpleMesh::SimpleMesh"},
        {0x4E56F, FN(MeshCopy), "SimpleMesh::SimpleMesh(copy) (FUN_1004e56f)"},
        {0x4E689, FN(MeshDestroy), "SimpleMesh::~SimpleMesh (FUN_1004e689)"},
        {0x4F990, FN(MeshDelete), "SimpleMesh deleting destructor (FUN_1004f990)"},
        {0x4E6F4, FN(MeshArchive), "SimpleMesh::Archive (FUN_1004e6f4)"},
        {0x4E76D, FN(MeshSetSystemOnly), "SimpleMesh system memory flag (FUN_1004e76d)"},
        {0x4E7BE, FN(MeshAddColours), "SimpleMesh vertex colours (FUN_1004e7be)"},
        {0x4E809, FN(MeshConstructFrom), "SimpleMesh::SimpleMesh(archive) (FUN_1004e809)"},
        {0x4EB53, FN(MeshInstantiate), "SimpleMesh::Instantiate"},
        {0x4ED3E, FN(MeshIndices32), "SimpleMesh::GetTriangleIndices(uint)"},
        {0x4ED88, FN(MeshIndices16), "SimpleMesh::GetTriangleIndices(ushort)"},
        {0x4F369, FN(MeshTriangles), "SimpleMesh::GetTriangles"},
        {0x4F723, FN(MeshPositions), "SimpleMesh::GetVertexPositions"},
        {0x4F779, FN(MeshNormals), "SimpleMesh::GetVertexNormals"},
        {0x4F0EB, FN(MeshUVs), "SimpleMesh::GetVertexUVCoords"},
        {0x4E8C2, FN(MeshMirror), "SimpleMesh::Mirror"},
        {0x4F820, FN(MeshMakeVolume), "SimpleMesh bounding volume (FUN_1004f820)"},
        {0x4F407, FN(MeshRay), "SimpleMesh::IsRayIntersecting"},
        {0x4EDFF, FN(Register), "RVisualData_t registry add (FUN_1004edff)"},
        {0x4E9C7, FN(Unregister), "RVisualData_t registry remove (FUN_1004e9c7)"},
        {0x4EE3F, FN(VisualDataConstruct), "RVisualData_t::RVisualData_t (FUN_1004ee3f)"},
        {0x4EEC6, FN(VisualDataCopy), "RVisualData_t::RVisualData_t(copy) (FUN_1004eec6)"},
        {0x4EFA8, FN(VisualDataConstructFrom), "RVisualData_t::RVisualData_t(archive) (FUN_1004efa8)"},
        {0x4F334, FN(VisualDataInstantiate), "RVisualData_t::Instantiate"},
        {0x4ECDC, FN(VisualDataDestroy), "RVisualData_t::~RVisualData_t (FUN_1004ecdc)"},
        {0x4FBC1, FN(VisualDataDelete), "RVisualData_t deleting destructor (FUN_1004fbc1)"},
        {0x4FD7B, FN(VisualDataClone), "RVisualData_t clone (FUN_1004fd7b)"},
        {0x4EA04, FN(VisualDataArchive), "RVisualData_t::Archive"},
        {0x4EB88, FN(VisualDataVertexCount), "RVisualData_t vertex count (FUN_1004eb88)"},
        {0x4EBA6, FN(VisualDataTriangleCount), "RVisualData_t triangle count (FUN_1004eba6)"},
        {0x4F7FC, FN(VisualDataPositions), "RVisualData_t::GetVertexPositions"},
        {0x4EC2C, FN(VisualDataFree), "RVisualData_t hardware copies freed (FUN_1004ec2c)"},
        {0x4EC5B, FN(FreeUnused), "unused mesh buffers freed (FUN_1004ec5b)"},
        {0x4AB7A, FN(TriMeshDataCopy), "RTriMeshData_t::RTriMeshData_t(copy) (FUN_1004ab7a)"},
        {0x4ABE3, FN(TriMeshDataDestroy), "RTriMeshData_t::~RTriMeshData_t (FUN_1004abe3)"},
        {0x4AC33, FN(TriMeshDataArchive), "RTriMeshData_t::Archive"},
        {0x4AC5F, FN(TriMeshDataPrivate), "RTriMeshData_t private copy (FUN_1004ac5f)"},
        {0x4AD93, FN(TriMeshDataSharedCopy), "RTriMeshData_t shared copy (FUN_1004ad93)"},
        {0x4ADD9, FN(TriMeshDataConstructFrom), "RTriMeshData_t::RTriMeshData_t(archive) (FUN_1004add9)"},
        {0x4AF1A, FN(TriMeshDataInstantiate), "RTriMeshData_t::Instantiate"},
        {0x187FC, FN(TriMeshDataClone), "RTriMeshData_t clone (FUN_100187fc)"},
        {0x1716A, FN(FafTriMeshDataInstantiate), "FAFTriMeshData_t::Instantiate"},
        {0x18087, FN(FafTriMeshDataConstructFrom), "FAFTriMeshData_t(archive) ctor (FUN_10018087)"},
        {0x180C4, FN(FafTriMeshDataArchive), "FAFTriMeshData_t::Archive (FUN_100180c4)"},
        {0x18893, FN(FafTriMeshDataDelete), "FAFTriMeshData_t deleting destructor (FUN_10018893)"},
        {0x1B69D, FN(TriangleIndexCount), "Triangle::GetIndexCount"},
        {0x1B6E3, FN(TriangleTable0), "Triangle static table 0 (FUN_1001b6e3)"},
        {0x1B6E9, FN(TriangleTable1), "Triangle static table 1 (FUN_1001b6e9)"},
        {0x1B6EF, FN(TriangleTable2), "Triangle static table 2 (FUN_1001b6ef)"},
        {0x4E7EB, FN(VisualDataSimpleMeshes), "RVisualData_t::GetSimpleMeshArray (const)"},
        {0x4E7EF, FN(VisualDataSimpleMeshes), "RVisualData_t::GetSimpleMeshArray"},
    };
#undef FN
    int installed = 0, writers = 0;
    for (const Entry& e : entries) {
        bool ok = Replace(orig, e.rva, e.target, e.what);
        installed += ok ? 1 : 0;
        for (uint32_t writer : {0x188FBu, 0x1FE9Du, 0x1FEDFu, 0x48CDFu, 0x1FE30u})   // the TriList writers
            if (ok && e.rva == writer) ++writers;
    }
    g_indexTracked = writers == 5;
    Log("mesh data: %d of %d functions native%s", installed, int(sizeof(entries) / sizeof(entries[0])),
        g_indexTracked ? "" : "; triangle list writes not all native: static meshes' indices are not kept");
}

uint64_t IndexGeneration() { return g_indexTracked ? g_indexGeneration : 0; }

}  // namespace rnative::meshdata
