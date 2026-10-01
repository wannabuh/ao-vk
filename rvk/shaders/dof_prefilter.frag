#version 450
// Depth of field: half resolution colour and circle of confusion (alpha, full-resolution pixels, negative = near).
// The CoC is the nearest of the four pixels' - the foreground's - so a sharp object's outline isn't blurred away.
layout(set = 0, binding = 0) uniform sampler2D colour;    // the scene with its ambient occlusion
layout(set = 0, binding = 1) uniform sampler2D depthTex;
layout(set = 0, binding = 2) uniform sampler2D focusTex;
#include "dof_common.glsl"
layout(location = 0) out vec4 outColor;

void main()
{
    ivec2 base = ivec2(gl_FragCoord.xy) * 2, size = ivec2(P.proj.zw) - 1;
    float f = texelFetch(focusTex, ivec2(0), 0).r;
    vec3 sum = vec3(0.0);
    float nearest = 1e6;
    for (int i = 0; i < 4; ++i) {
        ivec2 p = min(base + ivec2(i & 1, i >> 1), size);
        sum += texelFetch(colour, p, 0).rgb;
        nearest = min(nearest, ViewZ(texelFetch(depthTex, p, 0).r));
    }
    outColor = vec4(sum * 0.25, Coc(nearest, f));
}
