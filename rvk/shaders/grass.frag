#version 450
// Procedural ground grass (RVK_GrassOn, rvk/grass.cpp): a solid, tapered blade tinted by the ground's own colour, lit
// by the sun (with its shadow map) and by the frame's nearby point / spot lights. Writes the scene colour and the
// motion vectors (grass is static in the world, bar the wind: the vertex shader's position covers both); the glow,
// light-fraction and albedo attachments keep what the scene left.
#include "frame_lights.glsl"                    // FL (binding 4): the sun, its shadow cascades, the frame's lights

layout(set = 0, binding = 0) uniform GrassFrame {
    mat4 viewProj;
    mat4 prevViewProj;
    vec4 viewport;
    vec4 wind;
    vec4 camera;
} GF;
layout(set = 0, binding = 1) uniform sampler2D bladeTex;              // the blade atlas (its vein)
layout(set = 0, binding = 5) uniform sampler2DArrayShadow shadowMap;  // the sun's cascades
// Which of the frame's 64 lights reach this tile (grass.cpp: the tile's box against each light's range), so a blade
// only tests the few lights that matter, not all of them.
layout(push_constant) uniform GrassPush { uvec2 lightMask; } GP;

layout(location = 0) in vec3 vPosW;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUv;
layout(location = 3) in float vShade;
layout(location = 4) in vec3 vTint;
layout(location = 5) in vec4 vClip;
layout(location = 6) in vec4 vPrevClip;

layout(location = 0) out vec4 outScene;
layout(location = 3) out vec4 outMotion;

// Sun visibility at a blade point: 1 lit, 0 in shadow. The first cascade that covers the point (the near, sharp one),
// one tap; a 3x3 PCF is not worth its cost on a blade, thicker with overdraw.
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
        return texture(shadowMap, vec4(ndc.xy * 0.5 + 0.5, float(c), ndc.z));
    }
    return 1.0;
}

// The frame's nearby point / spot lights that reach this tile (D3D7's attenuation, its range cut faded out rather than
// a hard circle). No shadows on a blade.
vec3 LocalLights(vec3 posW, vec3 n)
{
    vec3 sum = vec3(0.0);
    for (uint word = 0u; word < 2u; ++word) {
        uint mask = GP.lightMask[word];
        while (mask != 0u) {
            uint i = word * 32u + uint(findLSB(mask));
            mask &= mask - 1u;
            Light l = FL.lights[i];
            uint type = uint(l.position.w);
            if (type == 3u)
                continue;                        // the sun, already applied
            vec3 d = l.position.xyz - posW;
            float dist = length(d);
            if (dist > l.direction.w)
                continue;
            vec3 L = d / max(dist, 1e-6);
            float denom = l.atten.x + l.atten.y * dist + l.atten.z * dist * dist;
            float att = denom > 0.0 ? 1.0 / denom : 1.0;
            float r = dist / max(l.direction.w, 1e-6);
            float w = clamp(1.0 - r * r * r * r, 0.0, 1.0);
            att *= w * w;
            if (type == 2u) {                    // spot: the cone
                float rho = dot(-L, normalize(l.direction.xyz));
                if (rho <= l.spot.y)
                    att = 0.0;
                else if (rho < l.spot.x)
                    att *= pow(clamp((rho - l.spot.y) / max(l.spot.x - l.spot.y, 1e-6), 0.0, 1.0), l.atten.w);
            }
            sum += att * (max(dot(n, L), 0.0) * l.diffuse.rgb + l.ambient.rgb);
        }
    }
    return sum;
}

void main()
{
    // The atlas is sampled for its vein only: the blade is a solid, tapered strip (no alpha cut), so the field is not
    // see-through the way cut-out cards are.
    vec4 blade = texture(bladeTex, vUv);
    // A blade is a flat, double-sided strip (nothing is culled), so light both of its faces: flip the normal towards
    // the eye. Otherwise a blade whose normal points away from a light goes black although its lit side is what is
    // seen - dark spears across an otherwise lit field, worst with a point light off to one side.
    vec3 n = normalize(vNormal);
    if (dot(n, GF.camera.xyz - vPosW) < 0.0)
        n = -n;
    float d = max(dot(n, -normalize(FL.sunDir.xyz)), 0.0);
    float shadow = mix(1.0, SunShadow(vPosW, n), FL.shadowParams.y);
    // The ground's own colour, a little brighter than it (a blade catches the sun more than the flat ground does),
    // dark at the root and bright at the tip - kept close to the ground's own lit colour so the field doesn't glow.
    vec3 base = vTint * blade.rgb * mix(vec3(0.7), vec3(1.1), clamp(vShade, 0.0, 1.0));
    // The ambient follows the sun, so the grass goes dark at night with the rest of the scene.
    float sunLum = clamp(dot(FL.sunColor.rgb, vec3(0.299, 0.587, 0.114)), 0.0, 1.0);
    vec3 lit = FL.sunColor.rgb * d * shadow * 0.8 + LocalLights(vPosW, n) + vec3(0.08 + 0.3 * sunLum);
    outScene = vec4(base * lit, 1.0);
    vec2 now = vClip.xy / vClip.w, before = vPrevClip.xy / max(vPrevClip.w, 1e-6);
    outMotion = vec4(vPrevClip.w > 1e-6 ? (now - before) * 0.5 * GF.viewport.xy * vec2(1.0, -1.0) : vec2(0.0), 0.0, 1.0);
}
