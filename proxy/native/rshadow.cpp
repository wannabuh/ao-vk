// RShadow natively (randy-vk.ini [Native] Scene=on). RShadow (0x1F0 bytes) is an RVisual_t (0x178 bytes) with, at
// +0x178, a material RResource_t, a visual RVisual_t, an origin RRefFrame_t, an RGB colour (+0x184), a normal
// (+0x190), a direction (+0x19C), an offset (+0x1A8), the shadow TMatrix4_t (+0x1AC) and a dirty flag (+0x1EC). Its
// two vtables are the RVisual_t part at +0 and the RRefFrame_t part at +0xA4. The base RVisual_t and the vector /
// matrix helpers are already native.
#include "native/rshadow.h"

#include "native/helpers.h"
#include "native/orig_api.gen.h"
#include "native/serialize.h"
#include "native/vc10.h"

#include <cstring>

namespace rnative::rshadow {

namespace {

HMODULE g_orig;

template <typename T>
T& G(uint32_t rva) { return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(g_orig) + rva); }
template <typename F>
F Internal(uint32_t rva) { return reinterpret_cast<F>(AddressFor(rva)); }
const serialize::Api& S() { return serialize::Get(); }

constexpr uint32_t kVtable0 = 0x956FC, kVtableA4 = 0x956E8;
// RVisual_t (already native): copy / archive ctor, destructor, Archive, RenderShadow, Process, SetRenderPriority.
constexpr uint32_t kVisualCopy = 0x4D5E5, kVisualArchiveCtor = 0x4D6D3, kVisualDtor = 0x4D7D3, kVisualArchive = 0x4C947,
                   kVisualRenderShadow = 0x4CABA, kVisualProcess = 0x4CC35, kVisualSetPriority = 0x4CB7E;
constexpr uint32_t kAddRef = 0x4621A, kRelease = 0x46224, kIdentity = 0x2A406;
// The type-specific archive finds (material, visual, origin) stay the original's.
constexpr uint32_t kFindMaterial = 0x13D19, kFindVisual = 0x46DB0, kFindOrigin = 0x2BDDC;

struct Shadow {
    uint8_t base[0x178];
    void* material;              // +0x178
    void* visual;                // +0x17c
    void* origin;                // +0x180
    float color[3];              // +0x184
    float normal[3];             // +0x190
    float dir[3];                // +0x19c
    float offset;                // +0x1a8
    float matrix[16];            // +0x1ac
    uint8_t dirty;               // +0x1ec
    uint8_t pad[3];
};
static_assert(sizeof(Shadow) == 0x1F0, "RShadow");

void SetVtables(Shadow* self)
{
    *reinterpret_cast<uint32_t*>(self) = reinterpret_cast<uint32_t>(reinterpret_cast<uint8_t*>(g_orig) + kVtable0);
    *reinterpret_cast<uint32_t*>(self->base + 0xA4) =
        reinterpret_cast<uint32_t>(reinterpret_cast<uint8_t*>(g_orig) + kVtableA4);
}
void Identity(float* matrix) { Internal<void(__fastcall*)(void*, void*)>(kIdentity)(matrix, nullptr); }
const float* WorldMatrix(void* frame)
{
    return static_cast<const float*>(orig::RRefFrame_t_GetWorldMatrix(frame));
}
void SetVector(float* to, const float* from)
{
    to[0] = from[0], to[1] = from[1], to[2] = from[2];
}

}  // namespace

