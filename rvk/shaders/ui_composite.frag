#version 450
// The interface layer over the frame (interface.cpp): premultiplied by its coverage, blended ONE / INV_SRC_ALPHA.
layout(set = 0, binding = 0) uniform sampler2D layer;
layout(location = 0) out vec4 outColor;

void main()
{
    outColor = texelFetch(layer, ivec2(gl_FragCoord.xy), 0);
}
