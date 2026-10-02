// The renderer's settings table, randy-vk.ini persistence and the exported C interface (rvk_settings.h).
#include "rvk_settings.h"

#include "rvk_backend.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace rvkproxy;

namespace {

enum Type : uint32_t { Bool = 0, Int = 1, Float = 2 };

struct Setting {
    const char* name;           // also the ini key; at most 15 characters (game option variables)
    const char* label;
    const char* section;
    Type type;
    float min, max, step, defaultValue;
    const char* legacyEnv;      // read once if the ini doesn't have the setting yet (the old ao-wine.sh variables)
    float value;
};

// Every renderer option. Order = display order within each section.
Setting g_settings[] = {
    // name               label                                                             section          type   min   max   step  default env
    {"RVK_PixelLight", "Per-pixel lighting",                                              "Lighting",       Bool,  0, 1, 1, 1, "RANDYVK_PIXEL_LIGHTING", 0},
    {"RVK_LightOver",  "All nearby lights light every surface (light override)",          "Lighting",       Bool,  0, 1, 1, 1, "RANDYVK_LIGHT_OVERRIDE", 0},
    {"RVK_OwnLight",   "A character's own light lights the character",                    "Lighting",       Bool,  0, 1, 1, 1, nullptr, 0},
    {"RVK_Bump",       "Generated surface relief (normal maps from textures)",            "Lighting",       Float, 0, 4, 0.25f, 1.5f, "RANDYVK_BUMP", 0},
    {"RVK_Headroom",   "Light headroom without HDR",                                       "Lighting",       Float, 1, 2, 0.05f, 1.25f, "RANDYVK_LIGHT_HEADROOM", 0},
    {"RVK_Aniso",      "Anisotropic filtering (1 = off)",                                 "Lighting",       Int,   1, 16, 1, 16, "RANDYVK_ANISOTROPY", 0},
    {"RVK_SunShadow",  "Sun shadows",                                                     "Shadows",        Bool,  0, 1, 1, 1, "RANDYVK_SHADOWS", 0},
    {"RVK_SunStrength","Sun shadow strength",                                             "Shadows",        Float, 0, 1, 0.05f, 0.65f, "RANDYVK_SHADOW_STRENGTH", 0},
    {"RVK_SunRange",   "Sun shadow range (world units)",                                  "Shadows",        Int,   20, 200, 5, 60, "RANDYVK_SHADOW_RANGE", 0},
    {"RVK_PtShadows",  "Point light shadows (lights, 0 = off)",                           "Shadows",        Int,   0, 8, 1, 8, "RANDYVK_POINT_SHADOWS", 0},
    {"RVK_PtStrength", "Point light shadow strength",                                     "Shadows",        Float, 0, 1, 0.05f, 0.9f, "RANDYVK_POINT_SHADOW_STRENGTH", 0},
    {"RVK_PtDay",      "Point light shadow strength in daylight (fraction)",              "Shadows",        Float, 0, 1, 0.05f, 0.25f, "RANDYVK_POINT_SHADOW_DAY", 0},
    {"RVK_Hdr",        "HDR scene and tone mapping",                                      "HDR and effects", Bool, 0, 1, 1, 1, "RANDYVK_HDR", 0},
    {"RVK_Exposure",   "Exposure",                                                        "HDR and effects", Float, 0.5f, 2, 0.05f, 1, "RANDYVK_EXPOSURE", 0},
    {"RVK_Knee",       "Tone mapping knee (1 = only clip)",                               "HDR and effects", Float, 0.5f, 1, 0.05f, 0.85f, "RANDYVK_TONEMAP_KNEE", 0},
    {"RVK_HdrRoom",    "Local light headroom",                                            "HDR and effects", Float, 1, 4, 0.25f, 1.5f, "RANDYVK_HDR_HEADROOM", 0},
    {"RVK_Bloom",      "Bloom strength (0 = off)",                                        "HDR and effects", Float, 0, 5, 0.25f, 1.5f, "RANDYVK_BLOOM", 0},
    {"RVK_BloomFx",    "Glow of effects (spells, fire, light halos)",                     "HDR and effects", Float, 0, 4, 0.25f, 1, "RANDYVK_BLOOM_EFFECTS", 0},
    {"RVK_BloomThr",   "Bloom threshold (1 = above white)",                               "HDR and effects", Float, 0.5f, 2, 0.05f, 1, "RANDYVK_BLOOM_THRESHOLD", 0},
    {"RVK_Ao",         "Ambient occlusion strength (0 = off)",                            "HDR and effects", Float, 0, 3, 0.25f, 1, "RANDYVK_AO", 0},
    {"RVK_AoRadius",   "Ambient occlusion radius (world units)",                          "HDR and effects", Float, 0.25f, 4, 0.25f, 1.5f, "RANDYVK_AO_RADIUS", 0},
    {"RVK_Gi",         "Indirect light: bounce light from the lit scene (0 = off)",       "HDR and effects", Float, 0, 3, 0.25f, 1, nullptr, 0},
    {"RVK_GiRadius",   "Indirect light reach (world units)",                              "HDR and effects", Float, 1, 12, 0.5f, 4, nullptr, 0},
    {"RVK_MBlur",      "Motion blur (exposure, fraction of 1/60 s; 0 = off)",             "HDR and effects", Float, 0, 2, 0.05f, 0.5f, "RANDYVK_MOTION_BLUR", 0},
    {"RVK_MBlurObj",   "Per-object motion blur (off = camera only)",                      "HDR and effects", Bool, 0, 1, 1, 1, nullptr, 0},
    {"RVK_MBlurNear",  "Camera motion blur: sharp nearer than (world units)",             "HDR and effects", Float, 2, 20, 0.5f, 8, "RANDYVK_MOTION_BLUR_NEAR", 0},
    {"RVK_Dof",        "Depth of field",                                                   "Depth of field", Bool, 0, 1, 1, 1, nullptr, 0},
    {"RVK_DofBokeh",   "Bokeh (hexagonal highlights; off = smooth blur)",                 "Depth of field", Bool, 0, 1, 1, 1, nullptr, 0},
    {"RVK_DofNear",    "Blur in front of the focus",                                      "Depth of field", Bool, 0, 1, 1, 1, nullptr, 0},
    {"RVK_DofFar",     "Blur behind the focus (always)",                                  "Depth of field", Bool, 0, 1, 1, 0, nullptr, 0},
    {"RVK_DofMacro",   "Close focus: blur behind it when nearer than (world units)",      "Depth of field", Float, 0, 10, 0.5f, 3, nullptr, 0},
    {"RVK_DofAmount",  "Blur strength",                                                   "Depth of field", Float, 0, 2, 0.05f, 0.5f, nullptr, 0},
    {"RVK_DofRadius",  "Largest blur (pixels at 1440 lines)",                             "Depth of field", Int, 4, 48, 1, 16, nullptr, 0},
    {"RVK_DofFocus",   "Focus distance (0 = auto: your character)",                       "Depth of field", Float, 0, 200, 1, 0, nullptr, 0},
    {"RVK_DofRange",   "In-focus band around it",                                         "Depth of field", Float, 0, 0.9f, 0.05f, 0.2f, nullptr, 0},
    {"RVK_Particles",  "GPU particles on sparkle effects (listed in randy-vk.ini [Particles])", "Particles", Bool, 0, 1, 1, 1, nullptr, 0},
    {"RVK_PartCount",  "Particles per sprite",                                            "Particles",      Int,   1, 32, 1, 12, nullptr, 0},
    {"RVK_PartUniform","Same particle size for every effect (0 = by its sprite's size)", "Particles",      Float, 0, 1, 0.05f, 1, nullptr, 0},
    {"RVK_PartAbsSize","Particle size, same for every effect (world units)",             "Particles",      Float, 0.005f, 0.3f, 0.005f, 0.02f, nullptr, 0},
    {"RVK_PartSize",   "Particle size by its sprite (fraction of the sprite)",           "Particles",      Float, 0.01f, 0.6f, 0.01f, 0.15f, nullptr, 0},
    {"RVK_PartBright", "Particle brightness (hot white core from 1 up, full at 4)",       "Particles",      Float, 0.25f, 8, 0.25f, 2, nullptr, 0},
    {"RVK_PartTrail",  "Motion trails (seconds of motion shown, 0 = off)",               "Particles",      Float, 0, 0.3f, 0.01f, 0.05f, nullptr, 0},
    {"RVK_PartTrailMx","Longest trail (particle sizes)",                                  "Particles",      Float, 1, 20, 0.5f, 6, nullptr, 0},
    {"RVK_PartLife",   "Particle life (seconds)",                                         "Particles",      Float, 0.25f, 5, 0.05f, 1.5f, nullptr, 0},
    {"RVK_PartCurl",   "Flow (curl noise) speed",                                         "Particles",      Float, 0, 5, 0.1f, 1.5f, nullptr, 0},
    {"RVK_PartScale",  "Flow feature size (world units)",                                 "Particles",      Float, 0.25f, 5, 0.05f, 1.5f, nullptr, 0},
    {"RVK_PartSwirl",  "Swirl around the effect",                                         "Particles",      Float, 0, 5, 0.1f, 1, nullptr, 0},
    {"RVK_PartPull",   "Pull back towards the effect",                                    "Particles",      Float, 0, 3, 0.05f, 0.6f, nullptr, 0},
    {"RVK_PartDrag",   "How quickly particles follow the flow",                           "Particles",      Float, 0.25f, 10, 0.25f, 2.5f, nullptr, 0},
    {"RVK_PartSpeed",  "Launch speed",                                                    "Particles",      Float, 0, 4, 0.1f, 0.6f, nullptr, 0},
    {"RVK_PartAdapt",  "Adapt to each effect's own motion (0 = all alike)",               "Particles",      Float, 0, 1, 0.05f, 1, nullptr, 0},
    {"RVK_PartFollow", "Young particles follow their sprite",                             "Particles",      Float, 0, 1, 0.05f, 0.7f, nullptr, 0},
    {"RVK_PartCore",   "Brightness of the game's own sprites (1 = unchanged)",            "Particles",      Float, 0, 1, 0.05f, 0.35f, nullptr, 0},
};
constexpr uint32_t kCount = sizeof(g_settings) / sizeof(g_settings[0]);

char g_iniPath[MAX_PATH];
bool g_loaded;

Setting* Find(const char* name)
{
    for (Setting& s : g_settings)
        if (std::strcmp(s.name, name) == 0) return &s;
    return nullptr;
}

float V(const char* name) { Setting* s = Find(name); return s ? s->value : 0.0f; }

float Clamp(const Setting& s, float v)
{
    v = std::clamp(v, s.min, s.max);
    if (s.type != Float) v = std::round(v);
    return v;
}

// randy-vk.ini next to randy31.dll (the client folder).
void ResolveIniPath()
{
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&ResolveIniPath), &self);
    GetModuleFileNameA(self, g_iniPath, MAX_PATH);
    char* slash = std::strrchr(g_iniPath, '\\');
    if (slash) slash[1] = 0; else g_iniPath[0] = 0;
    std::strncat(g_iniPath, "randy-vk.ini", MAX_PATH - std::strlen(g_iniPath) - 1);
}

