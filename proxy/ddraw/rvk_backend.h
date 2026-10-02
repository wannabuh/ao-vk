// DirectDraw 7 / Direct3D 7 implemented on rvk (RANDYVK_DDRAW=rvk). Only randy31_orig.dll sees these
// objects (its two DirectDraw imports are redirected); other modules keep the system DirectDraw.
#pragma once

#include "com_trace.h"
#include "rvk.h"
#include "threaded.h"

#include <atomic>
#include <map>
#include <string>
#include <vector>

namespace rvkproxy {

HRESULT StubCall(unsigned index);                 // logs an unimplemented method once
void CountBackendDraw();                          // heartbeat statistics
unsigned ComIndex(const char* name);              // counter index of "Interface::Method"
void RvkLog(const char* fmt, ...);
void ParticleFrame();                             // each presented frame (rvk_particles.cpp)

// rvk_materials.cpp: side-loaded material maps of RDB textures (normal maps). A surface learns its RDB identity
// from the hooked TextureStreamCreator::CreateTexture exports; QueryInterface(IID_RvkSurface) tells our surfaces
// apart from another backend's.
extern const GUID IID_RvkSurface;
class RSurface;
void AttachMaterialMaps(RSurface* top);           // after the surface's rvk texture is (re)created

// One lock around every call into the rvk backend: the game may use DirectDraw from more than one thread
// (D3D serialises internally too). Recursive, since methods call each other. Logs each new thread once.
class ComScope {
public:
    explicit ComScope(unsigned methodIndex);
    ~ComScope();
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
};

#include "com_stub.gen.h"

// IUnknown for rvk objects: reference counting plus QueryInterface through Cast().
template <typename B>
class Com : public B {
public:
    COM_DECLSPEC_NOTHROW HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) final
    {
        static const unsigned index = ComIndex((std::string(B::kName) + "::QueryInterface").c_str());
        CountComCall(index);
        ComScope scope(index);
        if (!out)
            return E_POINTER;
        *out = iid == IID_IUnknown ? static_cast<IUnknown*>(this) : Cast(iid);
        if (!*out) {
            RvkLog("%s::QueryInterface: no interface {%08lx-%04x-%04x}", B::kName, iid.Data1, iid.Data2, iid.Data3);
            return E_NOINTERFACE;
        }
        if (*out == static_cast<IUnknown*>(this) || iid == IID_IUnknown)
            AddRef();
        return S_OK;
    }
    COM_DECLSPEC_NOTHROW ULONG STDMETHODCALLTYPE AddRef() final
    {
        static const unsigned index = ComIndex((std::string(B::kName) + "::AddRef").c_str());
        CountComCall(index);
        return ++m_refs;
    }
    COM_DECLSPEC_NOTHROW ULONG STDMETHODCALLTYPE Release() final
    {
        static const unsigned index = ComIndex((std::string(B::kName) + "::Release").c_str());
        CountComCall(index);
        ComScope scope(index);
        ULONG r = --m_refs;
        if (r == 0)
            delete this;
        return r;
    }

protected:
    // Interface for iid. Return `this` (Com adds the reference) or another object with a reference added.
    virtual void* Cast(REFIID iid) = 0;
    std::atomic<ULONG> m_refs{1};
};

class RDirectDraw;
class RDirect3D;
class RDevice;
class RSurface;
class RClipper;
class RPalette;

// Process-wide state shared by all objects.
struct RvkState {
    rvk::ThreadedDevice* device = nullptr;      // rvk on its own thread (RANDYVK_THREADED=0: on the caller's)
    HWND window = nullptr;            // from SetCooperativeLevel or a clipper
    RSurface* mainSurface = nullptr;  // the back buffer the 3D device renders to (rvk main target)
    DWORD displayWidth = 0, displayHeight = 0;   // SetDisplayMode (0 = desktop)
    std::string gpuName = "Vulkan GPU";

    bool EnsureDevice(uint32_t width, uint32_t height);   // creates or resizes the rvk device
    void SetWindow(HWND hwnd);                             // remember / attach the presentation window
    void Frame();                                          // begins a frame if none is open
    void Present();                                        // ends (presents) the current frame
};
extern RvkState g_rvk;

