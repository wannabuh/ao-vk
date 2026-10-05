// Boots the game's real renderer (randy31.dll, i.e. the proxy in front of randy31_orig.dll) the way
// DisplaySystem does, draws a test frame through Randy's own exports and saves the back buffer as a BMP.
// Must run from the client folder (copied there as randy_harness.exe) so it loads the same DLLs as the
// game, including whatever ddraw.dll is installed (D7VK). tools/randy-harness.sh does that.
//
//   randy_harness.exe [--frames N] [--shot out.bmp]
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define INITGUID
#include <windows.h>
#include <ddraw.h>
#include <d3d.h>

#include <algorithm>
#include <cmath>
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

// A checksum of the DeviceState (render states, texture stage states 1..24, textures: priority / applied / wanted,
// their changed bits) - not texture stage state 0 or padding, which nothing writes.
void PrintDeviceState(const char* what, const void* deviceState)
{
    const uint8_t* ds = static_cast<const uint8_t*>(deviceState);
    uint32_t sum = 0;
    auto add = [&](uint32_t offset, uint32_t bytes) {
        for (uint32_t i = 0; i < bytes; ++i) sum = sum * 31 + ds[offset + i];
    };
    add(0, 0x741);                                   // render states, changed bits, flag
    for (uint32_t array : {0x744u, 0xA64u, 0xD84u})
        for (uint32_t stage = 0; stage < 8; ++stage) add(array + (stage * 0x19 + 1) * 4, 24 * 4);
    for (uint32_t stage = 0; stage < 8; ++stage) add(0x10A4 + stage * 8, 5);
    add(0x10E4, 0x65);                               // stages in use, textures, changed bits, flag
    std::printf("%s %08x\n", what, sum);
}

// surface_t*: its IDirectDrawSurface7's description.
std::string DescribeSurface(void* surface)
{
    if (!surface || !*static_cast<IDirectDrawSurface7**>(surface)) return std::string("-");
    DDSURFACEDESC2 d{};
    d.dwSize = sizeof(d);
    (*static_cast<IDirectDrawSurface7**>(surface))->GetSurfaceDesc(&d);
    char text[160];
    std::snprintf(text, sizeof(text), "%lux%lu caps %lx pf %lx/%lu/%lx", d.dwWidth, d.dwHeight, d.ddsCaps.dwCaps,
                  d.ddpfPixelFormat.dwFlags, d.ddpfPixelFormat.dwRGBBitCount, d.ddpfPixelFormat.dwRBitMask);
    return std::string(text);
}

// --debug-draw: what DisplaySystem and the camera add to Randy's debugger each frame (drawn at the end of
// RViewPort_t::Render); at frame 3 a checksum of its lists (`debugger` line).
void AddDebugShapes(int frame)
{
    void* debugger = Export<void*(__cdecl*)()>("?Get@Debugger_t@@SAPAV1@XZ")();
    auto line = Export<void(__fastcall*)(void*, void*, float, float, float, float, float, float, float, float, float)>(
        "?AddLine@Debugger_t@@QAEXVVector3_t@@0MMM@Z");
    auto screenLine = Export<void(__fastcall*)(void*, void*, float, float, float, float, float, float, float, float,
                                               float, bool)>("?Add2DLine@Debugger_t@@QAEXVVector3_t@@0MMM_N@Z");
    auto sphere = Export<void(__fastcall*)(void*, void*, float, float, float, float, float, float, float)>(
        "?AddSphere@Debugger_t@@QAEXVVector3_t@@MMMM@Z");
    uint8_t* orig = reinterpret_cast<uint8_t*>(GetModuleHandleA("randy31_orig.dll"));
    auto point = reinterpret_cast<void(__fastcall*)(void*, void*, float, float, float, float, float, float, bool)>(
        orig + 0x2CBAA);
    const float t = 0.1f * float(frame);
    for (int i = 0; i < 12; ++i)
        line(debugger, nullptr, -3.0f + 0.5f * float(i), 0.0f, -2.0f, 1.0f, 2.0f + t, 2.0f + 0.25f * float(i), 1.0f,
             0.1f * float(i), 0.3f);
    sphere(debugger, nullptr, 0.5f, 1.0f + t, 0.25f, 1.7f, 0.2f, 1.0f, 0.4f);
    sphere(debugger, nullptr, -1.0f, 0.3f, 2.0f, 0.6f, 1.0f, 1.0f, 0.0f);
    screenLine(debugger, nullptr, -0.5f, -0.5f, 7.0f, 0.5f, 0.4f + t, 9.0f, 0.0f, 1.0f, 1.0f, false);
    screenLine(debugger, nullptr, -0.25f, 0.5f, 0.0f, 0.75f, -0.4f, 0.0f, 1.0f, 0.5f, 0.0f, true);
    for (int i = 0; i < 9; ++i)
        point(debugger, nullptr, -0.8f + 0.2f * float(i), 0.1f * float(i) - t, 3.0f, 0.5f, 0.5f, 1.0f, (i & 1) != 0);
    if (frame != 3) return;
    uint32_t sum = 0;
    auto add = [&](const void* p, size_t n) {
        for (size_t k = 0; k < n; ++k) sum = sum * 31 + static_cast<const uint8_t*>(p)[k];
    };
    for (uint32_t list : {0x17D240u, 0x17D248u, 0x17D250u}) {
        const uint8_t* entries = *reinterpret_cast<uint8_t* const*>(orig + list);
        const uint32_t count = *reinterpret_cast<const uint32_t*>(orig + list + 4);
        add(&count, 4);
        if (entries) add(entries, count * (list == 0x17D250u ? 0x18u : 0x24u));
    }
    std::printf("debugger lists %08x (%u lines, %u screen lines, %u points), mode %x\n", sum,
                *reinterpret_cast<const uint32_t*>(orig + 0x17D244), *reinterpret_cast<const uint32_t*>(orig + 0x17D24C),
                *reinterpret_cast<const uint32_t*>(orig + 0x17D254), *reinterpret_cast<const uint32_t*>(orig + 0xB7500));
}

// What Randy_t::Initialize set up (`init` lines): hardware level, the hardware checks, buffer depths, the window's
// rectangle, render_t's interfaces, the primary surface and render target 0.
void PrintInit()
{
    const uint8_t* orig = reinterpret_cast<uint8_t*>(GetModuleHandleA("randy31_orig.dll"));
    auto g = [&](uint32_t rva) { return *reinterpret_cast<const uint32_t*>(orig + rva); };
    auto b = [&](uint32_t rva) { return unsigned(orig[rva]); };
    const uint8_t* render = *reinterpret_cast<uint8_t* const*>(orig + 0x16BED0);
    std::printf("init level %u fullscreen %u 3dfx %u kyro %u dxt %u%u nvidia %u clearfix %u bits %u/%u/%u "
                "rect %d %d %d %d\n",
                g(0xB772C), b(0x17D31C), b(0x17D31D), b(0x17D31E), b(0x17D31F), b(0x17D330), b(0x17D331),
                b(0x17D332), g(0x17D324), g(0x17D328), g(0x17D32C), int(g(0x17D2A4)), int(g(0x17D2A8)),
                int(g(0x17D2AC)), int(g(0x17D2B0)));
    std::printf("init render %d%d%d (globals %d%d%d) rect %d %d %d %d +288 %x\n",
                *reinterpret_cast<void* const*>(render) != nullptr, *reinterpret_cast<void* const*>(render + 4) != nullptr,
                *reinterpret_cast<void* const*>(render + 8) != nullptr, g(0x17D318) != 0, g(0x17D314) != 0,
                g(0x17D2F0) != 0, *reinterpret_cast<const int*>(render + 0x278), *reinterpret_cast<const int*>(render + 0x27C),
                *reinterpret_cast<const int*>(render + 0x280), *reinterpret_cast<const int*>(render + 0x284),
                *reinterpret_cast<const uint32_t*>(render + 0x288));
    void* primary = *reinterpret_cast<void* const*>(orig + 0x17D2F4);
    void* const* rt0 = *reinterpret_cast<void* const* const*>(orig + 0x17D2F8);
    std::printf("init primary %s, target 0 colour %s, Z %s, failed %u\n", DescribeSurface(primary).c_str(),
                rt0 ? DescribeSurface(rt0[0]).c_str() : "none", rt0 ? DescribeSurface(rt0[1]).c_str() : "none",
                rt0 ? unsigned(reinterpret_cast<const uint8_t*>(rt0)[8]) : 0u);
}

// --targets: the user allows every emulated feature and the scene asks for all of them (Randy_t::SetFeatureUsage),
// so Randy makes its offscreen render targets; what it got printed (`rendertarget` lines).
void MakeRenderTargets(void* randy)
{
    auto setUsage = Export<void(__fastcall*)(void*, void*, unsigned)>("?SetFeatureUsage@Randy_t@@QAEXI@Z");
    uint8_t* offscreen = Export<uint8_t*>("?m_bUseOffscreenTechnology@Randy_t@@0_NA");
    auto target = Export<void*(__cdecl*)(int)>("?GetRenderTarget@Randy_t@@SAPAVRenderTarget_t@@H@Z");
    std::printf("rendertarget before:");
    for (int i = 1; i < 7; ++i) std::printf(" %d", target(i) ? 1 : 0);
    std::printf("\n");
    *offscreen = 0;                                  // targets 1..4 dropped
    setUsage(randy, nullptr, 0x3F8);
    *offscreen = 1;
    uint32_t* user = Export<uint32_t*>("?m_nRandyEmulationCapUserDefined@Randy_t@@0IA");
    *user = 0x7B;
    setUsage(randy, nullptr, 0x3F8);
    setUsage(randy, nullptr, 0x3F8);                 // (all there already)
    const float* widths = Export<float*>("?m_avRenderTargetWidth@Randy_t@@0PAMA");
    const float* heights = Export<float*>("?m_avRenderTargetHeight@Randy_t@@0PAMA");
    const uint8_t* orig = reinterpret_cast<uint8_t*>(GetModuleHandleA("randy31_orig.dll"));
    std::printf("rendertarget caps %x user %x used %x z %d stencil %d\n",
                *Export<uint32_t*>("?m_nRandyEmulationCap@Randy_t@@0IA"), *user,
                *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(randy) + 0x288),
                *reinterpret_cast<const int*>(orig + 0x17D328), *reinterpret_cast<const int*>(orig + 0x17D32C));
    auto describe = DescribeSurface;
    for (int i = 1; i < 7; ++i) {
        void** rt = static_cast<void**>(target(i));
        std::printf("rendertarget %d: %s", i, rt ? "" : "none");
        if (rt)
            std::printf("colour %s, Z %s, failed %d, size %g x %g", describe(rt[0]).c_str(), describe(rt[1]).c_str(),
                        reinterpret_cast<uint8_t*>(rt)[8], widths[i], heights[i]);
        std::printf("\n");
    }
}

// Randy_t::GetDevices into a VS2010 std::vector<DeviceDesc_t> (0x834-byte records): each adapter's names, display
// modes and Direct3D devices printed (`adapter` lines; the vector is left to the process's end).
void PrintAdapters()
{
    struct { uint8_t* first; uint8_t* last; uint8_t* end; uint32_t allocator; } devices{};
    Export<void(__cdecl*)(void*)>(
        "?GetDevices@Randy_t@@SAXAAV?$vector@UDeviceDesc_t@Randy_t@@V?$allocator@UDeviceDesc_t@Randy_t@@@std@@@std@@@Z")(
        &devices);
    auto text = [](const uint8_t* s) {               // a VS2010 std::string
        const uint32_t capacity = *reinterpret_cast<const uint32_t*>(s + 0x14);
        return capacity > 15 ? *reinterpret_cast<const char* const*>(s) : reinterpret_cast<const char*>(s);
    };
    const size_t count = size_t(devices.last - devices.first) / 0x834;
    std::printf("adapter count %zu, capacity %zu\n", count, size_t(devices.end - devices.first) / 0x834);
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* d = devices.first + i * 0x834;
        uint32_t sum = 0;
        auto add = [&](const uint8_t* p, size_t n) {
            for (size_t k = 0; k < n; ++k) sum = sum * 31 + p[k];
        };
        add(d, 16);                                  // GUID
        add(d + 0x48, 4 + 0x17C + 3);                // monitor, DDCAPS, flags
        const uint8_t modes = d[0x4CC], devs = d[0x830];
        for (int m = 0; m < modes; ++m) add(d + 0x1CC + m * 6, 5);
        for (int k = 0; k < devs; ++k) {
            const uint8_t* dev = d + 0x4D0 + k * 0x6C;
            add(dev, 0x15);                           // GUID, level, W-buffer
            for (int s3 = 0; s3 < 3; ++s3) {
                const char* t = text(dev + 0x18 + s3 * 0x1C);
                add(reinterpret_cast<const uint8_t*>(t), std::strlen(t) + 1);
            }
        }
        std::printf("adapter %zu: '%s' '%s', %u modes, %u devices", i, text(d + 0x10), text(d + 0x2C), modes, devs);
        for (int k = 0; k < devs; ++k)
            std::printf(" ['%s' level %d]", text(d + 0x4D0 + k * 0x6C + 0x18),
                        *reinterpret_cast<const int*>(d + 0x4D0 + k * 0x6C + 0x10));
        std::printf(", sum %08x\n", sum);
    }
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
using TextureCtorFn = void*(__fastcall*)(void* self, void*, const char* name, unsigned short w, unsigned short h,
                                         int format, bool mipmaps, void* palette);
using TextureLoadFn = bool(__fastcall*)(void* self, void*, const void* data, int format, unsigned pitch, RECT* rect,
                                        void* palette, bool filter);
using SetTextureFn = bool(__fastcall*)(void* self, void*, void* surface, unsigned stage, int priority);

// RTexture_t::TextureFormat_e is D3DX 7's D3DX_SURFACEFORMAT.
enum { D3DX_SF_A8R8G8B8 = 2, D3DX_SF_X8R8G8B8 = 3, D3DX_SF_A4R4G4B4 = 10, D3DX_SF_DXT1 = 18 };
struct VtxRhwTex { float x, y, z, rhw; uint32_t color; float u, v; };
constexpr uint32_t kFvfRhwDiffuseTex = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

// RTexture_t (0xBC bytes) through Randy's own D3DX path: D3DXCreateTexture + D3DXLoadTextureFromMemory.
void* MakeTexture(const char* name, unsigned w, unsigned h, int format, const void* pixels, unsigned pitch)
{
    void* storage = ::operator new(0xBC);
    void* tex = Export<TextureCtorFn>("??0RTexture_t@@QAE@PBDGGW4TextureFormat_e@0@_NPAX@Z")(
        storage, nullptr, name, (unsigned short)w, (unsigned short)h, format, false, nullptr);
    Export<TextureLoadFn>("?Load@RTexture_t@@QAE_NPBXW4TextureFormat_e@1@IPAUtagRECT@@PAX_N@Z")(
        tex, nullptr, pixels, format, pitch, nullptr, nullptr, true);
    return tex;
}

