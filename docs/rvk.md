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
- **Depth pre-pass** (`rvk/prepass.cpp`, setting `RVK_Prepass`): the frame's main commands are split at the
  scene's depth clear into `mainA` (before it), a depth-only pre-pass (the clear, then the depth of every opaque
  draw that follows, recorded as the draws come) and `mainB` (the rest), submitted in that order. The scene's
  draws then find the nearest opaque depth already there and early-Z rejects what is hidden, so the expensive
  scene shader runs about once per pixel. "Opaque": no discard, depth written, not blended or blended by an alpha
  that is provably 1 (`AlphaOneCheck`); not characters, swaying plants, labels or particles. A segment ends with the
  scene's rendering (target switch, copy, flush, end of the scene) or at a second depth clear. `ffp.vert` declares
  `gl_Position` invariant so both passes produce the same depth; a pre-passed draw's `LESS` becomes `LESSEQUAL`. Backgrounds (draws ignoring depth, not writing it, before anything
  in the segment wrote depth) get the depth bounds test [clear, clear]: they are shaded only where the pre-pass found
  no opaque surface, which is the only place they stay visible.
  **Cut-outs** go in with a second pipeline, `prepass_cutout.frag` (`ffp_main.glsl` built with `RVK_PREPASS_CUTOUT`),
  which computes the draw's alpha as the main shader's cut-out test does and writes depth only for pixels that hide
  what is behind them in any draw order: an alpha tested draw that isn't blended where its test passes, and two-pass
  foliage's core (below). Other blended cut-outs stay out - a partly transparent pixel of theirs writes depth too, and
  drawn before an opaque pixel behind it that the pre-pass knew about, it would blend over a background the pre-pass
  had rejected. Swaying plants go in under the same rules. `RANDYVK_PREPASS_CUTOUT=0` keeps cut-outs out,
  `RANDYVK_PREPASS_SWAY=0` swaying plants.
- **Two-pass foliage** (setting `RVK_FolEdges`, Plants; on): the game draws its foliage blended with depth writes, so
  every partly transparent leaf pixel hides whatever is drawn behind it afterwards and shows whatever was drawn before
  - the tree behind, the sky, seen through some leaves' edges depending on draw order. A blended cut-out that writes
  depth is now drawn as its core (alpha >= 0.95, blended, with depth: its record's `motion.w` = 1) and its soft edges
  (the rest: `motion.w` = 2, its own record) are queued and drawn blended without depth writes over the finished
  scene (`Device::FlushEdges`: when the rendering ends - target switch, copy, flush, scene end - before a clear, and
  before a draw that writes no depth, so effects still come after them). The replay sets each edge's state (cull,
  depth compare, viewport, buffers, per-draw bindings) and goes through `ApplyDynamicState`. With the edges out of the
  depth, the core is exact for the cut-out pre-pass.
- **Interface refresh rate** (setting `RVK_UiRate`, General; 0 = every frame, the default): GUI.dll's drawing of the
  interface costs the game thread ~1.5 ms a frame (its view tree, and ~1,000 draws). With a rate, the interface is
  drawn only that many times a second into an image of its own, which is put over every frame. `rnative::gui`
  (proxy/native/gui.cpp) wraps GUI.dll's `WindowController_c::Render` - the interface's per-frame work (what is under
  the mouse, the pointer's shape, tooltips) runs every frame - and makes its top-level `View::_CallRender` calls, the
  drawing, return at once on frames that don't redraw (the backend decides: `RVK_UiRate`, or the renderer has no
  current layer). The renderer (rvk/interface.cpp) ends the scene where the interface's first draw would, draws a
  redrawn interface into `m_uiLayer` (cleared; every draw blends, its alpha accumulating coverage, so the layer is
  premultiplied) and blends the layer over the main target (ONE, INV_SRC_ALPHA) at the end of `Render`. Exact for
  alpha-blended and additive interface draws; an unblended one keeps its texture's alpha as coverage, one multiplying
  the frame is approximated. Hooks are only made with a rate set; the two entries' code is checked (MSVC's exception
  prologue calling GUI.dll's `_EH_prolog`), so another GUI.dll build is left alone (logged).
  Measured (a 245 fps spot, 60): the interface's game-thread time 1.5 -> 0.34 ms, its GPU time 0.3 -> 0.07 ms;
  cursor, interface look and input unchanged in play. 60 is the recommended value.
- **Ground grass** (setting `RVK_GrassOn`, Plants; **off** by default): our own grass over the game's terrain, an
  addition on top of its dated foliage cards. The ground heights are sampled from the game's own terrain draws
  (`Device::CaptureTerrain`, called from `Draw` for `IsTerrain` draws) into a world grid of half-unit cells near the
  camera, so a blade sits on whatever the terrain is there; a cell the terrain hasn't been seen at (a building, water,
  the sky) gets no grass. Each frame `RenderGrassField` (grass.cpp) generates a field of blades - a grid of patches
  around the camera out to `RVK_GrassDist`, `RVK_GrassDensity` blades each, `RVK_GrassHeight` tall - and draws them
  into the scene rendering at its end (`EndScene`, before the post passes) through a standalone pipeline: no descriptor
  sets of the scene's, the current and previous camera and the sun arrive as one uniform buffer and one push constant.
  The blades are tapered, bending strips (the wind bends their upper sections), faded out over the last third of the
  radius; they write the scene colour and the motion vectors (static in the world, so the camera's motion), leave the
  glow, light-fraction and albedo attachments as the scene left them (the pipeline's per-attachment write masks, which
  need the `independentBlend` device feature), depth-test against the scene, and are lit by the sun with a root-to-tip
  gradient. Blades are placed from a position hash, not the frame number, so they don't crawl. `--grass-field` runs the
  shadow test's terrain under a field of them in the demo.
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
