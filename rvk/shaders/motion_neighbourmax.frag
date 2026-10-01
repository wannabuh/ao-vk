#version 450
// Per-object motion blur: the strongest tile motion among each tile and its 8 neighbours (blur reaches across tiles).
layout(set = 0, binding = 0) uniform sampler2D tiles;
layout(location = 0) out vec4 outTile;

void main()
{
    ivec2 c = ivec2(gl_FragCoord.xy), size = textureSize(tiles, 0);
    vec2 best = vec2(0.0);
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            vec2 m = texelFetch(tiles, clamp(c + ivec2(x, y), ivec2(0), size - 1), 0).rg;
            if (dot(m, m) > dot(best, best)) best = m;
        }
    outTile = vec4(best, 0.0, 1.0);
}
