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

`Skin=cpu`: SSE on the game's thread, identical results (native_check), 2.8x faster than the original loop.
`Skin=on` (deferred): the game's thread hands each piece's vertex buffer a skin job (`rvk/skin.h`: the piece's mesh
vertices, copied once per mesh, and the character's bones); pool threads skin it right away and the renderer draws the
result, sharing the piece's indices. Characters with vertex effects (callbacks other than the blob shadow) and picking
(`RCATMesh_t::IsLineIntersecting`, the line test) skin on the game's thread; reading a buffer back materializes it.

100 animated Atrox in the harness (`--crowd 100`), game thread per frame / overall frame rate:

| Skin | game thread | overall |
| --- | --- | --- |
| off (original) | 7.9 ms | 127 fps |
| cpu | 4.7 ms | 210 fps |
| on | 1.2 ms | 244 fps (now the renderer's worker is the limit: copying vertices into the GPU ring) |

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
