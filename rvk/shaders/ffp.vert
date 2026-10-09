#version 450
// Direct3D 7 fixed-function vertex processing.
#include "constants.glsl"
// Point light shadows are only looked up per pixel (ffp.frag); per-vertex lighting uses the game's lights, which have none.
float PointShadow(Light l, vec3 posW, vec3 normalW, float nl) { return 1.0; }
#include "lighting.glsl"

#ifndef RVK_LEAF
layout(location = 0) in vec4 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inDiffuse;
layout(location = 3) in vec4 inSpecular;
layout(location = 4) in vec4 inTex0;
layout(location = 5) in vec4 inTex1;
#else
// leaf.vert (the same shader built with RVK_LEAF): no vertex input - the leaves of a canopy (leaves.cpp), six vertices
// (two triangles) a leaf, made from the leaf's record in binding 10 into what the canopy's own vertices would carry.
vec4 inPos, inDiffuse = vec4(1.0), inSpecular = vec4(0.0), inTex0, inTex1 = vec4(0.0);
vec3 inNormal;
#endif

layout(location = 0) out vec4 vDiffuse;
layout(location = 1) out vec4 vSpecular;
layout(location = 2) out vec4 vTex0;
layout(location = 3) out vec4 vTex1;
layout(location = 4) out float vFogDist;     // view-space distance (table fog)
layout(location = 5) out float vFogFactor;   // vertex fog factor
// Per-pixel lighting inputs (F_PERPIXEL); vDiffuse / vSpecular then carry the material diffuse / specular.
layout(location = 6) out vec3 vMatAmbient;
layout(location = 7) out vec3 vMatEmissive;
layout(location = 8) out vec3 vPosW;
layout(location = 9) out vec4 vNormalW;      // xyz direction, w = length the vertex path would light with
layout(location = 10) out vec2 vSet0;        // texture coordinate set 0 as is (the ground's base texture, F_BUMPBASE)
layout(location = 11) out vec4 vClip;        // motion vectors: clip position now
layout(location = 12) out vec4 vPrevClip;    // ... and last frame (previous world matrix and camera)
layout(location = 13) out float vCutout;     // F_CUTOUT: alpha below which the fragment is dropped
layout(location = 14) out vec3 vSmoothN;     // tessellated draws (D.tess.x): the normal averaged over the vertices at
                                             // this position (world), which the Phong shape follows - no cracks at
                                             // hard edges, where a position has several normals
layout(location = 15) flat out uint vRecord; // the draw's record index (M3: gl_InstanceIndex), for the later stages
// The depth pre-pass (prepass.cpp) runs this shader without a fragment stage: the same position to the bit, so the
// scene's draw passes its own pre-pass depth on equal.
invariant gl_Position;
#ifndef RVK_LEAF
// Tessellated draws: per vertex the averaged normal (model space, 3 floats), from the draw's base vertex D.tess.z.
layout(set = 0, binding = 10, std430) readonly buffer SmoothNormals { float smoothN[]; } SN;
#else
// The leaf pool (leaves.cpp LeafRecord), two words of four a leaf:
//   a.xyz: the leaf's centre (model space)   a.w: half its width along the texture's u (model space) x, y (half x 2)
//   b.x: ... z, half its height along v x    b.y: ... y, z    b.z: the centre in the texture (unorm16 x 2)
//   b.w: the half size in the texture, u and v (unorm12 x 0.25 each), its phase (unorm8)
layout(set = 0, binding = 10, std430) readonly buffer Leaves { uvec4 words[]; } LV;
#endif
// Last frame's vertex positions of an animated (CPU-skinned) mesh, model space, 3 floats a vertex (D.motion.y).
layout(set = 0, binding = 8, std430) readonly buffer PrevPositions { float prevPos[]; } PP;
// The plants' sway reads texture stage 0, bindless now (TEX0 in constants.glsl): swayTex is that sampler.
#define swayTex TEX0
#include "sway.glsl"
#include "wind.glsl"

