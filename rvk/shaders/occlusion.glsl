// The tone mapping's inputs, ambient occlusion and indirect light (hdr.cpp), shared with the depth of field composite.
layout(set = 0, binding = 0) uniform sampler2D scene;
layout(set = 0, binding = 1) uniform sampler2D bloom;   // half resolution, light above the bloom threshold, blurred
layout(set = 0, binding = 2) uniform sampler2D ao;      // half resolution: ambient occlusion (1 = open), view depth
layout(set = 0, binding = 3) uniform sampler2D depthTex; // full resolution scene depth (for the AO upsampling)
layout(set = 0, binding = 4) uniform sampler2D localFraction;   // how much of each pixel local lights lit
layout(set = 0, binding = 5) uniform sampler2D gi;      // half resolution: indirect light, view depth
layout(set = 0, binding = 6) uniform sampler2D albedo;  // the surfaces' own colour (fogged)
layout(set = 0, binding = 7) uniform sampler2D volume;  // half resolution: light scattered by the air, view depth
layout(set = 0, binding = 8) uniform sampler2D ssr;     // reflected colour, how much of it shows
layout(push_constant) uniform Push {
    vec4 params;        // knee, exposure, bloom strength, bloom left on objects nearer than its light (-1: no depth)
    vec4 proj;          // D3D projection m[2][2], m[3][2] (view depth from the depth buffer), indirect light strength,
                        // volumetric light on (bit 1), reflections on (bit 2), ambient occlusion on (bit 4)
} P;

// A half-resolution image at this pixel: of the 4 nearest texels, those at this pixel's depth (the same surface,
// view depth in channel depthChannel), so an object's occlusion or light doesn't spill onto the background around
// its outline.
vec4 Upsample(sampler2D t, int depthChannel)
{
    ivec2 pix = ivec2(gl_FragCoord.xy), size = textureSize(t, 0);
    float z = P.proj.y / (min(texelFetch(depthTex, pix, 0).r, 0.999999) - P.proj.x);
    vec2 h = gl_FragCoord.xy * 0.5 - 0.5;        // texel i stands for full-resolution pixel 2i
    ivec2 base = ivec2(floor(h));
    vec2 f = h - vec2(base);
    vec4 sum = vec4(0.0);
    float weight = 0.0;
    for (int j = 0; j < 4; ++j) {
        ivec2 o = ivec2(j & 1, j >> 1);
        vec4 s = texelFetch(t, clamp(base + o, ivec2(0), size - 1), 0);
        float bilinear = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y) + 1e-3;
        float w = bilinear / (1e-3 + abs(s[depthChannel] - z) / z * 40.0);
        sum += s * w;
        weight += w;
    }
    return sum / weight;
}

float Occlusion()
{
    if (texelFetch(depthTex, ivec2(gl_FragCoord.xy), 0).r >= 1.0) return 1.0;
    return Upsample(ao, 1).r;
}

// The light surfaces reflect from the lit scene around them (indirect light x their own colour); 0 when off.
vec3 Indirect()
{
    if (P.proj.z <= 0.0 || texelFetch(depthTex, ivec2(gl_FragCoord.xy), 0).r >= 1.0) return vec3(0.0);
    return Upsample(gi, 3).rgb * texelFetch(albedo, ivec2(gl_FragCoord.xy), 0).rgb * P.proj.z;
}

// What the scene's colour is multiplied by for ambient occlusion: occlusion takes ambient light away, not the light
// local lights shine into the corner.
float AmbientFactor()
{
    if ((int(P.proj.w) & 4) == 0) return 1.0;
    float local = texelFetch(localFraction, ivec2(gl_FragCoord.xy), 0).r;
    return mix(Occlusion(), 1.0, local);
}

// Light scattered towards the camera by the air in front of this pixel (volumetric light); 0 when off.
vec3 Volumetric()
{
    if ((int(P.proj.w) & 1) == 0) return vec3(0.0);
    return Upsample(volume, 3).rgb;
}

// The surface's colour with its reflection over it (screen-space reflections).
vec3 Reflected(vec3 c)
{
    if ((int(P.proj.w) & 2) == 0) return c;
    vec4 r = texelFetch(ssr, ivec2(gl_FragCoord.xy), 0);
    return mix(c, r.rgb, r.a);
}

// The bloom at this pixel, held back where the pixel is well in front of the light the bloom spreads (the levels carry
// the light's brightness / view depth): foliage before a lamp doesn't glow with the lamp's halo.
vec3 Bloom()
{
    vec4 b = texture(bloom, gl_FragCoord.xy / vec2(textureSize(scene, 0)));
    if (P.params.w < 0.0) return b.rgb;
    float luma = dot(b.rgb, vec3(0.3, 0.59, 0.11));
    float d = texelFetch(depthTex, ivec2(gl_FragCoord.xy), 0).r;
    if (luma <= 1e-5 || d >= 1.0) return b.rgb;
    float lightInvZ = b.a / luma;                       // how far the light is (1 / view depth, brightness-weighted)
    float z = P.proj.y / (min(d, 0.999999) - P.proj.x);
    float ratio = 1.0 / max(z * lightInvZ, 1e-4);      // light depth / this pixel's depth: > 1 = the pixel is in front
    return b.rgb * mix(1.0, P.params.w, smoothstep(1.15, 1.6, ratio));
}
