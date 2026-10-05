// Material maps for RDB textures, side-loaded from <client>\randy-vk\materials\:
//   <type>_<id>_n.png    tangent-space normal map (OpenGL convention, as Blender bakes; any size); <type> is the
//                        full-quality table (1010004 world, 1010006 ground), which also covers its lower levels
//   <id>_n.png           the same, any type (fallback)
// ao-assets writes these names (python -m aoassets material-template).
//
// Identity (see HOOKED_EXPORTS in tools/gen_interface.py; the hooks call the original, then record the identity on the
// surface_t's IDirectDrawSurface7 when it is one of ours):
// - World textures: DisplaySystem's RDBTexture_t hands randy an AnarchyTexCreator_t {+0x40 type, +0x44 id}, whose
//   create method calls the exported TextureStreamCreator::CreateTexture with the creator as `this`.
// - Ground textures: RDBGroundTexture_t {DbObject_t: +0x08 type, +0x0C id, +0x20 its RTexture_t once made} uses a
//   plain TextureStreamCreator, created and called inside randy. What crosses the export table is the
//   RTexture_t(name, TextureCreator*) constructor it calls; the RDBGroundTexture_t making it is found by RTTI in the
//   caller's saved registers and stack, and the new RTexture_t's surface_t (+0x30) gets its identity.
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "rvk_backend.h"

#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "stb/stb_image.h"

namespace rvkproxy {

// {6a1f0e52-2d43-4b8e-9c55-7f3a0b1d9e10}
const GUID IID_RvkSurface = {0x6a1f0e52, 0x2d43, 0x4b8e, {0x9c, 0x55, 0x7f, 0x3a, 0x0b, 0x1d, 0x9e, 0x10}};

namespace {

// ---------------------------------------------------------------- creator identity (MSVC RTTI)
bool Readable(const void* p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || !VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
        return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
        return false;
    auto end = static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
    return static_cast<const uint8_t*>(p) + n <= end;
}

enum class Kind { Other, AnarchyTexCreator, RDBGroundTexture };

// Class of a polymorphic object from its vtable's complete object locator (x86: vtable[-1] -> COL, COL+12 -> type
// descriptor, name at +8, e.g. ".?AVAnarchyTexCreator_t@@").
Kind ClassOf(const void* object)
{
    static std::unordered_map<const void*, Kind> cache;   // vtable -> class
    if (!Readable(object, 4))
        return Kind::Other;
    const void* vtable = *static_cast<void* const*>(object);
    auto it = cache.find(vtable);
    if (it != cache.end())
        return it->second;
    Kind kind = Kind::Other;
    auto vt = static_cast<const uint8_t* const*>(vtable);
    if (Readable(vt - 1, 4)) {
        const uint8_t* col = vt[-1];
        if (Readable(col, 16)) {
            const uint8_t* td = *reinterpret_cast<const uint8_t* const*>(col + 12);
            static const char kTex[] = ".?AVAnarchyTexCreator_t@@", kGround[] = ".?AVRDBGroundTexture_t@@";
            if (Readable(td + 8, sizeof(kTex)) && std::memcmp(td + 8, kTex, sizeof(kTex)) == 0)
                kind = Kind::AnarchyTexCreator;
            else if (Readable(td + 8, sizeof(kGround)) && std::memcmp(td + 8, kGround, sizeof(kGround)) == 0)
                kind = Kind::RDBGroundTexture;
        }
    }
    cache.emplace(vtable, kind);
    return kind;
}

uint32_t U32(const void* p, size_t offset)
{
    return *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(p) + offset);
}

// An RDBGroundTexture_t still making its texture: a ground texture type, no RTexture_t yet.
bool IsGroundMaker(const void* p)
{
    if (!p || (reinterpret_cast<uintptr_t>(p) & 3) || !Readable(p, 0x28) || ClassOf(p) != Kind::RDBGroundTexture)
        return false;
    uint32_t type = U32(p, 0x08);
    return (type == 1010006 || type == 1010021 || type == 1010022) && U32(p, 0x20) == 0;
}

// ---------------------------------------------------------------- material folder
std::string g_dir;
std::unordered_set<std::string> g_files;   // lower-case names in the folder
bool g_scanned = false;

std::string Lower(std::string s)
{
    for (char& c : s) c = char(tolower(static_cast<unsigned char>(c)));
    return s;
}

void Scan()
{
    if (g_scanned)
        return;
    g_scanned = true;
    char path[MAX_PATH] = {};
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&Scan), &self);
    GetModuleFileNameA(self, path, MAX_PATH);
    char* slash = std::strrchr(path, '\\');
    if (slash) slash[1] = 0; else path[0] = 0;
    g_dir = std::string(path) + "randy-vk\\materials\\";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((g_dir + "*.png").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do g_files.insert(Lower(fd.cFileName));
        while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RvkLog("materials: %u files in %s", unsigned(g_files.size()), g_dir.c_str());
}

