#version 450
// Procedural ground grass (RVK_GrassOn, rvk/grass.cpp): a camera-centred field of blades over the terrain. The blades
// are generated on the CPU each frame in world space; this pass only transforms them. A standalone pipeline: it has no
// descriptor sets, everything it needs arrives through the push constant, so it never touches the scene's bindings.
layout(push_constant) uniform Grass {
    mat4 viewProj;      // world -> clip (D3D row-vector convention, as the scene's)
    vec4 sunDir;        // xyz: the direction the sunlight travels (negate to light); w: strength
    vec4 sunColor;      // rgb: the sun's colour; a: ambient
    vec4 params;        // xyz: the camera, w: the wind time (seconds)
} G;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in float inShade;      // 0 at the root, 1 at the tip

layout(location = 0) out vec3 vNormal;
layout(location = 1) out float vShade;

void main()
{
    gl_Position = G.viewProj * vec4(inPos, 1.0);
    vNormal = inNormal;
    vShade = inShade;
}
