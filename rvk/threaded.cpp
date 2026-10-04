#include "threaded.h"

#include <chrono>

#include <cstdio>
#include <malloc.h>

namespace rvk {

namespace {

constexpr uint32_t Align16(uint32_t v) { return (v + 15) & ~15u; }

template <typename T>
void WaitWhileEqual(std::atomic<T>& value, T expected, DWORD timeoutMs)
{
    // WaitOnAddress is a futex under Wine: cheap, and returns at once if the value already changed.
    WaitOnAddress(reinterpret_cast<volatile void*>(&value), &expected, sizeof(T), timeoutMs);
}

}  // namespace

ThreadedDevice::ThreadedDevice() = default;

ThreadedDevice::~ThreadedDevice()
{
    if (m_thread) {
        Sync();
        m_stop.store(true);
        m_writePos.fetch_add(0);                       // make the wake below see a "change"
        WakeByAddressAll(&m_writePos);
        WaitForSingleObject(m_thread, INFINITE);
        CloseHandle(m_thread);
    }
    _aligned_free(m_ring);
}

bool ThreadedDevice::Init(HWND window, uint32_t width, uint32_t height, std::string* error, bool threaded)
{
    if (!m_device.Init(window, width, height, error))
        return false;
    m_window = window;
    m_width = width;
    m_height = height;
    m_viewport = {0, 0, width, height, 0.0f, 1.0f};
    m_ring = static_cast<uint8_t*>(_aligned_malloc(kQueueBytes, 64));
    if (!threaded) {
        Log("worker thread disabled: rendering on the calling thread");
        return true;
    }
    m_thread = CreateThread(nullptr, 0, WorkerMain, this, 0, nullptr);
    if (!m_thread) {
        if (error) *error = "could not start the rvk worker thread";
        return false;
    }
    SetThreadDescription(m_thread, L"rvk-worker");
    return true;
}

// ---------------------------------------------------------------------------------------------------
// Queue

uint8_t* ThreadedDevice::Reserve(uint32_t bytes)
{
    uint32_t total = Align16(sizeof(Header) + bytes);
    if (total >= kQueueBytes / 2) {
        Log("record of %u bytes is too large for the queue", bytes);
        total = kQueueBytes / 2 - 16;                  // the caller's data is truncated; better than a hang
    }
    uint32_t w = m_writePos.load(std::memory_order_relaxed);
    for (int spins = 0;; ++spins) {
        uint32_t r = m_readPos.load(std::memory_order_acquire);
        if (w >= r) {
            // Free: [w, end) and [0, r). Never let w catch up with r (that would look empty).
            if (total < kQueueBytes - w || (total == kQueueBytes - w && r != 0)) {
                m_reserveStart = w;
                break;
            }
            if (total < r) {                           // wrap: mark the tail as skipped, start at 0
                reinterpret_cast<Header*>(m_ring + w)->size = 0;
                m_reserveStart = 0;
                break;
            }
        } else if (total < r - w) {
            m_reserveStart = w;
            break;
        }
        // Full: wait for the worker to consume.
        if (spins < 64)
            YieldProcessor();
        else
            WaitWhileEqual(m_readPos, r, 1);
    }
    m_reserveSize = total;
    auto* h = reinterpret_cast<Header*>(m_ring + m_reserveStart);
    h->size = total;
    return reinterpret_cast<uint8_t*>(h + 1);
}

void ThreadedDevice::Commit()
{
    uint32_t end = m_reserveStart + m_reserveSize;
    if (end == kQueueBytes)
        end = 0;
    m_recordsQueued.fetch_add(1, std::memory_order_relaxed);
    m_writePos.store(end, std::memory_order_seq_cst);
    if (!m_thread) {                                   // direct mode: the caller is the consumer
        uint32_t r = m_readPos.load(std::memory_order_relaxed);
        while (r != end)
            RunOne(r);
        return;
    }
    if (m_workerSleeping.load(std::memory_order_seq_cst))
        WakeByAddressSingle(&m_writePos);
}

template <typename F>
void ThreadedDevice::Enqueue(F&& f, const void* data, uint32_t dataBytes, void** dataCopy)
{
    using Fn = std::decay_t<F>;
    constexpr uint32_t kFnBytes = Align16(sizeof(Fn));
    uint8_t* payload = Reserve(kFnBytes + dataBytes);
    if (dataBytes) {
        std::memcpy(payload + kFnBytes, data, dataBytes);
        if (dataCopy) *dataCopy = payload + kFnBytes;
    }
    new (payload) Fn(std::forward<F>(f));
    reinterpret_cast<Header*>(payload - sizeof(Header))->run = [](void* p) {
        Fn* fn = static_cast<Fn*>(p);
        (*fn)(static_cast<uint8_t*>(p) + kFnBytes);
        fn->~Fn();
    };
    Commit();
}

DWORD WINAPI ThreadedDevice::WorkerMain(void* self)
{
    static_cast<ThreadedDevice*>(self)->Worker();
    return 0;
}

void ThreadedDevice::Worker()
{
    uint32_t r = m_readPos.load();
    for (;;) {
        uint32_t w = m_writePos.load(std::memory_order_acquire);
        if (r == w) {
            if (m_stop.load())
                return;
            auto idleStart = std::chrono::steady_clock::now();   // profiling: the render thread waiting for work
            // Idle: spin briefly (the next record usually follows quickly), then sleep until woken.
            bool woke = false;
            for (int i = 0; i < 4000 && !woke; ++i) {
                YieldProcessor();
                woke = m_writePos.load(std::memory_order_acquire) != r;
            }
            if (!woke) {
                m_workerSleeping.store(1, std::memory_order_seq_cst);
                if (m_writePos.load(std::memory_order_seq_cst) == r && !m_stop.load())
                    WaitWhileEqual(m_writePos, r, INFINITE);
                m_workerSleeping.store(0, std::memory_order_relaxed);
            }
            m_device.ProfileAddIdle(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - idleStart).count());
            continue;
        }
        RunOne(r);
    }
}

