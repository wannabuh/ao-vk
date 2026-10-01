#version 450
// HDR scene -> 8-bit main target (hdr.cpp). Colours up to the knee pass unchanged, so the game's own look is kept;
// brighter ones roll off smoothly towards 1 by their brightest channel, which keeps their hue, and lose saturation
// the more they are overexposed, as bright light does.
layout(set = 0, binding = 0) uniform sampler2D scene;
layout(set = 0, binding = 1) uniform sampler2D bloom;   // half resolution, light above the bloom threshold, blurred
layout(set = 0, binding = 2) uniform sampler2D ao;      // half resolution: ambient occlusion (1 = open), view depth
layout(set = 0, binding = 3) uniform sampler2D depthTex; // full resolution scene depth (for the AO upsampling)
layout(set = 0, binding = 4) uniform sampler2D localFraction;   // how much of each pixel local lights lit
layout(push_constant) uniform Push {
    vec4 params;        // knee, exposure, bloom strength, ambient occlusion on (1)
    vec4 proj;          // D3D projection m[2][2], m[3][2] (view depth from the depth buffer)
} P;

// The half-resolution occlusion at this pixel: of the 4 nearest texels, those at this pixel's depth (the same
// surface), so an object's occlusion doesn't spill onto the background around its outline.
float Occlusion()
{
    ivec2 pix = ivec2(gl_FragCoord.xy), size = textureSize(ao, 0);
    float d = texelFetch(depthTex, pix, 0).r;
    if (d >= 1.0) return 1.0;
    float z = P.proj.y / (min(d, 0.999999) - P.proj.x);
    vec2 h = gl_FragCoord.xy * 0.5 - 0.5;        // AO texel i stands for full-resolution pixel 2i
    ivec2 base = ivec2(floor(h));
    vec2 f = h - vec2(base);
    float sum = 0.0, weight = 0.0;
    for (int j = 0; j < 4; ++j) {
        ivec2 o = ivec2(j & 1, j >> 1);
        vec2 s = texelFetch(ao, clamp(base + o, ivec2(0), size - 1), 0).rg;
        float bilinear = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y) + 1e-3;
        float w = bilinear / (1e-3 + abs(s.g - z) / z * 40.0);
        sum += s.r * w;
        weight += w;
    }
    return sum / weight;
}
layout(location = 0) out vec4 outColor;

void main()
{
    vec4 s = texelFetch(scene, ivec2(gl_FragCoord.xy), 0);
    vec3 glow = P.params.z > 0.0 ? texture(bloom, gl_FragCoord.xy / vec2(textureSize(scene, 0))).rgb * P.params.z : vec3(0.0);
    vec2 uv = gl_FragCoord.xy / vec2(textureSize(scene, 0));
    float occlusion = P.params.w > 0.5 ? Occlusion() : 1.0;
    // Occlusion takes ambient light away, not the light local lights shine into the corner.
    float local = P.params.w > 0.5 ? texelFetch(localFraction, ivec2(gl_FragCoord.xy), 0).r : 0.0;
    vec3 c = max((s.rgb * mix(occlusion, 1.0, local) + glow) * P.params.y, vec3(0.0));
    float m = max(c.r, max(c.g, c.b)), k = P.params.x;
    if (m > k) {
        float room = 1.0 - k;
        float shown = room > 0.0 ? k + room * (1.0 - exp(-(m - k) / room)) : 1.0;
        float white = m > 1.0 ? 1.0 - 1.0 / (1.0 + 0.25 * (m - 1.0)) : 0.0;   // 0 at 1, 0.2 at 2, 0.5 at 5
        c = mix(c * (shown / m), vec3(shown), white);
    }
    outColor = vec4(min(c, vec3(1.0)), clamp(s.a, 0.0, 1.0));
}
