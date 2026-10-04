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
#include <vector>

struct VmaAllocator_T;
struct VmaAllocation_T;

namespace rvk::detail { struct FrameLights; struct FvfLayout; }

namespace rvk {

class Device;

// Diagnostics. Default: stderr, flushed per line. SetLogSink redirects (e.g. into the game's log file).
void Log(const char* fmt, ...);
void SetLogSink(void (*sink)(const char* line));

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

private:
    friend class Device;
    VkImage m_image = VK_NULL_HANDLE;
    VkImageView m_view = VK_NULL_HANDLE;
    VmaAllocation_T* m_allocation = nullptr;
    uint32_t m_width = 0, m_height = 0, m_levels = 1;
    Format m_format = Format::A8R8G8B8;
    bool m_renderTarget = false;
    Texture* m_normalMap = nullptr;    // tangent-space normal map drawn with this texture (owned; SetNormalMap)
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
    void RequestFrameDump(const std::string& path);       // the next frame's 3D draws, written as text (diag.cpp)
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
    void SetHdr(bool enable) { m_hdr = enable; m_constantsDirty = true; m_frameLightsDirty = true; }
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
    void SetVolume(float strength, float haze, float shafts) { m_volume = strength; m_volumeHaze = haze; m_volumeShafts = shafts; }
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
    // Wind for small plants (grass, bushes, flowers): 0 = still.
    void SetSway(float strength) { m_sway = strength; m_constantsDirty = true; m_frameLightsDirty = true; }
    // Grass and plants bending out of the way of characters walking through them (0 = off; scales reach and bend).
    void SetGrassPush(float strength) { m_grassPush = strength; m_frameLightsDirty = true; }
    // Plants' big quads split into pieces for smooth bending (0 = off; 1 = pieces about 0.3 units across).
    void SetPlantDetail(float detail) { m_plantDetail = detail; }
    // Shadow map sizes in pixels: the sun's (each cascade) and the point lights' (each cube face). Takes effect at
    // the next frame (the maps are recreated).
    void SetShadowResolution(uint32_t sun, uint32_t point)
    {
        m_shadowSizeWanted = std::clamp<uint32_t>(sun, 512, 8192);
        m_pointShadowSizeWanted = std::clamp<uint32_t>(point, 128, 2048);
    }
    // Foliage beyond this distance (world units) is shaded more cheaply (0 = off).
    void SetFoliageLod(float distance) { m_foliageLod = distance; }
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
    // Sun shadow penumbra growth with blocker distance (0 = hard), sunlight through leaves, night glow of bright
    // texels on unlit / self-lit surfaces, contact shadows (screen-space, against the sun).
    void SetSunSoftness(float s) { m_sunSoftness = s; m_frameLightsDirty = true; }
    void SetLeafLight(float s) { m_leafLight = s; m_frameLightsDirty = true; }
    void SetNightGlow(float s) { m_nightGlow = s; m_frameLightsDirty = true; m_constantsDirty = true; }
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

    void SetTexture(uint32_t stage, Texture* texture);
    // Null = the main target. Like D3D, resets the viewport to the whole target.
    void SetRenderTarget(Texture* target);
    Texture* GetRenderTarget() const { return m_target == m_main ? nullptr : m_target; }

