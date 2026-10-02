// Direct3D 7 fixed-function pixel processing: two texture stages, specular add, fog, alpha test.
#include "constants.glsl"

layout(set = 0, binding = 1) uniform sampler2D tex0;
layout(set = 0, binding = 2) uniform sampler2D tex1;
layout(set = 0, binding = 5) uniform sampler2DArrayShadow shadowMap;   // sun shadow cascades
layout(set = 0, binding = 6) uniform samplerCubeArrayShadow pointShadowMaps;
layout(set = 0, binding = 7) uniform sampler2D bumpBase;   // F_BUMPBASE: the ground's base texture, for its relief
layout(set = 0, binding = 9) uniform sampler2DArray shadowDepths;   // the sun cascades' depths (soft shadows' blockers)

// Visibility of a frame light with a cube shadow map (l.spot.z = cube + 1): 1 = lit. Must match
// Device::RenderPointShadowMaps (pointshadow.cpp): depth along the face's axis, near kPointShadowNear, far = range.
const float kPointShadowNear = 0.25;
float PointShadow(Light l, vec3 posW, vec3 n, float nl)
{
    vec3 d = posW - l.position.xyz;
    float dist = length(d);
    if (dist <= kPointShadowNear) return 1.0;
    // World size of a cube texel at this distance (90 degree faces): offset along the normal against acne, more at
    // grazing angles. The angle from the unit normal: game normals aren't always unit length (no NORMALIZENORMALS;
    // some meshes have length-2 normals), and nl from the lighting would turn the offset into the surface.
    float texel = 2.0 * dist / float(textureSize(pointShadowMaps, 0).x);
    vec3 nu = normalize(n);
    float cosAngle = clamp(dot(nu, -d / dist), 0.0, 1.0);
    d += nu * texel * (1.0 + 2.0 * (1.0 - cosAngle));
    vec3 a = abs(d);
    float w = max(a.x, max(a.y, a.z));
    float f = l.direction.w, nr = kPointShadowNear;
    float ref = f / (f - nr) - f * nr / ((f - nr) * w);
    // 4 taps around the direction, each 2x2 comparison-filtered.
    vec3 dirN = d / length(d);
    vec3 t1 = normalize(cross(dirN, abs(dirN.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
    vec3 t2 = cross(dirN, t1);
    float r = 0.75 * texel;
    float layer = l.spot.z - 1.0, s = 0.0;
    s += texture(pointShadowMaps, vec4(d + (t1 + t2) * r, layer), ref);
    s += texture(pointShadowMaps, vec4(d + (t1 - t2) * r, layer), ref);
    s += texture(pointShadowMaps, vec4(d - (t1 + t2) * r, layer), ref);
    s += texture(pointShadowMaps, vec4(d - (t1 - t2) * r, layer), ref);
    return 1.0 - (1.0 - 0.25 * s) * FL.shadowParams.w * l.spot.w;
}

#include "lighting.glsl"

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
layout(location = 10) in vec2 vSet0;
layout(location = 11) in vec4 vClip;
layout(location = 12) in vec4 vPrevClip;

// Lit vertex colours: the interpolated ones, or computed here for per-pixel lighting.
vec4 gDiffuse, gSpecular;
// F_OVERBRIGHT: the frame lights' part, kept out of gDiffuse / gSpecular (and the game's clamp).
vec3 gLocalDiffuse = vec3(0.0), gLocalSpecular = vec3(0.0);
float gLocalFraction = 0.0;                     // how much of the final colour the local lights gave (outLocal)
float gLightmapRelief = 1.0;                    // F_BUMPBASE: the ground's relief in its baked sunlight (stage 0)
float gSunShare = 0.0;                          // how much of the colour is direct sunlight (contact shadows)

layout(location = 0) out vec4 outColor;
#ifdef RVK_GLOW
layout(location = 1) out vec4 outGlow;     // HDR scene only: the glow attachment (F_GLOW)
layout(location = 2) out vec4 outLocal;    // HDR scene only: local lights' fraction of the colour, reflectivity, sun's share
layout(location = 3) out vec4 outMotion;   // HDR scene only: screen motion since last frame, pixels (solid geometry)
layout(location = 4) out vec4 outAlbedo;   // HDR scene only: the surface's colour without lighting (indirect light)
#endif

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

// A point of a Vogel (golden angle) disk of n points, radius 1, rotated.
vec2 Vogel(int i, int n, float rotation)
{
    float r = sqrt((float(i) + 0.5) / float(n)), a = float(i) * 2.39996323 + rotation;
    return r * vec2(cos(a), sin(a));
}

// Sun visibility at a surface point in one shadow cascade: 1 = lit, 0 = in shadow; edge: how near the point is to
// the cascade's border (0 = centre, >= 1 = outside). Soft (FL.effects.z > 0): the penumbra grows with the distance
// between the blocker and the receiver, as under a sun of some size - sharp where a trunk meets the ground, soft at
// the far edge of a canopy's shadow (blocker search, then a filter that wide). Hard: 3x3 taps of 2x2 filtering.
float CascadeVisibility(int c, vec3 posW, vec3 n, float nl, out float edge)
{
    if (nl >= 0.0)
        posW += n * FL.cascadeTexel[c] * (1.5 + 2.0 * (1.0 - nl));   // normal offset against self-shadowing
    vec4 sc = FL.shadowViewProj[c] * vec4(posW, 1.0);
    vec3 ndc = sc.xyz / sc.w;
    edge = max(abs(ndc.x), abs(ndc.y));
    if (edge >= 1.0 || ndc.z <= 0.0 || ndc.z >= 1.0) { edge = 2.0; return 1.0; }
    vec2 uv = ndc.xy * 0.5 + 0.5;
    vec2 ts = 1.0 / vec2(textureSize(shadowMap, 0).xy);
    float soft = FL.effects.z;
    if (soft <= 0.0) {
        float sum = 0.0;
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x)
                sum += texture(shadowMap, vec4(uv + vec2(x, y) * ts, float(c), ndc.z));
        return sum / 9.0;
    }
    float texel = FL.cascadeTexel[c];
    float rotation = 6.2831853 * fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))) + FL.taa.z);
    float k = 0.02 * soft;                       // penumbra width per world unit from blocker to receiver
    // Blockers: depths in front of the receiver within the widest penumbra (blockers up to ~30 units away). None:
    // lit; all: deep in the shadow (the filter wouldn't reach out of it) - both without the filter.
    // The centre too: a thin shadow (a pole) must not fall between the taps.
    float searchTexels = clamp(k * 30.0 / texel, 1.5, 24.0), blockerSum = 0.0, blockers = 0.0;
    for (int i = 0; i < 9; ++i) {
        vec2 o = i == 8 ? vec2(0.0) : Vogel(i, 8, rotation) * searchTexels * ts;
        float d = texture(shadowDepths, vec3(uv + o, float(c))).r;
        if (d < ndc.z) { blockerSum += d; blockers += 1.0; }
    }
    if (blockers == 0.0) return 1.0;
    if (blockers == 9.0) return 0.0;
    float distance = (ndc.z - blockerSum / blockers) * FL.cascadeDepth[c];
    float radius = clamp(distance * k / texel, 1.0, 24.0);
    float sum = 0.0;
    for (int i = 0; i < 12; ++i)
        sum += texture(shadowMap, vec4(uv + Vogel(i, 12, rotation) * radius * ts, float(c), ndc.z));
    return sum / 12.0;
}

