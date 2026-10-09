#version 450
// Screen-space reflections (hdr.cpp), at full resolution, only where the scene says the surface reflects (attachment 2
// G: water, glossy surfaces, wet ground): the view ray mirrored at the surface (normal from the depth buffer) is
// marched across the screen until it passes behind the depth buffer; the HDR scene there is the reflection. Rays that
// only find sky reflect the sky; rays leaving the screen fade out. Output: reflected colour and how much of it shows
// (reflectivity x Fresnel x confidence).
layout(set = 0, binding = 0) uniform sampler2D depthTex;
layout(set = 0, binding = 1) uniform sampler2D scene;          // HDR scene
layout(set = 0, binding = 2) uniform sampler2D reflectivity;   // G: how much the surface reflects
layout(set = 0, binding = 3) uniform sampler2D normals;        // the motion vectors' zw: PBR surfaces' shading normal
layout(push_constant) uniform Push {
    vec4 proj;          // D3D projection: m[2][2], m[3][2], m[0][0], m[1][1]
    vec4 size;          // full target width, height, steps, longest ray (world units)
    vec4 params;        // strength
} P;
layout(location = 0) out vec4 outSsr;

float ViewZ(float d) { return P.proj.y / (min(d, 0.999999) - P.proj.x); }

vec3 ViewPos(vec2 pix)
{
    float z = ViewZ(texelFetch(depthTex, ivec2(clamp(pix, vec2(0.0), P.size.xy - 1.0)), 0).r);
    vec2 ndc = vec2((pix.x + 0.5) / P.size.x * 2.0 - 1.0, 1.0 - (pix.y + 0.5) / P.size.y * 2.0);
    return vec3(ndc.x * z / P.proj.z, ndc.y * z / P.proj.w, z);
}

vec2 ToPixel(vec3 q)
{
    vec2 ndc = vec2(q.x * P.proj.z, q.y * P.proj.w) / q.z;
    return vec2((ndc.x + 1.0) * 0.5 * P.size.x, (1.0 - ndc.y) * 0.5 * P.size.y);
}

void main()
{
    ivec2 ipix = ivec2(gl_FragCoord.xy);
    float r = texelFetch(reflectivity, ipix, 0).g * P.params.x;
    float d = texelFetch(depthTex, ipix, 0).r;
    if (r < 0.004 || d >= 1.0) { outSsr = vec4(0.0); return; }
    vec2 pix = vec2(ipix);
    vec3 p = ViewPos(pix);
    vec3 dx1 = ViewPos(pix + vec2(1, 0)) - p, dx0 = p - ViewPos(pix - vec2(1, 0));
    vec3 dy1 = ViewPos(pix + vec2(0, 1)) - p, dy0 = p - ViewPos(pix - vec2(0, 1));
    vec3 n = normalize(cross(abs(dx1.z) < abs(dx0.z) ? dx1 : dx0, abs(dy1.z) < abs(dy0.z) ? dy1 : dy0));
    // A PBR surface's own shading normal where it gave one (ffp_main.glsl: octahedral, view space, w above 1.5):
    // smooth over curved low-poly meshes, and with their normal maps' detail.
    vec2 o = texelFetch(normals, ipix, 0).zw;
    if (o.y > 1.5) {
        o.y -= 3.0;
        vec3 s = vec3(o, 1.0 - abs(o.x) - abs(o.y));
        if (s.z < 0.0) s.xy = (1.0 - abs(s.yx)) * vec2(s.x >= 0.0 ? 1.0 : -1.0, s.y >= 0.0 ? 1.0 : -1.0);
        n = normalize(s);
    }
    if (dot(n, p) > 0.0) n = -n;
    vec3 v = normalize(p);
    vec3 dir = reflect(v, n);
    // Fresnel: surfaces reflect more seen at a grazing angle.
    float cosView = clamp(dot(-v, n), 0.0, 1.0);
    float amount = r * (0.25 + 0.75 * pow(1.0 - cosView, 3.0));

    int steps = int(P.size.z);
    float maxDist = P.size.w;
    float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    vec3 origin = p + n * (0.02 * p.z);                           // off the surface: no self-hit
    float prevT = 0.0;
    vec3 sky = vec3(0.0);
    bool sawSky = false;
    vec2 last = pix;                                               // the ray's last point on screen
    for (int i = 1; i <= steps; ++i) {
        float u = (float(i) - 1.0 + noise) / float(steps);
        float t = maxDist * u * u + 0.05;                          // closer together near the surface
        vec3 q = origin + dir * t;
        if (q.z <= 0.05) break;                                    // behind the camera
        vec2 hp = ToPixel(q);
        if (any(lessThan(hp, vec2(0.0))) || any(greaterThanEqual(hp, P.size.xy))) break;
        last = hp;
        float sd = texelFetch(depthTex, ivec2(hp), 0).r;
        if (sd >= 1.0) {                                           // over the sky: nothing in front there
            if (!sawSky) { sky = texelFetch(scene, ivec2(hp), 0).rgb; sawSky = true; }
            prevT = t;
            continue;
        }
        float behind = q.z - ViewZ(sd);
        float thickness = max(0.25, (t - prevT) * 1.5 + 0.02 * q.z);
        if (behind > 0.0 && behind < thickness) {
            // Hit: refine between the last step in front and this one.
            float a = prevT, b = t;
            for (int k = 0; k < 5; ++k) {
                float m = 0.5 * (a + b);
                vec3 qm = origin + dir * m;
                float bm = qm.z - ViewZ(texelFetch(depthTex, ivec2(ToPixel(qm)), 0).r);
                if (bm > 0.0) b = m; else a = m;
            }
            vec2 hit = ToPixel(origin + dir * b);
            vec2 uv = hit / P.size.xy;
            float edge = smoothstep(0.0, 0.08, min(min(uv.x, uv.y), min(1.0 - uv.x, 1.0 - uv.y)));
            float far = 1.0 - smoothstep(0.6, 1.0, b / maxDist);
            outSsr = vec4(texelFetch(scene, ivec2(hit), 0).rgb, amount * edge * far);
            return;
        }
        prevT = t;
    }
    if (sawSky) { outSsr = vec4(sky, amount); return; }
    // Left the screen without hitting anything: a ray heading up the screen most likely reaches the sky - the sky at
    // the top of the screen above where it left; the more steeply up, the surer.
    vec3 ahead = origin + dir;
    if (ahead.z > 0.05) {
        vec2 screenDir = ToPixel(ahead) - ToPixel(origin);
        float up = clamp(-screenDir.y / max(length(screenDir), 1e-4), 0.0, 1.0);
        ivec2 top = ivec2(clamp(last.x, 0.0, P.size.x - 1.0), 1);
        if (up > 0.0 && texelFetch(depthTex, top, 0).r >= 1.0) {
            outSsr = vec4(texelFetch(scene, top, 0).rgb, amount * smoothstep(0.0, 0.5, up));
            return;
        }
    }
    outSsr = vec4(0.0);
}