void Save(const Setting& s)
{
    char buf[32];
    if (s.type == Float) std::snprintf(buf, sizeof(buf), "%g", s.value);
    else std::snprintf(buf, sizeof(buf), "%d", int(s.value));
    WritePrivateProfileStringA("Renderer", s.name, buf, g_iniPath);
}

// Puts a setting into effect: each device call takes the setting and its partners.
void Apply(const Setting& s, rvk::ThreadedDevice* d)
{
    if (!d) return;
    const char* n = s.name;
    auto is = [n](const char* a) { return std::strcmp(n, a) == 0; };
    if (is("RVK_PixelLight")) d->SetPixelLighting(s.value != 0.0f);
    else if (is("RVK_LightOver")) d->SetLightOverride(s.value != 0.0f);
    else if (is("RVK_OwnLight")) d->SetCarrierLit(s.value != 0.0f);
    else if (is("RVK_Bump")) d->SetBump(s.value);
    else if (is("RVK_Headroom")) d->SetLightHeadroom(s.value);
    else if (is("RVK_Aniso")) d->SetAnisotropy(uint32_t(s.value));
    else if (is("RVK_SunShadow")) d->SetShadows(s.value != 0.0f);
    else if (is("RVK_SunStrength") || is("RVK_SunRange")) d->SetShadowParams(V("RVK_SunStrength"), V("RVK_SunRange"));
    else if (is("RVK_PtShadows")) d->SetPointShadows(uint32_t(s.value));
    else if (is("RVK_PtStrength") || is("RVK_PtDay")) d->SetPointShadowStrength(V("RVK_PtStrength"), V("RVK_PtDay"));
    else if (is("RVK_Hdr")) d->SetHdr(s.value != 0.0f);
    else if (is("RVK_Exposure") || is("RVK_Knee")) d->SetTonemap(V("RVK_Knee"), V("RVK_Exposure"));
    else if (is("RVK_HdrRoom")) d->SetHdrHeadroom(s.value);
    else if (is("RVK_Bloom") || is("RVK_BloomThr")) d->SetBloom(V("RVK_Bloom"), V("RVK_BloomThr"));
    else if (is("RVK_BloomFx")) d->SetEffectGlow(s.value);
    else if (is("RVK_Ao") || is("RVK_AoRadius")) d->SetAo(V("RVK_Ao"), V("RVK_AoRadius"));
    else if (is("RVK_Gi") || is("RVK_GiRadius")) d->SetGi(V("RVK_Gi"), V("RVK_GiRadius"));
    else if (is("RVK_MBlur") || is("RVK_MBlurNear")) d->SetMotionBlur(V("RVK_MBlur"), V("RVK_MBlurNear"));
    else if (is("RVK_MBlurObj")) d->SetMotionBlurMode(s.value != 0.0f ? 1u : 0u);
    else if (std::strncmp(n, "RVK_Part", 8) == 0 && !is("RVK_PartCore")) {
        rvk::Device::ParticleParams p;
        p.enable = V("RVK_Particles") != 0.0f;
        p.perSprite = uint32_t(V("RVK_PartCount"));
        p.size = V("RVK_PartSize");
        p.fixedSize = V("RVK_PartAbsSize");
        p.uniformSize = V("RVK_PartUniform");
        p.brightness = V("RVK_PartBright");
        p.trail = V("RVK_PartTrail");
        p.trailMax = V("RVK_PartTrailMx");
        p.life = V("RVK_PartLife");
        p.curl = V("RVK_PartCurl");
        p.scale = V("RVK_PartScale");
        p.swirl = V("RVK_PartSwirl");
        p.pull = V("RVK_PartPull");
        p.drag = V("RVK_PartDrag");
        p.speed = V("RVK_PartSpeed");
        p.adapt = V("RVK_PartAdapt");
        p.follow = V("RVK_PartFollow");
        d->SetParticleParams(p);
    }
    else if (std::strncmp(n, "RVK_Dof", 7) == 0)
        d->SetDof(V("RVK_Dof") != 0.0f, V("RVK_DofBokeh") != 0.0f, V("RVK_DofNear") != 0.0f, V("RVK_DofAmount"),
                  V("RVK_DofRadius"), V("RVK_DofFocus"), V("RVK_DofRange"), V("RVK_DofFar") != 0.0f, V("RVK_DofMacro"));
}

}  // namespace