// The texture's mean alpha around a point (~16 texels across): a coarse mip, or without mips a ring of taps.
float NeighbourhoodAlpha(vec2 uv)
{
    if (textureQueryLevels(tex0) > 4) return textureLod(tex0, uv, 4.0).a;
    vec2 step = 8.0 / vec2(textureSize(tex0, 0));
    float sum = textureLod(tex0, uv, 0.0).a;
    for (int i = 0; i < 8; ++i) {
        float a = float(i) * 0.7853982;
        sum += textureLod(tex0, uv + vec2(cos(a), sin(a)) * step, 0.0).a;
    }
    return sum / 9.0;
}

// Sun visibility at a surface point: 1 = lit, 0 = in shadow. The sharpest cascade covering the point, blended into
// the next one towards its border; the last fades out to lit.
float SunVisibility(vec3 posW, vec3 n)
{
    vec3 L = -FL.sunDir.xyz;
    float nl = -1.0;
    if (dot(n, n) > 0.0) {
        n = normalize(n);
        nl = dot(n, L);
        if (nl <= 0.0) return 1.0;                        // faces away: the light equation / lightmap handles it
    }
    int count = int(FL.shadowParams.z);
    for (int c = 0; c < count; ++c) {
        float edge;
        float v = CascadeVisibility(c, posW, n, nl, edge);
        if (edge >= 1.0) continue;
        float blend = smoothstep(0.8, 1.0, edge);
        if (blend <= 0.0) return v;
        float next = 1.0;
        if (c + 1 < count) {
            float e2;
            next = CascadeVisibility(c + 1, posW, n, nl, e2);
        }
        return mix(v, next, blend);
    }
    return 1.0;
}

