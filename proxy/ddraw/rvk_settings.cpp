// The renderer's settings table, randy-vk.ini persistence and the exported C interface (rvk_settings.h).
#include "rvk_settings.h"

#include "rvk_backend.h"
#include "native/mesh.h"
#include "native/camera.h"
#include "native/occluder.h"
#include "native/ground.h"
#include "native/rings.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

using namespace rvkproxy;

namespace {

enum Type : uint32_t { Bool = 0, Int = 1, Float = 2, Choice = 3, Colour = 4 };   // Choice: an int from `choices`;
                                                                                // Colour: RRGGBB (hex in the ini)

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
    {"RVK_Enhance",    "All enhancements (Ctrl+Shift+E)", "General", Bool, 0, 1, 1, 1, nullptr, 0},
    {"RVK_Prepass",    "Depth pre-pass", "General", Bool, 0, 1, 1, 1, nullptr, 0},
    {"RVK_LiteShader", "The interface drawn with a lean shader of its own (the same picture, far less GPU time)", "General", Bool, 0, 1, 1, 1, nullptr, 0},
    {"RVK_PrepassEq",  "Depth pre-pass: what it covers shaded only where it is seen", "General", Bool, 0, 1, 1, 1, nullptr, 0, "RVK_Prepass"},
    {"RVK_PrepassMore", "Depth pre-pass: a second stretch after the water's copy of the scene", "General", Bool, 0, 1, 1, 1, nullptr, 0, "RVK_Prepass"},
    {"RVK_UiRate", "Interface refresh rate (0 = every frame)", "General", Choice, 0, 120, 1, 60, nullptr, 0, nullptr, "0 30 60 90 120"},
    {"RVK_ZoneFade", "Zone changes: the last view stays up and fades into the new zone once it has loaded (off: the blank wait)", "General", Bool, 0, 1, 1, 1, nullptr, 0},
    {"RVK_ZoneHoldMax", "Zone changes: the longest the new zone stays hidden once it draws (seconds)", "General", Float, 0.5f, 5, 0.1f, 1.5f, nullptr, 0, "RVK_ZoneFade"},
    {"RVK_Portals", "Shops' insides seen through their doors (once you have been in one of the kind)", "General", Bool, 0, 1, 1, 1, nullptr, 0},
    {"RVK_PortalDist", "Shops' insides through their doors: drawn within this distance", "General", Float, 10, 200, 5, 60, nullptr, 0, "RVK_Portals"},
    {"RVK_PortalDim", "Shops' insides through their doors: their brightness by day (1 = as inside)", "General", Float, 0, 1, 0.05f, 0.7f, nullptr, 0, "RVK_Portals"},
    {"RVK_StreetView", "The street seen from inside a shop through its open door (once you have stood at that door outside)", "General", Bool, 0, 1, 1, 1, nullptr, 0, "RVK_Portals"},
    {"RVK_StreetNow", "The street seen from inside a shop: lit for the hour it is now (off: as when it was recorded)", "General", Bool, 0, 1, 1, 1, nullptr, 0, "RVK_StreetView"},
    {"RVK_PerfHud", "Performance readout in the top left: frames a second, the game's and the renderer's CPU time, the GPU's, client memory and VRAM", "General", Bool, 0, 1, 1, 0, nullptr, 0},
    {"RVK_AlienMap", "AI missions' PF map: rooms shown black on green (the game's is black on black)", "General", Bool, 0, 1, 1, 1, nullptr, 0},
    {"RVK_DetailRange", "How far out rocks and shrubs are shown, x the game's (1: the game's own; more draws)", "General", Float, 1, 4, 0.25f, 2, nullptr, 0},
    {"RVK_ViewRange",   "How far you see, x the game's view distance - the far land and mountains fading into the sky by true distance, no popping as you turn (1: the game's own; more draws)", "General", Float, 1, 3, 0.25f, 1.5f, nullptr, 0},

    {"RVK_Darkness",   "Darkness: how dark nights, storms and dim places get (0 lightest: the game's own night light; 1 ao-vk's darker nights; 2 very dark)", "Lighting", Float, 0, 2, 0.05f, 0, nullptr, 0, "RVK_Hdr"},
    {"RVK_DarkFloor",  "Darkness: the floor under the land's brightness, x (the exposure raised in the darkest places, as eyes adapt; 0 none)", "Lighting", Float, 0, 2, 0.1f, 1, nullptr, 0, "RVK_Hdr"},
    {"RVK_PixelLight", "Per-pixel lighting",                                       "Lighting",         Bool,  0, 1, 1, 1, "RANDYVK_PIXEL_LIGHTING", 0},
    {"RVK_LightOver",  "All nearby lights (light override)",   "Lighting",         Bool,  0, 1, 1, 1, "RANDYVK_LIGHT_OVERRIDE", 0},
    {"RVK_Headroom", "Light headroom without effects", "Lighting",         Float, 1, 2, 0.05f, 1.15f, "RANDYVK_LIGHT_HEADROOM", 0, "RVK_LightOver"},
    {"RVK_OwnLight",   "A character's own light lights the character",             "Lighting",         Bool,  0, 1, 1, 1, nullptr, 0, "RVK_LightOver"},
    {"RVK_LeafLightOn","Sunlight through leaves",                                  "Lighting",         Bool,  0, 1, 1, 1, nullptr, 0},
    {"RVK_LeafLight",  "Strength",                                                 "Lighting",         Float, 0.25f, 2, 0.25f, 1, nullptr, 0, "RVK_LeafLightOn"},
    {"RVK_CrownShade", "Tree crowns lit as one rounded mass (darker inside, each tree's colour its own)", "Lighting", Float, 0, 1, 0.1f, 1, nullptr, 0},
    {"RVK_CrownSky",   "Tree crowns lit by the sky from above (brighter tops, darker undersides)", "Lighting", Float, 0, 1, 0.1f, 1, nullptr, 0},
    {"RVK_CrownDepth", "Sunlight through tree crowns fades with their depth (bright thin rims, dark middles)", "Lighting", Float, 0, 1, 0.1f, 1, nullptr, 0, "RVK_LeafLightOn"},
    {"RVK_PtLight", "Lamp and fire brightness", "Lighting",         Float, 0.25f, 2, 0.05f, 0.25f, nullptr, 0},
    {"RVK_CharLight", "Character light brightness", "Lighting",  Float, 0.1f, 2, 0.05f, 0.25f, nullptr, 0},
    {"RVK_TessOn", "Tessellation", "Lighting",         Bool,  0, 1, 1, 1, nullptr, 0},
    {"RVK_Tess",       "Roundness",                                                "Lighting",         Float, 0.25f, 1, 0.05f, 0.6f, nullptr, 0, "RVK_TessOn"},
    {"RVK_TessLevel",  "Detail up close (pieces per triangle edge)",               "Lighting",         Int,   2, 8, 1, 3, nullptr, 0, "RVK_TessOn"},
    {"RVK_TessDist",   "Up to this distance (world units)",                        "Lighting",         Int,   5, 60, 1, 16, nullptr, 0, "RVK_TessOn"},
    {"RVK_Aniso", "Texture filtering (anisotropic; 1 = off)", "Lighting",         Choice, 1, 16, 1, 16, "RANDYVK_ANISOTROPY", 0, nullptr, "1 2 4 8 16"},

    {"RVK_HillShade",   "The terrain's shade from a low sun (valleys under mountains)", "Lighting", Bool, 0, 1, 1, 1, nullptr, 0},
    {"RVK_LightRig",    "One light for day and night (off: the old night lighting)", "Lighting", Bool, 0, 1, 1, 1, nullptr, 0},
    {"RVK_Metering",    "Camera metering: a view brighter than the land (a sunset sky) darkens the picture, as a camera does (0 = off)", "Lighting", Float, 0, 2, 0.1f, 0.5f, nullptr, 0, "RVK_LightRig"},
    {"RVK_BumpOn", "Generated bump maps", "Materials",         Bool,  0, 1, 1, 1, nullptr, 0},
    {"RVK_Bump", "Strength", "Materials",         Float, 0.25f, 4, 0.25f, 0.25f, "RANDYVK_BUMP", 0, "RVK_BumpOn"},
    {"RVK_NormalMaps", "Normal maps", "Materials",         Bool,  0, 1, 1, 1, nullptr, 0},
    {"RVK_NormalStr", "Strength", "Materials",         Float, 0.1f, 4, 0.1f, 1, nullptr, 0, "RVK_NormalMaps"},
    {"RVK_Parallax", "Depth: textures with a height map (_h.png) stand out of the surface up close (0 = flat)", "Materials", Float, 0, 2, 0.1f, 0.3f, nullptr, 0, "RVK_NormalMaps"},
    {"RVK_DepthSteps", "Depth detail: how finely the depth is traced (more: sharper edges at flat views, slower)", "Materials", Float, 0.5f, 3, 0.25f, 1, nullptr, 0, "RVK_NormalMaps"},
    {"RVK_Pbr", "PBR materials", "Materials", Bool, 0, 1, 1, 1, nullptr, 0},
    {"RVK_PbrSpec", "Highlights", "Materials",         Float, 0, 2, 0.1f, 0.5f, nullptr, 0, "RVK_Pbr"},
    {"RVK_PbrEnv", "Reflections", "Materials",         Float, 0, 2, 0.1f, 1, nullptr, 0, "RVK_Pbr"},
    {"RVK_PbrSsr", "Screen-space reflections", "Materials", Float, 0, 2, 0.1f, 1, nullptr, 0, "RVK_Pbr"},
    {"RVK_PbrAo", "Occlusion map strength", "Materials",         Float, 0, 1, 0.1f, 0.1f, nullptr, 0, "RVK_Pbr"},
    {"RVK_PbrProbe", "Environment probe", "Materials", Float, 0, 1, 0.1f, 1, nullptr, 0, "RVK_Pbr"},
    {"RVK_PbrGlow", "Emissive map brightness", "Materials", Float, 0, 8, 0.25f, 1, nullptr, 0, "RVK_Pbr"},
    {"RVK_PbrDebug",   "Debug view (Ctrl+Shift+K)", "Debug", Choice, 0, 11, 1, 0, nullptr, 0, "RVK_Pbr", "0 1 2 3 4 5 6 7 8 9 10 11"},
    {"RVK_Albedo", "Enhanced textures", "Materials",     Bool,  0, 1, 1, 1, nullptr, 0},
    {"RVK_LiveMeshes", "Meshes with a texture of their own and live edits from the ao-assets workbench", "Materials", Bool, 0, 1, 1, 1, nullptr, 0},

    {"RVK_GroundOn", "Ground drawn by ao-vk: every patch of land with its own textures to the horizon, no stutter", "Terrain", Bool, 0, 1, 1, 1, nullptr, 0},
    {"RVK_GroundOne",  "Ground drawn by ao-vk: in one pass - its texture and its light together (off: the game's two passes, the same look, slower)", "Terrain", Bool, 0, 1, 1, 1, nullptr, 0, "RVK_GroundOn"},
    {"RVK_GroundAo",   "Ground drawn by ao-vk: valleys and the feet of cliffs darker, as less of the sky reaches them (0 = none)", "Terrain", Float, 0, 1, 0.05f, 0.6f, nullptr, 0, "RVK_GroundOn"},
    {"RVK_GroundSun",  "Ground drawn by ao-vk: slopes lit from where the sun is now, not where it was when the hour's light was made (0 = as made)", "Terrain", Float, 0, 1, 0.1f, 1, nullptr, 0, "RVK_GroundOn"},
    {"RVK_GroundRocks", "Ground drawn by ao-vk: rocks and scree at the feet of cliffs and on rough slopes (0 = none, 2 = twice as many)", "Terrain", Float, 0, 2, 0.25f, 1, nullptr, 0, "RVK_GroundOn"},
    {"RVK_Pebbles", "Ground drawn by ao-vk: small loose stones on flat dirt, gravel and sand near you (0 = none, 2 = twice as many)", "Terrain", Float, 0, 2, 0.25f, 2, nullptr, 0, "RVK_GroundOn"},
    {"RVK_SandRipple", "Ground drawn by ao-vk: wind ripples in loose sand, in depth up close (0 = none)", "Terrain", Float, 0, 1, 0.1f, 1, nullptr, 0, "RVK_GroundOn"},
    {"RVK_SandPrints", "Ground drawn by ao-vk: footprints in loose sand, where characters walk (0 = none)", "Terrain", Float, 0, 1, 0.1f, 1, nullptr, 0, "RVK_GroundOn"},
    {"RVK_RippleDepth", "Ground drawn by ao-vk: how deep the sand ripples are up close (0 = flat; its own, not Depth's)", "Terrain", Float, 0, 3, 0.1f, 2, nullptr, 0, "RVK_GroundOn"},
    {"RVK_RippleSoft", "Ground drawn by ao-vk: the sand ripples smoother - less wobble in their crests, less of the tile's bumps (0 = rough)", "Terrain", Float, 0, 1, 0.1f, 0.3f, nullptr, 0, "RVK_GroundOn"},
    {"RVK_DuneLight", "Ground drawn by ao-vk: dunes of loose sand shaped by the sun as it stands, their far sides in shade (0 = the game's flat light)", "Terrain", Float, 0, 1, 0.1f, 1, nullptr, 0, "RVK_GroundOn"},
    {"RVK_SandGlint", "Ground drawn by ao-vk: a sparkle of sunlit grains in loose sand near you (0 = none)", "Terrain", Float, 0, 1, 0.1f, 0.5f, nullptr, 0, "RVK_GroundOn"},
    {"RVK_GroundGrain", "Ground drawn by ao-vk: fine grit in the ground at your feet, where its texture is a soft smear (0 = none)", "Terrain", Float, 0, 1, 0.1f, 1, nullptr, 0, "RVK_GroundOn"},
    {"RVK_GroundBlend", "Ground drawn by ao-vk: neighbouring ground textures blend into each other along a ragged edge instead of meeting in a hard straight line (0 = hard edges)", "Terrain", Float, 0, 1, 0.1f, 1.0f, nullptr, 0, "RVK_GroundOn"},
    {"RVK_GroundTess", "Ground drawn by ao-vk: hills and cliff edges rounded, finer triangles up close (0 = the game's shapes)", "Terrain", Float, 0, 1, 0.1f, 1, nullptr, 0, "RVK_GroundOn"},
    {"RVK_CliffDepth", "Ground drawn by ao-vk: cliffs' rock stands out of their faces up close (units; 0 = flat)", "Terrain", Float, 0, 1, 0.05f, 0.4f, nullptr, 0, "RVK_GroundOn"},
    {"RVK_GroundFar",  "Ground past that: texture detail (texels a 4 x 4 cell; 8 = the game's, 32 costs a lot of memory)", "Terrain", Choice, 8, 32, 8, 16, nullptr, 0, nullptr, "8 16 32"},
    {"RVK_GroundRange", "Ground drawn at full quality this many times further out (1 = the game's; above 2 the game stutters as you move)", "Terrain", Float, 1, 2.5f, 0.25f, 1, nullptr, 0},
    {"RVK_CliffTex",   "Cliffs: the ground's texture laid on their faces, not stretched down them", "Terrain", Float, 0, 1, 0.1f, 1, nullptr, 0},
    {"RVK_CliffRock",  "Cliffs: rock laid on their faces, in the colour of the ground", "Terrain",       Bool,  0, 1, 1, 1, nullptr, 0, "RVK_CliffTex"},
    {"RVK_TileHide",   "Cliffs: the same tile not repeated in rows down their faces", "Terrain",       Float, 0, 1, 0.1f, 1, nullptr, 0},
    {"RVK_GroundVary", "Large patches of lighter and darker ground over the land", "Terrain",          Float, 0, 2, 0.1f, 1, nullptr, 0},

