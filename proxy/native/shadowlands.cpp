// RandyShadowlandsData_s natively (randy-vk.ini [Native] Scene=on): the Shadowlands special lights' state. The class
// is all static data; its members sit at fixed addresses in randy31_orig's data, so both our functions and the
// original's other (unported) code read and write the same storage. The setters clamp an intensity to [0, 1], keep a
// texture with an AddRef / Release, and invalidate the light's texture matrix; the getters rebuild the matrix from
// the camera and the light's scale / offset through FUN_1006e302 when it is invalid (the validity flag is only ever
// cleared - never set - so the matrix is in practice remade on every ask, exactly as the original does it).
#include "native/shadowlands.h"

#include "native/orig_api.gen.h"

#include <cstdint>
#include <cstring>

namespace rnative::shadowlands {

namespace {

HMODULE g_orig;

template <typename T>
T& G(uint32_t rva) { return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(g_orig) + rva); }
template <typename F>
F Internal(uint32_t rva) { return reinterpret_cast<F>(AddressFor(rva)); }

// FUN_1006e302: out = camera * in (16 floats each).
float* Multiply(const float* camera, float* out, const float* in)
{
    return Internal<float*(__fastcall*)(const float*, void*, float*, intptr_t)>(0x6E302)(
        camera, nullptr, out, reinterpret_cast<intptr_t>(in));
}

// The class's static storage (randy31_orig rvas; the names are the original's).
constexpr uint32_t kGroundScale = 0xB7788, kGroundOffset = 0x17D46C, kGroundMatrix = 0xB7810;
constexpr uint32_t kStatelScale = 0xB77A0, kStatelOffset = 0x17D478, kStatelMatrix = 0xB7850;
constexpr uint32_t kCATScale = 0xB77B8, kCATOffset = 0x17D484, kCATMatrix = 0xB7890;
constexpr uint32_t kCameraMatrix = 0xB77D0;
constexpr uint32_t kGroundEnabled = 0x17D44C, kStatelEnabled = 0x17D44D, kCATEnabled = 0x17D44E;
constexpr uint32_t kGroundValid = 0x17D44F, kStatelValid = 0x17D468, kCATValid = 0x17D469;
constexpr uint32_t kGroundTexture = 0x17D450, kGroundIntensity = 0x17D454;
constexpr uint32_t kStatelTexture = 0x17D458, kStatelIntensity = 0x17D45C;
constexpr uint32_t kCATTexture = 0x17D460, kCATIntensity = 0x17D464;
constexpr uint32_t kGroundDirection = 0xB7794, kStatelDirection = 0xB77AC, kCATDirection = 0xB77C4;
constexpr uint32_t kRender = 0x16BED0;               // render_t::m_pcInstance

void SetTexture(uint32_t at, void* texture)
{
    if (texture) orig::RTexture_t_AddRefRTexture(texture);
    if (void* old = G<void*>(at)) orig::RTexture_t_ReleaseRTexture(old);
    G<void*>(at) = texture;
}

void SetParameters(uint32_t scale, uint32_t offset, uint32_t valid, const float* from)
{
    std::memcpy(&G<float>(scale), from, 12);
    std::memcpy(&G<float>(offset), from + 3, 12);
    G<uint8_t>(valid) = 0;
}

void SetDirection(uint32_t at, const float* from) { std::memcpy(&G<float>(at), from, 12); }

// `v = p`, then below 0 to 0 and above 1 to 1; in [0, 1] (and -0.0, and a NaN - the FPU's compares pass it) p stays.
void SetIntensity(uint32_t at, float p)
{
    G<float>(at) = p;
    if (p < 0.0f) G<float>(at) = 0.0f;
    else if (p > 1.0f) G<float>(at) = 1.0f;
}

// The matrix from the camera and the light's scale / offset: scale.x and scale.z on the diagonal and the offset in
// the last row (scale.y and offset.y are stored by the setters but never enter the matrix - the original's quirk).
const float* GetMatrix(uint32_t scale, uint32_t offset, uint32_t valid, uint32_t matrix)
{
    if (!G<uint8_t>(valid)) {
        float in[16] = {};
        in[0] = G<float>(scale);
        in[9] = G<float>(scale + 8);
        in[12] = G<float>(offset);
        in[13] = G<float>(offset + 8);
        in[15] = 1.0f;
        float out[16];
        Multiply(reinterpret_cast<const float*>(reinterpret_cast<uint8_t*>(g_orig) + kCameraMatrix), out, in);
        std::memcpy(&G<float>(matrix), out, sizeof(out));
    }
    return &G<float>(matrix);
}

bool IsUsed(uint32_t enabled, uint32_t texture, uint32_t intensity)
{
    return G<uint8_t>(enabled) && PriCheck() && G<void*>(texture) != nullptr && G<float>(intensity) > 0.0f;
}

}  // namespace