// Plants bending out of the way of characters (FL.pushers: where they are and the trail behind them, w = seconds since
// a character was there; FL.pusherBorn: seconds since the point was made - fresh while the character walks on;
// Device::FillPushers). The nearer a character, the further a plant bends away from it - each vertex on its own, so a
// quad's top parts around the legs while its base stays. Behind a character a plant springs back a little past upright
// and settles (a well-damped spring); while one walks through, the plants it touches rustle gently. A vertex moves at
// most a fixed distance (world units, not an angle: a tall plant leans just enough to clear the character instead of
// swinging its whole height), and down so it keeps its distance from the base. sway as SwayDistance; plantHeight in
// world units.
float PushSpring(float age)
{
    float t = max(age - 0.08, 0.0);
    return exp(-6.0 * t) * cos(7.0 * t);         // overshoot ~7%
}

vec3 PushOffset(vec3 posW, vec3 modelPos, vec4 sway, float plantHeight, vec2 originXZ, float holes)
{
    uint mask = D.lightMask.z & ((1u << min(FL.info.y, 16u)) - 1u);   // the pushers near this draw (CPU)
    float amount = FL.effects.w;
    if (mask == 0u || amount <= 0.0 || holes <= 0.0) return vec3(0.0);
    float h = SwayHeight(modelPos, sway);
    if (h <= 0.0) return vec3(0.0);
    float reach = 1.4 * sqrt(amount);
    float phase = dot(originXZ, vec2(3.1, 2.3));
    vec2 push = vec2(0.0), rustle = vec2(0.0);
    while (mask != 0u) {
        uint i = uint(findLSB(mask));
        mask &= mask - 1u;
        vec4 p = FL.pushers[i];
        float dy = posW.y - p.y;
        if (dy < -1.0 || dy > 3.0) continue;     // a plant on another floor, or a character standing on a roof
        vec2 d = posW.xz - p.xz;
        float dist = length(d);
        if (dist >= reach) continue;
        float near = 1.0 - smoothstep(0.15 * reach, reach, dist);
        vec2 dir = dist > 1e-3 ? d / dist : vec2(0.7071);
        push += dir * near * PushSpring(p.w);
        float born = FL.pusherBorn[i >> 2u][i & 3u];
        float moving = exp(-3.0 * born) * exp(-4.0 * max(p.w - 0.1, 0.0));
        rustle += vec2(-dir.y, dir.x) * near * moving * sin(FL.wind.z * 11.0 + phase + float(i));
    }
    vec2 v = push + 0.12 * rustle;
    float len = length(v);
    if (len <= 1e-4) return vec3(0.0);
    vec2 dir = v / len;
    float above = h * plantHeight;               // the vertex's height above the base
    float most = min(0.45 * amount, 0.7 * plantHeight);   // the top's furthest lean
    float lean = most * min(len, 1.0) * holes * h * sqrt(h);   // bends most up top
    lean = min(lean, 0.9 * above);
    return vec3(dir.x * lean, -(above - sqrt(max(above * above - lean * lean, 0.0))), dir.y * lean);
}

// Leaves: the branches swaying (a canopy with leaves, D.leaf.w > 0: its own cards and its leaves alike, so the leaves
// stay on them). Smooth over the crown - neighbouring sprigs on one branch move together - growing out from its centre,
// a slow swing with the wind and a little bob; world units, at the wind's `time`, gusts at the object's position.
vec3 BranchSway(vec3 modelPos, float time, vec2 originXZ)
{
    return BranchSwayAt(modelPos, D.leaf, originXZ, FL.wind.xy, time, FL.leaves.x * FL.wind.w, FL.leaves.z);
}

// A canopy with leaves: how much of it the leaves stand for at this distance - all of it up to half the leaves'
// distance, none beyond it (Device::CanopyParams draws that share of its leaves). Its own cards are cut back to an
// inner core by as much.
float LeafKeep()
{
    vec3 c = (D.world * vec4(D.leaf.xyz, 1.0)).xyz;
    float r = D.leaf.w * length(D.world[1].xyz);
    float d = max(length(C.eyePos.xyz - c) - r, 0.0), reach = FL.leafView.x;
    float keep = clamp((reach - d) / (0.5 * reach), 0.0, 1.0);
    return keep * keep;
}

// The crown's own shade: leaves and twigs deep inside it get less of the sky's light (and the sun's) than those on its
// outside - darker towards its centre, a little lighter at its top.
float CrownShade(vec3 modelPos)
{
    vec3 d = (modelPos - D.leaf.xyz) / D.leaf.w;
    float r = length(d);
    return mix(0.72, 1.0, smoothstep(0.15, 0.95, r)) * (1.0 + 0.06 * clamp(d.y, -1.0, 1.0));
}

