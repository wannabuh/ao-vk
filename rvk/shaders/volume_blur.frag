#version 450
// Volumetric light (hdr.cpp): separable blur at half resolution. Scattered light is smooth, so neighbours mix in
// unless they lie at a very different depth (a near object's edge against the far background).
layout(set = 0, binding = 0) uniform sampler2D src;      // scattered light, view depth
layout(push_constant) uniform Push {
    vec4 params;        // direction xy (texels), tap spacing
} P;
layout(location = 0) out vec4 outVolume;

void main()
{
    ivec2 c = ivec2(gl_FragCoord.xy), size = textureSize(src, 0);
    vec4 centre = texelFetch(src, c, 0);
    vec3 sum = centre.rgb;
    float weight = 1.0;
    for (int i = -4; i <= 4; ++i) {
        if (i == 0) continue;
        ivec2 q = clamp(c + ivec2(P.params.xy * P.params.z) * i, ivec2(0), size - 1);
        vec4 s = texelFetch(src, q, 0);
        float w = exp(-float(i * i) / 8.0) * exp(-abs(s.a - centre.a) * 2.0 / max(min(s.a, centre.a), 1e-3));
        sum += s.rgb * w;
        weight += w;
    }
    outVolume = vec4(sum / weight, centre.a);
}
