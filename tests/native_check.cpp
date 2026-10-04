// Runs native replacements (proxy/native) and the original code they replace in randy31_orig.dll on the same
// synthetic input and compares the results, then times both. Run under Wine with tools/native-check.sh.
//
//   native_check.exe <path to randy31_orig.dll>
#include "native/cat_anim.h"
#include "native/cat_query.h"
#include "native/cat_skin.h"
#include "native/vc10.h"
#include "native/xmath.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
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
        for (auto& slot : c.slots[g]) slot = {0, &c.vertexBuffers[vb++]};
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

// Job::ComputeBounds: a box from the bones and the mesh's per-bone boxes must hold every skinned vertex.
void CheckBounds()
{
    for (int round = 0; round < 6; ++round) {
        Character c = Make(1, 1, 400, 20, round == 5, round == 4 ? 2 : 0);
        Piece& piece = c.groups[0][0];
        auto source = std::make_shared<rvk::skin::Source>();
        source->vertices = piece.vertices;
        source->indices = piece.indices;
        source->Finish();
        auto palette = std::make_shared<rvk::skin::Palette>();
        palette->Set(c.bones.data(), uint32_t(c.bones.size()));
        auto job = std::make_shared<rvk::skin::Job>();
        job->source = source;
        job->bones = palette;
        job->rest = round == 5;
        job->ComputeBounds();
        const rvk::skin::Vertex* v = job->Skinned();
        float exactMin[3] = {1e30f, 1e30f, 1e30f}, exactMax[3] = {-1e30f, -1e30f, -1e30f};
        bool inside = true;
        for (size_t i = 0; i < piece.vertices.size(); ++i)
            for (int j = 0; j < 3; ++j) {
                exactMin[j] = std::min(exactMin[j], v[i].pos[j]);
                exactMax[j] = std::max(exactMax[j], v[i].pos[j]);
                inside &= v[i].pos[j] >= job->boundsMin[j] - 1e-4f && v[i].pos[j] <= job->boundsMax[j] + 1e-4f;
            }
        float grow = 0.0f;
        for (int j = 0; j < 3; ++j)
            grow = std::max(grow, (job->boundsMax[j] - job->boundsMin[j]) / std::max(exactMax[j] - exactMin[j], 1e-3f));
        std::printf("bounds round %d: %s, box up to %.2fx the exact one\n", round, inside ? "holds every vertex" : "MISSES",
                    grow);
        Check(inside, "job bounds hold the vertices");
    }
}

// ---- Animation: keyframe sampling and the bone hierarchy against FUN_10051d2a / FUN_10051df4 / FUN_100540a5 ----
HMODULE g_orig;

struct TrackSet {
    std::vector<std::vector<anim::RotationKey>> rotations;
    std::vector<std::vector<anim::PositionKey>> positions;
    std::vector<anim::Track> tracks;
    std::vector<uint8_t> data;                       // CATKeyframeAnimData_t: +0x38 tracks
};

TrackSet MakeTracks(int bones)
{
    TrackSet s;
    s.rotations.resize(bones);
    s.positions.resize(bones);
    for (int b = 0; b < bones; ++b) {
        int nr = b % 5 == 0 ? 1 : int(Rand(2, 30)), np = b % 7 == 0 ? 1 : int(Rand(2, 30));
        float t = 0.0f;
        for (int k = 0; k < nr; ++k) {
            float q[4] = {Rand(-1, 1), Rand(-1, 1), Rand(-1, 1), Rand(-1, 1)};
            float l = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
            anim::RotationKey key{t, {q[0] / l, q[1] / l, q[2] / l, q[3] / l}};
            if (k % 4 == 3) std::memcpy(key.q, s.rotations[b].back().q, 16);   // equal neighbours: linear
            s.rotations[b].push_back(key);
            t += Rand(10, 200);
        }
        t = 0.0f;
        for (int k = 0; k < np; ++k) {
            s.positions[b].push_back({t, {Rand(-1, 1), Rand(-1, 1), Rand(-1, 1)}});
            t += Rand(10, 200);
        }
    }
    for (int b = 0; b < bones; ++b)
        s.tracks.push_back({int32_t(s.rotations[b].size()), s.rotations[b].data(), int32_t(s.positions[b].size()),
                            s.positions[b].data()});
    s.data.assign(0x48, 0);
    At<anim::Track*>(s.data.data(), anim::kDataTracks) = s.tracks.data();
    return s;
}

