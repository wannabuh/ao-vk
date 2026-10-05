// RandyShadowlandsData_s natively (shadowlands.cpp): randy-vk.ini [Native] Scene=on. The Shadowlands special lights'
// state - the ground light, the statel light and the CAT (character) light: each one's texture, its scale and offset
// in texture space, its intensity, its direction and the texture matrix built from the camera. DisplaySystem and
// Gamecode call the setters (they are exports); the renderer asks Get*LightMatrix / Is*LightUsed per frame.
#pragma once

#include "native/native.h"

namespace rnative::shadowlands {

void Install(HMODULE orig);

// The module the class's static storage lives in (for tests that call the functions without installing them).
void SetModule(HMODULE orig);

// The class's static methods, as they are (static, __cdecl): 16 floats for a matrix, 3 for a vector. `Get*Matrix`
// returns the class's own matrix (it rebuilds it when the camera or the scale / offset changed).
void __cdecl SetCameraMatrix(const float* matrix);

void __cdecl SetGroundLightTexture(void* texture);
void __cdecl SetGroundLightParameters(const float* scale, const float* offset);
void __cdecl SetGroundLightIntensity(float intensity);
void __cdecl EnableGroundLight(bool on);
void __cdecl SetGroundLightDirection(const float* direction);
float __cdecl GetGroundLightIntensity();
void* __cdecl GetGroundLightTexture();
const float* __cdecl GetGroundLightMatrix();
bool __cdecl IsGroundLightUsed();

void __cdecl SetStatelLightTexture(void* texture);
void __cdecl SetStatelLightParameters(const float* scale, const float* offset);
void __cdecl SetStatelLightIntensity(float intensity);
void __cdecl EnableStatelLight(bool on);
void __cdecl SetStatelLightDirection(const float* direction);
float __cdecl GetStatelLightIntensity();
void* __cdecl GetStatelLightTexture();
const float* __cdecl GetStatelLightMatrix();
bool __cdecl IsStatelLightUsed();

void __cdecl SetCATLightTexture(void* texture);
void __cdecl SetCATLightParameters(const float* scale, const float* offset);
void __cdecl SetCATLightIntensity(float intensity);
void __cdecl EnableCATLight(bool on);
void __cdecl SetCATLightDirection(const float* direction);
float __cdecl GetCATLightIntensity();
void* __cdecl GetCATLightTexture();
const float* __cdecl GetCATLightMatrix();
bool __cdecl IsCATLightUsed();

void __cdecl FreeAllTextures();
bool __cdecl PriCheck();

}  // namespace rnative::shadowlands