void __cdecl SetCameraMatrix(const float* matrix)
{
    std::memcpy(&G<float>(kCameraMatrix), matrix, 64);
    G<uint8_t>(kGroundValid) = 0;
    G<uint8_t>(kStatelValid) = 0;
    G<uint8_t>(kCATValid) = 0;
}

void __cdecl SetGroundLightTexture(void* texture) { SetTexture(kGroundTexture, texture); }
void __cdecl SetGroundLightParameters(const float* scale, const float* offset) { SetParameters(kGroundScale, kGroundOffset, kGroundValid, scale); }
void __cdecl SetGroundLightIntensity(float intensity) { SetIntensity(kGroundIntensity, intensity); }
void __cdecl EnableGroundLight(bool on) { G<uint8_t>(kGroundEnabled) = on; }
void __cdecl SetGroundLightDirection(const float* direction) { SetDirection(kGroundDirection, direction); }
float __cdecl GetGroundLightIntensity() { return G<float>(kGroundIntensity); }
void* __cdecl GetGroundLightTexture() { return G<void*>(kGroundTexture); }
const float* __cdecl GetGroundLightMatrix() { return GetMatrix(kGroundScale, kGroundOffset, kGroundValid, kGroundMatrix); }
bool __cdecl IsGroundLightUsed() { return IsUsed(kGroundEnabled, kGroundTexture, kGroundIntensity); }

void __cdecl SetStatelLightTexture(void* texture) { SetTexture(kStatelTexture, texture); }
void __cdecl SetStatelLightParameters(const float* scale, const float* offset) { SetParameters(kStatelScale, kStatelOffset, kStatelValid, scale); }
void __cdecl SetStatelLightIntensity(float intensity) { SetIntensity(kStatelIntensity, intensity); }
void __cdecl EnableStatelLight(bool on) { G<uint8_t>(kStatelEnabled) = on; }
void __cdecl SetStatelLightDirection(const float* direction) { SetDirection(kStatelDirection, direction); }
float __cdecl GetStatelLightIntensity() { return G<float>(kStatelIntensity); }
void* __cdecl GetStatelLightTexture() { return G<void*>(kStatelTexture); }
const float* __cdecl GetStatelLightMatrix() { return GetMatrix(kStatelScale, kStatelOffset, kStatelValid, kStatelMatrix); }
bool __cdecl IsStatelLightUsed() { return IsUsed(kStatelEnabled, kStatelTexture, kStatelIntensity); }

void __cdecl SetCATLightTexture(void* texture) { SetTexture(kCATTexture, texture); }
void __cdecl SetCATLightParameters(const float* scale, const float* offset) { SetParameters(kCATScale, kCATOffset, kCATValid, scale); }
void __cdecl SetCATLightIntensity(float intensity) { SetIntensity(kCATIntensity, intensity); }
void __cdecl EnableCATLight(bool on) { G<uint8_t>(kCATEnabled) = on; }
void __cdecl SetCATLightDirection(const float* direction) { SetDirection(kCATDirection, direction); }
float __cdecl GetCATLightIntensity() { return G<float>(kCATIntensity); }
void* __cdecl GetCATLightTexture() { return G<void*>(kCATTexture); }
const float* __cdecl GetCATLightMatrix() { return GetMatrix(kCATScale, kCATOffset, kCATValid, kCATMatrix); }
bool __cdecl IsCATLightUsed() { return IsUsed(kCATEnabled, kCATTexture, kCATIntensity); }

void __cdecl FreeAllTextures()
{
    SetGroundLightTexture(nullptr);
    SetStatelLightTexture(nullptr);
    SetCATLightTexture(nullptr);
}

bool __cdecl PriCheck()
{
    uint8_t* render = G<uint8_t*>(kRender);
    if (!render) return false;                       // the original dereferences it (render_t::m_pcInstance)
    const uint32_t value = *reinterpret_cast<uint32_t*>(render + 0x288);
    return value != 0 && value < 7;
}

