// rvk backend: IDirect3DDevice7 and IDirect3DVertexBuffer7.
#include "rvk_backend.h"

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
    if (rvk::Device* dev = g_rvk.device) {
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
    static const rvk::Format formats[] = {rvk::Format::A8R8G8B8, rvk::Format::X8R8G8B8, rvk::Format::R5G6B5,
                                          rvk::Format::X1R5G5B5, rvk::Format::A1R5G5B5, rvk::Format::A4R4G4B4,
                                          rvk::Format::L8, rvk::Format::A8L8, rvk::Format::DXT1, rvk::Format::DXT2,
                                          rvk::Format::DXT3, rvk::Format::DXT4, rvk::Format::DXT5};
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
    if (rvk::Device* dev = g_rvk.device)
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
    rvk::Device* dev = g_rvk.device;
    if (!dev) return DDERR_GENERIC;
    g_rvk.Frame();
    dev->Clear(count, reinterpret_cast<const rvk::Device::Rect*>(rects), flags & (D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER), color, z);
    return D3D_OK;
}

HRESULT RDevice::DoSetTransform(D3DTRANSFORMSTATETYPE type, LPD3DMATRIX m)
{
    if (!m || DWORD(type) >= 32) return DDERR_INVALIDPARAMS;
    m_transforms[type] = *m;
    if (rvk::Device* dev = g_rvk.device)
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
    if (rvk::Device* dev = g_rvk.device)
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
    if (rvk::Device* dev = g_rvk.device)
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
    if (rvk::Device* dev = g_rvk.device)
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
    if (rvk::Device* dev = g_rvk.device)
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
    if (rvk::Device* dev = g_rvk.device)
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
    if (rvk::Device* dev = g_rvk.device)
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
    if (rvk::Device* dev = g_rvk.device)
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

HRESULT RDevice::DoDrawPrimitive(D3DPRIMITIVETYPE type, DWORD fvf, LPVOID verts, DWORD count, DWORD)
{
    rvk::Device* dev = g_rvk.device;
    if (!dev || !verts) return DDERR_INVALIDPARAMS;
    g_rvk.Frame();
    dev->DrawPrimitive(type, fvf, verts, count);
    return D3D_OK;
}

HRESULT RDevice::DoDrawIndexedPrimitive(D3DPRIMITIVETYPE type, DWORD fvf, LPVOID verts, DWORD vcount, LPWORD idx,
                                        DWORD icount, DWORD)
{
    rvk::Device* dev = g_rvk.device;
    if (!dev || !verts || !idx) return DDERR_INVALIDPARAMS;
    g_rvk.Frame();
    dev->DrawIndexedPrimitive(type, fvf, verts, vcount, idx, icount);
    return D3D_OK;
}

HRESULT RDevice::DoDrawPrimitiveVB(D3DPRIMITIVETYPE type, LPDIRECT3DVERTEXBUFFER7 iface, DWORD start, DWORD count, DWORD)
{
    auto* vb = static_cast<RVertexBuffer*>(iface);
    rvk::Device* dev = g_rvk.device;
    if (!dev || !vb || start + count > vb->desc.dwNumVertices) return DDERR_INVALIDPARAMS;
    g_rvk.Frame();
    dev->DrawPrimitive(type, vb->desc.dwFVF, vb->data.data() + size_t(start) * vb->stride, count);
    return D3D_OK;
}

HRESULT RDevice::DoDrawIndexedPrimitiveVB(D3DPRIMITIVETYPE type, LPDIRECT3DVERTEXBUFFER7 iface, DWORD start, DWORD vcount,
                                          LPWORD idx, DWORD icount, DWORD)
{
    // D3D7: the indices are relative to start; vcount vertices from there are referenced.
    auto* vb = static_cast<RVertexBuffer*>(iface);
    rvk::Device* dev = g_rvk.device;
    if (!dev || !vb || !idx || start + vcount > vb->desc.dwNumVertices) return DDERR_INVALIDPARAMS;
    g_rvk.Frame();
    dev->DrawIndexedPrimitive(type, vb->desc.dwFVF, vb->data.data() + size_t(start) * vb->stride, vcount, idx, icount);
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
    data.assign(size_t(stride) * desc.dwNumVertices, 0);
}

HRESULT RVertexBuffer::DoLock(DWORD, LPVOID* out, LPDWORD size)
{
    if (!out) return DDERR_INVALIDPARAMS;
    *out = data.data();
    if (size) *size = DWORD(data.size());
    return D3D_OK;
}

HRESULT RVertexBuffer::DoGetVertexBufferDesc(LPD3DVERTEXBUFFERDESC out)
{
    if (!out) return DDERR_INVALIDPARAMS;
    *out = desc;
    return D3D_OK;
}

}  // namespace rvkproxy