#ifdef RVK_LEAF
const vec2 kLeafCorner[6] = vec2[6](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
                                    vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));
vec3 gLeafPrev;                                  // the vertex's model position last frame (its flutter then)
vec2 gLeafCorner;                                // where on the card (-1 .. 1)

// A leaf's flutter: its card flapping about its middle, the far edge most (model units, along the card's normal).
float LeafFlutter(float phase, float corner, float time, float gust)
{
    return sin(time * (6.0 + 2.0 * fract(phase * 7.0)) + phase * 6.2831853) * (0.15 + 0.45 * gust) * corner;
}

uint LeafHash(uint x)
{
    x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15; x *= 0x846CA68Bu; x ^= x >> 16;
    return x;
}
float LeafHashU(uint x) { return float(LeafHash(x) >> 8) / 16777216.0; }

// A falling leaf (FL.leafView.z, RVK_LeafFall): each of a canopy's slots (D.leafSet.w) now and then - likelier in a
// gust - takes one of its leaves, which drifts down with the wind, tumbling and spiralling, lies on the ground (at the
// object's origin height) a few seconds and shrinks away. A function of the time only, so its motion last frame is the
// same function a frame earlier. Its card's corner in model space, its normal and texture coordinate; false: no leaf
// in the slot now (the card collapses).
bool FallingLeaf(uint slot, vec2 corner, float time, out vec3 pos, out vec3 normal, out vec2 uv)
{
    pos = vec3(0.0);
    normal = vec3(0.0, 1.0, 0.0);
    uv = vec2(0.0);
    if (D.leafSet.z == 0u || FL.leafView.z <= 0.0) return false;
    const float kCycle = 9.0, kLie = 5.0;
    uint seed = LeafHash(slot * 0x9E3779B9u ^ floatBitsToUint(D.world[3].x) ^ (floatBitsToUint(D.world[3].z) << 1u));
    float t = time + LeafHashU(seed) * kCycle;
    float cycle = floor(t / kCycle), age = t - cycle * kCycle;
    uint cs = LeafHash(seed ^ (uint(cycle) * 0x85EBCA6Bu));
    vec2 wd = FL.wind.xy;
    float gust = Gust(D.world[3].xz, wd, time - age) * FL.leaves.z;
    if (LeafHashU(cs) >= clamp(FL.leafView.z * (0.3 + 1.2 * gust), 0.0, 1.0)) return false;
    uint src = D.leafSet.x + LeafHash(cs ^ 0x51ED27u) % D.leafSet.z;
    uvec4 a = LV.words[2u * src], b = LV.words[2u * src + 1u];
    vec2 h0 = unpackHalf2x16(a.w), h1 = unpackHalf2x16(b.x), h2 = unpackHalf2x16(b.y);
    mat3 W = mat3(D.world), invW = inverse(W);
    vec3 start = (D.world * vec4(uintBitsToFloat(a.xyz), 1.0)).xyz;
    vec3 u = W * vec3(h0, h1.x), v = W * vec3(h1.y, h2);
    float height = max(start.y - D.world[3].y, 0.0);
    float speed = 0.7 + 0.5 * LeafHashU(cs ^ 7u);    // world units a second
    float fallTime = height / speed;
    if (age >= fallTime + kLie) return false;
    float flying = min(age, fallTime), drift = 0.5 + 0.9 * gust;
    float spin = flying * (2.0 + 2.0 * LeafHashU(cs ^ 11u)) + 6.2831853 * LeafHashU(cs ^ 23u);
    vec3 off = vec3(wd.x, 0.0, wd.y) * drift * flying + vec3(cos(spin), 0.0, sin(spin)) * 0.35 * min(flying, 1.0);
    off.y = -speed * flying;
    if (age < fallTime) {                        // tumbling as it falls
        vec3 k = normalize(vec3(LeafHashU(cs ^ 17u) - 0.5, 0.35, LeafHashU(cs ^ 19u) - 0.5));
        float angle = age * (2.5 + 3.0 * LeafHashU(cs ^ 13u));
        float c = cos(angle), s = sin(angle);
        u = u * c + cross(k, u) * s + k * dot(k, u) * (1.0 - c);
        v = v * c + cross(k, v) * s + k * dot(k, v) * (1.0 - c);
    } else {                                     // lying flat on the ground, shrinking away over its last second
        float lu = length(u), lv = length(v);
        vec3 flat_ = vec3(u.x, 0.0, u.z);
        flat_ = dot(flat_, flat_) > 1e-8 ? normalize(flat_) : vec3(1.0, 0.0, 0.0);
        float fade = clamp(fallTime + kLie - age, 0.0, 1.0);
        u = flat_ * lu * fade;
        v = normalize(cross(vec3(0.0, 1.0, 0.0), flat_)) * lv * fade;
        off.y += 0.03;
    }
    vec3 world = start + off + corner.x * u + corner.y * v;
    pos = invW * (world - D.world[3].xyz);
    vec3 n = cross(u, v);
    n = dot(n, n) > 0.0 ? normalize(n) : vec3(0.0, 1.0, 0.0);
    normal = normalize(invW * (n.y < 0.0 ? -n : n));
    vec2 uvHalf = vec2(float(b.w & 0xFFFu), float((b.w >> 12u) & 0xFFFu)) * (0.25 / 4095.0);
    uv = unpackUnorm2x16(b.z) + corner * uvHalf;
    return true;
}

