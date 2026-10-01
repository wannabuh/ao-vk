#version 450
// Shadow map pass: position through (world * light view-projection); texture coordinates for alpha-tested casters.
layout(push_constant) uniform Push {
    mat4 worldLightViewProj;    // raw D3DMATRIX memory: GLSL M * v == D3D v * M
    vec4 alpha;                 // x = alpha reference (0..1) for alpha-tested casters
} P;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec2 inTex0;

layout(location = 0) out vec2 vTex0;

void main()
{
    gl_Position = P.worldLightViewProj * vec4(inPos, 1.0);
    vTex0 = inTex0;
}
