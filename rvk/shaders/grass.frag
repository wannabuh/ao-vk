#version 450
// Procedural ground grass (RVK_GrassOn, rvk/grass.cpp): a solid blade lit by the sun, darker at the root and brighter
// at the tip. Writes the scene colour only (the glow, motion and albedo attachments keep what the scene left).
layout(push_constant) uniform Grass {
    mat4 viewProj;
    vec4 sunDir;
    vec4 sunColor;
    vec4 params;
} G;

layout(location = 0) in vec3 vNormal;
layout(location = 1) in float vShade;

layout(location = 0) out vec4 outScene;

void main()
{
    vec3 n = normalize(vNormal);
    float d = max(dot(n, -normalize(G.sunDir.xyz)), 0.0);
    vec3 root = vec3(0.16, 0.30, 0.08);
    vec3 tip  = vec3(0.52, 0.78, 0.24);
    vec3 base = mix(root, tip, clamp(vShade, 0.0, 1.0));
    outScene = vec4(base * (G.sunColor.a + G.sunColor.rgb * d * G.sunDir.w), 1.0);
}
