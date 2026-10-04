// The device layer natively (device.cpp): randy-vk.ini [Native] Device=on. DeviceState (the render state cache),
// render_t's draw calls, transforms, lights and vertex buffer creation, VertexBuffer_c - in the original's layouts,
// talking to the same IDirect3DDevice7 / IDirect3DVertexBuffer7 (the call log of a frame is identical).
#pragma once

#include "native/native.h"

#include <cstdint>

namespace rnative::device {

void Install(HMODULE orig);

// VertexBuffer_c::GetFormatSize: bytes per vertex of an FVF, as Randy counts them (its table of FVF bits).
uint32_t FormatSize(uint32_t fvf);

}  // namespace rnative::device
