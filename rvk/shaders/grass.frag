#version 450
// Procedural ground grass (RVK_GrassOn, rvk/grass.cpp): a solid blade lit by the sun, darker at the root and brighter
// at the tip. Writes the scene colour and the motion vectors (grass is static in the world, so its motion is the
// camera's); the glow, light-fraction and albedo attachments keep what the scene left.
layout(set = 0, binding = 0) uniform GrassFrame {
    mat4 viewProj;
    mat4 prevViewProj;
    vec4 viewport;
} GF;
layout(push_constant) uniform Grass {
    vec4 sunDir;
    vec4 sunColor;
    vec4 params;
} G;

layout(location = 0) in vec3 vNormal;
layout(location = 1) in float vShade;
layout(location = 2) in vec4 vClip;
layout(location = 3) in vec4 vPrevClip;

layout(location = 0) out vec4 outScene;
layout(location = 3) out vec4 outMotion;

void main()
{
    vec3 n = normalize(vNormal);
    float d = max(dot(n, -normalize(G.sunDir.xyz)), 0.0);
    vec3 root = vec3(0.16, 0.30, 0.08);
    vec3 tip  = vec3(0.52, 0.78, 0.24);
    vec3 base = mix(root, tip, clamp(vShade, 0.0, 1.0));
    outScene = vec4(base * (G.sunColor.a + G.sunColor.rgb * d * G.sunDir.w), 1.0);
    vec2 now = vClip.xy / vClip.w, before = vPrevClip.xy / max(vPrevClip.w, 1e-6);
    outMotion = vec4(vPrevClip.w > 1e-6 ? (now - before) * 0.5 * GF.viewport.xy * vec2(1.0, -1.0) : vec2(0.0), 0.0, 1.0);
}
