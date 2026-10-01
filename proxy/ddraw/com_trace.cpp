#define INITGUID                                // define the DirectDraw / Direct3D IIDs in this unit
#include "com_trace.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace rvkproxy {
namespace {

std::atomic<uint32_t>* g_counts;                 // allocated once kComMethodCount is known
std::mutex g_mutex;
std::unordered_map<void*, void*> g_realToWrapper;
std::unordered_set<const void*> g_wrappers;
bool g_active;

void LogLine(const char* fmt, ...)
{
    char path[MAX_PATH] = "randy-vk.log";
    GetEnvironmentVariableA("RANDYVK_LOG", path, sizeof(path));
    if (FILE* f = std::fopen(path, "a")) {
        va_list args;
        va_start(args, fmt);
        std::vfprintf(f, fmt, args);
        va_end(args);
        std::fputc('\n', f);
        std::fclose(f);
    }
}

}  // namespace

void CountComCall(unsigned index) { g_counts[index].fetch_add(1, std::memory_order_relaxed); }

void* LookupWrapper(void* real)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_realToWrapper.find(real);
    return it == g_realToWrapper.end() ? nullptr : it->second;
}

void RegisterWrapper(void* real, void* wrapper)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_realToWrapper[real] = wrapper;
    g_wrappers.insert(wrapper);
}

void UnregisterWrapper(void* real, void* wrapper)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_realToWrapper.find(real);
    if (it != g_realToWrapper.end() && it->second == wrapper)
        g_realToWrapper.erase(it);
    g_wrappers.erase(wrapper);
}

bool IsWrapper(const void* p)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_wrappers.count(p) != 0;
}

}  // namespace rvkproxy

