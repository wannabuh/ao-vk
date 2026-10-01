// rvk backend: shared state, pixel formats, caps, IDirectDraw7, IDirect3D7, clipper, palette, import hooks.
#include "rvk_backend.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace rvkproxy {

RvkState g_rvk;

void RvkLog(const char* fmt, ...)
{
    char path[MAX_PATH] = "randy-vk.log";
    GetEnvironmentVariableA("RANDYVK_LOG", path, sizeof(path));
    if (FILE* f = std::fopen(path, "a")) {
        std::fputs("randy-vk rvk: ", f);
        va_list args;
        va_start(args, fmt);
        std::vfprintf(f, fmt, args);
        va_end(args);
        std::fputc('\n', f);
        std::fclose(f);
    }
}

// ---------------------------------------------------------------------------------------------------
// Shared state

bool RvkState::EnsureDevice(uint32_t width, uint32_t height)
{
    if (device) {
        if (device->Width() == width && device->Height() == height)
            return true;
        if (device->InFrame())
            device->EndFrame();
        return device->Resize(width, height);
    }
    device = new rvk::Device;
    std::string error;
    if (!device->Init(window, width, height, &error)) {
        RvkLog("rvk device creation failed: %s", error.c_str());
        delete device;
        device = nullptr;
        return false;
    }
    gpuName = device->Info().gpu;
    RvkLog("rvk device %ux%u on %s (window %p)", width, height, gpuName.c_str(), (void*)window);
    return true;
}

void RvkState::SetWindow(HWND hwnd)
{
    if (!hwnd)
        return;
    window = hwnd;
    if (device && device->Window() != hwnd) {
        if (device->InFrame())
            device->EndFrame();
        if (device->SetWindow(hwnd))
            RvkLog("presenting to window %p", (void*)hwnd);
    }
}

void RvkState::Frame()
{
    if (device && !device->InFrame())
        device->BeginFrame();
}

void RvkState::Present()
{
    if (!device)
        return;
    Frame();                  // a present without any rendering still shows a frame
    device->EndFrame();
}

// ---------------------------------------------------------------------------------------------------
// Pixel formats

bool FormatFromPixelFormat(const DDPIXELFORMAT& pf, rvk::Format* format, bool* convert24, bool* palettized)
{
    *convert24 = *palettized = false;
    if (pf.dwFlags & DDPF_FOURCC) {
        switch (pf.dwFourCC) {
        case MAKEFOURCC('D', 'X', 'T', '1'): *format = rvk::Format::DXT1; return true;
        case MAKEFOURCC('D', 'X', 'T', '2'): *format = rvk::Format::DXT2; return true;
        case MAKEFOURCC('D', 'X', 'T', '3'): *format = rvk::Format::DXT3; return true;
        case MAKEFOURCC('D', 'X', 'T', '4'): *format = rvk::Format::DXT4; return true;
        case MAKEFOURCC('D', 'X', 'T', '5'): *format = rvk::Format::DXT5; return true;
        default: return false;
        }
    }
    if (pf.dwFlags & DDPF_PALETTEINDEXED8) {
        *format = rvk::Format::A8R8G8B8;
        *palettized = true;
        return true;
    }
    bool alpha = (pf.dwFlags & DDPF_ALPHAPIXELS) != 0;
    if (pf.dwFlags & DDPF_LUMINANCE) {
        if (pf.dwRGBBitCount == 8) { *format = rvk::Format::L8; return true; }
        if (pf.dwRGBBitCount == 16 && alpha) { *format = rvk::Format::A8L8; return true; }
        return false;
    }
    if ((pf.dwFlags & DDPF_ALPHA) && pf.dwAlphaBitDepth == 8) { *format = rvk::Format::A8; return true; }
    if (!(pf.dwFlags & DDPF_RGB))
        return false;
    switch (pf.dwRGBBitCount) {
    case 32:
        *format = alpha && pf.dwRGBAlphaBitMask ? rvk::Format::A8R8G8B8 : rvk::Format::X8R8G8B8;
        return pf.dwRBitMask == 0xFF0000;
    case 24:
        *format = rvk::Format::X8R8G8B8;
        *convert24 = true;
        return pf.dwRBitMask == 0xFF0000;
    case 16:
        if (pf.dwRBitMask == 0xF800) { *format = rvk::Format::R5G6B5; return true; }
        if (pf.dwRBitMask == 0x7C00) { *format = alpha && pf.dwRGBAlphaBitMask ? rvk::Format::A1R5G5B5 : rvk::Format::X1R5G5B5; return true; }
        if (pf.dwRBitMask == 0x0F00) { *format = rvk::Format::A4R4G4B4; return true; }
        return false;
    default:
        return false;
    }
}

