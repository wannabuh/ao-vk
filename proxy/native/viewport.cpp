// RViewPort_t natively (randy-vk.ini [Native] Scene=on): materials on the device, the viewport rectangle and camera
// transforms, Open / Clear / Close of a frame, picking and projection helpers, and the frame itself: Process (the
// camera, the scene's update, its lights onto the device) and Render / RenderRefraction (the render lists' visuals,
// list by list, bucket by bucket). With Visuals on, scene.cpp learns which visual draws through these.
//
// Render lists (0x101CEDE8): 11 lists x 0x708 buckets of visuals, chained through RVisual_t +0xE8; filled while the
// scene updates, emptied after a Render with type flag 4. Lights (0x1017D290): std::vector<RLight_t*>, filled while
// the scene updates.
//
// RViewPort_t (0x178 bytes; DisplaySystem allocates them): +0x08 DeviceState, +0x0C RCamera_t, +0x10 the material
// set, +0x18 a pixel's size at distance 1, +0x1C x, +0x20 y, +0x24 / +0x2C width / height (clamped to the target),
// +0x28 / +0x30 asked for, +0x34 clear colour (RGB_t), +0x40 / +0x68 / +0x90 Timers, +0xB8 target surface_t,
// +0xBC D3DVIEWPORT7, +0xD8 two D3DMATERIAL7 (0x44 each, the device's material double-buffered), +0x160 which is
// current, +0x164 the visual being drawn, +0x170 rectangle changed.
#include "native/viewport.h"
#include "native/orig_api.gen.h"
#include "native/scene.h"
#include "native/vc10.h"
#include "native/xmath.h"

#include <cstring>
#include <initializer_list>

namespace rnative::viewport {

namespace {

HMODULE g_orig;
void* const* g_render;
void* const* g_randy;
const uint32_t* g_debuggerMode;
float* g_viewMatrix;                                // RViewPort_t::m_CurrentViewMatrix
float* g_projectionMatrix;                          // RViewPort_t::m_CurrentProjectionMatrix

template <typename T>
T& Field(void* object, uint32_t offset) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(object) + offset); }
template <typename T>
T& Global(uint32_t rva) { return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(g_orig) + rva); }
template <typename F>
F Internal(uint32_t rva) { return reinterpret_cast<F>(reinterpret_cast<uint8_t*>(g_orig) + rva); }

template <typename... A>
HRESULT Com(void* object, uint32_t offset, A... args)
{
    using Fn = HRESULT(__stdcall*)(void*, A...);
    return reinterpret_cast<Fn>((*static_cast<void***>(object))[offset / 4])(object, args...);
}

void* Render() { return *g_render; }
void* Device() { return Field<void*>(Render(), 0); }
void* Randy() { return *g_randy; }

constexpr HRESULT kSurfaceLost = HRESULT(0x887601C2);
constexpr uint32_t kLastViewport = 0x1BB4EC;        // the viewport whose rectangle the device has

float ToFloat(uint32_t v) { return float(int32_t(v)) + (int32_t(v) < 0 ? 4294967296.0f : 0.0f); }   // unsigned

// render_t::SetMaterial (FUN_10023d0a).
void DeviceMaterial(const void* material)
{
    if ((*g_debuggerMode & 0x100) || !Device()) return;
    Com(Device(), 0x40, material);
}

