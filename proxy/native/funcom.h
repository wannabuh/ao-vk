// The Funcom libraries linked into Randy (funcom.cpp): randy-vk.ini [Native] Scene=on. Funcom's config / stream base
// classes (FC_Base_t, SL_Chunk_t, SL_StreamControl_t) and a 4x4 matrix multiply, all in 0x6E06A-0x787AE. They only
// call Randy's own code, so they are ported like Randy's.
#pragma once

#include "native/native.h"

namespace rnative::funcom {

void Install(HMODULE orig);

}  // namespace rnative::funcom
