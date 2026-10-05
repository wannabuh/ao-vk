// The FAF data classes from the scene reader: their archive constructors and Instantiate (the base's archive ctor,
// this class's vtable, then the stream), plus the two that have destructors here. The bases (RTexture_t, RRefFrame_t)
// and their destructors are already native, so these are thin.
#include "native/faf.h"

#include "native/orig_api.gen.h"
#include "native/serialize.h"
#include "native/vc10.h"

#include <cmath>

namespace rnative::faf {

namespace {

HMODULE g_orig;

template <typename F>
F Internal(uint32_t rva) { return reinterpret_cast<F>(AddressFor(rva)); }
template <typename F>
F Export(const char* name) { return reinterpret_cast<F>(GetProcAddress(g_orig, name)); }

constexpr uint32_t kTextureVtable = 0x8A9DC, kAttractorVtable = 0x8A834, kBoxVtable = 0x8A868, kSphereVtable = 0x8A89C;
constexpr uint32_t kMaterialVtable = 0x8A9C8;
constexpr uint32_t kTextureDtor = 0x477FE, kFrameDtor = 0x45471;
// RMaterial_t (already native): its archive ctor, Archive, destructor, and the FAFMaterial_t's extra vector's dtor.
constexpr uint32_t kRMaterialCtor = 0x4132D, kRMaterialArchive = 0x40916, kRMaterialDtor = 0x408CB, kVectorDtor = 0x17E2F;
const char* const kRTextureCtor = "??0RTexture_t@@QAE@PAVObjectArchive_c@fun@@@Z";
const char* const kRRefFrameCtor = "??0RRefFrame_t@@QAE@PAVObjectArchive_c@fun@@@Z";

void SetVtable(void* self, uint32_t rva) { *static_cast<void**>(self) = reinterpret_cast<uint8_t*>(g_orig) + rva; }
void* __fastcall CtorCollision(void* self, void*, uint32_t vtable, void* archive)
{
    Export<void*(__fastcall*)(void*, void*, void*)>(kRRefFrameCtor)(self, nullptr, archive);
    SetVtable(self, vtable);
    uint8_t* at = static_cast<uint8_t*>(self) + 0xA4;   // a std::string
    *reinterpret_cast<uint32_t*>(at + 0x10) = 0;
    *reinterpret_cast<uint32_t*>(at + 0x14) = 0xf;
    at[0] = 0;
    serialize::Get().getStream(archive, nullptr);
    return self;
}
void* __cdecl NewCollision(uint32_t vtable, uint32_t size, void* archive)
{
    uint8_t* self = static_cast<uint8_t*>(vc10::Allocate(size));
    return self ? CtorCollision(self, nullptr, vtable, archive) : nullptr;
}

// FUN_10016ac0: the collision sphere's Archive (its RRefFrame_t base, then its own stream).
void __fastcall ArchiveCollision(void* self, void*, void* archive)
{
    orig::RRefFrame_t_Archive(self, archive);
    serialize::Get().getStream(archive, nullptr);
}

// FUN_10016a92: the collision box's std::string at +0xA4 as a C string.
void* __fastcall BoxName(void* self, void*)
{
    uint8_t* s = static_cast<uint8_t*>(self) + 0xA4;
    return *reinterpret_cast<uint32_t*>(s + 0x14) > 0xF ? *reinterpret_cast<void**>(s) : static_cast<void*>(s);
}

// FUN_10017f2b: one over the length of the 3-float vector in this (the original's x87 sums c, a, b, then sqrt).
float __fastcall InverseLength(void* self, void*)
{
    const float* v = static_cast<const float*>(self);
    const float sum = float(double(v[2]) * double(v[2]) + double(v[0]) * double(v[0]) + double(v[1]) * double(v[1]));
    return float(1.0 / double(std::sqrt(double(sum))));
}

}  // namespace

// ---- FAFAttractor_t (RRefFrame_t) ----

void* __fastcall CtorAttractor(void* self, void*, void* archive)
{
    Export<void*(__fastcall*)(void*, void*, void*)>(kRRefFrameCtor)(self, nullptr, archive);
    SetVtable(self, kAttractorVtable);
    serialize::Get().getStream(archive, nullptr);
    return self;
}

void __fastcall ArchiveAttractor(void* self, void*, void* archive)
{
    orig::RRefFrame_t_Archive(self, archive);
    serialize::Get().getStream(archive, nullptr);
}

void* __fastcall DeletingDtorAttractor(void* self, void*, uint8_t flags)
{
    SetVtable(self, kAttractorVtable);
    Internal<void(__fastcall*)(void*, void*)>(kFrameDtor)(self, nullptr);
    if (flags & 1) vc10::Free(self);
    return self;
}

void* __cdecl InstantiateAttractor(void* archive)
{
    uint8_t* self = static_cast<uint8_t*>(vc10::Allocate(0xA4));
    return self ? CtorAttractor(self, nullptr, archive) : nullptr;
}

// ---- FAFCollisionBox_c / FAFCollisionSphere_c (RRefFrame_t, a std::string at +0xA4) ----

void* __fastcall CtorBox(void* self, void*, void* archive) { return CtorCollision(self, nullptr, kBoxVtable, archive); }
void* __fastcall CtorSphere(void* self, void*, void* archive)
{
    return CtorCollision(self, nullptr, kSphereVtable, archive);
}
void* __cdecl InstantiateBox(void* archive) { return NewCollision(kBoxVtable, 0xCC, archive); }
void* __cdecl InstantiateSphere(void* archive) { return NewCollision(kSphereVtable, 0xC4, archive); }

// ---- FAFMaterial_t (RMaterial_t, its own vector of extra data at +0xCC) ----

void* __fastcall CtorMaterial(void* self, void*, void* archive)
{
    Internal<void(__fastcall*)(void*, void*, void*)>(kRMaterialCtor)(self, nullptr, archive);
    SetVtable(self, kMaterialVtable);
    serialize::Get().getStream(archive, nullptr);
    *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(self) + 0xCC) = 0;
    return self;
}