// FUN_1004b0c0: the device's viewport rectangle and camera transforms, when they changed or another viewport set
// them.
void __fastcall Update(uint8_t* vp)
{
    if (!vp[0x170] && Global<void*>(kLastViewport) == vp) return;
    Global<void*>(kLastViewport) = vp;
    vp[0x170] = 0;
    const uint8_t* desc = static_cast<const uint8_t*>(orig::surface_t_GetSurfaceDesc(Field<void*>(vp, 0xB8)));
    const uint32_t surfaceHeight = Field<uint32_t>(const_cast<uint8_t*>(desc), 8);
    const uint32_t surfaceWidth = Field<uint32_t>(const_cast<uint8_t*>(desc), 0xC);
    const uint32_t width = Field<uint32_t>(vp, 0x28) <= surfaceWidth ? Field<uint32_t>(vp, 0x28) : surfaceWidth;
    const uint32_t height = Field<uint32_t>(vp, 0x30) <= surfaceHeight ? Field<uint32_t>(vp, 0x30) : surfaceHeight;
    Field<uint32_t>(vp, 0x24) = width;
    Field<uint32_t>(vp, 0x2C) = height;
    Field<float>(vp, 0xCC) = 0.0f;                  // D3DVIEWPORT7: x, y, width, height, min z, max z
    Field<uint32_t>(vp, 0xBC) = Field<uint32_t>(vp, 0x1C);
    Field<float>(vp, 0xD0) = 1.0f;
    Field<uint32_t>(vp, 0xC0) = Field<uint32_t>(vp, 0x20);
    Field<uint32_t>(vp, 0xC4) = width;
    Field<uint32_t>(vp, 0xC8) = height;
    if (Device()) Com(Device(), 0x34, static_cast<void*>(vp + 0xBC));   // render_t SetViewport (FUN_10023572)
    void* camera = Field<void*>(vp, 0xC);
    if (!camera) return;
    orig::RCamera_t_GetViewMatrix(camera, g_viewMatrix);
    orig::render_t_SetTransformMatrix(Render(), 2, g_viewMatrix);   // VIEW
    // FUN_1004afe5: the view plane window's width over the viewport's, halved.
    Field<float>(vp, 0x18) = (Field<float>(camera, 0xAC) - Field<float>(camera, 0xA4)) / ToFloat(width) * 0.5f;
    orig::RCamera_t_GetTransformationMatrix(camera, g_projectionMatrix);
    orig::render_t_SetTransformMatrix(Render(), 3, g_projectionMatrix);   // PROJECTION
}

// The device's material, in the slot not current, when it differs from it (by more than 1e-4 squared in the fields
// Randy uses: diffuse alpha, specular, specular strength, emissive, power).
float* Slot(uint8_t* vp, int i) { return &Field<float>(vp, 0xD8 + i * 0x44); }

bool Differs(float a, float b) { return 1e-4f < (a - b) * (a - b); }

void SetDeviceMaterial(uint8_t* vp, float alpha, const float* emissive, const float* specular, float strength,
                       float power)
{
    int32_t& current = Field<int32_t>(vp, 0x160);
    const float* m = Slot(vp, current);              // diffuse +0, ambient +4, specular +8, emissive +12, power +16
    if (!(Differs(m[3], alpha) || Differs(m[12], emissive[0]) || Differs(m[13], emissive[1]) ||
          Differs(m[14], emissive[2]) || Differs(m[8], specular[0]) || Differs(m[9], specular[1]) ||
          Differs(m[10], specular[2]) || Differs(m[11], strength) || Differs(m[16], power)))
        return;
    current = 1 - current;
    float* n = Slot(vp, current);
    n[3] = alpha;
    n[12] = emissive[0], n[13] = emissive[1], n[14] = emissive[2];
    n[8] = specular[0], n[9] = specular[1], n[10] = specular[2];
    n[11] = strength;
    n[16] = power;
    DeviceMaterial(n);
}

void __fastcall SetMaterial(uint8_t* vp, void*, uint8_t* material)
{
    if (material) {
        Internal<void(__fastcall*)(void*, void*, void*)>(0x40AA0)(material, nullptr, Field<void*>(vp, 8));   // its states
        orig::RResource_t_AddRefRResource(material);
        const float strength = Field<float>(material, 0x64);
        const float specular[3] = {strength * Field<float>(material, 0x38), Field<float>(material, 0x3C) * strength,
                                   Field<float>(material, 0x40) * strength};
        SetDeviceMaterial(vp, Field<float>(material, 0x68), &Field<float>(material, 0x50), specular, strength,
                          Field<float>(material, 0x60));
    }
    if (void* old = Field<void*>(vp, 0x10)) orig::RResource_t_ReleaseRResource(old);
    Field<void*>(vp, 0x10) = material;
}

void __fastcall SetD3DMaterial(uint8_t* vp, void*, const float* m)   // a D3DMATERIAL7
{
    SetDeviceMaterial(vp, m[3], m + 12, m + 8, m[11], m[16]);
}

