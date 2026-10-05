// Timer natively (timer.cpp): randy-vk.ini [Native] Scene=on. A stopwatch over QueryPerformanceCounter: start / stop
// accumulate the ticks it ran, GetSec returns the accumulated (or, while running, start-to-now) ticks times the tick
// length. The tick length is one over the counter's frequency, computed once and kept where the original keeps it.
#pragma once

#include "native/native.h"

namespace rnative::timer {

void Install(HMODULE orig);

}  // namespace rnative::timer
