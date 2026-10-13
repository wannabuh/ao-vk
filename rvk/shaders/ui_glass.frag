#version 450
// Interface glass (interface.cpp): frosted glass behind one window, drawn onto the frame before the interface layer
// goes over it - so where the window is see-through (its background's alpha, AO's window transparency settings) the
// blurred, tinted scene shows, and its text and icons stay sharp. Drawn per window with the scissor around it:
// a rounded rectangle (anti-aliased signed distance), a thin bright rim inside its edge and a soft glow outside.
// Premultiplied (ONE, INV_SRC_ALPHA): the glow outside adds light and covers nothing.
layout(set = 0, binding = 0) uniform sampler2D blurred;  // the frame blurred, half its size or less (linear)
layout(push_constant) uniform Push {
    vec4 rect;          // the window on screen: x0, y0, x1, y1 (pixels)
    vec4 style;         // corner radius (pixels), dim (0-1), tint (0-1), edge glow (0-1)
    vec4 screen;        // 1 / target width, 1 / target height, glow width (pixels)
    vec4 tintColor;     // rgb: what the tint goes towards (AoVkGlass::tintColor)
    vec4 rimColor;      // rgb: the rim light and glow (AoVkGlass::rimColor)
} P;
layout(location = 0) out vec4 outColor;

// Signed distance to the rounded rectangle (negative inside).
float RoundedRect(vec2 p, vec2 centre, vec2 half_, float r)
{
    vec2 q = abs(p - centre) - (half_ - r);
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

void main()
{
    vec2 p = gl_FragCoord.xy;
    vec2 centre = 0.5 * (P.rect.xy + P.rect.zw), half_ = 0.5 * (P.rect.zw - P.rect.xy);
    float r = min(P.style.x, min(half_.x, half_.y));
    float d = RoundedRect(p, centre, half_, r);
    float inside = clamp(0.5 - d, 0.0, 1.0);

    vec3 scene = texture(blurred, p * P.screen.xy).rgb;
    vec3 glass = scene * (1.0 - P.style.y);
    // Tinted: the scene's light kept as brightness, its colour pulled towards the tint colour, which is added.
    float luma = dot(glass, vec3(0.3, 0.59, 0.11));
    vec3 hue = P.tintColor.rgb / max(max(P.tintColor.r, max(P.tintColor.g, P.tintColor.b)), 1e-3);
    glass = mix(glass, luma * hue * 0.8 + P.tintColor.rgb, P.style.z);

    float edge = P.style.w;
    float rim = edge * 0.6 * (1.0 - smoothstep(0.0, 1.5, abs(d + 1.0)));            // a line just inside the edge
    float glow = edge * 0.25 * exp(-max(d, 0.0) / max(P.screen.z, 0.5)) * (1.0 - inside);
    outColor = vec4(glass * inside + P.rimColor.rgb * (rim * inside + glow), inside);
}