    {"RVK_SwayOn",     "Plants sway in the wind",                                  "Plants",           Bool,  0, 1, 1, 1, nullptr, 0},
    {"RVK_Sway",       "Sway strength",                                            "Plants",           Float, 0.25f, 3, 0.25f, 1, nullptr, 0, "RVK_SwayOn"},
    {"RVK_PushOn", "Interactive grass", "Plants",           Bool,  0, 1, 1, 1, nullptr, 0},
    {"RVK_GrassPush", "How far grass leans", "Plants",           Float, 0.25f, 2, 0.25f, 1, nullptr, 0, "RVK_PushOn"},
    {"RVK_PlantDetOn", "Split big plant quads so they bend smoothly",              "Plants",           Bool,  0, 1, 1, 1, nullptr, 0},
    {"RVK_PlantDetail","Detail (pieces per 0.3 units)",                            "Plants",           Float, 0.5f, 2, 0.5f, 1, nullptr, 0, "RVK_PlantDetOn"},
    {"RVK_FolEdges",   "Soft leaf edges", "Plants", Bool, 0, 1, 1, 1, nullptr, 0},
    {"RVK_FolDither",  "Soft leaf edges dithered (one pass, the TAA smooths them) from this distance (0 never)", "Plants", Int, 0, 150, 5, 20, nullptr, 0, "RVK_FolEdges"},
    {"RVK_FolLodOn",   "Cheaper shading of distant foliage",                       "Plants",           Bool,  0, 1, 1, 1, nullptr, 0},
    {"RVK_FoliageLod", "From this distance (world units)",                         "Plants",           Int,   10, 150, 1, 35, nullptr, 0, "RVK_FolLodOn"},
    {"RVK_GrassOn", "Ground grass", "Plants",           Bool,  0, 1, 1, 1, nullptr, 0},
    {"RVK_GrassTex",   "Only on grassy ground",    "Plants",           Bool,  0, 1, 1, 1, nullptr, 0, "RVK_GrassOn"},
    {"RVK_GrassDist", "Distance (m)", "Plants",           Int,   8, 80, 1, 40, nullptr, 0, "RVK_GrassOn"},
    {"RVK_GrassBlades", "Density", "Plants",           Float, 1, 12, 0.5f, 8, nullptr, 0, "RVK_GrassOn"},
    {"RVK_GrassHeight", "Blade height (m)", "Plants",           Float, 0.15f, 2, 0.05f, 0.3f, nullptr, 0, "RVK_GrassOn"},
    {"RVK_GrassWidth", "Blade width (1 = default)",                                 "Plants",           Float, 0.25f, 3, 0.25f, 0.75f, nullptr, 0, "RVK_GrassOn"},
    {"RVK_GrassBright", "Brightness", "Plants",           Float, 0.5f, 1.5f, 0.05f, 1, nullptr, 0, "RVK_GrassOn"},
    {"RVK_GrassVary",  "Grass variety (0 = even)",      "Plants",           Float, 0, 2, 0.1f, 1, nullptr, 0, "RVK_GrassOn"},
    {"RVK_GrassFlower", "Wildflowers", "Plants",           Float, 0, 3, 0.25f, 1, nullptr, 0, "RVK_GrassOn"},
    {"RVK_GrassGlow",  "Sunlight on the blades",      "Plants",           Float, 0, 2, 0.1f, 1, nullptr, 0, "RVK_GrassOn"},
    {"RVK_GrassGusts", "Gusts of wind sweeping over the grass (0 = none)",          "Plants",           Float, 0, 2, 0.1f, 1, nullptr, 0, "RVK_GrassOn"},
    {"RVK_GrassTrail", "Grass trails", "Plants",           Bool,  0, 1, 1, 1, nullptr, 0, "RVK_GrassOn"},
    {"RVK_TrailTime", "Grass trails: how long (seconds)", "Plants",           Float, 2, 30, 1, 12, nullptr, 0, "RVK_GrassOn"},
    {"RVK_TrailShade", "Grass trails: lawn stripes", "Plants", Float, 0, 2, 0.1f, 1, nullptr, 0, "RVK_GrassOn"},
    {"RVK_GrassEven",  "Even grass colour", "Plants",          Float, 0, 1, 0.1f, 1, nullptr, 0, "RVK_GrassOn"},
    {"RVK_LeafOn",     "Leaves on trees and shrubs",  "Plants",     Bool,  0, 1, 1, 1, nullptr, 0},
    {"RVK_LeafDensity","Leaf density", "Plants", Float, 0.25f, 3, 0.25f, 1, nullptr, 0, "RVK_LeafOn"},
    {"RVK_LeafSize",   "Sprig size (1 = an eighth of the leaf texture)",            "Plants",     Float, 0.5f, 2.5f, 0.25f, 1, nullptr, 0, "RVK_LeafOn"},
    {"RVK_LeafDist", "Distance", "Plants",     Int,   15, 150, 5, 50, nullptr, 0, "RVK_LeafOn"},
    {"RVK_LeafCore",   "Canopy cut to an inner core", "Plants", Float, 0, 1, 0.1f, 0.3f, nullptr, 0, "RVK_LeafOn"},
    {"RVK_LeafWind",   "Branches swaying in the wind",                              "Plants",     Float, 0, 3, 0.25f, 0.25f, nullptr, 0, "RVK_LeafOn"},
    {"RVK_LeafFlutter","Leaves fluttering",                                         "Plants",     Float, 0, 3, 0.25f, 1, nullptr, 0, "RVK_LeafOn"},
    {"RVK_LeafOwn",    "Trees: our own canopies where there is leaf art (randy-vk\\materials\\*_leaves.png), not the game's cards", "Plants", Bool, 0, 1, 1, 1, nullptr, 0, "RVK_LeafOn"},
    {"RVK_LeafTrees",  "Leaves on trees",                                           "Plants",     Bool,  0, 1, 1, 1, nullptr, 0, "RVK_LeafOn"},
    {"RVK_LeafShrubs", "Leaves on shrubs and bushes",                               "Plants",     Bool,  0, 1, 1, 1, nullptr, 0, "RVK_LeafOn"},
    {"RVK_LeafPalms",  "Palm fronds bending in the wind",                           "Plants",     Bool,  0, 1, 1, 1, nullptr, 0, "RVK_LeafOn"},
    {"RVK_LeafShadow", "Leaves cast sun shadows",  "Plants",     Bool,  0, 1, 1, 1, nullptr, 0, "RVK_LeafOn"},
    {"RVK_LeafFall", "Falling leaves (0 = none)", "Plants",     Float, 0, 3, 0.25f, 1, nullptr, 0, "RVK_LeafOn"},

    {"RVK_WaterOn", "New water", "Water",    Bool,  0, 1, 1, 1, nullptr, 0},
    {"RVK_WaterStyle", "Look (game-like to realistic)",  "Water",     Float, 0, 1, 0.05f, 0.5f, nullptr, 0, "RVK_WaterOn"},
    {"RVK_WaterQual",  "Quality (1 low, 2 medium, 3 high)",                        "Water",     Int,   1, 3, 1, 2, nullptr, 0, "RVK_WaterOn"},
    {"RVK_WaterMesh",  "Surface from the game's own triangles (off: the older grid)", "Water", Bool, 0, 1, 1, 1, nullptr, 0, "RVK_WaterOn"},
    {"RVK_WaterWaves", "Wave height",                                              "Water",     Float, 0, 3, 0.1f, 0.3f, nullptr, 0, "RVK_WaterOn"},
    {"RVK_WaterRipple","Ripples",                                                  "Water",     Float, 0, 2, 0.1f, 1, nullptr, 0, "RVK_WaterOn"},
    {"RVK_WaterSwim",  "Wake strength (characters in the water)",                  "Water",     Float, 0, 3, 0.1f, 2, nullptr, 0, "RVK_WaterOn"},
    {"RVK_WakeGain","Wake visibility",                                        "Water",     Float, 0, 4, 0.1f, 2, nullptr, 0, "RVK_WaterOn"},
    {"RVK_WakeLife","Wake duration",                                          "Water",     Float, 0.2f, 4, 0.1f, 0.3f, nullptr, 0, "RVK_WaterOn"},
    {"RVK_WakeSpeed","Wake speed (how fast the waves travel, m/s)",           "Water",     Float, 0.5f, 4, 0.1f, 3.6f, nullptr, 0, "RVK_WaterOn"},
    {"RVK_WakeStir","Treading water (a character standing still)",           "Water",     Float, 0, 4, 0.1f, 1, nullptr, 0, "RVK_WaterOn"},
    {"RVK_WakeSplash","Splash (jumping into the water)",                       "Water",     Float, 0, 4, 0.1f, 0, nullptr, 0, "RVK_WaterOn"},
    {"RVK_WaterRefl",  "Reflections",                                              "Water",     Float, 0, 1.5f, 0.05f, 1, nullptr, 0, "RVK_WaterOn"},
    {"RVK_WaterRefr",  "Refraction (bending what is seen through it)",            "Water",     Float, 0, 2, 0.1f, 0.3f, nullptr, 0, "RVK_WaterOn"},
    {"RVK_WaterClear", "Clarity (how far one sees into it)",                       "Water",     Float, 0.2f, 4, 0.1f, 1, nullptr, 0, "RVK_WaterOn"},
    {"RVK_WaterFoam",  "Foam (shores, crests)",                                    "Water",     Float, 0, 2, 0.1f, 0, nullptr, 0, "RVK_WaterOn"},
    {"RVK_WaterCaust", "Caustics (sunlight patterns under water)",                 "Water",     Float, 0, 2, 0.1f, 1, nullptr, 0, "RVK_WaterOn"},
    {"RVK_WaterTex",   "The game's water texture on the surface",                  "Water",     Float, 0, 1, 0.05f, 0.5f, nullptr, 0, "RVK_WaterOn"},
    {"RVK_WaterSheen", "Waterfalls: sheen (how much the clear water reflects)",    "Water",     Float, 0, 2, 0.1f, 0.2f, nullptr, 0, "RVK_WaterOn"},
    {"RVK_WaterSpark", "Waterfalls: sparkles (sunlight and lamplight)",            "Water",     Float, 0, 3, 0.1f, 0.1f, nullptr, 0, "RVK_WaterOn"},
    {"RVK_WaterSpray", "Waterfalls: spray and mist at the foot",                   "Water",     Float, 0, 3, 0.1f, 1, nullptr, 0, "RVK_WaterOn"},
    {"RVK_WaterChurn", "Waterfalls: churn and foam where they land",               "Water",     Float, 0, 3, 0.1f, 1, nullptr, 0, "RVK_WaterOn"},
    {"RVK_WaterSpeed", "Waterfalls: how fast the water falls",                    "Water",     Float, 0.25f, 4, 0.05f, 2, nullptr, 0, "RVK_WaterOn"},

    {"RVK_WeatherOn", "New weather", "Weather",    Bool,  0, 1, 1, 1, nullptr, 0},
    {"RVK_RainForce", "Force rain (0 = the game's weather)", "Weather",     Float, 0, 1, 0.05f, 0, nullptr, 0, "RVK_WeatherOn"},
    {"RVK_RainSpeed",  "Rain: how fast the drops fall (units a second)", "Weather", Float, 3, 40, 0.5f, 30, nullptr, 0, "RVK_WeatherOn"},
    {"RVK_RainLength", "Rain: how long each falling drop's streak is (units)", "Weather", Float, 0.05f, 2, 0.05f, 0.2f, nullptr, 0, "RVK_WeatherOn"},
    {"RVK_RainLean",   "Rain: how far the wind slants it at most, sideways per unit of fall (0.3 ~17 degrees; 0 straight down)", "Weather", Float, 0, 1, 0.05f, 0.3f, nullptr, 0, "RVK_WeatherOn"},
    {"RVK_RainAmount", "Rain amount", "Weather",     Float, 0, 4, 0.1f, 3, nullptr, 0, "RVK_WeatherOn"},
    {"RVK_RainSplash", "Rain: splashes where it lands (0 = none)",                "Weather",     Float, 0, 3, 0.1f, 1, nullptr, 0, "RVK_WeatherOn"},
    {"RVK_RainBright", "Rain: brightness",                                         "Weather",     Float, 0, 3, 0.05f, 1, nullptr, 0, "RVK_WeatherOn"},
    {"RVK_WindStorm",  "Storm wind: how hard storms blow - grass, plants, trees and leaves (0: as on a calm day)", "Weather", Float, 0, 3, 0.1f, 2, nullptr, 0, "RVK_WeatherOn"},
    {"RVK_StormTint",  "The game's storm tint: its flat sheet over the screen in sand and ash storms (off: left out)", "Weather", Bool, 0, 1, 1, 1, nullptr, 0, "RVK_WeatherOn"},
    {"RVK_StormForce", "Force a thunderstorm (0 = the game's weather)", "Weather", Float, 0, 1, 0.05f, 0, nullptr, 0, "RVK_WeatherOn"},
    {"RVK_StrikeRate", "Thunderstorm, forced: lightning strikes a minute at full strength", "Weather", Float, 0, 60, 1, 20, nullptr, 0, "RVK_WeatherOn"},
    {"RVK_StormDebris", "Storms: blown debris - leaves, straw and twigs skimming over the ground, leaves torn off the trees (x; 0: none)", "Weather", Float, 0, 3, 0.1f, 1, nullptr, 0, "RVK_WeatherOn"},
    {"RVK_StormBolt",  "Lightning: our own bolts - branching, flickering with each stroke, glowing - in place of the game's (x; 0: the game's)", "Weather", Float, 0, 3, 0.1f, 1, nullptr, 0, "RVK_WeatherOn"},
    {"RVK_RainWet",    "Rain: the ground soaked - darker and reflecting - and puddles gathering in hollows; dry under roofs and trees (x; 0: none)", "Weather", Float, 0, 1.5f, 0.05f, 1, nullptr, 0, "RVK_WeatherOn"},
    {"RVK_StormDark",  "Thunderstorms: the land darker as the storm hides the sun - its shadows gone (x; 0: lit as the game lights it)", "Weather", Float, 0, 1.5f, 0.05f, 1, nullptr, 0, "RVK_WeatherOn"},
    {"RVK_StormGreen", "Thunderstorms: their clouds and mist cast green, as the game's storm air (0: grey)", "Weather", Float, 0, 1, 0.05f, 1, nullptr, 0, "RVK_WeatherOn"},
    {"RVK_StormThick", "Thunderstorms: how thick and dark their clouds (1: shapes and light in them; 2: one dark mass)", "Weather", Float, 0.25f, 2, 0.05f, 1, nullptr, 0, "RVK_WeatherOn"},
    {"RVK_RainVeils",  "Thunderstorms: grey rain veils under the far clouds, x (0: none)", "Weather", Float, 0, 3, 0.1f, 1, nullptr, 0, "RVK_WeatherOn"},
    {"RVK_RainFog",    "Thunderstorms: the rain's mist - the distance grey, rain shafts hanging under the storm, lit by lightning (x; 0: none; with RVK_SandFog)", "Weather", Float, 0, 3, 0.1f, 1, nullptr, 0, "RVK_WeatherOn"},
    {"RVK_Lightning",  "Lightning: each of the game's strikes lights the land and the clouds round it with a flash (x; 0: none)", "Weather", Float, 0, 3, 0.1f, 1, nullptr, 0, "RVK_WeatherOn"},
    {"RVK_HeatFlash", "Heat lightning: on some warm, dry nights silent flashes in far-off clouds (x; 0: none)", "Weather", Float, 0, 2, 0.1f, 1, nullptr, 0, "RVK_WeatherOn"},
    {"RVK_StormFlash", "The game's lightning glow: the sky flashing over the clouds in storms - it flickers (off: left out; the bolts stay)", "Weather", Bool, 0, 1, 1, 1, nullptr, 0, "RVK_WeatherOn"},
    {"RVK_SandHaze",   "Sand and ash storms: how thick the storm's air - the fog (or the flat haze), in place of the game's tint, x (0: none)", "Weather", Float, 0, 3, 0.05f, 0.4f, nullptr, 0, "RVK_WeatherOn"},
    {"RVK_SandForce", "Force a sandstorm (1 = full storm; 0 = the game's weather)", "Weather", Float, 0, 2, 0.05f, 0, nullptr, 0, "RVK_WeatherOn"},
    {"RVK_SandAsh",    "Sand and ash storms: the forced one is an ash storm (grey) instead of sand", "Weather", Bool, 0, 1, 1, 0, nullptr, 0, "RVK_WeatherOn"},
    {"RVK_SandRes",    "Sand and ash storms: the blowing sand's cells a side (its 32-unit box; finer costs more; 0: by RVK_SmokeQual - 48, 64, 80)", "Weather", Choice, 0, 128, 1, 0, nullptr, 0, "RVK_WeatherOn", "0 48 64 80 96 112 128"},
    {"RVK_SandProc",   "Sand and ash storms: the blowing sand made procedurally (cheaper; off: simulated - the default)", "Weather", Bool, 0, 1, 1, 0, nullptr, 0, "RVK_WeatherOn"},
    {"RVK_SandContrast", "Sand and ash storms: the blowing sand's contrast (crisper streaks, deeper shade; 1 = soft)", "Weather", Float, 0.5f, 3, 0.1f, 1, nullptr, 0, "RVK_WeatherOn"},
    {"RVK_SandClear",  "Sand and ash storms: the air clear round the camera, units (so you always see out; 0 = none)", "Weather", Float, 0, 20, 1, 6, nullptr, 0, "RVK_WeatherOn"},
    {"RVK_SandGusts",  "Sand and ash storms: distant gusts - billows of dust drifting through the fog beyond the near sand (0 = none)", "Weather", Float, 0, 2, 0.1f, 2, nullptr, 0, "RVK_WeatherOn"},
    {"RVK_SandFog",    "Sand and ash storms: the storm's air as a lit fog - shaded by dunes and walls, glowing towards the sun (off: a flat haze, cheaper)", "Weather", Bool, 0, 1, 1, 1, nullptr, 0, "RVK_WeatherOn"},
    {"RVK_SandSun",    "Sand and ash storms, with the sand fog: how clearly the sun shows through the storm, dim and red (0: hidden as the dust)", "Weather", Float, 0, 1, 0.05f, 0.9f, nullptr, 0, "RVK_WeatherOn"},
    {"RVK_SandTint",   "Sand and ash storms, with the sand fog: the light under the storm the dust's colour - the sun behind it lights the land orange (0: the sun's own)", "Weather", Float, 0, 1, 0.05f, 0.7f, nullptr, 0, "RVK_WeatherOn"},
    {"RVK_SandBlow",   "Sand and ash storms: sand blowing round you, simulated - streaks, eddies, kicked up by feet (x; 0: none)", "Weather", Float, 0, 3, 0.1f, 2, nullptr, 0, "RVK_WeatherOn"},

