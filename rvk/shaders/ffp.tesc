#version 450
// Phong tessellation of characters (D.tess): every triangle split evenly at the draw's level, the vertex shader's
// outputs passed on per corner.
#include "constants.glsl"
#include "ffp_varyings.glsl"

layout(vertices = 3) out;

#define IN(type, name, loc) layout(location = loc) in type name[];
#define OUT(type, name, loc) layout(location = loc) out type t_##name[];
#define COPY(type, name, loc) t_##name[gl_InvocationID] = name[gl_InvocationID];
FFP_VARYINGS(IN)
FFP_VARYINGS(OUT)

void main()
{
    FFP_VARYINGS(COPY)
    if (gl_InvocationID == 0) {
        // One level for every edge of the draw: an edge shared by two triangles is split alike on both (no cracks).
        float level = max(D.tess.x, 1.0);
        gl_TessLevelOuter[0] = gl_TessLevelOuter[1] = gl_TessLevelOuter[2] = level;
        gl_TessLevelInner[0] = level;
    }
}