using SetTransformFn = void(__fastcall*)(void* self, void*, D3DTRANSFORMSTATETYPE, D3DMATRIX*);
using VbCtorFn = void*(__fastcall*)(void* self, void*, unsigned fvf, unsigned flags, unsigned memory, unsigned bytes);
using VbLockFn = void*(__fastcall*)(void* self, void*, unsigned, unsigned);
using VbUnlockFn = void(__fastcall*)(void* self, void*);
using ProcessVerticesFn = void(__fastcall*)(void* self, void*, void* dst, unsigned long op, unsigned long dstIndex,
                                            unsigned long count, void* src, unsigned long srcIndex, unsigned long flags);

D3DMATRIX Mat(float a, float b, float c, float d, float e, float f, float g, float h, float i, float j, float k, float l,
              float m, float n, float o, float p)
{
    D3DMATRIX r;
    float v[16] = {a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p};
    std::memcpy(&r, v, sizeof(v));
    return r;
}

// Software vertex processing through Randy (render_t::ProcessVertices) with lighting; prints the results so
// the D3D7 and rvk backends can be compared.
void TestProcessVertices(void* render)
{
    auto* device = *static_cast<IDirect3DDevice7**>(render);
    auto setTransform = Export<SetTransformFn>("?SetTransformMatrix@render_t@@QAEXW4_D3DTRANSFORMSTATETYPE@@PAU_D3DMATRIX@@@Z");
    D3DMATRIX world = Mat(0.8f, 0.0f, -0.6f, 0, 0, 1, 0, 0, 0.6f, 0.0f, 0.8f, 0, 0.5f, -0.2f, 0.3f, 1);   // rotate Y + move
    D3DMATRIX view = Mat(1, 0, 0, 0, 0, 0.9701f, 0.2425f, 0, 0, -0.2425f, 0.9701f, 0, 0, -0.2425f, 5.0f, 1);
    D3DMATRIX proj = Mat(1.299f, 0, 0, 0, 0, 1.732f, 0, 0, 0, 0, 1.001f, 1, 0, 0, -0.1001f, 0);
    setTransform(render, nullptr, D3DTRANSFORMSTATE_WORLD, &world);
    setTransform(render, nullptr, D3DTRANSFORMSTATE_VIEW, &view);
    setTransform(render, nullptr, D3DTRANSFORMSTATE_PROJECTION, &proj);
    D3DVIEWPORT7 vp{0, 0, 640, 480, 0.0f, 1.0f};
    device->SetViewport(&vp);

    device->SetRenderState(D3DRENDERSTATE_LIGHTING, TRUE);
    device->SetRenderState(D3DRENDERSTATE_SPECULARENABLE, TRUE);
    device->SetRenderState(D3DRENDERSTATE_AMBIENT, 0xFF202830);
    device->SetRenderState(D3DRENDERSTATE_COLORVERTEX, TRUE);
    device->SetRenderState(D3DRENDERSTATE_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1);
    device->SetRenderState(D3DRENDERSTATE_AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL);
    device->SetRenderState(D3DRENDERSTATE_NORMALIZENORMALS, TRUE);
    D3DMATERIAL7 mat{};
    mat.diffuse = {1, 1, 1, 1};
    mat.ambient = {0.5f, 0.5f, 0.5f, 1};
    mat.specular = {1, 1, 1, 1};
    mat.emissive = {0.05f, 0.0f, 0.0f, 0};
    mat.power = 12.0f;
    device->SetMaterial(&mat);
    D3DLIGHT7 sun{};
    sun.dltType = D3DLIGHT_DIRECTIONAL;
    sun.dcvDiffuse = {0.7f, 0.7f, 0.6f, 1};
    sun.dcvSpecular = {0.5f, 0.5f, 0.5f, 1};
    sun.dvDirection = {-0.3f, -1.0f, 0.4f};
    D3DLIGHT7 point{};
    point.dltType = D3DLIGHT_POINT;
    point.dcvDiffuse = {0.9f, 0.2f, 0.1f, 1};
    point.dcvAmbient = {0.05f, 0.05f, 0.05f, 1};
    point.dvPosition = {-1.0f, 0.5f, -1.0f};
    point.dvRange = 20;
    point.dvAttenuation0 = 0.5f;
    point.dvAttenuation1 = 0.2f;
    D3DLIGHT7 spot{};
    spot.dltType = D3DLIGHT_SPOT;
    spot.dcvDiffuse = {0.1f, 0.3f, 1.0f, 1};
    spot.dvPosition = {0.0f, 3.0f, 0.0f};
    spot.dvDirection = {0.0f, -1.0f, 0.1f};
    spot.dvRange = 30;
    spot.dvAttenuation0 = 1.0f;
    spot.dvFalloff = 1.0f;
    spot.dvTheta = 0.6f;
    spot.dvPhi = 1.4f;
    device->SetLight(0, &sun);
    device->SetLight(1, &point);
    device->SetLight(2, &spot);
    for (DWORD i = 0; i < 3; ++i) device->LightEnable(i, TRUE);

    struct Src { float x, y, z, nx, ny, nz; uint32_t c; float u, v; };      // 0x152
    struct Dst { float x, y, z, rhw; uint32_t c, s; float u, v; };            // 0x1c4
    const Src vertices[4] = {{-1, 0, 0, 0, 1, 0, 0xFFFFFFFF, 0, 0}, {1, 0, 0, 0.5f, 0.5f, -0.5f, 0xFF80FF80, 1, 0},
                             {0, 1.5f, 0, -1, 0.2f, -0.3f, 0xC0FFC080, 0.5f, 1}, {0.2f, -0.5f, -1, 0, 0, -1, 0xFF4060FF, 0.25f, 0.75f}};
    static uint8_t srcStorage[4], dstStorage[4];
    auto vbCtor = Export<VbCtorFn>("??0VertexBuffer_c@@QAE@IIII@Z");
    auto lock = Export<VbLockFn>("?Lock@VertexBuffer_c@@QAEPAXII@Z");
    auto unlock = Export<VbUnlockFn>("?Unlock@VertexBuffer_c@@QAEXXZ");
    void* src = vbCtor(srcStorage, nullptr, 0x152, 0, 2, sizeof(vertices));
    void* dst = vbCtor(dstStorage, nullptr, 0x1c4, 0, 2, 4 * sizeof(Dst));
    std::memcpy(lock(src, nullptr, 0, 0), vertices, sizeof(vertices));
    unlock(src, nullptr);
    Export<ProcessVerticesFn>("?ProcessVertices@render_t@@QAEXPAVVertexBuffer_c@@KKK0KK@Z")(
        render, nullptr, dst, D3DVOP_TRANSFORM | D3DVOP_LIGHT | D3DVOP_CLIP | D3DVOP_EXTENTS, 0, 4, src, 0, 0);
    auto* out = static_cast<const Dst*>(lock(dst, nullptr, 0, 0));
    for (int i = 0; i < 4; ++i)
        std::printf("pv %d: pos %.3f %.3f %.5f rhw %.5f diffuse %08x specular %08x uv %.3f %.3f\n", i, out[i].x, out[i].y,
                    out[i].z, out[i].rhw, out[i].c, out[i].s, out[i].u, out[i].v);
    unlock(dst, nullptr);
    for (DWORD i = 0; i < 3; ++i) device->LightEnable(i, FALSE);
    device->SetRenderState(D3DRENDERSTATE_LIGHTING, FALSE);
}

void* SurfaceOf(void* rtexture) { return *reinterpret_cast<void**>(static_cast<uint8_t*>(rtexture) + 0x30); }

// ---- --texture: the texture creator's path. An LBitmap_t loaded from a BMP in memory through a fake
// fun::PositionIO_t, then TextureStreamCreator::CreateTexture(bitmap, name) - the exported call the game's world
// textures end in. It exercises the creator and its D3DX calls (tools/calllog-ab.sh Device texture). ----
struct FakeStream {
    void** vtable;
    const uint8_t* data;
    size_t size, pos;
};
uint16_t __fastcall FakeStreamReadWord(void* self, void*)
{
    FakeStream* s = static_cast<FakeStream*>(self);
    uint16_t v = 0;
    if (s->pos + 2 <= s->size) std::memcpy(&v, s->data + s->pos, 2);
    s->pos += 2;
    return v;
}
uint32_t __fastcall FakeStreamReadDword(void* self, void*)
{
    FakeStream* s = static_cast<FakeStream*>(self);
    uint32_t v = 0;
    if (s->pos + 4 <= s->size) std::memcpy(&v, s->data + s->pos, 4);
    s->pos += 4;
    return v;
}
void __fastcall FakeStreamRead(void* self, void*, void* buf, int size)
{
    FakeStream* s = static_cast<FakeStream*>(self);
    if (size > 0 && s->pos + size_t(size) <= s->size) std::memcpy(buf, s->data + s->pos, size);
    s->pos += size_t(size);
}
void __fastcall FakeStreamSeek(void* self, void*, int offset, int origin)
{
    FakeStream* s = static_cast<FakeStream*>(self);
    s->pos = origin == 0 ? size_t(offset) : origin == 1 ? s->pos + size_t(offset) : s->size + size_t(offset);
}
uint32_t __fastcall FakeStreamSize(void* self, void*) { return uint32_t(static_cast<FakeStream*>(self)->size); }

// A 10x6 24-bit PNG with a deterministic pattern (built once, tools/ or tests), decoded through LBitmap_t::Load: the
// PNG loader's factory and its stb_image decoder. Only the header (a --png run) and the pixel checksum are printed.
void TestPng()
{
    static const uint8_t png[] = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00,
        0x00, 0x0a, 0x00, 0x00, 0x00, 0x06, 0x08, 0x02, 0x00, 0x00, 0x00, 0x75, 0x92, 0x98, 0x91, 0x00, 0x00, 0x00,
        0xc5, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x01, 0xba, 0x00, 0x45, 0xff, 0x00, 0x00, 0x00, 0x00, 0x25, 0x0d,
        0x07, 0x4a, 0x1a, 0x0e, 0x6f, 0x27, 0x15, 0x94, 0x34, 0x1c, 0xb9, 0x41, 0x23, 0xde, 0x4e, 0x2a, 0x03, 0x5b,
        0x31, 0x28, 0x68, 0x38, 0x4d, 0x75, 0x3f, 0x00, 0x0b, 0x35, 0x61, 0x30, 0x42, 0x68, 0x55, 0x4f, 0x6f, 0x7a,
        0x5c, 0x76, 0x9f, 0x69, 0x7d, 0xc4, 0x76, 0x84, 0xe9, 0x83, 0x8b, 0x0e, 0x90, 0x92, 0x33, 0x9d, 0x99, 0x58,
        0xaa, 0xa0, 0x00, 0x16, 0x6a, 0xc2, 0x3b, 0x77, 0xc9, 0x60, 0x84, 0xd0, 0x85, 0x91, 0xd7, 0xaa, 0x9e, 0xde,
        0xcf, 0xab, 0xe5, 0xf4, 0xb8, 0xec, 0x19, 0xc5, 0xf3, 0x3e, 0xd2, 0xfa, 0x63, 0xdf, 0x01, 0x00, 0x21, 0x9f,
        0x23, 0x46, 0xac, 0x2a, 0x6b, 0xb9, 0x31, 0x90, 0xc6, 0x38, 0xb5, 0xd3, 0x3f, 0xda, 0xe0, 0x46, 0xff, 0xed,
        0x4d, 0x24, 0xfa, 0x54, 0x49, 0x07, 0x5b, 0x6e, 0x14, 0x62, 0x00, 0x2c, 0xd4, 0x84, 0x51, 0xe1, 0x8b, 0x76,
        0xee, 0x92, 0x9b, 0xfb, 0x99, 0xc0, 0x08, 0xa0, 0xe5, 0x15, 0xa7, 0x0a, 0x22, 0xae, 0x2f, 0x2f, 0xb5, 0x54,
        0x3c, 0xbc, 0x79, 0x49, 0xc3, 0x00, 0x37, 0x09, 0xe5, 0x5c, 0x16, 0xec, 0x81, 0x23, 0xf3, 0xa6, 0x30, 0xfa,
        0xcb, 0x3d, 0x01, 0xf0, 0x4a, 0x08, 0x15, 0x57, 0x0f, 0x3a, 0x64, 0x16, 0x5f, 0x71, 0x1d, 0x84, 0x7e, 0x24,
        0x60, 0x3d, 0x4f, 0x75, 0xd2, 0x9e, 0xda, 0x71, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42,
        0x60, 0x82};
    void* streamVt[21] = {};
    streamVt[0] = reinterpret_cast<void*>(&FakeStreamRead);
    streamVt[19] = reinterpret_cast<void*>(&FakeStreamSize);
    FakeStream stream{streamVt, png, sizeof(png), 0};
    using LBitmapLoadFn = void*(__cdecl*)(void*, const char*, int);
    void* bitmap = Export<LBitmapLoadFn>("?Load@LBitmap_t@@SAPAV1@PAVPositionIO_t@fun@@PBDW4CreationFlags_e@1@@Z")(
        &stream, "harness.png", 0);
    if (!bitmap) {
        std::printf("png: no loader\n");
        return;
    }
    uint8_t* b = static_cast<uint8_t*>(bitmap);
    const uint32_t w = *reinterpret_cast<uint32_t*>(b + 0x110), h = *reinterpret_cast<uint32_t*>(b + 0x114);
    const int bpp = *reinterpret_cast<int*>(b + 0x10c);
    const uint8_t* data = *reinterpret_cast<const uint8_t**>(b + 0x118);
    uint32_t hash = 2166136261u;
    for (uint32_t i = 0, bytes = w * h * uint32_t(bpp / 8); i < bytes && data; ++i)
        hash = (hash ^ data[i]) * 16777619u;
    std::printf("png: bitmap %ux%u %d bpp checksum %08x\n", w, h, bpp, hash);
}

