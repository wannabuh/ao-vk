// A character's queries natively (cat_query.cpp): randy-vk.ini [Native] CatQuery=on.
#pragma once

#include "native/native.h"

namespace rnative::catquery {

void Install(HMODULE orig);

// FUN_10054df1: a named attractor's matrix in the character's space (the CATRender_t's); the bone it rides on adds
// its rotation and translation, not its scale. False without one - or without an animation (the matrix still written).
bool AttractorMatrix(const void* render, const char* name, float out[16]);
// FUN_10054f4f: a bone's matrix by name (needs an animation).
bool BoneMatrix(const void* render, const char* name, float out[16]);

}  // namespace rnative::catquery
