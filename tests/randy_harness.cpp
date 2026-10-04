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
};

// Builds root -> {camera, sun, `count` characters in a grid}, all with the same model and animation (each its own
// animation instance, at different times). False (with a message) if a stream can't be read.
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
    void* animSource = Export<DataCtorFn>("??0CATKeyframeAnimData_t@@QAE@PAVDataIO_t@fun@@@Z")(
        ::operator new(0x48), nullptr, animIo);
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
        Export<PtrArgFn>("?SetAnim@CATRender_t@@QAEXPAVCATAnim_t@@@Z")(character, nullptr, anim);
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
        scene.characters.push_back(character);
        scene.anims.push_back(anim);
    }
    // The camera: close for one character, back and up for a crowd.
    float back = count == 1 ? 4.0f : 2.0f + 1.6f * float(columns);
    Vector3 eye{0.0f, count == 1 ? 1.2f : 0.5f * back, -back}, target{0.0f, 1.0f, 0.8f * float(columns / 2)};
    setPos(scene.camera, nullptr, &eye, nullptr);
    setTarget(scene.camera, nullptr, &target);
    // A white sun from above and in front (RLight_t::Type_e 1 = directional).
    using LightCtorFn = void*(__fastcall*)(void* self, void*, void* parent, const float* rgb, int type, void* anim);
    const float white[3] = {1.0f, 0.95f, 0.85f};
    void* sun = Export<LightCtorFn>("??0RLight_t@@QAE@PAVRRefFrame_t@@ABVRGB_t@@W4Type_e@0@PAVRAnimation_t@@@Z")(
        ::operator new(0x11C), nullptr, scene.root, white, 1, nullptr);
    Vector3 sunPos{3.0f, 6.0f, -5.0f}, origin{0.0f, 0.0f, 0.0f};
    setPos(sun, nullptr, &sunPos, nullptr);
    setTarget(sun, nullptr, &origin);
    std::printf("character: mesh %p (%d materials), %d characters, animation %.0f ms\n", mesh, materials, count,
                scene.duration);
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
        viewport, nullptr, 0, 10, 4, 0, 1799);
}

}  // namespace

int main(int argc, char** argv)
{
    int frames = 3;
    std::string shot = "randy_harness.bmp";
    std::string characterMesh, characterAnim;
    float characterTime = 0.0f;
    int crowd = 1;
    bool pick = false;
    bool still = false;                              // the animation time doesn't advance (static vertex buffers)
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--frames" && i + 1 < argc) frames = std::atoi(argv[++i]);
        else if (a == "--shot" && i + 1 < argc) shot = argv[++i];
        else if (a == "--character" && i + 2 < argc) { characterMesh = argv[++i]; characterAnim = argv[++i]; }
        else if (a == "--time" && i + 1 < argc) characterTime = float(std::atof(argv[++i]));
        else if (a == "--crowd" && i + 1 < argc) crowd = std::max(1, std::atoi(argv[++i]));
        else if (a == "--pick") pick = true;
        else if (a == "--still") still = true;
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
    LARGE_INTEGER frequency, frameStart, sceneStart{};
    QueryPerformanceFrequency(&frequency);
    double sceneSeconds = 0.0;
    for (int frame = 0; frame < frames; ++frame) {
        bool restored = false;
        open(viewport, nullptr, &restored);
        unsigned clearColor = 0xFF203040;
        clear(viewport, nullptr, &clearColor, true, true, 0);
        if (!scene.characters.empty()) {
            QueryPerformanceCounter(&frameStart);
            DrawCharacterScene(scene, viewport, characterTime + (still ? 0.0f : 33.0f * float(frame)), !still || frame == 0);
            if (pick) PickCharacter(scene);
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
