// RTexture_t natively (part of [Native] Device=on): textures as Randy's resources - their surface (or the shared pink
// default), description, locking, loading through D3DX, and the registry of textures by name (FindTexture).
//
// RTexture_t (0xBC bytes, an RResource_t): +0x2C TextureCreator* (makes / remakes the surface), +0x30 surface_t*,
// +0x34 the locked IDirectDrawSurface7, +0x38 DDSURFACEDESC2 of the top level (0x7C bytes: +0x3C flags, +0x40 height,
// +0x44 width, +0x48 pitch / linear size, +0x50 mip levels, +0x84 pixel format flags, +0x88 FourCC, +0x8C bits,
// +0x9C alpha mask), +0xB4 flags (1 = has alpha, 8 = unlocked since), +0xB8 registered by name.
// The registry is a std::multimap<std::string, RTexture_t*> in the original (0x1017D4A0); only these functions see
// it, so ours keeps its own: equal names in the order made, FindTexture gives the oldest.
#include "native/texture.h"
#include "native/orig_api.gen.h"
#include "native/serialize.h"
#include "native/vc10.h"

#include <cstring>
#include <list>
#include <map>
#include <string>

namespace rnative::texture {

namespace {

HMODULE g_orig;
void* const* g_render;

template <typename T>
T& Field(void* object, uint32_t offset) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(object) + offset); }
template <typename T>
T& Global(uint32_t rva) { return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(g_orig) + rva); }
template <typename F>
F Internal(uint32_t rva) { return reinterpret_cast<F>(AddressFor(rva)); }

template <typename... A>
HRESULT Com(void* object, uint32_t offset, A... args)
{
    using Fn = HRESULT(__stdcall*)(void*, A...);
    return reinterpret_cast<Fn>((*static_cast<void***>(object))[offset / 4])(object, args...);
}

constexpr uint32_t kVtable = 0x95788;
constexpr uint32_t kTextureCount = 0x17D490, kTextureRefs = 0x17D494, kDefaultSurface = 0x17D498,
                   kDefaultTexture = 0x17D49C, kCounted = 0x17D448;
constexpr uint32_t kCreator = 0x2C, kSurface = 0x30, kLocked = 0x34, kDesc = 0x38, kFlags = 0xB4, kRegistered = 0xB8;

std::map<std::string, std::list<void*>>& Registry()
{
    static auto* registry = new std::map<std::string, std::list<void*>>;
    return *registry;
}

void* Render() { return *g_render; }
void* DefaultSurface() { return Global<void*>(kDefaultSurface); }

void* Creator(void* t) { return Field<void*>(t, kCreator); }
template <typename R>
R CreatorCall(void* creator, uint32_t slot)
{
    return reinterpret_cast<R(__fastcall*)(void*)>((*static_cast<void***>(creator))[slot / 4])(creator);
}

// FUN_1004743d / FUN_100473dd: the IDirectDrawSurface7 of mip `level` (the last there is, if fewer).
void* MipSurface(void* t, uint8_t level)
{
    void* surface = *static_cast<void**>(Field<void*>(t, kSurface));
    uint32_t caps[4] = {0x401000, 0, 0, 0};          // DDSCAPS_TEXTURE | DDSCAPS_MIPMAP
    for (uint32_t i = 0; i < level; ++i) {
        void* next = nullptr;
        Com(surface, 0x30, static_cast<void*>(caps), &next);   // GetAttachedSurface (left referenced, as the original)
        if (!next) break;
        surface = next;
    }
    return surface;
}

void Describe(void* t)                              // FUN_1004756b: the top level's description
{
    void* surface = MipSurface(t, 0);
    if (!surface) return;
    uint8_t* desc = static_cast<uint8_t*>(t) + kDesc;
    std::memset(desc, 0, 0x7C);
    Field<uint32_t>(desc, 0) = 0x7C;
    Field<uint32_t>(desc, 0x48) = 0x20;             // ddpfPixelFormat.dwSize
    Com(surface, 0x58, static_cast<void*>(desc));   // GetSurfaceDesc
}

