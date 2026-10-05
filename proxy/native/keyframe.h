// RKeyFrameAnimation_t natively (keyframe.cpp): randy-vk.ini [Native] Scene=on.
#pragma once

#include "native/native.h"

#include <cstdint>

namespace rnative::keyframe {

void Install(HMODULE orig);

// FUN_10028fde (for tests): the animation `a` at `time` into `matrix` (rotation, position) and `visible`.
void __fastcall Evaluate(uint8_t* a, void*, float time, float* matrix, uint8_t* visible);

}  // namespace rnative::keyframe
