// GUI.dll's interface drawing, for the interface layer (docs/rvk.md "Interface refresh rate", RVK_UiRate) and the
// interface plugin (docs/ui-dll.md): WindowController_c::Render and View::_CallRender hooked here, in one place, and
// the plugin called from them.
#pragma once

#include "aovk_plugin.h"

namespace rnative::uilayer {

// The plugin's table (null: none). Before Install.
void SetPlugin(const AoVkPlugin* plugin);

// Hooks GUI.dll once it is loaded (called each presented frame while the hooks are wanted); later calls do nothing.
void Install();

}  // namespace rnative::uilayer