    {"RVK_SmokeOn", "Simulated smoke", "Smoke",    Bool,  0, 1, 1, 1, nullptr, 0},
    {"RVK_SmokeQual", "Quality (1 low, 2 medium, 3 high)", "Smoke",     Int,   1, 3, 1, 2, nullptr, 0, "RVK_SmokeOn"},
    {"RVK_SmokeRes",   "Smoke: cells per world unit (detail)",                    "Smoke",     Float, 4, 24, 1, 12, nullptr, 0, "RVK_SmokeOn"},
    {"RVK_SmokeThick", "Thickness", "Smoke",     Float, 0.1f, 8, 0.1f, 1, nullptr, 0, "RVK_SmokeOn"},
    {"RVK_SmokeLife",  "Smoke: how long it lasts (x the game's)",                 "Smoke",     Float, 0.25f, 4, 0.25f, 2, nullptr, 0, "RVK_SmokeOn"},
    {"RVK_SmokeStir",  "Smoke: how much characters stir it",                      "Smoke",     Float, 0, 2, 0.25f, 1, nullptr, 0, "RVK_SmokeOn"},
    {"RVK_SmokeWisps", "Smoke: fine wisps carried by the flow",                   "Smoke",     Float, 0, 1, 0.25f, 1, nullptr, 0, "RVK_SmokeOn"},
    {"RVK_SmokeBright","Smoke: how light it is",                                   "Smoke",     Float, 0.25f, 3, 0.25f, 0.25f, nullptr, 0, "RVK_SmokeOn"},
    {"RVK_SmokeOpaque", "Smoke: how opaque it is drawn",                           "Smoke",     Float, 0.05f, 8, 0.05f, 3, nullptr, 0, "RVK_SmokeOn"},
    {"RVK_SmokeBudget","Smoke: the simulation's GPU time a frame, about (ms; over it the smoke steps less often; 0: no cap)", "Smoke", Float, 0, 8, 0.25f, 1.5f, nullptr, 0, "RVK_SmokeOn"},
    {"RVK_SmokeHalf",  "Smoke: drawn at half resolution (much cheaper)",          "Smoke",     Bool,  0, 1, 1, 1, nullptr, 0, "RVK_SmokeOn"},
    {"RVK_CloudsOn", "Volumetric clouds", "Sky", Bool,  0, 1, 1, 1, nullptr, 0},
    {"RVK_CloudThick", "Clouds: how thick",                                        "Sky",     Float, 0.1f, 4, 0.1f, 1, nullptr, 0, "RVK_CloudsOn"},
    {"RVK_CloudBright", "Clouds: how light",                                       "Sky",     Float, 0.25f, 3, 0.05f, 0.5f, nullptr, 0, "RVK_CloudsOn"},
    {"RVK_CloudNight",  "Clouds: how light at night (x the above)",                 "Sky",     Float, 0.25f, 2, 0.05f, 0.4f, nullptr, 0, "RVK_CloudsOn"},
    {"RVK_ShadowMem",   "Shadows of things out of view: what stood in view and stands still keeps casting", "Shadows", Bool, 0, 1, 1, 1, nullptr, 0, "RVK_SunShadow"},
    {"RVK_RtShadow",    "Sun shadows ray traced: soft, exact shadows of the static world (characters and plants: the shadow map as before; GPUs with ray tracing only)", "Shadows", Bool, 0, 1, 1, 0, nullptr, 0, "RVK_SunShadow"},
    {"RVK_StaticShad",  "Shadows of the playfield's placed objects from its own list - rocks, walls, trees, seen or not (ao-assets statels): 0 off, 1 once their placement is confirmed, 2 always", "Shadows", Int, 0, 2, 1, 1, nullptr, 0, "RVK_SunShadow"},
    {"RVK_ShadowView",  "Debug: show the sun shadow map's light (white lit, dark shadowed; tint = cascade)", "Debug", Bool, 0, 1, 1, 0, nullptr, 0},
    {"RVK_SceneProf",   "Debug: the scene's GPU time and overdraw by kind of draw in the gpu ms log (costs a little; console sceneprof 1)", "Debug", Bool, 0, 1, 1, 0, "RANDYVK_SCENE_PROFILE", 0},
    {"RVK_Occlusion",   "The game's culling of what hills hide (off: all in view drawn - whole shadows, more draws)", "Debug", Bool, 0, 1, 1, 1, nullptr, 0},
    {"RVK_TimeOfDay",   "Testing: the game's time of day set to this hour (-1: the game's own; a day is 27 h)", "Debug", Float, -1, 27, 0.25f, -1, nullptr, 0},
    {"RVK_TimeSpeed",   "Testing: how fast the time of day runs, x the game's (0 still; 1 and the hour -1: the game's own)", "Debug", Float, 0, 500, 0.5f, 1, nullptr, 0},
    {"RVK_TexKeepCpu",  "Keep a CPU copy of every texture (off: given up once on the graphics card, read back if the game locks it again - saves the game's 32-bit address space)", "Debug", Bool, 0, 1, 1, 0, nullptr, 0},
    {"RVK_HeapCheck",   "Testing: the heap checked for corruption at points, the first broken one logged (0 off, 1 round the grass and each frame, 2 also round every object's grass cover - slow)", "Debug", Float, 0, 2, 1, 0, nullptr, 0},
    {"RVK_SkyVary",     "Clouds: the sky changes, clear to overcast (0: always some cloud)", "Sky", Float, 0, 1, 0.1f, 1, nullptr, 0, "RVK_CloudsOn"},
    {"RVK_ProcSky",     "Sky: our own sky in place of the game's - blue by day, sunsets, the blue hour and night from the sun's height (off: the game's sky)", "Sky", Bool, 0, 1, 1, 1, nullptr, 0, "RVK_Hdr"},
    {"RVK_SkyZone",     "Sky: how much of each zone's own sky colour is kept (0: none, 1: mostly the zone's)", "Sky", Float, 0, 1, 0.05f, 0.25f, nullptr, 0, "RVK_ProcSky"},
    {"RVK_SkyBright",   "Sky: our sky's brightness against the game's (and the far haze's, which takes its colour)", "Sky", Float, 0.25f, 2, 0.05f, 1, nullptr, 0, "RVK_ProcSky"},
#define RVK_SKY_KEY(name, label, key) \
    {name, label, "Sky", Colour, 0, 16777215, 1, float(rvk::kSkyKeyRef[rvk::key]), nullptr, 0, "RVK_ProcSky"},
    RVK_SKY_KEY("RVK_SkyDayTop", "Sky colour: by day, overhead", kSkyDayTop)
    RVK_SKY_KEY("RVK_SkyDayLow", "Sky colour: by day, low", kSkyDayLow)
    RVK_SKY_KEY("RVK_SkyDuskSun", "Sky colour: dawn and dusk, low towards the sun", kSkyDuskSun)
    RVK_SKY_KEY("RVK_SkyDuskAway", "Sky colour: dawn and dusk, low away from the sun", kSkyDuskAway)
    RVK_SKY_KEY("RVK_SkyDuskTop", "Sky colour: dawn and dusk, overhead", kSkyDuskTop)
    RVK_SKY_KEY("RVK_SkyBlueTop", "Sky colour: the blue hour, overhead", kSkyBlueTop)
    RVK_SKY_KEY("RVK_SkyBlueLow", "Sky colour: the blue hour, low", kSkyBlueLow)
    RVK_SKY_KEY("RVK_SkyNightTop", "Sky colour: at night, overhead", kSkyNightTop)
    RVK_SKY_KEY("RVK_SkyNightLow", "Sky colour: at night, low", kSkyNightLow)
#undef RVK_SKY_KEY
    {"RVK_Twilight",    "Sky: sunrise and sunset colours - orange and red towards the sun, pink above, the pink band opposite (0: the game's sky; above 1 more vivid)", "Sky", Float, 0, 1.5f, 0.05f, 0.5f, nullptr, 0, "RVK_Hdr"},
    {"RVK_BlueHour",    "Sky: the blue hour after sunset and before sunrise - a deep blue sky going over to the night's (0: none)", "Sky", Float, 0, 1, 0.05f, 1, nullptr, 0, "RVK_Hdr"},
    {"RVK_SkyGlow",     "Sky: the glow round the sun and along the horizon under it (0: none)", "Sky", Float, 0, 2, 0.1f, 0.25f, nullptr, 0, "RVK_Hdr"},
    {"RVK_SkyZones",    "Clouds: each zone its own skies - deserts clearer, coasts and forests cloudier (0: the same everywhere)", "Sky", Float, 0, 1, 0.1f, 1, nullptr, 0, "RVK_CloudsOn"},
    {"RVK_Afterglow",   "Clouds: how strongly they take the sunset's colours - gold, orange, red, pink undersides after the sun has set (0: the game's hues)", "Sky", Float, 0, 2, 0.1f, 1, nullptr, 0, "RVK_CloudsOn"},
    {"RVK_StarFade",    "Sky: the stars come out slowly as the sky darkens after sunset, and fade before sunrise (off: as the game shows them)", "Sky", Bool, 0, 1, 1, 1, nullptr, 0},
    {"RVK_Stars",       "Sky: our own stars - thousands, in their colours, wheeling through the night, twinkling low (0: the game's)", "Sky", Choice, 0, 1, 1, 1, nullptr, 0, "RVK_ProcSky", "0 1"},
    {"RVK_StarBright",  "Sky: the stars' brightness, x", "Sky", Float, 0.25f, 3, 0.05f, 3, nullptr, 0, "RVK_ProcSky"},
    {"RVK_Twinkle",     "Sky: how much the stars twinkle (most low in the sky; 0 steady)", "Sky", Float, 0, 1, 0.05f, 0.2f, nullptr, 0, "RVK_ProcSky"},
    {"RVK_Galaxy",      "Sky: the galaxy's band across the night sky, x (0 none)", "Sky", Float, 0, 2, 0.1f, 1, nullptr, 0, "RVK_ProcSky"},
    {"RVK_Meteors",     "Sky: shooting stars on clear nights, how often x (0 none)", "Sky", Float, 0, 3, 0.25f, 1, nullptr, 0, "RVK_ProcSky"},
    {"RVK_Aurora",      "Sky: auroras on some clear nights, how often and how strong x (0 none)", "Sky", Float, 0, 2, 0.1f, 1, nullptr, 0, "RVK_ProcSky"},
    {"RVK_AuroraForce", "Sky: an aurora every night, this strong (0: only some nights, by chance)", "Sky", Float, 0, 2, 0.1f, 0, nullptr, 0, "RVK_ProcSky"},
    {"RVK_AuroraCol",   "Sky colour: the aurora's glow", "Sky", Colour, 0, 16777215, 1, float(0x40FF90), nullptr, 0, "RVK_ProcSky"},
    {"RVK_Rainbow",     "Sky: rainbows after rain, with the sun low behind you, x (0 none)", "Sky", Float, 0, 2, 0.1f, 1, nullptr, 0, "RVK_Hdr"},
    {"RVK_Cirrus",      "Clouds: thin high wisps, most on clear days - pink and gold round sunrise and sunset (0: none; off by default: under our own sky they showed as coarse streaks)", "Sky", Float, 0, 2, 0.1f, 0, nullptr, 0, "RVK_CloudsOn"},
    {"RVK_CloudDetail", "Clouds: billows and wisps (0 smooth)",                    "Sky",     Float, 0, 2, 0.1f, 2, nullptr, 0, "RVK_CloudsOn"},
    {"RVK_CloudHalf",   "Clouds: drawn at half resolution (much cheaper)",         "Sky",     Bool,  0, 1, 1, 1, nullptr, 0, "RVK_CloudsOn"},
    {"RVK_CloudShadow", "Shadows on the ground (0 = none)", "Sky",     Float, 0, 1, 0.05f, 0, nullptr, 0, "RVK_CloudsOn"},
    {"RVK_CloudSteps", "Quality (steps through a cloud)", "Sky",     Int,   8, 96, 4, 32, nullptr, 0, "RVK_CloudsOn"},
    {"RVK_CloudLayer",  "Clouds: a cloud layer over the sky (off: each game cloud a volume)", "Sky", Bool, 0, 1, 1, 1, nullptr, 0, "RVK_CloudsOn"},
    {"RVK_CloudCover", "Coverage (- less, + more)", "Sky",     Float, -1, 1, 0.05f, 0, nullptr, 0, "RVK_CloudsOn"},
    {"RVK_CloudHeight", "Clouds: the layer's height",                              "Sky",     Float, 200, 3000, 50, 900, nullptr, 0, "RVK_CloudsOn"},
    {"RVK_CloudShade",  "Clouds: contrast (light and shade; 1 as lit)",            "Sky",     Float, 0.5f, 3, 0.1f, 1, nullptr, 0, "RVK_CloudsOn"},
    {"RVK_CloudLow",    "Clouds: low grey deck on overcast days, x (0 none)",      "Sky",     Float, 0, 2, 0.1f, 0, nullptr, 0, "RVK_CloudsOn"},
    {"RVK_CloudQual",   "Clouds: quality - less noise, more cost (0 low .. 3 ultra)", "Sky",   Choice, 0, 3, 1, 3, nullptr, 0, "RVK_CloudsOn", "0 1 2 3"},
    {"RVK_CloudMorph",  "Clouds: how fast they churn and change shape, x (0 still)", "Sky",   Float, 0, 3, 0.1f, 1, nullptr, 0, "RVK_CloudsOn"},
    {"RVK_CloudSheet",  "Clouds: the game's flat sheets behind them, x (0 none)",  "Sky",     Float, 0, 2, 0.1f, 1, nullptr, 0, "RVK_CloudsOn"},
    {"RVK_SmokeGame",  "Smoke: the game's smoke sprites too",                     "Smoke",     Bool,  0, 1, 1, 0, nullptr, 0, "RVK_SmokeOn"},

    {"RVK_SunShadow",  "Sun shadows",                                              "Shadows",          Bool,  0, 1, 1, 1, "RANDYVK_SHADOWS", 0},
    {"RVK_SunRes", "Resolution", "Shadows",          Choice, 1024, 8192, 1, 2048, nullptr, 0, "RVK_SunShadow", "1024 2048 4096 8192"},
    {"RVK_SunStrength", "Darkness", "Shadows",          Float, 0, 1, 0.05f, 0.75f, "RANDYVK_SHADOW_STRENGTH", 0, "RVK_SunShadow"},
    {"RVK_SunDist", "Distance (m)", "Shadows",          Int,   40, 1000, 1, 400, nullptr, 0, "RVK_SunShadow"},
    {"RVK_SunCascade", "Cascades (more = sharper near, same reach)",               "Shadows",          Int,   1, 4, 1, 4, nullptr, 0, "RVK_SunShadow"},
    {"RVK_SunSoft", "Softness (0 = hard)", "Shadows",          Float, 0, 4, 0.25f, 1, nullptr, 0, "RVK_SunShadow"},
    {"RVK_GrassShadow", "Grass shadows", "Shadows",           Bool,  0, 1, 1, 1, nullptr, 0, "RVK_SunShadow"},
    {"RVK_ContactOn",  "Contact shadows (small sun shadows the map misses)",       "Shadows",          Bool,  0, 1, 1, 1, nullptr, 0},
    {"RVK_Contact",    "Strength",                                                 "Shadows",          Float, 0.05f, 1, 0.05f, 0.65f, nullptr, 0, "RVK_ContactOn"},
    {"RVK_PtOn",       "Point light shadows",                                      "Shadows",          Bool,  0, 1, 1, 1, nullptr, 0},
    {"RVK_PtShadows",  "Lights with shadows",                                      "Shadows",          Int,   1, 16, 1, 8, "RANDYVK_POINT_SHADOWS", 0, "RVK_PtOn"},
    {"RVK_PtRes",      "Resolution (per face)",                 "Shadows",          Choice, 256, 2048, 1, 512, nullptr, 0, "RVK_PtOn", "256 512 1024 2048"},
    {"RVK_PtStrength", "Darkness", "Shadows",          Float, 0, 1, 0.05f, 1, "RANDYVK_POINT_SHADOW_STRENGTH", 0, "RVK_PtOn"},
    {"RVK_PtDay",      "Strength in daylight (fraction)",                          "Shadows",          Float, 0, 1, 0.05f, 1, "RANDYVK_POINT_SHADOW_DAY", 0, "RVK_PtOn"},
    {"RVK_OwnShadow",  "A character's own light (a lamp it wears) casts the character's shadow", "Shadows", Bool, 0, 1, 1, 1, nullptr, 0, "RVK_PtOn"},