bool HasAlpha(void* t)                              // FUN_100475c5: an alpha channel, or DXT3
{
    if (!Field<void*>(t, kSurface)) return false;
    uint32_t format[8] = {0x20};
    orig::render_t_D3DXMakeDDPixelFormat(Render(), 0x13, format);   // D3DX_SF_DXT3
    return (Field<uint8_t>(t, 0x84) & 3) != 0 || Field<uint32_t>(t, 0x88) == format[2];
}

void Counted(void* t)                               // FUN_1004624f(0)
{
    Field<int32_t>(t, 0x28) = 0;
    ++Global<int32_t>(kCounted);
}

void Register(void* t)                              // FUN_10047876
{
    const char* name = orig::RResource_t_GetName(t);
    if (!name || !*name) {
        Field<uint32_t>(t, kRegistered) = 0;
        return;
    }
    Registry()[name].push_back(t);
    Field<uint32_t>(t, kRegistered) = 1;
}

void Finish(void* t, bool askCreator)               // what every constructor ends with
{
    Describe(t);
    ++Global<int32_t>(kTextureCount);
    ++Global<int32_t>(kTextureRefs);
    Counted(t);
    void* creator = Creator(t);
    const bool creatorSaysNo = askCreator && creator && CreatorCall<bool>(creator, 0x10);
    if (!creatorSaysNo && HasAlpha(t)) Field<uint32_t>(t, kFlags) |= 1;
    Register(t);
}

void Begin(void* t, void* creator)
{
    Field<uintptr_t>(t, 0) = reinterpret_cast<uintptr_t>(g_orig) + kVtable;
    Field<void*>(t, kCreator) = creator;
    Field<void*>(t, kSurface) = nullptr;
    Field<void*>(t, kLocked) = nullptr;
    Field<uint32_t>(t, kFlags) = 0;
    Field<uint32_t>(t, kRegistered) = 0;
}

void UseDefaultSurface(void* t)
{
    Field<void*>(t, kSurface) = DefaultSurface();
    orig::surface_t_AddRefDXSurface(DefaultSurface());
}

// A new texture: D3DX picks the size / format it can have; mip levels unless asked not to.
void* __fastcall ConstructNew(void* t, void*, const char* name, uint16_t width, uint16_t height, int32_t format,
                              bool mipmaps, void* palette)
{
    orig::RResource_t_RResource_t_38(t, name);
    uint32_t flags = mipmaps ? 0 : 0x100;           // D3DX_TEXTURE_NOMIPMAP
    uint32_t w = width, h = height, levels = 0;
    int32_t fmt = format;
    Begin(t, nullptr);
    void* surface = vc10::Allocate(0x88);
    orig::surface_t_surface_t(surface, nullptr, const_cast<char*>("Texture"));
    Field<void*>(t, kSurface) = surface;
    orig::render_t_D3DXCreateTexture(Render(), &flags, &w, &h, &fmt, palette, surface, &levels);
    Finish(t, false);
    return t;
}

void* __fastcall ConstructNamed(void* t, void*, const char* name)   // the default surface
{
    orig::RResource_t_RResource_t_38(t, name);
    Begin(t, nullptr);
    UseDefaultSurface(t);
    Finish(t, false);
    return t;
}

void* __fastcall ConstructWithCreator(void* t, void*, const char* name, void* creator)
{
    orig::RResource_t_RResource_t_38(t, name);
    Begin(t, creator);
    Field<int32_t>(t, 0x24) = 0;                    // (no references while the creator makes the surface)
    Field<void*>(t, kSurface) = CreatorCall<void*>(creator, 0xC);
    if (!Field<void*>(t, kSurface)) UseDefaultSurface(t);
    Field<int32_t>(t, 0x24) = 1;
    Finish(t, true);
    return t;
}

