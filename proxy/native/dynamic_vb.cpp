// DynamicVB_c natively (part of [Native] Device=on): one ring of vertices per FVF for vertices written each frame
// (the UI, effects, debugger copies). Callers only see the object through its seven exports, so ours keeps its own
// table instead of the original's std::map (FUN_10014bc0 & co).
//
// Per FVF: a write-only system memory VertexBuffer_c of 4000 vertices (twice a bigger request, up to 0xFFFE). GetVertices hands out the next `count`
// vertices (locked; the ring starts over with a discard when full), GetVB / CloseBuffer unlock, Reset discards all.
#include "native/dynamic_vb.h"
#include "native/device.h"
#include "native/orig_api.gen.h"
#include "native/vc10.h"

#include <map>

namespace rnative::dynamicvb {

namespace {

struct Ring {                                       // VBHandler_c
    uint32_t fvf = 0, stride = 0, capacity = 0, used = 0;
    void* buffer = nullptr;                         // VertexBuffer_c*
    bool locked = false;
};

struct Dynamic {
    std::map<uint32_t, Ring> rings;
};

Dynamic* g_instance;                                // DynamicVB_c::Get's object (0x100B96A4 in the original)

constexpr uint32_t kMaxVertices = 0xFFFE;

Ring& RingFor(Dynamic* d, uint32_t fvf)
{
    Ring& r = d->rings[fvf];
    r.fvf = fvf;
    return r;
}

void Create(Ring& r, uint32_t count)                // FUN_10013faf
{
    const uint32_t n = count > kMaxVertices ? kMaxVertices : count;
    if (r.buffer) orig::VertexBuffer_c_Release(r.buffer);
    void* buffer = vc10::Allocate(4);
    orig::VertexBuffer_c_VertexBuffer_c_61(buffer, r.fvf, 8, 2, device::FormatSize(r.fvf) * n);   // write only, sysmem
    r.buffer = buffer;
    r.capacity = n;
}

void Discard(Ring& r)                               // FUN_10013f76
{
    if (r.buffer) {
        orig::VertexBuffer_c_Lock(r.buffer, 0, 0x2000);   // DDLOCK_DISCARDCONTENTS
        orig::VertexBuffer_c_Unlock(r.buffer);
    }
    r.used = 0;
}

void Close(Ring& r)                                 // FUN_10013f9a
{
    if (r.buffer) orig::VertexBuffer_c_Unlock(r.buffer);
    r.locked = false;
}

void __cdecl Initialize()
{
    if (!g_instance) g_instance = new Dynamic;
}

void* __cdecl Get()
{
    Initialize();
    return g_instance;
}

void __cdecl Shutdown()
{
    if (g_instance) {
        for (auto& [fvf, r] : g_instance->rings)
            if (r.buffer) orig::VertexBuffer_c_Release(r.buffer);
        delete g_instance;
    }
    g_instance = nullptr;
}

// Room for `count` vertices of `fvf` (`stride` bytes each): the first one's index; *out = where to write it.
uint32_t __fastcall GetVertices(Dynamic* d, void*, uint32_t fvf, uint32_t stride, uint32_t count, void** out)
{
    Ring& r = RingFor(d, fvf);
    r.stride = stride;
    if (!r.buffer) Create(r, 4000);
    if (r.capacity < count) {                       // too small: twice what's asked
        Discard(r);
        Create(r, count * 2);
    }
    if (r.capacity < r.used + count) Discard(r);
    const uint32_t start = r.used;
    r.used += count;
    uint8_t* data = static_cast<uint8_t*>(orig::VertexBuffer_c_Lock(r.buffer, 0, 0));
    if (orig::VertexBuffer_c_GetSize(r.buffer) < stride * count) {   // the original throws here
        static int logged;
        if (logged++ < 5) Log("DynamicVB_c::GetVertices: %u vertices of %u bytes don't fit", count, stride);
    }
    *out = data + stride * start;
    r.locked = true;
    return start;
}

void __fastcall CloseBuffer(Dynamic* d, void*, uint32_t fvf) { Close(RingFor(d, fvf)); }

void* __fastcall GetVB(Dynamic* d, void*, uint32_t fvf)
{
    Ring& r = RingFor(d, fvf);
    if (r.locked) Close(r);
    return r.buffer;
}

void __fastcall Reset(Dynamic* d)
{
    for (auto& [fvf, r] : d->rings) Discard(r);
}

// FUN_10014309: frees a vertex buffer's +4 field (a handler's own destructor path).
void __fastcall FreeBuffer(void* self, void*)
{
    vc10::Free(*reinterpret_cast<void**>(static_cast<uint8_t*>(self) + 4));
}

}  // namespace

void Install(HMODULE orig)
{
    struct Entry {
        uint32_t rva;
        void* target;
        const char* what;
    };
#define FN(f) reinterpret_cast<void*>(&f)
    const Entry entries[] = {
        {0x14033, FN(Reset), "DynamicVB_c::Reset"},
        {0x14177, FN(GetVertices), "DynamicVB_c::GetVertices"},
        {0x141AE, FN(CloseBuffer), "DynamicVB_c::CloseBuffer"},
        {0x141C5, FN(GetVB), "DynamicVB_c::GetVB"},
        {0x141FE, FN(Initialize), "DynamicVB_c::Initialize"},
        {0x14253, FN(Shutdown), "DynamicVB_c::Shutdown"},
        {0x14275, FN(Get), "DynamicVB_c::Get"},
        {0x14309, FN(FreeBuffer), "a vertex buffer freed (FUN_10014309)"},
    };
#undef FN
    for (const Entry& e : entries)                  // ours and the original's objects don't mix: all or none
        if (!Replaceable(orig, e.rva)) {
            Log("dynamic vertex buffers: %s can't be replaced - none are", e.what);
            return;
        }
    int installed = 0;
    for (const Entry& e : entries) installed += Replace(orig, e.rva, e.target, e.what) ? 1 : 0;
    Log("dynamic vertex buffers: %d of %d functions native", installed, int(sizeof(entries) / sizeof(entries[0])));
}

}  // namespace rnative::dynamicvb