// Pixel formats.
bool FormatFromPixelFormat(const DDPIXELFORMAT& pf, rvk::Format* format, bool* convert24, bool* palettized);
void PixelFormatFor(rvk::Format format, DDPIXELFORMAT* pf);
void FillDeviceDesc(D3DDEVICEDESC7* desc, REFCLSID device);

class RClipper final : public Com<BaseIDirectDrawClipper> {
public:
    HWND window = nullptr;

protected:
    void* Cast(REFIID iid) override { return iid == IID_IDirectDrawClipper ? this : nullptr; }
    HRESULT DoSetHWnd(DWORD, HWND hwnd) override;
    HRESULT DoGetHWnd(HWND* out) override;
    HRESULT DoIsClipListChanged(BOOL* changed) override;
    HRESULT DoSetClipList(LPRGNDATA, DWORD) override { return DD_OK; }
    HRESULT DoInitialize(LPDIRECTDRAW, DWORD) override { return DDERR_ALREADYINITIALIZED; }
};

class RPalette final : public Com<BaseIDirectDrawPalette> {
public:
    explicit RPalette(DWORD caps) : m_caps(caps) {}
    PALETTEENTRY entries[256] = {};

protected:
    void* Cast(REFIID iid) override { return iid == IID_IDirectDrawPalette ? this : nullptr; }
    HRESULT DoGetCaps(LPDWORD caps) override;
    HRESULT DoGetEntries(DWORD, DWORD start, DWORD count, LPPALETTEENTRY out) override;
    HRESULT DoSetEntries(DWORD, DWORD start, DWORD count, LPPALETTEENTRY in) override;
    HRESULT DoInitialize(LPDIRECTDRAW, DWORD, LPPALETTEENTRY) override { return DDERR_ALREADYINITIALIZED; }

private:
    DWORD m_caps;
};

class RSurface final : public Com<BaseIDirectDrawSurface7> {
public:
    enum class Kind { Primary, Main, RenderTarget, Depth, Texture, Plain };

    RSurface(Kind kind, const DDSURFACEDESC2& desc);
    ~RSurface() override;
    static HRESULT Create(RDirectDraw* owner, const DDSURFACEDESC2& requested, RSurface** out);
    RDirectDraw* owner = nullptr;                  // weak; GetDDInterface

    Kind kind;
    DDSURFACEDESC2 desc{};            // what GetSurfaceDesc reports (lpSurface/pitch filled by Lock)
    rvk::Format format = rvk::Format::X8R8G8B8;
    bool convert24 = false, palettized = false;    // CPU conversion to A8R8G8B8 before upload

    // Textures: one rvk texture for the whole mip chain, owned by level 0.
    rvk::Texture* texture = nullptr;
    uint32_t level = 0, levels = 1;
    RSurface* top = nullptr;                       // level 0 (this for level 0)
    RSurface* nextLevel = nullptr;                 // owned by level 0
    std::vector<uint8_t> shadow;                   // CPU copy of this level (textures, plain surfaces)
    bool dirty = false;                            // shadow newer than the GPU copy
    bool anyDirty = false;                         // level 0 only: some level of the chain is dirty

    uint32_t rdbType = 0, rdbId = 0;               // level 0: the RDB texture it holds (0 = unknown)
    rvk::Texture* materialsFor = nullptr;          // level 0: the rvk texture its material maps were attached to

    RSurface* depth = nullptr;                     // attached z-buffer
    RClipper* clipper = nullptr;
    RPalette* palette = nullptr;