// An 8x8 JPEG (PIL, quality 70, optimized tables), decoded through LBitmap_t::Load: the JPEG loader's factory.
void TestJpeg()
{
    static const uint8_t jpg[] = {
        0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 0x4a, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x01,
        0x00, 0x00, 0xff, 0xdb, 0x00, 0x43, 0x00, 0x0a, 0x07, 0x07, 0x08, 0x07, 0x06, 0x0a, 0x08, 0x08, 0x08, 0x0b,
        0x0a, 0x0a, 0x0b, 0x0e, 0x18, 0x10, 0x0e, 0x0d, 0x0d, 0x0e, 0x1d, 0x15, 0x16, 0x11, 0x18, 0x23, 0x1f, 0x25,
        0x24, 0x22, 0x1f, 0x22, 0x21, 0x26, 0x2b, 0x37, 0x2f, 0x26, 0x29, 0x34, 0x29, 0x21, 0x22, 0x30, 0x41, 0x31,
        0x34, 0x39, 0x3b, 0x3e, 0x3e, 0x3e, 0x25, 0x2e, 0x44, 0x49, 0x43, 0x3c, 0x48, 0x37, 0x3d, 0x3e, 0x3b, 0xff,
        0xdb, 0x00, 0x43, 0x01, 0x0a, 0x0b, 0x0b, 0x0e, 0x0d, 0x0e, 0x1c, 0x10, 0x10, 0x1c, 0x3b, 0x28, 0x22, 0x28,
        0x3b, 0x3b, 0x3b, 0x3b, 0x3b, 0x3b, 0x3b, 0x3b, 0x3b, 0x3b, 0x3b, 0x3b, 0x3b, 0x3b, 0x3b, 0x3b, 0x3b, 0x3b,
        0x3b, 0x3b, 0x3b, 0x3b, 0x3b, 0x3b, 0x3b, 0x3b, 0x3b, 0x3b, 0x3b, 0x3b, 0x3b, 0x3b, 0x3b, 0x3b, 0x3b, 0x3b,
        0x3b, 0x3b, 0x3b, 0x3b, 0x3b, 0x3b, 0x3b, 0x3b, 0x3b, 0x3b, 0x3b, 0x3b, 0x3b, 0x3b, 0xff, 0xc0, 0x00, 0x11,
        0x08, 0x00, 0x08, 0x00, 0x08, 0x03, 0x01, 0x22, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x01, 0xff, 0xc4, 0x00,
        0x15, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x04, 0xff, 0xc4, 0x00, 0x1b, 0x10, 0x01, 0x01, 0x00, 0x01, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0x03, 0x05, 0x06, 0x11, 0x61, 0xff, 0xc4, 0x00, 0x15, 0x01,
        0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x04,
        0xff, 0xc4, 0x00, 0x1c, 0x11, 0x00, 0x01, 0x05, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x04, 0x00, 0x01, 0x03, 0x05, 0x11, 0x02, 0x21, 0x22, 0xff, 0xda, 0x00, 0x0c, 0x03, 0x01,
        0x00, 0x02, 0x11, 0x03, 0x11, 0x00, 0x3f, 0x00, 0xbf, 0x64, 0xe1, 0xa0, 0x62, 0xba, 0x7d, 0x07, 0x92, 0x4a,
        0x52, 0x4e, 0x96, 0x0c, 0x6e, 0x39, 0xda, 0x4a, 0x8b, 0x02, 0x1c, 0x66, 0xf4, 0xbf, 0xff, 0xd9};
    void* streamVt[21] = {};
    streamVt[0] = reinterpret_cast<void*>(&FakeStreamRead);
    streamVt[19] = reinterpret_cast<void*>(&FakeStreamSize);
    FakeStream stream{streamVt, jpg, sizeof(jpg), 0};
    using LBitmapLoadFn = void*(__cdecl*)(void*, const char*, int);
    void* bitmap = Export<LBitmapLoadFn>("?Load@LBitmap_t@@SAPAV1@PAVPositionIO_t@fun@@PBDW4CreationFlags_e@1@@Z")(
        &stream, "harness.jpg", 0);
    if (!bitmap) {
        std::printf("jpeg: no loader\n");
        return;
    }
    uint8_t* b = static_cast<uint8_t*>(bitmap);
    const uint32_t w = *reinterpret_cast<uint32_t*>(b + 0x110), h = *reinterpret_cast<uint32_t*>(b + 0x114);
    const int bpp = *reinterpret_cast<int*>(b + 0x10c);
    const uint8_t* data = *reinterpret_cast<const uint8_t**>(b + 0x118);
    uint32_t hash = 2166136261u;
    for (uint32_t i = 0, bytes = w * h * uint32_t(bpp / 8); i < bytes && data; ++i)
        hash = (hash ^ data[i]) * 16777619u;
    std::printf("jpeg: bitmap %ux%u %d bpp checksum %08x\n", w, h, bpp, hash);
}

// CATStdioStatus_t through the exported constructor, its five virtual printers and its deleting destructor: the file
// it writes (the game's own msvcr100 FILE*, so the strings go through that CRT) is checksummed.
std::vector<uint8_t> ReadFile(const std::string& path);
void TestStatus()
{
    HMODULE msvcr = GetModuleHandleA("msvcr100.dll");
    auto fopen_ = reinterpret_cast<void*(__cdecl*)(const char*, const char*)>(GetProcAddress(msvcr, "fopen"));
    auto new_ = reinterpret_cast<void*(__cdecl*)(size_t)>(GetProcAddress(msvcr, "??2@YAPAXI@Z"));
    const char* path = "harness_status.txt";
    void* file = fopen_(path, "wb");
    if (!file) {
        std::printf("status: no file\n");
        return;
    }
    using CtorFn = void*(__fastcall*)(void*, void*, void*, bool);
    void* status = Export<CtorFn>("??0CATStdioStatus_t@@QAE@PAU_iobuf@@_N@Z")(new_(0x10), nullptr, file, true);
    for (int i = 1; i <= 5; ++i)                         // VPrintStatus, Warning1, Warning2, Error, Debug
        reinterpret_cast<void(__fastcall*)(void*, void*, const char*)>(
            (*static_cast<void***>(status))[i])(status, nullptr, "hello");
    reinterpret_cast<void*(__fastcall*)(void*, void*, unsigned)>((*static_cast<void***>(status))[0])(status, nullptr, 1);
    const std::vector<uint8_t> bytes = ReadFile(path);
    uint32_t hash = 2166136261u;
    for (uint8_t b : bytes) hash = (hash ^ b) * 16777619u;
    std::printf("status: file %zu bytes checksum %08x\n", bytes.size(), hash);
}

// A 24-bit BMP (rows bottom-up, each padded to 4 bytes).
std::vector<uint8_t> BuildBmp24(unsigned w, unsigned h, uint32_t (*color)(unsigned, unsigned))
{
    const unsigned rowBytes = w * 3, pad = (4 - (rowBytes % 4)) % 4, rowSize = rowBytes + pad;
    const unsigned imageBytes = rowSize * h;
    std::vector<uint8_t> bmp(54 + imageBytes, 0);
    auto put16 = [&](size_t at, uint16_t v) { std::memcpy(&bmp[at], &v, 2); };
    auto put32 = [&](size_t at, uint32_t v) { std::memcpy(&bmp[at], &v, 4); };
    bmp[0] = 'B', bmp[1] = 'M';
    put32(2, 54 + imageBytes);
    put32(10, 54);
    put32(14, 40), put32(18, w), put32(22, h), put16(26, 1), put16(28, 24), put32(30, 0), put32(34, imageBytes);
    put32(38, 2835), put32(42, 2835), put32(46, 0), put32(50, 0);
    for (unsigned y = 0; y < h; ++y) {
        uint8_t* row = bmp.data() + 54 + size_t(h - 1 - y) * rowSize;   // the file's rows are bottom-up
        for (unsigned x = 0; x < w; ++x) {
            const uint32_t c = color(x, y);                            // 0x00RRGGBB -> the BMP's b g r
            row[x * 3 + 0] = uint8_t(c), row[x * 3 + 1] = uint8_t(c >> 8), row[x * 3 + 2] = uint8_t(c >> 16);
        }
    }
    return bmp;
}

void TestTextureStream()
{
    static const unsigned w = 65, h = 33;                              // not multiples of 4: row padding is exercised
    std::vector<uint8_t> bmp = BuildBmp24(w, h, [](unsigned x, unsigned y) -> uint32_t {
        return ((x / 4) ^ (y / 4)) & 1 ? 0xF0C040u : 0x3060C0u;
    });
    void* streamVt[19] = {};
    streamVt[0] = reinterpret_cast<void*>(&FakeStreamRead);
    streamVt[5] = reinterpret_cast<void*>(&FakeStreamReadDword);
    streamVt[7] = reinterpret_cast<void*>(&FakeStreamReadWord);
    streamVt[18] = reinterpret_cast<void*>(&FakeStreamSeek);
    FakeStream stream{streamVt, bmp.data(), bmp.size(), 0};
    using LBitmapLoadFn = void*(__cdecl*)(void*, const char*, int);
    void* bitmap = Export<LBitmapLoadFn>("?Load@LBitmap_t@@SAPAV1@PAVPositionIO_t@fun@@PBDW4CreationFlags_e@1@@Z")(
        &stream, "harness.bmp", 0);
    std::printf("texture: bitmap %ux%u, %d bpp\n", w, h,
                bitmap ? *reinterpret_cast<int*>(static_cast<uint8_t*>(bitmap) + 0x10c) : 0);
    if (!bitmap) return;
    using TSCCtorBitmapFn = void*(__fastcall*)(void*, void*, void*, const char*, int);
    using TSCreateTextureBitmapFn = void*(__fastcall*)(void*, void*, void*, const char*);
    void* creator = Export<TSCCtorBitmapFn>("??0TextureStreamCreator@@QAE@PAVLBitmap_t@@PBDH@Z")(
        ::operator new(0x40), nullptr, bitmap, "harness_tex", 1);   // 1: the creator's downscale divisor
    void* surface = Export<TSCreateTextureBitmapFn>(
        "?CreateTexture@TextureStreamCreator@@QAEPAVsurface_t@@PAVLBitmap_t@@PBD@Z")(creator, nullptr, bitmap,
                                                                                    "harness_tex");
    (void)creator;
    IDirectDrawSurface7** pp = surface
                                   ? Export<GetSurfacePointerFn>("?GetSurfacePointer@surface_t@@QAEPAPAUIDirectDrawSurface7@@XZ")(
                                         surface, nullptr)
                                   : nullptr;
    if (pp && *pp) {
        DDSURFACEDESC2 sd{};
        sd.dwSize = sizeof(sd);
        if ((*pp)->Lock(nullptr, &sd, DDLOCK_READONLY | DDLOCK_WAIT, nullptr) == DD_OK) {
            uint32_t hash = 2166136261u;
            const uint32_t bytes = sd.dwWidth * (sd.ddpfPixelFormat.dwRGBBitCount / 8);
            for (uint32_t y = 0; y < sd.dwHeight; ++y) {
                const uint8_t* row = static_cast<const uint8_t*>(sd.lpSurface) + size_t(y) * sd.lPitch;
                for (uint32_t i = 0; i < bytes; ++i) hash = (hash ^ row[i]) * 16777619u;
            }
            (*pp)->Unlock(nullptr);
            std::printf("texture: surface %lux%lu %lu bpp checksum %08x\n", sd.dwWidth, sd.dwHeight,
                        sd.ddpfPixelFormat.dwRGBBitCount, hash);
        } else {
            std::printf("texture: surface lock failed\n");
        }
    }
}

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

// ---- --character: an animated character through Randy's own scene graph (RRefFrame_t tree, camera, viewport
// Process + Render), loaded from streams written by tools/extract-character.py ----
struct Vector3 { float x, y, z; };
using MemoryIoCtorFn = void*(__fastcall*)(void* self, void*, const void* data, unsigned size);
using DataCtorFn = void*(__fastcall*)(void* self, void*, void* io);
using MeshCtorFn = void*(__fastcall*)(void* self, void*, void* io, const void* textureNames);
using AnimCtorFn = void*(__fastcall*)(void* self, void*, void* data, unsigned layers);
using FrameCtorFn = void*(__fastcall*)(void* self, void*, void* parent, void* anim);
using CameraCtorFn = void*(__fastcall*)(void* self, void*, float fov, float aspect, float zNear, float zFar,
                                        void* parent, void* anim);
using PtrArgFn = void(__fastcall*)(void* self, void*, void* arg);
using SetTimeFn = void(__fastcall*)(void* self, void*, float time);
using SetPosFn = void(__fastcall*)(void* self, void*, const Vector3* pos, const void* relativeTo);
using SetTargetFn = void(__fastcall*)(void* self, void*, const Vector3* target);
using SetVisibleFn = void(__fastcall*)(void* self, void*, bool visible, bool children);
using SetPriorityFn = void(__fastcall*)(void* self, void*, int list);
using ViewRenderFn = void(__fastcall*)(void* self, void*, int listFrom, int listTo, int type, unsigned from, unsigned to);

std::vector<uint8_t> ReadFile(const std::string& path)
{
    std::vector<uint8_t> data;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        std::fseek(f, 0, SEEK_END);
        data.resize(size_t(std::ftell(f)));
        std::fseek(f, 0, SEEK_SET);
        if (std::fread(data.data(), 1, data.size(), f) != data.size()) data.clear();
        std::fclose(f);
    }
    return data;
}

struct CharacterScene {
    float duration = 0.0f;            // the animation's length (ms): times wrap, as the game wraps them
    void* root = nullptr;
    void* camera = nullptr;
    std::vector<void*> characters;    // RCATMesh_t
    std::vector<void*> anims;         // CATKeyframeAnim_t, one per character
    std::vector<void*> statics;       // --static: the loaded objects (RTriMesh_t ...)
    std::vector<void*> sprites;       // --sprites: RSprite, as DisplaySystem makes them
    std::vector<bool> spriteAnimated;
};

// Builds root -> {camera, sun, `count` characters in a grid}, all with the same model and animation (each its own
// animation instance, at different times). False (with a message) if a stream can't be read.
int g_carriedLights;                                 // --lights N: the first N characters carry a point light
float g_alpha = 1.0f;                                // --alpha A: the characters' transparency
int g_sfx;                                           // --sfx N: their effect type (1 special light, 2 pulse)
float g_terrain;                                      // --terrain H: a heightmap with a ridge H metres high
int g_playfield;
int g_attach;                                         // --attach N: N attractor children a character, half removed
bool g_restore;                                       // --restore: a lost device's restore at frame 2
bool g_fullscreen;                                    // --fullscreen: Randy_t::Initialize in full screen
int g_format = 1;                                     // --format N: its Randy_t::BufferFormat_e
bool g_anyDevice;                                     // --any-device: no Direct3D device asked for
bool g_debugDraw;                                     // --debug-draw: Debugger_t lines, spheres, screen lines, points
int g_resize[2];                                      // --resize W H: the viewport resized (forced) at frame 3
bool g_shutdown;                                      // --shutdown: Randy_t deleted at the end (as the game quits)
bool g_targets;                                       // --targets: every offscreen feature asked for (render targets)
bool g_defaults;                                      // --defaults: the device's default states (FUN_10041ede) at
                                                      // frame 3, once per texture filter caps branch