using SampleFn = void(__fastcall*)(const void* data, void*, float* out, int32_t bone, float time);

void CheckSampling()
{
    auto rotation = reinterpret_cast<SampleFn>(reinterpret_cast<uint8_t*>(g_orig) + 0x51D2A);
    auto position = reinterpret_cast<SampleFn>(reinterpret_cast<uint8_t*>(g_orig) + 0x51DF4);
    TrackSet s = MakeTracks(40);
    size_t checked = 0, bad = 0;
    float worst = 0.0f;
    for (int b = 0; b < 40; ++b) {
        float end = std::max(s.rotations[b].back().time, s.positions[b].back().time);
        std::vector<float> times = {-5.0f, 0.0f, end, end + 1.0f};
        for (int k = 0; k < 60; ++k) times.push_back(Rand(0, end));
        for (const auto& key : s.rotations[b]) times.push_back(key.time), times.push_back(key.time + 0.001f);
        for (float time : times) {
            float a[4], o[4], pa[3], po[3];
            rotation(s.data.data(), nullptr, o, b, time);
            anim::SampleRotation(s.tracks[b], time, a);
            position(s.data.data(), nullptr, po, b, time);
            anim::SamplePosition(s.tracks[b], time, pa);
            bool same = true;
            for (int j = 0; j < 4; ++j) same &= Near(a[j], o[j], &worst);
            for (int j = 0; j < 3; ++j) same &= Near(pa[j], po[j], &worst);
            ++checked;
            if (!same && ++bad <= 3)
                std::printf("  bone %d time %g: ours (%g %g %g %g | %g %g %g) original (%g %g %g %g | %g %g %g)\n", b,
                            time, a[0], a[1], a[2], a[3], pa[0], pa[1], pa[2], o[0], o[1], o[2], o[3], po[0], po[1], po[2]);
        }
    }
    std::printf("anim: %zu samples, %zu differ (largest relative error %.2g)\n", checked, bad, worst);
    Check(bad == 0, "keyframe sampling");
}

// A bone hierarchy (CATMesh_t bones with children and child scales) driven by a keyframe animation object with the
// original's vtable, plus controllers of type 1 and 2 on a few bones.
struct FakeHierarchy {
    TrackSet tracks;
    std::vector<uint8_t> meshBones;                  // 0x28 each
    std::vector<std::vector<int32_t>> children;
    std::vector<uint8_t> mesh, anim, render;
    std::vector<Bone> out;
    std::vector<uint8_t> controllers;               // 0x10 each
};

void __cdecl Controller1(void*, float*, float* q, float* position, void* user)
{
    float k = *static_cast<float*>(user);
    q[0] *= k; position[1] += k;
}
void __cdecl Controller2(float* out, void*, float* parent, float*, float* position, void*)
{
    for (int i = 0; i < 12; ++i) out[i] = parent[i] * 0.5f;
    out[9] += position[0];
}
float g_controllerScale = 0.75f;

