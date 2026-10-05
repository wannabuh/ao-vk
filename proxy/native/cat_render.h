// Drawing a character natively (cat_render.cpp): randy-vk.ini [Native] CatRender=on.
#pragma once

#include "native/native.h"

namespace rnative::catrender {

void Install(HMODULE orig);

// RVisual_t's observers (its std::set at +0xAC), as RVisual_t's drawing asks them (FUN_10013e2e / FUN_10013e71 /
// FUN_10013ebb / FUN_10013f00): whether one skips drawing it; whether they all set the D3D material; after the
// material; after drawing.
bool ObserversSkip(void* visual, void* viewport);
bool ObserversMaterial(void* visual, void* material, void* viewport, void* d3dMaterial);
void ObserversAfterMaterial(void* visual, void* material, void* viewport);
void ObserversAfter(void* visual, void* viewport);

}  // namespace rnative::catrender
