// Depth of field (hdr.cpp), shared by its passes.
layout(push_constant) uniform Push {
    vec4 proj;          // D3D projection m[2][2], m[3][2] (view depth from the depth buffer); full width, height
    vec4 params;        // strength, max blur radius (full-resolution pixels), in-focus band, flags (1 near blur, 2 bokeh)
    vec4 focus;         // frame time (s), manual focus distance (0 = auto), unused, unused
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
    return sign(t) * min(a * P.params.x, 1.0) * P.params.y;
}
