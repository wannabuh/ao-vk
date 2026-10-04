// Character skinning: replaces randy31_orig's CPU skinning loop (FUN_1005470d; docs/skinning.md).
//
// randy-vk.ini [Native] Skin:
//   off     the original loop
//   cpu     ours, on the game's thread (SSE; same results)
//   on      deferred: the game's thread only hands each piece's vertex buffer a skin job (its mesh vertices and the
//           character's bones); the renderer skins it when it draws it. Falls back to `cpu` for characters whose
//           vertices the game needs right away (effects that read or move them), and for picking.
//   verify  the original, compared with ours (differences in randy-vk.log)
#pragma once

#include "native/cat.h"
#include "native/native.h"

#include <cstdint>
#include <memory>

namespace rnative::skin {

using rvk::skin::Palette;
using rvk::skin::SkinVertices;

// VertexBuffer_c::Lock / Unlock (randy31 exports, thiscall).
using LockFn = void*(__fastcall*)(void* buffer, void* edx, uint32_t offset, uint32_t flags);
using UnlockFn = void(__fastcall*)(void* buffer, void* edx);

// The whole loop for one character (CATRender_t) on this thread, with the callbacks: the original's job.
void SkinRender(void* render, float* boxMin, float* boxMax, LockFn lock, UnlockFn unlock);

// The renderer's side of deferred skinning (proxy/ddraw): gives the IDirect3DVertexBuffer7 behind a piece's
// VertexBuffer_c its job. False: not a buffer the renderer skins (the game's thread does).
using SinkFn = bool (*)(void* d3dVertexBuffer, std::shared_ptr<rvk::skin::Job> job);
void SetSink(SinkFn sink);

// Installs the replacement (randy-vk.ini [Native] Skin).
void Install(HMODULE orig);

}  // namespace rnative::skin
