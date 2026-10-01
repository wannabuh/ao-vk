#version 450
// Depth of field: the sharp full-resolution scene blended with the half-resolution blur by this pixel's circle of
// confusion, or by near-field blur reaching over it.
layout(set = 0, binding = 0) uniform sampler2D colour;   // sharp scene (ambient occlusion applied)
layout(set = 0, binding = 1) uniform sampler2D blurred;  // half resolution: blur, near-field reach
layout(set = 0, binding = 2) uniform sampler2D depthTex;
layout(set = 0, binding = 3) uniform sampler2D focusTex;
#include "dof_common.glsl"
layout(location = 0) out vec4 outColor;

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 sharp = texelFetch(colour, p, 0);
    float f = texelFetch(focusTex, ivec2(0), 0).r;
    float coc = abs(Coc(ViewZ(texelFetch(depthTex, p, 0).r), f));
    vec4 blur = texture(blurred, gl_FragCoord.xy / P.proj.zw);
    float amount = max(smoothstep(1.0, 3.0, coc), smoothstep(1.0, 3.0, blur.a));
    outColor = vec4(mix(sharp.rgb, blur.rgb, amount), sharp.a);
}