int g_preprocess;                                     // --preprocess N: PreProcessPlayfield(N) on a 256 x 256 map
int g_mapSize = 64;                                      // --playfield N: the occluder's playfield (meshes register)
bool g_look;                                         // --look X Y Z: where the camera looks instead (some culled)
float g_lookAt[3];
bool g_env;                                          // --env: their materials get an environment map
bool g_shadow;                                       // --shadow: the first one also drawn as a projected shadow
bool g_dynamic;                                      // --dynamic: the 2D scene also draws through DynamicVB_c
float g_blend = -1.0f;                               // --blend F: each animated by a blend (F) of two keyframe animations

bool MakeCharacterScene(const std::string& meshPath, const std::string& animPath, int count, CharacterScene& scene)
{
    HMODULE serialize = LoadLibraryA("serialize.dll");
    if (!serialize) { std::printf("character: no serialize.dll\n"); return false; }
    auto memoryIo = reinterpret_cast<MemoryIoCtorFn>(GetProcAddress(serialize, "??0MemoryIO_t@fun@@QAE@PBXI@Z"));
    static std::vector<uint8_t> meshData, animData;          // MemoryIO_t reads in place
    meshData = ReadFile(meshPath);
    animData = ReadFile(animPath);
    if (!memoryIo || meshData.empty() || animData.empty()) {
        std::printf("character: can't read %s / %s\n", meshPath.c_str(), animPath.c_str());
        return false;
    }
    // CATKeyframeAnimData_t: i32 3, i32 version (low 24 bits: 0x105 = integer ms times, else float), duration.
    if (animData.size() >= 12) {
        uint32_t version;
        std::memcpy(&version, animData.data() + 4, 4);
        if ((version & 0xFFFFFF) < 0x106) {
            int32_t d;
            std::memcpy(&d, animData.data() + 8, 4);
            scene.duration = float(d);
        } else {
            std::memcpy(&scene.duration, animData.data() + 8, 4);
        }
    }
    void* meshIo = memoryIo(::operator new(0x100), nullptr, meshData.data(), unsigned(meshData.size()));
    void* animIo = memoryIo(::operator new(0x100), nullptr, animData.data(), unsigned(animData.size()));
    static const void* noTextures[3] = {};                  // std::vector<std::string>: empty
    void* mesh = Export<MeshCtorFn>("??0CATMesh_t@@QAE@PAVDataIO_t@fun@@ABV?$vector@V?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@V?$allocator@V?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@2@@std@@@Z")(
        ::operator new(0x64), nullptr, meshIo, noTextures);
    {                                                // what the loader made, as one checksum
        const uint8_t* m = static_cast<const uint8_t*>(mesh);
        uint32_t h = 2166136261u;
        auto mix = [&](const void* p, size_t n) {
            for (size_t i = 0; i < n; ++i) h = (h ^ static_cast<const uint8_t*>(p)[i]) * 16777619u;
        };
        auto at = [&](const uint8_t* o, int off) { return *reinterpret_cast<const uint8_t* const*>(o + off); };
        auto num = [&](const uint8_t* o, int off) { return *reinterpret_cast<const int*>(o + off); };
        auto str = [&](const uint8_t* o) {
            const unsigned cap = *reinterpret_cast<const unsigned*>(o + 0x14);
            const char* c = cap < 16 ? reinterpret_cast<const char*>(o) : *reinterpret_cast<const char* const*>(o);
            mix(c, *reinterpret_cast<const unsigned*>(o + 0x10));
        };
        mix(m + 0x2C, 12);
        mix(m + 0x58, 4);
        for (int i = 0; i < num(m, 0x38); ++i)          // the materials' power (+0x60)
            mix(reinterpret_cast<const uint8_t* const*>(at(m, 0x3C))[i] + 0x60, 4);
        mix(at(m, 0x54), size_t(num(m, 0x50)) * 0x14);
        for (int i = 0; i < num(m, 0x40); ++i) {
            const uint8_t* b = at(m, 0x44) + i * 0x28;
            str(b);
            mix(b + 0x1C, 8);
            mix(at(b, 0x24), size_t(num(b, 0x20)) * 4);
        }
        for (int g = 0; g < num(m, 0x48); ++g) {
            const uint8_t* gr = at(m, 0x4C) + g * 0x34;
            str(gr);
            for (int k = 0; k < num(gr, 0x1C); ++k) {
                const uint8_t* pc = at(gr, 0x20) + k * 0x34;
                mix(pc + 0x08, 4);
                mix(pc + 0x10, 4);
                mix(pc + 0x28, 12);
                mix(at(pc, 0x0C), size_t(num(pc, 0x08)) * 0x44);
                mix(at(pc, 0x14), size_t(num(pc, 0x10)) * 2);
                mix(at(pc, 0) + 0x60, 4);                 // its material's power (scaled by the loader)
            }
            mix(at(gr, 0x28), size_t(num(gr, 0x24)) * 0x14);
            for (int k = 0; k < num(gr, 0x2C); ++k) {
                const uint8_t* a2 = at(gr, 0x30) + k * 0x40;
                str(a2);
                mix(a2 + 0x1C, 0x24);
            }
        }
        if (const uint8_t* v = at(m, 0x5C)) mix(v + 8, 0x28);   // its bounding volume
        std::printf("meshdata %08x\n", h);
    }
    void* animSource = Export<DataCtorFn>("??0CATKeyframeAnimData_t@@QAE@PAVDataIO_t@fun@@@Z")(
        ::operator new(0x48), nullptr, animIo);
    {                                                // what the animation loader made, as one checksum
        const uint8_t* d = static_cast<const uint8_t*>(animSource);
        uint32_t h = 2166136261u;
        auto mix = [&](const void* p, size_t n) {
            for (size_t i = 0; i < n; ++i) h = (h ^ static_cast<const uint8_t*>(p)[i]) * 16777619u;
        };
        auto at = [&](const uint8_t* o, int off) { return *reinterpret_cast<const uint8_t* const*>(o + off); };
        auto num = [&](const uint8_t* o, int off) { return *reinterpret_cast<const int*>(o + off); };
        mix(d + 0x2C, 8);
        mix(d + 0x44, 4);
        mix(at(d, 0x40), size_t(num(d, 0x3C)) * 4);
        for (int t = 0; t < num(d, 0x34); ++t) {
            const uint8_t* tr = at(d, 0x38) + t * 0x10;
            mix(at(tr, 4), size_t(num(tr, 0)) * 0x14);
            mix(at(tr, 0xC), size_t(num(tr, 8)) * 0x10);
        }
        std::printf("animdata %08x (%d tracks)\n", h, num(d, 0x34));
    }
    auto frameCtor = Export<FrameCtorFn>("??0RRefFrame_t@@QAE@PAV0@PAVRAnimation_t@@@Z");
    auto setPos = Export<SetPosFn>("?SetRelativePosition@RRefFrame_t@@QAEXABVVector3_t@@PBV1@@Z");
    auto setTarget = Export<SetTargetFn>("?SetWorldTarget@RRefFrame_t@@QAEXABVVector3_t@@@Z");
    scene.root = frameCtor(::operator new(0xA4), nullptr, nullptr, nullptr);
    scene.camera = Export<CameraCtorFn>("??0RCamera_t@@QAE@MMMMPAVRRefFrame_t@@PAVRAnimation_t@@@Z")(
        ::operator new(0x200), nullptr, 0.6f, 640.0f / 480.0f, 0.1f, 200.0f, scene.root, nullptr);

    // Skins (extract-character.py: per material slot, u32 width, height, BGRA rows), shared by the crowd.
    const int materials = *reinterpret_cast<int*>(static_cast<uint8_t*>(mesh) + 0x38);
    const std::string base = meshPath.substr(0, meshPath.rfind('.'));
    std::vector<void*> skins(size_t(std::max(materials, 0)), nullptr);
    for (int i = 0; i < materials; ++i) {
        std::vector<uint8_t> tex = ReadFile(base + ".tex" + std::to_string(i));
        uint32_t w = 0, h = 0;
        if (tex.size() > 8) { std::memcpy(&w, tex.data(), 4); std::memcpy(&h, tex.data() + 4, 4); }
        if (w && h && tex.size() >= 8 + size_t(w) * h * 4)
            skins[size_t(i)] = MakeTexture(("skin" + std::to_string(i)).c_str(), w, h, D3DX_SF_A8R8G8B8, tex.data() + 8,
                                           w * 4);
    }
    using CreateSubstFn = void*(__fastcall*)(void* self, void*, int index);
    auto createSubst = Export<CreateSubstFn>("?CreateSubstMaterial@RCATMesh_t@@QAEPAVRMaterial_t@@H@Z");
    using SetMaterialTextureFn = void(__fastcall*)(void* self, void*, void* texture, unsigned char stage, int flags);
    auto setMaterialTexture = Export<SetMaterialTextureFn>("?SetTexture@RMaterial_t@@QAEXPAVRTexture_t@@EH@Z");
    const int columns = int(std::ceil(std::sqrt(double(count))));
    for (int c = 0; c < count; ++c) {
        void* anim = Export<AnimCtorFn>("??0CATKeyframeAnim_t@@QAE@PBVCATKeyframeAnimData_t@@I@Z")(
            ::operator new(0x60), nullptr, animSource, 0xFFFFFFFFu);
        void* character = Export<FrameCtorFn>("??0RCATMesh_t@@QAE@PAVRRefFrame_t@@@Z")(
            ::operator new(0x438), nullptr, scene.root, nullptr);
        Export<PtrArgFn>("?SetMesh@CATRender_t@@QAEXPBVCATMesh_t@@@Z")(character, nullptr, mesh);
        void* second = nullptr;
        if (g_blend >= 0.0f) {                         // CATAnimBlend_t of this and a second one (its own time), as DisplaySystem
            second = Export<AnimCtorFn>("??0CATKeyframeAnim_t@@QAE@PBVCATKeyframeAnimData_t@@I@Z")(
                ::operator new(0x60), nullptr, animSource, 0xFFFFFFFFu);
            using BlendCtorFn = void*(__fastcall*)(void* self, void*, void* a, void* b, float blend);
            void* blend = Export<BlendCtorFn>("??0CATAnimBlend_t@@QAE@PAVCATAnim_t@@0M@Z")(::operator new(0x70), nullptr,
                                                                                           anim, second, g_blend);
            Export<PtrArgFn>("?SetAnim@CATRender_t@@QAEXPAVCATAnim_t@@@Z")(character, nullptr, blend);
        } else {
            Export<PtrArgFn>("?SetAnim@CATRender_t@@QAEXPAVCATAnim_t@@@Z")(character, nullptr, anim);
        }
        // Each of the mesh's materials gets the character's own copy, as DisplaySystem does (drawing uses those).
        for (int i = 0; i < materials; ++i) {
            void* material = createSubst(character, nullptr, i);
            if (material && skins[size_t(i)]) setMaterialTexture(material, nullptr, skins[size_t(i)], 0, 0);
        }
        void* visual = static_cast<uint8_t*>(character) + 0x3C;  // its RVisual_t
        Export<SetPriorityFn>("?SetRenderPriority@RVisual_t@@QAEXW4RenderList_e@@@Z")(visual, nullptr, 3);
        Export<SetVisibleFn>("?SetVisible@RRefFrame_t@@QAEX_N0@Z")(visual, nullptr, true, true);
        Vector3 at{1.6f * float(c % columns - (columns - 1) / 2.0f), 0.0f, 1.6f * float(c / columns)};
        setPos(visual, nullptr, &at, nullptr);
        if (g_alpha < 1.0f) {                          // RVisual_t +0x88: drawn translucent, colour overrides apply
            using SetTransparencyFn = void(__fastcall*)(void* self, void*, float);
            Export<SetTransparencyFn>("?SetTransparency@RRefFrame_t@@QAEXM@Z")(visual, nullptr, g_alpha);
        }
        if (g_sfx == 1 && c == 0 && skins[0]) {         // the special light (RandyShadowlandsData_s' CAT light)
            Export<void(__cdecl*)(bool)>("?EnableCATLight@RandyShadowlandsData_s@@SAX_N@Z")(true);
            Export<void(__cdecl*)(void*)>("?SetCATLightTexture@RandyShadowlandsData_s@@SAXPAVRTexture_t@@@Z")(skins[0]);
            Export<void(__cdecl*)(float)>("?SetCATLightIntensity@RandyShadowlandsData_s@@SAXM@Z")(0.8f);
            Vector3 down{0.3f, -1.0f, 0.2f};
            Export<void(__cdecl*)(const Vector3*)>("?SetCATLightDirection@RandyShadowlandsData_s@@SAXABVVector3_t@@@Z")(&down);
        }
        if (g_sfx) {
            using SetSfxFn = void(__fastcall*)(void* self, void*, int);
            Export<SetSfxFn>("?SetSfxType@RCATMesh_t@@QAEXW4SfxType_e@1@@Z")(character, nullptr, g_sfx);
        }
        if (g_env) {
            using GetSubstFn = void*(__fastcall*)(void* self, void*, int index);
            using SetEnvFn = void(__fastcall*)(void* self, void*, void* texture);
            auto getSubst = Export<GetSubstFn>("?GetSubstMaterial@RCATMesh_t@@QBEPAVRMaterial_t@@H@Z");
            auto setEnv = Export<SetEnvFn>("?SetEnvTexture@RMaterial_t@@QAEXPAVRTexture_t@@@Z");
            for (int i = 0; i < materials; ++i)
                if (void* m = getSubst(character, nullptr, i))
                    if (skins[size_t(i)]) setEnv(m, nullptr, skins[size_t(i)]);
        }
        if (g_attach) {                              // children by attractor name (a std::map), every other one removed
            using AttachFn = void(__fastcall*)(void* self, void*, const char* name, void* frame);
            auto add = Export<AttachFn>("?AddAttractorChild@RCATMesh_t@@QAEXPBDPAVRRefFrame_t@@@Z");
            auto remove = Export<AttachFn>("?RemoveAttractorChild@RCATMesh_t@@QAEXPBDPBVRRefFrame_t@@@Z");
            static const char* const names[] = {"m", "f", "t", "c", "h", "q", "w", "a", "d", "k", "o", "r", "u", "y",
                                                "b", "e", "g", "i", "l", "n", "p", "s", "v", "x", "z", "j"};
            for (int i = 0; i < g_attach; ++i) {
                void* child = frameCtor(::operator new(0xA4), nullptr, nullptr, nullptr);
                add(character, nullptr, names[i % 26], child);
                add(character, nullptr, names[i % 26], child);   // (already there: ignored)
                if (i % 2) remove(character, nullptr, names[i % 26], child);
            }
        }
        if (c < g_carriedLights) {                   // a point light at head height, carried (RLight_t Type_e 2)
            using LightCtorFn = void*(__fastcall*)(void* self, void*, void* parent, const float* rgb, int type, void* anim);
            const float warm[3] = {1.0f, 0.8f, 0.5f};
            void* light = Export<LightCtorFn>("??0RLight_t@@QAE@PAVRRefFrame_t@@ABVRGB_t@@W4Type_e@0@PAVRAnimation_t@@@Z")(
                ::operator new(0x11C), nullptr, visual, warm, 2, nullptr);
            *reinterpret_cast<float*>(static_cast<uint8_t*>(light) + 0xF0) = 6.0f;     // range
            *reinterpret_cast<float*>(static_cast<uint8_t*>(light) + 0xFC) = 0.2f;     // attenuation 1
            Vector3 head{0.0f, 2.0f, 0.0f};
            setPos(light, nullptr, &head, visual);
        }
        scene.characters.push_back(character);
        scene.anims.push_back(anim);
        if (second) scene.anims.push_back(second);
    }
    // The camera: close for one character, back and up for a crowd.
    float back = count == 1 ? 4.0f : 2.0f + 1.6f * float(columns);
    Vector3 eye{0.0f, count == 1 ? 1.2f : 0.5f * back, -back}, target{0.0f, 1.0f, 0.8f * float(columns / 2)};
    setPos(scene.camera, nullptr, &eye, nullptr);
    if (g_look) target = Vector3{g_lookAt[0], g_lookAt[1], g_lookAt[2]};
    setTarget(scene.camera, nullptr, &target);
    // A white sun from above and in front (RLight_t::Type_e 1 = directional).
    using LightCtorFn = void*(__fastcall*)(void* self, void*, void* parent, const float* rgb, int type, void* anim);
    const float white[3] = {1.0f, 0.95f, 0.85f};
    void* sun = Export<LightCtorFn>("??0RLight_t@@QAE@PAVRRefFrame_t@@ABVRGB_t@@W4Type_e@0@PAVRAnimation_t@@@Z")(
        ::operator new(0x11C), nullptr, scene.root, white, 1, nullptr);
    Vector3 sunPos{3.0f, 6.0f, -5.0f}, origin{0.0f, 0.0f, 0.0f};
    setPos(sun, nullptr, &sunPos, nullptr);
    setTarget(sun, nullptr, &origin);
    if (g_terrain > 0.0f) {
        // The occluder's heightmap starts at the world's origin: the scene moved onto it (100, 0, 100), a ridge across
        // the crowd's front (64 x 64 cells of 4 m, heights in centimetres), occlusion culling on (its default).
        Vector3 offset{100.0f, 0.0f, 100.0f};
        setPos(scene.root, nullptr, &offset, nullptr);
        void* occ = Export<void*(__cdecl*)()>("?Get@HMOccluder_t@@SAAAV1@XZ")();
        if (g_preprocess) g_mapSize = 256;
        const int n = g_mapSize;
        Export<void(__fastcall*)(void*, void*, int, int)>("?SetHeightmapSize@HMOccluder_t@@QAEXHH@Z")(occ, nullptr, n, n);
        Export<void(__fastcall*)(void*, void*, float, float, float)>("?SetHeightmapScale@HMOccluder_t@@QAEXMMM@Z")(
            occ, nullptr, 4.0f, 0.0f, 0.01f);
        static std::vector<uint16_t> heights;
        heights.assign(size_t(n) * size_t(n), 0);
        const int row = 25;                          // z 100..104: along the crowd's front row
        for (int x = 0; x < n; ++x) heights[size_t(row) * size_t(n) + size_t(x)] = uint16_t(g_terrain * 100.0f);
        Export<void(__fastcall*)(void*, void*, const uint16_t*, int, int, int)>(
            "?SetHeightmapPatch@HMOccluder_t@@QAEXPBGHHH@Z")(occ, nullptr, heights.data(), 0, 0, n - 1);
        if (g_playfield) *reinterpret_cast<int*>(static_cast<uint8_t*>(occ) + 0x94) = g_playfield;
        if (g_preprocess) {                          // its caches in C:\linux\testclient\occtest\OCC
            Export<void(__fastcall*)(void*, void*, const char*)>("?SetDataPath@HMOccluder_t@@QAEXPBD@Z")(
                occ, nullptr, "C:\\linux\\testclient\\occtest");
            Export<void(__fastcall*)(void*, void*, int)>("?PreProcessPlayfield@HMOccluder_t@@QAEXH@Z")(occ, nullptr,
                                                                                                    g_preprocess);
        }
        std::printf("terrain: ridge %.1f m at row %d\n", g_terrain, row);
    }
    std::printf("character: mesh %p (%d materials), %d characters, animation %.0f ms\n", mesh, materials, count,
                scene.duration);
    return true;
}

