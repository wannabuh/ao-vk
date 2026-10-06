// The device layer, native (randy-vk.ini [Native] Device=on): DeviceState, render_t's drawing / transforms / lights /
// vertex buffer creation, VertexBuffer_c. Same objects, same D3D calls as the original (docs/native.md); the
// difference is that it's ours, and the next step can talk to rvk without the D3D7 interfaces.
//
// render_t (0x28C bytes): +0x00 IDirect3DDevice7*, +0x04 IDirect3D7*.
// DeviceState: render states wanted +0x4C8 / applied +0x264 (one per D3DRENDERSTATETYPE), changed bits +0x72C (5
// dwords) and flag +0x740; texture stage states wanted +0xD84 / applied +0xA64 (0x19 per stage), changed bits per stage
// +0x10A4 + stage * 8 (a dword, then a flag byte), stages in use +0x10E4; textures (surface_t*) wanted +0x1128 /
// applied +0x1108, changed bits +0x1148, flag +0x114C. Priorities (never read): render states +0x000, texture stage
// states +0x744, textures +0x10E8. One DeviceState (0x1150 bytes) for everything: 0x10168FEC, made by Randy_t's
// constructor, read back from the device on every Flip and new viewport.
// VertexBuffer_c: one pointer to its VertexBufferImpl_c (0x1C bytes): the IDirect3DVertexBuffer7, FVF, flags (8 =
// write only), memory (2 = system memory), bytes, stride, vertex count.
#include "native/device.h"
#include "native/orig_api.gen.h"
#include "native/dynamic_vb.h"
#include "native/debugger.h"
#include "native/devices.h"
#include "native/dxerror.h"
#include "native/randy.h"
#include "native/randy_init.h"
#include "native/surface.h"
#include "native/resource.h"
#include "native/texture.h"
#include "native/material.h"
#include "native/state_blob.h"
#include "native/vc10.h"

#include <cstring>
#include <vector>

namespace rnative::device {

namespace {

HMODULE g_orig;
const Direct* g_direct;                             // the rvk backend's direct channel (SetDirect), or null
bool g_directOn;                                    // [Native] Direct = on: use it
bool g_retainOn;                                    // [Native] Retain = on: native meshes' indices kept (phase 3)
uint64_t g_retainGeneration;                        // RetainedIndices: the current draw's triangle list generation
const uint32_t* g_debuggerMode;                     // Debugger_t::m_nDebuggerMode: 0x100 draws nothing, 0x200 copies
void* const* g_render;                              // render_t::m_pcInstance

constexpr uint32_t kDebugNoDraw = 0x100, kDebugCopyVertices = 0x200;

template <typename T>
T& Field(void* object, uint32_t offset) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(object) + offset); }

// A COM call through the vtable (`offset` = slot * 4), as the original makes it.
template <typename... A>
HRESULT Com(void* object, uint32_t offset, A... args)
{
    using Fn = HRESULT(__stdcall*)(void*, A...);
    return reinterpret_cast<Fn>((*static_cast<void***>(object))[offset / 4])(object, args...);
}

void Failed(const char* what, HRESULT hr)             // the original throws (most) or prints; ours never fail
{
    static int logged;
    if (logged < 20) {
        ++logged;
        Log("%s: D3D call failed (%08lx)", what, (unsigned long)hr);
    }
}

bool NoDraw() { return (*g_debuggerMode & kDebugNoDraw) != 0; }
void* Device(void* render) { return Field<void*>(render, 0); }

// ---- VertexBuffer_c ----

struct FvfBit {
    uint32_t bit, size;
};
// Randy's FVF table (0x1008A410): positions (0x2..0xE, one of them counts), normal, point size, diffuse, specular,
// texture sets (0x100..0x800, one of them counts).
constexpr FvfBit kFvf[19] = {{0x2, 12},   {0x4, 16},   {0x6, 16},   {0x8, 20},   {0xA, 24},   {0xC, 28},   {0xE, 32},
                             {0x10, 12},  {0x40, 4},   {0x80, 4},   {0x0, 0},    {0x100, 8},  {0x200, 16}, {0x300, 24},
                             {0x400, 32}, {0x500, 40}, {0x600, 48}, {0x700, 56}, {0x800, 64}};

struct Impl {                                       // VertexBufferImpl_c
    void* vb;                                       // IDirect3DVertexBuffer7
    uint32_t fvf, flags, memory, bytes, stride, count;
};
static_assert(sizeof(Impl) == 0x1C, "VertexBufferImpl_c");

Impl* ImplOf(void* buffer) { return *static_cast<Impl**>(buffer); }