void BuildHierarchy(FakeHierarchy& h, int bones, float time)
{
    h.tracks = MakeTracks(bones);
    h.children.assign(bones, {});
    for (int b = 1; b < bones; ++b) h.children[int(Rand(0, float(b) - 0.01f))].push_back(b);
    h.meshBones.assign(size_t(bones) * 0x28, 0);
    for (int b = 0; b < bones; ++b) {
        uint8_t* mb = h.meshBones.data() + b * 0x28;
        At<float>(mb, 0x1C) = Rand(0.8f, 1.2f);
        At<int32_t>(mb, 0x20) = int32_t(h.children[b].size());
        At<int32_t*>(mb, 0x24) = h.children[b].data();
    }
    h.mesh.assign(0x64, 0);
    At<uint8_t*>(h.mesh.data(), 0x44) = h.meshBones.data();
    h.anim.assign(0x60, 0);
    At<uintptr_t>(h.anim.data(), 0) = reinterpret_cast<uintptr_t>(g_orig) + 0x95BA4;   // CATKeyframeAnim_t vtable
    At<void*>(h.anim.data(), 0x4C) = h.tracks.data.data();
    At<float>(h.anim.data(), 0x50) = time;
    h.out.assign(bones, Bone{});
    h.controllers.assign(size_t(bones) * 0x10, 0);
    for (int b = 3; b < bones; b += 7) {
        uint8_t* c = h.controllers.data() + b * 0x10;
        bool second = (b / 7) % 2;
        At<int32_t>(c, 0) = second ? 2 : 1;
        At<void*>(c, 4) = &g_controllerScale;
        At<void*>(c, second ? 0xC : 8) = second ? reinterpret_cast<void*>(&Controller2) : reinterpret_cast<void*>(&Controller1);
    }
    h.render.assign(0x60, 0);
    At<void*>(h.render.data(), cat::kRenderMesh) = h.mesh.data();
    At<void*>(h.render.data(), cat::kRenderAnim) = h.anim.data();
    At<int32_t>(h.render.data(), cat::kRenderBoneCount) = bones;
    At<Bone*>(h.render.data(), cat::kRenderBones) = h.out.data();
    At<uint8_t*>(h.render.data(), 0x20) = h.controllers.data();
}

