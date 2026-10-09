#version 450
// Environment probe, accumulation (hdr.cpp RenderEnvProbe): each texel of the 128 x 128 octahedral map is a direction
// from the camera; where the HDR scene shows that direction (and what it shows is not right next to the camera - a
// character in front of it would be reflected everywhere), the texel moves towards it. rgb: the colour; a: how sure
// (1 = seen lately). Moving the camera, or time, makes it less sure (params.z), a jump forgets it (params.w).
#include "env_common.glsl"
layout(set = 0, binding = 0) uniform sampler2D prevEnv;
layout(set = 0, binding = 1) uniform sampler2D sceneTex;
layout(set = 0, binding = 2) uniform sampler2D depthTex;
layout(push_constant) uniform Push {
    mat4 viewProj;      // the world camera's view x projection (raw D3DMATRIX memory: rows times the matrix)
    vec4 proj;          // D3D projection m[2][2], m[3][2]; scene width, height
    vec4 params;        // blend, nearest distance taken, sureness kept, 1 = forget
} P;
layout(location = 0) out vec4 outEnv;

void main()
{
    ivec2 texel = ivec2(gl_FragCoord.xy);
    vec4 prev = P.params.w > 0.5 ? vec4(0.0) : texelFetch(prevEnv, texel, 0);
    prev.a *= P.params.z;
    vec3 d = EnvOctDecode(gl_FragCoord.xy / 128.0);
    vec4 clip = P.viewProj * vec4(d, 0.0);
    outEnv = prev;
    if (clip.w <= 1e-4)
        return;
    vec2 ndc = clip.xy / clip.w;
    if (max(abs(ndc.x), abs(ndc.y)) >= 1.0)
        return;
    vec2 uv = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    float depth = texelFetch(depthTex, ivec2(uv * P.proj.zw), 0).r;
    float z = depth >= 1.0 ? 1e6 : P.proj.y / (min(depth, 0.999999) - P.proj.x);
    if (z < P.params.y)
        return;
    // A few taps (the texel spans many pixels), less towards the screen's edges (no seam where the view ends).
    vec2 px = 1.5 / P.proj.zw;
    vec3 c = 0.25 * (textureLod(sceneTex, uv + vec2(px.x, px.y), 0.0).rgb + textureLod(sceneTex, uv + vec2(-px.x, px.y), 0.0).rgb +
                     textureLod(sceneTex, uv + vec2(px.x, -px.y), 0.0).rgb + textureLod(sceneTex, uv - px, 0.0).rgb);
    c = min(c, vec3(64.0));
    float edge = 1.0 - smoothstep(0.8, 1.0, max(abs(ndc.x), abs(ndc.y)));
    float w = P.params.x * edge;
    float a = mix(prev.a, 1.0, w);
    outEnv = vec4((prev.rgb * prev.a * (1.0 - w) + c * w) / max(a, 1e-4), a);
}