// FUN_100116f4: where `element` starts in a vertex of `fvf` (only the first of the position / texture groups counts).
uint32_t ElementOffset(uint32_t fvf, uint32_t element)
{
    uint32_t offset = 0;
    bool position = false, textures = false;
    for (const FvfBit& e : kFvf) {
        if (element == e.bit) return offset;
        if (!(fvf & e.bit)) continue;
        if (e.bit & 0xE) {
            if (position) continue;
            position = true;
        }
        if (e.bit & 0xF00) {
            if (textures) continue;
            textures = true;
        }
        offset += e.size;
    }
    return offset;
}

// FUN_10011745: the D3D vertex buffer for an Impl.
void Create(Impl* impl)
{
    struct {
        uint32_t size, caps, fvf, count;
    } desc{0x10, 0, impl->fvf, impl->count};
    if (impl->memory == 2) desc.caps = 0x800;        // D3DVBCAPS_SYSTEMMEMORY
    if (impl->flags & 8) desc.caps |= 0x10000;      // D3DVBCAPS_WRITEONLY
    orig::render_t_CreateVertexBuffer(*g_render, &desc, &impl->vb, 0);
}

void* LockImpl(Impl* impl, uint32_t flags)          // FUN_10011689
{
    void* data = nullptr;
    DWORD size = 0;
    Com(impl->vb, 0xC, DWORD(flags), &data, &size);
    return data;
}

void UnlockImpl(Impl* impl) { Com(impl->vb, 0x10); }

Impl* NewImpl() { return static_cast<Impl*>(vc10::Allocate(sizeof(Impl))); }

// A copy of `source` (its vertices too), with other flags / memory if asked (FUN_100117dd, FUN_10011853).
Impl* CopyImpl(const Impl* source, uint32_t flags, uint32_t memory)
{
    Impl* impl = NewImpl();
    *impl = *source;
    impl->vb = nullptr;
    impl->flags = flags;
    impl->memory = memory;
    Create(impl);
    const void* from = LockImpl(const_cast<Impl*>(source), 0x10);   // DDLOCK_READONLY
    void* to = LockImpl(impl, 0);
    std::memcpy(to, from, impl->bytes);
    UnlockImpl(const_cast<Impl*>(source));
    UnlockImpl(impl);
    return impl;
}

// FUN_100118c9: `source`'s vertices in another FVF - shared elements copied, missing ones zero (diffuse white).
Impl* ConvertImpl(const Impl* source, uint32_t fvf, uint32_t flags, uint32_t memory)
{
    Impl* impl = NewImpl();
    impl->vb = nullptr;
    impl->fvf = fvf;
    impl->flags = flags;
    impl->memory = memory;
    impl->stride = FormatSize(fvf);
    impl->count = source->count;
    impl->bytes = impl->stride * impl->count;
    Create(impl);
    struct Part {
        int32_t from;                               // -1: not in the source
        uint32_t to, size, bit;
    };
    Part parts[19];
    int partCount = 0;
    bool position = false, textures = false;
    for (const FvfBit& e : kFvf) {
        if (!(fvf & e.bit)) continue;
        if (e.bit & 0xE) {
            if (position) continue;
            position = true;
        }
        if (e.bit & 0xF00) {
            if (textures) continue;
            textures = true;
        }
        Part p{-1, ElementOffset(fvf, e.bit), FormatSize(e.bit), e.bit};
        if (source->fvf & e.bit) p.from = int32_t(ElementOffset(source->fvf, e.bit));
        parts[partCount++] = p;
    }
    const uint8_t* from = static_cast<const uint8_t*>(LockImpl(const_cast<Impl*>(source), 0x10));
    uint8_t* to = static_cast<uint8_t*>(LockImpl(impl, 0));
    for (uint32_t v = 0; v < impl->count; ++v)
        for (int i = 0; i < partCount; ++i) {
            const Part& p = parts[i];
            uint8_t* dst = to + v * impl->stride + p.to;
            if (p.from < 0)
                std::memset(dst, p.bit == 0x40 ? 0xFF : 0, p.size);
            else
                std::memcpy(dst, from + v * source->stride + uint32_t(p.from), p.size);
        }
    UnlockImpl(const_cast<Impl*>(source));
    UnlockImpl(impl);
    return impl;
}

void* __fastcall VbCreate(void** self, void*, uint32_t fvf, uint32_t flags, uint32_t memory, uint32_t bytes)
{
    Impl* impl = NewImpl();
    *impl = Impl{nullptr, fvf, flags, memory, bytes, FormatSize(fvf), 0};
    impl->count = bytes / impl->stride;
    Create(impl);
    *self = impl;
    return self;
}

