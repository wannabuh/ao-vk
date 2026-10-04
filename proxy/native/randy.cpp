// Randy_t's frame-level functions natively (part of [Native] Device=on): presenting (Flip), render targets and their
// stack, fog / W-buffer / ambient light, buffer and capability queries. Device creation (Initialize) is not here yet.
//
// Randy_t: +0x004 fog end, +0x008 W-buffer on, +0x194 / +0x1CC device / HAL raster caps, +0x242 texture stages,
// +0x274 frames presented, +0x278 the frame textures were last aged, +0x27C its DeviceState, +0x280 the render
// target set, +0x284 the one asked for, +0x288 features in use, +0x28C std::list<int> pushed render targets.
// Globals: 0x1017D2F8 RenderTarget_t*[7] (0 = the back buffer; first field its surface_t*), 0x1017D2F4 the primary
// surface (windowed), 0x1017D320 the window, 0x1017D2A4 its screen rectangle.
#include "native/randy.h"
#include "native/orig_api.gen.h"
#include "native/vc10.h"

#include <cstdio>
#include <cstring>

namespace rnative::randy {

namespace {

HMODULE g_orig;
void* const* g_render;                              // render_t::m_pcInstance
uint32_t* g_debuggerMode;

template <typename T>
T& Global(uint32_t rva) { return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(g_orig) + rva); }
template <typename T>
T& Field(void* object, uint32_t offset) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(object) + offset); }
template <typename F>
F Internal(uint32_t rva) { return reinterpret_cast<F>(reinterpret_cast<uint8_t*>(g_orig) + rva); }

void** RenderTargets() { return &Global<void*>(0x17D2F8); }
void* Devicestate(void* randy) { return Field<void*>(randy, 0x27C); }

constexpr uint32_t kHasWindow = 0x17D320, kWindowRect = 0x17D2A4, kPrimary = 0x17D2F4, kFullscreenFlip = 0x17D31C;
constexpr uint32_t kWaitForVSync = 0xB7738, kHardwareLevel = 0xB772C, kAlwaysValidate = 0xB72F4;
constexpr uint32_t kEmulationCap = 0xB7730, kEmulationCapUser = 0xB7734, kUseOffscreen = 0x17D333;

void SetState(void* randy, uint32_t state, uint32_t value)
{
    orig::DeviceState_SetRenderState(Devicestate(randy), int32_t(state), value, 10);
}

// FUN_10041566: gives up an offscreen feature (its emulation cap bit) and render target `index` (1..6).
void DropRenderTarget(int index)
{
    static const uint32_t kCapBits[7] = {0, 1, 2, 8, 0x10, 0x20, 0x40};
    if (index < 1 || index > 6) return;
    Global<uint32_t>(kEmulationCap) &= ~kCapBits[index];
    void*& target = RenderTargets()[index];
    if (target) {
        void** surfaces = static_cast<void**>(target);
        if (surfaces[1]) {
            orig::surface_t_ReleaseDXSurface(surfaces[1]);
            surfaces[1] = nullptr;
        }
        if (surfaces[0]) {
            orig::surface_t_ReleaseDXSurface(surfaces[0]);
            surfaces[0] = nullptr;
        }
        vc10::Free(target);
        target = nullptr;
    }
}

void* __fastcall GetBackBuffer(void* randy)
{
    void* target = RenderTargets()[Field<int32_t>(randy, 0x280)];
    if (!target) target = RenderTargets()[0];
    return *static_cast<void**>(target);
}

void* __fastcall GetDDSurface(void* randy)
{
    if (Global<HWND>(kHasWindow)) return Global<void*>(kPrimary);
    return GetBackBuffer(randy);
}

void* __cdecl GetRenderTarget(int32_t index) { return RenderTargets()[index]; }

bool __fastcall IsDeviceLost(void*)
{
    void* render = *g_render;
    void* dd = Field<void*>(render, 8);             // the IDirectDraw7: TestCooperativeLevel
    if (!dd) return false;
    return reinterpret_cast<HRESULT(__stdcall*)(void*)>((*static_cast<void***>(dd))[0x68 / 4])(dd) != 0;
}

void __fastcall EnableWBuffer(void* randy, void*, bool on)
{
    // D3DPRASTERCAPS_WBUFFER in both the device's and the HAL's caps.
    if (!(Field<uint32_t>(randy, 0x1CC) & 0x40000) || !(Field<uint32_t>(randy, 0x194) & 0x40000)) return;
    Field<uint8_t>(randy, 8) = on;
    SetState(randy, 7, on ? 2 : 1);                 // D3DRENDERSTATE_ZENABLE: D3DZB_USEW / TRUE
    if (!(*g_debuggerMode & 0x100))                 // and on the device right away (FUN_1002397c)
        if (void* device = *static_cast<void**>(*g_render))
            reinterpret_cast<HRESULT(__stdcall*)(void*, DWORD, DWORD)>((*static_cast<void***>(device))[0x50 / 4])(
                device, 7, Field<uint8_t>(randy, 8) ? 2 : 1);
}