namespace rvk_settings {

void Load()
{
    if (g_loaded) return;
    g_loaded = true;
    ResolveIniPath();
    for (Setting& s : g_settings) {
        s.value = s.defaultValue;
        char buf[64] = "";
        GetPrivateProfileStringA("Renderer", s.name, "", buf, sizeof(buf), g_iniPath);
        if (buf[0]) {
            s.value = float(std::atof(buf));
        } else if (s.legacyEnv && GetEnvironmentVariableA(s.legacyEnv, buf, sizeof(buf)) && buf[0]) {
            s.value = float(std::atof(buf));      // first run: take over the old ao-wine.sh setting
        } else if (std::strcmp(s.name, "RVK_MBlurObj") == 0 &&
                   GetEnvironmentVariableA("RANDYVK_MOTION_BLUR_MODE", buf, sizeof(buf)) && buf[0]) {
            s.value = std::strcmp(buf, "camera") == 0 ? 0.0f : 1.0f;
        }
        s.value = Clamp(s, s.value);
        Save(s);                                  // the ini always lists every setting
    }
}

float Get(const char* name)
{
    Load();
    return V(name);
}

void Set(const char* name, float value)
{
    Load();
    Setting* s = Find(name);
    if (!s) return;
    float v = Clamp(*s, value);
    if (v == s->value) return;
    s->value = v;
    Save(*s);
    Apply(*s, g_rvk.device);
    RvkLog("setting %s = %g", s->name, s->value);
}

void ApplyAll(rvk::ThreadedDevice* device)
{
    Load();
    for (const Setting& s : g_settings)
        Apply(s, device);
}

const char* IniPath()
{
    Load();
    return g_iniPath;
}

void LogAll()
{
    Load();
    RvkLog("settings (%s):", g_iniPath);
    for (const Setting& s : g_settings)
        RvkLog("  %s = %g", s.name, s.value);
}

}  // namespace rvk_settings

extern "C" {

uint32_t RvkSettings_Version() { return 1; }

uint32_t RvkSettings_Count()
{
    rvk_settings::Load();
    return kCount;
}

int RvkSettings_Get(uint32_t index, RvkSettingInfo* out)
{
    rvk_settings::Load();
    if (index >= kCount || !out || out->size < sizeof(RvkSettingInfo))
        return 0;
    const Setting& s = g_settings[index];
    out->name = s.name;
    out->label = s.label;
    out->section = s.section;
    out->type = s.type;
    out->min = s.min;
    out->max = s.max;
    out->step = s.step;
    out->value = s.value;
    out->defaultValue = s.defaultValue;
    return 1;
}

int RvkSettings_Set(const char* name, float value)
{
    if (!name || !Find(name))
        return 0;
    rvk_settings::Set(name, value);
    return 1;
}

}  // extern "C"