void __fastcall ResetMaterial(uint8_t* vp)
{
    if (void* material = Field<void*>(vp, 0x10)) {
        Internal<void(__fastcall*)(void*, void*, void*)>(0x40AC9)(material, nullptr, Field<void*>(vp, 8));
        orig::RResource_t_ReleaseRResource(material);
    }
    Field<void*>(vp, 0x10) = nullptr;
}

// White diffuse / ambient, grey specular (0.505) of power 10, no emissive; slot 0 on the device, slot 1 the same.
void __fastcall SetDefaultMaterial(uint8_t* vp)
{
    Field<int32_t>(vp, 0x160) = 0;
    for (int i = 0; i < 2; ++i) {
        float* m = Slot(vp, i);
        for (int k = 0; k < 8; ++k) m[k] = 1.0f;
        m[8] = m[9] = m[10] = 0.505f;
        m[11] = 1.0f;
        m[12] = m[13] = m[14] = 0.0f;
        m[15] = 1.0f;
        m[16] = 10.0f;
        if (i == 0) DeviceMaterial(m);
    }
}

void __fastcall SetCamera(uint8_t* vp, void*, void* camera) { Field<void*>(vp, 0xC) = camera; }

void __fastcall RealizeRenderStates(uint8_t* vp)
{
    void* randy = Randy();
    if (Field<int32_t>(randy, 0x284) != Field<int32_t>(randy, 0x280))   // FUN_10042f58: the render target asked for
        orig::Randy_t_SetRenderTarget(randy, vp, Field<int32_t>(randy, 0x284));
    orig::DeviceState_UpdateDevice(Field<void*>(vp, 8));
}

void* ZSurface()                                    // FUN_1004196a: the render target's depth surface_t
{
    void* randy = Randy();
    void** targets = &Global<void*>(0x17D2F8);
    void* target = targets[Field<int32_t>(randy, 0x280)];
    if (!target) target = targets[0];
    return Field<void*>(target, 4);
}

// Clears the target (`target`) and / or depth (`depth`, if there is a depth surface) to `color` (0: the viewport's).
void __fastcall Clear(uint8_t* vp, void*, const uint32_t*, bool depth, bool target, uint32_t color)
{
    Update(vp);
    const uint32_t flags = ZSurface() ? (depth ? 2u : 0u) | (target ? 1u : 0u) : (target ? 1u : 0u);
    if (!flags) return;
    if (color == 0) orig::Color_t_Color_t_14(&color, vp + 0x34);
    if ((*g_debuggerMode & 0x100) || !Device()) return;
    Com(Device(), 0x28, DWORD(0), static_cast<void*>(nullptr), DWORD(flags), DWORD(color), 1.0f, DWORD(0));
}

bool SurfaceLost(void* surface) { return Com(*static_cast<void**>(surface), 0x60) == kSurfaceLost; }   // IsLost
void Restore(void* surface) { Com(*static_cast<void**>(surface), 0x6C); }

// Starts a frame: surfaces lost (fullscreen switch, alt-tab) restored and cleared, then BeginScene.
bool __fastcall Open(uint8_t* vp, void*, bool* restored)
{
    void* randy = Randy();
    bool any = false;
    if (restored) *restored = false;
    void* primary = orig::Randy_t_GetDDSurface(randy);
    if (SurfaceLost(primary)) {
        any = true;
        Restore(primary);
        if (restored) *restored = true;
    }
    if (!Global<uint8_t>(0x17D31C)) {               // not the fullscreen flip chain: the back buffer too
        void* back = orig::Randy_t_GetBackBuffer(randy);
        if (SurfaceLost(back)) {
            any = true;
            Restore(back);
            if (restored) *restored = true;
        }
    }
    void* z = ZSurface();
    if (z && SurfaceLost(z)) {
        Restore(z);
        if (restored) *restored = true;
        any = true;
    }
    if (any) Clear(vp, nullptr, nullptr, true, true, 0);
    if (void* device = Device()) {                  // render_t BeginScene (FUN_100233f1)
        int32_t& revalidate = Global<int32_t>(0xB72F4);
        if (revalidate) --revalidate;
        Com(device, 0x14);
    }
    return true;
}

void __fastcall Close(uint8_t*)
{
    if (void* device = Device()) Com(device, 0x18);   // EndScene
}

