# Native Randy

The goal: replace `randy31_orig.dll` (the game's Direct3D 7 renderer, which ao-vk currently runs on top of) with our
own code, piece by piece, until the game no longer needs it and draws straight through rvk.

## How a piece is replaced

`proxy/native/` (static library `randy_native`, linked into `randy31.dll` and the tests):

- `native.h` / `hook.cpp`: `HookEntry` puts a jump to our function at the start of one of the original's functions
  (after checking its first bytes, so an unknown client build is left alone) and returns a trampoline that still runs
  the original. `HookSlot` replaces a vtable entry. Exports are replaced in `proxy/randy31.def`
  (`tools/gen_interface.py` HOOKED_EXPORTS).
- Every replacement has a mode in `randy-vk.ini` `[Native]`: `off` (the original), `on` (ours), `verify` (both run;
  the original's result is kept, differences are logged to `randy-vk.log`). New replacements start `off`.
- `rnative::Install` (native.cpp) installs them when randy31_orig.dll is loaded (proxy/trace.cpp).
- `tests/native_check.cpp` (`tools/native-check.sh`): runs each replacement and the original code it replaces on the
  same synthetic input under Wine, compares, and times both. Works while the game runs (uses the test client folder).

The original's object layouts must be kept wherever other modules see them (docs/architecture.md); layouts are taken
from the decompiles (`re/`, not in the repository) and written down in `docs/`.

## Replaced so far

| piece | original | `[Native]` | notes |
| --- | --- | --- | --- |
| character skinning | FUN_1005470d | `Skin` = off / cpu / on / verify | docs/skinning.md |
| character animation | FUN_10051d2a, FUN_10051df4, FUN_100540a5 | `Anim` = off / on | docs/animation.md; same results (native_check), crowd's game thread -10% |
| character drawing | FUN_10056ed6 (+ FUN_10055dba) | `CatRender` = off / on | `proxy/native/cat_render.cpp`: materials, overrides, environment map, special light, pulsing glow; identical call log to the original in every path (randy_harness --alpha / --env / --sfx 1 / --sfx 2) |
| which visual draws | RViewPort_t::Render / RenderRefraction (hooked, not replaced) | `Visuals` = off / on | `proxy/native/scene.cpp`: the visual Render keeps at RViewPort_t +0x164, its RTTI class, attached-to-a-character by its frames; the renderer gets it per draw (Device::SetDrawVisual, frame dumps show it) and uses it for character detection and blob shadows. Also hooks RViewPort_t::Process: after the scene update it reads Randy's light list (0x1017D290, world-space D3DLIGHT7 at RLight_t +0xA4) with each light's carrier (a character up its frames, or next to it under a small parent); draws carry their character too, so which character carries which light (point shadows, the carried-light setting, characters unlit by their own light) is exact |

`Skin=cpu`: SSE on the game's thread, identical results (native_check), 2.8x faster than the original loop.
`Skin=on` (deferred): the game's thread hands each piece's vertex buffer a skin job (`rvk/skin.h`: the piece's mesh
vertices, copied once per mesh, and the character's bones); pool threads skin it right away and the renderer draws the
result, sharing the piece's indices. Characters with vertex effects (callbacks other than the blob shadow) and picking
(`RCATMesh_t::IsLineIntersecting`, the line test) skin on the game's thread; reading a buffer back materializes it.

100 animated Atrox in the harness (`--crowd 100`), game thread per frame / overall frame rate:

| Skin | game thread | overall |
| --- | --- | --- |
| off (original) | 8.4 ms | 118 fps |
| cpu | 5.0 ms | 167 fps |
| on, skinned on the CPU (`RANDYVK_GPU_SKIN=0`) | 1.6 ms | 180 fps (the worker: per-vertex copies) |
| on, skinned on the GPU (default) | 0.9-1.0 ms | 350-410 fps |

**GPU skinning** (`rvk/skin_gpu.cpp`, `shaders/skin.comp`): a piece drawn with `Device::DrawSkinned` is skinned by a
compute dispatch in the frame's upload command buffer (run before the main pass) into a per-frame arena; the mesh
(vertices, same-position groups, indices) is uploaded once. The same dispatch writes last frame's positions (the
piece's previous bones: motion vectors); tessellated draws get the smooth normals from a second one. The CPU never
touches the vertices: the box comes from the bones and the mesh's per-bone boxes (`Job::ComputeBounds`), shadow
casters reference the arena. Pieces drawn partially or with other indices take the CPU way.

Skinned draws tell the renderer what they are (`Device::DrawSkinned`): exactly a character (any size; the heuristic
in CharacterDraw is for the rest), its box and index hash known - no pass over its vertices.

## How a port is checked

Pure computations: `tests/native_check.cpp` runs the original function and ours on the same input. Drawing code: the
**call log** (`RANDYVK_CALLLOG=<file> RANDYVK_CALLLOG_FRAME=<n>`) lists every Direct3D call of one frame with objects as
ids and data as hashes; the harness scene logged with the replacement off and on must give the same file.
`tools/port-status.py` reports progress against `docs/port-ledger.tsv`; native code calls what isn't ported yet
through `proxy/native/orig_api.gen.h` (tools/gen_orig_api.py), and lives in the game's heap / containers via
`proxy/native/vc10.h`.

## Tools

- `tools/extract-character.py <cir> <ani> build/characters`, then `tools/randy-harness.sh <out> --character
  <catmesh> <catanim> [--crowd N] [--frames N] [--time ms]`: animated characters through Randy's own scene graph,
  offline; prints the game thread's time per frame. AO_CLIENT=linux/testclient keeps the game's folder alone.
- `tools/profile-harness.sh <harness args>`: perf profile of the harness by module and function
  (`tools/profile-report.py`; `--thread` for another thread than the busiest).

## Order

1. Characters: skinning, then animation (bone matrices, keyframes), then their drawing (RCATMesh_t), which moves
   skinning to the GPU and tells rvk exactly which draws are characters.
2. The device layer (render_t, DeviceState, StateBlob_c, VertexBuffer_c, textures) directly on rvk, without the
   emulated DirectDraw / Direct3D 7.
3. The scene graph, viewports, lights, meshes and materials.
4. The rest; then randy31_orig.dll is no longer loaded.
