// rvk backend: IDirectDrawSurface7.
//
// Kinds: Primary (the window; a Blt from the back buffer presents), Main (back buffer = rvk main target),
// RenderTarget (texture + 3D device = rvk render target), Depth (bookkeeping only; rvk has one depth
// buffer), Texture (one level of a mip chain; CPU shadow per level, uploaded on Unlock) and Plain
// (system memory / offscreen plain: CPU only).
#include "rvk_backend.h"

#include <algorithm>
#include <cstring>

namespace rvkproxy {

namespace {

uint32_t LevelCount(uint32_t w, uint32_t h)
{
    uint32_t n = 1;
    for (uint32_t s = std::max(w, h); s > 1; s >>= 1)
        ++n;
    return n;
}

uint32_t BytesPerPixel(const RSurface* s)
{
    if (s->palettized) return 1;
    if (s->convert24) return 3;
    return s->desc.ddpfPixelFormat.dwRGBBitCount ? s->desc.ddpfPixelFormat.dwRGBBitCount / 8
                                                 : rvk::FormatRowBytes(s->format, 1);
}

bool IsGpu(const RSurface* s)
{
    return s->kind == RSurface::Kind::Main || s->kind == RSurface::Kind::RenderTarget;
}

// Clears part of a GPU surface ignoring the current viewport (Blt colour/depth fill semantics).
void FillGpu(RSurface* s, const RECT* rect, uint32_t flags, uint32_t argb, float z)
{
    rvk::ThreadedDevice* dev = g_rvk.device;
    if (!dev)
        return;
    g_rvk.Frame();
    rvk::Texture* previous = dev->GetRenderTarget();
    rvk::d3d::Viewport savedViewport = dev->GetViewport();
    dev->SetRenderTarget(s->kind == RSurface::Kind::Main ? nullptr : s->RvkTexture());   // viewport = whole target
    rvk::Device::Rect r{};
    if (rect)
        r = {rect->left, rect->top, rect->right, rect->bottom};
    dev->Clear(rect ? 1 : 0, rect ? &r : nullptr, flags, argb, z);
    dev->SetRenderTarget(previous);
    dev->SetViewport(savedViewport);
}

}  // namespace

RSurface::RSurface(Kind k, const DDSURFACEDESC2& d) : kind(k), desc(d)
{
    top = this;
}

RSurface::~RSurface()
{
    if (depth) depth->Release();
    if (clipper) clipper->Release();
    if (palette) palette->Release();
    if (nextLevel) nextLevel->Release();
    if (top == this && texture && g_rvk.device)
        g_rvk.device->DestroyTexture(texture);
    if (g_rvk.mainSurface == this)
        g_rvk.mainSurface = nullptr;
}

HRESULT RSurface::Create(RDirectDraw* owner, const DDSURFACEDESC2& requested, RSurface** out)
{
    DDSURFACEDESC2 d = requested;
    d.dwSize = sizeof(d);
    DWORD caps = (d.dwFlags & DDSD_CAPS) ? d.ddsCaps.dwCaps : 0;
    Kind kind;
    if (caps & DDSCAPS_PRIMARYSURFACE) kind = Kind::Primary;
    else if (caps & DDSCAPS_ZBUFFER) kind = Kind::Depth;
    else if ((caps & DDSCAPS_TEXTURE) && (caps & DDSCAPS_3DDEVICE)) kind = Kind::RenderTarget;
    else if (caps & DDSCAPS_3DDEVICE) kind = Kind::Main;
    else if (caps & DDSCAPS_TEXTURE) kind = Kind::Texture;
    else kind = Kind::Plain;

    if (kind == Kind::Primary) {
        d.dwWidth = g_rvk.displayWidth ? g_rvk.displayWidth : DWORD(GetSystemMetrics(SM_CXSCREEN));
        d.dwHeight = g_rvk.displayHeight ? g_rvk.displayHeight : DWORD(GetSystemMetrics(SM_CYSCREEN));
    }
    if (!(d.dwFlags & DDSD_WIDTH) && kind != Kind::Primary) {
        RvkLog("CreateSurface without a size (caps 0x%lx)", caps);
        return DDERR_INVALIDPARAMS;
    }

    // Pixel format: as requested, else the display format.
    rvk::Format format = rvk::Format::X8R8G8B8;
    bool convert24 = false, palettized = false;
    if (kind == Kind::Depth) {
        // rvk has one D32 depth buffer; report back what was asked for. A 32-bit z-buffer with a full 32-bit
        // mask is refused like D3D7 (D7VK) does, so the game falls back to 24-bit as it normally would.
        if ((d.dwFlags & DDSD_PIXELFORMAT) && d.ddpfPixelFormat.dwZBufferBitDepth == 32 &&
            d.ddpfPixelFormat.dwZBitMask == 0xFFFFFFFF)
            return DDERR_INVALIDPIXELFORMAT;
    } else if (d.dwFlags & DDSD_PIXELFORMAT) {
        if (!FormatFromPixelFormat(d.ddpfPixelFormat, &format, &convert24, &palettized)) {
            RvkLog("CreateSurface: unsupported pixel format (flags 0x%lx, %lu bpp, fourcc %.4s)", d.ddpfPixelFormat.dwFlags,
                   d.ddpfPixelFormat.dwRGBBitCount, reinterpret_cast<const char*>(&d.ddpfPixelFormat.dwFourCC));
            return DDERR_INVALIDPIXELFORMAT;
        }
        bool gpu = kind == Kind::Main || kind == Kind::RenderTarget;
        if (gpu && format != rvk::Format::X8R8G8B8 && format != rvk::Format::A8R8G8B8) {
            RvkLog("CreateSurface: render target in a non-32-bit format");
            return DDERR_INVALIDPIXELFORMAT;
        }
    } else {
        PixelFormatFor(format, &d.ddpfPixelFormat);
    }
    d.dwFlags |= DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    if (kind != Kind::Plain && kind != Kind::Primary && !(caps & DDSCAPS_SYSTEMMEMORY) &&
        !(d.ddsCaps.dwCaps2 & (DDSCAPS2_TEXTUREMANAGE | DDSCAPS2_D3DTEXTUREMANAGE)))
        d.ddsCaps.dwCaps |= DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM;

    auto* s = new RSurface(kind, d);
    s->owner = owner;
    s->format = format;
    s->convert24 = convert24;
    s->palettized = palettized;

    switch (kind) {
    case Kind::Main:
        if (!g_rvk.EnsureDevice(d.dwWidth, d.dwHeight)) {
            s->Release();
            return DDERR_GENERIC;
        }
        g_rvk.mainSurface = s;
        break;
    case Kind::Primary:
        if ((caps & DDSCAPS_FLIP) && (d.dwFlags & DDSD_BACKBUFFERCOUNT) && d.dwBackBufferCount) {
            // Fullscreen flip chain: the back buffer is the rvk main target.
            DDSURFACEDESC2 bd = d;
            bd.dwFlags &= ~DDSD_BACKBUFFERCOUNT;
            bd.ddsCaps.dwCaps = DDSCAPS_BACKBUFFER | DDSCAPS_3DDEVICE | DDSCAPS_FLIP | DDSCAPS_COMPLEX |
                                DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM;
            if (!g_rvk.EnsureDevice(d.dwWidth, d.dwHeight)) {
                s->Release();
                return DDERR_GENERIC;
            }
            auto* back = new RSurface(Kind::Main, bd);
            back->owner = owner;
            back->format = format;
            g_rvk.mainSurface = back;
            s->nextLevel = back;
        }
        break;
    case Kind::Texture: {
        uint32_t full = LevelCount(d.dwWidth, d.dwHeight);
        uint32_t levels = (d.dwFlags & DDSD_MIPMAPCOUNT) ? std::clamp<uint32_t>(d.dwMipMapCount, 1, full)
                        : ((caps & DDSCAPS_MIPMAP) && (caps & DDSCAPS_COMPLEX)) ? full : 1;
        s->levels = levels;
        if (levels > 1) {
            s->desc.dwFlags |= DDSD_MIPMAPCOUNT;
            s->desc.dwMipMapCount = levels;
        }
        RSurface* prev = s;
        for (uint32_t i = 1; i < levels; ++i) {
            DDSURFACEDESC2 ld = s->desc;
            ld.dwWidth = std::max<DWORD>(1, d.dwWidth >> i);
            ld.dwHeight = std::max<DWORD>(1, d.dwHeight >> i);
            ld.dwMipMapCount = levels - i;
            ld.ddsCaps.dwCaps |= DDSCAPS_MIPMAP;
            ld.ddsCaps.dwCaps2 |= DDSCAPS2_MIPMAPSUBLEVEL;
            auto* l = new RSurface(Kind::Texture, ld);
            l->owner = owner;
            l->format = format;
            l->convert24 = convert24;
            l->palettized = palettized;
            l->level = i;
            l->levels = levels;
            l->top = s;
            prev->nextLevel = l;
            prev = l;
        }
        break;
    }
    default:
        break;
    }
    *out = s;
    return DD_OK;
}

uint32_t RSurface::Pitch() const
{
    if (rvk::FormatIsCompressed(format))
        return rvk::FormatRowBytes(format, desc.dwWidth);
    return desc.dwWidth * BytesPerPixel(this);
}

void RSurface::EnsureShadow()
{
    size_t size = size_t(Pitch()) * rvk::FormatRows(format, desc.dwHeight);
    if (shadow.size() != size)
        shadow.assign(size, 0);
}

rvk::Texture* RSurface::RvkTexture()
{
    rvk::ThreadedDevice* dev = g_rvk.device;
    if (!dev)
        return nullptr;
    if (kind == Kind::RenderTarget) {
        if (!texture)
            texture = dev->CreateRenderTarget(desc.dwWidth, desc.dwHeight);
        return texture;
    }
    if (kind != Kind::Texture)
        return nullptr;
    RSurface* t = top;
    if (!t->texture) {
        t->texture = dev->CreateTexture(t->desc.dwWidth, t->desc.dwHeight, t->format, t->levels);
        AttachMaterialMaps(t);
        for (RSurface* l = t; l; l = l->nextLevel)
            t->anyDirty |= (l->dirty = !l->shadow.empty());
    }
    if (t->anyDirty) {                             // SetTexture is hot: walk the chain only when needed
        t->anyDirty = false;
        for (RSurface* l = t; l; l = l->nextLevel)
            if (l->dirty)
                l->Upload();
    }
    return t->texture;
}

void RSurface::Upload()
{
    if (kind != Kind::Texture)
        return;
    rvk::ThreadedDevice* dev = g_rvk.device;
    RSurface* t = top;
    if (!dev || shadow.empty()) {
        dirty = !shadow.empty();
        t->anyDirty |= dirty;
        return;
    }
    if (!t->texture) {
        t->texture = dev->CreateTexture(t->desc.dwWidth, t->desc.dwHeight, t->format, t->levels);
        if (!t->texture) return;
        AttachMaterialMaps(t);
    }
    dirty = false;
    uint32_t w = desc.dwWidth, h = desc.dwHeight;
    if (convert24 || palettized) {
        std::vector<uint32_t> argb(size_t(w) * h);
        const PALETTEENTRY* pal = t->palette ? t->palette->entries : nullptr;
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                const uint8_t* p = shadow.data() + size_t(y) * Pitch() + x * BytesPerPixel(this);
                if (palettized) {
                    PALETTEENTRY e = pal ? pal[*p] : PALETTEENTRY{*p, *p, *p, 0xFF};
                    argb[y * w + x] = 0xFF000000u | (uint32_t(e.peRed) << 16) | (uint32_t(e.peGreen) << 8) | e.peBlue;
                } else {
                    argb[y * w + x] = 0xFF000000u | (uint32_t(p[2]) << 16) | (uint32_t(p[1]) << 8) | p[0];
                }
            }
        dev->UpdateTexture(t->texture, level, 0, 0, w, h, argb.data(), w * 4);
    } else {
        dev->UpdateTexture(t->texture, level, 0, 0, w, h, shadow.data(), Pitch());
    }
}

