// The FAF data classes natively (faf.cpp): randy-vk.ini [Native] Scene=on. The scene reader builds them from an
// object archive (tools/extract-static.py's `.archive`); they are thin subclasses of the renderer's own classes.
// Here FAFTexture_t (an RTexture_t) and FAFAttractor_t (an RRefFrame_t); the bases are already native.
#pragma once

#include "native/native.h"

namespace rnative::faf {

void Install(HMODULE orig);

}  // namespace rnative::faf
