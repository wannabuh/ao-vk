// rvk backend: shared state, pixel formats, caps, IDirectDraw7, IDirect3D7, clipper, palette, import hooks.
#include "rvk_backend.h"

#include <cstdarg>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <vector>

namespace rvkproxy {

RvkState g_rvk;

namespace {
// One critical section for the whole backend (recursive, cheap when uncontended). Each new calling thread is
// logged once; the check costs a TEB read per call.
CRITICAL_SECTION g_comLock;
bool g_comLockReady = [] { InitializeCriticalSectionAndSpinCount(&g_comLock, 1000); return true; }();
DWORD g_lastThread = 0;
std::vector<DWORD> g_seenThreads;
int64_t g_drawCalls = 0;
}

void CountBackendDraw() { ++g_drawCalls; }

ComScope::ComScope(unsigned methodIndex)
{
    EnterCriticalSection(&g_comLock);
    DWORD tid = GetCurrentThreadId();
    if (tid != g_lastThread) {
        g_lastThread = tid;
        if (std::find(g_seenThreads.begin(), g_seenThreads.end(), tid) == g_seenThreads.end()) {
            g_seenThreads.push_back(tid);
            RvkLog("thread %lu calls DirectDraw/Direct3D (first call: %s)", tid, ComMethodName(methodIndex));
        }
    }
}

ComScope::~ComScope() { LeaveCriticalSection(&g_comLock); }

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
    rvk::SetLogSink([](const char* line) { RvkLog("rvk: %s", line); });
    device = new rvk::ThreadedDevice;
    std::string error;
    char threaded[8] = "1";
    GetEnvironmentVariableA("RANDYVK_THREADED", threaded, sizeof(threaded));
    if (!device->Init(window, width, height, &error, threaded[0] != '0')) {
        RvkLog("rvk device creation failed: %s", error.c_str());
        delete device;
        device = nullptr;
        return false;
    }
    char pixelLighting[8] = "1";
    GetEnvironmentVariableA("RANDYVK_PIXEL_LIGHTING", pixelLighting, sizeof(pixelLighting));
    device->SetPixelLighting(pixelLighting[0] != '0');
    gpuName = device->Info().gpu;
    RvkLog("rvk device %ux%u on %s (window %p, %s, %s lighting)", width, height, gpuName.c_str(), (void*)window,
           device->Threaded() ? "worker thread" : "calling thread", device->PixelLighting() ? "per-pixel" : "per-vertex");
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
    // Ctrl+Shift+F10: per-pixel / per-vertex lighting. Ctrl+Shift+F11: lighting debug view.
    static bool f10Down, f11Down;
    bool chord = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000);
    bool f10 = chord && (GetAsyncKeyState(VK_F10) & 0x8000), f11 = chord && (GetAsyncKeyState(VK_F11) & 0x8000);
    if (f10 && !f10Down) {
        device->SetPixelLighting(!device->PixelLighting());
        RvkLog("lighting: %s", device->PixelLighting() ? "per-pixel" : "per-vertex");
    }
    if (f11 && !f11Down) {
        device->SetLightingDebug(!device->LightingDebug());
        RvkLog("lighting debug view %s", device->LightingDebug() ? "on" : "off");
    }
    f10Down = f10;
    f11Down = f11;
    // Ctrl+Shift+F9: dump the next frame's 3D draws + a screenshot next to the log (logs\rvk-frame-HHMMSS.*).
    static bool f9Down;
    bool f9 = chord && (GetAsyncKeyState(VK_F9) & 0x8000);
    if (f9 && !f9Down) {
        char dir[MAX_PATH] = "";
        GetEnvironmentVariableA("RANDYVK_LOG", dir, sizeof(dir));
        if (char* slash = std::strrchr(dir, '\\')) slash[1] = 0; else dir[0] = 0;
        SYSTEMTIME t;
        GetLocalTime(&t);
        char base[MAX_PATH];
        std::snprintf(base, sizeof(base), "%srvk-frame-%02d%02d%02d", dir, t.wHour, t.wMinute, t.wSecond);
        device->RequestFrameDump(std::string(base) + ".txt");
        device->RequestScreenshot(std::string(base) + ".bmp");
        RvkLog("frame dump: %s.txt / .bmp", base);
    }
    f9Down = f9;
    // Heartbeat: shows whether frames keep coming (a frozen picture vs. a hung game).
    static unsigned frames;
    static DWORD lastTick = GetTickCount();
    if (++frames % 600 == 0) {
        DWORD now = GetTickCount();
        double frameMs = double(now - lastTick) / 600.0;
        RvkLog("presented %u frames: %.1f fps, %.2f ms/frame, %lld draws/frame", frames, 1000.0 / std::max(frameMs, 0.001),
               frameMs, (long long)(g_drawCalls / 600));
        g_drawCalls = 0;
        lastTick = now;
    }
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
    // The values D3D7 reports through D7VK on this GPU (tools: randy_harness prints both), so the game and its
    // D3DX make the same decisions as with D3D7. rvk itself covers what the client actually uses.
    std::memset(d, 0, sizeof(*d));
    d->dwDevCaps = 0x000BBEF1;
    if (device != IID_IDirect3DTnLHalDevice)
        d->dwDevCaps &= ~D3DDEVCAPS_HWTRANSFORMANDLIGHT;
    D3DPRIMCAPS prim{};
    prim.dwSize = sizeof(prim);
    prim.dwMiscCaps = 0x00000072;
    prim.dwRasterCaps = 0x003361B1;
    prim.dwZCmpCaps = prim.dwAlphaCmpCaps = 0x000000FF;
    prim.dwSrcBlendCaps = prim.dwDestBlendCaps = 0x00001FFF;
    prim.dwShadeCaps = 0x000C528A;
    prim.dwTextureCaps = 0x00000DDF;
    prim.dwTextureFilterCaps = 0x0703073F;
    prim.dwTextureBlendCaps = 0x000000FF;
    prim.dwTextureAddressCaps = 0x0000001F;
    d->dpcLineCaps = d->dpcTriCaps = prim;
    d->dwDeviceRenderBitDepth = 0x00000700;
    d->dwDeviceZBufferBitDepth = 0x00000600;
    d->dwMinTextureWidth = d->dwMinTextureHeight = 1;
    d->dwMaxTextureWidth = d->dwMaxTextureHeight = 8192;
    d->dwMaxTextureRepeat = 8192;
    d->dwMaxTextureAspectRatio = 8192;
    d->dwMaxAnisotropy = 16;
    d->dvGuardBandLeft = d->dvGuardBandTop = -8192.0f;
    d->dvGuardBandRight = d->dvGuardBandBottom = 8192.0f;
    d->dwStencilCaps = 0x000000FF;
    d->dwFVFCaps = 0x00000008;
    d->dwTextureOpCaps = 0x00FFFFFF;
    d->wMaxTextureBlendStages = 8;
    d->wMaxSimultaneousTextures = 8;
    d->dwMaxActiveLights = 8;
    d->dvMaxVertexW = 1e10f;
    d->deviceGUID = device;
    d->wMaxUserClipPlanes = 6;
    d->wMaxVertexBlendMatrices = 4;
    d->dwVertexProcessingCaps = 0x0000003F;
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
    // D7VK's values (it reports the same caps for the driver and the emulation layer).
    auto fill = [](LPDDCAPS c) {
        DWORD size = c->dwSize ? c->dwSize : sizeof(DDCAPS);
        std::memset(c, 0, size);
        c->dwSize = size;
        c->dwCaps = 0xF5408661;
        c->dwCaps2 = 0x006A1801;
        c->dwCKeyCaps = 0x00000201;
        c->dwFXCaps = 0x0003FEC1;
        c->dwPalCaps = 0x00000014;
        c->dwZBufferBitDepths = 0x00000600;
        c->dwVidMemTotal = c->dwVidMemFree = 2139095040;
        c->ddsCaps.dwCaps = 0x3046737C;
        c->dwNumFourCCCodes = 5;
    };
    if (driver) fill(driver);
    if (hel) fill(hel);
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
    struct Z { DWORD bits, mask, stencil; } formats[] = {{16, 0xFFFF, 0}, {24, 0xFFFFFF, 0}, {32, 0xFFFFFF, 0},
                                                         {32, 0xFFFFFF, 0xFF000000}};   // as D7VK reports them
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
    ComScope scope(ComMethodCount() - 2);
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
    ComScope scope(ComMethodCount() - 1);
    if (!cb) return DDERR_INVALIDPARAMS;
    char desc[] = "Primary Display Driver", name[] = "display";
    cb(nullptr, desc, name, ctx, nullptr);
    return DD_OK;
}

}  // namespace rvkproxy
