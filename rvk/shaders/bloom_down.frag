#version 450
// Bloom (hdr.cpp), downsampling: 13 taps (Jimenez, "Next generation post processing in Call of Duty"). The first
// pass from the scene keeps only light above the threshold (soft knee) and weights its taps by brightness (Karis
// average), so single bright pixels don't flicker as they move.
layout(set = 0, binding = 0) uniform sampler2D src;
layout(set = 0, binding = 1) uniform sampler2D glow;    // first pass: what additive effects added (full resolution)
layout(push_constant) uniform Push {
    vec4 params;        // source texel size xy, threshold, first pass (1)
} P;
layout(location = 0) out vec4 outColor;

// Only light above the threshold: the excess of the brightest channel, eased in over the first 0.1 above it (so
// the glow doesn't switch on abruptly), nothing at or below it.
vec3 Prefilter(vec3 c)
{
    float m = max(c.r, max(c.g, c.b)), e = m - P.params.z, knee = 0.1;
    float excess = e <= 0.0 ? 0.0 : e < knee ? e * e / (2.0 * knee) : e - 0.5 * knee;
    return c * (excess / max(m, 1e-5));
}

float Weight(vec3 c) { return 1.0 / (1.0 + max(c.r, max(c.g, c.b))); }

// A tap: the source, or in the first pass the scene's light above the threshold plus the glow.
vec3 Tap(vec2 uv)
{
    if (P.params.w > 0.5)
        return Prefilter(texture(src, uv).rgb) + texture(glow, uv).rgb;
    return texture(src, uv).rgb;
}

void main()
{
    vec2 ts = P.params.xy, uv = gl_FragCoord.xy * 2.0 * ts;   // centre of this pixel in the (2x larger) source
    vec3 a = Tap(uv + ts * vec2(-2, -2)), b = Tap(uv + ts * vec2(0, -2));
    vec3 c = Tap(uv + ts * vec2(2, -2)), d = Tap(uv + ts * vec2(-2, 0));
    vec3 e = Tap(uv), f = Tap(uv + ts * vec2(2, 0));
    vec3 g = Tap(uv + ts * vec2(-2, 2)), h = Tap(uv + ts * vec2(0, 2));
    vec3 i = Tap(uv + ts * vec2(2, 2)), j = Tap(uv + ts * vec2(-1, -1));
    vec3 k = Tap(uv + ts * vec2(1, -1)), l = Tap(uv + ts * vec2(-1, 1));
    vec3 m = Tap(uv + ts * vec2(1, 1));
    vec3 result;
    if (P.params.w > 0.5) {
        // Five 2x2 boxes, brightness-weighted.
        vec3 b0 = (j + k + l + m) * 0.25, b1 = (a + b + d + e) * 0.25;
        vec3 b2 = (b + c + e + f) * 0.25, b3 = (d + e + g + h) * 0.25;
        vec3 b4 = (e + f + h + i) * 0.25;
        float w0 = Weight(b0) * 0.5, w1 = Weight(b1) * 0.125, w2 = Weight(b2) * 0.125, w3 = Weight(b3) * 0.125,
              w4 = Weight(b4) * 0.125;
        result = (b0 * w0 + b1 * w1 + b2 * w2 + b3 * w3 + b4 * w4) / (w0 + w1 + w2 + w3 + w4);
    } else {
        result = e * 0.125 + (a + c + g + i) * 0.03125 + (b + d + f + h) * 0.0625 + (j + k + l + m) * 0.125;
    }
    outColor = vec4(max(result, vec3(0.0)), 1.0);
}
