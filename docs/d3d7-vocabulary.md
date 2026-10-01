# The Direct3D 7 vocabulary a Vulkan Randy has to cover

Everything the client asks of Direct3D 7, from static analysis (2026-10-01). Sources:
`re/d3d_wrappers.c` (DumpDecomp `strings:failed`), `interface/state_usage.md`
(tools/state_report.py over ConstArgs.java output), `interface/fvf_args_*.tsv`.
Values only known at run time show up as `?`; the frame inspector can fill those in.

## 1. API calls (all inside randy31's `render_t` wrappers)

`render_t` holds the `IDirect3DDevice7*` at offset 0 and wraps every call in a function that throws
`fun::DXError("render_t::<Name>: D3D-Call failed")`. That makes the list complete for randy31's
own code, apart from statically linked D3DX internals.

**Device (`IDirect3DDevice7`)**

| Group | Calls (vtable offset) |
|---|---|
| Draw | DrawPrimitive, DrawIndexedPrimitive (0x68), DrawPrimitiveVB (0x7C), DrawIndexedPrimitiveVB (0x80); primitive types triangle list/strip/fan, line list/strip, point list |
| State | SetRenderState (0x50) / GetRenderState, SetTextureStageState (0x94) / Get, SetTexture (0x8C) |
| Fixed function | SetTransform (0x2C) / GetTransform, SetMaterial (0x40), SetLight (0x48), LightEnable (0xB0) |
| Targets | SetRenderTarget (0x20) / GetRenderTarget, Clear (0x28), SetViewport (0x34) / GetViewport, BeginScene / EndScene |
| Misc | GetCaps, EnumTextureFormats, GetInfo (0xC0) |

**DirectDraw (`IDirectDraw7`, surfaces, `IDirect3D7`)**: DirectDrawCreateEx, SetCooperativeLevel,
SetDisplayMode / RestoreDisplayMode, CreateSurface, GetAttachedSurface / AddAttachedSurface (z-buffer),
Lock / Unlock, Blt, Flip, FlipToGDISurface, Restore, ReleaseDC, GetPixelFormat, GetSurfaceDesc,
CreateClipper / SetClipper, CreatePalette, GetFourCCCodes, WaitForVerticalBlank, QueryInterface,
CreateDevice, CreateVertexBuffer, ProcessVertices, EvictManagedTextures.

**D3DX 7 (statically linked)**: D3DXInitialize / Uninitialize, D3DXGetDeviceDescription,
D3DXCreateTexture, D3DXLoadTextureFromMemory / FromSurface, D3DXMakeDDPixelFormat. Texture upload and
format conversion go through these.

## 2. Render state usage (`interface/state_usage.md`)

About 1,900 constant call sites across all five DLLs. In short:

- **Blending**: ALPHABLENDENABLE with SRCBLEND ∈ {SRCALPHA, ONE, ZERO, INVSRCALPHA, DESTCOLOR,
  INVDESTCOLOR, DESTALPHA}, DESTBLEND ∈ {ONE, INVSRCALPHA, SRCCOLOR, SRCALPHA, INVSRCCOLOR, ZERO, run-time}.
- **Alpha test**: GREATER with ref 0x80 / 0x1E / 0.
- **Depth**: ZENABLE (incl. run-time, possibly W-buffer via `Randy_t::EnableWBuffer`), ZWRITEENABLE, ZFUNC
  ∈ {ALWAYS, EQUAL, LESS, LESSEQUAL}.
- **Raster**: CULLMODE (mostly NONE), FILLMODE (debug wireframe), CLIPPING, DITHER, SPECULARENABLE.
- **Fog**: FOGENABLE, FOGCOLOR, FOGSTART/END/DENSITY, FOGTABLEMODE / FOGVERTEXMODE, RANGEFOGENABLE.
- **Lighting**: LIGHTING (mostly off for effects), COLORVERTEX, NORMALIZENORMALS, AMBIENT, the
  four material sources (mostly emissive = COLOR1).