// ---- --static: a static mesh (.abiff's ObjectArchive, tools/extract-static.py) loaded with serialize.dll the way
// DisplaySystem loads them, `count` copies in a grid under the scene's root ----
// --sprites N: RSprites as DisplaySystem makes them - every mode (0..4), with and without a grid of animation frames,
// plain and additive (both kinds), coloured; one in four duplicated (vtable slot 3) as well.
int g_sprites;
bool g_texture;                                       // --texture: the texture creator's path
bool g_png;                                           // --png: LBitmap_t::Load of a PNG
bool g_jpeg;                                          // --jpeg: LBitmap_t::Load of a JPEG
bool g_status;                                        // --status: CATStdioStatus_t's printers
bool g_lightmap;                                      // --lightmap: the statics' lightmap colours
void AddSprites(CharacterScene& scene, int count)
{
    std::vector<uint32_t> px(32 * 32);
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) px[y * 32 + x] = (((x / 4) ^ (y / 4)) & 1 ? 0xFFFFA020u : 0x8020A0FFu);
    void* texture = MakeTexture("sprite_grid", 32, 32, D3DX_SF_A8R8G8B8, px.data(), 32 * 4);
    using MaterialFn = void*(__fastcall*)(void*, void*, const char*, void*, const float*, const float*, const float*,
                                          const float*, float, float, float, bool, bool);
    const float white[3] = {1, 1, 1}, black[3] = {0, 0, 0};
    void* material = Export<MaterialFn>("??0RMaterial_t@@QAE@PBDPAVRTexture_t@@ABVRGB_t@@222MMM_N3@Z")(
        ::operator new(0xC0), nullptr, "sprite", texture, white, black, white, black, 1.0f, 0.5f, 1.0f, false, false);
    using SpriteFn = void*(__fastcall*)(void*, void*, void*, void*, float, float, void*, int);
    auto ctor = Export<SpriteFn>("??0RSprite@@QAE@PBVRMaterial_t@@PBVRSpriteAnim@@MMPAVRRefFrame_t@@W4SpriteMode_e@0@@Z");
    auto additive = Export<void(__fastcall*)(void*, void*, bool, int)>("?EnableAdditiveRendering@RSprite@@QAEX_NW4SpriteRenderMode_e@1@@Z");
    auto initColor = Export<void(__fastcall*)(void*, void*)>("?InitVertexColor@RSprite@@IAEXXZ");
    auto setPos = Export<SetPosFn>("?SetRelativePosition@RRefFrame_t@@QAEXABVVector3_t@@PBV1@@Z");
    auto addChild = Export<void(__fastcall*)(void*, void*, void*)>("?AddChild@RRefFrame_t@@UAEXPAV1@@Z");
    auto msvcrNew = reinterpret_cast<void*(__cdecl*)(size_t)>(GetProcAddress(GetModuleHandleA("msvcr100.dll"), "??2@YAPAXI@Z"));
    auto serializable = reinterpret_cast<void*(__fastcall*)(void*, void*)>(
        GetProcAddress(GetModuleHandleA("serialize.dll"), "??0Serializable_c@fun@@QAE@XZ"));
    uint8_t* orig = reinterpret_cast<uint8_t*>(GetModuleHandleA("randy31_orig.dll"));
    for (int i = 0; i < count; ++i) {
        void* anim = nullptr;
        if (i % 2) {                                 // an RSpriteAnim as DisplaySystem fills one: 4 x 3 frames
            auto* a = static_cast<uint8_t*>(msvcrNew(0x30));
            serializable(a, nullptr);
            *reinterpret_cast<void**>(a) = orig + 0x8A624;
            const float rect[4] = {0.0f, 0.0f, 0.25f, 0.25f}, step[2] = {0.25f, 0.33333334f};
            std::memcpy(a + 8, rect, 16);
            std::memcpy(a + 0x18, step, 8);
            *reinterpret_cast<uint32_t*>(a + 0x20) = 4;
            *reinterpret_cast<uint32_t*>(a + 0x24) = 10 + unsigned(i % 3);
            *reinterpret_cast<float*>(a + 0x28) = 37.0f + float(i);
            a[0x2C] = uint8_t((i / 2) % 2);
            anim = a;
        }
        const int mode = i % 5;
        void* sprite = ctor(::operator new(0x294), nullptr, material, anim, 0.6f + 0.1f * float(i % 4),
                            0.4f + 0.15f * float(i % 3), scene.root, mode);
        *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(sprite) + 0x18C) = 0xFF000000u | (0x3355AAu * unsigned(i + 1));
        initColor(sprite, nullptr);
        if (i % 3 == 1) additive(sprite, nullptr, true, 1);
        if (i % 3 == 2) additive(sprite, nullptr, true, 2);
        if (i % 6 == 4) additive(sprite, nullptr, false, 1);   // on and off again
        Vector3 at{-2.0f + 0.9f * float(i % 5), 0.6f + 0.5f * float(i / 5), 1.5f + 0.3f * float(i % 3)};
        setPos(sprite, nullptr, &at, nullptr);
        scene.sprites.push_back(sprite);
        scene.spriteAnimated.push_back(anim != nullptr);
        if (i % 4 == 3 && anim) {                    // a copy (vtable slot 3), placed nearby
            void* copy = reinterpret_cast<void*(__fastcall*)(void*, void*)>((*static_cast<void***>(sprite))[3])(sprite, nullptr);
            addChild(scene.root, nullptr, copy);
            // (the copy's colour is left unset - whatever its memory held - so it gets one here)
            *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(copy) + 0x18C) = 0xFF80FF80u;
            initColor(copy, nullptr);
            Vector3 beside{at.x + 0.3f, at.y - 0.2f, at.z};
            setPos(copy, nullptr, &beside, nullptr);
            scene.sprites.push_back(copy);
            scene.spriteAnimated.push_back(true);
        }
    }
    std::printf("sprites: %zu\n", scene.sprites.size());
}

void AnimateSprites(CharacterScene& scene, float time, int frame)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < scene.sprites.size(); ++i) {
        uint8_t* s = static_cast<uint8_t*>(scene.sprites[i]);
        if (scene.spriteAnimated[i])                 // vtable slot 21: the animation at a time
            reinterpret_cast<void(__fastcall*)(void*, void*, float)>((*reinterpret_cast<void***>(s))[21])(s, nullptr,
                                                                                                     time + 13.0f * float(i));
        for (uint32_t k = 0x178; k < 0x200; ++k) sum = sum * 31 + s[k];   // sizes, colour, rectangle, vertices
        for (uint32_t k = 0x208; k < 0x20D; ++k) sum = sum * 31 + s[k];   // mode, additive (not the pointers)
        for (uint32_t k = 0x15C; k < 0x160; ++k) sum = sum * 31 + s[k];   // render priority
    }
    if (frame == 3) std::printf("sprite state %08x (%zu sprites)\n", sum, scene.sprites.size());
}

bool AddStatics(const std::string& path, int count, CharacterScene& scene)
{
    HMODULE serialize = LoadLibraryA("serialize.dll");
    using ArchiveCtorFn = void*(__fastcall*)(void* self, void*, void* io, bool read);
    using FindFn = int(__fastcall*)(void* self, void*, long* id, const char* name, int);
    using ReadFn = void*(__fastcall*)(void* self, void*, long id);
    auto memoryIo = reinterpret_cast<MemoryIoCtorFn>(GetProcAddress(serialize, "??0MemoryIO_t@fun@@QAE@PBXI@Z"));
    auto archiveCtor = reinterpret_cast<ArchiveCtorFn>(GetProcAddress(serialize, "??0ObjectArchive_c@fun@@QAE@PAVIO_t@1@_N@Z"));
    auto find = reinterpret_cast<FindFn>(
        GetProcAddress(serialize, "?FindObjectID@ObjectArchive_c@fun@@QAE?AW4MsgErr_e@Message_c@2@AAJPBDH@Z"));
    auto read = reinterpret_cast<ReadFn>(GetProcAddress(serialize, "?ReadObject@ObjectArchive_c@fun@@QAEPAVSerializable_c@2@J@Z"));
    static std::vector<uint8_t> data;
    data = ReadFile(path);
    if (!memoryIo || !archiveCtor || !find || !read || data.empty()) {
        std::printf("static: can't read %s\n", path.c_str());
        return false;
    }
    using AddChildFn = void(__fastcall*)(void* self, void*, void* child);
    auto addChild = Export<AddChildFn>("?AddChild@RRefFrame_t@@UAEXPAV1@@Z");
    auto setPos = Export<SetPosFn>("?SetRelativePosition@RRefFrame_t@@QAEXABVVector3_t@@PBV1@@Z");
    const int columns = int(std::ceil(std::sqrt(double(count))));
    int loaded = 0;
    for (int i = 0; i < count; ++i) {
        void* io = memoryIo(::operator new(0x100), nullptr, data.data(), unsigned(data.size()));
        void* archive = archiveCtor(::operator new(0x400), nullptr, io, true);
        long id = -1;
        if (find(archive, nullptr, &id, "obj", 0) != 0) {
            std::printf("static: no \"obj\" in the archive\n");
            return false;
        }
        void* object = read(archive, nullptr, id);
        if (!object) {
            std::printf("static: ReadObject failed\n");
            return false;
        }
        addChild(scene.root, nullptr, object);
        scene.statics.push_back(object);
        Vector3 at{12.0f * float(i % columns - (columns - 1) / 2.0f), 0.0f, 12.0f * float(i / columns) + 20.0f};
        setPos(object, nullptr, &at, nullptr);
        ++loaded;
    }
    std::printf("static: %d copies of %s\n", loaded, path.c_str());
    return true;
}

