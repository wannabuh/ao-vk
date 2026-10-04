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

namespace rnative {

enum class Mode { Off, On, Verify };

// Where log lines go (randy-vk.log in the proxy; stdout in tests).
using LogFn = void (*)(const char* fmt, ...);
void SetLog(LogFn log);
void Log(const char* fmt, ...);

// randy-vk.ini [Native] `name` (written with `fallback` if absent).
void SetIniPath(const char* path);
const char* IniPath();
Mode GetMode(const char* name, Mode fallback);
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

// Replaces a vtable entry (`vtable` = rva of the table) that holds module + `expected`; returns the original.
void* HookSlot(HMODULE module, uint32_t vtable, uint32_t slot, uint32_t expected, void* target, const char* what);

// Every replacement, once randy31_orig.dll is loaded (native.cpp). `orig` is its module.
void Install(HMODULE orig);

}  // namespace rnative
