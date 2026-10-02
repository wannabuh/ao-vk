#version 450
// Shadow map pass: position through (world * light view-projection); texture coordinates for alpha-tested casters.
layout(push_constant) uniform Push {
    mat4 worldLightViewProj;    // raw D3DMATRIX memory: GLSL M * v == D3D v * M
    vec4 alpha;                 // x = alpha reference (0..1) for alpha-tested casters
    vec4 sway;                  // plants (sway.glsl): model y of the base, 1 / model height, tip sway, on
    vec4 windModel;             // the wind's direction in model space, per world unit of sway
    vec4 origin;                // the object's world x, z; wind time
} P;
layout(set = 0, binding = 0) uniform sampler2D swayTex;    // the caster's texture (bound for cut-out casters)
#include "sway.glsl"

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec2 inTex0;

layout(location = 0) out vec2 vTex0;

void main()
{
    vec3 pos = inPos;
    if (P.sway.w > 0.5) pos += P.windModel.xyz * SwayDistance(inPos, P.sway, P.origin.xy, P.origin.z);
    gl_Position = P.worldLightViewProj * vec4(pos, 1.0);
    vTex0 = inTex0;
}
