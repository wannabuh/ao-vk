// Randy_t::GetDevices natively (devices.cpp), installed with the device layer ([Native] Device=on).
#pragma once

#include "native/native.h"

#include <cstdint>

namespace rnative::devices {

void Install(HMODULE orig);

// Randy_t::Initialize's look at the adapters: the best Randy_t::HardwareLevel_e of their Direct3D devices (0 if
// none), the list made and let go.
int32_t BestHardwareLevel();

}  // namespace rnative::devices
