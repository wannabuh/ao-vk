// Patching helpers and the [Native] modes (native.h).
#include "native/native.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace rnative {

namespace {

void NoLog(const char*, ...) {}
LogFn g_log = &NoLog;
char g_ini[MAX_PATH];

// Trampolines live in one executable page, handed out in 32-byte pieces.
uint8_t* g_tramp;
size_t g_trampUsed;
constexpr size_t kTrampPage = 4096, kTrampSize = 32;

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

}  // namespace

void SetLog(LogFn log) { g_log = log ? log : &NoLog; }

void Log(const char* fmt, ...)
{
    char line[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    g_log("native: %s", line);
}

void SetIniPath(const char* path)
{
    std::snprintf(g_ini, sizeof(g_ini), "%s", path ? path : "");
}

const char* IniPath()
{
    return g_ini;
}

const char* ModeName(Mode mode)
{
    return mode == Mode::On ? "on" : mode == Mode::Verify ? "verify" : "off";
}

Mode GetMode(const char* name, Mode fallback)
{
    if (!g_ini[0])
        return fallback;
    char value[32] = "";
    GetPrivateProfileStringA("Native", name, "", value, sizeof(value), g_ini);
    if (!value[0]) {
        WritePrivateProfileStringA("Native", name, ModeName(fallback), g_ini);
        return fallback;
    }
    if (!_stricmp(value, "on") || !std::strcmp(value, "1")) return Mode::On;
    if (!_stricmp(value, "verify")) return Mode::Verify;
    return Mode::Off;
}

void* HookEntry(HMODULE module, uint32_t rva, const uint8_t* expected, size_t count, void* target, const char* what,
                const HookFixups& fixups)
{
    auto* at = reinterpret_cast<uint8_t*>(module) + rva;
    uint8_t want[kTrampSize];
    if (count <= kTrampSize) {
        std::memcpy(want, expected, count);
        const uint32_t delta = uint32_t(reinterpret_cast<uintptr_t>(module) - 0x10000000u);
        for (size_t i = 0; i < fixups.abs32Count; ++i) {
            uint32_t v;
            std::memcpy(&v, want + fixups.abs32[i], 4);
            v += delta;
            std::memcpy(want + fixups.abs32[i], &v, 4);
        }
    }
    if (count < 5 || count > kTrampSize - 5 || std::memcmp(at, want, count) != 0) {
        Log("%s: unexpected code at %p - unknown client build, not replaced", what, (void*)at);
        return nullptr;
    }
    uint8_t* tramp = NewTrampoline();
    if (!tramp)
        return nullptr;
    std::memcpy(tramp, at, count);
    for (size_t i = 0; i < fixups.rel32Count; ++i) {   // relative operands keep pointing at their targets
        const size_t o = fixups.rel32[i];
        int32_t rel;
        std::memcpy(&rel, at + o, 4);
        uintptr_t dest = reinterpret_cast<uintptr_t>(at + o + 4) + rel;
        rel = int32_t(dest - reinterpret_cast<uintptr_t>(tramp + o + 4));
        std::memcpy(tramp + o, &rel, 4);
    }
    WriteJump(tramp + count, at + count);
    DWORD protect;
    if (!VirtualProtect(at, count, PAGE_EXECUTE_READWRITE, &protect))
        return nullptr;
    WriteJump(at, target);
    for (size_t i = 5; i < count; ++i)
        at[i] = 0xCC;                                      // never reached: the jump skips the rest
    VirtualProtect(at, count, protect, &protect);
    FlushInstructionCache(GetCurrentProcess(), at, count);
    return tramp;
}

void* HookSlot(HMODULE module, uint32_t vtable, uint32_t slot, uint32_t expected, void* target, const char* what)
{
    auto base = reinterpret_cast<uintptr_t>(module);
    auto* entry = reinterpret_cast<uintptr_t*>(base + vtable + 4 * slot);
    if (*entry != base + expected) {
        Log("%s: vtable slot holds %p, expected %p - unknown client build, not replaced", what, (void*)*entry,
            (void*)(base + expected));
        return nullptr;
    }
    DWORD protect;
    if (!VirtualProtect(entry, sizeof(*entry), PAGE_READWRITE, &protect))
        return nullptr;
    void* original = reinterpret_cast<void*>(*entry);
    *entry = reinterpret_cast<uintptr_t>(target);
    VirtualProtect(entry, sizeof(*entry), protect, &protect);
    return original;
}

}  // namespace rnative
