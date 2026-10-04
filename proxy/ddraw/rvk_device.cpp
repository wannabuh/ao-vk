// rvk backend: IDirect3DDevice7 and IDirect3DVertexBuffer7.
#include "rvk_backend.h"
#include "native/scene.h"

#include <cmath>
#include <cstring>

namespace rvkproxy {

namespace {

static_assert(sizeof(D3DMATRIX) == sizeof(rvk::d3d::Matrix) && sizeof(D3DMATERIAL7) == sizeof(rvk::d3d::Material) &&
                  sizeof(D3DLIGHT7) == sizeof(rvk::d3d::Light) && sizeof(D3DVIEWPORT7) == sizeof(rvk::d3d::Viewport) &&
                  sizeof(D3DRECT) == sizeof(rvk::Device::Rect),
              "D3D7 and rvk layouts must match");

const D3DMATRIX kIdentity = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

D3DMATRIX Multiply(const D3DMATRIX& a, const D3DMATRIX& b)
{
    D3DMATRIX r{};
    const float(*x)[4] = reinterpret_cast<const float(*)[4]>(&a);
    const float(*y)[4] = reinterpret_cast<const float(*)[4]>(&b);
    float(*z)[4] = reinterpret_cast<float(*)[4]>(&r);
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            z[i][j] = x[i][0] * y[0][j] + x[i][1] * y[1][j] + x[i][2] * y[2][j] + x[i][3] * y[3][j];
    return r;
}

rvk::Texture* TextureOf(RSurface* s)
{
    if (!s) return nullptr;
    if (s->kind == RSurface::Kind::Texture) return s->top->RvkTexture();
    if (s->kind == RSurface::Kind::RenderTarget) return s->RvkTexture();
    return nullptr;
}

}  // namespace

RDevice::RDevice(RDirect3D* d3d, RSurface* target, REFCLSID clsid) : m_d3d(d3d), m_target(target), m_clsid(clsid)
{
    m_d3d->AddRef();
    m_target->AddRef();
    // D3D7 defaults (match rvk::Device's), so GetRenderState/GetTextureStageState report what is in effect.
    m_rs[D3DRENDERSTATE_TEXTUREPERSPECTIVE] = TRUE;
    m_rs[D3DRENDERSTATE_ZENABLE] = D3DZB_TRUE;
    m_rs[D3DRENDERSTATE_FILLMODE] = D3DFILL_SOLID;
    m_rs[D3DRENDERSTATE_SHADEMODE] = D3DSHADE_GOURAUD;
    m_rs[D3DRENDERSTATE_ZWRITEENABLE] = TRUE;
    m_rs[D3DRENDERSTATE_LASTPIXEL] = TRUE;
    m_rs[D3DRENDERSTATE_SRCBLEND] = D3DBLEND_ONE;
    m_rs[D3DRENDERSTATE_DESTBLEND] = D3DBLEND_ZERO;
    m_rs[D3DRENDERSTATE_CULLMODE] = D3DCULL_CCW;
    m_rs[D3DRENDERSTATE_ZFUNC] = D3DCMP_LESSEQUAL;
    m_rs[D3DRENDERSTATE_ALPHAFUNC] = D3DCMP_ALWAYS;
    m_rs[D3DRENDERSTATE_FOGEND] = 0x3F800000;
    m_rs[D3DRENDERSTATE_FOGDENSITY] = 0x3F800000;
    m_rs[D3DRENDERSTATE_STENCILFAIL] = m_rs[D3DRENDERSTATE_STENCILZFAIL] = m_rs[D3DRENDERSTATE_STENCILPASS] = D3DSTENCILOP_KEEP;
    m_rs[D3DRENDERSTATE_STENCILFUNC] = D3DCMP_ALWAYS;
    m_rs[D3DRENDERSTATE_STENCILMASK] = m_rs[D3DRENDERSTATE_STENCILWRITEMASK] = 0xFFFFFFFF;
    m_rs[D3DRENDERSTATE_TEXTUREFACTOR] = 0xFFFFFFFF;
    m_rs[D3DRENDERSTATE_CLIPPING] = TRUE;
    m_rs[D3DRENDERSTATE_LIGHTING] = TRUE;
    m_rs[D3DRENDERSTATE_COLORVERTEX] = TRUE;
    m_rs[D3DRENDERSTATE_LOCALVIEWER] = TRUE;
    m_rs[D3DRENDERSTATE_DIFFUSEMATERIALSOURCE] = D3DMCS_COLOR1;
    m_rs[D3DRENDERSTATE_SPECULARMATERIALSOURCE] = D3DMCS_COLOR2;
    for (DWORD s = 0; s < 8; ++s) {
        DWORD* t = m_tss[s];
        t[D3DTSS_COLOROP] = s == 0 ? D3DTOP_MODULATE : D3DTOP_DISABLE;
        t[D3DTSS_COLORARG1] = D3DTA_TEXTURE;
        t[D3DTSS_COLORARG2] = D3DTA_CURRENT;
        t[D3DTSS_ALPHAOP] = s == 0 ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE;
        t[D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
        t[D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
        t[D3DTSS_TEXCOORDINDEX] = s;
        t[D3DTSS_ADDRESS] = t[D3DTSS_ADDRESSU] = t[D3DTSS_ADDRESSV] = D3DTADDRESS_WRAP;
        t[D3DTSS_MAGFILTER] = D3DTFG_POINT;
        t[D3DTSS_MINFILTER] = D3DTFN_POINT;
        t[D3DTSS_MIPFILTER] = D3DTFP_NONE;
        t[D3DTSS_MAXANISOTROPY] = 1;
    }
    for (auto& m : m_transforms) m = kIdentity;
    m_viewport = {0, 0, target->desc.dwWidth, target->desc.dwHeight, 0.0f, 1.0f};
    if (rvk::ThreadedDevice* dev = g_rvk.device) {
        dev->SetRenderTarget(target->kind == RSurface::Kind::Main ? nullptr : TextureOf(target));
        dev->SetViewport(*reinterpret_cast<rvk::d3d::Viewport*>(&m_viewport));
    }
}

RDevice::~RDevice()
{
    for (RSurface*& t : m_textures)
        if (t) { t->Release(); t = nullptr; }
    m_target->Release();
    m_d3d->Release();
}

HRESULT RDevice::DoGetCaps(LPD3DDEVICEDESC7 desc)
{
    if (!desc) return DDERR_INVALIDPARAMS;
    FillDeviceDesc(desc, m_clsid);
    return D3D_OK;
}

HRESULT RDevice::DoEnumTextureFormats(LPD3DENUMPIXELFORMATSCALLBACK cb, LPVOID ctx)
{
    if (!cb) return DDERR_INVALIDPARAMS;
    // Same formats in the same order as D3D7 drivers report them (checked against D7VK): D3DX takes the first
    // suitable one when the caller leaves the format open, and game code depends on the result (the planet
    // map builder only copies 16-bit terrain textures). Bump-map formats are left out (unused, unsupported).
    static const rvk::Format formats[] = {rvk::Format::X1R5G5B5, rvk::Format::A1R5G5B5, rvk::Format::A4R4G4B4,
                                          rvk::Format::R5G6B5,   rvk::Format::X8R8G8B8, rvk::Format::A8R8G8B8,
                                          rvk::Format::DXT1,     rvk::Format::DXT2,     rvk::Format::DXT3,
                                          rvk::Format::DXT4,     rvk::Format::DXT5};
    for (rvk::Format f : formats) {
        DDPIXELFORMAT pf;
        PixelFormatFor(f, &pf);
        if (cb(&pf, ctx) == D3DENUMRET_CANCEL) break;
    }
    return D3D_OK;
}

HRESULT RDevice::DoBeginScene()
{
    g_rvk.Frame();
    return D3D_OK;
}

HRESULT RDevice::DoGetDirect3D(LPDIRECT3D7* out)
{
    if (!out) return DDERR_INVALIDPARAMS;
    m_d3d->AddRef();
    *out = m_d3d;
    return D3D_OK;
}

HRESULT RDevice::DoSetRenderTarget(LPDIRECTDRAWSURFACE7 iface, DWORD)
{
    auto* s = static_cast<RSurface*>(iface);
    if (!s || (s->kind != RSurface::Kind::Main && s->kind != RSurface::Kind::RenderTarget))
        return DDERR_INVALIDPARAMS;
    s->AddRef();
    m_target->Release();
    m_target = s;
    if (rvk::ThreadedDevice* dev = g_rvk.device)
        dev->SetRenderTarget(s->kind == RSurface::Kind::Main ? nullptr : TextureOf(s));
    m_viewport = {0, 0, s->desc.dwWidth, s->desc.dwHeight, m_viewport.dvMinZ, m_viewport.dvMaxZ};
    for (int i = 0; i < 8; ++i)                 // rvk unbinds a texture that becomes the target
        if (m_textures[i] == s) { s->Release(); m_textures[i] = nullptr; }
    return D3D_OK;
}

HRESULT RDevice::DoGetRenderTarget(LPDIRECTDRAWSURFACE7* out)
{
    if (!out) return DDERR_INVALIDPARAMS;
    m_target->AddRef();
    *out = m_target;
    return D3D_OK;
}

HRESULT RDevice::DoClear(DWORD count, LPD3DRECT rects, DWORD flags, D3DCOLOR color, D3DVALUE z, DWORD)
{
    rvk::ThreadedDevice* dev = g_rvk.device;
    if (!dev) return DDERR_GENERIC;
    g_rvk.Frame();
    dev->Clear(count, reinterpret_cast<const rvk::Device::Rect*>(rects), flags & (D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER), color, z);
    return D3D_OK;
}

HRESULT RDevice::DoSetTransform(D3DTRANSFORMSTATETYPE type, LPD3DMATRIX m)
{
    if (!m || DWORD(type) >= 32) return DDERR_INVALIDPARAMS;
    m_transforms[type] = *m;
    if (rvk::ThreadedDevice* dev = g_rvk.device)
        dev->SetTransform(type, *reinterpret_cast<const rvk::d3d::Matrix*>(m));
    return D3D_OK;
}

HRESULT RDevice::DoGetTransform(D3DTRANSFORMSTATETYPE type, LPD3DMATRIX m)
{
    if (!m || DWORD(type) >= 32) return DDERR_INVALIDPARAMS;
    *m = m_transforms[type];
    return D3D_OK;
}

HRESULT RDevice::DoMultiplyTransform(D3DTRANSFORMSTATETYPE type, LPD3DMATRIX m)
{
    if (!m || DWORD(type) >= 32) return DDERR_INVALIDPARAMS;
    D3DMATRIX r = Multiply(*m, m_transforms[type]);
    return DoSetTransform(type, &r);
}

HRESULT RDevice::DoSetViewport(LPD3DVIEWPORT7 vp)
{
    if (!vp) return DDERR_INVALIDPARAMS;
    m_viewport = *vp;
    if (rvk::ThreadedDevice* dev = g_rvk.device)
        dev->SetViewport(*reinterpret_cast<const rvk::d3d::Viewport*>(vp));
    return D3D_OK;
}

HRESULT RDevice::DoGetViewport(LPD3DVIEWPORT7 vp)
{
    if (!vp) return DDERR_INVALIDPARAMS;
    *vp = m_viewport;
    return D3D_OK;
}

HRESULT RDevice::DoSetMaterial(LPD3DMATERIAL7 m)
{
    if (!m) return DDERR_INVALIDPARAMS;
    m_material = *m;
    if (rvk::ThreadedDevice* dev = g_rvk.device)
        dev->SetMaterial(*reinterpret_cast<const rvk::d3d::Material*>(m));
    return D3D_OK;
}

HRESULT RDevice::DoGetMaterial(LPD3DMATERIAL7 m)
{
    if (!m) return DDERR_INVALIDPARAMS;
    *m = m_material;
    return D3D_OK;
}

HRESULT RDevice::DoSetLight(DWORD i, LPD3DLIGHT7 l)
{
    if (!l) return DDERR_INVALIDPARAMS;
    if (i >= m_lights.size()) m_lights.resize(i + 1, {D3DLIGHT7{}, FALSE});
    m_lights[i].first = *l;
    if (rvk::ThreadedDevice* dev = g_rvk.device)
        dev->SetLight(i, *reinterpret_cast<const rvk::d3d::Light*>(l));
    return D3D_OK;
}

HRESULT RDevice::DoGetLight(DWORD i, LPD3DLIGHT7 l)
{
    if (!l || i >= m_lights.size()) return DDERR_INVALIDPARAMS;
    *l = m_lights[i].first;
    return D3D_OK;
}

HRESULT RDevice::DoLightEnable(DWORD i, BOOL enable)
{
    if (i >= m_lights.size()) m_lights.resize(i + 1, {D3DLIGHT7{}, FALSE});
    m_lights[i].second = enable;
    if (rvk::ThreadedDevice* dev = g_rvk.device)
        dev->LightEnable(i, enable != FALSE);
    return D3D_OK;
}

HRESULT RDevice::DoGetLightEnable(DWORD i, BOOL* enable)
{
    if (!enable || i >= m_lights.size()) return DDERR_INVALIDPARAMS;
    *enable = m_lights[i].second;
    return D3D_OK;
}

HRESULT RDevice::DoSetRenderState(D3DRENDERSTATETYPE s, DWORD v)
{
    if (DWORD(s) < 256) m_rs[s] = v;
    if (rvk::ThreadedDevice* dev = g_rvk.device)
        dev->SetRenderState(s, v);
    return D3D_OK;
}

HRESULT RDevice::DoGetRenderState(D3DRENDERSTATETYPE s, LPDWORD v)
{
    if (!v || DWORD(s) >= 256) return DDERR_INVALIDPARAMS;
    *v = m_rs[s];
    return D3D_OK;
}

HRESULT RDevice::DoSetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE t, DWORD v)
{
    if (stage >= 8 || DWORD(t) >= 32) return DDERR_INVALIDPARAMS;
    m_tss[stage][t] = v;
    if (t == D3DTSS_ADDRESS) m_tss[stage][D3DTSS_ADDRESSU] = m_tss[stage][D3DTSS_ADDRESSV] = v;
    if (rvk::ThreadedDevice* dev = g_rvk.device)
        dev->SetTextureStageState(stage, t, v);
    return D3D_OK;
}

HRESULT RDevice::DoGetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE t, LPDWORD v)
{
    if (!v || stage >= 8 || DWORD(t) >= 32) return DDERR_INVALIDPARAMS;
    *v = m_tss[stage][t];
    return D3D_OK;
}

HRESULT RDevice::DoSetTexture(DWORD stage, LPDIRECTDRAWSURFACE7 iface)
{
    if (stage >= 8) return DDERR_INVALIDPARAMS;
    auto* s = static_cast<RSurface*>(iface);
    if (s) s->AddRef();
    if (m_textures[stage]) m_textures[stage]->Release();
    m_textures[stage] = s;
    if (rvk::ThreadedDevice* dev = g_rvk.device)
        dev->SetTexture(stage, TextureOf(s));
    return D3D_OK;
}

HRESULT RDevice::DoGetTexture(DWORD stage, LPDIRECTDRAWSURFACE7* out)
{
    if (!out || stage >= 8) return DDERR_INVALIDPARAMS;
    *out = m_textures[stage];
    if (*out) m_textures[stage]->AddRef();
    return D3D_OK;
}

// The game's visual behind the next draws (native scene tracking, [Native] Visuals), when it changes.
void NoteVisual(rvk::ThreadedDevice* dev)
{
    if (!rnative::scene::Installed())
        return;
    static const void* last = reinterpret_cast<const void*>(1);
    const void* visual = rnative::scene::CurrentVisual();
    if (visual == last)
        return;
    last = visual;
    rnative::scene::VisualInfo info = rnative::scene::Describe(visual);
    dev->SetDrawVisual(uint32_t(info.kind), info.className, uint32_t(reinterpret_cast<uintptr_t>(info.owner)));
}

// The game's lights after its scene update (native scene tracking), to the renderer.
void SceneLights(const rnative::scene::SceneLight* lights, size_t count)
{
    rvk::ThreadedDevice* dev = g_rvk.device;
    if (!dev)
        return;
    static std::vector<rvk::Device::SceneLight> out;
    out.resize(count);
    for (size_t i = 0; i < count; ++i) {
        std::memcpy(&out[i].light, lights[i].d3dLight, sizeof(out[i].light));
        out[i].owner = uint32_t(reinterpret_cast<uintptr_t>(lights[i].owner));
    }
    dev->SetSceneLights(out.data(), uint32_t(count));
}

HRESULT RDevice::DoDrawPrimitive(D3DPRIMITIVETYPE type, DWORD fvf, LPVOID verts, DWORD count, DWORD)
{
    CountBackendDraw();
    rvk::ThreadedDevice* dev = g_rvk.device;
    if (!dev || !verts) return DDERR_INVALIDPARAMS;
    g_rvk.Frame();
    NoteVisual(dev);
    dev->DrawPrimitive(type, fvf, verts, count);
    return D3D_OK;
}

HRESULT RDevice::DoDrawIndexedPrimitive(D3DPRIMITIVETYPE type, DWORD fvf, LPVOID verts, DWORD vcount, LPWORD idx,
                                        DWORD icount, DWORD)
{
    CountBackendDraw();
    rvk::ThreadedDevice* dev = g_rvk.device;
    if (!dev || !verts || !idx) return DDERR_INVALIDPARAMS;
    g_rvk.Frame();
    NoteVisual(dev);
    dev->DrawIndexedPrimitive(type, fvf, verts, vcount, idx, icount);
    return D3D_OK;
}

HRESULT RDevice::DoDrawPrimitiveVB(D3DPRIMITIVETYPE type, LPDIRECT3DVERTEXBUFFER7 iface, DWORD start, DWORD count, DWORD)
{
    CountBackendDraw();
    auto* vb = static_cast<RVertexBuffer*>(iface);
    rvk::ThreadedDevice* dev = g_rvk.device;
    if (!dev || !vb || start + count > vb->desc.dwNumVertices) return DDERR_INVALIDPARAMS;
    g_rvk.Frame();
    NoteVisual(dev);
    if (vb->skin)
        dev->DrawPrimitiveSkinned(type, vb->desc.dwFVF, vb->skin, start, count);
    else if (auto* shared = vb->StaticShared())
        dev->DrawPrimitiveShared(type, vb->desc.dwFVF, *shared, size_t(start) * vb->stride, count);
    else
        dev->DrawPrimitive(type, vb->desc.dwFVF, vb->Bytes() + size_t(start) * vb->stride, count);
    return D3D_OK;
}

HRESULT RDevice::DoDrawIndexedPrimitiveVB(D3DPRIMITIVETYPE type, LPDIRECT3DVERTEXBUFFER7 iface, DWORD start, DWORD vcount,
                                          LPWORD idx, DWORD icount, DWORD)
{
    CountBackendDraw();
    // D3D7: the indices are relative to start; vcount vertices from there are referenced.
    auto* vb = static_cast<RVertexBuffer*>(iface);
    rvk::ThreadedDevice* dev = g_rvk.device;
    if (!dev || !vb || !idx || start + vcount > vb->desc.dwNumVertices) return DDERR_INVALIDPARAMS;
    g_rvk.Frame();
    NoteVisual(dev);
    if (vb->skin)
        dev->DrawIndexedPrimitiveSkinned(type, vb->desc.dwFVF, vb->skin, start, vcount, idx, icount);
    else if (auto* shared = vb->StaticShared())
        dev->DrawIndexedPrimitiveShared(type, vb->desc.dwFVF, *shared, size_t(start) * vb->stride, vcount, idx, icount);
    else
        dev->DrawIndexedPrimitive(type, vb->desc.dwFVF, vb->Bytes() + size_t(start) * vb->stride, vcount, idx, icount);
    return D3D_OK;
}

HRESULT RDevice::DoGetInfo(DWORD, LPVOID data, DWORD size)
{
    // Randy treats any non-zero result as an error; report empty statistics.
    if (data) std::memset(data, 0, size);
    return D3D_OK;
}

// ---------------------------------------------------------------------------------------------------
// IDirect3DVertexBuffer7

RVertexBuffer::RVertexBuffer(const D3DVERTEXBUFFERDESC& d) : desc(d)
{
    desc.dwSize = sizeof(desc);
    stride = rvk::Device::FvfStride(desc.dwFVF);
    buf = std::make_shared<std::vector<uint8_t>>(size_t(stride) * desc.dwNumVertices, 0);
}

// About to be written (or the pointer for writing handed out): if queued draws still reference the vertices, the
// buffer goes on with a copy of its own (they keep theirs). Only the game's thread makes references, so a count of 1
// means none are left.
// The skin job's vertices into the buffer (for reading it); the job stays for draws.
void RVertexBuffer::Materialize()
{
    if (!skin)
        return;
    std::shared_ptr<rvk::skin::Job> job = std::move(skin);
    Written();                                   // its own copy if queued draws reference the old one
    size_t n = std::min<size_t>(job->source->vertices.size(), desc.dwNumVertices);
    std::memcpy(Bytes(), job->Skinned(), n * sizeof(rvk::skin::Vertex));
    skin = std::move(job);
}

bool AttachSkin(void* d3dVertexBuffer, std::shared_ptr<rvk::skin::Job> job)
{
    auto* vb = static_cast<RVertexBuffer*>(static_cast<IDirect3DVertexBuffer7*>(d3dVertexBuffer));
    if (!vb || !job || !g_rvk.device || vb->desc.dwFVF != rvk::skin::kVertexFvf ||
        job->source->vertices.size() > vb->desc.dwNumVertices)
        return false;
    if (vb->skin && !job->prevBones)
        job->prevBones = vb->skin->bones;        // what it was last time: motion vectors
    vb->skin = std::move(job);
    vb->shared.reset();
    vb->lastWriteFrame = g_rvk.presentCount;
    return true;
}

void RVertexBuffer::Written()
{
    skin.reset();
    shared.reset();
    if (buf.use_count() > 1)
        buf = std::make_shared<std::vector<uint8_t>>(*buf);
    lastWriteFrame = g_rvk.presentCount;
}

// The vertices to reference for a buffer unchanged for two frames, or null for one written lately - those
// (characters the game skins into their buffers each frame) are copied per draw, as before.
const std::shared_ptr<const std::vector<uint8_t>>* RVertexBuffer::StaticShared()
{
    if (g_rvk.presentCount < lastWriteFrame + 2)
        return nullptr;
    if (!shared)
        shared = buf;
    return &shared;
}

HRESULT RVertexBuffer::DoLock(DWORD flags, LPVOID* out, LPDWORD size)
{
    if (!out) return DDERR_INVALIDPARAMS;
    if (flags & DDLOCK_READONLY)                 // reading (picking, native verify) leaves it unchanged
        Materialize();
    else
        Written();
    *out = Bytes();
    if (size) *size = DWORD(buf->size());
    return D3D_OK;
}

HRESULT RVertexBuffer::DoGetVertexBufferDesc(LPD3DVERTEXBUFFERDESC out)
{
    if (!out) return DDERR_INVALIDPARAMS;
    *out = desc;
    return D3D_OK;
}

}  // namespace rvkproxy

