// The D3DX format conversions natively (d3dx.cpp): randy-vk.ini [Native] Device=on. D3DXMakeDDPixelFormat /
// D3DXMakeSurfaceFormat are lookups in D3DX7's format table; the render_t wrappers around them throw the game's
// fun::DXError on failure, as the original's do.
#pragma once

#include "native/native.h"

namespace rnative::d3dx {

void Install(HMODULE orig);

}  // namespace rnative::d3dx
