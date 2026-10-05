// fun::DXError's text natively (dxerror.cpp), installed with the device layer ([Native] Device=on).
#pragma once

#include "native/native.h"
#include "native/vc10.h"

#include <cstdint>

namespace rnative::dxerror {

void Install(HMODULE orig);

// An HRESULT's name and description as Randy has them (FUN_1001d619); false if it doesn't know it.
bool Describe(int32_t hr, const char** description, const char** name);

// ?GetErrorString@DXError@fun@@UBE?AV?$basic_string...@XZ (virtual; DisplaySystem calls it through its import): the
// message, "\r\n", then "NAME:description" or "0x%08X: unknown". `out` is the caller's uninitialised return slot.
vc10::String* __fastcall GetErrorString(const uint8_t* error, void*, vc10::String* out);

// Throws the original's fun::DXError (made by its constructor, thrown with its ThrowInfo), as render_t does when a
// D3D call fails: DisplaySystem catches it.
[[noreturn]] void Throw(int32_t hr, const char* message, const char* file, int line);

}  // namespace rnative::dxerror
