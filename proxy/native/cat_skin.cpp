// Character skinning on the game's thread (cpu) or deferred to the renderer (on); see cat_skin.h. The SSE loop itself
// is rvk/skin.cpp.
#include "native/cat_skin.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace rnative::skin {

using namespace cat;
using rvk::skin::Job;
using rvk::skin::Source;

namespace {

const Bone* Bones(void* render, uint32_t* count)
{
    *count = uint32_t(std::max(At<int32_t>(render, kRenderBoneCount), 0));
    return At<const Bone*>(render, kRenderBones);
}

}  // namespace

void SkinRender(void* render, float* boxMin, float* boxMax, LockFn lock, UnlockFn unlock)
{
    for (int i = 0; i < 3; ++i) {
        if (boxMin) boxMin[i] = FLT_MAX;
        if (boxMax) boxMax[i] = -FLT_MAX;
    }
    if (!At<void*>(render, kRenderAnim))
        return;
    static Palette palette;                                 // the game's thread only
    uint32_t boneCount;
    const Bone* bones = Bones(render, &boneCount);
    palette.Set(bones, boneCount);
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
SinkFn g_sink;
uintptr_t g_blobCallback;                                   // DisplaySystem's blob shadow: reads positions only
int g_exact;                                                // inside picking: skin on this thread, box included

constexpr uint32_t kSkinRva = 0x5470D;
constexpr uint8_t kSkinPrologue[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x38};   // push ebp; mov ebp, esp; sub esp, 38h
constexpr uint32_t kBlobCallbackRva = 0x1E75D;              // DisplaySystem.dll FUN_1001e75d
constexpr uint32_t kLockReadOnly = 0x10;                    // DDLOCK_READONLY
// RCATMesh_t's box (the original's skinning grows it; picking reads it).
constexpr uint32_t kBoxMin = 0x1FC, kBoxMax = 0x208;

void FindBlobCallback()
{
    if (!g_blobCallback)                                    // DisplaySystem may load after randy31
        if (HMODULE ds = GetModuleHandleA("DisplaySystem.dll"))
            g_blobCallback = reinterpret_cast<uintptr_t>(ds) + kBlobCallbackRva;
}

// ---- deferred ----

// Mesh vertices copied once per mesh piece, found again by address (checked against a few vertices, as the game may
// free a mesh and reuse its memory); unused ones are dropped after a while.
struct CachedSource {
    std::shared_ptr<const Source> source;
    uint32_t lastUse = 0;
};
std::unordered_map<const TriVertex*, CachedSource> g_sources;
uint32_t g_frameCalls, g_sweep;

// Still the same piece: same counts, and a few of its vertices and indices unchanged.
bool Same(const Source& s, const TriVertex* in, uint32_t count, const uint16_t* indices, uint32_t indexCount)
{
    if (s.vertices.size() != count || s.indices.size() != indexCount || s.gameIndices != indices)
        return false;
    const uint32_t picks[4] = {0, count / 3, (2 * count) / 3, count - 1};
    for (uint32_t i : picks)
        if (std::memcmp(&s.vertices[i], in + i, sizeof(TriVertex)) != 0)
            return false;
    return indexCount == 0 ||
           (s.indices[0] == indices[0] && s.indices[indexCount / 2] == indices[indexCount / 2] &&
            s.indices[indexCount - 1] == indices[indexCount - 1]);
}

std::shared_ptr<const Source> SourceOf(const TriVertex* in, uint32_t count, const uint16_t* indices, uint32_t indexCount)
{
    CachedSource& c = g_sources[in];
    if (!c.source || !Same(*c.source, in, count, indices, indexCount)) {
        auto s = std::make_shared<Source>();
        s->vertices.assign(in, in + count);
        if (indices) s->indices.assign(indices, indices + indexCount);
        s->gameIndices = indices;
        s->Finish();
        c.source = std::move(s);
    }
    c.lastUse = g_sweep;
    return c.source;
}

void SweepSources()
{
    if (++g_frameCalls % 20000 != 0)                        // every few hundred frames' worth of characters
        return;
    ++g_sweep;
    for (auto it = g_sources.begin(); it != g_sources.end();)
        it = it->second.lastUse + 2 < g_sweep ? g_sources.erase(it) : std::next(it);
}

// The IDirect3DVertexBuffer7 behind a VertexBuffer_c (its impl's first field).
void* D3dBuffer(void* buffer)
{
    void* impl = *static_cast<void**>(buffer);
    return impl ? *static_cast<void**>(impl) : nullptr;
}

// Deferred, if the character's vertices aren't needed on this thread; false to skin it here instead.
bool SkinDeferred(void* render)
{
    FindBlobCallback();
    const Callback* cbBegin = At<const Callback*>(render, kRenderCallbacks);
    const Callback* cbEnd = At<const Callback*>(render, kRenderCallbacks + 4);
    for (const Callback* cb = cbBegin; cb != cbEnd; ++cb)
        if (reinterpret_cast<uintptr_t>(cb->fn) != g_blobCallback)
            return false;                                   // an effect that reads or moves every vertex
    SweepSources();
    auto palette = std::make_shared<Palette>();
    uint32_t boneCount;
    const Bone* bones = Bones(render, &boneCount);
    palette->Set(bones, boneCount);
    const bool rest = At<uint8_t>(render, kRenderRest) == 1;
    static std::vector<Vertex> sparse;                      // the blob shadow's positions (every 4th vertex)
    // The pieces' jobs first, with one box for the character (from all its pieces' bone boxes).
    struct PieceJob { void* buffer; uint8_t* piece; std::shared_ptr<Job> job; };
    static std::vector<PieceJob> pieceJobs;
    static std::vector<const rvk::skin::Source*> sources;
    pieceJobs.clear();
    sources.clear();
    const int32_t groupCount = At<int32_t>(render, kRenderGroupCount);
    auto* groups = At<RenderGroup*>(render, kRenderGroups);
    for (int32_t g = 0; g < groupCount; ++g) {
        uint8_t* meshGroup = At<uint8_t*>(groups[g].mesh, kMeshGroups) + g * kGroupSize;
        const int32_t pieces = At<int32_t>(meshGroup, kGroupPieceCount);
        uint8_t* piece = At<uint8_t*>(meshGroup, kGroupPieces);
        for (int32_t p = 0; p < pieces; ++p, piece += kPieceSize) {
            PieceJob pj{groups[g].slots[p].buffer, piece, nullptr};
            const auto* in = At<const TriVertex*>(piece, kPieceVertices);
            const uint32_t count = At<uint32_t>(piece, kPieceVertexCount);
            if (count && in) {
                pj.job = std::make_shared<Job>();
                const auto* indices = At<const uint16_t*>(piece, kPieceIndices);
                const uint32_t tris = At<uint32_t>(piece, kPieceTriCount);
                pj.job->source = SourceOf(in, count, indices, indices ? tris * 3 : 0);
                pj.job->bones = palette;
                pj.job->rest = rest;
                sources.push_back(pj.job->source.get());
            }
            pieceJobs.push_back(std::move(pj));
        }
    }
    float boxMin[3], boxMax[3];
    Job::BoundsOf(sources.data(), sources.size(), *palette, rest, boxMin, boxMax);
    uint32_t vertexBase = 0, triBase = 0;
    size_t next = 0;
    for (int32_t g = 0; g < groupCount; ++g) {
        uint8_t* meshGroup = At<uint8_t*>(groups[g].mesh, kMeshGroups) + g * kGroupSize;
        const int32_t pieces = At<int32_t>(meshGroup, kGroupPieceCount);
        uint8_t* piece = At<uint8_t*>(meshGroup, kGroupPieces);
        for (int32_t p = 0; p < pieces; ++p, piece += kPieceSize) {
            void* buffer = groups[g].slots[p].buffer;
            const auto* in = At<const TriVertex*>(piece, kPieceVertices);
            const uint32_t count = At<uint32_t>(piece, kPieceVertexCount);
            const uint32_t tris = At<uint32_t>(piece, kPieceTriCount);
            bool handed = false;
            if (std::shared_ptr<Job>& job = pieceJobs[next++].job) {
                std::memcpy(job->boundsMin, boxMin, sizeof(boxMin));
                std::memcpy(job->boundsMax, boxMax, sizeof(boxMax));
                handed = g_sink(D3dBuffer(buffer), job);
                if (handed)
                    Job::Prefetch(job);
            }
            if (!handed && count) {                         // not the renderer's buffer: skin it here
                auto* out = static_cast<Vertex*>(g_lock(buffer, nullptr, 0, 0));
                SkinVertices(in, count, out, *palette, rest, nullptr, nullptr);
                g_unlock(buffer, nullptr);
            }
            if (cbBegin != cbEnd && count) {
                if (sparse.size() < count) sparse.resize(count);
                rvk::skin::SkinPositions(in, count, 4, sparse.data(), *palette, rest);
                for (const Callback* cb = cbBegin; cb != cbEnd; ++cb)
                    cb->fn(render, count, in, sparse.data(), tris, At<const uint16_t*>(piece, kPieceIndices),
                           vertexBase, triBase, cb->user);
            }
            vertexBase += count;
            triBase += tris;
        }
    }
    pieceJobs.clear();                                      // the buffers hold the jobs now
    return true;
}

void __fastcall SkinCpu(void* render, void*, float* boxMin, float* boxMax)
{
    SkinRender(render, boxMin, boxMax, g_lock, g_unlock);
}

void __fastcall SkinOn(void* render, void*, float* boxMin, float* boxMax)
{
    if (g_exact || !At<void*>(render, kRenderAnim) || !SkinDeferred(render))
        SkinRender(render, boxMin, boxMax, g_lock, g_unlock);
}

// Picking reads the vertices and the box right after skinning: exact, on this thread, for its duration (and once at
// its start, for a character whose animation didn't change since it was skinned deferred).
using IsLineFn = bool(__fastcall*)(void* self, void*, const void* from, const void* to, float* at, uint32_t flag);
IsLineFn g_isLine;
using PickFn = uint32_t(__fastcall*)(void* visual, void*, float* from, float* to, float* at);
PickFn g_pick;
uint32_t g_picks;

void SkinForPicking(void* render)
{
    if (++g_picks == 1 || g_picks % 10000 == 0)
        Log("skinning: %u pickings so far (each skins its character on the game's thread)", g_picks);
    SkinRender(render, &At<float>(render, kBoxMin), &At<float>(render, kBoxMax), g_lock, g_unlock);
}

bool g_deferred;                                            // Skin=on: picking needs the scope below

bool __fastcall IsLineHook(void* render, void*, const void* from, const void* to, float* at, uint32_t flag)
{
    ++g_exact;
    SkinForPicking(render);
    bool r = g_isLine(render, nullptr, from, to, at, flag);
    --g_exact;
    return r;
}

uint32_t __fastcall PickHook(void* visual, void*, float* from, float* to, float* at)
{
    ++g_exact;
    SkinForPicking(static_cast<uint8_t*>(visual) - 0x3C);
    uint32_t r = g_pick(visual, nullptr, from, to, at);
    --g_exact;
    return r;
}

constexpr uint32_t kIsLineRva = 0x56786;                    // RCATMesh_t::IsLineIntersecting
constexpr uint32_t kPickRva = 0x5632D;                      // RCATMesh_t's RVisual_t vtable slot 12 (line test)
constexpr uint8_t kIsLinePrologue[] = {0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x6C, 0x01, 0x00, 0x00};
constexpr uint8_t kPickPrologue[] = {0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x00, 0x01, 0x00, 0x00};

// ---- verify: ours into a copy of each piece's vertices (no callbacks), then the original as usual, then compared -
// unless a callback that changes vertices ran ----

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
    FindBlobCallback();
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
    static Palette palette;
    uint32_t boneCount;
    const Bone* bones = Bones(render, &boneCount);
    palette.Set(bones, boneCount);
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

void SetSink(SinkFn sink)
{
    g_sink = sink;
}

void Install(HMODULE orig)
{
    Mode mode = GetMode("Skin", Mode::Off);
    char value[16] = "";
    if (mode == Mode::Off) {                                // GetMode knows off / on / verify; `cpu` reads as off
        GetPrivateProfileStringA("Native", "Skin", "", value, sizeof(value), IniPath());
        if (_stricmp(value, "cpu") != 0)
            return;
    }
    const bool cpu = !_stricmp(value, "cpu") || (mode == Mode::On && !g_sink);
    g_lock = reinterpret_cast<LockFn>(GetProcAddress(orig, "?Lock@VertexBuffer_c@@QAEPAXII@Z"));
    g_unlock = reinterpret_cast<UnlockFn>(GetProcAddress(orig, "?Unlock@VertexBuffer_c@@QAEXXZ"));
    if (!g_lock || !g_unlock)
        return;
    void* target = mode == Mode::Verify ? reinterpret_cast<void*>(&SkinVerify)
                   : cpu                ? reinterpret_cast<void*>(&SkinCpu)
                                        : reinterpret_cast<void*>(&SkinOn);
    g_original = reinterpret_cast<OriginalFn>(
        HookEntry(orig, kSkinRva, kSkinPrologue, sizeof(kSkinPrologue), target, "skinning (FUN_1005470d)"));
    if (!g_original)
        return;
    if (target == reinterpret_cast<void*>(&SkinOn)) {
        g_deferred = true;
    }
    if (target == reinterpret_cast<void*>(&SkinOn) && GetMode("Scene", Mode::Off) != Mode::On) {
        // (Scene=on: picking is native, cat_pick.cpp, and opens this scope itself - PickingBegin.)
        g_isLine = reinterpret_cast<IsLineFn>(HookEntry(orig, kIsLineRva, kIsLinePrologue, sizeof(kIsLinePrologue),
                                                        reinterpret_cast<void*>(&IsLineHook),
                                                        "RCATMesh_t::IsLineIntersecting"));
        g_pick = reinterpret_cast<PickFn>(HookEntry(orig, kPickRva, kPickPrologue, sizeof(kPickPrologue),
                                                    reinterpret_cast<void*>(&PickHook), "RCATMesh_t line test"));
        if (!g_isLine || !g_pick)
            Log("skinning: picking hooks missing - deferred skinning may pick from older poses");
    }
    Log("skinning: %s", target == reinterpret_cast<void*>(&SkinOn) ? "on (deferred to the renderer)"
                        : cpu                                     ? "cpu (on the game's thread)"
                                                                  : "verify");
}

bool PickingBegin(void* render)
{
    if (!g_deferred) return false;
    ++g_exact;
    SkinForPicking(render);
    return true;
}

void PickingEnd() { --g_exact; }

}  // namespace rnative::skin