void __fastcall Reposition(uint8_t* vp, void*, uint32_t x, uint32_t y)
{
    if (Field<uint32_t>(vp, 0x1C) == x && Field<uint32_t>(vp, 0x20) == y) return;
    Field<uint32_t>(vp, 0x1C) = x;
    Field<uint32_t>(vp, 0x20) = y;
    vp[0x170] = 1;
    Update(vp);
}

void __fastcall SetViewRect(uint8_t* vp, void*, float x, float y, float width, float height)
{
    const int32_t ix = int32_t(x), iy = int32_t(y), iw = int32_t(width), ih = int32_t(height);   // _ftol
    if (ix == Field<int32_t>(vp, 0x1C) && iy == Field<int32_t>(vp, 0x20) && iw == Field<int32_t>(vp, 0x24) &&
        ih == Field<int32_t>(vp, 0x2C))
        return;
    Field<int32_t>(vp, 0x20) = iy;
    Field<int32_t>(vp, 0x1C) = ix;
    Field<int32_t>(vp, 0x24) = iw;
    Field<int32_t>(vp, 0x2C) = ih;
    Field<int32_t>(vp, 0x28) = iw;
    Field<int32_t>(vp, 0x30) = ih;
    vp[0x170] = 1;
    Update(vp);
}

// A new size; the back buffer grown (Randy_t, FUN_10043c48) when it is smaller - or always when `force`.
void __fastcall Resize(uint8_t* vp, void*, uint32_t width, uint32_t height, bool force)
{
    if (Field<uint32_t>(vp, 0x24) == width && Field<uint32_t>(vp, 0x2C) == height && !force) return;
    Field<uint32_t>(vp, 0x24) = width;
    Field<uint32_t>(vp, 0x2C) = height;
    Field<uint32_t>(vp, 0x28) = width;
    Field<uint32_t>(vp, 0x30) = height;
    bool grow = force;
    if (!force) {
        uint32_t bufferWidth = 0, bufferHeight = 0;
        orig::Randy_t_GetBufferSize(Randy(), &bufferWidth, &bufferHeight);
        grow = !(width <= bufferWidth && height <= bufferHeight);
    }
    if (grow) Internal<void(__fastcall*)(void*, void*, uint32_t, uint32_t)>(0x43C48)(Randy(), nullptr, width, height);
    vp[0x170] = 1;
    Update(vp);
}

// The direction (on the view plane at distance 1, camera space) through pixel (x, y) of the viewport.
float* __fastcall CalcPickLine(uint8_t* vp, void*, float* out, uint32_t x, uint32_t y)
{
    const float left = ToFloat(Field<uint32_t>(vp, 0x1C)), top = ToFloat(Field<uint32_t>(vp, 0x20));
    const float right = ToFloat(Field<uint32_t>(vp, 0x24) + Field<uint32_t>(vp, 0x1C));
    const float bottom = ToFloat(Field<uint32_t>(vp, 0x2C) + Field<uint32_t>(vp, 0x20));
    const float px = ToFloat(x), py = ToFloat(y), w = right - left, h = bottom - top;
    float l, t, r, b;
    orig::RCamera_t_GetViewPlaneWindow(Field<void*>(vp, 0xC), &l, &t, &r, &b);
    out[0] = (r - l) * (px / w - 0.5f) + (l + r) * 0.5f;
    out[1] = (t - b) * (0.5f - py / h) + (t + b) * 0.5f;
    out[2] = 1.0f;
    return out;
}

// A world point through view and projection (no divide by w).
bool __fastcall TransformPointToScreenSpace(uint8_t* vp, void*, const float* point, float* out)
{
    xm::M4 view = xm::Identity(), projection = xm::Identity();
    orig::RCamera_t_GetViewMatrix(Field<void*>(vp, 0xC), view.m);
    orig::RCamera_t_GetTransformationMatrix(Field<void*>(vp, 0xC), projection.m);
    const xm::M4 m = xm::Mul(view, projection);
    xm::Transform(point, m.m, out);
    return true;
}