void ThreadedDevice::RunOne(uint32_t& r)
{
    auto* h = reinterpret_cast<Header*>(m_ring + r);
    if (h->size == 0) {                                // wrap marker
        r = 0;
        return;
    }
    h->run(h + 1);
    r += h->size;
    if (r == kQueueBytes)
        r = 0;
    m_readPos.store(r, std::memory_order_release);
    m_recordsDone.fetch_add(1, std::memory_order_release);
}

void ThreadedDevice::Sync()
{
    uint64_t target = m_recordsQueued.load();
    while (m_recordsDone.load(std::memory_order_acquire) < target) {
        uint64_t done = m_recordsDone.load();
        WaitWhileEqual(m_recordsDone, done, 1);
    }
}

// ---------------------------------------------------------------------------------------------------
// Frames and synchronous calls

void ThreadedDevice::BeginFrame()
{
    m_inFrame = true;
    Enqueue([this](const uint8_t*) { m_device.BeginFrame(); });
}

void ThreadedDevice::EndFrame()
{
    m_inFrame = false;
    Enqueue([this](const uint8_t*) {
        m_device.EndFrame();
        m_framesDone.fetch_add(1, std::memory_order_release);
        WakeByAddressAll(&m_framesDone);
    });
    // Run at most kMaxFramesAhead frames ahead of the GPU-facing thread.
    uint64_t queued = m_framesQueued.fetch_add(1) + 1;
    auto waitStart = std::chrono::steady_clock::now();     // profiling: the game waiting for the render thread
    for (;;) {
        uint64_t done = m_framesDone.load(std::memory_order_acquire);
        if (queued - done <= kMaxFramesAhead)
            break;
        WaitWhileEqual(m_framesDone, done, 100);
    }
    m_device.m_profileGameWaitUs.fetch_add(uint64_t(
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - waitStart).count()));
}

bool ThreadedDevice::ReadPixels(Texture* target, void* out)
{
    Sync();                                            // the worker is idle until the next record
    return m_device.ReadPixels(target, out);
}

bool ThreadedDevice::Resize(uint32_t width, uint32_t height)
{
    Sync();
    if (!m_device.Resize(width, height))
        return false;
    m_width = width;
    m_height = height;
    m_viewport = {0, 0, width, height, 0.0f, 1.0f};
    m_target = nullptr;
    return true;
}

bool ThreadedDevice::SetWindow(HWND window)
{
    Sync();
    bool ok = m_device.SetWindow(window);
    if (ok)
        m_window = window;
    return ok;
}

void ThreadedDevice::RequestFrameDump(const std::string& path)
{
    std::string p = path;
    Enqueue([this, p](const uint8_t*) { m_device.RequestFrameDump(p); });
}

