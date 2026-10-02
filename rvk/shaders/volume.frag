#version 450
// Volumetric light (hdr.cpp), at half resolution: the light the air between the camera and each surface scatters
// towards the camera. Sunlight: marched along the ray through the sun shadow cascades, so shadowed air stays dark
// and light coming through gaps forms shafts, brightest looking towards the sun (forward scattering). Lamps: the
// glow of each frame light along the ray, integrated in closed form (no shadows). Output: the scattered light and
// view depth, for the depth-aware blur and upsampling.
#include "frame_lights.glsl"
layout(set = 0, binding = 0) uniform sampler2D depthTex;
layout(set = 0, binding = 1) uniform sampler2DArrayShadow shadowMap;
layout(push_constant) uniform Push {
    mat4 invViewProj;   // the world camera's clip -> world (raw D3DMATRIX memory)
    vec4 eye;           // camera position; w = longest ray (world units)
    vec4 params;        // haze density, sun strength, lamp strength, D3D projection m[2][2]
    vec4 size;          // full target width, height, steps, D3D projection m[3][2]
} P;
layout(location = 0) out vec4 outVolume;

// Sunlit (1) or in shadow (0) at a point in the air: one comparison tap in the sharpest cascade holding it.
float SunVisible(vec3 p)
{
    int count = int(FL.shadowParams.z);
    for (int c = 0; c < count; ++c) {
        vec4 sc = FL.shadowViewProj[c] * vec4(p, 1.0);
        vec3 ndc = sc.xyz / sc.w;
        if (max(abs(ndc.x), abs(ndc.y)) >= 0.98 || ndc.z <= 0.0 || ndc.z >= 1.0) continue;
        return texture(shadowMap, vec4(ndc.xy * 0.5 + 0.5, float(c), ndc.z));
    }
    return 1.0;
}

void main()
{
    vec2 pix = floor(gl_FragCoord.xy) * 2.0;
    float d = texelFetch(depthTex, ivec2(pix), 0).r;
    float dc = min(d, 0.999999);
    vec2 ndc = vec2((pix.x + 0.5) / P.size.x * 2.0 - 1.0, 1.0 - (pix.y + 0.5) / P.size.y * 2.0);
    vec4 w = P.invViewProj * vec4(ndc, dc, 1.0);
    vec3 toPoint = w.xyz / w.w - P.eye.xyz;
    float dist = length(toPoint);
    vec3 dir = toPoint / max(dist, 1e-4);
    float len = min(dist, P.eye.w);
    float viewZ = P.size.w / (dc - P.params.w);
    vec3 light = vec3(0.0);

    // Sun: march with a per-pixel offset (the blur hides the pattern), the steps closer together near the camera,
    // where shafts past nearby objects are big on screen: t = len * u^2, each step weighted by its length.
    if (FL.shadowParams.x > 0.5 && P.params.y > 0.0 && dot(FL.sunColor.rgb, FL.sunColor.rgb) > 0.0) {
        int steps = int(P.size.z);
        float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
        float lit = 0.0;
        for (int i = 0; i < steps; ++i) {
            float u = (float(i) + noise) / float(steps);
            lit += SunVisible(P.eye.xyz + dir * (len * u * u)) * (2.0 * u);
        }
        float stepLen = len / float(steps);
        // Henyey-Greenstein phase (g = 0.5), scaled so light scattered evenly would be 1.
        const float g = 0.5;
        float cosAngle = dot(dir, -normalize(FL.sunDir.xyz));
        float phase = (1.0 - g * g) / pow(1.0 + g * g - 2.0 * g * cosAngle, 1.5);
        light += FL.sunColor.rgb * (lit * stepLen * P.params.x * P.params.y * phase);
    }

    // Lamps: their light scattered along the ray within their range, falling off with the square of the distance;
    // the integral of 1 / (h^2 + t^2) is atan(t / h) / h. Not the lights characters carry: a haze over every head.
    if (P.params.z > 0.0) {
        for (uint i = 0u; i < FL.info.x; ++i) {
            Light l = FL.lights[i];
            if (l.ambient.w > 0.5) continue;
            float range = l.direction.w;
            vec3 oc = l.position.xyz - P.eye.xyz;
            float t0 = dot(oc, dir);
            vec3 perp = oc - t0 * dir;
            float h2 = dot(perp, perp), r2 = range * range;
            if (h2 >= r2) continue;
            float halfChord = sqrt(r2 - h2);
            float a = max(0.0, t0 - halfChord), b = min(dist, t0 + halfChord);
            if (b <= a) continue;
            float h = max(sqrt(h2), 0.3);        // passing right through a lamp: no infinite core
            float along = (atan((b - t0) / h) - atan((a - t0) / h)) / h;
            float fade = 1.0 - h2 / r2;
            light += l.diffuse.rgb * (along * fade * fade * P.params.z);
        }
    }
    outVolume = vec4(light, viewZ);
}
