// LBitmap_t natively (randy-vk.ini [Native] Device=on). The bitmap is randy31_orig's own memory, so the data and the
// palette come from / go to the CRT heap (vc10). The registry is a global array of the live bitmaps at 0xB9150, its
// count at 0xB92E0: every bitmap's constructor registers it, and a load looks one up whose vtable slot 3 (the class's
// extension string) matches. Scale only resizes when the target divides the original evenly (the original first asks
// fmod), steps by an integer factor and clamps to the Randy_t's texture size limits.
#include "native/lbitmap.h"

#include "native/vc10.h"

#include <cstdint>
#include <cstring>

namespace rnative::lbitmap {

namespace {

HMODULE g_orig;

template <typename T>
T& G(uint32_t rva) { return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(g_orig) + rva); }

constexpr uint32_t kVtable = 0x8A784;                 // LBitmap_t::vftable
constexpr uint32_t kRegistry = 0xB9150, kRegistryCount = 0xB92E0;
constexpr uint32_t kRandy = 0x17D2EC;                 // Randy_t::s_pcRandy

void SetVtable(Bitmap* self) { self->vtable = reinterpret_cast<uint8_t*>(g_orig) + kVtable; }

// The original asks fmod(original, target) == 0 before it scales; both are positive integers.
bool Divides(uint32_t original, uint32_t target) { return original % target == 0; }

constexpr uint32_t kBmpVtable = 0x8A4EC;
void SetBmpVtable(Bitmap* self) { self->vtable = reinterpret_cast<uint8_t*>(g_orig) + kBmpVtable; }

// fun::PositionIO_t's virtuals the BMP loader uses.
uint16_t ReadWord(Stream* s) { return reinterpret_cast<uint16_t(__fastcall*)(void*, void*)>(s->vtable[7])(s, nullptr); }
uint32_t ReadDword(Stream* s) { return reinterpret_cast<uint32_t(__fastcall*)(void*, void*)>(s->vtable[5])(s, nullptr); }
void Read(Stream* s, void* buf, int32_t size)
{
    reinterpret_cast<void(__fastcall*)(void*, void*, void*, int32_t)>(s->vtable[0])(s, nullptr, buf, size);
}
void Seek(Stream* s, int32_t offset, int32_t origin)
{
    reinterpret_cast<void(__fastcall*)(void*, void*, int32_t, int32_t)>(s->vtable[18])(s, nullptr, offset, origin);
}

}  // namespace

void SetModule(HMODULE orig) { g_orig = orig; }

void __fastcall Init(Bitmap* self, void*, int32_t)
{
    SetVtable(self);
    self->state = 0;
    self->flag = 0;
    self->palette = nullptr;
    self->data = nullptr;
}

void __fastcall Destroy(Bitmap* self, void*)
{
    SetVtable(self);
    vc10::Free(self->data);
    vc10::Free(self->palette);
}

int32_t __fastcall Size(const Bitmap* self, void*)
{
    uint32_t bytes = uint32_t(self->bitsPerPixel) >> 3;
    if (self->bitsPerPixel & 7) ++bytes;
    return int32_t(self->height * self->width * bytes);
}

void __fastcall Make24Bit(Bitmap* self, void*)
{
    if (self->bitsPerPixel == 0x18) return;
    uint8_t* out = static_cast<uint8_t*>(vc10::Allocate(size_t(self->width) * self->height * 3));
    uint8_t* dst = out;
    const uint8_t* src = self->data;
    for (uint32_t i = self->width * self->height; i != 0; --i) {
        const uint32_t c = self->palette[*src++];
        dst[0] = uint8_t(c);
        dst[1] = uint8_t(c >> 8);
        dst[2] = uint8_t(c >> 16);
        dst += 3;
    }
    vc10::Free(self->data);
    self->data = out;
    self->bitsPerPixel = 0x18;
}

void __cdecl Register(void* bitmap)
{
    const uint32_t i = G<uint32_t>(kRegistryCount);
    G<uint32_t>(kRegistryCount) = i + 1;
    G<void*>(kRegistry + i * 4) = bitmap;
}