void* __cdecl FindTexture(const vc10::String* name);

// From an archive: its creator; the surface of a texture of the same name if there is one, else the creator's.
void* __fastcall ConstructFrom(void* t, void*, void* archive)
{
    orig::RResource_t_RResource_t_37(t, archive);
    Begin(t, nullptr);
    void* stream = serialize::Get().getStream(archive, nullptr);
    Internal<void(__fastcall*)(void*, void*, const char*, void**, void*)>(0x47ECC)(   // the "creator" object
        stream, nullptr, "creator", &Field<void*>(t, kCreator), nullptr);
    const char* name = orig::RResource_t_GetName(t);
    bool fromCreator = true;
    if (name && *name) {
        vc10::String key;
        key.init();
        key.allocator = 0;
        key.assign(name, std::strlen(name));
        void* same = FindTexture(&key);
        key.release();
        if (same) {
            Field<void*>(t, kSurface) = Field<void*>(same, kSurface);
            if (Field<void*>(t, kSurface)) orig::surface_t_AddRefDXSurface(Field<void*>(t, kSurface));
            fromCreator = false;
        }
    }
    if (fromCreator && Creator(t)) Field<void*>(t, kSurface) = CreatorCall<void*>(Creator(t), 0xC);
    if (!Field<void*>(t, kSurface)) UseDefaultSurface(t);
    Finish(t, true);
    return t;
}

void* __cdecl Instantiate(void* archive) { return ConstructFrom(vc10::Allocate(0xBC), nullptr, archive); }

void __fastcall Destroy(void* t)                    // FUN_100477fe
{
    Field<uintptr_t>(t, 0) = reinterpret_cast<uintptr_t>(g_orig) + kVtable;
    if (void* surface = Field<void*>(t, kSurface)) {
        orig::surface_t_ReleaseDXSurface(surface);
        Field<void*>(t, kSurface) = nullptr;
    }
    --Global<int32_t>(kTextureCount);
    if (void* creator = Creator(t))                 // its scalar deleting destructor
        reinterpret_cast<void*(__fastcall*)(void*, void*, uint32_t)>((*static_cast<void***>(creator))[0])(creator,
                                                                                                        nullptr, 1);
    Field<void*>(t, kCreator) = nullptr;
    if (Field<uint32_t>(t, kRegistered)) {
        auto it = Registry().find(orig::RResource_t_GetName(t));
        if (it != Registry().end()) {
            it->second.remove(t);
            if (it->second.empty()) Registry().erase(it);
        }
    }
    Internal<void(__fastcall*)(void*)>(0x46283)(t);   // RResource_t::~RResource_t
}

void* __cdecl FindTexture(const vc10::String* name)
{
    auto it = Registry().find(name->c_str());
    return it == Registry().end() || it->second.empty() ? nullptr : it->second.front();
}

uint32_t __cdecl GetTotalPinkTextureCount()
{
    void* s = DefaultSurface();
    if (!s) return 0;
    orig::surface_t_AddRefDXSurface(s);
    return orig::surface_t_ReleaseDXSurface(s);
}

void __fastcall Archive(void* t, void*, void* archive)
{
    orig::RResource_t_Archive(t, archive);
    serialize::Get().addObject(serialize::Get().getStream(archive, nullptr), nullptr, "creator", Creator(t));
}

// Pixels into the texture (D3DX converts and fills the mip levels); has alpha if the source format has.
bool __fastcall LoadPixels(void* t, void*, const void* data, int32_t format, uint32_t pitch, RECT* rect,
                           void* palette, bool filter)
{
    if (pitch == 0) pitch = 0xFFFFFFFF;
    orig::render_t_D3DXLoadTextureFromMemory(Render(), Field<void*>(t, kSurface), 0xFFFFFFFF, const_cast<void*>(data),
                                             palette, format, pitch, rect, filter ? 2 : 1);
    Field<uint32_t>(t, kFlags) = Internal<int32_t(__cdecl*)(int32_t)>(0x26277)(format) > 0 ? 1 : 0;   // alpha bits
    return true;
}