HRESULT RSurface::DoAddAttachedSurface(LPDIRECTDRAWSURFACE7 s)
{
    auto* z = static_cast<RSurface*>(s);
    if (!z || z->kind != Kind::Depth)
        return DDERR_CANNOTATTACHSURFACE;
    if (depth) depth->Release();
    depth = z;
    z->AddRef();
    return DD_OK;
}

HRESULT RSurface::DoDeleteAttachedSurface(DWORD, LPDIRECTDRAWSURFACE7 s)
{
    if (depth && (!s || s == static_cast<IDirectDrawSurface7*>(depth))) {
        depth->Release();
        depth = nullptr;
        return DD_OK;
    }
    return DDERR_SURFACENOTATTACHED;
}

HRESULT RSurface::DoBlt(LPRECT dstRect, LPDIRECTDRAWSURFACE7 srcIface, LPRECT srcRect, DWORD flags, LPDDBLTFX fx)
{
    auto* src = static_cast<RSurface*>(srcIface);
    rvk::ThreadedDevice* dev = g_rvk.device;

    if (kind == Kind::Primary) {
        if (src && src->kind == Kind::Main)
            g_rvk.Present();
        return DD_OK;                                   // fills/copies onto the desktop are ignored
    }
    if (flags & DDBLT_COLORFILL) {
        if (!fx) return DDERR_INVALIDPARAMS;
        DWORD color = fx->dwFillColor;
        if (IsGpu(this)) {
            FillGpu(this, dstRect, rvk::d3d::CLEAR_TARGET, format == rvk::Format::X8R8G8B8 ? (color | 0xFF000000u) : color, 1.0f);
            return DD_OK;
        }
        if (rvk::FormatIsCompressed(format) || convert24 || palettized)
            return DDERR_UNSUPPORTED;
        EnsureShadow();
        uint32_t bpp = BytesPerPixel(this);
        RECT r = dstRect ? *dstRect : RECT{0, 0, LONG(desc.dwWidth), LONG(desc.dwHeight)};
        for (LONG y = std::max(0L, r.top); y < std::min(LONG(desc.dwHeight), r.bottom); ++y)
            for (LONG x = std::max(0L, r.left); x < std::min(LONG(desc.dwWidth), r.right); ++x)
                std::memcpy(shadow.data() + size_t(y) * Pitch() + size_t(x) * bpp, &color, bpp);
        Upload();
        return DD_OK;
    }
    if (flags & DDBLT_DEPTHFILL) {
        if (kind != Kind::Depth || !fx) return DDERR_INVALIDPARAMS;
        DWORD bits = desc.ddpfPixelFormat.dwZBufferBitDepth ? desc.ddpfPixelFormat.dwZBufferBitDepth : 32;
        DWORD mask = desc.ddpfPixelFormat.dwZBitMask ? desc.ddpfPixelFormat.dwZBitMask : 0xFFFFFFFF;
        float z = mask ? float(double(fx->dwFillDepth & mask) / double(mask)) : 1.0f;
        (void)bits;
        if (g_rvk.mainSurface)
            FillGpu(g_rvk.mainSurface, dstRect, rvk::d3d::CLEAR_ZBUFFER, 0, z);
        return DD_OK;
    }
    if (!src)
        return DDERR_INVALIDPARAMS;
    if (flags & (DDBLT_KEYSRC | DDBLT_KEYDEST | DDBLT_KEYSRCOVERRIDE | DDBLT_KEYDESTOVERRIDE)) {
        static bool logged;
        if (!logged) { logged = true; RvkLog("Blt with colour keys: keys ignored"); }
    }
    RECT s = srcRect ? *srcRect : RECT{0, 0, LONG(src->desc.dwWidth), LONG(src->desc.dwHeight)};
    RECT d = dstRect ? *dstRect : RECT{0, 0, LONG(desc.dwWidth), LONG(desc.dwHeight)};

    if (IsGpu(this)) {
        if (!dev) return DDERR_GENERIC;
        rvk::Texture* dstTex = kind == Kind::Main ? nullptr : RvkTexture();
        rvk::Texture* srcTex = nullptr;
        rvk::Texture* temp = nullptr;
        if (src->kind == Kind::Main) srcTex = nullptr;
        else if (src->kind == Kind::RenderTarget || src->kind == Kind::Texture) srcTex = src->RvkTexture();
        if (src->kind == Kind::Plain) {
            // CPU source: upload into a temporary texture (uploads run before this frame's draws).
            if (src->convert24 || src->palettized || rvk::FormatIsCompressed(src->format)) return DDERR_UNSUPPORTED;
            src->EnsureShadow();
            temp = dev->CreateTexture(src->desc.dwWidth, src->desc.dwHeight, src->format, 1);
            if (!temp) return DDERR_OUTOFVIDEOMEMORY;
            dev->UpdateTexture(temp, 0, 0, 0, src->desc.dwWidth, src->desc.dwHeight, src->shadow.data(), src->Pitch());
            srcTex = temp;
        } else if (src->kind != Kind::Main && !srcTex) {
            return DDERR_UNSUPPORTED;
        }
        g_rvk.Frame();
        rvk::Device::Rect dr{d.left, d.top, d.right, d.bottom}, sr{s.left, s.top, s.right, s.bottom};
        bool stretch = (d.right - d.left) != (s.right - s.left) || (d.bottom - d.top) != (s.bottom - s.top);
        dev->CopyTexture(dstTex, &dr, srcTex, &sr, stretch);
        if (temp) dev->DestroyTexture(temp);
        return DD_OK;
    }

    // CPU destination.
    if (kind == Kind::Depth || kind == Kind::Primary) return DDERR_UNSUPPORTED;
    std::vector<uint8_t> gpuPixels;
    const uint8_t* srcData;
    uint32_t srcPitch, srcBpp;
    if (IsGpu(src)) {
        if (!dev || (format != rvk::Format::X8R8G8B8 && format != rvk::Format::A8R8G8B8)) return DDERR_UNSUPPORTED;
        gpuPixels.resize(size_t(src->desc.dwWidth) * src->desc.dwHeight * 4);
        dev->ReadPixels(src->kind == Kind::Main ? nullptr : src->RvkTexture(), gpuPixels.data());
        srcData = gpuPixels.data();
        srcPitch = src->desc.dwWidth * 4;
        srcBpp = 4;
    } else {
        if (src->format != format || src->convert24 != convert24 || src->palettized != palettized) {
            RvkLog("Blt between different formats (%u -> %u) not supported", uint32_t(src->format), uint32_t(format));
            return DDERR_UNSUPPORTED;
        }
        src->EnsureShadow();
        srcData = src->shadow.data();
        srcPitch = src->Pitch();
        srcBpp = BytesPerPixel(src);
    }
    EnsureShadow();
    if (rvk::FormatIsCompressed(format)) {
        // Block copies only: same size, 4-aligned.
        if ((s.right - s.left) != (d.right - d.left) || (s.bottom - s.top) != (d.bottom - d.top)) return DDERR_UNSUPPORTED;
        uint32_t block = rvk::FormatRowBytes(format, 4);
        for (LONG by = 0; by < (s.bottom - s.top + 3) / 4; ++by)
            std::memcpy(shadow.data() + size_t(d.top / 4 + by) * Pitch() + size_t(d.left / 4) * block,
                        srcData + size_t(s.top / 4 + by) * srcPitch + size_t(s.left / 4) * block,
                        size_t((s.right - s.left + 3) / 4) * block);
    } else {
        uint32_t bpp = BytesPerPixel(this);
        LONG dw = d.right - d.left, dh = d.bottom - d.top, sw = s.right - s.left, sh = s.bottom - s.top;
        for (LONG y = 0; y < dh; ++y) {
            LONG dy = d.top + y, sy = s.top + y * sh / std::max(dh, 1L);
            if (dy < 0 || dy >= LONG(desc.dwHeight)) continue;
            for (LONG x = 0; x < dw; ++x) {
                LONG dx = d.left + x, sx = s.left + x * sw / std::max(dw, 1L);
                if (dx < 0 || dx >= LONG(desc.dwWidth)) continue;
                std::memcpy(shadow.data() + size_t(dy) * Pitch() + size_t(dx) * bpp,
                            srcData + size_t(sy) * srcPitch + size_t(sx) * srcBpp, bpp);
            }
        }
    }
    Upload();
    return DD_OK;
}