// The lower texture quality levels are other tables with the same ids: the ground's 1010021/22 for 1010006, the
// world's 1010016/17 for 1010004. A map is named after the full-quality table and serves every level.
uint32_t FullQualityType(uint32_t type)
{
    switch (type) {
    case 1010021: case 1010022: return 1010006;
    case 1010016: case 1010017: return 1010004;
    default: return type;
    }
}

std::string FindMap(uint32_t type, uint32_t id, const char* suffix)
{
    Scan();
    for (uint32_t t : {FullQualityType(type), type}) {
        std::string a = std::to_string(t) + "_" + std::to_string(id) + suffix;
        if (g_files.count(a)) return g_dir + a;
    }
    std::string b = std::to_string(id) + suffix;
    if (g_files.count(b)) return g_dir + b;
    return {};
}

// ---------------------------------------------------------------- normal map loading
// RGBA8 -> mip chain of A8R8G8B8 (BGRA bytes). Normal maps: each level averages the decoded normals of the level
// above and renormalises them, so distant surfaces don't flatten or tilt.
rvk::Texture* LoadNormalMap(const std::string& path, rvk::ThreadedDevice* dev)
{
    int w = 0, h = 0, n = 0;
    stbi_uc* rgba = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!rgba) {
        RvkLog("materials: can't read %s (%s)", path.c_str(), stbi_failure_reason());
        return nullptr;
    }
    std::vector<float> nrm(size_t(w) * h * 3);
    for (size_t i = 0; i < size_t(w) * h; ++i)
        for (int c = 0; c < 3; ++c)
            nrm[i * 3 + c] = rgba[i * 4 + c] / 127.5f - 1.0f;
    stbi_image_free(rgba);
    uint32_t levels = 1;
    for (int s = std::max(w, h); s > 1; s >>= 1) ++levels;
    rvk::Texture* tex = dev->CreateTexture(uint32_t(w), uint32_t(h), rvk::Format::A8R8G8B8, levels);
    if (!tex)
        return nullptr;
    std::vector<uint32_t> out;
    for (uint32_t level = 0; level < levels; ++level) {
        out.resize(size_t(w) * h);
        for (size_t i = 0; i < out.size(); ++i) {
            float x = nrm[i * 3], y = nrm[i * 3 + 1], z = nrm[i * 3 + 2];
            float len = std::sqrt(x * x + y * y + z * z);
            if (len < 1e-6f) { x = y = 0.0f; z = 1.0f; len = 1.0f; }
            auto enc = [&](float v) { return uint32_t(std::lround(std::clamp((v / len) * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f)); };
            out[i] = 0xFF000000u | enc(x) << 16 | enc(y) << 8 | enc(z);
        }
        dev->UpdateTexture(tex, level, 0, 0, uint32_t(w), uint32_t(h), out.data(), uint32_t(w) * 4);
        if (level + 1 == levels)
            break;
        int nw = std::max(w / 2, 1), nh = std::max(h / 2, 1);
        std::vector<float> next(size_t(nw) * nh * 3, 0.0f);
        for (int y = 0; y < nh; ++y)
            for (int x = 0; x < nw; ++x)
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx) {
                        int sx = std::min(x * 2 + dx, w - 1), sy = std::min(y * 2 + dy, h - 1);
                        for (int c = 0; c < 3; ++c)
                            next[(size_t(y) * nw + x) * 3 + c] += nrm[(size_t(sy) * w + sx) * 3 + c] * 0.25f;
                    }
        nrm.swap(next);
        w = nw;
        h = nh;
    }
    return tex;
}

unsigned g_registered = 0, g_attached = 0;

unsigned g_ground = 0;

void Register(void* surface_t, uint32_t type, uint32_t id)
{
    if (!surface_t || !Readable(surface_t, 4))
        return;
    auto* dds = *static_cast<IUnknown**>(surface_t);
    if (!dds)
        return;
    RSurface* s = nullptr;
    if (FAILED(dds->QueryInterface(IID_RvkSurface, reinterpret_cast<void**>(&s))) || !s)
        return;                                       // not our backend (D7VK): nothing to attach to
    RSurface* top = s->top ? s->top : s;
    top->rdbType = type;
    top->rdbId = id;
    bool ground = type == 1010006 || type == 1010021 || type == 1010022;
    g_ground += ground;
    if (++g_registered <= 8 || (g_registered & (g_registered - 1)) == 0 || (ground && g_ground <= 4))
        RvkLog("materials: RDB texture %u:%u is a %lux%lu surface (%u textures identified)", type, id,
               top->desc.dwWidth, top->desc.dwHeight, g_registered);
    if (top->texture)
        AttachMaterialMaps(top);
    s->Release();
}

