// Direct3D 7 fixed-function pixel processing: two texture stages, specular add, fog, alpha test.
#extension GL_KHR_shader_subgroup_quad : require
#include "constants.glsl"

// tex0 / tex1 / bumpBase / normalMap are bindless now (TEX0 / TEX1 / BUMPTEX / NORMALTEX in constants.glsl).
layout(set = 0, binding = 5) uniform sampler2DArrayShadow shadowMap;   // sun shadow cascades
layout(set = 0, binding = 6) uniform samplerCubeArrayShadow pointShadowMaps;
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
layout(location = 13) in float vCutout;
layout(location = 15) flat in uint vRecord;    // the draw's record index (from the vertex shader)

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
    return stage == 0u ? texture(TEX0, tc.xy) : texture(TEX1, tc.xy);
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
    if ((C.flags.x & F_SHADOWCHEAP) != 0u)              // far foliage: one filtered tap
        return texture(shadowMap, vec4(uv, float(c), ndc.z));
    float rotation = 6.2831853 * fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))) + FL.taa.z);
    // Far cascades (distance level of detail): a texel there is wider than most penumbrae, so a small fixed filter
    // instead of the blocker search and the wide one - a quarter of the taps.
    if (c >= 2) {
        float sum = 0.0;
        for (int i = 0; i < 4; ++i)
            sum += texture(shadowMap, vec4(uv + Vogel(i, 4, rotation) * 1.5 * ts, float(c), ndc.z));
        return sum / 4.0;
    }
    float k = 0.02 * soft;                       // penumbra width per world unit from blocker to receiver
    // Blockers: depths in front of the receiver within the widest penumbra (blockers up to ~30 units away). None:
    // lit; all: deep in the shadow (the filter wouldn't reach out of it) - both without the filter.
    // The centre too: a thin shadow (a pole) must not fall between the taps.
    float searchTexels = clamp(k * 30.0 / texel, 1.5, 24.0), blockerSum = 0.0, blockers = 0.0;
    const int kSearch = 5;                       // the centre plus four blockers (a thin shadow still hits the centre)
    for (int i = 0; i < kSearch; ++i) {
        vec2 o = i == kSearch - 1 ? vec2(0.0) : Vogel(i, kSearch - 1, rotation) * searchTexels * ts;
        float d = texture(shadowDepths, vec3(uv + o, float(c))).r;
        if (d < ndc.z) { blockerSum += d; blockers += 1.0; }
    }
    if (blockers == 0.0) return 1.0;
    if (blockers == float(kSearch)) return 0.0;
    float distance = (ndc.z - blockerSum / blockers) * FL.cascadeDepth[c];
    float radius = clamp(distance * k / texel, 1.0, 24.0);
    if (radius < 2.0)                            // a sharp penumbra: the wide filter would only re-read the same texels
        return texture(shadowMap, vec4(uv, float(c), ndc.z));
    float sum = 0.0;
    for (int i = 0; i < 12; ++i)
        sum += texture(shadowMap, vec4(uv + Vogel(i, 12, rotation) * radius * ts, float(c), ndc.z));
    return sum / 12.0;
}

// The texture's mean alpha around a point (~16 texels across): a coarse mip, or without mips a ring of taps.
float NeighbourhoodAlpha(vec2 uv)
{
    if (textureQueryLevels(TEX0) > 4) return textureLod(TEX0, uv, 4.0).a;
    vec2 step = 8.0 / vec2(textureSize(TEX0, 0));
    float sum = textureLod(TEX0, uv, 0.0).a;
    for (int i = 0; i < 8; ++i) {
        float a = float(i) * 0.7853982;
        sum += textureLod(TEX0, uv + vec2(cos(a), sin(a)) * step, 0.0).a;
    }
    return sum / 9.0;
}

// The ground grass's shadow on a surface point (the map's layer after the cascades, over the nearest cascade's square;
// cleared to lit without grass shadows): 1 = lit. One filtered tap - blades are a texel or two wide.
const float kGrassShadowLayer = 4.0;            // rvk.h kGrassShadowLayer
float GrassShadowVisibility(vec3 posW, vec3 n, float nl)
{
    if (nl >= 0.0)
        posW += n * FL.cascadeTexel[0] * (1.5 + 2.0 * (1.0 - nl));   // as the nearest cascade
    vec4 sc = FL.shadowViewProj[0] * vec4(posW, 1.0);
    vec3 ndc = sc.xyz / sc.w;
    if (max(abs(ndc.x), abs(ndc.y)) >= 1.0 || ndc.z <= 0.0 || ndc.z >= 1.0)
        return 1.0;
    return texture(shadowMap, vec4(ndc.xy * 0.5 + 0.5, kGrassShadowLayer, ndc.z));
}