bool __fastcall LoadFrom(void* t, void*, void* other)
{
    if (other != t) {
        Internal<void(__fastcall*)(void*, void*, void*, uint32_t, void*, void*, void*, uint32_t)>(0x249C1)(
            Render(), nullptr, Field<void*>(t, kSurface), 0xFFFFFFFF, Field<void*>(other, kSurface), nullptr, nullptr, 2);
        Field<uint32_t>(t, kFlags) = Field<uint32_t>(other, kFlags);
    }
    return true;
}

bool __fastcall Lock(void* t, void*, void** out, uint32_t type, uint8_t level)
{
    void* surface = MipSurface(t, level);
    uint32_t desc[0x1F] = {0x7C};
    orig::render_t_LockTexture(Render(), surface, nullptr, desc, type | 0x801, nullptr);   // DDLOCK_WAIT | NOSYSLOCK
    *out = reinterpret_cast<void*>(desc[0x24 / 4]);                                       // lpSurface
    Field<void*>(t, kLocked) = surface;
    Com(surface, 4);
    Field<uint32_t>(t, kFlags) &= ~8u;
    return true;
}

bool __fastcall Unlock(void* t)
{
    void* surface = Field<void*>(t, kLocked);
    orig::render_t_UnlockTexture(Render(), surface, nullptr);
    Com(surface, 8);
    Field<void*>(t, kLocked) = nullptr;
    return true;
}

void __fastcall AddRef(void* t)
{
    ++Global<int32_t>(kTextureRefs);
    orig::RResource_t_AddRefRResource(t);
}

void __fastcall Release(void* t)
{
    --Global<int32_t>(kTextureRefs);
    orig::RResource_t_ReleaseRResource(t);
}

// RTexture_t::TextureFormat_e of the surface.
int32_t __fastcall GetTextureFormat(void* t)
{
    const uint32_t fourcc = Field<uint32_t>(t, 0x88), bits = Field<uint32_t>(t, 0x8C);
    const uint8_t pf = Field<uint8_t>(t, 0x84);
    if (fourcc == 0x31545844) return 0x12;           // DXT1
    if (fourcc == 0x33545844) return 0x13;           // DXT3
    if (bits == 0x20) return (~pf & 1) | 2;
    if (bits == 0x18) return 1;
    if (bits == 0x10) {
        if (!(pf & 1)) return 4;
        uint32_t alpha = Field<uint32_t>(t, 0x9C);
        int n = 0;
        for (; alpha; alpha &= alpha - 1) ++n;
        if (n == 1) return 8;
        if (n == 4) return 10;
    }
    return (pf & 0x20) ? 7 : 2;
}

bool __fastcall IsCompressed(void* t) { return (Field<uint32_t>(t, 0x3C) >> 19) & 1; }   // DDSD_LINEARSIZE
uint16_t __fastcall GetWidth(void* t) { return Field<uint16_t>(t, 0x44); }
uint16_t __fastcall GetHeight(void* t) { return Field<uint16_t>(t, 0x40); }
void* __cdecl GetDefault() { return Global<void*>(kDefaultTexture); }

// Bytes in all mip levels.
uint32_t __fastcall GetTextureSize(void* t)
{
    if (!Field<void*>(t, kSurface)) return 0;
    uint32_t total = 0;
    const uint32_t levels = Field<uint32_t>(t, 0x50);
    if (!IsCompressed(t)) {
        uint32_t pitch = Field<uint32_t>(t, 0x48);
        const uint32_t pixel = pitch / Field<uint32_t>(t, 0x44);
        uint32_t height = Field<uint32_t>(t, 0x40);
        for (uint32_t i = 0; i < levels; ++i) {
            total += height * pitch;
            pitch >>= 1;
            height >>= 1;
            if (pitch < pixel) pitch = pixel;
            if (height == 0) height = 1;
        }
    } else {
        uint32_t width = Field<uint32_t>(t, 0x44), height = Field<uint32_t>(t, 0x40);
        const uint32_t block = Field<uint32_t>(t, 0x48) / (height * width >> 4);
        for (uint32_t i = 0; i < levels; ++i) {
            total += ((height + 3) >> 2) * ((width + 3) >> 2) * block;
            width = width != 1 ? width >> 1 : 1;
            height = height != 1 ? height >> 1 : 1;
        }
    }
    return total;
}

