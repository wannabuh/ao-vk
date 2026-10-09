// Per-draw constants shared by ffp.vert / ffp.frag. Must match DrawConstants / DrawTransform / DrawRecord in
// rvk/internal.h (std430 - every member is vec4/mat4/uvec4, so std140 and std430 agree).
#include "frame_lights.glsl"

// Changes on nearly every draw, so it has its own record (see DrawRecord below); the big constant block is only
// rewritten when render state changes.
struct DrawTransform {
    mat4 world;                 // raw D3DMATRIX memory: GLSL M * v == D3D v * M
    mat4 prevWorld;             // the same object's world matrix last frame (motion vectors)
    vec4 motion;                // x: 1 = world camera, write its motion; y: 1 = last frame's vertex positions in
                                // binding 8; z: the draw's base vertex (gl_VertexIndex - z = its vertex)
    vec4 sway;                  // plants: model y of the base, 1 / model height, tip sway (world units), 1 = on
    uvec4 lightMask;            // frame lights (bits 0-63 of x, y) that reach the draw's bounding box; z: pushers near it
    vec4 tess;                  // characters' Phong tessellation: level (0 = off), shape (0..1), base vertex (binding 10)
    uvec4 texIdx;               // bindless (set 1): image slots for stage 0, stage 1, bump base, normal map
    uvec4 sampIdx;              // bindless (set 1): sampler slots for stage 0, stage 1, bump, normal
    uvec4 mat;                  // PBR material: occlusion/roughness/metallic image slot, the maps' sampler slot, MAT_* bits,
                                // emissive image slot
    vec4 leaf;                  // a canopy with leaves (leaves.cpp): its crown's centre (model space), radius (0 = none)
    uvec4 leafSet;              // ... its leaves in the pool: first, drawn now | falling-leaf slots << 16, all; the
                                // ground under it (world y, float bits: where its fallen leaves lie)
};
const uint MAT_PBR = 1u, MAT_BASE = 2u, MAT_ALBEDO = 4u, MAT_EMISSIVE = 8u, MAT_PICKED = 16u;   // D.mat.z: the draw has an
                                // ORM map; it is the ground base texture's; stage 0 draws an albedo map; it has an
                                // emissive map; its texture was picked (Ctrl+Shift+I: flashes yellow)

struct DrawConstants {
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
    uvec4 vtx;                  // FVF, colour scale (float bits; 0 = 1: the particles' brightness), normal map strength (float bits)
    uvec4 flags;                // F_* bits, fog vertex mode, fog table mode, alpha func
    uvec4 matSources;           // diffuse, ambient, specular, emissive material sources
    uvec4 stageA[2];            // colorop, colorarg1, colorarg2, alphaop
    uvec4 stageB[2];            // alphaarg1, alphaarg2, texcoordindex, texturetransformflags
    uvec4 lightInfo;            // light count, point + spot light count, frame light (index + 1) the draw carries
    Light lights[8];
};

// GPU-driven M2: one small record per draw (its transform plus which deduplicated constants it uses). The
// constants themselves are appended once per render-state change and shared, as before. A draw names its record
// through a push constant now; M3 replaces that with gl_DrawID (and passes it to the tess/fragment stages).
struct DrawRecord {
    uint constIndex;
    uint pad[3];
    DrawTransform d;
};

layout(set = 0, binding = 12, std430) readonly buffer DrawRecords { DrawRecord records[]; } gRecords;
layout(set = 0, binding = 0, std430) readonly buffer DrawConstantArray { DrawConstants consts[]; } gConsts;

// The draw's record, selected by the GPU: gl_InstanceIndex in the vertex shader (the draw sets firstInstance to
// the record's index), passed to the tess/fragment stages as a varying. M3 batches draws into one indirect call.
uint gRecord;
#define D (gRecords.records[gRecord].d)
#define C (gConsts.consts[gRecords.records[gRecord].constIndex])

// Bindless textures (M1): one array of every texture and one of every sampler; a draw names the four it uses by
// index. TEX0/TEX1 are the game's two texture stages, BUMPTEX the ground's base texture, NORMALTEX its normal map.
layout(set = 1, binding = 0) uniform texture2D texImages[4096];
layout(set = 1, binding = 1) uniform sampler bindlessSamplers[256];
#define TEX0 sampler2D(texImages[D.texIdx.x], bindlessSamplers[D.sampIdx.x])
#define TEX1 sampler2D(texImages[D.texIdx.y], bindlessSamplers[D.sampIdx.y])
#define BUMPTEX sampler2D(texImages[D.texIdx.z], bindlessSamplers[D.sampIdx.z])
#define NORMALTEX sampler2D(texImages[D.texIdx.w], bindlessSamplers[D.sampIdx.w])
#define ORMTEX sampler2D(texImages[D.mat.x], bindlessSamplers[D.mat.y])
#define EMISSIVETEX sampler2D(texImages[D.mat.w], bindlessSamplers[D.mat.y])

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
           F_EMISSIVE = 16777216u,   // unlit or self-lit 3D surface: its bright texels glow at night (FL.effects.y)
           F_CUTOUT = 33554432u,     // blended with depth writes: (nearly) see-through fragments dropped (vCutout)
           F_SHADOWCHEAP = 67108864u, // far foliage: one tap of the sun's shadow, no cascade blending
           F_VERTEXSUN = 134217728u,  // far plants lit per vertex: vMatAmbient = the sun's part, shadowed per pixel
           F_NORMALMAP = 268435456u, // the stage 0 texture's own normal map (binding 11; C.misc.w = strength)
           F_CHARACTER = 536870912u, // a character's body or part: lights characters carry don't shadow it
           F_NOALBEDO = 1073741824u, // the albedo attachment isn't written (a multiplying pass keeps it)
           F_BLENDED = 2147483648u;  // blended SRCALPHA / INVSRCALPHA: hides what is behind only where alpha is 1
