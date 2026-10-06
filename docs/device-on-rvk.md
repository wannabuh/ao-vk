# The device layer on rvk

Goal: the native Randy code (`proxy/native/`, docs/native.md) talks to rvk directly instead of through the emulated
DirectDraw 7 / Direct3D 7 objects (`proxy/ddraw/rvk_*.cpp`), and - where the work allows it - hands rvk *retained*
objects (meshes, materials) instead of re-sending loose state and vertices every draw. Who benefits: everyone, but
most of all players on weaker CPUs - the renderer is CPU-bound (the render thread, ~9 ms a frame in the demanding
areas) long before the GPU is.

## Where a draw's time goes today

A static mesh draw (`RTriMesh_t` / `SimpleMesh::Render`, native `mesh.cpp`):

1. **Game thread, native code**: `DeviceState::SetRenderState` / `SetTextureStageState` / `SetTexture` record the
   wanted state; `DeviceState::UpdateDevice` sends each changed value as its own `IDirect3DDevice7` call;
   `render_t::RenderTriangleList` sends the draw.
2. **Game thread, COM layer** (`rvk_device.cpp`), per call: a call counter, the backend's critical section, the
   device's own copy of the state (for the Get* calls), then `rvk::ThreadedDevice`.
3. **Game thread, hand-off** (`rvk/threaded.cpp`), per call: one record in the queue. A draw from a vertex buffer
   unchanged for two frames passes a `shared_ptr` to its vertices (an atomic reference count touched on both
   threads) and copies its **indices** into the record; any other draw copies its **vertices and indices**. About
   9,000 records and 1.7 MB a frame.
4. **Render thread** (`rvk/draw.cpp` `Device::Draw`, ~1 us a draw, ~3,200 draws): re-derives everything per draw from
   loose state: the mesh's identity (a hash), its bounds, the constant block (render state, material, lights), the
   light mask, the caster key, the batch keys; copies the indices (and the non-static vertices) into the GPU ring.

So the D3D7 interfaces themselves (step 2) are a small part. The cost is that every frame re-sends, re-copies and
re-derives what does not change between frames: the static meshes' geometry and the state each mesh draws with.

## Phases

Each phase keeps the call log A/B (`tools/calllog-ab.sh`, docs/native.md) identical where it applies, and is behind a
`[Native]` mode, default off until tested in game, like every native piece so far.

### 1. A direct channel, and state in one record per update

- `rnative::device::Direct` (native/device.h): a table of functions the rvk backend registers when it is installed
  (`RANDYVK_DDRAW=rvk`). Native code calls through it when present, and through the D3D7 interfaces otherwise (the
  original DirectDraw, the tests). No link dependency from `randy_native` on the backend.
- `DeviceState::UpdateDevice` sends all changed render states, texture stage states and textures as **one** call
  (`Direct::applyStates`): the backend updates its device's copies (Get* stay right, the call log prints the same
  lines in the same order) and enqueues **one** record (`ThreadedDevice::SetStates`) that the render thread applies
  in order. Repeats are still dropped against what was last sent.
- Expected: about half of the 9,000 records a frame go; no COM lock / counter per state on the game thread.
- Checks: call log A/B identical; the hand-off line's record count drops.
- Status: done, `[Native] Direct` (with `Device = on`; default on). `native/device.h` `Direct` / `StateChange`,
  `RDevice::ApplyStates` (rvk_device.cpp: the same device copies, call log lines and call counts as the single
  calls), `ThreadedDevice::SetStates` (one record; repeats dropped as before). Call log A/B identical in all 36
  scenes. In game (demanding area): 9,100 -> 7,450 records a frame, 132 fps; the frame is now held by the game
  thread (render thread idle 1.3 ms, game thread never waiting), with 1.7 MB a frame still copied into the hand-off.

### 2. Draws without the COM layer

- render_t's 14 draw calls, `SetTransformMatrix`, `SetLight` / `LightEnable`, `SetViewport`, `SetMaterial` through
  `Direct` (the backend's device copies kept for the Get* calls).
- The draw record carries the vertex buffer's identity instead of a `shared_ptr` copy per draw (the buffer keeps its
  vertices alive until the frames that drew it are done: a retire list per frame, no atomic reference count on the
  render thread).

### 3. Retained meshes (the big one)

- `rvk::Mesh`: a static mesh's vertices **and indices** in a GPU buffer, uploaded once (on first draw, or when its
  `RTriMeshData_t` is loaded), with what `Device::Draw` now recomputes per draw computed once: bounds, index hash,
  vertex alpha, the caster identity.
- `SimpleMesh::Render` (native mesh.cpp) draws by handle: `Direct::drawMesh(mesh, subMesh, world)`. The record is a
  few dozen bytes; nothing is copied on either thread; `Device::Draw` skips the mesh hashing, the index copy and the
  caster key.
- The meshes' lifetime follows the game's: `RTriMeshData_t` destruction (native mesh_data.cpp) releases it after the
  frames in flight.
- Same for characters' pieces (already GPU-skinned with GPU-resident meshes: their draw becomes a handle too).

### 4. Retained state (materials)

- A mesh's state (render states, stages, textures, material) is the same every frame: `RMaterial_t` /
  `RDeltaState` (native material.cpp) get an rvk state block built once and rebuilt on change; a draw names the
  block instead of the loose state, and the render thread's constant block becomes a lookup.
- Identical meshes with the same block (trees, rocks, fences) become instanced draws.

### 5. Culling on the GPU (optional, hardware permitting)

- With meshes and state retained, the per-frame list of visible static meshes can be culled and its indirect commands
  written by a compute pass (frustum, and the depth pre-pass's previous frame for occlusion). Only on hardware that
  benefits; the CPU path stays the default.

## What stays

- DisplaySystem's ~120 visuals keep drawing through render_t's D3D7-shaped calls (docs/architecture.md); phases 1-2
  make those cheaper, phases 3-4 apply to what we own (static meshes, characters, sprites) and can then be extended to
  DisplaySystem's big visuals (terrain, rooms, water) by replacing their virtual methods.
- Surfaces (textures, render targets) stay on the COM shim (`RSurface`): they are created, locked and blitted, not
  per-draw work.
- The D3D7 path stays working: the original's DirectDraw (`RANDYVK_DDRAW` unset) and every `[Native]` mode off.

## Hardware notes

rvk needs Vulkan 1.3 with dynamic rendering, push descriptors, descriptor indexing and extended dynamic state 3. The
phases above lower CPU cost on every machine; phase 5 adds GPU work and is only worth it where the GPU has headroom, so
it would be a setting (off by default) rather than a requirement.