    {"RVK_Hdr", "Off-screen scene for every effect (0: none - the game's own drawing)", "Effects",  Bool,  0, 1, 1, 1, "RANDYVK_HDR", 0},
    {"RVK_Knee", "Tone mapping knee (1 = only clip)", "Tone mapping",  Float, 0.5f, 1, 0.05f, 0.85f, "RANDYVK_TONEMAP_KNEE", 0, "RVK_Hdr"},
    {"RVK_HdrRoom", "Local light headroom", "Tone mapping",  Float, 1, 4, 0.25f, 1.5f, "RANDYVK_HDR_HEADROOM", 0, "RVK_Hdr"},
    {"RVK_BloomOn", "Bloom", "Effects",  Bool,  0, 1, 1, 1, nullptr, 0},
    {"RVK_Bloom", "Strength", "Effects",  Float, 0.25f, 5, 0.25f, 1, "RANDYVK_BLOOM", 0, "RVK_BloomOn"},
    {"RVK_BloomThr", "Threshold (1 = above white)", "Effects",  Float, 0.5f, 2, 0.05f, 0.5f, "RANDYVK_BLOOM_THRESHOLD", 0, "RVK_BloomOn"},
    {"RVK_BloomOcc", "Over objects in front of its light (1 = unchanged)", "Effects",  Float, 0, 1, 0.05f, 0.15f, nullptr, 0, "RVK_BloomOn"},
    {"RVK_BloomWide", "Wide haze round its core (0 = even; wider in rain by night)", "Effects",  Float, 0, 1, 0.05f, 0.4f, nullptr, 0, "RVK_BloomOn"},
    {"RVK_NeonSat", "Bright coloured lights keep their colour (0 = they go white)", "Effects",  Float, 0, 1, 0.05f, 0.6f, nullptr, 0, "RVK_Hdr"},
    {"RVK_FxGlowOn", "Glow of effects (spells, fire, light halos)", "Effects",  Bool,  0, 1, 1, 1, nullptr, 0},
    {"RVK_BloomFx", "Strength", "Effects",  Float, 0.25f, 4, 0.25f, 1, "RANDYVK_BLOOM_EFFECTS", 0, "RVK_FxGlowOn"},
    {"RVK_NightOn", "Night glow of windows, signs and screens", "Effects",  Bool,  0, 1, 1, 1, nullptr, 0},
    {"RVK_NightGlow", "Strength", "Effects",  Float, 0.25f, 4, 0.25f, 1, nullptr, 0, "RVK_NightOn"},
    {"RVK_MoonOn", "Moonlight at night (light and shadows from the moon)", "Effects",  Bool,  0, 1, 1, 1, nullptr, 0},
    {"RVK_MoonLight", "Strength", "Effects",  Float, 0.25f, 3, 0.25f, 1, nullptr, 0, "RVK_MoonOn"},
    {"RVK_AoOn", "Ambient occlusion", "Effects",  Bool,  0, 1, 1, 1, nullptr, 0},
    {"RVK_Ao", "Strength", "Effects",  Float, 0.25f, 3, 0.25f, 1, "RANDYVK_AO", 0, "RVK_AoOn"},
    {"RVK_AoRadius", "Radius (world units)", "Effects",  Float, 0.25f, 4, 0.25f, 1, "RANDYVK_AO_RADIUS", 0, "RVK_AoOn"},
    {"RVK_AoMode", "Ambient occlusion: whose (0 = ours, 1 = AMD's FidelityFX CACAO, 2 = ray traced: GPUs with ray tracing only)", "Effects", Choice, 0, 2, 1, 0, nullptr, 0, "RVK_AoOn", "0 1 2"},
    {"RVK_GiOn", "Indirect light (bounce light from the lit scene)", "Effects",  Bool,  0, 1, 1, 1, nullptr, 0},
    {"RVK_Gi", "Strength", "Effects",  Float, 0.25f, 3, 0.25f, 1, nullptr, 0, "RVK_GiOn"},
    {"RVK_GiRadius", "Reach (world units)", "Effects",  Float, 1, 12, 0.5f, 4, nullptr, 0, "RVK_GiOn"},
    {"RVK_GiMode", "Indirect light: whose (0 = ours, 1 = AMD's FidelityFX Brixelizer GI)", "Effects", Choice, 0, 1, 1, 0, nullptr, 0, "RVK_GiOn", "0 1"},
    {"RVK_VolOn", "Volumetric light (sun shafts)", "Effects",  Bool,  0, 1, 1, 1, nullptr, 0},
    {"RVK_Volume", "Strength", "Effects",  Float, 0.25f, 4, 0.25f, 0.5f, nullptr, 0, "RVK_VolOn"},
    {"RVK_VolHaze", "Haze", "Effects",  Float, 0.25f, 4, 0.25f, 0.5f, nullptr, 0, "RVK_VolOn"},
    {"RVK_VolShafts", "Shaft contrast (0 = physical)", "Effects",  Float, 0, 3, 0.25f, 1, nullptr, 0, "RVK_VolOn"},
    {"RVK_VolRays", "Light rays through gaps in the clouds", "Effects",  Float, 0, 3, 0.25f, 1, nullptr, 0, "RVK_VolOn"},
 {"RVK_VolLamps", "Lamps' light in the air: a glow round each lamp, thicker in fog and mist (0: none)", "Effects", Float, 0, 2, 0.1f, 0, nullptr, 0, "RVK_VolOn"},
    {"RVK_DuskRays", "Sunset rays: beams fanning from a low sun through the clouds and over the hills (0: none)", "Effects", Float, 0, 2, 0.1f, 1, nullptr, 0, "RVK_VolOn"},
    {"RVK_VolLand", "Haze over distant land (cool, as the air makes it)", "Effects",  Float, 0, 1, 0.05f, 0.1f, nullptr, 0},
    {"RVK_DesertAir", "Deserts: dusty air in fair weather - far dunes paler and warmer, a pale band along the horizon (0 = none)", "Effects", Float, 0, 1, 0.1f, 1, nullptr, 0},
    {"RVK_HeatShimmer", "Deserts: heat shimmer over distant land in the midday sun (0 = none)", "Effects", Float, 0, 1, 0.1f, 0.5f, nullptr, 0},
    {"RVK_Sun", "Sun brightness (0 = the game's own sun)", "Effects", Float, 0, 2, 0.1f, 1, nullptr, 0, "RVK_Hdr"},
    {"RVK_SunSize", "The suns' size (1: the game's own; less: tamer)", "Effects", Float, 0.3f, 1.5f, 0.05f, 0.75f, nullptr, 0, "RVK_Sun"},
    {"RVK_SunBleed", "How much of the suns' glow shows through thick cloud (0 none)", "Effects", Float, 0, 1, 0.05f, 0.3f, nullptr, 0, "RVK_Sun"},
    {"RVK_Atmos", "Atmospheric haze", "Effects", Bool, 0, 1, 1, 1, nullptr, 0},
    {"RVK_Mist", "Valley mist (night and dawn)", "Effects", Float, 0, 2, 0.1f, 1, nullptr, 0, "RVK_Atmos"},
    {"RVK_FogBanks", "Fog banks: thick mist drifting through the low ground at dawn and after rain, most in wet country (x; 0: none)", "Effects", Float, 0, 2, 0.1f, 1, nullptr, 0, "RVK_Atmos"},
    {"RVK_DawnLight", "Low sun's light: golden on what faces it, the shade cooler, round sunrise and sunset (0: the game's colours)", "Effects", Float, 0, 2, 0.1f, 1, nullptr, 0, "RVK_Hdr"},
    {"RVK_MornHaze", "Morning haze: the air thicker round sunrise, half as much round sunset (0: none)", "Effects", Float, 0, 2, 0.1f, 1, nullptr, 0, "RVK_Atmos"},
    {"RVK_GameSun", "The game's own sun glow sprites (off: only the suns above)", "Effects", Bool, 0, 1, 1, 0, nullptr, 0},
    {"RVK_SsrOn", "Reflections (screen-space)", "Effects",  Bool,  0, 1, 1, 0, nullptr, 0},
    {"RVK_Ssr", "Strength", "Effects",  Float, 0.25f, 2, 0.25f, 0.25f, nullptr, 0, "RVK_SsrOn"},
    {"RVK_SsrWater", "Water", "Effects",  Float, 0, 1, 0.05f, 0.25f, nullptr, 0, "RVK_SsrOn"},
    {"RVK_SsrGloss", "Glossy surfaces", "Effects",  Float, 0, 1, 0.05f, 0.3f, nullptr, 0, "RVK_SsrOn"},
    {"RVK_SsrWet", "Wet look of floors and ground (0 = dry)", "Effects",  Float, 0, 1, 0.05f, 0, nullptr, 0, "RVK_SsrOn"},
    {"RVK_RtReflect", "Reflections ray traced where the screen has nothing to reflect - what is behind a wall, off the screen's edge, behind the camera (ours only; GPUs with ray tracing only)", "Effects", Bool, 0, 1, 1, 0, nullptr, 0, "RVK_SsrOn"},
    {"RVK_SsrMode", "Reflections: whose (0 = ours, 1 = AMD's FidelityFX SSSR: rough surfaces too, denoised)", "Effects", Choice, 0, 1, 1, 0, nullptr, 0, "RVK_SsrOn", "0 1"},
    {"RVK_SssrThick", "Reflections, AMD's: how far behind a surface a ray's hit still counts (world units; more: fewer gaps, more smearing)", "Effects", Float, 0.05f, 3, 0.05f, 0.5f, nullptr, 0, "RVK_SsrOn"},
    {"RVK_Vrs", "Variable rate shading: flat or fast-moving parts of the scene shaded in 2 x 2 blocks, for speed (AMD's FidelityFX VRS; needs a GPU with shading rate images)", "Effects", Choice, 0, 1, 1, 0, nullptr, 0, nullptr, "0 1"},
    {"RVK_GiDebug", "Indirect light, AMD's: a view (0 = none, 1 = its distance field of the world, 2 = its light alone)", "Effects", Choice, 0, 2, 1, 0, nullptr, 0, "RVK_GiOn", "0 1 2"},
    {"RVK_GiSky", "Indirect light, AMD's: the sky's light on rays that hit nothing (0 = bounce light only: the game's ambient is the sky's)", "Effects", Float, 0, 1, 0.05f, 0, nullptr, 0, "RVK_GiOn"},
    {"RVK_RtDebug", "Ray tracing: a view of what its rays hit, coloured by object (0 = off, 1 = on; GPUs with ray tracing only)", "Effects", Choice, 0, 1, 1, 0, nullptr, 0, nullptr, "0 1"},
    {"RVK_VrsCutoff", "Variable rate shading: how much a block's brightness may vary and still be shaded once (higher: more blocks, blurrier)", "Effects", Float, 0.01f, 0.2f, 0.01f, 0.05f, nullptr, 0, "RVK_Vrs"},
    {"RVK_MBlurOn", "Motion blur", "Effects",  Bool,  0, 1, 1, 0, nullptr, 0},
    {"RVK_MBlurObj", "Per-object (off = camera only)", "Effects",  Bool,  0, 1, 1, 1, nullptr, 0, "RVK_MBlurOn"},
    {"RVK_MBlur", "Amount", "Effects",  Float, 0.05f, 2, 0.05f, 0.15f, "RANDYVK_MOTION_BLUR", 0, "RVK_MBlurOn"},
    {"RVK_MBlurNear", "Camera blur: sharp nearer than (world units)", "Effects",  Float, 2, 20, 0.5f, 8, "RANDYVK_MOTION_BLUR_NEAR", 0, "RVK_MBlurOn"},

    {"RVK_Taa",        "Temporal anti-aliasing",                                   "Anti-aliasing",    Bool,  0, 1, 1, 1, nullptr, 0},
    {"RVK_Sharpen", "Sharpening (0 = none)", "Anti-aliasing",    Float, 0, 1, 0.05f, 0.4f, nullptr, 0, "RVK_Taa"},
    {"RVK_RenderScale", "Render scale: the 3D world drawn at this % of the screen's size, the TAA bringing it up to full size (lower is faster)", "Anti-aliasing", Choice, 50, 100, 1, 100, nullptr, 0, "RVK_Taa", "100 85 75 67 50"},
    {"RVK_FrameGen", "Frame generation (AMD FSR 3.1): a frame interpolated between each two - about twice the frames shown, a little more latency", "Anti-aliasing", Bool, 0, 1, 1, 0, nullptr, 0, "RVK_Taa"},
    {"RVK_Upscaler", "Upscaler for the render scale (0: ao-vk's own TAA, 1: AMD FSR 2.2, 2: AMD FSR 3.1)", "Anti-aliasing", Choice, 0, 2, 1, 0, nullptr, 0, "RVK_Taa", "0 1 2"},
    {"RVK_Fxaa",       "FXAA: fast anti-aliasing, used when TAA is off (no smearing, a little softer)", "Anti-aliasing", Bool, 0, 1, 1, 0, nullptr, 0},

    {"RVK_GradeOn",    "Colour grading",                                           "Colour grading",   Bool,  0, 1, 1, 1, nullptr, 0},
    {"RVK_Saturation", "Saturation",                                               "Colour grading",   Float, 0, 2, 0.05f, 1, nullptr, 0, "RVK_GradeOn"},
    {"RVK_Contrast",   "Contrast",                                                 "Colour grading",   Float, 0.5f, 1.5f, 0.05f, 1, nullptr, 0, "RVK_GradeOn"},
    {"RVK_Warmth",     "Warmth (- cooler, + warmer)",                              "Colour grading",   Float, -1, 1, 0.05f, 0.75f, nullptr, 0, "RVK_GradeOn"},
    {"RVK_NightTint", "Night tint (cooler, paler)", "Colour grading",   Float, 0, 1, 0.05f, 0.25f, nullptr, 0, "RVK_GradeOn"},
    {"RVK_Vignette",   "Vignette (darker corners)",                                "Colour grading",   Float, 0, 1, 0.05f, 0, nullptr, 0, "RVK_GradeOn"},
    {"RVK_LutAmount",  "Look-up tables (Ctrl+Shift+L)", "Colour grading", Float, 0, 1, 0.05f, 1, nullptr, 0, "RVK_GradeOn"},
    {"RVK_GoldenHour", "Golden hour: round sunrise and sunset the light warmer, the shade a little cooler, a touch more contrast", "Colour grading", Float, 0, 1, 0.05f, 0.5f, nullptr, 0, "RVK_GradeOn"},
    {"RVK_NeonNight",  "Neon nights: teal shadows, lights keep their colour (in place of randy-vk-night.cube)", "Colour grading", Bool, 0, 1, 1, 0, nullptr, 0, "RVK_GradeOn"},
    {"RVK_Exposure", "Brightness", "Colour grading",  Float, 0.5f, 2, 0.05f, 1, "RANDYVK_EXPOSURE", 0, "RVK_Hdr"},

    {"RVK_Dof",        "Depth of field",                                           "Depth of field",   Bool,  0, 1, 1, 0, nullptr, 0},
    {"RVK_DofBokeh",   "Bokeh (hexagonal highlights; off = smooth blur)",          "Depth of field",   Bool,  0, 1, 1, 1, nullptr, 0, "RVK_Dof"},
    {"RVK_DofNear",    "Blur in front of the focus",                               "Depth of field",   Bool,  0, 1, 1, 1, nullptr, 0, "RVK_Dof"},
    {"RVK_DofFar",     "Blur behind the focus (always)",                           "Depth of field",   Bool,  0, 1, 1, 0, nullptr, 0, "RVK_Dof"},
    {"RVK_DofMacro",   "Close focus distance", "Depth of field", Float, 0, 10, 0.5f, 3, nullptr, 0, "RVK_Dof"},
    {"RVK_DofAmount",  "Blur strength",                                            "Depth of field",   Float, 0, 2, 0.05f, 0.6f, nullptr, 0, "RVK_Dof"},
    {"RVK_DofRadius",  "Largest blur (pixels at 1440 lines)",                      "Depth of field",   Int,   4, 48, 1, 17, nullptr, 0, "RVK_Dof"},
    {"RVK_DofFocus", "Focus distance (m; 0 = your character)", "Depth of field",   Int,   0, 200, 1, 0, nullptr, 0, "RVK_Dof"},
    {"RVK_DofRange",   "In-focus band around it",                                  "Depth of field",   Float, 0, 0.9f, 0.05f, 0.15f, nullptr, 0, "RVK_Dof"},

