// Runs native replacements (proxy/native) and the original code they replace in randy31_orig.dll on the same
// synthetic input and compares the results, then times both. Run under Wine with tools/native-check.sh.
//
//   native_check.exe <path to randy31_orig.dll>
#include "native/cat_anim.h"
#include "native/cat_query.h"
#include "native/cat_skin.h"
#include "native/color.h"
#include "native/dxerror.h"
#include "native/helpers.h"
#include "native/keyframe.h"
#include "native/lbitmap.h"
#include "native/orig_api.gen.h"
#include "native/pixfmt.h"
#include "native/shadowlands.h"
#include "native/texture_stream.h"
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

// Blends (CATAnimBlend_t, made by the original's constructor) of keyframe animations with per-bone layer masks, and a
// blend of a blend: the original hierarchy through their vtables against ours.
struct FakeKeyframes {
    TrackSet tracks;
    std::vector<uint8_t> object, mask;
};

void MakeKeyframes(FakeKeyframes& k, int bones, float time, float radius)
{
    k.tracks = MakeTracks(bones);
    At<float>(k.tracks.data.data(), 0x30) = radius;
    k.mask.resize(bones);
    for (auto& m : k.mask) m = Rand(0, 1) < 0.7f ? 1 : 0;
    k.object.assign(0x60, 0);
    At<uintptr_t>(k.object.data(), 0) = reinterpret_cast<uintptr_t>(g_orig) + 0x95BA4;
    At<void*>(k.object.data(), 0x4C) = k.tracks.data.data();
    At<float>(k.object.data(), 0x50) = time;
    At<int32_t>(k.object.data(), 0x58) = bones;
    At<uint8_t*>(k.object.data(), 0x5C) = k.mask.data();
    At<int32_t>(k.object.data(), 0x2C) = int32_t(Rand(0, 100));
}

