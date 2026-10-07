#version 450
// Procedural ground grass (RVK_GrassOn, rvk/grass.cpp): the blade is lit per vertex (grass.vert); here only a faint
// vein down its middle. Writes all the scene's targets as its surfaces do: the colour, no glow, the local lights' and
// the sun's shares, the motion and the albedo - the blades may go in before the ground (the game's first blended draw
// can come before it), so what lies under them is not the ground's.
layout(set = 0, binding = 0) uniform GrassFrame {
    mat4 viewProj;
    mat4 prevViewProj;
    vec4 viewport;
    vec4 wind;
    vec4 camera;
    vec4 ambient;
    vec4 look;
    vec4 sunColour;
    vec4 sunDir;
} GF;

layout(location = 0) in vec3 vColour;
layout(location = 1) in float vAcross;
layout(location = 2) in vec4 vClip;
layout(location = 3) in vec4 vPrevClip;
layout(location = 4) in vec3 vAlbedo;
layout(location = 5) in vec2 vShares;

layout(location = 0) out vec4 outScene;
layout(location = 1) out vec4 outGlow;
layout(location = 2) out vec4 outLocal;
layout(location = 3) out vec4 outMotion;
layout(location = 4) out vec4 outAlbedo;

void main()
{
    float vein = 1.04 - 0.14 * exp(-vAcross * vAcross * 12.0);   // its mean across the blade is 1
    outScene = vec4(vColour * vein, 1.0);
    outGlow = vec4(0.0);
    outLocal = vec4(vShares.x, 0.0, vShares.y, 1.0);
    outAlbedo = vec4(clamp(vAlbedo * vein, 0.0, 1.0), 1.0);
    vec2 now = vClip.xy / vClip.w, before = vPrevClip.xy / max(vPrevClip.w, 1e-6);
    outMotion = vec4(vPrevClip.w > 1e-6 ? (now - before) * 0.5 * GF.viewport.xy * vec2(1.0, -1.0) : vec2(0.0), 0.0, 1.0);
}
