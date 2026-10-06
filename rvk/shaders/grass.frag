#version 450
// Procedural ground grass (RVK_GrassOn, rvk/grass.cpp): a solid blade lit by the sun, darker at the root and brighter
// at the tip, darkened where the sun's shadow map says it is in shadow. Writes the scene colour and the motion vectors
// (grass is static in the world, so its motion is the camera's); the glow, light-fraction and albedo attachments keep
// what the scene left.
#include "frame_lights.glsl"                    // FL (binding 4): the sun, its shadow cascades and their parameters

layout(set = 0, binding = 0) uniform GrassFrame {
    mat4 viewProj;
    mat4 prevViewProj;
    vec4 viewport;
} GF;
layout(set = 0, binding = 5) uniform sampler2DArrayShadow shadowMap;   // the sun's cascades

layout(location = 0) in vec3 vPosW;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in float vShade;
layout(location = 3) in vec4 vClip;
layout(location = 4) in vec4 vPrevClip;

layout(location = 0) out vec4 outScene;
layout(location = 3) out vec4 outMotion;

// Sun visibility at a blade point: 1 lit, 0 in shadow. The first cascade that covers the point (the near, sharp one),
// a small PCF; no soft-shadow blocker search (a blade is tiny).
float SunShadow(vec3 posW, vec3 n)
{
    if (FL.shadowParams.x < 0.5 || dot(FL.sunColor.rgb, FL.sunColor.rgb) <= 0.0)
        return 1.0;
    vec3 L = -normalize(FL.sunDir.xyz);
    if (dot(normalize(n), L) <= 0.0)
        return 1.0;
    int count = int(FL.shadowParams.z);
    for (int c = 0; c < count; ++c) {
        vec4 sc = FL.shadowViewProj[c] * vec4(posW, 1.0);
        vec3 ndc = sc.xyz / sc.w;
        if (max(abs(ndc.x), abs(ndc.y)) >= 1.0 || ndc.z <= 0.0 || ndc.z >= 1.0)
            continue;
        vec2 uv = ndc.xy * 0.5 + 0.5;
        vec2 ts = 1.0 / vec2(textureSize(shadowMap, 0).xy);
        float sum = 0.0;
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x)
                sum += texture(shadowMap, vec4(uv + vec2(x, y) * ts, float(c), ndc.z));
        return sum / 9.0;
    }
    return 1.0;
}

void main()
{
    vec3 n = normalize(vNormal);
    float d = max(dot(n, -normalize(FL.sunDir.xyz)), 0.0);
    float shadow = mix(1.0, SunShadow(vPosW, n), FL.shadowParams.y);
    vec3 root = vec3(0.16, 0.30, 0.08);
    vec3 tip  = vec3(0.52, 0.78, 0.24);
    vec3 base = mix(root, tip, clamp(vShade, 0.0, 1.0));
    outScene = vec4(base * (FL.sunColor.rgb * d * shadow + vec3(0.45)), 1.0);
    vec2 now = vClip.xy / vClip.w, before = vPrevClip.xy / max(vPrevClip.w, 1e-6);
    outMotion = vec4(vPrevClip.w > 1e-6 ? (now - before) * 0.5 * GF.viewport.xy * vec2(1.0, -1.0) : vec2(0.0), 0.0, 1.0);
}
