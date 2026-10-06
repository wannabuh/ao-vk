// GUI.dll's interface drawing (gui.cpp): drawn every frame, or - RVK_UiRate - only some frames, the renderer showing
// the last one drawn in between (docs/rvk.md "Interface refresh rate").
#pragma once

namespace rnative::gui {

// Hooks GUI.dll once it is loaded (the first presented frames); later calls do nothing.
void Install();

}  // namespace rnative::gui
