# rvk: the Vulkan renderer

`rvk/` is a static library: a Vulkan 1.3 renderer whose front end is shaped like `IDirect3DDevice7`
(same enums, structure layouts and semantics), limited to what Anarchy Online uses
(`docs/d3d7-vocabulary.md`). Randy's `render_t` wrappers will map onto it one to one.

## Design

- **One uber-shader pair** (`rvk/shaders/ffp.vert`, `ffp.frag`) implements D3D7 fixed function:
  row-vector transforms, XYZRHW screen vertices (with D3D's integer pixel centres), up to 8
  point/spot/directional lights with material colour sources, vertex/table fog (linear/exp/exp2, range
  fog), texture-coordinate generation (camera-space position/normal/reflection) and texture matrices
  (incl. projected), the two-stage texture combiner, specular add and alpha test.
- **Per-draw constants** (`DrawConstants`, ~1.5 KB, std140) go into a per-frame ring buffer and are bound
  with push descriptors together with the two stage textures.
- **No pipeline compiles at run time**: three pipelines (point/line/triangle topology class) with
  everything else dynamic: viewport, cull, depth test/write/compare, topology, blend enable/equation
  (`VK_EXT_extended_dynamic_state3`) and the vertex layout per FVF (`VK_EXT_vertex_input_dynamic_state`).
  Attributes a format lacks read from a zero buffer bound at stride 0.
- Rendering goes to an offscreen colour (RGBA8) + depth (D32) target with dynamic rendering; EndFrame
  blits it to the swapchain (if there is a window) and can save a BMP.
- Memory through VMA (from the Vulkan SDK); Vulkan entry points loaded at run time from `vulkan-1.dll`.
- Requirements: Vulkan 1.3 with dynamic rendering, synchronization2, demote-to-helper, push descriptors,
  extended dynamic state 3 (blend enable + equation), vertex input dynamic state. All present on the
  RTX 4070 Ti under Wine 11.18.

## Testing

`tools/rvk-demo.sh` runs `tests/rvk_demo.cpp` headless under Wine with the Khronos validation layer and
writes `build/rvk-demo/rvk_demo.png`: six tiles covering UI quads, a lit textured cube, additive and
alpha-tested effects, fog, two texture stages with texture matrix and coordinate generation, and
lines/fan/culling. `rvk_demo.exe --window` shows the same scenes animated.

## Not done yet

Persistent vertex buffers (`VertexBuffer_c`), render targets other than the main one (Randy's RT1/RT2
for refraction), texture formats other than A8R8G8B8 and mipmaps, `ProcessVertices`, wireframe fill
mode, W-buffer, `IDirectDrawSurface7` shims, window resize. Unbound stages sample (0, 0, 0, 1); check
against the game.
