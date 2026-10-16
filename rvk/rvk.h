// rvk: a Vulkan renderer with a Direct3D 7 shaped front end (the part of D3D7 Anarchy Online uses).
//
// Usage per frame: BeginFrame(), state and draw calls, EndFrame(). Rendering goes into the main colour
// target (or one set with SetRenderTarget) plus a shared depth buffer; EndFrame() copies the main target
// to the window's swapchain (if a window was given) and can save it as a screenshot.
#pragma once

#include "d3d7.h"
#include "skin.h"
#include "vk.h"

#include <array>
#include <memory>
#include <cstdint>
#include <string>
#include <atomic>
#include <algorithm>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct VmaAllocator_T;
struct VmaAllocation_T;
struct VmaVirtualBlock_T;

namespace rvk::detail { struct FrameLights; struct FvfLayout; struct DrawConstants; struct DrawTransform; struct ShadowRecord; }

namespace rvk {

// What the game's visual issuing a draw is (Device::SetDrawVisual; same values as rnative::scene::Kind).
enum class VisualKind : uint32_t {
    Unknown = 0, Character, CharacterPart, Static, Terrain, Room, Water, Sky, BlobShadow, Effect, Other,
};

class Device;

// Diagnostics. Default: stderr, flushed per line. SetLogSink redirects (e.g. into the game's log file).
void Log(const char* fmt, ...);
void SetLogSink(void (*sink)(const char* line));
// Where the pipeline cache is kept between runs (compiled pipelines load from it instead of being built again at every
// start). Empty or never set: RANDYVK_PIPELINE_CACHE, else no cache file ("off" also disables it).
void SetPipelineCachePath(const char* path);

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

private:
    friend class Device;
    VkImage m_image = VK_NULL_HANDLE;
    VkImageView m_view = VK_NULL_HANDLE;
    VmaAllocation_T* m_allocation = nullptr;
    uint32_t m_width = 0, m_height = 0, m_levels = 1;
    Format m_format = Format::A8R8G8B8;
    bool m_renderTarget = false;
    bool m_opaque = false;             // the texture's pixels are all alpha 1 (a blended static draw can be opaque)
    // A small RGB copy of level 0 for the ground grass (PixelsThumbnail): textures up to 64 x 64 (the terrain's
    // lightmaps) whole, bigger ones on a 16 x 16 grid (enough to classify a ground atlas' tiles).
    std::vector<uint8_t> m_thumb;
    uint32_t m_thumbW = 0, m_thumbH = 0;
    bool m_thumbValid = false;
    // Not fully opaque: the mean alpha (0..255) on a grid of up to 64 x 64 cells over level 0, for the leaves
    // (leaves.cpp: where a canopy card is leaf and where it is hole). Empty until a full level-0 upload.
    std::vector<uint8_t> m_alphaMask;
    uint32_t m_alphaW = 0, m_alphaH = 0;
    uint32_t m_alphaVersion = 0;       // counts the uploads that changed the mask (a canopy's leaves are rebaked)
    VkDeviceSize m_bytes = 0;          // its image's memory (the GPU memory report)
    bool m_lightmap = false;           // captured as a terrain lightmap (a re-upload recaptures the grass's light)
    bool m_groundBase = false;         // captured as a terrain base texture (a re-upload recaptures the grass's ground)
    Texture* m_normalMap = nullptr;    // tangent-space normal map drawn with this texture (owned; SetNormalMap)
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

class Device {
public:
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
    // Enhancement, with per-pixel lighting: every lit draw gets the frame's active point / spot lights nearest
    // the camera instead of the (at most 8) the game enabled for it. Directional lights stay as the game set them.
    void SetLightOverride(bool enable) { if (m_lightOverride != enable) { m_lightOverride = enable; m_constantsDirty = true; } }
    // A character's own light (at head height) lights the character too; it never shadows it either way.
    void SetCarrierLit(bool enable) { if (m_carrierLit != enable) { m_carrierLit = enable; m_constantsDirty = true; } }
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
    struct PickResult { Texture* texture = nullptr; uint32_t serial = 0, visual = 0, owner = 0, kind = 0, vertices = 0,
                        indices = 0; };
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
        return r;
    }
    void SetPickHighlight(Texture* t) { if (m_pickHighlight != t) { m_pickHighlight = t; m_constantsDirty = true; } }
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
    void SetBloomOverNearer(float keep) { m_bloomOverNearer = keep; }
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
    // Wind for small plants (grass, bushes, flowers): 0 = still.
    void SetSway(float strength) { m_sway = strength; m_constantsDirty = true; m_frameLightsDirty = true; }
    void SetWindStorm(float scale) { m_windStormScale = scale; }   // RVK_WindStorm (wind.cpp)
    void SetStormTint(bool on) { m_stormTint = on; }               // RVK_StormTint (Draw: the game's storm sheet)
    void SetStormFlash(bool on) { m_stormFlash = on; }             // RVK_StormFlash (Draw: the game's lightning glow)
    void SetLightning(float s) { m_lightning = s; }                // RVK_Lightning (rain.cpp: our flash, x)
    void TriggerLightning(const float pos[3]);                     // a strike there now (the game's, or a test's)
    void SetSandHaze(float amount) { m_sandHaze = amount; }        // RVK_SandHaze (wind.cpp)
    void SetSandBlow(float amount) { m_sandBlow = amount; }        // RVK_SandBlow (smoke.cpp: the sand volume)
    void SetSandProcedural(bool on) { m_sandProcedural = on; }     // RVK_SandProc (smoke_sim.comp stage 12)
    void SetSandContrast(float k) { m_sandContrast = k; }          // RVK_SandContrast (smoke.frag)
    void SetSandClear(float d) { m_sandClear = d; }                // RVK_SandClear (smoke.frag)
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
    void SetSandGusts(float s) { m_sandGusts = s; }                // RVK_SandGusts
    void SetSandFog(float s) { m_sandFog = s; }                    // RVK_SandFog (sand_fog.frag, the default; 0: the flat haze)
    void SetSandSun(float s) { m_sandSun = s; }                    // RVK_SandSun (sand_fog.frag)
    void SetSandTint(float s) { m_sandTint = s; }                  // RVK_SandTint (tonemap.frag)
    float SandFogThrough(const float eye[3], const float p[3]) const;   // (smoke.cpp)
    // The storm's air in the fog (sand_fog.frag): a sand or ash storm's dust, a thunderstorm's rain mist (RVK_RainFog),
    // or both - how thick at the ground (per unit), the dust's share, the thick layer's scale height, the share of the
    // tall column above (scale height upperHeight). (wind.cpp)
    struct StormAir { float sigma = 0.0f, sandShare = 1.0f, height = 120.0f, upper = 0.3f, upperHeight = 1500.0f; };
    StormAir StormAirNow() const;
    void SetRainFog(float s) { m_rainFog = s; }                    // RVK_RainFog (x; 0: none)
    void SetStormGreen(float s) { m_stormGreen = s; }              // RVK_StormGreen (0..1)
    void SetRainVeils(float s) { m_rainVeils = s; }                // RVK_RainVeils (x; clouds.cpp, cloud_layer.frag Veils)
    void SetStormDark(float s) { m_stormDark = s; }                // RVK_StormDark (x)
    void SetRainWet(float s) { m_rainWet = s; }                    // RVK_RainWet (x)
    void SetStormBolt(float s) { m_stormBolt = s; }                // RVK_StormBolt (x; 0: the game's bolts)
    void SetStormDebris(float s) { m_stormDebris = s; }            // RVK_StormDebris (x)
    void SetWetNow(float wet, float puddles) { m_wetness = wet; m_puddles = puddles; }   // (tests: soaked already)
    float StormOvercast() const;
    float StormSky() const { return std::clamp(m_stormSky, 0.0f, 1.0f); }   // how much a storm's sky (rain.cpp)
    void StormSmokeWind(float out[3]) const;                       // a storm's own wind on the smoke (smoke.cpp)                                   // how much a thunderstorm hides the sun (rain.cpp)
    void StormHue(float rgb[3]) const;                             // a colour turned the storm's dark yellow-green (rain.cpp)
    // A thunderstorm's green cast (RVK_StormGreen x how hard it rains): per channel, about unit brightness (rain.cpp).
    void StormGreen(float out[3]) const;
    bool SandFogOn() const { return m_sandFog > 0.0f && ((m_sand > 0.01f && m_sandHaze > 0.0f) || (m_rain + 0.3f * m_stormSky) * m_rainFog > 0.01f); }
    bool SmokeCoverValid() const { return m_smokeHalf && m_smokeHalfFrame == m_frameNumber; }
    // Grass and plants bending out of the way of characters walking through them (0 = off; scales reach and bend).
    void SetGrassPush(float strength) { m_grassPush = strength; m_frameLightsDirty = true; }
    // Plants' big quads split into pieces for smooth bending (0 = off; 1 = pieces about 0.3 units across).
    void SetPlantDetail(float detail) { m_plantDetail = detail; }
    // Procedural ground grass (RVK_GrassOn, grass.cpp): blades generated over the terrain near the camera. off by
    // default; distance is the radius (world units), density the blades per patch, height the blade height.
    // The blades' width as a multiplier (1 = the built-in width). Baked, so a change rebuilds the tiles.
    void SetGrassWidth(float width)
    {
        if (width != m_grassWidth) {
            m_grassWidth = width;
            m_grassDirty = true;
        }
    }
    // The grass's brightness against its ground (1 = as bright as the ground it grows on); per frame, no rebuild.
    void SetGrassBrightness(float brightness) { m_grassBright = brightness; }
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
    // RVK_CrownShade: tree and shrub canopies (the game's cards) lit as one rounded crown - their normals bent out from
    // its centre, darker inside and under, each tree's colour a little its own (ffp.vert), with the leaves off too.
    void SetCrownShade(float amount) { m_crownShade = amount; m_frameLightsDirty = true; }
    float m_crownShade = 1.0f;
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
    // Foliage beyond this distance (world units) is shaded more cheaply (0 = off).
    void SetFoliageLod(float distance) { m_foliageLod = distance; }
    // Depth pre-pass (prepass.cpp): the scene's opaque draws' depth first, so each pixel is shaded about once.
    void SetDepthPrepass(bool on) { m_prepassOn = on; }
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
    void SetFxaa(bool enable) { m_fxaa = enable; }   // FXAA: the anti-aliasing while TAA is off (fxaa.frag)
    // Sun shadow penumbra growth with blocker distance (0 = hard), sunlight through leaves, night glow of bright
    // texels on unlit / self-lit surfaces, contact shadows (screen-space, against the sun).
    void SetSunSoftness(float s) { m_sunSoftness = s; m_frameLightsDirty = true; }
    void SetLeafLight(float s) { m_leafLight = s; m_frameLightsDirty = true; }
    void SetNightGlow(float s) { m_nightGlow = s; m_frameLightsDirty = true; m_constantsDirty = true; }
    // The moon's light at night (night.cpp; 0 = none, 1 = as planned).
    void SetMoonLight(float s) { m_moonLight = s; m_frameLightsDirty = true; m_constantsDirty = true; }
    // How dark nights get (tonemap.frag; 0 = as the game, 1 = moonlit blue, 2 = dark).
    void SetNightDark(float s) { m_nightDark = s; }
    // The air's light scattering over distance (tonemap.frag Air; RVK_Atmos; off: the plain land haze).
    void SetAtmos(bool on) { m_atmos = on; }
    // Valley mist's amount (mist.cpp; 0 none, 1 as planned).
    void SetMist(float s) { m_mist = s; }
    // The sun's disc (sun.cpp; 0 none, 1 as planned: its brightness).
    void SetSun(float s) { m_sun = s; }
    // Their size (x the game's sun sprites) and the share of their glow over the clouds (sun.cpp).
    void SetSunSize(float s) { m_sunSize = s; }
    void SetSunBleed(float s) { m_sunBleed = s; }
    // Remembered shadow casters (things out of view still casting) and the terrain's shade from a low sun, on / off.
    // (0 off, 1 on, 2 on without the remembered sway, 3 only big things, 4 drawn solid; switched: the churn trace and a few seconds of
    // the shadow items' counts anew - finding a blinking shadow)
    void SetShadowMemory(uint32_t mode) { m_shadowMemory = mode; m_cacheTraceLines = 0; m_memoryTraceFrames = 240; }
    void SetHillShade(bool on) { m_hillShade = on; }
    void SetShadowView(bool on) { m_shadowViewDebug = on; m_frameLightsDirty = true; }   // (debug: RVK_ShadowView)
    // The game's own sun sprites (its glow, VisualSunRays_t), shown or not.
    void SetGameSun(bool on) { m_gameSun = on; }
    // Tests: the sky weather's clock (seconds, as UTC; < 0 the wall clock).
    void SetSkyTime(double t) { m_skyTime = t; }
    // The playfield the game is in (its resource id; 0 none): its climate leans the sky (sky.cpp SkyZoneLean).
    void SetSkyPlayfield(uint32_t playfield);
    void SetContactShadows(float s) { m_contact = s; m_constantsDirty = true; }
    void SetEffectGlow(float gain) { if (m_effectGlow != gain) { m_effectGlow = gain; m_constantsDirty = true; } }
    float EffectGlow() const { return m_effectGlow; }
    float BloomStrength() const { return m_bloomStrength; }
    // With HDR: how bright local lights may make a surface (1 = the game's clamp; soft roll-off towards it).
    void SetHdrHeadroom(float headroom) { m_hdrHeadroom = headroom < 1.0f ? 1.0f : headroom; m_frameLightsDirty = true; }
    float HdrHeadroom() const { return m_hdrHeadroom; }
    // Enhancement, with the light override: how bright the frame's lights may make a surface (1 = D3D's clamp, up to
    // 2). The game's own lighting stays clamped at 1; local lights add on top of it, so they (and their shadows) show
    // on surfaces the sun already lights fully.
    void SetLightHeadroom(float headroom) { headroom = headroom < 1.0f ? 1.0f : headroom > 2.0f ? 2.0f : headroom;
                                            if (m_lightHeadroom != headroom) { m_lightHeadroom = headroom; m_constantsDirty = true; m_frameLightsDirty = true; } }
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
        uint32_t quality = 2;                    // 1 low, 2 medium, 3 high: grid density, reflection steps
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
    // The layer holds an interface drawn at the current size (else the next frame must redraw it). Any thread.
    bool InterfaceLayerReady() const { return m_uiLayerReady.load(std::memory_order_relaxed); }
    // Interface glass (interface.cpp, docs/ui-dll.md phase 1): frosted glass behind the interface's windows. While
    // the interface is redrawn into its layer, each window's rectangle on screen (pixels) - kept for the frames that
    // show the layer without redrawing it.
    struct GlassParams {
        bool enable = false;
        uint32_t blur = 3;                       // downsampling steps (1-4): the blur's width
        float dim = 0.25f;                       // darkens the scene behind (0-1)
        float tint = 0.5f;                       // towards the deep blue (0-1)
        float edge = 0.5f;                       // rim light and glow (0-1)
        float radius = 6.0f;                     // corner radius (pixels)
        float tintColor[3] = {0.04f, 0.16f, 0.28f};   // what the tint goes towards
        float rimColor[3] = {0.45f, 0.80f, 1.00f};    // the rim light's and glow's colour
    };
    void SetInterfaceGlass(const GlassParams& p) { m_glass = p; }
    void InterfaceWindow(float x0, float y0, float x1, float y1);
    // The interface back end (interface.cpp, docs/ui-dll.md phase 2): shapes of our own drawn into the interface
    // where the game's drawing has got to - an anti-aliased rounded rectangle with a vertical gradient fill, a border
    // inside its edge and a glow outside it, premultiplied over what is there. At the interface's depth (`depth`:
    // GUI.dll's Z 0.01 in the game's viewport, InterfaceDepth): the shapes in order, depth tested, then their solid
    // parts write depth; glow-only shapes (glowOnly) depth tested LESS_OR_EQUAL - over shapes at that depth - and
    // nothing written (the caller keeps them off windows in front). Colours straight (not premultiplied).
    struct UiShape {
        float rect[4];                           // x0, y0, x1, y1 on the target (pixels)
        float clip[4];                           // drawn only inside this (pixels)
        float radius, border, glow, glowOnly;    // corner radius, border width, glow width (pixels); glowOnly 1:
                                                 // just the glow, drawn last (below)
        float fillTop[4], fillBottom[4];         // rgba
        float borderColor[4], glowColor[4];      // rgba
        // An image instead of a shape (image non-null): the texture's part `uv` (u0, v0, u1, v1) over rect, times
        // fillTop (straight rgba); additive 1: its light added (alpha out 0), else over by its own alpha.
        Texture* image;
        float uv[4];
        float additive;
    };
    // Meshes with the shapes (docs/ui-dll.md phase 9, RmlUi's geometry): triangles of UiVertex (RmlUi's Vertex
    // layout: position in pixels, premultiplied rgba8, texture coordinates), each mesh some of the batch's indices
    // moved by its translation, scissored to its clip, times its texture (null: white) and opacity. Drawn after the
    // shapes' colour pass and before their depth pass - in front of the window's shapes, behind windows in front (depth
    // tested LESS, not written).
    struct UiVertex {
        float x, y;
        uint8_t rgba[4];
        float u, v;
    };
    struct UiMesh {
        uint32_t firstIndex, indexCount;
        float translate[2];
        float clip[4];                           // x0, y0, x1, y1 (pixels)
        Texture* image;
        float opacity;
    };
    struct UiMeshes {
        const UiMesh* meshes;
        uint32_t count;
        const UiVertex* vertices;
        uint32_t vertexCount;
        const uint32_t* indices;
        uint32_t indexCount;
    };
    void UiShapes(const UiShape* shapes, uint32_t count, float depth, const UiMeshes* meshes = nullptr);
    float InterfaceDepth() const;
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
        // Clouds (clouds.cpp DrawClouds): their shadow map and half-size march, recorded where the sky ends but run
        // between the pre-pass and mainB - outside the scene's rendering, which goes on unbroken (its pre-pass too).
        VkCommandBuffer clouds = VK_NULL_HANDLE;
        bool cloudsRecorded = false;
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
        bool glowBlendSet = false;               // attachment 1 (glow) blend state, HDR pipelines
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
    Texture* CreateImage(uint32_t width, uint32_t height, Format format, uint32_t levels, bool renderTarget);
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
    float m_lightHeadroom = 1.0f;
    float m_bump = 0.0f;
    bool m_normalMaps = true;
    float m_normalStrength = 1.0f;
    Texture* m_flatNormal = nullptr;             // binding 9 when the draw has no normal map
    VkSampler m_normalSampler = VK_NULL_HANDLE;
    Texture* m_constantsNormalMap = nullptr;
    bool m_constantsSkyScenery = false;
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
    const void* m_pickSkinned = nullptr;         // a GPU-skinned draw's CPU-skinned vertices, for the pick (DrawSkinned)
    std::atomic<uint32_t> m_pickSerial{0};
    Texture* m_pickHighlight = nullptr;
    void PickDraw(uint32_t primitive, const detail::FvfLayout& layout, const void* vertices, uint32_t vertexCount,
                  const uint16_t* indices, uint32_t indexCount);
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
    static constexpr uint32_t kGrassShadowLayer = kShadowCascades, kShadowLayers = kShadowCascades + 1;
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
    uint64_t m_cascadeFrame = 0;                 // the far cascades are redrawn on alternate frames
    float m_shadowSunDir[3] = {};
    // The moon (night.cpp, RVK_MoonLight): once the sun is down, a dim cool light from the moon on the land - the
    // shadow map drawn from it, the game's night light (from under the ground) turned to come from it.
    float m_moonLight = 1.0f;                    // RVK_MoonLight (0: none)
    float m_nightDark = 1.0f;                    // RVK_NightDark (0: as the game)
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
    void SkyWind(float out[2]) const;
    void UpdateWind();                           // the frame's shared wind (wind.cpp): direction, strength, gusts, clock
    void UpdateLightning();                      // a strike from the game's glow last frame (rain.cpp)
    void Lightning(float flash[4], float view[4], float proj[4], float world[4]) const;   // this frame's flash (FrameLights)
    float Gusts() const { return std::max(m_grassGusts, 0.0f) * m_windGusts; }   // RVK_GrassGusts x the storm's
    float m_cloudWindSpeed = 8.0f;               // the clouds' drift (world units a second), eased (UpdateCloudDrift)
    static const char* SkyName(float cloudiness);
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
        uint64_t itemKey;                        // its shadow item's batch key (ShadowItemKey), 0: not yet
        Texture* itemKeyTexture;                 // ... made with this cut-out texture (null: solid)
    };
    static constexpr double kSwayStep = 0.05;
    static constexpr size_t kSwaySamples = 80;
    // (at: where it stood when the streak began; moved: it has moved since - a thing that moves isn't remembered.)
    struct CasterStreak { uint64_t lastFrame = 0; uint32_t count = 0; float at[3] = {}; bool moved = false; };
    std::unordered_map<uint64_t, CachedCaster> m_casterCache;
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
    void ForgetCachedCaster(std::unordered_map<uint64_t, CachedCaster>::iterator it);
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
    bool m_sceneSaw3D = false;
    uint32_t m_sceneEndDraw = 0, m_sceneEndFvf = 0;   // frame dumps: where the scene phase ended
    VkPipeline m_pipelinesHdr[3] = {};           // m_pipelines for the float target
    VkPipeline m_pipelinesNoCut[3] = {};         // ... and the no-discard variant (early-Z; no cut-out)
    VkPipeline m_pipelinesHdrNoCut[3] = {};
    VkPipeline m_tessPipelines[2] = {};          // characters' Phong tessellation: 8-bit target, float target
    VkPipeline m_tessPipelinesNoCut[2] = {};     // ... no-discard
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
    struct DeferredEdge {
        uint32_t primitive, fvf, stride, vertexCount, indexCount, record;
        VkDeviceSize vbOffset, ibOffset;
        VkBuffer vb, ib;
        VkDeviceSize frameLights, prevOffset, prevBytes, smoothOffset, smoothBytes;
        VkBuffer prevBuffer, smoothBuffer;
        d3d::Viewport viewport;
        uint32_t cull, zFunc;
    };
    std::vector<DeferredEdge> m_edges;
    uint64_t m_edgeFlushes = 0, m_edgeDraws = 0;   // logged with the pre-pass (PrepassLog)
    void FlushEdges();                           // the queued edges, drawn now (in the current rendering)
    // The main pass's per-draw push (bindings 4, 8, 10), skipped when it equals the last one (StateCache).
    void PushDrawSet(VkCommandBuffer cmd, VkDeviceSize frameLightsOffset, VkBuffer prevBuffer, VkDeviceSize prevOffset,
                     VkDeviceSize prevBytes, VkBuffer smoothBuffer, VkDeviceSize smoothOffset, VkDeviceSize smoothBytes);