    {"RVK_Particles",  "GPU particles on effects", "Particles", Bool, 0, 1, 1, 1, nullptr, 0},
    {"RVK_PartCount", "Amount (particles per sprite)", "Particles",        Int,   1, 32, 1, 20, nullptr, 0, "RVK_Particles"},
    {"RVK_PartUniform","Uniform particle size", "Particles",   Float, 0, 1, 0.05f, 1, nullptr, 0, "RVK_Particles"},
    {"RVK_PartAbsSize","Particle size, same for every effect (world units)",       "Particles",        Float, 0.005f, 0.3f, 0.005f, 0.02f, nullptr, 0, "RVK_Particles"},
    {"RVK_PartSize",   "Particle size by sprite",     "Particles",        Float, 0.01f, 0.6f, 0.01f, 0.03f, nullptr, 0, "RVK_Particles"},
    {"RVK_PartBright", "Brightness", "Particles",        Float, 0.25f, 8, 0.25f, 8, nullptr, 0, "RVK_Particles"},
    {"RVK_PartTrail", "Trail length (0 = off)", "Particles",        Float, 0, 0.3f, 0.01f, 0.05f, nullptr, 0, "RVK_Particles"},
    {"RVK_PartTrailMx","Longest trail (particle sizes)",                           "Particles",        Float, 1, 20, 0.5f, 6, nullptr, 0, "RVK_Particles"},
    {"RVK_PartLife",   "Particle life (seconds)",                                  "Particles",        Float, 0.25f, 5, 0.05f, 0.85f, nullptr, 0, "RVK_Particles"},
    {"RVK_PartCurl",   "Flow (curl noise) speed",                                  "Particles",        Float, 0, 5, 0.1f, 1.5f, nullptr, 0, "RVK_Particles"},
    {"RVK_PartScale",  "Flow feature size (world units)",                          "Particles",        Float, 0.25f, 5, 0.05f, 1.5f, nullptr, 0, "RVK_Particles"},
    {"RVK_PartSwirl",  "Swirl around the effect",                                  "Particles",        Float, 0, 5, 0.1f, 3.7f, nullptr, 0, "RVK_Particles"},
    {"RVK_PartPull",   "Pull back towards the effect",                             "Particles",        Float, 0, 3, 0.05f, 0.6f, nullptr, 0, "RVK_Particles"},
    {"RVK_PartDrag",   "How quickly particles follow the flow",                    "Particles",        Float, 0.25f, 10, 0.25f, 2.5f, nullptr, 0, "RVK_Particles"},
    {"RVK_PartSpeed",  "Launch speed",                                             "Particles",        Float, 0, 4, 0.1f, 0.6f, nullptr, 0, "RVK_Particles"},
    {"RVK_PartAdapt",  "Adapt to each effect's own motion (0 = all alike)",        "Particles",        Float, 0, 1, 0.05f, 1, nullptr, 0, "RVK_Particles"},
    {"RVK_PartFollow", "Young particles follow their sprite",                      "Particles",        Float, 0, 1, 0.05f, 0.7f, nullptr, 0, "RVK_Particles"},
    {"RVK_PartCore",   "Brightness of the game's sprites",     "Particles",        Float, 0, 1, 0.05f, 0.1f, nullptr, 0, "RVK_Particles"},
    {"RVK_Fx",         "Our own effects in place of the game's (randy-vk\\fx\\*.rvkfx in the client folder)", "Particles", Bool, 0, 1, 1, 1, nullptr, 0},
    {"RVK_FxLog",      "Log every effect the game makes: template, anchor, who asked (randy-vk.log)", "Particles", Bool, 0, 1, 1, 0, nullptr, 0},
};
constexpr uint32_t kCount = sizeof(g_settings) / sizeof(g_settings[0]);

char g_iniPath[MAX_PATH];
bool g_loaded;
FILETIME g_iniWritten;                           // randy-vk.ini's last write as we last read or wrote it (PollIni)

// randy-vk.ini's last write time (zero if it can't be read).
FILETIME IniWriteTime()
{
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExA(g_iniPath, GetFileExInfoStandard, &a))
        return FILETIME{};
    return a.ftLastWriteTime;
}

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
    {"RVK_PixelLight", 0}, {"RVK_LightOver", 0}, {"RVK_Bump", 0}, {"RVK_NormalMaps", 0}, {"RVK_Pbr", 0}, {"RVK_Albedo", 0}, {"RVK_LiveMeshes", 0}, {"RVK_GroundOn", 0}, {"RVK_GroundAo", 0}, {"RVK_GroundTess", 0}, {"RVK_GroundGrain", 0}, {"RVK_GroundBlend", 0}, {"RVK_GroundSun", 0}, {"RVK_GroundRocks", 0}, {"RVK_Pebbles", 0}, {"RVK_SandRipple", 0}, {"RVK_DuneLight", 0}, {"RVK_SandGlint", 0}, {"RVK_SandPrints", 0}, {"RVK_RippleDepth", 0}, {"RVK_Parallax", 0}, {"RVK_CliffDepth", 0}, {"RVK_GroundRange", 1}, {"RVK_GroundFar", 8}, {"RVK_CliffTex", 0}, {"RVK_CliffRock", 0}, {"RVK_TileHide", 0}, {"RVK_GroundVary", 0}, {"RVK_LeafLight", 0}, {"RVK_CrownShade", 0}, {"RVK_CrownSky", 0}, {"RVK_CrownDepth", 0}, {"RVK_Headroom", 1},
    {"RVK_Sway", 0}, {"RVK_FolEdges", 0}, {"RVK_FolDither", 0}, {"RVK_GrassPush", 0}, {"RVK_GrassOn", 0}, {"RVK_LeafOn", 0}, {"RVK_PlantDetail", 0}, {"RVK_FoliageLod", 0}, {"RVK_PtLight", 1}, {"RVK_CharLight", 1}, {"RVK_Tess", 0}, {"RVK_Aniso", 1}, {"RVK_SunShadow", 0}, {"RVK_Contact", 0}, {"RVK_PtShadows", 0},
    {"RVK_Hdr", 0}, {"RVK_Bloom", 0}, {"RVK_BloomWide", 0}, {"RVK_NeonSat", 0}, {"RVK_BloomFx", 0}, {"RVK_NightGlow", 0}, {"RVK_MoonLight", 0}, {"RVK_Mist", 0}, {"RVK_FogBanks", 0}, {"RVK_HeatFlash", 0}, {"RVK_Sun", 0}, {"RVK_GameSun", 1}, {"RVK_Ao", 0}, {"RVK_Gi", 0},
    {"RVK_Volume", 0}, {"RVK_VolLand", 0}, {"RVK_DesertAir", 0}, {"RVK_HeatShimmer", 0}, {"RVK_Ssr", 0}, {"RVK_MBlur", 0}, {"RVK_Taa", 0}, {"RVK_RenderScale", 100}, {"RVK_Fxaa", 0}, {"RVK_Saturation", 1}, {"RVK_Contrast", 1},
    {"RVK_Warmth", 0}, {"RVK_NightTint", 0}, {"RVK_Vignette", 0}, {"RVK_LutAmount", 0}, {"RVK_Dof", 0},
    {"RVK_Particles", 0}, {"RVK_PartCore", 1}, {"RVK_Fx", 0}, {"RVK_WaterOn", 0}, {"RVK_CloudsOn", 0}, {"RVK_WeatherOn", 0},
    {"RVK_SmokeOn", 0}, {"RVK_Twilight", 0}, {"RVK_SkyGlow", 0}, {"RVK_BlueHour", 0}, {"RVK_MornHaze", 0}, {"RVK_DawnLight", 0},
    {"RVK_GoldenHour", 0}, {"RVK_Cirrus", 0}, {"RVK_Afterglow", 0}, {"RVK_StarFade", 0}, {"RVK_DuskRays", 0}, {"RVK_ProcSky", 0}, {"RVK_Rainbow", 0}, {"RVK_ViewRange", 1},
    {"RVK_LightRig", 0}, {"RVK_Metering", 0}, {"RVK_ZoneFade", 0}, {"RVK_Portals", 0}, {"RVK_StreetView", 0},
};

// Left out of the settings window (randy-vk.ini only, like the "Debug" section): minor switches nearly nobody
// changes, and most of the experimental features' tuning - each keeps its value, its game value and its part in the
// presets. A setting whose feature switch is listed here is left out with it.
const char* const kHidden[] = {
    "RVK_HeapCheck", "RVK_TexKeepCpu",
    "RVK_Prepass", "RVK_LiteShader", "RVK_PrepassEq", "RVK_PrepassMore", "RVK_LightOver", "RVK_OwnLight", "RVK_LeafLightOn", "RVK_PlantDetOn", "RVK_FolLodOn",
    "RVK_GrassTex", "RVK_ShadowMem", "RVK_FxGlowOn", "RVK_NightOn", "RVK_MoonOn", "RVK_GameSun", "RVK_HillShade",
    "RVK_DarkFloor", "RVK_GroundOne", "RVK_ZoneHoldMax", "RVK_PortalDist", "RVK_StreetNow",
    "RVK_FxLog",
    // leaves
    "RVK_LeafSize", "RVK_LeafCore", "RVK_LeafWind", "RVK_LeafFlutter", "RVK_LeafOwn", "RVK_LeafTrees",
    "RVK_LeafShrubs", "RVK_LeafPalms", "RVK_LeafShadow",
    // water
    "RVK_WaterRipple", "RVK_WaterSwim", "RVK_WakeGain", "RVK_WakeLife", "RVK_WakeSpeed", "RVK_WakeStir",
    "RVK_WakeSplash", "RVK_WaterRefl", "RVK_WaterRefr", "RVK_WaterClear", "RVK_WaterFoam", "RVK_WaterCaust",
    "RVK_WaterTex", "RVK_WaterSheen", "RVK_WaterSpark", "RVK_WaterSpray", "RVK_WaterChurn", "RVK_WaterSpeed",
    // weather
    "RVK_RainSpeed", "RVK_RainLength", "RVK_RainLean", "RVK_RainSplash", "RVK_RainBright", "RVK_WindStorm",
    "RVK_StormTint", "RVK_StrikeRate", "RVK_StormDebris", "RVK_StormBolt", "RVK_RainWet", "RVK_StormDark",
    "RVK_StormGreen", "RVK_RainVeils", "RVK_RainFog", "RVK_Lightning", "RVK_StormFlash", "RVK_SandHaze", "RVK_SandAsh",
    "RVK_SandRes", "RVK_SandProc", "RVK_SandContrast", "RVK_SandClear", "RVK_SandGusts", "RVK_SandFog",
    "RVK_SandSun", "RVK_SandTint", "RVK_SandBlow",
    // smoke
    "RVK_SmokeRes", "RVK_SmokeLife", "RVK_SmokeStir", "RVK_SmokeWisps", "RVK_SmokeBright", "RVK_SmokeOpaque",
    "RVK_SmokeBudget", "RVK_SmokeHalf", "RVK_SmokeGame",
    // clouds
    "RVK_CloudThick", "RVK_CloudBright", "RVK_CloudNight", "RVK_SkyVary", "RVK_SkyZones", "RVK_CloudDetail",
    "RVK_CloudHalf", "RVK_CloudLayer", "RVK_CloudHeight", "RVK_CloudShade", "RVK_CloudLow", "RVK_CloudSheet",
    // the sky's colours (RRGGBB: tinkered with in the ini or the console - the window has no colour control)
    "RVK_SkyDayTop", "RVK_SkyDayLow", "RVK_SkyDuskSun", "RVK_SkyDuskAway", "RVK_SkyDuskTop",
    "RVK_SkyBlueTop", "RVK_SkyBlueLow", "RVK_SkyNightTop", "RVK_SkyNightLow", "RVK_Twinkle", "RVK_AuroraCol",
    // the Advanced tab: tuning past what most players change
    "RVK_CrownShade", "RVK_CrownSky", "RVK_CrownDepth", "RVK_TessLevel", "RVK_TessDist", "RVK_PbrSsr", "RVK_PbrAo", "RVK_PbrProbe", "RVK_PbrGlow",
    "RVK_GrassWidth", "RVK_GrassVary", "RVK_GrassGlow", "RVK_GrassGusts", "RVK_TrailTime", "RVK_TrailShade",
    "RVK_GrassEven", "RVK_SunCascade", "RVK_StaticShad", "RVK_PtDay", "RVK_Knee", "RVK_HdrRoom", "RVK_LutAmount",
    "RVK_BloomThr", "RVK_BloomOcc", "RVK_AoRadius", "RVK_GiRadius", "RVK_VolShafts", "RVK_VolRays", "RVK_VolLand",
    "RVK_SunSize", "RVK_SunBleed", "RVK_GiDebug", "RVK_RtDebug", "RVK_SsrWater", "RVK_SsrGloss", "RVK_SsrWet", "RVK_MBlurNear", "RVK_DofMacro",
    "RVK_DofRadius", "RVK_DofRange", "RVK_PartUniform", "RVK_PartAbsSize", "RVK_PartSize", "RVK_PartTrailMx",
    "RVK_PartLife", "RVK_PartCurl", "RVK_PartScale", "RVK_PartSwirl", "RVK_PartPull", "RVK_PartDrag", "RVK_PartSpeed",
    "RVK_PartAdapt", "RVK_PartFollow", "RVK_PartCore",
};

// Switches taken out of the window that are meant to stay on (in randy-vk.ini for troubleshooting): the settings
// they switch stay in the window as if they had no switch. RVK_Hdr - the off-screen scene every effect is drawn in -
// read as HDR display output to players.
const char* const kAlwaysOn[] = {"RVK_Hdr"};

bool AlwaysOn(const char* name)
{
    for (const char* a : kAlwaysOn)
        if (name && std::strcmp(a, name) == 0) return true;
    return false;
}

bool Hidden(const Setting& s)
{
    if (std::strcmp(s.section, "Debug") == 0 || AlwaysOn(s.name)) return true;
    for (const char* h : kHidden)
        if (std::strcmp(h, s.name) == 0) return true;
    const Setting* parent = s.parent && !AlwaysOn(s.parent) ? Find(s.parent) : nullptr;
    return parent && Hidden(*parent);
}

