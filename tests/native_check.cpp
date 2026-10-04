// Runs native replacements (proxy/native) and the original code they replace in randy31_orig.dll on the same
// synthetic input and compares the results, then times both. Run under Wine with tools/native-check.sh.
//
//   native_check.exe <path to randy31_orig.dll>
#include "native/cat_skin.h"

#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using namespace rnative;
using namespace rnative::cat;

namespace {

int g_failures;

void Check(bool ok, const char* what)
{
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

// ---- A fake IDirect3DVertexBuffer7 behind a VertexBuffer_c, as randy31's VertexBuffer_c::Lock reaches it ----
struct FakeVb;
HRESULT __stdcall FakeLock(FakeVb* self, DWORD flags, void** out, DWORD* size);
HRESULT __stdcall FakeUnlock(FakeVb* self);
void* g_fakeVtbl[8] = {nullptr, nullptr, nullptr, reinterpret_cast<void*>(&FakeLock),
                       reinterpret_cast<void*>(&FakeUnlock)};
struct FakeVb {
    void** vtbl = g_fakeVtbl;
    std::vector<Vertex> data;
    int locks = 0, unlocks = 0;
};
HRESULT __stdcall FakeLock(FakeVb* self, DWORD, void** out, DWORD* size)
{
    ++self->locks;
    *out = self->data.data();
    if (size) *size = DWORD(self->data.size() * sizeof(Vertex));
    return 0;
}
HRESULT __stdcall FakeUnlock(FakeVb* self)
{
    ++self->unlocks;
    return 0;
}
struct VertexBufferImpl {                 // VertexBufferImpl_c (0x1C bytes); Lock / Unlock use only the first field
    FakeVb* vb;
    uint32_t fvf, flags, memory, bytes, stride, count;
};

// ---- A synthetic character: groups of pieces, bones, vertex buffers ----
struct Piece {
    std::vector<TriVertex> vertices;
    std::vector<uint16_t> indices;
    uint32_t tris = 0;
};
struct Character {
    std::vector<std::vector<Piece>> groups;
    std::vector<Bone> bones;
    bool rest = false;
    // Built:
    std::vector<std::vector<uint8_t>> meshGroupBytes;   // per group: CATMesh group array (0x34 each, only [g] used)
    std::vector<std::vector<uint8_t>> pieceBytes;
    std::vector<std::vector<uint8_t>> meshBytes;
    std::vector<RenderGroup> renderGroups;
    std::vector<std::vector<RenderGroup::Slot>> slots;
    std::vector<FakeVb> vbs;
    std::vector<VertexBufferImpl> impls;
    std::vector<void*> vertexBuffers;                   // VertexBuffer_c: a pointer to its impl
    std::vector<Callback> callbacks;
    alignas(16) uint8_t render[0x60] = {};
    int anim = 1;
};

std::mt19937 g_rng(1234);
float Rand(float a, float b) { return std::uniform_real_distribution<float>(a, b)(g_rng); }

Bone RandomBone()
{
    // A rotation (from a random unit quaternion) with some scale, and a translation.
    float q[4] = {Rand(-1, 1), Rand(-1, 1), Rand(-1, 1), Rand(-1, 1)};
    float l = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (float& c : q) c /= l;
    float x = q[0], y = q[1], z = q[2], w = q[3], s = Rand(0.8f, 1.2f);
    float r[9] = {1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w),
                  2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w),
                  2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)};
    Bone b{};
    for (int i = 0; i < 9; ++i) b.m[i] = r[i] * s;
    for (int i = 9; i < 12; ++i) b.m[i] = Rand(-3, 3);
    return b;
}

