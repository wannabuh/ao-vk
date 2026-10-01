// Boots the game's real renderer (randy31.dll, i.e. the proxy in front of randy31_orig.dll) the way
// DisplaySystem does, draws a test frame through Randy's own exports and saves the back buffer as a BMP.
// Must run from the client folder (copied there as randy_harness.exe) so it loads the same DLLs as the
// game, including whatever ddraw.dll is installed (D7VK). tools/randy-harness.sh does that.
//
//   randy_harness.exe [--frames N] [--shot out.bmp]
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ddraw.h>
#include <d3d.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

HMODULE g_randy;

template <typename T>
T Export(const char* name)
{
    auto p = reinterpret_cast<T>(GetProcAddress(g_randy, name));
    if (!p) {
        std::printf("missing export %s\n", name);
        std::exit(2);
    }
    return p;
}

// Layout of the VS2010 std::string Randy reports errors into (release build: 16-byte buffer, size, capacity).
struct Vc10String {
    union { char buf[16]; char* ptr; };
    uint32_t size;
    uint32_t capacity;
    const char* c_str() const { return capacity > 15 ? ptr : buf; }
};

// thiscall exports are called as fastcall with an unused EDX: `this` in ECX, callee pops the stack args.
using InitializeFn = void*(__cdecl*)(unsigned short, unsigned short, Vc10String*, GUID*, GUID*, bool, HWND, int);
using ViewPortCtorFn = void*(__fastcall*)(void* self, void*, unsigned, unsigned, unsigned, unsigned, const void* rgb);
using OpenFn = bool(__fastcall*)(void* self, void*, bool* restored);
using CloseFn = void(__fastcall*)(void* self, void*);
using ClearFn = void(__fastcall*)(void* self, void*, const unsigned* color, bool target, bool zbuffer, unsigned);
using FlipFn = void(__fastcall*)(void* self, void*, bool);
using GetBackBufferFn = void*(__fastcall*)(void* self, void*);
using GetSurfacePointerFn = IDirectDrawSurface7**(__fastcall*)(void* self, void*);
using SetRenderStateFn = bool(__fastcall*)(void* self, void*, uint32_t state, uint32_t value, int priority);
using SetTssFn = bool(__fastcall*)(void* self, void*, unsigned stage, uint32_t type, uint32_t value, int priority);
using UpdateDeviceFn = void(__fastcall*)(void* self, void*);
using RenderUpFn = void(__fastcall*)(void* self, void*, uint32_t fvf, void* vertices, uint32_t count, uint32_t flags);

const GUID kTnLHalDevice = {0xf5049e78, 0x4861, 0x11d2, {0xa4, 0x07, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8}};

struct VtxRhw { float x, y, z, rhw; uint32_t color; };
constexpr uint32_t kFvfRhwDiffuse = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;

bool SaveSurface(IDirectDrawSurface7* surface, const char* path)
{
    DDSURFACEDESC2 sd{};
    sd.dwSize = sizeof(sd);
    HRESULT hr = surface->Lock(nullptr, &sd, DDLOCK_WAIT | DDLOCK_READONLY, nullptr);
    if (FAILED(hr)) {
        std::printf("back buffer Lock failed: 0x%08lx\n", hr);
        return false;
    }
    uint32_t w = sd.dwWidth, h = sd.dwHeight, bpp = sd.ddpfPixelFormat.dwRGBBitCount;
    std::printf("back buffer %ux%u, %u bpp, masks R %08lx G %08lx B %08lx\n", w, h, bpp, sd.ddpfPixelFormat.dwRBitMask,
                sd.ddpfPixelFormat.dwGBitMask, sd.ddpfPixelFormat.dwBBitMask);
    FILE* f = std::fopen(path, "wb");
    if (f && (bpp == 32 || bpp == 16)) {
        uint32_t rowBytes = w * 3, pad = (4 - rowBytes % 4) % 4, size = (rowBytes + pad) * h;
        uint8_t header[54] = {'B', 'M'};
        auto put32 = [&](int at, uint32_t v) { std::memcpy(header + at, &v, 4); };
        put32(2, 54 + size); put32(10, 54); put32(14, 40); put32(18, w); put32(22, h);
        header[26] = 1; header[28] = 24; put32(34, size);
        std::fwrite(header, 1, 54, f);
        std::vector<uint8_t> row(rowBytes + pad, 0);
        for (int y = int(h) - 1; y >= 0; --y) {
            const uint8_t* src = static_cast<const uint8_t*>(sd.lpSurface) + size_t(y) * sd.lPitch;
            for (uint32_t x = 0; x < w; ++x) {
                if (bpp == 32) {
                    std::memcpy(&row[x * 3], src + x * 4, 3);
                } else {                                       // R5G6B5
                    uint16_t p;
                    std::memcpy(&p, src + x * 2, 2);
                    row[x * 3 + 0] = uint8_t((p & 0x1F) * 255 / 31);
                    row[x * 3 + 1] = uint8_t(((p >> 5) & 0x3F) * 255 / 63);
                    row[x * 3 + 2] = uint8_t((p >> 11) * 255 / 31);
                }
            }
            std::fwrite(row.data(), 1, row.size(), f);
        }
    }
    if (f) std::fclose(f);
    surface->Unlock(nullptr);
    return f != nullptr;
}

}  // namespace