void ThreadedDevice::RequestScreenshot(const std::string& bmpPath)
{
    std::string path = bmpPath;
    Enqueue([this, path](const uint8_t*) { m_device.RequestScreenshot(path); });
}

// ---------------------------------------------------------------------------------------------------
// State

void ThreadedDevice::Clear(uint32_t count, const Device::Rect* rects, uint32_t flags, uint32_t argb, float z)
{
    Enqueue([this, count, flags, argb, z](const uint8_t* data) {
        m_device.Clear(count, count ? reinterpret_cast<const Device::Rect*>(data) : nullptr, flags, argb, z);
    }, rects, count ? count * uint32_t(sizeof(Device::Rect)) : 0);
}

void ThreadedDevice::SetViewport(const d3d::Viewport& vp)
{
    m_viewport = vp;
    Enqueue([this, vp](const uint8_t*) { m_device.SetViewport(vp); });
}

void ThreadedDevice::SetRenderState(uint32_t state, uint32_t value)
{
    Enqueue([this, state, value](const uint8_t*) { m_device.SetRenderState(state, value); });
}

void ThreadedDevice::SetTextureStageState(uint32_t stage, uint32_t type, uint32_t value)
{
    Enqueue([this, stage, type, value](const uint8_t*) { m_device.SetTextureStageState(stage, type, value); });
}

void ThreadedDevice::SetTransform(uint32_t type, const d3d::Matrix& m)
{
    Enqueue([this, type, m](const uint8_t*) { m_device.SetTransform(type, m); });
}

void ThreadedDevice::SetMaterial(const d3d::Material& m)
{
    Enqueue([this, m](const uint8_t*) { m_device.SetMaterial(m); });
}

void ThreadedDevice::SetLight(uint32_t index, const d3d::Light& light)
{
    Enqueue([this, index, light](const uint8_t*) { m_device.SetLight(index, light); });
}

void ThreadedDevice::LightEnable(uint32_t index, bool enable)
{
    Enqueue([this, index, enable](const uint8_t*) { m_device.LightEnable(index, enable); });
}

void ThreadedDevice::SetPixelLighting(bool enable)
{
    m_pixelLighting = enable;
    Enqueue([this, enable](const uint8_t*) { m_device.SetPixelLighting(enable); });
}

void ThreadedDevice::SetLightingDebug(bool enable)
{
    m_lightingDebug = enable;
    Enqueue([this, enable](const uint8_t*) { m_device.SetLightingDebug(enable); });
}

void ThreadedDevice::SetLightOverride(bool enable)
{
    m_lightOverride = enable;
    Enqueue([this, enable](const uint8_t*) { m_device.SetLightOverride(enable); });
}

void ThreadedDevice::SetCarrierLit(bool enable)
{
    Enqueue([this, enable](const uint8_t*) { m_device.SetCarrierLit(enable); });
}

void ThreadedDevice::SetShadows(bool enable)
{
    m_shadows = enable;
    Enqueue([this, enable](const uint8_t*) { m_device.SetShadows(enable); });
}

void ThreadedDevice::SetShadowParams(float strength, float distance, uint32_t cascades)
{
    Enqueue([this, strength, distance, cascades](const uint8_t*) { m_device.SetShadowParams(strength, distance, cascades); });
}

void ThreadedDevice::SetPointShadows(uint32_t count)
{
    m_pointShadows = count < Device::kMaxPointShadows ? count : Device::kMaxPointShadows;
    Enqueue([this, count](const uint8_t*) { m_device.SetPointShadows(count); });
}

void ThreadedDevice::SetPointShadowStrength(float strength, float dayFactor)
{
    Enqueue([this, strength, dayFactor](const uint8_t*) { m_device.SetPointShadowStrength(strength, dayFactor); });
}

void ThreadedDevice::SetDumpVertexCount(uint32_t count)
{
    Enqueue([this, count](const uint8_t*) { m_device.SetDumpVertexCount(count); });
}

void ThreadedDevice::SetHdr(bool enable)
{
    m_hdr = enable;
    Enqueue([this, enable](const uint8_t*) { m_device.SetHdr(enable); });
}

void ThreadedDevice::SetBloom(float strength, float threshold)
{
    m_bloomStrength = strength;
    Enqueue([this, strength, threshold](const uint8_t*) { m_device.SetBloom(strength, threshold); });
}

