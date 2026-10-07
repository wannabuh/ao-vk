// Procedural ground grass: what the visible blades (grass.vert) and their shadows (grass_shadow.vert) share - the
// blade record, its shape, the wind and the gusts - so a shadow sways with its blade.
// The blade records (grass.cpp GrassBlade): root xyz, then packed words.
//   a.w: up direction x, z (snorm16 x 2)      b.x: height (unorm16 x 4 units), half width (unorm8 x 0.25), droop (unorm8)
//   b.y: yaw (unorm16 x 2 pi), phase (unorm16 x 20 pi)   b.z: tint RGB      b.w: light RGB + A (nonzero = captured)
//   c.x: the ground texel's RGB, kind (2 bits) << 24, dense << 26, fade rank (5 bits) << 27
//   c.y: the head's RGB (seed stalks, flowers), the canopy's density around the blade (unorm8) << 24
//   c.z: how far inside its grass ground (unorm8: 0 at the edge of a path)
struct Blade { vec4 a; uvec4 b; uvec4 c; };
vec3 Unpack8(uint w) { return vec3(float((w >> 16u) & 0xFFu), float((w >> 8u) & 0xFFu), float(w & 0xFFu)) / 255.0; }


// The cross sections of each kind: how far up the blade (t) and how wide (of the half width). 0 a grass blade (tapering
// to its tip), 1 a broad blade (rounder), 2 a seed stalk (a thin stem, a spindle-shaped head), 3 a flower (a thin stem,
// a cup-shaped head on top, flat-topped). A head (kinds 2 and 3: sections 2 and 3) takes the head colour.
const vec4 kSectionT[4] = vec4[4](vec4(0.0, 0.3333, 0.6667, 1.0), vec4(0.0, 0.3333, 0.6667, 1.0),
                                  vec4(0.0, 0.62, 0.8, 1.0), vec4(0.0, 0.86, 0.91, 1.0));
const vec4 kSectionW[4] = vec4[4](vec4(1.0, 0.6667, 0.3333, 0.0), vec4(1.0, 0.92, 0.62, 0.0),
                                  vec4(0.4, 0.32, 1.0, 0.0), vec4(0.45, 0.4, 1.25, 0.95));

// A gust of wind: bands of stronger wind sweeping across the field along the wind, their fronts wavering. 0 .. 1.
float Gust(vec2 xz, vec2 wd, float time)
{
    float along = dot(xz, wd), across = dot(xz, vec2(-wd.y, wd.x));
    // ~16 units between fronts, moving ~6 units a second; the fronts bend and break up across the wind.
    float s = along * 0.39 - time * 2.3 + 1.6 * sin(across * 0.07 + time * 0.11) + 0.9 * sin(across * 0.21 - along * 0.05);
    float g = 0.5 + 0.5 * sin(s);
    float strength = 0.55 + 0.45 * sin(across * 0.043 + along * 0.021 - time * 0.21);   // some gusts weaker
    g *= g;
    return g * g * strength;                     // narrow fronts, calm between (mean ~0.15)
}
const float kGustMean = 0.15;

// The steady wind: waves running through the field along it - each row of grass sways a moment after the one upwind,
// as wind over a meadow does - a slower cross wave, and each blade's own flutter (stronger in a gust). Around a lean
// with the wind, never far back against it.
float Sway(vec2 xz, vec2 wd, float time, float phase, float gust)
{
    float along = dot(xz, wd), across = dot(xz, vec2(-wd.y, wd.x));
    float rows = sin(along * 0.55 - time * 2.4 + 0.7 * sin(phase));        // ~11 units apart, ~4.4 units a second
    float cross = sin(across * 0.23 + along * 0.31 - time * 1.7 + 1.3);
    float flutter = sin(time * 5.3 + phase * 1.7);
    return 0.45 + 0.35 * rows + 0.15 * cross + (0.12 + 0.35 * gust) * flutter;
}

// A sideways bend b (world units, horizontal) of a point above units up its blade, keeping the blade's length: the
// point comes down as it goes over.
vec3 BendKeepingLength(vec3 b, float above)
{
    float l = length(b), most = 0.9 * above;
    if (l > most)
        b *= most / l;
    return vec3(b.x, -(above - sqrt(max(above * above - dot(b, b), 0.0))), b.z);
}

// A blade's vertex placed (grass.cpp's record expanded): everything but the pushers, the billboard and the light.
struct BladeAt {
    vec3 root, up, upV, rr;     // the root, its up (the ground's slope), up leaning over for a top-down view, droop way
    float height, halfW, droop, phase, rank, inside;
    uint kind, section;
    bool dense, head;           // dense: one of the three in four that go first at the field's edge
    float t, profile, side;     // how far up the blade (0 .. 1), how wide there (of halfW), which edge (-1, 1)
    float steep, fade, widen;   // the camera looking down on it; the field's edge shrinking it; widening
    vec3 pos;                   // the axis point, shrunk by the fade, before the wind
    vec3 bendNow, bendBefore;   // the wind's bend now and at timeBefore (motion vectors)
    float gust;
};

