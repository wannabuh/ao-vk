#version 450
// Direct3D 7 fixed-function pixel processing: two texture stages, specular add, fog, alpha test.
#include "constants.glsl"
#include "lighting.glsl"

layout(set = 0, binding = 1) uniform sampler2D tex0;
layout(set = 0, binding = 2) uniform sampler2D tex1;
layout(set = 0, binding = 5) uniform sampler2DShadow shadowMap;

layout(location = 0) in vec4 vDiffuse;
layout(location = 1) in vec4 vSpecular;
layout(location = 2) in vec4 vTex0;
layout(location = 3) in vec4 vTex1;
layout(location = 4) in float vFogDist;
layout(location = 5) in float vFogFactor;
layout(location = 6) in vec3 vMatAmbient;
layout(location = 7) in vec3 vMatEmissive;
layout(location = 8) in vec3 vPosW;
layout(location = 9) in vec4 vNormalW;

// Lit vertex colours: the interpolated ones, or computed here for per-pixel lighting.
vec4 gDiffuse, gSpecular;

layout(location = 0) out vec4 outColor;

vec4 Arg(uint a, vec4 current, vec4 tex)
{
    uint sel = a & 0xFu;
    vec4 v = sel == 0u ? gDiffuse : sel == 1u ? current : sel == 2u ? tex : sel == 3u ? C.tfactor : gSpecular;
    if ((a & 0x10u) != 0u) v = 1.0 - v;
    if ((a & 0x20u) != 0u) v = v.aaaa;
    return v;
}

vec4 Op(uint op, vec4 a1, vec4 a2, vec4 current, vec4 tex)
{
    switch (op) {
    case 2u: return a1;                                   // SELECTARG1
    case 3u: return a2;                                   // SELECTARG2
    case 4u: return a1 * a2;                              // MODULATE
    case 5u: return a1 * a2 * 2.0;                        // MODULATE2X
    case 6u: return a1 * a2 * 4.0;                        // MODULATE4X
    case 7u: return a1 + a2;                              // ADD
    case 8u: return a1 + a2 - 0.5;                        // ADDSIGNED
    case 9u: return (a1 + a2 - 0.5) * 2.0;                // ADDSIGNED2X
    case 10u: return a1 - a2;                             // SUBTRACT
    case 11u: return a1 + a2 - a1 * a2;                   // ADDSMOOTH
    case 12u: return mix(a2, a1, gDiffuse.a);             // BLENDDIFFUSEALPHA
    case 13u: return mix(a2, a1, tex.a);                  // BLENDTEXTUREALPHA
    case 14u: return mix(a2, a1, C.tfactor.a);            // BLENDFACTORALPHA
    case 16u: return mix(a2, a1, current.a);              // BLENDCURRENTALPHA
    default: return a1;
    }
}

vec4 Sample(uint stage)
{
    vec4 tc = stage == 0u ? vTex0 : vTex1;
    uint ttf = C.stageB[stage].w;
    if ((ttf & 256u) != 0u) {                             // D3DTTFF_PROJECTED: divide by the last component
        uint count = ttf & 0xFFu;
        float q = count == 2u ? tc.y : count == 3u ? tc.z : tc.w;
        tc.xy /= q;
    }
    bool bound = (C.flags.x & (stage == 0u ? F_TEX0 : F_TEX1)) != 0u;
    if (!bound) return vec4(0.0, 0.0, 0.0, 1.0);
    return stage == 0u ? texture(tex0, tc.xy) : texture(tex1, tc.xy);
}

// Sun visibility at a surface point: 1 = lit, 0 = in shadow. 3x3 taps of 2x2 comparison filtering.
float SunVisibility(vec3 posW, vec3 n)
{
    vec3 L = -FL.sunDir.xyz;
    float texel = FL.shadowParams.z;
    if (dot(n, n) > 0.0) {
        n = normalize(n);
        float nl = dot(n, L);
        if (nl <= 0.0) return 1.0;                        // faces away: the light equation / lightmap handles it
        posW += n * texel * (1.5 + 2.0 * (1.0 - nl));     // normal offset against self-shadowing
    }
    vec4 sc = FL.shadowViewProj * vec4(posW, 1.0);
    vec3 ndc = sc.xyz / sc.w;
    float edge = max(abs(ndc.x), abs(ndc.y));
    if (edge >= 1.0 || ndc.z <= 0.0 || ndc.z >= 1.0) return 1.0;
    vec2 uv = ndc.xy * 0.5 + 0.5;
    vec2 ts = 1.0 / vec2(textureSize(shadowMap, 0));
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            sum += texture(shadowMap, vec3(uv + vec2(x, y) * ts, ndc.z));
    return mix(sum / 9.0, 1.0, smoothstep(0.8, 1.0, edge));   // fade out towards the map's edge
}

