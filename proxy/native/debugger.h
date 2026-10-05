// Debugger_t natively (debugger.cpp), installed with the device layer ([Native] Device=on).
#pragma once

#include "native/native.h"

namespace rnative::debugger {

void Install(HMODULE orig);

}  // namespace rnative::debugger
