// Direct3D 7 fixed-function pixel processing: two texture stages, specular add, fog, alpha test.
#extension GL_KHR_shader_subgroup_quad : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_control_flow_attributes : require
// RVK_NO_FRAME_LIGHTS: the build for draws no frame light reaches (lighting.glsl) - not the ground's either.
#ifdef RVK_NO_FRAME_LIGHTS
#define RVK_NO_GROUND
#endif
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
#include "env_common.glsl"
#include "wind.glsl"
#include "cloud_light.glsl"

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
// Parallax (parallax.glsl): stage 0's coordinates where the eye's ray meets a side-loaded height (MAT_HEIGHT), read with
// the pixel's own derivatives; and how much of the sun reaches that point past the relief (meshes and the ground).
bool gParallax = false;
vec2 gUv0 = vec2(0.0), gUv0Dx = vec2(0.0), gUv0Dy = vec2(0.0);
float gParallaxSun = 1.0;
float gSunShare = 0.0;                          // how much of the colour is direct sunlight (contact shadows)
// PBR material (D.mat.z & MAT_PBR, lit per pixel): its maps at this pixel, and the specular light the surface reflects,
// added after the texture stages (unclamped in the HDR scene).
bool gPbrOn = false, gPbrBase = false;
vec3 gPbrAlbedo = vec3(0.0), gPbrNormal = vec3(0.0, 0.0, 1.0), gPbrSpec = vec3(0.0);
float gPbrAo = 1.0, gPbrRough = 1.0, gPbrMetal = 0.0;
// The normal the surface was lit with per pixel (world space; its normal or bump map's detail in it), for the scene's
// normals (outMotion.zw). Zero: not lit per pixel.
vec3 gShadeNormal = vec3(0.0);

// A unit normal as two numbers, octahedral (-1 .. 1): the scene's normals (outMotion.zw; ssr.frag, gbuffer.frag).
vec2 OctNormal(vec3 n)
{
    vec2 o = n.xy / (abs(n.x) + abs(n.y) + abs(n.z));
    return n.z < 0.0 ? (1.0 - abs(o.yx)) * vec2(o.x >= 0.0 ? 1.0 : -1.0, o.y >= 0.0 ? 1.0 : -1.0) : o;
}

#ifndef RVK_PREPASS_CUTOUT                       // (the pre-pass: depth only, no colour attachment)
layout(location = 0) out vec4 outColor;
#endif
#ifdef RVK_GLOW
layout(location = 1) out vec4 outGlow;     // HDR scene only: the glow attachment (F_GLOW)
layout(location = 2) out vec4 outLocal;    // HDR scene only: local lights' fraction of the colour, reflectivity, sun's share
layout(location = 3) out vec4 outMotion;   // HDR scene only: screen motion since last frame, pixels (solid geometry);
                                           // zw: a PBR surface's shading normal for the reflections (PbrNormalOut)
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
    // ADD: past 1 (HDR's headroom - Cascade's maxColor) only as far as its brighter input already is. D3D clamps it
    // to 1; the game adds a texture's alpha to the lit colour as a glow mask (the Kyr'Ozch armour: texture x (alpha +
    // light)), which with the light's headroom on top came out two or three times over, blown out white.
    case 7u: return min(a1 + a2, max(vec4(1.0), max(a1, a2)));
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
    if (stage == 0u && gParallax) return textureGrad(TEX0, gUv0, gUv0Dx, gUv0Dy);
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
// (base: the map's first layer of the set read - 0 every caster, kUntracedLayer those the rays lack.)
const int kUntracedLayer = 5;                    // rvk.h kUntracedLayer
float CascadeVisibility(int c, int base, vec3 posW, vec3 n, float nl, out float edge)
{
    if (nl >= 0.0)
        posW += n * FL.cascadeTexel[c] * (1.5 + 2.0 * (1.0 - nl));   // normal offset against self-shadowing
    vec4 sc = FL.shadowViewProj[c] * vec4(posW, 1.0);
    vec3 ndc = sc.xyz / sc.w;
    edge = max(abs(ndc.x), abs(ndc.y));
    if (edge >= 1.0 || ndc.z <= 0.0 || ndc.z >= 1.0) { edge = 2.0; return 1.0; }
    vec2 uv = ndc.xy * 0.5 + 0.5;
    vec2 ts = 1.0 / vec2(textureSize(shadowMap, 0).xy);
    float layer = float(c + base);
    float soft = FL.effects.z;
    if (soft <= 0.0) {
        float sum = 0.0;
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x)
                sum += texture(shadowMap, vec4(uv + vec2(x, y) * ts, layer, ndc.z));
        return sum / 9.0;
    }
    float texel = FL.cascadeTexel[c];
    if ((C.flags.x & F_SHADOWCHEAP) != 0u)              // far foliage: one filtered tap
        return texture(shadowMap, vec4(uv, layer, ndc.z));
    float rotation = 6.2831853 * fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))) + FL.taa.z);
    // Far cascades (distance level of detail): a texel there is wider than most penumbrae, so a small fixed filter
    // instead of the blocker search and the wide one - a quarter of the taps.
    if (c >= 2) {
        float sum = 0.0;
        for (int i = 0; i < 4; ++i)
            sum += texture(shadowMap, vec4(uv + Vogel(i, 4, rotation) * 1.5 * ts, layer, ndc.z));
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
        float d = texture(shadowDepths, vec3(uv + o, layer)).r;
        if (d < ndc.z) { blockerSum += d; blockers += 1.0; }
    }
    if (blockers == 0.0) return 1.0;
    if (blockers == float(kSearch)) return 0.0;
    float distance = (ndc.z - blockerSum / blockers) * FL.cascadeDepth[c];
    float radius = clamp(distance * k / texel, 1.0, 24.0);
    if (radius < 2.0)                            // a sharp penumbra: the wide filter would only re-read the same texels
        return texture(shadowMap, vec4(uv, layer, ndc.z));
    float sum = 0.0;
    for (int i = 0; i < 12; ++i)
        sum += texture(shadowMap, vec4(uv + Vogel(i, 12, rotation) * radius * ts, layer, ndc.z));
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

// The clouds' shadow (rvk clouds.cpp): the sunlight through the cloud layer above the point - its shadow map, made in
// the layer's plane, read where the sun's ray through the point crosses it. 1 = no cloud.
float CloudShadow(vec3 posW)
{
    uint slot = floatBitsToUint(FL.clouds.w);
    vec3 L = -FL.sunDir.xyz;
    if ((slot & 0xFFFFFu) == 0u || L.y < 0.05)
        return 1.0;
    vec2 at = posW.xz + (FL.clouds2.x - posW.y) * L.xz / L.y;
    vec2 uv = (at - FL.clouds.xy) * FL.clouds.z;
    vec2 edge = min(uv, 1.0 - uv);
    float inside = smoothstep(0.0, 0.08, min(edge.x, edge.y));
    if (inside <= 0.0)
        return 1.0;
    uint img = (slot & 0xFFFFFu) - 1u, smp = (slot >> 20) & 0xFFu;
    float t = textureLod(sampler2D(texImages[img], bindlessSamplers[smp]), uv, 0.0).r;
    return mix(1.0, t, FL.clouds2.y * inside);
}

// The terrain's shade from a low sun (rvk terrain_sun.cpp): at a point, 1 in the sun, 0 under the terrain hiding it -
// by the height a point at its place must be to see the sun over the terrain (a map round the eye; 2 units' grace:
// the ground itself, and what stands on it, not shading itself on the flat). Weighted in low in the sky.
float TerrainSun(vec3 posW)
{
    uint slot = floatBitsToUint(FL.terrainSun.w);
    if ((slot & 0xFFFFFu) == 0u || FL.terrainSun2.y <= 0.0)
        return 1.0;
    vec2 uv = (posW.xz - FL.terrainSun.xy) * FL.terrainSun.z;
    vec2 edge = min(uv, 1.0 - uv);
    float inside = smoothstep(0.0, 0.03, min(edge.x, edge.y));
    if (inside <= 0.0)
        return 1.0;
    uint img = (slot & 0xFFFFFu) - 1u, smp = (slot >> 20) & 0xFFu;
    float need = textureLod(sampler2D(texImages[img], bindlessSamplers[smp]), uv, 0.0).r;
    float lit = smoothstep(need - 1.0, need + 3.0, posW.y + 2.0);
    return mix(1.0, lit, FL.terrainSun2.y * inside);
}

float gCasterVisibility = 1.0, gCloudVisibility = 1.0;   // SunVisibility's two parts: things' shadows, the clouds'
float gTerrainVisibility = 1.0;                         // ... and the terrain's shade from a low sun (TerrainSun)
float gGroundAway = 1.0;                                // ... and the native ground's own, turned from the sun (0)

// The native ground's own shade, turned from the sun (gGroundAway: 1 facing it .. 0 turned away), falling off over a
// few degrees round that line - with or without a shadow map: under ~3 degrees there is none, and the slopes turned
// away were only tinted, grey sand as bright as the lit (the user's rvk-frame-161624). Gone as the sun sets below the
// horizon (by then the hour's light has no sun in it), and by the moon's light.
float GroundAway(vec3 n)
{
    if (floatBitsToUint(D.terrain.y) == 0u || FL.moon.w > 0.5 || dot(n, n) <= 0.0) return 1.0;
    const vec3 L = -normalize(FL.sunDir.xyz + vec3(0.0, -1e-6, 0.0));
    return mix(1.0, smoothstep(-0.06, 0.06, dot(normalize(n), L)), smoothstep(-0.08, -0.02, L.y));
}

float SunVisibility(vec3 posW, vec3 n)
{
    gCloudVisibility = CloudShadow(posW);
    gTerrainVisibility = FL.moon.w > 0.5 ? 1.0 : TerrainSun(posW);
    gGroundAway = GroundAway(n);
    if (FL.shadowParams.x <= 0.5) {              // (no shadow map: the terrain's shade alone)
        gCasterVisibility = 1.0;
        return gCloudVisibility;
    }
    float v = SunVisibilityCasters(posW, n);
    if (v > 0.0 && (C.flags.x & F_SHADOWCHEAP) == 0u) {
        bool hasN = dot(n, n) > 0.0;
        vec3 nn = hasN ? normalize(n) : n;
        float nl = hasN ? dot(nn, -FL.sunDir.xyz) : -1.0;
        if (!hasN || nl > 0.0)                   // (facing away: the light equation handles it, as above)
            v *= mix(1.0, GrassShadowVisibility(posW, nn, nl), kGrassShadowStrength);
    }
    gCasterVisibility = v;
    return v * gCloudVisibility;
}

