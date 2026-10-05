// TextureStreamCreator's bitmap path (randy-vk.ini [Native] Device=on): CreateTexture(LBitmap_t*, name) scales the
// bitmap by the creator's divisor, picks a D3DX surface format from the quality / compression settings, makes the
// surface and uploads the pixels (render_t::D3DXCreateTexture / D3DXLoadTextureFromMemory). EnableCompression is the
// static switch it uses. The ctors, Archive / Instantiate, the PositionIO path and the format helpers are separate.
#include "native/texture_stream.h"

#include "native/lbitmap.h"
#include "native/orig_api.gen.h"

namespace rnative::texturestream {

namespace {

HMODULE g_orig;

template <typename T>
T& G(uint32_t rva) { return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(g_orig) + rva); }
template <typename F>
F Internal(uint32_t rva) { return reinterpret_cast<F>(reinterpret_cast<uint8_t*>(g_orig) + rva); }

// TextureStreamCreator's statics and the renderer's compression / format-support flags.
constexpr uint32_t kQuality = 0xB6608, kCompression = 0xB660C, kSupport = 0xB6610;   // s_nTextureQuality, s_bCompression, the format cache
constexpr uint32_t kDxt1 = 0x17D330, kSquared = 0x17D31D, kRender = 0x16BED0;

// VS2010 std::string (library): assign(const char*), find(const char*, pos) and _Tidy.
constexpr uint32_t kAssign = 0x1468C, kFind = 0x19826, kTidy = 0x11E82, kFormat = 0x1994C;
// FUN_100416f6 (compression supported) and FUN_10041e93 (the format-support flag).
constexpr uint32_t kCompressionSupported = 0x416F6, kFormatSupport = 0x41E93;

void Tidy(vc10::String& s) { Internal<void(__fastcall*)(void*, void*, uint8_t, uint32_t)>(kTidy)(&s, nullptr, 1, 0); }

}  // namespace

void SetModule(HMODULE orig) { g_orig = orig; }

void __cdecl EnableCompression(bool on)
{
    const uint32_t supported = Internal<uint32_t(__cdecl*)()>(kCompressionSupported)();
    G<uint8_t>(kCompression) = uint8_t((supported != 1) && on);
}

void* __fastcall CreateTexture(Creator* self, void*, void* bitmapRaw, const char* name)
{
    lbitmap::Bitmap* bitmap = static_cast<lbitmap::Bitmap*>(bitmapRaw);
    if (G<int32_t>(kSupport) == -1) G<int32_t>(kSupport) = Internal<uint8_t(__cdecl*)()>(kFormatSupport)() != 0;
    const int32_t divisor = self->divisor;
    uint32_t width = bitmap->width, height = bitmap->height;
    if (divisor != 1) {
        height = uint32_t(int32_t(height) / divisor);
        width = uint32_t(int32_t(width) / divisor);
    }
    lbitmap::Scale(bitmap, nullptr, width, height);
    if (bitmap->state != 0) {                            // the loader's error: a message, and no surface
        vc10::String message;
        message.init();
        const int32_t state = bitmap->state;
        const char* format = state == 1   ? "No support for extension found in %s!"
                             : state == 2 ? "Bad texture file %s!"
                             : state == 3 ? "Memory error opening texture file %s!"
                                          : "Unknown error opening texture file %s!";
        Internal<void(__cdecl*)(void*, const char*, ...)>(kFormat)(&message, format);
        Tidy(message);
        return nullptr;
    }
    if (bitmap->bitsPerPixel != 0x18 && bitmap->bitsPerPixel != 0x20)
        reinterpret_cast<void(__fastcall*)(void*, void*)>((*reinterpret_cast<void***>(bitmap))[2])(bitmap, nullptr);
    const bool wasCompressed = G<uint8_t>(kCompression) != 0;
    vc10::String fileName;
    fileName.init();
    Internal<void(__fastcall*)(void*, void*, const char*)>(kAssign)(&fileName, nullptr, name);
    const bool isPng = Internal<uint8_t(__fastcall*)(void*, void*, const char*, const char*)>(kFind)(
                           &fileName, nullptr, ".png", nullptr) != 0;
    Tidy(fileName);
    if (isPng) EnableCompression(false);
    const int32_t quality = G<int32_t>(kQuality);
    const bool compressed = G<uint8_t>(kCompression) == 1;
    if (bitmap->bitsPerPixel == 0x20) {
        self->format = (compressed && G<uint8_t>(kDxt1)) ? 0x13 : (quality == 0 ? 10 : 2);
    } else if (bitmap->bitsPerPixel == 0x18) {
        const int32_t uncompressed = compressed ? 0 : 1;
        self->format = quality < 2 ? ((uncompressed - 1) & 0xe) + 4 : ((uncompressed - 1) & 0xf) + 3;
    }
    uint32_t levels = self->flag30 ? 0 : 0x100;
    uint32_t surfaceWidth = bitmap->width, surfaceHeight = bitmap->height;
    int32_t format = self->format;
    uint32_t tail = 0;
    if (surfaceWidth != surfaceHeight && G<uint8_t>(kSquared)) levels = 0x100;
    vc10::String createError, convertError;
    createError.init();
    convertError.init();
    Internal<void(__cdecl*)(void*, const char*, ...)>(kFormat)(&createError, "Could not create texture from file %s!");
    Internal<void(__cdecl*)(void*, const char*, ...)>(kFormat)(&convertError, "Could not convert texture from file %s!");
    if (G<int32_t>(kSupport) == 1 && format == 0x12) {
        self->flag38 = 1;
        self->format = 0x13;
        format = 0x13;
    }
    void* surface = orig::surface_t_surface_t(vc10::Allocate(0x88), nullptr, const_cast<char*>("Texture"));
    void* render = G<void*>(kRender);
    orig::render_t_D3DXCreateTexture(render, &levels, &surfaceWidth, &surfaceHeight, &format, nullptr, surface, &tail);
    if (self->format == 3 && format == 2) {
        self->flag38 = 1;
        self->format = 2;
    }
    orig::render_t_D3DXLoadTextureFromMemory(render, surface, 0xFFFFFFFF, bitmap->data, nullptr,
                                             (bitmap->bitsPerPixel == 0x20) + 1, 0xFFFFFFFF, nullptr, self->filter);
    EnableCompression(wasCompressed);
    Tidy(convertError);
    Tidy(createError);
    return surface;
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
        {0x191B8, FN(EnableCompression), "TextureStreamCreator::EnableCompression"},
        {0x193E4, FN(CreateTexture), "TextureStreamCreator::CreateTexture(LBitmap_t*, name)"},
    };
#undef FN
    int installed = 0;
    for (const Entry& e : entries) installed += Replace(orig, e.rva, e.target, e.what) ? 1 : 0;
    Log("texture_stream: %d of %d functions native", installed, int(sizeof(entries) / sizeof(entries[0])));
}

}  // namespace rnative::texturestream