// Sun visibility at a surface point: 1 = lit, 0 = in shadow. The sharpest cascade covering the point, blended into
// the next one towards its border; the last fades out to lit. Times the ground grass's shadow, weakened (the ground
// under a field, a character's legs in it: in full, a sunlit field would be far darker than before it cast).
const float kGrassShadowStrength = 0.45;
float SunVisibilityCasters(vec3 posW, vec3 n);
float SunVisibility(vec3 posW, vec3 n)
{
    float v = SunVisibilityCasters(posW, n);
    if (v > 0.0 && (C.flags.x & F_SHADOWCHEAP) == 0u) {
        bool hasN = dot(n, n) > 0.0;
        vec3 nn = hasN ? normalize(n) : n;
        float nl = hasN ? dot(nn, -FL.sunDir.xyz) : -1.0;
        if (!hasN || nl > 0.0)                   // (facing away: the light equation handles it, as above)
            v *= mix(1.0, GrassShadowVisibility(posW, nn, nl), kGrassShadowStrength);
    }
    return v;
}

float SunVisibilityCasters(vec3 posW, vec3 n)
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
        float blend = (C.flags.x & F_SHADOWCHEAP) != 0u ? 0.0 : smoothstep(0.8, 1.0, edge);
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

// Bends n by a height field whose change across the pixel is dBs (to the right) and dBt (down the screen).
vec3 BendNormal(vec3 n, vec3 px, vec3 py, float dBs, float dBt)
{
    vec3 nu = normalize(n);
    vec3 r1 = cross(py, nu), r2 = cross(nu, px);
    float det = dot(px, r1);
    if (abs(det) < 1e-12) return n;
    vec3 grad = sign(det) * (dBs * r1 + dBt * r2);
    return normalize(abs(det) * nu - grad) * length(n);
}

// The bindless texture (image + sampler): a constructed sampler2D can only appear as a texture call's argument, so
// the two are passed separately and joined at each use.
vec3 BumpNormal(texture2D img, sampler smp, vec3 n, vec3 posW, vec2 uv)
{
    vec2 size = vec2(textureSize(sampler2D(img, smp), 0));
    float lod = max(textureQueryLod(sampler2D(img, smp), uv).y, 0.0);
    vec2 step = exp2(lod) / size;                          // one texel at the sampled mip level, in uv
    float hl = Luma(textureLod(sampler2D(img, smp), uv - vec2(step.x, 0.0), lod).rgb),
          hr = Luma(textureLod(sampler2D(img, smp), uv + vec2(step.x, 0.0), lod).rgb);
    float hd = Luma(textureLod(sampler2D(img, smp), uv - vec2(0.0, step.y), lod).rgb),
          hu = Luma(textureLod(sampler2D(img, smp), uv + vec2(0.0, step.y), lod).rgb);
    vec2 slope = vec2(hr - hl, hu - hd) * 0.5;             // height per texel of this level, along u and v
    vec2 uvx = dFdx(uv) / step, uvy = dFdy(uv) / step;     // screen pixel -> texels of this level
    vec3 px = dFdx(posW), py = dFdy(posW);
    float texelWorld = sqrt(max(dot(px, px), dot(py, py)) / max(max(dot(uvx, uvx), dot(uvy, uvy)), 1e-8));
    float k = C.misc.w * texelWorld;                       // world height of a full brightness step per texel
    return BendNormal(n, px, py, dot(slope, uvx) * k, dot(slope, uvy) * k);
}

