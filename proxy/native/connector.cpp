// RConnector, RRefFrameConnector and DummyConnector natively (randy-vk.ini [Native] Scene=on): the connector an
// RRefFrame_t hands out for an attachment point. RConnector is the abstract base (its originator / matrix / setter
// are pure there); RRefFrameConnector answers with its originator frame's world matrix; DummyConnector is Gamecode's
// stand-in (a matrix of its own, no originator).
//
// RResource_t (0x2C bytes) base. RConnector (vtable 0x8A7FC) adds nothing (its Archive is RResource_t's and the
// stream). RRefFrameConnector (0x30 bytes, vtable 0x8A818): +0x2C its originator RRefFrame_t*. DummyConnector
// (0x30 bytes, vtable 0x93F68): +0x2C a TMatrix4_t of its own.
#include "native/connector.h"

#include "native/helpers.h"
#include "native/orig_api.gen.h"
#include "native/serialize.h"
#include "native/vc10.h"

namespace rnative::connector {

namespace {

HMODULE g_orig;

template <typename T>
T& Field(void* object, uint32_t offset) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(object) + offset); }
template <typename T>
T& Global(uint32_t rva) { return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(g_orig) + rva); }
template <typename F>
F Internal(uint32_t rva) { return reinterpret_cast<F>(reinterpret_cast<uint8_t*>(g_orig) + rva); }
void SetVtable(void* o, uint32_t rva) { Field<uintptr_t>(o, 0) = reinterpret_cast<uintptr_t>(g_orig) + rva; }

const serialize::Api& S() { return serialize::Get(); }

constexpr uint32_t kRConnectorVtable = 0x8A7FC, kConnectorVtable = 0x8A818, kDummyVtable = 0x93F68;
constexpr uint32_t kOriginator = 0x2C, kSize = 0x30;
constexpr uint32_t kRResourceCtor = 0x46311, kRResourceArchiveCtor = 0x46362, kRResourceArchive = 0x4641A,
                   kRResourceDtor = 0x46283, kFrameConnect = 0x4514B, kRRefFrameTd = 0xB60B8;
constexpr uint32_t kIdentityGuard = 0x17D190, kIdentityMatrix = 0x17D150, kDummyOriginator = 0x17D198;

// ArchiveStream_c::FindObject<RRefFrame_t> (FUN_1002bddc): the object `name` #index as an RRefFrame_t.
int32_t FindFrame(void* stream, const char* name, void** out, int32_t index)
{
    using DynamicCastFn = void*(__cdecl*)(void*, long, void*, void*, int);
    static const auto cast =
        reinterpret_cast<DynamicCastFn>(GetProcAddress(GetModuleHandleA("msvcr100.dll"), "__RTDynamicCast"));
    void* object = nullptr;
    int32_t error = S().findObject(stream, nullptr, name, &object, index);
    if (error) return error;
    void* frame = object ? cast(object, 0, &Global<uint8_t>(0xB60D4), &Global<uint8_t>(kRRefFrameTd), 0) : nullptr;
    if (object && !frame) return 1;
    *out = frame;
    return 0;
}

// ---- RConnector (abstract) ----

void* __fastcall RConnectorConstructFrom(void* c, void*, void* archive)   // FUN_1002bc5f
{
    Internal<void*(__fastcall*)(void*, void*, void*)>(kRResourceArchiveCtor)(c, nullptr, archive);
    SetVtable(c, kRConnectorVtable);
    S().getStream(archive, nullptr);
    return c;
}

void __fastcall RConnectorArchive(void* c, void*, void* archive)   // FUN_1002bc9c
{
    Internal<void(__fastcall*)(void*, void*, void*)>(kRResourceArchive)(c, nullptr, archive);
    S().getStream(archive, nullptr);
}

void* __fastcall RConnectorDelete(void* c, void*, uint8_t flags)   // FUN_1002be25 (vtable slot 0)
{
    Internal<void(__fastcall*)(void*)>(kRResourceDtor)(c);
    if (flags & 1) vc10::Free(c);
    return c;
}

// ---- RRefFrameConnector (an RConnector: its originator frame) ----

void* __fastcall ConnectorConstruct(void* c, void*, const char* name, void* originator)   // 0x166a5
{
    Internal<void*(__fastcall*)(void*, void*, const char*)>(kRResourceCtor)(c, nullptr, name);
    SetVtable(c, kConnectorVtable);
    Field<void*>(c, kOriginator) = originator;
    if (originator) Internal<void(__fastcall*)(void*, void*, void*)>(kFrameConnect)(originator, nullptr, c);
    return c;
}

void __fastcall ConnectorSetOriginator(void* c, void*, void* originator)   // 0x2bcb4 (vtable slot 5)
{
    Field<void*>(c, kOriginator) = originator;
    if (originator) Internal<void(__fastcall*)(void*, void*, void*)>(kFrameConnect)(originator, nullptr, c);
}

void* __fastcall ConnectorGetOriginator(void* c) { return Field<void*>(c, kOriginator); }   // 0x1667e (slot 4)

