// rvk::ThreadedDevice: the same API as rvk::Device, executed on a worker thread.
//
// The calling thread only appends compact records to a single-producer / single-consumer byte ring; data
// behind pointers (vertices, indices, pixels, rectangles) is copied into the record. The worker replays the
// records on a private rvk::Device, so Vulkan calls, constant building, driver work, submission and present
// all leave the caller's thread. Getters are answered from mirrors on the caller's side. Calls that must
// return GPU results (ReadPixels) or reconfigure the device (Resize, SetWindow) drain the queue first and
// then run directly. The caller may run at most kMaxFramesAhead frames ahead of the worker.
//
// Single producer: all calls must come from one thread at a time (the rvk backend serialises its COM calls).
#pragma once

#include "rvk.h"
#include "skin.h"

#include <atomic>
#include <memory>
#include <cstdint>
#include <cstring>
#include <new>
#include <utility>

namespace rvk {

class ThreadedDevice {
public:
    ThreadedDevice();
    ~ThreadedDevice();
    ThreadedDevice(const ThreadedDevice&) = delete;
    ThreadedDevice& operator=(const ThreadedDevice&) = delete;

    // threaded = false: records execute immediately on the calling thread (for comparison / fallback).
    bool Init(HWND window, uint32_t width, uint32_t height, std::string* error, bool threaded = true);
    bool Threaded() const { return m_thread != nullptr; }
    bool Resize(uint32_t width, uint32_t height);
    bool SetWindow(HWND window);
    HWND Window() const { return m_window; }
    const DeviceInfo& Info() const { return m_device.Info(); }
    uint32_t Width() const { return m_width; }
    uint32_t Height() const { return m_height; }

    void BeginFrame();
    void EndFrame();
    bool InFrame() const { return m_inFrame; }
    bool ReadPixels(Texture* target, void* out);
    void RequestScreenshot(const std::string& bmpPath);
    void RequestFrameDump(const std::string& path);

    void Clear(uint32_t flags, uint32_t argb, float z) { Clear(0, nullptr, flags, argb, z); }
    void Clear(uint32_t count, const Device::Rect* rects, uint32_t flags, uint32_t argb, float z);
    void SetViewport(const d3d::Viewport& vp);
    const d3d::Viewport& GetViewport() const { return m_viewport; }
    void SetRenderState(uint32_t state, uint32_t value);
    void SetTextureStageState(uint32_t stage, uint32_t type, uint32_t value);
    void SetTransform(uint32_t type, const d3d::Matrix& m);
    void SetMaterial(const d3d::Material& m);
    void SetLight(uint32_t index, const d3d::Light& light);
    void LightEnable(uint32_t index, bool enable);
    void SetPixelLighting(bool enable);
    bool PixelLighting() const { return m_pixelLighting; }
    void SetLightingDebug(bool enable);
    bool LightingDebug() const { return m_lightingDebug; }
    void SetLightOverride(bool enable);
    void SetCarrierLit(bool enable);
    bool LightOverride() const { return m_lightOverride; }
    void SetShadows(bool enable);
    bool Shadows() const { return m_shadows; }
    void SetShadowParams(float strength, float distance, uint32_t cascades);
    void SetPointShadows(uint32_t count);
    uint32_t PointShadows() const { return m_pointShadows; }
    void SetPointShadowStrength(float strength, float dayFactor);
    void SetLightHeadroom(float headroom);
    void SetHdr(bool enable);
    bool Hdr() const { return m_hdr; }
    void SetTonemap(float knee, float exposure);
    void SetHdrHeadroom(float headroom);
    void SetBloom(float strength, float threshold);
    void SetEffectGlow(float gain);
    void SetBloomOverNearer(float keep);
    void SetGrading(float saturation, float contrast, float warmth, float lutAmount, float nightTint, float vignette);
    void SetColorLut(uint32_t slot, uint32_t size, const uint8_t* rgba);   // copied
    void SetSway(float strength);
    void SetGrassPush(float strength);
    void SetPlantDetail(float detail);
    void SetFoliageLod(float distance);
    void SetDepthPrepass(bool on);
    void SetShadowResolution(uint32_t sun, uint32_t point);
    void SetPointLightIntensity(float lights, float characters);
    void SetTessellation(float shape, float distance, uint32_t level);
    void SetTaa(bool enable, float sharpen);
    void ProfileWindow(bool start, const std::string& label);
    void ProfileManualEnd();
    void SetSunSoftness(float s);
    void SetLeafLight(float s);
    void SetNightGlow(float s);
    void SetContactShadows(float s);
    void SetAo(float strength, float radius);
    void SetGi(float strength, float radius);
    void SetVolume(float strength, float haze, float shafts);
    void SetSsr(float strength, float water, float gloss, float wet);
    void SetBump(float strength);
    void SetNormalMaps(bool enable, float strength);
    void SetNormalMap(Texture* texture, Texture* normal);   // the device owns `normal` from here on
    void SetAnisotropy(uint32_t level);
    void SetMotionBlur(float strength, float focusNear);
    void SetMotionBlurMode(uint32_t mode);
    void SetDof(bool enable, bool bokeh, bool nearBlur, float strength, float radius, float focus, float range, bool farBlur,
                float closeFocus);
    uint32_t MotionBlurMode() const { return m_motionMode; }
    float MotionBlur() const { return m_motionBlur; }
    uint32_t Anisotropy() const { return m_anisotropy; }
    float Bump() const { return m_bump; }
    float AoStrength() const { return m_aoStrength; }
    float EffectGlow() const { return m_effectGlow; }
    float BloomStrength() const { return m_bloomStrength; }
    float HdrHeadroom() const { return m_hdrHeadroom; }
    void SetDumpVertexCount(uint32_t count);
    void SetParticleParams(const Device::ParticleParams& params);
    const Device::ParticleParams& GetParticleParams() const { return m_particleParams; }
    void ParticleEmitter(uint64_t key, const float center[3], const float origin[3], const Device::ParticleSprite* sprites,
                         uint32_t count);
    void EndParticleEmitter();
    void SetTexture(uint32_t stage, Texture* texture);
    void SetDrawVisual(uint32_t kind, const char* className, uint32_t owner = 0);   // className: stays valid (RTTI)
    void SetSceneLights(const Device::SceneLight* lights, uint32_t count);   // copied
    void SetRenderTarget(Texture* target);
    Texture* GetRenderTarget() const { return m_target; }

