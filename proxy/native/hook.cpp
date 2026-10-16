// Patching helpers and the [Native] modes (native.h).
#include "native/native.h"
#include "native/entry_guard.gen.h"
#include "native/hooklib.h"

#include <algorithm>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <set>
#include <unordered_map>

namespace rnative {

namespace {

void NoLog(const char*, ...) {}
LogFn g_log = &NoLog;
char g_ini[MAX_PATH];

// The original module, and the native implementation registered for each rva (Replace / HookEntry). AddressFor is
// what native code calls instead of taking the original's address: the native one if we have it, else the original's.
HMODULE g_original;
std::unordered_map<uint32_t, void*> g_native;

}  // namespace

void SetLog(LogFn log) { g_log = log ? log : &NoLog; }

void SetOriginal(HMODULE module) { g_original = module; }

void* NativeFor(uint32_t rva)
{
    auto it = g_native.find(rva);
    return it == g_native.end() ? nullptr : it->second;
}

void* AddressFor(uint32_t rva)
{
    if (void* native = NativeFor(rva)) return native;
    return g_original ? reinterpret_cast<uint8_t*>(g_original) + rva : nullptr;
}

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

// Entries already wrapped (HookEntry): a Replace of one of them would silently drop the wrap.
std::set<uint32_t>& Wrapped()
{
    static std::set<uint32_t> wrapped;
    return wrapped;
}

void* HookEntry(HMODULE module, uint32_t rva, const uint8_t* expected, size_t count, void* target, const char* what,
                const HookFixups& fixups)
{
    auto* at = reinterpret_cast<uint8_t*>(module) + rva;
    uint8_t want[hooklib::kTrampSize];
    if (count <= hooklib::kTrampSize) {
        std::memcpy(want, expected, count);
        const uint32_t delta = uint32_t(reinterpret_cast<uintptr_t>(module) - 0x10000000u);
        for (size_t i = 0; i < fixups.abs32Count; ++i) {
            uint32_t v;
            std::memcpy(&v, want + fixups.abs32[i], 4);
            v += delta;
            std::memcpy(want + fixups.abs32[i], &v, 4);
        }
    }
    if (count < 5 || count > hooklib::kTrampSize - 5 || std::memcmp(at, want, count) != 0) {
        Log("%s: unexpected code at %p - unknown client build, not replaced", what, (void*)at);
        return nullptr;
    }
    uint8_t* tramp = hooklib::NewTrampoline();
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
    hooklib::WriteJump(tramp + count, at + count);
    DWORD protect;
    if (!VirtualProtect(at, count, PAGE_EXECUTE_READWRITE, &protect))
        return nullptr;
    hooklib::WriteJump(at, target);
    for (size_t i = 5; i < count; ++i)
        at[i] = 0xCC;                                      // never reached: the jump skips the rest
    VirtualProtect(at, count, protect, &protect);
    FlushInstructionCache(GetCurrentProcess(), at, count);
    Wrapped().insert(rva);
    g_native[rva] = target;
    return tramp;
}

namespace {

// "module+offset" for an address (module = file name), or the bare address.
void Where(uintptr_t address, char* out, size_t size)
{
    HMODULE m = nullptr;
    char path[MAX_PATH] = "";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<const char*>(address), &m) &&
        GetModuleFileNameA(m, path, sizeof(path))) {
        const char* name = std::strrchr(path, '\\');
        std::snprintf(out, size, "%s+%lx", name ? name + 1 : path, (unsigned long)(address - reinterpret_cast<uintptr_t>(m)));
    } else {
        std::snprintf(out, size, "%08lx", (unsigned long)address);
    }
}

}  // namespace

void DescribeAddress(uintptr_t address, char* out, size_t size) { Where(address, out, size); }

bool KnownBuild(HMODULE module)
{
    static int known = -1;
    if (known < 0) {
        const auto* base = reinterpret_cast<const uint8_t*>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
        known = nt->FileHeader.TimeDateStamp == 0x5CD328BAu && nt->OptionalHeader.SizeOfImage == 0x1ED000u ? 1 : 0;
        if (!known)
            Log("randy31_orig.dll is not the build the replacements know (timestamp %08lx, size %lx)",
                (unsigned long)nt->FileHeader.TimeDateStamp, (unsigned long)nt->OptionalHeader.SizeOfImage);
    }
    return known == 1;
}

bool Replaceable(HMODULE module, uint32_t rva)
{
    return KnownBuild(module) && !std::binary_search(std::begin(kUnsafeEntries), std::end(kUnsafeEntries), rva);
}

bool Replace(HMODULE module, uint32_t rva, void* target, const char* what)
{
    if (!KnownBuild(module))
        return false;
    if (std::binary_search(std::begin(kUnsafeEntries), std::end(kUnsafeEntries), rva)) {
        Log("%s: can't take a jump over its entry (entry_guard.gen.h), not replaced", what);
        return false;
    }
    if (Wrapped().count(rva)) {
        Log("%s: already wrapped by another [Native] mode (HookEntry) - not replaced; merge the two", what);
        return false;
    }
    auto* at = reinterpret_cast<uint8_t*>(module) + rva;
    DWORD protect;
    if (!VirtualProtect(at, 5, PAGE_EXECUTE_READWRITE, &protect))
        return false;
    hooklib::WriteJump(at, target);
    VirtualProtect(at, 5, protect, &protect);
    FlushInstructionCache(GetCurrentProcess(), at, 5);
    g_native[rva] = target;
    return true;
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