void* __fastcall Construct(uint8_t* vp, void*, uint32_t x, uint32_t y, uint32_t width, uint32_t height, const float* rgb)
{
    Field<uintptr_t>(vp, 0) = reinterpret_cast<uintptr_t>(g_orig) + 0x958EC;   // RViewPort_t's vftable
    Field<void*>(vp, 8) = Field<void*>(Randy(), 0x27C);
    Field<float>(vp, 0x34) = Field<float>(vp, 0x38) = Field<float>(vp, 0x3C) = 0.0f;
    for (uint32_t timer : {0x40u, 0x68u, 0x90u}) orig::Timer_Timer_57(vp + timer);
    vp[0xD4] = 0;
    Field<void*>(vp, 0x164) = nullptr;
    Field<void*>(vp, 0xC) = nullptr;
    Field<void*>(vp, 0x10) = nullptr;
    Field<void*>(vp, 0x14) = nullptr;
    Field<void*>(vp, 0xB8) = orig::Randy_t_GetDDSurface(Randy());
    Field<uint32_t>(vp, 0x1C) = x;
    Field<uint32_t>(vp, 0x20) = y;
    Field<uint32_t>(vp, 0x2C) = height;
    Field<uint32_t>(vp, 0x30) = height;
    Field<uint32_t>(vp, 0x24) = width;
    Field<uint32_t>(vp, 0x28) = width;
    std::memcpy(vp + 0x34, rgb, 12);
    vp[0x170] = 1;
    Update(vp);
    Internal<void(__fastcall*)(void*)>(0x1BD36)(Field<void*>(vp, 8));   // DeviceState: read back from the device
    if (!Global<uint8_t>(0x1BB4E8)) {
        Internal<void(__cdecl*)(int32_t, int32_t)>(0x298AC)(3000, 10000);   // the render lists' pools
        Global<uint8_t>(0x1BB4E8) = 1;
    }
    SetDefaultMaterial(vp);
    return vp;
}

// FUN_1004ba7c: a new target surface (Randy_t::SetRenderTarget).
void __fastcall SetTarget(uint8_t* vp, void*, void* surface)
{
    Field<void*>(vp, 0xB8) = surface;
    if (Device()) Com(Device(), 0x20, *static_cast<void**>(surface), DWORD(0));   // render_t SetRenderTarget
    vp[0x170] = 1;
    Update(vp);
}

// ---- the frame ----

constexpr uint32_t kRenderLists = 0x1CEDE8, kBucketsPerList = 0x708, kListBytes = 0x13560;
constexpr uint32_t kLightsBegin = 0x17D290, kLightsEnd = 0x17D294;

void SetCurrentList(int32_t list) { Field<int32_t>(Render(), 0x288) = list; }   // FUN_1002381a

// Every visual in lists `listFrom` to `listTo`, buckets `from` to `to` (either direction), drawn through its vtable
// `slot` (13 Render, 15 RenderRefraction); the visual being drawn kept at +0x164.
void Walk(uint8_t* vp, int32_t listFrom, int32_t listTo, uint32_t from, uint32_t to, uint32_t slot)
{
    const int32_t lists = (listFrom < listTo ? listTo - listFrom : listFrom - listTo) + 1;
    const uint32_t buckets = (from < to ? to - from : from - to) + 1;
    const int32_t listStep = listFrom < listTo ? 1 : -1;
    const int32_t bucketStep = from < to ? 1 : -1;
    void** base = &Global<void*>(kRenderLists);
    int32_t list = listFrom;
    for (int32_t i = 0; i < lists; ++i, list += listStep) {
        SetCurrentList(list);
        void** bucket = base + list * int32_t(kBucketsPerList) + int32_t(from);
        for (uint32_t j = 0; j < buckets; ++j, bucket += bucketStep) {
            void* visual = *bucket;
            while ((Field<void*>(vp, 0x164) = visual) != nullptr) {
                reinterpret_cast<void(__fastcall*)(void*, void*, void*)>((*static_cast<void***>(visual))[slot])(visual,
                                                                                                         nullptr, vp);
                visual = Field<void*>(Field<void*>(vp, 0x164), 0xE8);
            }
        }
    }
}

void DebuggerDraw(uint8_t* vp)                       // type flag 8
{
    void* debugger = orig::Debugger_t_Get();
    Internal<void(__fastcall*)(void*, void*, void*)>(0x2C082)(debugger, nullptr, vp);
}