void __fastcall SetAmbientLight(void* randy, void*, const void* rgb)
{
    uint32_t color;
    orig::Color_t_Color_t_14(&color, rgb);
    SetState(randy, 0x8B, color);                   // AMBIENT
}

void __fastcall EnableFog(void* randy, void*, bool on) { SetState(randy, 0x1C, on ? 1 : 0); }   // FOGENABLE

uint32_t FogColor(const float* rgb)                 // FUN_1004179f
{
    const uint32_t r = uint32_t(int32_t(double(rgb[0]) * 255.0));
    const uint32_t g = uint32_t(int32_t(double(rgb[1]) * 255.0));
    const uint32_t b = uint32_t(int32_t(double(rgb[2]) * 255.0));
    return ((r | 0xFFFFFF00u) << 8 | g) << 8 | b;
}

// Linear vertex fog from `start` to `end` (clamped to `limit` when the driver needs it).
void __fastcall SetFogParameters(void* randy, void*, float start, float end, float limit, float, const float* color,
                                 bool, bool)
{
    if (end > limit && Global<uint8_t>(0x17D31D)) end = limit;
    Field<float>(randy, 4) = end;
    SetState(randy, 0x1C, 1);                       // FOGENABLE
    SetState(randy, 0x22, FogColor(color));         // FOGCOLOR
    SetState(randy, 0x8C, 3);                       // FOGVERTEXMODE: LINEAR
    SetState(randy, 0x23, 0);                       // FOGTABLEMODE: NONE
    uint32_t bits;
    std::memcpy(&bits, &start, 4);
    SetState(randy, 0x24, bits);                    // FOGSTART
    std::memcpy(&bits, &end, 4);
    SetState(randy, 0x25, bits);                    // FOGEND
}

void __fastcall SetRenderTargetAsTexture(void* randy, void*, int32_t index, uint32_t stage)
{
    void* target = RenderTargets()[index];
    orig::DeviceState_SetTexture(Devicestate(randy), target ? *static_cast<void**>(target) : nullptr, stage, 10);
}

uint32_t __fastcall GetNumColorBits(void*) { return Global<uint32_t>(0x17D324); }
uint32_t __fastcall GetNumZBufferBits(void*) { return Global<uint32_t>(0x17D328); }
uint32_t __fastcall GetNumStencilBits(void*) { return Global<uint32_t>(0x17D32C); }
bool __fastcall NeedsClearFix(void*) { return Global<uint8_t>(0x17D332) != 0; }

uint16_t __fastcall GetTextureStageCount(void* randy)
{
    const uint16_t stages = Field<uint16_t>(randy, 0x242);
    if (Global<uint8_t>(0x17D31E)) return uint16_t((stages > 1 ? 1 : 0) + 1);   // at most two
    return stages;
}

void __fastcall GetBufferSize(void*, void*, uint32_t* width, uint32_t* height)
{
    const uint8_t* desc = static_cast<const uint8_t*>(orig::surface_t_GetSurfaceDesc(*static_cast<void**>(RenderTargets()[0])));
    *width = Field<uint32_t>(const_cast<uint8_t*>(desc), 0xC);
    *height = Field<uint32_t>(const_cast<uint8_t*>(desc), 8);
}

bool __fastcall FlushTextures(void*)
{
    Internal<void(__fastcall*)(void*)>(0x23E14)(*g_render);   // render_t: EvictManagedTextures
    return true;
}

bool __fastcall GetMemory(void*, void*, uint32_t* total, uint32_t* free)
{
    uint32_t caps[0x5F] = {0x17C};                  // render_t::GetCaps of the IDirectDraw7 (DDCAPS, dwSize)
    Internal<void(__fastcall*)(void*, void*, void*, void*)>(0x21073)(*g_render, nullptr, caps, nullptr);
    *total = caps[0x3C / 4];
    *free = caps[0x40 / 4];
    return true;
}

void __fastcall RestoreData(void*) { ++Global<uint32_t>(0x17D334); }   // Randy_t::s_nRestoreCount
void __cdecl SetWaitForVSync(bool on) { Global<uint8_t>(kWaitForVSync) = on; }
void __cdecl SetUseOffscreenTech(bool on) { Global<uint8_t>(kUseOffscreen) = on; }

// Makes render target `index` (0 = the back buffer) the viewport's.
bool __fastcall SetRenderTarget(void* randy, void*, void* viewport, int32_t index)
{
    if (Field<int32_t>(randy, 0x280) != index) {
        if (!RenderTargets()[index]) return false;
        Field<int32_t>(randy, 0x280) = index;
        Field<int32_t>(randy, 0x284) = index;
        Internal<void(__fastcall*)(void*, void*, void*)>(0x4BA7C)(viewport, nullptr, GetBackBuffer(randy));
    }
    return true;
}