void PixelFormatFor(rvk::Format format, DDPIXELFORMAT* pf)
{
    std::memset(pf, 0, sizeof(*pf));
    pf->dwSize = sizeof(*pf);
    auto rgb = [&](DWORD bits, DWORD r, DWORD g, DWORD b, DWORD a) {
        pf->dwFlags = DDPF_RGB | (a ? DDPF_ALPHAPIXELS : 0);
        pf->dwRGBBitCount = bits;
        pf->dwRBitMask = r;
        pf->dwGBitMask = g;
        pf->dwBBitMask = b;
        pf->dwRGBAlphaBitMask = a;
    };
    switch (format) {
    case rvk::Format::A8R8G8B8: rgb(32, 0xFF0000, 0xFF00, 0xFF, 0xFF000000); break;
    case rvk::Format::X8R8G8B8: rgb(32, 0xFF0000, 0xFF00, 0xFF, 0); break;
    case rvk::Format::R5G6B5: rgb(16, 0xF800, 0x07E0, 0x001F, 0); break;
    case rvk::Format::A1R5G5B5: rgb(16, 0x7C00, 0x03E0, 0x001F, 0x8000); break;
    case rvk::Format::X1R5G5B5: rgb(16, 0x7C00, 0x03E0, 0x001F, 0); break;
    case rvk::Format::A4R4G4B4: rgb(16, 0x0F00, 0x00F0, 0x000F, 0xF000); break;
    case rvk::Format::L8: pf->dwFlags = DDPF_LUMINANCE; pf->dwLuminanceBitCount = 8; pf->dwLuminanceBitMask = 0xFF; break;
    case rvk::Format::A8L8:
        pf->dwFlags = DDPF_LUMINANCE | DDPF_ALPHAPIXELS;
        pf->dwLuminanceBitCount = 16;
        pf->dwLuminanceBitMask = 0xFF;
        pf->dwLuminanceAlphaBitMask = 0xFF00;
        break;
    case rvk::Format::A8: pf->dwFlags = DDPF_ALPHA; pf->dwAlphaBitDepth = 8; break;
    default: {
        static const DWORD codes[] = {MAKEFOURCC('D', 'X', 'T', '1'), MAKEFOURCC('D', 'X', 'T', '2'), MAKEFOURCC('D', 'X', 'T', '3'),
                                      MAKEFOURCC('D', 'X', 'T', '4'), MAKEFOURCC('D', 'X', 'T', '5')};
        pf->dwFlags = DDPF_FOURCC;
        pf->dwFourCC = codes[uint32_t(format) - uint32_t(rvk::Format::DXT1)];
        break;
    }
    }
}

