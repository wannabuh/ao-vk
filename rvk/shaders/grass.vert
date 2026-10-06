#version 450
// Procedural ground grass (RVK_GrassOn, rvk/grass.cpp). The blades are baked in world-space tiles; this pass transforms
// them, fades them out near the field's edge and bends them with the wind. The frame's camera, radius and wind come in
// one uniform buffer; the sunlight, its shadows and the pushers (characters walking through the grass) in the frame
// light block, shared with the scene so a blade bends out of the way exactly as the game's own plants do.
#include "frame_lights.glsl"                    // FL (binding 4): the sun, its cascades, the pushers

layout(set = 0, binding = 0) uniform GrassFrame {
    mat4 viewProj;      // world -> clip (D3D row-vector convention, as the scene's)
    mat4 prevViewProj;  // ... last frame (motion vectors); equals viewProj when the camera didn't move
    vec4 viewport;      // xy: target size in pixels; z: the field radius; w: the wind clock last frame
    vec4 wind;          // x: time (s); yz: the wind's direction; w: strength
    vec4 camera;        // xyz: the camera
} GF;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUv;          // the blade atlas coordinate
layout(location = 3) in float inShade;      // 0 at the root, 1 at the tip
layout(location = 4) in float inPhase;      // the blade's wind phase
layout(location = 5) in float inHeight;     // the blade's height
layout(location = 6) in float inBaseY;      // its root's world y
layout(location = 7) in vec4 inColour;      // the ground texel's colour (the blade's tint)
layout(location = 8) in float inAcross;     // how far this edge is from the axis: expanded towards the camera

layout(location = 0) out vec3 vPosW;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUv;
layout(location = 3) out float vShade;
layout(location = 4) out vec3 vTint;
layout(location = 5) out vec4 vClip;
layout(location = 6) out vec4 vPrevClip;

// The well-damped spring behind a walking character: a plant springs back a little past upright and settles.
float PushSpring(float age)
{
    float t = max(age - 0.08, 0.0);
    return exp(-6.0 * t) * cos(7.0 * t);         // overshoot ~7%
}

// A blade bending out of the way of the frame's pushers (the characters' feet and the trails behind them, FL.pushers /
// FL.pusherBorn; built by Device::FillPushers, the same the game's plants use). h: how far up the blade (0 root, 1
// tip), plantHeight its height in world units - the top bends most and the base stays.
vec3 PusherOffset(vec3 posW, float h, float plantHeight)
{
    float amount = FL.effects.w;
    uint count = min(FL.info.y, 16u);
    if (amount <= 0.0 || count == 0u || h <= 0.0)
        return vec3(0.0);
    float reach = 0.7 * sqrt(amount);            // the game's plants use 1.4; the grass bends over a smaller radius
    vec2 push = vec2(0.0), rustle = vec2(0.0);
    for (uint i = 0u; i < count; ++i) {
        vec4 p = FL.pushers[i];
        float dy = posW.y - p.y;
        if (dy < -1.0 || dy > 3.0)               // a blade on another floor, or a character on a roof
            continue;
        vec2 d = posW.xz - p.xz;
        float dist = length(d);
        if (dist >= reach)
            continue;
        float near = 1.0 - smoothstep(0.15 * reach, reach, dist);
        vec2 dir = dist > 1e-3 ? d / dist : vec2(0.7071);
        push += dir * near * PushSpring(p.w);
        float born = FL.pusherBorn[i >> 2u][i & 3u];
        float moving = exp(-3.0 * born) * exp(-4.0 * max(p.w - 0.1, 0.0));
        rustle += vec2(-dir.y, dir.x) * near * moving * sin(GF.wind.x * 11.0 + dot(posW.xz, vec2(3.1, 2.3)) + float(i));
    }
    vec2 v = push + 0.12 * rustle;
    float len = length(v);
    if (len <= 1e-4)
        return vec3(0.0);
    vec2 dir = v / len;
    float above = h * plantHeight;               // the vertex's height above the base
    float most = min(0.45 * amount, 0.7 * plantHeight);   // the top's furthest lean
    float lean = most * min(len, 1.0) * h * sqrt(h);      // bends most up top
    lean = min(lean, 0.9 * above);
    return vec3(dir.x * lean, -(above - sqrt(max(above * above - lean * lean, 0.0))), dir.y * lean);
}

void main()
{
    // Fade the blade down to nothing near the field's edge, so its rim isn't a hard circle.
    float dist = length(inPos.xz - GF.camera.xz);
    float fade = clamp((GF.viewport.z - dist) / (GF.viewport.z * 0.5), 0.0, 1.0);
    fade = fade * fade * (3.0 - 2.0 * fade);
    vec3 root = inPos;
    root.y = inBaseY + (inPos.y - inBaseY) * fade;
    // The wind, per blade: a travelling gust, bending the upper part more (the square of the height along the blade).
    vec3 wdir = vec3(GF.wind.y, 0.0, GF.wind.z);
    float amp = 0.12 * inHeight * fade * GF.wind.w * inShade * inShade;
    float now = 0.6 * sin(GF.wind.x * 1.9 + inPhase) + 0.25 * sin(GF.wind.x * 3.7 + inPhase * 1.7) +
                0.35 * (0.5 + 0.5 * sin(GF.wind.x * 0.37 + inPhase * 0.1));
    float before = 0.6 * sin(GF.viewport.w * 1.9 + inPhase) + 0.25 * sin(GF.viewport.w * 3.7 + inPhase * 1.7) +
                   0.35 * (0.5 + 0.5 * sin(GF.viewport.w * 0.37 + inPhase * 0.1));
    // The blade's position now and last frame (the wind differs, and a character may be walking through), for temporal
    // reprojection. The pusher offset is the same in both: its own motion is fast and springs back, and putting it in
    // the motion vectors would smear it.
    vec3 bendNow = wdir * (amp * now);
    vec3 bendBefore = wdir * (amp * before);
    vec3 push = PusherOffset(root, inShade, inHeight);
    vec3 axis = root + bendNow + push;          // the blade's spine at this cross section
    // Billboard the blade about the vertical: expand its width along the direction across the view, so it always faces
    // the camera. A vertical blade seen from above is otherwise edge-on and vanishes.
    vec3 toCam = GF.camera.xyz - axis;
    vec2 vh = toCam.xz;
    float vl = length(vh);
    vec2 right = vl > 1e-3 ? vec2(-vh.y, vh.x) / vl : vec2(1.0, 0.0);
    vec3 across = vec3(right.x, 0.0, right.y) * (inAcross * fade);
    vec3 p = axis + across;
    vClip = GF.viewProj * vec4(p, 1.0);
    vPrevClip = GF.prevViewProj * vec4(axis - bendNow + bendBefore + across, 1.0);
    gl_Position = vClip;
    vPosW = inPos;                              // lighting and the shadow lookup: the blade's own spot
    // A per-blade normal, independent of the camera: one that followed the eye swept a band of shading across the
    // field as the camera turned (the dark wave). The blade is lit two-sided anyway, so which way it faces is moot.
    vNormal = inNormal;
    vUv = inUv;
    vShade = inShade;
    vTint = inColour.rgb;
}
