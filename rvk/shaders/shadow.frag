#version 450
// Shadow map pass for alpha-tested casters (foliage, fences): cut the texture's transparent parts out.
layout(push_constant) uniform Push {
    mat4 worldLightViewProj;
    vec4 alpha;
} P;

layout(set = 0, binding = 0) uniform sampler2D tex0;

layout(location = 0) in vec2 vTex0;

void main()
{
    if (texture(tex0, vTex0).a < P.alpha.x)
        discard;
}