// The pushed render targets: a VS2010 std::list<int> (head node: next, prev; size after it).
struct ListNode {
    ListNode* next;
    ListNode* prev;
    int32_t value;
};

bool __fastcall PushRenderTarget(void* randy, void*, void*, int32_t index)
{
    if (Field<int32_t>(randy, 0x280) != index && !RenderTargets()[index]) return false;
    ListNode* head = Field<ListNode*>(randy, 0x28C);
    ListNode* first = head->next;
    auto* node = static_cast<ListNode*>(vc10::Allocate(sizeof(ListNode)));
    *node = ListNode{first, first->prev, Field<int32_t>(randy, 0x284)};
    ++Field<uint32_t>(randy, 0x290);
    first->prev = node;
    node->prev->next = node;
    Field<int32_t>(randy, 0x284) = index;
    return true;
}

void __fastcall PopRenderTarget(void* randy, void*, void*)
{
    ListNode* head = Field<ListNode*>(randy, 0x28C);
    ListNode* first = head->next;
    if (first == head) std::printf("Render-target list is empty.\n");
    Field<int32_t>(randy, 0x284) = first->value;    // (the head's, when empty - as the original)
    if (first != head) {
        first->prev->next = first->next;
        first->next->prev = first->prev;
        vc10::Free(first);
        --Field<uint32_t>(randy, 0x290);
    }
}

// Which offscreen features the scene asks for (bits of +0x288), given what the device can emulate.
void __fastcall SetFeatureUsage(void* randy, void*, uint32_t features)
{
    if (!Global<uint8_t>(kUseOffscreen)) {
        for (int i = 1; i < 5; ++i) DropRenderTarget(i);
        return;
    }
    auto make = Internal<void(__cdecl*)(int)>(0x42732);   // FUN_10042732: creates render target n
    const uint32_t caps = Global<uint32_t>(kEmulationCapUser) & Global<uint32_t>(kEmulationCap);
    uint32_t& used = Field<uint32_t>(randy, 0x288);
    struct Feature {
        uint32_t bit;
        int targets[2];
        uint32_t needs;
    };
    static const Feature kFeatures[] = {{0x80, {1, 2}, 3}, {0x40, {1, 2}, 3}, {0x20, {1, 2}, 3}, {0x10, {1, 0}, 1},
                                        {0x08, {1, 0}, 1}, {0x100, {3, 0}, 8}, {0x200, {4, 0}, 0x10}};
    for (const Feature& f : kFeatures) {
        if (!(features & f.bit)) continue;
        for (int t : f.targets)
            if (t) make(t);
        if ((caps & f.needs) == f.needs) used |= f.bit;
    }
}

void __fastcall Flip(void* randy, void*, bool noWait)
{
    const bool vsync = Global<uint8_t>(kWaitForVSync) && !noWait && Global<int32_t>(kHardwareLevel) != 0;
    if (IsDeviceLost(randy))
        for (int i = 1; i < 7; ++i) DropRenderTarget(i);
    const uint32_t frame = ++Field<uint32_t>(randy, 0x274);
    if (frame - Field<uint32_t>(randy, 0x278) > 0x4B0) {   // textures unused for 1000 frames are let go
        Field<uint32_t>(randy, 0x278) = frame;
        Internal<void(__cdecl*)(uint32_t, uint32_t)>(0x4EC5B)(frame, 1000);
    }
    const uint32_t mode = *g_debuggerMode;
    *g_debuggerMode = mode | 0x10000;
    if (mode & 0x8000) {                            // debugger: touch the offscreen targets in turn
        void** target = static_cast<void**>(Global<void*>((frame & 1) == 0 ? 0x17D310 : 0x17D30C));
        if (target) {
            void* surface = target[0];
            void* dds = surface ? *static_cast<void**>(surface) : nullptr;
            orig::render_t_LockTexture(*g_render, dds, nullptr, orig::surface_t_GetSurfaceDesc(surface), 0x10, nullptr);
            orig::render_t_UnlockTexture(*g_render, dds, nullptr);
        }
    }
    if (Global<uint8_t>(kFullscreenFlip) == 1) {
        Internal<void(__fastcall*)(void*, void*, void*, void*, uint32_t)>(0x2428E)(   // render_t: flip the chain
            *g_render, nullptr, *static_cast<void**>(Global<void*>(kPrimary)), nullptr, vsync ? 0x20u : 0x28u);
    } else {
        HWND window = Global<HWND>(kHasWindow);
        RECT& rect = Global<RECT>(kWindowRect);
        if (window) {
            GetClientRect(window, &rect);
            if (rect.right - rect.left < 2 || rect.bottom - rect.top < 2) return;
            ClientToScreen(window, reinterpret_cast<POINT*>(&rect.left));
            ClientToScreen(window, reinterpret_cast<POINT*>(&rect.right));
            Internal<void(__fastcall*)(void*, void*, RECT*)>(0x20C0D)(*g_render, nullptr, &rect);   // clipper
        }
        if (vsync)                                  // render_t: WaitForVerticalBlank(DDWAITVB_BLOCKBEGIN)
            Internal<void(__fastcall*)(void*, void*, uint32_t, void*)>(0x2376A)(*g_render, nullptr, 1, nullptr);
        void* primary = Global<void*>(kPrimary);
        orig::render_t_Blt(*g_render, *static_cast<void**>(primary), &rect, *static_cast<void**>(GetBackBuffer(randy)),
                           nullptr, 0x8000000, nullptr);
    }
    if (IsDeviceLost(randy)) {                      // restore everything
        Internal<void(__fastcall*)(void*)>(0x23E14)(*g_render);
        Internal<void(__cdecl*)(uint32_t, uint32_t)>(0x4EC5B)(Field<uint32_t>(randy, 0x274), 0);
        orig::DeviceState_UpdateDevice(Devicestate(randy));
        orig::DeviceState_UpdateDevice(Devicestate(randy));
        Internal<void(__fastcall*)(void*)>(0x41EDE)(randy);
        Internal<void(__fastcall*)(void*)>(0x1BD36)(Devicestate(randy));
        Global<int32_t>(kAlwaysValidate) = 3;
    }
}

}  // namespace

