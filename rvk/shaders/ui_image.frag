#version 450
// The interface back end's image (interface.cpp, docs/ui-dll.md phase 2): part of a texture (a sprite sheet's frame)
// over a rectangle, tinted; premultiplied out over what is there (ONE, INV_SRC_ALPHA). Additive - an icon on black -
// puts out its light with alpha 0: added, covering nothing. At the interface's depth, tested, not written.
layout(set = 0, binding = 0) uniform sampler2D image;
layout(push_constant) uniform Push {
    vec4 rect;          // x0, y0, x1, y1 (pixels)
    vec4 uv;            // u0, v0, u1, v1: the texture's part
    vec4 tint;          // rgba, straight
    vec4 params;        // depth, additive (1)
} P;
layout(location = 0) out vec4 outColor;

void main()
{
    gl_FragDepth = P.params.x;
    vec2 t = (gl_FragCoord.xy - P.rect.xy) / max(P.rect.zw - P.rect.xy, vec2(1.0));
    vec4 c = texture(image, mix(P.uv.xy, P.uv.zw, clamp(t, 0.0, 1.0))) * P.tint;
    if (P.params.y > 0.5)
        outColor = vec4(c.rgb * P.tint.a, 0.0);
    else
        outColor = vec4(c.rgb * c.a, c.a);
}
