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
| character animation | FUN_10051d2a, FUN_10051df4, FUN_100540a5, CATAnimBlend_t's sampling / radius / version | `Anim` = off / on | docs/animation.md; same results (native_check, blends of blends included; harness `--blend F` call log identical), crowd's game thread -10% |
| character drawing | FUN_10056ed6 (+ FUN_10055dba) | `CatRender` = off / on | `proxy/native/cat_render.cpp`: materials, overrides, environment map, special light, pulsing glow; identical call log to the original in every path (randy_harness --alpha / --env / --sfx 1 / --sfx 2); also the projected shadow an RShadow draws (FUN_1005604b, vtable slot 20; `--shadow`) |
| character upkeep | FUN_1005798b (Process), FUN_10055d52, FUN_10055c1c, FUN_10055a23 | `CatMesh` = off / on | `proxy/native/cat_mesh.cpp`: visibility, attachments, when bones and skinning are redone; identical call log |
| character queries | HasAttractor, GetAttractor (FUN_10054df1), GetBoneMatrix (FUN_10054f4f), ProcessAttractorChilds, GetMaterialIndex, Get/Set/CreateSubstMaterial, SetSfxType, GetBoundingSphereRadius/Pos | `CatQuery` = off / on | `proxy/native/cat_query.cpp`, with the D3DX7 matrix / quaternion helpers in `proxy/native/xmath.h`: where weapons and effects attach, bones by name, per-character materials; same results as the original (native_check on random input, `randy_harness --query` on a real character) |
| device layer (first part) | DeviceState (SetRenderState, SetTextureStageState, SetTexture, UpdateDevice), render_t's 14 draw calls, SetTransformMatrix, SetLight, GetViewport, CreateVertexBuffer, ProcessVertices, VertexBuffer_c (all but GetImpl), StateBlob_c (all but the 4-byte getters), DynamicVB_c, Randy_t's frame-level functions (Flip, render targets and their stack, fog, W-buffer, ambient, queries; not Initialize), surface_t and render_t's DirectDraw calls (not the D3DX texture functions), RResource_t, RTexture_t (its registry by name its own), RDeltaState / RMaterial_t / DefaultMaterial_t | `Device` = off / on | `proxy/native/device.cpp`, `state_blob.cpp`, `dynamic_vb.cpp` (its own table: nothing else sees inside), `randy.cpp`, `surface.cpp`, `resource.cpp`, `texture.cpp`, `material.cpp`: the same objects and D3D7 calls; identical call log in every harness scene (`tools/calllog-ab.sh Device`) |
| viewports | RViewPort_t: materials on the device, rectangle and camera transforms, Open / Clear / Close, picking and projection helpers, Process (camera, scene update, lights) and Render / RenderRefraction (the render lists) | `Scene` = off / on | `proxy/native/viewport.cpp`; with Visuals on it tells scene.cpp which visual draws (scene::EnterRender / AfterProcess) instead of scene.cpp hooking the originals; identical call logs (Device off and on) and renderer frame dumps |
| frames | RRefFrame_t (all but its debug drawing): construction / copies / archives, children, world matrix, change flags, animation time / loop, visibility, group mask, colour overrides, relative position / rotation, pointing at targets and along directions, connectors, sphere culling | `Scene` = off / on | `proxy/native/refframe.cpp`; call logs bit-identical |
| visuals (first part) | RVisual_t: render lists (distance buckets), Process (fading, restore after a lost device), light culling, render priority, state changes, construction / copies / destruction, archiving (Rasterize / RenderWithTransparency come with SimpleMesh) | `Scene` = off / on | `proxy/native/visual.cpp`; identical call logs, also with 12 lights to cull (scene `manylights`) |
| cameras and lights | RCamera_t: projection, view matrix, frustum set-up (sphere, planes, occluder) and the sphere test every frame of the scene asks; RLight_t: construction / copies / archiving, Process (world position and direction, into the scene's light list), Enable | `Scene` = off / on | `proxy/native/camera.cpp`; identical call logs, also with half a crowd out of view (scene `culled`) |
| static meshes | RVisual_t::Rasterize / RenderWithTransparency (texture animation, observers, sub-meshes back to front, transparency / emissive / specular overrides, environment map and statel light passes), the depth and shadow passes, SimpleMesh::Render and accessors, RTriMesh_t Process (bounding volume culling, occluder registration) / Render / RenderDepth / RenderShadow, its construction / copies / archiving / destruction, data restore, ray test; the render state guard | `Scene` = off / on | `proxy/native/mesh.cpp`; identical call logs (scenes `statics`, `staticshadow`); 100 statics 0.79 -> 0.60 ms |
| mesh data | TriList, RTriList_t (vertex buffers, formats, hardware copies), SimpleMesh and RVisualData_t: construction, copies, archives (loading and writing), destruction, vertex / triangle accessors, mirroring, bounding volumes, the registry that frees hardware copies of meshes not drawn lately | `Scene` = off / on | `proxy/native/mesh_data.cpp`; statics load, draw and delete with identical call logs |
| heightmap occluder | HMOccluder_t: the heightmap and its setters, the per-frame horizons (depth slices of projected terrain per screen column) and the visibility test behind every culled sphere, GetHeight, the on-disk cache of preprocessed playfields, static meshes raising the heightmap, PreProcessPlayfield (its hand-made corrections are the game's data: `tools/gen_occlusion_data.py` extracts them from your randy31_orig.dll into the git-ignored `proxy/native/private/occlusion_data.inc`; without it the original's function stays) | `Scene` = off / on | `proxy/native/occluder.cpp`; harness `--terrain H` (a ridge in front of the crowd), identical call logs at several heights (scene `terrain`), identical heightmaps after meshes register (scene `occmeshes`) and after PreProcessPlayfield for all six corrected playfields and a cached one (scene `preprocess`) |
| character lifecycle | CATRender_t (construction, destruction, SetMesh, SetAnim, vertex process callbacks, the CPU rebuild after a lost device) and RCATMesh_t (construction, destruction, attractor children in their std::map, effect states, RestoreData) | `Scene` = off / on | `proxy/native/cat_life.cpp`; harness `--attach N` / `--restore`, scene `lifecycle`; all scenes delete their characters (`--destroy`) |
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
`tools/calllog-ab.sh <Mode> [scenes]` runs the harness scenes (2D test, characters with blends / shadow / alpha /
environment / effects / lights, statics) with a mode off and on and compares the logs. Functions ported whole are
patched with `Replace` (native.h): it checks the client build once (PE timestamp and size) instead of each function's
bytes, and refuses entries that can't take a 5-byte jump (`proxy/native/entry_guard.gen.h` from
`tools/gen_entry_guard.py`: functions shorter than that, or with a branch into their first bytes).
`tools/port-status.py` reports progress against `docs/port-ledger.tsv`; native code calls what isn't ported yet
through `proxy/native/orig_api.gen.h` (tools/gen_orig_api.py), and lives in the game's heap / containers via
`proxy/native/vc10.h`.

