// Patching code at run time (hooklib.h).
#include "native/hooklib.h"

#include <cstring>

namespace rnative {

namespace hooklib {

namespace {
uint8_t* g_tramp;
size_t g_trampUsed;
constexpr size_t kTrampPage = 4096;
}  // namespace

uint8_t* NewTrampoline()
{
    if (!g_tramp || g_trampUsed + kTrampSize > kTrampPage) {
        g_tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, kTrampPage, MEM_COMMIT | MEM_RESERVE,
                                                     PAGE_EXECUTE_READWRITE));
        g_trampUsed = 0;
        if (!g_tramp)
            return nullptr;
    }
    uint8_t* t = g_tramp + g_trampUsed;
    g_trampUsed += kTrampSize;
    return t;
}

void WriteJump(uint8_t* at, const void* to)
{
    at[0] = 0xE9;
    int32_t rel = int32_t(reinterpret_cast<uintptr_t>(to) - (reinterpret_cast<uintptr_t>(at) + 5));
    std::memcpy(at + 1, &rel, 4);
}

}  // namespace hooklib

void* HookAt(uint8_t* at, size_t count, const size_t* rel32, size_t rel32Count, void* target, const char* what)
{
    using hooklib::kTrampSize;
    if (count < 5 || count > kTrampSize - 5)
        return nullptr;
    uint8_t* tramp = hooklib::NewTrampoline();
    if (!tramp)
        return nullptr;
    std::memcpy(tramp, at, count);
    for (size_t i = 0; i < rel32Count; ++i) {
        const size_t o = rel32[i];
        int32_t rel;
        std::memcpy(&rel, at + o, 4);
        uintptr_t dest = reinterpret_cast<uintptr_t>(at + o + 4) + rel;
        rel = int32_t(dest - reinterpret_cast<uintptr_t>(tramp + o + 4));
        std::memcpy(tramp + o, &rel, 4);
    }
    hooklib::WriteJump(tramp + count, at + count);
    DWORD protect;
    if (!VirtualProtect(at, count, PAGE_EXECUTE_READWRITE, &protect)) {
        Log("%s: can't patch %p", what, (void*)at);
        return nullptr;
    }
    hooklib::WriteJump(at, target);
    for (size_t i = 5; i < count; ++i)
        at[i] = 0xCC;
    VirtualProtect(at, count, protect, &protect);
    FlushInstructionCache(GetCurrentProcess(), at, count);
    return tramp;
}

uint8_t* CheckedEntry(HMODULE gui, const char* name, const char* what)
{
    auto* at = reinterpret_cast<uint8_t*>(GetProcAddress(gui, name));
    if (!at) {
        Log("interface: %s not exported - not hooked", what);
        return nullptr;
    }
    constexpr uint32_t kEhProlog = 0x1738A4;        // FUN_101738a4 in GUI.dll (image base 0x10000000)
    int32_t rel;
    std::memcpy(&rel, at + 6, 4);
    const uint8_t* callee = at + 10 + rel;
    if (at[0] != 0xB8 || at[5] != 0xE8 || callee != reinterpret_cast<uint8_t*>(gui) + kEhProlog) {
        Log("interface: %s - unexpected code (another GUI.dll build?), not hooked", what);
        return nullptr;
    }
    return at;
}

}  // namespace rnative
