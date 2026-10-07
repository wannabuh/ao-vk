#version 450
// Procedural ground grass (RVK_GrassOn, rvk/grass.cpp). Each blade is one 32-byte record in the blade pool; this shader
// expands it into a tapered strip - vertex v of blade b is gl_VertexIndex = 8 b + v (the draw's vertexOffset places the
// tile's blades in the pool): cross section v >> 1 from the root (0) to the tip (3), edge v & 1 - bends it with the
// wind and the characters walking through, fades it out near the field's edge and lights it. The lighting is the
// terrain's own (its light pass: the baked lightmap plus the global ambient, darkened by the sun's shadow, plus the
// frame's local lights), so a blade is as bright as the ground it stands on by day and by night. Per vertex: a blade
// is a few pixels wide, and the fragment shader is then nearly free, which is what makes the field's overdraw cheap.
#include "frame_lights.glsl"                    // FL (binding 4): the sun, its cascades, the lights, the pushers

layout(set = 0, binding = 0) uniform GrassFrame {
    mat4 viewProj;      // world -> clip (D3D row-vector convention, as the scene's)
    mat4 prevViewProj;  // ... last frame (motion vectors); equals viewProj when the camera didn't move
    vec4 viewport;      // xy: target size in pixels; z: the field radius; w: the wind clock last frame
    vec4 wind;          // x: time (s); yz: the wind's direction; w: strength
    vec4 camera;        // xyz: the camera; w: 1 = apply the TAA jitter
    vec4 ambient;       // rgb: the terrain's global ambient this frame; w: 1 = captured
    vec4 look;          // x: brightness; y: local light scale
    vec4 sunColour;     // rgb: the directional light the terrain's light pass takes (0 under the light override)
    vec4 sunDir;        // xyz: the direction it travels
} GF;

// The blade records (grass.cpp GrassBlade): root xyz, then packed words.
//   a.w: up direction x, z (snorm16 x 2)      b.x: height (unorm16 x 4 units), half width (unorm8 x 0.25), droop (unorm8)
//   b.y: yaw (unorm16 x 2 pi), phase (unorm16 x 20 pi)   b.z: tint RGB      b.w: light RGB + A (nonzero = captured)
struct Blade { vec4 a; uvec4 b; };
layout(set = 0, binding = 2, std430) readonly buffer Blades { Blade blades[]; };
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
// FL.pusherBorn; built by Device::FillPushers, the same the game's plants use). h: how far up the blade (0 root, 1
// tip), plantHeight its height in world units - the top bends most and the base stays.
vec3 PusherOffset(vec3 posW, float h, float plantHeight)
{
    float amount = FL.effects.w;
    uint count = min(FL.info.y, 16u);
    if (amount <= 0.0 || count == 0u || h <= 0.0)
        return vec3(0.0);
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
    float len = length(v);
    if (len <= 1e-4)
        return vec3(0.0);
    vec2 dir = v / len;
    float above = h * plantHeight;               // the vertex's height above the base
    float most = min(0.45 * amount, 0.7 * plantHeight);   // the top's furthest lean
    float lean = most * min(len, 1.0) * h * sqrt(h);      // bends most up top
    lean = min(lean, 0.9 * above);
    return vec3(dir.x * lean, -(above - sqrt(max(above * above - lean * lean, 0.0))), dir.y * lean);
}

