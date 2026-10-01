// rvk: a Vulkan renderer with a Direct3D 7 shaped front end (the part of D3D7 Anarchy Online uses).
//
// Usage per frame: BeginFrame(), state and draw calls, EndFrame(). Rendering goes into the main colour
// target (or one set with SetRenderTarget) plus a shared depth buffer; EndFrame() copies the main target
// to the window's swapchain (if a window was given) and can save it as a screenshot.
#pragma once

#include "d3d7.h"
#include "vk.h"

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct VmaAllocator_T;
struct VmaAllocation_T;

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
    bool LightOverride() const { return m_lightOverride; }
    // Enhancement: sun shadows (shadow.cpp). strength = how much of the light a shadow takes away (0..1);
    // range = half the width of the shadowed square around the camera, in world units.
    void SetShadows(bool enable) { if (m_shadows != enable) { m_shadows = enable; m_constantsDirty = true; } }
    bool Shadows() const { return m_shadows; }
    void SetShadowParams(float strength, float range) { m_shadowStrength = strength; m_shadowRange = range; }
    // Enhancement, with the light override: shadows from up to `count` of the frame's point lights nearest the camera
    // (cube shadow maps, pointshadow.cpp; 0 = off). strength = how much of such a light a shadow takes away (0..1).
    void SetPointShadows(uint32_t count) { m_pointShadows = count < kMaxPointShadows ? count : kMaxPointShadows; }
    uint32_t PointShadows() const { return m_pointShadows; }
    // dayFactor: the strength left in full daylight. Under a bright sun the scene is already at full brightness
    // around a light, so its shadows only show where the sun's are (they'd look cut off there); they fade instead.
    void SetPointShadowFadeIn(double seconds) { m_pointShadowFadeIn = seconds; }
    void SetPointShadowStrength(float strength, float dayFactor) { m_pointShadowStrength = strength; m_pointShadowDay = dayFactor; }
    static constexpr uint32_t kMaxPointShadows = 8;
    // Enhancement: the 3D scene is drawn into a 16-bit float target (colours above 1 kept) and tone mapped into the
    // 8-bit main target when the interface starts drawing (hdr.cpp). knee: colours up to it are shown unchanged,
    // brighter ones roll off towards white (1 = only clip, keeping the hue). exposure scales the scene first.
    void SetHdr(bool enable) { m_hdr = enable; m_constantsDirty = true; m_frameLightsDirty = true; }
    bool Hdr() const { return m_hdr; }
    void SetTonemap(float knee, float exposure) { m_tonemapKnee = knee; m_exposure = exposure; }
    // With HDR: glow around light above the threshold (hdr.cpp). strength 0 = off.
    void SetBloom(float strength, float threshold) { m_bloomStrength = strength; m_bloomThreshold = threshold; }
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
    void SetTexture(uint32_t stage, Texture* texture);
    // Null = the main target. Like D3D, resets the viewport to the whole target.
    void SetRenderTarget(Texture* target);
    Texture* GetRenderTarget() const { return m_target == m_main ? nullptr : m_target; }

    void DrawPrimitive(uint32_t primitive, uint32_t fvf, const void* vertices, uint32_t vertexCount);
    void DrawIndexedPrimitive(uint32_t primitive, uint32_t fvf, const void* vertices, uint32_t vertexCount,
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
        VkDeviceSize ringOffset = 0;
        uint64_t serial = 0;                       // submission number last signalled through `fence`
        bool uploadsRecorded = false;
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
    float m_lightHeadroom = 1.0f;
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
    // The frame lights in binding-4 order, as indices into m_lightsPrev (WriteFrameLights), and which of them the
    // current draw carries.
    std::vector<uint32_t> m_frameLightIndices;
    uint32_t CarriedLight(uint32_t fvf, const void* vertices, uint32_t vertexCount, uint32_t stride) const;
    struct CapturedLight {
        d3d::Light light;
        float cosHalfTheta, cosHalfPhi;
        bool hasCarrier = false;                 // a character carries it (FindCarriers): its origin
        float carrier[3] = {};
    };
    // End of frame: for each light, the character carrying it - the body origin nearest under it. The game places a
    // character's light up to ~0.4 sideways off the body while it moves, so a fixed radius around the light either
    // misses the carrier or catches characters next to it.
    void FindCarriers();
    static bool IsCarrierPart(const CapturedLight& c, const d3d::Matrix& world, float extent);
    std::vector<CapturedLight> m_lightsCur, m_lightsPrev;   // point / spot lights used this / last frame
    void CaptureLight(LightSlot& slot);

    // Sun shadows (shadow.cpp)
    static constexpr uint32_t kShadowSize = 4096;
    struct ShadowCaster {
        uint32_t primitive, stride, vertexCount, indexCount;
        VkDeviceSize vbOffset, ibOffset;
        d3d::Matrix world;
        uint64_t generation;
        Texture* texture;                        // alpha-tested casters: texture 0, its coordinates' offset, ref
        int texOffset;
        float alphaRef;
        uint64_t key;                            // caster cache identity
        uint32_t view;                           // index into m_casterViews
        float boundsMin[3], boundsMax[3];        // model space
    };
    // One caster as the shadow passes draw it: from this frame's ring or from the caster cache (own buffer).
    struct ShadowItem {
        VkBuffer buffer;                         // VK_NULL_HANDLE: the frame's ring
        VkDeviceSize vbOffset, ibOffset;         // ring: byte offsets; cached: index data offset in `buffer`
        uint32_t primitive, stride, vertexCount, indexCount;
        Texture* texture;
        int texOffset;
        float alphaRef;
        d3d::Matrix world;
        float boundsMin[3], boundsMax[3];        // world space
        bool cached;                             // remembered, not drawn by the game this frame
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
    float m_shadowStrength = 0.65f, m_shadowRange = 60.0f;
    VkImage m_shadowImage = VK_NULL_HANDLE;
    VmaAllocation_T* m_shadowAllocation = nullptr;
    VkImageView m_shadowView = VK_NULL_HANDLE;
    VkImageLayout m_shadowImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkSampler m_shadowSampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_shadowSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_shadowPipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_shadowPipelines[2] = {};        // opaque, alpha-tested
    std::vector<ShadowCaster> m_casters;
    float m_sunDir[3] = {}, m_sunLuminance = 0.0f;   // this frame's brightest directional light
    bool m_shadowValid = false;                  // the map holds last frame's shadows
    d3d::Matrix m_shadowViewProj{};
    float m_shadowSunDir[3] = {};
    bool CreateShadowResources(std::string* error);
    void DestroyShadowResources();
    void PrepareShadowMap(VkCommandBuffer cmd);
    void RecordShadowCaster(uint32_t primitive, uint32_t fvf, uint32_t stride, const void* vertices, uint32_t vertexCount,
                            VkDeviceSize vbOffset, const uint16_t* indices, uint32_t indexCount, VkDeviceSize ibOffset);
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
    VkDescriptorSetLayout m_tonemapSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_tonemapLayout = VK_NULL_HANDLE;
    VkPipeline m_tonemapPipeline = VK_NULL_HANDLE;
    VkSampler m_pointSampler = VK_NULL_HANDLE, m_linearSampler = VK_NULL_HANDLE;
    float m_bloomStrength = 1.5f, m_bloomThreshold = 1.0f;
    std::vector<Texture*> m_bloomLevels;         // half resolution and down, float
    VkDescriptorSetLayout m_bloomSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_bloomLayout = VK_NULL_HANDLE;
    VkPipeline m_bloomDown = VK_NULL_HANDLE, m_bloomUp = VK_NULL_HANDLE;
    void RenderBloom(VkCommandBuffer cmd);
    void FullscreenPass(VkCommandBuffer cmd, Texture* dst, VkPipeline pipeline, VkPipelineLayout layout, Texture* src,
                        const float params[4], bool load);
    bool CreateHdrResources(std::string* error);
    void DestroyHdrResources();
    void BeginScene();
    void EndScene();

    // Point light shadows (pointshadow.cpp): a cube map per shadowed light, layers 6*i .. 6*i+5 of one cube array.
    static constexpr uint32_t kPointShadowSize = 1024;
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
    uint64_t m_ringGeneration = 0, m_constantsGeneration = ~0ull;
    VkDeviceSize m_constantsOffset = 0;
    uint32_t m_constantsFvf = ~0u;
    uint32_t m_constantsTexMask = ~0u;
    bool m_constantsTerrain = false, m_constantsLabel = false;
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
    uint32_t m_frameIndex = 0;
    bool m_inFrame = false;
    bool m_rendering = false;
    static constexpr VkDeviceSize kRingSize = 64ull << 20;

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