// After a pass (type flag 4): the lights off, the render lists emptied.
void EndPass(uint8_t* vp)
{
    Global<uint8_t>(0xB7728) = 0;
    if (Field<void*>(vp, 0xC))
        for (void** l = Global<void**>(kLightsBegin); l != Global<void**>(kLightsEnd); ++l)
            orig::RLight_t_Enable(*l, false);
    std::memset(&Global<uint8_t>(kRenderLists), 0, kListBytes);
}

// Debugger mode 0x8000 (with 0x10000, set by Flip): a degenerate triangle drawn into render target 6 or 5 in turn,
// with fixed states, to keep them in use.
void DebugTargetTouch(uint8_t* vp)
{
    void* randy = Randy();
    const bool even = (Field<uint32_t>(randy, 0x274) & 1) == 0;
    if (!orig::Randy_t_SetRenderTarget(randy, vp, even ? 6 : 5)) return;
    uint32_t& made = Global<uint32_t>(0x1CEDDC);
    uint8_t& filled = Global<uint8_t>(even ? 0x1CED74 : 0x1CEDA8);
    float* vertices = &Global<float>(even ? 0x1CED78 : 0x1CEDAC);   // 3 x {x, y, z, rhw}
    const uint32_t bit = even ? 2 : 1;
    if (!(made & bit)) {
        made |= bit;
        for (int v = 0; v < 3; ++v) vertices[v * 4] = vertices[v * 4 + 1] = vertices[v * 4 + 2] = 0.0f;
    }
    if (!filled) {
        filled = 1;
        for (int v = 0; v < 3; ++v) {
            vertices[v * 4] = -1.0f;
            vertices[v * 4 + 1] = -1.0f;
            vertices[v * 4 + 2] = 0.1f;
            vertices[v * 4 + 3] = 10.0f;
        }
    }
    void* ds = Field<void*>(Randy(), 0x27C);
    struct Saved {
        uint32_t state, old;
        bool set;
    } saved[6];
    const uint32_t states[6][2] = {{0x16, 1}, {0x17, 8}, {0x0E, 0}, {0x1B, 0}, {0x1C, 0}, {0x89, 0}};
    for (int i = 0; i < 6; ++i) {
        saved[i] = {states[i][0], Field<uint32_t>(ds, 0x4C8 + states[i][0] * 4), false};
        saved[i].set = orig::DeviceState_SetRenderState(ds, int32_t(states[i][0]), states[i][1], 10);
    }
    void* dynamic = orig::DynamicVB_c_Get();
    void* out = nullptr;
    const uint32_t start = orig::DynamicVB_c_GetVertices(orig::DynamicVB_c_Get(), 4, 0x10, 3, &out);   // XYZRHW
    std::memcpy(out, vertices, 0x30);
    RealizeRenderStates(vp);
    orig::render_t_RenderTriangleList_407(Render(), orig::DynamicVB_c_GetVB(dynamic, 4), start, 3, 0);
    orig::Randy_t_SetRenderTarget(randy, vp, 0);
    for (int i = 5; i >= 0; --i)
        if (saved[i].set) orig::DeviceState_SetRenderState(ds, int32_t(saved[i].state), saved[i].old, 2);
}

void __fastcall RenderLists(uint8_t* vp, void*, int32_t listFrom, int32_t listTo, uint32_t type, uint32_t from,
                            uint32_t to)
{
    void* previous = scene::EnterRender(vp);
    if (Field<void*>(vp, 0xC)) {
        Update(vp);
        Walk(vp, listFrom, listTo, from, to, 13);
        if (type & 8) DebuggerDraw(vp);
    }
    if (type & 4) {
        EndPass(vp);
        if ((*g_debuggerMode & 0x8000) && (*g_debuggerMode & 0x10000)) DebugTargetTouch(vp);
        RealizeRenderStates(vp);
    }
    scene::LeaveRender(previous);
}

void __fastcall RenderRefraction(uint8_t* vp, void*, int32_t listFrom, int32_t listTo, uint32_t type, uint32_t from,
                                 uint32_t to)
{
    void* previous = scene::EnterRender(vp);
    if (Field<void*>(vp, 0xC)) {
        Update(vp);                                 // FUN_1004b0c0
        Walk(vp, listFrom, listTo, from, to, 15);
        if (type & 8) DebuggerDraw(vp);
    }
    if (type & 4) {
        EndPass(vp);
        RealizeRenderStates(vp);
    }
    scene::LeaveRender(previous);
}