Character Make(int groups, int piecesPerGroup, int vertices, int bones, bool rest, int badBones)
{
    Character c;
    c.rest = rest;
    for (int i = 0; i < bones; ++i) c.bones.push_back(RandomBone());
    c.groups.resize(groups);
    for (auto& g : c.groups) {
        g.resize(piecesPerGroup);
        for (Piece& p : g) {
            int n = vertices + int(Rand(0, 7));
            for (int i = 0; i < n; ++i) {
                TriVertex v{};
                for (int k = 0; k < 3; ++k) {
                    v.posA[k] = Rand(-1, 1); v.posB[k] = Rand(-1, 1); v.bind[k] = Rand(-2, 2);
                    v.normal[k] = Rand(-1, 1);
                }
                v.uv[0] = Rand(0, 1); v.uv[1] = Rand(0, 1);
                v.boneA = int(Rand(0, float(bones) - 0.01f));
                v.boneB = int(Rand(0, float(bones) - 0.01f));
                float r = Rand(0, 1);
                v.weightA = r < 0.3f ? 1.0f : r < 0.35f ? 0.99f : r < 0.4f ? 0.995f : Rand(0, 1);
                if (badBones && i % 37 == 5) (i & 1 ? v.boneA : v.boneB) = bones + badBones;
                p.vertices.push_back(v);
            }
            p.tris = uint32_t(n / 3);
            for (uint32_t t = 0; t < p.tris * 3; ++t) p.indices.push_back(uint16_t(t));
        }
    }
    return c;
}

// Lays the character out in memory the way randy31 does (cat.h).
void Build(Character& c)
{
    const int groups = int(c.groups.size());
    size_t pieces = 0;
    for (auto& g : c.groups) pieces += g.size();
    c.vbs.resize(pieces);
    c.impls.resize(pieces);
    c.vertexBuffers.resize(pieces);
    c.meshGroupBytes.resize(groups);
    c.pieceBytes.resize(groups);
    c.meshBytes.resize(groups);
    c.slots.resize(groups);
    c.renderGroups.resize(groups);
    size_t vb = 0;
    for (int g = 0; g < groups; ++g) {
        auto& pieceBytes = c.pieceBytes[g];
        pieceBytes.assign(kPieceSize * c.groups[g].size(), 0);
        for (size_t p = 0; p < c.groups[g].size(); ++p, ++vb) {
            Piece& piece = c.groups[g][p];
            uint8_t* pb = pieceBytes.data() + p * kPieceSize;
            At<TriVertex*>(pb, kPieceVertices) = piece.vertices.data();
            At<int32_t>(pb, kPieceActiveTris) = int32_t(piece.tris);
            At<uint16_t*>(pb, kPieceIndices) = piece.indices.data();
            At<uint32_t>(pb, kPieceVertexCount) = uint32_t(piece.vertices.size());
            At<uint32_t>(pb, kPieceTriCount) = piece.tris;
            c.vbs[vb].data.resize(piece.vertices.size());
            for (Vertex& v : c.vbs[vb].data)             // some old contents (kept where a bone is out of range)
                for (float& f : v.pos) f = Rand(-5, 5);
            c.impls[vb] = {&c.vbs[vb], 0x112, 0, 0, 0, 32, 0};
            c.vertexBuffers[vb] = &c.impls[vb];
        }
        // Each render group g reads group g of its mesh: give every group's mesh a group array of g + 1 entries.
        auto& mg = c.meshGroupBytes[g];
        mg.assign(kGroupSize * (g + 1), 0);
        At<int32_t>(mg.data() + g * kGroupSize, kGroupPieceCount) = int32_t(c.groups[g].size());
        At<uint8_t*>(mg.data() + g * kGroupSize, kGroupPieces) = pieceBytes.data();
        c.meshBytes[g].assign(0x60, 0);
        At<uint8_t*>(c.meshBytes[g].data(), kMeshGroups) = mg.data();
        c.slots[g].resize(c.groups[g].size());
        c.renderGroups[g] = {c.meshBytes[g].data(), 0, c.slots[g].data()};
    }
    vb = 0;
    for (int g = 0; g < groups; ++g)
        for (auto& slot : c.slots[g]) slot = {nullptr, &c.vertexBuffers[vb++]};
    At<void*>(c.render, kRenderAnim) = &c.anim;
    At<int32_t>(c.render, kRenderGroupCount) = groups;
    At<RenderGroup*>(c.render, kRenderGroups) = c.renderGroups.data();
    At<int32_t>(c.render, kRenderBoneCount) = int32_t(c.bones.size());
    At<Bone*>(c.render, kRenderBones) = c.bones.data();
    At<uint8_t>(c.render, kRenderRest) = c.rest ? 1 : 0;
    At<Callback*>(c.render, kRenderCallbacks) = c.callbacks.data();
    At<Callback*>(c.render, kRenderCallbacks + 4) = c.callbacks.data() + c.callbacks.size();
}

