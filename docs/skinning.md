# Character skinning in randy31

How the game's renderer animates characters on the CPU, from Ghidra decompiles of the stock `randy31.dll`
(`randy31_orig.dll`; addresses at image base 0x10000000). Decompiles: `re/skin*.c` (regenerate with
`tools/ghidra-run.sh randy31.dll DumpDecomp.java ...`). Groundwork for moving skinning off the game thread.

## Objects

`RCATMesh_t` (0x438 bytes) starts with a `CATRender_t` (0x3C bytes); its `RVisual_t` base follows at +0x3C.

`CATRender_t`:

| offset | |
| --- | --- |
| +0x04 | `CATMesh_t*` (the mesh resource) |
| +0x08 | `CATAnim_t*` (current animation; null = rest pose) |
| +0x0C / +0x10 | piece-group count / array, 0xC bytes each: `[0]` `CATMesh_t*`, `[2]` array of 8-byte vertex buffer slots |
| +0x14 / +0x18 | bone count / bone matrices, 0x30 bytes each (3x4: rows 0-2 at floats 0-8, translation at 9-11) |
| +0x24 | change counter (SetMesh / SetAnim bump it) |
| +0x28 | byte: 1 = rest pose (copy bind position and normal) |
| +0x2C / +0x30 | vertex process callbacks (`std::vector` of {user, fn}) |

A mesh group (`CATMesh_t+0x4C`, 0x34 bytes each) has its pieces (`CATTriPolyList_t`) at +0x20 (count +0x1C), 0x34
bytes each: +0x0C input vertices, +0x14 indices (u16), +0x10 active triangle count (>0 = drawn), +0x2C vertex count,
+0x30 triangle count.

Each piece of each character has **its own** vertex buffer: slot `[1]` is a `VertexBuffer_c` whose impl's first field
is the `IDirect3DVertexBuffer7` - in ao-vk an `RVertexBuffer`. Format 0x112 (XYZ, NORMAL, TEX1; 32 bytes).
`CATRender_t::SetMesh` (0x55108) creates them and writes the texture coordinates once; skinning writes position and
normal only.

## Input vertex (`CATTriVertex_t`, 0x44 bytes)

| floats | |
| --- | --- |
| 0-2 | position in bone A's space |
| 3-5 | position in bone B's space |
| 6-8 | bind (rest) position |
| 9-11 | normal (bone A's space; also the rest normal) |
| 12-13 | texture coordinates |
| 14, 15 | bone A, bone B (int32) |
| 16 | weight of bone A |

## The skinning loop: `FUN_1005470d(CATRender_t*, float* boxMin, float* boxMax)`

For every group, piece and vertex: `VertexBuffer_c::Lock(0, 0)`, then

- rest pose: position = bind, normal = normal;
- weight <= 0.99f (`_DAT_10095db8`, the double 0.9900000095): position = w * (A * posA) + (1 - w) * (B * posB), normal = rot(A) * normal;
  vertices whose bones are out of range are left as they were;
- else (one bone): position = A * posA, normal = rot(A) * normal;

then the box grows by the position (x87, scalar). After each piece every registered callback runs (below), then
`Unlock`. Box: RCATMesh +0x1FC (min) / +0x208 (max), read only by line intersection (picking) -
`RCATMesh_t::IsLineIntersecting` (export 0x56786) and the vtable method `FUN_1005632d` (slot at 0x10095F0C),
which also read the vertex buffers back (`Lock(0, DDLOCK_READONLY)`).

Callers: `FUN_10055c1c` runs it only when the animation state changed (`FUN_10055a23` = change counter + animation
frame vs RCATMesh +0x1C4). Drawing: `FUN_10056ed6` (RCATMesh render, vtable data 0x10095F10) -> `FUN_10055dba` (world
matrix, bones `FUN_10055d52`, skin `FUN_10055c1c`) -> per piece and material `render_t::RenderTriangleList(vb, ...)`;
an environment map pass and the CAT light pass draw the same buffers again.

## Vertex process callbacks

`fn(CATRender_t*, vertexCount, const CATTriVertex_t* in, CATVertex_t* out, triCount, const u16* indices,
vertexBase, triBase, void* user)`, per piece, `out` = the locked buffer.

| registered by | callback | uses `out` |
| --- | --- | --- |
| `GfxVisualSimpleShadow_c` (DisplaySystem, every character with a blob shadow) | 0x1001E75D | reads the position of every 4th vertex |
| GfxVisual body effects (DisplaySystem 0x1001CE63, 0x1001DEB2) | 0x1001CD09, 0x1001DC74 | copy all vertices (effect active only) |
| `_GfxControl_t` (Gamecode 0x100D7752) | 0x100D7217 | **moves** vertices (deforming effects) |
| Gamecode 0x100F7A63 | 0x100F6EB6 | effect on a character's mesh |

## Consequences for off-thread / GPU skinning

- The skinned vertices are only needed on the game thread by: the blob shadow (sparse positions), active body effects
  (all vertices, sometimes modified), and picking (box + vertices, on demand).
- So the loop can be replaced by: snapshot the bone matrices, attach a skin job to each piece's `RVertexBuffer`, give
  the blob shadow callback a sparse position buffer, and fall back to the original loop for a character with any other
  callback. Picking materializes the vertices (and box) first.
- Draws of a buffer with a job are skinned by the renderer (worker thread, SIMD; or a compute shader later), and are
  known to be characters (no need for the animated-topology heuristic).

## What ao-vk does (proxy/native/cat_skin.cpp, rvk/skin.cpp)

`[Native] Skin=on` replaces the loop with deferred skinning; see docs/native.md for the modes and measurements. The
blob shadow callback gets the positions of every 4th vertex (all it reads); the box is computed only for picking,
which skins its character on the game's thread first.