void LeafVertex(vec2 originXZ)
{
    uint leaf = uint(gl_VertexIndex) / 6u;
    vec2 corner = kLeafCorner[uint(gl_VertexIndex) % 6u];
    gLeafCorner = corner;
    uint local = leaf - D.leafSet.x;
    if (local >= D.leafSet.y) {                  // past the drawn leaves: the falling-leaf slots
        vec3 now, prev, n, nPrev;
        vec2 uv, uvPrev;
        bool on = FallingLeaf(local - D.leafSet.y, corner, FL.wind.z, now, n, uv);
        bool wasOn = FallingLeaf(local - D.leafSet.y, corner, FL.taa.w, prev, nPrev, uvPrev);
        inPos = vec4(on ? now : D.leaf.xyz, 1.0);
        gLeafPrev = wasOn && on ? prev : inPos.xyz;
        inNormal = n;
        inTex0 = vec4(uv, 0.0, 1.0);
        return;
    }
    uvec4 a = LV.words[2u * leaf], b = LV.words[2u * leaf + 1u];
    vec3 centre = uintBitsToFloat(a.xyz);
    vec2 h0 = unpackHalf2x16(a.w), h1 = unpackHalf2x16(b.x), h2 = unpackHalf2x16(b.y);
    vec3 halfU = vec3(h0, h1.x), halfV = vec3(h1.y, h2);
    vec2 uv = unpackUnorm2x16(b.z);
    vec2 uvHalf = vec2(float(b.w & 0xFFFu), float((b.w >> 12u) & 0xFFFu)) * (0.25 / 4095.0);
    float phase = float(b.w >> 24u) / 255.0;
    vec3 n = cross(halfU, halfV);
    n = dot(n, n) > 0.0 ? normalize(n) : vec3(0.0, 1.0, 0.0);
    vec3 out_ = centre - D.leaf.xyz;
    if (dot(n, out_) < 0.0) n = -n;              // facing out of the crown
    vec3 pos = centre + corner.x * halfU + corner.y * halfV;
    float size = length(halfV);
    float gust = Gust(originXZ, FL.wind.xy, FL.wind.z) * FL.leaves.z;
    float gustPrev = Gust(originXZ, FL.wind.xy, FL.taa.w) * FL.leaves.z;
    float f = FL.leaves.y * size;
    float tip = 0.5 + 0.5 * corner.y;
    inPos = vec4(pos + n * f * LeafFlutter(phase, tip, FL.wind.z, gust), 1.0);
    gLeafPrev = pos + n * f * LeafFlutter(phase, tip, FL.taa.w, gustPrev);
    // Lit as part of a rounded crown, not as a flat card: its normal leans out from the crown's centre.
    float r = length(out_);
    inNormal = normalize(n + (r > 1e-4 ? out_ / r : vec3(0.0, 1.0, 0.0)) * 1.2);
    inTex0 = vec4(uv + corner * uvHalf, 0.0, 1.0);
}
#endif

