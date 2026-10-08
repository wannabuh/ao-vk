#version 450
// Procedural ground grass (RVK_GrassOn, rvk/grass.cpp). Each blade is one 48-byte record in the blade pool; this shader
// expands it into a strip - vertex v of blade b is gl_VertexIndex = 8 b + v (the draw's vertexOffset places the tile's
// blades in the pool): cross section v >> 1 from the root (0) to the tip (3), edge v & 1 - shaped by its kind (a grass
// blade, a broad blade, a seed stalk, a flower), bends it with the wind, the gusts and the characters walking through
// (and the trails they leave), thins and fades the field out towards its edge and lights it. The lighting is the
// terrain's own (its light pass: the baked lightmap plus the global ambient, darkened by the sun's shadow, plus the
// frame's local lights), so a field is as bright as the ground it stands on by day and by night; on top of it the sun
// shades the rounded blade, shines through it when the camera looks towards the sun and glints along it. Per vertex: a
// blade is a few pixels wide, and the fragment shader is then nearly free, which is what makes the field's overdraw
// cheap.
#include "frame_lights.glsl"                    // FL (binding 4): the sun, its cascades, the lights, the pushers
#include "grass_common.glsl"                    // the blade record, its shape, the wind

layout(set = 0, binding = 0) uniform GrassFrame {
    mat4 viewProj;      // world -> clip (D3D row-vector convention, as the scene's)
    mat4 prevViewProj;  // ... last frame (motion vectors); equals viewProj when the camera didn't move
    vec4 viewport;      // xy: target size in pixels; z: the field radius; w: the wind clock last frame
    vec4 wind;          // x: time (s); yz: the wind's direction; w: strength
    vec4 camera;        // xyz: the camera; w: 1 = apply the TAA jitter
    vec4 ambient;       // rgb: the terrain's global ambient this frame; w: 1 = captured
    vec4 look;          // x: brightness; y: local light scale; z: glow (backlight, sheen, rounded shading); w: gusts
    vec4 sunColour;     // rgb: the directional light the terrain's light pass takes (0 under the light override)
    vec4 sunDir;        // xyz: the direction it travels
    vec4 lod;           // x: pixels per world unit at view depth 1; y: the least width a blade is drawn at (pixels)
    vec4 trail;         // xy: the trail window's first cell (world cells); z: its cell size; w: cells a side (0 = none)
    vec4 trail2;        // x: the trodden grass's lawn-stripe shading (RVK_TrailShade)
} GF;

layout(set = 0, binding = 2, std430) readonly buffer Blades { Blade blades[]; };
// The trail grid (grass.cpp UpdateGrassTrail): per cell the push left behind, snorm16 x, z (direction x amount).
layout(set = 0, binding = 3, std430) readonly buffer Trails { uint trail[]; };
layout(set = 0, binding = 5) uniform sampler2DArrayShadow shadowMap;            // the sun's cascades
layout(set = 0, binding = 6) uniform samplerCubeArrayShadow pointShadowMaps;     // the point lights' cubes
// Which of the frame's 64 lights reach this tile (grass.cpp: the tile's box against each light's range).
layout(push_constant) uniform GrassPush { uvec2 lightMask; } GP;

layout(location = 0) out vec3 vColour;
layout(location = 1) out float vAcross;     // -1 .. 1 across the blade (the vein)
layout(location = 2) out vec4 vClip;
layout(location = 3) out vec4 vPrevClip;
layout(location = 4) out vec3 vAlbedo;      // the blade's colour unlit (the scene's albedo target: indirect light)
layout(location = 5) out vec2 vShares;      // the local lights' share of the light; the direct sunlight's

// The well-damped spring behind a walking character: a plant springs back a little past upright and settles.
float PushSpring(float age)
{
    float t = max(age - 0.08, 0.0);
    return exp(-6.0 * t) * cos(7.0 * t);         // overshoot ~7%
}

