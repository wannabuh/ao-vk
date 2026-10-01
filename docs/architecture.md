# What a replacement randy31.dll has to provide

Findings from static analysis of the PRK client (2026-10-01). Regenerate the data with
`tools/gen_interface.py`, `tools/rtti.py` and `ghidra-scripts/RandyFieldUse.java`.

## Who draws

Randy is not the whole renderer. Rendering is split between two DLLs:

- **randy31.dll**: device, render targets, textures, vertex buffers, state, scene graph base
  classes (`RRefFrame_t`, `RVisual_t`, `RTriMesh_t`), characters (`RCATMesh_t`, CPU skinning),
  lights, materials, viewports.
- **DisplaySystem.dll**: about 120 classes derived from Randy's base classes that draw themselves.
  That includes the terrain (`AnarchyGround_t`), indoor rooms (`VisualRoom_t`), sky (`VisualGloomySky_t`
  and friends), water (`VisualLiquid_t`), clouds, fog, sun rays, ships, every particle/beam/trail
  effect (`GfxVisual*`, 60+ classes), the simple blob shadow (`GfxVisualSimpleShadow_c`) and the
  offscreen/refraction passes (`VisualOffscreen2*`). Gamecode.dll adds two (`GfxVisualForceSword_t`
  and its own copy of `VisualEnvFXVisualBase_t`). GUI.dll and FXS.dll derive from none.

DisplaySystem only imports `DirectDrawCreate` from DirectDraw. Everything else it draws goes through
randy31 exports, so randy31 is still the single choke point between the game and the GPU.

## Object layout (`interface/subclasses.tsv`)

Every visual derives from `RVisual_t : RRefFrame_t : fun::Serializable_c` at offset 0, with a second base
`SubjectImpl<VisualEvents::Rendering>` at +0xA4. So:

- `sizeof(RVisual_t)` is 0xA4 and must stay 0xA4. Its vtable layout must stay identical, because
  DisplaySystem's classes override its virtual methods and Randy calls them.
- `RTriMesh_t` is also a base class (`VisualEnvFXMeshBase_t`, ships, rocks, nebula, vortex sky).
- `TextureCreator` / `TextureStreamCreator` are subclassed for texture loading (ads, ground).
- `RResource_t` is subclassed by the ground data.

Fields DisplaySystem touches directly inside Randy base objects (`interface/field_uses_DisplaySystem.tsv`,
a lower bound: tracking is per function): only 4 offsets in `RVisual_t` and 10 in `RRefFrame_t`
(0x14..0x28, 0x3C, 0x84, 0x88, 0x9E). Everything above 0xA4 belongs to the derived class.

Allocation sizes DisplaySystem compiles in (operator new + imported constructor), which therefore
can't change: `RDeltaState` 0x16C, `RMaterial_t`/`DefaultMaterial_t` 0xC0, `RTexture_t` 0xBC,
`RRefFrame_t` 0xA4, `RViewPort_t` 0x178, `RLight_t` 0x11C, `RCATMesh_t` 0x438, `CATMesh_t` 0x64,
`CATAnimBlend_t` 0x70, `CATKeyframeAnim_t` 0x60, `CATKeyframeAnimData_t` 0x48, `surface_t` 0x88,
`PixelFormat_t` 0x34, `TextureStreamCreator` 0x40/0x4C, `VertexBuffer_c` 4. Other modules add
`StateBlob_c` 0x84 (FXS) and `DummyConnector` 0x6C (Gamecode).

The other importers barely look inside Randy objects: GUI reads 9 `RViewPort_t` fields and
3 `RRefFrame_t` fields, Gamecode stores into `RRefFrame_t`/`RVisual_t` fields (positions, flags) in
about 100 places, and FXS touches none (`interface/field_uses_*.tsv`).

## The drawing API DisplaySystem uses

Draw calls are D3D7-shaped immediate mode: `render_t::RenderTriangleList/Strip/Fan/LineList`
(from `VertexBuffer_c` or user pointers, FVF codes), `render_t::SetTransformMatrix`, `SetLight`,
`ProcessVertices`, `StateBlob_c` / `RDeltaState` / `DeviceState::SetRenderState` and
`SetTextureStageState` with raw `D3DRENDERSTATETYPE` / `D3DTEXTURESTAGESTATETYPE` values, plus
`DynamicVB_c` for per-frame vertices and render-target switching (`Randy_t::PushRenderTarget`,
`SetRenderTargetAsTexture`). Surfaces are created, locked and blitted as `IDirectDrawSurface7`.

## Consequences for the Vulkan renderer

1. The new randy31 must implement this D3D7 fixed-function vocabulary on Vulkan: render states,
   texture stage states, FVF vertex layouts, lights/materials, transforms. Otherwise the ~120
   DisplaySystem visuals stop drawing. This is the compatibility layer, generated pipelines from state.
2. The big visuals can then be rebuilt natively one by one by replacing their virtual methods
   (DisplaySystem vtables are known, see `tools/rtti.py` output): terrain, rooms, static meshes,
   characters (GPU skinning), water, sky. Those are where shadows, normal maps and reflections go.
3. `RVisual_t`/`RRefFrame_t`/`RTriMesh_t` keep their binary layout and vtables. Randy's internal
   classes that nobody else sees can be designed freely.
4. `IDirectDrawSurface7` objects handed out by `render_t::CreateSurface` need a small COM shim backed
   by Vulkan images (Lock/Unlock/Blt/GetSurfaceDesc as used).
