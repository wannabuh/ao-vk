#version 450
// Procedural ground grass (RVK_GrassOn, rvk/grass.cpp). The blades are baked in world-space tiles; this pass transforms
// them, fades them out near the field's edge and bends them with the wind. The frame's camera, radius and wind come in
// one uniform buffer; the sunlight and its shadows are the fragment shader's (the frame light block).

layout(set = 0, binding = 0) uniform GrassFrame {
    mat4 viewProj;      // world -> clip (D3D row-vector convention, as the scene's)
    mat4 prevViewProj;  // ... last frame (motion vectors); equals viewProj when the camera didn't move
    vec4 viewport;      // xy: target size in pixels; z: the field radius
    vec4 wind;          // x: time (s); yz: the wind's direction; w: strength
    vec4 camera;        // xyz: the camera
} GF;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in float inShade;      // 0 at the root, 1 at the tip
layout(location = 3) in float inPhase;      // the blade's wind phase
layout(location = 4) in float inHeight;     // the blade's height
layout(location = 5) in float inBaseY;      // its root's world y

layout(location = 0) out vec3 vPosW;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out float vShade;
layout(location = 3) out vec4 vClip;
layout(location = 4) out vec4 vPrevClip;

void main()
{
    // Fade the blade down to nothing near the field's edge, so its rim isn't a hard circle.
    float dist = length(inPos.xz - GF.camera.xz);
    float fade = clamp((GF.viewport.z - dist) / (GF.viewport.z * 0.35), 0.0, 1.0);
    fade = fade * fade * (3.0 - 2.0 * fade);
    vec3 p = inPos;
    p.y = inBaseY + (inPos.y - inBaseY) * fade;
    // The wind, per blade: a travelling gust, bending the upper part more (the square of the height along the blade).
    float t = GF.wind.x;
    float w = 0.6 * sin(t * 1.9 + inPhase) + 0.25 * sin(t * 3.7 + inPhase * 1.7) +
              0.35 * (0.5 + 0.5 * sin(t * 0.37 + inPhase * 0.1));
    vec3 wdir = vec3(GF.wind.y, 0.0, GF.wind.z);
    p += wdir * (0.12 * inHeight * fade * GF.wind.w * w * inShade * inShade);
    vec4 world = vec4(p, 1.0);
    vClip = GF.viewProj * world;
    vPrevClip = GF.prevViewProj * world;
    gl_Position = vClip;
    vPosW = inPos;                              // lighting and the shadow lookup: the blade's own spot
    vNormal = inNormal;
    vShade = inShade;
}
