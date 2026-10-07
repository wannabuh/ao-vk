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
  addition on top of its dated foliage cards (grass.cpp, grass.vert / grass.frag).
  - *Ground*: the terrain's own draws are captured (`Device::CaptureTerrain`, from `Draw`) into world-aligned 8-unit
    tiles of quarter-unit cells around the camera: from the base pass each cell's height and its texture's colour
    (`RVK_GrassTex`: grass only where that texel is green - so not on sand, brick or roads, even in a tile atlas), from
    the light pass its lightmap texel. Small textures (the 64 x 64 lightmaps) are kept whole on the CPU at upload,
    bigger ones on a 16 x 16 grid (`Texture::m_thumb`). A cell keeps the finest terrain triangle that covered it (the
    ground's coarse levels of detail overlap its fine ones); 8 x 8-cell blocks skip triangles that can't refine them,
    and a terrain chunk already captured (known by its contents, pass and texture) is skipped whole unless a tile under
    it is newer - so once the ground is known a frame's capture is a hash per terrain draw (~0.05 ms).
  - *Blades*: once a tile's ground has settled its blades are baked (`BuildGrassTile`, ~0.6 ms a tile, at most ~0.6 ms
    of building a frame, nearest first) as 48-byte records (root, axis, height / width / droop, yaw / wind phase,
    colour, light, the ground texel's colour, kind, fade rank, head colour, canopy density) into one device-local pool
    (a VMA virtual block counted in blades, grown by copying); a tile is rebuilt when its ground changes (a finer level
    of detail, a newly seen part, a re-uploaded lightmap, a setting). A random scatter, clumped by low-frequency noise
    and gathered into tufts (jittered centres; blades thin out between tufts and fan out from their centre), thinned
    and shortened towards the edge of the grass ground (a path, a rock) and gone on steep slopes. `RVK_GrassVary` (0-2)
    scales the tufts, dry straw-coloured patches, taller patches and a per-blade hue jitter - every colour still matched
    to the ground texel's brightness. `RVK_GrassFlower` (0-3) scales the share of other kinds: broad blades, seed stalks
    (a straw head) and flowers (in patches of one colour, a few strays; a head a hand's width across on a stem above
    the grass, opening towards the camera). Each subset near-to-far from the tile's centre
    (early-Z); the first quarter is a sparse subset for the edge.
  - *Drawing* (`DrawGrassTiles`, at the game's first blended draw - which comes before the terrain - or at the end of
    the scene): frustum-culled tiles, nearest first, one indexed draw each through a shared index pattern: vertex v of
    blade b is 8 b + v (cross section v >> 1, edge v & 1); near, 6 triangles a blade (a pointed tip's last one has no
    area), from half the radius 4. The vertex shader expands the record into a strip facing the camera about the
    blade's axis, shaped by its kind (`kSectionT` / `kSectionW`); the steeper the camera looks down on a blade the
    further it lies over (its own way) and the wider it is, so the field stays full seen from above. It bends it -
    keeping its length - with the wind (waves running through the field along it, a cross wave, each blade's flutter),
    the gusts (`RVK_GrassGusts`: bands of stronger wind sweeping along it, the bent blades lighter) and the pushers (the plants' push), and lights it per vertex as the
    terrain's light pass lights the ground: (lightmap + its global ambient + its directional light - none under the
    light override) clamped, darkened by the sun's shadow, plus the frame's local lights (a per-tile light mask) with
    their point shadows. The blade's colour is the grass hue at the ground texel's brightness, with a root-to-tip
    gradient and a darker base in a dense canopy, each of mean 1, its root going over to the ground texel's colour - so
    the field is as bright as its ground at any time of day (measured in the demo: within ~4%, ~7% with flowers and
    everything on). `RVK_GrassGlow` adds the sun on the blade itself (scaled by the shadow-casting sun, none at night):
    a rounded blade's lit and shaded edges (zero-mean), the light through its upper part when the sun is behind it,
    a faint sheen along it; a dense canopy keeps a low sun off the lower blades. Towards the edge the field thins
    rather than ends: the dense three quarters shrink away one by one between half and 0.9 of the radius (each at its
    own rank), the sparse quarter widens to cover and shrinks over the last third, and the colour goes over to the lit
    ground's; a blade thinner than a pixel is drawn a pixel wide, its colour going towards the ground's by its
    coverage. The wind clock is the plants' (wrapped hourly - seconds since boot as a float stepped). It writes all
    five scene targets (its albedo for the GI, the local-light and sun shares for AO and contact shadows, motion),
    since the ground isn't under it yet.
  - *Trails* (`RVK_GrassTrail`, with the push on): a 128 x 128 grid of quarter-unit cells around the camera (world
    anchored, wrapping) where each character standing on the captured ground stamps its push - out from under it and
    the way it walks; a cell recovers over ~5 s. Uploaded with the frame (snorm16 pairs, only while something is
    pushed) and read bilinearly at each blade's root: trodden grass lies further over than the live push bends it.
  - The ground is captured afresh after a camera jump or a gap in the grass's frames (zone changes reuse coordinates).
  - Demo: `--grass-field` (the shadow test's terrain; `--grass-field-tan` / `--grass-field-any` for the filter),
    `--grass-bench` (a 192 x 192 terrain, the camera walking; prints mean and worst frame times; with
    `--grass-field-dist`, `--grass-bench-yaw 3.14` into the sun, `--grass-bench-walker` a character walking through,
    `--grass-bench-night`, `--grass-bench-pitch R` the camera looking down R radians, `--grass-bench-still` the camera
    standing, `--grass-bench-film N` ~60 frames a second with a screenshot every N: film_NNN.bmp), `--grass-style V,F,G,U,T` (variety, flowers, glow, gusts, trails). The log's `ground grass:` line every 600 frames: blades drawn, tiles, builds and their cost,
    the share of blades lit by a captured lightmap, the capture's cost, the terrain's ambient.
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