void CheckBlends()
{
    using BlendCtorFn = void*(__fastcall*)(void* self, void*, void* a, void* b, float blend);
    auto blendCtor = reinterpret_cast<BlendCtorFn>(GetProcAddress(g_orig, "??0CATAnimBlend_t@@QAE@PAVCATAnim_t@@0M@Z"));
    using HierarchyFn = void(__fastcall*)(void* render, void*, float* parent, int32_t bone, float scale);
    auto original = reinterpret_cast<HierarchyFn>(reinterpret_cast<uint8_t*>(g_orig) + 0x540A5);
    using RadiusFn = float(__fastcall*)(void*);
    using VersionFn = int32_t(__fastcall*)(void*);
    anim::SetKeyframeVtable(reinterpret_cast<uintptr_t>(g_orig) + 0x95BA4);
    anim::SetBlendVtable(reinterpret_cast<uintptr_t>(g_orig) + 0x95B40);
    size_t checked = 0, bad = 0;
    float worst = 0.0f;
    for (int round = 0; round < 6; ++round) {
        const int bones = 40;
        FakeHierarchy h;
        BuildHierarchy(h, bones, Rand(0, 2000));
        static FakeKeyframes k[3];                   // kept: the blends hold references
        for (int i = 0; i < 3; ++i) MakeKeyframes(k[i], bones, Rand(0, 2000), Rand(0.5f, 3.0f));
        void* inner = blendCtor(rnative::vc10::Allocate(0x100), nullptr, k[0].object.data(), k[1].object.data(),
                                Rand(0, 1));
        void* blend = round % 2 ? blendCtor(rnative::vc10::Allocate(0x100), nullptr, inner, k[2].object.data(), Rand(0, 1))
                                : inner;
        At<void*>(h.render.data(), cat::kRenderAnim) = blend;
        float identity[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
        original(h.render.data(), nullptr, identity, 0, 1.0f);
        std::vector<Bone> theirs = h.out;
        std::fill(h.out.begin(), h.out.end(), Bone{});
        anim::Hierarchy(h.render.data(), identity, 0, 1.0f);
        for (int b = 0; b < bones; ++b) {
            bool same = true;
            for (int i = 0; i < 12; ++i) same &= Near(h.out[b].m[i], theirs[b].m[i], &worst);
            ++checked;
            if (!same && ++bad <= 3)
                std::printf("  blend round %d bone %d: ours t (%g %g %g) original (%g %g %g)\n", round, b, h.out[b].m[9],
                            h.out[b].m[10], h.out[b].m[11], theirs[b].m[9], theirs[b].m[10], theirs[b].m[11]);
        }
        void** vtable = *static_cast<void***>(blend);
        float ro = reinterpret_cast<RadiusFn>(vtable[7])(blend), ra = anim::Radius(blend);
        // The version slot remembers the inner versions (in the blends): from the same state, the same answer and
        // the same state after; then once more after an inner animation changed.
        for (int step = 0; step < 2; ++step) {
            uint8_t before[2][0x70], after[2][0x70];
            void* objects[2] = {blend, inner};
            for (int i = 0; i < 2; ++i) std::memcpy(before[i], objects[i], 0x70);
            int32_t vo = reinterpret_cast<VersionFn>(vtable[8])(blend);
            for (int i = 0; i < 2; ++i) std::memcpy(after[i], objects[i], 0x70);
            for (int i = 1; i >= 0; --i) std::memcpy(objects[i], before[i], 0x70);
            int32_t va = anim::Version(blend);
            bool same = va == vo;
            for (int i = 0; i < 2; ++i) same &= std::memcmp(after[i], objects[i], 0x70) == 0;
            ++checked;
            if (!same && ++bad <= 6) std::printf("  blend round %d version: ours %d original %d\n", round, va, vo);
            At<int32_t>(k[step * 2].object.data(), 0x2C) += 3;
        }
        ++checked;
        if (ro != ra && ++bad <= 6) std::printf("  blend round %d radius: ours %g original %g\n", round, ra, ro);
    }
    std::printf("blends: %zu bone matrices / radii, %zu differ (largest relative error %.2g)\n", checked, bad, worst);
    Check(bad == 0, "animation blends");
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

// FUN_1001d619: an HRESULT's description and name (false: unknown).
using DxErrorFn = bool(__cdecl*)(int32_t hr, const char** description, const char** name);

// Every value the original knows (all 2^32 tried): hr, name, description (tab separated).
void DumpDxErrors()
{
    auto original = reinterpret_cast<DxErrorFn>(reinterpret_cast<uint8_t*>(g_orig) + 0x1D619);
    for (uint64_t v = 0; v < 0x100000000ull; ++v) {
        const char *description = nullptr, *name = nullptr;
        if (original(int32_t(uint32_t(v)), &description, &name))
            std::printf("%08X\t%s\t%s\n", uint32_t(v), name, description);
    }
}

// Our HRESULT table against the original's switch, over the 64K blocks its comparisons fall in (the full 2^32 is
// --dump-dxerrors); GetErrorString on DXErrors made by the original's constructor.
void CheckDxErrors()
{
    auto original = reinterpret_cast<DxErrorFn>(reinterpret_cast<uint8_t*>(g_orig) + 0x1D619);
    int differ = 0, known = 0;
    for (uint32_t high : {0x0000u, 0x8000u, 0x8007u, 0x8876u, 0x8877u, 0xC877u, 0xFFFFu})
        for (uint32_t low = 0; low < 0x10000; ++low) {
            const int32_t hr = int32_t(high << 16 | low);
            const char *d0 = nullptr, *n0 = nullptr, *d1 = nullptr, *n1 = nullptr;
            const bool a = original(hr, &d0, &n0), b = dxerror::Describe(hr, &d1, &n1);
            known += a;
            if (a != b || (a && (std::strcmp(d0, d1) || std::strcmp(n0, n1)))) ++differ;
        }
    std::printf("dxerror: %d known, %d different\n", known, differ);
    Check(differ == 0 && known == 169, "dxerror: HRESULT table");

    using CtorFn = void*(__fastcall*)(void*, void*, int32_t, const vc10::String*, const vc10::String*, int);
    using TextFn = vc10::String*(__fastcall*)(const void*, void*, vc10::String*);
    auto ctor = reinterpret_cast<CtorFn>(GetProcAddress(g_orig,
        "??0DXError@fun@@QAE@JABV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@0H@Z"));
    auto text = reinterpret_cast<TextFn>(GetProcAddress(g_orig,
        "?GetErrorString@DXError@fun@@UBE?AV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@XZ"));
    if (!ctor || !text) {
        Check(false, "dxerror: DXError exports");
        return;
    }
    const char* messages[] = {"", "short", "render_t::SetRenderState failed in a long enough message"};
    const int32_t hrs[] = {0, int32_t(0x887601AE), int32_t(0x8876017C), int32_t(0xC8770BDB), 12345, -1};
    int bad = 0;
    for (const char* m : messages)
        for (int32_t hr : hrs) {
            vc10::String message, file;
            message.init();
            file.init();
            message.assign(m, std::strlen(m));
            file.assign("render.cpp", 10);
            alignas(8) uint8_t error[0x4C];
            ctor(error, nullptr, hr, &message, &file, 42);
            vc10::String a, b;
            text(error, nullptr, &a);
            dxerror::GetErrorString(error, nullptr, &b);
            if (a.size != b.size || std::strcmp(a.c_str(), b.c_str())) {
                if (!bad) std::printf("dxerror: '%s' vs '%s'\n", a.c_str(), b.c_str());
                ++bad;
            }
        }
    Check(bad == 0, "dxerror: GetErrorString");
}

// RKeyFrameAnimation_t's evaluation (FUN_10028fde) against ours on random animations: the same objects (key lists,
// cached indices), the same times in a row (on, back, past the end, looped or held); everything it writes compared
// to the bit.
void CheckKeyframes()
{
    using EvaluateFn = void(__fastcall*)(void*, void*, float, float*, uint8_t*);
    auto original = reinterpret_cast<EvaluateFn>(reinterpret_cast<uint8_t*>(g_orig) + 0x28FDE);
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> unit(-1.0f, 1.0f), step(0.0f, 2.0f);
    auto times = [&](int n, float* out, int stride) {   // increasing, a few repeated
        float t = 0.0f;
        for (int i = 0; i < n; ++i) {
            out[i * stride] = t;
            t += (rng() % 7 == 0) ? 0.0f : step(rng);
        }
    };
    int mismatches = 0, calls = 0;
    for (int trial = 0; trial < 400; ++trial) {
        const int nt = 1 + int(rng() % 10), nr = 1 + int(rng() % 10), nv = 1 + int(rng() % 6), nu = 1 + int(rng() % 6);
        std::vector<float> trans(nt * 4), rot(nr * 5), vis(nv * 2), uv(nu * 6);
        for (float& v : trans) v = unit(rng) * 10.0f;
        for (float& v : rot) v = unit(rng);
        for (int i = 0; i < nv; ++i) reinterpret_cast<uint32_t&>(vis[i * 2 + 1]) = rng() & 1;
        for (float& v : uv) v = unit(rng) * 2.0f;
        for (int i = 0; i < nu; ++i) {
            uint32_t& flag = reinterpret_cast<uint32_t&>(uv[i * 6 + 5]);
            const uint32_t pick = rng() % 4;
            flag = pick == 0 ? 0u : pick == 1 ? 0x80000000u : pick == 2 ? 0x3F800000u : uint32_t(rng());
        }
        times(nt, trans.data() + 3, 4);
        times(nr, rot.data() + 4, 5);
        times(nv, vis.data(), 2);
        times(nu, uv.data() + 4, 6);
        const float longest = std::max({trans[(nt - 1) * 4 + 3], rot[(nr - 1) * 5 + 4], vis[(nv - 1) * 2], uv[(nu - 1) * 6 + 4]});
        alignas(16) uint8_t objects[2][0x94];
        std::vector<float> copies[2][4];
        for (int k = 0; k < 2; ++k) {
            uint8_t* a = objects[k];
            std::memset(a, 0, sizeof(objects[k]));
            copies[k][0] = trans, copies[k][1] = rot, copies[k][2] = vis, copies[k][3] = uv;
            const uint32_t at[4] = {0x3C, 0x4C, 0x5C, 0x6C};
            const size_t keyBytes[4] = {16, 20, 8, 24};
            for (int l = 0; l < 4; ++l) {
                float** v = reinterpret_cast<float**>(a + at[l]);
                v[0] = copies[k][l].data();
                v[1] = v[2] = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(v[0]) +
                                                       copies[k][l].size() * 4 / (keyBytes[l] / 4) * (keyBytes[l] / 4));
            }
            *reinterpret_cast<float*>(a + 0x7C) = longest * (0.5f + step(rng) * 0.5f);
            a[0x80] = uint8_t(trial & 1);
            *reinterpret_cast<float*>(a + 0x84) = 1.0f;
            *reinterpret_cast<float*>(a + 0x88) = 1.0f;
        }
        std::memcpy(objects[1] + 0x7C, objects[0] + 0x7C, 4);
        for (int c = 0; c < 40; ++c) {
            const float time = (c % 9 == 0) ? 0.0f : step(rng) * longest;
            float m[2][16];
            uint8_t visible[2] = {0xAA, 0xAA};
            for (int k = 0; k < 2; ++k)
                for (int i = 0; i < 16; ++i) m[k][i] = float(i) * 0.5f;
            original(objects[0], nullptr, time, m[0], &visible[0]);
            keyframe::Evaluate(objects[1], nullptr, time, m[1], &visible[1]);
            ++calls;
            if (std::memcmp(m[0], m[1], sizeof(m[0])) || visible[0] != visible[1] ||
                std::memcmp(objects[0] + 0x2C, objects[1] + 0x2C, 16) ||
                std::memcmp(objects[0] + 0x84, objects[1] + 0x84, 16)) {
                if (mismatches < 5) {
                    std::printf("keyframe: trial %d call %d time %.9g differs:", trial, c, time);
                    for (int i = 0; i < 16; ++i)
                        if (std::memcmp(&m[0][i], &m[1][i], 4)) std::printf(" m[%d] %.9g/%.9g", i, m[0][i], m[1][i]);
                    for (int i = 0; i < 4; ++i)
                        std::printf(" idx%d %d/%d", i, reinterpret_cast<int*>(objects[0] + 0x2C)[i],
                                    reinterpret_cast<int*>(objects[1] + 0x2C)[i]);
                    for (int i = 0; i < 4; ++i)
                        std::printf(" uv%d %.9g/%.9g", i, reinterpret_cast<float*>(objects[0] + 0x84)[i],
                                    reinterpret_cast<float*>(objects[1] + 0x84)[i]);
                    std::printf("\n");
                }
                ++mismatches;
            }
        }
    }
    unsigned short cw = 0;
    __asm__ volatile("fnstcw %0" : "=m"(cw));
    std::printf("keyframe: %d evaluations, %d differ (x87 control word %04x)\n", calls, mismatches, cw);
    Check(mismatches == 0, "keyframe: evaluation");
}

// RandyShadowlandsData_s against ours: the three lights' setters and getters on random input, the class's static data
// compared, and the matrix each light builds from the camera. The textures are a fake object (its refcount high, so
// AddRef / Release never reach its destructor).
template <typename F>
F Orig(uint32_t rva) { return reinterpret_cast<F>(reinterpret_cast<uint8_t*>(g_orig) + rva); }

void CheckShadowlands()
{
    shadowlands::SetModule(g_orig);
    uint8_t* base = reinterpret_cast<uint8_t*>(g_orig);
    auto at = [base](uint32_t rva) { return base + rva; };

    struct Args {
        float matrix[16], scale[3], offset[3], direction[3], intensity;
    };

    alignas(16) static uint8_t fakeTexture[0x100];
    alignas(16) static uint8_t fakeRender[0x400];
    *reinterpret_cast<uint32_t*>(fakeTexture + 0x24) = 100000;   // RResource_t's refcount: never reaches zero

    // Everything the class owns (the caller's textures excepted): scales, directions, the camera, the matrices, the
    // flags, textures, intensities and offsets.
    struct Span { uint32_t rva, size; };
    const Span spans[] = {{0xB7788, 0xB78D0 - 0xB7788}, {0x17D44C, 0x17D490 - 0x17D44C}};
    std::vector<uint8_t> a, b;
    auto snap = [&](std::vector<uint8_t>& out) {
        out.clear();
        for (const Span& s : spans) out.insert(out.end(), at(s.rva), at(s.rva) + s.size);
    };
    std::mt19937 rng(1234);
    auto seed = [&](uint32_t s) {
        std::mt19937 r(s);
        for (const Span& sp : spans)
            for (uint32_t i = 0; i < sp.size; i += 4) *reinterpret_cast<uint32_t*>(at(sp.rva) + i) = r();
        void* texture = (r() & 1) ? static_cast<void*>(fakeTexture) : nullptr;
        *reinterpret_cast<void**>(at(0x17D450)) = texture;
        *reinterpret_cast<void**>(at(0x17D458)) = (r() & 1) ? static_cast<void*>(fakeTexture) : nullptr;
        *reinterpret_cast<void**>(at(0x17D460)) = (r() & 1) ? static_cast<void*>(fakeTexture) : nullptr;
        at(0x17D44C)[0] = uint8_t(r() & 1), at(0x17D44D)[0] = uint8_t(r() & 1), at(0x17D44E)[0] = uint8_t(r() & 1);
        at(0x17D44F)[0] = uint8_t(r() & 1), at(0x17D468)[0] = uint8_t(r() & 1), at(0x17D469)[0] = uint8_t(r() & 1);
        *reinterpret_cast<uint8_t**>(at(0x16BED0)) = fakeRender;
        *reinterpret_cast<uint32_t*>(fakeRender + 0x288) = r() % 10;
    };
    auto bits = [](auto v) {
        uint64_t out = 0;
        std::memcpy(&out, &v, sizeof(v) > 8 ? 8 : sizeof(v));
        return out;
    };
    int mismatches = 0;
    auto diffV = [&](const char* what, uint32_t s, auto original, auto native) {
        seed(s);
        original();
        snap(a);
        seed(s);
        native();
        snap(b);
        if (a != b) {
            if (mismatches < 5) std::printf("shadowlands: %s differs\n", what);
            ++mismatches;
        }
    };
    auto diffR = [&](const char* what, uint32_t s, auto original, auto native) {
        seed(s);
        const uint64_t ra = bits(original());
        snap(a);
        seed(s);
        const uint64_t rb = bits(native());
        snap(b);
        if (a != b || ra != rb) {
            if (mismatches < 5) std::printf("shadowlands: %s differs (%llx/%llx)\n", what, (unsigned long long)ra,
                                            (unsigned long long)rb);
            ++mismatches;
        }
    };

    using SetParamsFn = void(__cdecl*)(const float*, const float*);
    using SetDirectionFn = void(__cdecl*)(const float*);
    using SetTextureFn = void(__cdecl*)(void*);
    using EnableFn = void(__cdecl*)(bool);
    using SetIntensityFn = void(__cdecl*)(float);
    using GetFloatFn = float(__cdecl*)();
    using GetTextureFn = void*(__cdecl*)();
    using GetMatrixFn = const float*(__cdecl*)();
    using GetBoolFn = bool(__cdecl*)();
    const uint32_t paramsRva[] = {0x46EAE, 0x46F3D, 0x46FCC}, directionRva[] = {0x4704C, 0x47038, 0x47024};
    const uint32_t textureRva[] = {0x46E84, 0x46F13, 0x46FA2}, enableRva[] = {0x46F06, 0x46F95, 0x47060};
    const uint32_t intensityRva[] = {0x46ED4, 0x46F63, 0x46FF2}, matrixRva[] = {0x4706D, 0x470F5, 0x4717D};
    const uint32_t getIntensityRva[] = {0x47238, 0x4724B, 0x4725E}, getTextureRva[] = {0x4723F, 0x47252, 0x47265};
    const uint32_t usedRva[] = {0x47271, 0x472A2, 0x472D3};
    SetParamsFn nParams[] = {shadowlands::SetGroundLightParameters, shadowlands::SetStatelLightParameters,
                             shadowlands::SetCATLightParameters};
    SetDirectionFn nDirections[] = {shadowlands::SetGroundLightDirection, shadowlands::SetStatelLightDirection,
                                    shadowlands::SetCATLightDirection};
    SetTextureFn nTextures[] = {shadowlands::SetGroundLightTexture, shadowlands::SetStatelLightTexture,
                                shadowlands::SetCATLightTexture};
    EnableFn nEnables[] = {shadowlands::EnableGroundLight, shadowlands::EnableStatelLight, shadowlands::EnableCATLight};
    SetIntensityFn nIntensities[] = {shadowlands::SetGroundLightIntensity, shadowlands::SetStatelLightIntensity,
                                     shadowlands::SetCATLightIntensity};
    GetFloatFn nGetIntensities[] = {shadowlands::GetGroundLightIntensity, shadowlands::GetStatelLightIntensity,
                                    shadowlands::GetCATLightIntensity};
    GetTextureFn nGetTextures[] = {shadowlands::GetGroundLightTexture, shadowlands::GetStatelLightTexture,
                                   shadowlands::GetCATLightTexture};
    GetMatrixFn nMatrices[] = {shadowlands::GetGroundLightMatrix, shadowlands::GetStatelLightMatrix,
                               shadowlands::GetCATLightMatrix};
    GetBoolFn nUseds[] = {shadowlands::IsGroundLightUsed, shadowlands::IsStatelLightUsed, shadowlands::IsCATLightUsed};
    const char* names[] = {"ground", "statel", "CAT"};

    for (int trial = 0; trial < 200; ++trial) {
        Args args;
        for (float& f : args.matrix) f = float(rng()) / float(rng() | 1);
        for (float& f : args.scale) f = float(int(rng() % 21) - 10);
        for (float& f : args.offset) f = float(int(rng() % 21) - 10);
        for (float& f : args.direction) f = float(int(rng() % 21) - 10);
        const uint32_t special[] = {0u, 0x3F800000u, 0xBF800000u, 0x40000000u, 0x40490FDBu, 0x7FC00000u, 0x80000000u};
        args.intensity = reinterpret_cast<const float&>(special[rng() % (sizeof(special) / sizeof(special[0]))]);
        const uint32_t s = uint32_t(trial) * 131 + 7;
        char what[64];

        diffV("SetCameraMatrix", s, [&] { Orig<void(__cdecl*)(const float*)>(0x46E5D)(args.matrix); },
              [&] { shadowlands::SetCameraMatrix(args.matrix); });
        for (int i = 0; i < 3; ++i) {
            std::snprintf(what, sizeof(what), "%s parameters", names[i]);
            diffV(what, s, [&] { Orig<SetParamsFn>(paramsRva[i])(args.scale, args.offset); },
                  [&] { nParams[i](args.scale, args.offset); });
            std::snprintf(what, sizeof(what), "%s direction", names[i]);
            diffV(what, s, [&] { Orig<SetDirectionFn>(directionRva[i])(args.direction); },
                  [&] { nDirections[i](args.direction); });
            std::snprintf(what, sizeof(what), "%s intensity", names[i]);
            diffV(what, s, [&] { Orig<SetIntensityFn>(intensityRva[i])(args.intensity); },
                  [&] { nIntensities[i](args.intensity); });
            std::snprintf(what, sizeof(what), "%s enable", names[i]);
            diffV(what, s, [&] { Orig<EnableFn>(enableRva[i])(args.intensity > 0.0f); },
                  [&] { nEnables[i](args.intensity > 0.0f); });
            std::snprintf(what, sizeof(what), "%s texture", names[i]);
            diffV(what, s, [&] { Orig<SetTextureFn>(textureRva[i])(static_cast<void*>(fakeTexture)); },
                  [&] { nTextures[i](static_cast<void*>(fakeTexture)); });
            std::snprintf(what, sizeof(what), "%s get intensity", names[i]);
            diffR(what, s, [&] { return Orig<GetFloatFn>(getIntensityRva[i])(); }, [&] { return nGetIntensities[i](); });
            std::snprintf(what, sizeof(what), "%s get texture", names[i]);
            diffR(what, s, [&] { return Orig<GetTextureFn>(getTextureRva[i])(); }, [&] { return nGetTextures[i](); });
            std::snprintf(what, sizeof(what), "%s matrix", names[i]);
            diffR(what, s, [&] { return Orig<GetMatrixFn>(matrixRva[i])(); }, [&] { return nMatrices[i](); });
            std::snprintf(what, sizeof(what), "%s used", names[i]);
            diffR(what, s, [&] { return Orig<GetBoolFn>(usedRva[i])(); }, [&] { return nUseds[i](); });
        }
        diffV("FreeAllTextures", s, [&] { Orig<void(__cdecl*)()>(0x47205)(); }, [&] { shadowlands::FreeAllTextures(); });
        diffR("PriCheck", s, [&] { return Orig<GetBoolFn>(0x4721E)(); }, [&] { return shadowlands::PriCheck(); });
    }
    std::printf("shadowlands: %d trials, %d differ\n", 200, mismatches);
    Check(mismatches == 0, "shadowlands: state and matrices");
}

// Color_t against ours: every constructor, Init, assignment, Interpolate, + and * on random (and boundary) input,
// the four bytes compared.
void CheckColor()
{
    using namespace rnative::color;
    std::mt19937 rng(2024);
    std::uniform_real_distribution<float> uf(-0.3f, 1.3f);
    const float special[] = {0.0f, 1.0f, 0.5f, -0.0f, 1.0f / 255.0f, 254.0f / 255.0f, -1.0f, 2.0f, 1000.0f};
    auto randf = [&]() { return (rng() % 4 == 0) ? special[rng() % (sizeof(special) / sizeof(special[0]))] : uf(rng); };
    int mism = 0;
    auto bad = [&](const char* what, const uint8_t* x, const uint8_t* y) {
        if (std::memcmp(x, y, 4) && mism++ < 6)
            std::printf("color: %s %08x / %08x\n", what, *reinterpret_cast<const uint32_t*>(x),
                        *reinterpret_cast<const uint32_t*>(y));
    };
    using CtorUintFn = void*(__fastcall*)(void*, void*, uint32_t);
    using InitFFn = void(__fastcall*)(void*, void*, float, float, float, float);
    using InitRgbFn = void(__fastcall*)(void*, void*, const float*, float);
    using AssignFn = void*(__fastcall*)(void*, void*, uint32_t);
    using InterpFn = void(__fastcall*)(void*, void*, const void*, const void*, float);
    using RgbFn = void*(__fastcall*)(void*, void*, const float*);
    using AddFn = void*(__fastcall*)(void*, void*, void*, const void*);
    using ScaleFn = void*(__fastcall*)(void*, void*, void*, float);
    uint8_t a[4], b[4], ca[4], cb[4], other[4];
    for (int trial = 0; trial < 5000; ++trial) {
        const float r = randf(), g = randf(), bl = randf(), al = randf();
        float rgb[3] = {r, g, bl};
        for (int k = 0; k < 4; ++k) ca[k] = uint8_t(rng()), other[k] = uint8_t(rng());
        const uint32_t v = rng();
        std::memset(a, 0xAA, 4), std::memset(b, 0xAA, 4);
        Orig<CtorUintFn>(0x1998F)(a, nullptr, v);
        CtorUint(reinterpret_cast<Color*>(b), nullptr, v);
        bad("ctor(uint)", a, b);
        std::memset(a, 0xAA, 4), std::memset(b, 0xAA, 4);
        Orig<InitFFn>(0x199BB)(a, nullptr, r, g, bl, al);
        InitF(reinterpret_cast<Color*>(b), nullptr, r, g, bl, al);
        bad("init4", a, b);
        std::memset(a, 0xAA, 4), std::memset(b, 0xAA, 4);
        Orig<InitRgbFn>(0x199FF)(a, nullptr, rgb, al);
        InitRgb(reinterpret_cast<Color*>(b), nullptr, rgb, al);
        bad("initRGB", a, b);
        std::memset(a, 0xAA, 4), std::memset(b, 0xAA, 4);
        Orig<AssignFn>(0x19A47)(a, nullptr, v);
        AssignUint(reinterpret_cast<Color*>(b), nullptr, v);
        bad("assign", a, b);
        std::memset(a, 0xAA, 4), std::memset(b, 0xAA, 4);
        Orig<InterpFn>(0x19A7A)(a, nullptr, ca, other, al);
        Interpolate(reinterpret_cast<Color*>(b), nullptr, reinterpret_cast<const Color*>(ca),
                    reinterpret_cast<const Color*>(other), al);
        bad("interp", a, b);
        std::memset(a, 0xAA, 4), std::memset(b, 0xAA, 4);
        Orig<RgbFn>(0x19B80)(a, nullptr, rgb);
        CtorRgb(reinterpret_cast<Color*>(b), nullptr, rgb);
        bad("ctor(RGB)", a, b);
        Orig<AddFn>(0x19BB1)(ca, nullptr, a, other);
        std::memset(cb, 0xAA, 4);
        Add(reinterpret_cast<Color*>(ca), nullptr, reinterpret_cast<Color*>(cb), reinterpret_cast<const Color*>(other));
        bad("add", a, cb);
        Orig<ScaleFn>(0x19C93)(ca, nullptr, a, al);
        std::memset(cb, 0xAA, 4);
        Scale(reinterpret_cast<Color*>(ca), nullptr, reinterpret_cast<Color*>(cb), al);
        bad("scale", a, cb);
    }
    std::printf("color: %d trials, %d differ\n", 5000, mism);
    Check(mism == 0, "color: Color_t");
}

// PixelFormat_t against ours: the mask helper on random masks, then real pixel formats (built from a DDPIXELFORMAT)
// with the packing / unpacking conversions on random pixels and colours.
void CheckPixelFormat()
{
    using namespace rnative::pixfmt;
    std::mt19937 rng(4242);
    using MaskFn = void(__stdcall*)(uint32_t, int32_t*, int32_t*);
    using CtorFn = void*(__fastcall*)(void*, void*, const void*);
    using PackFn = uint32_t(__fastcall*)(void*, void*, const float*);
    using ToBytesFn = void(__fastcall*)(void*, void*, void*, const void*);
    using PackBytesFn = uint32_t(__fastcall*)(void*, void*, const void*);
    using PackPackedFn = uint32_t(__fastcall*)(void*, void*, uint32_t);
    using ToFloatsFn = void(__fastcall*)(void*, void*, void*, const void*, const void*);
    int mism = 0;
    auto bad = [&](const char* what, const void* x, const void* y, size_t n) {
        if (std::memcmp(x, y, n) && mism++ < 6) std::printf("pixfmt: %s differs\n", what);
    };
    for (int i = 0; i < 20000; ++i) {
        const uint32_t mask = rng();
        int32_t sa = 0, sb = 0, ba = 0, bb = 0;
        Orig<MaskFn>(0x1F694)(mask, &sa, &ba);
        MaskToShiftBits(mask, &sb, &bb);
        if ((sa != sb || ba != bb) && mism++ < 6)
            std::printf("pixfmt: mask %08x %d,%d / %d,%d\n", mask, sa, ba, sb, bb);
    }
    // bit count, R, G, B, A masks (the formats the game's surfaces and textures use).
    const uint32_t formats[][5] = {
        {16, 0xF800, 0x07E0, 0x001F, 0},                       // R5G6B5
        {16, 0x7C00, 0x03E0, 0x001F, 0x8000},                  // A1R5G5B5
        {16, 0x0F00, 0x00F0, 0x000F, 0xF000},                  // A4R4G4B4
        {24, 0xFF0000, 0x00FF00, 0x0000FF, 0},                 // R8G8B8
        {32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0},           // X8R8G8B8
        {32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000},  // A8R8G8B8
        {8, 0xE0, 0x1C, 0x03, 0},                              // 3-3-2 palette
    };
    uint32_t ddpf[8] = {};
    for (int trial = 0; trial < 4000; ++trial) {
        const uint32_t* f = formats[rng() % (sizeof(formats) / sizeof(formats[0]))];
        ddpf[3] = f[0], ddpf[4] = f[1], ddpf[5] = f[2], ddpf[6] = f[3], ddpf[7] = f[4];
        Format fa, fb;
        std::memset(&fa, 0xAA, sizeof(fa));
        std::memset(&fb, 0xAA, sizeof(fb));
        Orig<CtorFn>(0x1FD01)(&fa, nullptr, ddpf);
        Ctor(&fb, nullptr, ddpf);
        bad("format", &fa, &fb, sizeof(fa));
        float rgb[4];
        for (float& v : rgb) v = float(double(rng() % 2000) / 1000.0 - 0.5);
        uint8_t bytes[3] = {uint8_t(rng()), uint8_t(rng()), uint8_t(rng())};
        uint8_t raw[4] = {uint8_t(rng()), uint8_t(rng()), uint8_t(rng()), uint8_t(rng())};
        const uint32_t pixel = rng();
        const uint32_t pa = Orig<PackFn>(0x1F79F)(&fa, nullptr, rgb), pb = Pack(&fb, nullptr, rgb);
        if (pa != pb && mism++ < 6) std::printf("pixfmt: Pack %08x/%08x\n", pa, pb);
        const uint32_t qa = Orig<PackBytesFn>(0x1FA9D)(&fa, nullptr, bytes), qb = PackBytes(&fb, nullptr, bytes);
        if (qa != qb && mism++ < 6) std::printf("pixfmt: PackBytes %08x/%08x\n", qa, qb);
        const uint32_t ra = Orig<PackPackedFn>(0x1FAE3)(&fa, nullptr, pixel), rb = PackPacked(&fb, nullptr, pixel);
        if (ra != rb && mism++ < 6) std::printf("pixfmt: PackPacked %08x/%08x\n", ra, rb);
        uint8_t oa[4] = {0, 0, 0, 0}, ob[4] = {0, 0, 0, 0};
        Orig<ToBytesFn>(0x1FA50)(&fa, nullptr, oa, rgb);
        ToBytes(&fb, nullptr, ob, rgb);
        bad("ToBytes", oa, ob, sizeof(oa));
        float xa[4] = {0, 0, 0, 0}, xb[4] = {0, 0, 0, 0};
        Orig<ToFloatsFn>(0x1F90D)(&fa, nullptr, xa, raw, nullptr);
        ToFloats(&fb, nullptr, xb, raw, nullptr);
        bad("ToFloats", xa, xb, sizeof(xa));
    }
    std::printf("pixfmt: 20000 masks, 4000 formats, %d differ\n", mism);
    Check(mism == 0, "pixfmt: PixelFormat_t");
}

// A fake registered bitmap for the registry: its vtable's slot 3 names it.
const char* __fastcall FakeBitmapName(void* self, void*) { return static_cast<const char*>(self) + 4; }

// LBitmap_t against ours: the registry, init / size / destroy / make-24-bit / scale / base ctor / the Vector3 compare,
// on random bitmaps. The stream (BMP) loader is still the original's and is not tested here.
void CheckLBitmap()
{
    using namespace rnative::lbitmap;
    lbitmap::SetModule(g_orig);
    using InitFn = void(__fastcall*)(void*, void*, int32_t);
    using DestroyFn = void(__fastcall*)(void*, void*);
    using SizeFn = int32_t(__fastcall*)(const void*, void*);
    using Make24Fn = void(__fastcall*)(void*, void*);
    using RegisterFn = void(__cdecl*)(void*);
    using FindFn = void*(__cdecl*)(const char*);
    using ScaleFn = void(__fastcall*)(void*, void*, uint32_t, uint32_t);
    using BaseCtorFn = void(__fastcall*)(void*, void*);
    using NotEqualFn = uint32_t(__fastcall*)(const float*, void*, const float*);
    int mism = 0;
    auto bad = [&](const char* what, const void* x, const void* y, size_t n) {
        if (std::memcmp(x, y, n) && mism++ < 6) std::printf("lbitmap: %s differs\n", what);
    };
    std::mt19937 rng(99);

    void* fakeVt[4] = {nullptr, nullptr, nullptr, reinterpret_cast<void*>(&FakeBitmapName)};
    alignas(8) uint8_t obj1[16] = {}, obj2[16] = {};
    *reinterpret_cast<void**>(obj1) = fakeVt;
    std::memcpy(obj1 + 4, "bmp", 4);
    *reinterpret_cast<void**>(obj2) = fakeVt;
    std::memcpy(obj2 + 4, "tga", 4);
    auto registry = [g = g_orig]() { return reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(g) + 0xB92E0); };
    auto list = [g = g_orig](uint32_t i) { return reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(g) + 0xB9150)[i]; };
    registry()[0] = 0;
    Orig<RegisterFn>(0x14EBC)(obj1);
    const uint32_t countOriginal = registry()[0];
    const void* slot0Original = list(0);
    registry()[0] = 0;
    Register(obj2);
    const uint32_t countNative = registry()[0];
    const void* slot0Native = list(0);
    if (countOriginal != countNative || slot0Original != reinterpret_cast<void*>(obj1) || slot0Native != reinterpret_cast<void*>(obj2))
        ++mism;
    registry()[0] = 0;
    Register(obj1);
    Register(obj2);
    void* foundA = Orig<FindFn>(0x14ED7)("bmp");
    void* foundB = Find("bmp");
    if (foundA != foundB) ++mism;
    if (Orig<FindFn>(0x14ED7)("tga") != Find("tga")) ++mism;
    if (Orig<FindFn>(0x14ED7)("png") != Find("png")) ++mism;

    alignas(16) uint8_t memA[0x124], memB[0x124];
    for (int trial = 0; trial < 200; ++trial) {
        // init
        std::memset(memA, 0xAA, sizeof(memA));
        std::memset(memB, 0xAA, sizeof(memB));
        Orig<InitFn>(0x14C9B)(memA, nullptr, int32_t(rng()));
        Init(reinterpret_cast<Bitmap*>(memB), nullptr, int32_t(rng()) ^ int32_t(rng()));   // the arg is ignored
        bad("init", memA, memB, sizeof(memA));
        std::memset(memB, 0xAA, sizeof(memB));
        Init(reinterpret_cast<Bitmap*>(memB), nullptr, 0);
        // size
        Bitmap* a = reinterpret_cast<Bitmap*>(memA);
        Bitmap* b = reinterpret_cast<Bitmap*>(memB);
        a->bitsPerPixel = int32_t(rng() % 40);
        b->bitsPerPixel = a->bitsPerPixel;
        a->width = b->width = rng() % 64;
        a->height = b->height = rng() % 64;
        if (Orig<SizeFn>(0x14CFF)(a, nullptr) != Size(b, nullptr)) ++mism;
        // not equal
        float v1[3], v2[3];
        for (int i = 0; i < 3; ++i) v1[i] = float(int(rng() % 5) - 2), v2[i] = (rng() % 4 == 0) ? v1[i] : float(int(rng() % 5) - 2);
        if (Orig<NotEqualFn>(0x155C5)(v1, nullptr, v2) != NotEqual(v1, nullptr, v2)) ++mism;
    }

    // make 24-bit (a paletted 8-bit bitmap): random indices and palette, compared after the call.
    for (int trial = 0; trial < 200; ++trial) {
        const uint32_t w = 1 + rng() % 16, h = 1 + rng() % 16;
        std::vector<uint8_t> indices(w * h);
        for (uint8_t& v : indices) v = uint8_t(rng());
        std::vector<uint32_t> pal(256);
        for (uint32_t& v : pal) v = rng();
        auto setup = [&](uint8_t* mem) {
            Bitmap* b = reinterpret_cast<Bitmap*>(mem);
            std::memset(mem, 0, 0x124);
            b->bitsPerPixel = 8;
            b->width = w, b->height = h;
            b->data = static_cast<uint8_t*>(vc10::Allocate(w * h));
            std::memcpy(b->data, indices.data(), w * h);
            b->palette = static_cast<uint32_t*>(vc10::Allocate(256 * 4));
            std::memcpy(b->palette, pal.data(), 256 * 4);
        };
        setup(memA);
        setup(memB);
        Orig<Make24Fn>(0x14DA8)(memA, nullptr);
        Make24Bit(reinterpret_cast<Bitmap*>(memB), nullptr);
        Bitmap* a = reinterpret_cast<Bitmap*>(memA);
        Bitmap* b = reinterpret_cast<Bitmap*>(memB);
        if (a->bitsPerPixel != b->bitsPerPixel || std::memcmp(a->data, b->data, w * h * 3) != 0) {
            if (mism++ < 6) std::printf("lbitmap: make24 differs\n");
        }
        vc10::Free(a->data);
        vc10::Free(a->palette);
        vc10::Free(b->data);
        vc10::Free(b->palette);
        if (a->data) { }   // (the originals' pointers were freed; the objects are re-used next trial)
    }

    // scale: a fake Randy_t with the texture size limits.
    alignas(8) uint8_t randy[0x220] = {};
    *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(g_orig) + 0x17D2EC) = randy;
    *reinterpret_cast<uint32_t*>(randy + 0x204) = 1;
    *reinterpret_cast<uint32_t*>(randy + 0x208) = 1;
    *reinterpret_cast<uint32_t*>(randy + 0x20C) = 1u << 20;
    *reinterpret_cast<uint32_t*>(randy + 0x210) = 1u << 20;
    for (int trial = 0; trial < 500; ++trial) {
        const uint32_t w = 1 + rng() % 32, h = 1 + rng() % 32;
        const int32_t bpp = (rng() & 1) ? 0x18 : 0x20;
        const uint32_t nw = 1 + rng() % (w + 2), nh = 1 + rng() % (h + 2);
        *reinterpret_cast<uint32_t*>(randy + 0x218) = (rng() % 3 == 0) ? uint32_t(2 + rng() % 8) : 0;
        const uint32_t bytes = uint32_t(bpp == 0x18 ? 3 : 4);
        std::vector<uint8_t> pixels(size_t(w) * h * bytes);
        for (uint8_t& v : pixels) v = uint8_t(rng());
        auto setup = [&](uint8_t* mem) {
            Bitmap* b = reinterpret_cast<Bitmap*>(mem);
            std::memset(mem, 0, 0x124);
            b->bitsPerPixel = bpp;
            b->width = w, b->height = h;
            b->data = static_cast<uint8_t*>(vc10::Allocate(pixels.size()));
            std::memcpy(b->data, pixels.data(), pixels.size());
        };
        setup(memA);
        setup(memB);
        Orig<ScaleFn>(0x14F53)(memA, nullptr, nw, nh);
        Scale(reinterpret_cast<Bitmap*>(memB), nullptr, nw, nh);
        Bitmap* a = reinterpret_cast<Bitmap*>(memA);
        Bitmap* b = reinterpret_cast<Bitmap*>(memB);
        bool same = a->width == b->width && a->height == b->height && a->bitsPerPixel == b->bitsPerPixel;
        if (same) {
            const uint32_t bytes = uint32_t(a->bitsPerPixel == 0x18 ? 3 : (a->bitsPerPixel == 0x20 ? 4 : 0));
            if (bytes) same = std::memcmp(a->data, b->data, size_t(a->width) * a->height * bytes) == 0;
        }
        if (!same && mism++ < 6) std::printf("lbitmap: scale %ux%u -> %ux%u (aspect %u) differs\n", w, h, nw, nh,
                                             *reinterpret_cast<uint32_t*>(randy + 0x218));
        vc10::Free(a->data);
        vc10::Free(b->data);
    }

    // base ctor registers: the struct and the registry count compared.
    registry()[0] = 0;
    std::memset(memA, 0xAA, sizeof(memA));
    Orig<BaseCtorFn>(0x15228)(memA, nullptr);
    const uint32_t countA = registry()[0];
    registry()[0] = 0;
    std::memset(memB, 0xAA, sizeof(memB));
    BaseCtor(reinterpret_cast<Bitmap*>(memB), nullptr);
    const uint32_t countB = registry()[0];
    bad("base ctor", memA, memB, sizeof(memA));
    if (countA != countB) ++mism;

    std::printf("lbitmap: registry, init, size, make24, scale, base ctor, vector compare; %d differ\n", mism);
    Check(mism == 0, "lbitmap: LBitmap_t");
}

