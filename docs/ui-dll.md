# The interface plugin API

ao-vk draws the game's 3D, and GUI.dll its interface. An interface plugin - a DLL of its own, built against
`include/aovk_plugin.h` - can change how the interface looks and add windows of its own: ao-vk loads it, calls it
around GUI.dll's drawing, and draws what it hands over (glass behind windows, shapes, images, triangle meshes) in the
interface layer. ao-vk runs without one: the game's own interface.

## The rules

- **A plugin needs ao-vk.** It draws only through the host table (the renderer's interface layer, glass, shapes,
  meshes, images); it has no drawing of its own. ao-vk loads it with the Vulkan backend only.
- **ao-vk runs without a plugin.** No plugin, `[Plugins] Load=off`, or one that refuses to attach: ao-vk logs it
  (`plugins:` lines) and goes on with the game's own interface and the interface refresh rate (`RVK_UiRate`) - and
  no glass, as nothing asks for it.
- **The plugin decides the look; ao-vk only draws it.** Glass (`setGlass`, `window`), shapes and images come from the
  plugin. The renderer keeps a veto: `RVK_UiSkin` ("Interface look from the interface plugin", off in the Original
  preset), which the plugin asks once a frame (`lookAllowed`): off, it is to draw nothing of its own.
- **One owner per patched function.** ao-vk hooks GUI.dll's `WindowController_c::Render` and `View::_CallRender`
  (proxy/native/ui_layer.cpp) and calls the plugin from them; a plugin must not hook those two, and may hook any other
  GUI.dll entry itself. Two detours on one entry would fail (the second's check finds the first's jump).
- **One plugin at a time.** The first in the folder (by file name) that attaches; the others are logged and left.

## Loading

ao-vk's `Init()` (proxy/trace.cpp) runs on the game's first call into randy31 - outside `DllMain`, before GUI.dll
makes its fonts - and loads plugins there (proxy/ddraw/rvk_plugin.cpp):

1. `randy-vk.ini` `[Plugins] Load` (written `on` when absent): `off` - none loaded.
2. Every `*.dll` in `ao-vk-plugins\` next to randy31.dll, by file name, until one attaches. A DLL without the export
   `AoVkPluginAttach` isn't a plugin (logged, unloaded).
3. `AoVkPluginAttach(host, plugin)` (`__cdecl`): the plugin checks the host table's version and size and whatever
   else it depends on (the client build), fills in its table and returns non-zero - or zero, and ao-vk unloads it.
   GUI.dll may or may not be loaded yet.
4. The crash log watches the plugin's module from then on (faults with its module + offset in randy-vk.log).

While a plugin is attached, ao-vk hooks `Render` / `_CallRender` every session (`InterfaceHooksWanted`).

## The tables

Plain C: no C++ types, exceptions or allocations cross over (images are handles the host owns; the plugin frees them
through the table). Everything runs on the game thread.

**The host's table** (`AoVkHost`, filled by ao-vk):

| Entry | |
|---|---|
| `log(fmt, ...)` | a line in randy-vk.log, `plugin:` prefixed |
| `lookAllowed()` | `RVK_UiSkin` in effect: 0 - the game's own look wanted |
| `setGlass(const AoVkGlass*)` | the glass's look (blur, dim, tint, edge, radius, tint and rim colours; `enable` 0: none), kept until changed; in effect while `RVK_UiSkin` is on |
| `window(x0, y0, x1, y1)` | while the interface is drawn: glass behind this rectangle (screen pixels) this frame |
| `shapes(shapes, count, meshes)` | while the interface is drawn: rounded rectangles (`AoVkUiShape`: gradient fill, border, glow, clip, or an image part) and / or triangle meshes (`AoVkUiMeshes`: RmlUi's vertex layout, premultiplied, each mesh translated, clipped, textured, faded), where GUI.dll's drawing has got to - at its depth, so windows in front stay in front |
| `image(bgra, w, h)` / `imageFree(image)` | an image from BGRA pixels (A8R8G8B8, tightly packed) for shapes and meshes; null when there's no renderer yet |

**The plugin's table** (`AoVkPlugin`, filled by `AoVkPluginAttach`; any entry may be null):

| Entry | |
|---|---|
| `frame()` | once a presented frame, before ao-vk's own per-frame work: the plugin's own (hooking GUI.dll once it is loaded, its settings, hotkeys) |
| `interfaceBegin(controller, drawn)` / `interfaceEnd(controller, drawn)` | around `WindowController_c::Render`; `drawn` 0: `RVK_UiRate` shows the last interface drawn - no view calls come between them |
| `viewBegin(view, depth, call)` | before each `View::_CallRender` (`AoVkViewCall`: its arguments; `depth` 0 a window's top-level view); non-zero: the plugin drew the view - GUI.dll's `_CallRender` (and `viewEnd`) isn't called |
| `viewEnd(view, depth, call)` | after it |

**Versions.** `AOVK_PLUGIN_VERSION` (1) is raised whenever a table or struct changes; new fields go at the end only.
Each table carries its `version` and `size`: a plugin refuses a host table smaller than it needs; ao-vk copies only
as much of the plugin's table as it gave (entries it doesn't know stay null).

## Building a plugin

A plugin is a 32-bit Windows DLL with the static CRT, built with ao-vk's toolchain (`cmake/xwin-linux.cmake`). It
includes `include/aovk_plugin.h`, and may compile ao-vk's `proxy/native/hooklib.cpp` (HookAt, CheckedEntry for
GUI.dll's exports, trampolines) and `vc10.cpp` (msvcr100's heap and VS2010 container layouts, for the game's
objects) into itself - it then defines `rnative::Log` (through the host's `log`).

Installed by putting the DLL into the client's `ao-vk-plugins\` folder; removed by taking it out.