// Picking: rays from the camera through a few heights of the first character (RCATMesh_t::IsLineIntersecting, what
// mouse-over uses), printed - the same answers with and without native skinning.
void PickCharacter(CharacterScene& scene)
{
    using IsLineFn = bool(__fastcall*)(void* self, void*, const Vector3* from, const Vector3* to, float* at, bool flag);
    auto isLine = Export<IsLineFn>("?IsLineIntersecting@RCATMesh_t@@QBE_NABVVector3_t@@0PAM_N@Z");
    using WorldFn = const float*(__fastcall*)(const void* self, void*);
    auto world = Export<WorldFn>("?GetWorldMatrix@RRefFrame_t@@QBEABVTMatrix4_t@@XZ");
    const float* c = world(scene.camera, nullptr);
    const float* m = world(static_cast<uint8_t*>(scene.characters[0]) + 0x3C, nullptr);
    Vector3 from{c[12], c[13], c[14]};
    std::printf("pick:");
    for (float h : {0.3f, 0.8f, 1.2f, 1.6f, 2.0f, 2.6f}) {
        for (float dx : {-0.25f, 0.0f, 0.25f}) {
            Vector3 to{m[12] + dx, m[13] + h, m[14]};
            Vector3 end{from.x + (to.x - from.x) * 3.0f, from.y + (to.y - from.y) * 3.0f, from.z + (to.z - from.z) * 3.0f};
            float at = -1.0f;
            bool hit = isLine(scene.characters[0], nullptr, &from, &end, &at, false);
            std::printf(" %s", hit ? "X" : ".");
            if (hit) std::printf("%.3f", at);
        }
    }
    std::printf("\n");
}

// The vertices of an RTriMesh_t's data (its RTriMeshData_t's SimpleMeshes), the count ConvertToLightmap consumes.
size_t StaticVertexCount(void* object)
{
    auto* data = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(object) + 0x184);
    if (!data) return 0;
    auto* first = *reinterpret_cast<uint8_t**>(data + 0x4C);
    auto* last = *reinterpret_cast<uint8_t**>(data + 0x50);
    size_t n = 0;
    for (uint8_t* at = first; at != last; at += sizeof(void*)) {
        auto* mesh = *reinterpret_cast<uint8_t**>(at);
        auto* triangles = *reinterpret_cast<uint8_t**>(mesh + 0x20);
        n += *reinterpret_cast<uint32_t*>(triangles + 0x34);
    }
    return n;
}

// The RTriMesh_t objects under the statics (each --static object is an RRefFrame_t holding its mesh and its light).
void CollectTriMeshes(void* frame, std::vector<void*>& out)
{
    using DynamicCastFn = void*(__cdecl*)(void*, long, void*, void*, int);
    static const auto cast =
        reinterpret_cast<DynamicCastFn>(GetProcAddress(GetModuleHandleA("msvcr100.dll"), "__RTDynamicCast"));
    static uint8_t* orig = reinterpret_cast<uint8_t*>(GetModuleHandleA("randy31_orig.dll"));
    for (void* c = *reinterpret_cast<void**>(static_cast<uint8_t*>(frame) + 0x1C); c;
         c = *reinterpret_cast<void**>(static_cast<uint8_t*>(c) + 0x18)) {
        if (cast && cast(c, 0, orig + 0xB60D4, orig + 0xB78EC, 0)) out.push_back(c);   // Serializable_c -> RTriMesh_t
        CollectTriMeshes(c, out);
    }
}

// --lightmap: the statics' lightmap colours, as the game applies them (RTriMesh_t::ConvertToLightmap).
void ApplyLightmap(CharacterScene& scene, int frame)
{
    using ConvertFn = void(__fastcall*)(void* self, void*, const uint16_t* colours);
    auto convert = Export<ConvertFn>("?ConvertToLightmap@RTriMesh_t@@QAEXPBG@Z");
    std::vector<void*> meshes;
    for (void* s : scene.statics) CollectTriMeshes(s, meshes);
    size_t total = 0;
    for (void* m : meshes) total += StaticVertexCount(m);
    static std::vector<uint16_t> colours;
    if (colours.size() < total) {
        colours.resize(total);
        for (size_t i = 0; i < colours.size(); ++i) colours[i] = uint16_t((i * 2654435761u) >> 16);
    }
    for (void* m : meshes) convert(m, nullptr, colours.data());
    if (frame == 3) std::printf("lightmap: %zu meshes, %zu colours\n", meshes.size(), colours.size());
}

// --static ... --pick: rays from the camera at the statics (RTriMesh_t::IsRayIntersecting, over
// SimpleMesh::IsRayIntersecting's triangles).
void PickStatics(CharacterScene& scene)
{
    using IsRayFn = bool(__fastcall*)(void* self, void*, const Vector3* from, const Vector3* along, float* at, bool nearest);
    auto isRay = Export<IsRayFn>("?IsRayIntersecting@RTriMesh_t@@UBE_NABVVector3_t@@0PAM_N@Z");
    using WorldFn = const float*(__fastcall*)(const void* self, void*);
    auto world = Export<WorldFn>("?GetWorldMatrix@RRefFrame_t@@QBEABVTMatrix4_t@@XZ");
    std::vector<void*> meshes;
    for (void* s : scene.statics) CollectTriMeshes(s, meshes);
    const float* c = world(scene.camera, nullptr);
    Vector3 from{c[12], c[13], c[14]};
    std::printf("static pick:");
    for (void* m : meshes) {
        const float* w = world(m, nullptr);
        for (float h : {0.0f, 1.5f, 4.0f}) {
            Vector3 to{w[12], w[13] + h, w[14]};
            Vector3 along{to.x - from.x, to.y - from.y, to.z - from.z};
            float at = -1.0f;
            bool hit = isRay(m, nullptr, &from, &along, &at, false);
            std::printf(" %s", hit ? "X" : ".");
            if (hit) std::printf("%.3f", at);
        }
    }
    std::printf("\n");
}

// --connector: an RRefFrameConnector (the attachment point a frame hands out) - its originator, its originator's
// world matrix (the same ones the frame gives), and its deletion.
bool g_connector;
void* g_connectorObject;
void TestConnector(CharacterScene& scene, int frame)
{
    if (!g_connectorObject) {
        using CtorFn = void*(__fastcall*)(void*, void*, const char*, void*);
        auto ctor = Export<CtorFn>("??0RRefFrameConnector@@QAE@PBDPAVRRefFrame_t@@@Z");
        auto msvcrNew =
            reinterpret_cast<void*(__cdecl*)(size_t)>(GetProcAddress(GetModuleHandleA("msvcr100.dll"), "??2@YAPAXI@Z"));
        g_connectorObject = ctor(msvcrNew(0x30), nullptr, "harness connector", scene.camera);
    }
    using WorldFn2 = const float*(__fastcall*)(void*, void*);
    auto world = Export<WorldFn2>("?GetWorldMatrix@RRefFrame_t@@QBEABVTMatrix4_t@@XZ");
    void* originator = reinterpret_cast<void*(__fastcall*)(void*, void*)>(
        (*reinterpret_cast<void***>(g_connectorObject))[4])(g_connectorObject, nullptr);
    const float* m = reinterpret_cast<const float*(__fastcall*)(void*, void*)>(
        (*reinterpret_cast<void***>(g_connectorObject))[3])(g_connectorObject, nullptr);
    if (frame == 3) {
        const float* expected = world(scene.camera, nullptr);
        uint32_t sum = 0;
        for (int i = 0; i < 16; ++i) {
            uint32_t bits;
            std::memcpy(&bits, &m[i], 4);
            sum = sum * 31 + bits;
        }
        std::printf("connector: originator %s, matrix %08x\n",
                    originator == scene.camera && m == expected ? "camera" : "other", sum);
    }
}

// --grid: an RGrid (the reference grid) built by its exported constructor, attached to the scene, coloured and drawn
// (render_t::RenderLineList) - its geometry, SetColor and draw are the same with and without the native grid.
bool g_grid;
void* g_gridObject;
void TestGrid(CharacterScene& scene, void* viewport, int frame)
{
    using CtorFn = void*(__fastcall*)(void*, void*, unsigned, unsigned, float, float, void*, void*);
    using AddChildFn = void(__fastcall*)(void*, void*, void*);
    using SetColorFn = void(__fastcall*)(void*, void*, const void*);
    using RenderFn = void(__fastcall*)(void*, void*, void*);
    if (!g_gridObject) {
        auto ctor = Export<CtorFn>("??0RGrid@@QAE@IIMMPAVRRefFrame_t@@PAVRAnimation_t@@@Z");
        auto addChild = Export<AddChildFn>("?AddChild@RRefFrame_t@@UAEXPAV1@@Z");
        auto setPos = Export<SetPosFn>("?SetRelativePosition@RRefFrame_t@@QAEXABVVector3_t@@PBV1@@Z");
        void* object = ctor(::operator new(0x19C), nullptr, 4, 3, 2.0f, 2.5f, nullptr, nullptr);
        addChild(scene.root, nullptr, object);
        Vector3 at{0.0f, 0.5f, 4.0f};
        setPos(object, nullptr, &at, nullptr);
        g_gridObject = object;
    }
    if (frame == 1) {
        const float colour[3] = {0.125f, 0.75f, 1.0f};
        Export<SetColorFn>("?SetColor@RGrid@@QAEXABVRGB_t@@@Z")(g_gridObject, nullptr, colour);
    }
    if (frame == 3) {
        auto* g = static_cast<const uint8_t*>(g_gridObject);
        const uint32_t lines = *reinterpret_cast<const uint32_t*>(g + 0x188);
        const uint32_t indices = *reinterpret_cast<const uint32_t*>(g + 0x18C);
        const uint8_t* vertices = *reinterpret_cast<const uint8_t* const*>(g + 0x194);
        const uint8_t* indexData = *reinterpret_cast<const uint8_t* const*>(g + 0x190);
        const uint8_t* bounds = *reinterpret_cast<const uint8_t* const*>(g + 0x198);
        uint32_t sum = 2166136261u;
        for (uint32_t i = 0; i < lines * 0x10; ++i) sum = (sum ^ vertices[i]) * 16777619u;
        for (uint32_t i = 0; i < indices * 4; ++i) sum = (sum ^ indexData[i]) * 16777619u;
        const float* centre = reinterpret_cast<const float*>(bounds + 0x08);
        const float radius = *reinterpret_cast<const float*>(bounds + 0x14);
        std::printf("grid: %u lines, %u indices, checksum %08x, bounds %.5g %.5g %.5g r %.5g\n", lines, indices, sum,
                    centre[0], centre[1], centre[2], radius);
    }
    reinterpret_cast<RenderFn>((*reinterpret_cast<void***>(g_gridObject))[13])(g_gridObject, nullptr, viewport);
}

// The first character's projected shadow (RVisual_t vtable slot 20, as RShadow draws its caster): a fake RShadow
// with its matrix (+0x1AC, flattening onto y = 0.01; +0x1EC 0 = up to date) and material (+0x178).
void DrawShadow(CharacterScene& scene, void* viewport)
{
    alignas(16) static uint8_t shadow[0x200];
    float m[16] = {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0.01f, 0, 1};
    std::memcpy(shadow + 0x1AC, m, sizeof(m));
    void* catMesh = *reinterpret_cast<void**>(static_cast<uint8_t*>(scene.characters[0]) + 4);
    *reinterpret_cast<void**>(shadow + 0x178) = (*reinterpret_cast<void***>(static_cast<uint8_t*>(catMesh) + 0x3C))[0];
    uint8_t* visual = static_cast<uint8_t*>(scene.characters[0]) + 0x3C;
    using ShadowFn = void(__fastcall*)(void* self, void*, void* viewport, void* shadow);
    reinterpret_cast<ShadowFn>((*reinterpret_cast<void***>(visual))[20])(visual, nullptr, viewport, shadow);
}

// What the game asks the first character (attractors, bones, materials, its sphere), printed: the same with and
// without CatQuery.
void QueryCharacter(CharacterScene& scene)
{
    uint8_t* mesh = static_cast<uint8_t*>(scene.characters[0]);
    uint8_t* catMesh = *reinterpret_cast<uint8_t**>(mesh + 4);
    using LookupFn = bool(__fastcall*)(void* self, void*, const char* name, float* out);
    auto attractor = Export<LookupFn>("?GetAttractor@RCATMesh_t@@QAE_NPBDAAVTMatrix4_t@@@Z");
    auto bone = Export<LookupFn>("?GetBoneMatrix@RCATMesh_t@@QAE_NPBDAAVTMatrix4_t@@@Z");
    auto has = Export<bool(__fastcall*)(void*, void*, const char*)>("?HasAttractor@CATRender_t@@QBE_NPBD@Z");
    auto materialIndex = Export<int(__fastcall*)(void*, void*, const char*)>("?GetMaterialIndex@RCATMesh_t@@QBEHPBD@Z");
    auto name = Export<const char*(__fastcall*)(void*, void*)>("?GetName@RResource_t@@QBEPBDXZ");
    auto radius = Export<float(__fastcall*)(void*, void*)>("?GetBoundingSphereRadius@RCATMesh_t@@QBEMXZ");
    auto center = Export<Vector3*(__fastcall*)(void*, void*, Vector3*)>("?GetBoundingSpherePos@RCATMesh_t@@QBE?AVVector3_t@@XZ");
    auto print = [](const char* what, const char* n, bool ok, const float* m) {
        std::printf("query %s '%s': %d", what, n, ok ? 1 : 0);
        if (ok) for (int i = 0; i < 16; ++i) std::printf(" %.5g", m[i]);
        std::printf("\n");
    };
    int groups = *reinterpret_cast<int*>(mesh + 0x0C);
    auto* renderGroups = *reinterpret_cast<uint8_t**>(mesh + 0x10);
    for (int g = 0; g < groups; ++g) {
        uint8_t* groupMesh = *reinterpret_cast<uint8_t**>(renderGroups + g * 0xC);
        uint8_t* record = *reinterpret_cast<uint8_t**>(groupMesh + 0x4C) + g * 0x34;
        int count = *reinterpret_cast<int*>(record + 0x2C);
        for (int i = 0; i < count; ++i) {
            auto* str = reinterpret_cast<Vc10String*>(*reinterpret_cast<uint8_t**>(record + 0x30) + i * 0x40);
            float out[16] = {};
            bool ok = attractor(mesh, nullptr, str->c_str(), out);
            print("attractor", str->c_str(), ok && has(mesh, nullptr, str->c_str()), out);
        }
    }
    float out[16] = {};
    print("attractor", "no such", attractor(mesh, nullptr, "no such", out), out);
    int bones = *reinterpret_cast<int*>(catMesh + 0x40);
    for (int b = 0; b < bones; b += 3) {
        auto* str = reinterpret_cast<Vc10String*>(*reinterpret_cast<uint8_t**>(catMesh + 0x44) + b * 0x28);
        bool ok = bone(mesh, nullptr, str->c_str(), out);
        print("bone", str->c_str(), ok, out);
    }
    int materials = *reinterpret_cast<int*>(catMesh + 0x38);
    for (int i = 0; i < materials; ++i) {
        const char* n = name((*reinterpret_cast<void***>(catMesh + 0x3C))[i], nullptr);
        std::printf("query material '%s': %d\n", n ? n : "(null)", n ? materialIndex(mesh, nullptr, n) : -2);
    }
    Vector3 c{};
    center(mesh, nullptr, &c);
    std::printf("query sphere: %.5g at %.5g %.5g %.5g\n", radius(mesh, nullptr), c.x, c.y, c.z);
}

