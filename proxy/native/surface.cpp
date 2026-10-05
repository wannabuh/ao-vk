// surface_t and render_t's DirectDraw side natively (part of [Native] Device=on).
//
// surface_t (0x88 bytes): +0x00 IDirectDrawSurface7*, +0x04 Randy's own count of users (the surface is AddRef'd once
// for all of them, released and the surface_t freed with the last), +0x08 a DDSURFACEDESC2 filled on first ask
// (+0x84: still to fill). render_t: +0x00 IDirect3DDevice7, +0x04 IDirect3D7, +0x08 IDirectDraw7, +0x10 its DDCAPS,
// +0x278 the monitor's rectangle (multi-monitor offset for windowed presents).
// The D3DX texture functions (D3DXCreateTexture, D3DXLoadTextureFromMemory, the format conversions) stay in the original
// for now: they are D3DX7 itself, statically linked.
#include "native/surface.h"
#include "native/vc10.h"

#include <cstring>

namespace rnative::surface {

namespace {

void* const* g_render;

template <typename T>
T& Field(void* object, uint32_t offset) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(object) + offset); }

template <typename... A>
HRESULT Com(void* object, uint32_t offset, A... args)
{
    using Fn = HRESULT(__stdcall*)(void*, A...);
    return reinterpret_cast<Fn>((*static_cast<void***>(object))[offset / 4])(object, args...);
}

constexpr HRESULT kSurfaceBusy = HRESULT(0x887601AE), kSurfaceLost = HRESULT(0x887601C2),
                  kStillDrawing = HRESULT(0x8876021C);

void Failed(const char* what, HRESULT hr)
{
    static int logged;
    if (logged < 20) {
        ++logged;
        Log("%s: DirectDraw call failed (%08lx)", what, (unsigned long)hr);
    }
}

void* DirectDraw(void* render) { return Field<void*>(render, 8); }

// ---- surface_t ----

void* __fastcall SurfaceConstruct(uint8_t* s, void*, void*, const char*)
{
    Field<void*>(s, 0) = nullptr;
    Field<uint32_t>(s, 4) = 0;
    s[0x84] = 1;
    return s;
}

void* __fastcall GetSurfaceDesc(uint8_t* s)
{
    if (s[0x84]) {
        s[0x84] = 0;
        std::memset(s + 8, 0, 0x7C);
        Field<uint32_t>(s, 8) = 0x7C;
        if (HRESULT hr = Com(Field<void*>(s, 0), 0x58, static_cast<void*>(s + 8)))
            if (hr != kSurfaceBusy) Failed("render_t::GetSurfaceDesc", hr);
    }
    return s + 8;
}

uint32_t __fastcall AddRefDXSurface(uint8_t* s)
{
    uint32_t& users = Field<uint32_t>(s, 4);
    if (users == 0)
        if (void* dds = Field<void*>(s, 0)) Com(dds, 4);   // AddRef
    return ++users;
}

void** __fastcall GetSurfacePointer(uint8_t* s)
{
    ++Field<uint32_t>(s, 4);
    return &Field<void*>(s, 0);
}

uint32_t __fastcall ReleaseDXSurface(uint8_t* s)
{
    uint32_t& users = Field<uint32_t>(s, 4);
    if (users == 0) return 0;
    if (users == 1) {
        if (void* dds = Field<void*>(s, 0)) Com(dds, 8);   // Release
        Field<void*>(s, 0) = nullptr;
    }
    if (--users != 0) return users;
    vc10::Free(s);
    return 0;
}

// ---- render_t ----

HRESULT __fastcall CreateSurface(void* render, void*, void* desc, void** out, void* outer)
{
    if (!DirectDraw(render)) return 0;
    return Com(DirectDraw(render), 0x18, desc, out, outer);
}

void __fastcall LockTexture(void*, void*, void* dds, RECT* rect, void* desc, uint32_t flags, void* event)
{
    HRESULT hr = Com(dds, 0x64, rect, desc, DWORD(flags), event);
    if (hr && hr != kSurfaceBusy) Failed("render_t::LockTexture", hr);
}

void __fastcall UnlockTexture(void*, void*, void* dds, RECT* rect)
{
    HRESULT hr = Com(dds, 0x80, rect);
    if (hr && hr != kSurfaceBusy) Failed("render_t::UnlockTexture", hr);
}