    rvk::Texture* RvkTexture();                    // creates/uploads as needed; null if not a GPU surface
    void Upload();                                 // shadow -> GPU for this level (if a device exists)
    uint32_t Pitch() const;
    void EnsureShadow();

protected:
    void* Cast(REFIID iid) override { return iid == IID_IDirectDrawSurface7 || iid == IID_RvkSurface ? this : nullptr; }
    HRESULT DoAddAttachedSurface(LPDIRECTDRAWSURFACE7 s) override;
    HRESULT DoDeleteAttachedSurface(DWORD, LPDIRECTDRAWSURFACE7 s) override;
    HRESULT DoBlt(LPRECT dst, LPDIRECTDRAWSURFACE7 src, LPRECT srcRect, DWORD flags, LPDDBLTFX fx) override;
    HRESULT DoBltFast(DWORD x, DWORD y, LPDIRECTDRAWSURFACE7 src, LPRECT srcRect, DWORD flags) override;
    HRESULT DoFlip(LPDIRECTDRAWSURFACE7, DWORD) override;
    HRESULT DoGetAttachedSurface(LPDDSCAPS2 caps, LPDIRECTDRAWSURFACE7* out) override;
    HRESULT DoGetBltStatus(DWORD) override { return DD_OK; }
    HRESULT DoGetFlipStatus(DWORD) override { return DD_OK; }
    HRESULT DoGetCaps(LPDDSCAPS2 caps) override;
    HRESULT DoGetClipper(LPDIRECTDRAWCLIPPER* out) override;
    HRESULT DoGetColorKey(DWORD flags, LPDDCOLORKEY key) override;
    HRESULT DoGetPalette(LPDIRECTDRAWPALETTE* out) override;
    HRESULT DoGetPixelFormat(LPDDPIXELFORMAT pf) override;
    HRESULT DoGetSurfaceDesc(LPDDSURFACEDESC2 out) override;
    HRESULT DoIsLost() override { return DD_OK; }
    HRESULT DoLock(LPRECT rect, LPDDSURFACEDESC2 out, DWORD flags, HANDLE) override;
    HRESULT DoUnlock(LPRECT) override;
    HRESULT DoRestore() override { return DD_OK; }
    HRESULT DoSetClipper(LPDIRECTDRAWCLIPPER c) override;
    HRESULT DoSetColorKey(DWORD flags, LPDDCOLORKEY key) override;
    HRESULT DoSetPalette(LPDIRECTDRAWPALETTE p) override;
    HRESULT DoGetDDInterface(LPVOID* out) override;
    HRESULT DoPageLock(DWORD) override { return DD_OK; }
    HRESULT DoPageUnlock(DWORD) override { return DD_OK; }
    HRESULT DoSetPrivateData(REFGUID guid, LPVOID data, DWORD size, DWORD flags) override;
    HRESULT DoGetPrivateData(REFGUID guid, LPVOID data, LPDWORD size) override;
    HRESULT DoFreePrivateData(REFGUID guid) override;
    HRESULT DoGetUniquenessValue(LPDWORD out) override;
    HRESULT DoChangeUniquenessValue() override { ++m_uniqueness; return DD_OK; }
    HRESULT DoSetPriority(DWORD p) override { m_priority = p; return DD_OK; }
    HRESULT DoGetPriority(LPDWORD p) override { if (p) *p = m_priority; return DD_OK; }
    HRESULT DoSetLOD(DWORD lod) override { m_lod = lod; return DD_OK; }
    HRESULT DoGetLOD(LPDWORD lod) override { if (lod) *lod = m_lod; return DD_OK; }

private:
    bool m_locked = false;
    RECT m_lockRect{};
    DWORD m_lockFlags = 0;
    DDCOLORKEY m_keys[4] = {};
    std::map<std::string, std::vector<uint8_t>> m_private;   // key = GUID bytes
    DWORD m_uniqueness = 1, m_priority = 0, m_lod = 0;
};

class RVertexBuffer final : public Com<BaseIDirect3DVertexBuffer7> {
public:
    explicit RVertexBuffer(const D3DVERTEXBUFFERDESC& d);
    D3DVERTEXBUFFERDESC desc{};
    std::vector<uint8_t> data;
    uint32_t stride = 0;

protected:
    void* Cast(REFIID iid) override { return iid == IID_IDirect3DVertexBuffer7 ? this : nullptr; }
    HRESULT DoLock(DWORD flags, LPVOID* out, LPDWORD size) override;
    HRESULT DoUnlock() override { return DD_OK; }
    HRESULT DoGetVertexBufferDesc(LPD3DVERTEXBUFFERDESC out) override;
    HRESULT DoOptimize(LPDIRECT3DDEVICE7, DWORD) override { return DD_OK; }
    HRESULT DoProcessVertices(DWORD op, DWORD dstIndex, DWORD count, LPDIRECT3DVERTEXBUFFER7 src, DWORD srcIndex,
                              LPDIRECT3DDEVICE7 device, DWORD flags) override;
};