// What each setting in the settings window costs the frame, shown after its label so players can tell where their
// frame rate and GPU time go. The bands are GPU time a frame at 1920 x 1080 (2560 x 1440 at 75% render scale) on an
// RTX 4070 Ti, from in-game logs and rvk_demo sweeps of the native ground: low under 0.3 ms, medium 0.3 to 1 ms, high
// over 1 ms; "more with the value" where the cost follows a slider, "no cost" where the setting only changes the look
// of work done anyway. AOReloaded's Renderer tab explains the bands; keep the two in step.
struct Cost { const char* name; const char* text; };
const Cost kCosts[] = {
    // General
    {"RVK_Enhance", "GPU: all of the below (off: the cheapest)"},
    {"RVK_UiRate", "saves GPU and CPU: 30 or 60 cheaper than every frame"},
    {"RVK_ZoneFade", "no cost"},
    {"RVK_ZoneHoldMax", "no cost"},
    {"RVK_Portals", "GPU: low, with a shop's door in view"},
    {"RVK_PortalDist", "no cost"},
    {"RVK_StreetNow", "no cost"},
    {"RVK_PortalDim", "no cost"},
    {"RVK_StreetView", "GPU: low, with a shop's open door in view"},
    {"RVK_PerfHud", "no cost"},
    {"RVK_AlienMap", "no cost"},
    {"RVK_DetailRange", "CPU and GPU: more with the value"},
    {"RVK_ViewRange", "CPU and GPU: more with the value"},
    // Lighting
    {"RVK_Darkness", "no cost"},
    {"RVK_PixelLight", "GPU: low"},
    {"RVK_PtLight", "no cost"},
    {"RVK_CharLight", "no cost"},
    {"RVK_TessOn", "GPU: low"},
    {"RVK_Tess", "no cost"},
    {"RVK_Aniso", "GPU: low"},
    // Materials
    {"RVK_BumpOn", "GPU: low"},
    {"RVK_Bump", "no cost"},
    {"RVK_NormalMaps", "GPU: low"},
    {"RVK_NormalStr", "no cost"},
    {"RVK_Parallax", "GPU: low"},
    {"RVK_DepthSteps", "GPU: low, more with the value"},
    {"RVK_Pbr", "GPU: low"},
    {"RVK_PbrSpec", "no cost"},
    {"RVK_PbrEnv", "no cost"},
    {"RVK_Albedo", "video memory: more textures"},
    {"RVK_LiveMeshes", "no cost (only meshes that have one)"},
    // Terrain
    {"RVK_GroundOn", "GPU: medium (in place of the game's ground)"},
    {"RVK_GroundAo", "no cost"},
    {"RVK_GroundSun", "no cost"},
    {"RVK_GroundRocks", "GPU: medium, more with the value"},
    {"RVK_Pebbles", "GPU: low"},
    {"RVK_SandRipple", "GPU: low"},
    {"RVK_SandPrints", "GPU: low"},
    {"RVK_RippleDepth", "no cost"},
    {"RVK_RippleSoft", "no cost"},
    {"RVK_DuneLight", "no cost"},
    {"RVK_SandGlint", "no cost"},
    {"RVK_GroundGrain", "GPU: low"},
    {"RVK_GroundBlend", "GPU: low"},
    {"RVK_GroundTess", "GPU: low"},
    {"RVK_CliffDepth", "GPU: low"},
    {"RVK_GroundFar", "video memory: 32 a lot"},
    {"RVK_GroundRange", "CPU: more with the value"},
    {"RVK_CliffTex", "GPU: low"},
    {"RVK_CliffRock", "no cost"},
    {"RVK_TileHide", "GPU: low"},
    {"RVK_GroundVary", "no cost"},
    // Plants
    {"RVK_SwayOn", "GPU: low"},
    {"RVK_Sway", "no cost"},
    {"RVK_PushOn", "GPU: low"},
    {"RVK_GrassPush", "no cost"},
    {"RVK_FolEdges", "GPU: low"},
    {"RVK_FolDither", "no cost"},
    {"RVK_GrassOn", "GPU: medium, high in thick grass"},
    {"RVK_GrassDist", "GPU: more with the value"},
    {"RVK_GrassBlades", "GPU: more with the value"},
    {"RVK_GrassHeight", "GPU: more with the value"},
    {"RVK_GrassBright", "no cost"},
    {"RVK_GrassFlower", "no cost"},
    {"RVK_GrassTrail", "GPU: low"},
    {"RVK_LeafOn", "GPU: medium, high in forests"},
    {"RVK_LeafDensity", "GPU: more with the value"},
    {"RVK_LeafDist", "GPU: more with the value"},
    {"RVK_LeafFall", "GPU: low"},
    // Water
    {"RVK_WaterOn", "GPU: low, medium on a large lake or the sea"},
    {"RVK_WaterStyle", "no cost"},
    {"RVK_WaterQual", "GPU: more with the value"},
    {"RVK_WaterMesh", "no cost"},
    {"RVK_WaterWaves", "no cost"},
    // Weather
    {"RVK_WeatherOn", "GPU: low, medium in storms"},
    {"RVK_RainForce", "GPU: medium while it rains"},
    {"RVK_RainAmount", "GPU: more with the value"},
    {"RVK_StormForce", "GPU: medium while it storms"},
    {"RVK_StormThick", "no cost"},
    {"RVK_HeatFlash", "no cost"},
    {"RVK_SandForce", "GPU: medium in a sandstorm"},
    // Smoke
    {"RVK_SmokeOn", "GPU: medium, high near many fires"},
    {"RVK_SmokeQual", "GPU: more with the value"},
    {"RVK_SmokeThick", "no cost"},
    // Sky
    {"RVK_CloudsOn", "GPU: medium"},
    {"RVK_ProcSky", "GPU: low"},
    {"RVK_SkyZone", "no cost"},
    {"RVK_SkyBright", "no cost"},
    {"RVK_Twilight", "no cost"},
    {"RVK_BlueHour", "no cost"},
    {"RVK_SkyGlow", "no cost"},
    {"RVK_Afterglow", "no cost"},
    {"RVK_StarFade", "no cost"},
    {"RVK_Stars", "GPU: low"},
    {"RVK_StarBright", "no cost"},
    {"RVK_Galaxy", "no cost"},
    {"RVK_Meteors", "no cost"},
    {"RVK_Aurora", "GPU: low"},
    {"RVK_AuroraForce", "GPU: low"},
    {"RVK_Rainbow", "no cost"},
    {"RVK_Cirrus", "GPU: low"},
    {"RVK_CloudShadow", "GPU: low"},
    {"RVK_CloudSteps", "GPU: more with the value"},
    {"RVK_CloudCover", "no cost"},
    {"RVK_CloudQual", "GPU: more with the value"},
    {"RVK_CloudMorph", "no cost"},
    // Shadows
    {"RVK_SunShadow", "GPU: high outdoors by day"},
    {"RVK_RtShadow", "GPU: medium"},
    {"RVK_SunRes", "GPU and video memory: more with the value"},
    {"RVK_SunStrength", "no cost"},
    {"RVK_SunDist", "GPU: more with the value"},
    {"RVK_SunSoft", "GPU: low"},
    {"RVK_GrassShadow", "GPU: medium in grass"},
    {"RVK_ContactOn", "GPU: low"},
    {"RVK_Contact", "no cost"},
    {"RVK_PtOn", "GPU: very high in towns at night"},
    {"RVK_PtShadows", "GPU: more with the value"},
    {"RVK_PtRes", "GPU and video memory: more with the value"},
    {"RVK_PtStrength", "no cost"},
    {"RVK_OwnShadow", "no cost"},
    // Effects
    {"RVK_BloomOn", "GPU: low"},
    {"RVK_Bloom", "no cost"},
    {"RVK_BloomWide", "no cost"},
    {"RVK_NeonSat", "no cost"},
    {"RVK_AoOn", "GPU: low (ray traced: medium)"},
    {"RVK_Ao", "no cost"},
    {"RVK_AoMode", "GPU: AMD's a little more, ray traced medium"},
    {"RVK_GiOn", "GPU: low (AMD's: high)"},
    {"RVK_Gi", "no cost"},
    {"RVK_GiMode", "GPU: AMD's high - about 1 ms more"},
    {"RVK_GiSky", "no cost"},
    {"RVK_VolOn", "GPU: medium"},
    {"RVK_Volume", "no cost"},
    {"RVK_VolHaze", "no cost"},
    {"RVK_VolLamps", "GPU: low, medium by many lamps"},
    {"RVK_DuskRays", "GPU: low"},
    {"RVK_DesertAir", "no cost"},
    {"RVK_HeatShimmer", "GPU: low"},
    {"RVK_Sun", "no cost"},
    {"RVK_Atmos", "GPU: low"},
    {"RVK_Mist", "no cost"},
    {"RVK_FogBanks", "GPU: low"},
    {"RVK_DawnLight", "no cost"},
    {"RVK_MornHaze", "no cost"},
    {"RVK_SsrOn", "GPU: low (AMD's: medium)"},
    {"RVK_Ssr", "no cost"},
    {"RVK_RtReflect", "GPU: low to medium"},
    {"RVK_SsrMode", "GPU: AMD's about 0.25 ms more"},
    {"RVK_SssrThick", "no cost"},
    {"RVK_Vrs", "saves GPU: about 0.5 ms"},
    {"RVK_VrsCutoff", "saves GPU: more with the value"},
    {"RVK_MBlurOn", "GPU: low"},
    {"RVK_MBlurObj", "GPU: low"},
    {"RVK_MBlur", "no cost"},
    // Anti-aliasing
    {"RVK_Taa", "GPU: low (FSR: medium) - and it lets Render scale save"},
    {"RVK_Sharpen", "no cost"},
    {"RVK_RenderScale", "saves GPU: the biggest saving - lower is much faster"},
    {"RVK_FrameGen", "GPU: medium - about twice the frames shown"},
    {"RVK_Upscaler", "GPU: FSR about 0.5 ms more than ao-vk's own"},
    {"RVK_Fxaa", "GPU: low"},
    // Colour grading, depth of field, particles
    {"RVK_GradeOn", "no cost"},
    {"RVK_Saturation", "no cost"},
    {"RVK_Contrast", "no cost"},
    {"RVK_LightRig", "no cost"},
    {"RVK_Metering", "no cost"},
    {"RVK_Warmth", "no cost"},
    {"RVK_NightTint", "no cost"},
    {"RVK_Vignette", "no cost"},
    {"RVK_GoldenHour", "no cost"},
    {"RVK_NeonNight", "no cost"},
    {"RVK_Exposure", "no cost"},
    {"RVK_Dof", "GPU: low"},
    {"RVK_DofBokeh", "GPU: low"},
    {"RVK_DofNear", "GPU: low"},
    {"RVK_DofFar", "no cost"},
    {"RVK_DofAmount", "no cost"},
    {"RVK_DofFocus", "no cost"},
    {"RVK_Particles", "GPU: low"},
    {"RVK_PartCount", "GPU: more with the value"},
    {"RVK_Fx", "GPU: low, while an effect plays"},
    {"RVK_FxLog", "no cost"},
    {"RVK_PartBright", "no cost"},
    {"RVK_PartTrail", "GPU: low"},
};

// Names for a choice setting's values in the settings window, in the order of its `choices`.
const Cost kChoiceNames[] = {
    {"RVK_UiRate", "every frame|30|60|90|120"},
    {"RVK_Aniso", "off|2x|4x|8x|16x"},
    {"RVK_GroundFar", "8 (the game's)|16|32"},
    {"RVK_Stars", "the game's|ours"},
    {"RVK_CloudQual", "low|medium|high|ultra"},
    {"RVK_AoMode", "ours|AMD CACAO|ray traced"},
    {"RVK_GiMode", "ours|AMD Brixelizer"},
    {"RVK_SsrMode", "ours|AMD SSSR"},
    {"RVK_Vrs", "off|on"},
    {"RVK_RenderScale", "100%|85%|75%|67%|50%"},
    {"RVK_Upscaler", "ao-vk's TAA|AMD FSR 2.2|AMD FSR 3.1"},
};

const char* Lookup(const Cost* table, size_t n, const char* name)
{
    for (size_t i = 0; i < n; ++i)
        if (std::strcmp(table[i].name, name) == 0) return table[i].text;
    return nullptr;
}
const char* CostOf(const char* name) { return Lookup(kCosts, std::size(kCosts), name); }
const char* ChoiceNamesOf(const char* name) { return Lookup(kChoiceNames, std::size(kChoiceNames), name); }

float Stored(const char* name) { Setting* s = Find(name); return s ? s->value : 0.0f; }

// The features out of "Experimental" (2026-10-16: tested enough - the user): on by default, never inferred.
bool NewFeature(const char* name)
{
    for (const char* n : {"RVK_LeafOn", "RVK_WaterOn", "RVK_WeatherOn", "RVK_SmokeOn", "RVK_CloudsOn"})
        if (std::strcmp(n, name) == 0) return true;
    return false;
}

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

// A value as written in randy-vk.ini: a number - or, for a colour, RRGGBB (# or 0x before it allowed). False: not one.
bool ParseIni(const Setting& s, const char* text, float* out)
{
    while (*text == ' ' || *text == '\t') ++text;
    char* end = nullptr;
    if (s.type == Colour) {
        if (*text == '#') ++text;
        else if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text += 2;
        const unsigned long v = std::strtoul(text, &end, 16);
        if (end == text || end - text > 6) return false;
        *out = float(v & 0xFFFFFFul);
        return true;
    }
    const float v = std::strtof(text, &end);
    if (end == text || !std::isfinite(v)) return false;
    *out = v;
    return true;
}

// Settings renamed or given a new default since players have had them. The ini keeps every setting, so neither
// reaches a player by itself: once (the entry's id under [Migrations]), the value under the old name moves to the new
// one - and a value still at the old default, which the player never chose, takes the new default instead (NaN: kept
// as it is). In [Renderer] and in the presets saved in the ini (AOReloaded's slots) alike. A new default for a
// setting that keeps its name: an entry with `was` the same as `now`. kAny: every value takes the new default (a
// setting whose scale changed meaning, so what a player chose on the old one no longer says what it did).
struct Migration { const char* id; const char* now; const char* was; float oldDefault; };
constexpr float kKeep = std::numeric_limits<float>::quiet_NaN();
constexpr float kAny = std::numeric_limits<float>::infinity();
const Migration kMigrations[] = {
    // (Most for the game's option variables, which take at most 15 characters: AOReloaded's settings window leaves a
    // longer name out, so the setting couldn't be changed in game - RVK_NativeGround, the native ground's own switch,
    // among them, 2026-10-20.)
    {"SmokeThick", "RVK_SmokeThick", "RVK_SmokeDensity", kKeep},
    {"GroundOn", "RVK_GroundOn", "RVK_NativeGround", kKeep},
    {"Pebbles", "RVK_Pebbles", "RVK_GroundPebble", kKeep},
    {"DepthSteps", "RVK_DepthSteps", "RVK_ParallaxSteps", kKeep},
    {"RippleSoft", "RVK_RippleSoft", "RVK_RippleSmooth", kKeep},
    // (One setting for how dark the game gets, lighter by default: docs/darkness-plan.md.)
    {"Darkness", "RVK_Darkness", "RVK_NightDark", 1.0f},
    // (0.7.0's defaults: the user's own setup, 2026-10-20 - render scale, upscaler and frame generation left off.
    // The settings 0.6.1 had already; the newer ones come with their new defaults.)
    {"BloomThr070", "RVK_BloomThr", "RVK_BloomThr", 1.0f},
    {"Gi070", "RVK_Gi", "RVK_Gi", 1.75f},
    {"Ssr070", "RVK_Ssr", "RVK_Ssr", 1.0f},
    {"BumpOn070", "RVK_BumpOn", "RVK_BumpOn", 0.0f},
    {"MBlurOn070", "RVK_MBlurOn", "RVK_MBlurOn", 1.0f},
    {"PbrGlow070", "RVK_PbrGlow", "RVK_PbrGlow", 1.75f},
    {"CloudQual070", "RVK_CloudQual", "RVK_CloudQual", 2.0f},
    // (0.8.1: RVK_Darkness 0 is the game's own night light again, as before the light rig; under 0.8.0 even 0 was
    // too dark for players, and any value chosen then was chosen against those darker nights. Everyone starts at 0.)
    {"Darkness081", "RVK_Darkness", "RVK_Darkness", kAny},
};

// A key in randy-vk.ini's [Renderer] twice (a hand edit): only the first counts - an edit to the other did nothing
// (the user's RVK_CloudNight, 2026-10-20). Logged once at load.
void LogDuplicateKeys()
{
    FILE* f = std::fopen(g_iniPath, "r");
    if (!f) return;
    std::vector<std::string> seen;
    char line[512];
    bool renderer = false;
    while (std::fgets(line, sizeof(line), f)) {
        if (line[0] == '[') {
            renderer = std::strncmp(line, "[Renderer]", 10) == 0;
            continue;
        }
        const char* eq = std::strchr(line, '=');
        if (!renderer || !eq) continue;
        std::string key(line, size_t(eq - line));
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
        if (std::find(seen.begin(), seen.end(), key) != seen.end())
            RvkLog("settings: %s is in randy-vk.ini's [Renderer] more than once - only the first counts; remove the "
                   "others", key.c_str());
        else
            seen.push_back(key);
    }
    std::fclose(f);
}

// kMigrations, each once (inIni: which settings the ini's [Renderer] held - a value moved in counts as held).
void Migrate(std::vector<bool>& inIni)
{
    for (const Migration& m : kMigrations) {
        if (GetPrivateProfileIntA("Migrations", m.id, 0, g_iniPath) != 0)
            continue;
        Setting* s = Find(m.now);
        if (!s) continue;
        const size_t index = size_t(s - g_settings);
        const bool renamed = std::strcmp(m.now, m.was) != 0;
        auto read = [&](const char* section, float* out) {
            char text[64] = "";
            GetPrivateProfileStringA(section, m.was, "", text, sizeof(text), g_iniPath);
            if (!text[0] || !ParseIni(*s, text, out))
                return false;
            if (std::isinf(m.oldDefault) || (!std::isnan(m.oldDefault) && std::fabs(*out - m.oldDefault) < 1e-4f))
                *out = s->defaultValue;
            *out = Clamp(*s, *out);
            return true;
        };
        float v;
        if ((!renamed || !inIni[index]) && read("Renderer", &v)) {
            RvkLog("settings: %s = %g (migration %s, from %s)", m.now, v, m.id, m.was);
            s->value = v;
            inIni[index] = true;
        }
        if (renamed)
            WritePrivateProfileStringA("Renderer", m.was, nullptr, g_iniPath);
        for (int i = 1; i <= 9; ++i) {
            char section[16];
            std::snprintf(section, sizeof(section), "Preset %d", i);
            if (!read(section, &v))
                continue;
            char text[32];
            if (s->type == Colour) std::snprintf(text, sizeof(text), "%06X", unsigned(v) & 0xFFFFFFu);
            else std::snprintf(text, sizeof(text), "%g", v);
            if (renamed)
                WritePrivateProfileStringA(section, m.was, nullptr, g_iniPath);
            WritePrivateProfileStringA(section, m.now, text, g_iniPath);
        }
        WritePrivateProfileStringA("Migrations", m.id, "1", g_iniPath);
    }
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
    else if (s.type == Colour) std::snprintf(buf, sizeof(buf), "%06X", unsigned(s.value) & 0xFFFFFFu);
    else std::snprintf(buf, sizeof(buf), "%d", int(s.value));
    WritePrivateProfileStringA("Renderer", s.name, buf, g_iniPath);
    g_iniWritten = IniWriteTime();              // (our own write: PollIni mustn't take it for an edit)
}

void ApplyOne(const Setting& s, rvk::ThreadedDevice* d);

