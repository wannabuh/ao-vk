// Character skinning (the game's CAT characters: up to two bones per vertex), shared by the game-thread side
// (proxy/native/skin.cpp, replacing randy31's loop) and the renderer, which skins deferred jobs when it draws them.
// docs/skinning.md.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace rvk::skin {

// CATTriVertex_t: a mesh vertex as stored (0x44 bytes).
struct TriVertex {
    float posA[3];             // in bone A's space
    float posB[3];             // in bone B's space
    float bind[3];             // rest position
    float normal[3];           // bone A's space; also the rest normal
    float uv[2];
    int32_t boneA, boneB;
    float weightA;             // over 0.99f: bone A alone
};
static_assert(sizeof(TriVertex) == 0x44, "CATTriVertex_t");

// CATVertex_t: a skinned vertex (FVF 0x112: XYZ, NORMAL, TEX1).
struct Vertex {
    float pos[3];
    float normal[3];
    float uv[2];
};
static_assert(sizeof(Vertex) == 0x20, "CATVertex_t");
constexpr uint32_t kVertexFvf = 0x112;

// Bone matrix (0x30 bytes): rows 0-2 (floats 0-8) and the translation (9-11); p' = p * M.
struct Bone {
    float m[12];
};

// The bones in the layout the SIMD loop wants: four columns per bone.
struct Palette {
    struct alignas(16) Columns {
        float c[4][4];
    };
    Columns* bones = nullptr;
    uint32_t count = 0, capacity = 0;
    Palette() = default;
    Palette(const Palette&) = delete;
    Palette& operator=(const Palette&) = delete;
    ~Palette();
    void Set(const Bone* source, uint32_t count);
};

// Skins `count` vertices like randy31's loop: each written as the original writes it (vertices with a bone out of
// range are left alone), the box (either may be null) grown the way the original grows it.
void SkinVertices(const TriVertex* in, uint32_t count, Vertex* out, const Palette& bones, bool rest, float* boxMin,
                  float* boxMax);

// Positions only, of every `step`-th vertex (0, step, 2 step, ...); the others are left alone.
void SkinPositions(const TriVertex* in, uint32_t count, uint32_t step, Vertex* out, const Palette& bones, bool rest);

// A piece's mesh vertices, copied once (the game may free its mesh while a draw of it is still queued).
struct Source {
    std::vector<TriVertex> vertices;
    std::vector<uint16_t> indices;               // the piece's triangles
    const uint16_t* gameIndices = nullptr;       // where the game keeps them (draws pass that pointer)
    uint64_t indexHash = 0;                      // of the indices, as the renderer's mesh cache hashes them
    // Per bone, the box of the vertex positions given in its space (posA of the vertices it is bone A of, posB of
    // those it is bone B of): min x y z, max x y z. Empty boxes have min > max.
    std::vector<float> boneBoxes;
    float bindMin[3] = {}, bindMax[3] = {};      // the rest pose's box
    void Finish();                               // after filling: indexHash, the boxes
};

// One piece of one character to skin: its vertices and the character's bones at the time. Shared by every draw of
// the piece until the game skins it again; the vertices are skinned once, by whoever needs them first (the renderer
// for a draw, the game's thread when it reads the buffer back).
struct Job {
    std::shared_ptr<const Source> source;
    std::shared_ptr<const Palette> bones;
    std::shared_ptr<const Palette> prevBones;    // the piece's bones before (motion vectors); null: none
    bool rest = false;
    uint64_t gpuFrame = 0;                       // the renderer's: the frame it first skinned it on the GPU
    const Vertex* Skinned();                     // thread safe
    // A box around the skinned vertices (model space), from the bones and the mesh's per-bone boxes - without
    // skinning (ComputeBounds, when the job is made). Larger than the exact one by a little.
    float boundsMin[3] = {}, boundsMax[3] = {};
    void ComputeBounds();
    // The same for several pieces of one character together (one box for all: cheaper than one each).
    static void BoundsOf(const Source* const* sources, size_t count, const Palette& bones, bool rest, float boxMin[3],
                         float boxMax[3]);
    // Starts skinning on a pool thread, so it's done by the time it's drawn (Skinned waits if it isn't) - unless the
    // renderer skins on the GPU (SetPrefetch(false)).
    static void Prefetch(const std::shared_ptr<Job>& job);
    static void SetPrefetch(bool enable);
private:
    std::once_flag m_once;
    std::vector<Vertex> m_out;
};

}  // namespace rvk::skin