HRESULT RSurface::DoBltFast(DWORD x, DWORD y, LPDIRECTDRAWSURFACE7 srcIface, LPRECT srcRect, DWORD flags)
{
    auto* src = static_cast<RSurface*>(srcIface);
    if (!src) return DDERR_INVALIDPARAMS;
    RECT s = srcRect ? *srcRect : RECT{0, 0, LONG(src->desc.dwWidth), LONG(src->desc.dwHeight)};
    RECT d{LONG(x), LONG(y), LONG(x) + (s.right - s.left), LONG(y) + (s.bottom - s.top)};
    DWORD bltFlags = (flags & DDBLTFAST_SRCCOLORKEY) ? DDBLT_KEYSRC : 0;
    return DoBlt(&d, srcIface, &s, bltFlags, nullptr);
}

HRESULT RSurface::DoFlip(LPDIRECTDRAWSURFACE7, DWORD)
{
    if (kind == Kind::Primary)
        g_rvk.Present();
    return DD_OK;
}

HRESULT RSurface::DoGetAttachedSurface(LPDDSCAPS2 caps, LPDIRECTDRAWSURFACE7* out)
{
    if (!caps || !out) return DDERR_INVALIDPARAMS;
    RSurface* found = nullptr;
    if ((caps->dwCaps & DDSCAPS_ZBUFFER) && depth) found = depth;
    else if ((caps->dwCaps & (DDSCAPS_MIPMAP | DDSCAPS_TEXTURE)) && kind == Kind::Texture) found = nextLevel;
    else if ((caps->dwCaps & DDSCAPS_BACKBUFFER) && kind == Kind::Primary) found = nextLevel;
    if (!found) {
        *out = nullptr;
        return DDERR_NOTFOUND;
    }
    found->AddRef();
    *out = found;
    return DD_OK;
}

