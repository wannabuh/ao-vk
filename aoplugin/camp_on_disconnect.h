#pragma once

// A lost or timed-out server connection ends like /camp - back to the login screen - instead of closing the game and
// starting the launcher. See camp_on_disconnect.cpp.

#include "aop_plugin.h"

namespace aovk::camp {

void Attach(const AopHost* host);
void GameReady();   // the code patches: GUI.dll is loaded, no connection yet

}  // namespace aovk::camp