    void DrawPrimitive(uint32_t primitive, uint32_t fvf, const void* vertices, uint32_t vertexCount);
    void DrawIndexedPrimitive(uint32_t primitive, uint32_t fvf, const void* vertices, uint32_t vertexCount,
                              const uint16_t* indices, uint32_t indexCount);
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
        VkViewport viewport{};
        VkRect2D scissor{};
        bool buffersBound = false;
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
    uint32_t m_anisotropy = 1;
    float m_maxAnisotropy = 1.0f;               // GPU limit (0 without the feature)
    // The ground's base pass textures this frame, by chunk (TerrainChunkKey): its lighting pass, drawn later with the
    // lightmap, takes its relief from them. And the sampler they're read with (repeat, mipmapped).
    std::unordered_map<uint64_t, Texture*> m_terrainBases;
    Texture* m_drawBumpBase = nullptr;           // the current draw's (Draw)
    Texture* m_constantsBumpBase = nullptr;
    VkSampler m_bumpSampler = VK_NULL_HANDLE;
    static uint64_t TerrainChunkKey(const void* vertices, uint32_t vertexCount, uint32_t stride, uint32_t indexCount);
    uint32_t m_dumpVertexCount = 0;
    bool m_drawOverbright2x = false;              // the current draw is blended at 2x (F_OVERBRIGHT2X)
    bool Overbright2x(uint32_t fvf) const;
    uint64_t m_frameNumber = 0;
    bool m_frameLightsDirty = true;
    bool m_frameEyeValid = false;                // the camera the frame's light list is chosen from
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
    static constexpr float kCasterCacheRange = 60.0f; // remembered casters: kept within 3x, forgotten beyond 4x
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
    };
    std::vector<ShadowItem> m_shadowItems;       // EndFrame: what the shadow passes draw
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
    void DrawShadowItem(VkCommandBuffer cmd, ShadowBind& bind, const ShadowItem& item, const d3d::Matrix& lightViewProj);
    void FinishShadowFrame();
    // The cameras casters were drawn with this frame; the shadow map follows the one most casters share (the
    // world's), so 3D interface elements drawn with their own camera neither move the map nor cast into it.
    struct CasterView { d3d::Matrix view, proj; uint32_t count; };
    std::vector<CasterView> m_casterViews;
    d3d::Matrix m_shadowWorldProj{};             // the world camera's projection: only its draws take shadows
    bool WorldCamera() const;
    bool m_drawIsLabel = false;                  // the current draw is a name label (IsLabel)
    bool IsLabel(uint32_t primitive, uint32_t fvf, uint32_t vertexCount) const;
    uint32_t m_midFrameFlushes = 0;
    bool m_shadows = false;
    float m_shadowStrength = 0.65f, m_shadowRange = 400.0f;
    uint32_t m_shadowCascades = kShadowCascades;
    VkImage m_shadowImage = VK_NULL_HANDLE;
    VmaAllocation_T* m_shadowAllocation = nullptr;
    VkImageView m_shadowView = VK_NULL_HANDLE;   // all cascades (sampled)
    VkImageView m_shadowLayerViews[kShadowCascades] = {};   // one cascade each (rendered)
    VkImageLayout m_shadowImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkSampler m_shadowSampler = VK_NULL_HANDLE;
    VkSampler m_shadowDepthSampler = VK_NULL_HANDLE;   // the same map without comparison
    VkDescriptorSetLayout m_shadowSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_shadowPipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_shadowPipelines[2] = {};        // opaque, alpha-tested
    std::vector<ShadowCaster> m_casters;
    float m_sunDir[3] = {}, m_sunLuminance = 0.0f;   // this frame's brightest directional light
    bool m_shadowValid = false;                  // the map holds last frame's shadows
    d3d::Matrix m_cascadeViewProj[kShadowCascades] = {};   // world -> each cascade's map
    float m_cascadeTexel[kShadowCascades] = {};  // world size of a texel of each
    float m_cascadeDepth[kShadowCascades] = {};  // world units per unit of each one's depth
    float m_frameSunDir[3] = {0.0f, -1.0f, 0.0f}, m_frameSunColor[3] = {};   // this frame's sun, shadows or not (0: none)
    uint32_t m_cascadeCount = 0;                 // cascades the map holds
    uint64_t m_cascadeFrame = 0;                 // the far cascades are redrawn on alternate frames
    float m_shadowSunDir[3] = {};
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
    struct CachedCaster {
        VkBuffer buffer;
        VmaAllocation_T* allocation;
        uint32_t primitive, stride, vertexCount, indexCount;
        VkDeviceSize indexOffset;
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
    };
    static constexpr double kSwayStep = 0.05;
    static constexpr size_t kSwaySamples = 80;
    struct CasterStreak { uint64_t lastFrame; uint32_t count; };
    std::unordered_map<uint64_t, CachedCaster> m_casterCache;
    std::unordered_map<uint64_t, CasterStreak> m_casterStreaks;
    std::vector<std::pair<uint64_t, std::pair<VkBuffer, VmaAllocation_T*>>> m_deadBuffers;
    d3d::Matrix m_frameViewProj{};               // the frame's camera, at its first shadow caster
    bool m_frameViewProjValid = false;
    void CacheCaster(uint64_t key, const ShadowCaster& c, const void* vertices, const uint16_t* indices);
    void ShadowCutout(uint32_t fvf, Texture** texture, int* texOffset, float* alphaRef) const;
    uint64_t CasterKey(uint32_t primitive, uint32_t fvf, uint32_t stride, const void* vertices, uint32_t vertexCount,
                       const uint16_t* indices, uint32_t indexCount, float boundsMin[3], float boundsMax[3]) const;
    uint32_t m_cachedCastersDrawn = 0;           // last shadow pass: remembered casters drawn (frame dumps)
    uint32_t m_forgottenInView = 0, m_forgottenFar = 0;   // remembered casters forgotten so far (frame dumps)
    void UpdateCasterCache();
    void ForgetCachedCaster(std::unordered_map<uint64_t, CachedCaster>::iterator it);
    void ForgetCasterTexture(Texture* texture);
    static double SwayClock();
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
    bool m_sceneSaw3D = false;
    uint32_t m_sceneEndDraw = 0, m_sceneEndFvf = 0;   // frame dumps: where the scene phase ended
    VkPipeline m_pipelinesHdr[3] = {};           // m_pipelines for the float target
    VkPipeline m_tessPipelines[2] = {};          // characters' Phong tessellation: 8-bit target, float target
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
    struct MeshInfo { float boundsMin[3], boundsMax[3]; uint64_t indexHash; uint64_t firstFrame, lastFrame; };
    std::unordered_map<uint64_t, MeshInfo> m_meshInfo;
    const MeshInfo* m_drawMesh = nullptr;        // the current draw's (null: external geometry, pre-transformed)
    const skin::Job* m_drawSkin = nullptr;       // the current draw is this skinned character piece (DrawSkinned)
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
    bool SwayParams(uint32_t fvf, uint32_t stride, const void* vertices, uint32_t vertexCount, float out[4]);
    // Plants' big triangles split for smooth bending (draw.cpp SubdividePlant), by mesh cache key and split.
    struct PlantMesh { std::vector<uint8_t> vertices; std::vector<uint16_t> indices; uint64_t lastFrame = 0; };
    std::unordered_map<uint64_t, PlantMesh> m_plantMeshes;
    std::unordered_map<uint64_t, float> m_plantMaxEdge;   // a plant mesh's longest edge (model units)
    float m_plantDetail = 1.0f;
    float m_foliageLod = 35.0f;                  // RVK_FoliageLod (draw.cpp FoliageFar)
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
    void Wind(float out[4]) const;               // direction x, z, time, strength
    double m_windTime = 0.0, m_windTimePrev = 0.0;   // this frame's and last frame's (set at the frame's start)
    bool m_taa = true;
    float m_sharpen = 0.4f;
    float m_taaJitter[2] = {};                   // this frame's jitter, clip units (FrameLights taa)
    Texture* m_taaHistory[2] = {};               // resolved frames (RGBA16F): [m_taaIndex] is the last one
    uint32_t m_taaIndex = 0;
    uint64_t m_taaFrame = ~0ull;                 // frame of the last resolve (history valid if it was the previous)
    VkPipeline m_taaPipeline = VK_NULL_HANDLE, m_sharpenPipeline = VK_NULL_HANDLE;
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
    static bool IsInterfaceDraw(uint32_t fvf);
    static bool IsWater(uint32_t fvf);           // VisualLiquid_t: pre-transformed, with specular (FVF 0x1C4)
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
    // Volumetric light: sun shafts through the shadow cascades and lamp glow (hdr.cpp, volume.frag).
    float m_volume = 1.0f, m_volumeHaze = 1.0f, m_volumeShafts = 1.0f;
    Texture* m_volumeTex[2] = {};                // half resolution: scattered light + view depth (ping-pong)
    VkDescriptorSetLayout m_volumeSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_volumeLayout = VK_NULL_HANDLE;
    VkPipeline m_volumePipeline = VK_NULL_HANDLE, m_volumeBlurPipeline = VK_NULL_HANDLE;
    bool RenderVolume(VkCommandBuffer cmd);
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
    // positions: the mesh's vertex positions (model space) when it has at most kMotionMaxVertices - a CPU-skinned
    // character's limbs move only in them.
    struct MotionEntry { d3d::Matrix world; bool used; std::vector<float> positions; };
    static constexpr uint32_t kMotionMaxVertices = 8192;
    std::unordered_map<uint64_t, std::vector<MotionEntry>> m_motionPrev, m_motionCur;
    bool MotionVectorDraw(uint32_t fvf) const;   // this draw writes motion vectors
    uint64_t MotionKey(uint32_t primitive, uint32_t fvf, uint32_t vertexCount, const uint16_t* indices, uint32_t indexCount) const;
    Texture* m_tonemapped = nullptr;             // with motion blur: the tone mapped scene, blurred into m_ldrMain
    VkPipeline m_motionPipeline = VK_NULL_HANDLE;
    d3d::Matrix m_prevViewProj{};                // the last frame's world camera
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
    static constexpr uint32_t kTonemapInputs = 12;   // occlusion.glsl's bindings
    void TonemapInputsPass(VkCommandBuffer cmd, Texture* dst, VkPipeline pipeline, Texture* scene, bool bloom, bool ao,
                           bool gi, bool volume, const float params[16]);
    void MakeDepthReadable(VkCommandBuffer cmd);
    bool m_aoProjValid = false;
    bool RenderAo(VkCommandBuffer cmd);          // false: no AO this frame
    bool CreateHdrResources(std::string* error);
    void DestroyHdrResources();
    void BeginScene();
    void EndScene();

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
    VkDeviceSize m_constantsOffset = 0;
    uint32_t m_constantsFvf = ~0u;
    uint32_t m_constantsTexMask = ~0u;
    bool m_constantsTerrain = false, m_constantsLabel = false;
    uint32_t m_constantsFoliageLod = 0;          // FoliageFar: 0 near, 1 far foliage, 2 far plant
    bool m_constantsCharacter = false;
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
    static constexpr uint32_t kProfileMarks = 48;
    struct ProfileFrame { VkQueryPool pool = VK_NULL_HANDLE; uint32_t count = 0; const char* names[kProfileMarks] = {}; };
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
    std::atomic<uint64_t> m_profileGameWaitUs{0};
    double m_profileIdleMs = 0.0;
    double m_timerCost = 0.0;                    // ms per clock read (subtracted from the draw sections)
    // Per-draw CPU sections, timed on every 16th frame (Draw); scaled to a per-frame average.
    void ProfileDrawSection(const char* name, double& since);