// ---- a fake fun::PositionIO_t over a memory buffer (vtable slots 0 Read, 5 ReadDword, 7 ReadWord, 18 Seek) ----
struct FakeStream {
    void** vtable;
    const uint8_t* data;
    size_t size, pos;
};
uint16_t __fastcall FakeReadWord(void* self, void*)
{
    FakeStream* s = static_cast<FakeStream*>(self);
    uint16_t v = 0;
    if (s->pos + 2 <= s->size) std::memcpy(&v, s->data + s->pos, 2);
    s->pos += 2;
    return v;
}
uint32_t __fastcall FakeReadDword(void* self, void*)
{
    FakeStream* s = static_cast<FakeStream*>(self);
    uint32_t v = 0;
    if (s->pos + 4 <= s->size) std::memcpy(&v, s->data + s->pos, 4);
    s->pos += 4;
    return v;
}
void __fastcall FakeRead(void* self, void*, void* buf, int32_t size)
{
    FakeStream* s = static_cast<FakeStream*>(self);
    if (s->pos + size_t(size) <= s->size && size > 0) std::memcpy(buf, s->data + s->pos, size);
    s->pos += size_t(size);
}
void __fastcall FakeSeek(void* self, void*, int32_t offset, int32_t origin)
{
    FakeStream* s = static_cast<FakeStream*>(self);
    s->pos = origin == 0 ? size_t(offset) : origin == 1 ? s->pos + size_t(offset) : s->size + size_t(offset);
}
void* __fastcall FakeCreate(void*, void*, void*, int32_t) { return reinterpret_cast<void*>(0x1234); }

