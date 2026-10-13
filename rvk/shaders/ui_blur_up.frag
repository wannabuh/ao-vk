#version 450
// Interface glass (interface.cpp): one dual-filter (Kawase) upsampling step from the next smaller level: eight taps
// on a diamond around the pixel (the diagonal ones weighted double), into a target twice the size.
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform Push {
    vec4 params;        // source (smaller) texel size xy, target texel size zw
} P;
layout(location = 0) out vec4 outColor;

void main()
{
    vec2 uv = gl_FragCoord.xy * P.params.zw, h = P.params.xy * 0.5;
    vec4 c = texture(src, uv + vec2(-h.x * 2.0, 0.0));
    c += texture(src, uv + vec2(-h.x, h.y)) * 2.0;
    c += texture(src, uv + vec2(0.0, h.y * 2.0));
    c += texture(src, uv + vec2(h.x, h.y)) * 2.0;
    c += texture(src, uv + vec2(h.x * 2.0, 0.0));
    c += texture(src, uv + vec2(h.x, -h.y)) * 2.0;
    c += texture(src, uv + vec2(0.0, -h.y * 2.0));
    c += texture(src, uv + vec2(-h.x, -h.y)) * 2.0;
    outColor = c / 12.0;
}