private:
    uint32_t m_frameIndex = 0;
    bool m_inFrame = false;
    bool m_rendering = false;
    static constexpr VkDeviceSize kRingSize = 64ull << 20;      // each frame's ring to start with
    static constexpr VkDeviceSize kRingMaxSize = 256ull << 20;  // ... at most (a 32-bit process: address space)
    VkDeviceSize m_ringWanted = kRingSize;       // after a mid-frame flush: the next frames' ring size
    VkDeviceSize m_ringPeak = 0;                 // the most a frame used (logged with the flushes)
    uint32_t m_ringFlushesLogged = 0;
    bool CreateRing(Frame& f, VkDeviceSize size, std::string* error);

    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    std::array<VkPipeline, 3> m_pipelines{};   // per topology class: points, lines, triangles
    VkBuffer m_nullBuffer = VK_NULL_HANDLE;    // zeros, bound at stride 0 for attributes a format lacks
    VmaAllocation_T* m_nullAllocation = nullptr;
    Texture* m_blackTexture = nullptr;         // what an unbound stage samples
    std::unordered_map<uint64_t, VkSampler> m_samplers;

    std::string m_screenshotPath;
    std::string m_dumpPath;
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
    std::array<d3d::Matrix, 2> m_texMatrix{};
    d3d::Material m_material{};
    std::vector<LightSlot> m_lights;
    std::array<Texture*, 2> m_textures{};
    d3d::Viewport m_viewport{};
};

}  // namespace rvk