// ---------------------------------------------------------------------------------------------------
// Software vertex processing: D3D7 ProcessVertices with D3DVOP_TRANSFORM [| LIGHT | CLIP | EXTENTS].
// Clip codes and extents are not kept (nothing in the client reads them back).

namespace rvkproxy {

namespace {

struct Fvf {
    uint32_t stride = 0;
    int pos = 0, normal = -1, diffuse = -1, specular = -1;
    bool rhw = false;
    int tex[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    uint32_t texSize[8] = {};                        // floats per set
    uint32_t texCount = 0;
};

Fvf DecodeFvf(DWORD fvf)
{
    Fvf f;
    DWORD position = fvf & D3DFVF_POSITION_MASK;
    f.rhw = position == D3DFVF_XYZRHW;
    f.stride = f.rhw ? 16 : 12 + (position >= D3DFVF_XYZB1 ? ((position - D3DFVF_XYZRHW) / 2) * 4 : 0);
    if (fvf & D3DFVF_NORMAL) { f.normal = int(f.stride); f.stride += 12; }
    if (fvf & D3DFVF_RESERVED1) f.stride += 4;
    if (fvf & D3DFVF_DIFFUSE) { f.diffuse = int(f.stride); f.stride += 4; }
    if (fvf & D3DFVF_SPECULAR) { f.specular = int(f.stride); f.stride += 4; }
    f.texCount = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
    static const uint32_t sizes[4] = {2, 3, 4, 1};
    for (uint32_t i = 0; i < f.texCount && i < 8; ++i) {
        f.tex[i] = int(f.stride);
        f.texSize[i] = sizes[(fvf >> (16 + 2 * i)) & 3];
        f.stride += f.texSize[i] * 4;
    }
    return f;
}

struct V3 { float x, y, z; };
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Normalize(V3 a) { float l = std::sqrt(Dot(a, a)); return l > 0 ? a * (1.0f / l) : a; }

void Transform(const D3DMATRIX& m, const float in[4], float out[4])
{
    const float(*r)[4] = reinterpret_cast<const float(*)[4]>(&m);
    for (int j = 0; j < 4; ++j)
        out[j] = in[0] * r[0][j] + in[1] * r[1][j] + in[2] * r[2][j] + in[3] * r[3][j];
}

struct C4 { float r, g, b, a; };
C4 FromArgb(DWORD c) { return {((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, (c & 0xFF) / 255.0f, (c >> 24) / 255.0f}; }
C4 FromValue(const D3DCOLORVALUE& c) { return {c.r, c.g, c.b, c.a}; }
DWORD ToArgb(C4 c)
{
    auto q = [](float v) { return DWORD((v <= 0.0f ? 0.0f : v >= 1.0f ? 1.0f : v) * 255.0f + 0.5f); };   // lroundf is slow
    return (q(c.a) << 24) | (q(c.r) << 16) | (q(c.g) << 8) | q(c.b);
}

}  // namespace

HRESULT RDevice::ProcessVertices(DWORD op, RVertexBuffer* dst, DWORD dstIndex, DWORD count, RVertexBuffer* src,
                                 DWORD srcIndex, DWORD flags)
{
    if (!(op & D3DVOP_TRANSFORM)) return DDERR_INVALIDPARAMS;
    Fvf sf = DecodeFvf(src->desc.dwFVF), df = DecodeFvf(dst->desc.dwFVF);
    if (!df.rhw || sf.rhw || srcIndex + count > src->desc.dwNumVertices || dstIndex + count > dst->desc.dwNumVertices)
        return DDERR_INVALIDPARAMS;
    src->Materialize();
    bool copyData = !(flags & D3DPV_DONOTCOPYDATA);
    bool light = (op & D3DVOP_LIGHT) && m_rs[D3DRENDERSTATE_LIGHTING];
    D3DMATRIX wvp = Multiply(Multiply(m_transforms[D3DTRANSFORMSTATE_WORLD], m_transforms[D3DTRANSFORMSTATE_VIEW]),
                             m_transforms[D3DTRANSFORMSTATE_PROJECTION]);
    const D3DMATRIX& world = m_transforms[D3DTRANSFORMSTATE_WORLD];
    const float(*v)[4] = reinterpret_cast<const float(*)[4]>(&m_transforms[D3DTRANSFORMSTATE_VIEW]);
    V3 eyePos{}, eyeDir{v[0][2], v[1][2], v[2][2]};
    eyePos.x = -(v[3][0] * v[0][0] + v[3][1] * v[0][1] + v[3][2] * v[0][2]);
    eyePos.y = -(v[3][0] * v[1][0] + v[3][1] * v[1][1] + v[3][2] * v[1][2]);
    eyePos.z = -(v[3][0] * v[2][0] + v[3][1] * v[2][1] + v[3][2] * v[2][2]);

    dst->Written();                              // its shared copy (StaticSnapshot) is out of date
    for (DWORD i = 0; i < count; ++i) {
        const uint8_t* in = src->Bytes() + size_t(srcIndex + i) * sf.stride;
        uint8_t* out = dst->Bytes() + size_t(dstIndex + i) * df.stride;
        float p[4] = {0, 0, 0, 1}, c[4];
        std::memcpy(p, in + sf.pos, 12);
        Transform(wvp, p, c);
        float rhw = c[3] != 0.0f ? 1.0f / c[3] : 1.0f;
        float screen[4] = {m_viewport.dwX + (1.0f + c[0] * rhw) * 0.5f * m_viewport.dwWidth,
                           m_viewport.dwY + (1.0f - c[1] * rhw) * 0.5f * m_viewport.dwHeight,
                           m_viewport.dvMinZ + c[2] * rhw * (m_viewport.dvMaxZ - m_viewport.dvMinZ), rhw};
        std::memcpy(out + df.pos, screen, 16);

        DWORD inDiffuse = 0xFFFFFFFF, inSpecular = 0xFF000000;   // specular alpha = fog factor, 1 without fog
        if (sf.diffuse >= 0) std::memcpy(&inDiffuse, in + sf.diffuse, 4);
        if (sf.specular >= 0) std::memcpy(&inSpecular, in + sf.specular, 4);
        DWORD outDiffuse = inDiffuse, outSpecular = inSpecular;
        if (light) {
            // D3D7 fixed-function lighting in world space (same model as rvk's vertex shader).
            float pw[4];
            Transform(world, p, pw);
            V3 posW{pw[0], pw[1], pw[2]};
            V3 n{0, 0, 0};
            if (sf.normal >= 0) {
                float nn[3];
                std::memcpy(nn, in + sf.normal, 12);
                const float(*w)[4] = reinterpret_cast<const float(*)[4]>(&world);
                n = {nn[0] * w[0][0] + nn[1] * w[1][0] + nn[2] * w[2][0], nn[0] * w[0][1] + nn[1] * w[1][1] + nn[2] * w[2][1],
                     nn[0] * w[0][2] + nn[1] * w[1][2] + nn[2] * w[2][2]};
                if (m_rs[D3DRENDERSTATE_NORMALIZENORMALS]) n = Normalize(n);
            }
            auto source = [&](DWORD which, const D3DCOLORVALUE& material) {
                if (m_rs[D3DRENDERSTATE_COLORVERTEX]) {
                    if (which == D3DMCS_COLOR1 && sf.diffuse >= 0) return FromArgb(inDiffuse);
                    if (which == D3DMCS_COLOR2 && sf.specular >= 0) return FromArgb(inSpecular);
                }
                return FromValue(material);
            };
            C4 mDiffuse = source(m_rs[D3DRENDERSTATE_DIFFUSEMATERIALSOURCE], m_material.diffuse);
            C4 mAmbient = source(m_rs[D3DRENDERSTATE_AMBIENTMATERIALSOURCE], m_material.ambient);
            C4 mSpecular = source(m_rs[D3DRENDERSTATE_SPECULARMATERIALSOURCE], m_material.specular);
            C4 mEmissive = source(m_rs[D3DRENDERSTATE_EMISSIVEMATERIALSOURCE], m_material.emissive);
            C4 ambient = FromArgb(m_rs[D3DRENDERSTATE_AMBIENT]);
            V3 amb{ambient.r, ambient.g, ambient.b}, diff{0, 0, 0}, spec{0, 0, 0};
            V3 toEye = m_rs[D3DRENDERSTATE_LOCALVIEWER] ? Normalize(eyePos - posW) : eyeDir * -1.0f;
            for (auto& [l, enabled] : m_lights) {
                if (!enabled) continue;
                V3 L;
                float att = 1.0f;
                if (l.dltType == D3DLIGHT_DIRECTIONAL) {
                    L = Normalize(V3{l.dvDirection.x, l.dvDirection.y, l.dvDirection.z} * -1.0f);
                } else {
                    V3 d = V3{l.dvPosition.x, l.dvPosition.y, l.dvPosition.z} - posW;
                    float dist = std::sqrt(Dot(d, d));
                    if (dist > l.dvRange) continue;
                    L = dist > 0 ? d * (1.0f / dist) : d;
                    float denom = l.dvAttenuation0 + l.dvAttenuation1 * dist + l.dvAttenuation2 * dist * dist;
                    att = denom > 0 ? 1.0f / denom : 1.0f;
                    if (l.dltType == D3DLIGHT_SPOT) {
                        float rho = Dot(L * -1.0f, Normalize(V3{l.dvDirection.x, l.dvDirection.y, l.dvDirection.z}));
                        float cosTheta = std::cos(l.dvTheta * 0.5f), cosPhi = std::cos(l.dvPhi * 0.5f);
                        if (rho <= cosPhi) att = 0;
                        else if (rho < cosTheta)
                            att *= std::pow(std::fmax((rho - cosPhi) / std::fmax(cosTheta - cosPhi, 1e-6f), 0.0f), l.dvFalloff);
                    }
                }
                amb = amb + V3{l.dcvAmbient.r, l.dcvAmbient.g, l.dcvAmbient.b} * att;
                float nl = std::fmax(Dot(n, L), 0.0f);
                diff = diff + V3{l.dcvDiffuse.r, l.dcvDiffuse.g, l.dcvDiffuse.b} * (att * nl);
                if (m_rs[D3DRENDERSTATE_SPECULARENABLE] && nl > 0) {
                    float nh = std::fmax(Dot(n, Normalize(L + toEye)), 0.0f);
                    spec = spec + V3{l.dcvSpecular.r, l.dcvSpecular.g, l.dcvSpecular.b} * (att * std::pow(nh, m_material.power));
                }
            }
            outDiffuse = ToArgb({mEmissive.r + mAmbient.r * amb.x + mDiffuse.r * diff.x,
                                 mEmissive.g + mAmbient.g * amb.y + mDiffuse.g * diff.y,
                                 mEmissive.b + mAmbient.b * amb.z + mDiffuse.b * diff.z, mDiffuse.a});
            outSpecular = ToArgb({mSpecular.r * spec.x, mSpecular.g * spec.y, mSpecular.b * spec.z, FromArgb(inSpecular).a});
        }
        if (df.diffuse >= 0 && (light || copyData)) std::memcpy(out + df.diffuse, &outDiffuse, 4);
        if (df.specular >= 0 && (light || copyData)) std::memcpy(out + df.specular, &outSpecular, 4);
        if (copyData)
            for (uint32_t t = 0; t < df.texCount && t < 8; ++t) {
                float tc[4] = {0, 0, 0, 0};
                if (t < sf.texCount) std::memcpy(tc, in + sf.tex[t], sf.texSize[t] * 4);
                std::memcpy(out + df.tex[t], tc, df.texSize[t] * 4);
            }
    }
    return D3D_OK;
}

HRESULT RVertexBuffer::DoProcessVertices(DWORD op, DWORD dstIndex, DWORD count, LPDIRECT3DVERTEXBUFFER7 src, DWORD srcIndex,
                                         LPDIRECT3DDEVICE7 device, DWORD flags)
{
    if (!src || !device) return DDERR_INVALIDPARAMS;
    return static_cast<RDevice*>(device)->ProcessVertices(op, this, dstIndex, count, static_cast<RVertexBuffer*>(src), srcIndex,
                                                          flags);
}

}  // namespace rvkproxy
