# Character animation in randy31

From the decompiled stock `randy31.dll` (`re/anim*.c`); replaced by `proxy/native/cat_anim.cpp` with
`randy-vk.ini [Native] Anim=on`.

## Keyframes (`CATKeyframeAnimData_t`)

`+0x38`: one track per bone, 0x10 bytes: rotation key count, rotation keys (0x14 bytes: time, quaternion x y z w),
position key count, position keys (0x10 bytes: time, x y z). Times in ms.

Sampling (FUN_10051d2a rotations, FUN_10051df4 positions; thiscall data, out, bone, time): a binary search for the
keys around the time, then `t` = the fraction between them. `t` under 0.01 (`_DAT_10095b88`) gives the earlier key
unchanged; past the last key, or with a single key, the **first** key (the game wraps times itself). Rotations
interpolate with FUN_10053d0b, a slerp the shorter way (linear when `1 - dot < 1e-6`); positions linearly.

`CATKeyframeAnim_t` (vtable 0x10095BA4): `+0x4C` its data, `+0x50` its time (SetTime), `+0x54` layers. Slots +0x14 /
+0x18 sample a bone's rotation / position. `CATAnimBlend_t` (vtable 0x10095B40) blends two animations through those
slots.

## Bones (FUN_100540a5, thiscall CATRender_t, parent matrix, bone, scale)

From the root (bone 0, identity, scale 1; FUN_10054d16): rotation and position from the animation's slots +0x14 /
+0x18; the bone's matrix = the rotation's matrix through the parent's 3x3, position times `scale` through the parent;
written to the bone matrices (`CATRender_t +0x18`, 0x30 each). Per bone a controller (`+0x20`, 0x10 each: type, user,
fn1, fn2): type 1 calls fn1(render, parent, q, position, user) to adjust first; type 2 calls fn2(out, render, parent,
q, position * scale, user), which makes the matrix itself. Then the children (`CATMesh_t +0x44`, 0x28 per bone:
`+0x1C` the children's scale factor, `+0x20` count, `+0x24` list), recursively with scale * factor.