int main(int argc, char** argv)
{
    int frames = 3;
    std::string shot = "randy_harness.bmp";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--frames" && i + 1 < argc) frames = std::atoi(argv[++i]);
        else if (a == "--shot" && i + 1 < argc) shot = argv[++i];
    }
    const unsigned width = 640, height = 480;

    g_randy = LoadLibraryA("randy31.dll");
    if (!g_randy) {
        std::printf("LoadLibrary(randy31.dll) failed: %lu\n", GetLastError());
        return 1;
    }
    WNDCLASSA wc{};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "randy_harness";
    RegisterClassA(&wc);
    RECT r{0, 0, LONG(width), LONG(height)};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowA("randy_harness", "randy harness", WS_OVERLAPPEDWINDOW, 0, 0, r.right - r.left,
                              r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);   // never shown

    auto initialize = Export<InitializeFn>(
        "?Initialize@Randy_t@@SAPAV1@GGAAV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@PAU_GUID@@1_NPAUHWND__@@W4BufferFormat_e@1@@Z");
    Vc10String error{};
    error.capacity = 15;
    GUID device = kTnLHalDevice;
    void* randy = initialize(width, height, &error, nullptr, &device, true, hwnd, 1);   // windowed, 32-bit
    if (!randy) {
        std::printf("Randy_t::Initialize failed: %s\n", error.c_str());
        return 1;
    }
    void* render = *Export<void**>("?m_pcInstance@render_t@@0PAV1@A");
    std::printf("Randy_t %p, render_t %p, IDirect3DDevice7 %p\n", randy, render, *static_cast<void**>(render));

    alignas(16) static uint8_t viewportStorage[0x178];    // sizeof(RViewPort_t), as DisplaySystem allocates
    void* viewport = Export<ViewPortCtorFn>("??0RViewPort_t@@QAE@IIIIABVRGB_t@@@Z")(
        viewportStorage, nullptr, 0, 0, width, height, Export<void*>("?white@RGB_t@@2V1@A"));   // clear colour
    void* deviceState = *reinterpret_cast<void**>(static_cast<uint8_t*>(viewport) + 8);   // RViewPort_t+8

    auto open = Export<OpenFn>("?Open@RViewPort_t@@QAE_NPA_N@Z");
    auto close = Export<CloseFn>("?Close@RViewPort_t@@QAEXXZ");
    auto clear = Export<ClearFn>("?Clear@RViewPort_t@@QAEXQBI_N1I@Z");
    auto flip = Export<FlipFn>("?Flip@Randy_t@@QAEX_N@Z");
    auto setRs = Export<SetRenderStateFn>("?SetRenderState@DeviceState@@QAE_NW4_D3DRENDERSTATETYPE@@KW4Priority_e@1@@Z");
    auto setTss = Export<SetTssFn>("?SetTextureStageState@DeviceState@@QAE_NIW4_D3DTEXTURESTAGESTATETYPE@@KW4Priority_e@1@@Z");
    auto update = Export<UpdateDeviceFn>("?UpdateDevice@DeviceState@@QAEXXZ");
    auto triList = Export<RenderUpFn>("?RenderTriangleList@render_t@@QAEXKPAXKK@Z");
    auto triStrip = Export<RenderUpFn>("?RenderTriangleStrip@render_t@@QAEXKPAXKK@Z");
    auto backBuffer = Export<GetBackBufferFn>("?GetBackBuffer@Randy_t@@QBEPAVsurface_t@@XZ");
    auto surfacePtr = Export<GetSurfacePointerFn>("?GetSurfacePointer@surface_t@@QAEPAPAUIDirectDrawSurface7@@XZ");

    for (int frame = 0; frame < frames; ++frame) {
        bool restored = false;
        open(viewport, nullptr, &restored);
        unsigned clearColor = 0xFF203040;
        clear(viewport, nullptr, &clearColor, true, true, 0);

        const int prio = 10;
        setRs(deviceState, nullptr, D3DRENDERSTATE_ZENABLE, FALSE, prio);
        setRs(deviceState, nullptr, D3DRENDERSTATE_LIGHTING, FALSE, prio);
        setRs(deviceState, nullptr, D3DRENDERSTATE_CULLMODE, D3DCULL_NONE, prio);
        setRs(deviceState, nullptr, D3DRENDERSTATE_ALPHABLENDENABLE, FALSE, prio);
        setTss(deviceState, nullptr, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG2, prio);
        setTss(deviceState, nullptr, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE, prio);
        setTss(deviceState, nullptr, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2, prio);
        setTss(deviceState, nullptr, 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE, prio);
        update(deviceState, nullptr);

        VtxRhw tri[3] = {{320, 40, 0, 1, 0xFFFF0000}, {560, 400, 0, 1, 0xFF00FF00}, {80, 400, 0, 1, 0xFF0000FF}};
        triList(render, nullptr, kFvfRhwDiffuse, tri, 3, 0);
        setRs(deviceState, nullptr, D3DRENDERSTATE_ALPHABLENDENABLE, TRUE, prio);
        setRs(deviceState, nullptr, D3DRENDERSTATE_SRCBLEND, D3DBLEND_SRCALPHA, prio);
        setRs(deviceState, nullptr, D3DRENDERSTATE_DESTBLEND, D3DBLEND_INVSRCALPHA, prio);
        update(deviceState, nullptr);
        VtxRhw quad[4] = {{200, 180, 0, 1, 0xC0FFFFFF}, {440, 180, 0, 1, 0xC0FFFF00},
                          {200, 300, 0, 1, 0x40FFFFFF}, {440, 300, 0, 1, 0x40FF00FF}};
        triStrip(render, nullptr, kFvfRhwDiffuse, quad, 4, 0);

        close(viewport, nullptr);
        if (frame == frames - 1) {
            void* bb = backBuffer(randy, nullptr);
            IDirectDrawSurface7** surface = bb ? surfacePtr(bb, nullptr) : nullptr;
            if (surface && *surface)
                SaveSurface(*surface, shot.c_str());
            else
                std::printf("no back buffer surface\n");
        }
        flip(randy, nullptr, false);
    }
    std::printf("harness done\n");
    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 0);   // skip Randy's static destructors / device teardown
}