// The terrain's shade from a low sun at the shadows' own strength (FL.terrainSun2.z: theirs fade out low, it doesn't -
// under the mountains the ground is wholly in shade, not lit between the trees' faded shadows - user, 2026-10-13).
// The ground turned from the sun takes the shadows' unfaded strength too: as the sun nears the horizon - when most of
// a dune field turns from it - theirs fades out (draw.cpp), and at 4 degrees up the lee slopes were lit again (the
// user's rvk-frame-155738).
float TerrainShade()
{
    return min(1.0 - (1.0 - gTerrainVisibility) * FL.terrainSun2.z,
               1.0 - (1.0 - gGroundAway) * max(FL.shadowParams.y, FL.terrainSun2.z));
}

// A cloud's shadow takes the sunlight, not the sky's: where a shadow darkens all of a surface's light (the ground's
// lightmap holds the sun and the sky's light together; unlit surfaces), only about the sun's part of it goes - a cloud
// over the sun had left the ground near black (user, 2026-10-13).
const float kCloudSunShare = 0.5;
// A low sun's light on a surface lit as a whole (the ground's lightmap, the grass): the sun's warm tint where its
// light reaches - however grazing: how bright is the lightmap's say, its colour the sun's (by the facing alone, level
// ground under a sun 2 degrees up went grey-blue) -, the sky's cool one where it faces away or lies in shade
// (FL.dawn; rvk sky.cpp).
vec3 DawnTint(vec3 n, float lit)
{
    vec3 nn = dot(n, n) > 0.0 ? normalize(n) : vec3(0.0, 1.0, 0.0);
    float reached = smoothstep(-0.03, 0.05, dot(nn, -normalize(FL.sunDir.xyz + vec3(0.0, -1e-6, 0.0)))) * lit;
    return mix(FL.dawn[1].rgb, FL.dawn[0].rgb, reached);
}

// The light rig (rvk light_rig.cpp, docs/light-rig-plan.md): FL.cond.z 1 when on; FL.cond.y the sun's share of the
// land's light (1 at noon, ~0.5 at 5 degrees up, none at the horizon: rvk LightRig sunShare). Off, the old day / night
// switches.
bool RigOn() { return FL.cond.z > 0.5; }

float FlatShade()
{
    // (A thunderstorm - FL.rainNear.y: the sun hidden as by cloud everywhere, more than a cloud's shadow takes - its
    // share of the land's light gone, so the shadows of things with it; and the sky's share halved, a storm's sky far
    // dimmer than the clear one the lightmap holds - rvk Device::StormSkyLight: a quarter of the light left.)
    float o = FL.rainNear.y;
    float things = min(1.0 - (1.0 - gCasterVisibility) * ShadowStrength(length(vPosW - C.eyePos.xyz)) * (1.0 - o),
                       mix(TerrainShade(), 1.0, o));
    if (!RigOn())
        return things * (1.0 - kCloudSunShare * max(1.0 - gCloudVisibility, o)) * (1.0 - 0.5 * o);
    // With the rig: the sun's parts (things' shadows, the clouds', the storm taking the sun) by the sun's share of the
    // light, the storm's dimmer sky whatever the hour - by night the moon's map's shadows are the moon's alone (the
    // ground pass's moonShade), and the stormy land no longer brightens as the moon takes over at dusk.
    float sun = FL.cond.y;
    if (FL.moon.w > 0.5) things = 1.0;
    return mix(1.0, things, sun) * (1.0 - kCloudSunShare * max(1.0 - gCloudVisibility, o) * sun) * (1.0 - 0.5 * o);
}

int gShadowCascade = -1;                         // the cascade SunVisibilityCasters read (RVK_ShadowView)

float SunVisibilityMap(vec3 posW, vec3 n, float nl, int base);

#ifdef RVK_GLOW
// The rays' sun visibility at this pixel (rvk raytrace.cpp, RVK_RtShadow: traced from the pre-pass's depth, before the
// scene is shaded) - where this fragment is the surface they were traced from (its view depth the mask's); else -1.
float RtShadowMask()
{
    uint bits = floatBitsToUint(FL.rtShadow.x);
    if ((bits & 0xFFFFFu) == 0u)
        return -1.0;
    uint img = (bits & 0xFFFFFu) - 1u, smp = (bits >> 20) & 0xFFu;
    vec2 m = texelFetch(sampler2D(texImages[img], bindlessSamplers[smp]), ivec2(gl_FragCoord.xy), 0).rg;
    float z = FL.rtShadow.z / (gl_FragCoord.z - FL.rtShadow.y);
    // (The pre-pass's own surfaces match to the mask's half-float depth, ~0.05 %; a thing in front of one - a character's
    // foot on the ground - by more.)
    return abs(m.g - z) < 0.002 * z + 0.01 ? m.r : -1.0;
}
#endif

// The native ground faced away from the sun is in its own shade (gGroundAway, taken with the terrain's shade in
// TerrainShade), and the map's shadows are drawn there too, not left to the lightmap - the hour's bake is a 4-unit
// cell's, and a low sun's shadows (a rock's, a character's) stopped dead at the line where the smooth ground turned
// from the sun, lit sand inside them past it (the user's rvk-frame-154711, rvk-frame-155057, 19 h). It falls off over
// a few degrees round that line.
float SunVisibilityCasters(vec3 posW, vec3 n)
{
    vec3 L = -FL.sunDir.xyz;
    float nl = -1.0;
    if (dot(n, n) > 0.0) {
        n = normalize(n);
        nl = dot(n, L);
        if (floatBitsToUint(D.terrain.y) != 0u) {
            if (gGroundAway <= 0.0) return 1.0;      // (SunVisibility: GroundAway)
            nl = max(nl, 0.0);
        } else if (nl <= 0.0)
            return 1.0;                                    // faces away: the light equation handles it
    }
#ifdef RVK_GLOW
    // Ray traced (RVK_RtShadow): the rays' mask, times the map's casters they lack (characters, plants, cut-outs).
    float traced = RtShadowMask();
    if (traced >= 0.0)
        return traced * SunVisibilityMap(posW, n, nl, kUntracedLayer);
#endif
    return SunVisibilityMap(posW, n, nl, 0);
}

float SunVisibilityMap(vec3 posW, vec3 n, float nl, int base)
{
    int count = int(FL.shadowParams.z);
    for (int c = 0; c < count; ++c) {
        float edge;
        float v = CascadeVisibility(c, base, posW, n, nl, edge);
        if (edge >= 1.0) continue;
        gShadowCascade = c;
        float blend = (C.flags.x & F_SHADOWCHEAP) != 0u ? 0.0 : smoothstep(0.8, 1.0, edge);
        if (blend <= 0.0) return v;
        float next = 1.0;
        if (c + 1 < count) {
            float e2;
            next = CascadeVisibility(c + 1, base, posW, n, nl, e2);
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

// What a PBR surface reflects of its surroundings without a reflection probe: a sky-and-ground environment made from
// the scene's ambient light, the fog colour (the horizon) and a little sunlight, through the split-sum BRDF, with
// specular occlusion from the occlusion map (Lagarde). Screen-space reflections replace part of it (reflectivity).
vec3 gPbrEnv = vec3(0.0);                        // the surroundings PbrAmbientSpecular reflected (debug view 9)
vec3 PbrAmbientSpecular(vec3 nu, vec3 V, vec3 ambientLight)
{
    float nv = max(dot(nu, V), 1e-4);
    vec3 R = reflect(-V, nu);
    vec3 sky = (C.flags.x & F_FOG) != 0u ? mix(ambientLight, C.fogColor.rgb, 0.5) : ambientLight;
    sky += FL.sunColor.rgb * 0.25;
    vec3 env = mix(ambientLight * 0.5, sky * 1.3, smoothstep(-0.4, 0.6, R.y));
    // The environment probe (what the camera has seen that way), where it is sure, between its two levels nearest
    // the roughness.
    uint probe = floatBitsToUint(FL.pbr2.w);
    if ((probe & 0xFFFFFu) != 0u) {
        float strength = float(probe >> 28) / 15.0;
        float lod = clamp(gPbrRough, 0.0, 1.0);
        lod = lod < 0.3 ? (lod - 0.08) / 0.22 : lod < 0.5 ? 1.0 + (lod - 0.3) / 0.2 : lod < 0.75 ? 2.0 + (lod - 0.5) / 0.25
            : 3.0 + (lod - 0.75) / 0.25;
        lod = clamp(lod, 0.0, kEnvLevels - 1.0);
        int l0 = int(lod), l1 = min(l0 + 1, int(kEnvLevels) - 1);
        uint img = (probe & 0xFFFFFu) - 1u, smp = (probe >> 20) & 0xFFu;
#define ENVATLAS sampler2D(texImages[img], bindlessSamplers[smp])
        vec4 p = mix(textureLod(ENVATLAS, EnvAtlasUv(R, l0), 0.0), textureLod(ENVATLAS, EnvAtlasUv(R, l1), 0.0), lod - float(l0));
#undef ENVATLAS
        env = mix(env, p.rgb, clamp(p.a, 0.0, 1.0) * strength);
    }
    gPbrEnv = env;
    float so = clamp(pow(nv + gPbrAo, exp2(-16.0 * gPbrRough - 1.0)) - 1.0 + gPbrAo, 0.0, 1.0);
    return EnvBrdfApprox(gPbrF0, gPbrRough, nv) * env * so;
}

// What wet ground reflects in a direction R: the environment probe (what the camera has seen that way; level 0 for
// a puddle's mirror, 1 for soaked ground's blur) where it is sure, else the sky's light guessed as PbrAmbientSpecular
// does. Needs no screen-space reflections.
vec3 WetReflection(vec3 R, int level)
{
    vec3 amb = C.ambient.rgb + FL.sunColor.rgb * 0.15;
    vec3 sky = ((C.flags.x & F_FOG) != 0u ? mix(amb, C.fogColor.rgb, 0.5) : amb) + FL.sunColor.rgb * 0.25;
    vec3 env = mix(amb * 0.5, max(sky * 1.3, FL.cloudSky.rgb), smoothstep(-0.4, 0.6, R.y));
    uint probe = floatBitsToUint(FL.pbr2.w);
    if ((probe & 0xFFFFFu) != 0u) {
        uint img = (probe & 0xFFFFFu) - 1u, smp = (probe >> 20) & 0xFFu;
        vec4 p = textureLod(sampler2D(texImages[img], bindlessSamplers[smp]), EnvAtlasUv(R, level), 0.0);
        env = mix(env, p.rgb, clamp(p.a, 0.0, 1.0));
    }
    return env;
}

// Normal mapping (F_NORMALMAP): the texture's own tangent-space normal map, OpenGL convention (what Blender bakes:
// +X along +u, +Y up in the image = against D3D's v). A normal (x, y, z) is the height slope (-x/z, y/z) per world
// unit along u and down-v; scaled by the world size of a uv unit it bends the normal exactly like the generated
// normals above, so it needs no tangents. C.misc.w = strength.
vec3 NormalMapNormalGrad(vec3 n, vec3 posW, vec2 uv, vec2 uvx, vec2 uvy)
{
    vec3 t = textureGrad(NORMALTEX, uv, uvx, uvy).xyz * 2.0 - 1.0;
    t.z = max(t.z, 0.05);
    vec2 slope = vec2(-t.x, t.y) / t.z * C.misc.w;     // height per world unit along u, v
    vec3 px = dFdx(posW), py = dFdy(posW);
    float worldPerUv = sqrt(max(dot(px, px), dot(py, py)) / max(max(dot(uvx, uvx), dot(uvy, uvy)), 1e-12));
    return BendNormal(n, px, py, dot(slope, uvx) * worldPerUv, dot(slope, uvy) * worldPerUv);
}
vec3 NormalMapNormal(vec3 n, vec3 posW, vec2 uv) { return NormalMapNormalGrad(n, posW, uv, dFdx(uv), dFdy(uv)); }

#include "terrain.glsl"

// Things' shadows fading out as the light nears the horizon, as their strength does (ShadowStrength, by distance) -
// for the uses of the visibility that don't go through that strength.
float CasterFade()
{
    return FL.shadowParams.y > 0.0 ? ShadowStrength(length(vPosW - C.eyePos.xyz)) / FL.shadowParams.y : 0.0;
}

// RVK_CrownDepth: how much of the sun's light gets through a crown to a point - by the stretch it crossed from the first
// thing it hit over the point (the cascades' depths) to here, in world units: exp(-d / 1.5) - a leaf at a crown's thin
// rim takes most of it, one deep inside little, one under another crown or a wall none. -1: no cascade covers it.
float CrownThrough(vec3 posW)
{
    int count = int(FL.shadowParams.z);
    for (int c = 0; c < count; ++c) {
        vec4 sc = FL.shadowViewProj[c] * vec4(posW, 1.0);
        vec3 ndc = sc.xyz / sc.w;
        if (max(abs(ndc.x), abs(ndc.y)) >= 0.98 || ndc.z <= 0.0 || ndc.z >= 1.0) continue;
        float d = texture(shadowDepths, vec3(ndc.xy * 0.5 + 0.5, float(c))).r;
        return exp(-max(ndc.z - d, 0.0) * FL.cascadeDepth[c] / 1.5);
    }
    return -1.0;
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
// later, over the finished scene, its soft edges - below it, without depth (D.motion.w: 1 core, 2 edges, 0 whole, 3 dithered).
const float kFoliageCore = 0.95;
// Far two-pass foliage dithered instead (RVK_FolDither, D.motion.w 3): drawn once, with depth, its soft edges a moving
// pattern of whole pixels - a pixel kept where its alpha beats the noise there - that the TAA averages back into the
// soft edge, with no second pass and no layers of edges lit behind each other. The noise is the same in the pre-pass
// (screen position, the frame's offset) and differs between the cards over a pixel by their depth: else the layers of
// a crown would all show or all hide together there. Below the plant's cut-out, nothing; from the core up, all.
float FoliageDitherCut()
{
    float n = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))) + FL.taa.z +
                    fract(7.31 / gl_FragCoord.w));
    return max(vCutout, min(n, kFoliageCore));
}

