#version 450
// HDR scene -> 8-bit main target (hdr.cpp). Colours up to the knee pass unchanged, so the game's own look is kept;
// brighter ones roll off smoothly towards 1 by their brightest channel, which keeps their hue, and lose saturation
// the more they are overexposed, as bright light does.
layout(set = 0, binding = 0) uniform sampler2D scene;
layout(push_constant) uniform Push {
    vec4 params;        // knee, exposure
} P;
layout(location = 0) out vec4 outColor;

void main()
{
    vec4 s = texelFetch(scene, ivec2(gl_FragCoord.xy), 0);
    vec3 c = max(s.rgb * P.params.y, vec3(0.0));
    float m = max(c.r, max(c.g, c.b)), k = P.params.x;
    if (m > k) {
        float room = 1.0 - k;
        float shown = room > 0.0 ? k + room * (1.0 - exp(-(m - k) / room)) : 1.0;
        float white = m > 1.0 ? 1.0 - 1.0 / (1.0 + 0.25 * (m - 1.0)) : 0.0;   // 0 at 1, 0.2 at 2, 0.5 at 5
        c = mix(c * (shown / m), vec3(shown), white);
    }
    outColor = vec4(min(c, vec3(1.0)), clamp(s.a, 0.0, 1.0));
}
