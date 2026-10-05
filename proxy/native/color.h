// Color_t natively (color.cpp): randy-vk.ini [Native] Scene=on. A 4-byte colour (bytes b, g, r, a - a D3DCOLOR) with
// its float / RGB_t constructors and Init (each channel * 255, truncated), a saturating +, a fixed-point Interpolate
// and a scale-and-round *. Used all over the renderer (sprites, the debugger, viewports, materials).
#pragma once

#include "native/native.h"

#include <cstdint>

namespace rnative::color {

// Color_t: +0 b, +1 g, +2 r, +3 a.
struct Color {
    uint8_t b, g, r, a;
};
static_assert(sizeof(Color) == 4, "Color_t");

void Install(HMODULE orig);

// The class's methods, with the original's calling conventions (this in ecx; the by-value return uses a hidden
// pointer, the second argument). `rgb` is an RGB_t: three floats r, g, b.
Color* __fastcall Ctor(Color* self, void*);
Color* __fastcall CtorUint(Color* self, void*, uint32_t value);
void __fastcall InitF(Color* self, void*, float r, float g, float b, float a);
void __fastcall InitRgb(Color* self, void*, const float* rgb, float alpha);
Color* __fastcall AssignUint(Color* self, void*, uint32_t value);
void __fastcall Interpolate(Color* self, void*, const Color* a, const Color* b, float t);
Color* __fastcall CtorRgb(Color* self, void*, const float* rgb);
Color* __fastcall Add(Color* self, void*, Color* result, const Color* other);
Color* __fastcall Scale(Color* self, void*, Color* result, float scale);

}  // namespace rnative::color
