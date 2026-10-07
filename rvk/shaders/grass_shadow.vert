#version 450
// Ground grass into the sun's nearest shadow cascade (RVK_GrassShadow, grass.cpp DrawGrassShadow). Each blade is placed
// as the visible pass places it (grass_common.glsl: its shape, the leaning over, the field's edge, the wind and the
// gusts - the characters' push is left out), but its width is turned square to the sun, so it casts its full width
// whichever way the camera looks. Towards the cascade's edge the blades' shadows shrink away: beyond it the grass casts
// none, and a hard edge would show as a ring. Depth only.
#include "grass_common.glsl"

layout(set = 0, binding = 2, std430) readonly buffer Blades { Blade blades[]; };
layout(push_constant) uniform GrassShadowPass {
    mat4 lightViewProj;     // the cascade's (D3D row-vector convention, as the scene's)
    vec4 camera;            // xyz: the camera the visible pass shapes the blades for; w: the field radius
    vec4 wind;              // as GrassFrame.wind: x time, yz direction, w strength
    vec4 sun;               // xyz: the direction sunlight travels; w: the gusts' strength
    vec4 cascade;           // x: the least half width (world units, about half a texel); y, z: the edge fade (NDC)
} P;

void main()
{
    const uint bi = uint(gl_VertexIndex) >> 3u, vi = uint(gl_VertexIndex) & 7u;
    BladeAt b = ShapeBlade(blades[bi], vi, P.camera.xyz, P.camera.w, P.wind, P.wind.x, P.sun.w);
    vec4 rc = P.lightViewProj * vec4(b.root, 1.0);
    float keep = 1.0 - smoothstep(P.cascade.y, P.cascade.z, max(abs(rc.x), abs(rc.y)) / max(rc.w, 1e-6));
    vec3 axis = b.root + (b.pos + b.bendNow - b.root) * keep;
    vec3 T = normalize(b.upV + b.rr * b.droop);
    vec3 right = cross(T, normalize(P.sun.xyz));
    float rl = length(right);
    right = rl > 1e-3 ? right / rl : vec3(1.0, 0.0, 0.0);   // (the sun straight along the blade: any way across)
    // At least about a texel wide where it has width (a thinner blade would flicker in and out of the map as it sways).
    float w = b.halfW * b.profile * b.widen;
    if (b.profile > 0.0)
        w = max(w, P.cascade.x);
    gl_Position = P.lightViewProj * vec4(axis + right * (b.side * w * b.fade * keep), 1.0);
}