void __fastcall ArchiveMaterial(void* self, void*, void* archive)
{
    Internal<void(__fastcall*)(void*, void*, void*)>(kRMaterialArchive)(self, nullptr, archive);
    serialize::Get().getStream(archive, nullptr);
}

void __fastcall DtorMaterial(void* self, void*)
{
    SetVtable(self, kMaterialVtable);
    if (void* extra = *reinterpret_cast<void**>(static_cast<uint8_t*>(self) + 0xCC))
        Internal<int*(__fastcall*)(void*, void*, uint8_t)>(kVectorDtor)(extra, nullptr, 3);
    Internal<void(__fastcall*)(void*, void*)>(kRMaterialDtor)(self, nullptr);
}

void* __fastcall DeletingDtorMaterial(void* self, void*, uint8_t flags)
{
    DtorMaterial(self, nullptr);
    if (flags & 1) vc10::Free(self);
    return self;
}

void* __cdecl InstantiateMaterial(void* archive)
{
    uint8_t* self = static_cast<uint8_t*>(vc10::Allocate(0xD0));
    return self ? CtorMaterial(self, nullptr, archive) : nullptr;
}

// ---- FAFDirectionalLight_t (RLight_t) ----

constexpr uint32_t kDirectionalLightVtable = 0x8A940;

void* __fastcall CtorDirectionalLight(void* self, void*, void* archive)
{
    Internal<void(__fastcall*)(void*, void*, void*)>(0x3FE81)(self, nullptr, archive);   // RLight_t(archive)
    SetVtable(self, kDirectionalLightVtable);
    serialize::Get().getStream(archive, nullptr);
    return self;
}

void __fastcall ArchiveDirectionalLight(void* self, void*, void* archive)
{
    Internal<void(__fastcall*)(void*, void*, void*)>(0x3FBD8)(self, nullptr, archive);   // RLight_t::Archive
    serialize::Get().getStream(archive, nullptr);
}

void* __cdecl InstantiateDirectionalLight(void* archive)
{
    uint8_t* self = static_cast<uint8_t*>(vc10::Allocate(0x11C));
    return self ? CtorDirectionalLight(self, nullptr, archive) : nullptr;
}

// ---- FAFPointLight_t / FAFSpotLight_t (RLight_t too) ----

constexpr uint32_t kPointLightVtable = 0x8A8D0, kSpotLightVtable = 0x8A908;

void* __cdecl InstantiatePointLight(void* archive)
{
    uint8_t* self = static_cast<uint8_t*>(vc10::Allocate(0x11C));
    if (!self) return nullptr;
    Internal<void*(__fastcall*)(void*, void*, void*)>(0x3FE81)(self, nullptr, archive);   // RLight_t(archive)
    SetVtable(self, kPointLightVtable);
    return self;
}

void* __cdecl InstantiateSpotLight(void* archive)
{
    uint8_t* self = static_cast<uint8_t*>(vc10::Allocate(0x11C));
    if (!self) return nullptr;
    Internal<void*(__fastcall*)(void*, void*, void*)>(0x3FE81)(self, nullptr, archive);
    SetVtable(self, kSpotLightVtable);
    return self;
}

void __fastcall ArchivePointLight(void* self, void*, void* archive)   // FUN_10016d3f (vtable slot 1)
{
    Internal<void(__fastcall*)(void*, void*, void*)>(0x3FBD8)(self, nullptr, archive);   // RLight_t::Archive
    serialize::Get().getStream(archive, nullptr);
}

void __fastcall ArchiveSpotLight(void* self, void*, void* archive)   // FUN_10016d9b (vtable slot 1)
{
    Internal<void(__fastcall*)(void*, void*, void*)>(0x3FBD8)(self, nullptr, archive);
    serialize::Get().getStream(archive, nullptr);
}

// ---- the collision classes' destructors ----

void __fastcall DtorCollision(void* self, void*)
{
    Internal<void(__fastcall*)(void*, void*, uint8_t, uint32_t)>(0x11E82)(static_cast<uint8_t*>(self) + 0xA4, nullptr, 1,
                                                                        0);   // ~std::string
    Internal<void(__fastcall*)(void*, void*)>(kFrameDtor)(self, nullptr);       // ~RRefFrame_t
}