void* __cdecl Find(const char* extension)
{
    for (uint32_t i = 0; i < G<uint32_t>(kRegistryCount); ++i) {
        void* bitmap = G<void*>(kRegistry + i * 4);
        auto name = reinterpret_cast<const char*(__fastcall*)(void*, void*)>((*reinterpret_cast<void***>(bitmap))[3]);
        if (_stricmp(name(bitmap, nullptr), extension) == 0) return bitmap;
    }
    return nullptr;
}

void __fastcall BaseCtor(Bitmap* self, void*)
{
    SetVtable(self);
    self->data = nullptr;
    self->palette = nullptr;
    self->width = 0;
    self->height = 0;
    self->bitsPerPixel = 0;
    self->state = 1;
    self->flag = 1;
    Register(self);
}

void __fastcall Scale(Bitmap* self, void*, uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0) return;
    const uint32_t originalWidth = self->width, originalHeight = self->height;
    if (width > originalWidth || height > originalHeight) return;
    uint8_t* randy = G<uint8_t*>(kRandy);
    auto field = [randy](uint32_t at) { return *reinterpret_cast<uint32_t*>(randy + at); };
    const uint32_t aspect = field(0x218);
    if (aspect != 0) {
        if (width / height > aspect) width = aspect * height;
        else if (height / width > aspect) height = aspect * width;
    }
    if (width < field(0x204)) width = field(0x204);
    if (height < field(0x208)) height = field(0x208);
    if (width > field(0x20C)) width = field(0x20C);
    if (height > field(0x210)) height = field(0x210);
    if (width == originalWidth && height == originalHeight) return;
    if (!Divides(originalWidth, width) || !Divides(originalHeight, height)) return;
    int32_t bytes;
    if (self->bitsPerPixel == 0x18) bytes = 3;
    else if (self->bitsPerPixel == 0x20) bytes = 4;
    else return;
    uint8_t* dst = static_cast<uint8_t*>(vc10::Allocate(size_t(width) * height * uint32_t(bytes)));
    const uint32_t stepX = originalWidth / width, stepY = originalHeight / height;
    const uint8_t* row = self->data;
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* src = row;
        uint8_t* out = dst + size_t(y) * width * uint32_t(bytes);
        for (uint32_t x = 0; x < width; ++x) {
            std::memcpy(out, src, size_t(bytes));
            src += size_t(stepX) * bytes;
            out += bytes;
        }
        row += size_t(originalWidth) * stepY * bytes;
    }
    vc10::Free(self->data);
    self->data = dst;
    self->width = width;
    self->height = height;
}

uint32_t __fastcall NotEqual(const float* self, void*, const float* other)
{
    return (self[0] == other[0] && self[1] == other[1] && self[2] == other[2]) ? 0u : 1u;
}

