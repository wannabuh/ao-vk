// RResource_t natively (part of [Native] Device=on): the base of textures, materials, animations - a named,
// reference counted fun::Serializable_c (serialize.dll).
//
// RResource_t (0x2C bytes): +0x00 vtable (0x100956C4 for a plain one), +0x04 Serializable_c's, +0x08 name
// (std::string), +0x24 references, +0x28 0xFF (0: counted in 0x1017D448). 0x1017D444: references to all resources.
#include "native/resource.h"
#include "native/serialize.h"
#include "native/vc10.h"

#include <cstdio>
#include <cstring>

namespace rnative::resource {

namespace {

HMODULE g_orig;

template <typename T>
T& Field(void* object, uint32_t offset) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(object) + offset); }
template <typename T>
T& Global(uint32_t rva) { return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(g_orig) + rva); }

constexpr uint32_t kVtable = 0x956C4, kTotalRefs = 0x17D444, kCounted = 0x17D448;

const serialize::Api& S() { return serialize::Get(); }

vc10::String& Name(void* r) { return Field<vc10::String>(r, 8); }

void __fastcall AddRef(void* r)
{
    ++Field<int32_t>(r, 0x24);
    ++Global<int32_t>(kTotalRefs);
}

void __fastcall Release(void* r)
{
    int32_t& refs = Field<int32_t>(r, 0x24);
    if (refs == 0) {
        std::printf("SERIOUS!!!! RResource_t::Release: is releasing a resource which is already released\n");
        return;
    }
    --refs;
    --Global<int32_t>(kTotalRefs);
    if (refs == 0)                                  // the scalar deleting destructor (vtable slot 0)
        reinterpret_cast<void*(__fastcall*)(void*, void*, uint32_t)>((*static_cast<void***>(r))[0])(r, nullptr, 1);
}

const char* __fastcall GetName(void* r) { return Name(r).c_str(); }

void* __fastcall Construct(void* r, void*, const char* name)
{
    S().construct(r, nullptr);
    Field<uintptr_t>(r, 0) = reinterpret_cast<uintptr_t>(g_orig) + kVtable;
    Name(r).init();
    Name(r).assign(name, std::strlen(name));
    Field<int32_t>(r, 0x24) = 1;
    Field<int32_t>(r, 0x28) = 0xFF;
    ++Global<int32_t>(kTotalRefs);
    return r;
}

// FUN_100462ad: a copy (Serializable_c's word copied as its inline copy constructor does), counted, uncounted flag.
void* __fastcall Copy(void* r, void*, void* from)
{
    Field<uint32_t>(r, 4) = Field<uint32_t>(from, 4);
    Field<uintptr_t>(r, 0) = reinterpret_cast<uintptr_t>(g_orig) + kVtable;
    Name(r).init();
    Name(r).allocator = 0;
    const char* n = Name(from).c_str();
    Name(r).assign(n, std::strlen(n));
    Field<int32_t>(r, 0x24) = 1;
    Field<int32_t>(r, 0x28) = 0xFF;
    ++Global<int32_t>(kTotalRefs);
    return r;
}

void __fastcall SetCounted(void* r, void*, int32_t value)   // FUN_1004624f
{
    Field<int32_t>(r, 0x28) = value;
    if (value == 0) ++Global<int32_t>(kCounted);
}

void __fastcall SetReferences(void* r, void*, int32_t value) { Field<int32_t>(r, 0x24) = value; }   // FUN_10046266

// From an archive: the name (or "*unknown*"); not counted in the references to all resources.
void* __fastcall ConstructFrom(void* r, void*, void* archive)
{
    S().constructFrom(r, nullptr, archive);
    Field<uintptr_t>(r, 0) = reinterpret_cast<uintptr_t>(g_orig) + kVtable;
    Name(r).init();
    Field<int32_t>(r, 0x24) = 1;
    Field<int32_t>(r, 0x28) = 0xFF;
    void* stream = S().getStream(archive, nullptr);
    vc10::String found;
    found.init();
    found.allocator = 0;
    if (S().findString(stream, nullptr, "name", &found, 0) == 0)
        Name(r).assign(found.c_str(), found.size);
    else
        Name(r).assign("*unknown*", 9);
    found.release();
    return r;
}

void __fastcall Destroy(void* r)
{
    Field<uintptr_t>(r, 0) = reinterpret_cast<uintptr_t>(g_orig) + kVtable;
    if (Field<int32_t>(r, 0x28) == 0) --Global<int32_t>(kCounted);
    Name(r).release();
    S().destroy(r, nullptr);
}

void __fastcall Archive(void* r, void*, void* archive)
{
    void* stream = S().getStream(archive, nullptr);
    S().addInt32(stream, nullptr, "version", 1);
    vc10::String name;
    name.init();
    name.allocator = 0;
    const char* n = Name(r).c_str();
    name.assign(n, std::strlen(n));
    S().addString(stream, nullptr, "name", &name);
    name.release();
}

void* __cdecl Instantiate(void* archive)
{
    void* r = vc10::Allocate(0x2C);
    return ConstructFrom(r, nullptr, archive);
}

}  // namespace

void Install(HMODULE orig)
{
    g_orig = orig;
    if (!S().complete) {
        Log("resources: serialize.dll exports missing - not replaced");
        return;
    }
    struct Entry {
        uint32_t rva;
        void* target;
        const char* what;
    };
#define FN(f) reinterpret_cast<void*>(&f)
    const Entry entries[] = {
        {0x4621A, FN(AddRef), "RResource_t::AddRefRResource"},
        {0x46224, FN(Release), "RResource_t::ReleaseRResource"},
        {0x46277, FN(GetName), "RResource_t::GetName"},
        {0x46283, FN(Destroy), "RResource_t::~RResource_t"},
        {0x46311, FN(Construct), "RResource_t::RResource_t(name)"},
        {0x46362, FN(ConstructFrom), "RResource_t::RResource_t(archive)"},
        {0x4641A, FN(Archive), "RResource_t::Archive"},
        {0x46499, FN(Instantiate), "RResource_t::Instantiate"},
        {0x462AD, FN(Copy), "RResource_t::RResource_t(copy) (FUN_100462ad)"},
        {0x4624F, FN(SetCounted), "RResource_t counted flag (FUN_1004624f)"},
        {0x46266, FN(SetReferences), "RResource_t references (FUN_10046266)"},
    };
#undef FN
    int installed = 0;
    for (const Entry& e : entries) installed += Replace(orig, e.rva, e.target, e.what) ? 1 : 0;
    Log("resources: %d of %d functions native", installed, int(sizeof(entries) / sizeof(entries[0])));
}

}  // namespace rnative::resource
