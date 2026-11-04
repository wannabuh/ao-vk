// rvk: a Vulkan renderer with a Direct3D 7 shaped front end (the part of D3D7 Anarchy Online uses).
//
// Usage per frame: BeginFrame(), state and draw calls, EndFrame(). Rendering goes into the main colour
// target (or one set with SetRenderTarget) plus a shared depth buffer; EndFrame() copies the main target
// to the window's swapchain (if a window was given) and can save it as a screenshot.
#pragma once

#include "d3d7.h"
#include "fx_def.h"
#include "skin.h"
#include "vk.h"
#include "water_surface.h"

#include <array>
#include <memory>
#include <mutex>
#include <cstring>
#include <cstdint>
#include <string>
#include <atomic>
#include <chrono>
#include <future>
#include <algorithm>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct VmaAllocator_T;
struct VmaAllocation_T;
struct VmaVirtualBlock_T;

namespace rvk::portal { struct Capture; }
namespace rvk::detail { struct FrameLights; struct FvfLayout; struct DrawConstants; struct DrawTransform; struct ShadowRecord; struct Cone; }

namespace rvk {

// What the game's visual issuing a draw is (Device::SetDrawVisual; same values as rnative::scene::Kind).
enum class VisualKind : uint32_t {
    Unknown = 0, Character, CharacterPart, Static, Terrain, Room, Water, Sky, BlobShadow, Effect, Other,
};

class Device;
class Texture;

// Our own sky's colours (docs/sky-plan.md §1; RVK_Sky* colour settings, RRGGBB): nine keys - by day, at dusk, in the
// blue hour and at night, overhead and low (at dusk low towards the sun and away from it). Each recolours the
// physical sky as the ratio of its colour to its reference here - what the physical sky shows there with the default
// settings - so at the references the sky is as the physics gives it.
enum SkyKey : uint32_t {
    kSkyDayTop, kSkyDayLow, kSkyDuskSun, kSkyDuskAway, kSkyDuskTop, kSkyBlueTop, kSkyBlueLow, kSkyNightTop,
    kSkyNightLow, kSkyKeyCount
};
// (Measured in the demo - the lake, a sky dome of 8FB0CC, the sun held at 30, -1, -9 and -18 degrees: the "sky: keys"
// log, 2026-10-19.)
inline constexpr uint32_t kSkyKeyRef[kSkyKeyCount] = {
    0x404F71, 0x8EA4B6, 0xDB8862, 0x79475D, 0x424361, 0x222A3C, 0x535455, 0x171B25, 0x25292E,
};
// Our own night sky (docs/sky-plan.md §2-4; drawn by the sky pass under RVK_ProcSky).
struct NightSky {
    bool stars = true;                     // RVK_Stars: ours in place of the game's points
    float bright = 1.0f;                   // RVK_StarBright, x
    float twinkle = 0.5f;                  // RVK_Twinkle: 0 steady .. 1
    float galaxy = 1.0f;                   // RVK_Galaxy: the band of the galaxy, x (0 none)
    float meteors = 1.0f;                  // RVK_Meteors: shooting stars, how often x (0 none)
    float aurora = 1.0f;                   // RVK_Aurora: how often and how strong x (0 none)
    uint32_t auroraColour = 0x40FF90;      // RVK_AuroraCol: its green, RRGGBB
    float auroraForce = -1.0f;             // RVK_AuroraForce: an aurora of this strength every night (< 0: by chance)
};
struct SkyColours {
    uint32_t keys[kSkyKeyCount] = {kSkyKeyRef[0], kSkyKeyRef[1], kSkyKeyRef[2], kSkyKeyRef[3], kSkyKeyRef[4],
                                   kSkyKeyRef[5], kSkyKeyRef[6], kSkyKeyRef[7], kSkyKeyRef[8]};
};

// A playfield's ground, for rvk to draw itself (native_ground.cpp, docs/native-ground-plan.md): read from the game's
// AnarchyGroundData_t (proxy rvk_ground.cpp; the format: docs/ground-format.md).
struct NativeGround {
    uint32_t id = 0;                       // (for the log)
    uint32_t cellsX = 0, cellsZ = 0;       // cells across (world x) and down (world z)
    uint32_t patch = 64;                   // cells a patch side (drawn a patch at a time)
    float cellX = 4.0f, cellZ = 4.0f;      // world units a cell
    std::vector<float> heights;            // (cellsX + 1) x (cellsZ + 1) world heights, rows along +z
    std::vector<uint16_t> codes;           // cellsX x cellsZ: the cell's tile (low 14 bits) and turn (top 2)
    std::vector<Texture*> tiles;           // each tile's texture (null: none)
    std::vector<uint16_t> types;           // each tile's surface type (tile_type_data; empty: not known - no pebbles)
};

// How a ground chunk's base texture lies on the world (terrain.cpp FitTerrain). The game gives each 4 x 4 unit cell of
// its near ground its own copy of the tile (coordinates 0..1 over the cell, turned or flipped one of eight ways - its
// own vertices), laid from above; its far ground is one plane, a map of the zone. So each triangle's own mapping is
// solved (its three vertices: set 0 from world x, z - every triangle of a heightmap stands over a square of the grid,
// steep or not): the repeats per world unit, and how many triangles are that many repeats of the tile turned or
// flipped. And, for the frame dump, the whole chunk fitted as one plane (least squares).
struct TerrainFit {
    float coef[4][4] = {};         // the plane: u0, v0, u1, v1 = x, y, z, 1 times these
    float rms[4] = {};             // ... its error (texture units)
    float normalY[2] = {};         // the normals' lowest and highest y (normalised)
    float normalLength[2] = {};    // ... their shortest and longest
    uint32_t vertices = 0;         // how many went into the plane's fit (0: none, or all in a line)
    float cellScale = 0.0f;        // the triangles' repeats per world unit (their median)
    float cellShare = 0.0f;        // ... the share of triangles laid from above at that, turned or flipped
    uint32_t triangles = 0;        // how many triangles were solved
    bool tiled = false;            // nine in ten are, at most 64 units a repeat (a tile, not the far map): the faces
                                   // of cliffs and the hexagons can be laid at its scale
};

// Diagnostics. Default: stderr, flushed per line. SetLogSink redirects (e.g. into the game's log file).
void Log(const char* fmt, ...);
void SetLogSink(void (*sink)(const char* line));
// The process's address space from its regions (VirtualQuery), bytes: free, the largest free block (a 32-bit game runs
// out of one big enough for a zone's load before the free total reaches nothing), and the whole. Under Wine,
// GlobalMemoryStatusEx's free figure is off by as much as a gigabyte either way; the regions are what can be reserved
// (crash_check --addr).
struct AddressSpace { uint64_t free = 0, largest = 0, total = 0; };
AddressSpace QueryAddressSpace();
// The same, walked once a second on a thread of its own (started at the first call; all zero until its first walk):
// a game's thousands of regions take ~9 ms to walk, too long for its thread four times a second (the perf readout).
AddressSpace LatestAddressSpace();
// Where the pipeline cache is kept between runs (compiled pipelines load from it instead of being built again at every
// start). Empty or never set: RANDYVK_PIPELINE_CACHE, else no cache file ("off" also disables it).
void SetPipelineCachePath(const char* path);
// Where captured interiors go (shop portals, portal_capture.cpp): <client>\randy-vk\portals. Unset: none captured.
void SetPortalDir(const char* dir);
const std::string& PortalDir();                  // (with a trailing backslash; empty: none)

// Texture formats (the D3DX 7 / DirectDraw pixel formats the client can create).
enum class Format : uint32_t {
    A8R8G8B8, X8R8G8B8, R5G6B5, A1R5G5B5, X1R5G5B5, A4R4G4B4, L8, A8, A8L8,
    DXT1, DXT2, DXT3, DXT4, DXT5,
    RGBA16F,    // internal: the HDR scene target
    RG11B10F,   // internal: the HDR glow target (additive effects, for the bloom)
    RG16F,      // internal: ambient occlusion (factor, view depth)
    RGBA8,      // internal: the HDR scene's local-light fraction (AO spares it), reflectivity, direct sun share
    Count
};

// Bytes for one row of 4x4 blocks (compressed) or of pixels, and rows of blocks/pixels, for a level.
uint32_t FormatRowBytes(Format format, uint32_t width);
uint32_t FormatRows(Format format, uint32_t height);
bool FormatIsCompressed(Format format);

class Texture {
public:
    uint32_t Width() const { return m_width; }
    uint32_t Height() const { return m_height; }
    uint32_t Levels() const { return m_levels; }
    Format GetFormat() const { return m_format; }
    bool IsRenderTarget() const { return m_renderTarget; }
    // The texture's colour at (u, v), RGB 0..255, from the small grid taken at upload (resources.cpp
    // PixelsThumbnail); false until a full level-0 upload has been sampled.
    bool TexelRgbAt(float u, float v, uint8_t rgb[3]) const;
    bool TexelRgbBilinear(float u, float v, uint8_t rgb[3]) const;   // ... filtered (the lightmaps)
    // (A normal map, before the device is handed it: its alpha is a height map's - rvk/material_maps.h.)
    void SetAlphaHeight(bool height) { m_alphaHeight = height; }

private:
    friend class Device;
    VkImage m_image = VK_NULL_HANDLE;
    VkImageView m_view = VK_NULL_HANDLE;
    VmaAllocation_T* m_allocation = nullptr;
    uint32_t m_width = 0, m_height = 0, m_levels = 1;
    Format m_format = Format::A8R8G8B8;
    bool m_renderTarget = false;
    bool m_storage = false;            // also a storage image (CreateImage: FSR 2's output, fsr2.cpp)
    bool m_opaque = false;             // the texture's pixels are all alpha 1 (a blended static draw can be opaque)
    // Up to 64 x 64, from level-0 uploads: every texel black - with m_opaque, the PF map's tiles in AI missions
    // (RVK_AlienMap).
    bool m_blackRgb = false;
    // A small RGB copy of level 0 for the ground grass (PixelsThumbnail): textures up to 64 x 64 (the terrain's
    // lightmaps) whole, bigger ones on a 16 x 16 grid (enough to classify a ground atlas' tiles).
    std::vector<uint8_t> m_thumb;
    uint32_t m_thumbW = 0, m_thumbH = 0;
    bool m_thumbValid = false;
    float m_mean[3] = {-1.0f, -1.0f, -1.0f};   // its mean colour (0..1; from the grid above, -1 until then): the cliffs'
                                               // rock tinted to a ground cell's tile (terrain.cpp)
    // How it looks up close, for the native ground's pebbles (ground_pebbles.cpp; square, 128 texels or more, else -1):
    // line - how far its rows' or columns' mean brightness strays (a flagstone's joints, a slab's edge: 0.3 and up; a
    // natural ground under 0.25), edge - the biggest step between neighbouring rows' or columns' means (both over its
    // spread of brightness); grain - the mean brightness step two texels apart (0..255: smooth dirt 9, gravel 30).
    float m_line = -1.0f, m_edge = -1.0f, m_grain = -1.0f;
    // Not fully opaque: the mean alpha (0..255) on a grid of up to 64 x 64 cells over level 0, for the leaves
    // (leaves.cpp: where a canopy card is leaf and where it is hole). Empty until a full level-0 upload.
    std::vector<uint8_t> m_alphaMask;
    uint32_t m_alphaW = 0, m_alphaH = 0;
    // The share of neighbouring mask cells whose alpha jumps by more than 0.3: a cut-out's hard edges (leaves: from
    // ~0.004 up), none in a soft glow, beam or smoke card (0). Plants are cut-outs (leaves.cpp, SwayParams).
    float m_alphaHardEdges = 1.0f;
    // Of the mask's cells that aren't holes (alpha over 0.05), the share partly see-through (under 0.9): with few hard
    // edges, a surface translucent all over (the alien ships' door membranes, glass, a force field), not dithered far
    // off (RVK_FolDither: a screen door over a whole membrane). Leaves on a coarse grid read soft too - their hard
    // edges tell them apart. 0 without a mask (opaque).
    float m_alphaSoftShare = 0.0f;
    uint32_t m_alphaVersion = 0;       // counts the uploads that changed the mask (a canopy's leaves are rebaked)
    VkDeviceSize m_bytes = 0;          // its image's memory (the GPU memory report)
    bool m_lightmap = false;           // captured as a terrain lightmap (a re-upload recaptures the grass's light)
    bool m_groundBase = false;         // captured as a terrain base texture (a re-upload recaptures the grass's ground)
    Texture* m_normalMap = nullptr;    // tangent-space normal map drawn with this texture (owned; SetNormalMap)
    bool m_alphaHeight = false;        // (a normal map) its alpha is a height map's height: drawn with parallax
    Texture* m_ormMap = nullptr;       // PBR material: R occlusion, G roughness, B metallic (owned; SetMaterialMaps)
    Texture* m_albedoMap = nullptr;    // drawn instead of this texture's own pixels (owned; SetMaterialMaps)
    Texture* m_emissiveMap = nullptr;  // light the surface gives off, added to it (owned; SetEmissiveMap)
    // A tree canopy's own leaf art (leaves.cpp; owned; SetLeafArt): an atlas of 4 x 4 sprigs (twig base at each cell's
    // bottom middle) drawn in place of this canopy texture's cards, and its normal map; m_leafArtVersion counts changes.
    Texture* m_leafArt = nullptr;
    Texture* m_leafArtNormal = nullptr;
    uint32_t m_leafArtVersion = 0;
    uint32_t m_rdbId = 0;              // the game's RDB texture it holds (SetTextureRdbId; 0: not known)
    uint32_t m_bindless = ~0u;         // its slot in the bindless image array (M1)
    // (Frame dump: its uploads, the frame of the last, and the non-zero bytes of the last whole level 0 - 256 x 256 or
    // smaller; ~0u: not counted.)
    uint32_t m_uploads = 0, m_level0NonZero = ~0u;
    uint64_t m_uploadFrame = 0;
    // Layout as of the end of the commands recorded so far (main command buffer for render targets;
    // plain textures only change layout in the upload command buffer, which runs first).
    VkImageLayout m_layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

// D3D7-style vertex buffer (IDirect3DVertexBuffer7). Contents live in CPU memory; each draw copies the
// vertices it uses, so rewriting the buffer between draws (as the game's CPU skinning does) is safe.
class VertexBuffer {
public:
    uint32_t Fvf() const { return m_fvf; }
    uint32_t Stride() const { return m_stride; }
    uint32_t Count() const { return m_count; }

private:
    friend class Device;
    friend class ThreadedDevice;
    uint32_t m_fvf = 0, m_stride = 0, m_count = 0;
    std::vector<uint8_t> m_data;
    bool m_locked = false;
};

struct DeviceInfo {
    std::string gpu;
    std::string driver;
    uint32_t apiVersion = 0;
};

// The renderer's plain settings (knobs.cpp; docs/audit-2026-10.md §5): each one value a feature reads as it is, set
// through one path - Device::SetKnob, ThreadedDevice::SetKnob and, from the settings table by name, KnobByName - in
// place of a setter, a forwarding method and an apply branch each. A new one is a line here: its setting (RVK_ and
// this name), the member it sets, and what a change must refresh (C the draws' constants, L the frame lights, G the
// ground grass's tiles).
#define RVK_FLOAT_KNOBS(X) \
    X(BloomOcc, m_bloomOverNearer, 0)                 /* the bloom over objects in front of its light (1 unchanged) */ \
    X(BloomWide, m_bloomWide, 0)                      /* the bloom's wide haze round its core (0 even) */ \
    X(NeonSat, m_neonSat, L)                          /* coloured light keeps its colour (0: to white) */ \
    X(Sway, m_sway, C|L)                              /* wind in small plants (0 still) */ \
    X(GrassPush, m_grassPush, L)                      /* plants bending from characters (0 off) */ \
    X(PlantDetail, m_plantDetail, 0)                  /* plants' big quads split for smooth bending (0 off) */ \
    X(FoliageLod, m_foliageLod, 0)                    /* foliage shaded cheaper past this (units; 0 off) */ \
    X(FolDither, m_foliageDither, 0)                  /* plants' soft edges dithered past this (units; 0 off) */ \
    X(GrassWidth, m_grassWidth, G)                    /* the grass blades' width, x (baked: rebuilds the tiles) */ \
    X(GrassBright, m_grassBright, 0)                  /* the grass against its ground (1 as bright) */ \
    X(WindStorm, m_windStormScale, 0)                 /* a storm's wind, x (wind.cpp) */ \
    X(Lightning, m_lightning, 0)                      /* our lightning's flash, x (rain.cpp) */ \
    X(RainFog, m_rainFog, 0)                          /* the rain's mist, x */ \
    X(StormGreen, m_stormGreen, 0)                    /* a thunderstorm's green cast (0..1) */ \
    X(StormThick, m_stormThick, 0)                    /* storm clouds' thickness (0.25..2) */ \
    X(RainVeils, m_rainVeils, 0)                      /* rain veils under the clouds, x */ \
    X(StormDark, m_stormDark, 0)                      /* a storm hiding the sun from the land, x */ \
    X(RainWet, m_rainWet, 0)                          /* the ground soaked by rain, x */ \
    X(StormBolt, m_stormBolt, 0)                      /* our bolts, x (0: the game's) */ \
    X(StormDebris, m_stormDebris, 0)                  /* a storm's blown debris, x */ \
    X(SandHaze, m_sandHaze, 0)                        /* a sandstorm's haze, x */ \
    X(SandBlow, m_sandBlow, 0)                        /* the blowing sand's volume, x */ \
    X(SandContrast, m_sandContrast, 0)                /* the blowing sand's contrast */ \
    X(SandClear, m_sandClear, 0)                      /* clear air round the camera in sand (units) */ \
    X(SandGusts, m_sandGusts, 0)                      /* a sandstorm's distant gusts, x */ \
    X(SandFog, m_sandFog, 0)                          /* a sandstorm's air as a lit volume (0: a flat haze) */ \
    X(SandSun, m_sandSun, 0)                          /* the sun through a sandstorm, x */ \
    X(SandTint, m_sandTint, 0)                        /* the land tinted by a sandstorm's light, x */ \
    X(SunSoft, m_sunSoftness, L)                      /* sun shadows' softening with distance (0 hard) */ \
    X(CrownShade, m_crownShade, L)                    /* tree crowns lit as one rounded mass (0..1) */ \
    X(Metering, m_metering, 0)                        /* the camera's metering: how far a bright view takes the exposure down (0 off) */ \
    X(CrownSky, m_crownSky, L)                        /* crowns' ambient as the sky's: from above (0..1) */ \
    X(CrownDepth, m_crownDepth, L)                    /* sunlight through crowns by their depth (0..1) */ \
    X(NightGlow, m_nightGlow, C|L)                    /* windows, signs and screens glowing at night, x */ \
    X(MoonLight, m_moonLight, C|L)                    /* the moon's light, x (night.cpp) */ \
    X(Mist, m_mist, 0)                                /* valley mist, x (mist.cpp) */ \
    X(MornHaze, m_mornHaze, 0)                        /* the haze round sunrise, x (half round sunset) */ \
    X(DawnLight, m_dawnLight, 0)                      /* a low sun's warm light, cool shade (0: the game's) */ \
    X(Afterglow, m_afterglow, 0)                      /* the clouds' sunset colours, x */ \
    X(DuskRays, m_duskRays, 0)                        /* sunset rays, x */ \
    X(GoldenHour, m_goldenHour, 0)                    /* the golden hour's grade (0 none) */ \
    X(SunSize, m_sunSize, 0)                          /* the suns' size, x the game's sprites */ \
    X(SunBleed, m_sunBleed, 0)                        /* the suns' glow through thick cloud (0..1) */ \
    X(Contact, m_contact, C)                          /* contact shadows' strength */ \
    X(Headroom, m_lightHeadroom, C|L)                 /* how bright local lights may make a surface (1..2) */ \
    X(SsrMode, m_ssrMode, 0)                          /* whose reflections: 0 ours, 1 AMD's (sssr.cpp) */ \
    X(SssrThick, m_sssrThick, 0)                      /* ... how far behind the depth buffer a hit counts (units) */ \
    X(AoMode, m_aoMode, 0)                            /* ambient occlusion: 0 ours, 1 AMD's (cacao.cpp), 2 traced */ \
    X(Vrs, m_vrsMode, 0)                              /* variable rate shading of the scene (vrs.cpp; 0 off) */ \
    X(VrsCutoff, m_vrsCutoff, 0)                      /* ... the luminance variance a coarser rate allows */ \
    X(GiMode, m_giMode, 0)                            /* whose indirect light: 0 ours, 1 AMD's (brixelizer.cpp) */ \
    X(GiDebug, m_giDebug, 0)                          /* ... a view: 1 its distance field, 2 its light alone */ \
    X(GiSky, m_giSky, 0)                              /* ... the sky's light on rays that miss (0: bounce only) */ \
    X(RtDebug, m_rtDebug, 0)                          /* ray tracing's view: 1 what its rays hit (raytrace.cpp) */ \
    X(RtShadow, m_rtShadow, 0)                        /* the sun's shadows ray traced (raytrace.cpp; 0 off) */ \
    X(RtReflect, m_rtReflect, 0)                      /* reflections the screen lacks ray traced (raytrace.cpp) */ \
    X(VolLamps, m_volLamps, 0)                        /* the lamps' light in the air (volume.frag; 0 none) */ \
    X(ZoneHoldMax, m_zoneHoldMax, 0)                  /* the longest a zone change hides the new zone once it draws (s) */ \
    X(PortalDist, m_portalDist, 0)                    /* shops' rooms drawn through their doors within this (units) */ \
    X(PortalDim, m_portalDim, 0)                      /* ... their brightness by day (0..1 of their own) */
#define RVK_BOOL_KNOBS(X) \
    X(Prepass, m_prepassOn, 0)                        /* the depth pre-pass */ \
    X(LiteShader, m_liteShader, 0)                    /* the interface's draws through the lean shader */ \
    X(PrepassEq, m_prepassEqual, 0)                   /* pre-passed draws shaded only where their depth is */ \
    X(PrepassMore, m_prepassMore, 0)                  /* a second pre-pass segment after the water's copy */ \
    X(FolEdges, m_foliageEdges, 0)                    /* plants' soft edges in a second pass */ \
    X(OwnLight, m_carrierLit, C)                      /* a character's own light lights the character */ \
    X(OwnShadow, m_carrierCasts, 0)                   /* a character casts a shadow in its own light */ \
    X(Fxaa, m_fxaa, 0)                                /* FXAA while TAA is off */ \
    X(StormTint, m_stormTint, 0)                      /* the game's storm sheet drawn */ \
    X(StormFlash, m_stormFlash, 0)                    /* the game's lightning glow drawn */ \
    X(SandProc, m_sandProcedural, 0)                  /* procedural blowing sand */ \
    X(Atmos, m_atmos, 0)                              /* the air's scattering (off: the plain land haze) */ \
    X(ShadowView, m_shadowViewDebug, L)               /* debug: the sun shadow map's light shown */ \
    X(AlienMap, m_alienMap, 0)                        /* AI missions' map on green */ \
    X(HillShade, m_hillShade, 0)                      /* the terrain shaded from a low sun */ \
    X(LightRig, m_lightRig, C)                        /* one light for day and night (light_rig.cpp) */ \
    X(GroundOne, m_groundOnePass, C)                  /* the native ground drawn in one pass, not two */ \
    X(SceneProf, m_sceneProfile, 0)                   /* debug: the scene timed and its overdraw counted by kind */ \
    X(ZoneFade, m_zoneFade, 0)                        /* a zone change holds the old zone's picture, fades into the new */ \
    X(Portals, m_portalsOn, 0)                        /* shops' rooms seen through their doors (portal.cpp) */ \
    X(StreetView, m_streetViewOn, 0)                  /* ... and the street from inside them */ \
    X(StreetNow, m_streetNow, 0)                      /* ... lit for the hour it is now */
enum class Knob : uint32_t {
#define RVK_KNOB_NAME(name, member, refresh) name,
    RVK_FLOAT_KNOBS(RVK_KNOB_NAME) RVK_BOOL_KNOBS(RVK_KNOB_NAME)
#undef RVK_KNOB_NAME
    Count
};
int KnobByName(const char* setting);
// Device::ZoneSignal's bits (the zone hand-over, handover.cpp; from proxy/native/zone.cpp's teleport hooks).
enum : uint32_t { kZoneTeleport = 1, kZoneTeleportFailed = 2, kZoneTeleportTry = 4 };   // (Try: an attempt, the world
                                                                                       // still there; Teleport: it stops)             // "RVK_Mist" -> Knob::Mist; -1: not a knob

class Device {
public:
    // One of the plain settings (Knob, above): a bool knob takes value != 0.
    void SetKnob(Knob knob, float value);
    Device();
    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    // window may be null (headless: no swapchain, screenshots still work).
    bool Init(HWND window, uint32_t width, uint32_t height, std::string* error);
    bool Resize(uint32_t width, uint32_t height);        // main target size; call outside a frame
    static void SetFixedClock(double step);              // tests: the clock steps `step` s a frame (0 = real time)
    bool SetWindow(HWND window);                         // attach (or change) the presentation window later
    HWND Window() const { return m_window; }
    const DeviceInfo& Info() const { return m_info; }
    uint32_t Width() const { return m_width; }
    uint32_t Height() const { return m_height; }

    void BeginFrame();
    void EndFrame();
    bool InFrame() const { return m_inFrame; }
    // Reads a target (null = main) as B8G8R8A8 rows of width*4 bytes. Mid-frame this submits the work
    // recorded so far and waits for it (rendering then continues in the same frame).
    bool ReadPixels(Texture* target, void* out);
    // A texture's level as it is on the GPU, its own format's bytes, rows packed (`bytes` must be that size): what
    // the game last wrote there, for a surface whose CPU copy was given up after the upload (rvk_surface.cpp).
    bool ReadTextureLevel(Texture* texture, uint32_t level, void* out, size_t bytes);
    void RequestScreenshot(const std::string& bmpPath);   // saved during the next EndFrame()
    void RequestFrameDump(const std::string& path, int frames = 1);   // the next frame's (frames') 3D draws, as text (diag.cpp)
    void SetDumpVertexCount(uint32_t count) { m_dumpVertexCount = count; }   // frame dumps list these draws' vertices

    // ---- D3D7-style interface (IDirect3DDevice7 semantics and enum values) ----
    void Clear(uint32_t flags, uint32_t argb, float z);
    struct Rect { int32_t left, top, right, bottom; };         // same layout as D3DRECT / RECT
    // D3D7 Clear with a rectangle list (target coordinates, clipped to the viewport); count 0 = viewport.
    void Clear(uint32_t count, const Rect* rects, uint32_t flags, uint32_t argb, float z);
    void SetViewport(const d3d::Viewport& vp);
    const d3d::Viewport& GetViewport() const { return m_viewport; }
    void SetRenderState(uint32_t state, uint32_t value);
    void SetTextureStageState(uint32_t stage, uint32_t type, uint32_t value);
    void SetTransform(uint32_t type, const d3d::Matrix& m);
    void SetMaterial(const d3d::Material& m);
    void SetLight(uint32_t index, const d3d::Light& light);
    void LightEnable(uint32_t index, bool enable);
    // Enhancement (not D3D7): evaluate the fixed-function lighting equation per pixel instead of per vertex.
    void SetPixelLighting(bool enable) { if (m_pixelLighting != enable) { m_pixelLighting = enable; m_constantsDirty = true; } }
    bool PixelLighting() const { return m_pixelLighting; }
    // Diagnostic: tint each draw by how it is lit (lighting off / no local light / lit by a local light).
    void SetLightingDebug(bool enable) { if (m_lightingDebug != enable) { m_lightingDebug = enable; m_constantsDirty = true; } }
    bool LightingDebug() const { return m_lightingDebug; }
    // The game's interface hidden (Ctrl+Shift+H, for screenshots): its draws after the 3D scene and the names over
    // characters are dropped, and the interface layer isn't put over the frame. Not a setting: off at each start.
    void SetHideInterface(bool hide) { m_hideInterface = hide; }
    bool HideInterface() const { return m_hideInterface; }
    // The console (the proxy's rvk_console.cpp): an image put over the top of the finished frame, after the interface
    // and after a screenshot's copy (screenshots leave it out) - A8R8G8B8 premultiplied by its coverage, tightly
    // packed, `width` the main target's. Copied; uploaded at the next EndFrame. Null / height 0: none.
    void SetConsoleImage(uint32_t width, uint32_t height, const void* pixels);
    // Enhancement, with per-pixel lighting: every lit draw gets the frame's active point / spot lights nearest
    // the camera instead of the (at most 8) the game enabled for it. Directional lights stay as the game set them.
    void SetLightOverride(bool enable) { if (m_lightOverride != enable) { m_lightOverride = enable; m_constantsDirty = true; } }
    bool LightOverride() const { return m_lightOverride; }
    // Enhancement: sun shadows (shadow.cpp). strength = how much of the light a shadow takes away (0..1);
    // range = half the width of the shadowed square around the camera, in world units.
    void SetShadows(bool enable) { if (m_shadows != enable) { m_shadows = enable; m_constantsDirty = true; } }
    bool Shadows() const { return m_shadows; }
    // distance: how far from the camera sun shadows reach (world units), split into `cascades` maps (1..4).
    void SetShadowParams(float strength, float distance, uint32_t cascades)
    { m_shadowStrength = strength; m_shadowRange = distance; m_shadowCascades = cascades < 1 ? 1 : cascades > kShadowCascades ? kShadowCascades : cascades; }
    // Enhancement, with the light override: shadows from up to `count` of the frame's point lights nearest the camera
    // (cube shadow maps, pointshadow.cpp; 0 = off). strength = how much of such a light a shadow takes away (0..1).
    // Lights with cube shadow maps (0 = off); the cubes are reallocated for the count at the next frame.
    void SetPointShadows(uint32_t count)
    {
        m_pointShadows = count < kMaxPointShadows ? count : kMaxPointShadows;
        m_cubeCountWanted = std::max<uint32_t>(m_pointShadows, 1);
    }
    uint32_t PointShadows() const { return m_pointShadows; }
    // dayFactor: the strength left in full daylight. Under a bright sun the scene is already at full brightness
    // around a light, so its shadows only show where the sun's are (they'd look cut off there); they fade instead.
    void SetPointShadowFadeIn(double seconds) { m_pointShadowFadeIn = seconds; }
    void SetPointShadowStrength(float strength, float dayFactor) { m_pointShadowStrength = strength; m_pointShadowDay = dayFactor; }
    static constexpr uint32_t kMaxPointShadows = 16;
    // Enhancement: the 3D scene is drawn into a 16-bit float target (colours above 1 kept) and tone mapped into the
    // 8-bit main target when the interface starts drawing (hdr.cpp). knee: colours up to it are shown unchanged,
    // brighter ones roll off towards white (1 = only clip, keeping the hue). exposure scales the scene first.
    void SetHdr(bool enable) { m_hdr = enable; m_constantsDirty = true; m_frameLightsDirty = true; UpdateWaterReady(); }
    bool Hdr() const { return m_hdr; }
    void SetTonemap(float knee, float exposure) { m_tonemapKnee = knee; m_exposure = exposure; }
    // With HDR: glow around light above the threshold (hdr.cpp). strength 0 = off.
    void SetBloom(float strength, float threshold) { m_bloomStrength = strength; m_bloomThreshold = threshold; }
    // With HDR: how much the game's additive effects (light halos, spells, fire) feed the bloom, on top of light above
    // the threshold. 0 = only the threshold.
    // Enhancement, with per-pixel lighting: normals generated from each surface's texture (brightness = height).
    // strength = height change per texel for a full brightness step, in texels (0 = off).
    // Normal maps (F_NORMALMAP): a texture's own tangent-space normal map (OpenGL convention, +Y = up in the
    // image), used instead of the generated normals when it is drawn in stage 0. The device owns `normal` and
    // frees it with the texture; null removes it.
    void SetNormalMap(Texture* texture, Texture* normal);
    // PBR materials: a texture's occlusion / roughness / metallic map (glTF packing: R occlusion, G roughness,
    // B metallic), lit with a GGX specular and metal-tinted reflections instead of the game's Blinn-Phong when it is
    // drawn like a normal map would be (per-pixel lit, stage 0, plain coordinates; or the ground's base texture).
    // `albedo` is drawn instead of the texture's own pixels wherever the texture is used (a higher resolution
    // replacement; its alpha is used too). The device owns both and frees them with the texture; null removes one.
    void SetMaterialMaps(Texture* texture, Texture* orm, Texture* albedo);
    void SetTextureRdbId(Texture* texture, uint32_t id) { if (texture) texture->m_rdbId = id; }
    // `emissive`: the light a texture's surface gives off (its colour, any resolution), added after the lighting and
    // as bright as PbrSettings::emissive - into the bloom with HDR. Owned by the device as above; null removes it.
    void SetEmissiveMap(Texture* texture, Texture* emissive);
    void SetLeafArt(Texture* texture, Texture* atlas, Texture* normal);   // (leaves.cpp: a tree type's own canopy)
    // Picking (the proxy's Ctrl+Shift+I textures, Ctrl+Shift+M meshes): the next frame's 3D scene draws are tested on
    // the CPU against the point (x, y: 0..1 across the target, y down) - GPU-skinned characters with their skinned
    // vertices - and the nearest one's stage 0 texture, visual and owning character (SetDrawVisual) are kept; LastPick
    // gives them once that frame has ended (serial: one more each result; thread-safe). SetPickHighlight: draws with
    // that texture flash yellow (null: none).
    void RequestPick(float x, float y) { m_pickX = x; m_pickY = y; m_pickArmed = true; }
    // triangles: the hit triangle (first) and those joined to it by shared vertices - in a dungeon room, the tile
    // piece's sub-mesh as placed - each as its corners drawn (model space x y z, then u v of stage 0); what the
    // workbench finds the piece by. Rooms only (triangleCount 0 otherwise).
    static constexpr uint32_t kPickTriangles = 48;
    struct PickResult { Texture* texture = nullptr; uint32_t serial = 0, visual = 0, owner = 0, kind = 0, vertices = 0,
                        indices = 0; uint32_t triangleCount = 0; float triangles[kPickTriangles * 15] = {}; };
    PickResult LastPick() const
    {
        PickResult r;
        r.serial = m_pickSerial.load(std::memory_order_acquire);
        r.texture = m_pickPublished.load(std::memory_order_relaxed);
        r.visual = m_pickPublishedVisual.load(std::memory_order_relaxed);
        r.owner = m_pickPublishedOwner.load(std::memory_order_relaxed);
        r.kind = m_pickPublishedKind.load(std::memory_order_relaxed);
        r.vertices = m_pickPublishedVertices.load(std::memory_order_relaxed);
        r.indices = m_pickPublishedIndices.load(std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(m_pickTriangleMutex);
            r.triangleCount = m_pickPublishedTriangleCount;
            std::memcpy(r.triangles, m_pickPublishedTriangles, sizeof(r.triangles));
        }
        return r;
    }
    void SetPickHighlight(Texture* t)
    {
        if (m_pickHighlight != t) { m_pickHighlight = t; m_constantsDirty = true; }
        m_pickHighlightTile = t ? m_pickTile : 0;   // (a ground tile picked: its cells flash - FillFrameLights)
    }
    // PBR lighting: on/off, direct specular strength, ambient (environment) specular strength, how much smooth
    // surfaces feed the screen-space reflections, the occlusion map's strength, debug view (0 off, 1 albedo,
    // 2 roughness, 3 metallic, 4 occlusion, 5 normal, 6 specular only, 7 which surfaces have maps, 8 emission);
    // whether albedo maps replace their textures; emissive maps' brightness (0 = off).
    struct PbrSettings {
        bool enabled = true, albedoMaps = true;
        float specular = 1.0f, ambient = 1.0f, reflections = 1.0f, occlusion = 1.0f, emissive = 2.0f;
        float probe = 1.0f;                      // how much the environment probe replaces the analytic sky (HDR)
        uint32_t debug = 0;
    };
    void SetPbr(const PbrSettings& s);
    // The draws that follow are of vertices the proxy's ProcessVertices wrote (the scene's: water, floating text)
    // or not (pre-transformed draws of the game's own: interface). See IsWater.
    void SetDrawProcessed(bool processed) { m_drawProcessed = processed; }
    void SetNormalMaps(bool enable, float strength) { if (m_normalMaps != enable || m_normalStrength != strength) { m_normalMaps = enable; m_normalStrength = strength; m_constantsDirty = true; } }
    void SetBump(float strength) { strength = strength < 0.0f ? 0.0f : strength; if (m_bump != strength) { m_bump = strength; m_constantsDirty = true; } }
    float Bump() const { return m_bump; }
    // Enhancement: anisotropic filtering level for linearly filtered textures (1 = off, up to the GPU's limit, 16).
    void SetAnisotropy(uint32_t level);
    uint32_t Anisotropy() const { return m_anisotropy; }
    // With HDR: camera motion blur (hdr.cpp). strength = exposure as a fraction of 1/60 s (0 = off); nothing nearer
    // than focusNear (the player's character, followed by the camera) is blurred.
    void SetMotionBlur(float strength, float focusNear) { m_motionBlur = strength; m_motionNear = focusNear; }
    // Motion blur mode: 0 = camera (from depth and the two cameras), 1 = per object (motion vectors: objects moving
    // through the world blur too, matched across frames by mesh; the character the camera follows stays sharp).
    void SetMotionBlurMode(uint32_t mode) { if (m_motionMode != mode) { m_motionMode = mode; m_frameLightsDirty = true; } }
    uint32_t MotionBlurMode() const { return m_motionMode; }
    float MotionBlur() const { return m_motionBlur; }
    // With HDR: depth of field (hdr.cpp). strength: blur at full CoC (0..1+); radius: largest blur in pixels at 1440
    // lines; focus: distance (0 = auto: the nearest surface near the screen centre - the player's character); range:
    // the in-focus band around it; nearBlur: blur in front of the focus too; bokeh: hexagonal highlights.
    // farBlur: blur behind the focus always; otherwise only when the focus is nearer than closeFocus.
    void SetDof(bool enable, bool bokeh, bool nearBlur, float strength, float radius, float focus, float range,
                bool farBlur, float closeFocus)
    { m_dof = enable; m_dofBokeh = bokeh; m_dofNear = nearBlur; m_dofStrength = strength; m_dofRadius = radius;
      m_dofFocusDistance = focus; m_dofRange = range; m_dofFar = farBlur; m_dofCloseFocus = closeFocus; }
    // With HDR: ambient occlusion from the depth buffer (hdr.cpp). strength 0 = off; radius in world units.
    void SetAo(float strength, float radius) { m_aoStrength = strength; m_aoRadius = radius; }
    float AoStrength() const { return m_aoStrength; }
    // With HDR: indirect light from the lit scene on screen (hdr.cpp). strength 0 = off; radius in world units.
    void SetGi(float strength, float radius) { m_giStrength = strength; m_giRadius = radius; }
    // With HDR: light scattered by the air - sun shafts (strength; 0 = off), lamp glow (relative), haze density.
    // rays: light shafts through the cloud layer's gaps, x; land: haze over the distant land (0..1).
    void LandHaze(float out[24]) const;
    void SetVolume(float strength, float haze, float shafts, float rays = 1.0f, float land = 0.4f)
    { m_volume = strength; m_volumeHaze = haze; m_volumeShafts = shafts; m_volumeRays = rays; m_landHaze = land; }
    // With HDR: screen-space reflections - strength (0 = off), reflectivity of water, glossy surfaces, wet ground.
    void SetSsr(float strength, float water, float gloss, float wet)
    { m_ssr = strength; m_ssrWater = water; m_ssrGloss = gloss; m_ssrWet = wet; m_constantsDirty = true; }
    // With HDR: colour grading after the tone mapping (grading.cpp). Neutral: 1, 1, 0, any, 0, 0.
    void SetGrading(float saturation, float contrast, float warmth, float lutAmount, float nightTint, float vignette)
    { m_saturation = saturation; m_contrast = contrast; m_warmth = warmth; m_lutAmount = lutAmount;
      m_nightTint = nightTint; m_vignette = vignette; }
    // A 3D colour lookup table: slot 0 day, 1 night; size^3 RGBA8, red fastest. size 0: the identity.
    void SetColorLut(uint32_t slot, uint32_t size, const uint8_t* rgba);
    // Mesh ids (meshids.cpp): ao-assets' table of the static meshes' piece geometries (mesh-ids.bin) - which mesh a
    // draw is. False (logged) when there is none.
    bool LoadMeshIds(const char* path);
    // Placed objects' shadows (statels.cpp): the playfield's pack (ao-assets statels: <client>/randy-vk/statels/
    // <playfield>.bin; "" none) - its static objects cast whether the game draws them or not. Mode 0 off, 1 once the
    // placement is confirmed against the game's draws, 2 always.
    bool LoadStatels(const char* path, uint32_t playfield);
    void SetStatelMode(uint32_t mode);
    void TriggerLightning(const float pos[3]);                     // a strike there now (the game's, or a test's)
    void SetSandForce(float amount, bool ash) { m_sandForce = amount; m_sandAsh = ash; }   // RVK_SandForce, RVK_SandAsh
    // A free camera for debugging (Ctrl+Shift+C, the proxy's keys and mouse; draw.cpp): once a frame, before its draws.
    // On, it starts where the game's camera is; every view the game sets is re-expressed about it.
    struct FreeCamInput {
        bool on = false;
        float move[3] = {};                      // right, up, forward (-1..1 each)
        float look[2] = {};                      // turn right, up (radians)
        float speed = 8.0f;                      // world units a second
        float dt = 0.0f;                         // seconds since the last
    };
    void SetFreeCam(const FreeCamInput& in);
    void SandHaze(float out[8]) const;                             // the tone mapping's sand: dust, dust2
    void SandGusts(float out[16]) const;                           // ... its distant gusts: gust0..3
    float SandFogThrough(const float eye[3], const float p[3]) const;   // (smoke.cpp)
    // The storm's air in the fog (sand_fog.frag): a sand or ash storm's dust, a thunderstorm's rain mist (RVK_RainFog),
    // or both - how thick at the ground (per unit), the dust's share, the thick layer's scale height, the share of the
    // tall column above (scale height upperHeight). (wind.cpp)
    struct StormAir { float sigma = 0.0f, sandShare = 1.0f, height = 120.0f, upper = 0.3f, upperHeight = 1500.0f; };
    StormAir StormAirNow() const;
    void SetWetNow(float wet, float puddles) { m_wetness = wet; m_puddles = puddles; }   // (tests: soaked already)
    float StormOvercast() const;
    float StormSky() const { return std::clamp(m_stormSky, 0.0f, 1.0f); }   // how much a storm's sky (rain.cpp)
    // The weather's ramps (rain.cpp), read live: a storm's deck thickening the cloud layer and its reach (StormSky
    // 0.1 .. 0.7, smoothed); the storm in full - the land's darkening, the air's hue (0.05 .. 0.6); how clear the sky
    // over us is - an overcast and a storm take it (the twilight's colours, the stars, the auroras).
    float StormDeck() const;
    float StormFull() const;
    float ClearSky() const;
    void StormSmokeWind(float out[3]) const;                       // a storm's own wind on the smoke (smoke.cpp)                                   // how much a thunderstorm hides the sun (rain.cpp)
    void StormHue(float rgb[3]) const;                             // a colour turned the storm's dark yellow-green (rain.cpp)
    void StormAirColour(float rgb[3]) const;                             // ... and the air's (fog, haze) dimmed under its deck
    float StormAirDim() const;                                     // ... by this (1 without a storm)
    float StormSkyLight() const;                                   // the sky's light left under its deck (0.5 .. 1)
    // A thunderstorm's green cast (RVK_StormGreen x how hard it rains): per channel, about unit brightness (rain.cpp).
    void StormGreen(float out[3]) const;
    bool SandFogOn() const { return m_sandFog > 0.0f && ((m_sand > 0.01f && m_sandHaze > 0.0f) || (m_rain + 0.3f * m_stormSky) * m_rainFog > 0.01f); }
    bool SmokeCoverValid() const { return m_smokeHalf && m_smokeHalfFrame == m_frameNumber; }
    // The grass's look: variety (clumps, dry and tall patches, colour jitter; baked), flowers (wildflowers, seed heads
    // and broad blades, a multiplier on how many; baked), glow (sunlight through and along the blades: backlit tips,
    // the sheen, the blades' rounded shading), gusts (gusts of wind visible as they sweep the field) and trails
    // (pushed grass stays down a few seconds behind a character). 0 = off each; the last three are per frame.
    void SetGrassStyle(float variety, float flowers, float glow, float gusts, bool trails)
    {
        if (variety != m_grassVariety || flowers != m_grassFlowers)
            m_grassDirty = true;
        m_grassVariety = variety;
        m_grassFlowers = flowers;
        m_grassGlow = glow;
        m_grassGusts = gusts;
        m_grassTrails = trails;
    }
    // RVK_TrailTime / RVK_TrailShade: how long trodden grass stays down (seconds; it springs back over the
    // last third or so) and how strongly it shows as lawn stripes (lighter lying away from the camera, darker towards).
    void SetGrassTrailLook(float seconds, float shade)
    {
        m_trailSeconds = seconds < 1.0f ? 1.0f : seconds;
        m_trailShade = shade < 0.0f ? 0.0f : shade;
    }
    // Leaves on trees, shrubs and palms (leaves.cpp; RVK_Leaf*): sprigs cut from the canopy's own texture, each its
    // own card, swaying with the branches and fluttering; the canopy itself thinned to an inner core. Baked per
    // canopy mesh (density, size: a change rebakes); the rest applies per frame.
    struct LeafSettings {
        bool on = false;
        float density = 1.0f;          // sprigs over a card's leafy area (1 = covered about twice)
        float size = 1.0f;             // a sprig's share of the leaf texture (1 = an eighth of its width)
        float distance = 60.0f;        // full leaves up to half of it, thinning out to none at it
        float core = 0.6f;             // the canopy's own cards cut back to an inner core (0 = left as they are)
        float wind = 1.0f;             // trunk and branches swaying
        float flutter = 1.0f;          // each sprig fluttering
        bool trees = true, shrubs = true, palms = true;
        bool shadows = true;           // the leaves cast the sun's shadow (dappled light under the trees)
        float fall = 1.0f;             // leaves coming off and drifting down (more in gusts), lying a while (0 = none)
        bool own = true;               // trees with leaf art (ao-assets leaf-art): our own canopy in place of the cards
    };
    void SetLeaves(const LeafSettings& s)
    {
        if (s.density != m_leaf.density || s.size != m_leaf.size || s.own != m_leaf.own)
            m_leafDirty = true;
        m_leaf = s;
        m_frameLightsDirty = true;
    }
    float m_crownShade = 1.0f;
    float m_crownSky = 1.0f, m_crownDepth = 1.0f;   // RVK_CrownSky, RVK_CrownDepth (ffp.vert, ffp_main.glsl; FL.foliage)
    // RVK_GrassShadow: the blades cast the sun's shadow (into its nearest cascade: onto the ground and each other).
    void SetGrassShadows(bool on) { m_grassShadows = on; }
    // RVK_GrassEven: the blades' colour from one green (1) rather than the ground texel under each (0); baked.
    void SetGrassEven(float even)
    {
        if (even != m_grassEven)
            m_grassDirty = true;
        m_grassEven = even;
    }
    void SetGrassField(bool on, float distance, float density, float height, bool texOnly)
    {
        // The blades are baked into tiles, so a change to their density, height or the ground filter must rebuild
        // them: mark the tiles stale and (for the filter) re-classify the ground. The distance is per frame.
        if (m_grassOn && (density != m_grassDensity || height != m_grassHeight || texOnly != m_grassTex))
            m_grassDirty = true;
        if (texOnly != m_grassTex)
            m_grassGroundReset = true;           // the ground's grass/not must be classified again
        m_grassOn = on;
        m_grassDistance = distance;
        m_grassDensity = density;
        m_grassHeight = height;
        m_grassTex = texOnly;
    }
    // Shadow map sizes in pixels: the sun's (each cascade) and the point lights' (each cube face). Takes effect at
    // the next frame (the maps are recreated).
    void SetShadowResolution(uint32_t sun, uint32_t point)
    {
        m_shadowSizeWanted = std::clamp<uint32_t>(sun, 512, 8192);
        m_pointShadowSizeWanted = std::clamp<uint32_t>(point, 128, 2048);
    }
    // Phong tessellation of characters: shape (0 = off, 1 = fully round), distance, the level up close.
    void SetTessellation(float shape, float distance, uint32_t level)
    {
        m_tessShape = shape;
        m_tessDistance = distance;
        m_tessLevel = std::clamp<uint32_t>(level, 1, 16);
    }
    // Intensity of point / spot lights: lamps and other lights, and lights characters carry (yours included).
    void SetPointLightIntensity(float lights, float characters)
    {
        m_pointLightScale = lights;
        m_charLightScale = characters;
        m_frameLightsDirty = true;
        m_constantsDirty = true;
    }
    // With HDR: temporal anti-aliasing (jittered scene, resolved against the last frame) and sharpening after it.
    void SetTaa(bool enable, float sharpen) { m_taa = enable; m_sharpen = sharpen; m_frameLightsDirty = true; }
    // RVK_RenderScale: the scene drawn at this percentage of the output size per axis (50..100), the TAA bringing it
    // up to the output size (docs/render-perf-plan.md Phase 1). Without TAA it acts as 100. Takes effect at the next
    // frame (the scene's targets are recreated).
    void SetRenderScale(uint32_t percent) { m_renderScaleWanted = std::clamp<uint32_t>(percent, 50, 100); }
    // RVK_Upscaler: what brings the scaled scene up to the output's size - 0 our TAA (taa.frag), 1 AMD's FSR 2.2
    // (fsr2.cpp). At 100% the TAA either way.
    void SetUpscaler(uint32_t which) { m_upscaler = which; }
    // RVK_FrameGen: AMD's FSR 3.1 frame generation (framegen.cpp) - the swapchain made again at the next frame.
    void SetFrameGen(bool enable) { if (enable != m_frameGenWanted) m_frameGenFailed = false; m_frameGenWanted = enable; }
    // Frames the frame interpolation swapchain has presented (generated ones too); 0 without frame generation.
    uint64_t FgShownFrames() const;
    // Sunlight through leaves (RVK_LeafLight).
    void SetLeafLight(float s) { m_leafLight = s; m_frameLightsDirty = true; }
    // How dark nights, storms and dim places get (darkness.cpp; docs/darkness-plan.md): RVK_Darkness (0 lightest,
    // 1 the old nights, 2 very dark) and RVK_DarkFloor, the brightness floor's share (0: no floor).
    void SetDarkness(float d, float floorScale)
    {
        m_darkness = d;
        m_darkFloorScale = floorScale;
        m_frameLightsDirty = true;
        m_constantsDirty = true;
    }
    // Fog banks (mist.cpp; docs/weather-events-plan.md): thick drifting banks in the low ground at dawn and after rain,
    // most in wet zones (RVK_FogBanks x; force: that strength now - tests; < 0 by the weather).
    void SetFogBanks(float s, float force = -1.0f) { m_fogBanks = s; m_fogBankForce = force; }
    // Heat lightning (rain.cpp): on some warm dry nights, silent flashes in far-off clouds (RVK_HeatFlash x; force:
    // tonight whatever the chance - tests).
    void SetHeatFlash(float s, bool force = false) { m_heatFlash = s; m_heatForce = force; }
    // The sun's disc (sun.cpp; 0 none, 1 as planned: its brightness).
    void SetSun(float s) { m_sun = s; }
    // The sky's twilight (0: the game's sky; 1: the twilight palette in its place) and the glow round the sun, x
    // (sky.cpp).
    void SetTwilight(float twilight, float glow, float blueHour = 1.0f)
    {
        m_twilight = twilight;
        m_skyGlow = glow;
        m_blueHour = blueHour;
    }
    void SetStarFade(bool on) { m_starFade = on; }
    void SetHeapCheck(int level) { m_heapCheck = level; }
    // RVK_ViewRange: the game's fog distances x this (its cull goes out with it: rnative::camera::SetViewRange); not 1:
    // fog by true distance, as the cull.
    void SetViewRange(float range) { m_viewRange = range > 0.0f ? range : 1.0f; }
    float m_viewRange = 1.0f;
    void SetProcSky(bool on, float zone, float bright)
    {
        m_procSky = on;
        m_skyZone = zone;
        m_skyBright = bright;
    }
    void SetSkyColours(const SkyColours& c) { m_skyColours = c; }
    void SetNightSky(const NightSky& n) { m_nightSky = n; }
    // Rainbows after rain (docs/sky-plan.md §5; RVK_Rainbow x, 0 none); force: one of that strength now (tests; < 0 by
    // the weather).
    void SetRainbow(float s, float force = -1.0f) { m_rainbowSetting = s; m_rainbowForce = force; }
    // A desert's air (docs/desert-plan.md phase 4): its dust - the far land paler and warmer, a pale band along the
    // horizon - (RVK_DesertAir) and the heat shimmer over its distant sand (RVK_HeatShimmer), each 0..1.
    void SetDesertAir(float air, float shimmer)
    {
        m_desertAir = air;
        m_heatShimmer = shimmer;
        m_frameLightsDirty = true;
    }
    // ... how much of each now (sky.cpp): out[0] the dust, out[1] the shimmer.
    void DesertAir(float out[2]) const;
    // Remembered shadow casters (things out of view still casting) and the terrain's shade from a low sun, on / off.
    // (0 off, 1 on, 2 on without the remembered sway, 3 only big things, 4 drawn solid; switched: the churn trace and a few seconds of
    // the shadow items' counts anew - finding a blinking shadow)
    void SetShadowMemory(uint32_t mode) { m_shadowMemory = mode; m_cacheTraceLines = 0; m_memoryTraceFrames = 240; }
    // The ground's own texturing (terrain.cpp, terrain.glsl), each 0..1: its texture laid on the faces of cliffs
    // instead of stretched down them (RVK_CliffTex), the faces' repeats hidden (RVK_TileHide), large-scale variation
    // in its colour (RVK_GroundVary); rock on the faces instead of the ground's tile (RVK_CliffRock).
    // round: the native ground's tessellation (Phong's shape, 0 off); depth: its cliffs' rock displaced out of the faces
    // (world units at most); grain: its fine grit up close; blend: its cells' tiles blended across their edges.
    struct TerrainSettings {
        float cliff = 1.0f, tiling = 1.0f, variation = 1.0f, round = 0.6f, depth = 0.4f, grain = 0.6f, blend = 1.0f;
        bool rock = true;
        float parallax = 1.0f;                   // RVK_Parallax: side-loaded height maps' depth, x
        float parallaxSteps = 1.0f;              // RVK_DepthSteps: its marches' step counts, x
        float ripple = 1.0f, dune = 0.6f, glint = 0.5f;   // the sand: RVK_SandRipple, RVK_DuneLight, RVK_SandGlint
        float smooth = 0.3f;                     // ... RVK_RippleSoft
        float rippleDepth = 1.0f;                // ... RVK_RippleDepth: their depth up close, x
        float prints = 1.0f;                     // ... RVK_SandPrints: footprints' depth, x (sand_prints.cpp)
    };
    void SetTerrain(const TerrainSettings& s) { m_terrainSettings = s; m_frameLightsDirty = true; }
    // A footprint at world x, z, heading dx, dz (unit), stamped at the next frame (sand_prints.cpp; the characters'
    // own come from the grass's trail, UpdatePushTrail - this for tests).
    void AddSandPrint(float x, float z, float dx, float dz) { m_printsNew.push_back({x, z, dx, dz}); }
    // The ground drawn by rvk from the playfield's data instead of the game's (native_ground.cpp, RVK_GroundOn): its
    // data (null: the game's own), the baked lightmap of the hour as the game hands it over (4-bit texels, two to a
    // byte, the high nibble first; (w + 1) / 2 bytes a row) with its shade, and on / off.
    void SetNativeGround(std::shared_ptr<const NativeGround> ground);
    void SetNativeGroundLightmap(std::shared_ptr<const std::vector<uint8_t>> packed, uint32_t w, uint32_t h, float shade);
    void SetNativeGroundOn(bool on) { m_native.on = on; }
    void SetNativeGroundAo(float strength) { m_native.ao = strength; }
    void SetNativeGroundSun(float strength) { m_native.sun = strength; }
    void SetNativeGroundRocks(float density);
    // RVK_Pebbles: pebbles on flat dirt, gravel and sand near the eye (ground_pebbles.cpp; 0 none, 1, 2 twice as
    // many); and the hand table of which surface types get them (randy-vk\terrain\pebbles.tsv; null none).
    void SetNativeGroundPebbles(float density);
    void SetPebbleTablePath(const char* path);
    bool NativeGroundDrawing() const { return m_native.drawing; }
    // The game's own sun sprites (its glow, VisualSunRays_t), shown or not.
    void SetGameSun(bool on) { m_gameSun = on; }
    // Tests: the sky weather's clock (seconds, as UTC; < 0 the wall clock).
    void SetSkyTime(double t) { m_skyTime = t; }
    // The playfield the game is in (its resource id; 0 none): its climate leans the sky (sky.cpp SkyZoneLean).
    void SetSkyPlayfield(uint32_t playfield);
    // The zone hand-over (handover.cpp, RVK_ZoneFade): kZoneTeleport a teleport began (the old zone's picture is kept
    // from here), kZoneTeleportFailed it failed.
    void ZoneSignal(uint32_t signal);
    bool HandoverFading() const;                 // (tests)
    void SetEffectGlow(float gain) { if (m_effectGlow != gain) { m_effectGlow = gain; m_constantsDirty = true; } }
    float EffectGlow() const { return m_effectGlow; }
    float BloomStrength() const { return m_bloomStrength; }
    // With HDR: how bright local lights may make a surface (1 = the game's clamp; soft roll-off towards it).
    void SetHdrHeadroom(float headroom) { m_hdrHeadroom = headroom < 1.0f ? 1.0f : headroom; m_frameLightsDirty = true; }
    float HdrHeadroom() const { return m_hdrHeadroom; }
    // Enhancement, with the light override: how bright the frame's lights may make a surface (1 = D3D's clamp, up to
    // 2). The game's own lighting stays clamped at 1; local lights add on top of it, so they (and their shadows) show
    // on surfaces the sun already lights fully.
    float LightHeadroom() const { return m_lightHeadroom; }
    // Enhancement: GPU particles (particles.cpp). A game sprite effect announces itself with ParticleEmitter before it
    // draws: its identity (stable while the effect exists) and its sprites in world space. Each live sprite sheds tiny
    // particles that a compute pass moves through a flow field (curl noise, a swirl around the effect, a pull back to
    // it) and that outlive it; they are drawn right after the effect's next sprite draw (FVF 0x142), with its texture
    // and blending. The simulation runs at the start of each frame from the previous frame's sprites.
    struct ParticleSprite {
        float pos[3], size;                      // world space; size = the sprite's width
        float uv[4];                             // texture rectangle: u, v, width, height
        uint32_t color;                          // D3DCOLOR (ARGB)
        uint32_t alive;
    };
    struct ParticleParams {
        bool enable = true;
        uint32_t perSprite = 12;                 // particles each sprite keeps alive (up to kParticleChildren)
        float size = 0.15f;                      // a particle's size as a fraction of its sprite's
        float life = 1.5f;                       // seconds (each particle 0.5x .. 1.5x)
        float curl = 1.5f;                       // flow field speed (world units / s)
        float swirl = 1.0f;                      // orbit speed around the effect's vertical axis
        float pull = 0.6f;                       // pull back towards the effect (per second)
        float drag = 2.5f;                       // how fast particles take on the flow's velocity (per second)
        float inherit = 0.5f;                    // fraction of the sprite's own velocity a new particle starts with
        float speed = 0.6f;                      // random launch speed
        float scale = 1.5f;                      // flow field feature size (world units)
        // How much each effect's own motion shapes its particles (0 = every effect alike; MeasureParticleMotion).
        float adapt = 1.0f;
        float follow = 0.7f;                     // how much young particles follow their sprite's motion (0..1)
        float brightness = 2.0f;                 // colour multiplier (smaller particles cover less: they need more)
        float trail = 0.05f;                     // motion trails: seconds of motion each particle is stretched over (0 = off)
        float trailMax = 6.0f;                   // longest trail, in particle sizes
        float fixedSize = 0.02f;                 // world units: the size every effect's particles get with uniformSize 1
        float uniformSize = 1.0f;                // 0 = particles sized from their sprite (size), 1 = all fixedSize
    };
    void SetParticleParams(const ParticleParams& params) { m_particleParams = params; }
    const ParticleParams& GetParticleParams() const { return m_particleParams; }
    // center: where the swirl and pull centre (the live sprites' middle); origin: the effect's own frame of reference
    // (it moves with a character - motion relative to it is the effect's own).
    void ParticleEmitter(uint64_t key, const float center[3], const float origin[3], const ParticleSprite* sprites,
                         uint32_t count);
    void EndParticleEmitter() { m_particlePending = nullptr; }

    // Our own effects (fx.cpp; docs/fx-plan.md): definitions by id (FxSetEffect, again to change one, FxForget), and
    // effects playing - each announced every frame it lives (FxInstance: the game effect it stands for, by a key stable
    // while it lives, its points and the phase it is in). Each emitter of a playing effect is a block of particles,
    // simulated on the GPU at the next frame's start and drawn at the scene's end; once an effect is no longer
    // announced its particles live out their lives.
    struct FxAnchors {
        float head[3] = {}, source[3] = {}, target[3] = {};
        uint32_t phase = fx::kFlight;            // the one of fx::PhaseBits it is in
    };
    void FxSetEffect(uint32_t id, std::shared_ptr<const fx::Effect> effect);
    void FxForget(uint32_t id);
    void FxInstance(uint64_t key, uint32_t effect, const FxAnchors& anchors);
    static constexpr uint32_t kFxBlocks = 256, kFxDrawnBlocks = 128;
    static constexpr uint32_t kParticleSlots = 128, kParticleChildren = 32;
    static constexpr uint32_t kParticlesPerBlock = kParticleSlots * kParticleChildren, kParticleBlocks = 256;
    static constexpr uint32_t kParticleDrawnBlocks = 128;     // effects simulated and drawn per frame (nearest first)
    static constexpr uint32_t kParticleFvf = 0x142;           // XYZ | DIFFUSE | TEX1: the game's sprite vertices

    // Enhancement: the water (water.cpp). The game's water triangles (VisualLiquid_t's world-space mesh, kept by the
    // proxy from its ProcessVertices call) drawn as our water surface in their place: waves, refraction, absorption,
    // reflections, foam, caustics. Called where the game draws its water (the scene's rendering).
    struct WaterVertex {
        float pos[3];                            // world space
        uint32_t colour;                         // the game's vertex colour (ARGB): the water's tint
    };
    struct WaterParams {
        bool enable = true;
        float style = 0.5f;                      // 0 = the game's water made richer, 1 = realistic
        float waves = 1.0f;                      // wave height (0 = flat)
        float ripples = 1.0f;                    // small ripples (normals)
        float reflections = 1.0f;
        float refraction = 1.0f;
        float clarity = 1.0f;                    // how far one sees into the water
        float foam = 1.0f;
        float caustics = 1.0f;
        float gameTexture = 0.5f;                // the game's water texture on the surface (enhanced AO look)
        float swimRipples = 2.0f;                // how hard characters in the water disturb it (RVK_WaterSwim)
        float swimStir = 1.0f;                   // a character standing still stirs it (RVK_WakeStir)
        float swimLife = 0.3f;                   // how long the disturbance lasts, relative (RVK_WakeLife)
        float swimSpeed = 3.6f;                  // the waves' speed, m/s (RVK_WakeSpeed)
        float swimGain = 2.0f;                   // how strongly it shows in the surface (RVK_WakeGain)
        float swimSplash = 1.0f;                 // the splash of a character jumping in (RVK_WakeSplash)
        float fallSheen = 1.0f;                  // how much a fall's clear water reflects (RVK_WaterSheen)
        float fallSparkle = 1.0f;                // a fall's sparkles of sunlight and lamplight (RVK_WaterSpark)
        float fallSpray = 1.0f;                  // the spray and mist at a fall's foot (RVK_WaterSpray)
        float fallChurn = 1.0f;                  // the pool churned and foaming where a fall lands (RVK_WaterChurn)
        float fallSpeed = 1.75f;                 // how fast a fall's water falls, x a natural fall's (RVK_WaterSpeed)
        uint32_t quality = 2;                    // 1 low, 2 medium, 3 high: mesh density, reflection steps
        bool mesh = true;                        // the surface from the game's triangles (RVK_WaterMesh; off: the grid)
    };
    void SetWaterParams(const WaterParams& params) { m_waterParams = params; UpdateWaterReady(); }
    void DrawWater(const WaterVertex* vertices, uint32_t vertexCount, const uint16_t* indices, uint32_t indexCount);
    // The water can be drawn (on, HDR on, resources made): the proxy replaces the game's water only then. Any thread.
    bool WaterReady() const { return m_waterReady.load(std::memory_order_relaxed); }

    // Weather (rain.cpp): the game's weather visuals, drawn by rvk instead. The proxy hands over what the game's
    // weather asks for each frame it is on (VisualRain_t::process: how hard, the wind, its colour); without a hand-off
    // for a moment it is over. Rain falls in a box round the camera, drawn after the scene.
    struct WeatherParams {
        bool enable = false;                     // RVK_WeatherOn (the proxy hides the game's rain only then)
        float force = 0.0f;                      // RVK_RainForce: rain this hard regardless of the game's (0 = the game's)
        float amount = 1.0f;                     // RVK_RainAmount: how many drops, x
        float splashes = 1.0f;                   // RVK_RainSplash: how many splashes, x (0 = none)
        float brightness = 1.0f;                 // RVK_RainBright: the drops' and splashes' light, x
        float speed = 15.0f;                     // RVK_RainSpeed: a drop's fall (units / s)
        float length = 0.45f;                    // RVK_RainLength: a falling drop's streak (units)
        float lean = 0.3f;                       // RVK_RainLean: its most slant, sideways per unit of fall (0.3 ~17 deg)
        float stormForce = 0.0f;                 // RVK_StormForce: a thunderstorm this hard regardless of the game's
                                                 // (its rain, its wind, lightning of our own; 0 = the game's)
        float strikeRate = 6.0f;                 // RVK_StrikeRate: its strikes a minute at full strength
    };
    struct WeatherState {
        float precipitation = 0.0f;              // how hard (the game's VisualRain_t::process value, as given)
        float wind[3] = {0, 0, 0};               // the game's wind (world)
        uint32_t colour = 0;                     // the game's weather colour (VisualRain_t::SetColor): 0x00RRGGBB
        float fallSpeed = 0.0f;                  // its drops' fall (VisualRain_t +0x24; rain ~9.8, snow slower)
        float windScale = 0.0f;                  // how much the wind moves them (+0x38)
        float opacity = 0.0f;                    // their opacity factor (+0x5bc)
    };
    void SetWeatherParams(const WeatherParams& params) { m_weatherParams = params; }
    void SetWeather(const WeatherState& state);

    // Smoke (smoke.cpp): a fluid simulated in a box round each smoke source, ray-marched into the scene after it. The
    // sources are handed over each frame they are on (SmokeSourceFrame, keyed); one not handed over goes on rising
    // and fading until its smoke is gone.
    struct SmokeParams {
        bool enable = false;                     // RVK_SmokeOn
        uint32_t quality = 2;                    // RVK_SmokeQual: a box's cells a side at most: 1 64, 2 96, 3 128
        uint32_t sandCells = 0;                  // RVK_SandRes: the sand volume's cells a side (0: by quality)
        float resolution = 12.0f;               // RVK_SmokeRes: cells per world unit (a bigger box has more)
        float density = 1.0f;                   // RVK_SmokeThick: how thick, x
        float stir = 1.0f;                      // RVK_SmokeStir: how much characters move it (0 not at all)
        float detail = 1.0f;                    // RVK_SmokeWisps: the wisps finer than the grid (0 none)
        float brightness = 1.0f;                // RVK_SmokeBright: its colour, x
        float opacity = 1.0f;                   // RVK_SmokeOpaque: how opaque it is drawn (<= 1 x; above, as if thicker)
        bool half = true;                        // RVK_SmokeHalf: marched at half size, upsampled by depth
        float budget = 1.5f;                     // RVK_SmokeBudget: the simulation's GPU ms a frame, about (0: no cap)
    };
    struct SmokeSource {
        float pos[3] = {0, 0, 0};                // world: where it comes from (its box stands round it)
        float radius = 0.6f;                     // the source's size (world)
        float rate = 0.6f;                       // the density it keeps where it comes from
        float heat = 1.0f;                       // temperature added a second (it rises the more)
        float speed = 1.0f;                      // its push upwards (world / s)
        float life = 4.0f;                       // about how long the smoke lasts (s)
        float size = 8.0f;                       // its box (world)
        uint32_t colour = 0x707070;              // 0xRRGGBB
        float wind[3] = {0, 0, 0};               // the wind carrying it (world / s; the game's effects' wind)
    };
    void SetSmokeParams(const SmokeParams& params) { m_smokeParams = params; }
    // Volumetric clouds (clouds.cpp): the game's cloud puffs (VisualSingleCloud_t) drawn as volumes in their place.
    struct CloudParams {
        bool enable = false;                     // RVK_CloudsOn
        float density = 1.0f;                    // RVK_CloudThick: how thick, x
        float brightness = 1.0f;                 // RVK_CloudBright: their colour, x
        float night = 0.8f;                      // RVK_CloudNight: ... at night, x more (eased in as the sun goes down)
        float vary = 1.0f;                       // RVK_SkyVary: how much the sky changes, clear .. overcast (0: always
                                                 // scattered, as the game's puffs say; sky.cpp)
        float zones = 1.0f;                      // RVK_SkyZones: how much a zone's climate leans its skies (sky.cpp)
        float detail = 1.0f;                     // RVK_CloudDetail: how much the noise frays them (0: smooth)
        uint32_t steps = 32;                     // RVK_CloudSteps: the march's steps through a puff
        bool half = true;                        // RVK_CloudHalf: marched at half resolution
        float shadow = 0.6f;                     // RVK_CloudShadow: how dark their shadows on the ground (0: none)
        bool layer = true;                       // RVK_CloudLayer: a cloud layer over the sky (else each puff a volume)
        float cover = 0.0f;                      // RVK_CloudCover: the layer's coverage, more (+) or less (-)
        float height = 900.0f;                   // RVK_CloudHeight: the layer's base above the eye's ground (world)
        float shade = 1.6f;                      // RVK_CloudShade: contrast (light and shade; 1 = as lit)
        float low = 0.0f;                        // RVK_CloudLow: the low deck (stratus), x (0: none - user)
        float sheet = 1.0f;                      // RVK_CloudSheet: the game's flat sheets behind the layer, x (0: none)
        float morph = 1.0f;                      // RVK_CloudMorph: how fast the clouds churn and change, x (0: still)
        uint32_t quality = 1;                    // RVK_CloudQual: 0 low .. 3 ultra (the layer's step length, steps to the sun)
        float cirrus = 0.0f;                     // RVK_Cirrus: thin high wisps over the layer, x (0: none; the default -
                                                 // coarse streaks across our own sky, user 2026-10-18)
    };
    void SetCloudParams(const CloudParams& params);
    void SmokeSourceFrame(uint64_t key, const SmokeSource& source);

    void SetTexture(uint32_t stage, Texture* texture);
    // The game's visual issuing the next draws (rnative::scene): its kind (VisualKind) and class name - for exact
    // decisions where the draw alone needs heuristics. 0 / "": not known.
    void SetDrawVisual(uint32_t kind, const char* className, uint32_t owner = 0, uint32_t visual = 0);
    // The game's lights of this frame (rnative::scene), each with its carrier's owner id (SetDrawVisual's) or 0: who
    // carries which light, exactly (FindCarriers).
    struct SceneLight { d3d::Light light; uint32_t owner; };
    void SetSceneLights(const SceneLight* lights, uint32_t count);
    // Null = the main target. Like D3D, resets the viewport to the whole target.
    void SetRenderTarget(Texture* target);
    // The interface layer (interface.cpp): the game's interface drawn into an image of its own and shown over each
    // frame, so it needn't be redrawn every frame (the game skips GUI.dll's drawing in between). Around the
    // interface's drawing: Begin(true) draws it into the layer (cleared first), Begin(false) means it isn't drawn
    // this frame; End shows the layer over the main target.
    void InterfaceBegin(bool redraw);
    void InterfaceEnd();
    // The world's floating text (damage, XP, names over heads) next: the scene ends here - its effects (smoke, fog,
    // the tone mapping, bloom, grading) applied - and the text is drawn over the finished picture, still hidden by
    // what stands in front of it (the scene's depth). The interface follows.
    void OverlayBegin() { if (m_inFrame && m_main) InterfaceSceneEnd(); }
    // The layer holds an interface drawn at the current size (else the next frame must redraw it). Any thread.
    bool InterfaceLayerReady() const { return m_uiLayerReady.load(std::memory_order_relaxed); }
    Texture* GetRenderTarget() const { return m_target == m_main ? nullptr : m_target; }

    void DrawPrimitive(uint32_t primitive, uint32_t fvf, const void* vertices, uint32_t vertexCount);
    void DrawIndexedPrimitive(uint32_t primitive, uint32_t fvf, const void* vertices, uint32_t vertexCount,
                              const uint16_t* indices, uint32_t indexCount);
    // Vertices that stay the same (a snapshot shared by every draw of them): kept on the GPU, uploaded once.
    void DrawShared(uint32_t primitive, uint32_t fvf, const std::shared_ptr<const std::vector<uint8_t>>& data,
                    size_t byteOffset, uint32_t vertexCount, const uint16_t* indices, uint32_t indexCount);
    // ... with the indices a snapshot too (kept by the caller while unchanged): retained on the GPU like the vertices.
    void DrawSharedIndexed(uint32_t primitive, uint32_t fvf, const std::shared_ptr<const std::vector<uint8_t>>& data,
                           size_t byteOffset, uint32_t vertexCount,
                           const std::shared_ptr<const std::vector<uint8_t>>& indexData, uint32_t indexCount);
    // Retained meshes (docs/device-on-rvk.md phase 3b): a static mesh - vertex and index snapshots that don't change -
    // registered once under a small id (the caller's: dense, reused after ReleaseMesh), then drawn by it. What every
    // DrawSharedIndexed looks up again (the snapshots' arena slots, the mesh info) is kept with the id.
    void RegisterMesh(uint32_t id, uint32_t primitive, uint32_t fvf, const std::shared_ptr<const std::vector<uint8_t>>& vertices,
                      size_t byteOffset, uint32_t vertexCount, const std::shared_ptr<const std::vector<uint8_t>>& indices,
                      uint32_t indexCount);
    void ReleaseMesh(uint32_t id);
    void DrawMesh(uint32_t id);
    // A character piece skinned by `job` (skinned here if no one did yet): exactly a character, its box known.
    void DrawSkinned(uint32_t primitive, uint32_t fvf, skin::Job& job, uint32_t startVertex, uint32_t vertexCount,
                     const uint16_t* indices, uint32_t indexCount);
    void DrawPrimitiveVB(uint32_t primitive, VertexBuffer* vb, uint32_t startVertex, uint32_t vertexCount);
    void DrawIndexedPrimitiveVB(uint32_t primitive, VertexBuffer* vb, uint32_t startVertex, uint32_t vertexCount,
                                const uint16_t* indices, uint32_t indexCount);

    // ---- resources ----
    Texture* CreateTexture(uint32_t width, uint32_t height, Format format, uint32_t levels);
    // A8R8G8B8 convenience: create one level and upload tightly packed pixels.
    Texture* CreateTexture(uint32_t width, uint32_t height, const void* argbPixels);
    // Uploads one mip level region. x, y, width, height must be multiples of 4 for DXT formats (or reach the
    // level's edge). pitch = bytes per row of pixels (or of 4x4 blocks). The new contents are visible to the
    // whole frame being recorded (uploads run before the frame's draws).
    void UpdateTexture(Texture* texture, uint32_t level, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                       const void* data, uint32_t pitch);
    Texture* CreateRenderTarget(uint32_t width, uint32_t height);   // A8R8G8B8, sampleable
    // GPU copy/stretch between images (render targets, the main target or textures as source; render
    // targets or the main target as destination). Null = main target; null rect = whole image.
    void CopyTexture(Texture* dst, const Rect* dstRect, Texture* src, const Rect* srcRect, bool linear);
    void DestroyTexture(Texture* texture);

    VertexBuffer* CreateVertexBuffer(uint32_t fvf, uint32_t vertexCount);
    void* Lock(VertexBuffer* vb);
    void Unlock(VertexBuffer* vb);
    void DestroyVertexBuffer(VertexBuffer* vb);

    static uint32_t FvfStride(uint32_t fvf);
    bool FormatSupported(Format format) const { return m_formatSupported[size_t(format)]; }

    // Two-step creation (used by ThreadedDevice): a handle without GPU resources, realised later. A texture
    // whose realisation failed (no image) is ignored by SetTexture / SetRenderTarget / UpdateTexture.
    static Texture* NewTexture(uint32_t width, uint32_t height, Format format, uint32_t levels, bool renderTarget);
    bool RealizeTexture(Texture* texture);

    static constexpr uint32_t kMaxLights = 8;
    static constexpr uint32_t kFramesInFlight = 2;

private:
    struct Frame {
        VkCommandBuffer upload = VK_NULL_HANDLE, main = VK_NULL_HANDLE;
        // Depth pre-pass (prepass.cpp): the frame's main commands split at the scene's depth clear - `mainA` up to
        // it, `prepass` (the clear and the depth of the opaque draws that follow), `mainB` from it on; `main` is the
        // one being recorded (mainA unless split). Submitted in that order.
        VkCommandBuffer mainA = VK_NULL_HANDLE, prepass = VK_NULL_HANDLE, mainB = VK_NULL_HANDLE;
        bool split = false;
        // ... and once more where a copy of the scene broke the rendering (the water's: PrepassContinue) - `prepass2`
        // (the depth of the opaque draws after it, over the depth so far), `mainC` from there on.
        VkCommandBuffer prepass2 = VK_NULL_HANDLE, mainC = VK_NULL_HANDLE;
        bool split2 = false;
        // Clouds (clouds.cpp DrawClouds): their shadow map and half-size march, recorded where the sky ends but run
        // between the pre-pass and mainB - outside the scene's rendering, which goes on unbroken (its pre-pass too).
        VkCommandBuffer clouds = VK_NULL_HANDLE;
        bool cloudsRecorded = false;
        // Ray-traced sun shadows (raytrace.cpp RenderRtShadow): recorded where the pre-pass ends, run after it (and
        // the clouds), before mainB - whose draws read their mask.
        VkCommandBuffer rays = VK_NULL_HANDLE;
        bool raysRecorded = false;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore imageAvailable = VK_NULL_HANDLE;
        VkBuffer ring = VK_NULL_HANDLE;
        VmaAllocation_T* ringAllocation = nullptr;
        uint8_t* ringData = nullptr;
        VkBuffer post = VK_NULL_HANDLE;            // the frame lights again, for the post passes (volumetric light)
        VmaAllocation_T* postAllocation = nullptr;
        void* postData = nullptr;
        VkDeviceSize ringOffset = 0;
        VkDeviceSize ringSize = 0;                 // grows (GrowRing) after a frame overflowed it
        uint64_t serial = 0;                       // submission number last signalled through `fence`
        bool uploadsRecorded = false;
        VkBuffer skinArena = VK_NULL_HANDLE;       // GPU skinning's output this frame (skin_gpu.cpp)
        VmaAllocation_T* skinArenaAllocation = nullptr;
        VkDeviceSize skinArenaSize = 0, skinArenaOffset = 0;
    };

    struct LightSlot {
        d3d::Light light{};
        bool enabled = false;
        float cosHalfTheta = 1.0f, cosHalfPhi = 1.0f;   // precomputed for the shader
        uint32_t version = 0;                            // bumped when the light data changes
        uint64_t capturedFrame = ~0ull;                  // CaptureLight: last capture (frame, version)
        uint32_t capturedVersion = 0;
    };

    // What is currently set in the main command buffer, so unchanged state isn't re-issued (every Vulkan
    // call crosses Wine's 32/64-bit boundary). Reset whenever a command buffer begins.
    struct StateCache {
        bool valid = false;
        uint32_t topologyClass = ~0u, topology = ~0u, fvf = ~0u;
        uint32_t cull = ~0u, depthTest = ~0u, depthWrite = ~0u, depthOp = ~0u, blendEnable = ~0u, src = ~0u, dst = ~0u;
        uint32_t depthBounds = ~0u;              // the bounds test on (the backdrop cull)
        float depthBoundsZ = -1.0f;              // ... at [z, z] for this z
        VkViewport viewport{};
        VkRect2D scissor{};
        VkBuffer vb = VK_NULL_HANDLE, ib = VK_NULL_HANDLE;   // the bound vertex (slot 0) / index buffers (BindGeometry)
        // The main pass's per-draw push (bindings 4, 8, 10) as last pushed: an unchanged one isn't pushed again.
        bool drawSetValid = false;
        VkDeviceSize frameLights = 0, prevOffset = 0, prevRange = 0, smoothOffset = 0, smoothRange = 0;
        VkBuffer prevBuffer = VK_NULL_HANDLE, smoothBuffer = VK_NULL_HANDLE;
        bool glowBlendSet = false;               // attachment 1 (glow) blend enabled, HDR pipelines
        uint8_t glowBlendMode = 0;               // ... its equation: 0 not set, 1 adds (effects), 2 occludes (GlowOccluder)
        uint32_t fractionEnable = ~0u, fractionSrc = ~0u, fractionDst = ~0u;   // attachment 2 (local-light fraction)
        uint32_t motionKeep = ~0u;               // attachment 3 (motion vectors): 1 = kept (draw doesn't write)
        uint32_t albedoEnable = ~0u, albedoSrc = ~0u, albedoDst = ~0u;   // attachment 4 (surface colour)
        uint32_t writeMask[5] = {~0u, ~0u, ~0u, ~0u, ~0u};   // dynamic colour write masks (m_dynamicWriteMask)
    };

    bool CreateInstance(std::string* error);
    bool PickDevice(std::string* error);
    bool CreateLogicalDevice(std::string* error);
    bool CreateMainTargets(std::string* error);
    void DestroyMainTargets();
    bool CreateSceneTargets();                   // (RVK_RenderScale's size)
    void DestroySceneTargets();                  // ... and both depth images
    bool CreateSwapchain(std::string* error);
    void DestroySwapchain();
    bool CreatePipelines(std::string* error);
    // The pipeline cache: loaded from its file when the device starts, saved after the pipelines are made, once the
    // game has run a while (the pipelines made on demand), and when the device goes.
    VkPipelineCache m_pipelineCache = VK_NULL_HANDLE;
    size_t m_pipelineCacheSaved = 0;             // the size of what the file holds (loaded or last saved)
    void CreatePipelineCache();
    void SavePipelineCache(const char* why);
    bool CreateFrames(std::string* error);
    Texture* CreateImage(uint32_t width, uint32_t height, Format format, uint32_t levels, bool renderTarget,
                         bool storage = false);
    friend class ThreadedDevice;

    VkDeviceSize Allocate(VkDeviceSize size, VkDeviceSize alignment, void** cpu);
    // Makes sure `bytes` (plus alignment slack) fit in this frame's ring; if not, submits and waits for the
    // work recorded so far and restarts the ring. Call before a group of allocations that belong together.
    void EnsureRingSpace(VkDeviceSize bytes);
    VkSampler SamplerFor(uint32_t stage);
    void Draw(uint32_t primitive, uint32_t fvf, const void* vertices, uint32_t vertexCount,
              const uint16_t* indices, uint32_t indexCount);
    void ApplyDynamicState(uint32_t primitive, uint32_t fvf, uint32_t stride);
    // The draw's vertex buffer (slot 0; slot 1 is the null buffer) and index buffer (null: leave it), each bound only
    // when it differs from what is (StateCache): every bind is a call into the driver (through Wine's 32/64-bit thunk).
    void BindGeometry(VkCommandBuffer cmd, VkBuffer vb, VkBuffer ib);
    StateCache m_cache;
    // The big constant block is reused while nothing that feeds it changes (m_constantsDirty) and the ring
    // it lives in hasn't been restarted (m_ringGeneration counts restarts).
    bool m_constantsDirty = true;
    bool m_pixelLighting = false, m_lightingDebug = false, m_lightOverride = false;
    bool m_carrierLit = true;
    bool m_carrierCasts = true;                  // RVK_OwnShadow: off, a carried light's cube leaves out its carrier
    float m_lightHeadroom = 1.0f;
    float m_bump = 0.0f;
    bool m_normalMaps = true;
    float m_normalStrength = 1.0f;
    Texture* m_flatNormal = nullptr;             // binding 9 when the draw has no normal map
    VkSampler m_normalSampler = VK_NULL_HANDLE;
    Texture* m_constantsNormalMap = nullptr;
    bool m_constantsSkyScenery = false;
    bool m_constantsDecal = false;
    // The last constant block's light list (draw.cpp): reused while the lights (a generation: SetLight / LightEnable
    // changing one), the frame and the list's kind stay. (GpuLight bytes: the type is internal.h's.)
    uint64_t m_lightsGen = 1, m_lightListGen = 0, m_lightListFrame = ~0ull;
    uint32_t m_lightListKind = 0, m_lightListCount = 0, m_lightListLocal = 0;
    std::vector<uint8_t> m_lightList;
    uint32_t m_constantsBlend = ~0u;             // the blend enable the block was written with (Draw may turn it off)
    bool m_opaqueBlend = true;                   // RANDYVK_OPAQUE_BLEND=0: alpha-1 blended draws stay blended (A/B)
    uint64_t m_opaqueBlendDraws = 0;             // (logged with the opaque fast path)
    PbrSettings m_pbr;
    uint32_t m_anisotropy = 1;
    float m_maxAnisotropy = 1.0f;               // GPU limit (0 without the feature)
    // The ground's base pass textures this frame, by chunk (TerrainChunkKey): its lighting pass, drawn later with the
    // lightmap, takes its relief from them. And the sampler they're read with (repeat, mipmapped).
    std::unordered_map<uint64_t, Texture*> m_terrainBases;
    // The native ground (native_ground.cpp): its patches (vertices: XYZ | NORMAL | TEX2, set 0 the cell coordinates,
    // set 1 the playfield's lightmap; a box each), the shared indices, the tile-code texture (each cell's tile's
    // bindless image, its turn), the lightmap (the hour's, through the game's palette).
    struct NativeClutter {                       // a patch's rocks (ground_clutter.cpp): one static mesh
        std::shared_ptr<const std::vector<uint8_t>> vertices, indices;
        uint32_t count = 0;                      // vertices (and indices: one each, unless indexCount)
        uint32_t indexCount = 0;                 // (the pebbles: their own indices)
        bool pebbles = false;                    // the small rocks (drawn nearer); the pebbles' blocks
        uint8_t size = 0;                        // (the pebbles: grit 0, small 1, bigger 2 - each its own range)
        float lo[3] = {}, hi[3] = {};            // (the rocks: the piece's box - each a block of the patch, culled alone)
    };
    struct ClutterBuild {                        // a patch's rocks as made (on a worker)
        std::vector<NativeClutter> pieces;
        uint32_t rocks = 0;
    };
    struct NativePatch {
        std::shared_ptr<const std::vector<uint8_t>> vertices, indices;   // (each cell's diagonal its own)
        std::shared_ptr<const std::vector<uint8_t>> allIndices;   // every cell's (indices: without the holes')
        uint32_t indexCount = 0;                 // indices' (0: all holes - not drawn)
        float lo[3], hi[3];
        std::vector<NativeClutter> clutter;      // its rocks, made when the eye is near
        bool clutterMade = false;
        std::future<ClutterBuild> clutterPending;   // ... being made
    };
    // The pebbles (ground_pebbles.cpp): which tiles get them - per tile how many (0..2: 1 the coarsest gravel's), their
    // size x, and where in it (16 x 16 texels, 1 its ground, 0 grass or another ground's); and how much of it is sand
    // (0..1: its ground taken for sand, times the share of its texels that are that ground - the ripples, terrain.glsl).
    struct PebbleTable {
        std::vector<float> density, size, sand;
        std::vector<uint8_t> mask;
    };
    struct PebbleBlock {                         // a block of 8 x 8 cells' pebbles, made when the eye is near
        std::vector<NativeClutter> pieces;
        std::future<ClutterBuild> pending;
        float lo[3], hi[3];
        bool made = false;
    };
    struct NativeState {
        std::shared_ptr<const NativeGround> ground;
        std::vector<NativePatch> patches;
        uint32_t vertices = 0, indexCount = 0;   // a patch's
        Texture* codes = nullptr;
        Texture* maps = nullptr;                 // each cell's tile's normal map and material map (their bindless images
                                                 // in R, G and B, A; 0xFFFF none)
        Texture* anyOrm = nullptr;               // a tile's material map (the lighting pass's stand-in: PBR on)
        bool anyHeight = false;                  // a tile's normal map has a height in its alpha (parallax.glsl)
        Texture* means = nullptr;                // each cell's tile's mean colour (the cliffs' rock tint, filtered)
        uint64_t mapsSignature = 0;              // the tiles' maps and the settings the two textures were made with
        std::vector<uint8_t> holes;              // each tile: a hole (NativeTileHole) - its cells left out
        uint32_t holeCells = 0;                  // (for the log)
        Texture* lightmap = nullptr;
        Texture* lightmapGrey = nullptr;         // (none handed over yet: a mid grey)
        Texture* lightmapPrev = nullptr;         // the hour's lightmap before (faded out over kLightmapFade seconds)
        std::chrono::steady_clock::time_point lightmapSince;   // when this one came
        float lightFit[4] = {};                  // the lightmap as a + b.n of the slope (FitNativeLight): a, b; a = 0 none
        float sun = 1.0f;                        // RVK_GroundSun
        float rocks = 1.0f;                      // RVK_GroundRocks
        bool clutter = false;                    // the rocks are drawing (ground_clutter.cpp)
        bool tessPatch = false;                  // the patch drawing is near enough to be tessellated (DrawNativeGround)
        uint32_t rocksMade = 0;
        float pebbles = 1.0f;                    // RVK_Pebbles
        std::shared_ptr<const PebbleTable> pebbleTable;   // (made once a playfield, its tiles uploaded)
        uint32_t pebbleTries = 0;
        std::unordered_map<uint32_t, PebbleBlock> pebbleBlocks;   // by block (z x blocks across + x)
        uint32_t pebblesMade = 0;
        bool pebbleDraw = false;                 // the pebbles are drawing (they cast no sun shadows: ground_pebbles.cpp)
        float pebbleRange = 0.0f;                // ... the range of their size (sunk over its last quarter)
        std::vector<uint32_t> lightPixels;       // the lightmap's texels (the grass's light: Device::NativeLightRgb)
        uint32_t lightW = 0, lightH = 0;
        Texture* sky = nullptr;                  // how much of the sky each height sample sees (0..1; RVK_GroundAo)
        std::future<std::shared_ptr<std::vector<uint32_t>>> skyPending;
        float ao = 0.6f;                         // RVK_GroundAo
        bool on = true, drawing = false;
        uint64_t drawnBase = ~0ull, drawnLight = ~0ull;   // the frames each pass was last drawn
        uint32_t patchesDrawn = 0, framesDrawn = 0;
        // One pass (RVK_GroundOne): the base pass deferred to the lighting pass, which draws the ground once - its
        // texture and its light together (DrawNativeGroundOne). The base pass's state, kept for that and for the base
        // pass drawn late, should something other than the lighting pass come next (DrawPendingGroundBase).
        bool basePending = false;
        uint32_t baseSampler = 0;
        std::array<uint32_t, d3d::RS_COUNT> baseRs{};
        std::array<std::array<uint32_t, d3d::TSS_COUNT>, 2> baseTss{};
        Texture* baseTex1 = nullptr;
        d3d::Matrix baseWorld{}, baseView{}, baseProj{};
    } m_native;
    void DrawNativeGroundOne();
    void DrawPendingGroundBase();
    void NoteSceneFog();
    bool m_groundOnePass = true;                 // RVK_GroundOne
    bool m_groundOne = false;                    // the ground's one pass is drawing: its lighting pass (IsMultiplyPass),
                                                 // drawn opaque, the base pass's colour in it (ffp_main)
    uint32_t m_recordExtra[3] = {};              // the draw record's spare words (AppendRecord): the one pass's base slots
    bool NativeGroundReplaces(uint32_t fvf);
    uint64_t NativeMapsSignature() const;
    float SandWindBearing() const;
    // The native ground for the grass's capture (grass.cpp): the colour of the ground at cell coordinates (u, v) - its
    // cell's tile, turned (false while that tile's picture can't be read yet; *tile: its texture all the same) - and its
    // light at lightmap coordinates.
    bool NativeGroundRgb(float u, float v, uint8_t rgb[3], Texture** tile = nullptr) const;
    bool NativeLightRgb(float u, float v, uint8_t rgb[3]) const;
    void PollNativeSky();
    void FitNativeLight();
    void FillNativeLights(detail::FrameLights* fl);
    void MakeNativeCodes();
    static bool NativeTileHole(const Texture* t);
    void ApplyNativeHoles();
    void DrawNativeGround(bool lightPass);
    static ClutterBuild BuildNativeClutter(std::shared_ptr<const NativeGround> ground, uint32_t patch, float density,
                                           std::vector<uint8_t> grassy);
    std::vector<uint8_t> GrassyTiles() const;
    void DrawNativeClutter(const d3d::Matrix& viewProj);
    std::shared_ptr<const PebbleTable> MakePebbleTable();
    bool EnsurePebbleTable();
    static ClutterBuild BuildNativePebbles(std::shared_ptr<const NativeGround> ground, uint32_t bx, uint32_t bz,
                                           float density, std::shared_ptr<const PebbleTable> table);
    void DrawNativePebbles(const d3d::Matrix& viewProj);
    void DropNativePebbles(bool table);
    std::string m_pebbleTablePath;               // randy-vk\terrain\pebbles.tsv (SetPebbleTablePath)
    void DestroyNativeGround();
    // The ground's chunks' texture coordinates fitted to the world (terrain.cpp), by TerrainChunkKey: kept across
    // frames; how many were laid from above and how many not (logged).
    std::unordered_map<uint64_t, TerrainFit> m_terrainFits;
    uint32_t m_terrainFitsAbove = 0, m_terrainFitsOther = 0;
    const TerrainFit& TerrainFitFor(uint64_t key, const void* vertices, uint32_t vertexCount,
                                    const detail::FvfLayout& layout, const uint16_t* indices, uint32_t indexCount);
    void FillTerrainRecord(detail::DrawTransform& dt, const TerrainFit& fit, uint32_t pass, const Texture* base) const;
    // The cliffs' rock (terrain.cpp; embedded): decoded on a worker thread once wanted, uploaded at a frame's start when
    // it is in. Its maps, and its mean colour (the tint to a cell's tile divides by it).
    struct RockMaps;
    struct TerrainRock {
        Texture* albedo = nullptr;
        Texture* normal = nullptr;
        Texture* orm = nullptr;
        float mean[3] = {0.5f, 0.5f, 0.5f};
        bool started = false;
        std::future<std::shared_ptr<RockMaps>> pending;
    } m_rock;
    static std::shared_ptr<RockMaps> DecodeRock();
    void PollTerrainRock();
    void FillTerrainRockLights(float rock[4], float rock2[4]);
    bool TerrainRockOn() const { return m_terrainSettings.rock && m_terrainSettings.cliff > 0.0f && m_rock.albedo; }
    bool TerrainTexturingOn() const
    {
        return m_terrainSettings.cliff > 0.0f || m_terrainSettings.tiling > 0.0f || m_terrainSettings.variation > 0.0f;
    }
    Texture* m_drawBumpBase = nullptr;           // the current draw's (Draw)
    Texture* m_drawOrm = nullptr;                // the current draw's PBR material map (Draw)
    bool m_drawOrmBase = false;                  // ... from the ground's base texture
    Texture* m_drawEmissive = nullptr;           // the draw's emissive map (stage 0's), or null
    // Texture picking (RequestPick): armed by the proxy, active for one frame, published at its end.
    float m_pickX = 0.0f, m_pickY = 0.0f;
    bool m_pickArmed = false, m_pickActive = false;
    Texture* m_pickBest = nullptr;
    float m_pickDepth = 2.0f;
    std::atomic<Texture*> m_pickPublished{nullptr};
    uint32_t m_pickBestVisual = 0, m_pickBestOwner = 0, m_pickBestKind = 0, m_pickBestVertices = 0, m_pickBestIndices = 0;
    std::atomic<uint32_t> m_pickPublishedVisual{0}, m_pickPublishedOwner{0}, m_pickPublishedKind{0},
        m_pickPublishedVertices{0}, m_pickPublishedIndices{0};   // the draw's counts (the workbench matches meshes by them)
    float m_pickBestTriangles[kPickTriangles * 15] = {};   // the nearest hit's triangles (PickResult::triangles)
    uint32_t m_pickBestTriangleCount = 0;
    mutable std::mutex m_pickTriangleMutex;      // (published by the render thread, read by the game's)
    float m_pickPublishedTriangles[kPickTriangles * 15] = {};
    uint32_t m_pickPublishedTriangleCount = 0;
    const void* m_pickSkinned = nullptr;         // a GPU-skinned draw's CPU-skinned vertices, for the pick (DrawSkinned)
    std::atomic<uint32_t> m_pickSerial{0};
    Texture* m_pickHighlight = nullptr;
    // The ground drawn natively (native_ground.cpp): a hit on the game's ground draws there (a stand-in texture, the
    // lightmap) is the tile of the cell under the point - the nearest hit's world point; and the tile flashed.
    bool m_pickBestTerrain = false;
    float m_pickBestWorld[3] = {};
    uint32_t m_pickTile = 0;                     // the picked cell's tile + 1 (0: none)
    uint32_t m_pickHighlightTile = 0;            // ... the one flashing (FL.pick.x)
    void PickDraw(uint32_t primitive, const detail::FvfLayout& layout, const void* vertices, uint32_t vertexCount,
                  const uint16_t* indices, uint32_t indexCount, bool terrain);
    Texture* m_constantsBumpBase = nullptr;
    bool m_drawTerrainBase = false;              // the current draw is the ground's unlit base pass (Draw)
    bool m_drawTerrainLight = false;             // ... or its multiplying lightmap pass
    uint64_t m_opaqueDraws = 0;                  // the opaque fast path's draws (logged every 600 frames)
    // Foliage counters (logged every 600 frames): draws flagged foliage, and how many of those took the far LOD.
    uint64_t m_foliageDraws = 0, m_foliageLodDraws = 0;
    uint64_t m_foliageStatic = 0, m_foliageDynamic = 0, m_foliageVerts = 0;   // survey (RVK_GrassOn off is fine too)
    float m_foliageMinH = 0.0f, m_foliageMaxH = 0.0f;
    uint32_t m_drawFoliage = 0;                  // the current constants' foliage class: 0 none, 1 near, 2 far (LOD)
    VkSampler m_bumpSampler = VK_NULL_HANDLE;
    uint64_t TerrainChunkKey(const void* vertices, uint32_t vertexCount, uint32_t stride, const uint16_t* indices,
                             uint32_t indexCount) const;
    uint32_t m_dumpVertexCount = 0;
    bool m_drawOverbright2x = false;              // the current draw is blended at 2x (F_OVERBRIGHT2X)
    bool Overbright2x(uint32_t fvf) const;
    uint64_t m_frameNumber = 0;
    bool m_frameLightsDirty = true;
    bool m_frameEyeValid = false;
    uint32_t m_frameRoomVisuals = 0, m_frameTerrainVisuals = 0;   // this frame's draws of rooms, of terrain (BeginFrame)
    uint32_t m_indoorRun = 0;                    // frames in a row the other way
    bool m_indoors = false;                      // a room's playfield: no weather                // the camera the frame's light list is chosen from
    float m_frameEye[3] = {}, m_frameForward[3] = {};
    void UpdateFrameEye();
    uint64_t m_frameLightsGeneration = ~0ull;
    VkDeviceSize m_frameLightsOffset = 0;
    VkDeviceSize WriteFrameLights();
    void FillFrameLights(detail::FrameLights* fl, bool dump);
    // The frame lights in binding-4 order, as indices into m_lightsPrev (WriteFrameLights), and which of them the
    // current draw carries.
    std::vector<uint32_t> m_frameLightIndices;
    uint32_t CarriedLight(uint32_t fvf, const void* vertices, uint32_t vertexCount, uint32_t stride) const;
    struct CapturedLight {
        d3d::Light light;
        float cosHalfTheta, cosHalfPhi;
        bool hasCarrier = false;                 // a character carries it (FindCarriers): its origin
        uint32_t carrierGroup = ~0u;             // the ShadowItem group (run of draws) of that origin this frame
        std::vector<uint32_t> carrierGroups;     // every group that is the carrier's (sorted): the point shadow pass
        float carrier[3] = {};
        float carried = 0.0f;                    // 0..1: counted as a character's light (smoothed over frames)
        uint32_t owner = 0;                      // its carrier (SetDrawVisual owner) when the scene's lights are known
        uint32_t carrierOwner = 0;               // the carrier's owner: owner, else that of the piece found under it
        uint8_t ownerHeld = 0;                   // frames carrierOwner has been kept with the carrier not found
    };
    // End of frame: for each light, the character carrying it - the body origin nearest under it. The game places a
    // character's light up to ~0.4 sideways off the body while it moves, so a fixed radius around the light either
    // misses the carrier or catches characters next to it.
    void FindCarriers();
    static bool IsCarrierPart(const CapturedLight& c, const d3d::Matrix& world, const float boundsMin[3],
                              const float boundsMax[3]);
    struct ShadowItem;
    static bool IsCarrierItem(const CapturedLight& c, const ShadowItem& item);
    std::vector<CapturedLight> m_lightsCur, m_lightsPrev;   // point / spot lights used this / last frame
    void CaptureLight(LightSlot& slot);

    // Sun shadows (shadow.cpp)
    uint32_t m_shadowSize = 4096;                // sun shadow map size (pixels, each cascade)
    uint32_t m_pointShadowSize = 1024;           // point light shadow cube face size (pixels)
    uint32_t m_shadowSizeWanted = 4096, m_pointShadowSizeWanted = 1024;   // SetShadowResolution: at the next frame
    bool CreateShadowMap(std::string* error);    // the sun's map image and views (m_shadowSize)
    void DestroyShadowMap();
    bool CreatePointShadowMaps(std::string* error);   // the cube array and views (m_pointShadowSize)
    void DestroyPointShadowMaps();
    void ApplyShadowResolution();                // at a frame's start: recreates maps whose size changed
    static constexpr uint32_t kShadowCascades = 4;   // layers of the sun shadow map, each 3x the area of the last
    // ... and after them the ground grass's own (grass_shadow.vert; the nearest cascade's square): receivers weigh it
    // apart from the other casters' (a field shading itself in full would be far darker than its ground).
    // ... and, with ray-traced shadows (RVK_RtShadow), the cascades again holding only the casters the rays lack
    // (characters, plants, cut-outs; m_shadowUntraced): what the traced pixels take with the rays' mask.
    static constexpr uint32_t kGrassShadowLayer = kShadowCascades, kUntracedLayer = kShadowCascades + 1,
                              kShadowLayers = 2 * kShadowCascades + 1;
    uint32_t m_shadowLayerCount = kShadowCascades + 1;   // the map's layers now (the untraced ones only while wanted)
    bool m_shadowUntraced = false;               // the map's untraced layers were drawn with it (the next frame's mask)
    static constexpr float kCasterCacheRange = 60.0f; // remembered casters: kept within 3x, forgotten beyond 4x - at least
    // As far as the sun's shadows reach (the widest cascade: its half width, and its centre ahead of the camera): a
    // caster further off than 180 units was never remembered, and its shadow on a cliff in view went when the camera
    // looked away from it (user, 2026-10-14). Forgotten beyond 4/3 of it.
    float CasterKeepRange() const { return std::max(3.0f * kCasterCacheRange, 1.4f * m_shadowRange); }
    struct ShadowCaster {
        uint32_t primitive, stride, vertexCount, indexCount;
        VkDeviceSize vbOffset, ibOffset;
        VkBuffer vb, ib;                         // VK_NULL_HANDLE: the frame's ring (else: skinned on the GPU)
        d3d::Matrix world;
        uint64_t generation;
        Texture* texture;                        // alpha-tested casters: texture 0, its coordinates' offset, ref
        int texOffset;
        float alphaRef;
        uint64_t key;                            // caster cache identity
        uint32_t view;                           // index into m_casterViews
        float boundsMin[3], boundsMax[3];        // model space
        uint32_t draw;                           // m_frameDraw when the game drew it
        float sway[4];                           // a plant's sway (DrawTransform sway), 0 = still
        bool animated;                           // its vertices changed since last frame (a character's body)
        uint32_t owner;                          // the character it belongs to (SetDrawVisual), 0: unknown
        uint32_t kind;                           // VisualKind of the visual that drew it (m_drawVisualKind)
        uint64_t leafKey;                        // a canopy with leaves: its leaf set (m_leafSets), 0 = none
        uint64_t rtPlace;                        // its instance in the ray tracing (raytrace.cpp RtTraced), 0 = none
    };
    // One caster as the shadow passes draw it: from this frame's ring or from the caster cache (own buffer).
    struct ShadowItem {
        VkBuffer buffer;                         // VK_NULL_HANDLE: the frame's ring
        VkBuffer ibBuffer;                       // the indices' buffer; VK_NULL_HANDLE: `buffer` (or the ring)
        VkDeviceSize vbOffset, ibOffset;         // byte offsets in their buffers
        uint32_t primitive, stride, vertexCount, indexCount;
        Texture* texture;
        int texOffset;
        float alphaRef;
        d3d::Matrix world;
        float boundsMin[3], boundsMax[3];        // world space
        bool cached;                             // remembered, not drawn by the game this frame
        uint32_t group;                          // drawn as part of the same object (character); ~0u = cached
        float sway[4];                           // a plant's sway, 0 = still
        bool animated;                           // ShadowCaster animated
        uint32_t owner;                          // ShadowCaster owner
        uint32_t kind;                           // ShadowCaster kind (VisualKind)
        uint64_t leafKey = 0;                    // ShadowCaster leafKey
        bool traced = false;                     // the rays hold it (RtTraced): not in the untraced layers
        // Made at its first draw this frame and shared by every cascade and cube face it is drawn into (the pass's
        // light matrix is a push constant): its record (kNoShadowRecord: the arena was full) and its group key.
        uint32_t record = ~0u;
        uint64_t key = 0;
        uint32_t cascades = 0;                   // (frame dump: the cascades it was drawn into, a bit each)
        uint64_t cacheKey = 0;                   // (frame dump: its caster cache key)
    };
    static constexpr uint32_t kNoShadowRecord = ~1u;
    std::vector<ShadowItem> m_shadowItems;       // EndFrame: what the shadow passes draw
    std::vector<int32_t> m_carrierHead, m_carrierNext;           // FindCarriers: drawn items by 1-unit cell (buckets)
    std::vector<std::pair<uint32_t, uint32_t>> m_carrierOwners;  // ... by owner
    std::vector<uint32_t> m_carrierNear;                         // ... a query's items
    // What a shadow pass has bound, so unchanged state isn't re-issued.
    struct ShadowBind {
        int pipeline = -1, texOffset = -2;
        uint32_t stride = ~0u, primitive = ~0u;
        Texture* texture = nullptr;
        VkBuffer vb = VK_NULL_HANDLE, ib = VK_NULL_HANDLE;
        VkDeviceSize ibOffset = ~0ull;
    };
    void CollectShadowItems();
    void GroupShadowItems(const std::vector<uint32_t>& drawOf);
    void DrawShadowItem(VkCommandBuffer cmd, ShadowBind& bind, ShadowItem& item);
    void ShadowPassMatrix(VkCommandBuffer cmd, const d3d::Matrix& lightViewProj);   // the cascade's / face's, pushed
    // M4: shadow casters are batched like the scene's draws - one record per caster (read by gl_InstanceIndex) and
    // one indirect call per group of casters that share their bindings.
    void BindShadowItem(VkCommandBuffer cmd, ShadowBind& bind, const ShadowItem& item);
    void BindShadowRecords(VkCommandBuffer cmd);          // binding 1 (the records array), once a shadow pass
    void ShadowCommand(VkCommandBuffer cmd, const ShadowItem& item, uint32_t recordIndex, uint64_t key);
    void FlushShadowGroup(VkCommandBuffer cmd);
    void FinishShadowFrame();
    // The cameras casters were drawn with this frame; the shadow map follows the one most casters share (the
    // world's), so 3D interface elements drawn with their own camera neither move the map nor cast into it.
    struct CasterView { d3d::Matrix view, proj; uint32_t count; };
    std::vector<CasterView> m_casterViews;
    uint32_t m_casterViewLast = 0;               // the view most casters matched (RecordShadowCaster's fast path)
    uint64_t m_casterViewGen = 0, m_casterViewFrame = 0;   // ... as of this camera generation, this frame
    d3d::Matrix m_shadowWorldProj{};             // the world camera's projection: only its draws take shadows
    bool WorldCamera() const;
    // (Diagnostics: this frame's casters left out as another camera's, and whether the world camera's projection
    // differed from the last frame's - which the frame's receivers were tested against; frame dumps and the log.)
    uint32_t m_casterOtherView = 0, m_logViewSplitFrames = 0, m_logOtherViewCasters = 0, m_logProjChanges = 0;
    bool m_shadowProjChanged = false;
    d3d::Matrix m_shadowProjBefore{};
    bool m_drawIsLabel = false;                  // the current draw is a name label (IsLabel)
    bool IsLabel(uint32_t primitive, uint32_t fvf, uint32_t vertexCount) const;
    // Name labels drawn during the scene are held back - their vertices and the state they were drawn with - and
    // drawn after the post passes onto the final 8-bit picture (DrawHeldLabels, from EndScene), as the game draws
    // them: flat, no depth of field, motion blur, fog, bloom, tone mapping or TAA; still behind what stands in front.
    struct HeldLabel {
        uint32_t primitive, fvf, vertexCount;
        std::vector<uint8_t> vertices;
        std::array<uint32_t, d3d::RS_COUNT> rs;
        std::array<std::array<uint32_t, d3d::TSS_COUNT>, 2> tss;
        std::array<Texture*, 2> textures;
        d3d::Matrix world, view, proj;
        std::array<d3d::Matrix, 2> texMatrix;
        d3d::Viewport viewport;
    };
    std::vector<HeldLabel> m_heldLabels;
    bool m_drawingHeldLabels = false;
    void DrawHeldLabels();
    uint32_t m_midFrameFlushes = 0;
    bool m_shadows = false;
    float m_shadowStrength = 0.65f, m_shadowRange = 400.0f;
    uint32_t m_shadowCascades = kShadowCascades;
    VkImage m_shadowImage = VK_NULL_HANDLE;
    VmaAllocation_T* m_shadowAllocation = nullptr;
    VkImageView m_shadowView = VK_NULL_HANDLE;   // all cascades (sampled)
    VkImageView m_shadowLayerViews[kShadowLayers] = {};     // one cascade each, and the grass's (rendered)
    VkImageLayout m_shadowImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkSampler m_shadowSampler = VK_NULL_HANDLE;
    VkSampler m_shadowDepthSampler = VK_NULL_HANDLE;   // the same map without comparison
    VkSampler m_cutoutSampler = VK_NULL_HANDLE;   // the shadow passes' cut-out textures (repeating, anisotropic)
    VkDescriptorSetLayout m_shadowSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_shadowPipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_shadowPipelines[2] = {};        // opaque, alpha-tested
    std::vector<ShadowCaster> m_casters;
    float m_sunDir[3] = {}, m_sunLuminance = 0.0f;   // this frame's brightest directional light
    bool m_shadowValid = false;                  // the map holds last frame's shadows
    // Blob shadow draws are dropped (sun shadows on and valid; IsBlobShadow) - for the game thread, which can then
    // skip the blob shadows' own work (ThreadedDevice::BlobShadowsReplaced). Set at each frame's start.
    std::atomic<bool> m_blobShadowsReplaced{false};
    d3d::Matrix m_cascadeViewProj[kShadowCascades] = {};   // world -> each cascade's map
    float m_cascadeTexel[kShadowCascades] = {};  // world size of a texel of each
    float m_cascadeDepth[kShadowCascades] = {};  // world units per unit of each one's depth
    float m_frameSunDir[3] = {0.0f, -1.0f, 0.0f}, m_frameSunColor[3] = {};   // this frame's sun, shadows or not (0: none)
    uint32_t m_cascadeCount = 0;                 // cascades the map holds
    uint64_t m_cascadeFrame = 0;                 // the cascades past the nearest take turns (RenderShadowMap)
    uint64_t m_cascadeDrawnFrame = ~0ull;        // the frame the map was last drawn in
    bool m_cascadeUntraced = false;              // ... with the rays' layers (RVK_RtShadow)
    float m_cascadeCenter[kShadowCascades][3] = {};   // each cascade's centre when it was last drawn
    float m_shadowSunDir[3] = {};
    // The moon (night.cpp, RVK_MoonLight): once the sun is down, a dim cool light from the moon on the land - the
    // shadow map drawn from it, the game's night light (from under the ground) turned to come from it.
    float m_moonLight = 1.0f;                    // RVK_MoonLight (0: none)
    // How dark the game gets (darkness.cpp, RVK_Darkness): what each darkening takes of the setting, this frame.
    struct DarkFactors {
        float tone = 0.5f;                       // the tone mapping's night darkening (the old RVK_NightDark)
        float skyLog = 0.0f;                     // our sky at night, x 10^this
        float clouds = 1.0f;                     // the clouds at night, x
        float moon = 1.0f;                       // the moon's light, x
        float storm = 1.0f;                      // a storm's darkening (StormOvercast), x
        float floor = 0.0f;                      // the land's least brightness as shown (log-average; 0: none)
    };
    DarkFactors m_dark;
    float m_darkness = 0.5f;                     // RVK_Darkness
    float m_darkFloorScale = 1.0f;               // RVK_DarkFloor
    bool m_darkIndoors = false;                  // no sun known (a room's playfield): the tone's darkening half a step less
    float m_darkGain = 1.0f;                     // the floor's exposure gain, eased
    float m_landLuma = -1.0f;                    // the land's brightness as shown, last measured (scene_luma.comp)
    uint64_t m_landLumaFrame = ~0ull;            // ... when
    float m_viewLuma = -1.0f;                    // the whole view's (sky too) in the scene, last measured (the light rig;
                                                 // m_landLuma then the land's alone, as the scene has it)
    uint64_t m_viewLumaFrame = ~0ull;
    bool m_lumaView[kFramesInFlight] = {};       // the measure in that slot is the whole view's
    VkPipeline m_lumaPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_lumaLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_lumaSetLayout = VK_NULL_HANDLE;
    VkBuffer m_lumaBuffer = VK_NULL_HANDLE;      // its results, a slot a frame in flight (host-visible)
    VmaAllocation_T* m_lumaAllocation = nullptr;
    uint8_t* m_lumaCpu = nullptr;
    VkDeviceSize m_lumaStride = 256;
    bool m_lumaMeasured[kFramesInFlight] = {};
    bool CreateDarknessResources(std::string* error);
    void DestroyDarknessResources();
    void UpdateDarkness();
    void UpdateEnvironment(VkCommandBuffer cmd);
    void MeasureSceneLuma(VkCommandBuffer cmd);
    bool m_atmos = true;                         // RVK_Atmos
    // Valley mist (mist.cpp, RVK_Mist).
    float m_mist = 1.0f;                         // RVK_Mist, x (0: none)
    float m_sun = 1.0f;                          // RVK_Sun: the suns' brightness, x (0: none; sun.cpp)
    float m_sunSize = 0.75f;                     // RVK_SunSize: their size, x the game's sprites
    float m_sunBleed = 0.3f;                     // RVK_SunBleed: the share of their glow over the clouds
    uint64_t m_sunDrawnFrame = ~0ull;            // ... drawn this frame (once: the first of the game's sun sprites)
    uint64_t m_sunOverFrame = ~0ull;             // ... their glow over the clouds drawn this frame (the sky's end)
    bool m_gameSun = false;                      // RVK_GameSun: the game's sun sprites drawn (off: our suns alone)
    // The game's sun sprites seen this frame / the last (sun.cpp): where, how big, their colour - grouped into suns.
    struct SunSprite { float dir[3], size, colour[3], rimFade; uint32_t hub; };
    // The sprites as drawn this frame (DrawSun under the clouds, then their glow over them: DrawSunOver).
    struct SunDrawn { float dir[3], size, colour[3], rimFade, seed, low, seen, core; };
    std::vector<SunDrawn> m_sunsDrawn;
    d3d::Matrix m_sunViewProj{};                 // the sky's camera they were drawn with
    // How much of each sprite's core is seen (sun_vis.comp, measured with the clouds; 1 when not): the frame slot's
    // measure written (read when the slot comes round again), the eased value a sprite (the glare follows it).
    bool m_sunVisMeasured[kFramesInFlight] = {};
    uint32_t m_sunVisCount[kFramesInFlight] = {};
    float m_sunSeen[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float m_sunThrough = 1.0f, m_sunThroughTarget = 1.0f;   // the sunlight the clouds let through (eased; MakeCloudSunRay)
    float m_frameSunLuma = 0.0f;                 // the frame sun's brightness before that (the clouds' own light)
    double m_sunSeenClock = -1.0;
    void MeasureSunVisibility(VkCommandBuffer cmd, Texture* clouds);
    std::vector<SunSprite> m_sunSprites;
    uint64_t m_sunSpritesFrame = 0;
    void NoteSunSprite(uint32_t fvf, const void* vertices, uint32_t vertexCount, const uint16_t* indices,
                       uint32_t indexCount);
    void DrawSun();
    void DrawSunOver();
    void DrawSuns(bool over);
    std::vector<float> m_mistSamples;            // this frame's terrain heights near the eye
    float m_mistFloor = 0.0f, m_mistAmount = 0.0f, m_mistWet = 0.0f, m_mistSunPrev = 0.0f, m_mistSunTrend = 0.0f;
    bool m_mistFloorSet = false;
    double m_mistClock = -1.0, m_mistSunPrevClock = 0.0;
    void NoteTerrainHeights(uint32_t fvf, const void* vertices, uint32_t vertexCount);
    void UpdateMist();
    void MistParams(float out[2]) const;
    void FogBankParams(float out[4]) const;
    float m_fogBanks = 1.0f, m_fogBankForce = -1.0f;   // RVK_FogBanks; a test's strength (< 0: none)
    float m_fogBankAmount = 0.0f, m_fogBankDepth = 0.0f;   // the banks now: how much, how deep (world units; eased)
    float m_fogBankDrift[2] = {};                // how far they have drifted (world x, z)
    float m_moonSeenDir[3] = {}, m_moonSeenSize = 0.0f;   // this frame's biggest moon in the sky (towards it)
    uint64_t m_moonSeenFrame = ~0ull, m_skySeenFrame = ~0ull;
    float m_moonPos[3] = {0.0f, 1.0f, 0.0f};     // towards the moon (eased)
    float m_moonDir[3] = {0.0f, -1.0f, 0.0f};    // the direction its light travels
    float m_moonColour[3] = {};                  // its light now (0: none)
    float m_moonFade = 0.0f;                     // 0..1: how much the moon lights (night x its height x the setting)
    float m_shadowLightDir[3] = {0.0f, -1.0f, 0.0f};   // what the shadow map is drawn from: the sun, or the moon
    float m_trueSunDir[3] = {0.0f, -1.0f, 0.0f};   // the sun's light's way, below the horizon too (last seen)
    // Sky weather (sky.cpp): how cloudy, from the clock.
    double m_skyTime = -1.0;                     // a test's sky clock (SetSkyTime; < 0: the wall clock)
    float m_skyZoneLean = 0.0f;                  // the playfield's climate: - clearer, + cloudier (SetSkyPlayfield)
    uint32_t m_skyPlayfield = 0;
    float m_skySheets = 1.0f;                    // the game's sheets' strength by the sky (none when clear)
    double SkyClock() const;
    float SkyCloudiness() const;
    // The sky's cloud cover as the effects take it (0 clear .. 1 overcast): SkyCloudiness x RVK_SkyVary, none without
    // our clouds.
    float CloudCover() const
    {
        return m_cloudParams.enable ? SkyCloudiness() * std::clamp(m_cloudParams.vary, 0.0f, 1.0f) : 0.0f;
    }
    void SkyWind(float out[2]) const;
    void UpdateWind();                           // the frame's shared wind (wind.cpp): direction, strength, gusts, clock
    void UpdateLightning();                      // a strike from the game's glow last frame (rain.cpp)
    void UpdateHeatLightning();
    float m_heatFlash = 1.0f;                    // RVK_HeatFlash, x (0: none)
    bool m_heatForce = false;                    // (tests: tonight whatever the chance)
    double m_heatStart = -1e9, m_heatNext = 0.0; // the last far flash; the next (m_frameClock)
    float m_heatPos[3] = {}, m_heatGain = 0.0f;  // where, how bright
    uint32_t m_heatSeed = 0;
    float m_heatTonight = 0.0f;                  // how much of a heat-lightning night it is (logged when it changes)
    void Lightning(float flash[4], float view[4], float proj[4], float world[4]) const;   // this frame's flash (FrameLights)
    float Gusts() const { return std::max(m_grassGusts, 0.0f) * m_windGusts; }   // RVK_GrassGusts x the storm's
    float m_cloudWindSpeed = 8.0f;               // the clouds' drift (world units a second), eased (UpdateCloudDrift)
    static const char* SkyName(float cloudiness);
    // The sky's twilight and the glow round the sun (sky.cpp; sky_common.glsl): its model, made once a frame
    // (UpdateSkyModel: s0..s3 as sky_common.glsl reads them), drawn over the game's sky once its dome is down.
    float m_twilight = 0.85f;                    // RVK_Twilight: how far the twilight palette replaces the game's sky
                                                 // (above 1: more saturated)
    float m_blueHour = 1.0f;                     // RVK_BlueHour: the blue hour's depth and length after sunset
    float m_afterglow = 1.0f;                    // RVK_Afterglow: how strongly the clouds take the sunset's colours
    float m_blueGrade = 0.0f;                    // the blue hour's grade now (UpdateSkyModel; tonemap.frag FL.dusk.y)
    bool m_starFade = true;                      // RVK_StarFade: the game's stars come out as the sky darkens
    float m_starsShown = 1.0f;                   // ... how much of them now (UpdateSunClock)
    float m_starsAge = 0.0f;                     // seconds the game has drawn its stars without a break
    float m_duskRays = 1.0f;                     // RVK_DuskRays: beams from a low sun through cloud gaps and over hills
    Texture* m_cloudCover = nullptr;             // this frame's clouds at half size (premultiplied: alpha their cover)
    uint64_t m_cloudCoverFrame = ~0ull;
    float DuskRaysNow() const;                   // RVK_DuskRays x how low the sun is (volume.frag P.shafts.w)
    float m_mornHaze = 1.0f;                     // RVK_MornHaze: the haze round sunrise thicker, x (hdr.cpp LandHaze)
    float m_dawnLight = 1.0f;                    // RVK_DawnLight: a low sun's light warm, the shade cool, x
    float m_goldenHour = 0.5f;                   // RVK_GoldenHour: the grade round sunrise and sunset (FL.dawn[1].w)
    float m_dawnTint[2][4] = {{1, 1, 1, 0}, {1, 1, 1, 0}};   // ... this frame's: the sun's, the sky's (UpdateSkyModel)
    float m_skyGlow = 1.0f;                      // RVK_SkyGlow: the glow round the sun and along the horizon, x
    float m_skyModel[16] = {};
    float m_skyFog[3] = {};                      // the game's fog colour, eased (the model's horizon)
    bool m_skyFogSet = false;
    double m_skyFogClock = -1.0;
    uint64_t m_skyDrawFrame = ~0ull, m_skyGlowFrame = ~0ull;
    uint32_t m_skyDraws = 0;                     // the sky's draws so far this frame (the glow before the second)
    VkPipeline m_skyGlowPipeline = VK_NULL_HANDLE, m_skyGlowFarPipeline = VK_NULL_HANDLE;   // (far: into a street portal)
    VkPipelineLayout m_skyGlowLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_skyGlowSetLayout = VK_NULL_HANDLE;   // (the sky view table)
    // Our own sky (atmosphere.cpp; RVK_ProcSky): the atmosphere's tables, the sky view's frame; the dome's draw skipped.
    bool m_procSky = true;                       // RVK_ProcSky
    float m_skyZone = 0.25f;                     // RVK_SkyZone: how much of the zone's own sky colour is kept
    float m_desertAir = 1.0f, m_heatShimmer = 0.5f;   // RVK_DesertAir, RVK_HeatShimmer (SetDesertAir)
    float m_skyBright = 1.0f;                    // RVK_SkyBright: the sky's brightness against the game's
    Texture* m_atmosTrans = nullptr;
    Texture* m_atmosMulti = nullptr;
    Texture* m_atmosView = nullptr;
    VkPipeline m_atmosTransPipeline = VK_NULL_HANDLE, m_atmosMultiPipeline = VK_NULL_HANDLE,
               m_atmosViewPipeline = VK_NULL_HANDLE;
    bool m_atmosReady = false;
    float m_atmosTune[2] = {};
    float m_skyDayFogL = 0.7f;                   // the game's sky by day: its fog's brightness (eased)
    uint64_t m_atmosViewFrame = ~0ull;
    uint32_t m_domeSkipVerts = 0, m_domeSkipTexture = 0;   // the dome last skipped (logged when it changes)
    bool m_bandSkipLogged = false, m_tintSkipLogged = false, m_layersSkipLogged = false;
    // The sky's horizon all round (FL.skyRing), read back from the sky view table a few frames late: the haze, the far
    // fog, the mist and the clouds low in the sky fade into it (sky_common.glsl SkyHorizonRing).
    VkBuffer m_atmosRingBuffer = VK_NULL_HANDLE;
    VmaAllocation_T* m_atmosRingAllocation = nullptr;
    uint8_t* m_atmosRingCpu = nullptr;
    bool m_atmosRingWritten[kFramesInFlight] = {};
    float m_skyRing[8][4] = {};                  // rgb, w 1 when known
    float m_skyUp[4] = {};                       // the sky over the land (overhead, 35 degrees up), w 1 when known
    // The sky's colours (docs/sky-plan.md §1): the settings; the ratios to the physics this frame (overhead, 35
    // degrees up, low towards the sun, low away from it) and those each frame slot's table was made with (to take
    // them back out of its readback); the physical sky there, so read back (w 1 when known).
    SkyColours m_skyColours;
    // Our night sky (docs/sky-plan.md §2-4: sky.cpp DrawSkyGlow, sky_glow.frag): its settings; how much of it the dark
    // shows (UpdateSunClock); the game's points left out (logged once); the shooting star and the aurora now.
    NightSky m_nightSky;
    float m_starsDark = 0.0f;
    bool m_starsSkipLogged = false;
    // The shooting star now (sky_glow.frag Meteor): its start (xyz) and how far its head is along its path (rad); the
    // axis the path turns about (xyz) and where it burns out (rad; < 0 none); how fast its train fades (rad of the
    // head's travel), its width (pixels), brightness, a seed; its colour.
    float m_meteor[4][4] = {};
    float m_auroraNow = 0.0f;
    void UpdateNightSky();
    // A rainbow (UpdateRainbow; tonemap.frag Rainbow): the setting; how strong now (eased); when it last rained.
    float m_rainbowSetting = 1.0f, m_rainbowForce = -1.0f, m_rainbow = 0.0f;
    double m_rainbowRainClock = -1e9;
    void UpdateRainbow();
    float m_skyKeyRatio[4][3] = {{1, 1, 1}, {1, 1, 1}, {1, 1, 1}, {1, 1, 1}};
    float m_skyKeyApplied[kFramesInFlight][4][3] = {};
    float m_skyPhys[4][4] = {};
    void SkyKeyRatios(float out[4][3]) const;
    // RANDYVK_HEAPCHECK (grass.cpp): the heap walked at points, the first broken one logged.
    int HeapCheckLevel() const { return m_heapCheck; }
    void HeapCheck(const char* where, int level);
    int m_heapCheck = 0;                         // RVK_HeapCheck
    bool m_heapBroken = false;
    const char* m_heapLastOk = nullptr;
    // The sun's colour through the air (unit luma); transmittance: how much of its light gets through (luma), if asked.
    void SunThroughAir(float up, float out[3], float* transmittance = nullptr) const;
    void OurSkyFog(float rgb[3]) const;          // the scene fog's colour into our sky's horizon (its mean), if on
    bool ProcSkyReady() const;
    // What the game fogs fades into our sky's horizon instead of the zone's fog colour: our sky drawn, outdoors (the sun
    // known). Indoors - the alien ships' green, a dungeon's - the zone's own colour stays: there is no sky to match.
    bool OwnSkyFog() const;
    void UpdateAtmosphere(VkCommandBuffer cmd);
    void DestroyAtmosphere();
    void UpdateSkyModel();
    void DrawSkyGlow(const VkRect2D* box = nullptr);   // (box: a street portal's opening, at the far plane only)
    // The sun's clock (sky.cpp UpdateSunClock; docs/dusk-plan.md): the true sun's direction eased over real time (the
    // game's light, below the horizon too: m_trueSunDir), and from its height the dusk's phases - one smooth day to
    // night that the sky, the light handover and the grading all read, instead of each its own narrow ramp.
    double m_sunClockLast = -1.0, m_sunLightClock = -1.0;
    float m_frameDt = 0.0f;                      // real seconds since the last frame (0..0.25)
    // The world's conditions (sky.cpp UpdateConditions, with the sun's clock - docs/audit-2026-10.md §1): every
    // feature's "how much is it night", each named and set here once instead of worked out again where it is read.
    // Each ramp was tuned against the game on its own and keeps its span (sun heights are sines: 0.035 ~2 degrees, 0.14
    // ~8, 0.24 ~14). A feature's own window (the morning haze, the sunset rays) stays with the feature; the weather's
    // ramps, which change within a frame, are functions read live (CloudCover, StormDeck, StormFull, ClearSky).
    struct Conditions {
        bool sunKnown = false;   // a sun seen lately (none: a room's playfield, a zone without one)
        float sunUp = 1.0f;      // the true sun's height (the sine of its elevation; eased: UpdateSunClock)
        float golden = 0.0f;     // the golden hour: ~15 degrees up to the horizon and a little under (FL.phase.x)
        float afterglow = 0.0f;  // the horizon to ~8 degrees under, most within the first 2 (FL.phase.y)
        float blueHour = 0.0f;   // ~2 to ~15 degrees under, all of it from 5 to 11 (FL.phase.z)
        float night = 0.0f;      // the sky's night: ~2 to ~14 degrees under (FL.phase.w)
        float moonNight = 0.0f;  // the moon's light coming in: ~2 to ~8 under (our sun's ends at the horizon: never both)
        float cloudNight = 0.0f; // the clouds' night (the sky's light on them raised): ~2 up to ~11 under (FL.cond.x)
        float deepNight = 0.0f;  // past the blue hour: ~8 to ~14 under (RVK_Darkness's sky and clouds; 1 without a sun)
        float gameStars = 1.0f;  // the dark the game's stars show in: ~6 to ~14 under (1 without a sun)
        float ourStars = 1.0f;   // ... our own: ~2 to ~14 under, or with RVK_StarFade off none only by day (1 without)
        float heatNight = 0.0f;  // dark enough for heat lightning: from ~2 under, linear to ~7 (0 without a sun)
        float grade = 0.0f;      // the grading's night (NightAmount): `night`, or by the game's light without a sun;
                                 // eased over ~3 s
    };
    Conditions m_cond;
    void UpdateConditions();
    // The light rig (light_rig.cpp, docs/light-rig-plan.md): one account of the frame's light, day and night alike -
    // the sun, the sky and the moon by the sun's height (physical shapes: the air's transmittance, the sky's light
    // through twilight), the storm's share taken from each, the eye's adaptation over all of it - in the game's units
    // (each zone's own day light, measured by day, is what noon is) and exposed already: what a surface lit by it shows.
    // So far only logged, beside what the game gives now (its sun and ambient: the gains the rig would apply).
    struct LightRig {
        bool on = false;          // a sun known (outdoors); off: the game's light as it is
        float sunUp = 1.0f;       // the sun's height (m_cond.sunUp)
        // Physical, relative to the noon sun's light on a surface facing it (sky: to the noon sky's on level ground).
        float sunN = 1.0f;        // the sun's light on a surface facing it (the air's transmittance; its disc setting)
        float sky = 1.0f;         // the sky's light on level ground
        float moonN = 0.0f;       // the moon's on a surface facing it
        float land = 1.0f;        // all of it on level ground, lux (the adaptation's measure)
        float adapt = 1.0f;       // the eye's gain: (land / noon's) ^ -alpha, eased (RVK_Darkness: alpha)
        // The key light: the sun or the moon, whichever lights more (they cross where both are next to none).
        float keyDir[3] = {0.0f, -1.0f, 0.0f};   // the way its light travels
        float keySun = 1.0f;      // 1 the sun's .. 0 the moon's
        float storm = 0.0f;       // a thunderstorm's overcast (StormOvercast), taken from the sun and the sky alike
        // Exposed, in the game's units: the key light on a surface facing it, the sky's light (an ambient), and the
        // key's share of the light on level ground (what a shadow can take).
        float key = 1.0f, ambient = 0.4f, keyShare = 0.7f;
        // The sun's share of the land's light (its light on level ground over that and the sky's, clear air), over
        // noon's: 1 at noon, ~0.5 with the sun 5 degrees up, none at the horizon - how much a shadow, a hill's shade, a
        // cloud's take (FL.cond.y): the difference between sun and shade dies away as the sun goes down.
        float sunShare = 1.0f;
        // The ground's lightmap round the camera (its rgb, eased; the ground grass's capture of it) and the sky's part
        // of it (all of it by night; by day what the sun's share leaves, physical; a storm's dimmer sky): the light
        // the ground has over what lit things got - its baked sky -, given to them too (FL.rigSky).
        float groundLight[3] = {}, groundSky[3] = {};
        bool groundKnown = false;
        // The land's exposure (RigLandGain, tonemap.frag: the darks and middle tones, the bright kept): the physical
        // target for the hour - the clear sky's, sun's and moon's light over noon's, to the power 1 - alpha (the eye's
        // adaptation) - over how bright the game's own light makes the land now, against the zone's noon (its
        // lightmap round the camera and its ambient). Storms are left out of both: their darkness is in our lighting.
        // In place of the night's darkening by NightAmount and RVK_Darkness, and the darkness floor.
        float landTarget = 1.0f, landGame = 1.0f, landGain = 1.0f;
        // The camera's metering (the one exposure for everything, on top of the land's): the whole view's brightness
        // over the land's (view), the zone's by day (viewRef), the exposure from them (meter: tone mapping's).
        float view = -1.0f, viewRef = 1.0f, meter = 1.0f;
        bool metered = false;
        // The zone's day light (its game sun's and ambient's luma, the ambient at noon): what the rig is in units of.
        uint32_t zone = 0;
        float zoneSun = 1.0f, zoneSky = 0.4f;
        bool zoneMeasured = false;
        // What the game gives now (its sun's luma - by night its light from under the ground -, its ambient's) and the
        // rig over it.
        float gameSun = 0.0f, gameAmbient = 0.0f, sunGain = 1.0f, skyGain = 1.0f;
        // The game's sun on lit things (RigGameLight): its own light down to ~10 degrees up, then the air's and its
        // disc's setting - to none a degree under.
        float sunFade = 1.0f;
    };
    LightRig m_rig;
    struct RigZone {
        float sun = 1.0f, sky = 0.4f, land = 1.0f, view = 1.0f;
        bool landSeen = false, viewDay = false;
    };
    std::unordered_map<uint32_t, RigZone> m_rigZones;   // each zone's day light, measured by day
    bool m_rigAdaptSet = false, m_rigGainSet = false;
    double m_rigSteadySince = -1.0;              // since when the zone and the game's ambient have been seen steadily
    void UpdateLightRig();
    // A game light as the rig has it (lit draws, the light override): false - not a light the rig changes. The game's
    // directional light from under the ground (its night light) gone, its sun faded out over its last degrees.
    bool RigGameLight(const d3d::Light& game, d3d::Light* out) const;
    float m_sunRising = 1.0f;                   // 1 rising .. 0 setting, eased (the trend over minutes)
    bool m_nightSet = false;
    void UpdateSunClock();
    float SunUp() const;                         // the eased sun's height (the sine of its elevation)
    bool SunKnown() const { return m_sunLightClock >= 0.0 && m_frameClock - m_sunLightClock < 5.0; }
    // The game's sky as it goes dark (the dusk log): its dome (the biggest lit sky mesh) and stars (point lists).
    struct DuskSky { uint32_t domeVerts = 0, domeTexture = 0, domeAmbient = 0, stars = 0, starVerts = 0; uint64_t frame = ~0ull; };
    DuskSky m_duskSky, m_duskSkyLast;
    void NoteDuskSky(uint32_t primitive, uint32_t vertexCount);
    bool m_shadowIsMoon = false;                 // ... the moon (FrameLights moon.w)
    bool m_drawIsSkyMesh = false;                // the current visual is a mesh in the sky (a moon?)
    void NoteSkyMesh(uint32_t fvf, const void* vertices, uint32_t vertexCount);
    void UpdateMoon();
    bool MoonOn() const { return m_moonFade > 0.0f; }
    bool MoonScaledGameLight(const d3d::Light& game, d3d::Light* out) const;
    d3d::Light MoonAsLight() const;
    float m_sunColor[3] = {}, m_shadowSunColor[3] = {};   // this frame's sun / the shadow map's
    bool CreateShadowResources(std::string* error);
    void DestroyShadowResources();
    void PrepareShadowMap(VkCommandBuffer cmd);
    void RecordShadowCaster(uint32_t primitive, uint32_t fvf, uint32_t stride, const void* vertices, uint32_t vertexCount,
                            VkDeviceSize vbOffset, const uint16_t* indices, uint32_t indexCount, VkDeviceSize ibOffset,
                            VkBuffer vb = VK_NULL_HANDLE, VkBuffer ib = VK_NULL_HANDLE);
    // Static casters remembered across frames, so objects the camera turned away from (and the game therefore
    // no longer draws) keep casting. A caster drawn unchanged for kPromoteFrames frames in a row is copied here.
    static constexpr uint32_t kPromoteFrames = 20, kMaxCachedCasters = 16384;
    // (Diagnostics, a frame's: streaks begun, and casters refused by the cache, by why.)
    uint32_t m_cacheStreakStarts = 0, m_cacheRefusedFar = 0, m_cacheRefusedFull = 0, m_cacheRefusedNoVertices = 0;
    uint32_t m_cacheRefusedMoving = 0;
    // Where characters stand (their bodies' draws, RCATMesh_t): this frame's and the last's. Their gear is drawn as
    // plain meshes (RTriMesh_t, no owner) following their bones - small casters inside a character's space aren't
    // remembered (an idle character's gear, re-keyed as it shifts, blinked in the remembered shadows - user).
    std::vector<std::array<float, 3>> m_characterSpots, m_characterSpotsPrev;
    bool NearCharacter(const float centre[3]) const;
    float m_cacheRefusedFarD = 0.0f;
    uint32_t m_cacheKeptSolid = 0;               // (since start)
    uint32_t m_cacheTraceLines = 0;              // (the churn trace's lines so far: 300 at most)
    uint32_t m_cacheCollisionsLogged = 0;        // (solid casters seen changing pose: logged, 20 at most)
    uint32_t m_shadowMemory = 1;                 // RVK_ShadowMem: remembered casters drawn into the sun's shadows
    uint32_t m_memoryTraceFrames = 0;            // (frames left of the counts' trace)
    bool m_hillShade = true;                     // RVK_HillShade: the terrain's shade from a low sun (terrain_sun.cpp)
    bool m_lightRig = true;                      // RVK_LightRig: one light for day and night (light_rig.cpp)
    float m_metering = 0.5f;                     // RVK_Metering: the camera metering's power of the view over the land
    TerrainSettings m_terrainSettings;           // RVK_Terrain*: the ground's own texturing (terrain.cpp)
    bool m_shadowViewDebug = false;              // RVK_ShadowView: the sun shadow map's say painted on the scene
    // The next draws only cast (native mesh.cpp: static meshes the game culled, within the shadows' reach): recorded as
    // sun shadow casters, not drawn (Draw). How many this frame (logged).
    struct ArenaChunk;
    struct CachedCaster {
        // Its vertices then indices in a slot of the caster arena (shared buffers: casters batch together).
        ArenaChunk* chunk;
        VkDeviceSize slot, slotBytes;            // the slot (freed with ArenaFree)
        VkDeviceSize vbOffset, indexOffset;      // in the chunk's buffer (vbOffset a multiple of the stride)
        uint32_t primitive, stride, vertexCount, indexCount;
        d3d::Matrix world;
        float boundsMin[3], boundsMax[3];        // world space
        Texture* texture;
        int texOffset;
        float alphaRef;
        uint64_t lastSeen;
        double lastSeenTime;                     // seconds (SwayClock)
        // Swaying (world matrix changing while it is seen): a recording of the matrix at kSwayStep intervals,
        // played back and forth while it is out of view so its shadow keeps moving.
        std::vector<d3d::Matrix> sway;
        double lastSwaySample;
        float plantSway[4];                      // a plant's wind sway (as ShadowCaster sway)
        uint32_t kind;                           // ShadowCaster kind (VisualKind)
        uint64_t leafKey;                        // ShadowCaster leafKey
        uint64_t rtPlace;                        // ShadowCaster rtPlace
        uint64_t itemKey;                        // its shadow item's batch key (ShadowItemKey), 0: not yet
        Texture* itemKeyTexture;                 // ... made with this cut-out texture (null: solid)
    };
    static constexpr double kSwayStep = 0.05;
    static constexpr size_t kSwaySamples = 80;
    // (at: where it stood when the streak began; moved: it has moved since - a thing that moves isn't remembered.)
    struct CasterStreak { uint64_t lastFrame = 0; uint32_t count = 0; float at[3] = {}; bool moved = false; };
    // The remembered casters, dense (a frame walks all of them: CollectShadowItems, UpdateCasterCache - through a
    // node map that was most of their time), found by key through an index. Erasing moves the last entry into the
    // gap, growing may move them all (emplace says so): pointers into it hold only while m_casterCacheEpoch stays.
    struct CasterCacheStore {
        using Entry = std::pair<uint64_t, CachedCaster>;   // (key, entry), as a map's
        std::vector<Entry> entries;
        std::unordered_map<uint64_t, uint32_t> index;
        size_t size() const { return entries.size(); }
        void clear() { entries.clear(); index.clear(); }
        size_t count(uint64_t key) const { return index.count(key); }
        CachedCaster* find(uint64_t key)
        {
            auto it = index.find(key);
            return it == index.end() ? nullptr : &entries[it->second].second;
        }
        // True: the entries moved (the vector grew).
        bool emplace(uint64_t key, const CachedCaster& e)
        {
            if (index.count(key)) return false;
            const bool moves = entries.size() == entries.capacity();
            index.emplace(key, uint32_t(entries.size()));
            entries.emplace_back(key, e);
            return moves;
        }
        void erase(uint64_t key)
        {
            auto it = index.find(key);
            if (it == index.end()) return;
            const uint32_t at = it->second;
            index.erase(it);
            if (at + 1 != entries.size()) {
                entries[at] = std::move(entries.back());
                index[entries[at].first] = at;
            }
            entries.pop_back();
        }
        std::vector<Entry>::iterator begin() { return entries.begin(); }
        std::vector<Entry>::iterator end() { return entries.end(); }
        std::vector<Entry>::const_iterator begin() const { return entries.begin(); }
        std::vector<Entry>::const_iterator end() const { return entries.end(); }
    };
    CasterCacheStore m_casterCache;
    // Placed objects (statels.cpp): every piece of the playfield's static objects, placed; a grid (x, z) over them.
    // (opaque: of a cut-out piece's mask, the share kept by its alpha test - a roof's cut-out mostly whole, a canopy's
    // leaves not; 1 for a solid piece.)
    struct StatelPiece { uint32_t firstVertex, vertexCount, firstIndex, indexCount; Texture* texture; float alphaRef, opaque; };
    struct StatelItem { d3d::Matrix world; float boundsMin[3], boundsMax[3]; uint32_t piece, statel; uint8_t kind; };
    struct StatelPlaced { float x, y, z; int yaw, pitch, roll; float scale; uint32_t mesh; };   // (for the log)
    std::vector<StatelPiece> m_statelPieces;
    std::vector<StatelItem> m_statelItems;
    std::vector<StatelPlaced> m_statelPlaced;
    std::vector<Texture*> m_statelMasks;
    std::vector<float> m_statelCpuPositions;     // the pack's vertex positions (x y z) and indices, for the CPU (the
    std::vector<uint16_t> m_statelCpuIndices;    // sand's shelter map)
    uint32_t m_statelGeneration = 0;             // one more each load / unload
    void StatelItemsIn(float x0, float z0, float x1, float z1, std::vector<uint32_t>& out);
    VkBuffer m_statelBuffer = VK_NULL_HANDLE;    // vertices (position, uv: 20 bytes), then the indices
    VmaAllocation_T* m_statelAllocation = nullptr;
    VkDeviceSize m_statelIndexBase = 0;
    float m_statelGridOrigin[2] = {};
    uint32_t m_statelGridW = 0, m_statelGridH = 0;
    std::vector<uint32_t> m_statelCellStart, m_statelCellItems, m_statelStamp;
    std::vector<uint64_t> m_statelItemKey;       // each item's shadow batch key (ShadowItemKey), 0: not yet
    uint32_t m_statelStampNow = 0, m_statelPlayfield = 0, m_statelMode = 1, m_statelItemsThisFrame = 0;
    // The placement, of turned statels [0] and of stretched ones [1] (fences, walls): 1 confirmed, -1 refused, 0 not
    // yet (for the session).
    int m_statelVerified[2] = {};
    uint32_t m_statelMatched[2] = {}, m_statelMismatched[2] = {};
    bool m_statelHasKind[2] = {};                // the playfield has statels of the kind
    bool StatelKindCasting(int kind) const;
    std::string StatelPlacementState() const;
    void DestroyStatels();
    void AddStatelShadowItems();
    void CheckStatelDraw(const d3d::Matrix& world, uint32_t vertexCount, uint32_t indexCount);
    // A drawn piece that is one of ours (same counts, same spot: m_statelLookup) while ours cast.
    bool StatelPieceCasting(const d3d::Matrix& world, uint32_t vertexCount, uint32_t indexCount) const;
    uint8_t StatelPieceKinds(const d3d::Matrix& world, uint32_t vertexCount, uint32_t indexCount) const;   // (its lookup)
    std::unordered_map<uint64_t, uint8_t> m_statelLookup;   // -> the kinds (bits) of ours there
    uint32_t m_statelCoveredThisFrame = 0, m_statelCoveredLast = 0;
    std::unordered_map<uint64_t, CasterStreak> m_casterStreaks;
    std::vector<std::pair<uint64_t, std::pair<VkBuffer, VmaAllocation_T*>>> m_deadBuffers;
    d3d::Matrix m_frameViewProj{};               // the frame's camera, at its first shadow caster
    bool m_frameViewProjValid = false;
    void CacheCaster(uint64_t key, const ShadowCaster& c, const void* vertices, const uint16_t* indices);
    void ShadowCutout(uint32_t fvf, Texture** texture, int* texOffset, float* alphaRef) const;
    uint64_t CasterKey(uint32_t primitive, uint32_t fvf, uint32_t stride, const void* vertices, uint32_t vertexCount,
                       const uint16_t* indices, uint32_t indexCount, Texture* texture, int texOffset, float boundsMin[3],
                       float boundsMax[3]) const;
    uint32_t m_cachedCastersDrawn = 0;           // last shadow pass: remembered casters drawn (frame dumps)
    uint32_t m_forgottenInView = 0, m_forgottenFar = 0;   // remembered casters forgotten so far (frame dumps)
    // Last sun pass (frame dumps): the items it considered and the item-cascade pairs it culled / drew, split by
    // VisualKind::Static and by the animated flag (a character's body changes vertices every frame).
    uint32_t m_shadowItemsCount = 0, m_shadowStaticItems = 0, m_shadowAnimatedItems = 0;
    uint32_t m_shadowDrawn = 0, m_shadowCulled = 0;
    uint32_t m_shadowStaticDrawn = 0, m_shadowAnimatedDrawn = 0;
    double m_shadowCollectMs = 0.0, m_shadowCullMs = 0.0, m_shadowDrawMs = 0.0;   // last pass (frame dumps)
    std::vector<uint32_t> m_cascadeVisible;      // scratch: indices into m_shadowItems for one cascade
    std::vector<uint32_t> m_animatedItems;       // indices of the animated items (characters' bodies)
    // The items in batch-key order (CollectShadowItems): depth-only drawing doesn't depend on order, so the passes walk
    // this and casters sharing pipeline, layout, texture and buffers end up next to each other - one indirect call
    // and one set of bindings for each run instead of a new batch at every change in the game's draw order.
    std::vector<uint32_t> m_shadowOrder;
    std::vector<float> m_shadowOrderBox;         // per m_shadowOrder entry, packed: world box min x, y, z, max x, y, z
                                                 // (the point pass's per-light scan reads these, not the items)
    std::vector<uint8_t> m_shadowOrderCached;    // ... and whether it is remembered (the cascades' cull reads these)
    static uint64_t ShadowItemKey(const ShadowItem& item);
    // Frame dump: how much the main pass could merge into instanced draws - consecutive static draws with the same
    // snapshot, vertex / index range, format, textures and geometry, differing only in their world / per-draw block.
    uint32_t m_batchRuns = 0, m_batchMerged = 0, m_batchMaxRun = 0;
    uint64_t m_batchKey = 0;
    uint32_t m_batchRun = 0;
    void NoteBatch(uint64_t key);
    uint32_t m_meshStaticDraws = 0, m_meshHashedDraws = 0;   // frame dump: DrawMeshInfo by snapshot vs by content hash
    void UpdateCasterCache();
    void ForgetCachedCaster(uint64_t key);
    void ForgetCasterTexture(Texture* texture);
    static double SwayClock();
    static void TickFixedClock();
    d3d::Matrix SwayPose(const CachedCaster& e) const;
    void CaptureSun(const d3d::Light& light);
    bool ShadowReceiver(uint32_t fvf) const;
    bool IsShadowCaster(uint32_t primitive, uint32_t fvf) const;
    bool IsTerrain(uint32_t fvf) const;
    bool ShadowCompensated(uint32_t fvf) const;
    bool ShadowInLightmap(uint32_t fvf) const;
    bool IsMultiplyPass() const;
    bool m_terrainLitPassCur = false, m_terrainLitPassPrev = false;   // the ground had a lightmap + lights pass
    bool IsBlobShadow(uint32_t primitive, uint32_t fvf, const void* vertices, uint32_t vertexCount, uint32_t indexCount) const;
    bool BlobShadowShape(uint32_t primitive, uint32_t fvf, const void* vertices, uint32_t vertexCount, uint32_t indexCount) const;
    void RenderShadowMap(VkCommandBuffer cmd);

    // HDR (hdr.cpp): during the scene phase m_main is m_scene (float); EndScene tone maps it into m_ldrMain, which is
    // m_main from then on (the interface, read-backs, presenting).
    float m_hdrHeadroom = 1.5f;                  // how far local lights may go above 1 in the HDR scene (soft roll-off)
    bool m_hdr = false;
    float m_tonemapKnee = 0.85f, m_exposure = 1.0f;
    Texture* m_ldrMain = nullptr;
    Texture* m_scene = nullptr;
    bool m_scenePhase = false;                   // 3D drawn into m_scene; ends at the first interface draw
    bool m_hideInterface = false;                // SetHideInterface
    std::vector<uint8_t> m_consolePixels;        // SetConsoleImage (interface.cpp DrawConsole)
    uint32_t m_consoleW = 0, m_consoleH = 0;
    bool m_consoleDirty = false;
    Texture* m_consoleTex = nullptr;
    void DrawConsole(VkCommandBuffer cmd, Texture* dst = nullptr);   // EndFrame: m_main in TRANSFER_SRC, left so
    // A draw into a layer laid over the frame later (the interface's, frame generation's labels): it blends, its alpha
    // as coverage (draw.cpp).
    bool InLayer() const
    {
        return (m_uiLayerActive && m_target == m_uiLayer) || (m_fgLabelsActive && m_target == m_fgLabels) ||
               (m_fgTopActive && m_target == m_fgTop);
    }
    bool m_sceneSaw3D = false;
    uint32_t m_sceneEndDraw = 0, m_sceneEndFvf = 0;   // frame dumps: where the scene phase ended
    VkPipeline m_pipelinesHdr[3] = {};           // m_pipelines for the float target
    VkPipeline m_pipelinesNoCut[3] = {};         // ... and the no-discard variant (early-Z; no cut-out)
    VkPipeline m_pipelinesHdrNoCut[3] = {};
    VkPipeline m_tessPipelines[2] = {};          // characters' Phong tessellation: 8-bit target, float target
    VkPipeline m_tessPipelinesNoCut[2] = {};     // ... no-discard
    // The scene pipelines above are built without the ground's code (ffp_*_ng.frag: 128 registers, the full shader
    // 168 and a spill - fewer pixels in flight); the ground's draws (and its rocks: their record's terrainAt.w) take
    // these, the full shader's, in the same arrangement.
    VkPipeline m_groundPipelines[3] = {}, m_groundPipelinesHdr[3] = {}, m_groundPipelinesNoCut[3] = {},
               m_groundPipelinesHdrNoCut[3] = {}, m_groundTessPipelines[2] = {}, m_groundTessPipelinesNoCut[2] = {};
    // ... and for the draws no frame light reaches (ffp_*_nf.frag, RVK_NO_FRAME_LIGHTS: 96 registers) - their mask is
    // empty, or the light override is off; never the ground's.
    VkPipeline m_noFramePipelines[3] = {}, m_noFramePipelinesHdr[3] = {}, m_noFramePipelinesNoCut[3] = {},
               m_noFramePipelinesHdrNoCut[3] = {}, m_noFrameTessPipelines[2] = {}, m_noFrameTessPipelinesNoCut[2] = {};
    bool m_drawGround = false;                   // the current draw takes the ground's path (its record's terrainAt.w)
    bool m_drawNoFrameLights = false;            // ... no frame light reaches it (m_noFrame*Pipelines)
    // The interface's lean shader (RVK_LiteShader, ffp_lite.frag): the 8-bit targets' 2D draws take it (m_drawLite).
    bool m_liteShader = true;
    bool m_drawLite = false;
    VkPipeline m_pipelinesLite[3] = {}, m_pipelinesLiteNoCut[3] = {};
    // The scene shader's variants as the driver compiled them (VK_KHR_pipeline_executable_properties): each stage's
    // statistics - registers, spills, instructions, waves - one line each in the log at startup. Registers decide how
    // many pixels the GPU shades at once: a variant's worst path sets them for every draw that takes it.
    bool m_shaderStats = false;
    void ShaderRegisterReport(VkPipeline pipeline, const char* build, const char* target, const char* cut);
    bool m_constLightOverride = false;           // the current constant block has F_LIGHTOVERRIDE (Draw, as it writes it)
    bool m_tessSupported = false;
    // Phong tessellation of characters near the camera (draw.cpp TessellateDraw): RVK_Tess*.
    float m_tessShape = 0.0f;                    // 0 = off; how far towards the smooth shape (Phong's alpha)
    float m_tessDistance = 20.0f;                // world units: full level up close, none from here on
    uint32_t m_tessLevel = 4;
    bool m_drawTess = false;                     // the current draw is tessellated (ApplyDynamicState, the pipeline)
    struct TessTopology { uint64_t frames[3] = {}; };   // the last 3 frames a topology was drawn animated, newest first
    std::unordered_map<uint64_t, TessTopology> m_tessTopologies;
    float TessellateDraw(uint32_t primitive, uint32_t fvf, uint32_t vertexCount);   // the level (0 = none)
    bool CharacterDraw(uint32_t fvf, uint32_t vertexCount);   // the current draw is a character's (body or part)
    bool m_drawIsCharacter = false;
    bool m_drawIsBlobShadow = false;             // the game's blob shadow (drawn: indoors, or no sun shadows) - its colour
                                                 // only, the surface's other targets left as they are
    struct TessCharacter { float x, z, minY, maxY; };
    std::vector<TessCharacter> m_tessChars, m_tessCharsPrev;   // animated characters' boxes: this / last frame
    bool SmoothNormals(const void* vertices, uint32_t vertexCount, const detail::FvfLayout& layout);
    std::vector<float> m_smoothNormals;          // the draw's averaged normals (binding 10)
    std::vector<int32_t> m_smoothTable;          // their position hash table
    // Skinned pieces: which vertices share a position (the first of each), found once per mesh in its rest pose.
    struct SmoothOwners { std::shared_ptr<const skin::Source> source; std::vector<int32_t> owner; uint64_t lastFrame = 0; };
    std::unordered_map<const skin::Source*, SmoothOwners> m_smoothOwners;
    bool SmoothNormalsSkinned(const void* vertices, uint32_t startVertex, uint32_t vertexCount);
    VkDescriptorSetLayout m_tonemapSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_tonemapLayout = VK_NULL_HANDLE;
    VkPipeline m_tonemapPipeline = VK_NULL_HANDLE;
    VkSampler m_pointSampler = VK_NULL_HANDLE, m_linearSampler = VK_NULL_HANDLE;
    float m_bloomStrength = 1.5f, m_bloomThreshold = 1.0f;
    float m_sunSoftness = 1.0f, m_leafLight = 1.0f, m_nightGlow = 1.5f, m_contact = 0.6f;
    bool m_drawMayDiscard = true;                // the current draw can cut out (alpha test / F_CUTOUT): pick the variant
    bool m_drawPrepassCutout = false;            // pre-passed with the cut-out pre-pass (prepass_cutout.frag)
    // Two-pass foliage (RVK_FolEdges): a blended cut-out (the game's foliage: blended SRCALPHA / INVSRCALPHA, writing
    // depth, cut out) is drawn as its core (alpha >= 0.95: blended, with depth, its record's motion.w = 1) and its soft
    // edges (the rest: motion.w = 2) are queued and drawn without depth writes over the finished scene (FlushEdges:
    // when its rendering ends, before a clear, before a draw that writes no depth). The game's way - every partly
    // transparent pixel writing depth - hid whatever was drawn behind it afterwards and showed what was drawn before:
    // the tree behind, the sky, seen through the leaves' edges. With the edges out of the depth, the core can go into
    // the cut-out pre-pass too.
    bool m_foliageEdges = true;
    bool m_drawSplit = false;                    // the current draw is two-pass foliage
    // Two-pass foliage past RVK_FolDither (units from the eye to its box; 0 never), with the TAA on, is drawn once
    // instead, its soft edges dithered (ffp_main.glsl FoliageDitherCut: the TAA averages them back) - no edges queued.
    // Close up the soft pass stays: its blend is exact, and the dither's grain could show on a big, near leaf.
    float m_foliageDither = 20.0f;
    bool m_drawDither = false;                   // the current two-pass foliage draw is dithered
    std::unordered_set<uint32_t> m_ditherRefusedLogged;   // (the log: textures kept blended, by game id)
    struct DeferredEdge {
        uint32_t primitive, fvf, stride, vertexCount, indexCount, record;
        VkDeviceSize vbOffset, ibOffset;
        VkBuffer vb, ib;
        VkDeviceSize frameLights, prevOffset, prevBytes, smoothOffset, smoothBytes;
        VkBuffer prevBuffer, smoothBuffer;
        d3d::Viewport viewport;
        uint32_t cull;
        uint64_t ringGeneration;                 // (its record is in this generation's arena: else dropped)
    };
    std::vector<DeferredEdge> m_edges;
    // The scene's frame-wide bindings - set 0's arena (constants, records, shadow maps) and set 1's bindless table -
    // pushed if a pass with another layout came between (the draws, FlushEdges).
    void BindSceneArena(VkCommandBuffer cmd);
    uint64_t m_edgeFlushes = 0, m_edgeDraws = 0;   // logged with the pre-pass (PrepassLog)
    void FlushEdges();                           // the queued edges, drawn now (in the current rendering)
    // The main pass's per-draw push (bindings 4, 8, 10), skipped when it equals the last one (StateCache).
    void PushDrawSet(VkCommandBuffer cmd, VkDeviceSize frameLightsOffset, VkBuffer prevBuffer, VkDeviceSize prevOffset,
                     VkDeviceSize prevBytes, VkBuffer smoothBuffer, VkDeviceSize smoothOffset, VkDeviceSize smoothBytes);
public:
private:
    uint64_t m_noCutDraws = 0;                   // draws that took the no-discard pipeline (early-Z; logged)
    // Depth pre-pass (prepass.cpp). Armed at the scene's depth clear (the frame's main commands split there: Frame);
    // while armed, each opaque scene draw also draws its depth into the pre-pass, which runs before the scene's
    // draws. Disarmed when the scene's rendering ends (a target switch, a copy, a flush, the scene's end) or at a
    // second depth clear - from there on draws go only to the main pass, as without it.
    bool m_prepassOn = true;                     // RVK_Prepass
    bool m_prepassArmed = false;
    bool m_drawPrepassed = false;                // the current draw is in the pre-pass (its depth test passes equal)
    // The backdrop cull: a draw that ignores depth (no test or ALWAYS) and doesn't write it, recorded in the segment
    // before anything has written depth there, is only visible where no opaque surface is drawn after it - which is
    // where the pre-pass depth still holds the clear value. The depth bounds test [clear, clear] rejects the rest
    // before shading (the game's backgrounds: lit meshes covering ~2.7 screens a frame, nearly all hidden).
    bool m_depthBounds = false;                  // the depthBounds feature (dynamic bounds in the scene pipelines)
    bool m_drawBackdrop = false;                 // the current draw gets the bounds test
    bool m_prepassDepthTouched = false;          // a draw of the armed segment wrote depth: no backdrops from here
    bool m_prepassFullClear = false;             // the arming clear covered the whole scene (else no backdrop cull)
    float m_prepassClearZ = 1.0f;
    uint64_t m_backdropDraws[5] = {};            // draws ignoring depth: culled, before the clear, after the segment,
                                                 // after depth was written, other (logged with the pre-pass)
    VkPipeline m_prepassPipeline = VK_NULL_HANDLE;   // ffp.vert only, depth attachment only
    // ... and for cut-outs: depth only where the pixel ends up fully opaque (prepass_cutout.frag). Null: cut-outs stay
    // out of the pre-pass (RANDYVK_PREPASS_CUTOUT=0, or the pipeline failed).
    VkPipeline m_prepassCutoutPipeline = VK_NULL_HANDLE;
    VkPipeline m_leafPrepassPipeline = VK_NULL_HANDLE;   // ... leaf.vert + prepass_cutout.frag (our own canopy's sprigs)
    bool m_prepassEqual = true;                  // RVK_PrepassEq: pre-passed draws test EQUAL, no depth write
    bool m_prepassMore = true;                   // RVK_PrepassMore: a second segment after the water's copy
    bool m_waterEndedPrepass = false;            // the water's copy ended this frame's segment (PrepassContinue)
    bool m_prepassSway = true;                   // swaying plants in the pre-pass too (RANDYVK_PREPASS_SWAY=0: not)
    struct PrepassCache {
        bool bound = false;                      // pipeline, front face, depth state, arrays, set 1
        int pipeline = -1;                       // bound: 0 the plain pipeline, 1 the cut-out one
        uint32_t topology = ~0u, cull = ~0u, fvf = ~0u;
        VkViewport viewport{};
        VkRect2D scissor{};
        VkBuffer vb = VK_NULL_HANDLE, ib = VK_NULL_HANDLE;
        VkDeviceSize frameLights = ~0ull, prevOffset = ~0ull, smoothOffset = ~0ull;
        VkBuffer prevBuffer = VK_NULL_HANDLE, smoothBuffer = VK_NULL_HANDLE;
        VkImage depth = VK_NULL_HANDLE;          // the depth buffer it draws into (armed)
        uint32_t groupFirst = 0, groupCount = 0; // pending indexed draws: one indirect call (FlushPrepassGroup)
    } m_pre;
    VkDeviceSize m_prepassIndirectBase = 0;      // its indirect commands (the frame's arenas: as many as records)
    uint32_t m_prepassIndirectCount = 0;
    uint64_t m_prepassCalls = 0;                 // (draw calls it issued: logged with its draws)
    void FlushPrepassGroup();
    bool CreatePrepassPipeline(VkShaderModule vert, VkShaderModule leafVert);   // false: the pre-pass stays off
    // Our own canopy's sprigs into the pre-pass (their solid middles' depth; prepass.cpp): true if recorded.
    bool PrepassLeaves(uint32_t recordIndex, uint32_t first, uint32_t count, VkDeviceSize frameLightsOffset,
                       VkBuffer prevBuffer, VkDeviceSize prevOffset, VkDeviceSize prevBytes);
    uint64_t m_prepassDraws = 0, m_prepassSegments = 0;   // logged every 600 frames (PrepassLog)
    // Why a scene draw isn't in the pre-pass (PrepassCheck; counted for the log), and why segments end.
    enum PrepassWhy : uint32_t {
        kPreIn, kPreBefore, kPreAfter, kPreCharacter, kPreSway, kPreKind, kPreNoZTest, kPreNoZWrite, kPreZFunc,
        kPreDiscard, kPreWater, kPreBlend, kPreAlphaTest,
        kPreAlphaDiffuse, kPreAlphaMaterial, kPreAlphaTexture, kPreAlphaArg, kPreAlphaTfactor, kPreWhyCount
    };
    enum RenderEnd : uint32_t { kEndOther, kEndScene, kEndTarget, kEndCopy, kEndReadback, kEndFlush, kEndClear, kEndCount };
    uint64_t m_prepassWhy[kPreWhyCount] = {};
    uint64_t m_prepassEndCause[kEndCount] = {};
    uint32_t m_renderEndCause = kEndOther;       // why the next EndRendering happens (set by its callers)
    bool m_prepassEndedThisFrame = false;        // a segment ended: later draws are "after end"
    bool PrepassArm(const VkClearRect* rects, uint32_t count, float z);   // at a scene depth clear: split, arm
    void PrepassEnd(uint32_t cause);             // end the armed segment (its commands are complete)
    bool PrepassContinue();                      // after the water's copy: a second segment over the depth so far
    VkCommandBuffer PrepassCommands() const;     // the armed segment's pre-pass command buffer
    uint32_t PrepassCheck(uint32_t primitive, uint32_t fvf, bool swaying) const;   // kPreIn: into the pre-pass
    uint32_t AlphaOneCheck(uint32_t fvf) const;  // kPreIn: the output alpha is provably 1; else the input in the way
    bool BackdropCull(uint32_t fvf) const;       // the current draw gets the backdrop cull (m_drawBackdrop)
    void PrepassLog();                           // every 600 frames
    void PrepassDraw(uint32_t primitive, uint32_t fvf, uint32_t stride, uint32_t vertexCount, uint32_t indexCount,
                     VkDeviceSize vbOffset, VkDeviceSize ibOffset, VkBuffer vb, VkBuffer ib,
                     VkDeviceSize frameLightsOffset, VkBuffer prevBuffer, VkDeviceSize prevOffset, VkDeviceSize prevBytes,
                     VkBuffer smoothBuffer, VkDeviceSize smoothOffset, VkDeviceSize smoothBytes, uint32_t recordIndex, bool cutout);
    uint32_t FrameCommands(Frame& f, VkCommandBuffer out[7]);   // what a submission runs (ends the pre-pass)
    void FrameCommandsSubmitted(Frame& f);       // back to one main command buffer
    // Shared with ApplyDynamicState: the D3D viewport as Vulkan's (and its scissor), the FVF's vertex input.
    void ViewportState(VkViewport* vp, VkRect2D* scissor) const;
    void ViewportStateOf(VkViewport* vp, VkRect2D* scissor) const;   // (without a portal's box)
    Rect SceneRect(const Rect& r) const;         // back-buffer pixels -> the scene's (RVK_RenderScale)
    static void SetVertexInput(VkCommandBuffer cmd, uint32_t fvf, uint32_t stride);
    Texture* m_contactTex[2] = {};               // half resolution: contact shadow (1 = lit), view depth (ping-pong)
    VkPipeline m_contactPipeline = VK_NULL_HANDLE;
    bool RenderContactShadows(VkCommandBuffer cmd);
    float m_saturation = 1.0f, m_contrast = 1.0f, m_warmth = 0.0f, m_lutAmount = 1.0f, m_nightTint = 0.3f, m_vignette = 0.0f;
    Texture* m_lut[2] = {};                      // colour lookup tables: day, night (3D)
    Texture* CreateLut(uint32_t size, const uint8_t* rgba);
    void GradingParams(float out[8]) const;
    float m_sway = 1.0f;
    // Plants pushed aside by characters (draw.cpp): animated meshes (CPU-skinned - their fingerprint isn't drawn
    // again the next frame) give the characters' feet; where they walked stays pushed for a moment (the trail).
    float m_grassPush = 1.0f;
    struct PushCandidate { uint64_t mesh; float x, y, z; };
    std::vector<PushCandidate> m_pushNew, m_pushOld;   // the last frame's / the one before (confirmed a frame later)
    struct PushPoint {
        float x, y, z;
        double time, born;                       // last there; last moved (head) or made (trail)
        float dropX, dropZ;                      // head: where it last dropped a trail point
        bool head;                               // follows a character (else a trail point)
        float printX = 0.0f, printZ = 0.0f;      // head: where it last left a footprint (sand_prints.cpp)
        float printSide = 1.0f;                  // ... and the next one's side (1 left, -1 right)
    };
    std::vector<PushPoint> m_pushTrail;
    uint64_t m_drawMeshKey = 0;                  // the current draw's mesh cache key
    void PushCandidateDraw(uint32_t fvf);
    void UpdatePushTrail();                      // at the start of a frame
    void FillPushers(detail::FrameLights* fl, const float eye[3]);
    struct FramePusher { float p[3]; };
    std::vector<FramePusher> m_framePushers;     // the pushers FillPushers last wrote, in FrameLights order
    void DrawWorldBox(float c[3], float e[3]) const;
    uint32_t PusherMask(float margin) const;     // bits of m_framePushers near the current draw
    // What the draw path needs to know of a mesh's vertices (draw.cpp DrawMeshInfo): its model-space box and its
    // indices' hash, remembered by a fingerprint of the mesh (sizes and 16 sampled vertices and indices) - a static
    // mesh drawn again (most of them) needs no pass over its vertices; an animated one (CPU-skinned) changes its
    // fingerprint every frame and gets one pass, shared by its users.
    struct MeshInfo {
        float boundsMin[3], boundsMax[3];
        uint64_t indexHash;
        uint64_t firstFrame, lastFrame;
        bool diffuseAlphaOne = true, specularAlphaOne = true;   // every vertex colour's alpha byte is 255 (pre-pass)
        mutable int8_t flatShape = -1;           // SwayParams: 1 = one plane (a sign, a card), 0 = not, -1 = not looked at
        uint64_t geo = 0, raw = 0;               // mesh ids (meshids.cpp): its geometry's hashes (0: not a static piece)
        uint32_t idCandidates = 0;               // ... the pieces in the table with its geometry
        bool rawSame = false;                    // ... one of them has its vertex bytes too
        bool skyScenery = false;                 // ... a mesh of the sky's scenery (the ships over the cities: mesh_id.h
                                                 // kSkyScenery): kept inside the far plane, fogged as the far sky
    };
    std::unordered_map<uint64_t, MeshInfo> m_meshInfo;
    // Mesh ids (meshids.cpp).
    struct MeshIdEntry {
        uint64_t geo = 0, raw = 0;
        uint32_t type = 0, id = 0;
        uint16_t piece = 0, pieces = 0;
        uint32_t name = 0;                       // offset in m_meshIdNames
    };
    std::vector<MeshIdEntry> m_meshIds;          // sorted by geo
    std::vector<char> m_meshIdNames;
    struct MeshIdObject {                        // the object (visual) drawing now: the meshes it can be
        uint32_t visual = 0;
        uint64_t frame = 0;
        uint32_t pieces = 0;
        bool known = false;                      // settled on an earlier frame: nothing to do
        std::vector<uint64_t> candidates;        // type << 32 | id, sorted
    };
    MeshIdObject m_idObject;
    struct MeshIdResult { uint64_t mesh; uint32_t candidates; uint64_t frame; };   // mesh: type << 32 | id (0: not one)
    std::unordered_map<uint32_t, MeshIdResult> m_meshIdObjects;   // by visual
    struct MeshIdStats {
        uint64_t pieces = 0, known = 0, rawSame = 0, objects = 0, named = 0, ambiguous = 0;
        uint32_t loggedSky = 0;
        uint32_t loggedPieces = 0, loggedObjects = 0;
    } m_meshIdStats;
    const char* MeshIdName(const MeshIdEntry& e) const;
    std::pair<const MeshIdEntry*, const MeshIdEntry*> MeshIdsOf(uint64_t geo) const;
    void HashMeshId(MeshInfo& info, uint32_t fvf, uint32_t stride, const void* vertices, uint32_t vertexCount,
                    const uint16_t* indices, uint32_t indexCount);
    void NoteMeshPiece();
    void SettleMeshObject();
    void LogMeshIds();
    const MeshInfo* m_drawMesh = nullptr;        // the current draw's (null: external geometry, pre-transformed)
    const skin::Job* m_drawSkin = nullptr;       // the current draw is this skinned character piece (DrawSkinned)
    uint32_t m_drawVisualKind = 0;               // SetDrawVisual (VisualKind)
    uint32_t m_drawOwner = 0;                    // ... the character it belongs to (0: none / unknown)
    uint32_t m_drawVisual = 0;                   // ... the visual object itself (the game's; for picking)
    std::vector<SceneLight> m_sceneLights;       // SetSceneLights, of frame m_sceneLightsFrame
    uint64_t m_sceneLightsFrame = 0;
    const char* m_drawVisualName = "";
    const skin::Vertex* m_drawSkinBase = nullptr;   // ... its first skinned vertex
    // GPU skinning (skin_gpu.cpp).
    struct SkinMesh {
        std::shared_ptr<const skin::Source> source;
        VkBuffer buffer = VK_NULL_HANDLE;        // vertices | groups | members | indices
        VmaAllocation_T* allocation = nullptr;
        VkDeviceSize groupsOffset = 0, indexOffset = 0;
        uint32_t membersBase = 0;
        uint64_t lastFrame = 0;
    };
    struct SkinOutput {
        SkinMesh* mesh = nullptr;
        VkDeviceSize vertexOffset = 0, prevOffset = 0, smoothOffset = 0;   // in the frame's skin arena
        bool smoothReady = false;
        bool moved = false;                      // prevOffset holds last frame's positions
    };
    bool m_gpuSkin = true;                       // RANDYVK_GPU_SKIN=0: skinned pieces skinned on the CPU
    // Static geometry on the GPU (static_gpu.cpp): shared vertex snapshots in a chunked arena (M0), so every one
    // has a stable (buffer, offset) an indirect draw can reference. Indices stay in the ring.
    struct ArenaChunk {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation_T* allocation = nullptr;
        uint8_t* mapped = nullptr;               // the caster arena's (host memory); null for the static arena
        VkDeviceSize size = 0, used = 0;
        std::vector<std::pair<VkDeviceSize, VkDeviceSize>> free;   // (offset, size) slots returned by eviction
    };
    struct StaticGeometry {
        std::shared_ptr<const std::vector<uint8_t>> data;
        ArenaChunk* chunk = nullptr;
        VkDeviceSize offset = 0, size = 0;
        uint64_t lastFrame = 0;
        uint64_t serial = 0;                     // unique per placed snapshot (an address can be reused once freed)
    };
    std::unordered_map<const std::vector<uint8_t>*, StaticGeometry> m_staticGeometry;
    uint64_t m_staticGeneration = 0;             // DestroyStaticGeometry: every StaticGeometry pointer kept is stale
    // A registered mesh (RegisterMesh), with what its draws found the last time it was resolved: its snapshots'
    // arena entries and slots, and its mesh info. Valid while it was resolved in the last kSlotFresh frames (the
    // static arena and the mesh info only drop entries unused for longer - its draws keep theirs used) of the same
    // static generation; else DrawMesh resolves it again.
    struct MeshSlot {
        std::shared_ptr<const std::vector<uint8_t>> vertices, indices;
        size_t byteOffset = 0;
        uint32_t primitive = 0, fvf = 0, vertexCount = 0, indexCount = 0;
        uint64_t resolvedFrame = 0, generation = 0;
        StaticGeometry* vertexEntry = nullptr;
        StaticGeometry* indexEntry = nullptr;    // null: resolved, but not in the arena (drawn through the ring)
        VkBuffer vb = VK_NULL_HANDLE, ib = VK_NULL_HANDLE;
        VkDeviceSize vbOffset = 0, ibOffset = 0;
        uint64_t serial = 0;
        MeshInfo* mesh = nullptr;                // DrawMeshInfo's, once found
        uint64_t meshKey = 0;
    };
    static constexpr uint64_t kSlotFresh = 20;
    std::vector<MeshSlot> m_meshSlots;
    MeshSlot* m_drawSlot = nullptr;              // the current draw's (DrawMesh): DrawMeshInfo takes or keeps its mesh info
    uint32_t m_slotDraws = 0, m_slotResolves = 0;   // since the last hand-off log line
    std::vector<std::unique_ptr<ArenaChunk>> m_staticArena;
    std::vector<std::unique_ptr<ArenaChunk>> m_casterArena;   // remembered shadow casters (host memory, mapped)
    bool m_staticResident = true;                // RANDYVK_STATIC_GPU=0: copied into the ring per draw as before
    VkDeviceSize m_staticBytes = 0;              // on the GPU now
    VkBuffer m_drawStaticBuffer = VK_NULL_HANDLE;   // the current draw's vertices are in this buffer, at this offset
    VkDeviceSize m_drawStaticOffset = 0;
    VkDeviceSize m_drawStaticSlot = 0;           // ... the start of its arena slot (Brixelizer: brixelizer.cpp)
    VkBuffer m_drawStaticIndexBuffer = VK_NULL_HANDLE;   // with m_drawStaticBuffer: its indices retained here, at
    VkDeviceSize m_drawStaticIndexOffset = 0;            // this offset (DrawSharedIndexed); else through the ring
    // A slot in the static arena (device memory, filled by uploads) or, `casters`, the caster arena (host memory).
    ArenaChunk* ArenaPlace(VkDeviceSize bytes, VkDeviceSize align, VkDeviceSize* offset, bool casters = false);
    void ArenaFree(ArenaChunk* chunk, VkDeviceSize offset, VkDeviceSize bytes);   // reusable once the GPU is done
    void CollectArenaSlots();                    // CollectGarbage: slots no submission in flight can still read
    struct DeadArenaSlot { uint64_t tag; ArenaChunk* chunk; VkDeviceSize offset, bytes; };
    std::vector<DeadArenaSlot> m_deadArenaSlots;
    uint64_t m_staticSerial = 0;                 // the last StaticGeometry::serial handed out
    uint64_t m_drawStaticSerial = 0;             // the current draw's snapshot (with m_drawStaticBuffer)
    VkBuffer StaticBufferFor(const std::shared_ptr<const std::vector<uint8_t>>& data, VkDeviceSize* baseOffset,
                             VkDeviceSize align, uint64_t* serial);
    void BeginStaticFrame();
    void DestroyStaticGeometry();
    SkinOutput* m_drawGpu = nullptr;             // the current draw is skinned on the GPU, here
    VkDescriptorSetLayout m_skinSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_skinLayout = VK_NULL_HANDLE;
    VkPipeline m_skinPipeline = VK_NULL_HANDLE;
    VkDeviceSize m_skinArenaWanted = 0;
    bool m_skinUploadsPending = false;
    std::unordered_map<const skin::Source*, SkinMesh> m_skinMeshes;
    std::unordered_map<const skin::Job*, SkinOutput> m_skinOutputs;   // this frame's
    struct SkinBonesAt { VkDeviceSize offset, bytes; uint64_t generation; };
    std::unordered_map<const skin::Palette*, SkinBonesAt> m_skinBonesAt;   // this frame's bones in the ring
    bool CreateSkinResources(std::string* error);
    void DestroySkinResources();
    void BeginSkinFrame();
    SkinMesh* SkinMeshFor(const std::shared_ptr<const skin::Source>& source);
    bool SkinArenaAlloc(VkDeviceSize bytes, VkDeviceSize* offset);
    VkDeviceSize SkinBones(const skin::Palette& bones, VkDeviceSize* bytes);
    void SkinDispatch(SkinMesh& mesh, const skin::Job& job, uint32_t flags, const SkinOutput& out);
    SkinOutput* SkinOnGpu(skin::Job& job);
    bool SkinSmoothOnGpu(const skin::Job& job, SkinOutput& out);
    void FinishSkinUploads(VkCommandBuffer cmd);
    bool m_drawMeshStatic = false;               // ... seen in an earlier frame with the same vertices
    void DrawMeshInfo(uint32_t fvf, uint32_t stride, const void* vertices, uint32_t vertexCount, const uint16_t* indices,
                      uint32_t indexCount);
    bool SwayParams(uint32_t primitive, uint32_t fvf, uint32_t stride, const void* vertices, uint32_t vertexCount,
                    const uint16_t* indices, uint32_t indexCount, float out[4]);
    // Plants' big triangles split for smooth bending (draw.cpp SubdividePlant), by mesh cache key and split.
    struct PlantMesh { std::vector<uint8_t> vertices; std::vector<uint16_t> indices; uint64_t lastFrame = 0; };
    std::unordered_map<uint64_t, PlantMesh> m_plantMeshes;
    std::unordered_map<uint64_t, float> m_plantMaxEdge;   // a plant mesh's longest edge (model units)
    float m_plantDetail = 1.0f;
    float m_foliageLod = 35.0f;                  // RVK_FoliageLod (draw.cpp FoliageFar)
    // Procedural ground grass (RVK_GrassOn, grass.cpp). The ground is captured from the terrain's own draws into
    // world-aligned tiles (CaptureTerrain): per quarter-unit cell its height, its texture's colour (grass or not) and
    // its baked light (the terrain lightmap). A tile's blades are baked once its ground is known (BuildGrassTile) as
    // compact records in one GPU pool, expanded into strips by the vertex shader (grass.vert).
    static constexpr float kGroundCell = 0.25f;  // the ground grid's cell size (world units)
    static constexpr float kGrassTileSize = 8.0f;   // world units a tile covers
    static constexpr int kGroundN = 32;          // cells along a tile's side (kGrassTileSize / kGroundCell)
    static constexpr int kGroundBlock = 8;       // cells along a capture block's side (4 x 4 blocks a tile)
    bool m_grassOn = false;                      // RVK_GrassOn (off: nothing drawn, nothing captured)
    float m_grassDistance = 25.0f;               // RVK_GrassDist (radius around the camera, world units)
    float m_grassDensity = 5.0f;                 // RVK_GrassBlades (blades a patch)
    float m_grassHeight = 0.5f;                  // RVK_GrassHeight (world units)
    float m_grassWidth = 1.0f;                   // RVK_GrassWidth: a multiplier on the blades' width
    float m_grassBright = 1.0f;                  // RVK_GrassBright: the blades' brightness against their ground
    bool m_grassTex = true;                      // RVK_GrassTex: grass only where the ground's texel is green
    float m_grassVariety = 1.0f;                 // RVK_GrassVary: clumps, dry and tall patches, colour jitter
    float m_grassFlowers = 1.0f;                 // RVK_GrassFlower: wildflowers, seed heads, broad blades (a multiplier)
    float m_grassGlow = 1.0f;                    // RVK_GrassGlow: backlit tips, the sheen, rounded shading
    float m_grassGusts = 1.0f;                   // RVK_GrassGusts: gusts sweeping the field
    bool m_grassTrails = true;                   // RVK_GrassTrail: pushed grass stays down behind a character
    float m_trailSeconds = 12.0f, m_trailShade = 1.0f;   // SetGrassTrailLook
    float m_grassEven = 1.0f;
    bool m_grassShadows = true;                  // RVK_GrassShadow: the blades cast the sun's shadow                    // RVK_GrassEven: the blades' colour from one green, not the texel
    std::vector<float> m_grassFootprint;         // CaptureCover's scratch: an object's sides crossing its footprint cut
    // The ground under one tile and its baked blades. A cell is written by the finest terrain triangle seen there
    // (rank: the triangle's size class), so a coarse level of detail never overwrites a finer one; a capture block
    // whose cells are all known at least as finely as a triangle skips that triangle.
    struct GrassTile {
        static constexpr int kCells = kGroundN * kGroundN;
        static constexpr int kBlocks = (kGroundN / kGroundBlock) * (kGroundN / kGroundBlock);
        float y[kCells];
        uint32_t colour[kCells];   // the ground texel's RGB; bit 24: grass
        uint32_t light[kCells];    // the lightmap texel's RGB
        uint8_t rank[kCells];      // 255: never seen
        // The lowest few surfaces of solid objects (a platform, a floor, a road piece) over each cell, lowest first
        // (CaptureCover; 1e30: none). One on or just above the ground (CoveredAt): no blades grow there.
        static constexpr int kCoverLayers = 8;
        float cover[kCells][kCoverLayers];
        uint8_t lightRank[kCells];
        uint8_t blockRank[kBlocks];        // the coarsest rank in each block (255: a cell unseen)
        uint8_t blockLightRank[kBlocks];
        int32_t tx = 0, tz = 0;
        uint32_t seen = 0;                 // cells with a height
        uint32_t version = 0;              // bumped whenever the ground or its light changes
        uint64_t lastChange = 0;           // the frame of that change
        uint64_t created = 0;              // (frame number + 1: CaptureTerrain's chunk times)
        uint64_t lightReset = 0;           // its light last dropped (a lightmap re-uploaded), the same clock
        uint64_t lastUsed = 0;
        const void* lightmaps[4] = {};     // the lightmaps captured into it (a re-upload recaptures their light)
        const void* bases[8] = {};         // the base textures captured into it (a re-upload recaptures its ground)
        bool basesOverflow = false;        // ... more than those: any ground texture's re-upload recaptures it
        // The baked blades: in the pool at `first` (in blades), `blades` of them, the first `sparse` a thinned subset.
        uint64_t alloc = 0;                // VmaVirtualAllocation
        uint32_t first = 0, blades = 0, sparse = 0;
        bool built = false;
        uint32_t builtVersion = 0;
        float minY = 0.0f, maxY = 0.0f;    // the roots' height range (culling, light masks)
    };
    std::unordered_map<uint64_t, std::unique_ptr<GrassTile>> m_grassTiles;
    GrassTile* GrassTileAt(int32_t tx, int32_t tz) const;
    // The terrain chunks seen (by contents, pass and texture): their ground box and when they were last captured, so
    // an unchanged chunk over tiles that already have it is skipped whole.
    struct GrassChunk { float box[4] = {}; bool boxed = false; uint64_t processed = 0, lastSeen = 0, firstSeen = 0; };
    std::unordered_map<uint64_t, GrassChunk> m_grassChunks;
    // Per-object frame cache (objects.cpp; docs/render-batching-plan.md P1): a static mesh drawn at a world matrix - one
    // of the game's objects - keeps what its draws work out from those two alone, so its next frames' draws look it up
    // instead of working it out again (the render thread's per-draw cost over a city).
    struct ObjectEntry {
        uint64_t key = 0;                        // its hash; 0: an empty slot
        uint64_t meshKey = 0;                    // (identity, exactly: the mesh's key and the world matrix)
        d3d::Matrix world{};
        uint64_t lastFrame = 0;                  // the frame it was last drawn in
        bool seenLastFrame = false;              // ... drawn in the frame before that too: it hasn't moved (motion)
        GrassChunk cover;                        // CaptureCover's record of it
        uint64_t casterStamp = 0;                // CasterKey's inputs besides the object (cut-out kind, sway); 0: none yet
        uint64_t casterKey = 0;
        float casterMin[3] = {}, casterMax[3] = {};
        CachedCaster* cached = nullptr;          // the caster cache's entry for casterKey, valid while its epoch is
        uint64_t cachedEpoch = 0;
        uint64_t statelGen = 0;                  // StatelPieceKinds as of this m_statelLookupGen (0: not looked up)
        uint8_t statelKinds = 0;
        float boxCentre[3], boxExtent[3];        // DrawWorldBox's box
        float lightMin[3], lightMax[3];          // FrameLightMask's (its own sums: the same bits)
        uint32_t next = ~0u;                     // the entry found after it last time (FindObject's prediction)
    };
    std::vector<ObjectEntry> m_objects;          // open addressing (linear probing), a power of two long
    std::vector<uint64_t> m_objectKeys;          // their keys alone (probed: an entry is a few cache lines)
    uint32_t m_objectPrev = ~0u;                 // the entry the frame's last lookup found (its `next` is predicted)
    // Objects seen at a matrix once, not yet given an entry (direct-mapped: key, frame): an entry is made when one is
    // seen there again the next frame - a moving thing (a new matrix every frame) never gets one.
    struct ObjectCandidate { uint64_t key = 0, frame = 0; };
    std::vector<ObjectCandidate> m_objectCandidates;
    uint64_t m_objectPassing = 0;                // (lookups of objects not still: logged)
    uint64_t m_objectPredicted = 0;              // (lookups the order answered: logged)
    size_t m_objectCount = 0;
    ObjectEntry* m_drawObject = nullptr;         // the current draw's, when it is a static mesh
    bool m_objectCache = true;                   // RANDYVK_OBJECT_CACHE=0: off (A/B)
    bool m_objectVerify = false;                 // RANDYVK_OBJECT_VERIFY=1: cached values worked out again and compared
    uint64_t m_objectHits = 0, m_objectMisses = 0, m_objectMismatches = 0;   // (logged with the caster cache)
    uint64_t m_casterCacheEpoch = 1;             // a remembered caster went (ObjectEntry::cached no longer valid)
    uint64_t m_statelLookupGen = 1;              // m_statelLookup changed (ObjectEntry::statelKinds)
    ObjectEntry* FindObject();
    void PruneObjects();
    void RehashObjects(size_t size, uint64_t keep);
    bool DrawStatelCasting(uint32_t vertexCount, uint32_t indexCount);   // StatelPieceCasting for the current draw
    std::vector<GrassTile*> m_grassChunkGrid;    // CaptureTerrain's scratch: the tiles under a chunk
    uint64_t m_grassTileEpoch = 0;               // the latest tile made or light reset (the chunks' clock)
    int32_t m_grassTileBox[4] = {0, 0, -1, -1};  // the tile range around the camera: x0, z0, x1, z1
    bool m_grassDirty = false;                   // a blade setting changed: rebuild the tiles
    bool m_grassGroundReset = false;             // the ground filter changed: capture the ground again
    float m_grassLastEye[3] = {};                // the camera at the last grass frame (a jump resets the ground)
    uint64_t m_grassLastFrame = 0;
    std::vector<const void*> m_lightmapsUploaded;   // terrain lightmaps re-uploaded since the last grass frame
    std::vector<const void*> m_groundTexUploaded;   // terrain base textures re-uploaded (or made unreadable) since then
    float m_terrainAmbient[4] = {};              // the terrain light pass's global ambient this frame (w: seen)
    // The scene's fog (the ground's draws: the game's weather and time of day), for what is drawn after the scene's draws
    // - smoke, rain - when the render states hold the interface's. SceneFog: colour (w: on), start, end, density, mode.
    float m_sceneFogColour[4] = {}, m_sceneFogParams[4] = {};
    uint64_t m_sceneFogFrame = ~0ull;
    // The world's global ambient (the terrain's base pass: RS_AMBIENT, luma, eased): the game's own day cycle - 0.23 by
    // night, ~0.34 at dawn, ~0.46 by day - lifts the night grading at dawn and dusk (NightAmount).
    // The game's sunlight on its clouds (its directional light on the puffs, the brightest this frame): its hue lights
    // our clouds (cloud_light.glsl SunLight) - amber at dawn, white by day - so they keep to the game's palette.
    float m_cloudGameLight[3] = {0.0f, 0.0f, 0.0f}, m_cloudGameLightNext[3] = {0.0f, 0.0f, 0.0f};
    float m_cloudGameHue[3] = {1.0f, 1.0f, 1.0f};
    uint64_t m_cloudGameLightFrame = ~0ull, m_cloudGameHueFrame = ~0ull;
    void NoteCloudGameLight(const d3d::Color& c);
    // The terrain's shade from a low sun (terrain_sun.cpp): the game's terrain heights in a grid round the eye, each
    // cell's height to see the sun over it (a map, FL.terrainSun), the eye's own; weighted in low in the sky.
    std::vector<float> m_tsHeights;
    std::unordered_set<uint64_t> m_tsChunks;      // the terrain chunks read into it since it last moved
    int m_tsOrigin[2] = {0, 0};
    float m_tsMapOrigin[2] = {0.0f, 0.0f}, m_tsSunUsed[3] = {0.0f, 0.0f, 0.0f};
    float m_tsEye = 1.0f, m_tsWeight = 0.0f;
    bool m_tsDirty = false;
    uint64_t m_tsMadeFrame = ~0ull;
    // Footprints in the sand (sand_prints.cpp, RVK_SandPrints): a print map round the eye, two images by turns (the
    // last read, the next written), the prints the characters left since it was last stepped, where it lies.
    struct SandPrint { float x, z, dx, dz; };    // world x, z; heading (unit)
    std::vector<SandPrint> m_printsNew;
    Texture* m_printMap[2] = {};
    uint32_t m_printCur = 0;                     // the map last written
    int m_printOrigin[2] = {};                   // its corner (world texels)
    bool m_printValid = false;
    double m_printTime = 0.0;
    uint64_t m_printsStamped = 0;                // (the log's count)
    VkPipeline m_printPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_printLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_printSetLayout = VK_NULL_HANDLE;
    bool SandPrintsOn() const { return m_terrainSettings.prints > 0.0f && m_native.on && m_native.ground; }
    void UpdateSandPrints(VkCommandBuffer cmd);
    void SandPrintLights(float out[3]);
    Texture* m_terrainSunMap = nullptr;
    VkPipeline m_terrainSunPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_terrainSunLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_terrainSunSetLayout = VK_NULL_HANDLE;
    void NoteTerrainSun(uint32_t fvf, const void* vertices, uint32_t vertexCount);
    void RecentreTerrainSun();
    void UpdateTerrainSun(VkCommandBuffer cmd);
    void TerrainSunLights(float out[8]);
    // Shadows drawn on things this frame: the shadow map's, or the terrain's shade from a low sun (without the map).
    bool ShadingOn() const { return m_shadowValid || (m_tsWeight > 0.0f && m_tsMadeFrame != ~0ull); }
    float m_sceneAmbient = -1.0f;
    uint64_t m_sceneAmbientFrame = ~0ull;
    // How much it is night for the grading (tone map, bloom, night glow): by the sun's eased height (UpdateSunClock),
    // from ~2 degrees under the horizon to ~14; without a sun (indoors) the game's light and ambient, as before.
    float NightAmount() const { return m_cond.grade; }
    float NightByLight() const;
    void SceneFog(float colour[4], float params[4]) const;
    void RenderStateFog(float colour[4], float params[4]) const;
    float m_terrainSun[8] = {};                  // ... the directional light it takes: colour, direction
    struct GrassTrash { uint64_t alloc; VkBuffer buffer; VmaAllocation_T* allocation; uint64_t frame; };
    std::vector<GrassTrash> m_grassTrash;        // pool ranges and buffers freed once no frame in flight reads them
    // Trails: pushed grass lying down behind the characters (RVK_GrassTrail) - a world-anchored grid around the camera,
    // wrapping (cell gx lives at gx mod kTrailN): per cell how far its grass is pushed over and which way, stamped
    // where a character stands and recovering over a few seconds. Uploaded with the frame (grass.vert Trail).
    // The window follows the camera and drops what leaves it: it covers the grass's whole range (the longest
    // RVK_GrassDist), so trodden grass comes back up by time, not by how far the camera went (at 128 x 0.25 it reached
    // only 16 units around the camera).
    static constexpr int kTrailN = 256;          // cells along a side (a power of two)
    static constexpr float kTrailCell = 0.4f;    // world units a cell
    std::vector<float> m_trail;                  // kTrailN^2 x (direction x, z scaled by the amount)
    int32_t m_trailOrigin[2] = {0, 0};           // the window's first cell (world cell coordinates)
    bool m_trailValid = false;                   // the window holds something
    double m_trailClock = 0.0;                   // the sway clock at the last update
    uint32_t m_trailActive = 0;                  // cells with any push left (none: nothing uploaded)
    void UpdateGrassTrail();
    uint64_t GrassTilesWaiting() const;          // tiles in range with ground seen, not built or from an older ground
    // Solid objects' upward surfaces near the ground (CaptureCover): the grass under them stays away. Each object
    // (mesh x world matrix) once, like the terrain chunks.
    void CaptureCover(uint32_t primitive, const detail::FvfLayout& layout, const void* vertices, uint32_t vertexCount,
                      const uint16_t* indices, uint32_t indexCount);
    bool CoveredAt(float x, float z, float groundY) const;
    std::unordered_map<uint64_t, GrassChunk> m_grassCovers;
    std::vector<GrassTile*> m_grassCoverGrid;    // CaptureCover's scratch: the new tiles under an object
    uint64_t m_grassCoverFrame = 0;              // the frame m_grassCoverMs counts
    double m_grassCoverMs = 0.0;                 // CaptureCover's time this frame (a budget: the rest waits)
    // asBase: the native ground drawn in one pass (RVK_GroundOne) taken as its base pass too - its heights and colours.
    void CaptureTerrain(uint32_t primitive, const detail::FvfLayout& layout, const void* vertices, uint32_t vertexCount,
                        const uint16_t* indices, uint32_t indexCount, bool asBase = false);
    bool GroundAt(float x, float z, float* y, uint32_t* colour, uint32_t* light) const;
    void GroundNormal(float x, float z, float out[3]) const;   // the ground's upward normal (the slope) at x, z
    bool BuildGrassTile(GrassTile& tile);
    void FreeGrassBlades(GrassTile& tile);
    bool GrowGrassPool(uint32_t minBlades);
    bool EnsureGrassIndices(uint32_t blades);
    void UpdateGrassTiles();
    void DestroyGrassTiles();
    void DrawGrassTiles(VkCommandBuffer cmd);      // the blades into the rendering already active on the scene
    void RenderGrassField(VkCommandBuffer cmd);    // ... on its own, at the end of the scene (nothing blended was drawn)
    // The blades into the sun's nearest cascade (RVK_GrassShadow), after its casters: lightViewProj its matrix, texel
    // its texel size, depth its light-space depth range.
    void DrawGrassShadow(VkCommandBuffer cmd, const d3d::Matrix& lightViewProj, float texel, float depth);
    // The blades into a point light's cube face; cone: a spot light's (only the tiles it reaches), or null.
    bool DrawGrassPointShadow(VkCommandBuffer cmd, const d3d::Matrix& faceViewProj, const float light[3], float range,
                              const detail::Cone* cone);
    bool GrassNearLight(const float light[3], float range, const detail::Cone* cone) const;   // some of our grass casts into its cube (it sways)
    bool GrassBelow(const float light[3], float range, const detail::Cone* cone) const;
    bool m_grassDrawnThisFrame = false;            // the blades go in before the game's transparent pass (see Draw)
    // The water (water.cpp).
    WaterParams m_waterParams;
    std::atomic<bool> m_waterReady{false};
    struct WaterMap {
        uint64_t key = 0;                        // the mesh's hash
        Texture* texture = nullptr;              // RGBA16F: R height above `base`, G water
        float origin[2] = {}, size[2] = {};      // the map's world x, z origin and size
        float lo[2] = {}, hi[2] = {};            // the water's x, z extent
        float base = 0.0f, texel = 1.0f, extent = 0.0f;
        uint32_t flatTriangles = 0, colour = 0;
        uint64_t lastFrame = 0;
    };
    std::vector<WaterMap> m_waterMaps;
    // The surface from the game's own triangles (water_surface.h), made once for each mesh the game hands over (by the
    // map's key), the last few kept.
    struct WaterSurface {
        uint64_t key = 0;
        uint64_t lastFrame = 0;
        water::Surface surface;
    };
    std::vector<WaterSurface> m_waterSurfaces;
    // With the surface from the game's triangles, the disturbance field's map: a window round the camera (the field
    // reaches some 45 units), made again when the camera leaves its cell or the mesh changes - not the playfield's
    // whole map (~30 ms over Borealis each time the game's mesh changed).
    WaterMap m_waterSimMap;
    int32_t m_waterSimMapCell[2] = {INT32_MIN, INT32_MIN};
    WaterMap* WaterSimMapFor(uint64_t key, const WaterVertex* v, const std::vector<uint32_t>& flat);
    const water::Surface* WaterSurfaceFor(uint64_t key, const WaterVertex* v, uint32_t count, const std::vector<uint32_t>& flat,
                                          const std::vector<uint32_t>& steep);
    std::vector<uint32_t> m_waterFlat;
    struct WaterGrid { uint32_t segments = 0; VkBuffer buffer = VK_NULL_HANDLE; VmaAllocation_T* allocation = nullptr; };
    std::vector<WaterGrid> m_waterGrids;
    std::vector<uint32_t> m_waterSteep;
    // Falls' sheets (water.cpp BuildFallSheet), made the first time a fall is seen; the last few kept.
    struct WaterFall {
        uint64_t key = 0;                        // its triangles' hash
        std::vector<float> vertices;             // 12 floats each (water.cpp MeshVertexOut)
        std::vector<uint32_t> indices;
        std::vector<float> foot;                 // where it meets the pool, every few units across it: x, y, z, and
                                                 // the way out from the cliff (x, z)
        uint64_t lastFrame = 0;
    };
    std::vector<WaterFall> m_waterFalls;
    // The feet of the falls drawn last (water.cpp FootSegment, 8 floats each): the spray, the churn in the disturbance
    // field and the foam on the pool start there.
    std::vector<float> m_waterFoot;
    VkPipeline m_waterSprayPipeline = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_waterSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_waterLayout = VK_NULL_HANDLE;
    VkPipeline m_waterPipeline = VK_NULL_HANDLE;
    VkPipeline m_waterSurfacePipeline = VK_NULL_HANDLE;   // the surface's tessellated patches (water_surface.*)
    VkSampler m_waterRepeatSampler = VK_NULL_HANDLE;
    Texture* m_waterDetail = nullptr;            // RG ripple slope, B noise, A cell edges (tiled, mipmapped)
    Texture* m_waterSceneCopy = nullptr;         // the scene before the water (once a frame)
    VkImage m_waterDepthCopy = VK_NULL_HANDLE;   // ... and its depth
    VkImageView m_waterDepthCopyView = VK_NULL_HANDLE;
    VmaAllocation_T* m_waterDepthCopyAllocation = nullptr;
    VkImageLayout m_waterDepthCopyLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t m_waterDepthW = 0, m_waterDepthH = 0;
    uint64_t m_waterCopyFrame = ~0ull;
    uint64_t m_waterDraws = 0, m_waterTriangles = 0;
    uint32_t m_waterLogCount = 0;
    // The characters of the last completed frame, merged (one body per character) and how each moves. They push the
    // water's disturbance field (StepWaterSim, water_sim.comp), a wave equation on a grid round the camera: a body
    // moving raises the water ahead of it and leaves a trough behind, so wakes, bow waves and their interference come
    // out of the simulation. Collected per frame (CollectWaterBodies), the nearest few handed to the simulation. The
    // simulation tests "in the water" itself (the feet below the surface there), so no water map is needed here.
    struct WaterBody {
        float x = 0, z = 0, minY = 0;   // world position, the box's lowest y
        float vx = 0, vz = 0;           // its velocity (smoothed over frames)
        float speed = 0;                // |v|
        float centreY = 0;              // the box's mid height, and how fast it falls (m/s, smoothed): a jump in
        float fall = 0;
        float prevMinY = 0;             // the feet's height last frame: a splash is the feet crossing the surface
        float phase = 0;                // its idle stirring's offset - kept while it is tracked
    };
    std::vector<WaterBody> m_waterBodies;
    double m_waterBodiesTime = 0.0;
    float m_waterDt = 1.0f / 60.0f;
    uint32_t m_waterPhaseSeed = 2463534242u;
    void CollectWaterBodies();
    // The disturbance field: two states (ping-pong), RGBA16F, kept in the general layout; a compute pass steps it once
    // a frame, when the water first draws.
    VkDescriptorSetLayout m_waterSimSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_waterSimLayout = VK_NULL_HANDLE;
    VkPipeline m_waterSimPipeline = VK_NULL_HANDLE;
    VkImage m_waterSim[2] = {};
    VkImageView m_waterSimView[2] = {};
    VmaAllocation_T* m_waterSimAllocation[2] = {};
    uint32_t m_waterSimCur = 0;                  // the state the water reads
    bool m_waterSimClear = true;                 // wipe it before the next step
    bool m_waterSimLive = false;                 // the field is current (the water reads it)
    float m_waterSimCell = 0.25f;
    double m_waterSimLastTime = -1e9;
    float m_waterSimCam[2] = {0, 0};
    double m_waterSimTime = 0.0;
    bool CreateWaterSim(std::string* error);
    void StepWaterSim(VkCommandBuffer cmd, const WaterMap* map);
    bool CreateWaterResources(std::string* error);
    void DestroyWaterResources();
    // Weather (rain.cpp).
    WeatherParams m_weatherParams;
    WeatherState m_weatherGame;                  // the game's, as last handed over
    double m_weatherGameTime = -1e9;             // when (m_frameClock)
    float m_rain = 0.0f;                         // how hard it rains now, 0..1 (eased towards the target)
    float m_stormSky = 0.0f;                     // how much a storm's sky now, 0..1: the rain's or the lightning's,
                                                 // eased (rain.cpp RenderRain) - the clouds, the sun, the storm's hue
    double m_lastGameStrike = -1e9;              // the game's last strike (m_frameClock)
    float m_strikeHeat = 0.0f;                   // the game's recent strikes, counted and fading (half a minute)
    float m_wetness = 0.0f, m_puddles = 0.0f;    // the ground soaked by the rain, the puddles on it (rain.cpp: slow
                                                 // to come, slower to dry)
    float m_rainWet = 1.0f;                      // RVK_RainWet: the wet ground and the puddles, x
    // The shared wind (wind.cpp UpdateWind, read through Wind()): every swaying, falling and drifting thing's.
    float m_windDir[2] = {0.848f, 0.530f};       // where it blows (world x, z, unit)
    // The water's swell: the wind's direction eased over a minute (in a storm the game's wind swings tens of degrees
    // every few seconds; the waves turned with it, sliding about). Its frame as the wind's (SwellFrame).
    float m_swellDir[2] = {0.848f, 0.530f};
    double m_swellOffset[2] = {0.0, 0.0};
    // The water's ripples' drift: the wind's direction x its clock, added up a frame at a time (as direction x clock,
    // a turn of the wind jumped them by whole tiles of the pattern).
    double m_rippleDrift[2] = {0.0, 0.0};
    float m_windStrength = 1.0f;                 // x the sway: 1 calm .. ~3 in a full storm
    float m_windGusts = 1.0f;                    // x RVK_GrassGusts: stronger gust fronts in storms
    float m_storm = 0.0f;                        // 0 .. 1: how stormy (the rain, eased)
    float m_windStormScale = 1.0f;               // RVK_WindStorm: how strong storms make the wind, x
    double m_windClockPrev = -1.0;               // m_frameClock at the last update
    bool m_stormTint = false;                    // RVK_StormTint: the game's storm tint drawn (with the new weather)
    uint32_t m_stormTintSkipped = 0;             // its draws left out (the wind log line)
    float m_stormSheetAlpha = 0.0f;              // the game's storm sheet drawn: its alpha, eased (Draw)
    double m_stormSheetTime = -10.0;
    bool m_stormFlash = false;                   // RVK_StormFlash: the game's lightning glow over the clouds drawn
    uint32_t m_stormFlashSkipped = 0;            // its draws left out (the wind log line)
    float m_lightning = 1.0f;                    // RVK_Lightning: our flash for each of the game's strikes, x (0: none)
    float m_glowSeen = 0.0f, m_glowLast = 0.0f;  // the game's lightning glow: its brightness this frame (so far), last
    float m_glowPos[3] = {};                     // ... where (world: its mesh's middle)
    double m_flashStart = -1e9;                  // the last strike (m_frameClock)
    double m_strikeClock = -1.0;                 // (a forced storm's strikes: the last frame's clock)
    float m_flashPos[3] = {};
    uint32_t m_flashSeed = 0, m_flashCount = 0;
    // A sand or ash storm (wind.cpp): its colour and strength from the game's tint, seen left out (Draw).
    uint32_t m_stormTintColour = 0;              // its vertex colour (ARGB: alpha its strength)
    double m_stormTintSeen = -1e9;               // when (m_frameClock)
    float m_sand = 0.0f;                         // 0 .. 1: how thick the storm's air (eased)
    float m_sandColour[3] = {0.75f, 0.47f, 0.19f};   // its colour (linear)
    float m_sandHaze = 0.1f;                     // RVK_SandHaze: x
    float m_sandBlow = 1.0f;                     // RVK_SandBlow: the blowing sand round the camera (smoke.cpp), x
    float m_sandClear = 6.0f;                    // RVK_SandClear: the sand clear round the camera (world units)
    float m_sandContrast = 2.0f;                 // RVK_SandContrast: the drawn sand's contrast (1 as made)
    float m_sandBoxSize = 32.0f;                 // the sand volume's box (smoke.cpp: 32, 48, 64 by the camera's zoom)
    bool m_sandProcedural = false;               // RVK_SandProc: the sand made procedurally (else simulated: the user's pick)
    float m_sandForce = 0.0f;                    // RVK_SandForce: a sand (ash) storm this strong whatever the game's weather
                                                 // (1: the game's full one; 0: the game's)
    bool m_sandAsh = false;                      // RVK_SandAsh: the forced one an ash storm
    // The sand volume (smoke.cpp UpdateSandVolume): a smoke volume round the camera in a sand or ash storm.
    // The sand's streak pattern (simulated and drawn): a point's place in it is its offset from m_sandPivot turned
    // into the wind's frame (along, across) plus m_sandOffset (the wind carrying it: along falls). Turned about a
    // point near the camera (moved, the offset made up for it, once the camera is 64 units off): turned about the
    // world's origin, thousands of units away, the wind's direction swinging a little slid it by hundreds.
    double m_sandPivot[2] = {0.0, 0.0};
    double m_sandOffset[2] = {0.0, 0.0};         // (wrapped: kSandOffsetWrap)
    bool m_sandPivotSet = false;
    // The sand's shelter map (sand_map.cpp): round the camera, what stands there (the ground, solid tops, see-through
    // spans) and the heights under which the wind is held off; built in slices into the next, swapped in.
    struct SandMap {
        int origin[2] = {0, 0};                  // its first cell (world cells)
        std::vector<float> ground, solid, crownLo, crownHi, feed, shelterSolid, shelterCrown;
        std::vector<float> wallLo, floor;        // (building it: walls' lowest point over a cell, floors' highest)
        // The rain's (sand_map.cpp RainCoverMap): the highest solid thing over a cell, whether it reaches the ground or
        // not (a roof, an overhang, an arch - the sand's solid keeps only what does); how far its ground lies below
        // the ground round it (a hollow: puddles gather there).
        std::vector<float> cover, hollow;
        uint32_t coverGen = 0;                   // (m_coverMeshGen when built)
        std::vector<uint8_t> fine;               // (the ground captured close up - not the coarse terrain grid's)
        // The flow round solid obstacles (SandFlow): the wind's direction there (x z, a unit wind's), how far the air
        // arriving there was pushed aside on its way (x z, world), the nearest obstacle's top (world y).
        std::vector<float> flowX, flowZ, offsetX, offsetZ, nearTop;
        std::vector<float> nearDX, nearDZ;       // ... from the nearest solid cell to this one (world; SandNearest)
        bool anySolid = false;
        bool valid = false;
        double built = -1e9;
        uint32_t statelGen = 0;
    };
    // A shelter and flow sweep for a wind (sand_map.cpp StepSandSweep: in slices, then swapped into its map).
    struct SandSweep {
        bool active = false;
        int stage = 0, row = 0;
        float wind[2] = {0, 0};
        std::vector<float> solid, crown, flowX, flowZ, offX, offZ;
    };
    struct SandMapJob {
        bool active = false, nearest = false, swept = false;
        uint32_t row = 0;
        size_t item = 0;
        std::vector<uint32_t> items;
        std::vector<uint64_t> meshes;            // (m_coverMeshes over the map, then)
        size_t mesh = 0;
        SandSweep sweep;                         // (the new map's, before it is swapped in)
    };
    SandSweep m_sandSweep;                       // the current map's, the wind having turned
    struct SandColumn { float ground, top, shelterSolid, shelterCrown, feed, flow[2], offset[2], nearTop; };
    SandMap m_sandMap, m_sandMapNext;
    SandMapJob m_sandMapJob;
    std::vector<float> m_sandCoarseShelter;      // the terrain grid's (terrain_sun.cpp) shelter heights
    std::vector<float> m_sandCoarsePending;      // ... being swept (32 rows a frame)
    bool m_sandCoarseJob = false;
    int m_sandCoarseRow = 0, m_sandCoarseJobOrigin[2] = {0, 0};
    float m_sandCoarseJobWind[2] = {0, 0};
    int m_sandCoarseOrigin[2] = {0, 0};
    float m_sandCoarseWind[2] = {0, 0}, m_sandShelterWind[2] = {0, 0};   // the wind they were swept for
    double m_sandCoarseMade = -1e9, m_sandMapMs = 0.0, m_sandMapMaxMs = 0.0, m_sandShelterMade = -1e9;
    uint32_t m_sandShelterGen = 0, m_sandGroundShelterGen = ~0u;
    float m_sandEyeShelter = 0.0f;               // the camera's (eased): the near haze held off
    float m_sandGusts = 1.0f;                    // RVK_SandGusts: the distant gusts in the haze (0 none)
    float m_rainFog = 1.0f;                      // RVK_RainFog: a thunderstorm's rain mist in the fog, x
    float m_stormDark = 1.0f;                    // RVK_StormDark: a thunderstorm hides the sun from the land, x
    float m_stormGreen = 0.6f;                   // RVK_StormGreen: its clouds and mist cast green (0: grey)
    float m_stormThick = 1.0f;                   // RVK_StormThick: how dense and dark a storm's clouds (2: ~as before
                                                 // 2026-10-19, nearly black)
    float m_rainVeils = 1.0f;                    // RVK_RainVeils: a storm's grey rain veils under its far clouds, x
    float m_sandFog = 1.0f;                      // RVK_SandFog: the storm's air a lit volume (docs/sand-fog-plan.md)
    float m_sandSun = 0.9f;                      // RVK_SandSun: how clearly the sun shows through the fog (0 .. 1)
    float m_sandTint = 0.6f;                     // RVK_SandTint: the light under the fog the dust's colour (0 .. 1)
    double m_sandFarCarried = 0.0;               // how far the wind has carried them (world; wrapped at 1e6)
    void UpdateSandMap();
    void RasterSandItem(SandMap& m, uint32_t item);
    // Three world points a triangle: into the map from above (RasterSandItem's own, for any piece).
    void RasterSandTriangles(SandMap& m, const float* tri, size_t triangles, bool seeThrough);
    // The remembered (cached) static casters' triangles in world space - what the game draws that isn't a placed
    // object (a playfield's buildings, roofs, awnings): rasterised into the sand / rain map too (CacheCaster, kept while
    // remembered).
    struct CoverMesh { float boundsMin[3], boundsMax[3]; std::vector<float> tri; bool seeThrough; };
    std::unordered_map<uint64_t, CoverMesh> m_coverMeshes;
    size_t m_coverTriangles = 0;
    uint32_t m_coverMeshGen = 0;
    void AddCoverMesh(uint64_t key, const CachedCaster& e, const void* vertices, const uint16_t* indices);
    void SandFlowCells(const SandMap& m, SandSweep& j);
    void SandOffsetRows(SandSweep& j, int from, int to);
    bool StepSandSweep(SandSweep& j, SandMap& m, double deadline);
    void SandNearest(SandMap& m);
    void FinishSandCells(SandMap& m);
    float CoarseTerrainAt(float x, float z) const;
    float CoarseShelterAt(float x, float z) const;
    bool SandMapAt(float x, float z, SandColumn* out) const;
    float SandMapSolidIn(float x0, float z0, float x1, float z1) const;
    float SandShelterAt(float x, float y, float z) const;
    // The map's solid shelter for the sand fog (sand_fog.frag binding 5): out its corner x, z, 1 / its size, the
    // height its tops are taken from; false without a map.
    bool SandFogShelter(float out[4]);
    // The rain's cover map (sand_map.cpp: rain.vert, splash.vert, the wet ground): RGBA16F over the sand map's cells -
    // the highest solid over each, the see-through cover's top and bottom (over m_rainCoverRef), the ground's hollow.
    // out: its corner x, z, 1 / its size, the height its heights are taken from; false without a map.
    bool RainCoverMap(float out[4]);
    Texture* m_rainCover = nullptr;
    uint32_t m_rainCoverGen = ~0u;
    float m_rainCoverRef = 0.0f;
    float m_rainCoverOut[4] = {};                // (this frame's: RainCoverMap's out, 0 0 0 0 without)
    void DumpRainCover(std::FILE* f, float x, float z);   // (the frame dump: the rain's cover there, the pieces over it)
    void DumpSandMap(const std::string& path) const;
    std::vector<float> m_sandGround;             // per column (4 floats): its top (the ground or a solid's; box units),
                                                 // how much sand it gives, the shelter heights solid, see-through (box
                                                 // units)
    float m_sandGroundCorner[3] = {1e30f, 0, 0}; // ... for the box there
    uint32_t m_sandGroundN = 0;
    uint64_t m_sandGroundFrame = 0;              // ... made then (made again now and then: the ground comes in)
    bool SandActive() const { return m_sand > 0.01f && m_sandBlow > 0.0f; }
    float m_rainWind[3] = {0, 0, 0};
    double m_rainClock = 0.0;                    // the rain's own time (s), for the drops' fall
    double m_rainDrift[3] = {0, 0, 0};           // how far the rain has moved (its velocity added up: it leans as the
                                                 // wind changes without the drops jumping)
    double m_rainLastClock = -1.0;
    uint32_t m_rainLogFrame = 0;
    VkDescriptorSetLayout m_rainSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_rainLayout = VK_NULL_HANDLE;
    VkPipeline m_rainPipeline = VK_NULL_HANDLE;
    VkPipeline m_splashPipeline = VK_NULL_HANDLE;
    VkPipeline m_rainNearPipeline = VK_NULL_HANDLE;   // the streaks again, into m_rainNear alone
    VkPipeline m_boltPipeline = VK_NULL_HANDLE;  // our lightning bolts, into m_rainNear (bolt.vert/frag)
    VkPipeline m_debrisPipeline = VK_NULL_HANDLE;   // a storm's blown debris (debris.vert/frag)
    void RenderDebris(VkCommandBuffer cmd);
    double m_debrisDrift[3] = {0, 0, 0}, m_debrisClock = -1.0, m_debrisClockAll = 0.0;
    float m_stormDebris = 1.0f;                  // RVK_StormDebris: blown leaves, straw and twigs, x
    std::vector<float> m_boltSegs;               // the last strike's bolt: per segment a (xyz, width), b (xyz, brightness)
    float m_stormBolt = 1.0f;                    // RVK_StormBolt: our own bolts (the game's left out), x; 0: the game's
    void MakeBolt(const float top[3], const float bottom[3], uint32_t seed);
    Texture* m_rainNear = nullptr;               // RGBA16F, full size: the rain drawn this frame (premultiplied) - the
                                                 // tone mapping keeps it in front of the storm's fog and the haze
    uint64_t m_rainNearFrame = ~0ull;            // (the frame it was drawn in)
    bool CreateRainResources(std::string* error);
    void DestroyRainResources();
    void RenderRain(VkCommandBuffer cmd);
    // Smoke (smoke.cpp).
    struct SmokeImage {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation_T* allocation = nullptr;
        VkImageView view = VK_NULL_HANDLE;
    };
    struct SmokeVolume {
        uint64_t key = 0;                        // 0: free
        SmokeSource source;
        double lastSeen = -1e9;                  // m_frameClock it was last handed over
        float corner[3] = {0, 0, 0};             // the box's lowest corner (world)
        float lastSize = 0.0f;                   // its size the last step (0: none yet; the sand's follows the zoom)
        bool fresh = true;                       // its images to be cleared first
        float age = 0.0f;                        // seconds simulated (warming up: faster, fading in)
        uint64_t parent = 0;                     // a drift cloud (smoke a character carries out of a source's box):
                                                 // that source's key; 0 a source's own
        float simWait = 0.0f;                    // seconds since its last step (stepped every kSimStep or so)
        float lastStep = 0.0f;                   // ... and that step's (drawn between it and the state before)
        uint32_t cur = 0;                        // which of each pair holds the state
        uint32_t n = 0;                          // its images' cells a side (0: none yet)
        SmokeImage vel[2], dens[2];
        SmokeImage look;                         // what the drawing reads: R density, G the sunlight reaching it; B A
                                                 // the same a step before (RGBA16F, written by the simulation's last stage)
        SmokeImage flow;                         // its velocity for the drawing's wisps: half the cells a side, RGBA8
                                                 // (/ kFlowMax)
        uint64_t shownFrame = 0;                 // the last frame its box showed past what is in front of it
        bool shownTested = false;                // ... known (smoke_seen.comp; untested: taken as showing)
    };
    // Which volumes' boxes show (smoke.cpp MeasureSmokeShown, smoke_seen.comp): a slot a frame in flight, read when it
    // comes round again; the volumes it measured (their index in m_smokeVolumes, their key).
    VkPipeline m_smokeSeenPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_smokeSeenLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_smokeSeenSetLayout = VK_NULL_HANDLE;
    VkBuffer m_smokeSeenBuffer = VK_NULL_HANDLE;
    VmaAllocation_T* m_smokeSeenAllocation = nullptr;
    uint8_t* m_smokeSeenCpu = nullptr;
    VkDeviceSize m_smokeSeenStride = 256;
    uint32_t m_smokeSeenCount[kFramesInFlight] = {};
    uint32_t m_smokeSeenIndex[kFramesInFlight][6] = {};
    uint64_t m_smokeSeenKey[kFramesInFlight][6] = {};
    void MeasureSmokeShown(VkCommandBuffer cmd, const std::vector<std::pair<float, SmokeVolume*>>& live, uint32_t sw,
                           uint32_t sh);
    void ReadSmokeShown();
    bool CreateSmokeSeen();
    void DestroySmokeSeen();
    SmokeParams m_smokeParams;
    // Clouds (clouds.cpp).
    struct CloudPuff {
        d3d::Matrix world, view, proj;           // as the game drew it
        float low[3], high[3];                   // its mesh's extent (mesh space)
        int thin = 0;                            // ... its least extent's axis (the sheet's thickness)
        float uvU[4], uvV[4];                    // its texture over mesh space (u, v = dot(xyz, p) + w)
        float colour[4];                         // as the game lit it; a: its opacity
        float fogColour[4], fogParams[4];        // the fog it was drawn with (w: on), start, end, density, mode
        Texture* texture = nullptr;
        uint32_t rdbId = 0;                      // its texture's RDB id (single_cloud01..04: kCloudTex*)
    };
    CloudParams m_cloudParams;
    std::vector<CloudPuff> m_cloudPuffs;         // this frame's, taken from the sky's draws
    // The last frame's that had any (RVK_CloudLayer: looking down or away the game draws none - the shadows on the
    // ground and the weather go on from these for a few seconds).
    std::vector<CloudPuff> m_cloudPuffsLast;
    uint64_t m_cloudPuffsLastFrame = 0;
    // The game's sheets drawn behind the layer (RVK_CloudSheet; FrameLights cloudTint/Sky/Air), with the layer's
    // colours (last frame's: MarchCloudLayer).
    bool CloudSheetsOn() const { return m_cloudParams.enable && m_cloudParams.layer && m_cloudParams.sheet > 0.0f; }
    float m_sheetTint[4] = {}, m_sheetSky[4] = {}, m_sheetAir[4] = {};
    bool m_cloudsDrawn = false;                  // drawn this frame (where the sky ended)
    bool m_drawIsCloudPuff = false;              // the current visual is a cloud puff (SetDrawVisual)
    bool m_cloudNoiseMade = false;
    VkImage m_cloudNoise = VK_NULL_HANDLE;
    VmaAllocation_T* m_cloudNoiseAllocation = nullptr;
    VkImageView m_cloudNoiseView = VK_NULL_HANDLE;
    VkSampler m_cloudRepeatSampler = VK_NULL_HANDLE, m_cloudClampSampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_cloudSetLayout = VK_NULL_HANDLE, m_cloudNoiseSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_cloudLayout = VK_NULL_HANDLE, m_cloudNoiseLayout = VK_NULL_HANDLE;
    VkPipeline m_cloudPipeline = VK_NULL_HANDLE, m_cloudNoisePipeline = VK_NULL_HANDLE;
    VkPipeline m_cloudHalfPipeline = VK_NULL_HANDLE, m_cloudUpPipeline = VK_NULL_HANDLE;   // RVK_CloudHalf
    VkPipeline m_sunPipeline = VK_NULL_HANDLE;   // the suns (sun.cpp)
    VkPipelineLayout m_sunLayout = VK_NULL_HANDLE;
    VkPipeline m_sunVisPipeline = VK_NULL_HANDLE;   // how much of each is seen (sun_vis.comp)
    VkPipelineLayout m_sunVisLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_sunVisSetLayout = VK_NULL_HANDLE;
    VkBuffer m_sunVisBuffer = VK_NULL_HANDLE;    // its results, a slot a frame in flight (host-visible)
    VmaAllocation_T* m_sunVisAllocation = nullptr;
    uint8_t* m_sunVisCpu = nullptr;
    VkDeviceSize m_sunVisStride = 256;
    VkPipeline m_cloudUpScenePipeline = VK_NULL_HANDLE;   // ... the filtering up inside the scene's rendering (its depth)
    VkDescriptorSetLayout m_cloudUpSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_cloudUpLayout = VK_NULL_HANDLE;
    Texture* m_cloudHalf = nullptr;              // RVK_CloudHalf: the clouds at half size (premultiplied)
    // RVK_CloudShadow: the sunlight through the cloud layer, top down round the camera (R: 1 = clear).
    Texture* m_cloudShadowMap = nullptr;
    uint64_t m_cloudShadowFrame = 0;             // made last in this frame
    float m_cloudShadowArea[4] = {};             // its corner x, z (world), its size, the layer's height (world y)
    VkDescriptorSetLayout m_cloudShadowSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_cloudShadowLayout = VK_NULL_HANDLE;
    VkPipeline m_cloudShadowPipeline = VK_NULL_HANDLE;
    void MakeCloudShadow(VkCommandBuffer cmd);
    // RVK_CloudLayer (clouds.cpp; docs/clouds-layer-plan.md): the shape and detail noise (3D, made once), the weather
    // map (each frame), the march.
    struct CloudVolume {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation_T* allocation = nullptr;
        VkImageView view = VK_NULL_HANDLE;
    };
    CloudVolume m_cloudShape, m_cloudDetail;
    bool m_cloudLayerNoiseMade = false;
    VkPipelineLayout m_cloudLayerNoiseLayout = VK_NULL_HANDLE;
    VkPipeline m_cloudLayerNoisePipeline = VK_NULL_HANDLE;
    Texture* m_cloudWeather = nullptr;
    float m_cloudWeatherArea[3] = {};            // its corner x, z (world), its size
    VkDescriptorSetLayout m_cloudWeatherSetLayout = VK_NULL_HANDLE, m_cloudLayerSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_cloudWeatherLayout = VK_NULL_HANDLE, m_cloudLayerLayout = VK_NULL_HANDLE;
    VkPipeline m_cloudWeatherPipeline = VK_NULL_HANDLE, m_cloudLayerPipeline = VK_NULL_HANDLE;
    float m_cloudBase = 0.0f;                    // the layer's base (world y), easing after the eye's height
    float m_cloudCloudy = 0.0f, m_cloudType = 0.5f;   // the weather map's cloudiness and type, eased
    double m_cloudWeatherClock = -1.0;
    float m_cloudTint[3] = {0.8f, 0.8f, 0.8f}, m_cloudHaze[3] = {0.5f, 0.5f, 0.5f};   // eased (MarchCloudLayer)
    double m_cloudColourClock = -1.0;
    float m_cloudFogged = 0.0f;                  // how fogged the game's clouds are (0..1), eased
    float m_cloudWispy = 0.0f;                   // the game's wispy puffs' share (eased; logged)
    float m_cloudDeck = 0.0f;                    // the low deck's amount (eased)
    float m_cloudDrift[3] = {};                  // the clouds' drift (world x, z) and evolution (UpdateCloudDrift)
    float m_scudDrift[2] = {};                   // a thunderstorm's scud: the low deck racing ahead of the layer (x, z)
    float m_cloudWindDir[2] = {0.8f, 0.6f};
    double m_cloudDriftClock = -1.0;
    void UpdateCloudDrift();
    double m_cloudBaseClock = -1.0;
    void MakeCloudWeather(VkCommandBuffer cmd, const float eye[3]);
    void MarchCloudLayer(VkCommandBuffer cmd, bool skipCovered, const float sub[4], const float* shadowArea = nullptr,
                         bool sunRay = false);
    // The sunlight the clouds let through to the eye (cloud_layer_sunray.frag; sun.cpp reads it, shadow.cpp dims the
    // low frame sun by it): kSunRays rays' transmittance, copied into the sun visibility buffer's slot at kSunRayOffset.
    static constexpr uint32_t kSunRays = 16, kSunRayOffset = 64;
    VkPipeline m_cloudSunRayPipeline = VK_NULL_HANDLE;
    Texture* m_cloudSunRay = nullptr;
    bool m_sunRayMeasured[kFramesInFlight] = {};
    void MakeCloudSunRay(VkCommandBuffer cmd);
    void MakeCloudLayerShadow(VkCommandBuffer cmd, const float eye[3]);
    VkPipeline m_cloudLayerShadowPipeline = VK_NULL_HANDLE;
    // ... reused over frames (cloud_resolve.frag): one half-size pixel of each 2x2 marched a frame (into the quarter-
    // size target), the half-size clouds rebuilt from them and last frame's (two, in turn).
    Texture* m_cloudQuarter = nullptr;
    Texture* m_cloudHistory[2] = {};
    uint32_t m_cloudHistoryIndex = 0;
    bool m_cloudHistoryValid = false;
    uint64_t m_cloudHistoryFrame = 0;            // the frame that made the newest
    float m_cloudHistoryEye[3] = {};
    // The cloud layer's last march, for its lightning added when filtered up (clouds.cpp DrawClouds, cloud_up.frag).
    d3d::Matrix m_cloudUpInvViewProj{};
    float m_cloudUpEye[4] = {}, m_cloudUpTint[4] = {}, m_cloudUpReach = 0.0f;
    uint64_t m_cloudUpFrameNumber = ~0ull;       // (the frame it was marched in)
    float m_cloudMorphPhase = 0.0f;              // the clouds' churn: how far through its field (tiles, wrapped at 1)
    float m_cloudBoil = 0.0f;                    // ... their detail's extra rise (world, wrapped)
    double m_cloudWeatherTime = 0.0;             // the weather map's own clock (faster in a storm: its masses change)
    d3d::Matrix m_cloudPrevRotation{};           // last frame's view-projection, the camera's rotation only
    VkDescriptorSetLayout m_cloudResolveSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_cloudResolveLayout = VK_NULL_HANDLE;
    VkPipeline m_cloudResolvePipeline = VK_NULL_HANDLE;
    Texture* ReusedCloudLayer(VkCommandBuffer cmd, bool skipCovered, const float eye[3]);
    VkImageView CloudShadowView() const;         // the map to bind (any image when there is none: FL.clouds says so)
    bool CreateCloudResources(std::string* error);
    void DestroyCloudResources();
    bool CaptureCloudPuff(uint32_t fvf, const void* vertices, uint32_t vertexCount, const uint16_t* indices,
                          uint32_t indexCount);
    void DrawClouds(VkCommandBuffer cmd);
    void MarchCloudPuffs(VkCommandBuffer cmd, const std::vector<std::pair<float, const CloudPuff*>>& order, bool half,
                         bool skipCovered, float lo[2], float hi[2]);
    void MakeCloudNoise(VkCommandBuffer cmd);
    std::vector<SmokeVolume> m_smokeVolumes;
    SmokeImage m_smokeCurl, m_smokeDiv, m_smokePressure[2];
    SmokeImage m_smokeNoise;                     // the wisps' noise (tiling; made once, smoke_sim.comp stage 10)
    bool m_smokeNoiseMade = false;
    bool m_smokeSharedFresh = true;
    uint32_t m_smokeN = 0;                       // the shared scratch images' cells a side (the most a volume has)
    float m_smokeSlow = 1.0f;                    // how much the smoke steps are stretched to keep RVK_SmokeBudget
    double m_smokeSlowLogged = 0.0;
    // The steps' estimated cost (ms, StepCost) recorded in each frame slot, and the measured / estimated ratio from the
    // slots' GPU timestamps ("smoke simulation"), eased: the budget in this GPU's (and this load's) real time.
    double m_smokeEstimate[2] = {};
    float m_smokeCostScale = 1.0f;
    double m_smokeLastClock = -1.0;
    VkSampler m_smokeSampler = VK_NULL_HANDLE, m_smokeRepeatSampler = VK_NULL_HANDLE;
    VkBuffer m_smokeBounds = VK_NULL_HANDLE;     // per volume: the cells with smoke (lowest, highest; the simulation's)
    VmaAllocation_T* m_smokeBoundsAllocation = nullptr;
    VkDescriptorSetLayout m_smokeSimSetLayout = VK_NULL_HANDLE, m_smokeSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_smokeSimLayout = VK_NULL_HANDLE, m_smokeLayout = VK_NULL_HANDLE;
    VkPipeline m_smokeSimPipeline = VK_NULL_HANDLE, m_smokePipeline = VK_NULL_HANDLE;
    VkPipeline m_smokeHalfPipeline = VK_NULL_HANDLE, m_smokeUpPipeline = VK_NULL_HANDLE;   // RVK_SmokeHalf
    VkDescriptorSetLayout m_smokeUpSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_smokeUpLayout = VK_NULL_HANDLE;
    Texture* m_smokeHalf = nullptr;              // RVK_SmokeHalf: the smoke at half size (premultiplied)
    Texture* m_smokeHalfDepth = nullptr;         // ... and the view depth each pixel was marched to (R)
    uint64_t m_smokeHalfFrame = ~0ull;           // the frame m_smokeHalf was drawn (the tone mapping's sand: its cover)
    bool CreateSmokeResources(std::string* error);
    void DestroySmokeResources();
    void DestroySmokeImages();
    bool MakeSmokeImage(VkFormat format, uint32_t n, SmokeImage& out);
    void RenderSmoke(VkCommandBuffer cmd);
    void UpdateSmokeDrift(double now);
    void UpdateSandVolume(double now, float dt);
    void MakeSandGround(const float corner[3], float size, uint32_t n);
    void UpdateWaterReady();
    const WaterGrid* WaterGridFor(uint32_t segments);
    void SplitWaterTriangles(const WaterVertex* v, uint32_t count, const uint16_t* idx, uint32_t icount,
                             std::vector<uint32_t>& flat, std::vector<uint32_t>& steep);
    WaterMap* WaterMapFor(uint64_t key, const WaterVertex* v, const std::vector<uint32_t>& flat);
    bool CopySceneForWater(VkCommandBuffer cmd, const WaterMap* map);
    void WaterLog(const char* fmt, ...);
    bool CreateGrassResources(std::string* error);
    void DestroyGrassResources();
    uint64_t m_grassDraws = 0, m_grassBlades = 0, m_grassBuilds = 0;   // counters, logged with the foliage line
    double m_grassBuildMs = 0.0, m_grassCaptureMs = 0.0;
    uint64_t m_grassLitBlades = 0, m_grassBuiltBlades = 0;   // built blades, and those with the ground's lightmap
    // Candidates left out in the builds, by why: the clumps/tufts, no grass ground under it, too steep, near its edge.
    uint64_t m_grassLeftOut[4] = {};
    VkDescriptorSetLayout m_grassSetLayout = VK_NULL_HANDLE;   // camera, blades, frame lights, shadow maps
    VkPipelineLayout m_grassLayout = VK_NULL_HANDLE;
    VkPipeline m_grassPipeline = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_grassShadowSetLayout = VK_NULL_HANDLE;   // the blades (grass_shadow.vert)
    VkPipelineLayout m_grassShadowLayout = VK_NULL_HANDLE;
    VkPipeline m_grassShadowPipeline = VK_NULL_HANDLE;
    uint64_t m_grassShadowBlades = 0;            // blades drawn into the sun's cascade (the log's 600 frames)
    uint64_t m_grassPointShadowBlades = 0;       // ... and into the point lights' cube faces
    // The blade pool (one storage buffer, suballocated per tile, its virtual block counted in blades) and the shared
    // index pattern (six triangles a blade near, then four far).
    VkBuffer m_grassPool = VK_NULL_HANDLE;
    VmaAllocation_T* m_grassPoolAllocation = nullptr;
    VmaVirtualBlock_T* m_grassPoolBlock = nullptr;
    uint32_t m_grassPoolBlades = 0;
    VkBuffer m_grassIndex = VK_NULL_HANDLE;
    VmaAllocation_T* m_grassIndexAllocation = nullptr;
    uint32_t m_grassIndexBlades = 0;
    // Leaves (leaves.cpp). A canopy - a lit, static, cut-out mesh with a leaf texture - gets a leaf set: its sprigs
    // baked once (model space) into one pool, drawn after the canopy's own draw with its record's state.
    enum class Canopy : uint8_t { None, Shrub, Tree, Palm };
    struct LeafSet {
        Canopy kind = Canopy::None;
        uint64_t alloc = 0;                      // its range in m_leafPool (VmaVirtualAllocation), 0 = none
        uint32_t first = 0, count = 0;           // in leaves
        uint32_t texVersion = 0;                 // the texture's m_alphaVersion it was baked from
        uint32_t settingsVersion = 0;            // m_leafVersion it was baked with
        uint64_t lastFrame = 0;
        float centre[3] = {}, radius = 0.0f;     // the crown (model space)
        bool baked = false;
        // A tree with its own leaf art (the canopy texture's m_leafArt, RVK_LeafOwn): our sprigs in place of its cards.
        Texture* art = nullptr;
        uint32_t artVersion = 0;
    };
    std::unordered_map<uint64_t, LeafSet> m_leafSets;   // by mesh key and texture
    LeafSettings m_leaf;
    bool m_leafDirty = false;                    // density / size changed: every set is baked again
    uint32_t m_leafVersion = 1;
    VkBuffer m_leafPool = VK_NULL_HANDLE;
    VmaAllocation_T* m_leafPoolAllocation = nullptr;
    VmaVirtualBlock_T* m_leafPoolBlock = nullptr;
    uint32_t m_leafPoolLeaves = 0;
    struct LeafTrash { uint64_t alloc; VkBuffer buffer; VmaAllocation_T* allocation; uint64_t frame; };
    std::vector<LeafTrash> m_leafTrash;
    VkPipeline m_leafPipelines[2] = {};          // 8-bit target, HDR scene (leaf.vert + ffp.frag)
    uint32_t m_leafBakes = 0, m_leafBakedLeaves = 0;   // counters (logged with the foliage line)
    uint32_t m_leafPrepassed = 0;                // ... leaf draws whose sprigs went into the depth pre-pass
    double m_leafBakeMs = 0.0;
    uint64_t m_leafDraws = 0, m_leafDrawn = 0;   // leaf draws and leaves drawn (the log's 600 frames)
    uint64_t m_canopyCount[4] = {};              // canopy draws by kind (the log's 600 frames)
    LeafSet* m_drawLeaves = nullptr;             // the current draw's leaf set, ready to draw (CanopyParams)
    uint64_t m_drawLeafKey = 0;                  // ... its key in m_leafSets
    VkPipeline m_leafShadowPipeline = VK_NULL_HANDLE;   // leaf_shadow.vert + shadow.frag
    uint64_t m_leafShadowLeaves = 0;             // leaves drawn into the sun's cascades (the log's 600 frames)
    void DrawLeafShadows(VkCommandBuffer cmd);   // the current cascade's canopies' leaves (shadow.cpp RenderShadowMap)
    uint32_t m_drawLeafCount = 0;                // ... how many of its leaves at this distance (with the falling slots
                                                 // in its record: DrawTransform leafSet)
    uint32_t m_drawLeafFall = 0;                 // ... its falling-leaf slots (ffp.vert FallingLeaf)
    float m_drawLeafGround = 0.0f;               // ... the ground under it (world y: where its fallen leaves lie)
    // The ground under a tree (LeafGround): each part of a mesh has its own origin - a canopy's is up in its crown -
    // so it comes from the captured terrain (the ground grass), else from the trunk: a solid static mesh drawn next to
    // the canopy, standing under it. Recent solid draws' world boxes, and per tree (canopy mesh + position) what was
    // found; a trunk drawn after its canopy is matched to it then, for the next frames.
    struct SolidBox { float lo[3], hi[3]; uint32_t draw; };
    SolidBox m_recentSolid[16] = {};
    uint32_t m_recentSolidNext = 0;
    struct RecentCanopy { uint64_t instance; float x, z, bottom; uint32_t draw; };
    RecentCanopy m_recentCanopy{};
    std::unordered_map<uint64_t, float> m_leafGround;   // tree instance -> ground y
    void NoteSolidDraw(uint32_t fvf);            // Draw: a solid static draw (a trunk?)
    float LeafGround(const LeafSet& set);
    Canopy CanopyKind(uint32_t primitive, uint32_t fvf, uint32_t stride, const void* vertices, uint32_t vertexCount,
                      const uint16_t* indices, uint32_t indexCount);
    void CanopyParams(uint32_t primitive, uint32_t fvf, const detail::FvfLayout& layout, const void* vertices,
                      uint32_t vertexCount, const uint16_t* indices, uint32_t indexCount);
    void BakeOwnCanopy(LeafSet& set, uint32_t primitive, const detail::FvfLayout& layout, const void* vertices,
                       uint32_t vertexCount, const uint16_t* indices, uint32_t indexCount, void* outLeaves);
    bool BakeLeaves(LeafSet& set, uint32_t primitive, const detail::FvfLayout& layout, const void* vertices,
                    uint32_t vertexCount, const uint16_t* indices, uint32_t indexCount);
    bool GrowLeafPool(uint32_t minLeaves);
    void CrownOf(LeafSet& set) const;
    void FreeLeaves(LeafSet& set);
    void UpdateLeaves();                         // once a frame: trash, unused sets, settings
    void DrawLeaves(VkCommandBuffer cmd, uint32_t recordIndex, uint32_t primitive, uint32_t fvf, uint32_t stride,
                    VkDeviceSize frameLightsOffset, VkBuffer prevBuffer, VkDeviceSize prevOffset,
                    VkDeviceSize prevBytes, VkBuffer smoothBuffer, VkDeviceSize smoothOffset, VkDeviceSize smoothBytes);
    uint32_t m_leafBakesThisFrame = 0;
    // GPU memory (diag.cpp GpuMemoryReport: in frame dumps and the 600-frame log).
    bool m_memoryBudget = false;                 // VK_EXT_memory_budget: the driver's usage and budget per heap
    VkDeviceSize m_textureBytes[2] = {};         // live textures' images: the game's, render targets
    uint32_t m_textureCount[2] = {};
    std::string GpuMemoryReport() const;
    void VramUse(uint64_t* used, uint64_t* budget) const;   // the GPU's own heaps, summed (the perf readout)
    void DestroyLeafResources();
    float m_pointLightScale = 1.0f, m_charLightScale = 1.0f;   // SetPointLightIntensity
    float LightScale(const d3d::Light& l) const;
    // RVK_PtLight for a lamp: outdoors only. In a room's playfield (no sun) the lamps are the room's light - its walls
    // and floor have theirs baked in at the game's strength - so the things in it take them at the game's strength too.
    float LampScale() const { return m_cond.sunKnown ? m_pointLightScale : 1.0f; }
    bool FoliageFar() const;                  // pieces per 0.3 world units (0 = plants drawn as the game gives them)
    bool DrawFartherThan(float distance) const;  // the current draw's box beyond `distance` from the camera
    void SubdividePlant(uint32_t& primitive, uint32_t fvf, const detail::FvfLayout& layout, const void*& vertices,
                        uint32_t& vertexCount, const uint16_t*& indices, uint32_t& indexCount);
    float m_drawSway[4] = {};                    // the current draw's sway (for its shadow caster)
    bool m_dynamicWriteMask = false;             // per-draw colour write masks (extended dynamic state 3)
    bool m_emulateVertexInput = false;           // dynamic vertex input emulated (dynemu.h: the driver lacks it)
    bool m_emulateBlend = false;                 // ... dynamic blend enable and equation
    void FrameLightMask(uint32_t fvf, uint32_t stride, const void* vertices, uint32_t vertexCount, uint32_t out[4]);
    struct LightSphere {
        float x, y, z, r2;
        // A spot light's cone (SpotCone, with its margin): the light gives nothing outside it - ambient included
        // (lighting.glsl) - so a draw wholly outside leaves it out of its mask. on: a cone under a half turn.
        float axis[3] = {}, cosHalf = 0.0f, sinHalf = 0.0f;
        bool spot = false;
    };
    std::vector<LightSphere> m_frameLightSpheres;  // the frame lights' spheres in FrameLights order (light masks)
    bool LightReachesBox(const LightSphere& l, const float mn[3], const float mx[3]) const;   // (draw.cpp)
    uint32_t m_pusherSeenMax = 0;                // the most pushers any frame in the window had (the grass's log)
    // A coarse x/z grid over those lights, so a draw's mask tests only the lights near it, not all of them (a busy
    // scene captures up to kFrameLights): per cell the mask of the lights whose sphere reaches into it, over the
    // lights' extent. Rebuilt by BuildLightGrid when the frame lights change; allocation-free after the first frames.
    static constexpr float kLightGridCell = 16.0f;
    static constexpr uint32_t kLightGridMaxCells = 1u << 16;   // beyond: the lights go to m_lightGridAlways
    std::vector<uint64_t> m_lightGrid;           // W x H cells from (m_lightGridX0, m_lightGridZ0), row-major by z
    int32_t m_lightGridX0 = 0, m_lightGridZ0 = 0;
    uint32_t m_lightGridW = 0, m_lightGridH = 0;
    uint64_t m_lightGridAlways = 0;              // lights too large for the grid: tested by every draw
    void BuildLightGrid();
    // ... the grid for the shaders too (FL.lightGrid, FL.pick.yzw): each pixel loops over only the lights reaching its
    // own cell, not every light reaching its draw's box - a ground patch's box is 256 units, and with RVK_LightOver
    // on, Newland City's 57 lights cost every ground pixel 57 tests (+5 ms). Two A8R8G8B8 texels a cell (the mask's
    // low and high words), 256 cells a row; uploaded once a frame.
    static constexpr uint32_t kLightGridTexCells = 256;   // cells a texture row
    Texture* m_lightGridTex = nullptr;
    uint64_t m_lightGridTexFrame = ~0ull;
    bool m_lightGridTexValid = false;
    void LightGridLights(detail::FrameLights* fl);
    int m_lightMaskVerify = -1;                  // RANDYVK_LIGHTMASK_VERIFY: check the grid against every light
    uint32_t m_lightMaskDiff = 0;
    void Wind(float out[4]) const;               // direction x, z, time, strength
    // The frame every pattern laid out along the wind (grass waves, gust fronts, water waves) is placed in: a point's
    // (along, across) = its offset from the pivot turned into the wind's frame, plus the offset; WindFrame folds the
    // two into what is added to (dot(xz, wd), dot(xz, wd turned left)) (wind.glsl WindAxes).
    // The pivot stays near the camera (moved once it is 64 units off, the offset making up for it): turned about the
    // world's origin, thousands of units away, the wind's direction swinging a little slid them by hundreds.
    void WindFrame(float out[4]) const;
    // (along, across) of a world x, z in it (as WindAxes).
    void WindAxes(float x, float z, float out[2]) const;
    // The swell's direction (xy) and its frame (zw, as WindFrame's xy), and the ripples' drift (xy, wrapped).
    void SwellFrame(float out[4]) const;
    double m_windPivot[2] = {0.0, 0.0}, m_windOffset[2] = {0.0, 0.0};
    bool m_windPivotSet = false;
    double m_windTime = 0.0, m_windTimePrev = 0.0;   // this frame's and last frame's (set at the frame's start)
    double m_frameClock = 0.0;                   // SwayClock at the frame's start (per-caster bookkeeping)
    bool m_taa = true;
    bool m_fxaa = false;
    float m_sharpen = 0.4f;
    float m_taaJitter[2] = {};                   // this frame's jitter, clip units (FrameLights taa)
    Texture* m_taaHistory[2] = {};               // resolved frames (RGBA16F): [m_taaIndex] is the last one
    uint32_t m_taaIndex = 0;
    uint64_t m_taaFrame = ~0ull;                 // frame of the last resolve (history valid if it was the previous)
    VkPipeline m_taaPipeline = VK_NULL_HANDLE, m_sharpenPipeline = VK_NULL_HANDLE, m_fxaaPipeline = VK_NULL_HANDLE;
    bool TaaActive() const { return m_taa && m_hdr; }
    // RVK_RenderScale: wanted, and what the scene's targets are made at now (100 without TAA). ApplyRenderScale (at
    // a frame's start) recreates them when the two differ.
    uint32_t m_renderScaleWanted = 100, m_renderScale = 100;
    // Frames with no 3D at all (loading screens) are drawn at full size after a second of them: what is in them is
    // the interface and 2D pictures, which the upscaling would only soften. Scaled again at the first 3D frame.
    uint32_t RenderScaleNow() const { return TaaActive() && m_framesWithout3D < 60 ? m_renderScaleWanted : 100u; }
    uint32_t m_framesWithout3D = 0;              // frames in a row whose scene drew no 3D (EndFrame)
    std::atomic<uint32_t> m_framesWithout3DShared{0};   // the same, for the game thread (FramesWithout3D)
    // The zone hand-over (handover.cpp, docs/zone-handover-plan.md, RVK_ZoneFade): the old zone's last picture held over
    // the blank frames of a zone change and the new zone's first ones, then faded out.
    void HandoverScene(VkCommandBuffer cmd);     // (EndScene, after the post passes)
    bool HandoverPick(bool before);
    void HandoverBegin(double now);
    bool HandoverShown() const;                  // the held picture is over the frame's scene (whole or fading)
    bool HandoverOpaque() const;                 // ... whole
    void DestroyHandover();
    enum class Handover : uint8_t { Idle, Armed, Holding, Fading };
    static constexpr uint32_t kHandoverFrames = 10;   // the new zone's frame times looked at for steadiness
    struct {
        Handover state = Handover::Idle;
        Texture* shots[2] = {};                  // the latest two 3D frames' finished scenes (the output's size)
        uint64_t shotFrame[2] = {};              // ... their frame numbers
        int latest = -1;                         // ... the newer one (-1: none)
        Texture* image = nullptr;                // the held picture: one of them
        bool cameraSet = false;
        bool picked = false;                     // the held picture chosen (Armed: the old playfield stopped)
        double armed = 0, holdStart = 0, first3D = 0, fadeStart = 0, fadeLength = 0.5, lastFrame = 0;   // (s)
        uint32_t frames3D = 0, frameCount = 0;
        float frameMs[kHandoverFrames] = {};
        float eye[3] = {}, forward[3] = {};      // the camera at the new zone's first 3D frame
        const NativeGround* groundAtArm = nullptr;   // a different one: the new zone's ground is in
    } m_ho;
    VkPipeline m_handoverPipeline = VK_NULL_HANDLE;
    // Shop portals, phase 0 (portal_diag.cpp, docs/shop-portal-plan.md): a playfield's pieces as first drawn after
    // arriving in it.
    void PortalDiagPlayfield(uint32_t playfield);
    void PortalDiagDraw(uint32_t fvf, uint32_t vertexCount, uint32_t indexCount);
    void PortalDiagFrame();
    // The reverse (docs/street-portal-plan.md, phase 0): what a walked-through doorway sees of the street (portal_diag.cpp).
    struct BlackQuad;
    void PortalStreetDiagStart(const BlackQuad& quad, const float outward[3], uint32_t interior);
    void PortalStreetDiagDraw(uint32_t fvf, uint32_t vertexCount, uint32_t indexCount, uint32_t stride);
    void PortalStreetDiagFrame();
    struct StreetDiag {
        bool active = false;
        uint32_t arrival = 0, interior = 0, framesLeft = 0, pieces = 0, terrain = 0;
        size_t bytes = 0, textureBytes = 0;
        float apex[3] = {}, dir[3] = {};
        char mesh[48] = {};
        std::unordered_set<uint64_t> seen;
        std::unordered_set<Texture*> textures;
    } m_sd;
    // Shop portals' capture (portal_capture.cpp): a room's playfield's static draws, saved when quiet.
    void PortalCapturePlayfield(uint32_t playfield);
    void PortalCaptureDraw(uint32_t primitive, uint32_t fvf, uint32_t stride, const void* vertices, uint32_t vertexCount,
                           const uint16_t* indices, uint32_t indexCount);
    void PortalCaptureFrame();                   // (BeginFrame, before anything is recorded)
    struct PortalCaptureState;
    void PortalCaptureSave(PortalCaptureState& state);
    void PortalNoteCharacters(PortalCaptureState& state);
    void PortalCaptureReadBack(PortalCaptureState& state);
    void PortalRecordPiece(PortalCaptureState& state, VisualKind kind, uint32_t primitive, uint32_t fvf, uint32_t stride,
                           const void* vertices, uint32_t vertexCount, const uint16_t* indices, uint32_t indexCount,
                           bool door = false);
    void PortalForgetTexture(Texture* texture);  // (DestroyTexture)
    bool ReadTexturesLevel0(const std::vector<Texture*>& textures, std::vector<std::vector<uint8_t>*>& out);   // (outside a frame)
    struct PortalDoor { float base[3], axis[3]; char name[48]; float width, height; };   // (size: its pieces' largest)
    struct PortalCaptureState {
        bool on = false, terrainSeen = false, roomsSeen = false, arrivalSet = false;
        std::shared_ptr<portal::Capture> capture;
        std::unordered_map<uint64_t, size_t> pieceAt;   // a piece's key -> its place in capture->pieces
        std::unordered_map<Texture*, int32_t> textureIndex;
        std::vector<std::pair<Texture*, int32_t>> pending;   // textures to read back
        std::vector<PortalDoor> doors;
        float arrival[3] = {}, arrivalForward[3] = {}, roomCentre[3] = {};
        float roomFloor = 1e30f;                 // the rooms' lowest point (an entrance without a door: its floor)
        uint32_t roomPieces = 0, unsaved = 0;
        uint32_t saves = 0, againLines = 0;   // saves this visit; lines on what made it unsaved after one
        size_t bytes = 0;                        // its vertices, indices and textures so far
        // A street's (portal_street.cpp): its file, and the doorway it is seen from - in front of it, the cone kept.
        bool street = false;
        std::string path;
        float apex[3] = {}, dir[3] = {};
        double lastNear = 0.0;
        float nearDistance = 0.0f;               // ... the doorway's distance from the camera then
        std::unordered_map<uint64_t, std::vector<std::array<float, 3>>> placesOf;   // a prop's places, by its geometry
        std::vector<std::array<float, 3>> characters;   // where characters stood (their attachments are dropped)
        double lastNew = 0.0, lastSave = 0.0;
    };
    PortalCaptureState m_pc, m_pcLeaving;        // (leaving: saved at the next frame's start)
    PortalCaptureState m_sc, m_scLeaving;        // a street's (portal_street.cpp), the same way
    // Shop portals' links (portal_links.cpp): the black texture's quads drawn (doorways and windows), and which
    // doorway leads to which room's playfield.
    struct BlackQuad {
        float centre[3] = {}, normal[3] = {}, modelCentre[3] = {};
        float modelNormal[3] = {};               // (normal on the mesh, before its world matrix)
        std::vector<float> triangles;            // its triangles in the world (9 floats each: the opening, portal.cpp)
        float sill = 0, width = 0, height = 0;
        char mesh[48] = {};
        uint16_t piece = 0;
        mutable uint64_t drawnFrame = ~0ull;     // its room drawn behind it (portal.cpp: once a frame)
    };
    struct BlackDraw {
        uint64_t lastFrame = 0;
        uint32_t reportedIn = 0;
        uint32_t linksGen = ~0u;                 // (hasPortal as of m_portalLinksGen)
        bool hasPortal = false;                  // a doorway of it leads to a room
        std::vector<BlackQuad> quads;
    };
    uint32_t m_portalLinksGen = 0;               // links loaded or learned
    bool m_portalArmed = false;                  // a doorway with a room drawn since its room was: DrawPortals next
    uint64_t m_portalsFrame = ~0ull;             // (the frame m_portalsDrawn counts)
    uint32_t m_portalsDrawn = 0;
    uint32_t m_portalArrivals = 1;               // playfield changes (PortalCapturePlayfield): BlackDraw::reportedIn
    void PortalReportLinks(BlackDraw& draw);
    std::unordered_map<uint64_t, BlackDraw> m_blackDraws;   // by geometry and place
    struct PortalLink {
        uint32_t outdoor = 0, interior = 0;
        char mesh[48] = {};
        uint16_t piece = 0;
        float modelCentre[3] = {}, centre[3] = {}, normal[3] = {};   // (normal: outward)
        float sill = 0, width = 0, height = 0;
        // Outward on the mesh (0 0 0: not known yet - a link saved before it was kept, learned when its doorway is
        // next drawn): the same building's doorway elsewhere faces out the same way.
        mutable float modelOutward[3] = {};
    };
    std::vector<PortalLink> m_portalLinks;
    bool m_portalLinksLoaded = false;
    struct PortalPending { bool active = false; double time = 0; uint32_t from = 0; BlackQuad quad; } m_portalPending;
    void PortalNoteBlack(uint32_t primitive, uint32_t fvf, uint32_t stride, const void* vertices, uint32_t vertexCount,
                         const uint16_t* indices, uint32_t indexCount);
    const PortalLink* PortalLinkFor(const BlackQuad& quad, uint32_t playfield);
    bool PortalOutward(const BlackQuad& quad, const PortalLink& link, float out[3]);
    void PortalLinkTeleport();                   // (ZoneSignal)
    void PortalLinkFrame();                      // (PortalCaptureFrame)
    void PortalLoadLinks();
    bool PortalSaveLinks();
    // Street portals' capture (portal_street.cpp): what a walked-through doorway sees of the street.
    void StreetCaptureNear(const PortalLink& link, const BlackQuad& quad, const float outward[3], float distance);
    void StreetCaptureDraw(uint32_t primitive, uint32_t fvf, uint32_t stride, const void* vertices, uint32_t vertexCount,
                           const uint16_t* indices, uint32_t indexCount);
    void StreetCaptureFrame();
    void StreetCaptureGround(const BlackQuad& quad, const float outward[3]);   // (the native ground in front of it)
    void StreetCaptureGroundPass(bool light);    // (NativeGroundReplaces: the game's ground passes' states)
    void StreetCaptureLight();                   // (the hour's lightmap over the cut-out, once there is one)
    int32_t PortalTextureOf(PortalCaptureState& state, Texture* texture);   // (into its capture; read back later)
    // The street's light now (phase 4): the game's light on the street's statics - its directional lights, ambient and
    // fog - noted each outdoor frame (m_outdoorNow), kept with the ground's lightmap at the teleport into a room
    // (m_streetLight): the street seen from inside is lit for the hour it is, not the hour it was recorded.
    struct OutdoorLight {
        bool known = false;
        uint32_t playfield = 0;
        uint64_t frame = 0;
        std::vector<d3d::Light> directional;
        uint32_t ambient = 0;
        std::array<uint32_t, 8> fog{};           // (kOutdoorFogStates' values)
        std::vector<uint32_t> lightPixels;       // the hour's lightmap over the playfield (cellsX x cellsZ cells)
        uint32_t lightW = 0, lightH = 0, cellsX = 0, cellsZ = 0;
        uint32_t generation = 0;
    };
    OutdoorLight m_outdoorNow, m_streetLight;
    uint32_t m_streetLightGen = 0, m_streetGroundLightGen = 0;
    // Shop portals (portal.cpp): the rooms behind the linked doorways in view, drawn at the scene's end.
    struct LoadedPortal {
        std::shared_ptr<portal::Capture> capture;
        std::vector<Texture*> textures;          // the capture's, made on the GPU
        bool ok = false;
        double lastUsed = 0.0, tried = 0.0;
    };
    std::unordered_set<uint32_t> m_portalMissingLogged;   // (rooms logged as not captured yet)
    void PortalForget(uint32_t playfield);
    std::unordered_map<uint32_t, LoadedPortal> m_portals;   // by the room's playfield (failed loads too: not retried)
    LoadedPortal* PortalLoad(uint32_t playfield);
    void PortalForgetUnused(bool all);
    void DestroyPortals();
    bool PortalCopyDepth(VkCommandBuffer cmd);
    void DrawPortals();                          // (EndScene)
    bool PortalOpenPass(VkCommandBuffer cmd, const float push[32], const VkRect2D& box, bool& copied,
                        const std::vector<float>* triangles = nullptr, VkPipeline pipeline = VK_NULL_HANDLE);
    uint32_t PortalReplay(const portal::Capture& capture, const std::vector<Texture*>& textures, const d3d::Matrix& place,
                          const OutdoorLight* now = nullptr);
    void PortalSetLights(const std::vector<d3d::Light>& lights, const d3d::Matrix& place);
    void PortalMarkPass(VkCommandBuffer cmd, const float push[32], const VkRect2D& box, const std::vector<float>& triangles);
    static void PortalTrianglePush(const float push[32], const float* triangle, float out[32]);
    bool m_drawingPortal = false;                // a room's piece being drawn into a doorway: no side effects
    VkRect2D m_portalScissor{};                  // ... kept to the doorway's box on the scene
    float m_portalClip[4] = {};                  // ... and behind its plane (DrawTransform clip)
    std::unordered_set<uint64_t> m_portalLogged; // (the doorways logged as drawn since arriving)
    VkPipeline m_portalOpenPipeline = VK_NULL_HANDLE, m_portalMarkPipeline = VK_NULL_HANDLE;
    VkPipeline m_portalRestorePipeline = VK_NULL_HANDLE;   // (the open pass's, depth GREATER: mode 2)
    VkImage m_portalDepthCopy = VK_NULL_HANDLE;
    VkImageView m_portalDepthCopyView = VK_NULL_HANDLE;
    VmaAllocation_T* m_portalDepthCopyAllocation = nullptr;
    VkImageLayout m_portalDepthCopyLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t m_portalDepthW = 0, m_portalDepthH = 0;
    bool m_portalsOn = true;                     // RVK_Portals
    float m_portalDist = 60.0f;                  // RVK_PortalDist: drawn within this of the camera (units)
    float m_portalDim = 0.7f;                    // RVK_PortalDim: the room's brightness by day, of its own (0..1)
    // The reverse (portal.cpp DrawStreetPortal, docs/street-portal-plan.md): the street from inside a shop.
    void DrawStreetPortal();                     // (EndScene)
    bool StreetDeclined(const char* why);        // why not, logged as it changes; true: declined
    // The street's ground (portal_street.cpp, phase 2): its capture's cut-out of the native ground, made a native
    // ground of its own - swapped into m_native to be made and drawn through the native ground's path.
    NativeState m_streetNative;
    void NoteOutdoorLight();                     // (the draw path: a lit static outdoors)
    void KeepStreetLight();                      // (PortalLinkTeleport: leaving the street through a doorway)
    void StreetLightPiece(const OutdoorLight& now, std::array<uint32_t, d3d::RS_COUNT>& rs,
                          const std::vector<d3d::Light>& lights, std::vector<d3d::Light>& out) const;
    bool m_streetGround = false;                 // it is drawing (its patches are the ground: IsTerrain)
    bool BuildStreetGround(const portal::Capture& capture, const std::vector<Texture*>& textures);
    void DrawStreetGround(const portal::Capture& capture, const d3d::Matrix& place, const OutdoorLight* now);
    void DestroyStreetGround();
    bool m_streetViewOn = true;                  // RVK_StreetView
    bool m_streetNow = true;                     // RVK_StreetNow: lit for the hour it is now (else as recorded)
    bool m_enteredValid = false;                 // the doorway walked in through (PortalLinkFrame)
    PortalLink m_enteredFrom;
    struct StreetRoom { uint32_t playfield = ~0u; bool known = false; float entrance[3] = {}, inward[3] = {}; } m_streetRoom;
    struct StreetLoaded {
        std::string path;
        LoadedPortal lp;
        bool logged = false;
        float inset = 0.0f;
    } m_street;
    uint64_t m_streetShownFrame = 0;             // the last frame the street was drawn through the entrance
    std::string m_streetWhy;                     // why it isn't drawn, as last logged (StreetDeclined)
    uint32_t m_streetWhyLines = 0;               // ... lines this visit
    struct {
        bool active = false, cameraLogged = false;
        uint32_t playfield = 0, pieces = 0, newSince = 0, lines = 0;
        uint32_t byKind[16] = {};
        double start = 0, lastSummary = 0;
        std::unordered_set<uint64_t> seen;
    } m_pd;
    bool m_zoneFade = true;                      // RVK_ZoneFade
    float m_zoneHoldMax = 1.5f;                  // RVK_ZoneHoldMax: the longest the new zone stays hidden once it draws (s)
    void ApplyRenderScale();
    // The scene is smaller than the output: the TAA upscales it, and the depth is brought up to the output size
    // after it (UpsampleDepth).
    bool SceneScaled() const
    {
        return m_scene && m_ldrMain && (m_scene->m_width != m_ldrMain->m_width || m_scene->m_height != m_ldrMain->m_height);
    }
    // Scene pixels per output (back-buffer) pixel, x and y: 1 1 unscaled.
    float SceneScaleX() const { return m_scene && m_ldrMain ? float(m_scene->m_width) / float(m_ldrMain->m_width) : 1.0f; }
    float SceneScaleY() const { return m_scene && m_ldrMain ? float(m_scene->m_height) / float(m_ldrMain->m_height) : 1.0f; }
    Texture* m_taaOut = nullptr;                 // scaled, with motion blur: the upscaled picture the blur reads
    // RVK_Upscaler 1 (fsr2.cpp) and 2 (fsr3.cpp): FSR 2's / FSR 3's context (made at the first scaled frame, again
    // when a size changes); the scene's motion vectors filled in for them and their output (sharpened into the frame
    // as the TAA's), shared; the reactive mask - m_tonemapped's alpha, the effects' share, as red: for FSR 2 a view
    // of it, for FSR 3 an image (reactive_mask.frag).
    uint32_t m_upscaler = 0;
    struct Fsr2State;
    Fsr2State* m_fsr2 = nullptr;
    struct Fsr3State;
    Fsr3State* m_fsr3 = nullptr;
    Texture* m_fsrMotion = nullptr;
    Texture* m_fsrOut = nullptr;
    VkImage m_fsr2ReactiveOf = VK_NULL_HANDLE;
    VkImageView m_fsr2Reactive = VK_NULL_HANDLE;
    Texture* m_fsr3Reactive = nullptr;
    VkPipeline m_motionFillPipeline = VK_NULL_HANDLE;    // motion_fill.frag
    VkPipeline m_reactiveMaskPipeline = VK_NULL_HANDLE;  // reactive_mask.frag
    bool FsrInputs(VkCommandBuffer cmd, const float params[28]);   // the motion vectors filled in, the output image
    uint64_t m_fsrInputsFrame = ~0ull;           // (FsrInputs: once a frame - FSR and frame generation both ask)
    // RVK_FrameGen (framegen.cpp): non-null while the swapchain is the frame interpolation one.
    bool m_frameGenWanted = false;
    bool m_frameGenFailed = false;               // (it couldn't be made: not tried again until the setting changes)
    struct FrameGenState;
    FrameGenState* m_fg = nullptr;
    bool m_fgUiLayer = false;                    // this frame's interface layer goes to the swapchain, not over the frame
    // What the swapchain lays over the real and the generated frames, so none of it is interpolated: the name labels
    // (drawn into a layer of their own, FgLabelsBegin/End - EndScene), the interface layer and the console, put
    // together each frame (FgBeforePresent).
    Texture* m_fgLabels = nullptr;
    bool m_fgLabelsActive = false;               // the held labels' draws go into m_fgLabels
    uint64_t m_fgLabelsFrame = ~0ull;            // the frame they were drawn in
    Texture* m_fgOverlay = nullptr;
    bool FgLabelsBegin(bool keep = false);
    void FgLabelsEnd();
    // What the game draws on the frame between the 3D scene and its interface - AOReloaded's cast bars, the text shown
    // after zoning - into the labels' layer too (FgUnderBegin from Draw, FgUnderEnd at the interface or the frame's
    // end): drawn into the frame, it was interpolated and smeared.
    bool m_fgUnderActive = false;
    bool FgUnderBegin();
    void FgUnderEnd();
    // What the game draws on the frame after its interface - the mouse pointer - into a layer of its own, the overlay's
    // top (FgTopBegin from Draw; it lasts to the frame's end): drawn into the frame it came out under the interface.
    Texture* m_fgTop = nullptr;
    bool m_fgTopActive = false;
    uint64_t m_fgTopFrame = ~0ull;               // the frame it was drawn in
    uint64_t m_interfaceEndFrame = ~0ull;        // the frame the interface layer was last ended in (InterfaceEnd)
    bool FgTopBegin();
    void ClearImage(VkCommandBuffer cmd, Texture* t);
    Texture* m_fgShot = nullptr;
    Texture* FgComposedFrame(VkCommandBuffer cmd);   // the frame as seen, for reading back (null: m_main is)
    bool FgCreateSwapchain(const VkSwapchainCreateInfoKHR& ci, std::string* error);
    void FgDestroySwapchain();
    void WaitIdle();                             // vkDeviceWaitIdle, frame generation's presents done first
    VkResult FgGetSwapchainImages(uint32_t* count, VkImage* images);
    VkResult FgAcquire(uint64_t timeout, VkSemaphore semaphore, uint32_t* index);
    VkResult FgPresent(const VkPresentInfoKHR* info);
    void FgPrepare(VkCommandBuffer cmd, const float params[28]);
    void FgBeforePresent(VkCommandBuffer cmd);
    bool RenderFsr2(VkCommandBuffer cmd, Texture* dst, const float params[28]);   // false: not done (the TAA then)
    bool RenderFsr3(VkCommandBuffer cmd, Texture* dst, const float params[28]);   // ...
    void DestroyFsr2();
    void DestroyFsr3();
    VkPipeline m_depthUpPipeline = VK_NULL_HANDLE;   // depth_up.frag (UpsampleDepth)
    // Scaled: the scene's depth drawn up to the output size into the other depth image, which becomes the depth from
    // there on (the motion blur, name labels and the interface test against it); BeginScene swaps the scene's back.
    void UpsampleDepth(VkCommandBuffer cmd);
    void SwapDepth();
    bool SceneMipBias() const;                   // the scene's textures sample finer mips (resources.cpp SamplerKey)
    // A per-frame offset (0..1) for the noise patterns of sampled effects while the TAA averages frames; else 0.
    float FrameNoise() const { return TaaActive() ? float(m_frameNumber % 64 * 618034 % 1000000) * 1e-6f : 0.0f; }
    bool TaaParams(float out[28]);               // before MotionBlurParams (it moves on the last frame's camera)
    void RenderTaa(VkCommandBuffer cmd, Texture* dst, const float params[28]);
    float m_bloomOverNearer = 0.15f;             // bloom left on objects in front of its light (1 = all)
    float m_neonSat = 0.0f, m_bloomWide = 0.0f;  // RVK_NeonSat, RVK_BloomWide (SetNeonSat, SetBloomWide)
    float m_bloomWeightSum = 1.0f;               // the bloom levels' weights summed this frame (RenderBloom)
    bool m_bloomDepth = false;                   // this frame's bloom carries its light's depth
    std::vector<Texture*> m_bloomLevels;         // half resolution and down, float
    float m_effectGlow = 1.0f;
    Texture* m_glow = nullptr;                   // second scene attachment: what additive effects add (F_GLOW);
                                                 // alpha: its brightness / view depth (the bloom's depth)
    Texture* m_localFraction = nullptr;          // third: how much of each pixel's colour local lights gave it;
                                                 // G: reflectivity; B: the direct sunlight's share (contact shadows)
    bool m_glowCleared = false;                  // this frame
    uint32_t m_glowDraws = 0;                    // this frame's draws feeding the glow (frame dumps)
    bool SurfaceSheen(uint32_t fvf) const;   // an added layer on a surface (depth EQUAL): not an effect
    bool GlowDraw(uint32_t fvf) const;
    bool GlowOccluder(uint32_t fvf) const;
    bool IsInterfaceDraw(uint32_t fvf) const;
    // The scene's pre-transformed draws (FVF 0x1C4, out of ProcessVertices: the game's water, floating text) - not
    // the game's screen sprites in the same format (the timer bars): the proxy says which (SetDrawProcessed).
    bool IsWater(uint32_t fvf) const;
    bool m_drawProcessed = false;
    bool WaterWritesDepth(uint32_t fvf) const;   // ... and depth-tested (floating text isn't): forced depth writes
    VkDescriptorSetLayout m_bloomSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_bloomLayout = VK_NULL_HANDLE;
    VkPipeline m_bloomDown = VK_NULL_HANDLE, m_bloomUp = VK_NULL_HANDLE;
    void RenderBloom(VkCommandBuffer cmd);
    // One full-target pass into dst reading src (binding 0) and src2 (binding 1), which must be readable already.
    void FullscreenPass(VkCommandBuffer cmd, Texture* dst, VkPipeline pipeline, VkImageView src, VkImageView src2,
                        VkSampler sampler, const float* params, uint32_t paramBytes, bool load,
                        VkImageView src3 = VK_NULL_HANDLE, VkImageView src4 = VK_NULL_HANDLE,
                        const VkRect2D* scissor = nullptr);   // (only this part of dst drawn)
    void RenderObjectMotionBlur(VkCommandBuffer cmd, Texture* src, const float params[28]);
    float m_aoStrength = 1.0f, m_aoRadius = 1.5f;
    Texture* m_aoTex[2] = {};                    // half resolution: raw, blurred (ping-pong)
    // RVK_AoMode 1 (cacao.cpp): AMD's AO (FidelityFX CACAO) in place of ours, into m_cacaoAo - full resolution,
    // occlusion and view depth as m_aoTex[1] holds ours; ours where it can't run. docs/amd-fx-plan.md.
    float m_aoMode = 0.0f;
    struct CacaoState;
    CacaoState* m_cacao = nullptr;
    Texture* m_cacaoOut = nullptr;               // its output (storage)
    Texture* m_cacaoAo = nullptr;
    bool m_aoFull = false;                       // this frame's AO is full resolution, in m_aoFullTex
    Texture* m_aoFullTex = nullptr;              // m_cacaoAo, or the ray-traced (RVK_AoMode 2, raytrace.cpp)
    bool m_gatherExtended = false;               // (its shaders gather with offsets: shaderImageGatherExtended)
    VkPipeline m_cacaoPackPipeline = VK_NULL_HANDLE;     // cacao_pack.frag
    bool RenderCacao(VkCommandBuffer cmd);
    void DestroyCacao();
    // RVK_Vrs (vrs.cpp): variable rate shading - AMD's shading rate image (FidelityFX VRS) as the scene rendering's
    // fragment shading rate attachment, taken by the scene shader's pipelines. m_sceneRateFlag: every pipeline is made
    // for the attachment when the GPU has it (VK_KHR_fragment_shading_rate; 0 otherwise); m_vrsTexel its tile size.
    float m_vrsMode = 0.0f, m_vrsCutoff = 0.05f;
    VkPipelineCreateFlags m_sceneRateFlag = 0;
    uint32_t m_vrsTexel = 16;
    struct VrsState;
    VrsState* m_vrs = nullptr;
    bool RenderVrsImage(VkCommandBuffer cmd);
    VkImageView VrsAttachment() const;
    void DestroyVrs();
    VkPipeline m_aoPipeline = VK_NULL_HANDLE, m_aoBlurPipeline = VK_NULL_HANDLE;
    float m_giStrength = 1.0f, m_giRadius = 4.0f;
    Texture* m_albedo = nullptr;                 // fifth scene attachment: surface colour without lighting (fogged)
    Texture* m_giTex[2] = {};                    // half resolution: indirect light + view depth (ping-pong)
    VkPipeline m_giPipeline = VK_NULL_HANDLE, m_giBlurPipeline = VK_NULL_HANDLE;
    bool RenderGi(VkCommandBuffer cmd);
    // The environment probe (PBR materials' reflections of their surroundings): what the camera has seen around it
    // by direction (octahedral, 128 x 128, ping-pong; a: how sure), and its prefiltered atlas (env_common.glsl) that
    // the scene shaders sample through FL.pbr2.w. Updated after each scene, used by the next.
    Texture* m_envProbe[2] = {};
    Texture* m_envAtlas = nullptr;
    uint32_t m_envCurrent = 0;                   // the m_envProbe holding the latest
    bool m_envReady = false;                     // the atlas has been written (and is readable)
    float m_envEye[3] = {};                      // the camera at the last update
    bool m_envEyeValid = false;
    VkPipeline m_envAccumPipeline = VK_NULL_HANDLE, m_envFilterPipeline = VK_NULL_HANDLE;
    bool RenderEnvProbe(VkCommandBuffer cmd);
    // Volumetric light: sun shafts through the shadow cascades and lamp glow (hdr.cpp, volume.frag).
    float m_volume = 1.0f, m_volumeHaze = 1.0f, m_volumeShafts = 1.0f, m_volumeRays = 1.0f, m_landHaze = 0.4f;
    Texture* m_volumeTex[2] = {};                // half resolution: scattered light + view depth (ping-pong)
    VkDescriptorSetLayout m_volumeSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_volumeLayout = VK_NULL_HANDLE;
    VkPipeline m_volumePipeline = VK_NULL_HANDLE, m_volumeBlurPipeline = VK_NULL_HANDLE;
    bool RenderVolume(VkCommandBuffer cmd);
    Texture* m_sandFogTex[2] = {};               // half resolution: the sand fog's light + transmittance (ping-pong)
    VkPipeline m_sandFogPipeline = VK_NULL_HANDLE, m_sandFogBlurPipeline = VK_NULL_HANDLE;
    bool m_sandFogValid = false;                 // this frame's fog drawn (the tone mapping lays it on)
    Texture* m_sandFogShelter = nullptr;         // RG16F, the sand map's cells: the solid shelter over the top, the top
    uint32_t m_sandFogShelterGen = ~0u;          // (the m_sandShelterGen it was made for)
    float m_sandFogShelterRef = 0.0f;            // (the height its tops are taken from)
    bool RenderSandFog(VkCommandBuffer cmd);
    // Screen-space reflections (hdr.cpp, ssr.frag): strength (0 = off) and reflectivity of water, glossy surfaces
    // and the wet look of surfaces facing up.
    float m_ssr = 1.0f, m_ssrWater = 1.0f, m_ssrGloss = 0.3f, m_ssrWet = 0.0f;
    Texture* m_ssrTex = nullptr;                 // full resolution: reflected colour, how much it shows
    VkPipeline m_ssrPipeline = VK_NULL_HANDLE;
    bool RenderSsr(VkCommandBuffer cmd, bool traced = false);   // (traced: misses marked for RenderRtReflect)
    // RVK_SsrMode 1 (sssr.cpp): AMD's reflections (FidelityFX SSSR) in place of ours, into m_ssrTex as ours; ours
    // where they can't run. docs/amd-fx-plan.md.
    float m_ssrMode = 0.0f, m_sssrThick = 0.5f;
    struct SssrState;
    SssrState* m_sssr = nullptr;
    Texture* m_sssrOut = nullptr;                // its output (storage)
    VkPipeline m_sssrResolvePipeline = VK_NULL_HANDLE;   // sssr_resolve.frag
    bool RenderSssr(VkCommandBuffer cmd);
    void DestroySssr();
    // RVK_GiMode 1 (brixelizer.cpp): AMD's indirect light (FidelityFX Brixelizer GI, traced through a distance field
    // of the static world) in place of ours, into m_brixGi - full resolution, light and view depth as m_giTex[1]
    // holds ours; ours where it can't run. RVK_GiDebug (ini): 1 the distance field, 2 its light alone.
    float m_giMode = 0.0f, m_giDebug = 0.0f, m_giSky = 0.0f;
    struct BrixState;
    BrixState* m_brix = nullptr;
    Texture* m_brixGi = nullptr;
    Texture* m_brixGiLast = nullptr;             // last frame's m_brixGi (brix_pack.frag steadies characters with it)
    uint64_t m_brixGiFrame = 0;                  // the frame m_brixGi was last made
    bool m_giFull = false;                       // this frame's indirect light is m_brixGi
    VkPipeline m_brixPackPipeline = VK_NULL_HANDLE;      // brix_pack.frag
    bool BrixWanted() const { return m_giStrength > 0.0f && m_giMode >= 0.5f; }
    void BrixNoteDraw(uint32_t primitive, uint32_t fvf, const void* vertices, uint32_t stride, uint32_t vertexCount,
                      VkBuffer ib, uint32_t indexCount);
    void BrixForgetSlot(VkBuffer buffer, VkDeviceSize offset);
    bool RenderBrixGi(VkCommandBuffer cmd);
    void DestroyBrix();
    // Ray queries (raytrace.cpp, docs/rt-plan.md): acceleration structures of the static world - a BLAS per mesh the
    // static arena keeps, a TLAS of their places round the camera each frame - for rays from any shader. Only where
    // the GPU has them (m_rayQuery); RVK_RtDebug 1 shows what the rays hit.
    bool m_rayQuery = false;
    VkDeviceSize m_asScratchAlign = 128;         // (the GPU's minAccelerationStructureScratchOffsetAlignment)
    float m_rtDebug = 0.0f;
    struct RtState;
    RtState* m_rt = nullptr;
    bool RtAoWanted() const { return m_rayQuery && m_aoStrength > 0.0f && m_aoMode >= 1.5f; }   // RVK_AoMode 2
    // RVK_RtShadow 1: the sun's shadows ray traced where the pre-pass's depth is (raytrace.cpp RenderRtShadow).
    float m_rtShadow = 0.0f;
    bool RtShadowWanted() const { return m_rayQuery && m_rtShadow >= 0.5f && m_shadows && m_prepassOn; }
    // RVK_RtReflect 1: where our screen-space reflections find nothing, a ray (raytrace.cpp RenderRtReflect).
    float m_rtReflect = 0.0f;
    float m_volLamps = 0.0f;                     // RVK_VolLamps (volume.frag: FL.volume2.x)
    bool RtReflectWanted() const { return m_rayQuery && m_rtReflect >= 0.5f && m_ssr > 0.0f; }
    bool RtReflectReady() const;                 // this frame's structures and its pass are there (else no misses marked)
    void RenderRtReflect(VkCommandBuffer cmd);
    bool RtWanted() const
    {
        return m_rayQuery && (m_rtDebug >= 0.5f || RtAoWanted() || RtShadowWanted() || RtReflectWanted());
    }
    // (Its instance's key when the draw's place is kept - else 0.)
    // (cutout: its texture, alpha-tested at the rays' hits - alpha over alphaRef, at uvOffset in its vertices.)
    uint64_t RtNoteDraw(uint32_t primitive, const void* vertices, uint32_t stride, uint32_t vertexCount, VkBuffer ib,
                        uint32_t indexCount, bool ground, Texture* cutout = nullptr, float alphaRef = 0.0f,
                        uint32_t uvOffset = 0);
    void RtForgetTexture(Texture* texture);      // (its cut-outs' instances go: DestroyTexture)
    uint64_t m_drawRtPlace = 0;                  // this draw's (RtNoteDraw), for its shadow caster
    // A character's piece skinned on the GPU (draw.cpp): its BLAS refitted this frame, an instance where it is drawn.
    uint64_t RtNoteSkinned(const void* job, VkBuffer arena, VkDeviceSize vertexOffset, uint32_t vertexCount,
                           VkBuffer meshBuffer, VkDeviceSize indexOffset, uint32_t indexCount);
    bool RtTraced(uint64_t place) const;         // that instance is in the rays (its BLAS built)
    uint64_t RtStatelPlace(uint32_t item) const; // a placed object's piece's instance key (statels.cpp), 0 = none
    void RenderRtShadow(uint32_t cause);         // at the pre-pass's end: the rays' sun mask (Frame::rays)
    uint32_t RtShadowMaskBits();                 // FL.rtShadow.x: the mask's bindless image + 1 | sampler << 20
    void RtForgetSlot(VkBuffer buffer, VkDeviceSize offset);
    void RtSyncStatels();                        // the placed objects' pieces as meshes and instances
    void RtCompact(VkCommandBuffer cmd);         // built BLAS copied into their compacted size
    bool RtUpdate(VkCommandBuffer cmd);          // this frame's BLAS builds and TLAS; false: none to trace
    void RenderRtDebug(VkCommandBuffer cmd);
    bool RenderRtAo(VkCommandBuffer cmd);
    void RtCollect();                            // (structures no submission in flight can still read)
    void DestroyRt();
    // The scene's surfaces for AMD's effects: every surface's normal written into the motion vectors' zw while one
    // reads them (FL.screen.z), unpacked by gbuffer.frag into m_sceneSurface - world normal, roughness. Each of these
    // is made once a frame, by the first effect that asks (sssr.cpp).
    bool SceneNormalsWanted() const { return (m_ssr > 0.0f && m_ssrMode >= 0.5f) || BrixWanted() || RtAoWanted(); }
    Texture* m_sceneSurface = nullptr;
    VkPipeline m_gbufferPipeline = VK_NULL_HANDLE;       // gbuffer.frag
    uint64_t m_sceneSurfaceFrame = ~0ull;
    bool RenderSceneSurface(VkCommandBuffer cmd);
    Texture* m_sceneMotion = nullptr;            // the motion vectors whole (motion_fill.frag), RG16F
    uint64_t m_sceneMotionFrame = ~0ull;
    bool RenderSceneMotion(VkCommandBuffer cmd);
    Texture* m_sceneEnv = nullptr;               // the environment: the probe's atlas, the sky guessed where unseen
    VkPipeline m_sssrEnvPipeline = VK_NULL_HANDLE;       // sssr_env.frag
    uint64_t m_sceneEnvFrame = ~0ull;
    bool RenderSceneEnv(VkCommandBuffer cmd);
    void DestroySceneSurfaces();
    d3d::Matrix m_aoProj{};                      // the world camera's projection (first depth-writing 3D draw)
    d3d::Matrix m_aoView{};                      // ... and view
    float m_motionBlur = 0.0f, m_motionNear = 8.0f;
    uint32_t m_motionMode = 0;
    Texture* m_motionVectors = nullptr;          // fourth scene attachment: screen motion since last frame (pixels)
    Texture* m_motionTiles[2] = {};              // per 32-pixel tile: strongest motion, then of the 3x3 neighbourhood
    VkPipeline m_tileMaxPipeline = VK_NULL_HANDLE, m_neighbourMaxPipeline = VK_NULL_HANDLE, m_objectBlurPipeline = VK_NULL_HANDLE;
    static constexpr uint32_t kMotionTile = 32;
    // Matching draws across frames (motion vectors): last frame's and this frame's world matrices by mesh key.
    // positions: the mesh's vertex positions (model space, 3 floats each, a slice of the frame's pool) when it has at
    // most kMotionMaxVertices - a CPU-skinned character's limbs move only in them. Flat arrays reused from frame to
    // frame (a map of vectors allocated per draw before); last frame's are looked up through `index`, sorted by key
    // then x (BeginScene), so a draw finds the entries within reach with one binary search, however many instances
    // of its mesh there are.
    struct MotionEntry { d3d::Matrix world; uint32_t positions, positionCount; bool used; };
    struct MotionFrame {
        struct Index { uint64_t key; float x; uint32_t entry; };
        std::vector<MotionEntry> entries;
        std::vector<uint64_t> keys;              // per entry
        std::vector<float> positions;
        std::vector<Index> index;
        void Clear() { entries.clear(); keys.clear(); positions.clear(); index.clear(); }
    };
    static constexpr uint32_t kMotionMaxVertices = 8192;
    MotionFrame m_motionPrev, m_motionCur;
    bool MotionVectorDraw(uint32_t fvf) const;   // this draw writes motion vectors
    // The camera (view, projection) changes: a generation, so per-draw checks against a camera compare a number.
    uint64_t m_cameraGen = 1;
    mutable uint64_t m_aoCameraGen = 0;          // AoCamera's answer as of this generation (0: none)
    mutable bool m_aoCameraSame = false;
    bool AoCamera() const;                       // the current camera is the scene's first (m_aoView / m_aoProj)
    mutable uint64_t m_worldCameraGen = 0;       // WorldCamera's answer as of this generation (0: none)
    mutable bool m_worldCameraSame = false;
    uint64_t MotionKey(uint32_t primitive, uint32_t fvf, uint32_t vertexCount, const uint16_t* indices, uint32_t indexCount) const;
    Texture* m_tonemapped = nullptr;             // with motion blur: the tone mapped scene, blurred into m_ldrMain
    Texture* m_fxaaOut = nullptr;                // FXAA with motion blur: the anti-aliased image the blur reads
    VkPipeline m_motionPipeline = VK_NULL_HANDLE;
    d3d::Matrix m_prevView{}, m_prevProj{};      // the last frame's world camera
    float m_prevEye[3] = {};
    bool m_prevViewProjValid = false;
    double m_prevSceneTime = 0.0;
    bool MotionBlurParams(float out[28]);        // reprojection + parameters for this frame; false: no blur
    bool m_dof = false, m_dofBokeh = true, m_dofNear = true, m_dofFar = false;
    float m_dofCloseFocus = 3.0f;
    float m_dofStrength = 0.5f, m_dofRadius = 16.0f, m_dofFocusDistance = 0.0f, m_dofRange = 0.2f;
    Texture* m_dofIn = nullptr;                  // the scene with its ambient occlusion
    Texture* m_dofOut = nullptr;                 // ... with depth of field: what the tone mapping reads
    Texture* m_dofHalf = nullptr;                // half resolution colour + circle of confusion
    Texture* m_dofBlur = nullptr;                // half resolution blur + near-field reach
    Texture* m_dofTiles[2] = {};                 // largest CoC per tile, per neighbourhood
    Texture* m_dofFocus[2] = {};                 // focus distance (1x1), this and last frame's
    uint32_t m_dofFocusIndex = 0;
    double m_dofPrevTime = 0.0;
    VkPipeline m_dofCompositePipeline = VK_NULL_HANDLE, m_dofFocusPipeline = VK_NULL_HANDLE,
               m_dofPrefilterPipeline = VK_NULL_HANDLE, m_dofTilesPipeline = VK_NULL_HANDLE,
               m_dofGatherPipeline = VK_NULL_HANDLE, m_dofFinalPipeline = VK_NULL_HANDLE;
    bool RenderDof(VkCommandBuffer cmd, bool bloom, bool ao, bool gi, bool volume, const float tonemapParams[16]);
    static constexpr uint32_t kTonemapInputs = 15;   // occlusion.glsl's bindings
    void TonemapInputsPass(VkCommandBuffer cmd, Texture* dst, VkPipeline pipeline, Texture* scene, bool bloom, bool ao,
                           bool gi, bool volume, const float params[16]);
    void MakeDepthReadable(VkCommandBuffer cmd);
    bool m_aoProjValid = false;
    bool RenderAo(VkCommandBuffer cmd);          // false: no AO this frame
    bool CreateHdrResources(std::string* error);
    void DestroyHdrResources();
    void BeginScene();
    void EndScene();
    // The interface layer (interface.cpp).
    Texture* m_uiLayer = nullptr;
    Texture* m_uiLayerReturn = nullptr;          // the target the interface began on (the main one)
    bool m_uiLayerActive = false;                // the interface's draws go into the layer
    bool m_alienMap = true;                      // RVK_AlienMap
    bool m_uiLayerOpen = false;                  // between InterfaceBegin and InterfaceEnd
    std::atomic<bool> m_uiLayerReady{false};
    VkPipeline m_uiCompositePipeline = VK_NULL_HANDLE;
    VkPipeline m_uiOverlayPipeline = VK_NULL_HANDLE;   // the same, its coverage kept (frame generation's overlay)
    void InterfaceSceneEnd();                    // what the interface's first draw does: the scene ends
    // GPU profiling (profile.cpp): a mark when the scene's draw class changes, so the 4.8 ms "scene" block splits
    // into opaque / terrain base / terrain light / foliage.
    int m_sceneClass = 0;
    uint32_t m_sceneClassMarks = 0;
    void ProfileSceneClass(int cls);
    // The scene shading count's class (finer than the timestamps': statics split by whether they are pre-passed).
    int m_shadeClass = 0;
    int m_drawSceneClass = 0;                    // the current draw's ProfileSceneClass class (0: not a scene draw)
    void ShadeClass(int cls);

    // Particles (particles.cpp): one block of kParticlesPerBlock particles per effect; particle p belongs to sprite slot
    // p / kParticleChildren. State and the generated quads (FVF 0x142, 4 vertices a particle) live in GPU buffers; the
    // quads are double buffered per frame slot.
    ParticleParams m_particleParams;
    struct ParticleBlock {
        uint64_t key = 0;                        // 0 = free
        uint64_t lastSeen = 0;                   // frame number of its last ParticleEmitter
        bool reset = true;                       // newly assigned: its particles start dead
        bool simulated = false;                  // quads were generated for it this frame
        uint32_t quadRegion = 0;                 // ... in this region of the quad buffer
        float center[3] = {};
        std::vector<ParticleSprite> sprites, prevSprites;
        bool havePrev = false;
        double lastAliveTime = 0.0;              // Clock() when a sprite was last alive: particles may live a while yet
        // How the effect's sprites move relative to its origin, smoothed (MeasureParticleMotion). orbit: tangential
        // speed around the vertical axis (sign = the swirl's direction), burst: outward speed (< 0 inward), rise:
        // upward speed, spread: RMS distance from the centre, speed: RMS speed, turnover: sprite births per second per
        // live sprite (1 / their life).
        struct Motion { float orbit, burst, rise, spread, speed, turnover; bool valid; } motion{};
        float origin[3] = {}, prevOrigin[3] = {};
        double uploadTime = 0.0;
        float noise[3] = {};                     // its offset into the noise field (from the key)
        uint64_t drawnFrame = 0;                 // frame its particles were last drawn
        // The state its particles were last drawn with (the effect's), so they can still be drawn - and fade out on
        // their own - once the game stops drawing the effect (DrawOrphanParticles).
        struct DrawState {
            std::array<uint32_t, d3d::RS_COUNT> rs;
            std::array<std::array<uint32_t, d3d::TSS_COUNT>, 2> tss;
            std::array<Texture*, 2> textures;
            d3d::Matrix view, proj, texMatrix[2];
            d3d::Viewport viewport;
            Texture* target;
        } state{};
        bool haveState = false;
        // Diagnostics (randy-vk.log, frame dumps): the effect's end and its particles fading out.
        bool wasAnnounced = false, fading = false, skipLogged = false;
        double endTime = 0.0;
        uint32_t fadingDraws = 0;
    };
    std::vector<ParticleBlock> m_particleBlocks;
    ParticleBlock* m_particlePending = nullptr;  // the effect whose sprite draw comes next
    VkBuffer m_particleState = VK_NULL_HANDLE;
    VmaAllocation_T* m_particleStateAllocation = nullptr;
    VkBuffer m_particleQuads[kFramesInFlight] = {};
    VmaAllocation_T* m_particleQuadsAllocation[kFramesInFlight] = {};
    VkBuffer m_particleIndices = VK_NULL_HANDLE;
    VmaAllocation_T* m_particleIndicesAllocation = nullptr;
    VkDescriptorSetLayout m_particleSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_particleLayout = VK_NULL_HANDLE;
    VkPipeline m_particlePipeline = VK_NULL_HANDLE;
    bool m_particleStateCleared = false;
    uint32_t m_particleSimCount = 0;             // particles per sprite this frame's simulation used
    bool m_particleFullLogged = false;
    double m_particleTime = 0.0, m_particleLastClock = 0.0;
    d3d::Matrix m_particleView{};                // the camera the quads face (last effect's, last frame)
    uint32_t m_particleDraws = 0;                // this frame (frame dumps)
    bool CreateParticleResources(std::string* error);
    void DestroyParticleResources();
    // Our own effects (fx.cpp).
    struct FxEmitterRun {
        int32_t block = -1;                      // its block of particles (-1: none yet / none free)
        uint32_t cursor = 0;                     // where the next particles spawn in it
        float carry = 0.0f;                      // the rate's fraction of a particle carried to the next frame
        bool reset = true;                       // newly given its block: its particles start dead
    };
    struct FxPlaying {
        uint64_t key = 0;
        uint32_t effect = 0;
        std::shared_ptr<const fx::Effect> def;   // (kept while its particles live, even once redefined)
        FxAnchors now, prev;
        bool havePrev = false, gone = false;
        uint32_t lastPhase = 0;
        uint64_t lastSeen = 0;                   // the frame it was last announced in
        double goneAt = 0.0;
        float noise = 0.0f;                      // its offset into the flow
        std::vector<FxEmitterRun> runs;          // one an emitter
    };
    struct FxDrawn { uint32_t playing, emitter, region; };
    std::unordered_map<uint32_t, std::shared_ptr<const fx::Effect>> m_fxEffects;
    std::vector<FxPlaying> m_fxPlaying;
    std::vector<uint32_t> m_fxFreeBlocks;
    std::vector<FxDrawn> m_fxDrawn;              // this frame's simulated blocks, in quad-region order
    VkDescriptorSetLayout m_fxSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_fxLayout = VK_NULL_HANDLE;
    VkPipeline m_fxPipeline = VK_NULL_HANDLE;
    VkBuffer m_fxState = VK_NULL_HANDLE, m_fxIndices = VK_NULL_HANDLE;
    VmaAllocation_T* m_fxStateAllocation = nullptr;
    VmaAllocation_T* m_fxIndicesAllocation = nullptr;
    VkBuffer m_fxQuads[kFramesInFlight] = {};
    VmaAllocation_T* m_fxQuadsAllocation[kFramesInFlight] = {};
    Texture* m_fxAtlas = nullptr;                // the sprites (fx::Sprite), made at the first effect
    bool m_fxStateCleared = false;
    double m_fxLastClock = 0.0;
    float m_fxTime = 0.0f;
    uint32_t m_fxDraws = 0;
    bool CreateFxResources(std::string* error);
    void DestroyFxResources();
    void SimulateFx(VkCommandBuffer cmd);        // BeginFrame, before rendering starts (last frame's announcements)
    void DrawFx();                               // EndScene, before the portals
    void MakeFxAtlas();
    void SimulateParticles(VkCommandBuffer cmd); // BeginFrame, before rendering starts
    void DrawParticles(ParticleBlock& block, bool orphan = false);   // right after the effect's own draw, with its state
    // Particles of effects the game didn't draw this frame (ended, or out of view): drawn with their effect's last state
    // at the end of the 3D scene, until they have died out. Once a frame.
    void DrawOrphanParticles();
    bool ParticlesMayLive(const ParticleBlock& block, double now) const;
    void MeasureParticleMotion(ParticleBlock& block, float dt);
    // The block's flow parameters: the global ones shaped by its motion. a: swirl, inherit, pull, lift; b: feature
    // size, curl speed, life factor.
    void ParticleBlockParams(const ParticleBlock& block, float a[4], float b[4]) const;
    bool m_particleOrphansDone = false, m_particleSaw3D = false;
    const char* m_particleOrphanTrigger = "none";   // frame dumps: what drew this frame's fading particles
    uint32_t m_particleLogBudget = 400;          // diagnostic lines left for randy-vk.log
    void ParticleLog(const char* fmt, ...);
    // DestroyTexture: true = kept alive for particles still fading out with it (destroyed once they have).
    bool HoldParticleTexture(Texture* texture);
    void ReleaseParticleTextures(bool all);
    std::vector<Texture*> m_particleHeldTextures;
    // A draw whose geometry is already in a GPU buffer (the particle quads): Draw uses it instead of copying vertices.
    struct ExternalGeometry {
        VkBuffer vertices, indices;
        int32_t baseVertex;
    };
    const ExternalGeometry* m_external = nullptr;
    float m_drawColorScale = 1.0f;               // the current draw's colour multiplier (particles)

    // Point light shadows (pointshadow.cpp): a cube map per shadowed light, layers 6*i .. 6*i+5 of one cube array.
    static constexpr float kPointShadowNear = 0.25f;   // geometry closer to the light (its fixture) is clipped
    uint32_t m_pointShadows = 0;                 // lights to shadow (0 = off)
    float m_pointShadowStrength = 0.9f, m_pointShadowDay = 0.25f;
    float m_daylight = 0.0f;                     // smoothed sun brightness on flat ground (previous frames)
    float PointShadowStrength() const;
    VkImage m_cubeImage = VK_NULL_HANDLE;
    VmaAllocation_T* m_cubeAllocation = nullptr;
    VkImageView m_cubeArrayView = VK_NULL_HANDLE;
    VkSampler m_cubeSampler = VK_NULL_HANDLE;
    VkImageView m_cubeFaceViews[kMaxPointShadows * 6] = {};
    uint32_t m_cubeCount = 8, m_cubeCountWanted = 8;   // cubes allocated (layers / 6), for SetPointShadows' count
    VkImageLayout m_cubeLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    struct PointShadowLight {
        float position[3], range;
        float fade;                              // 0..1: a newly shadowed light's shadow fades in (m_pointShadowFadeIn)
        uint64_t casters = 0;                    // what its cube was drawn from (RenderPointShadowMaps: kept while the same)
        uint32_t kept = 0;                       // frames since it was drawn
    };
    double m_pointShadowFadeIn = 0.3;            // seconds (0 = shadows appear at once)
    double m_pointShadowClock = 0.0;
    PointShadowLight m_pointShadowLights[kMaxPointShadows] = {};   // the lights the cubes hold (for this frame)
    uint32_t m_pointShadowCount = 0;
    uint32_t m_pointShadowDraws = 0;             // last pass: caster draws into the cubes (frame dumps)
    uint32_t m_pointCubesKept = 0, m_pointCubesAll = 0, m_pointCubeFrames = 0;   // cubes kept as drawn (the log)
    bool CreatePointShadowResources(std::string* error);
    void DestroyPointShadowResources();
    void PreparePointShadowMaps(VkCommandBuffer cmd);
    void RenderPointShadowMaps(VkCommandBuffer cmd);
    uint32_t PointShadowLayer(const d3d::Light& light) const;   // cube index + 1 the light's shadow is in, or 0
    struct ChurnLeft { float position[3]; float range; uint64_t frame; };
    std::vector<ChurnLeft> m_churnLeft;
    uint32_t m_churnIn = 0, m_churnOut = 0, m_churnBack = 0;
    double m_churnTime = 0.0;
    void PointShadowChurn(const PointShadowLight* previous, uint32_t previousCount, size_t candidates,
                          const std::vector<const d3d::Light*>& chosen);
    uint64_t m_ringGeneration = 0, m_constantsGeneration = ~0ull;
    // GPU-driven M2: the frame's draw arenas, slices of this frame's ring. `m_constsBase` holds the deduplicated
    // DrawConstants (appended on a render-state change) and `m_recordsBase` the per-draw DrawRecords. They are
    // (re)reserved when the ring has been restarted; a wrap or an overflow flush discards them (the GPU has
    // consumed the data). `m_constIndex` is the current shared block, reused while nothing changes.
    VkDeviceSize m_constsBase = 0, m_recordsBase = 0;
    uint64_t m_arenaGeneration = ~0ull;
    uint32_t m_constCount = 0, m_recordCount = 0, m_constIndex = 0;
    uint32_t m_constCapacity = 0, m_recordCapacity = 0;
    bool m_arenaBound = false;                   // bindings 0 and 12 pushed in this frame's command buffer
    void PrepareDrawArenas();                    // reserve the arrays in the ring if the generation changed
    void FlushDrawArenas();                      // start the arrays over after a mid-frame submit (the GPU has read them)
    uint32_t AppendConstant(const detail::DrawConstants& c, size_t bytes);   // the first `bytes` (the lights in use)
    uint32_t AppendRecord(uint32_t constIndex, const detail::DrawTransform& d);
    // M3: consecutive draws that share pipeline, dynamic state, descriptor bindings and vertex/index buffers are
    // recorded as indirect commands (one per draw, carrying its geometry and record index) and issued together.
    VkDeviceSize m_indirectBase = 0;             // this frame's indirect command array in the ring
    uint32_t m_indirectCount = 0;                // commands written
    bool m_groupIndirect = false;                // drawIndirectFirstInstance enabled
    struct DrawGroup { bool active = false; uint64_t key = 0; uint32_t first = 0, count = 0; } m_group;
    uint32_t m_groupCalls = 0, m_groupDraws = 0, m_singleDraws = 0;   // M3 stats (per frame)
    // Frame dump: why constant blocks were made again and why batches ended (Draw; the parts that differed).
    static constexpr int kRewhy = 15, kSplitParts = 16;
    uint32_t m_rewriteWhy[kRewhy] = {}, m_splitWhy[kSplitParts + 2] = {};
    uint64_t m_lastGroupParts[kSplitParts] = {};
    uint32_t m_constantsReused = 0;              // rewrites that matched the last block (per frame)
    std::vector<uint8_t> m_lastConstants;        // the last constant block written (its used bytes)
    void FlushGroup();
    void NoteGroup(uint32_t count);
    void RecordIndirect(uint32_t indexCount, VkDeviceSize ibOffset, VkDeviceSize vbOffset, uint32_t stride,
                        uint32_t recordIndex, uint64_t key);
    uint32_t m_constantsFvf = ~0u;
    uint32_t m_constantsTexMask = ~0u;
    bool m_constantsTerrain = false, m_constantsLabel = false;
    uint32_t m_constantsFoliageLod = 0;          // FoliageFar: 0 near, 1 far foliage, 2 far plant
    bool m_constantsCharacter = false;
    bool m_constantsOpaque = false;              // the draw's textures are fully opaque (F_CUTOUT omitted; Draw)
    bool m_constantsHoles = false;               // the stage 0 texture has transparent texels (foliage; Draw)
    uint32_t m_constantsCarrier = 0;
    void BeginRenderingOn(Texture* target);
    void EndRendering();
    bool EnsureDepth(uint32_t width, uint32_t height);   // grows the shared depth buffer if needed
    void SubmitAndWait();                                // mid-frame flush; recording continues
    void SubmitUploadsBetweenFrames(Frame& f);           // between frames: the recorded uploads run, waited for
    void Transition(VkCommandBuffer cmd, Texture* t, VkImageLayout to);
    VkCommandBuffer UploadCommands();
    void SaveScreenshot();
    void DestroyTextureNow(Texture* texture);

    // Vulkan objects
    VkInstance m_instance = VK_NULL_HANDLE;
    VkPhysicalDevice m_physical = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_queue = VK_NULL_HANDLE;
    uint32_t m_queueFamilyCount = 1;             // queues the chosen family has
    VkQueue m_fgPresentQueue = VK_NULL_HANDLE;   // RVK_FrameGen: the frame interpolation swapchain's (framegen.cpp)
    VkQueue m_fgAcquireQueue = VK_NULL_HANDLE;
    bool m_timelineSemaphores = false;           // (the frame interpolation swapchain needs them)
    uint32_t m_queueFamily = 0;
    VmaAllocator_T* m_allocator = nullptr;
    VkPhysicalDeviceProperties m_props{};
    DeviceInfo m_info;
    std::array<bool, size_t(Format::Count)> m_formatSupported{};

    HWND m_window = nullptr;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    std::vector<VkImage> m_swapImages;
    std::vector<VkSemaphore> m_renderDone;     // one per swapchain image
    VkExtent2D m_swapExtent{};
    bool m_swapchainStale = false;
    bool m_swapchainSupported = false;

    uint32_t m_width = 0, m_height = 0;
    Texture* m_main = nullptr;                 // main colour target (a render target texture)
    Texture* m_target = nullptr;               // current colour target
    VkImage m_depth = VK_NULL_HANDLE;          // shared by every target; sized to the largest seen
    VkImageView m_depthView = VK_NULL_HANDLE;
    VmaAllocation_T* m_depthAllocation = nullptr;
    VkImageLayout m_depthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t m_depthWidth = 0, m_depthHeight = 0;
    // RVK_RenderScale: the other depth image (SwapDepth): the output-size one during the scene, the scene's after
    // UpsampleDepth. Unused (empty) unscaled.
    struct DepthImage {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VmaAllocation_T* allocation = nullptr;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        uint32_t width = 0, height = 0;
    } m_otherDepth;
    bool m_depthSwapped = false;                 // m_depth is the output-size one (after UpsampleDepth)
    struct DeadImage { VkImage image; VkImageView view; VmaAllocation_T* allocation; };
    // Deferred destruction: each object is tagged with the last submission that may still use it and freed
    // once that submission has completed (m_completed). Frames run concurrently with recording, so freeing at
    // "the next BeginFrame of this slot" is not enough.
    uint64_t m_submitted = 0, m_completed = 0;
    std::vector<std::pair<uint64_t, Texture*>> m_deadTextures;
    std::vector<std::pair<uint64_t, DeadImage>> m_deadImages;
    bool m_deviceLost = false;
    // The next submission carries everything recorded so far (frame commands or pending uploads).
    uint64_t DeathTag() const { return m_submitted + 1; }
    void WaitFrame(Frame& f, const char* what);          // waits for f's fence, updates m_completed
    void CollectGarbage();
    static constexpr VkFormat kColorFormat = VK_FORMAT_B8G8R8A8_UNORM;
    static constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

    VkCommandPool m_pool = VK_NULL_HANDLE;
    std::array<Frame, kFramesInFlight> m_frames;
    // Profiling (profile.cpp): GPU timestamps per frame slot, CPU times of the frame's recording.
    static constexpr uint32_t kProfileMarks = 512;
    // Of them for the scene's class split (16 others a frame). Busy spots switch class (foliage / statics) hundreds of
    // times a frame: with too few, the rest of the scene went into the unattributed "scene" interval.
    static constexpr uint32_t kSceneClassMarks = 490;
    // The scene's shading (fragment shader invocations, a pipeline statistics query) per scene class: one query per
    // stretch of a class within a rendering, summed by class.
    static constexpr uint32_t kShadeQueries = 1024;
    static constexpr int kSceneClasses = 14;      // ShadeClass's names (0 = none; 11, 12: statics not pre-passed;
                                                  // 13: foliage alpha tested, not blended)
    struct ProfileFrame {
        VkQueryPool pool = VK_NULL_HANDLE;
        uint32_t count = 0;
        const char* names[kProfileMarks] = {};
        VkQueryPool shadePool = VK_NULL_HANDLE;
        uint32_t shadeCount = 0;                  // queries begun (and ended) this frame
        bool shadeActive = false;
        uint8_t shadeClass[kShadeQueries] = {};
        uint64_t shadePixels = 0;                 // the scene target's pixels (what the counts are divided by)
        bool shadeOverflow = false;               // ran out of queries: the rest of the frame went uncounted
        bool cloudsTimed = false;                 // the clouds' own commands (Frame::clouds) timed: the pool's last two
    };
    bool m_shadeQueries = false;                  // pipelineStatisticsQuery enabled
    bool m_sceneProfile = false;                  // RANDYVK_SCENE_PROFILE=1: the scene timed and counted by class
    uint64_t m_shadeSum[kSceneClasses] = {};      // fragment shader invocations by class since the last log
    uint64_t m_shadePixels = 0;                   // the frames' scene pixels, summed alike
    uint32_t m_shadeOverflowFrames = 0;           // of those frames, how many ran out of queries (undercounted)
    void ShadeQueryBegin();                       // in a scene rendering: start counting for m_shadeClass
    void ShadeQueryEnd();
    std::array<ProfileFrame, kFramesInFlight> m_profile;
    float m_timestampPeriod = 1.0f;
    uint32_t m_profileFrames = 0;
    double m_profileCpuStart = 0.0;
    std::vector<std::pair<const char*, double>> m_profileGpu, m_profileCpu;
    bool CreateProfiler(std::string* error);
    void DestroyProfiler();
    void ProfileBeginFrame(VkCommandBuffer cmd);
    void ProfileMark(const char* name);
    double ProfileCpu() const;
    void ProfileCpuAdd(const char* name, double since);
    void ProfileCpuAddMs(const char* name, double ms) { ProfileAdd(m_profileCpu, name, ms); }   // an already-measured duration
    static void ProfileAdd(std::vector<std::pair<const char*, double>>& sums, const char* name, double ms);
    void ProfileLog(const char* label);
    bool m_profileManual = false;
public:
    // The settings sweep's measurement windows: start (clears) / end (logs with the label). Automatic logging pauses.
    void ProfileWindow(bool start, const char* label);
    void ProfileManualEnd();
    // Threads (threaded.cpp): the render thread's idle time (waiting for the game's records), the game thread's
    // waits for the render thread (frames ahead, queue full) - microseconds since the last log.
    void ProfileAddIdle(double ms) { m_profileIdleMs += ms; }
    // The calling (game) thread's time inside the renderer's call path (ThreadedDevice::Enqueue), for the profiler.
    void ProfileAddCaller(double ms) { m_profileCallerMs += ms; }
    double m_profileCallerMs = 0.0;
    // ... and what it queued (records, bytes) - the hand-off's size.
    void ProfileAddCallerQueue(uint64_t records, uint64_t bytes, uint64_t repeats)
    {
        m_profileCallerRecords += records;
        m_profileCallerBytes += bytes;
        m_profileCallerRepeats += repeats;
    }
    uint64_t m_profileCallerRecords = 0, m_profileCallerBytes = 0, m_profileCallerRepeats = 0;
    // ... and its draws: vertices copied (count, bytes), shared vertex buffers, skinned pieces, indices copied (bytes),
    // shared draws whose indices were retained (no copy).
    void ProfileAddCallerDraws(uint64_t copied, uint64_t vertexBytes, uint64_t shared, uint64_t skinned,
                               uint64_t indexBytes, uint64_t retained, uint64_t handles = 0)
    {
        m_profileHandoff[6] += handles;
        m_profileHandoff[5] += retained;
        m_profileHandoff[0] += copied;
        m_profileHandoff[1] += vertexBytes;
        m_profileHandoff[2] += shared;
        m_profileHandoff[3] += skinned;
        m_profileHandoff[4] += indexBytes;
    }
    uint64_t m_profileHandoff[7] = {};
    uint64_t m_profileDraws = 0, m_profileGroupCalls = 0, m_profileGroupDraws = 0, m_profileConstBlocks = 0,
             m_profileConstReused = 0;   // the draws' batching (summed per frame for the log)
    std::atomic<uint64_t> m_profileGameWaitUs{0};
    // The live readout (RVK_PerfHud): the last frames' GPU time, the render thread's whole frame and its wait for the
    // GPU in it (ms, smoothed; written by the render thread, read by any).
    std::atomic<float> m_liveGpuMs{0.0f}, m_liveRenderMs{0.0f}, m_liveGpuWaitMs{0.0f};
    std::atomic<uint64_t> m_liveShown{0};        // frame generation's frames shown (framegen.cpp; 0 without it)
    static void LiveSmooth(std::atomic<float>& a, double ms)
    {
        const float old = a.load(std::memory_order_relaxed);
        a.store(old == 0.0f ? float(ms) : old + 0.1f * (float(ms) - old), std::memory_order_relaxed);
    }
    double m_profileIdleMs = 0.0;
    double m_timerCost = 0.0;                    // ms per clock read (subtracted from the draw sections)
    // Per-draw CPU sections, timed on every 16th frame (Draw); scaled to a per-frame average.
    // Every 16th frame only (sampled): the check inline - it runs about nine times a draw.
    void ProfileDrawSection(const char* name, double& since)
    {
        if ((m_frameNumber & 15) == 0) ProfileDrawSectionTimed(name, since);
    }
    void ProfileDrawSectionTimed(const char* name, double& since);
private:
    uint32_t m_frameIndex = 0;
    bool m_inFrame = false;
    bool m_rendering = false;
    static constexpr VkDeviceSize kRingSize = 64ull << 20;      // each frame's ring to start with
    static constexpr VkDeviceSize kRingMaxSize = 256ull << 20;  // ... at most (a 32-bit process: address space)
    VkDeviceSize m_ringWanted = kRingSize;       // after a mid-frame flush: the next frames' ring size
    VkDeviceSize m_ringPeak = 0;                 // the most a frame used (logged with the flushes)
    uint32_t m_ringQuietWindows = 0;             // 600-frame windows in a row that used under a quarter of the ring
    uint32_t m_ringFlushesLogged = 0;
    uint32_t m_ringTooSmallLogged = 0;           // requests bigger than the whole ring (logged, throttled)
    VkDeviceSize m_ringRefused = 0;              // a ring size the driver had no memory for: not asked for again
    std::vector<uint8_t> m_ringSpill;            // where a request bigger than the ring is written (never past its end)
    void WantRing(VkDeviceSize bytes);           // the next frames' ring holds at least `bytes` (to kRingMaxSize)
    // The draw arenas' reserve targets (entries), grown between frames when one overflows.
    static constexpr uint32_t kDrawConstCapacity = 2048, kDrawRecordCapacity = 16384;
    static constexpr uint32_t kMaxDrawConstCapacity = 8192, kMaxDrawRecordCapacity = 131072;
    uint32_t m_constWanted = kDrawConstCapacity, m_recordWanted = kDrawRecordCapacity;
    // The arenas' bytes in the ring (with the shadow arrays), and the sizes a frame reserves: the wanted ones, halved
    // until they take at most half of the ring (the rest is the frame's geometry); the ring grows for the next frames.
    VkDeviceSize ArenaBytes(uint32_t consts, uint32_t records) const;
    void FitArenas(uint32_t* consts, uint32_t* records);
    uint32_t m_arenaFlushes = 0;                 // mid-frame arena overflows (logged, throttled)
    // M4: the sun/point shadow passes' caster records and indirect commands, also slices of the frame's ring.
    // Reserved with the draw arenas; `m_shadowRecordCount` is shared by both passes (they append, not reset).
    static constexpr uint32_t kShadowRecordCapacity = 65536;
    VkDeviceSize m_shadowRecordBase = 0, m_shadowCmdBase = 0;
    uint32_t m_shadowRecordCount = 0, m_shadowCmdCount = 0, m_shadowArenaCapacity = 0;
    bool m_shadowArenaBound = false;
    struct ShadowGroup { bool active = false, indexed = false; uint64_t key = 0; uint32_t first = 0, count = 0; } m_shadowGroup;
    uint32_t m_shadowGroupCalls = 0, m_shadowGroupDraws = 0;   // M4 stats (per frame)
    uint32_t AppendShadowRecord(const detail::ShadowRecord& r);
    bool CreateRing(Frame& f, VkDeviceSize size, std::string* error);

    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    // Bindless textures (GPU-driven step M1): every texture and sampler in one descriptor set, so a draw selects
    // them by index instead of pushing descriptors. The set is created and populated now; the shaders still use the
    // per-draw bindings, so the frame is unchanged until they are switched over.
    static constexpr uint32_t kMaxBindlessImages = 65536, kMaxBindlessSamplers = 256;   // (images: texImages[] in constants.glsl, the same)
    bool m_bindless = false;
    VkDescriptorSetLayout m_bindlessSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_bindlessPool = VK_NULL_HANDLE;
    VkDescriptorSet m_bindlessSet = VK_NULL_HANDLE;
    uint32_t m_nextBindlessImage = 0, m_nextBindlessSampler = 0;
    std::vector<uint32_t> m_freeBindlessImages, m_freeBindlessSamplers;
    std::unordered_map<uint64_t, uint32_t> m_bindlessSamplerIndex;
    void RegisterBindlessTexture(Texture* t);
    void UnregisterBindlessTexture(Texture* t);
    uint32_t RegisterBindlessSampler(VkSampler s);
    uint32_t BindlessImage(Texture* t);          // its slot, registering it on first use (main targets predate the set)
    uint32_t BindlessSampler(VkSampler s);       // its slot, registering it on first use
    // A draw's sampler slots without the two map lookups (SamplerFor, then the slot) per sampler: each stage's last
    // sampler state and slot, and the last slot of each fixed sampler (bump, normal map) by handle.
    uint64_t SamplerKey(uint32_t stage) const;
    uint32_t StageSamplerSlot(uint32_t stage);
    uint32_t FixedSamplerSlot(uint32_t which, VkSampler s);
    uint64_t m_stageSamplerKey[2] = {~0ull, ~0ull};
    uint32_t m_stageSamplerSlot[2] = {};
    VkSampler m_fixedSampler[3] = {};
    uint32_t m_fixedSamplerSlot[3] = {};
    bool m_bindlessBound = false;                // set 1 bound in this frame's command buffer
    std::array<VkPipeline, 3> m_pipelines{};   // per topology class: points, lines, triangles
    VkBuffer m_nullBuffer = VK_NULL_HANDLE;    // zeros, bound at stride 0 for attributes a format lacks
    VmaAllocation_T* m_nullAllocation = nullptr;
    Texture* m_blackTexture = nullptr;         // what an unbound stage samples
    Texture* m_blueNoise = nullptr;            // 64 x 64 blue noise (r; g the same shifted half a tile): the cloud
                                               // march's offsets (clouds.cpp CreateBlueNoise)
    Texture* CreateBlueNoise();
    std::unordered_map<uint64_t, VkSampler> m_samplers;

    std::string m_screenshotPath;
    std::string m_dumpPath;
    std::string m_dumpOpenPath;                  // the dump being written (its files beside it: water mesh)
    std::string m_dumpBurstBase;                 // a run of frames dumped: <base>-NN.txt (RequestFrameDump frames)
    int m_dumpBurstLeft = 0, m_dumpBurstIndex = 0;
    FILE* m_dumpFile = nullptr;
    uint32_t m_dumpDraw = 0;
    uint32_t m_frameDraw = 0;                    // draws so far this frame (Draw calls)
    std::vector<d3d::Light> m_dumpedLights;
    std::vector<const Texture*> m_dumpGroundTextures;   // the ground's base textures drawn (the dump's -ground.bmp)
    void DumpGroundTextures(const std::string& path);
    void DumpTargets(const std::string& base);   // the frame's in-between images (diag.cpp; EndScene, with a dump)
    struct DumpedLightmap { const Texture* texture; float u[4], v[4]; };   // (the dump's -lightmaps.bin)
    std::vector<DumpedLightmap> m_dumpLightmaps;
    void DumpLightmaps(const std::string& path);
    std::vector<bool> m_dumpedLightValid;
    void BeginFrameDump();
    void EndFrameDump();
    void DumpDraw(uint32_t primitive, uint32_t fvf, const void* vertices, uint32_t vertexCount, const uint16_t* indices,
                  uint32_t indexCount);
    void DumpTerrainMapping(FILE* f, const void* vertices, uint32_t vertexCount, const detail::FvfLayout& layout,
                            const uint16_t* indices, uint32_t indexCount);
    VkBuffer m_readback = VK_NULL_HANDLE;
    VmaAllocation_T* m_readbackAllocation = nullptr;
    void* m_readbackData = nullptr;

    // D3D7 state
    std::array<uint32_t, d3d::RS_COUNT> m_rs{};
    std::array<std::array<uint32_t, d3d::TSS_COUNT>, 2> m_tss{};
    d3d::Matrix m_world{}, m_view{}, m_proj{};
    // The free camera (SetFreeCam): the game's latest view as it set it, its latest main view (one with a camera
    // position: not the sky's), and the free camera's place.
    d3d::Matrix m_gameView{}, m_gameMainView{};
    bool m_gameMainValid = false, m_freeCam = false;
    float m_freePos[3] = {}, m_freeYaw = 0.0f, m_freePitch = 0.0f;
    void ApplyFreeCam();
    std::array<d3d::Matrix, 2> m_texMatrix{};
    d3d::Material m_material{};
    std::vector<LightSlot> m_lights;
    std::array<Texture*, 2> m_textures{};
    d3d::Viewport m_viewport{};

    // The street seen from inside a shop (portal.cpp): the room's door's draws this frame, drawn again over the street
    // (its leaf swung open out of the room stands behind the opening, which the street covers).
    struct StreetDoorDraw {
        uint32_t primitive = 0, fvf = 0, vertexCount = 0;
        std::vector<uint8_t> vertices;
        std::vector<uint16_t> indices;
        std::array<uint32_t, d3d::RS_COUNT> rs{};
        std::array<std::array<uint32_t, d3d::TSS_COUNT>, 2> tss{};
        std::array<Texture*, 2> textures{};
        std::vector<LightSlot> lights;
        d3d::Matrix world{};
        d3d::Material material{};
        const MeshInfo* mesh = nullptr;
        const char* visualName = "";
        uint32_t kind = 0, visual = 0, owner = 0;
    };
    std::vector<StreetDoorDraw> m_streetDoorDraws;
    // What was taken, for the log: each piece once a visit, and frames that took fewer than the one before.
    std::vector<std::pair<uint64_t, uint32_t>> m_streetDoorSeen;   // (geometry, vertices)
    uint32_t m_streetDoorLast = 0, m_streetDoorLines = 0;
    bool m_redrawingDoor = false;
    void NoteStreetDoorDraw(uint32_t primitive, uint32_t fvf, const void* vertices, uint32_t vertexCount,
                            const uint16_t* indices, uint32_t indexCount);
    void RedrawStreetDoor(std::vector<StreetDoorDraw>& draws);
};

}  // namespace rvk
