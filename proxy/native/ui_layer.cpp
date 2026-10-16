// GUI.dll's interface drawing (ui_layer.h).
//
// WindowController_c::Render runs once a frame: the interface's per-frame work (which window is under the mouse, the
// mouse pointer's shape, the tooltip timer), then View::_CallRender for each top-level window - the drawing: it walks
// the window's view tree (recursing into itself) and commits each surface's quads through Randy. Both are exports.
// Render is wrapped to tell the renderer where the interface begins and ends; a frame the renderer doesn't want it
// redrawn (it shows its layer: the last one drawn), the top-level _CallRender calls return at once - the per-frame
// work still runs every frame. Layout a view does while drawing (ViewSurface_c::_LayoutSelf) waits for the next
// drawn frame. The plugin is told around both (aovk_plugin.h): it may draw a view itself instead of GUI.dll.
#include "native/ui_layer.h"

#include "native/device.h"
#include "native/native.h"

namespace rnative::uilayer {

namespace {

using RenderFn = void(__fastcall*)(void* self, void* edx, void* viewport, uint32_t time);
using CallRenderFn = void(__fastcall*)(void* view, void* edx, uint32_t order, void* viewport, const void* clip,
                                       uint32_t dirty, uint32_t alpha, uint32_t color);
RenderFn g_render;
CallRenderFn g_callRender;
int g_depth;                                        // _CallRender nesting (it recurses over the view tree)
bool g_skip;                                        // this frame's interface isn't drawn
const AoVkPlugin* g_plugin;

// WindowController_c::Render (thiscall: this in ecx, two stack arguments).
void __fastcall RenderHook(void* self, void* edx, void* viewport, uint32_t time)
{
    g_skip = !device::InterfaceBegin();
    g_depth = 0;
    const AoVkPlugin* p = g_plugin;
    if (p && p->interfaceBegin)
        p->interfaceBegin(self, !g_skip);
    g_render(self, edx, viewport, time);
    if (p && p->interfaceEnd)
        p->interfaceEnd(self, !g_skip);
    g_skip = false;
    device::InterfaceEnd();
}

// View::_CallRender (thiscall, six stack arguments).
void __fastcall CallRenderHook(void* view, void* edx, uint32_t order, void* viewport, const void* clip, uint32_t dirty,
                               uint32_t alpha, uint32_t color)
{
    if (g_depth == 0 && g_skip)
        return;
    const AoVkPlugin* p = g_plugin;
    const AoVkViewCall call{order, viewport, clip, dirty, alpha, color};
    const int depth = g_depth;
    if (p && p->viewBegin && p->viewBegin(view, depth, &call))
        return;
    g_depth = depth + 1;
    g_callRender(view, edx, order, viewport, clip, dirty, alpha, color);
    g_depth = depth;
    if (p && p->viewEnd)
        p->viewEnd(view, depth, &call);
}

}  // namespace

void SetPlugin(const AoVkPlugin* plugin) { g_plugin = plugin; }

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
    Log("interface: GUI.dll %s", g_render ? "hooked (RVK_UiRate can redraw it less often; the plugin draws in it)"
                                          : "not hooked");
}

}  // namespace rnative::uilayer
