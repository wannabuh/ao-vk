#version 450
// Indirect light (hdr.cpp): separable blur at half resolution, taps spread by params.z texels - the sampling pattern
// is coarse, the light it gathers smooth. Only taps on the centre's surface plane mix in (planes from the depth
// buffer), so a wall's light doesn't spread onto the floor behind it, while a floor at a grazing angle - its depth
// changing quickly - still blurs along itself.
layout(set = 0, binding = 0) uniform sampler2D src;      // indirect light, view depth
layout(set = 0, binding = 1) uniform sampler2D depthTex; // full resolution scene depth
layout(push_constant) uniform Push {
    vec4 params;        // direction xy (texels), tap spacing
    vec4 proj;          // D3D projection: m[2][2], m[3][2], m[0][0], m[1][1]
    vec4 size;          // full target width, height
} P;
layout(location = 0) out vec4 outGi;

vec3 ViewPos(vec2 pix)
{
    float d = texelFetch(depthTex, ivec2(clamp(pix, vec2(0.0), P.size.xy - 1.0)), 0).r;
    float z = P.proj.y / (min(d, 0.999999) - P.proj.x);
    vec2 ndc = vec2((pix.x + 0.5) / P.size.x * 2.0 - 1.0, 1.0 - (pix.y + 0.5) / P.size.y * 2.0);
    return vec3(ndc.x * z / P.proj.z, ndc.y * z / P.proj.w, z);
}

void main()
{
    ivec2 c = ivec2(gl_FragCoord.xy), size = textureSize(src, 0);
    vec4 centre = texelFetch(src, c, 0);
    if (centre.a >= 1e5) { outGi = centre; return; }              // sky
    vec2 pix = vec2(c) * 2.0;
    vec3 p = ViewPos(pix);
    vec3 r = ViewPos(pix + vec2(1, 0)) - p, l = p - ViewPos(pix - vec2(1, 0));
    vec3 d = ViewPos(pix + vec2(0, 1)) - p, u = p - ViewPos(pix - vec2(0, 1));
    vec3 n = normalize(cross(abs(r.z) < abs(l.z) ? r : l, abs(d.z) < abs(u.z) ? d : u));
    vec3 sum = centre.rgb;
    float weight = 1.0;
    for (int i = -4; i <= 4; ++i) {
        if (i == 0) continue;
        ivec2 q = clamp(c + ivec2(P.params.xy * P.params.z) * i, ivec2(0), size - 1);
        vec4 s = texelFetch(src, q, 0);
        if (s.a >= 1e5) continue;
        float off = abs(dot(n, ViewPos(vec2(q) * 2.0) - p)) / max(p.z, 1e-3);   // distance from the plane, by depth
        float w = exp(-float(i * i) / 8.0) * exp(-off * 200.0);
        sum += s.rgb * w;
        weight += w;
    }
    outGi = vec4(sum / weight, centre.a);
}