// A blade bending out of the way of the frame's pushers (the characters' feet and the trails behind them, FL.pushers /
// FL.pusherBorn; built by Device::FillPushers, the same the game's plants use), and lying over where the trail grid
// says a character went through a little while ago. h: how far up the blade (0 root, 1 tip), plantHeight its height
// in world units - the top bends most and the base stays.
// layDir / layAmount: which way and how far (0..1) the trail lays it (the lawn-stripe shading).
vec3 PusherOffset(vec3 posW, float h, float plantHeight, vec2 trailPush, out vec2 layDir, out float layAmount)
{
    layDir = vec2(0.0);
    layAmount = 0.0;
    float amount = FL.effects.w;
    if (amount <= 0.0 || h <= 0.0)
        return vec3(0.0);
    uint count = min(FL.info.y, 16u);
    float reach = 0.7 * sqrt(amount);            // the game's plants use 1.4; the grass bends over a smaller radius
    vec2 push = vec2(0.0), rustle = vec2(0.0);
    for (uint i = 0u; i < count; ++i) {
        vec4 p = FL.pushers[i];
        float dy = posW.y - p.y;
        if (dy < -1.0 || dy > 3.0)               // a blade on another floor, or a character on a roof
            continue;
        vec2 d = posW.xz - p.xz;
        float dist = length(d);
        if (dist >= reach)
            continue;
        float near = 1.0 - smoothstep(0.15 * reach, reach, dist);
        vec2 dir = dist > 1e-3 ? d / dist : vec2(0.7071);
        push += dir * near * PushSpring(p.w);
        float born = FL.pusherBorn[i >> 2u][i & 3u];
        float moving = exp(-3.0 * born) * exp(-4.0 * max(p.w - 0.1, 0.0));
        rustle += vec2(-dir.y, dir.x) * near * moving * sin(GF.wind.x * 11.0 + dot(posW.xz, vec2(3.1, 2.3)) + float(i));
    }
    vec2 v = push + 0.12 * rustle;
    float len = length(v), trailLen = length(trailPush);
    if (len <= 1e-4 && trailLen <= 1e-3)
        return vec3(0.0);
    float above = h * plantHeight;               // the vertex's height above the base
    float most = min(0.45 * amount, 0.7 * plantHeight);   // the top's furthest lean
    float live = most * min(len, 1.0);
    // A trail lays the grass nearly flat (trodden down), further than a passing foot, and keeps it down until the last
    // third of its time, then lets it spring back.
    float lay = smoothstep(0.0, 0.35, min(trailLen, 1.0));
    float trodden = min(0.95 * amount, 0.97 * plantHeight) * lay;
    if (trailLen > 1e-3) {
        layDir = trailPush / trailLen;
        layAmount = lay;
    }
    vec2 dirSum = (len > 1e-4 ? v / len * live : vec2(0.0)) + (trailLen > 1e-3 ? trailPush / trailLen * trodden : vec2(0.0));
    float dl = length(dirSum);
    if (dl <= 1e-5)
        return vec3(0.0);
    vec2 dir = dirSum / dl;
    float lean = max(live, trodden) * h * sqrt(h);   // bends most up top
    lean = min(lean, 0.97 * above);
    return vec3(dir.x * lean, -(above - sqrt(max(above * above - lean * lean, 0.0))), dir.y * lean);
}

// The trail grid's push at a world x, z (bilinear; nothing outside the window).
vec2 TrailAt(vec2 xz)
{
    int n = int(GF.trail.w);
    if (n == 0)
        return vec2(0.0);
    vec2 g = xz / GF.trail.z - 0.5;              // cell centres
    vec2 base = floor(g), f = g - base;
    ivec2 b = ivec2(base), o = ivec2(GF.trail.xy);
    vec2 sum = vec2(0.0);
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i) {
            ivec2 c = b + ivec2(i, j);
            if (any(lessThan(c, o)) || any(greaterThanEqual(c, o + n)))
                continue;
            float w = (i == 0 ? 1.0 - f.x : f.x) * (j == 0 ? 1.0 - f.y : f.y);
            sum += w * unpackSnorm2x16(trail[(c.y & (n - 1)) * n + (c.x & (n - 1))]);
        }
    return sum;
}

// Sun visibility at a point: 1 lit, 0 in shadow - the first cascade that covers it, one filtered tap; times the
// grass's own shadow (the map's layer after the cascades, grass_shadow.vert: the nearest cascade's square) at half
// strength - a field shading itself in full would be much darker than the ground it grows on.
const float kGrassShadowLayer = 4.0;            // rvk.h kGrassShadowLayer
float SunShadow(vec3 posW)
{
    int count = int(FL.shadowParams.z);
    for (int c = 0; c < count; ++c) {
        vec4 sc = FL.shadowViewProj[c] * vec4(posW, 1.0);
        vec3 ndc = sc.xyz / sc.w;
        if (max(abs(ndc.x), abs(ndc.y)) >= 1.0 || ndc.z <= 0.0 || ndc.z >= 1.0)
            continue;
        vec2 uv = ndc.xy * 0.5 + 0.5;
        float lit = texture(shadowMap, vec4(uv, float(c), ndc.z));
        if (c == 0)
            lit *= mix(1.0, texture(shadowMap, vec4(uv, kGrassShadowLayer, ndc.z)), 0.4);
        return lit;
    }
    return 1.0;
}

