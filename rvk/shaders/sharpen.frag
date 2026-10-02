#version 450
// Sharpening after the temporal anti-aliasing (hdr.cpp): contrast adaptive (after AMD's CAS) - neighbours are
// subtracted in proportion to how much room the pixel's neighbourhood leaves before clipping, so edges sharpen without
// halos and flat or already contrasty areas stay as they are.
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform Push {
    vec4 params;        // strength (0 = none .. 1)
} P;
layout(location = 0) out vec4 outColor;

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy), size = textureSize(src, 0);
    vec3 e = texelFetch(src, p, 0).rgb;
    if (P.params.x <= 0.0) { outColor = vec4(e, 1.0); return; }
    vec3 b = texelFetch(src, clamp(p + ivec2(0, -1), ivec2(0), size - 1), 0).rgb;
    vec3 d = texelFetch(src, clamp(p + ivec2(-1, 0), ivec2(0), size - 1), 0).rgb;
    vec3 f = texelFetch(src, clamp(p + ivec2(1, 0), ivec2(0), size - 1), 0).rgb;
    vec3 h = texelFetch(src, clamp(p + ivec2(0, 1), ivec2(0), size - 1), 0).rgb;
    vec3 mn = min(e, min(min(b, d), min(f, h))), mx = max(e, max(max(b, d), max(f, h)));
    vec3 amp = sqrt(clamp(min(mn, 1.0 - mx) / max(mx, vec3(1e-4)), 0.0, 1.0));
    vec3 w = -amp / mix(8.0, 5.0, clamp(P.params.x, 0.0, 1.0));
    outColor = vec4(clamp((e + w * (b + d + f + h)) / (1.0 + 4.0 * w), 0.0, 1.0), 1.0);
}
