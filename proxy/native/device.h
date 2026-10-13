// The device layer natively (device.cpp): randy-vk.ini [Native] Device=on. DeviceState (the render state cache),
// render_t's draw calls, transforms, lights and vertex buffer creation, VertexBuffer_c - in the original's layouts,
// talking to the same IDirect3DDevice7 / IDirect3DVertexBuffer7 (the call log of a frame is identical).
#pragma once

#include "native/native.h"

#include "aovk_plugin.h"

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
// The interface back end's shapes and meshes (rvk::Device::UiShape / UiVertex / UiMesh / UiMeshes, the same layouts):
// the interface plugin API's (include/aovk_plugin.h).
using UiShape = AoVkUiShape;
using UiVertex = AoVkUiVertex;
using UiMesh = AoVkUiMesh;
using UiMeshes = AoVkUiMeshes;

struct Direct {
    // DeviceState::UpdateDevice's changes in the order it would make the calls (SetRenderState, SetTextureStageState,
    // SetTexture), as one.
    bool (*applyStates)(void* d3dDevice, const StateChange* changes, uint32_t count);
    // Game-thread time in a part of the native scene code (`name` a string literal), for the renderer's profile log.
    void (*gameSection)(const char* name, double ms);
    // IDirect3DDevice7::DrawIndexedPrimitiveVB whose indices belong to a native triangle list unchanged since
    // `indexGeneration` (meshdata::IndexGeneration): the backend may keep them instead of copying them each draw.
    bool (*drawIndexedVB)(void* d3dDevice, uint32_t type, void* d3dVertexBuffer, uint32_t start, uint32_t vertexCount,
                          const uint16_t* indices, uint32_t indexCount, uint64_t indexGeneration);
    // The renderer drops blob shadow draws (its sun shadows replace them).
    bool (*blobShadowsReplaced)();
    // Around the interface's drawing (gui.cpp, GUI.dll's WindowController_c::Render): begin returns whether to draw it
    // this frame - false: the renderer shows the last one drawn (its interface layer, RVK_UiRate). end after it.
    bool (*interfaceBegin)();
    void (*interfaceEnd)();
    // While the interface is drawn: a window's rectangle on screen, for the glass behind it (docs/ui-dll.md).
    void (*interfaceWindow)(float x0, float y0, float x1, float y1);
    // While the interface is drawn: shapes of our own, where its drawing has got to (UiShape below, phase 2).
    // With meshes (or null) between the shapes' fills and their depth (UiMeshes below, phase 9).
    void (*interfaceShapes)(const UiShape* shapes, uint32_t count, const UiMeshes* meshes);
    // An interface image from BGRA pixels (A8R8G8B8, tightly packed): a handle for UiShape::image, or null; and its end.
    void* (*interfaceImage)(const void* bgra, uint32_t width, uint32_t height);
    void (*interfaceImageFree)(void* image);
};
void SetDirect(const Direct* direct);       // null: none (the backend's device is gone)

// True when the renderer draws its own shadows instead of the game's blob shadows (direct channel on): the blob
// shadows' per-frame work (cat_skin.cpp: its callback over skinned positions) can be skipped.
bool BlobShadowsReplaced();

// The direct channel's interfaceBegin / interfaceEnd / interfaceWindow (true / nothing without it). Unlike the rest of
// the channel these don't need [Native] Device: they come from GUI.dll's hooks (gui.cpp), not the device layer.
bool InterfaceBegin();
void InterfaceEnd();
void InterfaceWindow(float x0, float y0, float x1, float y1);
void InterfaceShapes(const UiShape* shapes, uint32_t count, const UiMeshes* meshes = nullptr);
void* InterfaceImage(const void* bgra, uint32_t width, uint32_t height);
void InterfaceImageFree(void* image);

// Around a native mesh's draw (mesh.cpp): its indices are a native triangle list's, unchanged while `generation`
// (meshdata::IndexGeneration, non-zero) is. [Native] Retain = on.
class RetainedIndices {
public:
    explicit RetainedIndices(uint64_t generation);
    ~RetainedIndices();
    RetainedIndices(const RetainedIndices&) = delete;
    RetainedIndices& operator=(const RetainedIndices&) = delete;

private:
    uint64_t m_previous;
};

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

// The same between two points that aren't one scope: GameClock() at the start (0 without the backend), then
// GameSectionSince(name, start) at the end.
int64_t GameClock();
void GameSectionSince(const char* name, int64_t start);

}  // namespace rnative::device
