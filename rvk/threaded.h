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
#include <chrono>
#include <memory>
#include <cstdint>
#include <cstring>
#include <new>
#include <vector>
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
    // Several state changes as one record (the native DeviceState's update: docs/device-on-rvk.md phase 1), applied
    // in order. Repeats of what was last sent are dropped, as by the single setters.
    struct StateItem {
        enum Kind : uint32_t { RenderState, StageState, Texture } kind;
        uint32_t stage, type, value;             // RenderState: type = state; Texture: stage
        rvk::Texture* texture;
    };
    void SetStates(const StateItem* items, uint32_t count);
    // Game-thread time in a named part of the frame (native scene code; `name` a string literal): summed per frame
    // and handed to the profile with the frame's start.
    void AddGameSection(const char* name, double ms);
    // The renderer drops blob shadow draws (sun shadows on and valid), as of a frame or two ago.
    bool BlobShadowsReplaced() const { return m_device.m_blobShadowsReplaced.load(std::memory_order_relaxed); }
    void SetTransform(uint32_t type, const d3d::Matrix& m);
    void SetMaterial(const d3d::Material& m);
    void SetLight(uint32_t index, const d3d::Light& light);
    void LightEnable(uint32_t index, bool enable);
    void SetPixelLighting(bool enable);
    bool PixelLighting() const { return m_pixelLighting; }
    void SetLightingDebug(bool enable);
    bool LightingDebug() const { return m_lightingDebug; }
    void SetHideInterface(bool hide);
    bool HideInterface() const { return m_hideInterface; }
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
    void SetGrassField(bool on, float distance, float density, float height, bool texOnly);
    void SetGrassWidth(float width);
    void SetGrassBrightness(float brightness);
    void SetGrassStyle(float variety, float flowers, float glow, float gusts, bool trails);
    void SetGrassEven(float even);
    void SetGrassShadows(bool on);
    void SetDepthPrepass(bool on);
    void SetFoliageEdges(bool on);
    // The interface layer (Device::InterfaceBegin / InterfaceEnd).
    void InterfaceBegin(bool redraw);
    void InterfaceEnd();
    bool InterfaceLayerReady() const { return m_device.InterfaceLayerReady(); }
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
    void SetMaterialMaps(Texture* texture, Texture* orm, Texture* albedo);   // ... and these
    void SetEmissiveMap(Texture* texture, Texture* emissive);               // ... and this
    void RequestPick(float x, float y);
    Device::PickResult LastPick() const { return m_device.LastPick(); }   // (thread-safe)
    void SetPickHighlight(Texture* t);
    void SetPbr(const Device::PbrSettings& s);
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
    // The water (Device::DrawWater): vertices and indices copied.
    void SetWaterParams(const Device::WaterParams& params);
    void DrawWater(const Device::WaterVertex* vertices, uint32_t vertexCount, const uint16_t* indices, uint32_t indexCount);
    bool WaterReady() const { return m_device.WaterReady(); }
    void SetTexture(uint32_t stage, Texture* texture);
    void SetDrawVisual(uint32_t kind, const char* className, uint32_t owner = 0);   // className: stays valid (RTTI)
    void SetSceneLights(const Device::SceneLight* lights, uint32_t count);   // copied
    void SetRenderTarget(Texture* target);
    Texture* GetRenderTarget() const { return m_target; }

    // fromBuffer: the vertices are a vertex buffer's written lately (else the caller's memory) - for the copied draws'
    // log line.
    void DrawPrimitive(uint32_t primitive, uint32_t fvf, const void* vertices, uint32_t vertexCount,
                       bool fromBuffer = false);
    void DrawIndexedPrimitive(uint32_t primitive, uint32_t fvf, const void* vertices, uint32_t vertexCount,
                              const uint16_t* indices, uint32_t indexCount, bool fromBuffer = false);
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
    // ... with the indices a snapshot as well (unchanged while the caller keeps it): no copy of them in the record,
    // and the renderer keeps them on the GPU (Device::DrawSharedIndexed).
    void DrawIndexedPrimitiveSharedRetained(uint32_t primitive, uint32_t fvf, const SharedVertices& data,
                                            size_t byteOffset, uint32_t vertexCount, const SharedVertices& indexData,
                                            uint32_t indexCount);
    // Retained meshes (Device::RegisterMesh / DrawMesh): registered once (a record with the snapshots), then each draw
    // a record with the id alone - no reference counts touched per draw on either thread.
    void RegisterMesh(uint32_t id, uint32_t primitive, uint32_t fvf, const SharedVertices& vertices, size_t byteOffset,
                      uint32_t vertexCount, const SharedVertices& indices, uint32_t indexCount);
    void ReleaseMesh(uint32_t id);
    void DrawMesh(uint32_t id);
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
    void Enqueue(F&& f, const void* data = nullptr, uint32_t dataBytes = 0, void** dataCopy = nullptr,
                 const char* who = __builtin_FUNCTION());
    // Records by the function that made them (profiling: which calls aren't folded into draws), logged every ~600
    // frames by BeginFrame.
    struct RecordKind { const char* who; uint64_t count; };
    std::vector<RecordKind> m_recordKinds;
    uint64_t m_recordKindFrames = 0;
    // The draws that copy their vertices into the record, by the visual drawing them (its class) and where the
    // vertices come from, over the last kCopySampleFrames frames of each ~600: how many and how big, and how many
    // drew exactly the vertices a draw drew the frame before (could be kept on the GPU instead).
    static constexpr uint64_t kCopySampleFrames = 8;
    struct CopyKind { const char* name; uint32_t kind; bool fromBuffer; uint64_t draws, bytes, same, sameBytes; };
    std::vector<CopyKind> m_copyKinds;
    std::vector<uint64_t> m_copyHashes[2];             // this sample frame's / the one before's (sorted) content hashes
    const char* m_curVisualName = nullptr;
    uint64_t m_statesIn = 0, m_statesKept = 0;         // SetStates' items over the ~600 frames, and those not repeats
    uint32_t m_curVisualKind = 0;
    void CountCopy(const void* vertices, uint32_t vbytes, const void* indices, uint32_t ibytes, bool fromBuffer);
    void LogCopies();
    void CountRecord(const char* who)
    {
        for (RecordKind& k : m_recordKinds)
            if (k.who == who) { ++k.count; return; }
        m_recordKinds.push_back({who, 1});
    }

    // One record per draw (docs/device-on-rvk.md phase 4a): render / texture stage states, textures, the world
    // matrix and the material wait here and go with the next draw, in its record (EnqueueDraw), instead of a record
    // each. Any other record, Sync and the frame's end send them first (FlushPending), so the device sees the same
    // sequence. Device applies states, lights, the world matrix, the material and the drawn visual independently of
    // each other, so only the states' own order and the lights' own order matter.
    // RANDYVK_COALESCE=0: off (a record each, as before).
    struct LightOp {                                   // SetLight (enable unused) or LightEnable (light unused)
        uint32_t enableOnly, index, enable, pad;
        d3d::Light light;
    };
    struct PendingHead {                               // a draw record's prefix: this, `states` StateItems, `lights` LightOps
        uint32_t states, lights, hasWorld, hasMaterial, hasVisual, visualKind, visualOwner;
        const char* visualName;
        d3d::Matrix world;
        d3d::Material material;
    };
    std::vector<LightOp> m_pendingLights;
    bool m_hasPendingVisual = false;
    uint32_t m_pendingVisualKind = 0, m_pendingVisualOwner = 0;
    const char* m_pendingVisualName = nullptr;
    bool m_coalesce = true;
    bool m_hasPendingWorld = false, m_hasPendingMaterial = false;
    d3d::Matrix m_pendingWorld{};
    d3d::Material m_pendingMaterial{};
    std::vector<StateItem> m_pendingStates;
    bool HasPending() const
    {
        return m_hasPendingWorld || m_hasPendingMaterial || m_hasPendingVisual || !m_pendingStates.empty() ||
               !m_pendingLights.empty();
    }
    void AddPendingState(const StateItem& item);
    uint32_t PendingBytes() const;
    void WritePending(uint8_t* out);                   // PendingBytes() bytes; clears what is pending
    void ApplyPending(const uint8_t* in);              // worker: the prefix written by WritePending
    void FlushPending();                               // a record of its own (before a record that isn't a draw)
    void ApplyStates(const StateItem* items, uint32_t count);   // worker
    // A draw's record: the pending prefix (if any), then `a` and `b` (copied, contiguous); f(data) gets their copy.
    template <typename F>
    void EnqueueDraw(F&& f, const void* a = nullptr, uint32_t aBytes = 0, const void* b = nullptr, uint32_t bBytes = 0,
                     const char* who = __builtin_FUNCTION());

    void Worker();
    bool RunOne(uint32_t& readPos);                    // executes the record at readPos (worker or direct mode)
    static DWORD WINAPI WorkerMain(void* self);

    Device m_device;
    HANDLE m_thread = nullptr;
    std::atomic<bool> m_stop{false};

    uint8_t* m_ring = nullptr;
    alignas(64) std::atomic<uint32_t> m_writePos{0};   // producer: committed end
    uint32_t m_reserveStart = 0, m_reserveSize = 0;    // producer: record being built
    uint32_t m_localWrite = 0;                         // producer: end of the committed records, published or not
    uint32_t m_pending = 0;                            // producer: committed records not published yet
    uint32_t m_cachedRead = 0;                         // producer: m_readPos as last read (Reserve)
    uint64_t m_frameRecords = 0, m_frameBytes = 0;     // producer: this frame's records and bytes (profiling)
    uint64_t m_frameRepeats = 0;                       // producer: this frame's state calls dropped as repeats
    // What the device was last sent, per state: a call repeating it is dropped before it becomes a record (the game
    // re-sets most of its state around every draw; Device would ignore the repeat, but only after the record was
    // built, queued and run). Only values this producer sent count (valid flags), so the device's own defaults never
    // match by accident. Not for lights (Device collects the frame's lights from every SetLight / LightEnable) or
    // textures (a destroyed texture's address can come back as a new one). The viewport is forgotten whenever the
    // device resets it (SetRenderTarget, Resize).
    bool KeepRenderState(uint32_t state, uint32_t value);              // false: a repeat (counted)
    bool KeepStageState(uint32_t stage, uint32_t type, uint32_t value);
    std::vector<StateItem> m_stateScratch;             // SetStates: the items kept
    std::vector<std::pair<const char*, double>> m_gameSections;   // this frame's AddGameSection sums
    // This frame's draws by how their data crossed the hand-off (the profile's 'hand-off draws' line).
    struct DrawStats { uint64_t copied = 0, vertexBytes = 0, shared = 0, skinned = 0, indexBytes = 0, retained = 0, handles = 0; } m_drawStats;
    std::chrono::steady_clock::time_point m_lastEndFrame{};       // the game thread's frame (EndFrame to EndFrame)
    struct SentState {
        uint32_t rs[256];
        bool rsValid[256];
        uint32_t tss[8][32];
        bool tssValid[8][32];
        d3d::Matrix transform[32];
        bool transformValid[32];
        d3d::Material material;
        bool materialValid;
        d3d::Viewport viewport;
        bool viewportValid;
        // Lights: forgotten at every frame's start - the device captures the frame's light set from the frame's
        // first SetLight / LightEnable of each light (Device::CaptureLight), so those always go through.
        static constexpr uint32_t kLights = 16;
        d3d::Light light[kLights];
        bool lightValid[kLights];
        bool enabled[kLights];
        bool enabledValid[kLights];
    };
    std::unique_ptr<SentState> m_sent = std::make_unique<SentState>();   // value-initialised: nothing valid
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
    bool m_hideInterface = false;
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