void* __fastcall CtorArchive(Shadow* self, void*, void* archive)
{
    Internal<void*(__fastcall*)(void*, void*, void*)>(kVisualArchiveCtor)(self, nullptr, archive);
    SetVtables(self);
    for (float& c : self->color) c = 0;
    for (float& n : self->normal) n = 0;
    for (float& d : self->dir) d = 0;
    self->offset = 0;
    Identity(self->matrix);
    self->material = nullptr;
    self->visual = nullptr;
    self->origin = nullptr;
    Internal<void(__fastcall*)(void*, void*, int)>(kVisualSetPriority)(self, nullptr, 2);
    float up[3] = {0.0f, 1.0f, 0.0f};
    helpers::NormalizeScale(up, nullptr, 1.0f);
    SetVector(self->normal, up);
    float direction[3] = {1.0f, G<float>(0x953D0), 1.0f};
    helpers::NormalizeScale(direction, nullptr, 1.0f);
    SetVector(self->dir, direction);
    self->offset = 0;
    void* stream = S().getStream(archive, nullptr);
    Internal<void(__fastcall*)(void*, void*, const char*, void**, void*)>(kFindMaterial)(stream, nullptr, "material",
                                                                                        &self->material, nullptr);
    Internal<void(__fastcall*)(void*, void*, const char*, void**, void*)>(kFindVisual)(stream, nullptr, "visual",
                                                                                      &self->visual, nullptr);
    Internal<void(__fastcall*)(void*, void*, const char*, void**, void*)>(kFindOrigin)(stream, nullptr, "origin",
                                                                                      &self->origin, nullptr);
    S().findRgb(stream, nullptr, "color", self->color, 0);
    S().findVector3(stream, nullptr, "normal", self->normal, 0);
    S().findVector3(stream, nullptr, "dir", self->dir, 0);
    S().findFloat(stream, nullptr, "offset", &self->offset, 0);
    return self;
}

void* __fastcall CtorCopy(Shadow* self, void*, const Shadow* source)
{
    Internal<void*(__fastcall*)(void*, void*, const void*)>(kVisualCopy)(self, nullptr, source);
    SetVtables(self);
    for (float& c : self->color) c = 0;
    for (float& n : self->normal) n = 0;
    for (float& d : self->dir) d = 0;
    self->offset = 0;
    Identity(self->matrix);
    self->visual = source->visual;
    self->origin = source->origin;
    SetVector(self->color, source->color);
    SetVector(self->normal, source->normal);
    SetVector(self->dir, source->dir);
    self->offset = source->offset;
    self->material = source->material;
    if (self->material) Internal<void(__fastcall*)(void*, void*)>(kAddRef)(self->material, nullptr);
    return self;
}

void __fastcall Dtor(Shadow* self, void*)
{
    SetVtables(self);
    if (self->material) Internal<void(__fastcall*)(void*, void*)>(kRelease)(self->material, nullptr);
    Internal<void(__fastcall*)(void*, void*)>(kVisualDtor)(self, nullptr);
}

void __fastcall Archive(Shadow* self, void*, void* archive)
{
    Internal<void(__fastcall*)(void*, void*, void*)>(kVisualArchive)(self, nullptr, archive);
    void* stream = S().getStream(archive, nullptr);
    S().addInt32(stream, nullptr, "version", 1);
    S().addObject(stream, nullptr, "material", self->material);
    S().addObject(stream, nullptr, "visual", self->visual);
    S().addObject(stream, nullptr, "origin", self->origin);
    S().addRgb(stream, nullptr, "color", self->color);
    S().addVector3(stream, nullptr, "normal", self->normal);
    S().addVector3(stream, nullptr, "dir", self->dir);
    S().addFloat(stream, nullptr, "offset", self->offset);
}

// The shadow matrix: the visual's world matrix with its origin moved along the direction by offset / (normal . dir),
// the normal * -offset added, then the shadow's own origin.
void __fastcall Process(Shadow* self)
{
    const float dot = self->normal[2] * self->dir[2] + self->normal[0] * self->dir[0] +
                      self->normal[1] * self->dir[1];
    const float scale = self->offset / dot;
    std::memcpy(self->matrix, WorldMatrix(self->visual), sizeof(self->matrix));
    float local[3];
    helpers::ScaleVector(self->dir, nullptr, local, scale);
    self->matrix[12] = local[0], self->matrix[13] = local[1], self->matrix[14] = local[2];
    helpers::MatrixMove(self->matrix, nullptr, self->dir[0], self->dir[2]);
    self->matrix[5] = 0;
    helpers::ScaleVector(self->normal, nullptr, local, -self->offset);
    helpers::TranslateAdd(self->matrix, nullptr, local);
    helpers::TranslateAdd(self->matrix, nullptr, WorldMatrix(self) + 12);
    self->dirty = 0;
}

