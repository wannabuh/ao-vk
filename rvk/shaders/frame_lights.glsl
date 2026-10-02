// The per-frame light block (binding 4), shared by the scene shaders (constants.glsl) and the volumetric light.
// Must match FrameLights in rvk/internal.h (std140).
struct Light {
    vec4 diffuse, specular, ambient;
    vec4 position;      // xyz world space, w = D3DLIGHTTYPE
    vec4 direction;     // xyz world space, w = range
    vec4 atten;         // attenuation0, attenuation1, attenuation2, falloff
    vec4 spot;          // cos(theta/2), cos(phi/2), frame lights: point shadow cube + 1 (0 = none), its fade-in (0..1)
};

// The frame's active point / spot lights nearest the camera (Device::SetLightOverride). Replaces the game's
// per-object choice of up to 8 lights, which drops lights on big objects such as the ground.
layout(set = 0, binding = 4, std140) uniform FrameLights {
    uvec4 info;                 // light count, pusher count
    mat4 shadowViewProj[4];     // world -> each sun shadow cascade (raw D3DMATRIX memory)
    vec4 cascadeTexel;          // world size of a texel of each cascade
    vec4 cascadeDepth;          // world units per unit of each cascade's depth (soft shadows)
    vec4 effects;               // light through leaves, night glow (x darkness), sun shadow softness, plant push
    vec4 wind;                  // plants' sway: direction x, z, time (s), strength
    vec4 taa;                   // temporal anti-aliasing: this frame's jitter (clip x, y per w), noise offset (0..1), the wind's time last frame
    vec4 shadowParams;          // enabled, strength, cascade count, point light shadow strength
    vec4 sunDir;                // direction the sunlight travels; w = light headroom (F_OVERBRIGHT)
    vec4 sunColor;              // the sun's colour (shadow-casting sun; 0 = none)
    mat4 prevViewProj;          // the world camera last frame (motion vectors; raw D3DMATRIX memory)
    vec4 pushers[16];           // what plants bend away from (characters and their trails): world xyz, seconds since
    vec4 pusherBorn[4];         // ... and seconds since each point was made (4 a vector)
    Light lights[64];
} FL;