namespace rvkproxy {
#define RVK_COM_TRACE_NAMES
#include "com_trace.gen.h"

namespace {

unsigned IndexOf(const char* name)
{
    for (unsigned i = 0; i < kComMethodCount; ++i)
        if (!std::strcmp(kComMethodNames[i], name))
            return i;
    return 0;
}

// Interface pointer for a wrapper of a known IID, or nullptr if the IID isn't one we wrap.
void* WrapByIid(REFIID iid, void* real)
{
    if (iid == IID_IDirectDraw7) return Wrap<TraceIDirectDraw7>(static_cast<IDirectDraw7*>(real));
    if (iid == IID_IDirectDrawSurface7) return Wrap<TraceIDirectDrawSurface7>(static_cast<IDirectDrawSurface7*>(real));
    if (iid == IID_IDirectDrawClipper) return Wrap<TraceIDirectDrawClipper>(static_cast<IDirectDrawClipper*>(real));
    if (iid == IID_IDirectDrawPalette) return Wrap<TraceIDirectDrawPalette>(static_cast<IDirectDrawPalette*>(real));
    if (iid == IID_IDirect3D7) return Wrap<TraceIDirect3D7>(static_cast<IDirect3D7*>(real));
    if (iid == IID_IDirect3DDevice7) return Wrap<TraceIDirect3DDevice7>(static_cast<IDirect3DDevice7*>(real));
    if (iid == IID_IDirect3DVertexBuffer7) return Wrap<TraceIDirect3DVertexBuffer7>(static_cast<IDirect3DVertexBuffer7*>(real));
    return nullptr;
}

template <typename W>
HRESULT TraceQueryInterface(W* self, REFIID iid, void** out)
{
    static const unsigned index = IndexOf((std::string(W::kName) + "::QueryInterface").c_str());
    CountComCall(index);
    if (!out)
        return E_POINTER;
    HRESULT r = self->Real()->QueryInterface(iid, out);
    if (FAILED(r) || !*out)
        return r;
    if (iid == IID_IUnknown) {                   // keep identity: IUnknown of a wrapper is the wrapper
        static_cast<IUnknown*>(*out)->Release();
        self->AddRef();
        *out = static_cast<IUnknown*>(self);
        return r;
    }
    if (void* w = WrapByIid(iid, *out)) {
        *out = w;
        return r;
    }
    LogLine("randy-vk ddraw trace: %s::QueryInterface for an unwrapped interface "
            "{%08lx-%04x-%04x-...} (passed through)", W::kName, iid.Data1, iid.Data2, iid.Data3);
    return r;
}

template <typename W>
ULONG TraceAddRef(W* self)
{
    static const unsigned index = IndexOf((std::string(W::kName) + "::AddRef").c_str());
    CountComCall(index);
    return self->Real()->AddRef();
}

template <typename W>
ULONG TraceRelease(W* self)
{
    static const unsigned index = IndexOf((std::string(W::kName) + "::Release").c_str());
    CountComCall(index);
    auto* real = self->Real();
    ULONG r = real->Release();
    if (r == 0) {
        UnregisterWrapper(real, self);
        delete self;
    }
    return r;
}

// Enumeration callbacks hand out surfaces: wrap them before the caller sees them.
struct EnumContext {
    LPDDENUMSURFACESCALLBACK7 callback;
    void* context;
};

HRESULT WINAPI EnumTrampoline(LPDIRECTDRAWSURFACE7 surface, LPDDSURFACEDESC2 desc, LPVOID ctx)
{
    auto* c = static_cast<EnumContext*>(ctx);
    return c->callback(Wrap<TraceIDirectDrawSurface7>(surface), desc, c->context);
}

}  // namespace

#define RVK_TRACE_IUNKNOWN(Cls)                                                                     \
    HRESULT Cls::QueryInterface(REFIID iid, LPVOID* out) { return TraceQueryInterface(this, iid, out); } \
    ULONG Cls::AddRef() { return TraceAddRef(this); }                                                 \
    ULONG Cls::Release() { return TraceRelease(this); }
RVK_TRACE_IUNKNOWN(TraceIDirectDraw7)
RVK_TRACE_IUNKNOWN(TraceIDirectDrawSurface7)
RVK_TRACE_IUNKNOWN(TraceIDirectDrawClipper)
RVK_TRACE_IUNKNOWN(TraceIDirectDrawPalette)
RVK_TRACE_IUNKNOWN(TraceIDirect3D7)
RVK_TRACE_IUNKNOWN(TraceIDirect3DDevice7)
RVK_TRACE_IUNKNOWN(TraceIDirect3DVertexBuffer7)
#undef RVK_TRACE_IUNKNOWN

HRESULT TraceIDirectDraw7::EnumSurfaces(DWORD flags, LPDDSURFACEDESC2 desc, LPVOID ctx, LPDDENUMSURFACESCALLBACK7 cb)
{
    static const unsigned index = IndexOf("IDirectDraw7::EnumSurfaces");
    CountComCall(index);
    EnumContext c{cb, ctx};
    return m_real->EnumSurfaces(flags, desc, &c, cb ? EnumTrampoline : nullptr);
}

HRESULT TraceIDirectDrawSurface7::EnumAttachedSurfaces(LPVOID ctx, LPDDENUMSURFACESCALLBACK7 cb)
{
    static const unsigned index = IndexOf("IDirectDrawSurface7::EnumAttachedSurfaces");
    CountComCall(index);
    EnumContext c{cb, ctx};
    return m_real->EnumAttachedSurfaces(&c, cb ? EnumTrampoline : nullptr);
}

HRESULT TraceIDirectDrawSurface7::EnumOverlayZOrders(DWORD flags, LPVOID ctx, LPDDENUMSURFACESCALLBACK7 cb)
{
    static const unsigned index = IndexOf("IDirectDrawSurface7::EnumOverlayZOrders");
    CountComCall(index);
    EnumContext c{cb, ctx};
    return m_real->EnumOverlayZOrders(flags, &c, cb ? EnumTrampoline : nullptr);
}

HRESULT TraceIDirectDrawSurface7::GetDDInterface(LPVOID* out)
{
    static const unsigned index = IndexOf("IDirectDrawSurface7::GetDDInterface");
    CountComCall(index);
    HRESULT r = m_real->GetDDInterface(out);
    if (SUCCEEDED(r) && out && *out)              // documented to return the IDirectDraw7 the surface came from
        *out = Wrap<TraceIDirectDraw7>(static_cast<IDirectDraw7*>(*out));
    return r;
}

HRESULT TraceIDirectDrawSurface7::BltBatch(LPDDBLTBATCH, DWORD, DWORD)
{
    static const unsigned index = IndexOf("IDirectDrawSurface7::BltBatch");
    CountComCall(index);
    return DDERR_UNSUPPORTED;                     // never implemented by DirectDraw either
}

// ---------------------------------------------------------------------------------------------------
// Import hooks

namespace {

using DirectDrawCreateExFn = HRESULT(WINAPI*)(GUID*, LPVOID*, REFIID, IUnknown*);
using DirectDrawEnumerateExAFn = HRESULT(WINAPI*)(LPDDENUMCALLBACKEXA, LPVOID, DWORD);
DirectDrawCreateExFn g_realCreateEx;
DirectDrawEnumerateExAFn g_realEnumerateExA;
unsigned g_createExIndex, g_enumerateIndex;      // extra counters after the COM methods

HRESULT WINAPI HookDirectDrawCreateEx(GUID* guid, LPVOID* out, REFIID iid, IUnknown* outer)
{
    CountComCall(g_createExIndex);
    HRESULT r = g_realCreateEx(guid, out, iid, outer);
    if (SUCCEEDED(r) && out && *out && iid == IID_IDirectDraw7)
        *out = Wrap<TraceIDirectDraw7>(static_cast<IDirectDraw7*>(*out));
    return r;
}

HRESULT WINAPI HookDirectDrawEnumerateExA(LPDDENUMCALLBACKEXA cb, LPVOID ctx, DWORD flags)
{
    CountComCall(g_enumerateIndex);
    return g_realEnumerateExA(cb, ctx, flags);
}

// Points module's import of dll!function at hook; returns the previous target (or nullptr).
void* PatchImport(HMODULE module, const char* dll, const char* function, void* hook)
{
    auto* base = reinterpret_cast<BYTE*>(module);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp(reinterpret_cast<char*>(base + imp->Name), dll) != 0)
            continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
        auto* iat = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                continue;
            auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<char*>(byName->Name), function) != 0)
                continue;
            void* previous = reinterpret_cast<void*>(iat->u1.Function);
            DWORD protect;
            VirtualProtect(&iat->u1.Function, sizeof(void*), PAGE_READWRITE, &protect);
            iat->u1.Function = reinterpret_cast<ULONG_PTR>(hook);
            VirtualProtect(&iat->u1.Function, sizeof(void*), protect, &protect);
            return previous;
        }
    }
    return nullptr;
}

}  // namespace

