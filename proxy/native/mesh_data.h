// The data behind static meshes natively (mesh_data.cpp): randy-vk.ini [Native] Scene=on.
#pragma once

#include "native/native.h"

namespace rnative::meshdata {

void Install(HMODULE orig);

// RTriMeshData_t's lightmap copies: a private one (its meshes cloned and, when asked, moved to system memory), and
// the shared one cached at +0x68. RTriMesh_t::ConvertToLightmap (mesh.cpp) calls these.
void* MakePrivate(void* data, bool systemOnly);   // FUN_1004ac5f
void* SharedCopy(void* data);                     // FUN_1004ad93

// A count bumped by every write to any TriList's triangles (loaded, added to, copied into, flipped, freed) - all of
// them native code. While it is unchanged, triangle lists the renderer saw hold the same indices at the same address,
// so it can keep them (docs/device-on-rvk.md phase 3). 0: not tracked (a writer isn't native: unknown build).
uint64_t IndexGeneration();

}  // namespace rnative::meshdata
