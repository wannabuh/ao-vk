#version 450
// Per-object motion blur: McGuire et al.'s reconstruction filter ("A Reconstruction Filter for Plausible Motion
// Blur", 2012). Samples along the neighbourhood's strongest motion; each sample counts if it is behind this pixel and
// this pixel's own motion covers it, or in front and its motion covers this pixel - so a moving object smears over
// a still background and not the reverse, and a still object (the character the camera follows) stays sharp.
layout(set = 0, binding = 0) uniform sampler2D colour;
layout(set = 0, binding = 1) uniform sampler2D depthTex;
layout(set = 0, binding = 2) uniform sampler2D motionTex;
layout(set = 0, binding = 3) uniform sampler2D tiles;    // neighbourhood strongest motion per 32-pixel tile
#include "motion_common.glsl"
layout(location = 0) out vec4 outColor;

float Cone(float dist, float radius) { return clamp(1.0 - dist / max(radius, 1e-3), 0.0, 1.0); }
float Cylinder(float dist, float radius) { return 1.0 - smoothstep(0.95 * radius, 1.05 * radius + 1e-3, dist); }
float SoftDepth(float za, float zb) { return clamp(1.0 - (za - zb) / max(0.02 * za, 0.05), 0.0, 1.0); }   // a in front of b

void main()
{
    ivec2 x = ivec2(gl_FragCoord.xy), size = ivec2(P.proj.zw);
    vec4 centre = texelFetch(colour, x, 0);
    vec2 vn = texelFetch(tiles, x / 32, 0).rg;
    float rn = 0.5 * length(vn);                                  // blur radius of the neighbourhood
    if (rn < 0.5) { outColor = centre; return; }
    vec2 vx = Motion(x);
    float rx = max(0.5 * length(vx), 0.5), zx = ViewZ(texelFetch(depthTex, x, 0).r);
    float weight = 1.0 / rx;
    vec3 sum = centre.rgb * weight;
    const int kSamples = 15;
    float jitter = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715)))) - 0.5;
    for (int i = 0; i < kSamples; ++i) {
        if (i == kSamples / 2) continue;
        float t = mix(-1.0, 1.0, (float(i) + jitter + 1.0) / float(kSamples + 1));
        ivec2 y = clamp(ivec2(vec2(x) + 0.5 + vn * 0.5 * t), ivec2(0), size - 1);
        float zy = ViewZ(texelFetch(depthTex, y, 0).r);
        float ry = max(0.5 * length(Motion(y)), 0.5), dist = length(vec2(y - x));
        float background = SoftDepth(zx, zy), foreground = SoftDepth(zy, zx);
        float a = foreground * Cone(dist, ry) + background * Cone(dist, rx) + Cylinder(dist, ry) * Cylinder(dist, rx) * 2.0;
        sum += texelFetch(colour, y, 0).rgb * a;
        weight += a;
    }
    outColor = vec4(sum / weight, centre.a);
}