// What local light adds to a clamped lit colour: as is up to 1, then rolling off smoothly towards the headroom.
vec3 Headroom(vec3 lit, vec3 local)
{
    float room = FL.sunDir.w - 1.0;
    vec3 total = lit + max(local, vec3(0.0));
    vec3 over = max(total - 1.0, vec3(0.0));
    vec3 soft = room > 0.0 ? room * (1.0 - exp(-over / room)) : vec3(0.0);
    return min(total, vec3(1.0)) + soft - lit;
}

// D3D's texture stages (and specular add) on gDiffuse / gSpecular. maxColor: the stages' clamp (1 = D3D).
vec4 Cascade(vec4 t0, vec4 t1, float maxColor)
{
    vec4 current = gDiffuse;
    for (uint s = 0u; s < 2u; ++s) {
        uint colorOp = C.stageA[s].x;
        if (colorOp == 1u) break;                         // DISABLE ends the cascade
        vec4 tex = s == 0u ? t0 : t1;
        vec3 rgb = Op(colorOp, Arg(C.stageA[s].y, current, tex), Arg(C.stageA[s].z, current, tex), current, tex).rgb;
        uint alphaOp = C.stageA[s].w;
        float a = alphaOp == 1u ? current.a
                : Op(alphaOp, Arg(C.stageB[s].x, current, tex), Arg(C.stageB[s].y, current, tex), current, tex).a;
        current = vec4(clamp(rgb, 0.0, maxColor), clamp(a, 0.0, 1.0));
    }
    if ((C.flags.x & F_SPECULAR) != 0u)
        current.rgb = min(current.rgb + gSpecular.rgb, maxColor);
    return current;
}

// Generated normal mapping (F_BUMP): the surface's texture (stage 0) read as a height field - brightness = height -
// tilts the shading normal. The slope comes from neighbouring texels at the mip level being sampled, so it doesn't
// alias in the distance, and is scaled per texel (C.misc.w: height change per texel for a full brightness step), so
// it looks the same on big and small surfaces. The normal is bent with the screen-space derivatives of position and
// height (Mikkelsen, "Bump Mapping Unparametrized Surfaces on the GPU"): no tangents needed.
float Luma(vec3 c) { return dot(c, vec3(0.3, 0.59, 0.11)); }