void FillDeviceDesc(D3DDEVICEDESC7* d, REFCLSID device)
{
    std::memset(d, 0, sizeof(*d));
    d->dwDevCaps = D3DDEVCAPS_FLOATTLVERTEX | D3DDEVCAPS_EXECUTESYSTEMMEMORY | D3DDEVCAPS_TLVERTEXSYSTEMMEMORY |
                   D3DDEVCAPS_TEXTUREVIDEOMEMORY | D3DDEVCAPS_DRAWPRIMTLVERTEX | D3DDEVCAPS_CANRENDERAFTERFLIP |
                   D3DDEVCAPS_DRAWPRIMITIVES2 | D3DDEVCAPS_DRAWPRIMITIVES2EX | D3DDEVCAPS_HWRASTERIZATION |
                   D3DDEVCAPS_CANBLTSYSTONONLOCAL | D3DDEVCAPS_TEXTURENONLOCALVIDMEM;
    if (device == IID_IDirect3DTnLHalDevice)
        d->dwDevCaps |= D3DDEVCAPS_HWTRANSFORMANDLIGHT;
    D3DPRIMCAPS prim{};
    prim.dwSize = sizeof(prim);
    prim.dwMiscCaps = D3DPMISCCAPS_MASKZ | D3DPMISCCAPS_CULLNONE | D3DPMISCCAPS_CULLCW | D3DPMISCCAPS_CULLCCW;
    prim.dwRasterCaps = D3DPRASTERCAPS_DITHER | D3DPRASTERCAPS_ZTEST | D3DPRASTERCAPS_FOGVERTEX | D3DPRASTERCAPS_FOGTABLE |
                        D3DPRASTERCAPS_MIPMAPLODBIAS | D3DPRASTERCAPS_FOGRANGE | D3DPRASTERCAPS_ANISOTROPY |
                        D3DPRASTERCAPS_WFOG | D3DPRASTERCAPS_ZFOG;
    prim.dwZCmpCaps = prim.dwAlphaCmpCaps = D3DPCMPCAPS_NEVER | D3DPCMPCAPS_LESS | D3DPCMPCAPS_EQUAL | D3DPCMPCAPS_LESSEQUAL |
                                           D3DPCMPCAPS_GREATER | D3DPCMPCAPS_NOTEQUAL | D3DPCMPCAPS_GREATEREQUAL | D3DPCMPCAPS_ALWAYS;
    prim.dwSrcBlendCaps = prim.dwDestBlendCaps = 0x1FFF;     // all D3DPBLENDCAPS_*
    prim.dwShadeCaps = D3DPSHADECAPS_COLORGOURAUDRGB | D3DPSHADECAPS_SPECULARGOURAUDRGB | D3DPSHADECAPS_ALPHAGOURAUDBLEND |
                       D3DPSHADECAPS_FOGGOURAUD;
    prim.dwTextureCaps = D3DPTEXTURECAPS_PERSPECTIVE | D3DPTEXTURECAPS_ALPHA | D3DPTEXTURECAPS_TRANSPARENCY |
                         D3DPTEXTURECAPS_BORDER | D3DPTEXTURECAPS_ALPHAPALETTE | D3DPTEXTURECAPS_PROJECTED;
    prim.dwTextureFilterCaps = D3DPTFILTERCAPS_NEAREST | D3DPTFILTERCAPS_LINEAR | D3DPTFILTERCAPS_MIPNEAREST |
                               D3DPTFILTERCAPS_MIPLINEAR | D3DPTFILTERCAPS_LINEARMIPNEAREST | D3DPTFILTERCAPS_LINEARMIPLINEAR |
                               D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR | D3DPTFILTERCAPS_MIPFPOINT |
                               D3DPTFILTERCAPS_MIPFLINEAR | D3DPTFILTERCAPS_MAGFPOINT | D3DPTFILTERCAPS_MAGFLINEAR;
    prim.dwTextureBlendCaps = D3DPTBLENDCAPS_DECAL | D3DPTBLENDCAPS_MODULATE | D3DPTBLENDCAPS_DECALALPHA |
                              D3DPTBLENDCAPS_MODULATEALPHA | D3DPTBLENDCAPS_ADD | D3DPTBLENDCAPS_COPY;
    prim.dwTextureAddressCaps = D3DPTADDRESSCAPS_WRAP | D3DPTADDRESSCAPS_MIRROR | D3DPTADDRESSCAPS_CLAMP |
                                D3DPTADDRESSCAPS_BORDER | D3DPTADDRESSCAPS_INDEPENDENTUV;
    d->dpcLineCaps = d->dpcTriCaps = prim;
    d->dwDeviceRenderBitDepth = DDBD_16 | DDBD_32;
    d->dwDeviceZBufferBitDepth = DDBD_16 | DDBD_24 | DDBD_32;
    d->dwMinTextureWidth = d->dwMinTextureHeight = 1;
    d->dwMaxTextureWidth = d->dwMaxTextureHeight = 4096;
    d->dwMaxTextureRepeat = 8192;
    d->dwMaxTextureAspectRatio = 4096;
    d->dwMaxAnisotropy = 16;
    d->dvGuardBandLeft = d->dvGuardBandTop = -8192.0f;
    d->dvGuardBandRight = d->dvGuardBandBottom = 8192.0f;
    d->dvExtentsAdjust = 0.0f;
    d->dwStencilCaps = 0;
    d->dwFVFCaps = 8;                                      // texture coordinate sets
    d->dwTextureOpCaps = D3DTEXOPCAPS_DISABLE | D3DTEXOPCAPS_SELECTARG1 | D3DTEXOPCAPS_SELECTARG2 | D3DTEXOPCAPS_MODULATE |
                         D3DTEXOPCAPS_MODULATE2X | D3DTEXOPCAPS_MODULATE4X | D3DTEXOPCAPS_ADD | D3DTEXOPCAPS_ADDSIGNED |
                         D3DTEXOPCAPS_ADDSIGNED2X | D3DTEXOPCAPS_SUBTRACT | D3DTEXOPCAPS_ADDSMOOTH |
                         D3DTEXOPCAPS_BLENDDIFFUSEALPHA | D3DTEXOPCAPS_BLENDTEXTUREALPHA | D3DTEXOPCAPS_BLENDFACTORALPHA |
                         D3DTEXOPCAPS_BLENDCURRENTALPHA;
    d->wMaxTextureBlendStages = 2;
    d->wMaxSimultaneousTextures = 2;
    d->dwMaxActiveLights = rvk::Device::kMaxLights;
    d->dvMaxVertexW = 1e10f;
    d->deviceGUID = device;
    d->wMaxUserClipPlanes = 0;
    d->wMaxVertexBlendMatrices = 0;
    d->dwVertexProcessingCaps = D3DVTXPCAPS_TEXGEN | D3DVTXPCAPS_MATERIALSOURCE7 | D3DVTXPCAPS_DIRECTIONALLIGHTS |
                                D3DVTXPCAPS_POSITIONALLIGHTS | D3DVTXPCAPS_LOCALVIEWER;
}

