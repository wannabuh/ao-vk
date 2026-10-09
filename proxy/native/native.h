// Native Randy: our own implementations of randy31_orig.dll's code, replacing it piece by piece.
//
// Each replacement has a name and a mode, read from randy-vk.ini [Native] (Name=off|on|verify):
//   off     the original runs (nothing patched)
//   on      ours runs instead
//   verify  both run on the same input, the original's result is kept and differences are logged (testing)
// Replacements of internal functions patch the original's code (HookEntry) after checking its bytes, so a different
// client build is left alone. See docs/native.md.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <emmintrin.h>

namespace rnative {

// lrint in the default rounding mode - to nearest, ties to even, as the original's x87 fistp - in one SSE2 instruction.
// The CRT's lrint saves and restores the floating-point environment around each call: ~12% of the game thread in a
// profile (render list buckets, the occluder's horizons). Out-of-range values give 0x80000000, as lrint does here.
inline long Lrint(double x) { return _mm_cvtsd_si32(_mm_set_sd(x)); }
inline long Lrint(float x) { return _mm_cvtss_si32(_mm_set_ss(x)); }

enum class Mode { Off, On, Verify };

// Where log lines go (randy-vk.log in the proxy; stdout in tests).
using LogFn = void (*)(const char* fmt, ...);
void SetLog(LogFn log);
void Log(const char* fmt, ...);

// randy-vk.ini [Native] `name` (written with `fallback` if absent).
void SetIniPath(const char* path);
const char* IniPath();
Mode GetMode(const char* name, Mode fallback);

// "module+offset" for a code address (module = file name), or the bare address.
void DescribeAddress(uintptr_t address, char* out, size_t size);
const char* ModeName(Mode mode);

// Replaces the function at `rva` in `module` with a jump to `target` once the `count` bytes there equal `expected`
// (whole instructions, position independent, at least 5 bytes). Returns a trampoline running the original (the stolen
// instructions, then a jump back), or null if the bytes differ (unknown client build; nothing patched).
// `rel32`: offsets (in the stolen bytes) of rel32 operands of calls / jumps, moved for the trampoline. `abs32`: offsets
// of absolute addresses, which the loader moved with the module (`expected` has them at the image base 0x10000000).
struct HookFixups {
    const size_t* rel32 = nullptr;
    size_t rel32Count = 0;
    const size_t* abs32 = nullptr;
    size_t abs32Count = 0;
};
void* HookEntry(HMODULE module, uint32_t rva, const uint8_t* expected, size_t count, void* target, const char* what,
                const HookFixups& fixups = {});
// The same at an address in another module (GUI.dll), whose code the caller has checked: its first `count` bytes run
// in the returned trampoline (rel32 operands at the given offsets re-aimed), then the rest of it. Not registered for
// NativeFor / AddressFor (those are Randy's offsets).
void* HookAt(uint8_t* at, size_t count, const size_t* rel32, size_t rel32Count, void* target, const char* what);

// The client build the replacements were written against (randy31_orig.dll's PE timestamp 0x5CD328BA and image size
// 0x1ED000): Replace works only on it.
bool KnownBuild(HMODULE module);

// Replaces the function at `rva` outright: a jump to `target` over its entry, no way back. Refused (false, logged) on
// another build and for the functions in entry_guard.gen.h (shorter than the jump, or something branches into it).
// For functions ported whole; HookEntry for ones that still run the original.
bool Replace(HMODULE module, uint32_t rva, void* target, const char* what);
// Whether Replace would take `rva` (for replacements that must go in together or not at all).
bool Replaceable(HMODULE module, uint32_t rva);

// Replaces a vtable entry (`vtable` = rva of the table) that holds module + `expected`; returns the original.
void* HookSlot(HMODULE module, uint32_t vtable, uint32_t slot, uint32_t expected, void* target, const char* what);

// Logs the first few faults (access violations etc.) in Randy (ours or the original), DisplaySystem or Gamecode, with their module + offset and the
// return addresses on the stack, before anyone handles them (randy-vk.log).
void InstallCrashLog();

// The module the replacements run against (the original randy31_orig.dll), set once by Install.
void SetOriginal(HMODULE module);
// The native implementation registered for `rva` (by Replace or HookEntry), or null if there is none.
void* NativeFor(uint32_t rva);
// The address native code calls for `rva`: the native implementation if one is registered, else the original's.
// While the original is still loaded the two are the same, because the registered ones are patched over it; once it
// is gone this is the only way our code reaches its own functions.
void* AddressFor(uint32_t rva);

// Every replacement, once randy31_orig.dll is loaded (native.cpp). `orig` is its module.
void Install(HMODULE orig);

}  // namespace rnative