// Our sky's colours (docs/sky-plan.md §1): the nine RRGGBB keys.
rvk::SkyColours SkyColoursNow()
{
    static const char* const kKeys[rvk::kSkyKeyCount] = {
        "RVK_SkyDayTop", "RVK_SkyDayLow", "RVK_SkyDuskSun", "RVK_SkyDuskAway", "RVK_SkyDuskTop", "RVK_SkyBlueTop",
        "RVK_SkyBlueLow", "RVK_SkyNightTop", "RVK_SkyNightLow"};
    rvk::SkyColours c;
    for (uint32_t i = 0; i < rvk::kSkyKeyCount; ++i) c.keys[i] = uint32_t(V(kKeys[i])) & 0xFFFFFFu;
    return c;
}

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
    // Static meshes the game culls cast anyway within the shadows' reach (native mesh.cpp; as the renderer's memory of
    // casters: 1.4 x the shadow distance), meshes under a unit across left out.
    // The plain settings (rvk.h RVK_FLOAT_KNOBS, RVK_BOOL_KNOBS): one value each, set by name.
    if (const int knob = rvk::KnobByName(n); knob >= 0) {
        d->SetKnob(rvk::Knob(knob), V(n));
        return;
    }
    if (is("RVK_Enhance")) {                     // every enhancement at its own value or the game's
        for (const Setting& other : g_settings)
            if (&other != &s) Apply(other, d);
    }
    else if (is("RVK_UiRate")) SetInterfaceRate(V(n));
    else if (is("RVK_LiveMeshes")) SetLiveMeshes(V(n) != 0.0f);
    else if (is("RVK_PixelLight")) d->SetPixelLighting(V(n) != 0.0f);
    else if (is("RVK_LightOver")) d->SetLightOverride(V(n) != 0.0f);
    else if (is("RVK_Bump")) d->SetBump(V(n));
    else if (is("RVK_NormalMaps") || is("RVK_NormalStr")) d->SetNormalMaps(V("RVK_NormalMaps") != 0.0f, V("RVK_NormalStr"));
    else if (is("RVK_Pbr") || is("RVK_PbrSpec") || is("RVK_PbrEnv") || is("RVK_PbrSsr") || is("RVK_PbrAo") ||
             is("RVK_PbrGlow") || is("RVK_PbrProbe") || is("RVK_PbrDebug") || is("RVK_Albedo")) {
        rvk::Device::PbrSettings p;
        p.enabled = V("RVK_Pbr") != 0.0f;
        p.specular = V("RVK_PbrSpec");
        p.ambient = V("RVK_PbrEnv");
        p.reflections = V("RVK_PbrSsr");
        p.occlusion = V("RVK_PbrAo");
        p.emissive = V("RVK_PbrGlow");
        p.probe = V("RVK_PbrProbe");
        p.debug = p.enabled ? uint32_t(V("RVK_PbrDebug")) : 0u;   // (not in kVanilla: the ini migration would read
                                                                   // its 0 as "the feature was off")
        p.albedoMaps = V("RVK_Albedo") != 0.0f;
        d->SetPbr(p);
    }
    else if (is("RVK_Aniso")) d->SetAnisotropy(uint32_t(V(n)));
    else if (is("RVK_Tess") || is("RVK_TessLevel") || is("RVK_TessDist"))
        d->SetTessellation(V("RVK_Tess"), V("RVK_TessDist"), uint32_t(V("RVK_TessLevel")));
    else if (is("RVK_PtLight") || is("RVK_CharLight")) d->SetPointLightIntensity(V("RVK_PtLight"), V("RVK_CharLight"));
    else if (is("RVK_SunShadow")) d->SetShadows(V(n) != 0.0f);
    else if (is("RVK_StaticShad")) d->SetStatelMode(uint32_t(V(n)));
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
    else if (is("RVK_GrassOn") || is("RVK_GrassTex") || is("RVK_GrassDist") || is("RVK_GrassBlades") ||
             is("RVK_GrassHeight"))
        d->SetGrassField(V("RVK_GrassOn") != 0.0f, V("RVK_GrassDist"), V("RVK_GrassBlades"), V("RVK_GrassHeight"),
                         V("RVK_GrassTex") != 0.0f);
    else if (is("RVK_GrassVary") || is("RVK_GrassFlower") || is("RVK_GrassGlow") || is("RVK_GrassGusts") ||
             is("RVK_GrassTrail"))
        d->SetGrassStyle(V("RVK_GrassVary"), V("RVK_GrassFlower"), V("RVK_GrassGlow"), V("RVK_GrassGusts"),
                         V("RVK_GrassTrail") != 0.0f);
    else if (is("RVK_TrailTime") || is("RVK_TrailShade"))
        d->SetGrassTrailLook(V("RVK_TrailTime"), V("RVK_TrailShade"));
    else if (is("RVK_GrassShadow"))
        d->SetGrassShadows(V("RVK_GrassShadow") != 0.0f);
    else if (is("RVK_GrassEven"))
        d->SetGrassEven(V("RVK_GrassEven"));
    else if (std::strncmp(n, "RVK_Leaf", 8) == 0 && std::strcmp(n, "RVK_LeafLight") != 0 &&
             std::strcmp(n, "RVK_LeafLightOn") != 0) {
        rvk::Device::LeafSettings l;
        l.on = V("RVK_LeafOn") != 0.0f;
        l.density = V("RVK_LeafDensity");
        l.size = V("RVK_LeafSize");
        l.distance = V("RVK_LeafDist");
        l.core = V("RVK_LeafCore");
        l.wind = V("RVK_LeafWind");
        l.flutter = V("RVK_LeafFlutter");
        l.trees = V("RVK_LeafTrees") != 0.0f;
        l.own = V("RVK_LeafOwn") != 0.0f;
        l.shrubs = V("RVK_LeafShrubs") != 0.0f;
        l.palms = V("RVK_LeafPalms") != 0.0f;
        l.shadows = V("RVK_LeafShadow") != 0.0f;
        l.fall = V("RVK_LeafFall");
        d->SetLeaves(l);
    }
    else if (is("RVK_Taa") || is("RVK_Sharpen")) d->SetTaa(V("RVK_Taa") != 0.0f, V("RVK_Sharpen"));
    else if (is("RVK_RenderScale")) d->SetRenderScale(uint32_t(V(n)));
    else if (is("RVK_Upscaler")) d->SetUpscaler(uint32_t(V(n)));
    else if (is("RVK_FrameGen")) d->SetFrameGen(V(n) != 0.0f);
    else if (is("RVK_HeatFlash")) d->SetHeatFlash(V(n));
    else if (is("RVK_SandForce") || is("RVK_SandAsh")) d->SetSandForce(V("RVK_SandForce"), V("RVK_SandAsh") != 0.0f);
    else if (is("RVK_Saturation") || is("RVK_Contrast") || is("RVK_Warmth") || is("RVK_NightTint") ||
             is("RVK_Vignette") || is("RVK_LutAmount"))
        d->SetGrading(V("RVK_Saturation"), V("RVK_Contrast"), V("RVK_Warmth"), V("RVK_LutAmount"), V("RVK_NightTint"),
                      V("RVK_Vignette"));
    else if (is("RVK_NeonNight")) rvk_settings::LoadLuts(d);
    else if (is("RVK_LeafLightOn") || is("RVK_LeafLight")) d->SetLeafLight(V("RVK_LeafLight"));
    else if (is("RVK_Darkness") || is("RVK_DarkFloor")) d->SetDarkness(V("RVK_Darkness"), V("RVK_DarkFloor"));
    else if (is("RVK_FogBanks")) d->SetFogBanks(V(n));
    else if (is("RVK_StarFade")) d->SetStarFade(V(n) > 0.5f);
    else if (is("RVK_HeapCheck")) d->SetHeapCheck(int(V(n) + 0.5f));
    else if (is("RVK_ViewRange")) {
        d->SetViewRange(V(n));
        rnative::camera::SetViewRange(V(n));
    }
    else if (is("RVK_ProcSky") || is("RVK_SkyZone") || is("RVK_SkyBright"))
        d->SetProcSky(V("RVK_ProcSky") > 0.5f, V("RVK_SkyZone"), V("RVK_SkyBright"));
    else if (is("RVK_Stars") || is("RVK_StarBright") || is("RVK_Twinkle") || is("RVK_Galaxy") || is("RVK_Meteors") ||
             is("RVK_Aurora") || is("RVK_AuroraCol") || is("RVK_AuroraForce")) {
        rvk::NightSky n;
        n.stars = V("RVK_Stars") > 0.5f;
        n.bright = V("RVK_StarBright");
        n.twinkle = V("RVK_Twinkle");
        n.galaxy = V("RVK_Galaxy");
        n.meteors = V("RVK_Meteors");
        n.aurora = V("RVK_Aurora");
        n.auroraColour = uint32_t(V("RVK_AuroraCol")) & 0xFFFFFFu;
        n.auroraForce = V("RVK_AuroraForce") > 0.0f ? V("RVK_AuroraForce") : -1.0f;
        d->SetNightSky(n);
    }
    else if (is("RVK_Rainbow")) d->SetRainbow(V(n));
    else if (s.type == Colour)                    // (the sky's colour keys)
        d->SetSkyColours(SkyColoursNow());
    else if (is("RVK_DesertAir") || is("RVK_HeatShimmer")) d->SetDesertAir(V("RVK_DesertAir"), V("RVK_HeatShimmer"));
    // The game's sun sprites: with RVK_GameSun, or whenever our suns are off (HDR off, RVK_Sun 0) - never no sun.
    else if (is("RVK_Sun") || is("RVK_GameSun")) {
        d->SetSun(V("RVK_Sun"));
        d->SetGameSun(V("RVK_GameSun") != 0.0f || V("RVK_Sun") == 0.0f);
    }
    else if (is("RVK_Twilight") || is("RVK_SkyGlow") || is("RVK_BlueHour"))
        d->SetTwilight(V("RVK_Twilight"), V("RVK_SkyGlow"), V("RVK_BlueHour"));
    else if (is("RVK_Occlusion")) rnative::occluder::SetHorizonCulling(V(n) != 0.0f);
    else if (is("RVK_DetailRange")) rnative::rings::SetScale(V(n));
    else if (is("RVK_GroundRange")) rnative::ground::SetRange(V(n));
    else if (is("RVK_GroundOn")) SetNativeGroundSetting(V(n) != 0.0f);
    else if (is("RVK_GroundAo")) { if (g_rvk.device) g_rvk.device->SetNativeGroundAo(V(n)); }
    else if (is("RVK_GroundSun")) { if (g_rvk.device) g_rvk.device->SetNativeGroundSun(V(n)); }
    else if (is("RVK_GroundRocks")) { if (g_rvk.device) g_rvk.device->SetNativeGroundRocks(V(n)); }
    else if (is("RVK_Pebbles")) { if (g_rvk.device) g_rvk.device->SetNativeGroundPebbles(V(n)); }
    else if (is("RVK_GroundFar")) rnative::ground::SetFar(int(V(n) + 0.5f));
    else if (is("RVK_ShadowMem")) d->SetShadowMemory(V(n) != 0.0f ? 1u : 0u);
    else if (is("RVK_CliffTex") || is("RVK_CliffRock") || is("RVK_TileHide") || is("RVK_GroundVary") ||
             is("RVK_GroundTess") || is("RVK_CliffDepth") || is("RVK_GroundGrain") ||
             is("RVK_GroundBlend") || is("RVK_Parallax") || is("RVK_DepthSteps") || is("RVK_SandRipple") ||
             is("RVK_DuneLight") || is("RVK_SandGlint") || is("RVK_RippleSoft") || is("RVK_SandPrints") || is("RVK_RippleDepth")) {
        rvk::Device::TerrainSettings t;
        t.cliff = V("RVK_CliffTex");
        t.rock = V("RVK_CliffRock") != 0.0f;
        t.tiling = V("RVK_TileHide");
        t.variation = V("RVK_GroundVary");
        t.round = V("RVK_GroundTess");
        t.depth = V("RVK_CliffDepth");
        t.grain = V("RVK_GroundGrain");
        t.blend = V("RVK_GroundBlend");
        t.parallax = V("RVK_Parallax");
        t.parallaxSteps = V("RVK_DepthSteps");
        t.ripple = V("RVK_SandRipple");
        t.dune = V("RVK_DuneLight");
        t.glint = V("RVK_SandGlint");
        t.smooth = V("RVK_RippleSoft");
        t.rippleDepth = V("RVK_RippleDepth");
        t.prints = V("RVK_SandPrints");
        d->SetTerrain(t);
    }
    else if (is("RVK_Ao") || is("RVK_AoRadius")) d->SetAo(V("RVK_Ao"), V("RVK_AoRadius"));
    else if (is("RVK_Gi") || is("RVK_GiRadius")) d->SetGi(V("RVK_Gi"), V("RVK_GiRadius"));
    else if (std::strncmp(n, "RVK_Ssr", 7) == 0)
        d->SetSsr(V("RVK_Ssr"), V("RVK_SsrWater"), V("RVK_SsrGloss"), V("RVK_SsrWet"));
    else if (std::strncmp(n, "RVK_Water", 9) == 0 || std::strncmp(n, "RVK_Wake", 8) == 0) {
        rvk::Device::WaterParams w;
        w.enable = V("RVK_WaterOn") != 0.0f;
        w.style = V("RVK_WaterStyle");
        w.quality = uint32_t(V("RVK_WaterQual"));
        w.mesh = V("RVK_WaterMesh") != 0.0f;
        w.waves = V("RVK_WaterWaves");
        w.ripples = V("RVK_WaterRipple");
        w.swimRipples = V("RVK_WaterSwim");
        w.swimGain = V("RVK_WakeGain");
        w.swimLife = V("RVK_WakeLife");
        w.swimSpeed = V("RVK_WakeSpeed");
        w.swimStir = V("RVK_WakeStir");
        w.swimSplash = V("RVK_WakeSplash");
        w.reflections = V("RVK_WaterRefl");
        w.refraction = V("RVK_WaterRefr");
        w.clarity = V("RVK_WaterClear");
        w.foam = V("RVK_WaterFoam");
        w.caustics = V("RVK_WaterCaust");
        w.gameTexture = V("RVK_WaterTex");
        w.fallSheen = V("RVK_WaterSheen");
        w.fallSparkle = V("RVK_WaterSpark");
        w.fallSpray = V("RVK_WaterSpray");
        w.fallChurn = V("RVK_WaterChurn");
        w.fallSpeed = V("RVK_WaterSpeed");
        d->SetWaterParams(w);
    }
    else if (std::strncmp(n, "RVK_Weather", 11) == 0 || std::strncmp(n, "RVK_Rain", 8) == 0 ||
             std::strcmp(n, "RVK_StormForce") == 0 || std::strcmp(n, "RVK_StrikeRate") == 0) {
        rvk::Device::WeatherParams w;
        w.enable = V("RVK_WeatherOn") != 0.0f;
        w.force = V("RVK_RainForce");
        w.amount = V("RVK_RainAmount");
        w.splashes = V("RVK_RainSplash");
        w.brightness = V("RVK_RainBright");
        w.speed = V("RVK_RainSpeed");
        w.length = V("RVK_RainLength");
        w.lean = V("RVK_RainLean");
        w.stormForce = V("RVK_StormForce");
        w.strikeRate = V("RVK_StrikeRate");
        d->SetWeatherParams(w);
    }
    else if (std::strncmp(n, "RVK_Smoke", 9) == 0 || std::strcmp(n, "RVK_SandRes") == 0) {
        rvk::Device::SmokeParams s;
        s.enable = V("RVK_SmokeOn") != 0.0f;
        s.quality = uint32_t(V("RVK_SmokeQual"));
        s.sandCells = uint32_t(V("RVK_SandRes"));
        s.resolution = V("RVK_SmokeRes");
        s.density = V("RVK_SmokeThick");
        s.stir = V("RVK_SmokeStir");
        s.detail = V("RVK_SmokeWisps");
        s.brightness = V("RVK_SmokeBright");
        s.opacity = V("RVK_SmokeOpaque");
        s.half = V("RVK_SmokeHalf") != 0.0f;
        s.budget = V("RVK_SmokeBudget");
        d->SetSmokeParams(s);
    }
    else if (std::strncmp(n, "RVK_Cloud", 9) == 0 || std::strcmp(n, "RVK_SkyVary") == 0 || std::strcmp(n, "RVK_SkyZones") == 0 ||
             std::strcmp(n, "RVK_Cirrus") == 0) {
        rvk::Device::CloudParams c;
        c.enable = V("RVK_CloudsOn") != 0.0f;
        c.density = V("RVK_CloudThick");
        c.brightness = V("RVK_CloudBright");
        c.night = V("RVK_CloudNight");
        c.vary = V("RVK_SkyVary");
        c.zones = V("RVK_SkyZones");
        c.detail = V("RVK_CloudDetail");
        c.steps = uint32_t(V("RVK_CloudSteps"));
        c.half = V("RVK_CloudHalf") != 0.0f;
        c.shadow = V("RVK_CloudShadow");
        c.layer = V("RVK_CloudLayer") != 0.0f;
        c.cover = V("RVK_CloudCover");
        c.height = V("RVK_CloudHeight");
        c.shade = V("RVK_CloudShade");
        c.low = V("RVK_CloudLow");
        c.morph = V("RVK_CloudMorph");
        c.quality = uint32_t(V("RVK_CloudQual"));
        c.sheet = V("RVK_CloudSheet");
        c.cirrus = V("RVK_Cirrus");
        d->SetCloudParams(c);
    }
    else if (std::strncmp(n, "RVK_Vol", 7) == 0) d->SetVolume(V("RVK_Volume"), V("RVK_VolHaze"), V("RVK_VolShafts"),
                                                                    V("RVK_VolRays"), V("RVK_VolLand"));
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
    LogDuplicateKeys();
    for (const Setting& s : g_settings)             // the game's option variables take at most 15 characters: a longer
        if (std::strlen(s.name) > 15)               // name's slider has no variable behind it and breaks the game
            RvkLog("settings: %s is longer than 15 characters - it can't be in the Renderer tab", s.name);
    std::vector<bool> inIni;
    for (Setting& s : g_settings) {
        s.value = s.defaultValue;
        char buf[64] = "";
        GetPrivateProfileStringA("Renderer", s.name, "", buf, sizeof(buf), g_iniPath);
        inIni.push_back(buf[0] != 0);
        float read = 0.0f;
        if (buf[0] && !ParseIni(s, buf, &read)) {   // (a NaN a broken slider saved - user, 2026-10-14)
            RvkLog("settings: %s = %s in randy-vk.ini is not a number - its default instead", s.name, buf);
            buf[0] = 0;
        }
        inIni.back() = buf[0] != 0;
        if (buf[0]) {
            s.value = read;
        } else if (s.legacyEnv && GetEnvironmentVariableA(s.legacyEnv, buf, sizeof(buf)) && buf[0]) {
            s.value = float(std::atof(buf));      // first run: take over the old ao-wine.sh setting
        } else if (std::strcmp(s.name, "RVK_MBlurObj") == 0 &&
                   GetEnvironmentVariableA("RANDYVK_MOTION_BLUR_MODE", buf, sizeof(buf)) && buf[0]) {
            s.value = std::strcmp(buf, "camera") == 0 ? 0.0f : 1.0f;
        }
    }
    Migrate(inIni);
    // A feature's on / off setting new to the ini (older ones had "0 = off" strengths): on unless its strength (the
    // first setting it switches with a game value) was at the game's value - which then gets its default back, so
    // that switching the feature on shows it.
    for (size_t i = 0; i < kCount; ++i) {
        Setting& t = g_settings[i];
        // Experimental features stay at their default for a new ini (and the ones out of it - NewFeature): don't infer
        // them from their children.
        if (inIni[i] || t.type != Bool || std::strcmp(t.section, "Experimental") == 0 || NewFeature(t.name)) continue;
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
    // Once: the always-on switches back on - turned off while they were in the window, they would now leave their
    // features off with no way back from it.
    if (GetPrivateProfileIntA("Migrations", "AlwaysOn", 0, g_iniPath) == 0) {
        for (Setting& s : g_settings)
            if (AlwaysOn(s.name) && s.value == 0.0f) {
                s.value = 1.0f;
                RvkLog("settings: %s was off - on again (no longer in the settings window)", s.name);
            }
        WritePrivateProfileStringA("Migrations", "AlwaysOn", "1", g_iniPath);
    }
    for (Setting& s : g_settings) {
        s.value = Clamp(s, s.value);
        Save(s);                                  // the ini always lists every setting
    }
    g_iniWritten = IniWriteTime();
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
    if (!s || !std::isfinite(value)) return;    // (a broken slider sent NaN every frame - user, 2026-10-14)
    float v = Clamp(*s, value);
    if (v == s->value) return;
    s->value = v;
    Save(*s);
    Apply(*s, g_rvk.device);
    RvkLog("setting %s = %g", s->name, s->value);
}

