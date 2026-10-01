// Per-draw constants shared by ffp.vert / ffp.frag. Must match DrawConstants in rvk/device.cpp (std140).
struct Light {
    vec4 diffuse, specular, ambient;
    vec4 position;      // xyz world space, w = D3DLIGHTTYPE
    vec4 direction;     // xyz world space, w = range
    vec4 atten;         // attenuation0, attenuation1, attenuation2, falloff
    vec4 spot;          // cos(theta/2), cos(phi/2)
};

// Changes on nearly every draw, so it has its own small block (binding 3); the big block below is only
// rewritten when render state changes.
layout(set = 0, binding = 3, std140) uniform DrawTransform {
    mat4 world;                 // raw D3DMATRIX memory: GLSL M * v == D3D v * M
} D;

layout(set = 0, binding = 0, std140) uniform DrawConstants {
    mat4 view, proj;
    mat4 texMatrix[2];
    vec4 viewport;              // D3D viewport x, y, width, height (pixels)
    vec4 matDiffuse, matAmbient, matSpecular, matEmissive;
    vec4 ambient;               // D3DRENDERSTATE_AMBIENT
    vec4 fogColor;
    vec4 fogParams;             // start, end, density
    vec4 tfactor;
    vec4 misc;                  // material power, alpha reference (0..255)
    vec4 eyePos;                // camera position, world space
    vec4 eyeDir;                // camera forward, world space (non-local viewer)
    uvec4 vtx;                  // FVF
    uvec4 flags;                // F_* bits, fog vertex mode, fog table mode, alpha func
    uvec4 matSources;           // diffuse, ambient, specular, emissive material sources
    uvec4 stageA[2];            // colorop, colorarg1, colorarg2, alphaop
    uvec4 stageB[2];            // alphaarg1, alphaarg2, texcoordindex, texturetransformflags
    uvec4 lightInfo;            // light count
    Light lights[8];
} C;

const uint F_LIGHTING = 1u, F_COLORVERTEX = 2u, F_SPECULAR = 4u, F_NORMALIZE = 8u, F_FOG = 16u,
           F_RANGEFOG = 32u, F_LOCALVIEWER = 64u, F_TEX0 = 128u, F_TEX1 = 256u, F_ALPHATEST = 512u,
           F_PERPIXEL = 1024u;    // lighting evaluated in ffp.frag (set together with F_LIGHTING)
