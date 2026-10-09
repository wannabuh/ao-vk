// The environment probe (hdr.cpp RenderEnvProbe): what the camera has seen around it, by direction, in an
// octahedral map (world space, +y the pole). The prefiltered atlas holds five levels side by side, rougher to the
// right: level 0 128 x 128 at (0, 0); 1 64 x 64 at (128, 0); 2 32 x 32 at (128, 64); 3 16 x 16 at (160, 64);
// 4 8 x 8 at (176, 64); 192 x 128 in all.
const float kEnvLevels = 5.0;
const vec2 kEnvAtlas = vec2(192.0, 128.0);

vec2 EnvOctEncode(vec3 d)                        // direction -> [0, 1]^2
{
    d /= abs(d.x) + abs(d.y) + abs(d.z);
    vec2 e = d.y >= 0.0 ? d.xz : (1.0 - abs(d.zx)) * vec2(d.x >= 0.0 ? 1.0 : -1.0, d.z >= 0.0 ? 1.0 : -1.0);
    return e * 0.5 + 0.5;
}

vec3 EnvOctDecode(vec2 uv)                       // [0, 1]^2 -> direction
{
    vec2 e = uv * 2.0 - 1.0;
    vec3 d = vec3(e.x, 1.0 - abs(e.x) - abs(e.y), e.y);
    if (d.y < 0.0)
        d.xz = (1.0 - abs(d.zx)) * vec2(d.x >= 0.0 ? 1.0 : -1.0, d.z >= 0.0 ? 1.0 : -1.0);
    return normalize(d);
}

// A level's tile in the atlas: xy its corner, z its size (texels).
vec3 EnvTile(int level)
{
    return level == 0 ? vec3(0.0, 0.0, 128.0) : level == 1 ? vec3(128.0, 0.0, 64.0)
         : level == 2 ? vec3(128.0, 64.0, 32.0) : level == 3 ? vec3(160.0, 64.0, 16.0) : vec3(176.0, 64.0, 8.0);
}

// The roughness each level is filtered for.
float EnvLevelRoughness(int level) { return level == 0 ? 0.08 : level == 1 ? 0.3 : level == 2 ? 0.5 : level == 3 ? 0.75 : 1.0; }

// A level's texel coordinate (atlas uv) for a direction: within the tile, half a texel from its edges.
vec2 EnvAtlasUv(vec3 d, int level)
{
    vec3 t = EnvTile(level);
    vec2 uv = clamp(EnvOctEncode(d) * t.z, vec2(0.5), vec2(t.z - 0.5));
    return (t.xy + uv) / kEnvAtlas;
}