void* __fastcall VbCopy(void** self, void*, void* const* source)
{
    const Impl* s = static_cast<const Impl*>(*source);
    *self = CopyImpl(s, s->flags, s->memory);
    return self;
}

void* __fastcall VbCopyAs(void** self, void*, void* const* source, uint32_t flags, uint32_t memory)
{
    *self = CopyImpl(static_cast<const Impl*>(*source), flags, memory);
    return self;
}

void* __fastcall VbConvert(void** self, void*, void* const* source, uint32_t fvf, uint32_t flags, uint32_t memory)
{
    *self = ConvertImpl(static_cast<const Impl*>(*source), fvf, flags, memory);
    return self;
}

void* __fastcall VbAssign(void** self, void*, void* const* other)
{
    *self = *other;
    return self;
}

void* __fastcall VbLock(void* self, void*, uint32_t, uint32_t flags) { return LockImpl(ImplOf(self), flags); }
void __fastcall VbUnlock(void* self) { UnlockImpl(ImplOf(self)); }
uint8_t* __fastcall VbNextVertex(void* self, void*, uint8_t* vertex) { return vertex + ImplOf(self)->stride; }
uint32_t __fastcall VbNumVertices(void* self) { return ImplOf(self)->count; }
uint32_t __fastcall VbStride(void* self) { return ImplOf(self)->stride; }
uint32_t __fastcall VbFormat(void* self) { return ImplOf(self)->fvf; }
uint32_t __fastcall VbSize(void* self) { return ImplOf(self)->bytes; }
uint32_t __fastcall VbElementOffset(void* self, void*, uint32_t element) { return ElementOffset(ImplOf(self)->fvf, element); }
uint32_t __cdecl VbFormatSize(uint32_t fvf) { return FormatSize(fvf); }

void __fastcall VbDestroy(void** self)
{
    if (Impl* impl = static_cast<Impl*>(*self)) {
        Com(impl->vb, 0x8);                         // Release
        impl->vb = nullptr;
        vc10::Free(impl);
    }
    *self = nullptr;
}

void __fastcall VbRelease(void** self)
{
    if (self) {
        VbDestroy(self);
        vc10::Free(self);
    }
}

// ---- render_t: drawing ----

// The D3D primitive types and IDirect3DDevice7 slots.
enum : uint32_t { kPoints = 1, kLines = 2, kTriangles = 4, kStrip = 5, kFan = 6 };
constexpr uint32_t kDrawPrimitive = 0x64, kDrawIndexedPrimitive = 0x68, kDrawPrimitiveVB = 0x7C,
                   kDrawIndexedPrimitiveVB = 0x80;

void* D3dVb(void* buffer) { return ImplOf(buffer)->vb; }

template <uint32_t Type>
void __fastcall DrawIndexedVB(void* render, void*, void* buffer, uint32_t start, uint32_t vertices, uint16_t* indices,
                              uint32_t indexCount, uint32_t)
{
    if (NoDraw() || !Device(render)) return;
    // A native mesh's triangles (RetainedIndices): through the direct channel, which can keep them.
    if (g_retainGeneration && g_direct && g_direct->drawIndexedVB &&
        g_direct->drawIndexedVB(Device(render), Type, D3dVb(buffer), start, vertices, indices, indexCount,
                                g_retainGeneration))
        return;
    HRESULT hr = Com(Device(render), kDrawIndexedPrimitiveVB, DWORD(Type), D3dVb(buffer), DWORD(start), DWORD(vertices),
                     indices, DWORD(indexCount), DWORD(0));
    if (hr) Failed("render_t indexed draw from a vertex buffer", hr);
}

template <uint32_t Type>
void __fastcall DrawVB(void* render, void*, void* buffer, uint32_t start, uint32_t count, uint32_t)
{
    if (NoDraw() || !Device(render)) return;
    HRESULT hr = Com(Device(render), kDrawPrimitiveVB, DWORD(Type), D3dVb(buffer), DWORD(start), DWORD(count), DWORD(0));
    if (hr) Failed("render_t draw from a vertex buffer", hr);
}

// Debugger mode 0x200: user vertices go through the dynamic vertex buffer instead.
void* CopyToDynamic(uint32_t fvf, const void* vertices, uint32_t count, uint32_t* start)
{
    void* dynamic = orig::DynamicVB_c_Get();
    void* to = nullptr;
    *start = orig::DynamicVB_c_GetVertices(dynamic, fvf, FormatSize(fvf), count, &to);
    std::memcpy(to, vertices, size_t(FormatSize(fvf)) * count);
    return orig::DynamicVB_c_GetVB(dynamic, fvf);
}