// Always waits (DDBLT_WAIT, never DDBLT_DONOTWAIT); a failed asynchronous blit is retried synchronously.
void __fastcall Blt(void* render, void*, void* dst, RECT* dstRect, void* src, RECT* srcRect, uint32_t flags, void* fx)
{
    HRESULT hr = Com(dst, 0x14, dstRect, src, srcRect, DWORD((flags & ~0x8000000u) | 0x1000000u), fx);
    if (hr && hr != kSurfaceBusy && hr != kSurfaceLost) {
        if (!(flags & 0x200)) {                     // (the original throws)
            Failed("render_t::Blt", hr);
            return;
        }
        Blt(render, nullptr, dst, dstRect, src, srcRect, (flags & ~0x200u) | 0x1000000u, fx);
    }
}

void __fastcall GetPixelFormat(void*, void*, void* dds, void* format)
{
    HRESULT hr = Com(dds, 0x54, format);
    if (hr && hr != kSurfaceBusy) Failed("render_t::GetPixelFormat", hr);
}

void __fastcall RestoreAllSurfaces(void* render)
{
    if (DirectDraw(render)) Com(DirectDraw(render), 0x64);
}

void __fastcall CreatePalette(void* render, void*, uint32_t flags, void* entries, void** out, void* outer)
{
    if (!DirectDraw(render)) return;
    HRESULT hr = Com(DirectDraw(render), 0x14, DWORD(flags), entries, out, outer);
    if (hr && hr != kSurfaceBusy) Failed("render_t::CreatePalette", hr);
}

void __fastcall CreateClipper(void* render, void*, uint32_t flags, void** out, void* outer)
{
    if (!DirectDraw(render)) return;
    HRESULT hr = Com(DirectDraw(render), 0x10, DWORD(flags), out, outer);
    if (hr && hr != kSurfaceBusy) Failed("render_t::CreateClipper", hr);
}

void __fastcall GetInfo(void* render, void*, uint32_t type, void* data, uint32_t size)
{
    void* device = Field<void*>(render, 0);
    if (!device) return;
    HRESULT hr = Com(device, 0xC0, DWORD(type), data, DWORD(size));   // IDirect3DDevice7::GetInfo
    if (hr != HRESULT(0x80004005) && hr != kSurfaceBusy && (hr < 0 || hr > 1)) Failed("render_t::GetInfo", hr);
}

void __fastcall SetMonitorRect(void* render, void*, const RECT* rect) { CopyRect(&Field<RECT>(render, 0x278), rect); }

void __fastcall RestoreDisplayMode(void* render)
{
    if (!DirectDraw(render)) return;
    HRESULT hr = Com(DirectDraw(render), 0x4C);
    if (hr && hr != kSurfaceBusy) Failed("render_t::RestoreDisplayMode", hr);
}

// FUN_1002428e: IDirectDrawSurface7::Flip, again while the card is still drawing.
void __fastcall FlipChain(void*, void*, void* dds, void* target, uint32_t flags)
{
    HRESULT hr;
    do hr = Com(dds, 0x2C, target, DWORD(flags));
    while (hr == kStillDrawing);
    if (hr && hr != kSurfaceBusy && hr != kSurfaceLost) Failed("render_t::Flip", hr);
}

// FUN_10020c0d: a screen rectangle relative to the monitor Randy presents on.
void __fastcall ToMonitor(void* render, void*, RECT* rect)
{
    const RECT& monitor = Field<RECT>(render, 0x278);
    if (IsRectEmpty(&monitor)) return;
    rect->left -= monitor.left;
    rect->top -= monitor.top;
    rect->right -= monitor.left;
    rect->bottom -= monitor.top;
}

void __fastcall WaitForVerticalBlank(void* render, void*, uint32_t flags, void* event)   // FUN_1002376a
{
    if (!DirectDraw(render)) return;
    HRESULT hr = Com(DirectDraw(render), 0x58, DWORD(flags), event);
    if (hr && hr != kSurfaceBusy) Failed("render_t::WaitForVerticalBlank", hr);
}

void __fastcall EvictManagedTextures(void* render)   // FUN_10023e14
{
    void* d3d = Field<void*>(render, 4);
    if (!d3d) return;
    HRESULT hr = Com(d3d, 0x1C);
    if (hr && hr != kSurfaceBusy) Failed("render_t::EvictManagedTextures", hr);
}