class RDirect3D final : public Com<BaseIDirect3D7> {
public:
    explicit RDirect3D(RDirectDraw* dd);
    ~RDirect3D() override;
    RDirectDraw* ddraw;

protected:
    void* Cast(REFIID iid) override;
    HRESULT DoEnumDevices(LPD3DENUMDEVICESCALLBACK7 cb, LPVOID ctx) override;
    HRESULT DoCreateDevice(REFCLSID clsid, LPDIRECTDRAWSURFACE7 target, LPDIRECT3DDEVICE7* out) override;
    HRESULT DoCreateVertexBuffer(LPD3DVERTEXBUFFERDESC desc, LPDIRECT3DVERTEXBUFFER7* out, DWORD flags) override;
    HRESULT DoEnumZBufferFormats(REFCLSID, LPD3DENUMPIXELFORMATSCALLBACK cb, LPVOID ctx) override;
    HRESULT DoEvictManagedTextures() override { return D3D_OK; }
};

class RDirectDraw final : public Com<BaseIDirectDraw7> {
protected:
    void* Cast(REFIID iid) override;
    HRESULT DoCompact() override { return DD_OK; }
    HRESULT DoCreateClipper(DWORD, LPDIRECTDRAWCLIPPER* out, IUnknown*) override;
    HRESULT DoCreatePalette(DWORD flags, LPPALETTEENTRY entries, LPDIRECTDRAWPALETTE* out, IUnknown*) override;
    HRESULT DoCreateSurface(LPDDSURFACEDESC2 desc, LPDIRECTDRAWSURFACE7* out, IUnknown*) override;
    HRESULT DoEnumDisplayModes(DWORD flags, LPDDSURFACEDESC2 filter, LPVOID ctx, LPDDENUMMODESCALLBACK2 cb) override;
    HRESULT DoFlipToGDISurface() override { return DD_OK; }
    HRESULT DoGetCaps(LPDDCAPS driver, LPDDCAPS hel) override;
    HRESULT DoGetDisplayMode(LPDDSURFACEDESC2 out) override;
    HRESULT DoGetFourCCCodes(LPDWORD count, LPDWORD codes) override;
    HRESULT DoGetMonitorFrequency(LPDWORD hz) override { if (hz) *hz = 60; return DD_OK; }
    HRESULT DoGetVerticalBlankStatus(LPBOOL in) override { if (in) *in = FALSE; return DD_OK; }
    HRESULT DoInitialize(GUID*) override { return DDERR_ALREADYINITIALIZED; }
    HRESULT DoRestoreDisplayMode() override { return DD_OK; }
    HRESULT DoSetCooperativeLevel(HWND hwnd, DWORD flags) override;
    HRESULT DoSetDisplayMode(DWORD w, DWORD h, DWORD, DWORD, DWORD) override
    {
        g_rvk.displayWidth = w;
        g_rvk.displayHeight = h;
        return DD_OK;
    }
    HRESULT DoWaitForVerticalBlank(DWORD, HANDLE) override { return DD_OK; }
    HRESULT DoGetAvailableVidMem(LPDDSCAPS2, LPDWORD total, LPDWORD free) override;
    HRESULT DoRestoreAllSurfaces() override { return DD_OK; }
    HRESULT DoTestCooperativeLevel() override { return DD_OK; }
    HRESULT DoGetDeviceIdentifier(LPDDDEVICEIDENTIFIER2 out, DWORD) override;

private:
    RDirect3D* m_d3d = nullptr;       // weak: created on demand by QueryInterface
    friend class RDirect3D;
};