void SetModule(HMODULE orig) { g_orig = orig; }

void Install(HMODULE orig)
{
    g_orig = orig;
    if (GetMode("Scene", Mode::Off) != Mode::On) return;
    struct Entry {
        uint32_t rva;
        void* target;
        const char* what;
    };
#define FN(f) reinterpret_cast<void*>(&f)
    const Entry entries[] = {
        {0x46E5D, FN(SetCameraMatrix), "RandyShadowlandsData_s::SetCameraMatrix"},
        {0x46E84, FN(SetGroundLightTexture), "RandyShadowlandsData_s::SetGroundLightTexture"},
        {0x46EAE, FN(SetGroundLightParameters), "RandyShadowlandsData_s::SetGroundLightParameters"},
        {0x46ED4, FN(SetGroundLightIntensity), "RandyShadowlandsData_s::SetGroundLightIntensity"},
        {0x46F06, FN(EnableGroundLight), "RandyShadowlandsData_s::EnableGroundLight"},
        {0x46F13, FN(SetStatelLightTexture), "RandyShadowlandsData_s::SetStatelLightTexture"},
        {0x46F3D, FN(SetStatelLightParameters), "RandyShadowlandsData_s::SetStatelLightParameters"},
        {0x46F63, FN(SetStatelLightIntensity), "RandyShadowlandsData_s::SetStatelLightIntensity"},
        {0x46F95, FN(EnableStatelLight), "RandyShadowlandsData_s::EnableStatelLight"},
        {0x46FA2, FN(SetCATLightTexture), "RandyShadowlandsData_s::SetCATLightTexture"},
        {0x46FCC, FN(SetCATLightParameters), "RandyShadowlandsData_s::SetCATLightParameters"},
        {0x46FF2, FN(SetCATLightIntensity), "RandyShadowlandsData_s::SetCATLightIntensity"},
        {0x47024, FN(SetCATLightDirection), "RandyShadowlandsData_s::SetCATLightDirection"},
        {0x47038, FN(SetStatelLightDirection), "RandyShadowlandsData_s::SetStatelLightDirection"},
        {0x4704C, FN(SetGroundLightDirection), "RandyShadowlandsData_s::SetGroundLightDirection"},
        {0x47060, FN(EnableCATLight), "RandyShadowlandsData_s::EnableCATLight"},
        {0x4706D, FN(GetGroundLightMatrix), "RandyShadowlandsData_s::GetGroundLightMatrix"},
        {0x470F5, FN(GetStatelLightMatrix), "RandyShadowlandsData_s::GetStatelLightMatrix"},
        {0x4717D, FN(GetCATLightMatrix), "RandyShadowlandsData_s::GetCATLightMatrix"},
        {0x47205, FN(FreeAllTextures), "RandyShadowlandsData_s::FreeAllTextures"},
        {0x4721E, FN(PriCheck), "RandyShadowlandsData_s::PriCheck"},
        {0x47238, FN(GetGroundLightIntensity), "RandyShadowlandsData_s::GetGroundLightIntensity"},
        {0x4723F, FN(GetGroundLightTexture), "RandyShadowlandsData_s::GetGroundLightTexture"},
        {0x4724B, FN(GetStatelLightIntensity), "RandyShadowlandsData_s::GetStatelLightIntensity"},
        {0x47252, FN(GetStatelLightTexture), "RandyShadowlandsData_s::GetStatelLightTexture"},
        {0x4725E, FN(GetCATLightIntensity), "RandyShadowlandsData_s::GetCATLightIntensity"},
        {0x47265, FN(GetCATLightTexture), "RandyShadowlandsData_s::GetCATLightTexture"},
        {0x47271, FN(IsGroundLightUsed), "RandyShadowlandsData_s::IsGroundLightUsed"},
        {0x472A2, FN(IsStatelLightUsed), "RandyShadowlandsData_s::IsStatelLightUsed"},
        {0x472D3, FN(IsCATLightUsed), "RandyShadowlandsData_s::IsCATLightUsed"},
    };
#undef FN
    int installed = 0;
    for (const Entry& e : entries) installed += Replace(orig, e.rva, e.target, e.what) ? 1 : 0;
    Log("shadowlands: %d of %d functions native", installed, int(sizeof(entries) / sizeof(entries[0])));
}

}  // namespace rnative::shadowlands
