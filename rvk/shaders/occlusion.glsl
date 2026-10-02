// The tone mapping's inputs, ambient occlusion and indirect light (hdr.cpp), shared with the depth of field composite.
layout(set = 0, binding = 0) uniform sampler2D scene;
layout(set = 0, binding = 1) uniform sampler2D bloom;   // half resolution, light above the bloom threshold, blurred
layout(set = 0, binding = 2) uniform sampler2D ao;      // half resolution: ambient occlusion (1 = open), view depth
layout(set = 0, binding = 3) uniform sampler2D depthTex; // full resolution scene depth (for the AO upsampling)
layout(set = 0, binding = 4) uniform sampler2D localFraction;   // how much of each pixel local lights lit
layout(set = 0, binding = 5) uniform sampler2D gi;      // half resolution: indirect light, view depth
layout(set = 0, binding = 6) uniform sampler2D albedo;  // the surfaces' own colour (fogged)
layout(set = 0, binding = 7) uniform sampler2D volume;  // half resolution: light scattered by the air, view depth
layout(push_constant) uniform Push {
    vec4 params;        // knee, exposure, bloom strength, ambient occlusion on (1)
    vec4 proj;          // D3D projection m[2][2], m[3][2] (view depth from the depth buffer), indirect light strength,
                        // volumetric light on (1)
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
    if (P.params.w < 0.5) return 1.0;
    float local = texelFetch(localFraction, ivec2(gl_FragCoord.xy), 0).r;
    return mix(Occlusion(), 1.0, local);
}

// Light scattered towards the camera by the air in front of this pixel (volumetric light); 0 when off.
vec3 Volumetric()
{
    if (P.proj.w <= 0.0) return vec3(0.0);
    return Upsample(volume, 3).rgb;
}
