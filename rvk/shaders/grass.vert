#version 450
// Procedural ground grass (RVK_GrassOn, rvk/grass.cpp): a camera-centred field of blades over the terrain. The blades
// are generated on the CPU each frame in world space; this pass only transforms them. A standalone pipeline: the
// frame's camera arrives in a small uniform buffer and the sunlight and its shadows in the frame light block.
#include "frame_lights.glsl"                    // FL (binding 4): the sun and its shadow cascades

layout(set = 0, binding = 0) uniform GrassFrame {
    mat4 viewProj;      // world -> clip (D3D row-vector convention, as the scene's)
    mat4 prevViewProj;  // ... last frame (motion vectors); equals viewProj when the camera didn't move
    vec4 viewport;      // xy: target size in pixels
} GF;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in float inShade;      // 0 at the root, 1 at the tip

layout(location = 0) out vec3 vPosW;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out float vShade;
layout(location = 3) out vec4 vClip;
layout(location = 4) out vec4 vPrevClip;

void main()
{
    vec4 world = vec4(inPos, 1.0);
    vClip = GF.viewProj * world;
    vPrevClip = GF.prevViewProj * world;
    gl_Position = vClip;
    vPosW = inPos;
    vNormal = inNormal;
    vShade = inShade;
}