// A callback that records what it was given.
struct CallRecord {
    uint32_t count, tris, vertexBase, triBase;
    const void* in;
    const void* indices;
    float firstPos[3];
};
void __cdecl Recorder(const void*, uint32_t count, const TriVertex* in, Vertex* out, uint32_t tris,
                      const uint16_t* indices, uint32_t vertexBase, uint32_t triBase, void* user)
{
    auto* log = static_cast<std::vector<CallRecord>*>(user);
    CallRecord r{count, tris, vertexBase, triBase, in, indices, {}};
    if (count) std::memcpy(r.firstPos, out[0].pos, 12);
    log->push_back(r);
}

using OriginalFn = void(__fastcall*)(void* render, void* edx, float* boxMin, float* boxMax);
OriginalFn g_original;
skin::LockFn g_lock;
skin::UnlockFn g_unlock;

bool Near(float a, float b, float* worst)
{
    float e = std::fabs(a - b) / (1.0f + std::fabs(b));
    if (e > *worst) *worst = e;
    return e <= 2e-5f;
}

void Compare(const char* name, int groups, int pieces, int vertices, int bones, bool rest, int badBones, bool boxes,
             bool callbacks)
{
    Character theirs = Make(groups, pieces, vertices, bones, rest, badBones);
    std::mt19937 saved = g_rng;
    Character ours = theirs;
    g_rng = saved;
    std::vector<CallRecord> theirCalls, ourCalls;
    if (callbacks) {
        theirs.callbacks = {{&theirCalls, &Recorder}, {&theirCalls, &Recorder}};
        ours.callbacks = {{&ourCalls, &Recorder}, {&ourCalls, &Recorder}};
    }
    Build(theirs);
    Build(ours);
    for (size_t i = 0; i < ours.vbs.size(); ++i) ours.vbs[i].data = theirs.vbs[i].data;   // same old contents
    float tMin[3] = {1, 2, 3}, tMax[3] = {4, 5, 6}, oMin[3] = {7, 8, 9}, oMax[3] = {1, 1, 1};
    g_original(theirs.render, nullptr, boxes ? tMin : nullptr, boxes ? tMax : nullptr);
    skin::SkinRender(ours.render, boxes ? oMin : nullptr, boxes ? oMax : nullptr, g_lock, g_unlock);
    float worst = 0.0f;
    size_t bad = 0, total = 0;
    for (size_t i = 0; i < ours.vbs.size(); ++i) {
        Check(ours.vbs[i].locks == theirs.vbs[i].locks && ours.vbs[i].unlocks == theirs.vbs[i].unlocks, "lock count");
        for (size_t v = 0; v < ours.vbs[i].data.size(); ++v, ++total) {
            const Vertex &a = ours.vbs[i].data[v], &b = theirs.vbs[i].data[v];
            bool same = true;
            for (int k = 0; k < 3; ++k)
                same &= Near(a.pos[k], b.pos[k], &worst) && Near(a.normal[k], b.normal[k], &worst);
            same &= a.uv[0] == b.uv[0] && a.uv[1] == b.uv[1];
            if (!same && ++bad <= 3)
                std::printf("  %s vb %zu vertex %zu: ours (%g %g %g | %g %g %g) original (%g %g %g | %g %g %g)\n",
                            name, i, v, a.pos[0], a.pos[1], a.pos[2], a.normal[0], a.normal[1], a.normal[2], b.pos[0],
                            b.pos[1], b.pos[2], b.normal[0], b.normal[1], b.normal[2]);
        }
    }
    float boxWorst = 0.0f;
    bool boxSame = true;
    if (boxes)
        for (int k = 0; k < 3; ++k)
            boxSame &= Near(oMin[k], tMin[k], &boxWorst) && Near(oMax[k], tMax[k], &boxWorst);
    bool callsSame = theirCalls.size() == ourCalls.size();
    for (size_t i = 0; callsSame && i < ourCalls.size(); ++i) {
        const CallRecord &a = ourCalls[i], &b = theirCalls[i];
        // `in` / `indices` point into each character's own copy: compare offsets, not addresses.
        callsSame = a.count == b.count && a.tris == b.tris && a.vertexBase == b.vertexBase && a.triBase == b.triBase &&
                    std::fabs(a.firstPos[0] - b.firstPos[0]) < 1e-3f;
    }
    std::printf("%-28s %7zu vertices: %zu differ (largest relative error %.2g), box %s (%g %g %g)-(%g %g %g), "
                "callbacks %zu %s\n", name, total, bad, worst, boxes ? (boxSame ? "same" : "DIFFERS") : "-",
                boxes ? tMin[0] : 0.0f, boxes ? tMin[1] : 0.0f, boxes ? tMin[2] : 0.0f, boxes ? tMax[0] : 0.0f,
                boxes ? tMax[1] : 0.0f, boxes ? tMax[2] : 0.0f, ourCalls.size(), callsSame ? "same" : "DIFFER");
    if (boxes && !boxSame)
        std::printf("  ours (%g %g %g)-(%g %g %g)\n", oMin[0], oMin[1], oMin[2], oMax[0], oMax[1], oMax[2]);
    Check(bad == 0, name);
    Check(!boxes || boxSame, "box");
    Check(callsSame, "callbacks");
}