// Normal mapping (F_NORMALMAP): the texture's own tangent-space normal map, OpenGL convention (what Blender bakes:
// +X along +u, +Y up in the image = against D3D's v). A normal (x, y, z) is the height slope (-x/z, y/z) per world
// unit along u and down-v; scaled by the world size of a uv unit it bends the normal exactly like the generated
// normals above, so it needs no tangents. C.misc.w = strength.
vec3 NormalMapNormal(vec3 n, vec3 posW, vec2 uv)
{
    vec3 t = texture(NORMALTEX, uv).xyz * 2.0 - 1.0;
    t.z = max(t.z, 0.05);
    vec2 slope = vec2(-t.x, t.y) / t.z * C.misc.w;     // height per world unit along u, v
    vec2 uvx = dFdx(uv), uvy = dFdy(uv);
    vec3 px = dFdx(posW), py = dFdy(posW);
    float worldPerUv = sqrt(max(dot(px, px), dot(py, py)) / max(max(dot(uvx, uvx), dot(uvy, uvy)), 1e-12));
    return BendNormal(n, px, py, dot(slope, uvx) * worldPerUv, dot(slope, uvy) * worldPerUv);
}

float gSunVisibility = 1.0;          // SunVisibility(vPosW, vNormalW) when computed (main)
bool gSunVisibilityValid = false;