// Sun visibility at a point: 1 lit, 0 in shadow - the first cascade that covers it, one filtered tap.
float SunShadow(vec3 posW)
{
    int count = int(FL.shadowParams.z);
    for (int c = 0; c < count; ++c) {
        vec4 sc = FL.shadowViewProj[c] * vec4(posW, 1.0);
        vec3 ndc = sc.xyz / sc.w;
        if (max(abs(ndc.x), abs(ndc.y)) >= 1.0 || ndc.z <= 0.0 || ndc.z >= 1.0)
            continue;
        return texture(shadowMap, vec4(ndc.xy * 0.5 + 0.5, float(c), ndc.z));
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

vec3 Unpack8(uint w) { return vec3(float((w >> 16u) & 0xFFu), float((w >> 8u) & 0xFFu), float(w & 0xFFu)) / 255.0; }

void main()
{
    const uint bi = uint(gl_VertexIndex) >> 3u, vi = uint(gl_VertexIndex) & 7u;
    Blade bl = blades[bi];
    vec3 root = bl.a.xyz;
    vec2 upXZ = unpackSnorm2x16(floatBitsToUint(bl.a.w));
    vec3 up = vec3(upXZ.x, sqrt(max(1.0 - dot(upXZ, upXZ), 0.0)), upXZ.y);
    float height = float(bl.b.x & 0xFFFFu) * (4.0 / 65535.0);
    float halfW = float((bl.b.x >> 16u) & 0xFFu) * (0.25 / 255.0);
    float droop = float(bl.b.x >> 24u) / 255.0;
    float yaw = float(bl.b.y & 0xFFFFu) * (6.2831853 / 65536.0);
    float phase = float(bl.b.y >> 16u) * (62.831853 / 65536.0);
    vec3 tint = Unpack8(bl.b.z);
    // The cross section: 0 root .. 3 tip (the far pattern draws only 0, 1 and 6).
    float t = float(vi >> 1u) / 3.0;
    float side = (vi & 1u) != 0u ? 1.0 : -1.0;
    vec2 r = vec2(cos(yaw), sin(yaw));
    // The blade arcs over towards the tip rather than standing straight.
    vec3 axis0 = root + up * (height * t) + vec3(r.x, 0.0, r.y) * (droop * height * t * t);
    // Fade the blade down to nothing near the field's edge, so its rim isn't a hard circle (a whole-blade scale towards
    // its root: the world stays static).
    float dist = length(root.xz - GF.camera.xz);
    float fade = clamp((GF.viewport.z - dist) / (GF.viewport.z * 0.5), 0.0, 1.0);
    fade = fade * fade * (3.0 - 2.0 * fade);
    vec3 pos = root + (axis0 - root) * fade;
    // The wind: a travelling gust, bending the upper part more (the square of the height along the blade).
    vec3 wdir = vec3(GF.wind.y, 0.0, GF.wind.z);
    float amp = 0.12 * height * fade * GF.wind.w * t * t;
    float now = 0.6 * sin(GF.wind.x * 1.9 + phase) + 0.25 * sin(GF.wind.x * 3.7 + phase * 1.7) +
                0.35 * (0.5 + 0.5 * sin(GF.wind.x * 0.37 + phase * 0.1));
    float before = 0.6 * sin(GF.viewport.w * 1.9 + phase) + 0.25 * sin(GF.viewport.w * 3.7 + phase * 1.7) +
                   0.35 * (0.5 + 0.5 * sin(GF.viewport.w * 0.37 + phase * 0.1));
    vec3 bendNow = wdir * (amp * now), bendBefore = wdir * (amp * before);
    // The pusher offset is the same now and last frame: its own motion is fast and springs back, and putting it in
    // the motion vectors would smear it.
    vec3 axis = pos + bendNow + PusherOffset(pos, t, height);
    // Billboard about the vertical: the width is spread across the view, so a blade is never edge-on (a vertical blade
    // seen from above would vanish).
    vec2 vh = GF.camera.xz - axis.xz;
    float vl = length(vh);
    vec2 right = vl > 1e-3 ? vec2(-vh.y, vh.x) / vl : vec2(1.0, 0.0);
    vec3 across = vec3(right.x, 0.0, right.y) * (side * halfW * (1.0 - t) * fade);
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
        shade = 1.0 - (1.0 - SunShadow(p)) * FL.shadowParams.y;
    vec3 groundN = normalize(vec3(up.x * 0.5, 1.0, up.z * 0.5));
    vec3 sun = GF.sunColour.rgb * max(dot(groundN, -normalize(GF.sunDir.xyz + vec3(0.0, -1e-6, 0.0))), 0.0);
    vec3 base = min(baked + amb + sun, vec3(1.0)) * shade, local = LocalLights(p, groundN) * GF.look.y;
    vec3 light = base + local;
    // A gentle root-to-tip gradient around the ground's own brightness (a blade's base sits in its neighbours' shade):
    // its mean over the blade's area (wider at the root) is 1, so the field as a whole is as bright as its ground.
    float grad = mix(0.88, 1.24, t);
    vAlbedo = tint * grad;
    vColour = vAlbedo * light * GF.look.x;
    // For the post passes, as the scene's surfaces write them: how much local lights lit it (ambient occlusion spares
    // that) and the direct sunlight's share (contact shadows take it) - kept small: a thick field would speckle.
    const vec3 kLuma = vec3(0.3, 0.59, 0.11);
    vShares = vec2(clamp(dot(local, kLuma) / max(dot(light, kLuma), 1e-3), 0.0, 1.0), 0.25 * shade);
}
