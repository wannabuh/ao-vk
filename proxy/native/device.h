// The device layer natively (device.cpp): randy-vk.ini [Native] Device=on. DeviceState (the render state cache),
// render_t's draw calls, transforms, lights and vertex buffer creation, VertexBuffer_c - in the original's layouts,
// talking to the same IDirect3DDevice7 / IDirect3DVertexBuffer7 (the call log of a frame is identical).
#pragma once

#include "native/native.h"

#include <cstdint>

namespace rnative::device {

void Install(HMODULE orig);

// VertexBuffer_c::GetFormatSize: bytes per vertex of an FVF, as Randy counts them (its table of FVF bits).
uint32_t FormatSize(uint32_t fvf);

// The direct channel to the renderer (docs/device-on-rvk.md): functions the rvk backend registers when it is the
// Direct3D 7 implementation. Each takes the IDirect3DDevice7 the call would have gone to and returns false when that
// isn't the backend's device - the caller then makes the D3D7 calls as before.
struct StateChange {
    enum Kind : uint32_t { RenderState, StageState, Texture } kind;
    uint32_t stage, type, value;            // RenderState: type = the state; Texture: stage
    void* surface;                          // Texture: the IDirectDrawSurface7 (or null)
};
struct Direct {
    // DeviceState::UpdateDevice's changes in the order it would make the calls (SetRenderState, SetTextureStageState,
    // SetTexture), as one.
    bool (*applyStates)(void* d3dDevice, const StateChange* changes, uint32_t count);
    // Game-thread time in a part of the native scene code (`name` a string literal), for the renderer's profile log.
    void (*gameSection)(const char* name, double ms);
};
void SetDirect(const Direct* direct);       // null: none (the backend's device is gone)

// Times a part of the game thread's frame into the profile ("cpu ms" line) when the rvk backend is there. Nested
// timers (an offscreen viewport rendered inside a visual's render) count in the outermost only.
class GameTimer {
public:
    explicit GameTimer(const char* name);
    ~GameTimer();
    GameTimer(const GameTimer&) = delete;
    GameTimer& operator=(const GameTimer&) = delete;

private:
    const char* m_name;
    int64_t m_start = 0;                    // 0: not the outermost (or no backend)
};

}  // namespace rnative::device