void __fastcall ProcessVisual(Shadow* self)
{
    Internal<void(__fastcall*)(void*, void*)>(kVisualProcess)(self, nullptr);
    if (self->origin && ((static_cast<uint8_t*>(self->origin)[0x9D] & 1) ||
                         (reinterpret_cast<uint8_t*>(self)[0x9D] & 1))) {
        float local[3];
        helpers::Subtract(WorldMatrix(self) + 12, nullptr, local, WorldMatrix(self->origin) + 12);
        helpers::NormalizeScale(local, nullptr, 1.0f);
        SetVector(self->dir, local);
        self->dirty = 1;
    }
}

void __fastcall RenderShadow(void* self, void*, void* viewport, void* shadow)
{
    Internal<void(__fastcall*)(void*, void*, void*, void*)>(kVisualRenderShadow)(self, nullptr, viewport, shadow);
}

void* __fastcall GetMatrix(Shadow* self, void*)
{
    if (self->dirty) Process(self);
    return self->matrix;
}

void* __fastcall DeletingDtor(Shadow* self, void*, uint8_t flags)   // 0x46e3e
{
    Dtor(self, nullptr);
    if (flags & 1) vc10::Free(self);
    return self;
}

void* __fastcall DeletingDtorA4(void* self, void*, uint8_t flags)   // 0x46df9: adjusts to the full object
{
    return DeletingDtor(reinterpret_cast<Shadow*>(static_cast<uint8_t*>(self) - 0xA4), nullptr, flags);
}

void* __fastcall NewCopy(const Shadow* source, void*)               // 0x46e04
{
    Shadow* self = static_cast<Shadow*>(vc10::Allocate(0x1F0));
    return self ? CtorCopy(self, nullptr, source) : nullptr;
}

void* __cdecl Instantiate(void* archive)                            // 0x46d78
{
    Shadow* self = static_cast<Shadow*>(vc10::Allocate(0x1F0));
    return self ? CtorArchive(self, nullptr, archive) : nullptr;
}

void Install(HMODULE orig)
{
    if (GetMode("Scene", Mode::Off) != Mode::On) return;
    g_orig = orig;
    struct Entry {
        uint32_t rva;
        void* target;
        const char* what;
    };
#define FN(f) reinterpret_cast<void*>(&f)
    const Entry entries[] = {
        {0x46587, FN(CtorCopy), "RShadow copy constructor (FUN_10046587)"},
        {0x4666A, FN(Dtor), "RShadow destructor (FUN_1004666a)"},
        {0x466B2, FN(Archive), "RShadow::Archive (FUN_100466b2)"},
        {0x46788, FN(Process), "RShadow shadow matrix (FUN_10046788)"},
        {0x46883, FN(RenderShadow), "RShadow::RenderShadow (FUN_10046883)"},
        {0x46B4A, FN(CtorArchive), "RShadow(archive) constructor (FUN_10046b4a)"},
        {0x46CF1, FN(ProcessVisual), "RShadow vtable Process (FUN_10046cf1)"},
        {0x46D5F, FN(GetMatrix), "RShadow shadow matrix accessor (FUN_10046d5f)"},
        {0x46D78, FN(Instantiate), "RShadow::Instantiate"},
        {0x46DF9, FN(DeletingDtorA4), "RShadow deleting destructor, RRefFrame_t part (FUN_10046df9)"},
        {0x46E04, FN(NewCopy), "RShadow copy into a fresh object (FUN_10046e04)"},
        {0x46E3E, FN(DeletingDtor), "RShadow deleting destructor (FUN_10046e3e)"},
    };
#undef FN
    int installed = 0;
    for (const Entry& e : entries) installed += Replace(orig, e.rva, e.target, e.what) ? 1 : 0;
    Log("rshadow: %d of %d functions native", installed, int(sizeof(entries) / sizeof(entries[0])));
}

}  // namespace rnative::rshadow