// Visibility of a frame light with a cube shadow map (l.spot.z = cube + 1): 1 = lit; the scene's (ffp_main.glsl), one
// tap. Must match Device::RenderPointShadowMaps.
const float kPointShadowNear = 0.25;
float PointShadow(Light l, vec3 posW, vec3 n)
{
    vec3 d = posW - l.position.xyz;
    float dist = length(d);
    if (dist <= kPointShadowNear) return 1.0;
    float texel = 2.0 * dist / float(textureSize(pointShadowMaps, 0).x);
    float cosAngle = clamp(dot(n, -d / dist), 0.0, 1.0);
    d += n * texel * (1.0 + 2.0 * (1.0 - cosAngle));
    vec3 a = abs(d);
    float w = max(a.x, max(a.y, a.z));
    float f = l.direction.w, nr = kPointShadowNear;
    float ref = f / (f - nr) - f * nr / ((f - nr) * w);
    float layer = l.spot.z - 1.0;
    return 1.0 - (1.0 - texture(pointShadowMaps, vec4(d, layer), ref)) * FL.shadowParams.w * l.spot.w;
}

// The frame's nearby point / spot lights that reach this tile, as the scene lights the ground with them (D3D's
// attenuation, the range cut faded), on the ground's normal.
vec3 LocalLights(vec3 posW, vec3 n)
{
    vec3 sum = vec3(0.0);
    for (uint word = 0u; word < 2u; ++word) {
        uint mask = GP.lightMask[word];
        while (mask != 0u) {
            uint i = word * 32u + uint(findLSB(mask));
            mask &= mask - 1u;
            Light l = FL.lights[i];
            uint type = uint(l.position.w);
            if (type == 3u)
                continue;                        // the sun: in the lightmap, as on the ground
            vec3 d = l.position.xyz - posW;
            float dist = length(d);
            if (dist > l.direction.w)
                continue;
            vec3 L = d / max(dist, 1e-6);
            float denom = l.atten.x + l.atten.y * dist + l.atten.z * dist * dist;
            float att = denom > 0.0 ? 1.0 / denom : 1.0;
            float r = dist / max(l.direction.w, 1e-6);
            float w = clamp(1.0 - r * r * r * r, 0.0, 1.0);
            att *= w * w;
            if (type == 2u) {                    // spot: the cone
                float rho = dot(-L, normalize(l.direction.xyz));
                if (rho <= l.spot.y)
                    att = 0.0;
                else if (rho < l.spot.x)
                    att *= pow(clamp((rho - l.spot.y) / max(l.spot.x - l.spot.y, 1e-6), 0.0, 1.0), l.atten.w);
            }
            float nl = max(dot(n, L), 0.0);
            sum += att * l.ambient.rgb;          // the light's ambient isn't shadowed
            if (l.spot.z > 0.0 && nl > 0.0 && att > 0.0)
                att *= PointShadow(l, posW, n);
            sum += att * nl * l.diffuse.rgb;
        }
    }
    return sum;
}