// Leaves (leaves.cpp; ffp.vert passes vSet0 for a canopy with leaves, D.leaf.w > 0): a leaf is a round sprig - its card
// fades out towards the rim (vSet0 = where on the card, -1 .. 1), not a square cut through the painted cluster; the
// canopy's own cards are thinned out in place (vSet0 = (how much, 2)): blocks of texels dropped by a stable hash, the
// leaves around them making up the crown.
float LeafMask()
{
    return D.leaf.w > 0.0 ? LeafMaskAt(vSet0, vTex0.xy * vec2(textureSize(TEX0, 0))) : 1.0;
}

// The rain's cover map at a point (rvk sand_map.cpp RainCoverMap, FL.cover): x how sheltered (under a roof 1, a canopy
// mostly), y how far the ground there lies in a hollow (puddles).
vec2 RainCoverAt(vec3 p)
{
    uint slot = floatBitsToUint(FL.cover.w);
    if ((slot & 0xFFFFFu) == 0u || FL.cover.z <= 0.0)
        return vec2(0.0);
    vec2 uv = (p.xz - FL.cover.xy) * FL.cover.z;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThanEqual(uv, vec2(1.0))))
        return vec2(0.0);
    uint img = (slot & 0xFFFFFu) - 1u, smp = (slot >> 20) & 0xFFu;
    vec4 m = textureLod(sampler2D(texImages[img], bindlessSamplers[smp]), uv, 0.0);
    // (Linear: a roof's edge softened over a cell. Its solid's top over the point - a hair: the roof's own top is open.)
    float solid = m.r > -8000.0 ? smoothstep(0.1, 0.5, m.r + FL.wet.w - p.y) : 0.0;
    float crown = m.g > -8000.0 ? 0.7 * smoothstep(0.1, 0.5, m.g + FL.wet.w - p.y) : 0.0;
    vec2 edge = min(uv, 1.0 - uv);
    float inside = smoothstep(0.0, 0.05, min(edge.x, edge.y));
    return vec2(max(solid, crown) * inside, max(m.a, 0.0) * inside);
}

float WetHash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float WetNoise(vec2 p)
{
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(WetHash(i), WetHash(i + vec2(1, 0)), f.x), mix(WetHash(i + vec2(0, 1)), WetHash(i + 1.0), f.x), f.y);
}
// Raindrops' rings on a puddle: a few drops a cell each time round, a ring spreading and fading (0..1).
float WetRipples(vec2 xz, float time)
{
    float sum = 0.0;
    for (int k = 0; k < 2; ++k) {
        vec2 q = xz * 2.2 + float(k) * 0.37;
        vec2 cell = floor(q);
        float h = WetHash(cell + float(k) * 13.1);
        float t = fract(time * 1.3 + h);
        vec2 at = cell + vec2(WetHash(cell + 3.7 + floor(time * 1.3 + h)), WetHash(cell + 9.1 + floor(time * 1.3 + h)));
        float r = length(q - at) - t * 0.5;
        sum += exp(-r * r * 900.0) * (1.0 - t);
    }
    return clamp(sum, 0.0, 1.0);
}

// The game's cloud sheets (MAT_CLOUDSHEET: its flat sky sheets, drawn behind the cloud layer) lit as the layer lights
// its clouds - the whitened game colour, the sky's light (raised by night), the sun through a thin sheet (bright and
// silvery towards it, thick parts darker; by night the moon's the same), the sunset's colours and afterglow - and hazed as the layer by the air
// (the game's fog colour at the horizon, paler above). tex: the sheet's texture (alpha: its shape; colour: shading).
vec3 CloudSheet(vec4 tex)
{
    const vec3 luma = vec3(0.3, 0.59, 0.11);
    vec3 dir = normalize(vPosW - C.eyePos.xyz);
    vec3 sunL = normalize(FL.sunTrue.xyz + vec3(0.0, 1e-6, 0.0));
    float thick = clamp(tex.a, 0.0, 1.0);
    const float g = 0.6;
    float phase = min((1.0 - g * g) / pow(1.0 + g * g - 2.0 * g * dot(dir, sunL), 1.5), 8.0);
    vec3 tint = FL.cloudTint.rgb;
    vec3 sun = SunLight(sunL) * (0.3 + 0.2 * phase) * (1.0 - 0.45 * thick) * mix(tint, vec3(dot(tint, luma)), 0.5);
    float moonPhase = min((1.0 - g * g) / pow(1.0 + g * g - 2.0 * g * dot(dir, MoonDir()), 1.5), 8.0);
    sun += MoonLight() * (0.3 + 0.2 * moonPhase) * (1.0 - 0.45 * thick) * mix(tint, vec3(dot(tint, luma)), 0.5);
    vec3 sky = FL.cloudSky.rgb * DuskSkyTint() * mix(1.0, 0.72, thick) * NightSky() * mix(1.0, 0.7, MoonShare()) * tint;
    vec3 S = (sun + sky + Afterglow(sunL) * 0.6 * tint) * FL.cloudTint.w * mix(0.8, 1.05, dot(tex.rgb, luma));
    // Lightning (rvk rain.cpp Lightning): the sheet lit as the cloud layer is (cloud_layer.frag FlashInCloud) - the
    // sheets sit on the game's small sky, so where this one would be at the clouds' height (~900 over the eye), by
    // how far across from the strike.
    if (FL.flash.w > 0.0) {
        vec2 at = C.eyePos.xz + dir.xz / max(dir.y, 0.08) * 900.0 - FL.flashWorld.xz;
        S += vec3(0.8, 0.85, 1.0) * tint * (FL.flash.w * 1.2 * (1.0 - 0.3 * thick) / (1.0 + dot(at, at) / (1500.0 * 1500.0)));
    }
    float low = HorizonBand(dir, FL.cloudAir.w);
    return mix(S, AirColour(SkyAirAll(FL.cloudAir.rgb, dir, FL.sky[0], FL.sky[1], FL.sky[3], FL.skyRing), low), max(0.25, low));
}