void ThreadedDevice::SetMotionBlur(float strength, float focusNear)
{
    m_motionBlur = strength;
    Enqueue([this, strength, focusNear](const uint8_t*) { m_device.SetMotionBlur(strength, focusNear); });
}

void ThreadedDevice::SetDof(bool enable, bool bokeh, bool nearBlur, float strength, float radius, float focus, float range,
                            bool farBlur, float closeFocus)
{
    Enqueue([=](const uint8_t*) { m_device.SetDof(enable, bokeh, nearBlur, strength, radius, focus, range, farBlur, closeFocus); });
}

void ThreadedDevice::SetMotionBlurMode(uint32_t mode)
{
    m_motionMode = mode;
    Enqueue([this, mode](const uint8_t*) { m_device.SetMotionBlurMode(mode); });
}

void ThreadedDevice::SetAnisotropy(uint32_t level)
{
    m_anisotropy = level < 1 ? 1 : level;
    Enqueue([this, level](const uint8_t*) { m_device.SetAnisotropy(level); });
}

void ThreadedDevice::SetBump(float strength)
{
    m_bump = strength < 0.0f ? 0.0f : strength;
    float s = m_bump;
    Enqueue([this, s](const uint8_t*) { m_device.SetBump(s); });
}

void ThreadedDevice::SetNormalMaps(bool enable, float strength)
{
    Enqueue([this, enable, strength](const uint8_t*) { m_device.SetNormalMaps(enable, strength); });
}

void ThreadedDevice::SetNormalMap(Texture* texture, Texture* normal)
{
    Enqueue([this, texture, normal](const uint8_t*) { m_device.SetNormalMap(texture, normal); });
}

void ThreadedDevice::SetAo(float strength, float radius)
{
    m_aoStrength = strength;
    Enqueue([this, strength, radius](const uint8_t*) { m_device.SetAo(strength, radius); });
}

void ThreadedDevice::SetGi(float strength, float radius)
{
    Enqueue([this, strength, radius](const uint8_t*) { m_device.SetGi(strength, radius); });
}

void ThreadedDevice::SetVolume(float strength, float haze, float shafts)
{
    Enqueue([this, strength, haze, shafts](const uint8_t*) { m_device.SetVolume(strength, haze, shafts); });
}

void ThreadedDevice::SetSsr(float strength, float water, float gloss, float wet)
{
    Enqueue([this, strength, water, gloss, wet](const uint8_t*) { m_device.SetSsr(strength, water, gloss, wet); });
}

void ThreadedDevice::SetSunSoftness(float s)
{
    Enqueue([this, s](const uint8_t*) { m_device.SetSunSoftness(s); });
}

void ThreadedDevice::SetLeafLight(float s)
{
    Enqueue([this, s](const uint8_t*) { m_device.SetLeafLight(s); });
}

void ThreadedDevice::SetNightGlow(float s)
{
    Enqueue([this, s](const uint8_t*) { m_device.SetNightGlow(s); });
}

void ThreadedDevice::SetContactShadows(float s)
{
    Enqueue([this, s](const uint8_t*) { m_device.SetContactShadows(s); });
}

void ThreadedDevice::SetGrading(float saturation, float contrast, float warmth, float lutAmount, float nightTint,
                                float vignette)
{
    Enqueue([=, this](const uint8_t*) { m_device.SetGrading(saturation, contrast, warmth, lutAmount, nightTint, vignette); });
}

void ThreadedDevice::SetColorLut(uint32_t slot, uint32_t size, const uint8_t* rgba)
{
    std::vector<uint8_t> data;
    if (size && rgba) data.assign(rgba, rgba + size_t(size) * size * size * 4);
    Enqueue([this, slot, size, data = std::move(data)](const uint8_t*) {
        m_device.SetColorLut(slot, data.empty() ? 0 : size, data.empty() ? nullptr : data.data());
    });
}

void ThreadedDevice::ProfileWindow(bool start, const std::string& label)
{
    Enqueue([this, start, label](const uint8_t*) { m_device.ProfileWindow(start, label.c_str()); });
}

void ThreadedDevice::ProfileManualEnd()
{
    Enqueue([this](const uint8_t*) { m_device.ProfileManualEnd(); });
}