// LBitmap_t's BMP stream loader against the original, over a fake fun::PositionIO_t.
void CheckLBitmapStream()
{
    using namespace rnative::lbitmap;
    lbitmap::SetModule(g_orig);
    using BMPCtorFn = void*(__fastcall*)(void*, void*, void*, int32_t);
    using CreateFn = void*(__fastcall*)(void*, void*, void*, int32_t);
    using LoadFn = void*(__cdecl*)(void*, const char*, int32_t);
    int mism = 0;
    auto eql = [&](const char* what, uint64_t x, uint64_t y) {
        if (x != y && mism++ < 6) std::printf("lbitmap stream: %s differs\n", what);
    };
    void* streamVt[19] = {};
    streamVt[0] = reinterpret_cast<void*>(&FakeRead);
    streamVt[5] = reinterpret_cast<void*>(&FakeReadDword);
    streamVt[7] = reinterpret_cast<void*>(&FakeReadWord);
    streamVt[18] = reinterpret_cast<void*>(&FakeSeek);
    std::mt19937 rng(2718);
    alignas(16) uint8_t memA[0x124], memB[0x124];
    for (int trial = 0; trial < 300; ++trial) {
        const uint16_t bpp = (rng() & 1) ? 8 : 24;
        const uint32_t w = 1 + rng() % 20, h = 1 + rng() % 20;
        const bool badSignature = rng() % 20 == 0;
        const uint32_t compression = (rng() % 20 == 0) ? 1 : 0;
        const int32_t flags = (rng() % 10 == 0) ? 1 : 0;
        std::vector<uint8_t> bmp;
        auto put16 = [&](uint16_t v) { bmp.push_back(uint8_t(v)), bmp.push_back(uint8_t(v >> 8)); };
        auto put32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) bmp.push_back(uint8_t(v >> (8 * i))); };
        const uint32_t paletteBytes = bpp == 8 ? 256 * 4 : 0;
        const uint32_t rowBytes = bpp == 8 ? w : w * 3;
        const uint32_t pad = (4 - (rowBytes & 3)) & 3;
        const uint32_t pixelBytes = h * (rowBytes + pad);
        const uint32_t dataOffset = 54 + paletteBytes;
        put16(badSignature ? 0x1234 : 0x4d42);
        put32(dataOffset + pixelBytes);
        put16(0), put16(0), put32(dataOffset);
        put32(40), put32(w), put32(h), put16(1), put16(bpp), put32(compression), put32(pixelBytes), put32(2835),
            put32(2835), put32(bpp == 8 ? 256 : 0), put32(0);
        while (bmp.size() < dataOffset + pixelBytes) bmp.push_back(uint8_t(rng()));
        FakeStream sa{streamVt, bmp.data(), bmp.size(), 0}, sb{streamVt, bmp.data(), bmp.size(), 0};
        std::memset(memA, 0xAA, sizeof(memA));
        std::memset(memB, 0xAA, sizeof(memB));
        Orig<BMPCtorFn>(0x1526B)(memA, nullptr, &sa, flags);
        BMPCtor(reinterpret_cast<Bitmap*>(memB), nullptr, reinterpret_cast<Stream*>(&sb), flags);
        Bitmap* a = reinterpret_cast<Bitmap*>(memA);
        Bitmap* b = reinterpret_cast<Bitmap*>(memB);
        eql("state", uint32_t(a->state), uint32_t(b->state));
        eql("bpp", uint32_t(a->bitsPerPixel), uint32_t(b->bitsPerPixel));
        eql("width", a->width, b->width);
        eql("height", a->height, b->height);
        eql("flag", a->flag, b->flag);
        eql("stream pos", sa.pos, sb.pos);
        if (a->data && b->data) {
            const size_t n = size_t(w) * h * (bpp == 8 ? 1 : 3);
            if (std::memcmp(a->data, b->data, n) != 0 && mism++ < 6) std::printf("lbitmap stream: data differs\n");
        } else {
            eql("data null", a->data == nullptr, b->data == nullptr);
        }
        if (bpp == 8 && a->palette && b->palette && std::memcmp(a->palette, b->palette, 256 * 4) != 0 && mism++ < 6)
            std::printf("lbitmap stream: palette differs\n");
        vc10::Free(a->data);
        vc10::Free(a->palette);
        vc10::Free(b->data);
        vc10::Free(b->palette);
    }
    // the factory: a fresh object from the stream, compared
    {
        std::vector<uint8_t> bmp;
        auto put16 = [&](uint16_t v) { bmp.push_back(uint8_t(v)), bmp.push_back(uint8_t(v >> 8)); };
        auto put32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) bmp.push_back(uint8_t(v >> (8 * i))); };
        const uint32_t w = 8, h = 8, rowBytes = w, pad = (4 - (rowBytes & 3)) & 3, pixelBytes = h * (rowBytes + pad);
        const uint32_t dataOffset = 54 + 256 * 4;
        put16(0x4d42), put32(dataOffset + pixelBytes), put16(0), put16(0), put32(dataOffset);
        put32(40), put32(w), put32(h), put16(1), put16(8), put32(0), put32(pixelBytes), put32(2835), put32(2835),
            put32(256), put32(0);
        while (bmp.size() < dataOffset + pixelBytes) bmp.push_back(uint8_t(rng()));
        FakeStream sa{streamVt, bmp.data(), bmp.size(), 0}, sb{streamVt, bmp.data(), bmp.size(), 0};
        Bitmap* a = reinterpret_cast<Bitmap*>(Orig<CreateFn>(0x1553D)(nullptr, nullptr, &sa, 0));
        Bitmap* b = Create(nullptr, nullptr, reinterpret_cast<Stream*>(&sb), 0);
        eql("create null", a == nullptr, b == nullptr);
        if (a && b) {
            eql("create bpp", uint32_t(a->bitsPerPixel), uint32_t(b->bitsPerPixel));
            eql("create data", std::memcmp(a->data, b->data, w * h) == 0, 1);
            vc10::Free(a->data);
            vc10::Free(a->palette);
            vc10::Free(b->data);
            vc10::Free(b->palette);
            vc10::Free(a);
            vc10::Free(b);
        }
    }
    // Load: a registered fake loader, found by extension and asked to create
    {
        void* loaderVt[5] = {nullptr, nullptr, nullptr, reinterpret_cast<void*>(&FakeBitmapName),
                             reinterpret_cast<void*>(&FakeCreate)};
        alignas(8) uint8_t loader[16] = {};
        *reinterpret_cast<void**>(loader) = loaderVt;
        std::memcpy(loader + 4, "bmp", 4);
        auto registry = [g = g_orig]() { return reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(g) + 0xB92E0); };
        auto list = [g = g_orig](uint32_t i) -> void*& { return reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(g) + 0xB9150)[i]; };
        registry()[0] = 1;
        list(0) = loader;
        void* fakeStream = streamVt;
        eql("load bmp", reinterpret_cast<uint64_t>(Orig<LoadFn>(0x151E5)(fakeStream, "foo.bmp", 0)),
            reinterpret_cast<uint64_t>(Load(static_cast<Stream*>(fakeStream), "foo.bmp", 0)));
        eql("load BMP", reinterpret_cast<uint64_t>(Orig<LoadFn>(0x151E5)(fakeStream, "x/y.BMP", 0)),
            reinterpret_cast<uint64_t>(Load(static_cast<Stream*>(fakeStream), "x/y.BMP", 0)));
        eql("load null name", reinterpret_cast<uint64_t>(Orig<LoadFn>(0x151E5)(fakeStream, nullptr, 0)),
            reinterpret_cast<uint64_t>(Load(static_cast<Stream*>(fakeStream), nullptr, 0)));
        eql("load unknown", reinterpret_cast<uint64_t>(Orig<LoadFn>(0x151E5)(fakeStream, "foo.tga", 0)),
            reinterpret_cast<uint64_t>(Load(static_cast<Stream*>(fakeStream), "foo.tga", 0)));
        registry()[0] = 0;
    }
    std::printf("lbitmap stream: 300 BMP loads, the factory and Load; %d differ\n", mism);
    Check(mism == 0, "lbitmap stream: BMP loader");
}

