#version 450
// Shadow map pass: position through (world * light view-projection); texture coordinates for alpha-tested casters.
// The per-caster data (M4: one draw per group of casters) is read by gl_InstanceIndex; the draw sets firstInstance
// to the caster's record index. gl_InstanceIndex is not available in the fragment shader, so the alpha reference
// travels there as a flat varying.
struct ShadowRecord {
    mat4 world;                 // raw D3DMATRIX memory: GLSL M * v == D3D v * M
    vec4 alpha;                 // x = alpha reference (0..1) for alpha-tested casters
    vec4 sway;                  // plants (sway.glsl): model y of the base, 1 / model height, tip sway, on
    vec4 windModel;             // the wind's direction in model space, per world unit of sway
    vec4 origin;                // the object's world x, z; wind time
};
layout(set = 0, binding = 1, std430) readonly buffer ShadowRecords { ShadowRecord records[]; } gRecords;
// The cascade's / cube face's light view-projection (the records are shared between passes).
layout(push_constant) uniform Pass { mat4 lightViewProj; } P;
layout(set = 0, binding = 0) uniform sampler2D swayTex;    // the caster's texture (bound for cut-out casters)
#include "sway.glsl"

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec2 inTex0;

layout(location = 0) out vec2 vTex0;
layout(location = 1) flat out float vAlphaRef;

void main()
{
    ShadowRecord r = gRecords.records[gl_InstanceIndex];
    vec3 pos = inPos;
    if (r.sway.w > 0.5) pos += r.windModel.xyz * SwayDistance(inPos, r.sway, r.origin.xy, r.origin.z);
    gl_Position = P.lightViewProj * (r.world * vec4(pos, 1.0));
    vTex0 = inTex0;
    vAlphaRef = r.alpha.x;
}