// Materials through the exported API, every combination of name (plain, "[n]" blend modes, "op1_"), texture (with
// alpha, without, none), opacity, two-sided and alpha-as-transparency, then changed (texture swapped and removed,
// opacity, two-sided) and copied; each state printed - flags, D3D material, the delta state's lists - so the original
// and the native materials can be compared line by line (tools/calllog-ab.sh).
bool g_materials;

void DumpMaterial(const char* what, void* m, void* alpha, void* opaque)
{
    using BoolFn = bool(__fastcall*)(void*, void*);
    using TexFn = void*(__fastcall*)(void*, void*, unsigned char);
    using InitFn = void(__fastcall*)(void*, void*, float*);
    auto* b = static_cast<uint8_t*>(m);
    auto texName = [&](void* t) { return t == alpha ? "alpha" : t == opaque ? "opaque" : t ? "other" : "-"; };
    std::printf("material %s: transparent %d two-sided %d alpha-transparency %d op1 %d tex0 %s tex1 %s", what,
                Export<BoolFn>("?IsTransparent@RMaterial_t@@QBE_NXZ")(m, nullptr),
                Export<BoolFn>("?IsTwoSided@RMaterial_t@@QBE_NXZ")(m, nullptr), b[0x74], b[0xBC],
                texName(Export<TexFn>("?GetTexture@RMaterial_t@@QBEPAVRTexture_t@@E@Z")(m, nullptr, 0)),
                texName(Export<TexFn>("?GetTexture@RMaterial_t@@QBEPAVRTexture_t@@E@Z")(m, nullptr, 1)));
    float d3d[17] = {};
    Export<InitFn>("?InitD3DMaterial@RMaterial_t@@QBEXAAU_D3DMATERIAL7@@@Z")(m, nullptr, d3d);
    std::printf(" d3d");
    for (float f : d3d) std::printf(" %g", f);
    uint8_t* delta = *reinterpret_cast<uint8_t**>(b + 0x70);
    if (delta) {
        struct Node { Node* next; Node* prev; uint32_t type, value, extra; };
        std::printf(" | delta textures %u stages %u highest %u rs", *reinterpret_cast<uint32_t*>(delta + 0x6C),
                    *reinterpret_cast<uint32_t*>(delta + 0xDC), *reinterpret_cast<uint32_t*>(delta + 0xE0));
        Node* head = *reinterpret_cast<Node**>(delta + 0x70);
        for (Node* n = head->next; n != head; n = n->next) std::printf(" %u=%u", n->type, n->value);
        for (int st = 0; st < 8; ++st) {
            Node* h = *reinterpret_cast<Node**>(delta + 0x7C + st * 0xC);
            if (h->next == h) continue;
            std::printf(" tss%d", st);
            for (Node* n = h->next; n != h; n = n->next) std::printf(" %u=%u", n->type, n->value);
        }
        for (int st = 0; st < 8; ++st)
            if (void* t = *reinterpret_cast<void**>(delta + 0x2C + st * 8)) std::printf(" t%d=%s", st, texName(t));
    }
    std::printf("\n");
}

void TestMaterials()
{
    std::vector<uint32_t> px(64, 0x80FF8040);
    void* alpha = MakeTexture("mt_alpha", 8, 8, D3DX_SF_A8R8G8B8, px.data(), 32);
    void* opaque = MakeTexture("mt_opaque", 8, 8, D3DX_SF_X8R8G8B8, px.data(), 32);
    using CtorFn = void*(__fastcall*)(void*, void*, const char*, void*, const float*, const float*, const float*,
                                      const float*, float, float, float, bool, bool);
    using CopyFn = void*(__fastcall*)(void*, void*, void*);
    using SetTexFn = void(__fastcall*)(void*, void*, void*, unsigned char, int);
    using SetFloatFn = void(__fastcall*)(void*, void*, float);
    using SetBoolFn = void(__fastcall*)(void*, void*, bool);
    auto ctor = Export<CtorFn>("??0RMaterial_t@@QAE@PBDPAVRTexture_t@@ABVRGB_t@@222MMM_N3@Z");
    auto copy = Export<CopyFn>("??0RMaterial_t@@QAE@ABV0@@Z");
    auto setTex = Export<SetTexFn>("?SetTexture@RMaterial_t@@QAEXPAVRTexture_t@@EH@Z");
    auto setOpacity = Export<SetFloatFn>("?SetOpacity@RMaterial_t@@QAEXM@Z");
    auto setTwoSided = Export<SetBoolFn>("?SetTwoSided@RMaterial_t@@QAEX_N@Z");
    const float diffuse[3] = {0.8f, 0.6f, 0.4f}, specular[3] = {0.5f, 0.5f, 0.5f}, ambient[3] = {0.2f, 0.2f, 0.2f},
                emissive[3] = {0, 0, 0}, black[3] = {0, 0, 0};
    const char* names[] = {"plain", "[0]m", "[1]m", "[2]m", "[3]m", "[4]m", "[5]m", "[9]m", "[x]m", "op1_m"};
    void* textures[] = {alpha, opaque, nullptr};
    char what[96];
    for (const char* name : names)
        for (void* tex : textures)
            for (float opacity : {1.0f, 0.5f})
                for (int twoSided = 0; twoSided < 2; ++twoSided)
                    for (int alphaT = 0; alphaT < 2; ++alphaT) {
                        std::snprintf(what, sizeof(what), "%s %s %g %d %d", name,
                                      tex == alpha ? "alpha" : tex ? "opaque" : "-", opacity, twoSided, alphaT);
                        void* m = ctor(::operator new(0xC0), nullptr, name, tex, diffuse, alphaT ? black : specular,
                                       ambient, emissive, 8.0f, 0.5f, opacity, twoSided != 0, alphaT != 0);
                        DumpMaterial(what, m, alpha, opaque);
                        setTex(m, nullptr, tex == alpha ? opaque : alpha, 0, -1);
                        DumpMaterial("  swapped", m, alpha, opaque);
                        setTex(m, nullptr, alpha, 1, 0);
                        DumpMaterial("  stage 1", m, alpha, opaque);
                        setTex(m, nullptr, nullptr, 0, -1);
                        DumpMaterial("  removed", m, alpha, opaque);
                        setOpacity(m, nullptr, 0.3f);
                        setTwoSided(m, nullptr, twoSided == 0);
                        DumpMaterial("  changed", m, alpha, opaque);
                        void* c = copy(::operator new(0xC0), nullptr, m);
                        DumpMaterial("  copy", c, alpha, opaque);
                    }
}

// One frame of the scene (inside Open / Close): time in ms; each character a bit further along.
void DrawCharacterScene(CharacterScene& scene, void* viewport, float time, bool setTimes)
{
    auto setTime = Export<SetTimeFn>("?SetTime@CATKeyframeAnim_t@@QAEXM@Z");
    for (size_t i = 0; i < scene.anims.size() && setTimes; ++i) {
        float t = time + 137.0f * float(i);
        setTime(scene.anims[i], nullptr, scene.duration > 0.0f ? std::fmod(t, scene.duration) : t);
    }
    Export<PtrArgFn>("?SetCamera@RViewPort_t@@QAEXPAVRCamera_t@@@Z")(viewport, nullptr, scene.camera);
    Export<PtrArgFn>("?Process@RViewPort_t@@QAEXPAVRRefFrame_t@@@Z")(viewport, nullptr, scene.root);
    Export<ViewRenderFn>("?Render@RViewPort_t@@QAEXW4RenderList_e@@0W4RenderType_e@1@II@Z")(
        viewport, nullptr, 0, 10, g_debugDraw ? 4 | 8 : 4, 0, 1799);   // 8: the debugger's lists too
    if (g_shadow) DrawShadow(scene, viewport);
}

// A crash ends the harness with a line on its output instead of Wine's crash dialog on the desktop.
LONG WINAPI Crashed(EXCEPTION_POINTERS* e)
{
    std::printf("harness crashed: exception %08lx at %p\n", (unsigned long)e->ExceptionRecord->ExceptionCode,
                e->ExceptionRecord->ExceptionAddress);
    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 3);
    return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

