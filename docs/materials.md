# Material maps (normal maps)

randy-vk side-loads a normal map for any RDB texture from `<client>\randy-vk\materials\`:

- `<type>_<id>_n.png` (or `<id>_n.png`; texture ids are unique across the texture tables), any size,
  tangent-space, **OpenGL convention** (+Y = up in the image; Blender's default bake). Mips are made on load
  (averaged and renormalised normals).
- ao-assets writes templates with the right names: `python -m aoassets material-template <texture or model>
  [--luma]` (flat, or relief from the texture's brightness like the generated normals).

## How a surface learns its texture id

Every RDB texture comes from DisplaySystem's RDB texture object (`FUN_100774c7`): it builds an
`AnarchyTexCreator_t` (+0x40 type, +0x44 id, flag +0x48 = 1) and `new RTexture_t(name, creator)`; randy's
constructor calls the creator's CreateTexture (`FUN_1006809e`), which calls back into the **exported**
`TextureStreamCreator::CreateTexture(LBitmap_t*|PositionIO_t*, name)` with the creator as `this`. The proxy
implements both exports (`HOOKED_EXPORTS` in tools/gen_interface.py, proxy/ddraw/rvk_materials.cpp): call the
original, check the creator's class by its RTTI (`.?AVAnarchyTexCreator_t@@` via vtable[-1]), and tag the
returned `surface_t`'s `IDirectDrawSurface7` - if `QueryInterface(IID_RvkSurface)` says it is ours - with the
id. The rvk texture gets its normal map then, or when it is (re)created (`AttachMaterialMaps`).
Log lines: `materials: N files in ...`, `materials: RDB texture T:ID is a WxH surface`, `materials: normal map
... on RDB texture T:ID`.

## Rendering

`Device::SetNormalMap(texture, normal)` (the texture owns it), binding 9 (`m_flatNormal` when none),
`F_NORMALMAP` when stage 0's texture has one and the draw is per-pixel lit with plain coordinates (not the ground's
lightmap pass). The shader turns the normal (x, y, z) into the height slope (-x/z, y/z) per world unit along u and
down-v and bends the normal with the same screen-derivative code as the generated normals (`BendNormal`), so no
tangents are needed. Strength `RVK_NormalStr` (C.vtx.z), on/off `RVK_NormalMaps` (Renderer tab).

Test: `tools/rvk-demo.sh build/nm --normal-map-test --bump 3`: a flat normal map is pixel-identical to none, and
a normal map computed from a height texture matches the generated normals of that texture within 2/255; the
centre tile shows domes from the normal map alone, lit from the upper left.