HRESULT RSurface::DoGetCaps(LPDDSCAPS2 caps)
{
    if (!caps) return DDERR_INVALIDPARAMS;
    *caps = desc.ddsCaps;
    return DD_OK;
}

HRESULT RSurface::DoGetClipper(LPDIRECTDRAWCLIPPER* out)
{
    if (!out) return DDERR_INVALIDPARAMS;
    *out = clipper;
    if (!clipper) return DDERR_NOCLIPPERATTACHED;
    clipper->AddRef();
    return DD_OK;
}

static int KeyIndex(DWORD flags)
{
    if (flags & DDCKEY_DESTBLT) return 0;
    if (flags & DDCKEY_DESTOVERLAY) return 1;
    if (flags & DDCKEY_SRCBLT) return 2;
    if (flags & DDCKEY_SRCOVERLAY) return 3;
    return -1;
}

HRESULT RSurface::DoGetColorKey(DWORD flags, LPDDCOLORKEY key)
{
    int i = KeyIndex(flags);
    if (i < 0 || !key) return DDERR_INVALIDPARAMS;
    static const DWORD sdFlags[4] = {DDSD_CKDESTBLT, DDSD_CKDESTOVERLAY, DDSD_CKSRCBLT, DDSD_CKSRCOVERLAY};
    if (!(desc.dwFlags & sdFlags[i])) return DDERR_NOCOLORKEY;
    *key = m_keys[i];
    return DD_OK;
}

