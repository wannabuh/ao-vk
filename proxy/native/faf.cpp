// The FAF data classes from the scene reader: their archive constructors and Instantiate (the base's archive ctor,
// this class's vtable, then the stream), plus the two that have destructors here. The bases (RTexture_t, RRefFrame_t)
// and their destructors are already native, so these are thin.
#include "native/faf.h"

#include "native/orig_api.gen.h"
#include "native/serialize.h"
#include "native/vc10.h"

namespace rnative::faf {

namespace {

HMODULE g_orig;

template <typename F>
F Internal(uint32_t rva) { return reinterpret_cast<F>(reinterpret_cast<uint8_t*>(g_orig) + rva); }
template <typename F>
F Export(const char* name) { return reinterpret_cast<F>(GetProcAddress(g_orig, name)); }

constexpr uint32_t kTextureVtable = 0x8A9DC, kAttractorVtable = 0x8A834, kBoxVtable = 0x8A868, kSphereVtable = 0x8A89C;
constexpr uint32_t kTextureDtor = 0x477FE, kFrameDtor = 0x45471;
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
        {0x168D6, FN(ArchiveAttractor), "FAFAttractor_t::Archive (FUN_100168d6)"},
        {0x168EE, FN(CtorBox), "FAFCollisionBox_c(archive) ctor (FUN_100168ee)"},
        {0x16946, FN(InstantiateBox), "FAFCollisionBox_c::Instantiate"},
        {0x16AD8, FN(CtorSphere), "FAFCollisionSphere_c(archive) ctor (FUN_10016ad8)"},
        {0x16B30, FN(InstantiateSphere), "FAFCollisionSphere_c::Instantiate"},
        {0x17EC8, FN(InstantiateTexture), "FAFTexture_t::Instantiate"},
        {0x17F0C, FN(DeletingDtorTexture), "FAFTexture_t deleting destructor (FUN_10017f0c)"},
    };
#undef FN
    int installed = 0;
    for (const Entry& e : entries) installed += Replace(orig, e.rva, e.target, e.what) ? 1 : 0;
    Log("faf: %d of %d functions native", installed, int(sizeof(entries) / sizeof(entries[0])));
}

}  // namespace rnative::faf
