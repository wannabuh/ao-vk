// GUI.dll's interface drawing, for the interface layer (docs/rvk.md "Interface refresh rate", RVK_UiRate).
//
// WindowController_c::Render runs once a frame: the interface's per-frame work (which window is under the mouse, the
// mouse pointer's shape, the tooltip timer), then View::_CallRender for each top-level window - the drawing: it walks
// the window's view tree (recursing into itself) and commits each surface's quads through Randy. Both are exports.
// Render is wrapped to tell the renderer where the interface begins and ends; a frame the renderer doesn't want it
// redrawn (it shows its layer: the last one drawn), the top-level _CallRender calls return at once - the per-frame
// work still runs every frame. Layout a view does while drawing (ViewSurface_c::_LayoutSelf) waits for the next
// drawn frame.
#include "native/gui.h"

#include "native/device.h"
#include "native/native.h"

#include <cstring>

namespace rnative::gui {

namespace {

using RenderFn = void(__fastcall*)(void* self, void* edx, void* viewport, uint32_t time);
using CallRenderFn = void(__fastcall*)(void* view, void* edx, uint32_t order, void* viewport, const void* clip,
                                       uint32_t dirty, uint32_t alpha, uint32_t color);
RenderFn g_render;
CallRenderFn g_callRender;
int g_depth;                                        // _CallRender nesting (it recurses over the view tree)
bool g_skip;                                        // this frame's interface isn't drawn

// WindowController_c::Render (thiscall: this in ecx, two stack arguments).
void __fastcall RenderHook(void* self, void* edx, void* viewport, uint32_t time)
{
    g_skip = !device::InterfaceBegin();
    g_depth = 0;
    g_render(self, edx, viewport, time);
    g_skip = false;
    device::InterfaceEnd();
}

// View::_CallRender (thiscall, six stack arguments).
void __fastcall CallRenderHook(void* view, void* edx, uint32_t order, void* viewport, const void* clip, uint32_t dirty,
                               uint32_t alpha, uint32_t color)
{
    if (g_depth == 0 && g_skip)
        return;
    ++g_depth;
    g_callRender(view, edx, order, viewport, clip, dirty, alpha, color);
    --g_depth;
}

// Both start with MSVC's exception frame prologue: mov eax, <handler>; call <GUI.dll's _EH_prolog> - checked, so a
// different GUI.dll is left alone. The ten bytes run from the trampoline (the call's target re-aimed).
uint8_t* CheckedEntry(HMODULE gui, const char* name, const char* what)
{
    auto* at = reinterpret_cast<uint8_t*>(GetProcAddress(gui, name));
    if (!at) {
        Log("interface: %s not exported - not hooked", what);
        return nullptr;
    }
    constexpr uint32_t kEhProlog = 0x1738A4;        // FUN_101738a4 in GUI.dll (image base 0x10000000)
    int32_t rel;
    std::memcpy(&rel, at + 6, 4);
    const uint8_t* callee = at + 10 + rel;
    if (at[0] != 0xB8 || at[5] != 0xE8 || callee != reinterpret_cast<uint8_t*>(gui) + kEhProlog) {
        Log("interface: %s - unexpected code (another GUI.dll build?), not hooked", what);
        return nullptr;
    }
    return at;
}

}  // namespace

void Install()
{
    static int attempts;
    if (g_render || attempts >= 600)
        return;
    ++attempts;
    HMODULE gui = GetModuleHandleA("GUI.dll");
    if (!gui)
        return;                                     // not loaded yet: next frame
    attempts = 600;
    uint8_t* render = CheckedEntry(gui, "?Render@WindowController_c@@UAEXPAVRViewPort_t@@M@Z", "WindowController_c::Render");
    uint8_t* callRender =
        CheckedEntry(gui, "?_CallRender@View@@AAEXIPAVRViewPort_t@@ABVRect@@_NMI@Z", "View::_CallRender");
    if (!render || !callRender)
        return;
    static const size_t kRel[] = {6};
    // _CallRender first: Render's hook skips through it.
    g_callRender = reinterpret_cast<CallRenderFn>(
        HookAt(callRender, 10, kRel, 1, reinterpret_cast<void*>(&CallRenderHook), "View::_CallRender"));
    if (!g_callRender)
        return;
    g_render = reinterpret_cast<RenderFn>(
        HookAt(render, 10, kRel, 1, reinterpret_cast<void*>(&RenderHook), "WindowController_c::Render"));
    Log("interface: GUI.dll %s", g_render ? "hooked (RVK_UiRate can redraw it less often)" : "not hooked");
}

}  // namespace rnative::gui