void ThreadedDevice::SetTaa(bool enable, float sharpen)
{
    Enqueue([this, enable, sharpen](const uint8_t*) { m_device.SetTaa(enable, sharpen); });
}

void ThreadedDevice::SetSway(float strength)
{
    Enqueue([this, strength](const uint8_t*) { m_device.SetSway(strength); });
}

void ThreadedDevice::SetGrassPush(float strength)
{
    Enqueue([this, strength](const uint8_t*) { m_device.SetGrassPush(strength); });
}

void ThreadedDevice::SetPlantDetail(float detail)
{
    Enqueue([this, detail](const uint8_t*) { m_device.SetPlantDetail(detail); });
}

void ThreadedDevice::SetFoliageLod(float distance)
{
    Enqueue([this, distance](const uint8_t*) { m_device.SetFoliageLod(distance); });
}

void ThreadedDevice::SetShadowResolution(uint32_t sun, uint32_t point)
{
    Enqueue([this, sun, point](const uint8_t*) { m_device.SetShadowResolution(sun, point); });
}

void ThreadedDevice::SetPointLightIntensity(float lights, float characters)
{
    Enqueue([this, lights, characters](const uint8_t*) { m_device.SetPointLightIntensity(lights, characters); });
}

void ThreadedDevice::SetTessellation(float shape, float distance, uint32_t level)
{
    Enqueue([this, shape, distance, level](const uint8_t*) { m_device.SetTessellation(shape, distance, level); });
}

void ThreadedDevice::SetBloomOverNearer(float keep)
{
    Enqueue([this, keep](const uint8_t*) { m_device.SetBloomOverNearer(keep); });
}

void ThreadedDevice::SetEffectGlow(float gain)
{
    m_effectGlow = gain < 0.0f ? 0.0f : gain;
    Enqueue([this, gain](const uint8_t*) { m_device.SetEffectGlow(gain < 0.0f ? 0.0f : gain); });
}

void ThreadedDevice::SetHdrHeadroom(float headroom)
{
    m_hdrHeadroom = headroom < 1.0f ? 1.0f : headroom;
    Enqueue([this, headroom](const uint8_t*) { m_device.SetHdrHeadroom(headroom); });
}

void ThreadedDevice::SetTonemap(float knee, float exposure)
{
    Enqueue([this, knee, exposure](const uint8_t*) { m_device.SetTonemap(knee, exposure); });
}

void ThreadedDevice::SetLightHeadroom(float headroom)
{
    Enqueue([this, headroom](const uint8_t*) { m_device.SetLightHeadroom(headroom); });
}

void ThreadedDevice::SetTexture(uint32_t stage, Texture* texture)
{
    Enqueue([this, stage, texture](const uint8_t*) { m_device.SetTexture(stage, texture); });
}

void ThreadedDevice::SetRenderTarget(Texture* target)
{
    // Mirror Device semantics: a non-render-target is ignored; the viewport resets to the target.
    if (target && !target->IsRenderTarget())
        return;
    m_target = target;
    m_viewport = {0, 0, target ? target->Width() : m_width, target ? target->Height() : m_height, 0.0f, 1.0f};
    Enqueue([this, target](const uint8_t*) { m_device.SetRenderTarget(target); });
}

void ThreadedDevice::SetParticleParams(const Device::ParticleParams& params)
{
    m_particleParams = params;
    Enqueue([this, params](const uint8_t*) { m_device.SetParticleParams(params); });
}

void ThreadedDevice::ParticleEmitter(uint64_t key, const float center[3], const float origin[3],
                                     const Device::ParticleSprite* sprites, uint32_t count)
{
    float c[3] = {center[0], center[1], center[2]}, o[3] = {origin[0], origin[1], origin[2]};
    Enqueue([this, key, c, o, count](const uint8_t* data) {
        m_device.ParticleEmitter(key, c, o, reinterpret_cast<const Device::ParticleSprite*>(data), count);
    }, sprites, uint32_t(count * sizeof(Device::ParticleSprite)));
}

void ThreadedDevice::EndParticleEmitter()
{
    Enqueue([this](const uint8_t*) { m_device.EndParticleEmitter(); });
}

// ---------------------------------------------------------------------------------------------------
// Drawing