public:
    void SetFoliageEdges(bool on) { m_foliageEdges = on; }
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
    uint32_t PrepassCheck(uint32_t primitive, uint32_t fvf, bool swaying) const;   // kPreIn: into the pre-pass
    uint32_t AlphaOneCheck(uint32_t fvf) const;  // kPreIn: the output alpha is provably 1; else the input in the way
    bool BackdropCull(uint32_t fvf) const;       // the current draw gets the backdrop cull (m_drawBackdrop)
    void PrepassLog();                           // every 600 frames
    void PrepassDraw(uint32_t primitive, uint32_t fvf, uint32_t stride, uint32_t vertexCount, uint32_t indexCount,
                     VkDeviceSize vbOffset, VkDeviceSize ibOffset, VkBuffer vb, VkBuffer ib,
                     VkDeviceSize frameLightsOffset, VkBuffer prevBuffer, VkDeviceSize prevOffset, VkDeviceSize prevBytes,
                     VkBuffer smoothBuffer, VkDeviceSize smoothOffset, VkDeviceSize smoothBytes, uint32_t recordIndex, bool cutout);
    uint32_t FrameCommands(Frame& f, VkCommandBuffer out[4]);   // what a submission runs (ends the pre-pass)
    void FrameCommandsSubmitted(Frame& f);       // back to one main command buffer
    // Shared with ApplyDynamicState: the D3D viewport as Vulkan's (and its scissor), the FVF's vertex input.
    void ViewportState(VkViewport* vp, VkRect2D* scissor) const;
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
    // How much it is night for the grading (tone map, bloom, night glow): the sun's light on flat ground, but no more
    // than the game's ambient says (at dawn its light is still level and off while its world has brightened).
    float NightAmount() const;
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
    void CaptureTerrain(uint32_t primitive, const detail::FvfLayout& layout, const void* vertices, uint32_t vertexCount,
                        const uint16_t* indices, uint32_t indexCount);
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
    bool DrawGrassPointShadow(VkCommandBuffer cmd, const d3d::Matrix& faceViewProj, const float light[3], float range);
    bool GrassBelow(const float light[3], float range) const;
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
    };
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
    WaterMap* WaterMapFor(const WaterVertex* v, uint32_t count, const uint16_t* idx, uint32_t icount,
                          std::vector<uint32_t>& steep);
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
    void DestroyLeafResources();
    float m_pointLightScale = 1.0f, m_charLightScale = 1.0f;   // SetPointLightIntensity
    float LightScale(const d3d::Light& l) const;
    bool FoliageFar() const;                  // pieces per 0.3 world units (0 = plants drawn as the game gives them)
    void SubdividePlant(uint32_t& primitive, uint32_t fvf, const detail::FvfLayout& layout, const void*& vertices,
                        uint32_t& vertexCount, const uint16_t*& indices, uint32_t& indexCount);
    float m_drawSway[4] = {};                    // the current draw's sway (for its shadow caster)
    bool m_dynamicWriteMask = false;             // per-draw colour write masks (extended dynamic state 3)
    void FrameLightMask(uint32_t fvf, uint32_t stride, const void* vertices, uint32_t vertexCount, uint32_t out[4]);
    struct LightSphere { float x, y, z, r2; };
    std::vector<LightSphere> m_frameLightSpheres;  // the frame lights' spheres in FrameLights order (light masks)
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
    // A per-frame offset (0..1) for the noise patterns of sampled effects while the TAA averages frames; else 0.
    float FrameNoise() const { return TaaActive() ? float(m_frameNumber % 64 * 618034 % 1000000) * 1e-6f : 0.0f; }
    bool TaaParams(float out[24]);               // before MotionBlurParams (it moves on the last frame's camera)
    void RenderTaa(VkCommandBuffer cmd, Texture* dst, const float params[24]);
    float m_bloomOverNearer = 0.15f;             // bloom left on objects in front of its light (1 = all)
    bool m_bloomDepth = false;                   // this frame's bloom carries its light's depth
    std::vector<Texture*> m_bloomLevels;         // half resolution and down, float
    float m_effectGlow = 1.0f;
    Texture* m_glow = nullptr;                   // second scene attachment: what additive effects add (F_GLOW);
                                                 // alpha: its brightness / view depth (the bloom's depth)
    Texture* m_localFraction = nullptr;          // third: how much of each pixel's colour local lights gave it;
                                                 // G: reflectivity; B: the direct sunlight's share (contact shadows)
    bool m_glowCleared = false;                  // this frame
    uint32_t m_glowDraws = 0;                    // this frame's draws feeding the glow (frame dumps)
    bool GlowDraw(uint32_t fvf) const;
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
                        VkImageView src3 = VK_NULL_HANDLE, VkImageView src4 = VK_NULL_HANDLE);
    void RenderObjectMotionBlur(VkCommandBuffer cmd, const float params[24]);
    float m_aoStrength = 1.0f, m_aoRadius = 1.5f;
    Texture* m_aoTex[2] = {};                    // half resolution: raw, blurred (ping-pong)
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
    bool RenderSsr(VkCommandBuffer cmd);
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
    bool MotionBlurParams(float out[24]);        // reprojection + parameters for this frame; false: no blur
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
    bool m_uiLayerOpen = false;                  // between InterfaceBegin and InterfaceEnd
    std::atomic<bool> m_uiLayerReady{false};
    VkPipeline m_uiCompositePipeline = VK_NULL_HANDLE;
    void InterfaceSceneEnd();                    // what the interface's first draw does: the scene ends
    // Interface glass (interface.cpp).
    GlassParams m_glass;
    std::vector<std::array<float, 4>> m_glassRects;   // the windows of the interface in the layer
    Texture* m_glassTex[4] = {};                 // the blur's levels: 1/2, 1/4, 1/8, 1/16 of the main target
    VkPipeline m_glassDownPipeline = VK_NULL_HANDLE, m_glassUpPipeline = VK_NULL_HANDLE, m_glassPipeline = VK_NULL_HANDLE;
    void RenderGlass(VkCommandBuffer cmd);
    // The interface back end's shapes (interface.cpp): made at their first use.
    VkPipelineLayout m_uiShapeLayout = VK_NULL_HANDLE;
    VkPipeline m_uiShapePipeline = VK_NULL_HANDLE;         // colour: depth tested, not written
    VkPipeline m_uiShapeDepthPipeline = VK_NULL_HANDLE;    // depth only, where the shape is solid
    VkPipeline m_uiShapeGlowPipeline = VK_NULL_HANDLE;     // glows: colour, depth tested LESS_OR_EQUAL
    bool m_uiShapeFailed = false;
    bool CreateUiShapePipeline();
    void DestroyUiShapePipeline();
    // ... and its images (ui_image.frag): one texture by push descriptor, the colour pass only.
    VkDescriptorSetLayout m_uiImageSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_uiImageLayout = VK_NULL_HANDLE;
    VkPipeline m_uiImagePipeline = VK_NULL_HANDLE;
    bool DrawUiImage(VkCommandBuffer cmd, const UiShape& s, float depth, int32_t w, int32_t h);
    // ... and its meshes (ui_mesh.vert / .frag): vertices and indices from the ring, one texture by push descriptor.
    VkPipelineLayout m_uiMeshLayout = VK_NULL_HANDLE;
    VkPipeline m_uiMeshPipeline = VK_NULL_HANDLE;
    void DrawUiMeshes(VkCommandBuffer cmd, const UiMeshes& m, float depth, int32_t w, int32_t h);
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
    };
    double m_pointShadowFadeIn = 0.3;            // seconds (0 = shadows appear at once)
    double m_pointShadowClock = 0.0;
    PointShadowLight m_pointShadowLights[kMaxPointShadows] = {};   // the lights the cubes hold (for this frame)
    uint32_t m_pointShadowCount = 0;
    uint32_t m_pointShadowDraws = 0;             // last pass: caster draws into the cubes (frame dumps)
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
    void Transition(VkCommandBuffer cmd, Texture* t, VkImageLayout to);
    VkCommandBuffer UploadCommands();
    void SaveScreenshot();
    void DestroyTextureNow(Texture* texture);

    // Vulkan objects
    VkInstance m_instance = VK_NULL_HANDLE;
    VkPhysicalDevice m_physical = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_queue = VK_NULL_HANDLE;
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
    // The draw arenas' reserve targets (entries), grown between frames when one overflows.
    static constexpr uint32_t kDrawConstCapacity = 2048, kDrawRecordCapacity = 16384;
    static constexpr uint32_t kMaxDrawConstCapacity = 8192, kMaxDrawRecordCapacity = 131072;
    uint32_t m_constWanted = kDrawConstCapacity, m_recordWanted = kDrawRecordCapacity;
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
    std::string m_dumpBurstBase;                 // a run of frames dumped: <base>-NN.txt (RequestFrameDump frames)
    int m_dumpBurstLeft = 0, m_dumpBurstIndex = 0;
    FILE* m_dumpFile = nullptr;
    uint32_t m_dumpDraw = 0;
    uint32_t m_frameDraw = 0;                    // draws so far this frame (Draw calls)
    std::vector<d3d::Light> m_dumpedLights;
    std::vector<bool> m_dumpedLightValid;
    void BeginFrameDump();
    void EndFrameDump();
    void DumpDraw(uint32_t primitive, uint32_t fvf, const void* vertices, uint32_t vertexCount, const uint16_t* indices,
                  uint32_t indexCount);
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
};

}  // namespace rvk