template <uint32_t Type>
void __fastcall DrawIndexedUP(void* render, void*, uint32_t fvf, void* vertices, uint32_t count, uint16_t* indices,
                              uint32_t indexCount, uint32_t flags)
{
    if (NoDraw() || !Device(render)) return;
    if (*g_debuggerMode & kDebugCopyVertices) {
        uint32_t start;
        void* vb = CopyToDynamic(fvf, vertices, count, &start);
        DrawIndexedVB<Type>(render, nullptr, vb, start, count, indices, indexCount, flags);
        return;
    }
    HRESULT hr = Com(Device(render), kDrawIndexedPrimitive, DWORD(Type), DWORD(fvf), vertices, DWORD(count), indices,
                     DWORD(indexCount), DWORD(0));
    if (hr) Failed("render_t indexed draw", hr);
}

template <uint32_t Type>
void __fastcall DrawUP(void* render, void*, uint32_t fvf, void* vertices, uint32_t count, uint32_t flags)
{
    if (NoDraw() || !Device(render)) return;
    if (*g_debuggerMode & kDebugCopyVertices) {
        uint32_t start;
        void* vb = CopyToDynamic(fvf, vertices, count, &start);
        DrawVB<Type>(render, nullptr, vb, start, count, flags);
        return;
    }
    HRESULT hr = Com(Device(render), kDrawPrimitive, DWORD(Type), DWORD(fvf), vertices, DWORD(count), DWORD(0));
    if (hr) Failed("render_t draw", hr);
}

// ---- render_t: state, transforms, lights, buffers ----

void __fastcall SetTransformMatrix(void* render, void*, uint32_t type, void* matrix)
{
    if (NoDraw() || !Device(render)) return;
    if (HRESULT hr = Com(Device(render), 0x2C, DWORD(type), matrix)) Failed("render_t::SetTransformMatrix", hr);
}

void __fastcall SetLight(void* render, void*, uint32_t index, void* light)
{
    if (NoDraw() || !Device(render)) return;
    if (HRESULT hr = Com(Device(render), 0x48, DWORD(index), light)) Failed("render_t::SetLight", hr);
}

void __fastcall LightEnable(void* render, void*, uint32_t index, uint32_t enable)   // FUN_100218be
{
    if (NoDraw() || !Device(render)) return;
    if (HRESULT hr = Com(Device(render), 0xB0, DWORD(index), BOOL(enable))) Failed("render_t::LightEnable", hr);
}

void __fastcall GetViewport(void* render, void*, void* viewport)
{
    if (!Device(render)) return;
    if (HRESULT hr = Com(Device(render), 0x3C, viewport)) Failed("render_t::GetViewport", hr);
}

void __fastcall CreateVertexBuffer(void* render, void*, void* desc, void** out, uint32_t flags)
{
    void* d3d = Field<void*>(render, 4);
    if (!d3d) return;
    HRESULT hr = Com(d3d, 0x14, desc, out, DWORD(flags));
    if (hr && hr != HRESULT(0x887601AE)) Failed("render_t::CreateVertexBuffer", hr);   // DDERR_SURFACEBUSY passes
}

void __fastcall ProcessVertices(void* render, void*, void* destination, uint32_t op, uint32_t destIndex, uint32_t count,
                                void* source, uint32_t sourceIndex, uint32_t flags)
{
    if (NoDraw() || !Device(render)) return;
    HRESULT hr = Com(D3dVb(destination), 0x14, DWORD(op), DWORD(destIndex), DWORD(count), D3dVb(source),
                     DWORD(sourceIndex), Device(render), DWORD(flags));
    if (hr) Failed("render_t::ProcessVertices", hr);
}

// ---- DeviceState ----

constexpr uint32_t kRsWanted = 0x4C8, kRsApplied = 0x264, kRsChanged = 0x72C, kRsFlag = 0x740;
constexpr uint32_t kTssWanted = 0xD84, kTssApplied = 0xA64, kTssChanged = 0x10A4, kStages = 0x10E4;
constexpr uint32_t kTexWanted = 0x1128, kTexApplied = 0x1108, kTexChanged = 0x1148, kTexFlag = 0x114C;
constexpr uint32_t kRsPriority = 0x000, kTssPriority = 0x744, kTexPriority = 0x10E8;

void Mark(uint32_t& bits, uint32_t bit, bool changed)
{
    if (changed) bits |= 1u << (bit & 31);
    else bits &= ~(1u << (bit & 31));
}

bool __fastcall SetRenderState(uint8_t* ds, void*, uint32_t state, uint32_t value, int32_t)
{
    uint32_t& wanted = Field<uint32_t>(ds, kRsWanted + state * 4);
    if (wanted != value) {
        wanted = value;
        ds[kRsFlag] = 1;
        Mark(Field<uint32_t>(ds, kRsChanged + (state >> 5) * 4), state,
             Field<uint32_t>(ds, kRsApplied + state * 4) != value);
    }
    return true;
}