void CheckHierarchy()
{
    using HierarchyFn = void(__fastcall*)(void* render, void*, float* parent, int32_t bone, float scale);
    auto original = reinterpret_cast<HierarchyFn>(reinterpret_cast<uint8_t*>(g_orig) + 0x540A5);
    size_t checked = 0, bad = 0;
    float worst = 0.0f;
    for (int round = 0; round < 8; ++round) {
        std::mt19937 saved = g_rng;
        FakeHierarchy theirs, ours;
        BuildHierarchy(theirs, 40, Rand(0, 2000));
        g_rng = saved;
        BuildHierarchy(ours, 40, Rand(0, 2000));
        float identity[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
        original(theirs.render.data(), nullptr, identity, 0, 1.0f);
        // Ours, with the hooks' entry point: its own recursion and sampling.
        anim::SetKeyframeVtable(reinterpret_cast<uintptr_t>(g_orig) + 0x95BA4);
        anim::Hierarchy(ours.render.data(), identity, 0, 1.0f);
        for (int b = 0; b < 40; ++b) {
            bool same = true;
            for (int i = 0; i < 12; ++i) same &= Near(ours.out[b].m[i], theirs.out[b].m[i], &worst);
            ++checked;
            if (!same && ++bad <= 3)
                std::printf("  round %d bone %d: ours t (%g %g %g) original (%g %g %g)\n", round, b, ours.out[b].m[9],
                            ours.out[b].m[10], ours.out[b].m[11], theirs.out[b].m[9], theirs.out[b].m[10],
                            theirs.out[b].m[11]);
        }
    }
    std::printf("hierarchy: %zu bone matrices, %zu differ (largest relative error %.2g)\n", checked, bad, worst);
    Check(bad == 0, "bone hierarchy");
}

// A random rotation (unit quaternion) as a matrix, scaled, with a translation.
xm::M4 RandomTransform(float scale)
{
    float q[4], n = 0;
    for (float& v : q) v = Rand(-1, 1), n += v * v;
    n = std::sqrt(n);
    for (float& v : q) v /= n;
    xm::M4 m = xm::FromQuaternion(q);
    xm::ScaleColumns(m, scale, scale, scale);
    for (int i = 12; i < 15; ++i) m.m[i] = Rand(-10, 10);
    return m;
}

void CheckMath()
{
    auto toQuat = reinterpret_cast<float*(__fastcall*)(float*, void*, const float*)>(reinterpret_cast<uint8_t*>(g_orig) + 0x6ED0B);
    auto fromQuat = reinterpret_cast<void(__fastcall*)(float*, void*, const float*)>(reinterpret_cast<uint8_t*>(g_orig) + 0x6EBED);
    auto mul = reinterpret_cast<float*(__fastcall*)(const float*, void*, float*, const float*)>(reinterpret_cast<uint8_t*>(g_orig) + 0x6E302);
    size_t bad = 0;
    float worst = 0.0f;
    for (int i = 0; i < 2000; ++i) {
        xm::M4 a = RandomTransform(i % 3 == 0 ? Rand(0.5f, 2.0f) : 1.0f), b = RandomTransform(1.0f);
        float qo[4], qa[4];
        toQuat(qo, nullptr, a.m);
        xm::ToQuaternion(a, qa);
        xm::M4 mo, ma = xm::FromQuaternion(qa), po, pa = xm::Mul(a, b);
        fromQuat(mo.m, nullptr, qa);
        mul(a.m, nullptr, po.m, b.m);
        bool same = true;
        for (int j = 0; j < 4; ++j) same &= Near(qa[j], qo[j], &worst);
        for (int j = 0; j < 16; ++j) same &= Near(ma.m[j], mo.m[j], &worst) && Near(pa.m[j], po.m[j], &worst);
        if (!same && ++bad <= 3)
            std::printf("  matrix %d: quaternion ours (%g %g %g %g) original (%g %g %g %g)\n", i, qa[0], qa[1], qa[2],
                        qa[3], qo[0], qo[1], qo[2], qo[3]);
    }
    std::printf("math: 2000 quaternions / rotations / products, %zu differ (largest relative error %.2g)\n", bad, worst);
    Check(bad == 0, "matrix / quaternion helpers");
}

// A character with named attractors on its mesh groups and named bones.
struct FakeAttach {
    std::vector<std::vector<uint8_t>> meshes;        // CATMesh_t per group
    std::vector<std::vector<uint8_t>> groupRecords;  // the mesh's group records (0x34 each)
    std::vector<std::vector<uint8_t>> attractors;    // 0x40 each
    std::vector<uint8_t> boneNames;                  // 0x28 each
    std::vector<RenderGroup> groups;
    std::vector<Bone> bones;
    std::vector<uint8_t> render, anim;
};

void Name(void* at, const char* s)
{
    auto* str = static_cast<vc10::String*>(at);
    str->init();
    str->assign(s, std::strlen(s));
}

void BuildAttach(FakeAttach& f, int groupCount, int boneCount)
{
    f.bones.resize(boneCount);
    for (auto& b : f.bones) {
        xm::M4 m = RandomTransform(Rand(0.7f, 1.4f));
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 3; ++j) b.m[i * 3 + j] = m.m[i * 4 + j];
    }
    f.boneNames.assign(size_t(boneCount) * 0x28, 0);
    char name[64];
    for (int b = 0; b < boneCount; ++b) {
        std::snprintf(name, sizeof(name), b % 4 == 0 ? "Bip01 a long bone name %d" : "bone%d", b);
        Name(f.boneNames.data() + b * 0x28, name);
    }
    f.meshes.assign(groupCount, std::vector<uint8_t>(0x64, 0));
    f.groupRecords.assign(groupCount, std::vector<uint8_t>(size_t(groupCount) * 0x34, 0));
    f.attractors.assign(groupCount, {});
    f.groups.assign(groupCount, RenderGroup{});
    for (int g = 0; g < groupCount; ++g) {
        int count = g == 1 ? 0 : 3;
        f.attractors[g].assign(size_t(count) * 0x40, 0);
        for (int i = 0; i < count; ++i) {
            uint8_t* a = f.attractors[g].data() + i * 0x40;
            std::snprintf(name, sizeof(name), i == 2 ? "attractor with a long name %d-%d" : "attr%d-%d", g, i);
            Name(a, name);
            for (int k = 0; k < 3; ++k) At<float>(a, 0x1C + k * 4) = Rand(-1, 1);
            float q[4], n = 0;
            for (float& v : q) v = Rand(-1, 1), n += v * v;
            for (int k = 0; k < 4; ++k) At<float>(a, 0x28 + k * 4) = q[k] / std::sqrt(n);
            At<float>(a, 0x38) = Rand(0.5f, 1.5f);
            At<int32_t>(a, 0x3C) = int32_t(Rand(0, float(boneCount) - 0.01f));
        }
        uint8_t* record = f.groupRecords[g].data() + g * 0x34;   // group g reads its own index from its mesh
        At<int32_t>(record, 0x2C) = count;
        At<uint8_t*>(record, 0x30) = f.attractors[g].data();
        At<uint8_t*>(f.meshes[g].data(), cat::kMeshGroups) = f.groupRecords[g].data();
        f.groups[g].mesh = f.meshes[g].data();
    }
    At<int32_t>(f.meshes[0].data(), 0x40) = boneCount;
    At<uint8_t*>(f.meshes[0].data(), 0x44) = f.boneNames.data();
    f.anim.assign(0x60, 0);
    f.render.assign(0x60, 0);
    At<void*>(f.render.data(), cat::kRenderMesh) = f.meshes[0].data();
    At<void*>(f.render.data(), cat::kRenderAnim) = f.anim.data();
    At<int32_t>(f.render.data(), cat::kRenderGroupCount) = groupCount;
    At<RenderGroup*>(f.render.data(), cat::kRenderGroups) = f.groups.data();
    At<int32_t>(f.render.data(), cat::kRenderBoneCount) = boneCount;
    At<Bone*>(f.render.data(), cat::kRenderBones) = f.bones.data();
}