float FogFactor(uint mode, float d)
{
    if (mode == 3u) return clamp((C.fogParams.y - d) / max(C.fogParams.y - C.fogParams.x, 1e-6), 0.0, 1.0);
    if (mode == 1u) return clamp(exp(-C.fogParams.z * d), 0.0, 1.0);
    if (mode == 2u) { float x = C.fogParams.z * d; return clamp(exp(-x * x), 0.0, 1.0); }
    return 1.0;
}

vec4 MaterialColor(uint source, vec4 material, vec4 diffuse, vec4 specular, bool hasDiffuse, bool hasSpecular)
{
    if ((C.flags.x & F_COLORVERTEX) != 0u) {
        if (source == 1u && hasDiffuse) return diffuse;
        if (source == 2u && hasSpecular) return specular;
    }
    return material;
}

int TexCoordSize(uint fvf, uint set)
{
    uint code = (fvf >> (16u + 2u * set)) & 3u;
    return code == 0u ? 2 : code == 1u ? 3 : code == 2u ? 4 : 1;
}

vec4 TexCoord(uint stage, uint fvf, vec3 posV, vec3 normalV)
{
    uint tci = C.stageB[stage].z;
    uint ttf = C.stageB[stage].w;
    uint mode = tci & 0xFFFF0000u;
    uint set = tci & 0xFFFFu;
    vec4 c;
    int size;
    if (mode == 0x10000u) { c = vec4(normalV, 0.0); size = 3; }
    else if (mode == 0x20000u) { c = vec4(posV, 0.0); size = 3; }
    else if (mode == 0x30000u) { c = vec4(reflect(normalize(posV), normalV), 0.0); size = 3; }
    else { c = set == 0u ? inTex0 : inTex1; size = TexCoordSize(fvf, set); }
    if ((ttf & 0xFFu) == 0u)
        return c;
    // D3D pads the coordinate with a 1 after its last component before the texture matrix.
    vec4 p = size == 1 ? vec4(c.x, 1.0, 0.0, 0.0) : size == 2 ? vec4(c.xy, 1.0, 0.0)
           : size == 3 ? vec4(c.xyz, 1.0) : c;
    return C.texMatrix[stage] * p;
}