class RDevice final : public Com<BaseIDirect3DDevice7> {
public:
    RDevice(RDirect3D* d3d, RSurface* target, REFCLSID clsid);
    ~RDevice() override;
    // Software vertex processing (IDirect3DVertexBuffer7::ProcessVertices) with this device's state.
    HRESULT ProcessVertices(DWORD op, RVertexBuffer* dst, DWORD dstIndex, DWORD count, RVertexBuffer* src,
                            DWORD srcIndex, DWORD flags);

protected:
    void* Cast(REFIID iid) override { return iid == IID_IDirect3DDevice7 ? this : nullptr; }
    HRESULT DoGetCaps(LPD3DDEVICEDESC7 desc) override;
    HRESULT DoEnumTextureFormats(LPD3DENUMPIXELFORMATSCALLBACK cb, LPVOID ctx) override;
    HRESULT DoBeginScene() override;
    HRESULT DoEndScene() override { return D3D_OK; }
    HRESULT DoGetDirect3D(LPDIRECT3D7* out) override;
    HRESULT DoSetRenderTarget(LPDIRECTDRAWSURFACE7 s, DWORD) override;
    HRESULT DoGetRenderTarget(LPDIRECTDRAWSURFACE7* out) override;
    HRESULT DoClear(DWORD count, LPD3DRECT rects, DWORD flags, D3DCOLOR color, D3DVALUE z, DWORD) override;
    HRESULT DoSetTransform(D3DTRANSFORMSTATETYPE type, LPD3DMATRIX m) override;
    HRESULT DoGetTransform(D3DTRANSFORMSTATETYPE type, LPD3DMATRIX m) override;
    HRESULT DoSetViewport(LPD3DVIEWPORT7 vp) override;
    HRESULT DoMultiplyTransform(D3DTRANSFORMSTATETYPE type, LPD3DMATRIX m) override;
    HRESULT DoGetViewport(LPD3DVIEWPORT7 vp) override;
    HRESULT DoSetMaterial(LPD3DMATERIAL7 m) override;
    HRESULT DoGetMaterial(LPD3DMATERIAL7 m) override;
    HRESULT DoSetLight(DWORD i, LPD3DLIGHT7 l) override;
    HRESULT DoGetLight(DWORD i, LPD3DLIGHT7 l) override;
    HRESULT DoSetRenderState(D3DRENDERSTATETYPE s, DWORD v) override;
    HRESULT DoGetRenderState(D3DRENDERSTATETYPE s, LPDWORD v) override;
    HRESULT DoPreLoad(LPDIRECTDRAWSURFACE7) override { return D3D_OK; }
    HRESULT DoDrawPrimitive(D3DPRIMITIVETYPE type, DWORD fvf, LPVOID verts, DWORD count, DWORD) override;
    HRESULT DoDrawIndexedPrimitive(D3DPRIMITIVETYPE type, DWORD fvf, LPVOID verts, DWORD vcount, LPWORD idx,
                                   DWORD icount, DWORD) override;
    HRESULT DoDrawPrimitiveVB(D3DPRIMITIVETYPE type, LPDIRECT3DVERTEXBUFFER7 vb, DWORD start, DWORD count, DWORD) override;
    HRESULT DoDrawIndexedPrimitiveVB(D3DPRIMITIVETYPE type, LPDIRECT3DVERTEXBUFFER7 vb, DWORD start, DWORD vcount,
                                     LPWORD idx, DWORD icount, DWORD) override;
    HRESULT DoGetTexture(DWORD stage, LPDIRECTDRAWSURFACE7* out) override;
    HRESULT DoSetTexture(DWORD stage, LPDIRECTDRAWSURFACE7 s) override;
    HRESULT DoGetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE t, LPDWORD v) override;
    HRESULT DoSetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE t, DWORD v) override;
    HRESULT DoValidateDevice(LPDWORD passes) override { if (passes) *passes = 1; return D3D_OK; }
    HRESULT DoLightEnable(DWORD i, BOOL enable) override;
    HRESULT DoGetLightEnable(DWORD i, BOOL* enable) override;
    HRESULT DoGetInfo(DWORD, LPVOID data, DWORD size) override;

private:
    RDirect3D* m_d3d;
    RSurface* m_target;
    GUID m_clsid;
    DWORD m_rs[256] = {};
    DWORD m_tss[8][32] = {};
    RSurface* m_textures[8] = {};
    D3DMATRIX m_transforms[32] = {};               // indexed by D3DTRANSFORMSTATE (world = 1 ... texture7 = 23)
    D3DVIEWPORT7 m_viewport{};
    D3DMATERIAL7 m_material{};
    std::vector<std::pair<D3DLIGHT7, BOOL>> m_lights;
};

}  // namespace rvkproxy