- **Texture stages**: only stages **0 and 1**. COLOROP/ALPHAOP ∈ {SELECTARG1/2, MODULATE, MODULATE2X,
  MODULATE4X, ADD, ADDSMOOTH, SUBTRACT, DISABLE}; args DIFFUSE, TEXTURE, CURRENT, TFACTOR (+ALPHAREPLICATE).
  Filters POINT/LINEAR, mip NONE/POINT, addressing WRAP/CLAMP/MIRROR/BORDER.
- **Texture coordinates**: TEXCOORDINDEX with CAMERASPACEPOSITION / CAMERASPACENORMAL generation and
  TEXTURETRANSFORMFLAGS COUNT2 / COUNT3 (texture matrices: environment maps, projected textures, scrolling).

**Never used**: stencil, bump-env / DOTPRODUCT3, hardware vertex blending (skinning is on the CPU),
user clip planes, more than two texture stages, cube maps (as far as constants show).

## 3. Vertex formats (FVF codes seen as constants)

| FVF | Layout | Seen in |
|---|---|---|
| 0x042 | XYZ, DIFFUSE | line lists |
| 0x044 | XYZRHW, DIFFUSE | screen-space fans/lines |
| 0x102 | XYZ, TEX1 | vertex buffers |
| 0x104 | XYZRHW, TEX1 | fans, triangle lists |
| 0x112 | XYZ, NORMAL, TEX1 | lit meshes |
| 0x142 | XYZ, DIFFUSE, TEX1 | most effects (strips, lists, fans) |
| 0x144 | XYZRHW, DIFFUSE, TEX1 | the whole UI (`ViewSurface_c`), strips/fans |
| 0x152 | XYZ, NORMAL, DIFFUSE, TEX1 (36 bytes) | skinned characters / meshes (the format measured in-game) |
| 0x1C2 | XYZ, DIFFUSE, SPECULAR, TEX1 | strips/fans |
| 0x1C4 | XYZRHW, DIFFUSE, SPECULAR, TEX1 | vertex buffer |
| 0x212 | XYZ, NORMAL, TEX2 | vertex buffer (lightmapped geometry?) |

Plus run-time formats from `DynamicVB_c::GetVertices` (e.g. 0x004 XYZRHW in `RViewPort_t::Render`).

## 4. What this means for the Vulkan backend

The compatibility layer is small enough for **one uber-shader pair** instead of a shader per state combination:

- **Vertex shader**: one input layout per FVF (11 + a few), selected per draw; transforms
  (world/view/projection, XYZRHW pass-through), up to N fixed-function lights (point / spot / directional,
  `RVisual_t::SetMaxActiveLightCount` caps them), material sources, fog factor, texture-coordinate
  generation + 2D/3D texture matrices for 2 stages.
- **Fragment shader**: the two-stage combiner (8 ops × 5 args, alpha separately), alpha test, fog.
- **Pipeline state**: blend factors, depth test/write/func, cull, topology. Either dynamic state
  (`VK_EXT_extended_dynamic_state` 1-3, supported on the RTX 4070 Ti) or a small cache of pipelines keyed
  by this state. Combiner and lighting options go in a push-constant / uniform block, or in specialization
  constants if a pipeline cache is used.
- **Resources**: textures from D3DX loads → `VkImage` (formats from `D3DXMakeDDPixelFormat`), vertex
  buffers (`VertexBuffer_c`) and the per-frame `DynamicVB_c` ring → host-visible buffers, render targets
  0/1/2 (+ debug 5/6) → `VkImage`s with a depth attachment.
- **Surfaces** handed to other modules as `IDirectDrawSurface7*` (`render_t::CreateSurface`,
  `LockTexture`, `Blt`) need a small COM object backed by a CPU copy plus a `VkImage`.

This is the same job D7VK does for every D3D7 game. Here it only has to cover what Anarchy Online uses,
and it sits behind Randy's own API rather than COM, so the native features (shadow maps, normal
maps, G-buffer) can be built next to it.
