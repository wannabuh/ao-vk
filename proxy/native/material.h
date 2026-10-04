// RDeltaState, RMaterial_t and DefaultMaterial_t natively (material.cpp), installed with the device layer
// ([Native] Device=on).
#pragma once

#include "native/native.h"

namespace rnative::material {

void Install(HMODULE orig);

}  // namespace rnative::material
