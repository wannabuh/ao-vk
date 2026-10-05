// RShadow natively (rshadow.cpp): randy-vk.ini [Native] Scene=on. The projector shadow a character (or a static) casts:
// an RVisual_t whose visual, origin and material come from an archive, and which builds the shadow matrix from its
// normal / direction / offset and the two world matrices when asked (the `--shadow` scene draws it).
#pragma once

#include "native/native.h"

namespace rnative::rshadow {

void Install(HMODULE orig);

}  // namespace rnative::rshadow