void CheckAttractors()
{
    using LookupFn = uint32_t(__fastcall*)(const void* render, void*, const char* name, float* out);
    auto attractor = reinterpret_cast<LookupFn>(reinterpret_cast<uint8_t*>(g_orig) + 0x54DF1);
    auto bone = reinterpret_cast<LookupFn>(reinterpret_cast<uint8_t*>(g_orig) + 0x54F4F);
    FakeAttach f;
    BuildAttach(f, 4, 30);
    std::vector<std::string> names = {"nope", "", "bone"};
    char name[64];
    for (int g = 0; g < 4; ++g)
        for (int i = 0; i < 3; ++i) {
            std::snprintf(name, sizeof(name), i == 2 ? "attractor with a long name %d-%d" : "attr%d-%d", g, i);
            names.push_back(name);
        }
    for (int b = 0; b < 30; ++b) {
        std::snprintf(name, sizeof(name), b % 4 == 0 ? "Bip01 a long bone name %d" : "bone%d", b);
        names.push_back(name);
    }
    size_t checked = 0, bad = 0;
    float worst = 0.0f;
    for (int withAnim = 1; withAnim >= 0; --withAnim) {
        At<void*>(f.render.data(), cat::kRenderAnim) = withAnim ? f.anim.data() : nullptr;
        for (const auto& n : names)
            for (int which = 0; which < 2; ++which) {
                float o[16] = {}, a[16] = {};
                bool ro = ((which ? bone : attractor)(f.render.data(), nullptr, n.c_str(), o) & 0xFF) != 0;
                bool ra = which ? catquery::BoneMatrix(f.render.data(), n.c_str(), a)
                                : catquery::AttractorMatrix(f.render.data(), n.c_str(), a);
                bool same = ro == ra;
                for (int j = 0; j < 16; ++j) same &= Near(a[j], o[j], &worst);
                ++checked;
                if (!same && ++bad <= 3)
                    std::printf("  %s '%s' (anim %d): ours %d (%g %g %g) original %d (%g %g %g)\n",
                                which ? "bone" : "attractor", n.c_str(), withAnim, ra, a[12], a[13], a[14], ro, o[12],
                                o[13], o[14]);
            }
    }
    std::printf("attractors / bones by name: %zu lookups, %zu differ (largest relative error %.2g)\n", checked, bad, worst);
    Check(bad == 0, "attractor / bone lookups");
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
    g_orig = orig;
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
    CheckBounds();
    CheckSampling();
    CheckHierarchy();
    CheckMath();
    CheckAttractors();
    Time();
    std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "all passed", g_failures);
    return g_failures ? 1 : 0;
}
