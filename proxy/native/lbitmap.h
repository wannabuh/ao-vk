// LBitmap_t natively (lbitmap.cpp): randy-vk.ini [Native] Device=on (the software bitmap textures are loaded into).
// This piece is the in-memory bitmap itself: its construction / destruction, data size, palette-to-24-bit conversion,
// the loader registry (every bitmap registers itself; a load finds a registered one by the extension its vtable slot
// 3 returns) and the integer nearest-neighbour Scale. The BMP/POSitionIO stream loader is still the original's.
#pragma once

#include "native/native.h"

#include <cstdint>

namespace rnative::lbitmap {

// LBitmap_t (0x124 bytes).
struct Bitmap {
    void* vtable;                                            // +0x00
    uint8_t pad[0x104];                                      // +0x04
    int32_t state;                                           // +0x108 (1 while registered, 2 on a load error)
    int32_t bitsPerPixel;                                    // +0x10c
    uint32_t width;                                          // +0x110
    uint32_t height;                                         // +0x114
    uint8_t* data;                                           // +0x118
    uint32_t* palette;                                       // +0x11c (a paletted bitmap's 0x00RRGGBB entries)
    uint8_t flag;                                            // +0x120
    uint8_t pad2[3];
};
static_assert(sizeof(Bitmap) == 0x124, "LBitmap_t");

void Install(HMODULE orig);
void SetModule(HMODULE orig);

void __fastcall Init(Bitmap* self, void*, int32_t arg);                              // 0x14c9b
void __fastcall Destroy(Bitmap* self, void*);                                        // 0x14cc0
int32_t __fastcall Size(const Bitmap* self, void*);                                  // 0x14cff
void __fastcall Make24Bit(Bitmap* self, void*);                                      // 0x14da8 (vtable slot 2)
void __cdecl Register(void* bitmap);                                                 // 0x14ebc
void* __cdecl Find(const char* extension);                                           // 0x14ed7
void __fastcall Scale(Bitmap* self, void*, uint32_t width, uint32_t height);         // 0x14f53
void __fastcall BaseCtor(Bitmap* self, void*);                                       // 0x15228
uint32_t __fastcall NotEqual(const float* self, void*, const float* other);          // 0x155c5 (a Vector3 compare)

}  // namespace rnative::lbitmap
