#version 450
// Per-object motion blur: the strongest motion in each 32x32 tile (every other pixel).
layout(set = 0, binding = 0) uniform sampler2D motionTex;
layout(set = 0, binding = 1) uniform sampler2D depthTex;
#include "motion_common.glsl"
layout(location = 0) out vec4 outTile;

void main()
{
    ivec2 base = ivec2(gl_FragCoord.xy) * 32, size = ivec2(P.proj.zw);
    vec2 best = vec2(0.0);
    for (int y = 0; y < 32; y += 2)
        for (int x = 0; x < 32; x += 2) {
            ivec2 p = min(base + ivec2(x, y), size - 1);
            vec2 m = Motion(p);
            if (dot(m, m) > dot(best, best)) best = m;
        }
    outTile = vec4(best, 0.0, 1.0);
}