void main()
{
    const uint bi = uint(gl_VertexIndex) >> 3u, vi = uint(gl_VertexIndex) & 7u;
    Blade bl = blades[bi];
    BladeAt b = ShapeBlade(bl, vi, GF.camera.xyz, GF.viewport.z, GF.wind, GF.viewport.w, GF.look.w);
    vec3 root = b.root, up = b.up, upV = b.upV, rr = b.rr, pos = b.pos, bendNow = b.bendNow, bendBefore = b.bendBefore;
    float height = b.height, halfW = b.halfW, droop = b.droop, t = b.t, profile = b.profile, side = b.side;
    float fade = b.fade, widen = b.widen, gust = b.gust;
    bool head = b.head;
    vec3 tint = Unpack8(bl.b.z);
    vec3 groundAlbedo = Unpack8(bl.c.x);
    vec3 headColour = Unpack8(bl.c.y);
    float canopy = float(bl.c.y >> 24u) / 255.0;
    float R = GF.viewport.z;
    float dist = length(root.xz - GF.camera.xz);
    // The pusher offset is the same now and last frame: its own motion is fast and springs back, and putting it in
    // the motion vectors would smear it.
    vec2 layDir;
    float layAmount;
    vec3 axis = pos + bendNow + PusherOffset(pos, t, height, TrailAt(root.xz), layDir, layAmount);
    vec3 Vd = normalize(axis - GF.camera.xyz);
    // Billboard about the blade's own axis: the width is spread square to it and to the view, so a blade is never
    // edge-on (about the vertical when it is seen straight along its axis).
    vec2 vh = GF.camera.xz - axis.xz;
    float vl = length(vh);
    vec3 horiz = vl > 1e-3 ? vec3(-vh.y, 0.0, vh.x) / vl : vec3(1.0, 0.0, 0.0);
    vec3 right = normalize(cross(normalize(upV + rr * droop), Vd) + 0.1 * horiz);
    // A blade thinner than a pixel flickers in and out as it sways (and the TAA can't settle it): drawn at least
    // GF.lod.y pixels wide, its colour going towards the ground's by as much as it was widened (its coverage).
    float rootW = 2.0 * halfW * widen * max(fade, 0.05);
    float depth = max((GF.viewProj * vec4(root, 1.0)).w, 1e-3);
    float pixels = rootW * GF.lod.x / depth;
    float widenAA = GF.lod.y > 0.0 ? max(GF.lod.y / max(pixels, 1e-4), 1.0) : 1.0;
    vec3 across = right * (side * halfW * profile * fade * widen * widenAA);
    vec3 p = axis + across;
    vClip = GF.viewProj * vec4(p, 1.0);
    vPrevClip = GF.prevViewProj * vec4(p - bendNow + bendBefore, 1.0);
    gl_Position = vClip;
    if (GF.camera.w > 0.5)
        gl_Position.xy += FL.taa.xy * gl_Position.w;   // the TAA's sub-pixel offset, as every scene draw has
    vAcross = side;

    // The ground's light at the root: (lightmap + global ambient), clamped as D3D does, darkened by the sun's shadow
    // (the scene's lightmap shadow, F_SHADOWTEX), plus the local lights. Where the light pass wasn't captured, the
    // sun and the ambient stand in for the lightmap.
    vec3 baked = (bl.b.w >> 24u) != 0u ? Unpack8(bl.b.w) : FL.sunColor.rgb * 0.6;
    vec3 amb = GF.ambient.w > 0.5 ? GF.ambient.rgb : vec3(0.3);
    float shade = 1.0;
    if (FL.shadowParams.x > 0.5)
        // (Taken at the blade's axis a little towards the sun: the blades cast into the map too, and a blade
        // mustn't shadow itself - the map's copy of it faces the sun, this one the camera.)
        shade = 1.0 - (1.0 - SunShadow(axis - normalize(FL.sunDir.xyz + vec3(0.0, -1e-6, 0.0)) * 0.05)) *
                FL.shadowParams.y;
    vec3 groundN = normalize(vec3(up.x * 0.5, 1.0, up.z * 0.5));
    vec3 sun = GF.sunColour.rgb * max(dot(groundN, -normalize(GF.sunDir.xyz + vec3(0.0, -1e-6, 0.0))), 0.0);
    vec3 base = min(baked + amb + sun, vec3(1.0)) * shade, local = LocalLights(p, groundN) * GF.look.y;
    vec3 light = base + local;
    // The colour: a gentle root-to-tip gradient around the ground's own brightness, the base darkened by the canopy
    // around it (in a dense tuft deeper) - over the blade's area (wider at the root) each averages 1, so the field as a
    // whole is as bright as its ground - and the root going over to the ground's own colour (as bright as the blade's:
    // the tint is matched to it), so the blades grow out of it rather than stand on it. A head takes its own colour.
    // (Its mean over the area is a little under 1: in a full field more of the lighter upper blades is seen.)
    float grad = mix(0.85, 1.19, t);
    float ao = (1.0 - 0.55 * canopy * (1.0 - t) * (1.0 - t)) / (1.0 - 0.275 * canopy);
    // Trodden blades (layAmount: how far the trail lays them) keep their own colour to the root - lying flat, a root
    // gone over to the ground's colour would make the lane look like bare ground - and turn a paler, yellower green
    // (the pressed blades' lighter undersides), so the path shows against both the ground and the standing grass.
    float trodden = layAmount * min(GF.trail2.x, 1.0);
    float toSoil = 0.55 * (1.0 - smoothstep(0.0, 0.4, t)) * (1.0 - trodden);
    vec3 albedo = head ? headColour : mix(tint, groundAlbedo, toSoil) * (grad * ao);
    if (!head && layAmount > 0.0)
        albedo = mix(albedo, albedo * vec3(1.12, 1.15, 0.78), clamp(layAmount * GF.trail2.x, 0.0, 1.0));
    // The sun on the blade itself (GF.look.z, RVK_GrassGlow; FL.sunColor: none at night): a rounded blade, lit on the
    // side towards the sun and darker on the other (around the ground's light: the mean stays); the light shining
    // through its thin upper part when the camera looks towards the sun; a glint along it (a hair's sheen). In a dense
    // field a low sun reaches only the upper blades.
    vec3 extra = vec3(0.0);
    float lit = 1.0;
    const vec3 kLuma = vec3(0.3, 0.59, 0.11);
    if (GF.look.z > 0.0) {
        vec3 Ls = -normalize(FL.sunDir.xyz + vec3(0.0, -1e-6, 0.0));
        float sunOn = clamp(dot(FL.sunColor.rgb, kLuma) * 2.5, 0.0, 1.0) * smoothstep(0.0, 0.1, Ls.y);
        float canopyShade = mix(1.0, smoothstep(0.0, 0.75, t), canopy * 0.7 * (1.0 - Ls.y));
        float sunlit = shade * sunOn * canopyShade * GF.look.z;
        // Rounded: the edge towards the sun lighter, the other darker (zero-mean across the blade, so the field stays
        // as bright as its ground - which already holds the sunlight).
        vec3 face = vec3(vh.x, 0.0, vh.y) / max(vl, 1e-3);
        lit = 1.0 + sunlit * 0.3 * side * dot(right, Ls);
        // Through the blade: diffusely, wherever the sun is on its far side (the face away from the camera), and
        // strongest looking straight towards the sun; the thin upper blade lets more through than the base.
        vec3 V = normalize(p - GF.camera.xyz);
        float behind = max(-dot(face, Ls), 0.0), bk = max(dot(V, Ls), 0.0);
        float through = (0.5 * behind + 0.5 * bk * bk * bk) * pow(t, 1.3);
        extra += albedo * vec3(1.2, 1.25, 0.7) * FL.sunColor.rgb * (0.6 * through * sunlit);
        vec3 T = normalize(upV + rr * (2.0 * droop * t));
        vec3 H = normalize(Ls - V);
        float th = dot(T, H);
        float sheen = pow(max(1.0 - th * th, 0.0), 20.0) * t;
        extra += FL.sunColor.rgb * (0.07 * sheen * sunlit);
    }
    // Trodden grass: darker as it lies in the shade of the grass standing around it, whichever way it is seen; and as
    // lawn stripes - blades lying away from the camera show their lighter, sky-lit backs, those lying towards it their
    // shaded faces - so the path stands out even where it is only partly flattened.
    if (layAmount > 0.0 && GF.trail2.x > 0.0) {
        vec2 away = root.xz - GF.camera.xz;
        float al = length(away);
        float facing = al > 1e-3 ? dot(layDir, away / al) : 0.0;
        lit *= 1.0 + GF.trail2.x * layAmount * (0.22 * facing - 0.14) * (0.5 + 0.5 * t);
    }
    // A gust bends the blades over, showing more of their lighter faces: the sweep shows as a brighter band.
    lit *= 1.0 + 0.45 * (gust - kGustMean * GF.look.w) * t;   // (around the gusts' mean: the brightness stays)
    vAlbedo = albedo;
    vec3 colour = albedo * light * lit + extra;
    // Towards the field's edge, and for blades drawn wider than they are, the colour goes over to the lit ground's:
    // the field's rim melts into the ground instead of ending.
    vec3 groundLit = groundAlbedo * light;
    float toGround = 0.7 * smoothstep(0.55 * R, R, dist) + 0.5 * (1.0 - 1.0 / widenAA);
    colour = mix(colour, groundLit, min(toGround, 0.85));
    vColour = colour * GF.look.x;
    // For the post passes, as the scene's surfaces write them: how much local lights lit it (ambient occlusion spares
    // that) and the direct sunlight's share (contact shadows take it) - kept small: a thick field would speckle.
    vShares = vec2(clamp(dot(local, kLuma) / max(dot(light, kLuma), 1e-3), 0.0, 1.0), 0.25 * shade);
}