vec3 BumpNormal(sampler2D tex, vec3 n, vec3 posW, vec2 uv)
{
    vec2 size = vec2(textureSize(tex, 0));
    float lod = max(textureQueryLod(tex, uv).y, 0.0);
    vec2 step = exp2(lod) / size;                          // one texel at the sampled mip level, in uv
    float hl = Luma(textureLod(tex, uv - vec2(step.x, 0.0), lod).rgb), hr = Luma(textureLod(tex, uv + vec2(step.x, 0.0), lod).rgb);
    float hd = Luma(textureLod(tex, uv - vec2(0.0, step.y), lod).rgb), hu = Luma(textureLod(tex, uv + vec2(0.0, step.y), lod).rgb);
    vec2 slope = vec2(hr - hl, hu - hd) * 0.5;             // height per texel of this level, along u and v
    vec2 uvx = dFdx(uv) / step, uvy = dFdy(uv) / step;     // screen pixel -> texels of this level
    vec3 px = dFdx(posW), py = dFdy(posW);
    float texelWorld = sqrt(max(dot(px, px), dot(py, py)) / max(max(dot(uvx, uvx), dot(uvy, uvy)), 1e-8));
    float k = C.misc.w * texelWorld;                       // world height of a full brightness step per texel
    float dBs = dot(slope, uvx) * k, dBt = dot(slope, uvy) * k;
    vec3 nu = normalize(n);
    vec3 r1 = cross(py, nu), r2 = cross(nu, px);
    float det = dot(px, r1);
    if (abs(det) < 1e-12) return n;
    vec3 grad = sign(det) * (dBs * r1 + dBt * r2);
    return normalize(abs(det) * nu - grad) * length(n);
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
    float shade = 1.0, localScale = 1.0, texShade = 1.0;
    if ((C.flags.x & (F_SHADOW | F_SHADOWCOMP | F_SHADOWTEX)) != 0u)
        shade = 1.0 - (1.0 - SunVisibility(vPosW, vNormalW.xyz)) * FL.shadowParams.y;
    if ((C.flags.x & F_SHADOWCOMP) != 0u) {
        localScale = 1.0 / max(shade, 0.05);
        shade = 1.0;
    }
    if ((C.flags.x & F_SHADOWTEX) != 0u) {
        texShade = shade;
        shade = 1.0;
    }
    bool shadeSun = (C.flags.x & (F_PERPIXEL | F_LIGHTING)) == (F_PERPIXEL | F_LIGHTING);
    if ((C.flags.x & F_PERPIXEL) != 0u) {
        // Interpolated normals shrink between vertices; restore the length the vertex path lights with
        // (1 with NORMALIZENORMALS, otherwise whatever the world matrix made of the vertex normal).
        vec3 n = vNormalW.xyz;
        float len2 = dot(n, n);
        n = len2 > 0.0 ? n * (vNormalW.w * inversesqrt(len2)) : vec3(0.0);
        // The ground's lighting pass draws the lightmap; its relief comes from the base pass's texture (F_BUMPBASE).
        if ((C.flags.x & F_BUMP) != 0u && len2 > 0.0) {
            vec3 unbumped = n;
            n = (C.flags.x & F_BUMPBASE) != 0u ? BumpNormal(bumpBase, n, vPosW, vSet0) : BumpNormal(tex0, n, vPosW, vTex0.xy);
            // The ground's sunlight is baked into its lightmap (the live sun is kept off it): the relief scales the
            // lightmap by how much more or less the bumped surface faces the sun than the flat one.
            if ((C.flags.x & F_BUMPBASE) != 0u && dot(FL.sunDir.xyz, FL.sunDir.xyz) > 0.0) {
                vec3 L = -normalize(FL.sunDir.xyz);
                float before = max(dot(normalize(unbumped), L), 0.0), after = max(dot(normalize(n), L), 0.0);
                gLightmapRelief = clamp((after + 0.25) / (before + 0.25), 0.6, 1.4);
            }
        }
        vec3 ambient = vec3(0.0), diff = vec3(0.0), spec = vec3(0.0), diffL = vec3(0.0), specL = vec3(0.0);
        // Sunlight is shadowed: by the receiver's shade, or in the ground's lighting pass by the lightmap's.
        float sunScale = (C.flags.x & F_SHADOWTEX) != 0u ? texShade : shadeSun ? shade : 1.0;
        AccumulateLights(vPosW, n, sunScale, localScale, ambient, diff, spec, diffL, specL);
        // The ground's lighting pass (F_SHADOWTEX): the shadow takes the global ambient and emissive part along
        // with the lightmap, leaving only the lights' own contribution: (lightmap + ambient) * shadow + lights.
        vec3 base = (vMatEmissive + vMatAmbient * C.ambient.rgb) * texShade;
        vec3 lit = base + vMatAmbient * ambient + vDiffuse.rgb * diff, litSpec = vSpecular.rgb * spec;
        vec3 local = vDiffuse.rgb * diffL, localSpec = vSpecular.rgb * specL;
        // The direct sunlight's share of the colour: what the contact shadows may take away. The ground's sunlight
        // is baked into its lightmap: about the lit side's part of it.
        if (dot(FL.sunColor.rgb, FL.sunColor.rgb) > 0.0 && len2 > 0.0) {
            float ndl = max(dot(normalize(n), -normalize(FL.sunDir.xyz)), 0.0);
            if ((C.flags.x & F_SHADOWTEX) != 0u) {
                gSunShare = 0.6 * ndl * texShade;
            } else {
                const vec3 kLuma = vec3(0.3, 0.59, 0.11);
                float sunPart = dot(vDiffuse.rgb * FL.sunColor.rgb, kLuma) * ndl * sunScale;
                gSunShare = clamp(sunPart / max(dot(lit + local, kLuma), 1e-3), 0.0, 1.0);
            }
        }
        if ((C.flags.x & F_OVERBRIGHT) != 0u) {
            // The game's lighting is clamped as D3D does; the frame lights add on top, up to the headroom. Under a
            // bright sun a surface is already near 1, and a light (and its shadow) would otherwise barely show on
            // surfaces facing the sun while those facing away light up a lot.
            gDiffuse = clamp(vec4(lit, vDiffuse.a), 0.0, 1.0);
            gSpecular = clamp(vec4(litSpec, vSpecular.a), 0.0, 1.0);
            gLocalDiffuse = Headroom(gDiffuse.rgb, local);
            gLocalSpecular = Headroom(gSpecular.rgb, localSpec);
        } else {
            gDiffuse = clamp(vec4(lit + local, vDiffuse.a), 0.0, 1.0);
            gSpecular = clamp(vec4(litSpec + localSpec, vSpecular.a), 0.0, 1.0);
        }
    }
    vec4 t0 = Sample(0u), t1 = C.stageA[0].x != 1u ? Sample(1u) : vec4(0.0);
#ifdef RVK_GLOW
    // The surface's own colour, for the indirect light it reflects: lit draws through the stages under white light,
    // unlit ones as they are (the ground's base pass is its bare texture; its lightmap pass keeps this).
    vec3 albedo;
    if ((C.flags.x & F_LIGHTING) != 0u) {
        vec4 d = gDiffuse, sp = gSpecular;
        gDiffuse = vec4(1.0, 1.0, 1.0, d.a);
        gSpecular = vec4(0.0);
        albedo = Cascade(t0, t1, 1.0).rgb;
        gDiffuse = d;
        gSpecular = sp;
    }
#endif
    t0.rgb *= texShade * gLightmapRelief;
    vec4 current = Cascade(t0, t1, 1.0);
#ifdef RVK_GLOW
    if ((C.flags.x & F_LIGHTING) == 0u)
        albedo = current.rgb;
#endif
    if (any(greaterThan(gLocalDiffuse + gLocalSpecular, vec3(0.0)))) {
        // What the frame lights add through the stages (with and without them, unclamped), on top of the
        // D3D result: identical to it wherever that didn't clip.
        vec4 d = gDiffuse, sp = gSpecular;
        vec4 without = Cascade(t0, t1, 1e4);
        gDiffuse.rgb += gLocalDiffuse;
        gSpecular.rgb += gLocalSpecular;
        vec4 with = Cascade(t0, t1, 1e4);
        gDiffuse = d;
        gSpecular = sp;
        float maxColor = (C.flags.x & (F_OVERBRIGHT2X | F_HDR)) != 0u ? FL.sunDir.w : 1.0;
        vec3 added = max(with.rgb - without.rgb, 0.0);
        current.rgb = min(current.rgb + added, vec3(maxColor));
        const vec3 kLuma = vec3(0.3, 0.59, 0.11);
        gLocalFraction = clamp(dot(added, kLuma) / max(dot(current.rgb, kLuma), 1e-4), 0.0, 1.0);
    }
    if (!shadeSun)
        current.rgb *= shade;
    // Sunlight through leaves: where the sun is behind a leaf (seen from the camera), its light comes through tinted
    // by the leaf, most looking into the sun; other leaves' shadows still block it. Only textures with holes around
    // this point (their coarse mip's alpha) are leaves - not walls drawn the same way.
    bool sun = dot(FL.sunColor.rgb, FL.sunColor.rgb) > 0.0 && dot(FL.sunDir.xyz, FL.sunDir.xyz) > 0.0;
    if ((C.flags.x & F_FOLIAGE) != 0u && sun && FL.effects.x > 0.0 && dot(vNormalW.xyz, vNormalW.xyz) > 0.0) {
        float holes = smoothstep(0.97, 0.75, NeighbourhoodAlpha(vTex0.xy));
        if (holes > 0.0) {
            vec3 L = -normalize(FL.sunDir.xyz), toEye = normalize(C.eyePos.xyz - vPosW);
            vec3 nf = normalize(vNormalW.xyz);
            if (dot(nf, toEye) < 0.0) nf = -nf;                  // the side the camera sees
            float through = max(-dot(nf, L), 0.0), glare = pow(max(dot(-toEye, L), 0.0), 4.0);
            if (through + glare > 0.0) {
                float visible = FL.shadowParams.x > 0.5 ? SunVisibility(vPosW, -nf) : 1.0;
                current.rgb += t0.rgb * FL.sunColor.rgb * ((0.5 * through + 0.7 * glare) * visible * holes * FL.effects.x);
            }
        }
    }
    // Night glow: on self-lit surfaces (windows, signs, screens), bright texels shine beyond white, for the bloom.
    if ((C.flags.x & F_EMISSIVE) != 0u && FL.effects.y > 0.0) {
        float glow = smoothstep(0.55, 0.9, max(t0.r, max(t0.g, t0.b))) * FL.effects.y;
        if ((C.flags.x & F_LIGHTING) != 0u) current.rgb += t0.rgb * vMatEmissive * glow;
        else current.rgb *= 1.0 + glow;
    }
    if (C.vtx.y != 0u) {
        // GPU particles' brightness: a hot core. Where in the particle a pixel is comes from its texture (a sparkle is
        // bright and opaque in the middle, fading out to the edge; shape in alpha or in intensity). Brighter settings
        // raise the centre most and bleach it towards white - a red or orange particle stays red at its edge but gets
        // a white-hot middle, which reads as bright where a brighter red wouldn't. Below 1: dimmer all over.
        float bright = uintBitsToFloat(C.vtx.y);
        if (bright < 1.0) {
            current.rgb *= bright;
        } else {
            float shape = clamp(min(t0.a, max(t0.r, max(t0.g, t0.b))), 0.0, 1.0);
            float core = smoothstep(0.25, 0.9, shape);
            current.rgb *= mix(1.0, bright, core);
            float peak = max(current.r, max(current.g, current.b));
            current.rgb = mix(current.rgb, vec3(peak), core * clamp((bright - 1.0) / 3.0, 0.0, 1.0));
        }
    }
#ifdef RVK_GLOW
    // How much the surface reflects (screen-space reflections): the draw's own, or its wet look where it faces up.
    float reflectivity = uintBitsToFloat(C.vtx.z), wet = uintBitsToFloat(C.vtx.w);
    if (wet > 0.0 && dot(vNormalW.xyz, vNormalW.xyz) > 0.0)
        reflectivity = max(reflectivity, wet * smoothstep(0.7, 0.95, normalize(vNormalW.xyz).y));
#endif
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
#ifdef RVK_GLOW
        albedo *= f;                                      // indirect light fades into the fog with the surface
        reflectivity *= f;                                // ... and so do reflections
#endif
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
    // An additive effect adds what it adds to the scene to the glow too, for the bloom.
#ifdef RVK_GLOW
    vec3 glow = vec3(0.0);
    if ((C.flags.x & F_GLOW) != 0u)
        glow = current.rgb * ((C.flags.x & F_GLOWALPHA) != 0u ? clamp(current.a, 0.0, 1.0) : 1.0) * C.misc.z;
    // Alpha: its brightness over its view depth (1 / w), summed like the colour - the bloom's light's distance.
    outGlow = vec4(glow, dot(glow, vec3(0.3, 0.59, 0.11)) * gl_FragCoord.w);
    // Blended with this fragment's alpha like the colour (attachment 2's blend state follows the colour's).
    outLocal = vec4(gLocalFraction, clamp(reflectivity, 0.0, 1.0), gSunShare, current.a);
    // Motion vectors (written by depth-writing draws only, see the blend state): where this point was last frame.
    vec2 now = vClip.xy / vClip.w, before = vPrevClip.xy / max(vPrevClip.w, 1e-6);
    outMotion = vec4(vPrevClip.w > 1e-6 ? (now - before) * 0.5 * C.viewport.zw * vec2(1.0, -1.0) : vec2(0.0), 0.0, 1.0);
    // Blended with this fragment's alpha like the colour (kept by multiplying and additive passes, see the blend state).
    outAlbedo = vec4(clamp(albedo, 0.0, 1.0), current.a);
#endif
    if ((C.flags.x & F_OVERBRIGHT2X) != 0u)
        current.rgb *= 0.5;                               // blended as dst * src * 2
    outColor = current;
}