## Tools

- `tools/extract-character.py <cir> <ani> build/characters`, then `tools/randy-harness.sh <out> --character
  <catmesh> <catanim> [--crowd N] [--frames N] [--time ms]`: animated characters through Randy's own scene graph,
  offline; prints the game thread's time per frame. AO_CLIENT=linux/testclient keeps the game's folder alone.
- Harness checks of the character code: `--pick` (line tests), `--query` (attractors, bones, materials, sphere of
  the first character), `--shadow` (it also drawn as an RShadow's projected shadow), `--alpha A`, `--env`,
  `--sfx N`, `--dynamic` (the 2D scene also draws through DynamicVB_c), `--materials` (materials in every combination, their states printed), `--blend F` (animated by a CATAnimBlend_t of two keyframe animations), `--lights N`, `--static <file> [--statics N]`.
- `tools/profile-harness.sh <harness args>`: perf profile of the harness by module and function
  (`tools/profile-report.py`; `--thread` for another thread than the busiest).

## Order

1. Characters: skinning, then animation (bone matrices, keyframes), then their drawing (RCATMesh_t), which moves
   skinning to the GPU and tells rvk exactly which draws are characters.
2. The device layer (render_t, DeviceState, StateBlob_c, VertexBuffer_c, textures) directly on rvk, without the
   emulated DirectDraw / Direct3D 7.
3. The scene graph, viewports, lights, meshes and materials.
4. The rest; then randy31_orig.dll is no longer loaded.