HRESULT RSurface::DoSetColorKey(DWORD flags, LPDDCOLORKEY key)
{
    int i = KeyIndex(flags);
    if (i < 0) return DDERR_INVALIDPARAMS;
    static const DWORD sdFlags[4] = {DDSD_CKDESTBLT, DDSD_CKDESTOVERLAY, DDSD_CKSRCBLT, DDSD_CKSRCOVERLAY};
    if (key) {
        m_keys[i] = *key;
        desc.dwFlags |= sdFlags[i];
    } else {
        desc.dwFlags &= ~sdFlags[i];
    }
    return DD_OK;
}

HRESULT RSurface::DoGetPalette(LPDIRECTDRAWPALETTE* out)
{
    if (!out) return DDERR_INVALIDPARAMS;
    *out = palette;
    if (!palette) return DDERR_NOPALETTEATTACHED;
    palette->AddRef();
    return DD_OK;
}

HRESULT RSurface::DoSetPalette(LPDIRECTDRAWPALETTE p)
{
    auto* pal = static_cast<RPalette*>(p);
    if (pal) pal->AddRef();
    if (palette) palette->Release();
    palette = pal;
    if (palettized)                             // re-convert every level with the new palette
        for (RSurface* l = top; l; l = l->nextLevel)
            l->Upload();
    return DD_OK;
}

