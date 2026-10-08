#version 450
// Environment probe, prefiltering (hdr.cpp RenderEnvProbe): the atlas of five levels (env_common.glsl), each texel the
// probe's light around its direction over a cone as wide as its level's roughness - a GGX-like lobe of Fibonacci
// taps, weighted by how sure the probe is of each (a: the weighted sureness, so unseen directions fall back to the
// shader's sky).
#include "env_common.glsl"
layout(set = 0, binding = 0) uniform sampler2D env;
layout(push_constant) uniform Push {
    vec4 params;        // x: frame noise (rotates the taps; the probe's blend averages them over frames)
} P;
layout(location = 0) out vec4 outAtlas;

void main()
{
    vec2 p = gl_FragCoord.xy;
    int level = -1;
    for (int l = 0; l < 5; ++l) {
        vec3 t = EnvTile(l);
        if (all(greaterThanEqual(p, t.xy)) && all(lessThan(p, t.xy + t.z))) { level = l; break; }
    }
    if (level < 0) { outAtlas = vec4(0.0); return; }
    vec3 t = EnvTile(level);
    vec3 n = EnvOctDecode((p - t.xy) / t.z);
    if (level == 0) {
        outAtlas = textureLod(env, EnvOctEncode(n), 0.0);
        return;
    }
    float r = EnvLevelRoughness(level), a2 = r * r * r * r;
    vec3 up = abs(n.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 tx = normalize(cross(up, n)), ty = cross(n, tx);
    const int kTaps = 48;
    vec3 sum = vec3(0.0);
    float sure = 0.0, total = 0.0;
    for (int i = 0; i < kTaps; ++i) {
        // GGX half-vector sample (n = v: the lobe around the reflection), Fibonacci spiral in (u1, u2).
        float u1 = (float(i) + 0.5) / float(kTaps);
        float u2 = fract(float(i) * 0.618034 + P.params.x);
        float cosT = sqrt((1.0 - u1) / (1.0 + (a2 - 1.0) * u1));
        float sinT = sqrt(max(1.0 - cosT * cosT, 0.0));
        float phi = 6.2831853 * u2;
        vec3 h = tx * (sinT * cos(phi)) + ty * (sinT * sin(phi)) + n * cosT;
        vec3 l = reflect(-n, h);
        float nl = dot(n, l);
        if (nl <= 0.0) continue;
        vec4 c = textureLod(env, EnvOctEncode(l), 0.0);
        sum += c.rgb * c.a * nl;
        sure += c.a * nl;
        total += nl;
    }
    outAtlas = vec4(sum / max(sure, 1e-4), sure / max(total, 1e-4));
}
