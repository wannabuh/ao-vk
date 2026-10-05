// Small shared helpers (randy-vk.ini [Native] Scene=on): the Vector3 cross product and scale, TMatrix4_t's identity,
// the frame-hierarchy NextNode walk, and two debug-descriptor stubs. These are the functions tools/port-status.py has
// no named function for (the free functions), used across the mesh, camera, shadow and debug code.
#pragma once

#include "native/native.h"

#include <cstdint>

namespace rnative::helpers {

void Install(HMODULE orig);
void SetModule(HMODULE orig);

float* __fastcall NormalizeScale(float* self, void*, float scale);                           // FUN_10018833
void __fastcall NormalizeScaleTo(const float* self, void*, float* out, float scale);         // FUN_10018864
void __fastcall TranslateAdd(float* self, void*, const float* v);                            // FUN_1002cdb2
void __fastcall MatrixMove(float* self, void*, float a, float b);                            // FUN_10046518
float* __fastcall Subtract(const float* self, void*, float* out, const float* other);        // FUN_10012cf0
void __fastcall CrossProduct(float* self, void*, const float* other);                        // FUN_1002a358
float* __fastcall ScaleVector(const float* self, void*, float* out, float scale);            // FUN_1002a39f
void __fastcall CrossProductTo(const float* self, void*, float* out, const float* other);    // FUN_1002a3db
void __fastcall Identity(void* self, void*);                                                 // FUN_1002a406
void __cdecl NextNode(void** node, const void* end);                                         // NextNode
void __stdcall TypeStore(void* descriptor, int32_t value);                                   // FUN_1001b63b
int32_t __cdecl TypeSize();                                                                  // FUN_1001b662

}  // namespace rnative::helpers
