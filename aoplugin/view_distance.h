#pragma once

// View distance: characters and full-quality ground further out than the game's sliders allow, and placed objects
// (statels) further out. See view_distance.cpp.

#include "aop_plugin.h"

#include <string>

namespace aovk::viewdist {

void Attach(const AopHost* host);   // its settings (AOPlugins.ini [ao-vk])
void GameReady();                   // the code patches: before the game world exists
void WorldReady();                  // the sliders' new ranges; the saved values go back on the next frame
void GameFrame();
void DValueChanged(const char* name, const AopValue* value);
bool SettingChanged(const char* name, int32_t value);   // true: one of its settings
void WriteTab(std::string& xml);    // its part of the Renderer tab

// A value the AOReloaded fork kept in AOReloaded.ini (the first time a setting is added here), or `fallback`.
int32_t AOReloadedIni(const AopHost* host, const char* key, int32_t fallback);

}  // namespace aovk::viewdist