bool AlphaPass(float a)
{
    float a8 = floor(a * 255.0 + 0.5), ref = C.misc.y;
    switch (C.flags.w & 0xFFu) {
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

// Two-pass foliage (Device::FlushEdges): a blended cut-out is drawn as its core - alpha from here up, with depth - and,
// later, over the finished scene, its soft edges - below it, without depth (D.motion.w: 1 core, 2 edges, 0 whole).
const float kFoliageCore = 0.95;

// Leaves (leaves.cpp; ffp.vert passes vSet0 for a canopy with leaves, D.leaf.w > 0): a leaf is a round sprig - its card
// fades out towards the rim (vSet0 = where on the card, -1 .. 1), not a square cut through the painted cluster; the
// canopy's own cards are thinned out in place (vSet0 = (how much, 2)): blocks of texels dropped by a stable hash, the
// leaves around them making up the crown.
float LeafMask()
{
    if (D.leaf.w <= 0.0) return 1.0;
    if (vSet0.y < 1.5) return smoothstep(1.0, 0.6, length(vSet0));
    if (vSet0.x <= 0.0) return 1.0;
    vec2 cell = floor(vTex0.xy * vec2(textureSize(TEX0, 0)) * 0.25);
    float h = fract(sin(dot(cell, vec2(12.9898, 78.233))) * 43758.5453);
    return h < vSet0.x ? 0.0 : 1.0;
}

void main()
{
    gRecord = vRecord;
    gDiffuse = vDiffuse;
    gSpecular = vSpecular;
    const float leafMask = LeafMask();
#ifdef RVK_PREPASS_CUTOUT
    // Depth only where the pixel is fully opaque in the end (prepass_cutout.frag): the same alpha as the cut-out test
    // below (texture stages; the lit diffuse keeps the vertex alpha).
    vec4 p0 = Sample(0u), p1 = C.stageA[0].x != 1u ? Sample(1u) : vec4(0.0);
    float pa = Cascade(p0, p1, 1.0).a;
    // Where the alpha test passes and, blended (only two-pass foliage's core comes here blended: prepass.cpp), where
    // the core is - its edges write no depth.
    if (((C.flags.x & F_ALPHATEST) != 0u && !AlphaPass(pa)) ||
        ((C.flags.x & (F_CUTOUT | F_BLENDED)) != 0u && pa < kFoliageCore))
        discard;
}
#else
    // Cut-out pixels (alpha test, F_CUTOUT) dropped before the lighting and shadows, not after: most of a plant's quad
    // is see-through. The alpha doesn't depend on the lighting (the lit diffuse keeps the vertex alpha). A dropped
    // pixel is demoted (a helper: its neighbours' derivatives still need it); a 2x2 block all dropped stops here.
#ifndef RVK_NO_CUTOUT
    bool drop = false;
    if ((C.flags.x & (F_ALPHATEST | F_CUTOUT)) != 0u) {
        vec4 e0 = Sample(0u), e1 = C.stageA[0].x != 1u ? Sample(1u) : vec4(0.0);
        float a = Cascade(e0, e1, 1.0).a * leafMask;
        drop = ((C.flags.x & F_ALPHATEST) != 0u && !AlphaPass(a)) || ((C.flags.x & F_CUTOUT) != 0u && a < vCutout);
        uint split = uint(D.motion.w + 0.5);             // two-pass foliage: the core or the edges only
        if (split == 1u) drop = drop || a < kFoliageCore;
        else if (split == 2u) drop = drop || a >= kFoliageCore;
    }
    bool q0 = subgroupQuadBroadcast(drop, 0u), q1 = subgroupQuadBroadcast(drop, 1u);
    bool q2 = subgroupQuadBroadcast(drop, 2u), q3 = subgroupQuadBroadcast(drop, 3u);
    if (drop) {
        discard;                                          // demote (Device: shaderDemoteToHelperInvocation)
        if (q0 && q1 && q2 && q3) return;
    }
#endif
    // Shadow: lit draws scale the sunlight (per-pixel lighting), others darken their final colour.
    float shade = 1.0, localScale = 1.0, texShade = 1.0;
    if ((C.flags.x & (F_SHADOW | F_SHADOWCOMP | F_SHADOWTEX)) != 0u) {
        gSunVisibility = SunVisibility(vPosW, vNormalW.xyz);
        gSunVisibilityValid = true;
        shade = 1.0 - (1.0 - gSunVisibility) * FL.shadowParams.y;
    }
    if ((C.flags.x & F_SHADOWCOMP) != 0u) {
        localScale = 1.0 / max(shade, 0.05);
        shade = 1.0;
    }
    if ((C.flags.x & F_SHADOWTEX) != 0u) {
        texShade = shade;
        shade = 1.0;
    }
    bool shadeSun = (C.flags.x & (F_PERPIXEL | F_LIGHTING)) == (F_PERPIXEL | F_LIGHTING);
    if ((C.flags.x & F_VERTEXSUN) != 0u) {             // far plant lit per vertex: only its sunlight is shadowed
        gDiffuse.rgb = clamp(vDiffuse.rgb + vMatAmbient * shade, 0.0, 1.0);
        shadeSun = true;
    }
    if ((C.flags.x & F_PERPIXEL) != 0u) {
        // Interpolated normals shrink between vertices; restore the length the vertex path lights with
        // (1 with NORMALIZENORMALS, otherwise whatever the world matrix made of the vertex normal).
        vec3 n = vNormalW.xyz;
        float len2 = dot(n, n);
        n = len2 > 0.0 ? n * (vNormalW.w * inversesqrt(len2)) : vec3(0.0);
        // The ground's lighting pass draws the lightmap; its relief comes from the base pass's texture (F_BUMPBASE):
        // that texture's normal map, else its generated normals.
        if ((C.flags.x & (F_NORMALMAP | F_BUMP)) != 0u && len2 > 0.0) {
            vec3 unbumped = n;
            bool base = (C.flags.x & F_BUMPBASE) != 0u;
            if ((C.flags.x & F_NORMALMAP) != 0u)
                n = NormalMapNormal(n, vPosW, base ? vSet0 : vTex0.xy);
            else
                n = base ? BumpNormal(texImages[D.texIdx.z], bindlessSamplers[D.sampIdx.z], n, vPosW, vSet0)
                         : BumpNormal(texImages[D.texIdx.x], bindlessSamplers[D.sampIdx.x], n, vPosW, vTex0.xy);
            // The ground's sunlight is baked into its lightmap (the live sun is kept off it): the relief scales the
            // lightmap by how much more or less the bumped surface faces the sun than the flat one.
            if (base && dot(FL.sunDir.xyz, FL.sunDir.xyz) > 0.0) {
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
    // unlit ones as they are (the ground's base pass is its bare texture; its lightmap pass keeps this). A
    // multiplying pass (F_NOALBEDO) keeps the albedo the base pass wrote, so computing it here would be wasted work.
    vec3 albedo = vec3(0.0);
    bool wantAlbedo = (C.flags.x & F_NOALBEDO) == 0u;
    if (wantAlbedo && (C.flags.x & F_LIGHTING) != 0u) {
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
    if (wantAlbedo && (C.flags.x & F_LIGHTING) == 0u)
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
                // The sun's visibility on the lit side: the shadow already computed for the surface (a second,
                // opposite-side search only darkened back-lit leaves and cost another few taps).
                float visible = FL.shadowParams.x <= 0.5 ? 1.0 : gSunVisibilityValid ? gSunVisibility : 1.0;
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
    current.a *= leafMask;
#ifndef RVK_NO_CUTOUT
    if ((C.flags.x & F_ALPHATEST) != 0u && !AlphaPass(current.a))
        discard;
    if ((C.flags.x & F_CUTOUT) != 0u && current.a < vCutout)
        discard;
#endif
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
    if ((C.flags.w & 256u) != 0u)
        current.a = 1.0;                                  // the interface layer: a replacing draw covers fully
    outColor = current;
}
#endif
