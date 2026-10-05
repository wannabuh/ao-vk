// PixelFormat_t natively (pixfmt.cpp): randy-vk.ini [Native] Device=on. A DirectDraw pixel format - per channel its
// shift, bit count and mask, and the total bits per pixel - with the conversions between a packed pixel, an RGBI_t
// (four floats) and an RGB byte triple. Used by DisplaySystem and by Randy's surfaces / textures.
#pragma once

#include "native/native.h"

#include <cstdint>

namespace rnative::pixfmt {

// PixelFormat_t (0x34 bytes; the masks are the ORIGINAL's static names).
struct Format {
    int32_t aShift, rShift, gShift, bShift;                 // +0x00
    int32_t aBits, rBits, gBits, bBits;                     // +0x10
    uint32_t aMask, rMask, gMask, bMask;                    // +0x20
    int32_t bitsPerPixel;                                   // +0x30
};
static_assert(sizeof(Format) == 0x34, "PixelFormat_t");

void Install(HMODULE orig);

// FUN_1001f694 (__stdcall): a mask's lowest set bit and its number of set bits (0 / 0 when the mask is 0).
void __stdcall MaskToShiftBits(uint32_t mask, int32_t* shift, int32_t* bits);
// FUN_1001fc3e: the class's init from a bit count and the masks (each mask to its shift / bits).
void __fastcall Init(Format* self, void*, int32_t bitsPerPixel, uint32_t rMask, int32_t gMask, int32_t bMask, int32_t aMask);
// PixelFormat_t::PixelFormat_t(_DDPIXELFORMAT const*); ddpf is the raw struct (its bit count at +0x0c, masks after).
void* __fastcall Ctor(Format* self, void*, const uint32_t* ddpf);
// FUN_1001f79f: an RGBI_t (r g b a floats) packed.
uint32_t __fastcall Pack(Format* self, void*, const float* rgb);
// PixelFormat_t::ConvertColor(void*, RGBI_t const&): Pack, then written in 1 / 2 / 4 bytes (a 3-byte format writes 4).
void __fastcall ToBytes(Format* self, void*, void* out, const float* rgb);
// PixelFormat_t::ConvertColor(uchar const*): an r g b byte triple packed.
uint32_t __fastcall PackBytes(Format* self, void*, const uint8_t* rgb);
// PixelFormat_t::ConvertColorPacked(uint): a 0x00RRGGBB packed.
uint32_t __fastcall PackPacked(Format* self, void*, uint32_t pixel);
// PixelFormat_t::ConvertColor(void const*, RGBI_t const*): a pixel to r g b a floats (each / ((1 << bits) - 1)).
void __fastcall ToFloats(Format* self, void*, float* out, const void* src, const void* unused);

}  // namespace rnative::pixfmt
