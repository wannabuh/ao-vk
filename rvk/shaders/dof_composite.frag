#version 450
// Depth of field (hdr.cpp), first: the HDR scene with its ambient occlusion applied, so the blur takes it along (the
// tone mapping then leaves the occlusion out).
#include "occlusion.glsl"
layout(location = 0) out vec4 outColor;

void main()
{
    vec4 s = texelFetch(scene, ivec2(gl_FragCoord.xy), 0);
    outColor = vec4(s.rgb * AmbientFactor(), s.a);
}
