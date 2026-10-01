#version 450
// Ambient occlusion (hdr.cpp): separable depth-aware blur at half resolution - neighbours at a different depth
// (another object) don't mix in.
layout(set = 0, binding = 0) uniform sampler2D src;      // occlusion, view depth
layout(push_constant) uniform Push {
    vec4 params;        // direction xy (texels)
} P;
layout(location = 0) out vec4 outAo;

void main()
{
    ivec2 c = ivec2(gl_FragCoord.xy), size = textureSize(src, 0);
    vec2 centre = texelFetch(src, c, 0).rg;
    float sum = centre.r, weight = 1.0;
    for (int i = -4; i <= 4; ++i) {
        if (i == 0) continue;
        ivec2 q = clamp(c + ivec2(P.params.xy) * i, ivec2(0), size - 1);
        vec2 s = texelFetch(src, q, 0).rg;
        float w = exp(-float(i * i) / 8.0) * exp(-abs(s.g - centre.g) * 8.0 / max(centre.g, 1e-3));
        sum += s.r * w;
        weight += w;
    }
    outAo = vec4(sum / weight, centre.g, 0.0, 0.0);
}
