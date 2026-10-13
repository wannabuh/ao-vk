#version 450
// The interface back end's shape (interface.cpp, docs/ui-dll.md phase 2): a rounded rectangle - signed distance, so
// its edges are anti-aliased at any size - filled with a vertical gradient, a border inside its edge and a glow
// outside. Drawn with the scissor around it (and its clip); premultiplied out (ONE, INV_SRC_ALPHA, alpha too: the
// interface layer's coverage). The glow adds light and covers nothing.
//
// At the interface's depth (GUI.dll draws every quad at one Z, depth test LESS and write: the first to draw a pixel
// keeps it): the colour pass tests it and writes no depth; the depth pass (kDepthOnly) then writes it where the shape
// is solid - not its soft edge or glow - so what is drawn after it stays behind.
layout(constant_id = 0) const bool kDepthOnly = false;
layout(push_constant) uniform Push {
    vec4 rect;          // x0, y0, x1, y1 (pixels)
    vec4 shape;         // corner radius, border width, glow width (pixels), depth
    vec4 fillTop;       // rgba, straight
    vec4 fillBottom;
    vec4 borderColor;
    vec4 glowColor;
} P;
layout(location = 0) out vec4 outColor;

float RoundedRect(vec2 p, vec2 centre, vec2 half_, float r)
{
    vec2 q = abs(p - centre) - (half_ - r);
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

void main()
{
    gl_FragDepth = P.shape.w;
    vec2 p = gl_FragCoord.xy;
    vec2 centre = 0.5 * (P.rect.xy + P.rect.zw), half_ = max(0.5 * (P.rect.zw - P.rect.xy), vec2(0.0));
    float r = min(P.shape.x, min(half_.x, half_.y));
    float d = RoundedRect(p, centre, half_, r);
    float outer = clamp(0.5 - d, 0.0, 1.0);                 // inside the shape
    if (kDepthOnly) {
        if (outer < 0.5)
            discard;
        outColor = vec4(0.0);
        return;
    }
    float border = P.shape.y;
    float inner = clamp(0.5 - (d + border), 0.0, 1.0);      // inside the border
    float band = border > 0.0 ? outer - inner : 0.0;
    float t = clamp((p.y - P.rect.y) / max(P.rect.w - P.rect.y, 1.0), 0.0, 1.0);
    vec4 fill = mix(P.fillTop, P.fillBottom, t);
    float fillCover = (border > 0.0 ? inner : outer) * fill.a;
    float borderCover = band * P.borderColor.a;

    vec3 rgb = fill.rgb * fillCover + P.borderColor.rgb * borderCover;
    float a = fillCover + borderCover;              // (the fill and the border don't overlap)
    float glow = P.shape.z > 0.0 ? P.glowColor.a * exp(-max(d, 0.0) / P.shape.z) * (1.0 - outer) : 0.0;
    outColor = vec4(rgb + P.glowColor.rgb * glow, min(a, 1.0));
}
