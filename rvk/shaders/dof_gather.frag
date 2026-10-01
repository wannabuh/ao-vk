#version 450
// Depth of field: the blur, at half resolution - samples on a disc (golden-angle spiral) as large as the largest
// circle of confusion around (tiles). Depth-aware: a sample behind this pixel only counts within the smaller of the
// two circles (the background doesn't blur over a sharper foreground), one in front within its own (an out-of-focus
// foreground spreads over what's behind it). Normal: Gaussian weights. Bokeh: a hexagonal aperture, flat weights
// and bright samples weighted up, so highlights become hexagons. Alpha: how far near-field blur reaches this pixel.
layout(set = 0, binding = 0) uniform sampler2D halfTex;     // colour, CoC (full-resolution pixels)
layout(set = 0, binding = 1) uniform sampler2D tiles;    // neighbourhood largest CoC per tile
#include "dof_common.glsl"
layout(location = 0) out vec4 outColor;

float Hexagon(float angle)                               // aperture radius at this angle, 1 at the corners
{
    const float kSide = 1.0471976;                       // 60 degrees
    float a = mod(angle + 0.5236, kSide) - 0.5 * kSide;
    return 0.8660254 / cos(a);
}

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy), size = textureSize(halfTex, 0) - 1;
    vec4 centre = texelFetch(halfTex, p, 0);
    float radius = 0.5 * texelFetch(tiles, p / 16, 0).r;  // half-resolution pixels
    if (radius < 0.5) { outColor = vec4(centre.rgb, 0.0); return; }
    bool bokeh = (int(P.params.w) & 2) != 0;
    float cocC = 0.5 * abs(centre.a);
    const int kSamples = 48;
    vec3 sum = centre.rgb;
    float weight = 1.0, nearReach = 0.0;
    for (int i = 0; i < kSamples; ++i) {
        float r = radius * sqrt((float(i) + 0.5) / float(kSamples)), angle = float(i) * 2.3999632;
        vec2 o = vec2(cos(angle), sin(angle)) * r;
        vec4 s = texelFetch(halfTex, clamp(p + ivec2(round(o)), ivec2(0), size), 0);
        float cocS = 0.5 * abs(s.a);
        bool inFront = s.a < centre.a - 0.5;              // CoC rises with depth: smaller = nearer
        float reach = inFront ? cocS : min(cocS, cocC);
        if (bokeh) reach *= Hexagon(angle);
        float cover = clamp(reach - r + 0.5, 0.0, 1.0);   // inside the sample's circle, with a soft edge
        if (cover <= 0.0) continue;
        float w = cover;
        if (bokeh) {
            float luma = dot(s.rgb, vec3(0.3, 0.59, 0.11));
            w *= 1.0 + 4.0 * luma * luma;                 // highlights dominate their disc
        } else {
            float sigma = max(0.5 * reach, 0.5);
            w *= exp(-r * r / (2.0 * sigma * sigma));
        }
        sum += s.rgb * w;
        weight += w;
        if (inFront && s.a < 0.0) nearReach = max(nearReach, abs(s.a) * cover);
    }
    outColor = vec4(sum / weight, nearReach);
}
