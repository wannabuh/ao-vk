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
| character skinning | FUN_1005470d | `Skin` | SSE; identical results (native_check), 3.1x faster. docs/skinning.md |

## Order

1. Characters: skinning, then animation (bone matrices, keyframes), then their drawing (RCATMesh_t), which moves
   skinning to the GPU and tells rvk exactly which draws are characters.
2. The device layer (render_t, DeviceState, StateBlob_c, VertexBuffer_c, textures) directly on rvk, without the
   emulated DirectDraw / Direct3D 7.
3. The scene graph, viewports, lights, meshes and materials.
4. The rest; then randy31_orig.dll is no longer loaded.