// The frame's scene update: camera transforms and frustum, the lights cleared, the scene processed (lights register
// themselves), each light onto the device.
void __fastcall Process(uint8_t* vp, void*, void* root)
{
    Field<float>(vp, 0x168) = 1000.0f;
    Field<uint32_t>(vp, 0x16C) = 0;
    Field<uint32_t>(Randy(), 0x288) &= 7;
    void* camera = Field<void*>(vp, 0xC);
    if (!camera) return;
    orig::RCamera_t_GetViewMatrix(camera, g_viewMatrix);
    orig::render_t_SetTransformMatrix(Render(), 2, g_viewMatrix);
    Field<float>(vp, 0x18) =
        (Field<float>(camera, 0xAC) - Field<float>(camera, 0xA4)) / ToFloat(Field<uint32_t>(vp, 0x24)) * 0.5f;
    orig::RCamera_t_GetTransformationMatrix(camera, g_projectionMatrix);
    orig::render_t_SetTransformMatrix(Render(), 3, g_projectionMatrix);
    Internal<void(__fastcall*)(void*, void*, void*)>(0x2ADF7)(camera, nullptr, vp);   // its frustum
    orig::RandyShadowlandsData_s_SetCameraMatrix(orig::RRefFrame_t_GetWorldMatrix(camera));
    Global<void**>(kLightsEnd) = Global<void**>(kLightsBegin);   // FUN_1003ff5e: no lights yet
    Global<uint32_t>(0x17D28C) = 0;
    Global<void*>(0x1CEDE0) = nullptr;               // FUN_1004ccfa: the sun among them (none yet)
    reinterpret_cast<void(__fastcall*)(void*)>((*static_cast<void***>(root))[8])(root);   // Process
    for (void** l = Global<void**>(kLightsBegin); l != Global<void**>(kLightsEnd); ++l)
        orig::render_t_SetLight(Render(), Field<uint32_t>(*l, 0x110), static_cast<uint8_t*>(*l) + 0xA4);
    scene::AfterProcess();
}

// The render lists' vertex / index pools (FUN_100298ac / FUN_100298fb): one global copy, made by the first
// viewport and freed by its destructor. RViewPort_t's own destructor (FUN_1004b045) and deleting destructor
// (FUN_1004c7c8).
constexpr uint32_t kPoolVertices = 0x17BEF8, kPoolIndices = 0x17BF10, kPoolVerticesBytes = 0x17BF00,
                   kPoolIndicesBytes = 0x17BF0C, kPoolUse1 = 0x17BEFC, kPoolUse2 = 0x17BF08, kPoolUse3 = 0x17BF04,
                   kPoolUse4 = 0x17BF14, kPoolsExist = 0x1BB4E8, kViewportVtable = 0x958EC;

void __cdecl ScratchInit(int32_t vertices, int32_t indices)
{
    Global<void*>(kPoolIndices) = vc10::AllocateArray(size_t(indices) * 0xC);
    Global<void*>(kPoolVertices) = vc10::AllocateArray(size_t(vertices) * 0x28);
    Global<uint32_t>(kPoolVerticesBytes) = uint32_t(vertices) * 0x28;
    Global<uint32_t>(kPoolIndicesBytes) = uint32_t(indices) * 0xC;
    Global<uint32_t>(kPoolUse1) = 0;
    Global<uint32_t>(kPoolUse2) = 0;
    Global<uint32_t>(kPoolUse3) = 0;
    Global<uint32_t>(kPoolUse4) = 0;
}

void __cdecl ScratchFree()
{
    vc10::FreeArray(Global<void*>(kPoolIndices));
    vc10::FreeArray(Global<void*>(kPoolVertices));
}

void __fastcall Destroy(void* vp)   // FUN_1004b045
{
    Field<uintptr_t>(vp, 0) = reinterpret_cast<uintptr_t>(g_orig) + kViewportVtable;
    if (Global<uint8_t>(kPoolsExist) == 1) {
        ScratchFree();
        Global<uint8_t>(kPoolsExist) = 0;
    }
}