unsigned ComMethodCount() { return kComMethodCount + 2; }

const char* ComMethodName(unsigned index)
{
    if (index < kComMethodCount) return kComMethodNames[index];
    return index == kComMethodCount ? "ddraw!DirectDrawCreateEx" : "ddraw!DirectDrawEnumerateExA";
}

uint32_t ComCallCount(unsigned index)
{
    return g_counts && index < ComMethodCount() ? g_counts[index].load(std::memory_order_relaxed) : 0;
}

bool ComTraceActive() { return g_active; }

bool InstallDDrawHooks(HMODULE randyOrig)
{
    char mode[32] = "";
    GetEnvironmentVariableA("RANDYVK_DDRAW", mode, sizeof(mode));
    if (_stricmp(mode, "trace") != 0)
        return false;
    g_counts = new std::atomic<uint32_t>[ComMethodCount()]();
    g_createExIndex = kComMethodCount;
    g_enumerateIndex = kComMethodCount + 1;
    g_realCreateEx = reinterpret_cast<DirectDrawCreateExFn>(
        PatchImport(randyOrig, "DDRAW.dll", "DirectDrawCreateEx", reinterpret_cast<void*>(HookDirectDrawCreateEx)));
    g_realEnumerateExA = reinterpret_cast<DirectDrawEnumerateExAFn>(
        PatchImport(randyOrig, "DDRAW.dll", "DirectDrawEnumerateExA", reinterpret_cast<void*>(HookDirectDrawEnumerateExA)));
    g_active = g_realCreateEx && g_realEnumerateExA;
    LogLine("randy-vk ddraw trace: hooks %s (DirectDrawCreateEx %p, DirectDrawEnumerateExA %p)",
            g_active ? "installed" : "FAILED", reinterpret_cast<void*>(g_realCreateEx),
            reinterpret_cast<void*>(g_realEnumerateExA));
    return g_active;
}

}  // namespace rvkproxy