int main(int argc, char** argv)
{
    SetUnhandledExceptionFilter(&Crashed);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    int frames = 3;
    std::string shot = "randy_harness.bmp";
    std::string characterMesh, characterAnim;
    float characterTime = 0.0f;
    int crowd = 1;
    bool pick = false;
    bool query = false;
    bool destroy = false;                            // --destroy: delete the characters and statics at the end
    bool still = false;                              // the animation time doesn't advance (static vertex buffers)
    std::string staticMesh;
    int statics = 1;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--frames" && i + 1 < argc) frames = std::atoi(argv[++i]);
        else if (a == "--shot" && i + 1 < argc) shot = argv[++i];
        else if (a == "--character" && i + 2 < argc) { characterMesh = argv[++i]; characterAnim = argv[++i]; }
        else if (a == "--time" && i + 1 < argc) characterTime = float(std::atof(argv[++i]));
        else if (a == "--crowd" && i + 1 < argc) crowd = std::max(1, std::atoi(argv[++i]));
        else if (a == "--look" && i + 3 < argc) {
            g_look = true;
            for (float& v : g_lookAt) v = float(std::atof(argv[++i]));
        }
        else if (a == "--pick") pick = true;
        else if (a == "--still") still = true;
        else if (a == "--destroy") destroy = true;
        else if (a == "--terrain" && i + 1 < argc) g_terrain = float(std::atof(argv[++i]));
        else if (a == "--playfield" && i + 1 < argc) g_playfield = std::atoi(argv[++i]);
        else if (a == "--preprocess" && i + 1 < argc) g_preprocess = std::atoi(argv[++i]);
        else if (a == "--attach" && i + 1 < argc) g_attach = std::atoi(argv[++i]);
        else if (a == "--restore") g_restore = true;
        else if (a == "--defaults") g_defaults = true;
        else if (a == "--targets") g_targets = true;
        else if (a == "--shutdown") g_shutdown = true;
        else if (a == "--debug-draw") g_debugDraw = true;
        else if (a == "--sprites" && i + 1 < argc) g_sprites = std::atoi(argv[++i]);
        else if (a == "--resize" && i + 2 < argc) {
            g_resize[0] = std::atoi(argv[++i]);
            g_resize[1] = std::atoi(argv[++i]);
        }
        else if (a == "--fullscreen") g_fullscreen = true;
        else if (a == "--format" && i + 1 < argc) g_format = std::atoi(argv[++i]);
        else if (a == "--any-device") g_anyDevice = true;
        else if (a == "--lights" && i + 1 < argc) g_carriedLights = std::atoi(argv[++i]);
        else if (a == "--alpha" && i + 1 < argc) g_alpha = float(std::atof(argv[++i]));
        else if (a == "--sfx" && i + 1 < argc) g_sfx = std::atoi(argv[++i]);
        else if (a == "--env") g_env = true;
        else if (a == "--shadow") g_shadow = true;
        else if (a == "--dynamic") g_dynamic = true;
        else if (a == "--materials") g_materials = true;
        else if (a == "--texture") g_texture = true;
        else if (a == "--png") g_png = true;
        else if (a == "--jpeg") g_jpeg = true;
        else if (a == "--status") g_status = true;
        else if (a == "--lightmap") g_lightmap = true;
        else if (a == "--connector") g_connector = true;
        else if (a == "--grid") g_grid = true;
        else if (a == "--blend" && i + 1 < argc) g_blend = float(std::atof(argv[++i]));
        else if (a == "--query") query = true;
        else if (a == "--static" && i + 1 < argc) staticMesh = argv[++i];
        else if (a == "--statics" && i + 1 < argc) statics = std::max(1, std::atoi(argv[++i]));
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
    void* randy = initialize(width, height, &error, nullptr, g_anyDevice ? nullptr : &device, !g_fullscreen, hwnd,
                             g_format);           // default: windowed, 32-bit, Z32
    if (!randy) {
        std::printf("Randy_t::Initialize failed: %s\n", error.c_str());
        return 1;
    }
    void* render = *Export<void**>("?m_pcInstance@render_t@@0PAV1@A");
    std::printf("Randy_t %p, render_t %p, IDirect3DDevice7 %p\n", randy, render, *static_cast<void**>(render));
    PrintInit();

    alignas(16) static uint8_t viewportStorage[0x178];    // sizeof(RViewPort_t), as DisplaySystem allocates
    void* viewport = Export<ViewPortCtorFn>("??0RViewPort_t@@QAE@IIIIABVRGB_t@@@Z")(
        viewportStorage, nullptr, 0, 0, width, height, Export<void*>("?white@RGB_t@@2V1@A"));   // clear colour
    void* deviceState = *reinterpret_cast<void**>(static_cast<uint8_t*>(viewport) + 8);   // RViewPort_t+8
    PrintDeviceState("devicestate (new viewport)", deviceState);
    PrintAdapters();
    if (g_targets) MakeRenderTargets(randy);

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

    auto setTexture = Export<SetTextureFn>("?SetTexture@DeviceState@@QAE_NPAVsurface_t@@IW4Priority_e@1@@Z");

    // Textures: a 64x64 A8R8G8B8 checker with a transparent ring, and an 8x8 DXT1 with four coloured blocks.
    std::vector<uint32_t> checker(64 * 64);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            float dx = x - 31.5f, dy = y - 31.5f, d = dx * dx + dy * dy;
            uint32_t alpha = (d > 20 * 20 && d < 26 * 26) ? 0x00 : 0xFF;
            checker[y * 64 + x] = (alpha << 24) | ((((x / 8) ^ (y / 8)) & 1) ? 0xF0C040 : 0x3060C0);
        }
    void* texChecker = MakeTexture("harness_checker", 64, 64, D3DX_SF_A8R8G8B8, checker.data(), 64 * 4);
    uint8_t dxt1[4 * 8] = {};
    const uint16_t blocks[4] = {0xF800, 0x07E0, 0x001F, 0xFFFF};
    for (int b = 0; b < 4; ++b) std::memcpy(dxt1 + b * 8, &blocks[b], 2);
    void* texDxt = MakeTexture("harness_dxt1", 8, 8, D3DX_SF_DXT1, dxt1, 16);
    std::printf("textures: checker surface_t %p, dxt1 surface_t %p\n", SurfaceOf(texChecker), SurfaceOf(texDxt));
    TestProcessVertices(render);
    if (g_materials) TestMaterials();
    if (g_texture) TestTextureStream();   // before the first present: in the frame=1 call log window
    if (g_png) TestPng();                 // likewise
    if (g_jpeg) TestJpeg();               // likewise
    if (g_status) TestStatus();           // likewise
    {
        auto* device = *static_cast<IDirect3DDevice7**>(render);
        device->EnumTextureFormats([](LPDDPIXELFORMAT pf, LPVOID) -> HRESULT {
            if (pf->dwFlags & DDPF_FOURCC)
                std::printf("texfmt FOURCC %.4s\n", reinterpret_cast<const char*>(&pf->dwFourCC));
            else
                std::printf("texfmt flags %08lx %lubpp R %08lx G %08lx B %08lx A %08lx\n", pf->dwFlags, pf->dwRGBBitCount,
                            pf->dwRBitMask, pf->dwGBitMask, pf->dwBBitMask, pf->dwRGBAlphaBitMask);
            return D3DENUMRET_OK;
        }, nullptr);
        D3DDEVICEDESC7 d{};
        device->GetCaps(&d);
        std::printf("caps devcaps %08lx tri raster %08lx zcmp %08lx src %08lx dst %08lx shade %08lx tex %08lx filter %08lx "
                    "blend %08lx addr %08lx\n", d.dwDevCaps, d.dpcTriCaps.dwRasterCaps, d.dpcTriCaps.dwZCmpCaps,
                    d.dpcTriCaps.dwSrcBlendCaps, d.dpcTriCaps.dwDestBlendCaps, d.dpcTriCaps.dwShadeCaps,
                    d.dpcTriCaps.dwTextureCaps, d.dpcTriCaps.dwTextureFilterCaps, d.dpcTriCaps.dwTextureBlendCaps,
                    d.dpcTriCaps.dwTextureAddressCaps);
        std::printf("caps misc %08lx render %08lx zdepth %08lx tex %lux%lu..%lux%lu repeat %lu aspect %lu aniso %lu "
                    "fvf %08lx texop %08lx stages %u simul %u lights %lu clip %u blend %u vtx %08lx stencil %08lx\n",
                    d.dpcTriCaps.dwMiscCaps, d.dwDeviceRenderBitDepth, d.dwDeviceZBufferBitDepth, d.dwMinTextureWidth,
                    d.dwMinTextureHeight, d.dwMaxTextureWidth, d.dwMaxTextureHeight, d.dwMaxTextureRepeat,
                    d.dwMaxTextureAspectRatio, d.dwMaxAnisotropy, d.dwFVFCaps, d.dwTextureOpCaps, d.wMaxTextureBlendStages,
                    d.wMaxSimultaneousTextures, d.dwMaxActiveLights, d.wMaxUserClipPlanes, d.wMaxVertexBlendMatrices,
                    d.dwVertexProcessingCaps, d.dwStencilCaps);
        IDirect3D7* d3d = nullptr;
        device->GetDirect3D(&d3d);
        IDirectDraw7* dd = nullptr;
        d3d->QueryInterface(IID_IDirectDraw7, reinterpret_cast<void**>(&dd));
        DDCAPS c{};
        c.dwSize = sizeof(c);
        DDCAPS hel{};
        hel.dwSize = sizeof(hel);
        dd->GetCaps(&c, &hel);
        std::printf("ddcaps caps %08lx caps2 %08lx ckey %08lx fx %08lx fxalpha %08lx pal %08lx svcaps %08lx alpha %lu/%lu z %08lx "
                    "vidmem %lu ddscaps %08lx/%08lx fourcc %lu hel caps %08lx hel ddscaps %08lx\n", c.dwCaps, c.dwCaps2,
                    c.dwCKeyCaps, c.dwFXCaps, c.dwFXAlphaCaps, c.dwPalCaps, c.dwSVCaps, c.dwAlphaBltConstBitDepths,
                    c.dwAlphaBltPixelBitDepths, c.dwZBufferBitDepths, c.dwVidMemTotal, c.ddsCaps.dwCaps, c.ddsCaps.dwCaps2,
                    c.dwNumFourCCCodes, hel.dwCaps, hel.ddsCaps.dwCaps);
        dd->Release();
        d3d->EnumZBufferFormats(IID_IDirect3DTnLHalDevice, [](LPDDPIXELFORMAT pf, LPVOID) -> HRESULT {
            std::printf("zfmt flags %08lx bits %lu z %08lx stencil %08lx\n", pf->dwFlags, pf->dwZBufferBitDepth,
                        pf->dwZBitMask, pf->dwStencilBitMask);
            return D3DENUMRET_OK;
        }, nullptr);
        d3d->Release();
    }

    CharacterScene scene;
    if (!characterMesh.empty() && !MakeCharacterScene(characterMesh, characterAnim, crowd, scene))
        return 1;
    if (!staticMesh.empty() && (scene.characters.empty() || !AddStatics(staticMesh, statics, scene)))
        return 1;
    if (g_sprites && !scene.characters.empty()) AddSprites(scene, g_sprites);
    LARGE_INTEGER frequency, frameStart, sceneStart{};
    QueryPerformanceFrequency(&frequency);
    double sceneSeconds = 0.0;
    for (int frame = 0; frame < frames; ++frame) {
        if (g_restore && frame == 2) ++*Export<unsigned*>("?s_nRestoreCount@Randy_t@@0IA");
        if (g_debugDraw) AddDebugShapes(frame);
        if (!scene.sprites.empty()) AnimateSprites(scene, 41.0f * float(frame), frame);
        if (g_resize[0] && frame == 3) {
            Export<void(__fastcall*)(void*, void*, unsigned, unsigned, bool)>("?Resize@RViewPort_t@@QAEXII_N@Z")(
                viewport, nullptr, unsigned(g_resize[0]), unsigned(g_resize[1]), true);
            PrintInit();
        }
        if (g_defaults && frame == 3) {
            auto* orig = reinterpret_cast<uint8_t*>(GetModuleHandleA("randy31_orig.dll"));
            uint32_t& caps = *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(randy) + 0x1E8);
            const uint32_t saved = caps;
            for (uint32_t c : {0u, 2u, 0x20u, 0x22u, saved}) {   // point, linear, linear mip linear
                caps = c;
                reinterpret_cast<void(__fastcall*)(void*)>(orig + 0x41EDE)(randy);
            }
            caps = saved;
        }
        bool restored = false;
        open(viewport, nullptr, &restored);
        unsigned clearColor = 0xFF203040;
        clear(viewport, nullptr, &clearColor, true, true, 0);
        if (!scene.characters.empty()) {
            if (g_lightmap) ApplyLightmap(scene, frame);
            QueryPerformanceCounter(&frameStart);
            DrawCharacterScene(scene, viewport, characterTime + (still ? 0.0f : 33.0f * float(frame)), !still || frame == 0);
            if (pick) PickCharacter(scene);
            if (pick && !scene.statics.empty()) PickStatics(scene);
            if (g_connector) TestConnector(scene, frame);
            if (g_grid) TestGrid(scene, viewport, frame);
            if (query && frame + 1 == frames) QueryCharacter(scene);
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            if (frame > 0) sceneSeconds += double(now.QuadPart - frameStart.QuadPart) / double(frequency.QuadPart);
            (void)sceneStart;
            if (frame == frames - 1 && frames > 1)
                std::printf("character: %d frames, %.3f ms per frame in Process + Render (the game thread's share)\n",
                            frames - 1, 1000.0 * sceneSeconds / (frames - 1));
            close(viewport, nullptr);
            if (frame == frames - 1) {
                void* bb = backBuffer(randy, nullptr);
                IDirectDrawSurface7** surface = bb ? surfacePtr(bb, nullptr) : nullptr;
                if (surface && *surface) SaveSurface(*surface, shot.c_str());
            }
            flip(randy, nullptr, false);
            continue;
        }

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

        // Textured quads (texture * vertex colour, alpha test drops the transparent ring).
        setTss(deviceState, nullptr, 0, D3DTSS_COLOROP, D3DTOP_MODULATE, prio);
        setTss(deviceState, nullptr, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE, prio);
        setTss(deviceState, nullptr, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1, prio);
        setTss(deviceState, nullptr, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE, prio);
        setTss(deviceState, nullptr, 0, D3DTSS_MAGFILTER, D3DTFG_POINT, prio);
        setTss(deviceState, nullptr, 0, D3DTSS_MINFILTER, D3DTFN_POINT, prio);
        setRs(deviceState, nullptr, D3DRENDERSTATE_ALPHABLENDENABLE, FALSE, prio);
        setRs(deviceState, nullptr, D3DRENDERSTATE_ALPHATESTENABLE, TRUE, prio);
        setRs(deviceState, nullptr, D3DRENDERSTATE_ALPHAREF, 0x80, prio);
        setRs(deviceState, nullptr, D3DRENDERSTATE_ALPHAFUNC, D3DCMP_GREATER, prio);
        setTexture(deviceState, nullptr, SurfaceOf(texChecker), 0, prio);
        update(deviceState, nullptr);
        VtxRhwTex texQuad[4] = {{20, 20, 0, 1, 0xFFFFFFFF, 0, 0}, {148, 20, 0, 1, 0xFFFFFFFF, 1, 0},
                                {20, 148, 0, 1, 0xFFFFFFFF, 0, 1}, {148, 148, 0, 1, 0xFFFFFFFF, 1, 1}};
        triStrip(render, nullptr, kFvfRhwDiffuseTex, texQuad, 4, 0);
        setRs(deviceState, nullptr, D3DRENDERSTATE_ALPHATESTENABLE, FALSE, prio);
        setTexture(deviceState, nullptr, SurfaceOf(texDxt), 0, prio);
        update(deviceState, nullptr);
        VtxRhwTex dxtQuad[4] = {{492, 20, 0, 1, 0xFFFFFFFF, 0, 0}, {620, 20, 0, 1, 0xFFFFFFFF, 1, 0},
                                {492, 148, 0, 1, 0xFFFFFFFF, 0, 1}, {620, 148, 0, 1, 0xFFFFFFFF, 1, 1}};
        triStrip(render, nullptr, kFvfRhwDiffuseTex, dxtQuad, 4, 0);
        setTexture(deviceState, nullptr, nullptr, 0, prio);
        update(deviceState, nullptr);
        if (g_dynamic) {
            // Triangles through the dynamic vertex buffer, as the UI draws: growing requests (the ring is made,
            // remade bigger, wraps), every other frame starts with a reset.
            using GetFn = void*(__cdecl*)();
            using GetVerticesFn = unsigned(__fastcall*)(void*, void*, unsigned fvf, unsigned stride, unsigned n, void** out);
            using GetVBFn = void*(__fastcall*)(void*, void*, unsigned fvf);
            using DrawVBFn = void(__fastcall*)(void*, void*, void* vb, unsigned start, unsigned n, unsigned flags);
            void* dyn = Export<GetFn>("?Get@DynamicVB_c@@SAAAV1@XZ")();
            if (frame % 2 == 0) Export<void(__fastcall*)(void*, void*)>("?Reset@DynamicVB_c@@QAEXXZ")(dyn, nullptr);
            auto getVertices = Export<GetVerticesFn>("?GetVertices@DynamicVB_c@@QAEIIIIPAPAX@Z");
            auto getVB = Export<GetVBFn>("?GetVB@DynamicVB_c@@QAEPAVVertexBuffer_c@@I@Z");
            auto drawVB = Export<DrawVBFn>("?RenderTriangleList@render_t@@QAEXPAVVertexBuffer_c@@KKK@Z");
            for (int k = 0; k < 40; ++k) {
                const unsigned n = k == 20 ? 4500u : 3u * unsigned(1 + k % 7);   // one past the 4000 it starts with
                void* out = nullptr;
                unsigned start = getVertices(dyn, nullptr, kFvfRhwDiffuse, sizeof(VtxRhw), n, &out);
                auto* v = static_cast<VtxRhw*>(out);
                for (unsigned i = 0; i < n; ++i) {
                    float x = 40.0f + 14.0f * float(k) + (i % 3 == 1 ? 10.0f : 0.0f), y = 300.0f + 5.0f * float(i / 3);
                    v[i] = {x, y + (i % 3 == 2 ? 10.0f : 0.0f), 0, 1, 0xFF000000u | (0x10101u * unsigned(k * 6))};
                }
                drawVB(render, nullptr, getVB(dyn, nullptr, kFvfRhwDiffuse), start, n, 0);
            }
        }

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
    PrintDeviceState("devicestate (end)", deviceState);
    if (g_terrain > 0.0f) {                          // the heightmap as meshes left it
        uint8_t* occ = static_cast<uint8_t*>(Export<void*(__cdecl*)()>("?Get@HMOccluder_t@@SAAAV1@XZ")());
        const uint16_t* heights = *reinterpret_cast<uint16_t**>(occ);
        const uint8_t* flags = *reinterpret_cast<uint8_t**>(occ + 4);
        uint32_t sum = 0, raised = 0, settled = 0;
        for (uint32_t i = 0; i < uint32_t(g_mapSize * g_mapSize); ++i) {
            sum = sum * 31 + heights[i];
            raised += heights[i] != 0;
            settled += flags[i] == 0xFF;
        }
        std::printf("heightmap %08x, %u cells above 0, %u settled\n", sum, raised, settled);
    }
    if (destroy) {                                   // --destroy: their destructors too (as the game deletes them)
        using DeleteFn = void*(__fastcall*)(void* self, void*, unsigned flags);
        auto deleting = [](void* o) { reinterpret_cast<DeleteFn>((*static_cast<void***>(o))[0])(o, nullptr, 1); };
        if (g_connectorObject) deleting(g_connectorObject);
        for (void* o : scene.statics) deleting(o);
        for (void* o : scene.sprites) deleting(o);
        for (void* c : scene.characters) deleting(c);
        std::printf("destroyed %zu statics, %zu characters\n", scene.statics.size(), scene.characters.size());
    }
    if (g_shutdown) {                                // Randy_t's deleting destructor (vtable slot 0), then what is left
        using DeleteFn = void*(__fastcall*)(void* self, void*, unsigned flags);
        reinterpret_cast<DeleteFn>((*static_cast<void***>(randy))[0])(randy, nullptr, 1);
        const uint8_t* orig = reinterpret_cast<uint8_t*>(GetModuleHandleA("randy31_orig.dll"));
        auto set = [&](uint32_t rva) { return *reinterpret_cast<void* const*>(orig + rva) != nullptr ? 1 : 0; };
        std::printf("shutdown: render_t %d Randy_t %d DeviceState %d Debugger_t %d primary %d default texture %d%d "
                    "targets", set(0x16BED0), set(0x17D2EC), set(0x168FEC), set(0x17D23C), set(0x17D2F4),
                    set(0x17D498), set(0x17D49C));
        for (uint32_t i = 0; i < 7; ++i) std::printf(" %d", set(0x17D2F8 + i * 4));
        std::printf(", emulated %x\n", *reinterpret_cast<const uint32_t*>(orig + 0xB7730));
    }
    std::printf("harness done\n");
    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 0);   // skip Randy's static destructors / device teardown
}
