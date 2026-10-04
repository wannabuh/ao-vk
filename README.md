# ao-vk

A modern Vulkan renderer for Anarchy Online (the Project Rubi-Ka client), dropped in as a replacement for the
game's renderer DLL, `randy31.dll`. The game keeps running its own code; ao-vk takes over the Direct3D 7 drawing
underneath it and draws everything with Vulkan, adding modern lighting, shadows and effects while doing so - and is
step by step replacing the game's renderer code itself with native code (see *Replacing the game's renderer*).

(Its files still carry the project's earlier name, randy-vk: `randy-vk.ini`, `randy-vk.log`, the `RANDYVK_*`
environment variables.)

It is client-side only: no game files, network traffic or gameplay are changed, and no Funcom files are part of this
repository. Every enhancement can be switched off (in-game or with one hotkey) to get the game's own look back.

## What it adds

**Lighting**
- Per-pixel lighting instead of the game's per-vertex lighting.
- Light override: every nearby light lights every surface (the game picks at most 8 lights per object, so the
  ground and big objects often missed the lights around you). Up to 64 lights a frame.
- Separate intensity sliders for lamps and for the lights characters carry (your own included).
- Generated surface relief: normal maps derived from each texture's brightness, the ground included.
- Side-loaded normal maps for individual game textures (`randy-vk\materials\`, see `docs/materials.md`).
- Sunlight shining through leaves and grass.
- Anisotropic filtering.

**Shadows**
- Sun shadows: cascaded shadow maps with soft shadows whose penumbra grows with distance (sharp where an object
  meets the ground, soft further out). Resolution 1024-8192.
- Contact shadows for small details the shadow map misses.
- Point light shadows: lamps and character lights cast real shadows (up to 16 at once, resolution 256-2048),
  fading in and out smoothly as lights come and go.
- The game's round blob shadows are replaced while sun shadows are on.

**HDR and effects**
- HDR rendering with tone mapping and exposure control.
- Bloom (depth-aware, so halos don't bleed through objects in front) and glow on spell and fire effects.
- Night glow: lit windows, signs and screens glow after dark.
- Ambient occlusion and screen-space indirect light (bounce light).
- Volumetric light: sun shafts through haze.
- Screen-space reflections on water, glossy surfaces and optionally wet-looking floors.
- Motion blur (camera and per object), temporal anti-aliasing with sharpening.
- Depth of field with bokeh, focused on your character by default.
- Colour grading: saturation, contrast, warmth, a cooler night tint, vignette, and your own `.cube` look-up tables
  (`randy-vk-day.cube`, `randy-vk-night.cube` next to `randy-vk.ini`).

**Plants**
- Grass, bushes and trees sway in the wind.
- Grass and plants bend out of the way of characters walking through them, then spring back.
- Big plant quads are split near characters so they bend smoothly instead of tilting as one piece.
- Distant foliage is shaded more cheaply (a level of detail), for speed in grassy areas.

**Characters**
- Phong tessellation: character bodies and heads get rounder silhouettes up close. Hard edges (armour, weapons)
  stay sharp.

**Particles**
- GPU particles with curl-noise flow on sparkle-type spell effects (which effects: `[Particles]` in `randy-vk.ini`).

**Performance**
- The Vulkan work runs on its own thread, so the game's thread only hands draws over.
- Characters are skinned on the GPU and animated by native code (see *Replacing the game's renderer* below): in a
  100-character test crowd the game thread's renderer work drops from 8.4 to about 0.9 ms a frame.
- Caches (mesh fingerprints, shadow casters, shared vertex buffers) keep the per-frame cost down; in big crowds the
  game's own CPU work, not the renderer, is usually the limit.
- A built-in profiler that measures what each enhancement costs where you stand (Ctrl+Shift+O, see below).

## Replacing the game's renderer

ao-vk started as a layer under the game's renderer. It is now replacing that renderer too: Randy's own code (the
original `randy31_orig.dll`, about 1,900 functions) is being rewritten function by function as native code in
`proxy/native/`, with the goal of no longer loading the original at all. Each part is switched on in `randy-vk.ini`:

```ini
[Native]
Skin=on       ; character skinning - on the GPU (cpu = native on the CPU, off = the original)
Anim=on       ; character animation: keyframes, blends, the bone hierarchy
Visuals=on    ; tells the renderer exactly what each draw is (character, ground, effect...) and who carries a light
CatRender=on  ; drawing characters
CatMesh=on    ; characters' per-frame upkeep
CatQuery=on   ; attach points (weapons, effects), bones and materials by name
Device=on     ; the device layer: render state, drawing, vertex buffers, state blocks, presenting, surfaces, textures
```

All of them default to `off` (the original code). They have been played with in game; if something misbehaves,
turning the one key back off brings the original back for that part.

Every part is checked against the original before it is switched on: computations side by side on random input
(`tools/native-check.sh`), drawing with a log of every Direct3D call of a frame, which must be identical with the
part off and on across a set of test scenes (`tools/calllog-ab.sh`). `tools/port-status.py` reports progress
(`docs/port-ledger.tsv`): about 170 functions are native so far and another 270 no longer reachable, roughly 17% of
the work. `docs/native.md` has the plan, results and method.

## Requirements

- Anarchy Online client (the Project Rubi-Ka client this was developed with).
- A GPU and driver with **Vulkan 1.3**, including: dynamic rendering, synchronization2, push descriptors,
  extended dynamic state 3 (colour blend enable and equation), vertex input dynamic state,
  demote-to-helper-invocation, cube map arrays and fragment-shader quad operations. Tessellation is optional
  (rounder characters need it). Any recent NVIDIA, AMD or Intel desktop GPU should qualify.
- **Tested on:** Linux with Wine 11.18 (staging, WoW64), NVIDIA RTX 4070 Ti, driver 615. Windows should work the
  same way (the DLL is a normal 32-bit Windows DLL that loads `vulkan-1.dll` from the GPU driver) but has not been
  tested yet - reports welcome. AMD and Intel GPUs have not been tested either.

## Installing

You need up to two files from a [release](../../releases) (or your own build, see below):

| File | What it is |
| --- | --- |
| `randy31.dll` | ao-vk itself |
| `version.dll` | optional: [AOReloaded](https://github.com/wannabuh/AOReloaded) with an in-game **Renderer** settings tab |

### Steps (Windows and Wine alike)

1. **Close the game** and open your Anarchy Online client folder (the one with `AnarchyOnline.exe`).
2. **Keep the game's own renderer:** rename (or copy) the existing `randy31.dll` to **`randy31_orig.dll`**.
   ao-vk loads it and lets the game's renderer code do its usual work; it must sit in the same folder.
3. **Copy ao-vk's `randy31.dll`** into the client folder.
4. Optional: **copy AOReloaded's `version.dll`** into the client folder for the in-game settings tab
   (Options, F10, **Renderer** tab). Without it, settings are changed in `randy-vk.ini` or with the hotkeys.
5. **Start the game as usual** (launcher or `AnarchyOnline.exe`). ao-vk creates `randy-vk.ini` (settings) and
   `randy-vk.log` (log) in the client folder on the first start.

To check it is active: the log's first lines say `rvk backend installed` and name your GPU, and Ctrl+Shift+E
switches between ao-vk's look and the game's own.

### Wine / Linux notes

- Use a prefix where Vulkan works (any DXVK game in it proves that). ao-vk talks to Vulkan directly; it does
  not need DXVK.
- The game still asks DirectDraw for display information outside the renderer. Wine's builtin DirectDraw should do
  for that; the tested setup used D7VK as the prefix's DirectDraw, which works. dgVoodoo is not needed.

### After a game patch

The game's patcher may replace `randy31.dll` with a fresh stock one. If ao-vk's look is gone after an update:
rename the new `randy31.dll` to `randy31_orig.dll` again (it may be a newer version of the game's renderer) and put
ao-vk's `randy31.dll` back.

### Uninstalling

Delete ao-vk's `randy31.dll`, rename `randy31_orig.dll` back to `randy31.dll`, and delete `randy-vk.ini` /
`randy-vk.log` if you like. Remove `version.dll` to remove AOReloaded.

## Settings

All settings live in `randy-vk.ini` (section `[Renderer]`; the native code's switches in `[Native]`, above) and are
saved whenever they change. With AOReloaded they
appear in the game's options window (F10) under **Renderer**: every feature as an on/off checkbox at the top, its
sliders and choices further down, grouped by feature. Changes apply immediately.

Some useful ones:

| Setting | Default | |
| --- | --- | --- |
| `RVK_Enhance` | 1 | master switch: 0 = the game's own look (keeps your other settings) |
| `RVK_SunRes` | 4096 | sun shadow resolution; **1024 is much faster** and still looks good |
| `RVK_PtShadows` / `RVK_PtRes` | 8 / 1024 | how many lights cast point shadows, and their resolution |
| `RVK_CharLight` | 1 | brightness of lights characters carry (lower it if your own light feels too strong) |
| `RVK_TessOn`, `RVK_Tess*` | on | rounder characters: roundness, detail, distance |
| `RVK_FoliageLod` | 35 | distance from which foliage is shaded cheaply |

### Hotkeys (Ctrl+Shift + key)

| Key | Action | Key | Action |
| --- | --- | --- | --- |
| E | all enhancements on / off | F10 | per-pixel lighting |
| F8 | light override | F7 | sun shadows |
| F6 | point light shadows | F5 | HDR |
| F4 | bloom | F3 | ambient occlusion |
| F2 | generated relief | F1 | anisotropic filtering |
| G | indirect light | V | volumetric light |
| R | reflections | M | motion blur |
| N | per-object motion blur | D | depth of field |
| T | anti-aliasing | P | GPU particles |
| ] / [ | relief stronger / weaker | Home / End | bloom stronger / weaker |
| Insert / Delete | effect glow stronger / weaker | PgUp / PgDn | local light headroom up / down |
| L | reload the colour look-up tables | F11 | lighting debug view |
| F9 | frame dump (draw list + screenshot next to the log) | O | profiling sweep (below) |

**Profiling sweep (Ctrl+Shift+O):** stand still for a minute or two; ao-vk switches each enhancement off in turn,
measures, and writes lines like `sweep sun shadows off (133.6 fps): gpu ms: ...` to the log. It shows what each
feature costs exactly where you are.

### Environment variables

| Variable | |
| --- | --- |
| `RANDYVK_DDRAW` | `rvk` (default): render with ao-vk. `off`: the game's stock renderer, ao-vk passing everything through. `trace`: the stock renderer, counting its Direct3D calls |
| `RANDYVK_THREADED` | `0` runs the Vulkan work on the game's thread (for comparison) |
| `RANDYVK_LOG` | log file path (default `randy-vk.log` in the client folder; moved to `randy-vk.old.log` once over 8 MB) |

## Known limitations

- Tested on one system (Linux, Wine, NVIDIA). Windows, AMD and Intel are untested.
- The game is a 32-bit program, so its memory is limited. Very high settings (8192 sun shadows = 1 GB of video
  memory, 16 point shadows at 2048 = 1.5 GB) are best avoided.
- In very large crowds the game's own CPU work (its interface, game logic) still limits the frame rate; the native
  character code (below) takes the renderer's share of it off the game's thread.
- Shadows are drawn from the untessellated characters.

## How it works

The built `randy31.dll` exports the same 771 functions as the game's original. Almost all of them forward to the
original (`randy31_orig.dll`), so the game's renderer code still manages its scene; ao-vk replaces what is under
it: the original's Direct3D 7 / DirectDraw calls are redirected to an implementation of those interfaces on top of
**rvk**, a Vulkan 1.3 renderer shaped like Direct3D 7's fixed-function pipeline (`docs/rvk.md`), which adds the
enhancements. On top of that, `proxy/native/` replaces the original's own functions with native ones (a jump
patched over the original's entry once the client build is recognised), keeping its object layouts so the game's
other DLLs, which derive from Randy's classes, keep working.

- `docs/architecture.md`: the overall design; `docs/frame.md`: how the game draws a frame
- `docs/rvk.md`: the renderer; `docs/materials.md`: normal maps; `docs/d3d7-vocabulary.md`: what the game uses
- `docs/native.md`: replacing Randy; `docs/skinning.md`, `docs/animation.md`: the character code

## Building

Cross-compiled on Linux (no Windows needed):

- clang-cl, lld-link, cmake, ninja
- the MSVC CRT and Windows SDK for x86, unpacked by [xwin](https://github.com/Jake-Shadle/xwin) into `~/.xwin`
- the [Vulkan SDK](https://vulkan.lunarg.com/) for Linux (glslc, headers, VMA); its `x86_64` folder is
  `VULKAN_SDK_DIR` (CMake cache variable, default `~/repos/vulkan-sdk/1.4.321.1/x86_64`)

```sh
cmake --preset linux-release -DVULKAN_SDK_DIR=/path/to/vulkan-sdk/x86_64
cmake --build --preset linux-release
```

The result is `build/linux-release/randy31.dll`, plus test programs.

### Testing

- `tools/rvk-demo.sh [out-dir] [args]`: renders test scenes with rvk headless under Wine with the Khronos
  validation layer and writes a PNG (`--shadow-test`, `--point-shadow-test`, `--grass-walk X`, `--tess 0.75`, ... -
  see `tests/rvk_demo.cpp`).
- `tools/randy-harness.sh`: drives the game's renderer through the proxy outside the game (`tests/randy_harness.cpp`):
  a 2D test scene, or animated characters (`--character`, `--crowd N`, `--blend`, `--shadow`, `--env`, `--sfx N`,
  `--lights N`, `--pick`, `--query`) and static meshes (`--static`), from files extracted from the game with
  `tools/extract-character.py` / `tools/extract-static.py`. It prints the game thread's time per frame.
- `tools/native-check.sh`: native replacements against the original functions on random input.
- `tools/calllog-ab.sh <Mode>`: every Direct3D call of a frame in each harness scene, with a `[Native]` part off and
  on; they must be identical.
- `tools/port-status.py`: how much of the original is replaced, unreachable, left.

### Layout

- `rvk/`: the Vulkan renderer (`rvk/shaders/`: GLSL, compiled into the DLL)
- `proxy/`: the `randy31.dll` proxy, the DirectDraw / Direct3D 7 implementation on rvk (`proxy/ddraw/`), the
  settings (`proxy/ddraw/rvk_settings.cpp`) and the exported settings interface AOReloaded uses
- `proxy/native/`: the native replacements of the original's functions
- `interface/`: tables generated from the original DLL and its importers (`tools/gen_interface.py`)
- `ghidra-scripts/`, `tools/`: the analysis scripts used along the way

## License

MIT, see `LICENSE`. Anarchy Online is a trademark of Funcom; this project is not affiliated with Funcom and
contains none of its files.
