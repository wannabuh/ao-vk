// The water (rvk/water.cpp): the per-draw block, the water map and the waves - shared by water.vert and water.frag.
//
// The game's water is a few huge flat triangles per body. We draw a camera-centred polar grid instead (dense near the
// camera, coarser with distance: no cracks, one mesh), lifted onto the water by the water map - a top-down image of
// the game's triangles: R the surface's height (relative to W.mapInfo.x), G whether there is water there - and moved
// by a sum of Gerstner waves. Small ripples are normals only (the detail texture, in water.frag).
#include "frame_lights.glsl"                    // FL (binding 4): the sun, its cascades, the TAA jitter

layout(set = 0, binding = 0) uniform WaterFrame {
    mat4 viewProj;      // world -> clip (D3D row-vector convention, as the scene's)
    mat4 prevViewProj;  // ... last frame (motion vectors)
    mat4 view;          // world -> view
    vec4 proj;          // D3D projection m[2][2], m[3][2], m[0][0], m[1][1]
    vec4 camera;        // xyz: the eye; w: 1 = apply the TAA jitter
    vec4 viewport;      // target width, height, 1 / width, 1 / height
    vec4 time;          // x: seconds (wrapped hourly); y: last frame's; z: 1 = mesh mode (the game's triangles); w: the
                        // reflection's ray steps
    vec4 wind;          // xy: the wind's direction (unit); z: wave height scale; w: wavelength scale
    vec4 map;           // xy: the water map's world x, z origin; zw: 1 / its world size in x, z
    vec4 mapInfo;       // x: base height; y: texel size (world); z: 1 = a map is bound
    vec4 grid;          // x: angular segments; y: rings; z: first ring radius; w: ring radius ratio
    vec4 sunColour;     // rgb: the sun (0 = none)
    vec4 sunDir;        // xyz: the direction the sunlight travels
    vec4 ambient;       // rgb: the terrain's ambient light
    vec4 tint;          // rgba: the game's water colour (its vertex colour)
    vec4 look;          // x: style (0 = enhanced AO, 1 = realistic); y: reflections; z: refraction; w: clarity
    vec4 look2;         // x: foam; y: caustics; z: ripples (detail normals); w: the game's texture on the surface
    vec4 fogColour;     // rgb; w: 1 = fog on
    vec4 fogParams;     // start, end, density, table mode (D3DFOG_*: 1 exp, 2 exp2, 3 linear; 0 = none)
    vec4 sky;           // rgb: the sky's colour overhead (from the frame's sky pixels; 0 = unknown); w: unused
} W;

layout(set = 0, binding = 7) uniform sampler2D waterMap;       // R: height above W.mapInfo.x, G: water (0 / 1)
layout(set = 0, binding = 8) uniform sampler2D sceneDepth;     // the scene's depth before the water (a copy)

const float kPi = 3.14159265;

vec2 MapUv(vec2 xz) { return (xz - W.map.xy) * W.map.zw; }

// The linear view depth of a depth buffer value.
float ViewZ(float d) { return W.proj.y / (min(d, 0.9999999) - W.proj.x); }

// The waves: a sum of Gerstner waves around the wind's direction. Each wave is faded out where the grid is too coarse
// for it (spacing: the grid's vertex spacing there), so distant water doesn't alias into noise. Returns the
// displacement; n: the surface normal (unnormalised); fold: the surface's compression (1 flat, near 0 at a sharp
// crest: where the foam gathers).
const int kWaves = 8;
vec3 Waves(vec2 xz, float t, float spacing, float scale, out vec3 n, out float fold)
{
    // Wavelengths (world units, x W.wind.w), angles off the wind (radians), relative heights, phases.
    const float lambda[kWaves] = float[](31.0, 23.0, 17.0, 11.5, 7.7, 5.3, 3.7, 2.6);
    const float angle[kWaves] = float[](0.0, 0.55, -0.42, 0.95, -0.8, 0.25, -1.2, 1.35);
    const float height[kWaves] = float[](1.0, 0.75, 0.6, 0.42, 0.3, 0.2, 0.13, 0.09);
    const float phase[kWaves] = float[](0.0, 1.7, 4.1, 2.3, 5.5, 0.9, 3.3, 6.0);
    vec3 offset = vec3(0.0);
    vec3 sum = vec3(0.0);                       // -dX, dY (for the normal's y), -dZ
    float steep = 0.0;
    vec2 wd = W.wind.xy;
    for (int i = 0; i < kWaves; ++i) {
        float L = lambda[i] * W.wind.w;
        float fade = 1.0 - smoothstep(L * 0.18, L * 0.4, spacing);
        if (fade <= 0.0)
            continue;
        float k = 2.0 * kPi / L;
        float omega = sqrt(9.81 * k);
        float ca = cos(angle[i]), sa = sin(angle[i]);
        vec2 d = vec2(wd.x * ca - wd.y * sa, wd.x * sa + wd.y * ca);
        float A = 0.012 * L * height[i] * scale * fade;   // steepness ~0.075 x scale at the longest wave
        float Q = 0.55;                                  // how sharp the crests are (0 = sine waves)
        float th = k * dot(d, xz) - omega * t + phase[i];
        float c = cos(th), s = sin(th);
        offset += vec3(Q * A * d.x * c, A * s, Q * A * d.y * c);
        sum += vec3(d.x * k * A * c, Q * k * A * s, d.y * k * A * c);
        steep += Q * k * A * s;
    }
    n = vec3(-sum.x, 1.0 - sum.y, -sum.z);
    fold = 1.0 - steep;
    return offset;
}
