#version 450
// Phong tessellation (Boubekeur & Alexa 2008): a point of the triangle is projected onto the tangent plane of each
// corner (its averaged normal), the projections blended by the same barycentric weights - the surface bulges like the
// smooth normals say, rounding low-polygon silhouettes. D.tess.y: how much (0 = flat); each corner's pull also
// weighted by how smooth the model is there (t_vSmoothN's length: 0 at hard edges, which stay sharp). Then what the vertex shader
// computes from the world position: the clip position (with the TAA jitter), the motion vector inputs, fog distance.
#include "constants.glsl"
#include "ffp_varyings.glsl"

// Lower-left domain origin (pipeline): counter-clockwise here keeps the patch's winding.
layout(triangles, equal_spacing, ccw) in;

#define IN(type, name, loc) layout(location = loc) in type t_##name[];
#define OUT(type, name, loc) layout(location = loc) out type name;
#define LERP(type, name, loc) name = gl_TessCoord.x * t_##name[0] + gl_TessCoord.y * t_##name[1] + gl_TessCoord.z * t_##name[2];
FFP_VARYINGS(IN)
FFP_VARYINGS(OUT)

void main()
{
    FFP_VARYINGS(LERP)
    vec3 b = gl_TessCoord;
    vec3 p = vPosW, pull = vec3(0.0);
    for (int i = 0; i < 3; ++i) {
        float weight = length(t_vSmoothN[i]);
        if (weight <= 0.0) continue;
        vec3 n = t_vSmoothN[i] / weight;
        pull += b[i] * weight * (-dot(p - t_vPosW[i], n) * n);   // towards corner i's tangent plane
    }
    vec3 posW = p + D.tess.y * pull;
    vec3 delta = posW - p;
    vPosW = posW;
    vec4 pv = C.view * vec4(posW, 1.0);
    gl_Position = C.proj * pv;
    vClip = gl_Position;                         // motion vectors: without the jitter
    if ((C.flags.x & F_HDR) != 0u)
        gl_Position.xy += FL.taa.xy * gl_Position.w;
    vPrevClip += FL.prevViewProj * vec4(delta, 0.0);
    vFogDist = (C.flags.x & F_RANGEFOG) != 0u ? length(pv.xyz) : abs(pv.z);
}
