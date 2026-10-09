// Pop-in log (randy-vk.ini [Native] PopLog=on): why visuals appear or vanish in view at a distance. Every frame the
// visuals added to the render lists are compared with the frame before; one that starts (or stops) being drawn inside
// the view, further than PopLogMin metres (default 15), is logged with what changed just before - made or destroyed,
// attached to or detached from the scene, made visible or invisible (with the game's callers found on the stack) - or,
// when none of that happened, its own Process (vtable slot 8) deciding not to draw it. A summary every 600 frames
// groups the events by cause, class and caller with their distances.
#pragma once

#include "native/native.h"

#include <cstdint>

namespace rnative::popin {

void Install(HMODULE orig);
bool Enabled();

// From the native scene code; each returns at once while the log is off.
void FrameStart(void* viewport, void* root);       // RViewPort_t::Process, before the scene is processed
void Drawn(const void* visual);                    // RVisual_t::AddToRenderList
void Created(const void* visual, const void* caller);
void Destroyed(const void* visual);
void Attached(const void* frame, bool attached, const void* caller);
void VisibleSet(const void* frame, bool visible, const void* caller);

}  // namespace rnative::popin