double Seconds(std::chrono::steady_clock::time_point since)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
}

void Time()
{
    // A crowd-sized load: 200 characters' worth of vertices, skinned 20 times.
    Character c = Make(4, 6, 120, 40, false, 0);
    Build(c);
    float mn[3], mx[3];
    const int reps = 200 * 20;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < reps; ++i) g_original(c.render, nullptr, mn, mx);
    double original = Seconds(t0);
    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < reps; ++i) skin::SkinRender(c.render, mn, mx, g_lock, g_unlock);
    double native = Seconds(t0);
    size_t vertices = 0;
    for (auto& g : c.groups) for (auto& p : g) vertices += p.vertices.size();
    std::printf("timing: %zu vertices x %d: original %.1f ns/vertex, native %.1f ns/vertex (%.1fx)\n", vertices, reps,
                original * 1e9 / (double(vertices) * reps), native * 1e9 / (double(vertices) * reps), original / native);
}

}  // namespace

int main(int argc, char** argv)
{
    const char* path = argc > 1 ? argv[1] : "randy31_orig.dll";
    HMODULE orig = LoadLibraryExA(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!orig) {
        std::printf("can't load %s (%lu)\n", path, GetLastError());
        return 2;
    }
    g_original = reinterpret_cast<OriginalFn>(reinterpret_cast<uint8_t*>(orig) + 0x5470D);
    g_lock = reinterpret_cast<skin::LockFn>(GetProcAddress(orig, "?Lock@VertexBuffer_c@@QAEPAXII@Z"));
    g_unlock = reinterpret_cast<skin::UnlockFn>(GetProcAddress(orig, "?Unlock@VertexBuffer_c@@QAEXXZ"));
    if (!g_lock || !g_unlock) {
        std::printf("VertexBuffer_c exports missing\n");
        return 2;
    }
    Compare("skin: one bone and two", 3, 4, 50, 30, false, 0, true, false);
    Compare("skin: rest pose", 2, 3, 40, 10, true, 0, true, false);
    Compare("skin: bones out of range", 2, 5, 80, 20, false, 3, true, false);
    Compare("skin: callbacks", 3, 2, 30, 12, false, 0, true, true);
    Compare("skin: no box", 1, 2, 20, 8, false, 0, false, false);
    Compare("skin: one vertex pieces", 2, 3, 1, 4, false, 0, true, false);
    Compare("skin: big", 6, 8, 600, 60, false, 1, true, true);
    Time();
    std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "all passed", g_failures);
    return g_failures ? 1 : 0;
}