// The small shared helpers against the original.
void CheckHelpers()
{
    using namespace rnative::helpers;
    helpers::SetModule(g_orig);
    using CrossFn = void(__fastcall*)(void*, void*, const void*);
    using ScaleFn = void(__fastcall*)(void*, void*, void*, float);
    using CrossToFn = void(__fastcall*)(void*, void*, void*, const void*);
    using IdentityFn = void(__fastcall*)(void*, void*);
    using NextFn = void(__cdecl*)(void**, const void*);
    using TypeStoreFn = void(__stdcall*)(void*, int32_t);
    using TypeSizeFn = int32_t(__cdecl*)();
    std::mt19937 rng(31415);
    std::uniform_real_distribution<float> uf(-10.0f, 10.0f);
    int mism = 0;
    for (int trial = 0; trial < 20000; ++trial) {
        float a[3], b[3], x[3], y[3], oa[3] = {}, ob[3] = {}, oc[3] = {};
        for (int i = 0; i < 3; ++i) a[i] = uf(rng), b[i] = uf(rng);
        const float scale = uf(rng);
        std::memcpy(x, a, sizeof(a));
        std::memcpy(y, a, sizeof(a));
        Orig<CrossFn>(0x2A358)(x, nullptr, b);
        CrossProduct(y, nullptr, b);
        if (std::memcmp(x, y, sizeof(x)) != 0 && mism++ < 6) std::printf("helpers: cross differs\n");
        Orig<ScaleFn>(0x2A39F)(a, nullptr, oa, scale);
        ScaleVector(a, nullptr, ob, scale);
        if (std::memcmp(oa, ob, sizeof(oa)) != 0 && mism++ < 6) std::printf("helpers: scale differs\n");
        Orig<CrossToFn>(0x2A3DB)(a, nullptr, oa, b);
        CrossProductTo(a, nullptr, ob, b);
        if (std::memcmp(oa, ob, sizeof(oa)) != 0 && mism++ < 6) std::printf("helpers: crossTo differs\n");
        float m1[16], m2[16];
        std::memset(m1, 0xAA, sizeof(m1));
        std::memset(m2, 0xAA, sizeof(m2));
        Orig<IdentityFn>(0x2A406)(m1, nullptr);
        Identity(m2, nullptr);
        if (std::memcmp(m1, m2, sizeof(m1)) != 0 && mism++ < 6) std::printf("helpers: identity differs\n");
        (void)oc;
        // normalize and scale (0x18833 / 0x18864), the matrix origin add (0x2cdb2), the matrix move (0x46518) and
        // the Vector3 subtract (0x12cf0).
        std::memcpy(x, a, sizeof(a));
        std::memcpy(y, a, sizeof(a));
        Orig<void(__fastcall*)(void*, void*, float)>(0x18833)(x, nullptr, scale);
        NormalizeScale(y, nullptr, scale);
        if (std::memcmp(x, y, sizeof(x)) != 0 && mism++ < 6) std::printf("helpers: normalizeScale differs\n");
        Orig<void(__fastcall*)(void*, void*, void*, float)>(0x18864)(a, nullptr, oa, scale);
        NormalizeScaleTo(a, nullptr, ob, scale);
        if (std::memcmp(oa, ob, sizeof(oa)) != 0 && mism++ < 6) std::printf("helpers: normalizeScaleTo differs\n");
        for (float& v : m1) v = uf(rng);
        std::memcpy(m2, m1, sizeof(m1));
        Orig<void(__fastcall*)(void*, void*, const void*)>(0x2CDB2)(m1, nullptr, b);
        TranslateAdd(m2, nullptr, b);
        if (std::memcmp(m1, m2, sizeof(m1)) != 0 && mism++ < 6) std::printf("helpers: translateAdd differs\n");
        std::memcpy(m2, m1, sizeof(m1));
        Orig<void(__fastcall*)(void*, void*, float, float)>(0x46518)(m1, nullptr, a[0], a[1]);
        MatrixMove(m2, nullptr, a[0], a[1]);
        if (std::memcmp(m1, m2, sizeof(m1)) != 0 && mism++ < 6) std::printf("helpers: matrixMove differs\n");
        Orig<void(__fastcall*)(void*, void*, void*, const void*)>(0x12CF0)(a, nullptr, oa, b);
        Subtract(a, nullptr, ob, b);
        if (std::memcmp(oa, ob, sizeof(oa)) != 0 && mism++ < 6) std::printf("helpers: subtract differs\n");
    }
    // the frame-hierarchy walk: a fixed forest (node i's parent is below it, its sibling above it; end is the root)
    uint8_t nodes[8][0x20] = {};
    auto set = [&](int i, int parent, int sibling, int child) {
        *reinterpret_cast<void**>(nodes[i] + 0x14) = parent < 0 ? nullptr : nodes[parent];
        *reinterpret_cast<void**>(nodes[i] + 0x18) = sibling < 0 ? nullptr : nodes[sibling];
        *reinterpret_cast<void**>(nodes[i] + 0x1c) = child < 0 ? nullptr : nodes[child];
    };
    set(0, -1, -1, 1);
    set(1, 0, 4, 2);
    set(2, 1, 3, -1);
    set(3, 1, -1, -1);
    set(4, 0, -1, -1);
    for (int start = 0; start < 5; ++start) {
        void* pa = nodes[start];
        void* pb = nodes[start];
        for (int step = 0; step < 20 && (pa || pb); ++step) {
            Orig<NextFn>(0x45289)(&pa, nodes[0]);
            NextNode(&pb, nodes[0]);
            if (pa != pb) {
                if (mism++ < 6) std::printf("helpers: NextNode from %d step %d differs\n", start, step);
                break;
            }
        }
    }
    alignas(8) uint8_t descriptorA[16] = {}, descriptorB[16] = {};
    const int32_t value = int32_t(rng());
    Orig<TypeStoreFn>(0x1B63B)(descriptorA, value);
    TypeStore(descriptorB, value);
    if (std::memcmp(descriptorA, descriptorB, sizeof(descriptorA)) != 0 && mism++ < 6)
        std::printf("helpers: TypeStore differs\n");
    if (Orig<TypeSizeFn>(0x1B662)() != TypeSize()) ++mism;
    std::printf("helpers: cross / scale / crossTo / identity / NextNode / Type*; %d differ\n", mism);
    Check(mism == 0, "helpers: shared helpers");
}

