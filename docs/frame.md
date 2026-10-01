# How a frame flows through Randy

From the decompiled code in `re/` (regenerate with `ghidra-scripts/DumpDecomp.java`). Addresses are
the original image bases: randy31 0x10000000, DisplaySystem 0x10000000 (separate images).

## 1. DisplaySystem drives the frame

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

## Where a Vulkan backend plugs in

- Device + swapchain: `Randy_t::Initialize`, `Flip`, render targets (`SetRenderTarget`,
  `SetRenderTargetAsTexture`, `PushRenderTarget`), `RViewPort_t::Open/Close/Clear`.
- Draw vocabulary: `render_t::Render*` + `DeviceState`/`StateBlob_c`/`RDeltaState` + transforms/lights.
- The scene graph, render lists and visual classes can stay as they are at first. Native passes
  (shadow maps, G-buffer) are added by walking the same `g_renderLists` with our own pass code.

## Open questions

- Where GUI.dll draws the UI relative to this sequence (after the DisplaySystem frame, before Flip?).
- What the other three pass sequences are for (`Randy_t+0x288` values; probably quality settings / no
  offscreen support).
- Where characters (`RCATMesh_t`) sit: Gamecode list 3 sites are the likely candidates.
- Device creation in `Randy_t::Initialize` (not decompiled yet).