void RegisterCreator(void* surface, const void* creator)
{
    if (ClassOf(creator) != Kind::AnarchyTexCreator || !Readable(creator, 0x48))
        return;
    static const unsigned index = ComIndex("rvk::Materials");
    ComScope scope(index);
    Register(surface, U32(creator, 0x40), U32(creator, 0x44));
}

using CreateFromBitmap = void*(__thiscall*)(void* self, void* bitmap, const char* name);
using CreateFromStream = void*(__thiscall*)(void* self, void* stream, const char* name);
using RTextureFromCreator = void*(__thiscall*)(void* self, const char* name, void* creator);

template <typename F>
F Original(const char* mangled)
{
    HMODULE orig = GetModuleHandleA("randy31_orig.dll");
    if (!orig) orig = LoadLibraryA("randy31_orig.dll");
    return reinterpret_cast<F>(GetProcAddress(orig, mangled));
}

}  // namespace

void AttachMaterialMaps(RSurface* top)
{
    if (!top || !top->rdbId || !top->texture || top->materialsFor == top->texture || !g_rvk.device)
        return;
    top->materialsFor = top->texture;
    std::string path = FindMap(top->rdbType, top->rdbId, "_n.png");
    if (path.empty())
        return;
    if (rvk::Texture* normal = LoadNormalMap(path, g_rvk.device)) {
        g_rvk.device->SetNormalMap(top->texture, normal);
        ++g_attached;
        RvkLog("materials: normal map %s (%ux%u) on RDB texture %u:%u (%u attached)", path.c_str(),
               normal->Width(), normal->Height(), top->rdbType, top->rdbId, g_attached);
    }
}

}  // namespace rvkproxy

using namespace rvkproxy;

// The hooked exports (thiscall: `this` in ecx; __fastcall's unused edx keeps the stack arguments in place).
extern "C" void* __fastcall rvk_CreateTextureBitmap(void* self, void*, void* bitmap, const char* name)
{
    static auto original = Original<CreateFromBitmap>("?CreateTexture@TextureStreamCreator@@QAEPAVsurface_t@@PAVLBitmap_t@@PBD@Z");
    void* surface = original(self, bitmap, name);
    RegisterCreator(surface, self);
    return surface;
}

extern "C" void* __fastcall rvk_CreateTextureStream(void* self, void*, void* stream, const char* name)
{
    static auto original = Original<CreateFromStream>("?CreateTexture@TextureStreamCreator@@QAEPAVsurface_t@@PAVPositionIO_t@fun@@PBD@Z");
    void* surface = original(self, stream, name);
    RegisterCreator(surface, self);
    return surface;
}

// RTexture_t::RTexture_t(char const*, TextureCreator*): world textures are identified by the CreateTexture hooks above;
// a plain creator may be an RDBGroundTexture_t's. Its `this` is in a callee-saved register of the caller, which this
// function's prologue pushes just below its return address, or on the caller's stack above it.
extern "C" void* __fastcall rvk_RTextureFromCreator(void* self, void*, const char* name, void* creator)
{
    const void* maker = nullptr;
    if (ClassOf(creator) != Kind::AnarchyTexCreator) {
        auto* sp = reinterpret_cast<const uint32_t*>(_AddressOfReturnAddress()) - 8;
        for (int i = 0; !maker && i < 264 && Readable(sp + i, 4); ++i)
            if (IsGroundMaker(reinterpret_cast<const void*>(uintptr_t(sp[i]))))
                maker = reinterpret_cast<const void*>(uintptr_t(sp[i]));
    }
    static auto original = Original<RTextureFromCreator>("??0RTexture_t@@QAE@PBDPAVTextureCreator@@@Z");
    void* texture = original(self, name, creator);
    if (maker && texture && Readable(static_cast<uint8_t*>(texture) + 0x30, 4)) {
        static const unsigned index = ComIndex("rvk::Materials");
        ComScope scope(index);
        Register(*reinterpret_cast<void**>(static_cast<uint8_t*>(texture) + 0x30), U32(maker, 0x08), U32(maker, 0x0C));
    }
    return texture;
}