// ---------------------------------------------------------------------------------------------------
// Clipper and palette

HRESULT RClipper::DoSetHWnd(DWORD, HWND hwnd)
{
    window = hwnd;
    g_rvk.SetWindow(hwnd);
    return DD_OK;
}

HRESULT RClipper::DoGetHWnd(HWND* out)
{
    if (!out) return DDERR_INVALIDPARAMS;
    *out = window;
    return DD_OK;
}

HRESULT RClipper::DoIsClipListChanged(BOOL* changed)
{
    if (changed) *changed = FALSE;
    return DD_OK;
}

HRESULT RPalette::DoGetCaps(LPDWORD caps)
{
    if (caps) *caps = m_caps;
    return DD_OK;
}

HRESULT RPalette::DoGetEntries(DWORD, DWORD start, DWORD count, LPPALETTEENTRY out)
{
    if (!out || start + count > 256) return DDERR_INVALIDPARAMS;
    std::memcpy(out, entries + start, count * sizeof(PALETTEENTRY));
    return DD_OK;
}

HRESULT RPalette::DoSetEntries(DWORD, DWORD start, DWORD count, LPPALETTEENTRY in)
{
    if (!in || start + count > 256) return DDERR_INVALIDPARAMS;
    std::memcpy(entries + start, in, count * sizeof(PALETTEENTRY));
    return DD_OK;
}

// ---------------------------------------------------------------------------------------------------
// IDirectDraw7

void* RDirectDraw::Cast(REFIID iid)
{
    if (iid == IID_IDirectDraw7)
        return this;
    if (iid == IID_IDirect3D7) {
        if (m_d3d) {
            m_d3d->AddRef();
            return static_cast<IDirect3D7*>(m_d3d);
        }
        m_d3d = new RDirect3D(this);
        return static_cast<IDirect3D7*>(m_d3d);
    }
    return nullptr;
}

