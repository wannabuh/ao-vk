// D3DX natively (d3dx.cpp): randy-vk.ini [Native] Device=on. D3DXMakeDDPixelFormat / D3DXMakeSurfaceFormat are
// lookups in D3DX7's format table; D3DXCreateTexture builds the surface and its image info; the render_t wrappers
// throw the game's fun::DXError on failure, as the original's do. The D3DX loaders and their conversion core are
// still the original's.
#pragma once

#include "native/native.h"

namespace rnative::d3dx {

void Install(HMODULE orig);

}  // namespace rnative::d3dx
