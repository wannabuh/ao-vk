#version 450
// Shadow map pass for alpha-tested casters (foliage, fences): cut the texture's transparent parts out. A canopy's leaves
// and its thinned cards (leaves.cpp) as the scene draws them (wind.glsl LeafMaskAt).
layout(set = 0, binding = 0) uniform sampler2D tex0;

layout(location = 0) in vec2 vTex0;
layout(location = 1) flat in float vAlphaRef;
layout(location = 2) in vec2 vLeaf;

#include "wind.glsl"

void main()
{
    if (texture(tex0, vTex0).a * LeafMaskAt(vLeaf, vTex0 * vec2(textureSize(tex0, 0))) < vAlphaRef)
        discard;
}