bool __fastcall SetTextureStageState(uint8_t* ds, void*, uint32_t stage, uint32_t type, uint32_t value, int32_t)
{
    uint32_t& stages = Field<uint32_t>(ds, kStages);
    if (stages <= stage) stages = stage + 1;
    const uint32_t i = stage * 0x19 + type;
    uint32_t& wanted = Field<uint32_t>(ds, kTssWanted + i * 4);
    if (wanted != value) {
        const uint32_t applied = Field<uint32_t>(ds, kTssApplied + i * 4);
        wanted = value;
        ds[kTssChanged + stage * 8 + 4] = 1;
        Mark(Field<uint32_t>(ds, kTssChanged + stage * 8 + (type >> 5) * 4), type, applied != value);
    }
    return true;
}

bool __fastcall SetTexture(uint8_t* ds, void*, void* surface, uint32_t stage, int32_t)
{
    uint32_t& stages = Field<uint32_t>(ds, kStages);
    if (stages <= stage) stages = stage + 1;
    void*& wanted = Field<void*>(ds, kTexWanted + stage * 4);
    if (wanted == surface) return false;
    if (surface) orig::surface_t_AddRefDXSurface(surface);
    if (wanted) orig::surface_t_ReleaseDXSurface(wanted);
    wanted = surface;
    ds[kTexFlag] = 1;
    const bool changed = Field<void*>(ds, kTexApplied + stage * 4) != surface;
    Mark(Field<uint32_t>(ds, kTexChanged + (stage >> 5) * 4), stage, changed);
    return changed;
}

// The changed bits (`words` dwords) as indices, cleared (FUN_1001c1c1 / FUN_1001c232).
template <typename Fn>
void TakeChanged(uint32_t* bits, int words, Fn&& fn)
{
    uint32_t list[160];
    int n = 0;
    for (int w = 0; w < words; ++w) {
        uint32_t b = bits[w];
        if (!b) continue;
        for (int i = 0; i < 32; ++i)
            if (b & (1u << i)) list[n++] = uint32_t(w * 32 + i);
        bits[w] = 0;
    }
    for (int i = 0; i < n; ++i) fn(list[i]);
}

// The render states changed since the last update, to the device (FUN_1001ba52) - or, with `batch`, into it (the
// direct channel makes the calls later, in this order).
void FlushRenderStates(uint8_t* ds, void* device, bool apply, std::vector<StateChange>* batch = nullptr)
{
    if (ds[kRsFlag]) {
        TakeChanged(&Field<uint32_t>(ds, kRsChanged), 5, [&](uint32_t s) {
            const uint32_t v = Field<uint32_t>(ds, kRsWanted + s * 4);
            if (batch)
                batch->push_back({StateChange::RenderState, 0, s, v, nullptr});
            else if (apply)
                if (HRESULT hr = Com(device, 0x50, DWORD(s), DWORD(v))) Failed("render_t::SetRenderState", hr);
            Field<uint32_t>(ds, kRsApplied + s * 4) = v;
        });
        ds[kRsFlag] = 0;
    }
}

// The changes go to the device as one call through the direct channel when the backend is rvk (docs/device-on-rvk.md
// phase 1); otherwise as the original makes them, one D3D call each.
void __fastcall UpdateDevice(uint8_t* ds)
{
    void* render = *g_render;
    void* device = Device(render);
    const bool apply = !NoDraw() && device;
    static std::vector<StateChange> changes;        // the game thread's (the DeviceState is one, used from there)
    std::vector<StateChange>* batch = apply && g_directOn && g_direct ? &changes : nullptr;
    changes.clear();
    FlushRenderStates(ds, device, apply, batch);
    for (uint32_t stage = 0; stage < Field<uint32_t>(ds, kStages); ++stage) {
        uint8_t* changed = ds + kTssChanged + stage * 8;
        if (!changed[4]) continue;
        TakeChanged(reinterpret_cast<uint32_t*>(changed), 1, [&](uint32_t t) {
            const uint32_t i = stage * 0x19 + t;
            const uint32_t v = Field<uint32_t>(ds, kTssWanted + i * 4);
            if (batch)
                batch->push_back({StateChange::StageState, stage, t, v, nullptr});
            else if (apply)
                if (HRESULT hr = Com(device, 0x94, DWORD(stage), DWORD(t), DWORD(v)))
                    Failed("render_t::SetTextureStageState", hr);
            Field<uint32_t>(ds, kTssApplied + i * 4) = v;
        });
        changed[4] = 0;
    }
    if (ds[kTexFlag]) {
        TakeChanged(&Field<uint32_t>(ds, kTexChanged), 1, [&](uint32_t stage) {
            void* surface = Field<void*>(ds, kTexWanted + stage * 4);
            void* d3dSurface = surface ? *static_cast<void**>(surface) : nullptr;
            if (batch)
                batch->push_back({StateChange::Texture, stage, 0, 0, d3dSurface});
            else if (apply)
                if (HRESULT hr = Com(device, 0x8C, DWORD(stage), d3dSurface)) Failed("render_t::SetTexture", hr);
            Field<void*>(ds, kTexApplied + stage * 4) = surface;
        });
        ds[kTexFlag] = 0;
    }
    if (!batch || changes.empty())
        return;
    if (g_direct->applyStates(device, changes.data(), uint32_t(changes.size())))
        return;
    for (const StateChange& c : changes) {          // not the backend's device: one D3D call each, as above
        HRESULT hr = c.kind == StateChange::RenderState ? Com(device, 0x50, DWORD(c.type), DWORD(c.value))
                     : c.kind == StateChange::StageState
                         ? Com(device, 0x94, DWORD(c.stage), DWORD(c.type), DWORD(c.value))
                         : Com(device, 0x8C, DWORD(c.stage), c.surface);
        if (hr) Failed("render_t: a state change", hr);
    }
}


