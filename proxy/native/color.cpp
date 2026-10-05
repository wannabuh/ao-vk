// Color_t natively (randy-vk.ini [Native] Scene=on). A float channel becomes a byte as (int)(double(channel) * 255):
// the original multiplies by the double 255.0 in x87 and converts with the CRT's _ftol (truncation toward zero), and
// a float times 255 is exact in double, so the result is the same to the bit. Interpolate's weight is (int)(t * 256)
// and the channel is (a * (256 - w) + b * w) >> 8; Scale is clamp((int)(channel * scale + 0.5), 0, 255).
#include "native/color.h"

#include <cstring>

namespace rnative::color {

namespace {

int Truncate(double v) { return static_cast<int>(v); }   // _ftol: truncation toward zero (its out-of-range NaN case is 0x80000000, as cvttsd2si too)
uint8_t Channel(float v) { return uint8_t(Truncate(double(v) * 255.0)); }

}  // namespace

Color* __fastcall Ctor(Color* self, void*) { return self; }

Color* __fastcall CtorUint(Color* self, void*, uint32_t value)
{
    std::memcpy(self, &value, sizeof(value));
    return self;
}

void __fastcall InitF(Color* self, void*, float r, float g, float b, float a)
{
    self->r = Channel(r);
    self->g = Channel(g);
    self->b = Channel(b);
    self->a = Channel(a);
}

void __fastcall InitRgb(Color* self, void*, const float* rgb, float alpha) { InitF(self, nullptr, rgb[0], rgb[1], rgb[2], alpha); }

Color* __fastcall AssignUint(Color* self, void*, uint32_t value)
{
    std::memcpy(self, &value, sizeof(value));
    return self;
}

void __fastcall Interpolate(Color* self, void*, const Color* a, const Color* b, float t)
{
    const int w = Truncate(double(t) * 256.0), inv = 0x100 - w;
    auto mix = [inv, w](uint8_t av, uint8_t bv) { return uint8_t((int(av) * inv + int(bv) * w) >> 8); };
    self->r = mix(a->r, b->r);
    self->g = mix(a->g, b->g);
    self->b = mix(a->b, b->b);
    self->a = mix(a->a, b->a);
}

Color* __fastcall CtorRgb(Color* self, void*, const float* rgb)
{
    InitF(self, nullptr, rgb[0], rgb[1], rgb[2], 1.0f);
    return self;
}

Color* __fastcall Add(Color* self, void*, Color* result, const Color* other)
{
    auto sat = [](int v) { return uint8_t(v > 255 ? 255 : v); };
    result->r = sat(int(self->r) + int(other->r));
    result->g = sat(int(self->g) + int(other->g));
    result->b = sat(int(self->b) + int(other->b));
    result->a = sat(int(self->a) + int(other->a));
    return result;
}

Color* __fastcall Scale(Color* self, void*, Color* result, float scale)
{
    auto chan = [scale](uint8_t v) {
        int i = Truncate(double(v) * double(scale) + 0.5);
        return uint8_t(i < 0 ? 0 : i > 255 ? 255 : i);
    };
    result->r = chan(self->r);
    result->g = chan(self->g);
    result->b = chan(self->b);
    result->a = chan(self->a);
    return result;
}

void Install(HMODULE orig)
{
    if (GetMode("Scene", Mode::Off) != Mode::On) return;
    struct Entry {
        uint32_t rva;
        void* target;
        const char* what;
    };
#define FN(f) reinterpret_cast<void*>(&f)
    const Entry entries[] = {
        {0x1998C, FN(Ctor), "Color_t::Color_t"},
        {0x1998F, FN(CtorUint), "Color_t::Color_t(uint)"},
        {0x199BB, FN(InitF), "Color_t::Init(float,float,float,float)"},
        {0x199FF, FN(InitRgb), "Color_t::Init(RGB_t,float)"},
        {0x19A47, FN(AssignUint), "Color_t::operator=(uint)"},
        {0x19A7A, FN(Interpolate), "Color_t::Interpolate"},
        {0x19B80, FN(CtorRgb), "Color_t::Color_t(RGB_t)"},
        {0x19BB1, FN(Add), "Color_t::operator+"},
        {0x19C93, FN(Scale), "Color_t::operator*"},
    };
#undef FN
    int installed = 0;
    for (const Entry& e : entries) installed += Replace(orig, e.rva, e.target, e.what) ? 1 : 0;
    Log("color: %d of %d functions native", installed, int(sizeof(entries) / sizeof(entries[0])));
}

}  // namespace rnative::color