void ThreadedDevice::DrawPrimitive(uint32_t primitive, uint32_t fvf, const void* vertices, uint32_t vertexCount)
{
    uint32_t bytes = FvfStride(fvf) * vertexCount;
    Enqueue([this, primitive, fvf, vertexCount](const uint8_t* data) {
        m_device.DrawPrimitive(primitive, fvf, data, vertexCount);
    }, vertices, bytes);
}

void ThreadedDevice::DrawIndexedPrimitive(uint32_t primitive, uint32_t fvf, const void* vertices, uint32_t vertexCount,
                                          const uint16_t* indices, uint32_t indexCount)
{
    // One record holds both arrays: vertices, then indices.
    uint32_t vbytes = FvfStride(fvf) * vertexCount, ibytes = indexCount * 2;
    auto run = [this, primitive, fvf, vertexCount, indexCount, vbytes](const uint8_t* data) {
        m_device.DrawIndexedPrimitive(primitive, fvf, data, vertexCount, reinterpret_cast<const uint16_t*>(data + vbytes),
                                      indexCount);
    };
    using Fn = decltype(run);
    constexpr uint32_t kFnBytes = Align16(sizeof(Fn));
    uint8_t* payload = Reserve(kFnBytes + vbytes + ibytes);
    std::memcpy(payload + kFnBytes, vertices, vbytes);
    std::memcpy(payload + kFnBytes + vbytes, indices, ibytes);
    new (payload) Fn(run);
    reinterpret_cast<Header*>(payload - sizeof(Header))->run = [](void* p) {
        Fn* fn = static_cast<Fn*>(p);
        (*fn)(static_cast<uint8_t*>(p) + kFnBytes);
        fn->~Fn();
    };
    Commit();
}

void ThreadedDevice::DrawPrimitiveShared(uint32_t primitive, uint32_t fvf, const SharedVertices& data, size_t byteOffset,
                                         uint32_t vertexCount)
{
    Enqueue([this, primitive, fvf, vertexCount, data, byteOffset](const uint8_t*) {
        m_device.DrawPrimitive(primitive, fvf, data->data() + byteOffset, vertexCount);
    });
}

void ThreadedDevice::DrawIndexedPrimitiveShared(uint32_t primitive, uint32_t fvf, const SharedVertices& data,
                                                size_t byteOffset, uint32_t vertexCount, const uint16_t* indices,
                                                uint32_t indexCount)
{
    Enqueue([this, primitive, fvf, vertexCount, indexCount, data, byteOffset](const uint8_t* idx) {
        m_device.DrawIndexedPrimitive(primitive, fvf, data->data() + byteOffset, vertexCount,
                                      reinterpret_cast<const uint16_t*>(idx), indexCount);
    }, indices, indexCount * 2);
}

void ThreadedDevice::DrawPrimitiveSkinned(uint32_t primitive, uint32_t fvf, const SkinJob& job, uint32_t startVertex,
                                          uint32_t vertexCount)
{
    if (!job || size_t(startVertex) + vertexCount > job->source->vertices.size())
        return;
    Enqueue([this, primitive, fvf, job, startVertex, vertexCount](const uint8_t*) {
        m_device.DrawSkinned(primitive, fvf, *job, startVertex, vertexCount, nullptr, 0);
    });
}

void ThreadedDevice::DrawIndexedPrimitiveSkinned(uint32_t primitive, uint32_t fvf, const SkinJob& job,
                                                 uint32_t startVertex, uint32_t vertexCount, const uint16_t* indices,
                                                 uint32_t indexCount)
{
    if (!job || size_t(startVertex) + vertexCount > job->source->vertices.size())
        return;
    // The piece's own triangles (as almost always): the job's copy of them, no copy per draw.
    const skin::Source& source = *job->source;
    if (indices == source.gameIndices && indexCount <= source.indices.size()) {
        Enqueue([this, primitive, fvf, job, startVertex, vertexCount, indexCount](const uint8_t*) {
            m_device.DrawSkinned(primitive, fvf, *job, startVertex, vertexCount, job->source->indices.data(),
                                 indexCount);
        });
        return;
    }
    Enqueue([this, primitive, fvf, job, startVertex, vertexCount, indexCount](const uint8_t* idx) {
        m_device.DrawSkinned(primitive, fvf, *job, startVertex, vertexCount, reinterpret_cast<const uint16_t*>(idx),
                             indexCount);
    }, indices, indexCount * 2);
}

