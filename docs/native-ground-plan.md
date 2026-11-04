# Native ground plan

Status (2026-10-20): built, on main for the next release (CHANGELOG.md, Unreleased).

The game's ground renderer (DisplaySystem.dll `AnarchyGround_t`) rebuilds a split tree of triangles on the game's
thread as the camera moves, draws its near ring with each cell's own tile and bakes everything beyond into blurry
patch pictures (2 texels a unit) that fill the 32-bit address space. Raising its radius stutters (docs/rvk.md,
"Ground range"). This replaces its drawing with rvk's own, from the same data (docs/ground-format.md): every cell
with its own tile at every distance, geometry built once per playfield, no rebuilding as the camera moves.

**Status (2026-10-18):** built; checked in rvk_demo on a synthetic playfield (drawn the game's way and natively: the
same tiles, turns and light) and on Stret West Bank's real data (its heights, tiles and 6 p.m. lightmap, exported by
`tools/ground-data.py`: the zone as in game). Not yet run in game. docs/rvk.md, "Native ground".

Setting `RVK_GroundOn` (Terrain; on). Off, or on a playfield it can't read (the `GNDA` format, ground data not as
expected), the game's ground draws as before.

## Approach: the same two passes, from our data

rvk's enhancements key on the game's ground draws: the format (XYZ | NORMAL | TEX2, identity world: `IsTerrain`), a
base pass (unlit, the tile) and a lighting pass (lit, the lightmap, multiplying it). Cliff texturing, shadows, hill
shade, mist, wet ground, fog, the sand and rain maps all read them. So the native ground draws the same two passes,
through the same `Draw` path, with the game's own render state for each: when the game's first ground draw of a pass
comes in a frame, rvk draws its patches for that pass instead, and skips every game ground draw.

- **Geometry** (`rvk/native_ground.cpp`): per 64-cell patch, a static vertex buffer of (P+1)^2 vertices - world x,
  height, z, the normal from the heights, set 0 = (x, z) / cell size (cell coordinates: the cliff fit reads it as a
  tile laid from above at one repeat a cell), set 1 = (x, z) / (cells across, down) (the whole playfield's lightmap)
  - and a shared index buffer, through `DrawSharedIndexed` (arena-resident: no per-frame copies). Patches outside the
  view frustum or past the fog's end are not drawn.
- **Base pass** (`terrain.glsl`, native mode: `D.terrain.y` the tile-code texture's bindless image + 1): each pixel's
  cell from its world position; the cell's tile and turn from a tile-code texture (RGBA8, one texel a cell: its
  tile's bindless image index in R, G; its turn in B); the tile read at the cell-local coordinates turned (the turns'
  mappings, docs/ground-format.md), with the derivatives of the continuous cell coordinates (no seams in mip
  selection), through the game's clamped sampler. Cliff faces, rock, the hexagons and the variation as before (the
  faces' texture without rock: the cell's tile).
- **One pass of the game's**: some playfields (the Shadowlands') have their ground drawn in one lit, opaque pass: the
  lightmap and the lights added in stage 0, the tile multiplied in by stage 1. Such a draw stands for both passes
  (`DrawNativeGroundSingle`): the lighting pass's state is its own with stage 1 off, the base pass's is made from it
  (unlit, the tile's stage as stage 0). `rvk_demo --native-ground-test --ground-single` draws the game's ground so.
- **Lighting pass**: the game's lighting state, its stage 0 texture replaced by the playfield's lightmap - the
  baked map of the hour as the game handed it to `AnarchyGround_t::SetShadowMap` (4-bit, high nibble first, through
  the game's palette: grey `(i / 15 x 188 + 4) x (1 - shade)`), one texel a cell.

## Game side (`proxy/native/ground.cpp`)

- The playfield's ground: in the `Tesselate` and N3 `n3GroundRenderer_t::RunFunction` (each frame) hooks, a ground
  whose data (`+0x17C`) is new is read - its size, patch
  size, patches' compressed heights and tile codes (`AnarchyGroundData_t` `+0x34/+0x38/+0x40/+0x50/+0x54/+0x58`,
  `Patch_t` `+0x08/+0x10/+0x14/+0x28/+0x2C`), its scale (`+0x198/+0x19C/+0x1A0`), decoded (zlib: stb's).
- Its tile textures: the game's own loader (`FUN_10036594(data, tile)`: an `RTexture_t`, cached in the data), a few
  a frame, each `RTexture_t` -> `surface_t` (`+0x30`) -> our surface -> its rvk texture.
- The hour's lightmap: `SetShadowMap` hooked (the data, its size, the shade).
- While native: the game's full-quality radius at its smallest and its far patches baked at the fewest texels (its
  own work and memory, unseen).

## Since the first version

- Ground grass on it; the tiles' side-loaded normal and material maps, per cell; the sky's visibility from the
  heights (`RVK_GroundAo`); the slopes' light turned with the sun and the hours faded (`RVK_GroundSun`); tessellation
  near the eye (`RVK_GroundTess`, `RVK_CliffDepth`); rocks at the feet of cliffs (`RVK_GroundRocks`). docs/rvk.md,
  "Native ground".

## Not yet

- Level of detail (whole patches at full detail, culled by view and fog - ~8k triangles a patch).
- `GNDA` playfields (the game draws them).

## Checks

- `rvk_demo --native-ground-test`: a synthetic playfield through the same code (heights, tile codes, turns, a
  lightmap), against the cell-per-draw drawing the game does.
- In game: Stret West Bank against the frame dumps (the ground's shape, tiles, turns and light as the game's).
