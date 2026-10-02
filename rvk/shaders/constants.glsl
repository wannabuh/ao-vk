// Per-draw constants shared by ffp.vert / ffp.frag. Must match DrawConstants in rvk/device.cpp (std140).
#include "frame_lights.glsl"

// Changes on nearly every draw, so it has its own small block (binding 3); the big block below is only
// rewritten when render state changes.
layout(set = 0, binding = 3, std140) uniform DrawTransform {
    mat4 world;                 // raw D3DMATRIX memory: GLSL M * v == D3D v * M
    mat4 prevWorld;             // the same object's world matrix last frame (motion vectors)
    vec4 motion;                // x: 1 = world camera, write its motion; y: 1 = last frame's vertex positions in
                                // binding 8; z: the draw's base vertex (gl_VertexIndex - z = its vertex)
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
    vec4 misc;                  // material power, alpha reference (0..255), effect glow gain, bump strength
    vec4 eyePos;                // camera position, world space
    vec4 eyeDir;                // camera forward, world space (non-local viewer)
    uvec4 vtx;                  // FVF, colour scale (float bits; 0 = 1: the particles' brightness)
    uvec4 flags;                // F_* bits, fog vertex mode, fog table mode, alpha func
    uvec4 matSources;           // diffuse, ambient, specular, emissive material sources
    uvec4 stageA[2];            // colorop, colorarg1, colorarg2, alphaop
    uvec4 stageB[2];            // alphaarg1, alphaarg2, texcoordindex, texturetransformflags
    uvec4 lightInfo;            // light count, point + spot light count, frame light (index + 1) the draw carries
    Light lights[8];
} C;


const uint F_LIGHTING = 1u, F_COLORVERTEX = 2u, F_SPECULAR = 4u, F_NORMALIZE = 8u, F_FOG = 16u,
           F_RANGEFOG = 32u, F_LOCALVIEWER = 64u, F_TEX0 = 128u, F_TEX1 = 256u, F_ALPHATEST = 512u,
           F_PERPIXEL = 1024u,    // lighting evaluated in ffp.frag (set together with F_LIGHTING)
           F_DEBUGLIGHT = 2048u,  // tint draws by how they are lit (Device::SetLightingDebug)
           F_LIGHTOVERRIDE = 4096u, // point / spot lights come from FL (the frame's nearest lights), not C.lights
           F_SHADOW = 8192u,        // receives sun shadows (FL.shadow*, shadow map at binding 5)
           F_SHADOWCOMP = 16384u,   // multiplies a shadowed surface: local lights divided by its shadow factor
           F_SHADOWTEX = 32768u,    // the shadow darkens texture stage 0 (the ground's lightmap) only
           F_OVERBRIGHT = 65536u,   // frame lights may light beyond the game's clamp, up to FL.sunDir.w
           F_OVERBRIGHT2X = 131072u, // multiplying pass blended at 2x (DESTCOLOR/SRCCOLOR): output halved, up to 2
           F_HDR = 262144u,          // drawn into the HDR scene (float): colours above 1 are kept
           F_GLOW = 524288u,         // an additive effect: also adds itself (x misc.z) to the glow attachment
           F_GLOWALPHA = 1048576u,   // ... blended SRCALPHA: its contribution is colour x alpha
           F_BUMP = 2097152u,        // per-pixel lighting with a normal generated from texture 0 (C.misc.w = strength)
           F_BUMPBASE = 4194304u,    // ... from the ground's base texture (binding 7, coordinate set 0) instead
           F_FOLIAGE = 8388608u,     // lit with a cut-out texture: sunlight through it (FL.effects.x) if it has holes
           F_EMISSIVE = 16777216u;   // unlit or self-lit 3D surface: its bright texels glow at night (FL.effects.y)