HRESULT RSurface::DoGetPixelFormat(LPDDPIXELFORMAT pf)
{
    if (!pf) return DDERR_INVALIDPARAMS;
    *pf = desc.ddpfPixelFormat;
    return DD_OK;
}

HRESULT RSurface::DoGetSurfaceDesc(LPDDSURFACEDESC2 out)
{
    if (!out || out->dwSize < sizeof(DDSURFACEDESC2)) return DDERR_INVALIDPARAMS;
    *out = desc;
    out->lpSurface = nullptr;
    if (rvk::FormatIsCompressed(format)) {
        out->dwFlags = (out->dwFlags & ~DDSD_PITCH) | DDSD_LINEARSIZE;
        out->dwLinearSize = Pitch() * rvk::FormatRows(format, desc.dwHeight);
    } else if (kind != Kind::Depth) {
        out->dwFlags |= DDSD_PITCH;
        out->lPitch = LONG(kind == Kind::Main || kind == Kind::RenderTarget ? desc.dwWidth * 4 : Pitch());
    }
    return DD_OK;
}

HRESULT RSurface::DoLock(LPRECT rect, LPDDSURFACEDESC2 out, DWORD flags, HANDLE)
{
    if (!out || out->dwSize < sizeof(DDSURFACEDESC2)) return DDERR_INVALIDPARAMS;
    if (kind == Kind::Primary || kind == Kind::Depth) {
        RvkLog("Lock on a %s surface not supported", kind == Kind::Primary ? "primary" : "depth");
        return DDERR_UNSUPPORTED;
    }
    if (m_locked) return DDERR_SURFACEBUSY;
    uint32_t pitch;
    if (IsGpu(this)) {
        shadow.assign(size_t(desc.dwWidth) * desc.dwHeight * 4, 0);
        if (g_rvk.device)
            g_rvk.device->ReadPixels(kind == Kind::Main ? nullptr : RvkTexture(), shadow.data());
        pitch = desc.dwWidth * 4;
    } else {
        EnsureShadow();
        pitch = Pitch();
    }
    size_t offset = 0;
    if (rect) {
        if (rvk::FormatIsCompressed(format))
            offset = size_t(rect->top / 4) * pitch + size_t(rect->left / 4) * rvk::FormatRowBytes(format, 4);
        else
            offset = size_t(rect->top) * pitch + size_t(rect->left) * (IsGpu(this) ? 4 : BytesPerPixel(this));
    }
    *out = desc;
    out->dwFlags |= DDSD_LPSURFACE | DDSD_PITCH;
    out->lPitch = LONG(pitch);
    out->lpSurface = shadow.data() + offset;
    m_locked = true;
    m_lockFlags = flags;
    m_lockRect = rect ? *rect : RECT{0, 0, LONG(desc.dwWidth), LONG(desc.dwHeight)};
    return DD_OK;
}

