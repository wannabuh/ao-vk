#version 450
// Indirect light (hdr.cpp), at quarter resolution (it is very soft): the light the lit scene on screen reflects onto each surface point -
// one bounce, gathered from the HDR scene along the same kind of horizon search as the ambient occlusion. Per
// direction, a sample adds its light only where it rises above everything nearer in that direction (what it shows
// of the hemisphere above the point), so light doesn't come through walls. Output: incoming light (the tone mapping
// multiplies it by the surface colour) and view depth, for the depth-aware blur.
layout(set = 0, binding = 0) uniform sampler2D depthTex;
layout(set = 0, binding = 1) uniform sampler2D scene;     // HDR scene, full resolution
layout(push_constant) uniform Push {
    vec4 proj;          // D3D projection: m[2][2], m[3][2], m[0][0], m[1][1]
    vec4 params;        // radius (world units), this frame's noise offset, full target width, height
} P;
layout(location = 0) out vec4 outGi;

float ViewZ(float d) { return P.proj.y / (d - P.proj.x); }     // D3D: depth = m22 + m32 / z

vec3 ViewPos(vec2 pix, out bool sky)
{
    float d = texelFetch(depthTex, ivec2(pix), 0).r;
    sky = d >= 1.0;
    float z = ViewZ(min(d, 0.999999));
    vec2 ndc = vec2((pix.x + 0.5) / P.params.z * 2.0 - 1.0, 1.0 - (pix.y + 0.5) / P.params.w * 2.0);
    return vec3(ndc.x * z / P.proj.z, ndc.y * z / P.proj.w, z);
}

void main()
{
    vec2 pix = floor(gl_FragCoord.xy) * 4.0;     // the full-resolution pixel this one stands for
    bool sky;
    vec3 p = ViewPos(pix, sky);
    if (sky) { outGi = vec4(0.0, 0.0, 0.0, 1e6); return; }
    // Normal from the neighbours on the side with the smaller depth step (as ao.frag).
    bool s0;
    vec3 r = ViewPos(pix + vec2(1, 0), s0) - p, l = p - ViewPos(pix - vec2(1, 0), s0);
    vec3 d = ViewPos(pix + vec2(0, 1), s0) - p, u = p - ViewPos(pix - vec2(0, 1), s0);
    vec3 dx = abs(r.z) < abs(l.z) ? r : l, dy = abs(d.z) < abs(u.z) ? d : u;
    vec3 n = normalize(cross(dx, dy));
    if (dot(n, p) > 0.0) n = -n;

    float radius = P.params.x;
    float pixRadius = clamp(radius * P.proj.z * 0.5 * P.params.z / p.z, 4.0, 0.25 * P.params.z);
    float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))) + P.params.y);
    const int kDirs = 8, kSteps = 8;
    vec3 light = vec3(0.0);
    for (int i = 0; i < kDirs; ++i) {
        float a = (float(i) + noise) * (6.2831853 / float(kDirs));
        vec2 dir = vec2(cos(a), sin(a));
        float horizon = 0.0;                     // sine of the highest elevation above the surface seen so far
        for (int s = 0; s < kSteps; ++s) {
            // Denser near the point, where most of the bounce light comes from.
            float t = (float(s) + fract(noise * 7.0 + float(s) * 0.618)) / float(kSteps);
            vec2 q = clamp(pix + dir * (t * t * pixRadius + 2.0), vec2(0.0), P.params.zw - 1.0);
            bool qs;
            vec3 v = ViewPos(q, qs) - p;
            float len2 = dot(v, v);
            if (qs || len2 < 1e-6 || len2 > radius * radius) continue;
            float h = dot(n, v) * inversesqrt(len2);
            if (h <= horizon) continue;
            vec3 radiance = texelFetch(scene, ivec2(q), 0).rgb;
            light += radiance * (h - horizon) * (1.0 - len2 / (radius * radius));
            horizon = h;
        }
    }
    outGi = vec4(light / float(kDirs), p.z);
}