HRESULT RDirectDraw::DoCreateClipper(DWORD, LPDIRECTDRAWCLIPPER* out, IUnknown*)
{
    if (!out) return DDERR_INVALIDPARAMS;
    *out = new RClipper;
    return DD_OK;
}

HRESULT RDirectDraw::DoCreatePalette(DWORD flags, LPPALETTEENTRY entries, LPDIRECTDRAWPALETTE* out, IUnknown*)
{
    if (!out) return DDERR_INVALIDPARAMS;
    auto* p = new RPalette(flags);
    if (entries)
        std::memcpy(p->entries, entries, ((flags & DDPCAPS_8BIT) ? 256 : 16) * sizeof(PALETTEENTRY));
    *out = p;
    return DD_OK;
}

HRESULT RDirectDraw::DoCreateSurface(LPDDSURFACEDESC2 desc, LPDIRECTDRAWSURFACE7* out, IUnknown*)
{
    if (!desc || !out) return DDERR_INVALIDPARAMS;
    RSurface* s = nullptr;
    HRESULT r = RSurface::Create(this, *desc, &s);
    *out = s;
    return r;
}

static void DesktopMode(DDSURFACEDESC2* d, DWORD width, DWORD height, DWORD bpp)
{
    std::memset(d, 0, sizeof(*d));
    d->dwSize = sizeof(*d);
    d->dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
    d->dwWidth = width;
    d->dwHeight = height;
    d->lPitch = LONG(width * bpp / 8);
    d->dwRefreshRate = 60;
    PixelFormatFor(bpp == 16 ? rvk::Format::R5G6B5 : rvk::Format::X8R8G8B8, &d->ddpfPixelFormat);
}

HRESULT RDirectDraw::DoEnumDisplayModes(DWORD, LPDDSURFACEDESC2 filter, LPVOID ctx, LPDDENUMMODESCALLBACK2 cb)
{
    if (!cb) return DDERR_INVALIDPARAMS;
    static const DWORD sizes[][2] = {{640, 480}, {800, 600}, {1024, 768}, {1280, 720}, {1280, 1024}, {1366, 768},
                                     {1600, 900}, {1680, 1050}, {1920, 1080}, {1920, 1200}, {2560, 1440}, {3840, 2160}};
    DWORD dw = DWORD(GetSystemMetrics(SM_CXSCREEN)), dh = DWORD(GetSystemMetrics(SM_CYSCREEN));
    for (DWORD bpp : {16u, 32u}) {
        bool desktopListed = false;
        for (auto& sz : sizes) {
            if (sz[0] > dw || sz[1] > dh) continue;
            desktopListed |= sz[0] == dw && sz[1] == dh;
            DDSURFACEDESC2 d;
            DesktopMode(&d, sz[0], sz[1], bpp);
            if (filter && (filter->dwFlags & DDSD_WIDTH) && filter->dwWidth != d.dwWidth) continue;
            if (filter && (filter->dwFlags & DDSD_HEIGHT) && filter->dwHeight != d.dwHeight) continue;
            if (cb(&d, ctx) == DDENUMRET_CANCEL) return DD_OK;
        }
        if (!desktopListed) {
            DDSURFACEDESC2 d;
            DesktopMode(&d, dw, dh, bpp);
            if (cb(&d, ctx) == DDENUMRET_CANCEL) return DD_OK;
        }
    }
    return DD_OK;
}

