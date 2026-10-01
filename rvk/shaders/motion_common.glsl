// Per-object motion blur (hdr.cpp), shared by its passes. Requires bindings motionTex (screen motion since last
// frame, pixels, written by solid geometry) and depthTex.
layout(push_constant) uniform Push {
    mat4 reproject;     // current clip -> previous clip (raw D3D matrix): the camera's motion, for the sky
    vec4 params;        // motion scale (exposure / frame time), max length (pixels), unused, unused
    vec4 proj;          // D3D projection m[2][2], m[3][2]; target width, height
} P;

float ViewZ(float d) { return d >= 1.0 ? 1e6 : P.proj.y / (d - P.proj.x); }

// Motion during the exposure at a pixel: the motion vectors, or where none are written (the sky) the camera's.
vec2 Motion(ivec2 pix)
{
    float d = texelFetch(depthTex, pix, 0).r;
    vec2 m;
    if (d >= 1.0) {
        vec2 size = P.proj.zw;
        vec2 ndc = vec2((float(pix.x) + 0.5) / size.x * 2.0 - 1.0, 1.0 - (float(pix.y) + 0.5) / size.y * 2.0);
        vec4 prev = P.reproject * vec4(ndc, d, 1.0);
        m = prev.w > 1e-5 ? (ndc - prev.xy / prev.w) * 0.5 * size * vec2(1.0, -1.0) : vec2(0.0);
    } else {
        m = texelFetch(motionTex, pix, 0).rg;
    }
    m *= P.params.x;
    float len = length(m);
    return len > P.params.y ? m * (P.params.y / len) : m;
}