bool AlphaPass(float a)
{
    float a8 = floor(a * 255.0 + 0.5), ref = C.misc.y;
    switch (C.flags.w) {
    case 1u: return false;
    case 2u: return a8 < ref;
    case 3u: return a8 == ref;
    case 4u: return a8 <= ref;
    case 5u: return a8 > ref;
    case 6u: return a8 != ref;
    case 7u: return a8 >= ref;
    default: return true;
    }
}

void main()
{
    gDiffuse = vDiffuse;
    gSpecular = vSpecular;
    // Shadow: lit draws scale the sunlight (per-pixel lighting), others darken their final colour.
    float shade = 1.0, localScale = 1.0;
    if ((C.flags.x & (F_SHADOW | F_SHADOWCOMP)) != 0u)
        shade = 1.0 - (1.0 - SunVisibility(vPosW, vNormalW.xyz)) * FL.shadowParams.y;
    if ((C.flags.x & F_SHADOWCOMP) != 0u) {
        localScale = 1.0 / max(shade, 0.05);
        shade = 1.0;
    }
    bool shadeSun = (C.flags.x & (F_PERPIXEL | F_LIGHTING)) == (F_PERPIXEL | F_LIGHTING);
    if ((C.flags.x & F_PERPIXEL) != 0u) {
        // Interpolated normals shrink between vertices; restore the length the vertex path lights with
        // (1 with NORMALIZENORMALS, otherwise whatever the world matrix made of the vertex normal).
        vec3 n = vNormalW.xyz;
        float len2 = dot(n, n);
        n = len2 > 0.0 ? n * (vNormalW.w * inversesqrt(len2)) : vec3(0.0);
        vec3 ambient = C.ambient.rgb, diff = vec3(0.0), spec = vec3(0.0);
        AccumulateLights(vPosW, n, shadeSun ? shade : 1.0, localScale, ambient, diff, spec);
        gDiffuse = clamp(vec4(vMatEmissive + vMatAmbient * ambient + vDiffuse.rgb * diff, vDiffuse.a), 0.0, 1.0);
        gSpecular = clamp(vec4(vSpecular.rgb * spec, vSpecular.a), 0.0, 1.0);
    }
    vec4 current = gDiffuse;
    for (uint s = 0u; s < 2u; ++s) {
        uint colorOp = C.stageA[s].x;
        if (colorOp == 1u) break;                         // DISABLE ends the cascade
        vec4 tex = Sample(s);
        vec3 rgb = Op(colorOp, Arg(C.stageA[s].y, current, tex), Arg(C.stageA[s].z, current, tex), current, tex).rgb;
        uint alphaOp = C.stageA[s].w;
        float a = alphaOp == 1u ? current.a
                : Op(alphaOp, Arg(C.stageB[s].x, current, tex), Arg(C.stageB[s].y, current, tex), current, tex).a;
        current = clamp(vec4(rgb, a), 0.0, 1.0);
    }
    if ((C.flags.x & F_SPECULAR) != 0u)
        current.rgb = min(current.rgb + gSpecular.rgb, 1.0);
    if (!shadeSun)
        current.rgb *= shade;
    if ((C.flags.x & F_FOG) != 0u) {
        float f = vFogFactor;
        uint table = C.flags.z;
        if (table != 0u) {
            float d = vFogDist;
            if (table == 3u) f = clamp((C.fogParams.y - d) / max(C.fogParams.y - C.fogParams.x, 1e-6), 0.0, 1.0);
            else if (table == 1u) f = clamp(exp(-C.fogParams.z * d), 0.0, 1.0);
            else { float x = C.fogParams.z * d; f = clamp(exp(-x * x), 0.0, 1.0); }
        }
        current.rgb = mix(C.fogColor.rgb, current.rgb, f);
    }
    if ((C.flags.x & F_DEBUGLIGHT) != 0u && (C.vtx.x & 0xEu) != 4u) {
        // Blue: lighting off. Red: lit, but no point / spot light reaches the draw. Green: lit by a point light.
        vec3 tint = (C.flags.x & F_LIGHTING) == 0u ? vec3(0.1, 0.3, 1.0)
                  : C.lightInfo.y == 0u && ((C.flags.x & F_LIGHTOVERRIDE) == 0u || FL.info.x == 0u) ? vec3(1.0, 0.15, 0.1)
                  : vec3(0.1, 1.0, 0.2);
        current.rgb = mix(current.rgb, tint, 0.45);
    }
    if ((C.flags.x & F_ALPHATEST) != 0u && !AlphaPass(current.a))
        discard;
    outColor = current;
}