HRESULT RDirectDraw::DoGetCaps(LPDDCAPS driver, LPDDCAPS hel)
{
    auto fill = [](LPDDCAPS c, bool hw) {
        DWORD size = c->dwSize ? c->dwSize : sizeof(DDCAPS);
        std::memset(c, 0, size);
        c->dwSize = size;
        if (!hw) return;
        c->dwCaps = DDCAPS_3D | DDCAPS_BLT | DDCAPS_BLTCOLORFILL | DDCAPS_BLTDEPTHFILL | DDCAPS_BLTSTRETCH |
                    DDCAPS_COLORKEY | DDCAPS_CANBLTSYSMEM | DDCAPS_CANCLIP | DDCAPS_CANCLIPSTRETCHED | DDCAPS_ZBLTS |
                    DDCAPS_BLTFOURCC;
        c->dwCaps2 = DDCAPS2_CANRENDERWINDOWED | DDCAPS2_WIDESURFACES | DDCAPS2_NOPAGELOCKREQUIRED |
                     DDCAPS2_FLIPNOVSYNC | DDCAPS2_PRIMARYGAMMA;
        c->dwCKeyCaps = DDCKEYCAPS_SRCBLT;
        c->dwFXCaps = DDFXCAPS_BLTSTRETCHX | DDFXCAPS_BLTSTRETCHY | DDFXCAPS_BLTSHRINKX | DDFXCAPS_BLTSHRINKY;
        c->dwVidMemTotal = c->dwVidMemFree = 0x7FFF0000;
        c->ddsCaps.dwCaps = DDSCAPS_3DDEVICE | DDSCAPS_BACKBUFFER | DDSCAPS_COMPLEX | DDSCAPS_FLIP | DDSCAPS_FRONTBUFFER |
                            DDSCAPS_MIPMAP | DDSCAPS_OFFSCREENPLAIN | DDSCAPS_PRIMARYSURFACE | DDSCAPS_SYSTEMMEMORY |
                            DDSCAPS_TEXTURE | DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM | DDSCAPS_ZBUFFER | DDSCAPS_PALETTE;
        c->dwNumFourCCCodes = 5;
    };
    if (driver) fill(driver, true);
    if (hel) fill(hel, false);
    return DD_OK;
}

HRESULT RDirectDraw::DoGetDisplayMode(LPDDSURFACEDESC2 out)
{
    if (!out) return DDERR_INVALIDPARAMS;
    DesktopMode(out, DWORD(GetSystemMetrics(SM_CXSCREEN)), DWORD(GetSystemMetrics(SM_CYSCREEN)), 32);
    return DD_OK;
}

HRESULT RDirectDraw::DoGetFourCCCodes(LPDWORD count, LPDWORD codes)
{
    static const DWORD all[] = {MAKEFOURCC('D', 'X', 'T', '1'), MAKEFOURCC('D', 'X', 'T', '2'), MAKEFOURCC('D', 'X', 'T', '3'),
                                MAKEFOURCC('D', 'X', 'T', '4'), MAKEFOURCC('D', 'X', 'T', '5')};
    if (!count) return DDERR_INVALIDPARAMS;
    if (codes) std::memcpy(codes, all, std::min<DWORD>(*count, 5) * sizeof(DWORD));
    *count = 5;
    return DD_OK;
}

HRESULT RDirectDraw::DoSetCooperativeLevel(HWND hwnd, DWORD)
{
    g_rvk.SetWindow(hwnd);
    return DD_OK;
}

HRESULT RDirectDraw::DoGetAvailableVidMem(LPDDSCAPS2, LPDWORD total, LPDWORD free)
{
    if (total) *total = 0x7FFF0000;
    if (free) *free = 0x7FFF0000;
    return DD_OK;
}

HRESULT RDirectDraw::DoGetDeviceIdentifier(LPDDDEVICEIDENTIFIER2 out, DWORD)
{
    if (!out) return DDERR_INVALIDPARAMS;
    std::memset(out, 0, sizeof(*out));
    std::snprintf(out->szDriver, sizeof(out->szDriver), "randy-vk");
    std::snprintf(out->szDescription, sizeof(out->szDescription), "%s (randy-vk)", g_rvk.gpuName.c_str());
    out->dwVendorId = 0x10DE;
    out->dwDeviceId = 0x2782;
    out->guidDeviceIdentifier = {0x72616e64, 0x7976, 0x6b00, {'r', 'a', 'n', 'd', 'y', 'v', 'k', 0}};
    return DD_OK;
}

// ---------------------------------------------------------------------------------------------------
// IDirect3D7

RDirect3D::RDirect3D(RDirectDraw* dd) : ddraw(dd)
{
    ddraw->AddRef();
}

RDirect3D::~RDirect3D()
{
    ddraw->m_d3d = nullptr;
    ddraw->Release();
}

