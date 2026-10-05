// fun::FindObjectFor<T> natively (randy-vk.ini [Native] Scene=on). Randy's archive readers ask the stream for a named
// object and cast it to the class they want; the original instantiations are ArchiveStream_c::DoFindObject (in
// serialize.dll) then an __RTDynamicCast to the class's RTTI descriptor (in the original's .rdata). We do the same:
// serialize::Api::findObject, then msvcr100's __RTDynamicCast with the original's descriptors.
#include "native/findobject.h"

#include "native/serialize.h"

#include <windows.h>

namespace rnative::findobject {

namespace {

HMODULE g_orig;

void* At(uint32_t rva) { return reinterpret_cast<uint8_t*>(g_orig) + rva; }
constexpr uint32_t kSerializableRtti = 0xB60D4;             // fun::Serializable_c's RTTI descriptor

// FUN_10046db0's shape: find the object, cast it, and either store it or report "wrong type".
int32_t Find(void* stream, const char* name, void** out, intptr_t index, uint32_t targetRtti)
{
    void* object = nullptr;
    const int32_t err = serialize::Get().findObject(stream, nullptr, name, &object, int32_t(index));
    if (err != 0) return err;
    static const auto cast = [] {
        return reinterpret_cast<void*(__cdecl*)(void*, int, void*, void*, int)>(
            GetProcAddress(GetModuleHandleA("msvcr100.dll"), "__RTDynamicCast"));
    }();
    void* result = cast(object, 0, At(kSerializableRtti), At(targetRtti), 0);
    if (!object || result)
        *out = result;
    else
        return 1;
    return 0;
}

// One instantiation per rva (the second column is its target class's RTTI descriptor).
#define FIND(rva, rtti)                                                                          \
    int32_t __fastcall Find_##rva(void* stream, void*, const char* name, void** out, intptr_t index) \
    {                                                                                            \
        return Find(stream, name, out, index, rtti);                                             \
    }
FIND(0x13D19, 0xB617C)
FIND(0x2BDDC, 0xB60B8)
FIND(0x2F989, 0xB6544)
FIND(0x414DF, 0xB7544)
FIND(0x460B8, 0xB62D4)
FIND(0x46101, 0xB629C)
FIND(0x46DB0, 0xB60A0)
FIND(0x47ECC, 0xB6614)
#undef FIND

}  // namespace

void Install(HMODULE orig)
{
    if (GetMode("Scene", Mode::Off) != Mode::On) return;
    g_orig = orig;
    if (!serialize::Get().findObject) {
        Log("find object: serialize.dll missing - not replaced");
        return;
    }
    struct Entry {
        uint32_t rva;
        void* target;
        const char* what;
    };
#define FN(f) reinterpret_cast<void*>(&f)
    const Entry entries[] = {
        {0x13D19, FN(Find_0x13D19), "fun::FindObjectFor<T> (FUN_10013d19)"},
        {0x2BDDC, FN(Find_0x2BDDC), "fun::FindObjectFor<T> (FUN_1002bddc)"},
        {0x2F989, FN(Find_0x2F989), "fun::FindObjectFor<T> (FUN_1002f989)"},
        {0x414DF, FN(Find_0x414DF), "fun::FindObjectFor<T> (FUN_100414df)"},
        {0x460B8, FN(Find_0x460B8), "fun::FindObjectFor<T> (FUN_100460b8)"},
        {0x46101, FN(Find_0x46101), "fun::FindObjectFor<T> (FUN_10046101)"},
        {0x46DB0, FN(Find_0x46DB0), "fun::FindObjectFor<T> (FUN_10046db0)"},
        {0x47ECC, FN(Find_0x47ECC), "fun::FindObjectFor<T> (FUN_10047ecc)"},
    };
#undef FN
    int installed = 0;
    for (const Entry& e : entries) installed += Replace(orig, e.rva, e.target, e.what) ? 1 : 0;
    Log("find object: %d of %d functions native", installed, int(sizeof(entries) / sizeof(entries[0])));
}

}  // namespace rnative::findobject