void* __fastcall DeletingDtorBox(void* self, void*, uint8_t flags)
{
    DtorCollision(self, nullptr);
    if (flags & 1) vc10::Free(self);
    return self;
}

// ---- 0x17DF5: an RMaterial_t copy into a fresh object (the source is in ECX) ----

void* __fastcall CopyMaterial(const void* source, void*)
{
    void* object = vc10::Allocate(0xC0);
    if (object) Internal<void(__fastcall*)(void*, void*, const void*)>(0x41146)(object, nullptr, source);
    return object;
}

// ---- FAFTexture_t (RTexture_t) ----

void* __cdecl InstantiateTexture(void* archive)
{
    uint8_t* self = static_cast<uint8_t*>(vc10::Allocate(0xBC));
    if (!self) return nullptr;
    Export<void*(__fastcall*)(void*, void*, void*)>(kRTextureCtor)(self, nullptr, archive);
    SetVtable(self, kTextureVtable);
    return self;
}

void* __fastcall DeletingDtorTexture(void* self, void*, uint8_t flags)
{
    Internal<void(__fastcall*)(void*, void*)>(kTextureDtor)(self, nullptr);
    if (flags & 1) vc10::Free(self);
    return self;
}

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
        {0x166EE, FN(CtorAttractor), "FAFAttractor_t(archive) ctor (FUN_100166ee)"},
        {0x16743, FN(InstantiateAttractor), "FAFAttractor_t::Instantiate"},
        {0x1682C, FN(DeletingDtorAttractor), "FAFAttractor_t deleting destructor (FUN_1001682c)"},
        {0x168D6, FN(ArchiveAttractor), "FAFCollisionBox_c::Archive (FUN_100168d6)"},
        {0x1672B, FN(ArchiveAttractor), "FAFAttractor_t::Archive (FUN_1001672b)"},
        {0x168EE, FN(CtorBox), "FAFCollisionBox_c(archive) ctor (FUN_100168ee)"},
        {0x16946, FN(InstantiateBox), "FAFCollisionBox_c::Instantiate"},
        {0x16AA1, FN(DeletingDtorBox), "FAFCollisionBox_c deleting destructor (FUN_10016aa1)"},
        {0x16AC0, FN(ArchiveCollision), "FAFCollisionSphere_c::Archive (FUN_10016ac0)"},
        {0x16C5E, FN(DtorCollision), "the collision classes' destructor (FUN_10016c5e)"},
        {0x16CB2, FN(CtorDirectionalLight), "FAFDirectionalLight_t(archive) ctor (FUN_10016cb2)"},
        {0x16CEF, FN(ArchiveDirectionalLight), "FAFDirectionalLight_t::Archive (FUN_10016cef)"},
        {0x16D07, FN(InstantiateDirectionalLight), "FAFDirectionalLight_t::Instantiate"},
        {0x16D3F, FN(ArchivePointLight), "FAFPointLight_t::Archive (FUN_10016d3f)"},
        {0x16D57, FN(InstantiatePointLight), "FAFPointLight_t::Instantiate"},
        {0x16D9B, FN(ArchiveSpotLight), "FAFSpotLight_t::Archive (FUN_10016d9b)"},
        {0x16DB3, FN(InstantiateSpotLight), "FAFSpotLight_t::Instantiate"},
        {0x16AD8, FN(CtorSphere), "FAFCollisionSphere_c(archive) ctor (FUN_10016ad8)"},
        {0x16B30, FN(InstantiateSphere), "FAFCollisionSphere_c::Instantiate"},
        {0x17D1D, FN(CtorMaterial), "FAFMaterial_t(archive) ctor (FUN_10017d1d)"},
        {0x17DF5, FN(CopyMaterial), "RMaterial_t copy into a fresh object (FUN_10017df5)"},
        {0x17D61, FN(InstantiateMaterial), "FAFMaterial_t::Instantiate"},
        {0x17D99, FN(ArchiveMaterial), "FAFMaterial_t::Archive (FUN_10017d99)"},
        {0x17DB1, FN(DtorMaterial), "FAFMaterial_t destructor (FUN_10017db1)"},
        {0x17E85, FN(DeletingDtorMaterial), "FAFMaterial_t deleting destructor (FUN_10017e85)"},
        {0x17EC8, FN(InstantiateTexture), "FAFTexture_t::Instantiate"},
        {0x17F0C, FN(DeletingDtorTexture), "FAFTexture_t deleting destructor (FUN_10017f0c)"},
        {0x16A92, FN(BoxName), "FAFCollisionBox_c name (FUN_10016a92)"},
        {0x17F2B, FN(InverseLength), "a vector's inverse length (FUN_10017f2b)"},
    };
#undef FN
    int installed = 0;
    for (const Entry& e : entries) installed += Replace(orig, e.rva, e.target, e.what) ? 1 : 0;
    Log("faf: %d of %d functions native", installed, int(sizeof(entries) / sizeof(entries[0])));
}

}  // namespace rnative::faf
