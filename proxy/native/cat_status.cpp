// CATStatus_t / CATStdioStatus_t natively (see cat_status.h).
#include "native/cat_status.h"

#include "native/vc10.h"

#include <windows.h>

#include <cstdint>

namespace rnative::catstatus {

namespace {

HMODULE g_orig;

template <typename T>
T& Field(void* object, uint32_t offset) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(object) + offset); }
void SetVtable(void* o, uint32_t rva) { Field<uintptr_t>(o, 0) = reinterpret_cast<uintptr_t>(g_orig) + rva; }

constexpr uint32_t kStatusVtable = 0x95DD8, kStdioVtable = 0x95DF4;

// The FILE* is the caller's (the game's msvcr100), so format through msvcr100's CRT rather than our own.
int Print(void* file, const char* format, const char* message)
{
    static const auto fprintf_ = reinterpret_cast<int(__cdecl*)(void*, const char*, ...)>(
        GetProcAddress(GetModuleHandleA("msvcr100.dll"), "fprintf"));
    return fprintf_ ? fprintf_(file, format, message) : -1;
}
void Close(void* file)
{
    static const auto fclose_ = reinterpret_cast<int(__cdecl*)(void*)>(
        GetProcAddress(GetModuleHandleA("msvcr100.dll"), "fclose"));
    if (fclose_) fclose_(file);
}

// ---- CATStatus_t ----

void __fastcall StatusDestroy(void* s)   // FUN_100556db
{
    SetVtable(s, kStatusVtable);
}

void* __fastcall StatusDelete(void* s, void*, uint8_t flags)   // FUN_100556f2 (vtable slot 0)
{
    if (!(flags & 2)) {
        SetVtable(s, kStatusVtable);
        if (flags & 1) vc10::Free(s);
        return s;
    }
    uint8_t* block = static_cast<uint8_t*>(s) - 4;
    for (uint32_t i = *reinterpret_cast<uint32_t*>(block); i-- > 0;)
        SetVtable(static_cast<uint8_t*>(s) + i * 4, kStatusVtable);
    if (flags & 1) vc10::Free(block);
    return block;
}

// ---- CATStdioStatus_t ----

void __fastcall StdioDestroy(void* s)   // FUN_10055883 (vtable slot 0's thunk calls it)
{
    SetVtable(s, kStdioVtable);
    if (Field<uint8_t>(s, 4)) Close(Field<void*>(s, 8));
    SetVtable(s, kStatusVtable);
}

void __fastcall VPrintStatus(void* s, void*, const char* message)
{
    Print(Field<void*>(s, 8), "CatStatus:%s\n", message);
}
void __fastcall VPrintWarningLevel1(void* s, void*, const char* message)
{
    Print(Field<void*>(s, 8), "CatWarning1:%s\n", message);
}
void __fastcall VPrintWarningLevel2(void* s, void*, const char* message)
{
    Print(Field<void*>(s, 8), "CatWarning2:%s\n", message);
}
void __fastcall VPrintError(void* s, void*, const char* message)
{
    Print(Field<void*>(s, 8), "CatError:%s\n", message);
}
void __fastcall VPrintDebug(void* s, void*, const char* message)
{
    Print(Field<void*>(s, 8), "CatDebug:%s\n", message);
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
        {0x556DB, FN(StatusDestroy), "CATStatus_t::~CATStatus_t"},
        {0x556F2, FN(StatusDelete), "CATStatus_t deleting destructor (FUN_100556f2)"},
        {0x55883, FN(StdioDestroy), "CATStdioStatus_t::~CATStdioStatus_t"},
        {0x558A4, FN(VPrintStatus), "CATStdioStatus_t::VPrintStatus"},
        {0x558BF, FN(VPrintWarningLevel1), "CATStdioStatus_t::VPrintWarningLevel1"},
        {0x558DA, FN(VPrintWarningLevel2), "CATStdioStatus_t::VPrintWarningLevel2"},
        {0x558F5, FN(VPrintError), "CATStdioStatus_t::VPrintError"},
        {0x55910, FN(VPrintDebug), "CATStdioStatus_t::VPrintDebug"},
    };
#undef FN
    int installed = 0;
    for (const Entry& e : entries) installed += Replace(orig, e.rva, e.target, e.what) ? 1 : 0;
    Log("cat status: %d of %d functions native", installed, int(sizeof(entries) / sizeof(entries[0])));
}

}  // namespace rnative::catstatus