#ifdef RVK_LITE
// The lean build (ffp_lite.frag; rvk draw.cpp m_drawLite, RVK_LiteShader - after gorg's ao-vk #21): pre-transformed 2D
// draws on 8-bit targets - the interface - unlit, unfogged, unshadowed, with no material maps. Of main() below only
// this much ever runs for them; the full build is held at the GPU's register limit by the rest (128 a pixel on an RTX
// 4070 Ti, 256 on an RX 6600 XT: its pixels few at a time, each waiting on its texture reads), and the interface paid
// that too. The same output as the full path, step for step in its order - a change to those steps there belongs here
// too (RVK_LiteShader 0 and rvk_demo --ui-window compare the two). (The debug views aside.)
void main()
{
    gRecord = vRecord;
    gDiffuse = vDiffuse;
    gSpecular = vSpecular;
    vec4 t0 = Sample(0u);
    vec4 t1 = C.stageA[0].x != 1u ? Sample(1u) : vec4(0.0);
    vec4 current = Cascade(t0, t1, 1.0);
    // (Unlit and unshadowed: the full path's flat shade is 1 - with the light rig off, by night a storm's dimming.)
    if (!RigOn() && FL.moon.w > 0.5)
        current.rgb *= (1.0 - kCloudSunShare * FL.rainNear.y) * (1.0 - 0.5 * FL.rainNear.y);
    if (C.vtx.y != 0u) {                                  // (the per-draw colour scale: as the full path's)
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
    if ((D.mat.z & MAT_PICKED) != 0u)
        current.rgb = mix(current.rgb, vec3(1.0, 0.85, 0.1) * max(dot(current.rgb, vec3(0.3, 0.59, 0.11)), 0.35) * 2.0, 0.7);
#ifndef RVK_NO_CUTOUT
    if ((C.flags.x & F_ALPHATEST) != 0u && !AlphaPass(current.a))
        discard;
    if ((C.flags.x & F_CUTOUT) != 0u && current.a < vCutout)
        discard;
#endif
    if ((C.flags.w & 512u) != 0u)
        current = vec4(0.10, 0.32, 0.14, 1.0);
    if ((C.flags.w & 256u) != 0u)
        current.a = 1.0;
    if (FL.terrainSun2.w > 0.5 && (C.flags.w & 256u) == 0u)   // (RVK_ShadowView: no shadow looked up here)
        current.rgb = vec3(0.5) * mix(0.08, 1.0, 0.6);
    if (any(isnan(current)) || any(isinf(current))) current = vec4(0.0, 0.0, 0.0, any(isnan(current.aaaa)) ? 1.0 : current.a);
    current.rgb = max(current.rgb, vec3(0.0));
    outColor = current;
}
#else
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
    float pa = Cascade(p0, p1, 1.0).a * leafMask;   // (a canopy's thinned cards: none where they are dropped)
    // Where the alpha test passes and, blended (only two-pass foliage's core comes here blended: prepass.cpp), where
    // the core is - its edges write no depth.
    float core = uint(D.motion.w + 0.5) == 3u ? FoliageDitherCut() : kFoliageCore;
    if (((C.flags.x & F_ALPHATEST) != 0u && !AlphaPass(pa)) || ((C.flags.x & (F_CUTOUT | F_BLENDED)) != 0u && pa < core))
        discard;
}
#else
    // Cut-out pixels (alpha test, F_CUTOUT) dropped before the lighting and shadows, not after: most of a plant's quad
    // is see-through. The alpha doesn't depend on the lighting (the lit diffuse keeps the vertex alpha). A dropped
    // pixel is demoted (a helper: its neighbours' derivatives still need it); a 2x2 block all dropped stops here.
    // A shop's room drawn into its doorway: nothing of it on the street's side of the doorway's plane (its entrance's
    // own wall and frame stand there - they hid the opening).
    if (dot(D.clip.xyz, D.clip.xyz) > 0.0 && dot(vPosW, D.clip.xyz) + D.clip.w < 0.0)
        discard;
#ifndef RVK_NO_CUTOUT
    bool drop = false;
    if ((C.flags.x & (F_ALPHATEST | F_CUTOUT)) != 0u) {
        vec4 e0 = Sample(0u), e1 = C.stageA[0].x != 1u ? Sample(1u) : vec4(0.0);
        float a = Cascade(e0, e1, 1.0).a * leafMask;
        drop = ((C.flags.x & F_ALPHATEST) != 0u && !AlphaPass(a)) || ((C.flags.x & F_CUTOUT) != 0u && a < vCutout);
        uint split = uint(D.motion.w + 0.5);             // two-pass foliage: the core or the edges only
        if (split == 1u) drop = drop || a < kFoliageCore;
        else if (split == 2u) drop = drop || a >= kFoliageCore;
        else if (split == 3u) drop = drop || a < FoliageDitherCut();   // (dithered: all of it, once)
    }
    bool q0 = subgroupQuadBroadcast(drop, 0u), q1 = subgroupQuadBroadcast(drop, 1u);
    bool q2 = subgroupQuadBroadcast(drop, 2u), q3 = subgroupQuadBroadcast(drop, 3u);
    if (drop) {
        discard;                                          // demote (Device: shaderDemoteToHelperInvocation)
        if (q0 && q1 && q2 && q3) return;
    }
#endif
    // The ground laid on its cliffs and its repeats hidden (terrain.glsl): where its texture lies here, taken while
    // the derivatives are good; on the lighting pass, the base texture as the base pass drew it (its PBR albedo, and
    // the hexagons' weights its maps are read with).
    // A mesh's side-loaded height (MAT_HEIGHT: its normal map's alpha): stage 0's coordinates, its normal and material
    // maps' read where the eye's ray meets it (parallax.glsl).
    if ((D.mat.z & MAT_HEIGHT) != 0u && (C.flags.x & (F_NORMALMAP | F_PERPIXEL)) == (F_NORMALMAP | F_PERPIXEL) &&
        (C.flags.x & F_BUMPBASE) == 0u) {
        gUv0Dx = dFdx(vTex0.xy);
        gUv0Dy = dFdy(vTex0.xy);
        gUv0 = ParallaxUv(texImages[D.texIdx.w], bindlessSamplers[D.sampIdx.w], vTex0.xy, gUv0Dx, gUv0Dy, vPosW,
                          dFdx(vPosW), dFdy(vPosW), vNormalW.xyz, gParallaxSun);
        gParallax = true;
    }
    TerrainMap terrain;
    GroundRelief ground;
    ground.sand = 0.0;
    ground.fade = vec2(0.0);
    ground.tiles = false;
    ground.cell = vec2(0.0);
    ground.rip.smallDepth = ground.rip.bigDepth = 0.0;
    const bool terrainMapped = TerrainMapped();
    vec4 terrainBase = vec4(0.0);
    const uvec3 oneSlots = uvec3(gRecords.records[gRecord].pad[0], gRecords.records[gRecord].pad[1],
                                 gRecords.records[gRecord].pad[2]);
    const bool groundOne = oneSlots.x != 0u && terrainMapped && D.terrainAt.w > 1.5;
    vec3 oneBase = vec3(1.0);
    if (terrainMapped) {
        terrain = TerrainMapAt(vPosW, vSet0, vNormalW.xyz);
        // The native ground's relief in depth up close - its tiles' heights, the sand's ripples - in both passes alike
        // (parallax.glsl): its cells' coordinates moved to where the eye's ray meets it.
        if (TerrainNative()) {
            ground = GroundReliefAt(terrain, vPosW, vNormalW.xyz);
            gParallaxSun = GroundParallax(terrain, ground, vPosW, vNormalW.xyz);
        }
        // The native ground in one pass (RVK_GroundOne; rvk native_ground.cpp DrawNativeGroundOne): its lighting pass
        // lays its base pass's colour too, as that pass lays it - its stand-in and sampler in the record's spare words
        // - and multiplies it in at the fog (below), where the blend did with two passes.
        // Laid on a copy of the map (laying it settles its weights, which this pass's lighting would then read where it
        // doesn't lay the colour itself - the ripples' crests lit a little differently); once only where it does: its
        // material maps' albedo below is the same colour.
        if (groundOne) {
            TerrainMap mb = terrain;
            const vec4 b = TerrainBase(oneSlots.x - 1u, bindlessSamplers[oneSlots.y], mb);
            oneBase = TerrainVariation(b.rgb, vPosW, mb);
            if (TerrainNative()) {
                const vec3 at = vPosW + vec3((mb.uv[0] - ground.cell).x, 0.0, (mb.uv[0] - ground.cell).y) / max(D.terrain.x, 1e-6);
                oneBase = SandRippleTone(TerrainGrain(oneBase, mb, ground.tiles || ground.sand > 0.0 ? at : vPosW), ground, mb.uv[0]);
            }
            if (D.terrainAt.w > 1.5 && ((C.flags.x & F_BUMPBASE) != 0u || (TerrainNative() && (D.mat.z & MAT_BASE) != 0u))) {
                terrainBase = b;
                terrain = mb;
            } else if (D.terrainAt.w > 1.5 && terrain.rock && !terrain.pebble) {
                TerrainSettle(texImages[TerrainRockImage(0)], terrain);
            }
        }
        // (The native ground's with material maps: its colour as its base pass drew it - the cells' own tiles.)
        else if (D.terrainAt.w > 1.5 && ((C.flags.x & F_BUMPBASE) != 0u || (TerrainNative() && (D.mat.z & MAT_BASE) != 0u)))
            terrainBase = TerrainBase(D.texIdx.z, bindlessSamplers[D.sampIdx.x], terrain);
        else if (D.terrainAt.w > 1.5 && terrain.rock && !terrain.pebble)   // (no maps of the base's own: only the rock's relief)
            TerrainSettle(texImages[TerrainRockImage(0)], terrain);
    }
    // Shadow: lit draws scale the sunlight (per-pixel lighting), others darken their final colour.
    float shade = 1.0, localScale = 1.0, texShade = 1.0, flatShade = 1.0;
    if ((C.flags.x & (F_SHADOW | F_SHADOWCOMP | F_SHADOWTEX)) != 0u) {
        gSunVisibility = SunVisibility(vPosW, vNormalW.xyz);
        gSunVisibilityValid = true;
        shade = min(1.0 - (1.0 - gSunVisibility) * ShadowStrength(length(vPosW - C.eyePos.xyz)), TerrainShade());
        flatShade = FlatShade();                  // (all of the light: the clouds' only in part)
    }
    if ((C.flags.x & F_SHADOWCOMP) != 0u) {
        localScale = 1.0 / max(flatShade, 0.05);
        shade = 1.0;
    }
    // By night the map is the moon's (FL.moon.w): its shadow takes only the moon's light - the ground's lightmap and
    // ambient (the night's own light, not the moon's) stay.
    float moonShade = 1.0;
    if ((C.flags.x & F_SHADOWTEX) != 0u) {
        if (FL.moon.w > 0.5)
            moonShade = shade;
        // (With the rig the lightmap takes the storm and the clouds by night too: FlatShade.)
        if (FL.moon.w <= 0.5 || RigOn())
            texShade = flatShade;
        shade = 1.0;
    }
    bool shadeSun = (C.flags.x & (F_PERPIXEL | F_LIGHTING)) == (F_PERPIXEL | F_LIGHTING);
    if ((C.flags.x & F_VERTEXSUN) != 0u) {             // far plant lit per vertex: only its sunlight is shadowed
        gDiffuse.rgb = clamp(vDiffuse.rgb + vMatAmbient * shade, 0.0, 1.0);
        shadeSun = true;
    }
    // The stage textures, the native ground's lightmap among them, ahead of the lighting: they don't need it, and done
    // after it the ground's map and relief were held through it. (A rock takes its shade here: texShade 1 after it -
    // the lighting takes the shade as it was, texShadeLit.)
    const float texShadeLit = texShade;
    // (The ground's base pass: its texture as terrain.glsl lays it.)
    const bool terrainBasePass = terrainMapped && D.terrainAt.w < 1.5;
#ifdef RVK_NO_GROUND
    vec4 t0 = vec4(0.0);   // (sampled after the lighting: with no ground's map to let go first, held through it it cost registers)
