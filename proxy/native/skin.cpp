// Character skinning on the CPU with SSE (the original: x87, one value at a time). Same results as randy31_orig's
// FUN_1005470d, including its quirks: vertices with a bone out of range keep their old contents, and the box grows the
// original's way (a value that lowers the minimum doesn't also raise the maximum - so the first vertex never counts
// towards the maximum).
#include "native/skin.h"

#include <emmintrin.h>
#include <malloc.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <vector>

namespace rnative::skin {

using namespace cat;

Palette::~Palette()
{
    _aligned_free(bones);
}

void Palette::Set(const Bone* source, uint32_t n)
{
    if (n > capacity) {
        _aligned_free(bones);
        capacity = n + 16;
        bones = static_cast<Columns*>(_aligned_malloc(sizeof(Columns) * capacity, 16));
    }
    count = n;
    for (uint32_t i = 0; i < n; ++i) {
        const float* m = source[i].m;
        for (int c = 0; c < 4; ++c) {
            bones[i].c[c][0] = m[c * 3 + 0];
            bones[i].c[c][1] = m[c * 3 + 1];
            bones[i].c[c][2] = m[c * 3 + 2];
            bones[i].c[c][3] = 0.0f;
        }
    }
}

namespace {

// p * M (x' = m0 x + m3 y + m6 z + m9, ...), and the rotation alone for normals.
inline __m128 Transform(const Palette::Columns& b, const float* p)
{
    __m128 r = _mm_add_ps(_mm_mul_ps(_mm_load_ps(b.c[0]), _mm_set1_ps(p[0])),
                          _mm_mul_ps(_mm_load_ps(b.c[1]), _mm_set1_ps(p[1])));
    return _mm_add_ps(r, _mm_add_ps(_mm_mul_ps(_mm_load_ps(b.c[2]), _mm_set1_ps(p[2])), _mm_load_ps(b.c[3])));
}

inline __m128 Rotate(const Palette::Columns& b, const float* n)
{
    __m128 r = _mm_add_ps(_mm_mul_ps(_mm_load_ps(b.c[0]), _mm_set1_ps(n[0])),
                          _mm_mul_ps(_mm_load_ps(b.c[1]), _mm_set1_ps(n[1])));
    return _mm_add_ps(r, _mm_mul_ps(_mm_load_ps(b.c[2]), _mm_set1_ps(n[2])));
}

// Position and normal into the vertex, leaving its texture coordinates.
inline void Store(Vertex* v, __m128 pos, __m128 normal)
{
    __m128 zx = _mm_shuffle_ps(pos, normal, _MM_SHUFFLE(0, 0, 2, 2));            // pos.z, pos.z, n.x, n.x
    _mm_storeu_ps(v->pos, _mm_shuffle_ps(pos, zx, _MM_SHUFFLE(2, 0, 1, 0)));      // pos.x, pos.y, pos.z, n.x
    _mm_storel_pi(reinterpret_cast<__m64*>(&v->normal[1]), _mm_shuffle_ps(normal, normal, _MM_SHUFFLE(3, 3, 2, 1)));
}

inline __m128 Load3(const float* p)
{
    return _mm_set_ps(0.0f, p[2], p[1], p[0]);
}

}  // namespace

void SkinVertices(const TriVertex* in, uint32_t count, Vertex* out, const Palette& bones, bool rest, float* boxMin,
                  float* boxMax)
{
    __m128 mn = boxMin ? Load3(boxMin) : _mm_set1_ps(FLT_MAX);
    __m128 mx = boxMax ? Load3(boxMax) : _mm_set1_ps(-FLT_MAX);
    const auto boneCount = int32_t(bones.count);
    for (uint32_t i = 0; i < count; ++i) {
        const TriVertex& v = in[i];
        Vertex* o = out + i;
        __m128 pos;
        if (rest) {
            pos = Load3(v.bind);
            Store(o, pos, Load3(v.normal));
        } else if (v.weightA <= 0.99f) {                // the original: <= (double)0.99f
            if (uint32_t(v.boneA) < uint32_t(boneCount) && uint32_t(v.boneB) < uint32_t(boneCount)) {
                const Palette::Columns& a = bones.bones[v.boneA];
                const Palette::Columns& b = bones.bones[v.boneB];
                __m128 w = _mm_set1_ps(v.weightA), w1 = _mm_set1_ps(1.0f - v.weightA);
                pos = _mm_add_ps(_mm_mul_ps(w, Transform(a, v.posA)), _mm_mul_ps(w1, Transform(b, v.posB)));
                Store(o, pos, Rotate(a, v.normal));
            } else {
                pos = Load3(o->pos);
            }
        } else if (uint32_t(v.boneA) < uint32_t(boneCount)) {
            const Palette::Columns& a = bones.bones[v.boneA];
            pos = Transform(a, v.posA);
            Store(o, pos, Rotate(a, v.normal));
        } else {
            pos = Load3(o->pos);
        }
        // The original: if (!(min <= p)) min = p; else if (max < p) max = p - per coordinate.
        if (boxMin) {
            __m128 lower = _mm_cmpnle_ps(mn, pos);
            if (boxMax)
                mx = _mm_or_ps(_mm_and_ps(lower, mx), _mm_andnot_ps(lower, _mm_max_ps(mx, pos)));
            mn = _mm_or_ps(_mm_and_ps(lower, pos), _mm_andnot_ps(lower, mn));
        } else if (boxMax) {
            mx = _mm_max_ps(mx, pos);
        }
    }
    alignas(16) float t[4];
    if (boxMin) {
        _mm_store_ps(t, mn);
        boxMin[0] = t[0], boxMin[1] = t[1], boxMin[2] = t[2];
    }
    if (boxMax) {
        _mm_store_ps(t, mx);
        boxMax[0] = t[0], boxMax[1] = t[1], boxMax[2] = t[2];
    }
}

void SkinRender(void* render, float* boxMin, float* boxMax, LockFn lock, UnlockFn unlock)
{
    for (int i = 0; i < 3; ++i) {
        if (boxMin) boxMin[i] = FLT_MAX;
        if (boxMax) boxMax[i] = -FLT_MAX;
    }
    if (!At<void*>(render, kRenderAnim))
        return;
    static Palette palette;                                 // the game's thread only
    palette.Set(At<const Bone*>(render, kRenderBones), uint32_t(std::max(At<int32_t>(render, kRenderBoneCount), 0)));
    const bool rest = At<uint8_t>(render, kRenderRest) == 1;
    const Callback* cbBegin = At<const Callback*>(render, kRenderCallbacks);
    const Callback* cbEnd = At<const Callback*>(render, kRenderCallbacks + 4);
    uint32_t vertexBase = 0, triBase = 0;
    const int32_t groupCount = At<int32_t>(render, kRenderGroupCount);
    auto* groups = At<RenderGroup*>(render, kRenderGroups);
    for (int32_t g = 0; g < groupCount; ++g) {
        uint8_t* meshGroup = At<uint8_t*>(groups[g].mesh, kMeshGroups) + g * kGroupSize;
        const int32_t pieces = At<int32_t>(meshGroup, kGroupPieceCount);
        uint8_t* piece = At<uint8_t*>(meshGroup, kGroupPieces);
        for (int32_t p = 0; p < pieces; ++p, piece += kPieceSize) {
            void* buffer = groups[g].slots[p].buffer;
            auto* out = static_cast<Vertex*>(lock(buffer, nullptr, 0, 0));
            const auto* in = At<const TriVertex*>(piece, kPieceVertices);
            const uint32_t count = At<uint32_t>(piece, kPieceVertexCount);
            if (count)
                SkinVertices(in, count, out, palette, rest, boxMin, boxMax);
            for (const Callback* cb = cbBegin; cb != cbEnd; ++cb)
                cb->fn(render, count, in, out, At<uint32_t>(piece, kPieceTriCount),
                       At<const uint16_t*>(piece, kPieceIndices), vertexBase, triBase, cb->user);
            vertexBase += count;
            triBase += At<uint32_t>(piece, kPieceTriCount);
            unlock(buffer, nullptr);
        }
    }
}

namespace {

using OriginalFn = void(__fastcall*)(void* render, void* edx, float* boxMin, float* boxMax);
OriginalFn g_original;
LockFn g_lock;
UnlockFn g_unlock;
uintptr_t g_blobCallback;                                   // DisplaySystem's blob shadow: only reads

constexpr uint32_t kSkinRva = 0x5470D;
constexpr uint8_t kSkinPrologue[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x38};   // push ebp; mov ebp, esp; sub esp, 38h
constexpr uint32_t kBlobCallbackRva = 0x1E75D;              // DisplaySystem.dll FUN_1001e75d
constexpr uint32_t kLockReadOnly = 0x10;                    // DDLOCK_READONLY

void __fastcall SkinOn(void* render, void*, float* boxMin, float* boxMax)
{
    SkinRender(render, boxMin, boxMax, g_lock, g_unlock);
}

// Verify: ours into a copy of each piece's vertices (no callbacks), then the original as usual, then compared -
// unless a callback that changes vertices ran.
struct VerifyStats {
    uint64_t calls = 0, compared = 0, vertices = 0, mismatches = 0, boxMismatches = 0, skipped = 0;
    float maxError = 0.0f;
} g_stats;

bool Close(float a, float b, float* worst)
{
    float e = std::fabs(a - b) / (1.0f + std::fabs(b));
    if (e > *worst) *worst = e;
    return e <= 1e-4f || (a == b);
}

void __fastcall SkinVerify(void* render, void*, float* boxMin, float* boxMax)
{
    ++g_stats.calls;
    if (!g_blobCallback)                                    // DisplaySystem may load after randy31
        if (HMODULE ds = GetModuleHandleA("DisplaySystem.dll"))
            g_blobCallback = reinterpret_cast<uintptr_t>(ds) + kBlobCallbackRva;
    bool readOnlyCallbacks = true;
    for (const Callback* cb = At<const Callback*>(render, kRenderCallbacks);
         cb != At<const Callback*>(render, kRenderCallbacks + 4); ++cb)
        if (reinterpret_cast<uintptr_t>(cb->fn) != g_blobCallback)
            readOnlyCallbacks = false;
    if (!readOnlyCallbacks || !At<void*>(render, kRenderAnim)) {
        ++g_stats.skipped;
        g_original(render, nullptr, boxMin, boxMax);
        return;
    }
    // Ours, into copies of the current buffers.
    static Palette palette;
    palette.Set(At<const Bone*>(render, kRenderBones), uint32_t(std::max(At<int32_t>(render, kRenderBoneCount), 0)));
    const bool rest = At<uint8_t>(render, kRenderRest) == 1;
    std::vector<std::vector<Vertex>> ours;
    float oursMin[3] = {FLT_MAX, FLT_MAX, FLT_MAX}, oursMax[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    const int32_t groupCount = At<int32_t>(render, kRenderGroupCount);
    auto* groups = At<RenderGroup*>(render, kRenderGroups);
    for (int32_t g = 0; g < groupCount; ++g) {
        uint8_t* meshGroup = At<uint8_t*>(groups[g].mesh, kMeshGroups) + g * kGroupSize;
        uint8_t* piece = At<uint8_t*>(meshGroup, kGroupPieces);
        for (int32_t p = 0; p < At<int32_t>(meshGroup, kGroupPieceCount); ++p, piece += kPieceSize) {
            void* buffer = groups[g].slots[p].buffer;
            const uint32_t count = At<uint32_t>(piece, kPieceVertexCount);
            auto* current = static_cast<const Vertex*>(g_lock(buffer, nullptr, 0, kLockReadOnly));
            ours.emplace_back(current, current + count);
            g_unlock(buffer, nullptr);
            if (count)
                SkinVertices(At<const TriVertex*>(piece, kPieceVertices), count, ours.back().data(), palette, rest,
                             oursMin, oursMax);
        }
    }
    g_original(render, nullptr, boxMin, boxMax);
    // Compared with what the original wrote.
    size_t index = 0;
    bool bad = false;
    for (int32_t g = 0; g < groupCount; ++g) {
        uint8_t* meshGroup = At<uint8_t*>(groups[g].mesh, kMeshGroups) + g * kGroupSize;
        for (int32_t p = 0; p < At<int32_t>(meshGroup, kGroupPieceCount); ++p, ++index) {
            void* buffer = groups[g].slots[p].buffer;
            auto* theirs = static_cast<const Vertex*>(g_lock(buffer, nullptr, 0, kLockReadOnly));
            const std::vector<Vertex>& mine = ours[index];
            for (size_t v = 0; v < mine.size(); ++v) {
                bool same = true;
                for (int k = 0; k < 3; ++k)
                    same &= Close(mine[v].pos[k], theirs[v].pos[k], &g_stats.maxError) &&
                            Close(mine[v].normal[k], theirs[v].normal[k], &g_stats.maxError);
                ++g_stats.vertices;
                if (!same && ++g_stats.mismatches <= 20)
                    Log("skin verify: render %p group %d piece %d vertex %zu: ours (%g %g %g | %g %g %g) "
                        "original (%g %g %g | %g %g %g)", render, g, p, v, mine[v].pos[0], mine[v].pos[1],
                        mine[v].pos[2], mine[v].normal[0], mine[v].normal[1], mine[v].normal[2], theirs[v].pos[0],
                        theirs[v].pos[1], theirs[v].pos[2], theirs[v].normal[0], theirs[v].normal[1],
                        theirs[v].normal[2]);
                bad |= !same;
            }
            g_unlock(buffer, nullptr);
        }
    }
    float ignore = 0.0f;
    for (int k = 0; k < 3 && boxMin && boxMax; ++k)
        if (!Close(oursMin[k], boxMin[k], &ignore) || !Close(oursMax[k], boxMax[k], &ignore)) {
            if (++g_stats.boxMismatches <= 10)
                Log("skin verify: render %p box ours (%g %g %g)-(%g %g %g) original (%g %g %g)-(%g %g %g)", render,
                    oursMin[0], oursMin[1], oursMin[2], oursMax[0], oursMax[1], oursMax[2], boxMin[0], boxMin[1],
                    boxMin[2], boxMax[0], boxMax[1], boxMax[2]);
            break;
        }
    ++g_stats.compared;
    if (g_stats.calls % 5000 == 0 || (bad && g_stats.mismatches <= 20))
        Log("skin verify: %llu calls, %llu compared (%llu skipped: deforming callbacks), %llu vertices, %llu differ, "
            "%llu boxes differ, largest relative error %g", g_stats.calls, g_stats.compared, g_stats.skipped,
            g_stats.vertices, g_stats.mismatches, g_stats.boxMismatches, g_stats.maxError);
}

}  // namespace

void Install(HMODULE orig)
{
    Mode mode = GetMode("Skin", Mode::Off);
    if (mode == Mode::Off)
        return;
    g_lock = reinterpret_cast<LockFn>(GetProcAddress(orig, "?Lock@VertexBuffer_c@@QAEPAXII@Z"));
    g_unlock = reinterpret_cast<UnlockFn>(GetProcAddress(orig, "?Unlock@VertexBuffer_c@@QAEXXZ"));
    if (!g_lock || !g_unlock)
        return;
    void* target = mode == Mode::Verify ? reinterpret_cast<void*>(&SkinVerify) : reinterpret_cast<void*>(&SkinOn);
    g_original = reinterpret_cast<OriginalFn>(
        HookEntry(orig, kSkinRva, kSkinPrologue, sizeof(kSkinPrologue), target, "skinning (FUN_1005470d)"));
    if (g_original)
        Log("skinning: %s", ModeName(mode));
}

}  // namespace rnative::skin
