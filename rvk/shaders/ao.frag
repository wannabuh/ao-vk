#version 450
// Ambient occlusion (hdr.cpp), at half resolution from the scene's depth buffer: how much of the hemisphere above
// each surface point nearby geometry covers (horizon-based, normals reconstructed from depth). Output: occlusion
// factor (1 = open) and view depth, for the depth-aware blur.
layout(set = 0, binding = 0) uniform sampler2D depthTex;
layout(push_constant) uniform Push {
    vec4 proj;          // D3D projection: m[2][2], m[3][2], m[0][0], m[1][1]
    vec4 params;        // radius (world units), strength, full target width, height
} P;
layout(location = 0) out vec4 outAo;

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
    vec2 pix = floor(gl_FragCoord.xy) * 2.0;     // the full-resolution pixel this one stands for
    bool sky;
    vec3 p = ViewPos(pix, sky);
    if (sky) { outAo = vec4(1.0, 1e6, 0.0, 0.0); return; }
    // Normal from the neighbours on the side with the smaller depth step (no smearing across edges).
    bool s0;
    vec3 r = ViewPos(pix + vec2(1, 0), s0) - p, l = p - ViewPos(pix - vec2(1, 0), s0);
    vec3 d = ViewPos(pix + vec2(0, 1), s0) - p, u = p - ViewPos(pix - vec2(0, 1), s0);
    vec3 dx = abs(r.z) < abs(l.z) ? r : l, dy = abs(d.z) < abs(u.z) ? d : u;
    vec3 n = normalize(cross(dx, dy));
    if (dot(n, p) > 0.0) n = -n;                  // facing the camera (view space, camera at the origin)

    float radius = P.params.x;
    float pixRadius = clamp(radius * P.proj.z * 0.5 * P.params.z / p.z, 2.0, 96.0);
    // Per-pixel rotation and step offset (interleaved gradient noise); the blur removes the pattern.
    float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    const int kDirs = 6, kSteps = 4;
    float occlusion = 0.0;
    for (int i = 0; i < kDirs; ++i) {
        float a = (float(i) + noise) * (6.2831853 / float(kDirs));
        vec2 dir = vec2(cos(a), sin(a));
        for (int s = 0; s < kSteps; ++s) {
            float t = (float(s) + fract(noise * 7.0 + float(s) * 0.618)) / float(kSteps);
            vec2 q = pix + dir * (t * pixRadius + 1.0);
            bool qs;
            vec3 v = ViewPos(clamp(q, vec2(0.0), P.params.zw - 1.0), qs) - p;
            float len2 = dot(v, v);
            if (qs || len2 < 1e-6) continue;
            float cosine = dot(n, v) * inversesqrt(len2);
            occlusion += clamp(cosine - 0.15, 0.0, 1.0) * clamp(1.0 - len2 / (radius * radius), 0.0, 1.0);
        }
    }
    float ao = clamp(1.0 - 2.0 * P.params.y * occlusion / float(kDirs * kSteps), 0.0, 1.0);
    outAo = vec4(ao, p.z, 0.0, 0.0);
}