// Render states D3D7 knows (FUN_1001b9c4): the others are neither asked for nor kept.
bool KnownRenderState(uint32_t s)
{
    if (s >= 0x21 && s <= 0x26) return true;
    if (s <= 0x26) {                                // 2..0x1E, as its byte table has them
        static const uint32_t known = 0x7FD9C794;   // bit s: 2 4 7-10 14-16 19 20 22-30
        return s < 31 && (known >> s & 1);
    }
    if (s <= 0x3C) return (s >= 0x34) || s == 0x28 || s == 0x29 || s == 0x2F || s == 0x30;
    return (s >= 0x80 && s <= 0x94) || s == 0x97 || s == 0x98;
}

// FUN_1001bd36: the pending render states to the device, then everything as the device has it (texture stage
// states 1..24 of 8 stages); wanted = applied, priorities 0, no textures (not released: the wanted ones are forgotten).
void __fastcall Capture(uint8_t* ds)
{
    void* device = Device(*g_render);
    const bool ask = !NoDraw() && device;
    FlushRenderStates(ds, device, ask);
    for (uint32_t s = 0; s < 0x99; ++s) {
        uint32_t& applied = Field<uint32_t>(ds, kRsApplied + s * 4);
        if (KnownRenderState(s)) {
            if (ask)
                if (HRESULT hr = Com(device, 0x54, DWORD(s), &applied)) Failed("render_t::GetRenderState", hr);
            Field<uint32_t>(ds, kRsWanted + s * 4) = applied;
        } else {
            applied = 0;
            Field<uint32_t>(ds, kRsWanted + s * 4) = 0;
        }
        Field<uint32_t>(ds, kRsPriority + s * 4) = 0;
    }
    for (uint32_t stage = 0; stage < 8; ++stage) {
        for (uint32_t type = 1; type < 0x19; ++type) {
            const uint32_t i = stage * 0x19 + type;
            uint32_t& applied = Field<uint32_t>(ds, kTssApplied + i * 4);
            if (ask)
                if (HRESULT hr = Com(device, 0x90, DWORD(stage), DWORD(type), &applied))
                    Failed("render_t::GetTextureStageState", hr);
            Field<uint32_t>(ds, kTssPriority + i * 4) = 0;
            Field<uint32_t>(ds, kTssWanted + i * 4) = applied;
        }
        Field<void*>(ds, kTexApplied + stage * 4) = nullptr;
        Field<void*>(ds, kTexWanted + stage * 4) = nullptr;
        Field<uint32_t>(ds, kTexPriority + stage * 4) = 0;
    }
}

uint8_t*& Instance() { return Field<uint8_t*>(g_orig, 0x168FEC); }

// FUN_1001bdf2: the DeviceState, made the first time (FUN_1001c408: nothing changed, then read back from the device).
uint8_t* __cdecl GetDeviceState()
{
    uint8_t*& ds = Instance();
    if (!ds) {
        ds = static_cast<uint8_t*>(vc10::Allocate(0x1150));
        std::memset(ds + kRsChanged, 0, 5 * 4);
        ds[kRsFlag] = 0;
        for (uint32_t stage = 0; stage < 8; ++stage) {
            Field<uint32_t>(ds, kTssChanged + stage * 8) = 0;
            ds[kTssChanged + stage * 8 + 4] = 0;
        }
        Field<uint32_t>(ds, kStages) = 0;
        Field<uint32_t>(ds, kTexChanged) = 0;
        ds[kTexFlag] = 0;
        Capture(ds);
    }
    return ds;
}

