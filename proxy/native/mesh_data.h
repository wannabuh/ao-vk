// The data behind static meshes natively (mesh_data.cpp): randy-vk.ini [Native] Scene=on.
#pragma once

#include "native/native.h"

namespace rnative::meshdata {

void Install(HMODULE orig);

// RTriMeshData_t's lightmap copies: a private one (its meshes cloned and, when asked, moved to system memory), and
// the shared one cached at +0x68. RTriMesh_t::ConvertToLightmap (mesh.cpp) calls these.
void* MakePrivate(void* data, bool systemOnly);   // FUN_1004ac5f
void* SharedCopy(void* data);                     // FUN_1004ad93

}  // namespace rnative::meshdata