void __fastcall ConnectorArchive(void* c, void*, void* archive)   // 0x2bccd (vtable slot 1)
{
    RConnectorArchive(c, nullptr, archive);
    void* stream = S().getStream(archive, nullptr);
    S().addObject(stream, nullptr, "originator", Field<void*>(c, kOriginator));
}

void* __fastcall ConnectorConstructFrom(void* c, void*, void* archive)   // 0x2bd18
{
    RConnectorConstructFrom(c, nullptr, archive);
    SetVtable(c, kConnectorVtable);
    void* stream = S().getStream(archive, nullptr);
    FindFrame(stream, "originator", &Field<void*>(c, kOriginator), 0);
    return c;
}

// 0x2bd6a (vtable slot 3): the originator's world matrix, or a shared identity.
const void* __fastcall ConnectorRequestMatrix(void* c)
{
    if (void* originator = Field<void*>(c, kOriginator)) return orig::RRefFrame_t_GetWorldMatrix(originator);
    uint32_t& guard = Global<uint32_t>(kIdentityGuard);
    if (!(guard & 1)) {
        guard |= 1;
        helpers::Identity(&Global<float>(kIdentityMatrix), nullptr);
    }
    return &Global<float>(kIdentityMatrix);
}

void* __cdecl ConnectorInstantiate(void* archive)
{
    void* c = vc10::Allocate(kSize);
    return c ? ConnectorConstructFrom(c, nullptr, archive) : nullptr;
}

// ---- DummyConnector (Gamecode's: a matrix of its own) ----

void* __fastcall DummyConstruct(void* c, void*)
{
    Internal<void*(__fastcall*)(void*, void*, const char*)>(kRResourceCtor)(c, nullptr, "");
    SetVtable(c, kDummyVtable);
    helpers::Identity(static_cast<uint8_t*>(c) + kOriginator, nullptr);
    return c;
}

void __fastcall DummyNoop(void*, void*) {}
void* __fastcall DummyGetOriginator(void*) { return reinterpret_cast<uint8_t*>(g_orig) + kDummyOriginator; }
void* __fastcall DummyRequestMatrix(void* c) { return static_cast<uint8_t*>(c) + kOriginator; }

}  // namespace

void Install(HMODULE orig)
{
    if (GetMode("Scene", Mode::Off) != Mode::On) return;
    g_orig = orig;
    if (!S().complete || !S().findObject) {
        Log("connectors: serialize.dll missing - not replaced");
        return;
    }
    struct Entry {
        uint32_t rva;
        void* target;
        const char* what;
    };
#define FN(f) reinterpret_cast<void*>(&f)
    const Entry entries[] = {
        {0x2BC5F, FN(RConnectorConstructFrom), "RConnector::RConnector(archive) (FUN_1002bc5f)"},
        {0x2BC9C, FN(RConnectorArchive), "RConnector::Archive (FUN_1002bc9c)"},
        {0x2BE25, FN(RConnectorDelete), "RConnector deleting destructor (FUN_1002be25)"},
        {0x166A5, FN(ConnectorConstruct), "RRefFrameConnector::RRefFrameConnector(name, originator)"},
        {0x2BCB4, FN(ConnectorSetOriginator), "RRefFrameConnector::SetOriginator"},
        {0x2BCCD, FN(ConnectorArchive), "RRefFrameConnector::Archive"},
        {0x2BD18, FN(ConnectorConstructFrom), "RRefFrameConnector::RRefFrameConnector(archive)"},
        {0x2BD6A, FN(ConnectorRequestMatrix), "RRefFrameConnector::RequestMatrix"},
        {0x2BDA7, FN(ConnectorInstantiate), "RRefFrameConnector::Instantiate"},
        {0x2BCF9, FN(DummyConstruct), "DummyConnector::DummyConnector"},
        {0x16682, FN(DummyGetOriginator), "DummyConnector::GetOriginator"},
    };
    int installed = 0;
    for (const Entry& e : entries) installed += Replace(orig, e.rva, e.target, e.what) ? 1 : 0;
    // The 3-4-byte vtable methods the entry guard refuses: their slots point at ours instead.
    installed += HookSlot(orig, kConnectorVtable, 4, 0x1667E, FN(ConnectorGetOriginator),
                          "RRefFrameConnector::GetOriginator") ? 1 : 0;
    installed += HookSlot(orig, kDummyVtable, 1, 0x16688, FN(DummyNoop), "DummyConnector::Archive") ? 1 : 0;
    installed += HookSlot(orig, kDummyVtable, 5, 0x16688, FN(DummyNoop), "DummyConnector::SetOriginator") ? 1 : 0;
    installed += HookSlot(orig, kDummyVtable, 3, 0x1668B, FN(DummyRequestMatrix),
                          "DummyConnector::RequestMatrix") ? 1 : 0;
#undef FN
    Log("connectors: %d of 15 functions native", installed);
}

}  // namespace rnative::connector
