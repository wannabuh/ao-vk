// PixelFormat_t natively (randy-vk.ini [Native] Device=on). The shifts and bit counts are worked out once from the
// masks (FUN_1001f694: a mask's lowest set bit, and how many bits it has); the conversions use them with x86 shift
// semantics (the shift count is taken modulo 32, and shr is logical), and ToFloats divides by (1 << bits) - 1.
#include "native/pixfmt.h"

#include <cstring>

namespace rnative::pixfmt {

namespace {

// A channel packed: the byte (0..255) >> (8 - bits), then << shift (both shifts count modulo 32, as x86 does).
uint32_t Shift(uint32_t value, int32_t bits, int32_t shift)
{
    return (value >> ((8 - bits) & 31)) << (shift & 31);
}

// An RGBI_t channel as a byte, as the original's x87 code: (int)(float * 255) with the CRT's _ftol (truncation).
uint32_t Channel(float v) { return uint32_t(int32_t(double(v) * 255.0)); }

}  // namespace

void __stdcall MaskToShiftBits(uint32_t mask, int32_t* shift, int32_t* bits)
{
    if (mask == 0) {
        *shift = 0;
        *bits = 0;
        return;
    }
    *shift = 0xff;
    *bits = 0;
    int32_t i = 0;
    for (; mask != 0; mask >>= 1, ++i) {
        if (mask & 1) {
            if (*shift == 0xff) *shift = i;
            ++*bits;
        }
    }
}

void __fastcall Init(Format* self, void*, int32_t bitsPerPixel, uint32_t rMask, int32_t gMask, int32_t bMask, int32_t aMask)
{
    self->gMask = uint32_t(gMask);
    self->bMask = uint32_t(bMask);
    self->aMask = uint32_t(aMask);
    self->bitsPerPixel = bitsPerPixel;
    self->rMask = rMask;
    MaskToShiftBits(rMask, &self->rShift, &self->rBits);
    MaskToShiftBits(self->gMask, &self->gShift, &self->gBits);
    MaskToShiftBits(self->bMask, &self->bShift, &self->bBits);
    MaskToShiftBits(self->aMask, &self->aShift, &self->aBits);
}

void* __fastcall Ctor(Format* self, void*, const uint32_t* ddpf)
{
    // _DDPIXELFORMAT: +0x0c bit count, +0x10 R mask, +0x14 G, +0x18 B, +0x1c A.
    Init(self, nullptr, int32_t(ddpf[3]), ddpf[4], int32_t(ddpf[5]), int32_t(ddpf[6]), int32_t(ddpf[7]));
    return self;
}

uint32_t __fastcall Pack(Format* self, void*, const float* rgb)
{
    return Shift(Channel(rgb[2]), self->bBits, self->bShift) + Shift(Channel(rgb[1]), self->gBits, self->gShift) +
           Shift(Channel(rgb[0]), self->rBits, self->rShift) + Shift(Channel(rgb[3]), self->aBits, self->aShift);
}

void __fastcall ToBytes(Format* self, void*, void* out, const float* rgb)
{
    const uint32_t packed = Pack(self, nullptr, rgb);
    const int32_t bytes = (self->bitsPerPixel + 7) >> 3;
    if (bytes == 1) *static_cast<uint8_t*>(out) = uint8_t(packed);
    else if (bytes == 2) *static_cast<uint16_t*>(out) = uint16_t(packed);
    else *static_cast<uint32_t*>(out) = packed;      // 3-byte formats write 4 bytes too, as the original's do
}

uint32_t __fastcall PackBytes(Format* self, void*, const uint8_t* rgb)
{
    return Shift(rgb[2], self->bBits, self->bShift) + Shift(rgb[1], self->gBits, self->gShift) +
           Shift(rgb[0], self->rBits, self->rShift);
}

uint32_t __fastcall PackPacked(Format* self, void*, uint32_t pixel)
{
    return Shift((pixel >> 8) & 0xff, self->gBits, self->gShift) + Shift((pixel >> 16) & 0xff, self->rBits, self->rShift) +
           Shift(pixel & 0xff, self->bBits, self->bShift);
}

void __fastcall ToFloats(Format* self, void*, float* out, const void* src, const void*)
{
    const int32_t bytes = (self->bitsPerPixel + 7) >> 3;
    const uint8_t* p = static_cast<const uint8_t*>(src);
    uint32_t raw;
    if (bytes == 1) {
        raw = p[0];
    } else if (bytes == 2) {
        uint16_t v;
        std::memcpy(&v, p, 2);
        raw = v;
    } else if (bytes == 3) {
        raw = uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16);
    } else {
        std::memcpy(&raw, p, 4);
    }
    auto channel = [raw](uint32_t mask, int32_t shift, int32_t bits) {
        return float((raw & mask) >> (shift & 31)) / float(int32_t((1u << (bits & 31)) - 1));
    };
    out[0] = channel(self->rMask, self->rShift, self->rBits);
    out[1] = channel(self->gMask, self->gShift, self->gBits);
    out[2] = channel(self->bMask, self->bShift, self->bBits);
    out[3] = channel(self->aMask, self->aShift, self->aBits);
}

void Install(HMODULE orig)
{
    if (GetMode("Device", Mode::Off) != Mode::On) return;
    struct Entry {
        uint32_t rva;
        void* target;
        const char* what;
    };
#define FN(f) reinterpret_cast<void*>(&f)
    const Entry entries[] = {
        {0x1F694, FN(MaskToShiftBits), "FUN_1001f694 mask -> shift, bits"},
        {0x1F79F, FN(Pack), "FUN_1001f79f RGBI_t packed"},
        {0x1F90D, FN(ToFloats), "PixelFormat_t::ConvertColor(void const*, RGBI_t const*)"},
        {0x1FA50, FN(ToBytes), "PixelFormat_t::ConvertColor(void*, RGBI_t const&)"},
        {0x1FA9D, FN(PackBytes), "PixelFormat_t::ConvertColor(uchar const*)"},
        {0x1FAE3, FN(PackPacked), "PixelFormat_t::ConvertColorPacked"},
        {0x1FC3E, FN(Init), "PixelFormat_t init from a pixel format (FUN_1001fc3e)"},
        {0x1FD01, FN(Ctor), "PixelFormat_t::PixelFormat_t"},
    };
#undef FN
    int installed = 0;
    for (const Entry& e : entries) installed += Replace(orig, e.rva, e.target, e.what) ? 1 : 0;
    Log("pixfmt: %d of %d functions native", installed, int(sizeof(entries) / sizeof(entries[0])));
}

}  // namespace rnative::pixfmt