// An edit made to randy-vk.ini while the game runs: checked twice a second (its write time), and every setting whose
// value there differs from the one in effect is applied - as from the settings window, but not written back (the
// file already says it). A setting missing from the file or not a number (a save half done) is left as it is.
void PollIni(rvk::ThreadedDevice* device)
{
    Load();
    static ULONGLONG next;
    const ULONGLONG now = GetTickCount64();
    if (now < next)
        return;
    next = now + 500;
    const FILETIME written = IniWriteTime();
    if (CompareFileTime(&written, &g_iniWritten) == 0)
        return;
    g_iniWritten = written;
    std::vector<const Setting*> changed;
    for (Setting& s : g_settings) {
        char buf[64] = "";
        GetPrivateProfileStringA("Renderer", s.name, "", buf, sizeof(buf), g_iniPath);
        float read = 0.0f;
        if (!buf[0] || !ParseIni(s, buf, &read))
            continue;
        const float v = Clamp(s, read);
        if (v == s.value)
            continue;
        s.value = v;
        RvkLog("setting %s = %g (randy-vk.ini edited)", s.name, s.value);
        changed.push_back(&s);
    }
    // Applied once all are read (a device call takes a setting with its partners, some of which may have changed
    // too); a feature's on / off setting with the settings it switches.
    if (device)
        for (const Setting* s : changed)
            Apply(*s, device);
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

// The built-in neon night table (RVK_NeonNight), over the graded colour by night: the darks lean teal, saturated
// colours above the darks a little more saturated (signs, screens, lamps), a touch of magenta in the upper middle
// tones. Light: the darks stay as dark, white stays white.
void NeonNightLut(uint32_t n, std::vector<uint8_t>* out)
{
    out->resize(size_t(n) * n * n * 4);
    auto smooth = [](float a, float b, float x) {
        float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    };
    for (uint32_t b = 0; b < n; ++b)
        for (uint32_t g = 0; g < n; ++g)
            for (uint32_t r = 0; r < n; ++r) {
                float c[3] = {r / float(n - 1), g / float(n - 1), b / float(n - 1)};
                float l = 0.3f * c[0] + 0.59f * c[1] + 0.11f * c[2];
                float hi = std::max(c[0], std::max(c[1], c[2])), lo = std::min(c[0], std::min(c[1], c[2]));
                // Darks towards teal, as bright as they were.
                const float teal[3] = {0.72f, 1.04f, 1.2f};   // (luma 0.98)
                float dark = 1.0f - smooth(0.0f, 0.4f, l);
                for (int k = 0; k < 3; ++k) c[k] += (l * teal[k] - c[k]) * 0.45f * dark;
                // Saturated colours above the darks more so.
                float boost = 0.3f * smooth(0.1f, 0.5f, hi - lo) * smooth(0.15f, 0.5f, l);
                for (int k = 0; k < 3; ++k) c[k] = l + (c[k] - l) * (1.0f + boost);
                // Magenta in the upper middle tones.
                float mid = smooth(0.35f, 0.6f, l) * (1.0f - smooth(0.75f, 1.0f, l));
                const float magenta[3] = {0.035f, -0.03f, 0.04f};
                for (int k = 0; k < 3; ++k) c[k] += magenta[k] * mid;
                uint8_t* p = &(*out)[((size_t(b) * n + g) * n + r) * 4];
                for (int k = 0; k < 3; ++k) p[k] = uint8_t(std::lround(std::clamp(c[k], 0.0f, 1.0f) * 255.0f));
                p[3] = 255;
            }
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
    if (Find("RVK_NeonNight") && Find("RVK_NeonNight")->value != 0.0f) {
        size[1] = 33;
        NeonNightLut(size[1], &data[1]);
        RvkLog("colour lookup table for the night: neon (RVK_NeonNight)");
    }
    for (uint32_t i = 0; i < 2; ++i)
        device->SetColorLut(i, size[i], size[i] ? data[i].data() : nullptr);
    // The static meshes' ids (ao-assets: python -m aoassets mesh-ids), from the same folder.
    device->LoadMeshIds((dir + "randy-vk\\mesh-ids.bin").c_str());   // (with the material maps: ao-assets writes it there)
    // Which ground gets pebbles, by hand (RVK_Pebbles; optional): read when a playfield's ground is classified.
    device->SetPebbleTablePath((dir + "randy-vk\\terrain\\pebbles.tsv").c_str());
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

// ---- Presets ----
// Original: every enhancement off; Classic+: the game's look, lit and shadowed better; Modern: the defaults (the
// user's own setup, 2026-10-20, with render scale 100 and neither upscaler nor frame generation); Ultra: that look
// with its quality up. A preset holds every setting except the General ones (performance switches a preset shouldn't
// change), with RVK_Enhance on; Original is RVK_Enhance off alone (the stored values stay for switching back). Players
// keep a setup of their own by backing up randy-vk.ini.
namespace {

struct PresetValue { const char* name; float value; };
const PresetValue kClassicPlus[] = {
    {"RVK_Bump", 0.75f}, {"RVK_Tess", 0.5f}, {"RVK_GrassOn", 0}, {"RVK_LeafOn", 0}, {"RVK_WaterStyle", 0}, {"RVK_WaterQual", 2},
    {"RVK_SunStrength", 0.5f}, {"RVK_PtShadows", 4}, {"RVK_Contact", 0.4f}, {"RVK_Bloom", 0.5f},
    {"RVK_NightGlow", 0.35f}, {"RVK_BloomWide", 0.2f}, {"RVK_NeonSat", 0.3f}, {"RVK_Ao", 0.75f}, {"RVK_GiOn", 0}, {"RVK_VolOn", 0}, {"RVK_SsrGloss", 0.15f},
    {"RVK_MBlurOn", 0}, {"RVK_Warmth", 0}, {"RVK_Dof", 0}, {"RVK_Particles", 0},
    {"RVK_CloudsOn", 0}, {"RVK_WeatherOn", 0}, {"RVK_SmokeOn", 0}, {"RVK_WaterOn", 0},   // (the new features off)
};
const PresetValue kUltra[] = {   // (the user's own setup, 2026-10-16: Modern's look with its quality up)
    {"RVK_GrassDist", 80}, {"RVK_GrassBlades", 12}, {"RVK_WaterQual", 3}, {"RVK_PtShadows", 16}, {"RVK_Ao", 2},
    {"RVK_CloudQual", 3}, {"RVK_CloudSteps", 64}, {"RVK_SmokeQual", 3}, {"RVK_SmokeRes", 20},
};
const char* const kPresetNames[] = {"Original", "Classic+", "Modern", "Ultra"};
constexpr uint32_t kPresetCount = 4;

bool InPresets(const Setting& s)
{
    if (std::strcmp(s.section, "Debug") == 0) return false;   // (debug switches: randy-vk.ini only)
    if (s.type == Colour) return false;            // (the sky's colours: their own presets - console skypreset)
    return std::strcmp(s.section, "General") != 0 || std::strcmp(s.name, "RVK_Enhance") == 0;
}

// Preset i's value of every setting (g_settings order; only InPresets ones count).
void PresetValues(uint32_t i, std::vector<float>& out)
{
    out.resize(kCount);
    for (size_t k = 0; k < kCount; ++k)
        out[k] = g_settings[k].defaultValue;
    auto over = [&](const PresetValue* list, size_t n) {
        for (size_t j = 0; j < n; ++j)
            for (size_t k = 0; k < kCount; ++k)
                if (std::strcmp(g_settings[k].name, list[j].name) == 0) out[k] = list[j].value;
    };
    if (i == 1) over(kClassicPlus, sizeof(kClassicPlus) / sizeof(kClassicPlus[0]));
    else if (i == 3) over(kUltra, sizeof(kUltra) / sizeof(kUltra[0]));
    for (size_t k = 0; k < kCount; ++k)
        if (std::strcmp(g_settings[k].name, "RVK_Enhance") == 0) out[k] = i == 0 ? 0.0f : 1.0f;
}

}  // namespace

namespace rvk_settings {

int PresetCurrent()
{
    Load();
    if (Stored("RVK_Enhance") == 0.0f) return 0;
    std::vector<float> v;
    for (uint32_t i = 1; i < kPresetCount; ++i) {
        PresetValues(i, v);
        bool same = true;
        for (size_t k = 0; k < kCount && same; ++k)
            if (InPresets(g_settings[k]) && std::fabs(v[k] - g_settings[k].value) > 1e-4f) same = false;
        if (same) return int(i);
    }
    return -1;
}

bool PresetApply(uint32_t i)
{
    Load();
    if (i == 0) {
        Set("RVK_Enhance", 0.0f);
        return true;
    }
    std::vector<float> v;
    if (i >= kPresetCount) return false;
    PresetValues(i, v);
    for (size_t k = 0; k < kCount; ++k)
        if (InPresets(g_settings[k])) Set(g_settings[k].name, v[k]);
    RvkLog("preset %s applied", kPresetNames[i]);
    return true;
}

// ---- The sky's colour presets ----
// earth: the physical sky's own colours (the keys' references); rubika: teal days, violet dusks, green-black nights;
// alien: magenta dusks, a cyan blue hour, violet nights; noir: the colour drained, the nights deep.
namespace {
const char* const kSkyKeyNames[rvk::kSkyKeyCount] = {
    "RVK_SkyDayTop", "RVK_SkyDayLow", "RVK_SkyDuskSun", "RVK_SkyDuskAway", "RVK_SkyDuskTop", "RVK_SkyBlueTop",
    "RVK_SkyBlueLow", "RVK_SkyNightTop", "RVK_SkyNightLow"};
struct SkyPreset { const char* name; uint32_t keys[rvk::kSkyKeyCount]; };
const SkyPreset kSkyPresets[] = {
    {"earth", {rvk::kSkyKeyRef[0], rvk::kSkyKeyRef[1], rvk::kSkyKeyRef[2], rvk::kSkyKeyRef[3], rvk::kSkyKeyRef[4],
               rvk::kSkyKeyRef[5], rvk::kSkyKeyRef[6], rvk::kSkyKeyRef[7], rvk::kSkyKeyRef[8]}},
    {"rubika", {0x2E6E78, 0x8CC8C0, 0xE07850, 0x8A5A8C, 0x4A3C6E, 0x1E2A4A, 0x4A4A6A, 0x0E1A1A, 0x1C2A28}},
    {"alien", {0x5A4A8C, 0xB4A0C8, 0xF05A8C, 0x6A3C8C, 0x3C2864, 0x104050, 0x307880, 0x140A20, 0x28142E}},
    {"noir", {0x50555C, 0xA0A2A4, 0xB49682, 0x6E6670, 0x3C3C46, 0x1E2228, 0x4A4C50, 0x08090B, 0x121315}},
};
constexpr uint32_t kSkyPresetCount = sizeof(kSkyPresets) / sizeof(kSkyPresets[0]);
}  // namespace

uint32_t SkyPresetCount() { return kSkyPresetCount; }
const char* SkyPresetName(uint32_t i) { return i < kSkyPresetCount ? kSkyPresets[i].name : ""; }

bool SkyPresetApply(uint32_t i)
{
    Load();
    if (i >= kSkyPresetCount) return false;
    for (uint32_t k = 0; k < rvk::kSkyKeyCount; ++k) Set(kSkyKeyNames[k], float(kSkyPresets[i].keys[k]));
    RvkLog("sky preset %s applied", kSkyPresets[i].name);
    return true;
}

int SkyPresetCurrent()
{
    Load();
    for (uint32_t i = 0; i < kSkyPresetCount; ++i) {
        bool same = true;
        for (uint32_t k = 0; k < rvk::kSkyKeyCount && same; ++k)
            same = uint32_t(Stored(kSkyKeyNames[k])) == kSkyPresets[i].keys[k];
        if (same) return int(i);
    }
    return -1;
}

uint32_t VanillaCount() { return uint32_t(sizeof(kVanilla) / sizeof(kVanilla[0])); }
const char* VanillaName(uint32_t i) { return i < VanillaCount() ? kVanilla[i].name : ""; }
float VanillaValue(uint32_t i) { return i < VanillaCount() ? kVanilla[i].value : 0.0f; }

void ApplyTemporarySet(const std::vector<std::pair<std::string, float>>& values)
{
    Load();
    if (!g_rvk.device) return;
    std::vector<std::pair<Setting*, float>> stored;
    for (const auto& [name, value] : values)
        if (Setting* s = Find(name.c_str())) {
            stored.push_back({s, s->value});
            s->value = value;
        }
    for (const auto& [s, v] : stored) Apply(*s, g_rvk.device);
    for (const auto& [s, v] : stored) s->value = v;
}

void RestoreAll()
{
    Load();
    if (g_rvk.device) ApplyAll(g_rvk.device);
}

uint32_t PresetCount() { return kPresetCount; }
const char* PresetName(uint32_t i) { return i < kPresetCount ? kPresetNames[i] : ""; }

uint32_t Count()
{
    Load();
    return uint32_t(kCount);
}

bool Info(uint32_t index, RvkSettingInfo* out, bool* hidden)
{
    Load();
    if (index >= kCount || !out)
        return false;
    const Setting& s = g_settings[index];
    out->size = sizeof(RvkSettingInfo);
    out->name = s.name;
    out->label = s.label;
    out->section = s.section;
    out->type = s.type;
    out->min = s.min;
    out->max = s.max;
    out->step = s.step;
    out->value = s.value;
    out->defaultValue = s.defaultValue;
    out->parent = AlwaysOn(s.parent) ? nullptr : s.parent;
    out->choices = s.choices;
    out->cost = CostOf(s.name);
    out->choiceNames = ChoiceNamesOf(s.name);
    if (hidden) *hidden = Hidden(s);
    return true;
}


}  // namespace rvk_settings

extern "C" {

uint32_t RvkSettings_Version() { return 2; }

// The settings the Renderer tab shows: all but the hidden ones (the "Debug" section's and kHidden: randy-vk.ini only).
static const std::vector<uint32_t>& Shown()
{
    static const std::vector<uint32_t> shown = [] {
        std::vector<uint32_t> v;
        for (uint32_t i = 0; i < kCount; ++i)
            if (!Hidden(g_settings[i])) v.push_back(i);
        return v;
    }();
    return shown;
}

uint32_t RvkSettings_Count()
{
    rvk_settings::Load();
    return uint32_t(Shown().size());
}

int RvkSettings_Get(uint32_t index, RvkSettingInfo* out)
{
    rvk_settings::Load();
    if (index >= Shown().size() || !out || out->size < offsetof(RvkSettingInfo, parent))   // version 1: no parent
        return 0;
    const Setting& s = g_settings[Shown()[index]];
    out->name = s.name;
    out->label = s.label;
    out->section = s.section;
    out->type = s.type;
    out->min = s.min;
    out->max = s.max;
    out->step = s.step;
    out->value = s.value;
    out->defaultValue = s.defaultValue;
    if (out->size >= offsetof(RvkSettingInfo, cost)) {
        out->parent = AlwaysOn(s.parent) ? nullptr : s.parent;   // (grouped by their section instead)
        out->choices = s.choices;
    }
    if (out->size >= sizeof(RvkSettingInfo)) {
        out->cost = CostOf(s.name);
        out->choiceNames = ChoiceNamesOf(s.name);
    }
    return 1;
}

// Presets (version 2 callers may use them when exported): index 0 Original, 1 Classic+, 2 Modern, 3 Ultra. Current:
// the preset the settings match, -1 custom.
uint32_t RvkPresets_Count() { return kPresetCount; }

const char* RvkPresets_Name(uint32_t index) { return index < kPresetCount ? kPresetNames[index] : ""; }

int RvkPresets_Apply(uint32_t index) { return rvk_settings::PresetApply(index) ? 1 : 0; }
int RvkPresets_Current() { return rvk_settings::PresetCurrent(); }

int RvkSettings_Set(const char* name, float value)
{
    if (!name || !Find(name))
        return 0;
    rvk_settings::Set(name, value);
    return 1;
}

}  // extern "C"