// camera: the eye; R: the field radius; wind: x time, yz direction, w strength; gusts: their strength (0 = none).
BladeAt ShapeBlade(Blade bl, uint vi, vec3 camera, float R, vec4 wind, float timeBefore, float gusts)
{
    BladeAt b;
    b.root = bl.a.xyz;
    vec2 upXZ = unpackSnorm2x16(floatBitsToUint(bl.a.w));
    b.up = vec3(upXZ.x, sqrt(max(1.0 - dot(upXZ, upXZ), 0.0)), upXZ.y);
    b.height = float(bl.b.x & 0xFFFFu) * (4.0 / 65535.0);
    b.halfW = float((bl.b.x >> 16u) & 0xFFu) * (0.25 / 255.0);
    b.droop = float(bl.b.x >> 24u) / 255.0;
    float yaw = float(bl.b.y & 0xFFFFu) * (6.2831853 / 65536.0);
    b.phase = float(bl.b.y >> 16u) * (62.831853 / 65536.0);
    b.kind = (bl.c.x >> 24u) & 3u;
    b.dense = (bl.c.x & (1u << 26u)) != 0u;
    b.rank = float(bl.c.x >> 27u) / 31.0;
    b.inside = float(bl.c.z & 0xFFu) / 255.0;
    // The cross section: 0 root .. 3 tip (the far pattern skips 1).
    b.section = vi >> 1u;
    b.t = kSectionT[b.kind][b.section];
    b.profile = kSectionW[b.kind][b.section];
    b.head = b.kind >= 2u && b.section >= 2u;
    b.side = (vi & 1u) != 0u ? 1.0 : -1.0;
    b.rr = vec3(cos(yaw), 0.0, sin(yaw));
    // Seen from above, an upright field is mostly the ground between its blades (each is seen end-on): the steeper
    // the camera looks down on a blade, the further it leans over (each its own way, at most ~25 degrees) and the
    // wider it is drawn. Not at the edge of its ground: it would lie over the path beside it.
    vec3 toCam = camera - b.root;
    b.steep = smoothstep(0.4, 0.95, toCam.y / max(length(toCam), 1e-3));
    float tilt = 0.45 * b.steep * b.inside * b.inside;
    b.upV = normalize(b.up * cos(tilt) + b.rr * sin(tilt));
    // The blade arcs over towards the tip rather than standing straight.
    vec3 axis0 = b.root + b.upV * (b.height * b.t) + b.rr * (b.droop * b.height * b.t * b.t);
    // Towards the field's edge it thins out and then shrinks away, so its rim is no hard circle and no ring pops: the
    // dense blades (three in four) go first, each at its own distance (its rank), the sparse ones - widening to cover
    // for them - last, shrinking towards their roots over the outer third (a whole-blade scale: the world stays put).
    float dist = length(b.root.xz - camera.xz);
    float fade = clamp((R - dist) / (R * 0.35), 0.0, 1.0);
    b.fade = fade * fade * (3.0 - 2.0 * fade);
    b.widen = 1.0;
    if (b.dense) {
        float gone = R * (0.5 + 0.4 * b.rank);   // its own distance, by 0.9 R all gone (grass.cpp's sparse-only LOD)
        float keep = clamp((gone - dist) / (R * 0.08), 0.0, 1.0);
        b.fade *= keep * keep * (3.0 - 2.0 * keep);
    } else {
        b.widen = 1.0 + 0.7 * smoothstep(0.5 * R, 0.9 * R, dist);
    }
    b.widen *= 1.0 + 0.3 * b.steep;
    b.pos = b.root + (axis0 - b.root) * b.fade;
    // The wind: waves running through the field (the upper blade bending most: the square of the height along it),
    // and the gusts sweeping over it on top - each bending the blade over, not stretching it.
    vec2 wd = normalize(wind.yz);
    vec3 wdir = vec3(wd.x, 0.0, wd.y);
    b.gust = 0.0;
    float gustBefore = 0.0;
    if (gusts > 0.0) {
        b.gust = Gust(b.root.xz, wd, wind.x) * gusts;
        gustBefore = Gust(b.root.xz, wd, timeBefore) * gusts;
    }
    float amp = 0.16 * b.height * b.fade * wind.w * b.t * b.t, gustAmp = 0.7 * b.height * b.fade * wind.w * b.t * b.t;
    float above = b.height * b.t * b.fade;
    b.bendNow = BendKeepingLength(wdir * (amp * Sway(b.root.xz, wd, wind.x, b.phase, b.gust) + gustAmp * b.gust), above);
    b.bendBefore = BendKeepingLength(wdir * (amp * Sway(b.root.xz, wd, timeBefore, b.phase, gustBefore) +
                                             gustAmp * gustBefore), above);
    return b;
}