HRESULT RSurface::DoUnlock(LPRECT)
{
    if (!m_locked) return DDERR_NOTLOCKED;
    m_locked = false;
    if (m_lockFlags & DDLOCK_READONLY)
        return DD_OK;
    if (kind == Kind::Texture) {
        Upload();
    } else if (IsGpu(this)) {
        static bool logged;
        if (!logged) { logged = true; RvkLog("writes to a render target through Lock are ignored"); }
    }
    return DD_OK;
}

HRESULT RSurface::DoSetClipper(LPDIRECTDRAWCLIPPER c)
{
    auto* clip = static_cast<RClipper*>(c);
    if (clip) clip->AddRef();
    if (clipper) clipper->Release();
    clipper = clip;
    if (clip && clip->window)
        g_rvk.SetWindow(clip->window);
    return DD_OK;
}

HRESULT RSurface::DoGetDDInterface(LPVOID* out)
{
    if (!out) return DDERR_INVALIDPARAMS;
    *out = owner ? static_cast<IDirectDraw7*>(owner) : nullptr;
    if (!owner) return DDERR_GENERIC;
    owner->AddRef();
    return DD_OK;
}

static std::string GuidKey(REFGUID g) { return std::string(reinterpret_cast<const char*>(&g), sizeof(GUID)); }

HRESULT RSurface::DoSetPrivateData(REFGUID guid, LPVOID data, DWORD size, DWORD flags)
{
    if (flags & DDSPD_IUNKNOWNPOINTER) {
        RvkLog("SetPrivateData with an IUnknown pointer not supported");
        return DDERR_UNSUPPORTED;
    }
    auto& v = m_private[GuidKey(guid)];
    v.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
    return DD_OK;
}

HRESULT RSurface::DoGetPrivateData(REFGUID guid, LPVOID data, LPDWORD size)
{
    auto it = m_private.find(GuidKey(guid));
    if (it == m_private.end()) return DDERR_NOTFOUND;
    if (!size) return DDERR_INVALIDPARAMS;
    if (!data || *size < it->second.size()) {
        *size = DWORD(it->second.size());
        return DDERR_MOREDATA;
    }
    std::memcpy(data, it->second.data(), it->second.size());
    *size = DWORD(it->second.size());
    return DD_OK;
}

HRESULT RSurface::DoFreePrivateData(REFGUID guid)
{
    return m_private.erase(GuidKey(guid)) ? DD_OK : DDERR_NOTFOUND;
}

HRESULT RSurface::DoGetUniquenessValue(LPDWORD out)
{
    if (!out) return DDERR_INVALIDPARAMS;
    *out = m_uniqueness;
    return DD_OK;
}

}  // namespace rvkproxy