// The BMP stream loader. It reads the header through the stream's virtuals (a word for the signature, dwords for the
// rest), a paletted 8-bit bitmap's palette, then its rows bottom-up (each row padded to 4 bytes); a 24-bit one has no
// palette. `flags` == 1 stops after the header. A wrong signature, header size or compression sets state 2.
Bitmap* __fastcall BMPCtor(Bitmap* self, void*, Stream* stream, int32_t flags)
{
    Init(self, nullptr, 1);
    SetBmpVtable(self);
    if (self->state != 0) return self;
    const uint16_t signature = ReadWord(stream);
    ReadDword(stream);
    ReadWord(stream);
    ReadWord(stream);
    const uint32_t dataOffset = ReadDword(stream);
    if (signature != 0x4d42) {
        self->state = 2;
        return self;
    }
    const uint32_t headerSize = ReadDword(stream);
    const uint32_t width = ReadDword(stream);
    const uint32_t height = ReadDword(stream);
    ReadWord(stream);
    const uint16_t bitsPerPixel = ReadWord(stream);
    const uint32_t compression = ReadDword(stream);
    ReadDword(stream);
    ReadDword(stream);
    ReadDword(stream);
    uint32_t colorsUsed = ReadDword(stream);
    ReadDword(stream);
    if (headerSize == 0 || compression != 0) {
        self->state = 2;
        return self;
    }
    self->bitsPerPixel = bitsPerPixel;
    self->width = width;
    self->height = height;
    if (flags == 1) return self;
    const uint32_t count = width * height;
    if (colorsUsed == 0 && bitsPerPixel == 8) colorsUsed = 0x100;
    if (bitsPerPixel == 8) {
        uint32_t* palette = static_cast<uint32_t*>(vc10::Allocate(size_t(colorsUsed) * 4));
        self->palette = palette;
        Read(stream, palette, int32_t(colorsUsed) * 4);
        Seek(stream, int32_t(dataOffset), 0);
        uint8_t* pixels = static_cast<uint8_t*>(vc10::Allocate(count));
        self->data = pixels;
        const int32_t rowPad = (0 - int32_t(width)) & 3;
        for (uint32_t i = 0; i < height; ++i) {
            Read(stream, pixels + size_t(height - 1 - i) * width, int32_t(width));
            Seek(stream, rowPad, 1);
        }
    } else {
        Seek(stream, int32_t(dataOffset), 0);
        uint8_t* pixels = static_cast<uint8_t*>(vc10::Allocate(size_t(count) * 3));
        self->data = pixels;
        const int32_t rowPad = int32_t(width) & 3;
        for (uint32_t i = 0; i < height; ++i) {
            Read(stream, pixels + size_t(height - 1 - i) * width * 3, int32_t(width) * 3);
            Seek(stream, rowPad, 1);
        }
    }
    return self;
}

Bitmap* __fastcall Create(Bitmap* self, void*, Stream* stream, int32_t flags)
{
    (void)self;                                          // the prototype is only a factory
    Bitmap* object = static_cast<Bitmap*>(vc10::Allocate(0x124));
    if (object) object = BMPCtor(object, nullptr, stream, flags);
    return object;
}

Bitmap* __cdecl Load(Stream* stream, const char* name, int32_t flags)
{
    if (!name) {
        name = "bmp";
    } else if (const char* dot = std::strrchr(name, '.')) {
        name = dot + 1;
    }
    void* loader = Find(name);
    if (!loader) return nullptr;
    auto create = reinterpret_cast<Bitmap*(__fastcall*)(void*, void*, Stream*, int32_t)>((*reinterpret_cast<void***>(loader))[4]);
    return create(loader, nullptr, stream, flags);
}

void Install(HMODULE orig)
{
    if (GetMode("Device", Mode::Off) != Mode::On) return;
    g_orig = orig;
    struct Entry {
        uint32_t rva;
        void* target;
        const char* what;
    };
#define FN(f) reinterpret_cast<void*>(&f)
    const Entry entries[] = {
        {0x14C9B, FN(Init), "LBitmap_t init (FUN_10014c9b)"},
        {0x14CC0, FN(Destroy), "LBitmap_t destroy (FUN_10014cc0)"},
        {0x14CFF, FN(Size), "LBitmap_t data size (FUN_10014cff)"},
        {0x14DA8, FN(Make24Bit), "LBitmap_t make 24-bit (FUN_10014da8)"},
        {0x14EBC, FN(Register), "LBitmap_t register (FUN_10014ebc)"},
        {0x14ED7, FN(Find), "LBitmap_t find by extension (FUN_10014ed7)"},
        {0x14F53, FN(Scale), "LBitmap_t::Scale"},
        {0x151E5, FN(Load), "LBitmap_t::Load"},
        {0x15228, FN(BaseCtor), "LBitmap_t constructor (FUN_10015228)"},
        {0x1526B, FN(BMPCtor), "the BMP stream loader (FUN_1001526b)"},
        {0x1553D, FN(Create), "the loader's factory (FUN_1001553d)"},
        {0x155C5, FN(NotEqual), "Vector3 compare (FUN_100155c5)"},
    };
#undef FN
    int installed = 0;
    for (const Entry& e : entries) installed += Replace(orig, e.rva, e.target, e.what) ? 1 : 0;
    Log("lbitmap: %d of %d functions native", installed, int(sizeof(entries) / sizeof(entries[0])));
}

}  // namespace rnative::lbitmap
