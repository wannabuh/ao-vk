#version 450
// Interface glass (interface.cpp): the frame under the interface blurred - one dual-filter (Kawase) downsampling
// step: the centre and four diagonal taps half a texel out, each bilinear (4 texels), into a target half the size.
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform Push {
    vec4 params;        // source texel size xy
} P;
layout(location = 0) out vec4 outColor;

void main()
{
    vec2 uv = gl_FragCoord.xy * 2.0 * P.params.xy, h = P.params.xy;
    vec4 c = texture(src, uv) * 4.0;
    c += texture(src, uv + vec2(-h.x, -h.y));
    c += texture(src, uv + vec2(h.x, -h.y));
    c += texture(src, uv + vec2(-h.x, h.y));
    c += texture(src, uv + vec2(h.x, h.y));
    outColor = c * 0.125;
}