void* __fastcall Delete(void* vp, void*, uint8_t flags)   // FUN_1004c7c8
{
    Destroy(vp);
    if (flags & 1) vc10::Free(vp);
    return vp;
}

}  // namespace

void Install(HMODULE orig)
{
    if (GetMode("Scene", Mode::Off) != Mode::On) return;
    g_orig = orig;
    g_render = reinterpret_cast<void* const*>(GetProcAddress(orig, "?m_pcInstance@render_t@@0PAV1@A"));
    g_randy = reinterpret_cast<void* const*>(GetProcAddress(orig, "?s_pcRandy@Randy_t@@1PAV1@A"));
    g_debuggerMode = reinterpret_cast<const uint32_t*>(GetProcAddress(orig, "?m_nDebuggerMode@Debugger_t@@2IA"));
    g_viewMatrix = reinterpret_cast<float*>(GetProcAddress(orig, "?m_CurrentViewMatrix@RViewPort_t@@0VTMatrix4_t@@A"));
    g_projectionMatrix =
        reinterpret_cast<float*>(GetProcAddress(orig, "?m_CurrentProjectionMatrix@RViewPort_t@@0VTMatrix4_t@@A"));
    if (!g_render || !g_randy || !g_debuggerMode || !g_viewMatrix || !g_projectionMatrix) {
        Log("viewports: exports missing - not replaced");
        return;
    }
    struct Entry {
        uint32_t rva;
        void* target;
        const char* what;
    };
#define FN(f) reinterpret_cast<void*>(&f)
    const Entry entries[] = {
        {0x4B0C0, FN(Update), "RViewPort_t update (FUN_1004b0c0)"},
        {0x4BA7C, FN(SetTarget), "RViewPort_t target (FUN_1004ba7c)"},
        {0x4B199, FN(SetMaterial), "RViewPort_t::SetMaterial"},
        {0x4B3F6, FN(SetD3DMaterial), "RViewPort_t::SetD3DMaterial"},
        {0x4B61E, FN(SetDefaultMaterial), "RViewPort_t::SetDefaultMaterial"},
        {0x4B724, FN(ResetMaterial), "RViewPort_t::ResetMaterial"},
        {0x4B744, FN(Close), "RViewPort_t::Close"},
        {0x4B75A, FN(Clear), "RViewPort_t::Clear"},
        {0x4B7D9, FN(CalcPickLine), "RViewPort_t::CalcPickLine"},
        {0x4B95D, FN(RealizeRenderStates), "RViewPort_t::RealizeRenderStates"},
        {0x4B997, FN(SetCamera), "RViewPort_t::SetCamera"},
        {0x4B9A4, FN(Construct), "RViewPort_t::RViewPort_t"},
        {0x4BAAD, FN(Reposition), "RViewPort_t::Reposition"},
        {0x4BAD6, FN(SetViewRect), "RViewPort_t::SetViewRect"},
        {0x4BB4D, FN(Resize), "RViewPort_t::Resize"},
        {0x4BBCA, FN(Open), "RViewPort_t::Open"},
        {0x4BD1B, FN(TransformPointToScreenSpace), "RViewPort_t::TransformPointToScreenSpace"},
        {0x4BF39, FN(Process), "RViewPort_t::Process"},
        {0x4BFFF, FN(RenderLists), "RViewPort_t::Render"},
        {0x4C4EA, FN(RenderRefraction), "RViewPort_t::RenderRefraction"},
        {0x4B045, FN(Destroy), "RViewPort_t::~RViewPort_t (FUN_1004b045)"},
        {0x4C7C8, FN(Delete), "RViewPort_t deleting destructor (FUN_1004c7c8)"},
        {0x298AC, FN(ScratchInit), "RViewPort_t render pools (FUN_100298ac)"},
        {0x298FB, FN(ScratchFree), "RViewPort_t render pools free (FUN_100298fb)"},
    };
#undef FN
    int installed = 0;
    for (const Entry& e : entries) installed += Replace(orig, e.rva, e.target, e.what) ? 1 : 0;
    Log("viewports: %d of %d functions native", installed, int(sizeof(entries) / sizeof(entries[0])));
}

}  // namespace rnative::viewport