void ThreadedDevice::DrawPrimitiveVB(uint32_t primitive, VertexBuffer* vb, uint32_t startVertex, uint32_t vertexCount)
{
    if (!vb || startVertex + vertexCount > vb->m_count)
        return;
    DrawPrimitive(primitive, vb->m_fvf, vb->m_data.data() + size_t(startVertex) * vb->m_stride, vertexCount);
}

void ThreadedDevice::DrawIndexedPrimitiveVB(uint32_t primitive, VertexBuffer* vb, uint32_t startVertex, uint32_t vertexCount,
                                            const uint16_t* indices, uint32_t indexCount)
{
    if (!vb || startVertex + vertexCount > vb->m_count)
        return;
    DrawIndexedPrimitive(primitive, vb->m_fvf, vb->m_data.data() + size_t(startVertex) * vb->m_stride, vertexCount, indices,
                         indexCount);
}

// ---------------------------------------------------------------------------------------------------
// Resources

Texture* ThreadedDevice::CreateTexture(uint32_t width, uint32_t height, Format format, uint32_t levels)
{
    if (!m_device.FormatSupported(format)) {
        Log("texture format %u not supported by the GPU", uint32_t(format));
        return nullptr;
    }
    Texture* t = Device::NewTexture(width, height, format, levels, false);
    if (t)
        Enqueue([this, t](const uint8_t*) { m_device.RealizeTexture(t); });
    return t;
}

Texture* ThreadedDevice::CreateTexture(uint32_t width, uint32_t height, const void* argbPixels)
{
    Texture* t = CreateTexture(width, height, Format::A8R8G8B8, 1);
    if (t)
        UpdateTexture(t, 0, 0, 0, width, height, argbPixels, width * 4);
    return t;
}

Texture* ThreadedDevice::CreateRenderTarget(uint32_t width, uint32_t height)
{
    Texture* t = Device::NewTexture(width, height, Format::A8R8G8B8, 1, true);
    if (t)
        Enqueue([this, t](const uint8_t*) { m_device.RealizeTexture(t); });
    return t;
}

void ThreadedDevice::UpdateTexture(Texture* t, uint32_t level, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                                   const void* data, uint32_t pitch)
{
    if (!t || !width || !height)
        return;
    // Copy tightly packed rows into the record (the caller may reuse its buffer right away).
    uint32_t rowBytes = FormatRowBytes(t->GetFormat(), width), rows = FormatRows(t->GetFormat(), height);
    auto run = [this, t, level, x, y, width, height, rowBytes](const uint8_t* p) {
        m_device.UpdateTexture(t, level, x, y, width, height, p, rowBytes);
    };
    using Fn = decltype(run);
    constexpr uint32_t kFnBytes = Align16(sizeof(Fn));
    uint8_t* payload = Reserve(kFnBytes + rowBytes * rows);
    for (uint32_t r = 0; r < rows; ++r)
        std::memcpy(payload + kFnBytes + size_t(r) * rowBytes, static_cast<const uint8_t*>(data) + size_t(r) * pitch, rowBytes);
    new (payload) Fn(run);
    reinterpret_cast<Header*>(payload - sizeof(Header))->run = [](void* p) {
        Fn* fn = static_cast<Fn*>(p);
        (*fn)(static_cast<uint8_t*>(p) + kFnBytes);
        fn->~Fn();
    };
    Commit();
}

void ThreadedDevice::DestroyTexture(Texture* t)
{
    if (!t)
        return;
    if (m_target == t)                                 // Device falls back to the main target
        SetRenderTarget(nullptr);
    Enqueue([this, t](const uint8_t*) { m_device.DestroyTexture(t); });
}

void ThreadedDevice::CopyTexture(Texture* dst, const Device::Rect* dstRect, Texture* src, const Device::Rect* srcRect,
                                 bool linear)
{
    Device::Rect rects[2] = {dstRect ? *dstRect : Device::Rect{}, srcRect ? *srcRect : Device::Rect{}};
    bool hasDst = dstRect != nullptr, hasSrc = srcRect != nullptr;
    Enqueue([this, dst, src, linear, hasDst, hasSrc](const uint8_t* data) {
        auto* r = reinterpret_cast<const Device::Rect*>(data);
        m_device.CopyTexture(dst, hasDst ? &r[0] : nullptr, src, hasSrc ? &r[1] : nullptr, linear);
    }, rects, sizeof(rects));
}

}  // namespace rvk
