#version 450
// The water's surface (rvk/water.cpp). Grid mode: vertex v of a polar grid around the camera - ring v / segments
// (ring 0 the centre), segment v % segments - placed on the water map's surface and moved by the waves. Mesh mode
// (W.time.z = 1): the game's own triangles (its steep ones: falls), from the vertex buffer as they are.
#include "water_common.glsl"

struct MeshVertex { float x, y, z; uint colour; };
layout(set = 0, binding = 2, std430) readonly buffer Mesh { MeshVertex meshVertices[]; };

layout(location = 0) out vec3 vPos;         // world position (displaced)
layout(location = 1) out vec3 vNormal;      // the waves' normal (world)
layout(location = 2) out vec4 vClip;
layout(location = 3) out vec4 vPrevClip;
layout(location = 4) out vec3 vRest;        // where the surface is at rest (xz on the map; y its height)
layout(location = 5) out float vFold;       // the waves' compression (foam on the crests)
layout(location = 6) out float vSpacing;    // the grid's spacing here (world)

// How much the waves calm down where the water is shallow - judged from the scene's depth (behind the water, at the
// rest point) where it is on the screen; elsewhere not at all.
float Shallow(vec3 rest)
{
    vec4 c = W.viewProj * vec4(rest, 1.0);
    if (c.w <= 0.05)
        return 1.0;
    vec2 ndc = c.xy / c.w;
    if (any(greaterThan(abs(ndc), vec2(1.0))))
        return 1.0;
    vec2 uv = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    float d = textureLod(sceneDepth, uv, 0.0).r;
    if (d >= 1.0)
        return 1.0;
    float behind = ViewZ(d) - c.w;              // how far the view ray goes on under the surface
    return smoothstep(0.0, 2.5, behind);
}

void main()
{
    vec3 p;
    vec3 n = vec3(0.0, 1.0, 0.0);
    float fold = 1.0, spacing = 0.0;
    vec3 prevP;
    if (W.time.z > 0.5) {
        MeshVertex m = meshVertices[gl_VertexIndex];
        p = vec3(m.x, m.y, m.z);
        prevP = p;
        vRest = p;
    } else {
        uint segments = uint(W.grid.x);
        uint ring = uint(gl_VertexIndex) / segments, seg = uint(gl_VertexIndex) % segments;
        float r = ring == 0u ? 0.0 : W.grid.z * pow(W.grid.w, float(ring - 1u));
        float a = (float(seg) + 0.5 * float(ring & 1u)) / float(segments) * 2.0 * kPi;
        vec2 xz = W.camera.xz + r * vec2(cos(a), sin(a));
        spacing = max(r * (W.grid.w - 1.0), 2.0 * kPi * r / float(segments)) + 0.05;
        float h = W.mapInfo.z > 0.5 ? textureLod(waterMap, MapUv(xz), 0.0).r : 0.0;
        vec3 rest = vec3(xz.x, W.mapInfo.x + h, xz.y);
        vRest = rest;
        // Calmer in the shallows and near the water's edge on the map.
        float edge = W.mapInfo.z > 0.5 ? smoothstep(0.5, 1.0, textureLod(waterMap, MapUv(xz), 0.0).g) : 1.0;
        float scale = W.wind.z * edge * mix(0.25, 1.0, Shallow(rest));
        vec3 dn, dummyN;
        float dummyF;
        vec3 off = Waves(xz, W.time.x, spacing, scale, dn, fold);
        vec3 offPrev = Waves(xz, W.time.y, spacing, scale, dummyN, dummyF);
        p = rest + off;
        prevP = rest + offPrev;
        n = dn;
    }
    vPos = p;
    vNormal = n;
    vFold = fold;
    vSpacing = spacing;
    vClip = W.viewProj * vec4(p, 1.0);
    vPrevClip = W.prevViewProj * vec4(prevP, 1.0);
    gl_Position = vClip;
    if (W.camera.w > 0.5)
        gl_Position.xy += FL.taa.xy * gl_Position.w;   // the TAA's sub-pixel offset, as every scene draw has
}