void main()
{
    gRecord = uint(gl_InstanceIndex);            // the draw sets firstInstance to its record index (M3)
    vRecord = gRecord;
#ifdef RVK_LEAF
    LeafVertex(D.world[3].xz);
#endif
    uint fvf = C.vtx.x;
    bool rhw = (fvf & 0xEu) == 4u;
    bool hasNormal = (fvf & 0x10u) != 0u;
    bool hasDiffuse = (fvf & 0x40u) != 0u;
    bool hasSpecular = (fvf & 0x80u) != 0u;
    vec4 diffuse = hasDiffuse ? inDiffuse : vec4(1.0);
    vec4 specular = hasSpecular ? inSpecular : vec4(0.0);
    vec3 posV = vec3(0.0), normalV = vec3(0.0, 0.0, 1.0);
    vMatAmbient = vec3(0.0);
    vMatEmissive = vec3(0.0);
    vPosW = vec3(0.0);
    vNormalW = vec4(0.0);

    vClip = vec4(0.0, 0.0, 0.0, 1.0);
    vPrevClip = vClip;
    vCutout = 0.02;
    vSmoothN = vec3(0.0);
    float leafThin = 0.0;                        // a canopy's core: how much it is thinned out
    if (rhw) {
        // Screen-space vertex. The Vulkan viewport is the D3D one shifted by half a pixel (D3D pixel
        // centres are at integer coordinates), so position relative to it.
        float w = inPos.w != 0.0 ? 1.0 / inPos.w : 1.0;
        vec2 ndc = vec2((inPos.x - C.viewport.x) / C.viewport.z * 2.0 - 1.0,
                        1.0 - (inPos.y - C.viewport.y) / C.viewport.w * 2.0);
        gl_Position = vec4(ndc * w, inPos.z * w, w);
        vDiffuse = diffuse;
        vSpecular = specular;
        vFogDist = inPos.z;
        vFogFactor = specular.a;          // pre-transformed vertices carry their fog factor in specular alpha
    } else {
        // A canopy with leaves: its own cards drawn as an inner core - drawn in towards the crown's centre (by RVK_LeafCore,
        // as much as its leaves stand for at this distance), so the leaves around it make the crown's outside.
        // ... a little only (a wide, flat crown's tips would leave its leaves behind), and thinned out in place (vSet0:
        // ffp_main.glsl LeafMask).
        vec3 modelPos = inPos.xyz;
        float crownShade = 1.0;
        if (D.leaf.w > 0.0 && FL.leafView.y > 0.0) {   // (a palm, D.leaf.w < 0, only sways: its fronds are whole)
            float keep = LeafKeep();
            crownShade = mix(1.0, CrownShade(modelPos), keep);
#ifndef RVK_LEAF
            modelPos = mix(modelPos, D.leaf.xyz, CoreShrink(FL.leaves.w, keep));
            crownShade *= 1.0 - 0.1 * FL.leaves.w * keep;
            leafThin = CoreThin(FL.leaves.w, keep);
#endif
        }
        vec4 posW = D.world * vec4(modelPos, 1.0);
        vec3 swayPrev = vec3(0.0);               // the sway last frame (motion vectors)
        if (D.sway.w > 0.5) {
            int axis = clamp(int(D.sway.w + 0.5) - 1, 0, 2);
            float meanAlpha = SwayMeanAlpha(), holes = SwayHoles(meanAlpha);   // once a vertex
            vec3 push = PushOffset(posW.xyz, inPos.xyz, D.sway, length(D.world[axis].xyz) / abs(D.sway.y),
                                   D.world[3].xz, holes);
            posW.xz += FL.wind.xy * SwayDistanceHoles(inPos.xyz, D.sway, D.world[3].xz, FL.wind.z, holes);
            posW.xyz += push;
            swayPrev = push;
            swayPrev.xz += FL.wind.xy * SwayDistanceHoles(inPos.xyz, D.sway, D.prevWorld[3].xz, FL.taa.w, holes);
            // A plant (its texture has holes) blended with depth writes is cut out like its shadow: no depth or
            // motion where it is see-through (the ambient occlusion, motion blur and TAA read those).
            if (meanAlpha < 0.92) vCutout = 0.35;
        }
        if (D.leaf.w != 0.0) {                   // a canopy with leaves: its branches sway (its cards and its leaves)
            vec3 now = BranchSway(inPos.xyz, FL.wind.z, D.world[3].xz);
            posW.xyz += now;
#ifdef RVK_LEAF
            swayPrev += BranchSway(gLeafPrev, FL.taa.w, D.prevWorld[3].xz);
#else
            swayPrev += BranchSway(inPos.xyz, FL.taa.w, D.prevWorld[3].xz);
#endif
        }
#ifdef RVK_LEAF
        vCutout = 0.35;                          // as a plant's: the soft rim blends (the texture's own edge)
#endif
        vec4 pv = C.view * posW;
        gl_Position = C.proj * pv;
        vClip = gl_Position;                     // motion vectors: without the jitter
        if ((C.flags.x & F_HDR) != 0u)
            gl_Position.xy += FL.taa.xy * gl_Position.w;   // temporal anti-aliasing: this frame's sub-pixel offset
        vec3 prevLocal = modelPos;
#ifdef RVK_LEAF
        prevLocal = gLeafPrev;
#else
        if (D.motion.y > 0.5) {
            int i = (gl_VertexIndex - int(D.motion.z)) * 3;
            prevLocal = vec3(PP.prevPos[i], PP.prevPos[i + 1], PP.prevPos[i + 2]);
        }
#endif
        // Last frame's camera applied like this frame's (view, then projection), so a still object under a still
        // camera has exactly zero motion. Without object motion: none (vClip - gl_Position has the jitter); the TAA
        // then reprojects with the camera.
        vPrevClip = D.motion.x > 0.5 ? FL.prevProj * (FL.prevView * (D.prevWorld * vec4(prevLocal, 1.0) + vec4(swayPrev, 0.0)))
                                     : vClip;
        posV = pv.xyz;
        vec3 normalW = mat3(D.world) * (hasNormal ? inNormal : vec3(0.0));
        if ((C.flags.x & F_NORMALIZE) != 0u && dot(normalW, normalW) > 0.0)
            normalW = normalize(normalW);
        normalV = mat3(C.view) * normalW;

        vPosW = posW.xyz;
        vNormalW = vec4(normalW, length(normalW));
#ifndef RVK_LEAF
        if (D.tess.x > 0.5) {
            // Direction: the averaged normal in the world; length: how smooth the surface is at this corner (the
            // model-space mean of the unit normals here: 1 where they agree, ~0.58 at a box's corner) as a weight.
            int i = (gl_VertexIndex - int(D.tess.z)) * 3;
            vec3 m = vec3(SN.smoothN[i], SN.smoothN[i + 1], SN.smoothN[i + 2]);
            vec3 s = mat3(D.world) * m;
            vSmoothN = dot(s, s) > 0.0 ? normalize(s) * smoothstep(0.85, 0.97, length(m)) : vec3(0.0);
        }
#endif
        if ((C.flags.x & F_LIGHTING) != 0u) {
            vec4 mDiffuse = MaterialColor(C.matSources.x, C.matDiffuse, diffuse, specular, hasDiffuse, hasSpecular);
            vec4 mAmbient = MaterialColor(C.matSources.y, C.matAmbient, diffuse, specular, hasDiffuse, hasSpecular);
            vec4 mSpecular = MaterialColor(C.matSources.z, C.matSpecular, diffuse, specular, hasDiffuse, hasSpecular);
            vec4 mEmissive = MaterialColor(C.matSources.w, C.matEmissive, diffuse, specular, hasDiffuse, hasSpecular);
            if ((C.flags.x & F_PERPIXEL) != 0u) {
                vMatAmbient = mAmbient.rgb;
                vMatEmissive = mEmissive.rgb;
                diffuse = mDiffuse;
                specular = vec4(mSpecular.rgb, specular.a);
            } else {
                vec3 ambient = C.ambient.rgb, diff = vec3(0.0), spec = vec3(0.0), diffL = vec3(0.0), specL = vec3(0.0);
                // F_VERTEXSUN: the lights without the sun's direct light, which goes apart to be shadowed per pixel.
                bool sunApart = (C.flags.x & F_VERTEXSUN) != 0u;
                AccumulateLights(posW.xyz, normalW, sunApart ? 0.0 : 1.0, 1.0, ambient, diff, spec, diffL, specL);
                if (sunApart) {
                    vec3 a2 = vec3(0.0), withSun = vec3(0.0), s2 = vec3(0.0), dl2 = vec3(0.0), sl2 = vec3(0.0);
                    AccumulateLights(posW.xyz, normalW, 1.0, 0.0, a2, withSun, s2, dl2, sl2);
                    vec3 a3 = vec3(0.0), without = vec3(0.0), s3 = vec3(0.0), dl3 = vec3(0.0), sl3 = vec3(0.0);
                    AccumulateLights(posW.xyz, normalW, 0.0, 0.0, a3, without, s3, dl3, sl3);
                    vMatAmbient = mDiffuse.rgb * max(withSun - without, vec3(0.0));
                }
                diff += diffL;
                spec += specL;
                diffuse = vec4(mEmissive.rgb + mAmbient.rgb * ambient + mDiffuse.rgb * diff, mDiffuse.a);
                specular = vec4(mSpecular.rgb * spec, specular.a);
            }
        }
        diffuse.rgb *= crownShade;
        vMatAmbient *= crownShade;
        vMatEmissive *= crownShade;
        // Material colours for per-pixel lighting stay unclamped; the fragment shader clamps the lit result.
        bool perPixel = (C.flags.x & (F_LIGHTING | F_PERPIXEL)) == (F_LIGHTING | F_PERPIXEL);
        vDiffuse = perPixel ? diffuse : clamp(diffuse, 0.0, 1.0);
        vSpecular = perPixel ? specular : clamp(specular, 0.0, 1.0);
        float dist = (C.flags.x & F_RANGEFOG) != 0u ? length(pv.xyz) : abs(pv.z);
        vFogDist = dist;
        vFogFactor = FogFactor(C.flags.y, dist);
    }
    vTex0 = TexCoord(0u, fvf, posV, normalV);
    vSet0 = inTex0.xy;
#ifdef RVK_LEAF
    vSet0 = gLeafCorner;                         // where on the leaf's card (ffp_main.glsl LeafMask)
#else
    if (D.leaf.w > 0.0 && (fvf & 0xEu) != 4u)
        vSet0 = vec2(leafThin, 2.0);                 // the canopy's core: how much it is thinned out
#endif
    vTex1 = TexCoord(1u, fvf, posV, normalV);
}