void* RDirect3D::Cast(REFIID iid)
{
    if (iid == IID_IDirect3D7)
        return this;
    if (iid == IID_IDirectDraw7) {
        ddraw->AddRef();
        return static_cast<IDirectDraw7*>(ddraw);
    }
    return nullptr;
}

HRESULT RDirect3D::DoEnumDevices(LPD3DENUMDEVICESCALLBACK7 cb, LPVOID ctx)
{
    if (!cb) return DDERR_INVALIDPARAMS;
    D3DDEVICEDESC7 desc;
    FillDeviceDesc(&desc, IID_IDirect3DHALDevice);
    char halDesc[] = "Direct3D HAL", halName[] = "Direct3D HAL";
    if (cb(halDesc, halName, &desc, ctx) == D3DENUMRET_CANCEL) return D3D_OK;
    FillDeviceDesc(&desc, IID_IDirect3DTnLHalDevice);
    char tnlDesc[] = "Direct3D T&L HAL", tnlName[] = "Direct3D T&L HAL";
    cb(tnlDesc, tnlName, &desc, ctx);
    return D3D_OK;
}

HRESULT RDirect3D::DoCreateDevice(REFCLSID clsid, LPDIRECTDRAWSURFACE7 target, LPDIRECT3DDEVICE7* out)
{
    auto* s = static_cast<RSurface*>(target);
    if (!s || !out || (s->kind != RSurface::Kind::Main && s->kind != RSurface::Kind::RenderTarget))
        return DDERR_INVALIDPARAMS;
    if (!g_rvk.device) {
        RvkLog("CreateDevice without an rvk device (no 3D back buffer created yet)");
        return DDERR_GENERIC;
    }
    *out = new RDevice(this, s, clsid);
    return D3D_OK;
}

HRESULT RDirect3D::DoCreateVertexBuffer(LPD3DVERTEXBUFFERDESC desc, LPDIRECT3DVERTEXBUFFER7* out, DWORD)
{
    if (!desc || !out) return DDERR_INVALIDPARAMS;
    *out = new RVertexBuffer(*desc);
    return D3D_OK;
}

HRESULT RDirect3D::DoEnumZBufferFormats(REFCLSID, LPD3DENUMPIXELFORMATSCALLBACK cb, LPVOID ctx)
{
    if (!cb) return DDERR_INVALIDPARAMS;
    struct Z { DWORD bits, mask, stencil; } formats[] = {{16, 0xFFFF, 0}, {32, 0xFFFFFF00, 0}, {32, 0xFFFFFF00, 0xFF}, {32, 0xFFFFFFFF, 0}};
    for (auto& z : formats) {
        DDPIXELFORMAT pf{};
        pf.dwSize = sizeof(pf);
        pf.dwFlags = DDPF_ZBUFFER | (z.stencil ? DDPF_STENCILBUFFER : 0);
        pf.dwZBufferBitDepth = z.bits;
        pf.dwZBitMask = z.mask;
        pf.dwStencilBitDepth = z.stencil ? 8 : 0;
        pf.dwStencilBitMask = z.stencil;
        if (cb(&pf, ctx) == D3DENUMRET_CANCEL) break;
    }
    return D3D_OK;
}

// ---------------------------------------------------------------------------------------------------
// Import hooks for RANDYVK_DDRAW=rvk

HRESULT WINAPI RvkDirectDrawCreateEx(GUID*, LPVOID* out, REFIID iid, IUnknown*)
{
    if (!out) return DDERR_INVALIDPARAMS;
    if (iid != IID_IDirectDraw7) {
        RvkLog("DirectDrawCreateEx for a non-IDirectDraw7 interface");
        return DDERR_UNSUPPORTED;
    }
    *out = static_cast<IDirectDraw7*>(new RDirectDraw);
    return DD_OK;
}

HRESULT WINAPI RvkDirectDrawEnumerateExA(LPDDENUMCALLBACKEXA cb, LPVOID ctx, DWORD)
{
    if (!cb) return DDERR_INVALIDPARAMS;
    char desc[] = "Primary Display Driver", name[] = "display";
    cb(nullptr, desc, name, ctx, nullptr);
    return DD_OK;
}

}  // namespace rvkproxy
