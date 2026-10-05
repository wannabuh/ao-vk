// TextureStreamCreator natively (texture_stream.cpp): randy-vk.ini [Native] Device=on. The creator DisplaySystem uses
// to turn an RDB texture (a fun::PositionIO_t) or an already-decoded LBitmap_t into a D3D7 texture. This part is the
// bitmap path CreateTexture and EnableCompression; the ctors and the stream path follow.
#pragma once

#include "native/native.h"
#include "native/vc10.h"

#include <cstdint>

namespace rnative::texturestream {

// TextureStreamCreator (0x40 bytes): a fun::Serializable_c (its name std::string at +8).
struct Creator {
    void* vtable;                                            // +0x00
    uint32_t pad;                                            // +0x04
    vc10::String name;                                       // +0x08
    void* stream;                                            // +0x24 (fun::PositionIO_t, or null)
    int32_t format;                                          // +0x28 (D3DX_SURFACEFORMAT)
    void* bitmap;                                            // +0x2c (LBitmap_t, or null)
    uint8_t flag30;                                          // +0x30
    uint8_t pad2[3];
    int32_t divisor;                                         // +0x34 (the downscale divisor)
    uint8_t flag38;                                          // +0x38
    uint8_t pad3[3];
    int32_t filter;                                          // +0x3c (D3DX_FILTERTYPE)
};
static_assert(sizeof(Creator) == 0x40, "TextureStreamCreator");

void Install(HMODULE orig);
void SetModule(HMODULE orig);

// TextureStreamCreator::EnableCompression(bool) (static, 0x191b8): whether texture compression is used.
void __cdecl EnableCompression(bool on);
// TextureStreamCreator::CreateTexture(LBitmap_t*, char const*): makes the surface (0x193e4).
void* __fastcall CreateTexture(Creator* self, void*, void* bitmap, const char* name);
// TextureStreamCreator::CreateTexture(fun::PositionIO_t*, char const*): load the bitmap, then make the surface (0x1973a).
void* __fastcall CreateFromStream(Creator* self, void*, void* stream, const char* name);
// The vtable's Process (0x19960): make the surface from the stored bitmap or stream.
void* __fastcall Process(Creator* self, void*);

// The ctors / archive (rest of the class).
void* __fastcall CtorArchive(Creator* self, void*, void* archive);                                     // 0x1920f
void* __fastcall CtorStream(Creator* self, void*, void* stream, const char* name, int32_t divisor);    // 0x192be
void* __fastcall CtorBitmap(Creator* self, void*, void* bitmap, const char* name, int32_t divisor);    // 0x19351
void* __cdecl Instantiate(void* archive);                                                              // 0x19289
void __fastcall Archive(Creator* self, void*, void* archive);                                          // 0x191a0
void __fastcall ArchiveTexture(void* self, void*, void* archive);                                      // 0x17eb0
void __fastcall Dtor(Creator* self, void*);                                                            // 0x19852
void* __fastcall DeletingDtor(Creator* self, void*, uint8_t flags);                                    // 0x19870
void* __fastcall DeletingDtorBase(Creator* self, void*, uint8_t flags);                                // 0x19774
void* __fastcall NameAccessor(Creator* self, void*);                                                   // 0x1984a
uint8_t __fastcall FlagAccessor(Creator* self, void*);                                                 // 0x1984e

}  // namespace rnative::texturestream