void Install(HMODULE orig)
{
    g_orig = orig;
    g_render = reinterpret_cast<void* const*>(GetProcAddress(orig, "?m_pcInstance@render_t@@0PAV1@A"));
    g_debuggerMode = reinterpret_cast<uint32_t*>(GetProcAddress(orig, "?m_nDebuggerMode@Debugger_t@@2IA"));
    struct Entry {
        uint32_t rva;
        void* target;
        const char* what;
    };
#define FN(f) reinterpret_cast<void*>(&f)
    const Entry entries[] = {
        {0x4170C, FN(EnableWBuffer), "Randy_t::EnableWBuffer"},
        {0x41755, FN(SetAmbientLight), "Randy_t::SetAmbientLight"},
        {0x4177F, FN(EnableFog), "Randy_t::EnableFog"},
        {0x41876, FN(SetFogParameters), "Randy_t::SetFogParameters"},
        {0x418EE, FN(GetDDSurface), "Randy_t::GetDDSurface"},
        {0x41931, FN(SetRenderTargetAsTexture), "Randy_t::SetRenderTargetAsTexture"},
        {0x41951, FN(GetBackBuffer), "Randy_t::GetBackBuffer"},
        {0x41984, FN(GetNumColorBits), "Randy_t::GetNumColorBits"},
        {0x4198D, FN(GetNumZBufferBits), "Randy_t::GetNumZBufferBits"},
        {0x41993, FN(GetNumStencilBits), "Randy_t::GetNumStencilBits"},
        {0x4199D, FN(GetTextureStageCount), "Randy_t::GetTextureStageCount"},
        {0x419BD, FN(GetBufferSize), "Randy_t::GetBufferSize"},
        {0x41CA7, FN(FlushTextures), "Randy_t::FlushTextures"},
        {0x41CB5, FN(GetMemory), "Randy_t::GetMemory"},
        {0x41DC0, FN(IsDeviceLost), "Randy_t::IsDeviceLost"},
        {0x41E99, FN(NeedsClearFix), "Randy_t::NeedsClearFix"},
        {0x41E9F, FN(RestoreData), "Randy_t::RestoreData"},
        {0x41EC4, FN(SetWaitForVSync), "Randy_t::SetWaitForVSync"},
        {0x41ED1, FN(SetUseOffscreenTech), "Randy_t::SetUseOffscreenTech"},
        {0x42F1C, FN(SetRenderTarget), "Randy_t::SetRenderTarget"},
        {0x42F76, FN(Flip), "Randy_t::Flip"},
        {0x431C7, FN(SetFeatureUsage), "Randy_t::SetFeatureUsage"},
        {0x43319, FN(PopRenderTarget), "Randy_t::PopRenderTarget"},
        {0x43504, FN(PushRenderTarget), "Randy_t::PushRenderTarget"},
        {0x11A8C, FN(GetRenderTarget), "Randy_t::GetRenderTarget"},
    };
#undef FN
    int installed = 0;
    for (const Entry& e : entries) installed += Replace(orig, e.rva, e.target, e.what) ? 1 : 0;
    Log("Randy_t: %d of %d functions native", installed, int(sizeof(entries) / sizeof(entries[0])));
}

}  // namespace rnative::randy
