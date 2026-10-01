# How a frame flows through Randy

From the decompiled code in `re/` (regenerate with `ghidra-scripts/DumpDecomp.java`). Addresses are
the original image bases: randy31 0x10000000, DisplaySystem 0x10000000 (separate images).

## 0. Who calls whom

The game executable is `AnarchyOnline.exe` (it constructs `D3DDisplaySystem_t`; it already has
IMAGE_FILE_LARGE_ADDRESS_AWARE). The per-frame chain found so far:

```
GUI.dll  SpriteList_t::ParseList            UI views update, SpriteRenderSystem_t::FrameProcess
  DisplaySystem_t::Commit (export)          calls this+0x24 -> vtable+4, then:
    DisplaySystem!FUN_10079a2e              the 3D frame below
DisplaySystem!FUN_10001cdb (virtual)        Randy_t::Flip
```

The UI itself is drawn by GUI.dll through `WindowController_c::Render` (virtual, so the caller is not
visible statically) → `View::_CallRender` (recursive over the view tree) →
`ViewSurface_c::_CommitRendering(RViewPort_t*)`: batched screen-space quads, FVF 0x144
(XYZRHW | DIFFUSE | TEX1, 28 bytes), from `DynamicVB_c`, grouped by `StateBlob_c` (3 blobs at GUI
0x102767A0, 0x84 bytes each), texture and material, drawn with `render_t::RenderTriangleList`
(indexed). Other GUI draws: `BrowserView_c::Render` (in-game browser, triangle strip) and a few virtual
widget methods (triangle fans, line lists; probably map/radar). Where exactly the UI is drawn relative
to the 3D passes is for the frame inspector to show.

`DisplaySystem!FUN_10079a2e` (the per-frame render function):

1. `RenderStats_t::Reset`; if `Randy_t::IsDeviceLost` → remember and return. After a lost device:
   `render_t::RestoreAllSurfaces` + DisplaySystem's own restore.
2. `RViewPort_t::Open` restores lost surfaces (DDERR_SURFACELOST = 0x887601C2), clears on restore,
   then BeginScene.
3. `RViewPort_t::Process(RandyRoot_t::GetRoot())` is the scene update (see 2).
4. Picks a pass sequence from `Randy_t+0x288` (feature flags, masked `& 0xFFFFFEF8`): `FUN_100793b8`
   (main, offscreen + refraction), `FUN_1007957f` (flag 0x200), `FUN_100796d9`, `FUN_100797f3`.
5. Optional debug pass if `Randy_t+0x288 & 1`.
6. `RViewPort_t::Close` (EndScene), `DynamicVB_c::Reset`.

Presenting is separate: `DisplaySystem!FUN_10001cdb` → `Randy_t::Flip(false)`.

## 2. Scene update: `RViewPort_t::Process`

Sets the view (`D3DTRANSFORMSTATE_VIEW` = 2) and projection (3) from the camera, writes the static
`RViewPort_t::m_CurrentViewMatrix` / `m_CurrentProjectionMatrix`, gives the camera to
`RandyShadowlandsData_s`, then calls the root frame's virtual `Process` (vtable +0x20). That
recurses through the `RRefFrame_t` tree. `RVisual_t::Process`:

- calls `RestoreData` (+0x4C) if `Randy_t::s_nRestoreCount` changed since last time (device restore),
- fades `+0x0C` (current alpha) towards `+0x10` (target) by a fixed step,
- if render list `+0xE4` != -1 and visible flag `+0x94`: `AddToRenderList(list, -1)`. Visuals with
  alpha < 1 always go to list 6 (transparent).

`RVisual_t::AddToRenderList(list, bucket)`: bucket -1 means "by camera distance": 0..799 linear in
distance, beyond that +800, clamped to 0..1799 (0x707). The visual is pushed onto the front of the
singly linked list `g_renderLists[list][bucket]` (next pointer at `RVisual_t+0xE8`).
`g_renderLists` = randy31 0x101CEDE8, 11 lists × 1800 buckets × 4 bytes, cleared after each frame.

## 3. Drawing: `RViewPort_t::Render(listFrom, listTo, flags, bucketFrom, bucketTo)`

Walks lists listFrom..listTo, and within each list buckets bucketFrom..bucketTo in that direction
(so front-to-back or back-to-front), calling **vtable slot 13 (+0x34) `Render(RViewPort_t&)`** on
every visual. That slot is the draw entry point every DisplaySystem visual overrides (empty in
`RVisual_t`). `flags & 4` ends the frame's list use (clears `g_renderLists`); `flags & 8` = debug.

The common implementation many visuals call is `RVisual_t::Rasterize(viewport, RVisualData_t*,
sortByDistance, ...)`: pushes scoped render states, optional texture-matrix animation
(`D3DTSS_TEXTURETRANSFORMFLAGS`, `D3DTRANSFORMSTATE_TEXTURE0/1`), virtual `StoreStateChanges`
(+0x40) / `ApplyStateChanges` (+0x44), then `RenderWithTransparency` per `SimpleMesh`
(optionally sorted by distance) → `render_t::RenderTriangleList(VertexBuffer_c*, ...)`, then
`RestoreStateChanges` (+0x48). Scoped render-state helpers: randy31 `FUN_1001237d` (push
state, value, priority) / `FUN_100123c0` (pop).

## 4. Render lists

From every `RVisual_t::SetRenderPriority` call in DisplaySystem and Gamecode:

| List | Who | Notes |
|---|---|---|
| 0 | environment/backdrop objects (`GenericVisualObject_t`, `EnvironmentLightObject_t`, `LineMonitor_t`, …), 19 sites | drawn first |
| 1 | terrain `AnarchyGround_t`, indoor `VisualRoom_t`, 1 Gamecode site | opaque world |
| 2 | `DirectionalLight_t`, one more | |
| 3 | static meshes (`GenericMeshObject_t`, rocks, ships), 3 Gamecode sites | opaque objects, likely characters too |
| 4 | water `VisualLiquid_t`, one more | also drawn by `RenderRefraction` |
| 5 | lightning, boosters, some effects | |
| 6 | most particle/beam/sprite effects (50 sites); any fading visual | transparent |
| 7 | `GfxVisualMeshTest`, `RLineCollection_t`, a Gamecode site | overlay, after the composite |
| 8 | volume fog/dots | not drawn by the main pass sequence |
| 9 | `VisualOffscreen2Refraction_t` | copies the scene for refraction |
| 10 | `VisualOffscreen2Screen_t` | offscreen scene → back buffer |

## 5. Main pass sequence (`DisplaySystem!FUN_100793b8`)

Render targets are `Randy_t::SetRenderTarget(viewport, n)`: 0 = back buffer, 1 = offscreen scene,
2 = refraction copy, 5/6 = debug; up to 7 slots at randy31 0x1017D2F8.

```
RT1: Clear
     Render lists 0-2, buckets 0→1799        environment, terrain/rooms
     Render lists 3-4, buckets 500→1799      far opaque + water
     Render lists 5-6, buckets 1799→500      far transparent, back to front
RT2: Render list 9                           grab scene for refraction
RT1: RenderRefraction list 4, 1799→500       far water with refraction
     Render lists 3-4, buckets 0→499         near opaque
     Render lists 5-6, buckets 499→0         near transparent
     RenderRefraction list 4, 499→0
RT0: Clear
     Render list 10                          composite offscreen scene to back buffer
     Render list 7 (flags 0xC: end + debug)  overlays
```

## 6. Present: `Randy_t::Flip`

Increments the frame counter `Randy_t+0x274`; windowed: `GetClientRect`/`ClientToScreen`, then
`render_t::Blt(primary, clientRect, backBuffer, NULL, DDBLT_WAIT)`; fullscreen: a real flip
(`FUN_1002428e`). On device loss: restores surfaces, re-applies `DeviceState`, bumps
`s_nRestoreCount` path (`FUN_10041ede`).

## 7. Device creation: `Randy_t::Initialize`

The DisplaySystem-facing overload (`?Initialize@Randy_t@@SAPAV1@KKPAUHWND__@@0KKKKKKKKAAV...`) takes
fullscreen flag, window handles, size and buffer depths. It initialises the statically linked
**D3DX for DirectX 7** context (hardware level from the D3DX device index: -6 → `s_eHardwareLevel` 2,
-5 → 1, -3/-4 → 0), then creates the primary surface "FrameBuffer": windowed `DDSCAPS_PRIMARYSURFACE`
(0x200) + clipper, fullscreen a flip chain (caps 0x2218, flags 0x21). Colour depth 16/24/32 comes
from the primary's pixel format. Offscreen render targets are set up in `FUN_100435e3`; the `Randy_t`
object (0x298 bytes) is built by `FUN_10043365`. Errors go through `fun::DXError`.

## Where a Vulkan backend plugs in

- Device + swapchain: `Randy_t::Initialize`, `Flip`, render targets (`SetRenderTarget`,
  `SetRenderTargetAsTexture`, `PushRenderTarget`), `RViewPort_t::Open/Close/Clear`.
- Draw vocabulary: `render_t::Render*` + `DeviceState`/`StateBlob_c`/`RDeltaState` + transforms/lights.
- The scene graph, render lists and visual classes can stay as they are at first. Native passes
  (shadow maps, G-buffer) are added by walking the same `g_renderLists` with our own pass code.

## Measured in game (frame inspector, 2026-10-01, busy outdoor scene, default settings)

- The pass sequence used is the direct one: `SetRenderTarget(0)` (back buffer), Clear, lists 0-2, 3-4
  (front to back), 5-6 (back to front), 7 (flags 0xC). No offscreen / refraction targets: Randy's two
  render-target textures exist but are 8x8 placeholders with these settings.
- List contents: 0 = sky/environment (clouds, sun rays, GenericMeshObject_t/GenericVisualObject_t,
  VisualFillSquare_t), 1 = AnarchyGround_t (terrain), 2 = 48 GfxVisualSimpleShadow_c (blob shadows),
  3 = 46 RCATMesh_t (characters) + 448 RTriMesh_t (static world meshes) + 12 GenericMeshObject_t,
  4 = VisualLiquid_t (water, uses ProcessVertices for screen-space texture coordinates), 5 = booster/ship
  lights, 6 = transparent effects and fading meshes (RTriMesh_t, sprites, flares, a fading RCATMesh_t).
- GUI.dll draws the whole UI after list 7 and before `RViewPort_t::Close`: ~1150 draw calls
  (`RenderTriangleList` from vertex buffers, fans for round widgets).
- FXS.dll draws a few primitives at the start of list 0-2 rendering.

## Open questions

- What the other three pass sequences are for (`Randy_t+0x288` values; probably quality settings / no
  offscreen support).