// FUN_1001bd14 (Randy_t's destructor): the DeviceState gone, its wanted textures released (FUN_1001b9a1).
void __cdecl ShutdownDeviceState()
{
    uint8_t* ds = Instance();
    if (!ds) return;
    for (uint32_t stage = 0; stage < 8; ++stage) {
        void*& surface = Field<void*>(ds, kTexWanted + stage * 4);
        if (surface) {
            orig::surface_t_ReleaseDXSurface(surface);
            surface = nullptr;
        }
    }
    vc10::Free(ds);
    Instance() = nullptr;
}

// RenderStats_t (FUN_10025e1b / FUN_10025e22 / FUN_10025e2a): nothing to reset, and "Unknown" for both names.
void __fastcall StatsReset(void*, void*) {}
const char* __fastcall StatsGroupName(void*, void*, uint32_t) { return "Unknown"; }
const char* __fastcall StatsNameName(void*, void*, uint32_t) { return "Unknown"; }

}  // namespace

void SetDirect(const Direct* direct) { g_direct = direct; }

bool BlobShadowsReplaced() { return g_directOn && g_direct && g_direct->blobShadowsReplaced && g_direct->blobShadowsReplaced(); }

RetainedIndices::RetainedIndices(uint64_t generation) : m_previous(g_retainGeneration)
{
    g_retainGeneration = g_retainOn && g_directOn ? generation : 0;
}

RetainedIndices::~RetainedIndices() { g_retainGeneration = m_previous; }

namespace {
int g_timerDepth;                                   // GameTimer nesting (the game thread's)
double g_qpcMs;                                     // milliseconds per QueryPerformanceCounter tick
}

GameTimer::GameTimer(const char* name) : m_name(name)
{
    if (g_timerDepth++ || !g_direct || !g_direct->gameSection)
        return;
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    m_start = t.QuadPart;
}

GameTimer::~GameTimer()
{
    --g_timerDepth;
    if (!m_start || !g_direct || !g_direct->gameSection)
        return;
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    if (g_qpcMs == 0.0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_qpcMs = 1000.0 / double(f.QuadPart);
    }
    g_direct->gameSection(m_name, double(t.QuadPart - m_start) * g_qpcMs);
}

uint32_t FormatSize(uint32_t fvf)                   // FUN_100116c7
{
    uint32_t size = 0;
    for (const FvfBit& e : kFvf)
        if (fvf & e.bit) {
            size += e.size;
            fvf &= ~e.bit;
        }
    return size;
}

