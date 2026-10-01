#version 450
// Depth of field: the focus distance (1x1 target). Auto focus: the nearest surface in a small box around the screen
// centre (a third-person camera centres the player's character), eased towards over ~0.3 s; or the manual distance.
layout(set = 0, binding = 0) uniform sampler2D depthTex;
layout(set = 0, binding = 1) uniform sampler2D previous;  // last frame's focus (r), valid (g)
#include "dof_common.glsl"
layout(location = 0) out vec4 outFocus;

void main()
{
    vec2 size = P.proj.zw;
    float target = 1e5;
    for (int y = 0; y < 7; ++y)
        for (int x = 0; x < 7; ++x) {
            vec2 uv = vec2(0.5, 0.55) + (vec2(x, y) / 6.0 - 0.5) * vec2(0.08, 0.12);
            float z = ViewZ(texelFetch(depthTex, ivec2(uv * size), 0).r);
            target = min(target, z);
        }
    vec2 prev = texelFetch(previous, ivec2(0), 0).rg;
    if (P.focus.y > 0.0) target = P.focus.y;
    else if (target >= 1e5) target = prev.g > 0.5 ? prev.r : 100.0;   // only sky: keep the focus
    float focus = prev.g > 0.5 ? mix(prev.r, target, 1.0 - exp(-P.focus.x / 0.3)) : target;
    outFocus = vec4(focus, 1.0, 0.0, 1.0);
}