// The vsnprintf-into-a-VS2010-std::string helpers against the original.
void CheckFormat()
{
    texturestream::SetModule(g_orig);
    using FormatFn = void(__cdecl*)(void*, const char*, ...);
    int mism = 0;
    auto run = [&](const char* format, auto... args) {
        vc10::String a, b;
        a.init();
        b.init();
        Orig<FormatFn>(0x1994C)(&a, format, args...);
        texturestream::Format(&b, format, args...);
        if (a.size != b.size || std::strcmp(a.c_str(), b.c_str()) != 0) {
            if (mism++ < 5) std::printf("format: '%s' -> '%s' / '%s'\n", format, a.c_str(), b.c_str());
        }
        a.release();
        b.release();
    };
    run("plain");
    run("n=%d", 42);
    run("s=%s end", "hello");
    run("mix %d %s %c", 7, "xy", 'Z');
    run("%f", 3.5);
    run("%08x-%08x", 0xdeadbeefu, 0x12345678u);
    std::string long255(255, 'a'), long300(300, 'b'), huge(3000, 'c');
    run("255=%s", long255.c_str());
    run("300=%s", long300.c_str());
    run("3000=%s", huge.c_str());
    std::printf("format: %d differ\n", mism);
    Check(mism == 0, "format: std::string formatting");
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
    rnative::SetOriginal(orig);                      // AddressFor's fallback (the replacements resolve through it)
    rnative::orig::Init(orig);                       // the orig:: bindings the replacements call through
    if (argc > 2 && std::strcmp(argv[2], "--dump-dxerrors") == 0) {   // the original's HRESULT table (FUN_1001d619)
        DumpDxErrors();
        return 0;
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
    CheckBounds();
    CheckSampling();
    CheckHierarchy();
    CheckBlends();
    CheckMath();
    CheckAttractors();
    CheckDxErrors();
    CheckKeyframes();
    CheckShadowlands();
    CheckColor();
    CheckPixelFormat();
    CheckLBitmap();
    CheckLBitmapStream();
    CheckHelpers();
    CheckFormat();
    Time();
    std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "all passed", g_failures);
    return g_failures ? 1 : 0;
}
