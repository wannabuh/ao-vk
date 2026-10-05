// The D3DX format conversions natively (randy-vk.ini [Native] Device=on): D3DX_SURFACEFORMAT <-> _DDPIXELFORMAT.
//
// D3DX7 keeps one table at 0xB7CA0 (16 records of 0x28 bytes: eight dwords of _DDPIXELFORMAT, then the
// D3DX_SURFACEFORMAT code at +0x24). FUN_10063870 finds a record by the code and copies the DDPIXELFORMAT out,
// FUN_100638be finds one by the DDPIXELFORMAT and returns the code. render_t's wrappers over them throw the game's
// fun::DXError when the call fails (except DDERR_SURFACEBUSY), the same as the original's.
#include "native/d3dx.h"

#include "native/dxerror.h"

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
        {0x21541, FN(MakeDDPixelFormatWrapper), "render_t::D3DXMakeDDPixelFormat (FUN_10021541)"},
        {0x23DB3, FN(MakeSurfaceFormatWrapper), "render_t::D3DXMakeSurfaceFormat (FUN_10023db3)"},
    };
#undef FN
    int installed = 0;
    for (const Entry& e : entries) installed += Replace(orig, e.rva, e.target, e.what) ? 1 : 0;
    Log("D3DX formats: %d of %d functions native", installed, int(sizeof(entries) / sizeof(entries[0])));
}

}  // namespace rnative::d3dx
