// The game's character classes (CAT = the animation system), as laid out in randy31_orig.dll. Field offsets from
// the decompiled original (docs/skinning.md); accessed through At<> since the classes are only partly known.
#pragma once

#include "skin.h"

#include <cstdint>

namespace rnative {

template <typename T>
inline T& At(void* object, uint32_t offset)
{
    return *reinterpret_cast<T*>(static_cast<uint8_t*>(object) + offset);
}
template <typename T>
inline const T& At(const void* object, uint32_t offset)
{
    return *reinterpret_cast<const T*>(static_cast<const uint8_t*>(object) + offset);
}

namespace cat {

// Vertices and bones (rvk/skin.h).
using TriVertex = rvk::skin::TriVertex;
using Vertex = rvk::skin::Vertex;
using Bone = rvk::skin::Bone;

// CATRender_t (the first 0x3C bytes of RCATMesh_t).
constexpr uint32_t kRenderMesh = 0x04;          // CATMesh_t*
constexpr uint32_t kRenderAnim = 0x08;          // CATAnim_t* (null: not skinned at all)
constexpr uint32_t kRenderGroupCount = 0x0C;
constexpr uint32_t kRenderGroups = 0x10;        // RenderGroup[groupCount]
constexpr uint32_t kRenderBoneCount = 0x14;
constexpr uint32_t kRenderBones = 0x18;         // Bone[boneCount]
constexpr uint32_t kRenderRest = 0x28;          // uint8_t: 1 = rest pose
constexpr uint32_t kRenderCallbacks = 0x2C;     // Callback* begin, end (std::vector)

// Per group of the mesh (0xC bytes): the mesh, and one vertex buffer slot per piece.
struct RenderGroup {
    void* mesh;                // CATMesh_t*
    uint32_t unknown;
    struct Slot {
        void* d3dBuffer;
        void* buffer;          // VertexBuffer_c*
    }* slots;
};
static_assert(sizeof(RenderGroup) == 0xC, "CATRender_t group");

constexpr uint32_t kMeshGroups = 0x4C;          // CATMesh_t: MeshGroup array, 0x34 bytes each
constexpr uint32_t kGroupSize = 0x34;
constexpr uint32_t kGroupPieceCount = 0x1C;
constexpr uint32_t kGroupPieces = 0x20;         // CATTriPolyList_t array, 0x34 bytes each
constexpr uint32_t kPieceSize = 0x34;
constexpr uint32_t kPieceVertices = 0x0C;       // TriVertex*
constexpr uint32_t kPieceActiveTris = 0x10;
constexpr uint32_t kPieceIndices = 0x14;        // uint16_t*
constexpr uint32_t kPieceVertexCount = 0x2C;
constexpr uint32_t kPieceTriCount = 0x30;

// A vertex process callback (CATRender_t::RegisterVertexProcessCallback), run after each piece is skinned.
using CallbackFn = void(__cdecl*)(const void* render, uint32_t vertexCount, const TriVertex* in, Vertex* out,
                                  uint32_t triCount, const uint16_t* indices, uint32_t vertexBase, uint32_t triBase,
                                  void* user);
struct Callback {
    void* user;
    CallbackFn fn;
};

}  // namespace cat
}  // namespace rnative
