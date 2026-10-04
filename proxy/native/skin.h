// Character skinning (replaces randy31_orig FUN_1005470d, the CPU skinning loop; docs/skinning.md).
#pragma once

#include "native/cat.h"
#include "native/native.h"

#include <cstdint>

namespace rnative::skin {

// The bones in the layout the SIMD loop wants: four columns per bone.
struct Palette {
    struct alignas(16) Columns {
        float c[4][4];
    };
    Columns* bones = nullptr;
    uint32_t count = 0, capacity = 0;
    void Set(const cat::Bone* source, uint32_t count);
    ~Palette();
};

// Skins `count` vertices like the original: each written as the original writes it (vertices with a bone out of range
// are left alone), the box (either may be null) grown the way the original grows it.
void SkinVertices(const cat::TriVertex* in, uint32_t count, cat::Vertex* out, const Palette& bones, bool rest,
                  float* boxMin, float* boxMax);

// VertexBuffer_c::Lock / Unlock (randy31 exports, thiscall).
using LockFn = void*(__fastcall*)(void* buffer, void* edx, uint32_t offset, uint32_t flags);
using UnlockFn = void(__fastcall*)(void* buffer, void* edx);

// The whole loop for one character (CATRender_t), with the callbacks: the original's job.
void SkinRender(void* render, float* boxMin, float* boxMax, LockFn lock, UnlockFn unlock);

// Installs the replacement (randy-vk.ini [Native] Skin).
void Install(HMODULE orig);

}  // namespace rnative::skin
