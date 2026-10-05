// D3DX natively (randy-vk.ini [Native] Device=on): the format conversions, D3DXCreateTexture and its image info.
//
// D3DX7 keeps one table at 0xB7CA0 (16 records of 0x28 bytes: eight dwords of _DDPIXELFORMAT, then the
// D3DX_SURFACEFORMAT code at +0x24). FUN_10063870 finds a record by the code and copies the DDPIXELFORMAT out,
// FUN_100638be finds one by the DDPIXELFORMAT and returns the code. render_t's wrappers over them throw the game's
// fun::DXError when the call fails (except DDERR_SURFACEBUSY), the same as the original's.
//
// FUN_10063df8 (D3DXCreateTexture) makes the surface: the device's IDirectDraw7, D3DX's size fix-up, the
// DDSURFACEDESC2 (pixel format from the table) and CreateSurface, then the image info (content size, flags, mip
// count) as the surface's private data, which the loaders read back. FUN_10063d5d builds that info. The size
// fix-up (FUN_10063a8f) and the loaders (FUN_100642ce / FUN_10063fca, D3DX's conversion core) are still the
// original's; port them with the rest of the core.
#include "native/d3dx.h"

#include "native/dxerror.h"

#include <d3d.h>

#include <cstring>

namespace rnative::d3dx {

namespace {

HMODULE g_orig;

void* At(uint32_t rva) { return reinterpret_cast<uint8_t*>(g_orig) + rva; }

constexpr uint32_t kFormatTable = 0xB7CA0, kFormatStride = 0x28, kFormatCodesEnd = 0x690, kPixelFormatBytes = 0x20;
constexpr int32_t kNotAPixelFormat = int32_t(0xC8770BC4), kNoPixelFormat = int32_t(0xC8770BB9);
constexpr int32_t kSurfaceBusy = int32_t(0x887601AE);

int32_t __stdcall MakeDDPixelFormat(int32_t format, void* pf)   // FUN_10063870
{
    if (!pf) return kNoPixelFormat;
    if (format) {
        const uint8_t* table = static_cast<const uint8_t*>(At(kFormatTable));
        for (uint32_t off = 0; off < kFormatCodesEnd; off += kFormatStride) {
            if (format == *reinterpret_cast<const int32_t*>(table + off + 0x24)) {
                std::memcpy(pf, table + off, kPixelFormatBytes);
                return 0;
            }
        }
    }
    return kNotAPixelFormat;
}

int32_t __stdcall MakeSurfaceFormat(const void* pf)   // FUN_100638be
{
    if (!pf) return 0;
    const uint8_t* table = static_cast<const uint8_t*>(At(kFormatTable));
    for (uint32_t off = 0; off < kFormatCodesEnd; off += kFormatStride) {
        if (std::memcmp(pf, table + off, kPixelFormatBytes) == 0)
            return *reinterpret_cast<const int32_t*>(table + off + 0x24);
    }
    return 0;
}

// render_t's wrappers (the exports DisplaySystem imports).
void __fastcall MakeDDPixelFormatWrapper(void*, void*, int32_t format, void* pf)   // FUN_10021541
{
    const int32_t hr = MakeDDPixelFormat(format, pf);
    if (hr && hr != kSurfaceBusy)
        dxerror::Throw(hr, "render_t::D3DXMakeDDPixelFormat: D3D-Call failed",
                       "..\\renderlib\\render_Interface_Device.cpp", 0x388);
}

int32_t __fastcall MakeSurfaceFormatWrapper(void*, void*, const void* pf)   // FUN_10023db3
{
    return MakeSurfaceFormat(pf);
}

// ---- D3DXCreateTexture (FUN_10063df8) and its helpers ----

// The input image info D3DX hangs on every texture surface as private data: its size, the caller's flags, the
// content's width / height (before D3DX pads the surface) and the mip-map count. The loaders read it back.
struct TextureInfo {
    uint32_t size, flags, width, height, mips;
};
static_assert(sizeof(TextureInfo) == 0x14);

constexpr uint32_t kCreateTexture = 0x63DF8, kBuildTextureInfo = 0x63D5D, kFixTextureSize = 0x63A8F;
constexpr uint32_t kD3DXInitialised = 0x1E2394;     // D3DXInitialize's flag: 0 means every D3DX call fails
constexpr uint32_t kImageInfoGuid = 0x99EE4;        // the texture surfaces' private image info
constexpr uint32_t kHalGuid = 0x99718;              // IID_IDirect3DHALDevice
constexpr uint32_t kTnlHalGuid = 0x996E8;           // IID_IDirect3DTnLHalDevice
constexpr uint32_t kDirectDrawIID = 0x99748;        // IID_IDirectDraw7
constexpr int32_t kNotInitialised = int32_t(0xC8770BC7), kNoSuchFormat = int32_t(0xC8770BC4),
                  kInvalidParams = int32_t(0xC8770BB9);

template <typename... A>
HRESULT Com(void* object, uint32_t offset, A... args)
{
    using Fn = HRESULT(__stdcall*)(void*, A...);
    return reinterpret_cast<Fn>((*static_cast<void***>(object))[offset / 4])(object, args...);
}

void ReleaseCom(void* object) { if (object) Com(object, 8); }

// FUN_10063d5d: the image info from the caller's flags / width / height, and the caller's two out slots zeroed.
// It refuses a null size (DDERR_INVALIDPARAMS) or a 0 / -1 dimension (D3DXERR_INVALIDDATA).
int32_t __cdecl BuildTextureInfo(TextureInfo* info, const uint32_t* flags, const uint32_t* width,
                                 const uint32_t* height, uint32_t* outA, uint32_t* outB)
{
    if (outB) *outB = 0;
    if (!outA) return kInvalidParams;
    *outA = 0;
    if (!width || !height) return kInvalidParams;
    if (*width == 0 || *width == 0xFFFFFFFF || *height == 0 || *height == 0xFFFFFFFF) return kNoSuchFormat;
    info->size = 0x14;
    info->flags = flags ? *flags : 0;
    info->width = *width;
    info->height = *height;
    uint32_t smallest = *width < *height ? *width : *height;
    info->mips = 0;
    if (smallest) {
        uint32_t bits = 0;
        do {
            ++bits;
            smallest >>= 1;
        } while (smallest);
        info->mips = bits;
    }
    return 0;
}

// FUN_1006381d: the IDirectDraw7 behind the device (IDirect3DDevice7::GetDirect3D, then QueryInterface). The
// original takes the device in EAX and the out slot in ECX, so callers inline it; only FUN_10063df8 did.
HRESULT DirectDrawFor(void* device, void** out)
{
    if (!device) {
        if (out) *out = nullptr;
        return HRESULT(kInvalidParams);
    }
    void* d3d = nullptr;
    HRESULT hr = Com(device, 0x1C, &d3d);
    if (hr < 0 || !d3d) {
        if (out) *out = nullptr;
        return HRESULT(kInvalidParams);
    }
    if (out) {
        *out = nullptr;
        hr = Com(d3d, 0x00, static_cast<const GUID*>(At(kDirectDrawIID)), out);
    } else {
        hr = HRESULT(kInvalidParams);
    }
    Com(d3d, 0x08);
    return hr;
}

// FUN_10063a8f: D3DX's texture size / format / caps adjustment (a software device drops the mip chain, a 3dfx or
// Kyro card caps the size). Still the original's: port it with the rest of D3DX's core.
int32_t __stdcall FixTextureSize(void* device, uint32_t* flags, uint32_t* width, uint32_t* height, int32_t* format)
{
    using Fn = int32_t(__stdcall*)(void*, void*, void*, void*, void*);
    return reinterpret_cast<Fn>(AddressFor(kFixTextureSize))(device, flags, width, height, format);
}

// FUN_10063df8: D3DXCreateTexture. Takes the device's IDirectDraw7, adjusts the size, builds the surface
// description (dwSize 0x7c, flags 0x101007, the pixel format from the format table; a software device gets
// system memory, a hardware one texture management, and unless the caller asks for no mip chain, complex +
// mip-mapped), creates the surface into the caller's slot, and tags it with the image info the loaders read.
int32_t __stdcall CreateTexture(void* device, uint32_t* flags, uint32_t* width, uint32_t* height, int32_t* format,
                                void* palette, void** surfaceOut, uint32_t* mipsOut)
{
    (void)palette;                                                  // the original ignores it too
    if (*reinterpret_cast<const uint32_t*>(At(kD3DXInitialised)) == 0) return kNotInitialised;
    if (!format) return kInvalidParams;
    void* dd = nullptr;
    HRESULT hr = DirectDrawFor(device, &dd);
    if (hr < 0) return hr;
    uint32_t localFlags = 0;
    if (!flags) flags = &localFlags;
    TextureInfo info{};
    hr = BuildTextureInfo(&info, flags, width, height, reinterpret_cast<uint32_t*>(surfaceOut), mipsOut);
    if (hr < 0) {
        ReleaseCom(dd);
        return hr;
    }
    hr = FixTextureSize(device, flags, width, height, format);
    if (hr < 0) {
        ReleaseCom(dd);
        return hr;
    }
    DDSURFACEDESC2 desc;
    std::memset(&desc, 0, sizeof(desc));
    desc.dwSize = 0x7C;
    desc.dwFlags = 0x101007;
    desc.dwWidth = *width;
    desc.dwHeight = *height;
    desc.ddsCaps.dwCaps = 0x1000;                                  // DDSCAPS_TEXTURE
    hr = MakeDDPixelFormat(*format, &desc.ddpfPixelFormat);
    if (hr < 0) {
        ReleaseCom(dd);
        return hr;
    }
    D3DDEVICEDESC7 caps;
    std::memset(&caps, 0, sizeof(caps));
    hr = Com(device, 0x0C, &caps);                                 // IDirect3DDevice7::GetCaps
    if (hr >= 0) {
        if (std::memcmp(&caps.deviceGUID, At(kHalGuid), 16) != 0 &&
            std::memcmp(&caps.deviceGUID, At(kTnlHalGuid), 16) != 0)
            desc.ddsCaps.dwCaps |= 0x800;                          // DDSCAPS_SYSTEMMEMORY: a software device
        else
            desc.ddsCaps.dwCaps2 |= 0x10;                          // DDSCAPS2_TEXTUREMANAGE
    }
    if ((*flags & 0x100) == 0) desc.ddsCaps.dwCaps |= 0x400008;    // DDSCAPS_MIPMAP | DDSCAPS_COMPLEX
    hr = Com(dd, 0x18, &desc, surfaceOut, nullptr);                // IDirectDraw7::CreateSurface
    if (hr < 0) {
        ReleaseCom(dd);
        return hr;
    }
    if (*flags & 0x100) info.mips = 0;
    void* surface = *surfaceOut;
    hr = Com(surface, 0xA0, static_cast<const GUID*>(At(kImageInfoGuid)), &info, uint32_t(0x14), uint32_t(0));
    if (hr < 0) {
        Com(surface, 0x08);
        *surfaceOut = nullptr;
    }
    ReleaseCom(dd);
    return hr;
}

}  // namespace

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
        {0x63870, FN(MakeDDPixelFormat), "D3DXMakeDDPixelFormat (FUN_10063870)"},
        {0x638BE, FN(MakeSurfaceFormat), "D3DXMakeSurfaceFormat (FUN_100638be)"},
        {0x63DF8, FN(CreateTexture), "D3DXCreateTexture (FUN_10063df8)"},
        {0x63D5D, FN(BuildTextureInfo), "D3DX texture info (FUN_10063d5d)"},
        {0x21541, FN(MakeDDPixelFormatWrapper), "render_t::D3DXMakeDDPixelFormat (FUN_10021541)"},
        {0x23DB3, FN(MakeSurfaceFormatWrapper), "render_t::D3DXMakeSurfaceFormat (FUN_10023db3)"},
    };
#undef FN
    int installed = 0;
    for (const Entry& e : entries) installed += Replace(orig, e.rva, e.target, e.what) ? 1 : 0;
    Log("D3DX: %d of %d functions native", installed, int(sizeof(entries) / sizeof(entries[0])));
}

}  // namespace rnative::d3dx
