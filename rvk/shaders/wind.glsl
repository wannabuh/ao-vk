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