#else
    vec4 t0 = terrainBasePass ? TerrainBase(D.texIdx.x, bindlessSamplers[D.sampIdx.x], terrain) : Sample(0u);
#endif
    // The native ground's lightmap read where the pixel is (its cell from world x, z), not where its triangle's corners
    // say: a cliff's cells are split corner to corner and each half spread the light its own way - dark wedges up the
    // face. Read so, a cliff's light runs evenly down it, as the baked texel says.
    // On a cliff, blurred over the cells round it (by its steepness): one texel a cell stretched up a face of 40 units
    // turned the cells' light into hard triangles where the face crosses from a dark cell to a lit one.
    vec3 sandGlint = vec3(0.0), sandMean = vec3(1.0);   // (the glints and the sand's colour they shine against)
    if (terrainMapped && D.terrainAt.w > 1.5 && TerrainNative()) {
        const vec2 lmSize = vec2(textureSize(TEX0, 0)), at = vPosW.xz * D.terrain.x / lmSize;
        t0 = NativeLightmapAt(at);
        if (terrain.side > 0.01 && !terrain.clutter) {   // (not the rocks on it: they stand on its flat)
            vec4 blur = vec4(0.0);
            for (int j = -1; j <= 1; ++j)
                for (int i = -1; i <= 1; ++i) blur += NativeLightmapAt(at + vec2(i, j) * 1.5 / lmSize);
            t0 = mix(t0, blur / 9.0, terrain.side);
        }
        // (The rocks on it: shaped by the sun now, its shadow on its part only - NativeRockLight.)
        if (terrain.clutter) {
            t0.rgb *= NativeRockLight(vNormalW.xyz, t0.g, texShade);
            texShade = 1.0;
        } else {
            // (In its own relief's shadow - a ripple's lee, the gap between stones: the lightmap's sun share of it.)
            // (The sand's glints: where the lightmap says the sun reaches - the shadow map's shade comes after. Added
            // past the stages, which clip at 1: below.)
            // (Whether the sun reaches it: on sand, our own shadows say - the map's, the ground turned from the sun, the
            // terrain's - not the bake's cell: a low sun's bake has cells in shade where the sun now isn't hidden, and
            // the ripples in them lost their troughs' shadows, square pale patches among shaded ripples - the user's
            // rvk-frame-160817/160823, the sun 2 degrees up. Elsewhere, and without our shadows, the bake's.)
            const float bakedSun = NativeSunShare(vNormalW.xyz, false, t0.g);
            const float sunShare = gSunVisibilityValid
                ? mix(bakedSun, gCasterVisibility * gGroundAway * gTerrainVisibility, NativeSand(terrain.uv[0])) : bakedSun;
            sandGlint = FL.sunColor.rgb * (SandGlint(ground, terrain.uv[0], vPosW, vNormalW.xyz, terrain.px, terrain.py) *
                                           gParallaxSun * sunShare);
            sandMean = NativeMean(terrain.uv[0]);
            const float dunes = NativeSand(terrain.uv[0]) * clamp(FL.sand.y, 0.0, 1.0);   // (on sand: NativeSandLight)
            const vec3 turn = dunes > 0.0 ? mix(vec3(NativeSunTurn(vNormalW.xyz, t0.g)), NativeSandLight(vNormalW.xyz, t0.g), dunes)
                                          : vec3(NativeSunTurn(vNormalW.xyz, t0.g));
            // (Its relief's own shadow takes the sun's part of the light, not the sky's - kSandSun's share, as the
            // dunes' light: taking it all, a footprint's floor and the ripples' troughs went near black.)
            t0.rgb *= turn * mix(1.0, gParallaxSun, 0.75 * sunShare);
        }
    }
    if (terrainBasePass) t0.rgb = TerrainVariation(t0.rgb, vPosW, terrain);
    if (terrainBasePass && TerrainNative()) {
        // (The grit where the parallax met the ground: its cells' coordinates moved, the world point with them.)
        const vec3 at = vPosW + vec3((terrain.uv[0] - ground.cell).x, 0.0, (terrain.uv[0] - ground.cell).y) / max(D.terrain.x, 1e-6);
        t0.rgb = SandRippleTone(TerrainGrain(t0.rgb, terrain, ground.tiles || ground.sand > 0.0 ? at : vPosW), ground,
                                terrain.uv[0]);
    }
    // (What the end needs of the ground's map, taken now: the map and the relief go before the lighting, which needs
    // the most registers there are - the ground's build spilled while both were held through it.)
    const bool pickedCell = FL.pick.x > 0.5 && terrainMapped && TerrainNative() && !terrain.clutter &&
                            NativeCell(terrain.uv[0]).x + 1u == uint(FL.pick.x + 0.5);
    const bool pebbleAlbedo = terrainMapped && terrain.pebble;
    if ((C.flags.x & F_PERPIXEL) != 0u) {
        // Interpolated normals shrink between vertices; restore the length the vertex path lights with
        // (1 with NORMALIZENORMALS, otherwise whatever the world matrix made of the vertex normal).
        vec3 n = vNormalW.xyz;
        float len2 = dot(n, n);
        n = len2 > 0.0 ? n * (vNormalW.w * inversesqrt(len2)) : vec3(0.0);
        // The ground's lighting pass draws the lightmap; its relief comes from the base pass's texture (F_BUMPBASE):
        // that texture's normal map, else its generated normals - laid by terrain.glsl, with the cliffs' rock's on
        // their faces (then a base without a normal map of its own has the flat one, and no relief from above).
        // (Not the native ground's pebbles: a few pixels across, a relief there is none to see.)
        const bool terrainRelief = terrainMapped && D.terrainAt.w > 1.5 && !terrain.pebble &&
                                   ((C.flags.x & F_NORMALMAP) != 0u || terrain.rock || TerrainNativeMaps() ||
                                    ground.sand > 0.0);
        if (((C.flags.x & (F_NORMALMAP | F_BUMP)) != 0u || terrainRelief) && len2 > 0.0) {
            vec3 unbumped = n;
            bool base = (C.flags.x & F_BUMPBASE) != 0u || terrainRelief;
            if (terrainRelief) {
                const float top = (C.flags.x & F_NORMALMAP) != 0u ? C.misc.w : TerrainNativeMaps() ? FL.terrain.w : 0.0;
                const uint face = terrain.rock ? TerrainRockImage(1) : D.texIdx.w;
                n = TerrainNormalMap(n, texImages[D.texIdx.w], top, texImages[face], terrain.rock ? FL.terrainRock2.w : top,
                                     terrain);
                // (On sand the tile's own relief less, by RVK_RippleSoft: under the ripples its lumps were noise
                // between the crests.)
                if (ground.sand > 0.0) n = mix(n, unbumped, 0.83 * clamp(FL.sand2.x, 0.0, 1.0) * clamp(ground.sand, 0.0, 1.0));
                n = SandRippleNormal(n, ground, terrain.uv[0]);   // (the sand's ripples: parallax.glsl)
            } else if ((C.flags.x & F_NORMALMAP) != 0u && gParallax)
                n = NormalMapNormalGrad(n, vPosW, gUv0, gUv0Dx, gUv0Dy);
            else if ((C.flags.x & F_NORMALMAP) != 0u)
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
                // (In a shadow the sun's relief mostly goes: the sky lights it. Sunlit ripples inside a shadow read
                // as no shadow at all.)
                if (gSunVisibilityValid)
                    gLightmapRelief = mix(1.0, gLightmapRelief, 0.3 + 0.7 * min(gCasterVisibility, gGroundAway));
            }
        }
        if (len2 > 0.0) gShadeNormal = normalize(n);
        // PBR material: its maps, and the specular lobe the lights below use (lighting.glsl gPbr*). The ground's
        // lighting pass takes its chunk's base texture's material (MAT_BASE), read with the base's coordinates.
        gPbrOn = (D.mat.z & MAT_PBR) != 0u && FL.pbr2.y > 0.5 && (C.flags.x & F_SHADOWCHEAP) == 0u;
        if (gPbrOn) {
            gPbrBase = (D.mat.z & MAT_BASE) != 0u;
            float ormCover = 1.0;
            vec3 orm = gPbrBase && terrainMapped && TerrainNative()
                           ? NativeOrm(terrain, ormCover)
                           : gPbrBase && terrainMapped
                           ? TerrainTexture(texImages[D.mat.x], bindlessSamplers[D.mat.y],
                                            texImages[terrain.rock ? TerrainRockImage(2) : D.mat.x], terrain, vec4(1.0)).rgb
                           : gParallax ? textureGrad(ORMTEX, gUv0, gUv0Dx, gUv0Dy).rgb
                           : texture(ORMTEX, gPbrBase ? vSet0 : vTex0.xy).rgb;
            // (A material blended in where it is only partly: none at all - the lighting as without.)
            orm = mix(vec3(1.0, 1.0, 0.0), orm, ormCover);
            gPbrOn = ormCover > 0.0;
            gPbrAo = mix(1.0, orm.r, clamp(FL.pbr.w, 0.0, 1.0));
            gPbrRough = clamp(orm.g, 0.03, 1.0);
            gPbrMetal = clamp(orm.b, 0.0, 1.0);
            gPbrAlbedo = gPbrBase ? (terrainMapped ? TerrainVariation(terrainBase.rgb, vPosW, terrain) : texture(BUMPTEX, vSet0).rgb)
                                  : Sample(0u).rgb * clamp(vDiffuse.rgb, 0.0, 1.0);
            gPbrNormal = len2 > 0.0 ? normalize(n) : vec3(0.0, 1.0, 0.0);
            // Specular anti-aliasing: how much the normal turns across the pixel widens the highlight (Kaplanyan /
            // Tokuyoshi), so small bright highlights don't sparkle on curved or bumpy surfaces in the distance.
            vec3 dnx = dFdx(gPbrNormal), dny = dFdy(gPbrNormal);
            float a2 = gPbrRough * gPbrRough * gPbrRough * gPbrRough;
            a2 = clamp(a2 + min(0.5 * (dot(dnx, dnx) + dot(dny, dny)), 0.18), 1e-6, 1.0);
            gPbrAlpha = sqrt(a2);
            gPbrF0 = mix(vec3(0.04), gPbrAlbedo, gPbrMetal);
            gPbrSunVisibility = gSunVisibilityValid ? gSunVisibility : 1.0;
            gPbr = true;
        }
        vec3 ambient = vec3(0.0), diff = vec3(0.0), spec = vec3(0.0), diffL = vec3(0.0), specL = vec3(0.0);
        // Sunlight is shadowed: by the receiver's shade, or in the ground's lighting pass by the lightmap's.
        float sunScale = ((C.flags.x & F_SHADOWTEX) != 0u ? texShadeLit * moonShade : shadeSun ? shade : 1.0) * gParallaxSun;
        AccumulateLights(vPosW, n, sunScale, localScale, ambient, diff, spec, diffL, specL);
        // The light rig: lit things take the sky's light the ground's lightmap holds (FL.rigSky: all of it by night,
        // what the sun leaves by day) - before, they had only the global ambient, the ground that and its lightmap, and
        // by night buildings and characters stood at about half the ground's light. By how far they face up: a sky's.
        // (Not the ground itself, nor the sky.)
        if (RigOn() && (C.flags.x & F_SHADOWTEX) == 0u && !terrainMapped && (D.mat.z & MAT_SKY) == 0u && len2 > 0.0)
            ambient += FL.rigSky.rgb * (0.5 + 0.5 * normalize(n).y);
        // PBR: metals have no diffuse light; the occlusion map takes ambient light away.
        float kd = gPbrOn ? 1.0 - gPbrMetal : 1.0, kAmb = gPbrOn ? kd * gPbrAo : 1.0;
        // The ground's lighting pass (F_SHADOWTEX): the shadow takes the global ambient and emissive part along
        // with the lightmap, leaving only the lights' own contribution: (lightmap + ambient) * shadow + lights.
        vec3 base = (vMatEmissive + vMatAmbient * C.ambient.rgb * kAmb) * texShadeLit;
        vec3 lit = base + vMatAmbient * ambient * kAmb + vDiffuse.rgb * diff * kd, litSpec = vSpecular.rgb * spec;
        vec3 local = vDiffuse.rgb * diffL * kd, localSpec = vSpecular.rgb * specL;
        if (gPbrOn) {
            vec3 V = normalize(C.eyePos.xyz - vPosW);
            gPbrSpec = (spec + specL) * FL.pbr.x;
            // The ground's sunlight is baked into its lightmap, so the sun isn't among its lights: its highlight here.
            // Shadowed fully (the shadow strength keeps some baked light in the lightmap's shade; a highlight has none
            // to keep), gone where the lightmap is dark (the game's baked shade of buildings - also those beyond the
            // shadow cascades, whose shadows a low sun makes long), and fading as the sun nears the horizon.
            if (gPbrBase && dot(FL.sunColor.rgb, FL.sunColor.rgb) > 0.0 && dot(FL.sunDir.xyz, FL.sunDir.xyz) > 0.0) {
                vec3 Ls = -normalize(FL.sunDir.xyz);
                float sunVis = (gSunVisibilityValid ? mix(1.0, gSunVisibility, CasterFade()) : 1.0) * TerrainSun(vPosW);
                float horizon = smoothstep(0.03, 0.25, Ls.y);
                float baked = smoothstep(0.2, 0.55, dot(Sample(0u).rgb, vec3(0.3, 0.59, 0.11)));
                gPbrSpec += FL.sunColor.rgb * PbrSpecular(gPbrNormal, Ls, V) * (sunVis * horizon * baked) * FL.pbr.x;
            }
            gPbrSpec += PbrAmbientSpecular(gPbrNormal, V, vMatAmbient * (C.ambient.rgb + ambient)) * FL.pbr.y;
            litSpec = localSpec = vec3(0.0);     // the game's own specular is replaced
            // The ground's lightmap (its baked sun and ambient): less of it on metal, less in occluded creases.
            if (gPbrBase) gLightmapRelief *= kd * mix(1.0, gPbrAo, 0.6);
        }
        // The direct sunlight's share of the colour: what the contact shadows may take away. The ground's sunlight
        // is baked into its lightmap: about the lit side's part of it.
        if (dot(FL.sunColor.rgb, FL.sunColor.rgb) > 0.0 && len2 > 0.0) {
            float ndl = max(dot(normalize(n), -normalize(FL.sunDir.xyz)), 0.0);
            if ((C.flags.x & F_SHADOWTEX) != 0u) {
                gSunShare = 0.6 * ndl * texShadeLit * (1.0 - FL.windFrame.z) * (1.0 - kStormSunTaken * FL.rainNear.y);
            } else {
                const vec3 kLuma = vec3(0.3, 0.59, 0.11);
                float sunPart = dot(vDiffuse.rgb * FL.sunColor.rgb, kLuma) * ndl * sunScale;
                gSunShare = clamp(sunPart * (1.0 - FL.windFrame.z) * (1.0 - kStormSunTaken * FL.rainNear.y) / max(dot(lit + local, kLuma), 1e-3), 0.0, 1.0);
            }
        }
        // RVK_CrownSky: a crown's light rolls off towards 1 from 0.6 rather than stopping dead there: the game's ambient
        // (0.46 in a city) and the sun went past 1 on every leaf facing the sun more than halfway, and the clamp made the
        // whole lit side of the crown one flat light (a noon dump: its leaves' light at 1.04 and below, little between).
        if (D.leaf.w > 0.0 && FL.foliage.x > 0.0) {
            vec3 over = max(lit - 0.6, vec3(0.0));
            lit = mix(lit, min(lit, 0.6) + 0.4 * (1.0 - exp(-over / 0.4)), FL.foliage.x);
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
#ifdef RVK_NO_GROUND
    t0 = Sample(0u);
#endif
    vec4 t1 = C.stageA[0].x != 1u ? Sample(1u) : vec4(0.0);
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
    if (terrainMapped && D.terrainAt.w > 1.5) t0.rgb *= NativeSky(vPosW);   // (the native ground's lightmap: less sky, darker)
    vec4 current = Cascade(t0, t1, 1.0);
    // A low sun's light (rvk sky.cpp, RVK_DawnLight): the ground's lightmap holds the sun's and the sky's light
    // together - warm where the sun reaches, cool where the ground faces away or lies in shade. (The ground's light
    // pass by F_NOALBEDO: with the sun's shadows off too.)
    if ((C.flags.x & F_NOALBEDO) != 0u && FL.dawn[0].w > 0.0)
        current.rgb *= DawnTint(vNormalW.xyz, texShade);
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
    if (gPbrOn) {
        // The ground's lighting pass multiplies its base pass (about its albedo): divided by it, it adds the highlight.
        vec3 s = gPbrBase ? gPbrSpec / max(gPbrAlbedo, vec3(0.04)) : gPbrSpec;
        current.rgb += s;
        if ((C.flags.x & F_HDR) == 0u) current.rgb = min(current.rgb, vec3(1.0));
    }
    // The sand's glints: past the stages' clip, divided by the sand's colour as the highlights above - this pass
    // multiplies the base pass's - so they shine white, not the sand's orange. Only where the sun itself reaches: a
    // glint is the sun's image, and a shadow's shade keeps the sky's share of the light - the glints in it had stayed
    // a third as bright, many times the sand's own.
    // (... and as much as the clouds let the sun through - a glint needs the sun's disc: under a storm's deck, none.)
    const float glintSun = (gSunVisibilityValid ? gCasterVisibility * gGroundAway * gTerrainVisibility * gCloudVisibility
                                                : 1.0) *
                           FL.sand3.y * (1.0 - FL.rainNear.y);
    if (glintSun > 0.0 && any(greaterThan(sandGlint, vec3(0.0)))) {
        current.rgb += sandGlint * glintSun / max(sandMean, vec3(0.08));
        if ((C.flags.x & F_HDR) == 0u) current.rgb = min(current.rgb, vec3(1.0));
    }
    if (!shadeSun)
        // (By night the moon's: unlit, half of it - and a storm's dimming kept, as FlatShade has it by day: without,
        // the stormy land brightened the moment the moon took over at dusk.)
        // (With the rig, FlatShade is the one formula day and night - by night the moon's shadow half again on it.)
        current.rgb *= RigOn() ? flatShade * (FL.moon.w > 0.5 ? mix(1.0, shade, 0.5) : 1.0)
                     : FL.moon.w > 0.5 ? mix(1.0, shade, 0.5) * (1.0 - kCloudSunShare * FL.rainNear.y) * (1.0 - 0.5 * FL.rainNear.y)
                                       : flatShade;
    // Sunlight through leaves: where the sun is behind a leaf (seen from the camera), its light comes through tinted
    // by the leaf, most looking into the sun; other leaves' shadows still block it. Only textures with holes around
    // this point (their coarse mip's alpha) are leaves - not walls drawn the same way.
    bool sun = dot(FL.sunColor.rgb, FL.sunColor.rgb) > 0.0 && dot(FL.sunDir.xyz, FL.sunDir.xyz) > 0.0;
    if ((C.flags.x & F_FOLIAGE) != 0u && sun && FL.effects.x > 0.0 && dot(vNormalW.xyz, vNormalW.xyz) > 0.0) {
        // (A crown the leaves know - D.leaf - is a tree's whatever its texture: a dense canopy's coarse mip is nearly
        // solid, and the test let next to none of its light through - user, 2026-10-21: RVK_LeafLight did nothing.)
        float holes = D.leaf.w != 0.0 ? 1.0 : smoothstep(0.97, 0.75, NeighbourhoodAlpha(vTex0.xy));
        if (holes > 0.0) {
            vec3 L = -normalize(FL.sunDir.xyz), toEye = normalize(C.eyePos.xyz - vPosW);
            vec3 nf = normalize(vNormalW.xyz);
            if (dot(nf, toEye) < 0.0) nf = -nf;                  // the side the camera sees
            float through = max(-dot(nf, L), 0.0), glare = pow(max(dot(-toEye, L), 0.0), 4.0);
            if (through + glare > 0.0) {
                // The sun's visibility on the lit side: the shadow already computed for the surface (a second,
                // opposite-side search only darkened back-lit leaves and cost another few taps).
                float visible = FL.shadowParams.x <= 0.5 ? 1.0 : gSunVisibilityValid ? mix(1.0, gSunVisibility, CasterFade()) : 1.0;
                // RVK_CrownDepth: rather by how much crown the light crossed to get here (the surface's own shadow
                // let only the outermost leaves facing the sun glow), and deeper in colour - light through a leaf is
                // its colour twice over (its colour squared, over its brightness: about as bright).
                vec3 tint = t0.rgb;
                if (FL.foliage.y > 0.0) {
                    float t = FL.shadowParams.x > 0.5 ? CrownThrough(vPosW) : -1.0;
                    if (t >= 0.0) visible = mix(visible, mix(1.0, t, CasterFade()), FL.foliage.y);
                    tint = mix(tint, tint * tint / max(dot(tint, vec3(0.3, 0.59, 0.11)), 0.04), FL.foliage.y);
                }
                visible *= TerrainSun(vPosW);    // (low sun: leaves under the mountains don't glow)
                current.rgb += tint * FL.sunColor.rgb * ((0.5 * through + 0.7 * glare) * visible * holes * FL.effects.x);
            }
        }
    }
    // One glow per surface. On a surface the game lights itself (F_EMISSIVE: drawn unlit, or an emissive material -
    // signs, windows, screens) the night glow alone: by night its bright texels shine beyond white, for the bloom
    // (FL.effects.y, RVK_NightGlow), and an emissive map, when it has one, only says where - in place of "bright" -
    // adding no light of its own: the game shows such a surface at its full colour already. (Both added on top of it
    // made a lit sign ~3x its colour and bloomed its letters away - user, 2026-10-20.) Elsewhere the emissive map
    // (D.mat.z & MAT_EMISSIVE) adds the light the surface gives off, whatever lights it, x RVK_PbrGlow (FL.pbr2.z) -
    // beyond white with HDR, for the bloom.
    const bool selfLit = (C.flags.x & F_EMISSIVE) != 0u, hasMap = (D.mat.z & MAT_EMISSIVE) != 0u;
    vec3 emitted = vec3(0.0), map = vec3(0.0);
    if (hasMap && (selfLit ? FL.effects.y > 0.0 : FL.pbr2.z > 0.0))
        map = (gParallax ? textureGrad(EMISSIVETEX, gUv0, gUv0Dx, gUv0Dy) : texture(EMISSIVETEX, vTex0.xy)).rgb;
    if (selfLit && FL.effects.y > 0.0) {
        float glow = (hasMap ? max(map.r, max(map.g, map.b)) : smoothstep(0.55, 0.9, max(t0.r, max(t0.g, t0.b)))) *
                     FL.effects.y;
        vec3 before = current.rgb;
        if ((C.flags.x & F_LIGHTING) != 0u) current.rgb += t0.rgb * vMatEmissive * glow;
        else current.rgb *= 1.0 + glow;
        if (hasMap) emitted = current.rgb - before;   // (the debug views: what the map made glow)
    } else if (hasMap && !selfLit && FL.pbr2.z > 0.0) {
        emitted = map * FL.pbr2.z;
        current.rgb += emitted;
        if ((C.flags.x & F_HDR) == 0u) current.rgb = min(current.rgb, vec3(1.0));
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
    // The wet ground (rvk rain.cpp: FL.wet): soaked surfaces darker and reflecting, puddles on the flat in hollows -
    // not where a roof or a canopy keeps the rain off (the rain's cover map).
    float wetFlag = uintBitsToFloat(C.vtx.w);
    float gWetLook = 0.0, gPuddle = 0.0, gWetUp = 0.0;
    if (wetFlag >= 1.5 && FL.wet.x > 0.0) {
        vec2 cov = RainCoverAt(vPosW);
        float open = 1.0 - cov.x;
        if (open > 0.0) {
            // (The surface's facing: its own smooth normal; else from its shape - many of the game's are drawn unlit,
            // without normals - each triangle flat then.)
            vec3 n = normalize(cross(dFdx(vPosW), dFdy(vPosW)));
            if (dot(n, C.eyePos.xyz - vPosW) < 0.0) n = -n;
            if (dot(vNormalW.xyz, vNormalW.xyz) > 0.0) n = normalize(vNormalW.xyz) * (dot(vNormalW.xyz, n) < 0.0 ? -1.0 : 1.0);
            gWetUp = smoothstep(0.2, 0.85, n.y);
            gWetLook = FL.wet.x * open * mix(0.35, 1.0, gWetUp);
            // Puddles: on level ground, where it lies in a hollow (the map's) and the noise says, spreading as they fill.
            float site = clamp(cov.y * 1.2 + WetNoise(vPosW.xz / 4.5) * 0.7 + WetNoise(vPosW.xz / 1.6 + 7.3) * 0.3 - 0.25, 0.0, 1.0);
            float fill = FL.wet.y;
            gPuddle = open * smoothstep(0.8, 0.95, n.y) * smoothstep(1.0 - 0.6 * fill, 1.12 - 0.6 * fill, site) * min(fill * 3.0, 1.0);
            current.rgb *= (1.0 - 0.35 * gWetLook) * (1.0 - 0.25 * gPuddle);   // (soaked: darker; under water: darker still)
            // The sky's sheen on it (Fresnel: the more the flatter it is seen) and the sun's (or a flash's) glint - its
            // own, so the wet shows without the screen-space reflections (the user plays with them off); with them on,
            // lighter (they reflect the scene over it).
            vec3 V = normalize(C.eyePos.xyz - vPosW);
            float fres = 0.02 + 0.98 * pow(1.0 - max(dot(n, V), 0.0), 5.0);
            float own = FL.rainNear.w > 0.5 ? 0.35 : 1.0;
            vec3 Rw = reflect(-V, n);
            vec3 sky = WetReflection(Rw, gPuddle > 0.5 ? 0 : 1);
            if (FL.flash.w > 0.0) sky += FL.flash.rgb * 0.6;
            float sheen = gWetLook * gWetUp * (0.06 + 0.55 * fres) + gPuddle * (0.12 + 0.85 * fres);
            current.rgb = mix(current.rgb, sky, clamp(sheen * own, 0.0, 0.9));
            vec3 L = -normalize(FL.sunDir.xyz + vec3(0.0, -1e-6, 0.0));
            float glint = pow(max(dot(reflect(-V, n), L), 0.0), mix(40.0, 400.0, gPuddle));
            current.rgb += FL.sunColor.rgb * (glint * (0.4 * gWetLook * gWetUp + 2.0 * gPuddle) * (0.3 + fres));
            // Rain on the puddles: rings spreading, glinting with the sky's light.
            if (gPuddle > 0.0 && FL.rainNear.y + FL.wet.x > 0.0)
                current.rgb += C.fogColor.rgb * (0.25 * gPuddle * WetRipples(vPosW.xz, FL.wet.z) * min(FL.wet.x * 2.0, 1.0));
#ifdef RVK_GLOW
            albedo *= (1.0 - 0.35 * gWetLook) * (1.0 - 0.4 * gPuddle);
#endif
        }
    }
#ifdef RVK_GLOW
    // How much the surface reflects (screen-space reflections): the draw's own, or its wet look where it faces up.
    float reflectivity = uintBitsToFloat(C.vtx.z), wet = uintBitsToFloat(C.vtx.w);
    if (wet >= 1.5) wet -= 2.0;                    // (the draw may get wet: above)
    if (wet > 0.0 && dot(vNormalW.xyz, vNormalW.xyz) > 0.0)
        reflectivity = max(reflectivity, wet * smoothstep(0.7, 0.95, normalize(vNormalW.xyz).y));
    reflectivity = max(reflectivity, max(0.3 * gWetLook * gWetUp, 0.85 * gPuddle));   // (the rain's wet ground, puddles)
    // PBR: smooth surfaces reflect the scene (metals most); rough ones only their ambient environment.
    if (gPbrOn)
        reflectivity = max(reflectivity, FL.pbr.z * (1.0 - smoothstep(0.1, 0.55, gPbrRough)) * mix(0.12, 0.9, gPbrMetal));
    // ... and their albedo for indirect light is the diffuse part: none on metal, less in occluded creases.
    if (gPbrOn && !gPbrBase)
        albedo *= (1.0 - gPbrMetal) * gPbrAo;
#endif
    // The game's cloud sheets: coloured as the cloud layer's clouds, hazed by its air (not the game's fog).
    // (Strength 0 - a clear sky's (rvk sky.cpp) - they vanish; not drawn as the game draws them.)
    bool cloudSheet = (D.mat.z & MAT_CLOUDSHEET) != 0u;
    if (cloudSheet) {
        current.rgb = CloudSheet(Sample(0u));
        // (None below the horizon: the game puts some sheets under the eye, far off - behind the far hills, a lit
        // strip under the horizon all round with a hard top edge, brighter than the night sky: user's dump.)
        float above = smoothstep(-0.004, 0.012, normalize(vPosW - C.eyePos.xyz).y);
        current.a = clamp(current.a * FL.cloudSky.w * above, 0.0, 1.0);
    }
    float fogF = 1.0;                                     // (how much of the surface shows through the fog)
    if ((C.flags.x & F_FOG) != 0u && !cloudSheet) {
        float f = vFogFactor;
        uint table = C.flags.z;
        if (table != 0u) {
            float d = vFogDist;
            if (table == 3u) f = clamp((C.fogParams.y - d) / max(C.fogParams.y - C.fogParams.x, 1e-6), 0.0, 1.0);
            else if (table == 1u) f = clamp(exp(-C.fogParams.z * d), 0.0, 1.0);
            else { float x = C.fogParams.z * d; f = clamp(exp(-x * x), 0.0, 1.0); }
        }
        // (Our own sky - C.fogParams.w: its horizon towards the pixel, rvk draw.cpp.)
        vec3 fogC = C.fogParams.w > 0.5 ? SkyHorizonRing(normalize(vPosW - C.eyePos.xyz), FL.skyRing) : C.fogColor.rgb;
        current.rgb = mix(fogC, current.rgb, f);
        fogF = f;
#ifdef RVK_GLOW
        albedo *= f;                                      // indirect light fades into the fog with the surface
        reflectivity *= f;                                // ... and so do reflections
#endif
    }
    if (groundOne) {
        // (The base pass's colour, fogged as that pass fogs - the game's fog in a storm's air, or our sky's horizon -
        // times this pass's light, fogged to white: what the multiplying blend made of the two. Its albedo too, the
        // base pass's - this pass's keeps it.)
        const float fb = (oneSlots.z & (1u << 25)) != 0u ? fogF : 1.0;
        const vec3 baseFog = (oneSlots.z & (1u << 24)) != 0u
            ? SkyHorizonRing(normalize(vPosW - C.eyePos.xyz), FL.skyRing)
            : vec3(float((oneSlots.z >> 16) & 255u), float((oneSlots.z >> 8) & 255u), float(oneSlots.z & 255u)) / 255.0;
        current.rgb *= mix(baseFog, oneBase, fb);
#ifdef RVK_GLOW
        albedo = oneBase * fb;
#endif
    }
    if ((C.flags.x & F_DEBUGLIGHT) != 0u && (C.vtx.x & 0xEu) != 4u) {
        // Blue: lighting off. Red: lit, but no point / spot light reaches the draw. Green: lit by a point light.
        vec3 tint = (C.flags.x & F_LIGHTING) == 0u ? vec3(0.1, 0.3, 1.0)
                  : C.lightInfo.y == 0u && ((C.flags.x & F_LIGHTOVERRIDE) == 0u || FL.info.x == 0u) ? vec3(1.0, 0.15, 0.1)
                  : vec3(0.1, 1.0, 0.2);
        current.rgb = mix(current.rgb, tint, 0.45);
    }
    // Texture picking (Ctrl+Shift+I): the picked texture's draws in yellow for a few seconds.
    // (The ground drawn natively: the cells laid with the picked tile.)
    if ((D.mat.z & MAT_PICKED) != 0u || pickedCell)
        current.rgb = mix(current.rgb, vec3(1.0, 0.85, 0.1) * max(dot(current.rgb, vec3(0.3, 0.59, 0.11)), 0.35) * 2.0, 0.7);
    // PBR debug views (RVK_PbrDebug). 7: the scene, tinted where surfaces have maps - green a PBR material (lit with
    // it), blue only a normal map, magenta an albedo map (any draw), orange where an emissive map gives off light.
    // 8: only the emitted light (everything else dark grey). 9: what PBR surfaces reflect (the environment probe
    // or, where it hasn't seen, the analytic sky), at their roughness. 10: two-pass foliage's soft edges (magenta) and
    // cores (green). 11: the frame lights tested and reaching per pixel.
    uint pbrDebug = uint(FL.pbr2.x + 0.5);
    if (pbrDebug == 7u && (C.vtx.x & 0xEu) != 4u) {
        vec3 tint = gPbrOn ? vec3(0.1, 1.0, 0.2) : (C.flags.x & F_NORMALMAP) != 0u ? vec3(0.15, 0.4, 1.0)
                  : (D.mat.z & MAT_ALBEDO) != 0u ? vec3(1.0, 0.15, 0.9) : vec3(-1.0);
        if (dot(emitted, emitted) > 1e-4)
            tint = vec3(1.0, 0.5, 0.05);
        if (tint.x >= 0.0)
            current.rgb = mix(current.rgb, tint * max(dot(current.rgb, vec3(0.3, 0.59, 0.11)), 0.25) * 1.6, 0.6);
    } else if (pbrDebug == 11u && (C.flags.x & F_LIGHTING) != 0u) {
        // 11: the frame lights per pixel (RVK_LightOver) - red how many the loop tests, green how many reach the
        // pixel, each / 16 (lighting.glsl): red over green is work a tighter light list would save.
        current.rgb = vec3(float(gLightsTested), float(gLightsReaching), 0.0) / 16.0;
    } else if (pbrDebug == 10u) {
        // 10: two-pass foliage (RVK_FolEdges) - its soft edges, drawn later over the scene, magenta; its cores green;
        // far plants dithered in one pass (RVK_FolDither) cyan.
        uint split = uint(D.motion.w + 0.5);
        if (split == 2u) current.rgb = vec3(1.0, 0.0, 1.0);
        else if (split == 1u) current.rgb = mix(current.rgb, vec3(0.1, 1.0, 0.2), 0.5);
        else if (split == 3u) current.rgb = mix(current.rgb, vec3(0.1, 0.8, 1.0), 0.5);   // dithered: cyan
    } else if (pbrDebug == 9u && (C.vtx.x & 0xEu) != 4u) {
        current.rgb = gPbrOn ? gPbrEnv : vec3(dot(current.rgb, vec3(0.3, 0.59, 0.11)) * 0.15);
    } else if (pbrDebug == 8u && (C.vtx.x & 0xEu) != 4u) {
        current.rgb = emitted + vec3(dot(current.rgb - emitted, vec3(0.3, 0.59, 0.11)) * 0.15);
    } else if (pbrDebug != 0u && (C.flags.x & F_LIGHTING) != 0u && (C.vtx.x & 0xEu) != 4u) {
        vec3 v = pbrDebug == 1u ? gPbrAlbedo : pbrDebug == 2u ? vec3(gPbrRough) : pbrDebug == 3u ? vec3(gPbrMetal)
               : pbrDebug == 4u ? vec3(gPbrAo) : pbrDebug == 5u ? gPbrNormal * 0.5 + 0.5 : gPbrSpec;
        current.rgb = gPbrOn ? v : vec3(dot(current.rgb, vec3(0.3, 0.59, 0.11)) * 0.25);
    }
    current.a *= leafMask;
#ifndef RVK_NO_CUTOUT
    if ((C.flags.x & F_ALPHATEST) != 0u && !AlphaPass(current.a))
        discard;
    if ((C.flags.x & F_CUTOUT) != 0u && current.a < vCutout)
        discard;
#endif
    if (uint(D.motion.w + 0.5) == 3u)
        current.a = 1.0;                                  // dithered foliage: what is kept covers (FoliageDitherCut)
    // An additive effect adds what it adds to the scene to the glow too, for the bloom.
#ifdef RVK_GLOW
    vec3 glow = vec3(0.0);
    if ((C.flags.x & F_GLOW) != 0u)
        glow = current.rgb * ((C.flags.x & F_GLOWALPHA) != 0u ? clamp(current.a, 0.0, 1.0) : 1.0) * C.misc.z;
    // Alpha: its brightness over its view depth (1 / w), summed like the colour - the bloom's light's distance.
    outGlow = vec4(glow, dot(glow, vec3(0.3, 0.59, 0.11)) * gl_FragCoord.w);
    // A solid surface (F_GLOWALPHA alone): blended to scale the glow there by 1 - its coverage - an effect drawn
    // before it behind it no longer glows through it.
    if ((C.flags.x & (F_GLOW | F_GLOWALPHA)) == F_GLOWALPHA)
        outGlow = vec4(0.0, 0.0, 0.0, (C.flags.x & F_BLENDED) != 0u ? clamp(current.a, 0.0, 1.0) : 1.0);
    // Blended with this fragment's alpha like the colour (attachment 2's blend state follows the colour's).
    outLocal = vec4(gLocalFraction, clamp(reflectivity, 0.0, 1.0), gSunShare, current.a);
    // A shop's room drawn into its doorway (D.clip): lit by its own lamps, none of it by the street's sun - the passes
    // after the scene (traced sun shadows, ambient occlusion) leave it as it is - and no screen-space reflections of the
    // street. Written whole whatever its alpha (blended by it, the mark was only part there, and the traced occlusion
    // from the street's building around the doorway darkened a patch of its floor).
    if (dot(D.clip.xyz, D.clip.xyz) > 0.0)
        outLocal = vec4(1.0, 0.0, 0.0, 1.0);
    // Motion vectors (written by depth-writing draws only, see the blend state): where this point was last frame.
    vec2 now = vClip.xy / vClip.w, before = vPrevClip.xy / max(vPrevClip.w, 1e-6);
    outMotion = vec4(vPrevClip.w > 1e-6 ? (now - before) * 0.5 * C.viewport.zw * FL.screen.xy * vec2(1.0, -1.0) : vec2(0.0),
                     0.0, 1.0);   // (scene pixels: the game's viewport is in back-buffer pixels)
    // PBR surfaces give the screen-space reflections their shading normal (view space, octahedral; w 2 .. 4 marks
    // it): the depth buffer's normals are the triangles' own, which mirror faceted on curved low-poly meshes. Other
    // surfaces with a normal give theirs too, for AMD's effects (w 6 .. 8; ssr.frag keeps the depth buffer's there):
    // the per-pixel one, else the vertices'; facing the camera (two-sided leaves, the inside of a cloak). A character's
    // 8 higher again (w 10 .. 12, 14 .. 16): brix_pack.frag steadies AMD's indirect light on characters with it.
    const float marked = (C.flags.x & F_CHARACTER) != 0u ? 8.0 : 0.0;
    if (gPbrOn && reflectivity > 0.0) {
        outMotion.zw = OctNormal((C.view * vec4(gPbrNormal, 0.0)).xyz) + vec2(0.0, 3.0 + marked);
    } else if (FL.screen.z > 0.5) {
        vec3 n = dot(gShadeNormal, gShadeNormal) > 0.0 ? gShadeNormal : vNormalW.xyz;
        if (dot(n, n) > 0.0) {
            if (dot(n, C.eyePos.xyz - vPosW) < 0.0) n = -n;
            outMotion.zw = OctNormal((C.view * vec4(n, 0.0)).xyz) + vec2(0.0, 7.0 + marked);
        }
    }
    // Blended with this fragment's alpha like the colour (kept by multiplying and additive passes, see the blend state).
    // (Its alpha read by the indirect light only: the native ground's pebbles mark themselves 0.5 there - gi.frag.)
    outAlbedo = vec4(clamp(albedo, 0.0, 1.0), pebbleAlbedo ? 0.5 : current.a);
    // The game's blob shadow (drawn: indoors, no sun shadows to replace it): a shade laid on the surface - its colour
    // darkened, nothing else of it changed (blended by alpha 0: kept). Its own lamp share, surface colour and glow
    // stamped the floor round every table and character with patches the later passes lit wrongly - coloured, hard-
    // edged, changing - user, 2026-10-19.
    if ((C.flags.w & 1024u) != 0u) {
        outGlow = vec4(0.0);                     // (all zero: kept whether a target blends by alpha or premultiplied)
        outLocal = vec4(0.0);
        outAlbedo = vec4(0.0);
    }
#endif
    if ((C.flags.x & F_OVERBRIGHT2X) != 0u && !groundOne)
        current.rgb *= 0.5;                               // blended as dst * src * 2
    // An AI mission's PF map tile (RVK_AlienMap, draw.cpp): the space that isn't a room, drawn green - the rooms black.
    if ((C.flags.w & 512u) != 0u)
        current = vec4(0.10, 0.32, 0.14, 1.0);
    if ((C.flags.w & 256u) != 0u)
        current.a = 1.0;                                  // the interface layer: a replacing draw covers fully
    // RVK_ShadowView (debug): the sun shadow map's say at each point - white lit, dark shadowed by things - tinted by
    // the cascade it came from (red nearest, green, blue, yellow widest); grey where no shadow was looked up.
    if (FL.terrainSun2.w > 0.5 && (C.flags.w & 256u) == 0u) {
        const vec3 tints[4] = vec3[4](vec3(1.0, 0.45, 0.45), vec3(0.45, 1.0, 0.45), vec3(0.5, 0.6, 1.0), vec3(1.0, 1.0, 0.4));
        vec3 tint = gShadowCascade >= 0 ? tints[min(gShadowCascade, 3)] : vec3(0.5);
        float vis = gSunVisibilityValid ? gCasterVisibility : 0.6;
        current.rgb = tint * mix(0.08, 1.0, vis);
    }
    // Never a negative or not-a-number colour into the scene: one such pixel (a degenerate normal, a zero-length
    // vector somewhere in the lighting) is invisible alone, but the indirect light gathers it over its reach and
    // the temporal anti-aliasing spreads it - dark and coloured blobs jumping about the floor round it (the subway,
    // user 2026-10-19: in its tiles' grooves). Black instead, that pixel only.
    if (any(isnan(current)) || any(isinf(current))) current = vec4(0.0, 0.0, 0.0, any(isnan(current.aaaa)) ? 1.0 : current.a);
    current.rgb = max(current.rgb, vec3(0.0));
    outColor = current;
}
#endif
#endif   // RVK_LITE
