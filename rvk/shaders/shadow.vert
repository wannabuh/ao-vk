#version 450
// Shadow map pass: position through (world * light view-projection); texture coordinates for alpha-tested casters.
// The per-caster data (M4: one draw per group of casters) is read by gl_InstanceIndex; the draw sets firstInstance
// to the caster's record index. gl_InstanceIndex is not available in the fragment shader, so the alpha reference
// travels there as a flat varying.
// Built with RVK_LEAF (leaf_shadow.vert): a canopy's leaves (leaves.cpp) - no vertex input, six vertices a leaf from
// the leaf pool (binding 2), placed and swayed as leaf.vert places them (without their flutter).
struct ShadowRecord {
    mat4 world;                 // raw D3DMATRIX memory: GLSL M * v == D3D v * M
    vec4 alpha;                 // x = alpha reference (0..1) for alpha-tested casters
    vec4 sway;                  // plants (sway.glsl): model y of the base, 1 / model height, tip sway, on
    vec4 windModel;             // the wind's direction in model space, per world unit of sway
    vec4 origin;                // the object's world x, z; wind time; RVK_LeafCore (a canopy with leaves)
    vec4 leaf;                  // a canopy with leaves: its crown's centre (model space), radius (0 = none)
    vec4 leafWind;              // ... the wind's direction (world x, z), branch sway amount, gusts
};
layout(set = 0, binding = 1, std430) readonly buffer ShadowRecords { ShadowRecord records[]; } gRecords;
// The cascade's / cube face's light view-projection (the records are shared between passes).
layout(push_constant) uniform Pass { mat4 lightViewProj; } P;
layout(set = 0, binding = 0) uniform sampler2D swayTex;    // the caster's texture (bound for cut-out casters)
#include "sway.glsl"
#include "wind.glsl"

#ifdef RVK_LEAF
layout(set = 0, binding = 2, std430) readonly buffer Leaves { uvec4 words[]; } LV;   // ffp.vert's leaf records
const vec2 kLeafCorner[6] = vec2[6](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
                                    vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));
#else
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec2 inTex0;
#endif

layout(location = 0) out vec2 vTex0;
layout(location = 1) flat out float vAlphaRef;
layout(location = 2) out vec2 vLeaf;         // wind.glsl LeafMaskAt: a leaf's card position, a core's thinning

void main()
{
    ShadowRecord r = gRecords.records[gl_InstanceIndex];
#ifdef RVK_LEAF
    uint leaf = uint(gl_VertexIndex) / 6u;
    vec2 corner = kLeafCorner[uint(gl_VertexIndex) % 6u];
    uvec4 a = LV.words[2u * leaf], b = LV.words[2u * leaf + 1u];
    vec2 h0 = unpackHalf2x16(a.w), h1 = unpackHalf2x16(b.x), h2 = unpackHalf2x16(b.y);
    vec3 pos = uintBitsToFloat(a.xyz) + corner.x * vec3(h0, h1.x) + corner.y * vec3(h1.y, h2);
    vec2 uvHalf = vec2(float(b.w & 0xFFFu), float((b.w >> 12u) & 0xFFFu)) * (0.25 / 4095.0);
    vTex0 = unpackUnorm2x16(b.z) + corner * uvHalf;
    vLeaf = corner;
    vec3 swayPos = pos;
#else
    vec3 pos = inPos, swayPos = inPos;
    vTex0 = inTex0;
    vLeaf = vec2(0.0, 3.0);
    if (r.leaf.w > 0.0) {                        // a canopy with leaves: its cards as the scene draws them (ffp.vert)
        pos = mix(pos, r.leaf.xyz, CoreShrink(r.origin.w, 1.0));
        vLeaf = vec2(CoreThin(r.origin.w, 1.0), 2.0);
    }
#endif
    if (r.sway.w > 0.5) pos += r.windModel.xyz * SwayDistance(swayPos, r.sway, r.origin.xy, r.origin.z);
    vec4 world = r.world * vec4(pos, 1.0);
    world.xyz += BranchSwayAt(swayPos, r.leaf, r.origin.xy, r.leafWind.xy, r.origin.z, r.leafWind.z, r.leafWind.w);
    gl_Position = P.lightViewProj * world;
    vAlphaRef = r.alpha.x;
}
