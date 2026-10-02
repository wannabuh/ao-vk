#version 450
// Contact shadows (hdr.cpp), at half resolution: a short march from each surface point towards the sun through the
// depth buffer - small shadows the shadow map is too coarse for (feet on the ground, gaps under objects, folds).
// Output: 1 = lit, 0 = shadowed, and view depth for the depth-aware blur (ao_blur.frag) and upsampling. The tone
// mapping takes the shadow only from the direct sunlight's share of the colour.
layout(set = 0, binding = 0) uniform sampler2D depthTex;
layout(push_constant) uniform Push {
    vec4 proj;          // D3D projection: m[2][2], m[3][2], m[0][0], m[1][1]
    vec4 size;          // full target width, height, steps, ray length (world units)
    vec4 sun;           // towards the sun, view space; w = farthest distance it applies at
    vec4 params;        // strength
} P;
layout(location = 0) out vec4 outContact;

float ViewZ(float d) { return P.proj.y / (min(d, 0.999999) - P.proj.x); }

vec3 ViewPos(vec2 pix)
{
    float z = ViewZ(texelFetch(depthTex, ivec2(clamp(pix, vec2(0.0), P.size.xy - 1.0)), 0).r);
    vec2 ndc = vec2((pix.x + 0.5) / P.size.x * 2.0 - 1.0, 1.0 - (pix.y + 0.5) / P.size.y * 2.0);
    return vec3(ndc.x * z / P.proj.z, ndc.y * z / P.proj.w, z);
}

void main()
{
    vec2 pix = floor(gl_FragCoord.xy) * 2.0;
    float d = texelFetch(depthTex, ivec2(pix), 0).r;
    vec3 p = ViewPos(pix);
    if (d >= 1.0 || p.z > P.sun.w) { outContact = vec4(1.0, p.z, 0.0, 0.0); return; }
    vec3 r = ViewPos(pix + vec2(1, 0)) - p, l = p - ViewPos(pix - vec2(1, 0));
    vec3 dn = ViewPos(pix + vec2(0, 1)) - p, up = p - ViewPos(pix - vec2(0, 1));
    vec3 n = normalize(cross(abs(r.z) < abs(l.z) ? r : l, abs(dn.z) < abs(up.z) ? dn : up));
    if (dot(n, p) > 0.0) n = -n;
    vec3 L = P.sun.xyz;
    if (dot(n, L) <= 0.0) { outContact = vec4(1.0, p.z, 0.0, 0.0); return; }   // faces away: no direct sun anyway
    int steps = int(P.size.z);
    float len = P.size.w;
    float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    vec3 origin = p + n * (0.01 * p.z + 0.02);
    float shadow = 0.0;
    for (int i = 0; i < steps; ++i) {
        float t = (float(i) + noise) / float(steps) * len;
        vec3 q = origin + L * t;
        if (q.z <= 0.05) break;
        vec2 hp = vec2(q.x * P.proj.z, q.y * P.proj.w) / q.z;
        hp = vec2((hp.x + 1.0) * 0.5 * P.size.x, (1.0 - hp.y) * 0.5 * P.size.y);
        if (any(lessThan(hp, vec2(0.0))) || any(greaterThanEqual(hp, P.size.xy))) break;
        float behind = q.z - ViewZ(texelFetch(depthTex, ivec2(hp), 0).r);
        if (behind > 0.02 && behind < 0.3 + 0.01 * q.z) {
            shadow = 1.0 - float(i) / float(steps);              // nearer occluders: darker
            break;
        }
    }
    float fade = 1.0 - smoothstep(0.7 * P.sun.w, P.sun.w, p.z);
    outContact = vec4(1.0 - shadow * fade * P.params.x, p.z, 0.0, 0.0);
}
