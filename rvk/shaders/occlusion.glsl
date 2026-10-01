// The tone mapping's inputs and ambient occlusion (hdr.cpp), shared with the depth of field composite.
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

// What the scene's colour is multiplied by for ambient occlusion: occlusion takes ambient light away, not the light
// local lights shine into the corner.
float AmbientFactor()
{
    if (P.params.w < 0.5) return 1.0;
    float local = texelFetch(localFraction, ivec2(gl_FragCoord.xy), 0).r;
    return mix(Occlusion(), 1.0, local);
}
