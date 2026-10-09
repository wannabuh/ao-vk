// The wind's gusts, shared by the ground grass (grass_common.glsl) and the leaves (ffp.vert BranchSway), so a gust
// crossing a field reaches the trees beside it too.
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

// Leaves (leaves.cpp): a canopy's branches swaying - its own cards and its leaves alike, in the scene (ffp.vert) and in
// the sun's shadow (shadow.vert), so the leaves stay on their branches and the light through them moves with them.
// Smooth over the crown (neighbouring sprigs on one branch move together), growing out from its centre: a slow swing
// with the wind, a little bob, and the gusts at the object's position. crown: centre (model space), radius; wd: the
// wind's direction (world x, z); amount: RVK_LeafWind x the wind's strength. World units.
vec3 BranchSwayAt(vec3 modelPos, vec4 crown, vec2 originXZ, vec2 wd, float time, float amount, float gusts)
{
    if (crown.w <= 0.0 || amount <= 0.0) return vec3(0.0);
    vec3 d = (modelPos - crown.xyz) / crown.w;
    float r = min(length(d), 1.3);
    vec3 n = d / max(r, 1e-3);
    float phase = dot(n, vec3(2.3, 1.7, 3.1)) + dot(originXZ, vec2(0.31, 0.23));
    float gust = Gust(originXZ, wd, time) * gusts;
    float swing = 0.55 * sin(time * 1.3 + phase) + 0.3 * sin(time * 2.9 + phase * 1.9) + 0.6 * gust;
    float bob = 0.4 * sin(time * 2.1 + phase * 2.7);
    float amp = 0.035 * crown.w * amount * r * r;
    return vec3(wd.x * swing, bob, wd.y * swing) * amp;
}

// How much a canopy's own cards are drawn in towards its crown's centre (a share of the way) and thinned out (the
// share of texel blocks dropped), for RVK_LeafCore = core and its leaves all drawn (keep = 1).
float CoreShrink(float core, float keep) { return 0.12 * core * keep; }
float CoreThin(float core, float keep) { return 0.35 * core * keep; }

// Leaves: a leaf is a round sprig - its card fades out towards the rim (leaf = where on the card, -1 .. 1) - and a
// canopy's own cards are thinned out in place (leaf = (how much, 2)): blocks of texels dropped by a stable hash
// (texel: the texture coordinate in texels). leaf.y > 2.5: neither. Multiplies the alpha.
float LeafMaskAt(vec2 leaf, vec2 texel)
{
    if (leaf.y > 2.5) return 1.0;
    if (leaf.y < 1.5) return smoothstep(1.0, 0.6, length(leaf));
    if (leaf.x <= 0.0) return 1.0;
    vec2 cell = floor(texel * 0.25);
    float h = fract(sin(dot(cell, vec2(12.9898, 78.233))) * 43758.5453);
    return h < leaf.x ? 0.0 : 1.0;
}
