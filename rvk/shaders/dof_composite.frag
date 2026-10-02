#version 450
// Depth of field (hdr.cpp), first: the HDR scene with its ambient occlusion and indirect light applied, so the blur
// takes them along (the tone mapping then leaves them out).
#include "occlusion.glsl"
layout(location = 0) out vec4 outColor;

void main()
{
    vec4 s = texelFetch(scene, ivec2(gl_FragCoord.xy), 0);
    outColor = vec4(s.rgb * AmbientFactor() + Indirect(), s.a);
}