    void DrawPrimitive(uint32_t primitive, uint32_t fvf, const void* vertices, uint32_t vertexCount);
    void DrawIndexedPrimitive(uint32_t primitive, uint32_t fvf, const void* vertices, uint32_t vertexCount,
                              const uint16_t* indices, uint32_t indexCount);
    // Vertex buffers live in CPU memory; draws copy the range they use into the record, so the caller may
    // rewrite a buffer right after drawing from it.
    void DrawPrimitiveVB(uint32_t primitive, VertexBuffer* vb, uint32_t startVertex, uint32_t vertexCount);
    // Draws from a shared snapshot of a static vertex buffer (the caller's copy of its data, made once after it last
    // changed): no copy of the vertices per draw - the record holds a reference, released by the worker.
    using SharedVertices = std::shared_ptr<const std::vector<uint8_t>>;
    void DrawPrimitiveShared(uint32_t primitive, uint32_t fvf, const SharedVertices& data, size_t byteOffset,
                             uint32_t vertexCount);
    void DrawIndexedPrimitiveShared(uint32_t primitive, uint32_t fvf, const SharedVertices& data, size_t byteOffset,
                                    uint32_t vertexCount, const uint16_t* indices, uint32_t indexCount);
    void DrawIndexedPrimitiveVB(uint32_t primitive, VertexBuffer* vb, uint32_t startVertex, uint32_t vertexCount,
                                const uint16_t* indices, uint32_t indexCount);
    // Draws a character piece the worker skins first (once per job, however often it is drawn): no vertices copied.
    using SkinJob = std::shared_ptr<skin::Job>;
    void DrawPrimitiveSkinned(uint32_t primitive, uint32_t fvf, const SkinJob& job, uint32_t startVertex,
                              uint32_t vertexCount);
    void DrawIndexedPrimitiveSkinned(uint32_t primitive, uint32_t fvf, const SkinJob& job, uint32_t startVertex,
                                     uint32_t vertexCount, const uint16_t* indices, uint32_t indexCount);
    VertexBuffer* CreateVertexBuffer(uint32_t fvf, uint32_t vertexCount) { return m_device.CreateVertexBuffer(fvf, vertexCount); }
    void* Lock(VertexBuffer* vb) { return m_device.Lock(vb); }
    void Unlock(VertexBuffer* vb) { m_device.Unlock(vb); }
    void DestroyVertexBuffer(VertexBuffer* vb) { m_device.DestroyVertexBuffer(vb); }