uint32_t __fastcall GetPitch(void* self, void*) { return Field<uint32_t>(self, 0x48); }   // RTexture_t::GetPitch

}  // namespace

void Install(HMODULE orig)
{
    g_orig = orig;
    g_render = reinterpret_cast<void* const*>(GetProcAddress(orig, "?m_pcInstance@render_t@@0PAV1@A"));
    struct Entry {
        uint32_t rva;
        void* target;
        const char* what;
    };
#define FN(f) reinterpret_cast<void*>(&f)
    const Entry entries[] = {
        {0x47900, FN(ConstructNew), "RTexture_t::RTexture_t(name, size, format)"},
        {0x47A99, FN(ConstructNamed), "RTexture_t::RTexture_t(name)"},
        {0x47B2E, FN(ConstructWithCreator), "RTexture_t::RTexture_t(name, creator)"},
        {0x47BF2, FN(ConstructFrom), "RTexture_t::RTexture_t(archive)"},
        {0x47E59, FN(Instantiate), "RTexture_t::Instantiate"},
        {0x477FE, FN(Destroy), "RTexture_t::~RTexture_t (FUN_100477fe)"},
        {0x477D6, FN(FindTexture), "RTexture_t::FindTexture"},
        {0x47876, FN(Register), "RTexture_t registry insert (FUN_10047876)"},
        {0x47304, FN(GetTotalPinkTextureCount), "RTexture_t::GetTotalPinkTextureCount"},
        {0x47321, FN(Archive), "RTexture_t::Archive"},
        {0x4734D, FN(LoadPixels), "RTexture_t::Load(pixels)"},
        {0x473A2, FN(LoadFrom), "RTexture_t::Load(texture)"},
        {0x476C8, FN(Lock), "RTexture_t::LockRTexture"},
        {0x4749D, FN(Unlock), "RTexture_t::UnlockRTexture"},
        {0x474C1, FN(AddRef), "RTexture_t::AddRefRTexture"},
        {0x474CC, FN(Release), "RTexture_t::ReleaseRTexture"},
        {0x474D7, FN(GetTextureFormat), "RTexture_t::GetTextureFormat"},
        {0x475BB, FN(IsCompressed), "RTexture_t::IsCompressed"},
        {0x4762E, FN(GetWidth), "RTexture_t::GetWidth"},
        {0x47633, FN(GetHeight), "RTexture_t::GetHeight"},
        {0x47693, FN(GetDefault), "RTexture_t::GetDefault"},
        {0x47730, FN(GetTextureSize), "RTexture_t::GetTextureSize"},
        {0x47638, FN(GetPitch), "RTexture_t::GetPitch"},
    };
#undef FN
    // The registry is ours once any of its users is: all of them or none.
    for (uint32_t rva : {0x47900u, 0x47A99u, 0x47B2Eu, 0x47BF2u, 0x477FEu, 0x477D6u, 0x47876u})
        if (!Replaceable(orig, rva)) {
            Log("textures: %05X can't be replaced - none are", rva);
            return;
        }
    int installed = 0;
    for (const Entry& e : entries) installed += Replace(orig, e.rva, e.target, e.what) ? 1 : 0;
    Log("textures: %d of %d functions native", installed, int(sizeof(entries) / sizeof(entries[0])));
}

}  // namespace rnative::texture
