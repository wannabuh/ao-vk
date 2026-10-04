// surface_t and render_t's DirectDraw side natively (surface.cpp), installed with the device layer
// ([Native] Device=on).
#pragma once

#include "native/native.h"

namespace rnative::surface {

void Install(HMODULE orig);

}  // namespace rnative::surface
