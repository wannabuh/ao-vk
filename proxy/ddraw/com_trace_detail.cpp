// Argument logging for selected COM calls in trace mode (see DETAIL in tools/gen_com_trace.py).
// The first kMaxLines calls of each method go to the log one by one; all calls are also counted per
// distinct description, printed by ComDetailSummary() in inspector captures.
#include "com_trace.h"

#include <cstdio>
#include <map>
#include <mutex>
#include <string>

namespace rvkproxy {
#include "com_trace.gen.h"

namespace {

constexpr unsigned kMaxLines = 200;
std::mutex g_detailMutex;
std::map<std::string, unsigned> g_lines;           // method -> lines logged
std::map<std::string, uint32_t> g_summary;         // description -> calls

std::string Format(const char* fmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    return buf;
}

// method: "Interface::Method"; summary: what to count (no addresses); detail: extra per-call info to log.
void Detail(const char* method, const std::string& summary, const std::string& detail = "")
{
    std::lock_guard<std::mutex> lock(g_detailMutex);
    ++g_summary[std::string(method) + " " + summary];
    if (g_lines[method]++ >= kMaxLines)
        return;
    char path[MAX_PATH] = "randy-vk.log";
    GetEnvironmentVariableA("RANDYVK_LOG", path, sizeof(path));
    if (FILE* f = std::fopen(path, "a")) {
        std::fprintf(f, "ddraw %s %s%s%s\n", method, summary.c_str(), detail.empty() ? "" : " ", detail.c_str());
        std::fclose(f);
    }
}

std::string Flags(DWORD value, const std::pair<DWORD, const char*>* names, size_t count)
{
    std::string s;
    for (size_t i = 0; i < count; ++i)
        if (value & names[i].first) {
            s += s.empty() ? "" : "|";
            s += names[i].second;
            value &= ~names[i].first;
        }
    if (value)
        s += Format("%s0x%lx", s.empty() ? "" : "|", value);
    return s.empty() ? "0" : s;
}

#define F(x) {x, #x}
const std::pair<DWORD, const char*> kCaps[] = {
    F(DDSCAPS_ALPHA), F(DDSCAPS_BACKBUFFER), F(DDSCAPS_COMPLEX), F(DDSCAPS_FLIP), F(DDSCAPS_FRONTBUFFER),
    F(DDSCAPS_OFFSCREENPLAIN), F(DDSCAPS_OVERLAY), F(DDSCAPS_PALETTE), F(DDSCAPS_PRIMARYSURFACE),
    F(DDSCAPS_SYSTEMMEMORY), F(DDSCAPS_TEXTURE), F(DDSCAPS_3DDEVICE), F(DDSCAPS_VIDEOMEMORY), F(DDSCAPS_VISIBLE),
    F(DDSCAPS_WRITEONLY), F(DDSCAPS_ZBUFFER), F(DDSCAPS_OWNDC), F(DDSCAPS_MIPMAP), F(DDSCAPS_ALLOCONLOAD),
    F(DDSCAPS_LOCALVIDMEM), F(DDSCAPS_NONLOCALVIDMEM), F(DDSCAPS_STANDARDVGAMODE), F(DDSCAPS_OPTIMIZED)};
const std::pair<DWORD, const char*> kCaps2[] = {
    F(DDSCAPS2_HINTDYNAMIC), F(DDSCAPS2_HINTSTATIC), F(DDSCAPS2_TEXTUREMANAGE), F(DDSCAPS2_OPAQUE),
    F(DDSCAPS2_HINTANTIALIASING), F(DDSCAPS2_CUBEMAP), F(DDSCAPS2_MIPMAPSUBLEVEL), F(DDSCAPS2_D3DTEXTUREMANAGE),
    F(DDSCAPS2_DONOTPERSIST), F(DDSCAPS2_STEREOSURFACELEFT)};
const std::pair<DWORD, const char*> kSdFlags[] = {
    F(DDSD_CAPS), F(DDSD_HEIGHT), F(DDSD_WIDTH), F(DDSD_PITCH), F(DDSD_BACKBUFFERCOUNT), F(DDSD_ZBUFFERBITDEPTH),
    F(DDSD_ALPHABITDEPTH), F(DDSD_LPSURFACE), F(DDSD_PIXELFORMAT), F(DDSD_CKDESTOVERLAY), F(DDSD_CKDESTBLT),
    F(DDSD_CKSRCOVERLAY), F(DDSD_CKSRCBLT), F(DDSD_MIPMAPCOUNT), F(DDSD_REFRESHRATE), F(DDSD_LINEARSIZE),
    F(DDSD_TEXTURESTAGE), F(DDSD_FVF), F(DDSD_SRCVBHANDLE), F(DDSD_DEPTH)};
const std::pair<DWORD, const char*> kLock[] = {
    F(DDLOCK_WAIT), F(DDLOCK_EVENT), F(DDLOCK_READONLY), F(DDLOCK_WRITEONLY), F(DDLOCK_NOSYSLOCK),
    F(DDLOCK_NOOVERWRITE), F(DDLOCK_DISCARDCONTENTS), F(DDLOCK_DONOTWAIT)};
const std::pair<DWORD, const char*> kBlt[] = {
    F(DDBLT_ALPHADEST), F(DDBLT_ALPHASRC), F(DDBLT_ASYNC), F(DDBLT_COLORFILL), F(DDBLT_DDFX), F(DDBLT_DDROPS),
    F(DDBLT_KEYDEST), F(DDBLT_KEYSRC), F(DDBLT_KEYDESTOVERRIDE), F(DDBLT_KEYSRCOVERRIDE), F(DDBLT_ROP),
    F(DDBLT_ROTATIONANGLE), F(DDBLT_ZBUFFER), F(DDBLT_WAIT), F(DDBLT_DEPTHFILL), F(DDBLT_DONOTWAIT)};
const std::pair<DWORD, const char*> kCoop[] = {
    F(DDSCL_FULLSCREEN), F(DDSCL_ALLOWREBOOT), F(DDSCL_NOWINDOWCHANGES), F(DDSCL_NORMAL), F(DDSCL_EXCLUSIVE),
    F(DDSCL_ALLOWMODEX), F(DDSCL_SETFOCUSWINDOW), F(DDSCL_SETDEVICEWINDOW), F(DDSCL_CREATEDEVICEWINDOW),
    F(DDSCL_MULTITHREADED), F(DDSCL_FPUSETUP), F(DDSCL_FPUPRESERVE)};
const std::pair<DWORD, const char*> kVbCaps[] = {
    F(D3DVBCAPS_SYSTEMMEMORY), F(D3DVBCAPS_WRITEONLY), F(D3DVBCAPS_OPTIMIZED), F(D3DVBCAPS_DONOTCLIP)};
#undef F

std::string PixelFormat(const DDPIXELFORMAT& pf)
{
    if (pf.dwFlags & DDPF_FOURCC)
        return Format("FOURCC %.4s", reinterpret_cast<const char*>(&pf.dwFourCC));
    std::string s = Format("%lubpp", pf.dwRGBBitCount);
    if (pf.dwFlags & DDPF_RGB) s += Format(" RGB %06lx/%06lx/%06lx", pf.dwRBitMask, pf.dwGBitMask, pf.dwBBitMask);
    if (pf.dwFlags & DDPF_ALPHAPIXELS) s += Format(" A %08lx", pf.dwRGBAlphaBitMask);
    if (pf.dwFlags & DDPF_LUMINANCE) s += " LUMINANCE";
    if (pf.dwFlags & DDPF_ALPHA) s += " ALPHAONLY";
    if (pf.dwFlags & DDPF_ZBUFFER) s += Format(" Z %08lx", pf.dwZBitMask);
    if (pf.dwFlags & DDPF_PALETTEINDEXED8) s += " P8";
    return s;
}

std::string Describe(const DDSURFACEDESC2* sd, bool withSize)
{
    if (!sd)
        return "(null desc)";
    std::string s = "flags " + Flags(sd->dwFlags, kSdFlags, std::size(kSdFlags));
    if (sd->dwFlags & DDSD_CAPS) {
        s += " caps " + Flags(sd->ddsCaps.dwCaps, kCaps, std::size(kCaps));
        if (sd->ddsCaps.dwCaps2) s += " caps2 " + Flags(sd->ddsCaps.dwCaps2, kCaps2, std::size(kCaps2));
    }
    if (withSize && (sd->dwFlags & (DDSD_WIDTH | DDSD_HEIGHT))) s += Format(" %lux%lu", sd->dwWidth, sd->dwHeight);
    if (sd->dwFlags & DDSD_PIXELFORMAT) s += " fmt " + PixelFormat(sd->ddpfPixelFormat);
    if (sd->dwFlags & DDSD_MIPMAPCOUNT) s += Format(" mips %lu", sd->dwMipMapCount);
    if (sd->dwFlags & DDSD_BACKBUFFERCOUNT) s += Format(" backbuffers %lu", sd->dwBackBufferCount);
    return s;
}

std::string SurfaceKind(IDirectDrawSurface7* real)
{
    DDSURFACEDESC2 sd{};
    sd.dwSize = sizeof(sd);
    if (!real || FAILED(real->GetSurfaceDesc(&sd)))
        return "?";
    return Flags(sd.ddsCaps.dwCaps & (DDSCAPS_PRIMARYSURFACE | DDSCAPS_BACKBUFFER | DDSCAPS_TEXTURE | DDSCAPS_ZBUFFER |
                                      DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY | DDSCAPS_MIPMAP | DDSCAPS_3DDEVICE),
                 kCaps, std::size(kCaps)) +
           Format(" %lux%lu %s", sd.dwWidth, sd.dwHeight, PixelFormat(sd.ddpfPixelFormat).c_str());
}

const char* Result(HRESULT r) { return SUCCEEDED(r) ? "ok" : "FAILED"; }

}  // namespace

void ComDetailSummary(FILE* f)
{
    std::lock_guard<std::mutex> lock(g_detailMutex);
    if (g_summary.empty())
        return;
    std::fprintf(f, "Direct3D 7 call details since start (calls  description):\n");
    for (auto& [desc, n] : g_summary)
        std::fprintf(f, "  %6u  %s\n", n, desc.c_str());
}

void TraceDetail_IDirectDraw7_CreateSurface(TraceIDirectDraw7*, HRESULT r, LPDDSURFACEDESC2 sd, LPDIRECTDRAWSURFACE7*, IUnknown*)
{
    // Texture sizes vary a lot; summarise without them but log them per call.
    Detail("IDirectDraw7::CreateSurface", Describe(sd, false) + " -> " + Result(r),
           sd ? Format("size %lux%lu", sd->dwWidth, sd->dwHeight) : "");
}

void TraceDetail_IDirectDraw7_SetCooperativeLevel(TraceIDirectDraw7*, HRESULT r, HWND, DWORD flags)
{
    Detail("IDirectDraw7::SetCooperativeLevel", Flags(flags, kCoop, std::size(kCoop)) + " -> " + Result(r));
}

void TraceDetail_IDirectDraw7_SetDisplayMode(TraceIDirectDraw7*, HRESULT r, DWORD w, DWORD h, DWORD bpp, DWORD rate, DWORD)
{
    Detail("IDirectDraw7::SetDisplayMode", Format("%lux%lux%lu @%lu -> %s", w, h, bpp, rate, Result(r)));
}

void TraceDetail_IDirectDraw7_GetAvailableVidMem(TraceIDirectDraw7*, HRESULT r, LPDDSCAPS2 caps, LPDWORD total, LPDWORD free)
{
    Detail("IDirectDraw7::GetAvailableVidMem",
           (caps ? Flags(caps->dwCaps, kCaps, std::size(kCaps)) : std::string("?")) + " -> " + Result(r),
           Format("total %lu free %lu", total ? *total : 0, free ? *free : 0));
}

void TraceDetail_IDirectDrawSurface7_AddAttachedSurface(TraceIDirectDrawSurface7* self, HRESULT r, LPDIRECTDRAWSURFACE7 s)
{
    Detail("IDirectDrawSurface7::AddAttachedSurface",
           SurfaceKind(self->Real()) + " <- " + SurfaceKind(Unwrap<TraceIDirectDrawSurface7>(s)) + " -> " + Result(r));
}

void TraceDetail_IDirectDrawSurface7_Blt(TraceIDirectDrawSurface7* self, HRESULT r, LPRECT dst, LPDIRECTDRAWSURFACE7 src,
                                         LPRECT srcRect, DWORD flags, LPDDBLTFX)
{
    std::string rects = Format("dst %s src %s", dst ? Format("%ld,%ld-%ld,%ld", dst->left, dst->top, dst->right, dst->bottom).c_str() : "all",
                               srcRect ? Format("%ld,%ld-%ld,%ld", srcRect->left, srcRect->top, srcRect->right, srcRect->bottom).c_str() : "all");
    Detail("IDirectDrawSurface7::Blt", SurfaceKind(self->Real()) + " <- " +
           (src ? SurfaceKind(Unwrap<TraceIDirectDrawSurface7>(src)) : std::string("(no source)")) + " flags " +
           Flags(flags, kBlt, std::size(kBlt)) + " -> " + Result(r), rects);
}

void TraceDetail_IDirectDrawSurface7_BltFast(TraceIDirectDrawSurface7* self, HRESULT r, DWORD, DWORD, LPDIRECTDRAWSURFACE7 src,
                                             LPRECT, DWORD flags)
{
    Detail("IDirectDrawSurface7::BltFast", SurfaceKind(self->Real()) + " <- " + SurfaceKind(Unwrap<TraceIDirectDrawSurface7>(src)) +
           Format(" flags 0x%lx -> %s", flags, Result(r)));
}

void TraceDetail_IDirectDrawSurface7_Flip(TraceIDirectDrawSurface7* self, HRESULT r, LPDIRECTDRAWSURFACE7, DWORD flags)
{
    Detail("IDirectDrawSurface7::Flip", SurfaceKind(self->Real()) + Format(" flags 0x%lx -> %s", flags, Result(r)));
}

void TraceDetail_IDirectDrawSurface7_GetAttachedSurface(TraceIDirectDrawSurface7* self, HRESULT r, LPDDSCAPS2 caps, LPDIRECTDRAWSURFACE7*)
{
    Detail("IDirectDrawSurface7::GetAttachedSurface", SurfaceKind(self->Real()) + " caps " +
           (caps ? Flags(caps->dwCaps, kCaps, std::size(kCaps)) : std::string("?")) + " -> " + Result(r));
}

void TraceDetail_IDirectDrawSurface7_GetDC(TraceIDirectDrawSurface7* self, HRESULT r, HDC*)
{
    Detail("IDirectDrawSurface7::GetDC", SurfaceKind(self->Real()) + " -> " + Result(r));
}

void TraceDetail_IDirectDrawSurface7_Lock(TraceIDirectDrawSurface7* self, HRESULT r, LPRECT rect, LPDDSURFACEDESC2, DWORD flags, HANDLE)
{
    Detail("IDirectDrawSurface7::Lock", SurfaceKind(self->Real()) + (rect ? " rect" : " whole") + " flags " +
           Flags(flags, kLock, std::size(kLock)) + " -> " + Result(r));
}

void TraceDetail_IDirectDrawSurface7_SetColorKey(TraceIDirectDrawSurface7* self, HRESULT r, DWORD flags, LPDDCOLORKEY key)
{
    Detail("IDirectDrawSurface7::SetColorKey", SurfaceKind(self->Real()) + Format(" flags 0x%lx -> %s", flags, Result(r)),
           key ? Format("key %08lx-%08lx", key->dwColorSpaceLowValue, key->dwColorSpaceHighValue) : "");
}

void TraceDetail_IDirectDrawSurface7_SetPalette(TraceIDirectDrawSurface7* self, HRESULT r, LPDIRECTDRAWPALETTE)
{
    Detail("IDirectDrawSurface7::SetPalette", SurfaceKind(self->Real()) + " -> " + Result(r));
}

void TraceDetail_IDirectDrawSurface7_SetPrivateData(TraceIDirectDrawSurface7*, HRESULT r, REFGUID guid, LPVOID, DWORD size, DWORD flags)
{
    Detail("IDirectDrawSurface7::SetPrivateData",
           Format("guid {%08lx-%04x-%04x} size %lu flags 0x%lx -> %s", guid.Data1, guid.Data2, guid.Data3, size, flags, Result(r)));
}

void TraceDetail_IDirect3D7_CreateDevice(TraceIDirect3D7*, HRESULT r, REFCLSID clsid, LPDIRECTDRAWSURFACE7 target, LPDIRECT3DDEVICE7*)
{
    Detail("IDirect3D7::CreateDevice", Format("device {%08lx-%04x-%04x} on ", clsid.Data1, clsid.Data2, clsid.Data3) +
           SurfaceKind(Unwrap<TraceIDirectDrawSurface7>(target)) + " -> " + Result(r));
}

void TraceDetail_IDirect3D7_CreateVertexBuffer(TraceIDirect3D7*, HRESULT r, LPD3DVERTEXBUFFERDESC desc, LPDIRECT3DVERTEXBUFFER7*, DWORD flags)
{
    Detail("IDirect3D7::CreateVertexBuffer",
           desc ? Format("fvf 0x%lx caps %s flags 0x%lx -> %s", desc->dwFVF, Flags(desc->dwCaps, kVbCaps, std::size(kVbCaps)).c_str(),
                         flags, Result(r))
                : std::string("(null)"),
           desc ? Format("vertices %lu", desc->dwNumVertices) : "");
}

void TraceDetail_IDirect3DDevice7_SetRenderTarget(TraceIDirect3DDevice7*, HRESULT r, LPDIRECTDRAWSURFACE7 target, DWORD)
{
    Detail("IDirect3DDevice7::SetRenderTarget", SurfaceKind(Unwrap<TraceIDirectDrawSurface7>(target)) + " -> " + Result(r));
}

void TraceDetail_IDirect3DDevice7_PreLoad(TraceIDirect3DDevice7*, HRESULT r, LPDIRECTDRAWSURFACE7 s)
{
    Detail("IDirect3DDevice7::PreLoad", SurfaceKind(Unwrap<TraceIDirectDrawSurface7>(s)) + " -> " + Result(r));
}

void TraceDetail_IDirect3DDevice7_DrawPrimitive(TraceIDirect3DDevice7*, HRESULT r, D3DPRIMITIVETYPE type, DWORD fvf, LPVOID, DWORD, DWORD flags)
{
    Detail("IDirect3DDevice7::DrawPrimitive", Format("type %d fvf 0x%lx flags 0x%lx -> %s", type, fvf, flags, Result(r)));
}

void TraceDetail_IDirect3DDevice7_DrawIndexedPrimitive(TraceIDirect3DDevice7*, HRESULT r, D3DPRIMITIVETYPE type, DWORD fvf, LPVOID,
                                                       DWORD, LPWORD, DWORD, DWORD flags)
{
    Detail("IDirect3DDevice7::DrawIndexedPrimitive", Format("type %d fvf 0x%lx flags 0x%lx -> %s", type, fvf, flags, Result(r)));
}

void TraceDetail_IDirect3DDevice7_Load(TraceIDirect3DDevice7*, HRESULT r, LPDIRECTDRAWSURFACE7 dst, LPPOINT, LPDIRECTDRAWSURFACE7 src,
                                       LPRECT, DWORD flags)
{
    Detail("IDirect3DDevice7::Load", SurfaceKind(Unwrap<TraceIDirectDrawSurface7>(dst)) + " <- " +
           SurfaceKind(Unwrap<TraceIDirectDrawSurface7>(src)) + Format(" flags 0x%lx -> %s", flags, Result(r)));
}

void TraceDetail_IDirect3DVertexBuffer7_Lock(TraceIDirect3DVertexBuffer7* self, HRESULT r, DWORD flags, LPVOID*, LPDWORD)
{
    D3DVERTEXBUFFERDESC desc{};
    desc.dwSize = sizeof(desc);
    self->Real()->GetVertexBufferDesc(&desc);
    Detail("IDirect3DVertexBuffer7::Lock", Format("fvf 0x%lx caps %s flags %s -> %s", desc.dwFVF,
           Flags(desc.dwCaps, kVbCaps, std::size(kVbCaps)).c_str(), Flags(flags, kLock, std::size(kLock)).c_str(), Result(r)));
}

void TraceDetail_IDirect3DVertexBuffer7_ProcessVertices(TraceIDirect3DVertexBuffer7*, HRESULT r, DWORD op, DWORD, DWORD count,
                                                        LPDIRECT3DVERTEXBUFFER7, DWORD, LPDIRECT3DDEVICE7, DWORD flags)
{
    Detail("IDirect3DVertexBuffer7::ProcessVertices", Format("op 0x%lx flags 0x%lx -> %s", op, flags, Result(r)), Format("count %lu", count));
}

}  // namespace rvkproxy
