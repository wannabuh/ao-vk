// Patching helpers and the [Native] modes (native.h).
#include "native/native.h"
#include "native/entry_guard.gen.h"

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
    Wrapped().insert(rva);
    g_native[rva] = target;
    return tramp;
}

void* HookAt(uint8_t* at, size_t count, const size_t* rel32, size_t rel32Count, void* target, const char* what)
{
    if (count < 5 || count > kTrampSize - 5)
        return nullptr;
    uint8_t* tramp = NewTrampoline();
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
    WriteJump(tramp + count, at + count);
    DWORD protect;
    if (!VirtualProtect(at, count, PAGE_EXECUTE_READWRITE, &protect)) {
        Log("%s: can't patch %p", what, (void*)at);
        return nullptr;
    }
    WriteJump(at, target);
    for (size_t i = 5; i < count; ++i)
        at[i] = 0xCC;
    VirtualProtect(at, count, protect, &protect);
    FlushInstructionCache(GetCurrentProcess(), at, count);
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

bool IsCode(uintptr_t address)                      // in a loaded module's image
{
    HMODULE m = nullptr;
    return GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<const char*>(address), &m);
}

LONG CALLBACK CrashLogger(EXCEPTION_POINTERS* e)
{
    static LONG reports;
    const DWORD code = e->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_STACK_OVERFLOW)
        return EXCEPTION_CONTINUE_SEARCH;
    char where[MAX_PATH + 32];
    Where(reinterpret_cast<uintptr_t>(e->ExceptionRecord->ExceptionAddress), where, sizeof(where));
    static const char* const kWatched[] = {"randy31.dll+", "randy31_orig.dll+", "DisplaySystem.dll+", "Gamecode.dll+"};
    bool watched = false;
    for (const char* m : kWatched) watched |= _strnicmp(where, m, std::strlen(m)) == 0;
    if (!watched || InterlockedIncrement(&reports) > 8) return EXCEPTION_CONTINUE_SEARCH;
    const ULONG_PTR* info = e->ExceptionRecord->ExceptionInformation;
    Log("fault %08lx at %s (%s %08lx), thread %lu - first chance, may be handled", (unsigned long)code, where,
        code == EXCEPTION_ACCESS_VIOLATION ? (info[0] ? "writing" : "reading") : "-",
        (unsigned long)(code == EXCEPTION_ACCESS_VIOLATION ? info[1] : 0), (unsigned long)GetCurrentThreadId());
    const CONTEXT* c = e->ContextRecord;
    Log("  eax %08lx ebx %08lx ecx %08lx edx %08lx esi %08lx edi %08lx ebp %08lx esp %08lx", c->Eax, c->Ebx, c->Ecx,
        c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    // Return addresses on the stack (anything pointing into a module).
    const auto* stack = reinterpret_cast<const uintptr_t*>(c->Esp);
    int shown = 0;
    for (int i = 0; i < 256 && shown < 20; ++i) {
        uintptr_t v;
        __try {
            v = stack[i];
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            break;
        }
        if (v < 0x10000 || !IsCode(v)) continue;
        Where(v, where, sizeof(where));
        Log("  [esp+%03x] %s", i * 4, where);
        ++shown;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void DescribeAddress(uintptr_t address, char* out, size_t size) { Where(address, out, size); }

void InstallCrashLog()
{
    static bool done;
    if (!done) {
        done = true;
        AddVectoredExceptionHandler(1, &CrashLogger);
    }
}

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
    WriteJump(at, target);
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
