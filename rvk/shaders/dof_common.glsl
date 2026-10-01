// Depth of field (hdr.cpp), shared by its passes.
layout(push_constant) uniform Push {
    vec4 proj;          // D3D projection m[2][2], m[3][2] (view depth from the depth buffer); full width, height
    vec4 params;        // strength, max blur radius (full-resolution pixels), in-focus band, flags (1 near blur, 2 bokeh,
                        // 4 far blur always)
    vec4 focus;         // frame time (s), manual focus distance (0 = auto), close focus distance, unused
} P;

float ViewZ(float d) { return d >= 1.0 ? 1e5 : P.proj.y / (d - P.proj.x); }

// Circle of confusion in full-resolution pixels, negative in front of the focus distance f. The thin-lens shape
// |1 - f / z| (0 at the focus, 1 at infinity, growing quickly closer than the focus), minus the in-focus band.
float Coc(float z, float f)
{
    float t = 1.0 - f / max(z, 1e-3);
    float band = P.params.z;
    float a = max(abs(t) - band, 0.0) / (1.0 - band);
    if (t < 0.0 && (int(P.params.w) & 1) == 0) a = 0.0;      // near blur off
    // Behind the focus: always, or only when the focus is close to the camera (fading in from the close focus distance
    // to half of it) - something held up to the camera blurs the distance, the character at play distance doesn't.
    if (t > 0.0 && (int(P.params.w) & 4) == 0) a *= smoothstep(P.focus.z, 0.5 * P.focus.z, f);
    return sign(t) * min(a * P.params.x, 1.0) * P.params.y;
}