void Install(HMODULE orig)
{
    if (GetMode("Device", Mode::Off) != Mode::On)
        return;
    g_orig = orig;
    g_directOn = GetMode("Direct", Mode::On) == Mode::On;    // docs/device-on-rvk.md (call log identical: default on)
    g_retainOn = GetMode("Retain", Mode::Off) == Mode::On;    // phase 3: new, off until tested in game
    g_debuggerMode = reinterpret_cast<const uint32_t*>(GetProcAddress(orig, "?m_nDebuggerMode@Debugger_t@@2IA"));
    g_render = reinterpret_cast<void* const*>(GetProcAddress(orig, "?m_pcInstance@render_t@@0PAV1@A"));
    if (!g_debuggerMode || !g_render || !KnownBuild(orig)) {
        Log("device layer: not replaced (unknown client build)");
        return;
    }
    struct Entry {
        uint32_t rva;
        void* target;
        const char* what;
    };
#define FN(f) reinterpret_cast<void*>(&f)
    const Entry entries[] = {
        {0x1BBAA, FN(UpdateDevice), "DeviceState::UpdateDevice"},
        {0x1BBC1, FN(SetRenderState), "DeviceState::SetRenderState"},
        {0x1BC15, FN(SetTextureStageState), "DeviceState::SetTextureStageState"},
        {0x1BC8E, FN(SetTexture), "DeviceState::SetTexture"},
        {0x1BD36, FN(Capture), "DeviceState: read back from the device (FUN_1001bd36)"},
        {0x1BDF2, FN(GetDeviceState), "DeviceState: the one instance (FUN_1001bdf2)"},
        {0x1BD14, FN(ShutdownDeviceState), "DeviceState: shut down (FUN_1001bd14)"},
        {0x21A88, FN(DrawIndexedVB<kTriangles>), "render_t::RenderTriangleList (vertex buffer, indexed)"},
        {0x21AAA, FN(DrawIndexedVB<kLines>), "render_t::RenderLineList (vertex buffer, indexed)"},
        {0x21CE3, FN(DrawIndexedVB<kFan>), "render_t::RenderTriangleFan (vertex buffer, indexed)"},
        {0x21E1F, FN(DrawIndexedVB<kStrip>), "render_t::RenderTriangleStrip (vertex buffer, indexed)"},
        {0x2256E, FN(DrawVB<kTriangles>), "render_t::RenderTriangleList (vertex buffer)"},
        {0x227B6, FN(DrawVB<kPoints>), "render_t::RenderPointList (vertex buffer)"},
        {0x2325A, FN(DrawVB<kStrip>), "render_t::RenderTriangleStrip (vertex buffer)"},
        {0x21E41, FN(DrawIndexedUP<kTriangles>), "render_t::RenderTriangleList (indexed)"},
        {0x21FC8, FN(DrawIndexedUP<kLines>), "render_t::RenderLineList (indexed)"},
        {0x2214F, FN(DrawIndexedUP<kFan>), "render_t::RenderTriangleFan (indexed)"},
        {0x22AF3, FN(DrawUP<kTriangles>), "render_t::RenderTriangleList"},
        {0x22C6E, FN(DrawUP<kLines>), "render_t::RenderLineList"},
        {0x230DF, FN(DrawUP<kFan>), "render_t::RenderTriangleFan"},
        {0x23276, FN(DrawUP<kStrip>), "render_t::RenderTriangleStrip"},
        {0x2382A, FN(SetTransformMatrix), "render_t::SetTransformMatrix"},
        {0x21815, FN(SetLight), "render_t::SetLight"},
        {0x218BE, FN(LightEnable), "render_t::LightEnable (FUN_100218be)"},
        {0x234D8, FN(GetViewport), "render_t::GetViewport"},
        {0x24A77, FN(CreateVertexBuffer), "render_t::CreateVertexBuffer"},
        {0x24B25, FN(ProcessVertices), "render_t::ProcessVertices"},
        {0x114A1, FN(VbAssign), "VertexBuffer_c::operator="},
        {0x114B1, FN(VbCreate), "VertexBuffer_c::VertexBuffer_c(fvf, flags, memory, bytes)"},
        {0x114F9, FN(VbCopy), "VertexBuffer_c::VertexBuffer_c(copy)"},
        {0x1153A, FN(VbCopyAs), "VertexBuffer_c::VertexBuffer_c(copy, flags, memory)"},
        {0x11581, FN(VbConvert), "VertexBuffer_c::VertexBuffer_c(copy, fvf, flags, memory)"},
        {0x115CB, FN(VbLock), "VertexBuffer_c::Lock"},
        {0x115D6, FN(VbUnlock), "VertexBuffer_c::Unlock"},
        {0x115DD, FN(VbNextVertex), "VertexBuffer_c::NextVertex"},
        {0x115E8, FN(VbNumVertices), "VertexBuffer_c::GetNumVertices"},
        {0x115EE, FN(VbStride), "VertexBuffer_c::GetStride"},
        {0x115F4, FN(VbElementOffset), "VertexBuffer_c::GetElementOffset"},
        {0x115FF, FN(VbFormat), "VertexBuffer_c::GetFormat"},
        {0x11605, FN(VbSize), "VertexBuffer_c::GetSize"},
        {0x1160B, FN(VbFormatSize), "VertexBuffer_c::GetFormatSize"},
        {0x11614, FN(VbDestroy), "VertexBuffer_c::~VertexBuffer_c"},
        {0x11632, FN(VbRelease), "VertexBuffer_c::Release"},
        {0x25E1B, FN(StatsReset), "RenderStats_t::Reset"},
        {0x25E22, FN(StatsGroupName), "RenderStats_t::GetGroupName"},
        {0x25E2A, FN(StatsNameName), "RenderStats_t::GetNameName"},
    };
#undef FN
    int installed = 0;
    for (const Entry& e : entries) installed += Replace(orig, e.rva, e.target, e.what) ? 1 : 0;
    Log("device layer: %d of %d functions native%s", installed, int(sizeof(entries) / sizeof(entries[0])),
        g_directOn ? (g_retainOn ? "; state updates and static meshes' indices through the direct channel"
                                 : "; state updates through the direct channel when the backend is rvk")
                   : "");
    stateblob::Install(orig);
    dynamicvb::Install(orig);
    randy::Install(orig);
    surface::Install(orig);
    resource::Install(orig);
    texture::Install(orig);
    material::Install(orig);
    dxerror::Install(orig);
    devices::Install(orig);
    randyinit::Install(orig);
    debugger::Install(orig);
}

}  // namespace rnative::device
