// The renderer's settings table, randy-vk.ini persistence and the exported C interface (rvk_settings.h).
#include "rvk_settings.h"

#include "rvk_backend.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace rvkproxy;

namespace {

enum Type : uint32_t { Bool = 0, Int = 1, Float = 2, Choice = 3 };   // Choice: an int from `choices`

struct Setting {
    const char* name;           // also the ini key; at most 15 characters (game option variables)
    const char* label;
    const char* section;
    Type type;
    float min, max, step, defaultValue;
    const char* legacyEnv;      // read once if the ini doesn't have the setting yet (the old ao-wine.sh variables)
    float value;
    const char* parent;         // the feature's on / off setting: while it is off this one takes its game value
                                // (kVanilla; settings not listed there only tune the feature)
    const char* choices;        // Choice: the allowed values, separated by spaces
};

// Every renderer option. Order = display order within each section.
Setting g_settings[] = {
    // name               label                                                     section             type    min    max    step   default env                        value parent           choices
    {"RVK_Enhance",    "All renderer enhancements (off = the game's own look; Ctrl+Shift+E)", "General", Bool, 0, 1, 1, 1, nullptr, 0},
    {"RVK_Prepass",    "Depth pre-pass (faster: each pixel of the scene lit about once)", "General", Bool, 0, 1, 1, 1, nullptr, 0},
    {"RVK_UiRate",     "Interface redraws a second (0 = every frame; fewer: faster, the interface updates less often)", "General", Choice, 0, 120, 1, 0, nullptr, 0, nullptr, "0 30 60 90 120"},

    {"RVK_PixelLight", "Per-pixel lighting",                                       "Lighting",         Bool,  0, 1, 1, 1, "RANDYVK_PIXEL_LIGHTING", 0},
    {"RVK_LightOver",  "All nearby lights light every surface (light override)",   "Lighting",         Bool,  0, 1, 1, 1, "RANDYVK_LIGHT_OVERRIDE", 0},
    {"RVK_Headroom",   "Light headroom without HDR",                               "Lighting",         Float, 1, 2, 0.05f, 1.25f, "RANDYVK_LIGHT_HEADROOM", 0, "RVK_LightOver"},
    {"RVK_OwnLight",   "A character's own light lights the character",             "Lighting",         Bool,  0, 1, 1, 1, nullptr, 0, "RVK_LightOver"},
    {"RVK_BumpOn",     "Generated surface relief (normal maps from textures)",     "Lighting",         Bool,  0, 1, 1, 1, nullptr, 0},
    {"RVK_Bump",       "Relief strength",                                          "Lighting",         Float, 0.25f, 4, 0.25f, 1.5f, "RANDYVK_BUMP", 0, "RVK_BumpOn"},
    {"RVK_NormalMaps", "Normal maps (from the randy-vk materials folder)",          "Lighting",         Bool,  0, 1, 1, 1, nullptr, 0},
    {"RVK_NormalStr",  "Strength",                                                 "Lighting",         Float, 0.1f, 4, 0.1f, 1, nullptr, 0, "RVK_NormalMaps"},
    {"RVK_LeafOn",     "Sunlight through leaves",                                  "Lighting",         Bool,  0, 1, 1, 1, nullptr, 0},
    {"RVK_LeafLight",  "Strength",                                                 "Lighting",         Float, 0.25f, 2, 0.25f, 1, nullptr, 0, "RVK_LeafOn"},
    {"RVK_PtLight",    "Point light intensity (lamps, fires, other lights)",       "Lighting",         Float, 0.25f, 2, 0.05f, 1, nullptr, 0},
    {"RVK_CharLight",  "Character light intensity (lights characters carry, yours too)", "Lighting",  Float, 0.1f, 2, 0.05f, 1, nullptr, 0},
    {"RVK_TessOn",     "Rounder characters (Phong tessellation)",                  "Lighting",         Bool,  0, 1, 1, 1, nullptr, 0},
    {"RVK_Tess",       "Roundness",                                                "Lighting",         Float, 0.25f, 1, 0.05f, 0.75f, nullptr, 0, "RVK_TessOn"},
    {"RVK_TessLevel",  "Detail up close (pieces per triangle edge)",               "Lighting",         Int,   2, 8, 1, 4, nullptr, 0, "RVK_TessOn"},
    {"RVK_TessDist",   "Up to this distance (world units)",                        "Lighting",         Int,   5, 60, 1, 20, nullptr, 0, "RVK_TessOn"},
    {"RVK_Aniso",      "Anisotropic filtering (1 = off)",                          "Lighting",         Choice, 1, 16, 1, 16, "RANDYVK_ANISOTROPY", 0, nullptr, "1 2 4 8 16"},

    {"RVK_SwayOn",     "Plants sway in the wind",                                  "Plants",           Bool,  0, 1, 1, 1, nullptr, 0},
    {"RVK_Sway",       "Sway strength",                                            "Plants",           Float, 0.25f, 3, 0.25f, 1, nullptr, 0, "RVK_SwayOn"},
    {"RVK_PushOn",     "Grass and plants bend away from characters",               "Plants",           Bool,  0, 1, 1, 1, nullptr, 0},
    {"RVK_GrassPush",  "How far plants lean",                                      "Plants",           Float, 0.25f, 2, 0.25f, 1, nullptr, 0, "RVK_PushOn"},
    {"RVK_PlantDetOn", "Split big plant quads so they bend smoothly",              "Plants",           Bool,  0, 1, 1, 1, nullptr, 0},
    {"RVK_PlantDetail","Detail (pieces per 0.3 units)",                            "Plants",           Float, 0.5f, 2, 0.5f, 1, nullptr, 0, "RVK_PlantDetOn"},
    {"RVK_FolEdges",   "Soft leaf edges drawn over the finished scene (no see-through outlines)", "Plants", Bool, 0, 1, 1, 1, nullptr, 0},
    {"RVK_FolLodOn",   "Cheaper shading of distant foliage",                       "Plants",           Bool,  0, 1, 1, 1, nullptr, 0},
    {"RVK_FoliageLod", "From this distance (world units)",                         "Plants",           Int,   10, 150, 1, 35, nullptr, 0, "RVK_FolLodOn"},

    {"RVK_SunShadow",  "Sun shadows",                                              "Shadows",          Bool,  0, 1, 1, 1, "RANDYVK_SHADOWS", 0},
    {"RVK_SunRes",     "Resolution (4096 = 256 MB, 8192 = 1 GB of video memory)",  "Shadows",          Choice, 1024, 8192, 1, 4096, nullptr, 0, "RVK_SunShadow", "1024 2048 4096 8192"},
    {"RVK_SunStrength","Strength",                                                 "Shadows",          Float, 0, 1, 0.05f, 0.65f, "RANDYVK_SHADOW_STRENGTH", 0, "RVK_SunShadow"},
    {"RVK_SunDist",    "Distance (world units)",                                   "Shadows",          Int,   40, 1000, 1, 400, nullptr, 0, "RVK_SunShadow"},
    {"RVK_SunCascade", "Cascades (more = sharper near, same reach)",               "Shadows",          Int,   1, 4, 1, 4, nullptr, 0, "RVK_SunShadow"},
    {"RVK_SunSoft",    "Softness (penumbra grows with distance; 0 = hard)",        "Shadows",          Float, 0, 4, 0.25f, 1, nullptr, 0, "RVK_SunShadow"},
    {"RVK_ContactOn",  "Contact shadows (small sun shadows the map misses)",       "Shadows",          Bool,  0, 1, 1, 1, nullptr, 0},
    {"RVK_Contact",    "Strength",                                                 "Shadows",          Float, 0.05f, 1, 0.05f, 0.6f, nullptr, 0, "RVK_ContactOn"},
    {"RVK_PtOn",       "Point light shadows",                                      "Shadows",          Bool,  0, 1, 1, 1, nullptr, 0},
    {"RVK_PtShadows",  "Lights with shadows",                                      "Shadows",          Int,   1, 16, 1, 8, "RANDYVK_POINT_SHADOWS", 0, "RVK_PtOn"},
    {"RVK_PtRes",      "Resolution (per cube face; 16 lights at 1024 = 384 MB)",                 "Shadows",          Choice, 256, 2048, 1, 1024, nullptr, 0, "RVK_PtOn", "256 512 1024 2048"},
    {"RVK_PtStrength", "Strength",                                                 "Shadows",          Float, 0, 1, 0.05f, 0.9f, "RANDYVK_POINT_SHADOW_STRENGTH", 0, "RVK_PtOn"},
    {"RVK_PtDay",      "Strength in daylight (fraction)",                          "Shadows",          Float, 0, 1, 0.05f, 0.25f, "RANDYVK_POINT_SHADOW_DAY", 0, "RVK_PtOn"},

    {"RVK_Hdr",        "HDR scene and tone mapping",                               "HDR and effects",  Bool,  0, 1, 1, 1, "RANDYVK_HDR", 0},
    {"RVK_Exposure",   "Exposure",                                                 "HDR and effects",  Float, 0.5f, 2, 0.05f, 1, "RANDYVK_EXPOSURE", 0, "RVK_Hdr"},
    {"RVK_Knee",       "Tone mapping knee (1 = only clip)",                        "HDR and effects",  Float, 0.5f, 1, 0.05f, 0.85f, "RANDYVK_TONEMAP_KNEE", 0, "RVK_Hdr"},
    {"RVK_HdrRoom",    "Local light headroom",                                     "HDR and effects",  Float, 1, 4, 0.25f, 1.5f, "RANDYVK_HDR_HEADROOM", 0, "RVK_Hdr"},
    {"RVK_BloomOn",    "Bloom",                                                    "HDR and effects",  Bool,  0, 1, 1, 1, nullptr, 0},
    {"RVK_Bloom",      "Strength",                                                 "HDR and effects",  Float, 0.25f, 5, 0.25f, 1.5f, "RANDYVK_BLOOM", 0, "RVK_BloomOn"},
    {"RVK_BloomThr",   "Threshold (1 = above white)",                              "HDR and effects",  Float, 0.5f, 2, 0.05f, 1, "RANDYVK_BLOOM_THRESHOLD", 0, "RVK_BloomOn"},
    {"RVK_BloomOcc",   "Over objects in front of its light (1 = unchanged)",       "HDR and effects",  Float, 0, 1, 0.05f, 0.15f, nullptr, 0, "RVK_BloomOn"},
    {"RVK_FxGlowOn",   "Glow of effects (spells, fire, light halos)",              "HDR and effects",  Bool,  0, 1, 1, 1, nullptr, 0},
    {"RVK_BloomFx",    "Strength",                                                 "HDR and effects",  Float, 0.25f, 4, 0.25f, 1, "RANDYVK_BLOOM_EFFECTS", 0, "RVK_FxGlowOn"},
    {"RVK_NightOn",    "Night glow of windows, signs and screens",                 "HDR and effects",  Bool,  0, 1, 1, 1, nullptr, 0},
    {"RVK_NightGlow",  "Strength",                                                 "HDR and effects",  Float, 0.25f, 4, 0.25f, 1.5f, nullptr, 0, "RVK_NightOn"},
    {"RVK_AoOn",       "Ambient occlusion",                                        "HDR and effects",  Bool,  0, 1, 1, 1, nullptr, 0},
    {"RVK_Ao",         "Strength",                                                 "HDR and effects",  Float, 0.25f, 3, 0.25f, 1, "RANDYVK_AO", 0, "RVK_AoOn"},
    {"RVK_AoRadius",   "Radius (world units)",                                     "HDR and effects",  Float, 0.25f, 4, 0.25f, 1.5f, "RANDYVK_AO_RADIUS", 0, "RVK_AoOn"},
    {"RVK_GiOn",       "Indirect light (bounce light from the lit scene)",         "HDR and effects",  Bool,  0, 1, 1, 1, nullptr, 0},
    {"RVK_Gi",         "Strength",                                                 "HDR and effects",  Float, 0.25f, 3, 0.25f, 1, nullptr, 0, "RVK_GiOn"},
    {"RVK_GiRadius",   "Reach (world units)",                                      "HDR and effects",  Float, 1, 12, 0.5f, 4, nullptr, 0, "RVK_GiOn"},
    {"RVK_VolOn",      "Volumetric light (sun shafts)",                            "HDR and effects",  Bool,  0, 1, 1, 1, nullptr, 0},
    {"RVK_Volume",     "Strength",                                                 "HDR and effects",  Float, 0.25f, 4, 0.25f, 1, nullptr, 0, "RVK_VolOn"},
    {"RVK_VolHaze",    "How hazy the air is",                                      "HDR and effects",  Float, 0.25f, 4, 0.25f, 1, nullptr, 0, "RVK_VolOn"},
    {"RVK_VolShafts",  "Shaft contrast (0 = physical)",                            "HDR and effects",  Float, 0, 3, 0.25f, 1, nullptr, 0, "RVK_VolOn"},
    {"RVK_SsrOn",      "Reflections (screen-space)",                               "HDR and effects",  Bool,  0, 1, 1, 1, nullptr, 0},
    {"RVK_Ssr",        "Strength",                                                 "HDR and effects",  Float, 0.25f, 2, 0.25f, 1, nullptr, 0, "RVK_SsrOn"},
    {"RVK_SsrWater",   "Water",                                                    "HDR and effects",  Float, 0, 1, 0.05f, 1, nullptr, 0, "RVK_SsrOn"},
    {"RVK_SsrGloss",   "Glossy surfaces",                                          "HDR and effects",  Float, 0, 1, 0.05f, 0.3f, nullptr, 0, "RVK_SsrOn"},
    {"RVK_SsrWet",     "Wet look of floors and ground (0 = dry)",                  "HDR and effects",  Float, 0, 1, 0.05f, 0, nullptr, 0, "RVK_SsrOn"},
    {"RVK_MBlurOn",    "Motion blur",                                              "HDR and effects",  Bool,  0, 1, 1, 1, nullptr, 0},
    {"RVK_MBlurObj",   "Per-object (off = camera only)",                           "HDR and effects",  Bool,  0, 1, 1, 1, nullptr, 0, "RVK_MBlurOn"},
    {"RVK_MBlur",      "Exposure (fraction of 1/60 s)",                            "HDR and effects",  Float, 0.05f, 2, 0.05f, 0.5f, "RANDYVK_MOTION_BLUR", 0, "RVK_MBlurOn"},
    {"RVK_MBlurNear",  "Camera blur: sharp nearer than (world units)",             "HDR and effects",  Float, 2, 20, 0.5f, 8, "RANDYVK_MOTION_BLUR_NEAR", 0, "RVK_MBlurOn"},

    {"RVK_Taa",        "Temporal anti-aliasing",                                   "Anti-aliasing",    Bool,  0, 1, 1, 1, nullptr, 0},
    {"RVK_Sharpen",    "Sharpening after it (0 = none)",                           "Anti-aliasing",    Float, 0, 1, 0.05f, 0.4f, nullptr, 0, "RVK_Taa"},

    {"RVK_GradeOn",    "Colour grading",                                           "Colour grading",   Bool,  0, 1, 1, 1, nullptr, 0},
    {"RVK_Saturation", "Saturation",                                               "Colour grading",   Float, 0, 2, 0.05f, 1, nullptr, 0, "RVK_GradeOn"},
    {"RVK_Contrast",   "Contrast",                                                 "Colour grading",   Float, 0.5f, 1.5f, 0.05f, 1, nullptr, 0, "RVK_GradeOn"},
    {"RVK_Warmth",     "Warmth (- cooler, + warmer)",                              "Colour grading",   Float, -1, 1, 0.05f, 0, nullptr, 0, "RVK_GradeOn"},
    {"RVK_NightTint",  "Night: cooler, paler colours",                             "Colour grading",   Float, 0, 1, 0.05f, 0.3f, nullptr, 0, "RVK_GradeOn"},
    {"RVK_Vignette",   "Vignette (darker corners)",                                "Colour grading",   Float, 0, 1, 0.05f, 0, nullptr, 0, "RVK_GradeOn"},
    {"RVK_LutAmount",  "Look-up tables randy-vk-day/night.cube (Ctrl+Shift+L reloads)", "Colour grading", Float, 0, 1, 0.05f, 1, nullptr, 0, "RVK_GradeOn"},

    {"RVK_Dof",        "Depth of field",                                           "Depth of field",   Bool,  0, 1, 1, 1, nullptr, 0},
    {"RVK_DofBokeh",   "Bokeh (hexagonal highlights; off = smooth blur)",          "Depth of field",   Bool,  0, 1, 1, 1, nullptr, 0, "RVK_Dof"},
    {"RVK_DofNear",    "Blur in front of the focus",                               "Depth of field",   Bool,  0, 1, 1, 1, nullptr, 0, "RVK_Dof"},
    {"RVK_DofFar",     "Blur behind the focus (always)",                           "Depth of field",   Bool,  0, 1, 1, 0, nullptr, 0, "RVK_Dof"},
    {"RVK_DofMacro",   "Close focus: blur behind it when nearer than (world units)", "Depth of field", Float, 0, 10, 0.5f, 3, nullptr, 0, "RVK_Dof"},
    {"RVK_DofAmount",  "Blur strength",                                            "Depth of field",   Float, 0, 2, 0.05f, 0.5f, nullptr, 0, "RVK_Dof"},
    {"RVK_DofRadius",  "Largest blur (pixels at 1440 lines)",                      "Depth of field",   Int,   4, 48, 1, 16, nullptr, 0, "RVK_Dof"},
    {"RVK_DofFocus",   "Focus distance (0 = auto: your character)",                "Depth of field",   Int,   0, 200, 1, 0, nullptr, 0, "RVK_Dof"},
    {"RVK_DofRange",   "In-focus band around it",                                  "Depth of field",   Float, 0, 0.9f, 0.05f, 0.2f, nullptr, 0, "RVK_Dof"},

    {"RVK_Particles",  "GPU particles on sparkle effects (listed in randy-vk.ini [Particles])", "Particles", Bool, 0, 1, 1, 1, nullptr, 0},
    {"RVK_PartCount",  "Particles per sprite",                                     "Particles",        Int,   1, 32, 1, 12, nullptr, 0, "RVK_Particles"},
    {"RVK_PartUniform","Same particle size for every effect (0 = by its sprite's size)", "Particles",   Float, 0, 1, 0.05f, 1, nullptr, 0, "RVK_Particles"},
    {"RVK_PartAbsSize","Particle size, same for every effect (world units)",       "Particles",        Float, 0.005f, 0.3f, 0.005f, 0.02f, nullptr, 0, "RVK_Particles"},
    {"RVK_PartSize",   "Particle size by its sprite (fraction of the sprite)",     "Particles",        Float, 0.01f, 0.6f, 0.01f, 0.15f, nullptr, 0, "RVK_Particles"},
    {"RVK_PartBright", "Particle brightness (hot white core from 1 up, full at 4)", "Particles",        Float, 0.25f, 8, 0.25f, 2, nullptr, 0, "RVK_Particles"},
    {"RVK_PartTrail",  "Motion trails (seconds of motion shown, 0 = off)",         "Particles",        Float, 0, 0.3f, 0.01f, 0.05f, nullptr, 0, "RVK_Particles"},
    {"RVK_PartTrailMx","Longest trail (particle sizes)",                           "Particles",        Float, 1, 20, 0.5f, 6, nullptr, 0, "RVK_Particles"},
    {"RVK_PartLife",   "Particle life (seconds)",                                  "Particles",        Float, 0.25f, 5, 0.05f, 1.5f, nullptr, 0, "RVK_Particles"},
    {"RVK_PartCurl",   "Flow (curl noise) speed",                                  "Particles",        Float, 0, 5, 0.1f, 1.5f, nullptr, 0, "RVK_Particles"},
    {"RVK_PartScale",  "Flow feature size (world units)",                          "Particles",        Float, 0.25f, 5, 0.05f, 1.5f, nullptr, 0, "RVK_Particles"},
    {"RVK_PartSwirl",  "Swirl around the effect",                                  "Particles",        Float, 0, 5, 0.1f, 1, nullptr, 0, "RVK_Particles"},
    {"RVK_PartPull",   "Pull back towards the effect",                             "Particles",        Float, 0, 3, 0.05f, 0.6f, nullptr, 0, "RVK_Particles"},
    {"RVK_PartDrag",   "How quickly particles follow the flow",                    "Particles",        Float, 0.25f, 10, 0.25f, 2.5f, nullptr, 0, "RVK_Particles"},
    {"RVK_PartSpeed",  "Launch speed",                                             "Particles",        Float, 0, 4, 0.1f, 0.6f, nullptr, 0, "RVK_Particles"},
    {"RVK_PartAdapt",  "Adapt to each effect's own motion (0 = all alike)",        "Particles",        Float, 0, 1, 0.05f, 1, nullptr, 0, "RVK_Particles"},
    {"RVK_PartFollow", "Young particles follow their sprite",                      "Particles",        Float, 0, 1, 0.05f, 0.7f, nullptr, 0, "RVK_Particles"},
    {"RVK_PartCore",   "Brightness of the game's own sprites (1 = unchanged)",     "Particles",        Float, 0, 1, 0.05f, 0.35f, nullptr, 0, "RVK_Particles"},
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

// With RVK_Enhance off, every enhancement takes the value that leaves the game's own look (the stored settings stay,
// so switching it back on restores them). Settings not listed here only tune something these switch off.
struct Vanilla { const char* name; float value; };
const Vanilla kVanilla[] = {
    {"RVK_PixelLight", 0}, {"RVK_LightOver", 0}, {"RVK_Bump", 0}, {"RVK_NormalMaps", 0}, {"RVK_LeafLight", 0}, {"RVK_Headroom", 1},
    {"RVK_Sway", 0}, {"RVK_FolEdges", 0}, {"RVK_GrassPush", 0}, {"RVK_PlantDetail", 0}, {"RVK_FoliageLod", 0}, {"RVK_PtLight", 1}, {"RVK_CharLight", 1}, {"RVK_Tess", 0}, {"RVK_Aniso", 1}, {"RVK_SunShadow", 0}, {"RVK_Contact", 0}, {"RVK_PtShadows", 0},
    {"RVK_Hdr", 0}, {"RVK_Bloom", 0}, {"RVK_BloomFx", 0}, {"RVK_NightGlow", 0}, {"RVK_Ao", 0}, {"RVK_Gi", 0},
    {"RVK_Volume", 0}, {"RVK_Ssr", 0}, {"RVK_MBlur", 0}, {"RVK_Taa", 0}, {"RVK_Saturation", 1}, {"RVK_Contrast", 1},
    {"RVK_Warmth", 0}, {"RVK_NightTint", 0}, {"RVK_Vignette", 0}, {"RVK_LutAmount", 0}, {"RVK_Dof", 0},
    {"RVK_Particles", 0}, {"RVK_PartCore", 1},
};

float Stored(const char* name) { Setting* s = Find(name); return s ? s->value : 0.0f; }

const Vanilla* FindVanilla(const char* name)
{
    for (const Vanilla& v : kVanilla)
        if (std::strcmp(v.name, name) == 0) return &v;
    return nullptr;
}

// The value a setting takes effect with: the game's (kVanilla) with RVK_Enhance off or its feature switched off.
float V(const char* name)
{
    Setting* s = Find(name);
    if (!s) return 0.0f;
    const Vanilla* vanilla = FindVanilla(name);
    if (vanilla && (Stored("RVK_Enhance") == 0.0f || (s->parent && Stored(s->parent) == 0.0f)))
        return vanilla->value;
    return s->value;
}

float Clamp(const Setting& s, float v)
{
    v = std::clamp(v, s.min, s.max);
    if (s.type != Float) v = std::round(v);
    if (s.type == Choice && s.choices) {         // the nearest allowed value
        float best = v, bestDistance = 1e30f;
        for (const char* c = s.choices; *c;) {
            char* next;
            float choice = std::strtof(c, &next);
            if (next == c) break;
            if (std::fabs(choice - v) < bestDistance) { bestDistance = std::fabs(choice - v); best = choice; }
            c = next;
        }
        v = best;
    }
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

void ApplyOne(const Setting& s, rvk::ThreadedDevice* d);

// Puts a setting into effect - and, for a feature's on / off setting, the settings it switches.
void Apply(const Setting& s, rvk::ThreadedDevice* d)
{
    ApplyOne(s, d);
    for (const Setting& child : g_settings)
        if (child.parent && std::strcmp(child.parent, s.name) == 0) ApplyOne(child, d);
}

// Puts one setting into effect: each device call takes the setting and its partners.
void ApplyOne(const Setting& s, rvk::ThreadedDevice* d)
{
    if (!d) return;
    const char* n = s.name;
    auto is = [n](const char* a) { return std::strcmp(n, a) == 0; };
    if (is("RVK_Enhance")) {                     // every enhancement at its own value or the game's
        for (const Setting& other : g_settings)
            if (&other != &s) Apply(other, d);
    }
    else if (is("RVK_Prepass")) d->SetDepthPrepass(V(n) != 0.0f);
    else if (is("RVK_UiRate")) SetInterfaceRate(V(n));
    else if (is("RVK_FolEdges")) d->SetFoliageEdges(V(n) != 0.0f);
    else if (is("RVK_PixelLight")) d->SetPixelLighting(V(n) != 0.0f);
    else if (is("RVK_LightOver")) d->SetLightOverride(V(n) != 0.0f);
    else if (is("RVK_OwnLight")) d->SetCarrierLit(V(n) != 0.0f);
    else if (is("RVK_Bump")) d->SetBump(V(n));
    else if (is("RVK_NormalMaps") || is("RVK_NormalStr")) d->SetNormalMaps(V("RVK_NormalMaps") != 0.0f, V("RVK_NormalStr"));
    else if (is("RVK_Headroom")) d->SetLightHeadroom(V(n));
    else if (is("RVK_Aniso")) d->SetAnisotropy(uint32_t(V(n)));
    else if (is("RVK_Tess") || is("RVK_TessLevel") || is("RVK_TessDist"))
        d->SetTessellation(V("RVK_Tess"), V("RVK_TessDist"), uint32_t(V("RVK_TessLevel")));
    else if (is("RVK_PtLight") || is("RVK_CharLight")) d->SetPointLightIntensity(V("RVK_PtLight"), V("RVK_CharLight"));
    else if (is("RVK_SunShadow")) d->SetShadows(V(n) != 0.0f);
    else if (is("RVK_SunRes") || is("RVK_PtRes"))
        d->SetShadowResolution(uint32_t(V("RVK_SunRes")), uint32_t(V("RVK_PtRes")));
    else if (is("RVK_SunStrength") || is("RVK_SunDist") || is("RVK_SunCascade"))
        d->SetShadowParams(V("RVK_SunStrength"), V("RVK_SunDist"), uint32_t(V("RVK_SunCascade")));
    else if (is("RVK_PtShadows")) d->SetPointShadows(uint32_t(V(n)));
    else if (is("RVK_PtStrength") || is("RVK_PtDay")) d->SetPointShadowStrength(V("RVK_PtStrength"), V("RVK_PtDay"));
    else if (is("RVK_Hdr")) d->SetHdr(V(n) != 0.0f);
    else if (is("RVK_Exposure") || is("RVK_Knee")) d->SetTonemap(V("RVK_Knee"), V("RVK_Exposure"));
    else if (is("RVK_HdrRoom")) d->SetHdrHeadroom(V(n));
    else if (is("RVK_Bloom") || is("RVK_BloomThr")) d->SetBloom(V("RVK_Bloom"), V("RVK_BloomThr"));
    else if (is("RVK_BloomFx")) d->SetEffectGlow(V(n));
    else if (is("RVK_BloomOcc")) d->SetBloomOverNearer(V(n));
    else if (is("RVK_Sway")) d->SetSway(V(n));
    else if (is("RVK_GrassPush")) d->SetGrassPush(V(n));
    else if (is("RVK_PlantDetail")) d->SetPlantDetail(V(n));
    else if (is("RVK_FoliageLod")) d->SetFoliageLod(V(n));
    else if (is("RVK_Taa") || is("RVK_Sharpen")) d->SetTaa(V("RVK_Taa") != 0.0f, V("RVK_Sharpen"));
    else if (is("RVK_Saturation") || is("RVK_Contrast") || is("RVK_Warmth") || is("RVK_NightTint") ||
             is("RVK_Vignette") || is("RVK_LutAmount"))
        d->SetGrading(V("RVK_Saturation"), V("RVK_Contrast"), V("RVK_Warmth"), V("RVK_LutAmount"), V("RVK_NightTint"),
                      V("RVK_Vignette"));
    else if (is("RVK_SunSoft")) d->SetSunSoftness(V(n));
    else if (is("RVK_LeafLight")) d->SetLeafLight(V(n));
    else if (is("RVK_NightGlow")) d->SetNightGlow(V(n));
    else if (is("RVK_Contact")) d->SetContactShadows(V(n));
    else if (is("RVK_Ao") || is("RVK_AoRadius")) d->SetAo(V("RVK_Ao"), V("RVK_AoRadius"));
    else if (is("RVK_Gi") || is("RVK_GiRadius")) d->SetGi(V("RVK_Gi"), V("RVK_GiRadius"));
    else if (std::strncmp(n, "RVK_Ssr", 7) == 0)
        d->SetSsr(V("RVK_Ssr"), V("RVK_SsrWater"), V("RVK_SsrGloss"), V("RVK_SsrWet"));
    else if (std::strncmp(n, "RVK_Vol", 7) == 0) d->SetVolume(V("RVK_Volume"), V("RVK_VolHaze"), V("RVK_VolShafts"));
    else if (is("RVK_MBlur") || is("RVK_MBlurNear")) d->SetMotionBlur(V("RVK_MBlur"), V("RVK_MBlurNear"));
    else if (is("RVK_MBlurObj")) d->SetMotionBlurMode(V(n) != 0.0f ? 1u : 0u);
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
    std::vector<bool> inIni;
    for (Setting& s : g_settings) {
        s.value = s.defaultValue;
        char buf[64] = "";
        GetPrivateProfileStringA("Renderer", s.name, "", buf, sizeof(buf), g_iniPath);
        inIni.push_back(buf[0] != 0);
        if (buf[0]) {
            s.value = float(std::atof(buf));
        } else if (s.legacyEnv && GetEnvironmentVariableA(s.legacyEnv, buf, sizeof(buf)) && buf[0]) {
            s.value = float(std::atof(buf));      // first run: take over the old ao-wine.sh setting
        } else if (std::strcmp(s.name, "RVK_MBlurObj") == 0 &&
                   GetEnvironmentVariableA("RANDYVK_MOTION_BLUR_MODE", buf, sizeof(buf)) && buf[0]) {
            s.value = std::strcmp(buf, "camera") == 0 ? 0.0f : 1.0f;
        }
    }
    // A feature's on / off setting new to the ini (older ones had "0 = off" strengths): on unless its strength (the
    // first setting it switches with a game value) was at the game's value - which then gets its default back, so
    // that switching the feature on shows it.
    for (size_t i = 0; i < kCount; ++i) {
        Setting& t = g_settings[i];
        if (inIni[i] || t.type != Bool) continue;
        for (Setting& child : g_settings) {
            const Vanilla* vanilla = child.parent && std::strcmp(child.parent, t.name) == 0 ? FindVanilla(child.name) : nullptr;
            if (!vanilla) continue;
            bool on = false;
            for (const Setting& other : g_settings)
                if (other.parent && std::strcmp(other.parent, t.name) == 0)
                    if (const Vanilla* ov = FindVanilla(other.name)) on = on || other.value != ov->value;
            t.value = on ? 1.0f : 0.0f;
            if (!on && child.value == vanilla->value) child.value = child.defaultValue;
            break;
        }
    }
    for (Setting& s : g_settings) {
        s.value = Clamp(s, s.value);
        Save(s);                                  // the ini always lists every setting
    }
}

float Get(const char* name)
{
    Load();
    return Stored(name);
}

// Puts a value into effect without storing it (the profiling sweep); Restore puts the stored one back.
void ApplyTemporary(const char* name, float value)
{
    Load();
    Setting* s = Find(name);
    if (!s || !g_rvk.device) return;
    float stored = s->value;
    s->value = value;
    Apply(*s, g_rvk.device);
    s->value = stored;
}

void Restore(const char* name)
{
    Load();
    Setting* s = Find(name);
    if (s && g_rvk.device) Apply(*s, g_rvk.device);
}

float GetEffective(const char* name)
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
    LoadLuts(device);
}

// A .cube lookup table (Adobe / Resolve: LUT_3D_SIZE n, then n^3 lines of r g b in 0..1, red fastest) as RGBA8.
// False: missing or unreadable.
static bool ReadCube(const char* path, uint32_t* size, std::vector<uint8_t>* rgba)
{
    FILE* f = std::fopen(path, "r");
    if (!f) return false;
    char line[256];
    uint32_t n = 0;
    size_t count = 0;
    while (std::fgets(line, sizeof(line), f)) {
        if (std::strncmp(line, "LUT_3D_SIZE", 11) == 0) {
            n = uint32_t(std::atoi(line + 11));
            if (n < 2 || n > 64) break;
            rgba->assign(size_t(n) * n * n * 4, 255);
            continue;
        }
        float r, g, b;
        if (!n || line[0] == '#' || std::sscanf(line, "%f %f %f", &r, &g, &b) != 3) continue;
        if (count >= size_t(n) * n * n) break;
        uint8_t* p = &(*rgba)[count++ * 4];
        p[0] = uint8_t(std::clamp(r, 0.0f, 1.0f) * 255.0f + 0.5f);
        p[1] = uint8_t(std::clamp(g, 0.0f, 1.0f) * 255.0f + 0.5f);
        p[2] = uint8_t(std::clamp(b, 0.0f, 1.0f) * 255.0f + 0.5f);
    }
    std::fclose(f);
    if (!n || count != size_t(n) * n * n) {
        RvkLog("colour lookup table %s: not a complete 3D .cube table", path);
        return false;
    }
    *size = n;
    return true;
}

void LoadLuts(rvk::ThreadedDevice* device)
{
    if (!device) return;
    Load();
    // randy-vk-day.cube and randy-vk-night.cube next to randy-vk.ini; the day table also serves the night if alone.
    std::string dir(g_iniPath);
    size_t slash = dir.find_last_of('\\');
    dir = slash == std::string::npos ? std::string() : dir.substr(0, slash + 1);
    uint32_t size[2] = {};
    std::vector<uint8_t> data[2];
    const char* names[2] = {"randy-vk-day.cube", "randy-vk-night.cube"};
    for (int i = 0; i < 2; ++i)
        if (ReadCube((dir + names[i]).c_str(), &size[i], &data[i]))
            RvkLog("colour lookup table %s: %u^3", names[i], size[i]);
    if (!size[1] && size[0]) { size[1] = size[0]; data[1] = data[0]; }
    for (uint32_t i = 0; i < 2; ++i)
        device->SetColorLut(i, size[i], size[i] ? data[i].data() : nullptr);
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

uint32_t RvkSettings_Version() { return 2; }

uint32_t RvkSettings_Count()
{
    rvk_settings::Load();
    return kCount;
}

int RvkSettings_Get(uint32_t index, RvkSettingInfo* out)
{
    rvk_settings::Load();
    if (index >= kCount || !out || out->size < offsetof(RvkSettingInfo, parent))   // version 1 callers: no parent
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
    if (out->size >= sizeof(RvkSettingInfo)) {
        out->parent = s.parent;
        out->choices = s.choices;
    }
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