void __fastcall GetCaps(void* render, void*, void* caps, void* helCaps)   // FUN_10021073: DirectDraw's, kept at +0x10
{
    void* dd = DirectDraw(render);
    if (!dd) return;
    HRESULT hr = Com(dd, 0x2C, caps, helCaps);
    if (hr && hr != kSurfaceBusy) Failed("render_t::GetCaps", hr);
    if (caps) std::memcpy(static_cast<uint8_t*>(render) + 0x10, caps, 0x5F * 4);
}

// FUN_10026277: a format code to a byte / bit count. FUN_100278b4: the format's colour bits. FUN_100263dc: nothing.
uint32_t __cdecl FormatBits(int format)
{
    int rest = 0;
    if (format < 0x13) {
        if (format == 0x12) return 1;
        if (format == 2) return 8;
        if (format == 8) return 1;
        rest = format - 10;
        if (rest == 0) return 4;
    } else {
        if (format == 0x13) return 4;
        rest = format - 0x14;
        if (rest == 0) return 8;
    }
    return rest == 2 ? 8 : 0;
}
uint32_t __cdecl ReturnTwo() { return 2; }                                    // FUN_100278b4
void __fastcall Ret8(void*, void*, uint32_t, uint32_t) {}                     // FUN_100263dc

}  // namespace

void Install(HMODULE orig)
{
    g_render = reinterpret_cast<void* const*>(GetProcAddress(orig, "?m_pcInstance@render_t@@0PAV1@A"));
    struct Entry {
        uint32_t rva;
        void* target;
        const char* what;
    };
#define FN(f) reinterpret_cast<void*>(&f)
    const Entry entries[] = {
        {0x261B9, FN(SurfaceConstruct), "surface_t::surface_t"},
        {0x261CD, FN(GetSurfaceDesc), "surface_t::GetSurfaceDesc"},
        {0x2620A, FN(AddRefDXSurface), "surface_t::AddRefDXSurface"},
        {0x26227, FN(GetSurfacePointer), "surface_t::GetSurfacePointer"},
        {0x2622D, FN(ReleaseDXSurface), "surface_t::ReleaseDXSurface"},
        {0x23DCC, FN(CreateSurface), "render_t::CreateSurface"},
        {0x23F58, FN(LockTexture), "render_t::LockTexture"},
        {0x24003, FN(UnlockTexture), "render_t::UnlockTexture"},
        {0x2434E, FN(Blt), "render_t::Blt"},
        {0x2414A, FN(GetPixelFormat), "render_t::GetPixelFormat"},
        {0x23DBC, FN(RestoreAllSurfaces), "render_t::RestoreAllSurfaces"},
        {0x213E1, FN(CreatePalette), "render_t::CreatePalette"},
        {0x23617, FN(CreateClipper), "render_t::CreateClipper"},
        {0x20CF7, FN(GetInfo), "render_t::GetInfo"},
        {0x20BF6, FN(SetMonitorRect), "render_t::SetMonitorRect"},
        {0x2133F, FN(RestoreDisplayMode), "render_t::RestoreDisplayMode"},
        {0x2428E, FN(FlipChain), "render_t flip (FUN_1002428e)"},
        {0x20C0D, FN(ToMonitor), "render_t monitor offset (FUN_10020c0d)"},
        {0x2376A, FN(WaitForVerticalBlank), "render_t::WaitForVerticalBlank (FUN_1002376a)"},
        {0x23E14, FN(EvictManagedTextures), "render_t::EvictManagedTextures (FUN_10023e14)"},
        {0x21073, FN(GetCaps), "render_t::GetCaps (FUN_10021073)"},
        {0x26277, FN(FormatBits), "a format code's bits (FUN_10026277)"},
        {0x263DC, FN(Ret8), "a surface no-op (FUN_100263dc)"},
        {0x278B4, FN(ReturnTwo), "a surface constant 2 (FUN_100278b4)"},
    };
#undef FN
    int installed = 0;
    for (const Entry& e : entries) installed += Replace(orig, e.rva, e.target, e.what) ? 1 : 0;
    Log("surfaces: %d of %d functions native", installed, int(sizeof(entries) / sizeof(entries[0])));
}

}  // namespace rnative::surface
