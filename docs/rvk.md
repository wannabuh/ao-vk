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
- **Targets**: the main colour target and any `CreateRenderTarget` texture are B8G8R8A8 images that can be
  drawn into and sampled; one D32 depth buffer (main size) is shared. Contents persist across target
  switches and frames (LOAD/STORE), like D3D. Image layouts are tracked per texture; a render target bound
  as a texture is switched to a readable layout automatically (ending and resuming rendering).
  `SetRenderTarget` resets the viewport, as D3D does. EndFrame blits the main target to the swapchain
  (recreated when out of date or after `Resize`) and can save a BMP.
- **Textures**: A8R8G8B8, X8R8G8B8, R5G6B5, A1R5G5B5, X1R5G5B5, A4R4G4B4, L8, A8, A8L8, DXT1-5 (BC1-3),
  with mip levels; D3D's 16-bit layouts match Vulkan's PACK16 formats, luminance/alpha formats use view
  swizzles. `UpdateTexture` uploads a region of one level through the frame's upload command buffer.
- **Vertex buffers** keep their contents in CPU memory; every draw copies the range it uses into the ring
  buffer, so rewriting a buffer between draws is safe (the game's CPU skinning reuses one buffer).
- Memory through VMA (from the Vulkan SDK); Vulkan entry points loaded at run time from `vulkan-1.dll`.
- Requirements: Vulkan 1.3 with dynamic rendering, synchronization2, demote-to-helper, push descriptors,
  extended dynamic state 3 (blend enable + equation), vertex input dynamic state. All present on the
  RTX 4070 Ti under Wine 11.18.

## Testing

`tools/rvk-demo.sh` runs `tests/rvk_demo.cpp` headless under Wine with the Khronos validation layer and
writes `build/rvk-demo/rvk_demo.png`: nine tiles covering UI quads, a lit textured cube, additive and
alpha-tested effects, fog, two texture stages with texture matrix and coordinate generation,
lines/fan/culling, render to texture mid-frame, every texture format, mip level selection and vertex
buffer semantics. `rvk_demo.exe --window` shows the same scenes animated.

## Not done yet

- `ProcessVertices` (software vertex transform into a buffer), wireframe fill mode (debug only), W-buffer.
- Texture updates take effect for the whole frame being recorded (uploads run before the draws); a
  texture changed after being drawn in the same frame shows the new contents everywhere. Uploads into
  render targets are ignored.
- Palettised (P8) textures: convert to A8R8G8B8 on the CPU in the integration layer if the game uses them.
- The `IDirectDrawSurface7` shims and everything else that maps Randy onto rvk (integration layer).
- Unbound stages sample (0, 0, 0, 1); check against the game.