    Texture* CreateTexture(uint32_t width, uint32_t height, Format format, uint32_t levels);
    Texture* CreateTexture(uint32_t width, uint32_t height, const void* argbPixels);
    Texture* CreateRenderTarget(uint32_t width, uint32_t height);
    void UpdateTexture(Texture* texture, uint32_t level, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                       const void* data, uint32_t pitch);
    void DestroyTexture(Texture* texture);
    void CopyTexture(Texture* dst, const Device::Rect* dstRect, Texture* src, const Device::Rect* srcRect, bool linear);

    static uint32_t FvfStride(uint32_t fvf) { return Device::FvfStride(fvf); }

    void Sync();                                       // waits until the worker has executed everything queued

    static constexpr uint32_t kMaxFramesAhead = 1;
    static constexpr uint32_t kQueueBytes = 48u << 20;

private:
    // A record: header + payload. The payload is a callable constructed in place, followed by copied data.
    struct Header {
        uint32_t size;                                 // whole record incl. header, multiple of 16; 0 = wrap marker
        void (*run)(void* payload);
    };

    // Reserves `bytes` of payload, waiting for the worker if the ring is full. Returns the payload pointer.
    uint8_t* Reserve(uint32_t bytes);
    void Commit();
    // Committed records reach the worker in batches: publishing one (the counter, the fenced store of m_writePos, the
    // cache line the worker polls) cost more than building a small record, and a frame queues tens of thousands.
    // Published every kPublishBatch records and before anything that waits for the worker.
    void Publish();
    static constexpr uint32_t kPublishBatch = 32;

    template <typename F>
    void Enqueue(F&& f, const void* data = nullptr, uint32_t dataBytes = 0, void** dataCopy = nullptr);

    void Worker();
    void RunOne(uint32_t& readPos);                    // executes the record at readPos (worker or direct mode)
    static DWORD WINAPI WorkerMain(void* self);

    Device m_device;
    HANDLE m_thread = nullptr;
    std::atomic<bool> m_stop{false};

    uint8_t* m_ring = nullptr;
    alignas(64) std::atomic<uint32_t> m_writePos{0};   // producer: committed end
    uint32_t m_reserveStart = 0, m_reserveSize = 0;    // producer: record being built
    uint32_t m_localWrite = 0;                         // producer: end of the committed records, published or not
    uint32_t m_pending = 0;                            // producer: committed records not published yet
    uint64_t m_frameRecords = 0, m_frameBytes = 0;     // producer: this frame's records and bytes (profiling)
    alignas(64) std::atomic<uint32_t> m_readPos{0};    // consumer: next record
    alignas(64) std::atomic<uint32_t> m_workerSleeping{0};
    std::atomic<uint64_t> m_framesQueued{0}, m_framesDone{0};
    std::atomic<uint64_t> m_recordsQueued{0}, m_recordsDone{0};
    uint64_t m_callerNs = 0;                           // calling thread's time inside Enqueue, this frame (its thread only)
    uint32_t m_callerSample = 0;                       // ... sampled 1 in 64 calls, so timing doesn't cost the hot path

    // Caller-side mirrors for getters.
    HWND m_window = nullptr;
    uint32_t m_width = 0, m_height = 0;
    bool m_inFrame = false;
    d3d::Viewport m_viewport{};
    Texture* m_target = nullptr;
    bool m_pixelLighting = false, m_lightingDebug = false, m_lightOverride = false, m_shadows = false;
    uint32_t m_pointShadows = 0;
    bool m_hdr = false;
    float m_hdrHeadroom = 1.5f;
    float m_bloomStrength = 1.5f, m_effectGlow = 1.0f, m_aoStrength = 1.0f, m_bump = 0.0f;
    uint32_t m_anisotropy = 1;
    float m_motionBlur = 0.0f;
    uint32_t m_motionMode = 0;
    Device::ParticleParams m_particleParams;
};

}  // namespace rvk
