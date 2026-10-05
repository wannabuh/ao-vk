// RGrid natively (rgrid.cpp): randy-vk.ini [Native] Scene=on. The reference grid: a line list built from hcellcnt x
// vcellcnt cells centred on the frame's origin, coloured by SetColor, with a BVolume_t around its vertices; drawn by a
// direct render_t::RenderLineList (it is not an RVisualData, so it has its own Render).
#pragma once

#include "native/native.h"

namespace rnative::rgrid {

void Install(HMODULE orig);

}  // namespace rnative::rgrid
