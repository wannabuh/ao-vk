# Material maps (normal maps, PBR materials, albedo, emissive)

randy-vk side-loads maps for any RDB texture from `<client>\randy-vk\materials\` (any size each; mips are made on
load):

| File | What |
|---|---|
| `<type>_<id>_n.png` | tangent-space normal map, **OpenGL convention** (+Y = up in the image; Blender's default bake) |
| `<type>_<id>_orm.png` | PBR material, glTF packing: **R occlusion, G roughness, B metallic** |
| `<type>_<id>_r.png`, `_m.png`, `_ao.png` | ... or separate greyscale roughness / metallic / occlusion maps, packed on load (a missing one is roughness 1, metallic 0, occlusion 1); ignored when there is an `_orm.png` |
| `<type>_<id>_d.png` | albedo (diffuse colour): drawn **instead of** the game's texture wherever it is used, alpha included (cut-outs and shadows use its alpha) |
| `<type>_<id>_e.png` | emissive: the light the surface gives off (black = none), added after its lighting; beyond white with HDR, so it blooms |

`<type>` is the full-quality table, which also covers the lower quality levels: 1010004 world (1010016/17),
1010006 ground (1010021/22), 1010011 character skins (1010019/20). `<id>_<suffix>` works for any type.

ao-assets makes and collects these files (its `docs/blender-workflow.md`):
- `python -m aoassets material-template <texture or model> [--luma|--relief] [--pbr [--roughness R] [--metallic M]]`
  writes the diffuse (reference), a flat or relief normal map and, with `--pbr`, an `_orm.png` (occlusion from the
  relief's creases). `ground:<surface>` does a ground surface and its transition tiles.
- GLB exports carry the side-loaded maps as the glTF material's normal, metallicRoughness, occlusion and base
  colour textures; `import` / `import-materials` writes what was painted or baked in Blender back as these files.

## Finding a surface's texture id in game

**Ctrl+Shift+I** with the mouse over a surface: the next frame's 3D draws are tested on the CPU against the cursor
(rvk `Device::RequestPick` / `PickDraw`: the triangles in clip space, nearest depth wins), and the log says what the
nearest one's texture is - `pick: RDB texture 1010004:55900 'brown_clan_teleporterside2', 256x256; maps: _n.png
_orm.png _e.png` - its `type:id` goes to the clipboard (paste it into `material-template`), and every surface drawn
with it flashes yellow for three seconds (`MAT_PICKED`). GPU-skinned characters are skinned on the CPU too in the
pick frame, so they are hit like anything else. rvk_demo `--pbr-test --pick X Y` tests it.

**Ctrl+Shift+J**: the same for the mesh under the cursor - `pick: RDB mesh 1010001:3545` (a character: `RDB character
model 1010002:5900`; a piece a character wears names it too). randy-vk learns the ids while DisplaySystem loads: the
exported RCATMesh_t constructor (characters) and RTriMesh_t::Archive (each static mesh read from its .abiff) are hooked
and find the RDBCATMesh_t / RDBMesh_t being loaded on the caller's stack (the log's `meshes:` lines). The meshes in the
world are clones sharing the loaded mesh's data (RTriMesh_t +0x184), so a picked mesh is looked up by its data. Both picks also go to `logs/last-pick.txt`, which the ao-assets workbench
follows (it jumps to the asset). `RANDYVK_PICK_AT=frame,x,y,I|J` picks without the keys (tests).

## How a surface learns its texture id

Every RDB texture comes from DisplaySystem's RDB texture object (`FUN_100774c7`): it builds an
`AnarchyTexCreator_t` (+0x40 type, +0x44 id, flag +0x48 = 1) and `new RTexture_t(name, creator)`; randy's
constructor calls the creator's CreateTexture (`FUN_1006809e`), which calls back into the **exported**
`TextureStreamCreator::CreateTexture(LBitmap_t*|PositionIO_t*, name)` with the creator as `this`. The proxy
implements both exports (`HOOKED_EXPORTS` in tools/gen_interface.py, proxy/ddraw/rvk_materials.cpp): call the
original, check the creator's class by its RTTI (`.?AVAnarchyTexCreator_t@@` via vtable[-1]), and tag the
returned `surface_t`'s `IDirectDrawSurface7` - if `QueryInterface(IID_RvkSurface)` says it is ours - with the
id. Ground textures are found through `RTexture_t(name, TextureCreator*)` and the `RDBGroundTexture_t` making them.
The rvk texture gets its maps then, or when it is (re)created (`AttachMaterialMaps`). Character skins (1010011)
come through the same creator.
Log lines: `materials: N files in ...`, `materials: RDB texture T:ID is a WxH surface`, `materials: normal map
... on RDB texture T:ID`, `materials: PBR material ... on RDB texture T:ID`, `materials: albedo ...`.

Decoding is `rvk/material_maps.h`, shared with `rvk_demo --pbr-maps`:
- normal maps: each mip averages the level above's normals and renormalises them;
- occlusion / roughness / metallic: occlusion and metallic average; roughness averages as GGX alpha squared
  (roughness^4) and is widened by what the normal map's bumps lose at that level (the averaged normal's length,
  vMF lobe: alpha^2 += 2 / kappa) - far away a bumpy glossy surface turns satin instead of sparkling;
- albedo: box filtered, colour weighted by alpha.

## Rendering

**Normal maps**: `Device::SetNormalMap(texture, normal)` (the texture owns it), bindless slot `texIdx.w`
(`m_flatNormal` when none), `F_NORMALMAP` when stage 0's texture has one and the draw is per-pixel lit with plain
coordinates (or the ground's lightmap pass: its chunk's base texture's map). The shader turns the normal (x, y, z)
into the height slope (-x/z, y/z) per world unit along u and down-v and bends the normal with the same
screen-derivative code as the generated normals (`BendNormal`), so no tangents are needed. Strength
`RVK_NormalStr`, on/off `RVK_NormalMaps` (Renderer tab).

**PBR materials**: `Device::SetMaterialMaps(texture, orm, albedo)`. A draw takes its stage 0 texture's material on
the same terms as its normal map (or, in the ground's lightmap pass, its chunk's base texture's); the record's
`mat` (constants.glsl `D.mat`: ORM image + sampler slot, `MAT_PBR` / `MAT_BASE`) carries it, so materials don't
split the constant blocks. ffp_main.glsl, per-pixel lit draws only (and not far foliage):
- the albedo for the specular colour is stage 0's texture times the material colour (the ground: its base texture);
- lights: the game's diffuse term weighted by 1 - metallic; the game's Blinn-Phong specular is replaced by GGX /
  height-correlated Smith / Schlick (lighting.glsl `PbrSpecular`), lit by each light's diffuse colour, scaled by
  pi to match the game's Lambert term (albedo x nl, no 1 / pi), shadows and the light override as before;
- ambient: the game's ambient terms x occlusion x (1 - metallic); plus an ambient specular through the split-sum
  approximation (`EnvBrdfApprox`, Karis), with specular occlusion (Lagarde), of the environment probe (below) where
  it has seen, else a sky-and-ground environment made of the ambient light, the fog colour and a little sunlight;
- specular anti-aliasing: the normal's change across the pixel widens alpha (Kaplanyan / Tokuyoshi);
- the specular is added after the texture stages, unclamped in the HDR scene. The ground's lightmap pass multiplies
  the base pass, so its highlight is divided by the base texture's colour; its sunlight is baked into the lightmap,
  so the sun's highlight is computed apart (shadowed by the lightmap pass's sun shadow) and its lightmap is scaled
  by 1 - metallic and the occlusion;
- screen-space reflections: smooth surfaces raise the reflectivity (metals most) and write their shading normal
  (octahedral, view space) into the motion vectors' zw (now RGBA16F; w above 1.5 marks it), which ssr.frag uses
  instead of the depth buffer's faceted normal;
- indirect light: the albedo attachment gets the diffuse part (x (1 - metallic) x occlusion).

Colours stay in the game's (gamma) space like everything else the renderer lights, so a fully rough non-metal looks
like the game's own lighting plus a faint sheen.

**Environment probe** (hdr.cpp `RenderEnvProbe`, HDR only): after each scene, `env_accum.frag` carries the HDR
scene into a 128 x 128 octahedral map of the directions around the camera (world space; alpha: how sure). Texels
whose direction is on screen move 12% a frame towards the scene there, unless what is there is nearer than 6 units
(the player's character would otherwise be reflected everywhere); sureness fades as the camera moves (half in ~140
units) and slowly with time, and a jump of 40+ units (a teleport, a zone) forgets it all. `env_filter.frag` then
prefilters it into a 192 x 128 atlas of five levels for roughness 0.08, 0.3, 0.5, 0.75 and 1 (48 GGX-lobe taps,
weighted by sureness; layout in env_common.glsl). The next frame's scene reads it through `FL.pbr2.w` (bindless
slot + 1, sampler slot, strength), between the two levels nearest the roughness, and mixes it over the analytic sky
by its sureness; the water reads it (binding 12) where its screen-space reflections miss. It only knows what the
camera has looked at, without parallax (a direction, not a place), so it is what lies around, not a mirror. While a
debug view colours the scene the probe keeps what it had. `RVK_PbrProbe` (strength, 0 = off).

**Emissive maps**: `Device::SetEmissiveMap(texture, emissive)`. A draw whose stage 0 texture (plain coordinates, not
the ground's passes nor screen-space draws) has one carries it in the record (`D.mat.w`, sampled with `D.mat.y`;
`MAT_EMISSIVE`); ffp_main.glsl adds map x `RVK_PbrGlow` (`FL.pbr2.z`) after the texture stages and night glow,
before the fog - lit or not, PBR material or not. Clamped to white without HDR.

**Albedo maps** replace the texture's image in the draw's record (stage 0, stage 1 and the ground's base) and for
shadow casters' alpha. The game's own uploads still go to the original texture.

Settings (Renderer tab, Lighting): `RVK_Pbr` (on/off), `RVK_PbrSpec` (highlights), `RVK_PbrEnv` (reflected
surroundings), `RVK_PbrSsr` (screen-space reflections on smooth materials, with reflections on), `RVK_PbrAo`
(occlusion map strength), `RVK_PbrProbe` (environment probe), `RVK_PbrGlow` (emissive maps' brightness, 0 = off),
`RVK_PbrDebug` (7 which surfaces have maps: the scene tinted green where a PBR material lights it, blue for a
normal map only, magenta for an albedo map, orange where an emissive map glows; 8 the emitted light only; 9 what
PBR surfaces reflect (probe or sky); 1 albedo, 2 roughness, 3 metallic, 4 occlusion, 5 normal, 6 highlights only -
other lit draws dimmed grey), `RVK_Albedo` (albedo maps on/off). Hotkeys: Ctrl+Shift+K steps through the debug views
(7, 8, 9, then the rest; switches PBR on), Ctrl+Shift+B toggles `RVK_Pbr`. They live in the frame block (`FL.pbr`,
`FL.pbr2`), which every lit 3D draw now writes.

## Tests

- `tools/rvk-demo.sh build/nm --normal-map-test --bump 3`: a flat normal map is pixel-identical to none, and a
  normal map computed from a height texture matches the generated normals of that texture within 2/255; the centre
  tile shows domes from the normal map alone, lit from the upper left.
- `tools/rvk-demo.sh build/pbr --pbr-test [--hdr --ssr 1] [--pbr-debug N] [--pbr-off]`: spheres, roughness 0.05 ..
  1 to the right; rows: the game's lighting (reference, middle: an emissive map's cyan bands, last: an albedo map's
  checker), red non-metal, gold, silver
  (last: occlusion stripes), over a material floor.
- `--pbr-maps DIR`: the floor and a ball get files through the loader (`floor_n/_ao/_r/_m/_d/_e.png`,
  `ball_n/_orm/_e.png`).
- `--pbr-room` (with `--pbr-test --hdr --frames 40`): unlit red, green and blue walls left, right and ahead for
  the environment probe; `--pbr-debug 9 --pbr-debug-late` shows what the surfaces reflect (the floor: red, blue,
  green from left to right), `--pbr-probe 0` the analytic sky only. `--water-lake --frames 40 [--pbr-probe 0]`: the
  lake reflects the hills where screen-space reflections miss.
- `--terrain-pbr [--grass-bench-yaw 3.8]`: the grass bench's ground the game's way (base pass + lightmap pass) with a
  glossy path through rough grass; the path catches the sun looking towards it.
