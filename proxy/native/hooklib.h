// Patching code at run time: randy31.dll's, and a plugin's built against ao-vk (docs/ui-dll.md) - each links it and
// defines rnative::Log itself.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace rnative {

// A line in the log (each DLL's own definition).
void Log(const char* fmt, ...);

// Replaces the code at `at` (in a module whose code the caller has checked) with a jump to `target`: its first
// `count` bytes (whole instructions, at least 5) run in the returned trampoline - rel32 operands at the given offsets
// re-aimed - then the rest of it. Null if it can't be patched.
void* HookAt(uint8_t* at, size_t count, const size_t* rel32, size_t rel32Count, void* target, const char* what);

// A GUI.dll export that starts with MSVC's exception frame prologue - mov eax, <handler>; call <GUI.dll's _EH_prolog>
// (the PRK client's GUI.dll, 0x5CD32BEA) - so that HookAt(at, 10, {6}) may take it; null (logged) when it isn't
// exported or its code differs (another GUI.dll build: left alone).
uint8_t* CheckedEntry(HMODULE gui, const char* name, const char* what);

namespace hooklib {

// Trampolines live in executable pages, handed out in pieces of kTrampSize bytes.
constexpr size_t kTrampSize = 32;
uint8_t* NewTrampoline();
void WriteJump(uint8_t* at, const void* to);        // jmp rel32 (5 bytes)

}  // namespace hooklib

}  // namespace rnative
