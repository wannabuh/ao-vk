#include "threaded.h"

#include <algorithm>
#include <chrono>
#include <iterator>

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
    char coalesce[8] = "";
    if (GetEnvironmentVariableA("RANDYVK_COALESCE", coalesce, sizeof(coalesce)) && coalesce[0] == '0') {
        m_coalesce = false;
        Log("state changes: a record each (RANDYVK_COALESCE=0)");
    }
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
    uint32_t w = m_localWrite;                         // after the unpublished records too
    // The worker's read position as last seen: free space only grows as it reads, so an old copy is safe, and the
    // worker rewrites m_readPos after every record - reading it each time cost a cache miss per record.
    uint32_t r = m_cachedRead;
    for (int spins = 0;; ++spins) {
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
        if (spins > 0) {
            // Full: wait for the worker to consume (it can only consume what is published).
            if (spins == 1)
                Publish();
            if (spins < 64)
                YieldProcessor();
            else
                WaitWhileEqual(m_readPos, r, 1);
        }
        r = m_cachedRead = m_readPos.load(std::memory_order_acquire);   // (the first time: the copy was too old)
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
    m_localWrite = end;
    ++m_pending;
    ++m_frameRecords;
    m_frameBytes += m_reserveSize;
    if (!m_thread) {                                   // direct mode: the caller is the consumer
        Publish();
        uint32_t r = m_readPos.load(std::memory_order_relaxed);
        uint64_t ran = 0;
        while (r != end)
            ran += RunOne(r);
        m_readPos.store(r, std::memory_order_release);
        m_recordsDone.fetch_add(ran, std::memory_order_release);
        return;
    }
    if (m_pending >= kPublishBatch)
        Publish();
}

void ThreadedDevice::Publish()
{
    if (!m_pending)
        return;
    m_recordsQueued.fetch_add(m_pending, std::memory_order_relaxed);
    m_pending = 0;
    m_writePos.store(m_localWrite, std::memory_order_seq_cst);
    if (m_thread && m_workerSleeping.load(std::memory_order_seq_cst))
        WakeByAddressSingle(&m_writePos);
}

template <typename F>
void ThreadedDevice::Enqueue(F&& f, const void* data, uint32_t dataBytes, void** dataCopy, const char* who)
{
    if (HasPending())
        FlushPending();                                // what waited for a draw goes first
    CountRecord(who);
    using Fn = std::decay_t<F>;
    constexpr uint32_t kFnBytes = Align16(sizeof(Fn));
    // Sampled: two clock reads per call on the hottest path would cost more than the calls themselves.
    bool timed = (m_callerSample++ & 63u) == 0;
    std::chrono::steady_clock::time_point callStart;
    if (timed) callStart = std::chrono::steady_clock::now();
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
    if (timed)
        m_callerNs += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now() - callStart).count()) * 64;
}

template <typename F>
void ThreadedDevice::EnqueueDraw(F&& f, const void* a, uint32_t aBytes, const void* b, uint32_t bBytes, const char* who)
{
    CountRecord(who);
    using Fn = std::decay_t<F>;
    struct Wrapped {
        Fn fn;
        ThreadedDevice* self;
        uint32_t prefixBytes;                          // the pending states / world ahead of the draw's data
    };
    constexpr uint32_t kFnBytes = Align16(sizeof(Wrapped));
    bool timed = (m_callerSample++ & 63u) == 0;        // as Enqueue
    std::chrono::steady_clock::time_point callStart;
    if (timed) callStart = std::chrono::steady_clock::now();
    const uint32_t prefixBytes = HasPending() ? PendingBytes() : 0;
    uint8_t* payload = Reserve(kFnBytes + prefixBytes + aBytes + bBytes);
    uint8_t* data = payload + kFnBytes;
    if (prefixBytes)
        WritePending(data);
    if (aBytes) std::memcpy(data + prefixBytes, a, aBytes);
    if (bBytes) std::memcpy(data + prefixBytes + aBytes, b, bBytes);
    new (payload) Wrapped{Fn(std::forward<F>(f)), this, prefixBytes};
    reinterpret_cast<Header*>(payload - sizeof(Header))->run = [](void* p) {
        Wrapped* w = static_cast<Wrapped*>(p);
        uint8_t* d = static_cast<uint8_t*>(p) + kFnBytes;
        if (w->prefixBytes)
            w->self->ApplyPending(d);
        w->fn(d + w->prefixBytes);
        w->~Wrapped();
    };
    Commit();
    if (timed)
        m_callerNs += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now() - callStart).count()) * 64;
}

DWORD WINAPI ThreadedDevice::WorkerMain(void* self)
{
    static_cast<ThreadedDevice*>(self)->Worker();
    return 0;
}

void ThreadedDevice::Worker()
{
    uint32_t r = m_readPos.load();
    // Progress (read position, records done) is published every kProgressBatch records and whenever the worker
    // catches up - not after every record: two atomic writes per record, ~9000 records a frame.
    constexpr uint32_t kProgressBatch = 64;
    uint32_t ran = 0;
    auto progress = [&] {
        m_readPos.store(r, std::memory_order_release);
        m_recordsDone.fetch_add(ran, std::memory_order_release);
        ran = 0;
    };
    for (;;) {
        uint32_t w = m_writePos.load(std::memory_order_acquire);
        if (r == w) {
            if (ran) {                                 // caught up: publish, then look again before idling
                progress();
                continue;
            }
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
        ran += RunOne(r);
        if (ran >= kProgressBatch)
            progress();
    }
}

// Runs the record at r and advances r past it; false for a wrap marker (not a record). The caller publishes the
// progress (m_readPos, m_recordsDone).
bool ThreadedDevice::RunOne(uint32_t& r)
{
    auto* h = reinterpret_cast<Header*>(m_ring + r);
    if (h->size == 0) {                                // wrap marker
        r = 0;
        return false;
    }
    h->run(h + 1);
    r += h->size;
    if (r == kQueueBytes)
        r = 0;
    return true;
}

void ThreadedDevice::Sync()
{
    if (HasPending())
        FlushPending();
    Publish();
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
    // The previous frame's time spent on this (the game) thread inside the renderer's call path.
    uint64_t callerNs = m_callerNs, records = m_frameRecords, bytes = m_frameBytes, repeats = m_frameRepeats;
    m_callerNs = 0;
    m_frameRecords = m_frameBytes = m_frameRepeats = 0;
    std::fill(std::begin(m_sent->lightValid), std::end(m_sent->lightValid), false);   // see SentState
    std::fill(std::begin(m_sent->enabledValid), std::end(m_sent->enabledValid), false);
    // The previous frame's game-thread sections (at most a handful), as a fixed array in the record.
    struct Sections { std::pair<const char*, double> s[8]; uint32_t n = 0; } sections;
    for (const auto& g : m_gameSections)
        if (sections.n < 8) sections.s[sections.n++] = g;
    m_gameSections.clear();
    if (++m_recordKindFrames >= 600) {                 // records a frame by origin, the most first
        std::sort(m_recordKinds.begin(), m_recordKinds.end(),
                  [](const RecordKind& a, const RecordKind& b) { return a.count > b.count; });
        std::string line;
        char buf[96];
        for (size_t i = 0; i < m_recordKinds.size() && i < 10; ++i) {
            std::snprintf(buf, sizeof(buf), "%s%s %.0f", i ? " | " : "", m_recordKinds[i].who,
                          double(m_recordKinds[i].count) / double(m_recordKindFrames));
            line += buf;
        }
        Log("hand-off records a frame by origin: %s", line.c_str());
        m_recordKinds.clear();
        m_recordKindFrames = 0;
    }
    DrawStats draws = m_drawStats;
    m_drawStats = {};
    Enqueue([this, callerNs, records, bytes, repeats, sections, draws](const uint8_t*) {
        m_device.ProfileAddCaller(double(callerNs) * 1e-6);
        m_device.ProfileAddCallerQueue(records, bytes, repeats);
        m_device.ProfileAddCallerDraws(draws.copied, draws.vertexBytes, draws.shared, draws.skinned, draws.indexBytes,
                                       draws.retained);
        m_device.BeginFrame();
        for (uint32_t i = 0; i < sections.n; ++i)   // after BeginFrame: its log (if due) has gone out
            m_device.ProfileCpuAddMs(sections.s[i].first, sections.s[i].second);
    });
}

void ThreadedDevice::AddGameSection(const char* name, double ms)
{
    for (auto& g : m_gameSections)
        if (g.first == name) { g.second += ms; return; }
    m_gameSections.push_back({name, ms});
}

void ThreadedDevice::EndFrame()
{
    m_inFrame = false;
    auto now = std::chrono::steady_clock::now();     // profiling: the game thread's whole frame
    if (m_lastEndFrame.time_since_epoch().count())
        AddGameSection("game thread frame", std::chrono::duration<double, std::milli>(now - m_lastEndFrame).count());
    m_lastEndFrame = now;
    Enqueue([this](const uint8_t*) {
        m_device.EndFrame();
        m_framesDone.fetch_add(1, std::memory_order_release);
        WakeByAddressAll(&m_framesDone);
    });
    Publish();                                         // the frame's last records: the worker may finish it
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
    m_sent->viewportValid = false;                     // the device resets its viewport
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

// The state setters drop a call that repeats what the device was last sent (SentState).
void ThreadedDevice::SetViewport(const d3d::Viewport& vp)
{
    m_viewport = vp;
    SentState& s = *m_sent;
    if (s.viewportValid && std::memcmp(&s.viewport, &vp, sizeof(vp)) == 0) {
        ++m_frameRepeats;
        return;
    }
    s.viewport = vp;
    s.viewportValid = true;
    Enqueue([this, vp](const uint8_t*) { m_device.SetViewport(vp); });
}

bool ThreadedDevice::KeepRenderState(uint32_t state, uint32_t value)
{
    if (state < 256) {
        SentState& s = *m_sent;
        if (s.rsValid[state] && s.rs[state] == value) {
            ++m_frameRepeats;
            return false;
        }
        s.rs[state] = value;
        s.rsValid[state] = true;
    }
    return true;
}

bool ThreadedDevice::KeepStageState(uint32_t stage, uint32_t type, uint32_t value)
{
    if (stage < 8 && type < 32) {
        SentState& s = *m_sent;
        if (type == d3d::TSS_ADDRESS) {
            // Sets U and V too (Device always applies it): they now hold the value.
            s.tss[stage][d3d::TSS_ADDRESSU] = s.tss[stage][d3d::TSS_ADDRESSV] = value;
            s.tssValid[stage][d3d::TSS_ADDRESSU] = s.tssValid[stage][d3d::TSS_ADDRESSV] = true;
        } else if (s.tssValid[stage][type] && s.tss[stage][type] == value) {
            ++m_frameRepeats;
            return false;
        }
        s.tss[stage][type] = value;
        s.tssValid[stage][type] = true;
    }
    return true;
}

void ThreadedDevice::SetRenderState(uint32_t state, uint32_t value)
{
    if (!KeepRenderState(state, value))
        return;
    if (m_coalesce)
        AddPendingState({StateItem::RenderState, 0, state, value, nullptr});
    else
        Enqueue([this, state, value](const uint8_t*) { m_device.SetRenderState(state, value); });
}

void ThreadedDevice::SetTextureStageState(uint32_t stage, uint32_t type, uint32_t value)
{
    if (!KeepStageState(stage, type, value))
        return;
    if (m_coalesce)
        AddPendingState({StateItem::StageState, stage, type, value, nullptr});
    else
        Enqueue([this, stage, type, value](const uint8_t*) { m_device.SetTextureStageState(stage, type, value); });
}

void ThreadedDevice::SetStates(const StateItem* items, uint32_t count)
{
    std::vector<StateItem>& kept = m_stateScratch;
    kept.clear();
    for (uint32_t i = 0; i < count; ++i) {
        const StateItem& it = items[i];
        if (it.kind == StateItem::RenderState ? KeepRenderState(it.type, it.value)
            : it.kind == StateItem::StageState ? KeepStageState(it.stage, it.type, it.value)
                                               : true)   // textures: not filtered (as SetTexture)
            kept.push_back(it);
    }
    if (kept.empty())
        return;
    if (m_coalesce) {                                  // with the next draw
        for (const StateItem& it : kept)
            AddPendingState(it);
        return;
    }
    uint32_t n = uint32_t(kept.size());
    Enqueue([this, n](const uint8_t* data) { ApplyStates(reinterpret_cast<const StateItem*>(data), n); },
            kept.data(), n * uint32_t(sizeof(StateItem)));
}

void ThreadedDevice::ApplyStates(const StateItem* list, uint32_t n)
{
    for (uint32_t i = 0; i < n; ++i) {
        const StateItem& it = list[i];
        switch (it.kind) {
        case StateItem::RenderState: m_device.SetRenderState(it.type, it.value); break;
        case StateItem::StageState: m_device.SetTextureStageState(it.stage, it.type, it.value); break;
        case StateItem::Texture: m_device.SetTexture(it.stage, it.texture); break;
        }
    }
}

void ThreadedDevice::AddPendingState(const StateItem& item)
{
    m_pendingStates.push_back(item);
    if (m_pendingStates.size() >= 4096)                // (a long run without a draw: don't grow without end)
        FlushPending();
}

uint32_t ThreadedDevice::PendingBytes() const
{
    return Align16(uint32_t(sizeof(PendingHead) + m_pendingStates.size() * sizeof(StateItem) +
                            m_pendingLights.size() * sizeof(LightOp)));
}

void ThreadedDevice::WritePending(uint8_t* out)
{
    PendingHead head{};
    head.states = uint32_t(m_pendingStates.size());
    head.hasWorld = m_hasPendingWorld ? 1u : 0u;
    head.world = m_pendingWorld;
    head.hasMaterial = m_hasPendingMaterial ? 1u : 0u;
    head.material = m_pendingMaterial;
    head.lights = uint32_t(m_pendingLights.size());
    head.hasVisual = m_hasPendingVisual ? 1u : 0u;
    head.visualKind = m_pendingVisualKind;
    head.visualOwner = m_pendingVisualOwner;
    head.visualName = m_pendingVisualName;
    std::memcpy(out, &head, sizeof(head));
    out += sizeof(head);
    if (head.states)
        std::memcpy(out, m_pendingStates.data(), m_pendingStates.size() * sizeof(StateItem));
    out += m_pendingStates.size() * sizeof(StateItem);
    if (head.lights)
        std::memcpy(out, m_pendingLights.data(), m_pendingLights.size() * sizeof(LightOp));
    m_pendingStates.clear();
    m_pendingLights.clear();
    m_hasPendingWorld = m_hasPendingMaterial = m_hasPendingVisual = false;
}

void ThreadedDevice::ApplyPending(const uint8_t* in)
{
    PendingHead head;
    std::memcpy(&head, in, sizeof(head));
    ApplyStates(reinterpret_cast<const StateItem*>(in + sizeof(head)), head.states);
    const uint8_t* lights = in + sizeof(head) + size_t(head.states) * sizeof(StateItem);
    for (uint32_t i = 0; i < head.lights; ++i) {
        LightOp op;
        std::memcpy(&op, lights + size_t(i) * sizeof(LightOp), sizeof(op));
        if (op.enableOnly)
            m_device.LightEnable(op.index, op.enable != 0);
        else
            m_device.SetLight(op.index, op.light);
    }
    if (head.hasVisual)
        m_device.SetDrawVisual(head.visualKind, head.visualName, head.visualOwner);
    if (head.hasWorld)
        m_device.SetTransform(d3d::World, head.world);
    if (head.hasMaterial)
        m_device.SetMaterial(head.material);
}

// What is pending as a record of its own: a draw record that draws nothing (EnqueueDraw writes the prefix).
void ThreadedDevice::FlushPending() { EnqueueDraw([](const uint8_t*) {}, nullptr, 0, nullptr, 0, "(states before a non-draw)"); }

void ThreadedDevice::SetTransform(uint32_t type, const d3d::Matrix& m)
{
    if (type < 32) {
        SentState& s = *m_sent;
        if (s.transformValid[type] && std::memcmp(&s.transform[type], &m, sizeof(m)) == 0) {
            ++m_frameRepeats;
            return;
        }
        s.transform[type] = m;
        s.transformValid[type] = true;
    }
    if (m_coalesce && type == d3d::World) {            // with the next draw
        m_pendingWorld = m;
        m_hasPendingWorld = true;
        return;
    }
    Enqueue([this, type, m](const uint8_t*) { m_device.SetTransform(type, m); });
}

void ThreadedDevice::SetMaterial(const d3d::Material& m)
{
    SentState& s = *m_sent;
    if (s.materialValid && std::memcmp(&s.material, &m, sizeof(m)) == 0) {
        ++m_frameRepeats;
        return;
    }
    s.material = m;
    s.materialValid = true;
    if (m_coalesce) {                                  // with the next draw
        m_pendingMaterial = m;
        m_hasPendingMaterial = true;
        return;
    }
    Enqueue([this, m](const uint8_t*) { m_device.SetMaterial(m); });
}

void ThreadedDevice::SetLight(uint32_t index, const d3d::Light& light)
{
    SentState& s = *m_sent;
    if (index < SentState::kLights) {
        if (s.lightValid[index] && std::memcmp(&s.light[index], &light, sizeof(light)) == 0) {
            ++m_frameRepeats;
            return;
        }
        s.light[index] = light;
        s.lightValid[index] = true;
    }
    if (m_coalesce) {                                  // with the next draw (in order with LightEnable)
        LightOp op{0, index, 0, 0, light};
        m_pendingLights.push_back(op);
        if (m_pendingLights.size() >= 1024)           // (no draw for long: don't grow without end)
            FlushPending();
        return;
    }
    Enqueue([this, index, light](const uint8_t*) { m_device.SetLight(index, light); });
}

void ThreadedDevice::LightEnable(uint32_t index, bool enable)
{
    SentState& s = *m_sent;
    if (index < SentState::kLights) {
        if (s.enabledValid[index] && s.enabled[index] == enable) {
            ++m_frameRepeats;
            return;
        }
        s.enabled[index] = enable;
        s.enabledValid[index] = true;
    }
    if (m_coalesce) {                                  // with the next draw (in order with SetLight)
        LightOp op{1, index, enable ? 1u : 0u, 0, {}};
        m_pendingLights.push_back(op);
        if (m_pendingLights.size() >= 1024)           // (no draw for long: don't grow without end)
            FlushPending();
        return;
    }
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

void ThreadedDevice::SetDepthPrepass(bool on)
{
    Enqueue([this, on](const uint8_t*) { m_device.SetDepthPrepass(on); });
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
    if (m_coalesce) {
        AddPendingState({StateItem::Texture, stage, 0, 0, texture});
        return;
    }
    Enqueue([this, stage, texture](const uint8_t*) { m_device.SetTexture(stage, texture); });
}

void ThreadedDevice::SetRenderTarget(Texture* target)
{
    // Mirror Device semantics: a non-render-target is ignored; the viewport resets to the target.
    if (target && !target->IsRenderTarget())
        return;
    m_target = target;
    m_viewport = {0, 0, target ? target->Width() : m_width, target ? target->Height() : m_height, 0.0f, 1.0f};
    m_sent->viewportValid = false;                     // the device resets its viewport
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
    m_drawStats.copied++;
    m_drawStats.vertexBytes += bytes;
    EnqueueDraw([this, primitive, fvf, vertexCount](const uint8_t* data) {
        m_device.DrawPrimitive(primitive, fvf, data, vertexCount);
    }, vertices, bytes);
}

void ThreadedDevice::DrawIndexedPrimitive(uint32_t primitive, uint32_t fvf, const void* vertices, uint32_t vertexCount,
                                          const uint16_t* indices, uint32_t indexCount)
{
    // One record holds both arrays: vertices, then indices.
    uint32_t vbytes = FvfStride(fvf) * vertexCount, ibytes = indexCount * 2;
    m_drawStats.copied++;
    m_drawStats.vertexBytes += vbytes;
    m_drawStats.indexBytes += ibytes;
    EnqueueDraw([this, primitive, fvf, vertexCount, indexCount, vbytes](const uint8_t* data) {
        m_device.DrawIndexedPrimitive(primitive, fvf, data, vertexCount, reinterpret_cast<const uint16_t*>(data + vbytes),
                                      indexCount);
    }, vertices, vbytes, indices, ibytes);
}

void ThreadedDevice::DrawPrimitiveShared(uint32_t primitive, uint32_t fvf, const SharedVertices& data, size_t byteOffset,
                                         uint32_t vertexCount)
{
    m_drawStats.shared++;
    EnqueueDraw([this, primitive, fvf, vertexCount, data, byteOffset](const uint8_t*) {
        m_device.DrawShared(primitive, fvf, data, byteOffset, vertexCount, nullptr, 0);
    });
}

void ThreadedDevice::DrawIndexedPrimitiveShared(uint32_t primitive, uint32_t fvf, const SharedVertices& data,
                                                size_t byteOffset, uint32_t vertexCount, const uint16_t* indices,
                                                uint32_t indexCount)
{
    m_drawStats.shared++;
    m_drawStats.indexBytes += indexCount * 2;
    EnqueueDraw([this, primitive, fvf, vertexCount, indexCount, data, byteOffset](const uint8_t* idx) {
        m_device.DrawShared(primitive, fvf, data, byteOffset, vertexCount, reinterpret_cast<const uint16_t*>(idx),
                            indexCount);
    }, indices, indexCount * 2);
}

void ThreadedDevice::DrawIndexedPrimitiveSharedRetained(uint32_t primitive, uint32_t fvf, const SharedVertices& data,
                                                        size_t byteOffset, uint32_t vertexCount,
                                                        const SharedVertices& indexData, uint32_t indexCount)
{
    m_drawStats.shared++;
    m_drawStats.retained++;
    EnqueueDraw([this, primitive, fvf, vertexCount, indexCount, data, byteOffset, indexData](const uint8_t*) {
        m_device.DrawSharedIndexed(primitive, fvf, data, byteOffset, vertexCount, indexData, indexCount);
    });
}

void ThreadedDevice::SetDrawVisual(uint32_t kind, const char* className, uint32_t owner)
{
    if (m_coalesce) {                                  // with the next draw: only the latest matters (Device keeps one)
        m_hasPendingVisual = true;
        m_pendingVisualKind = kind;
        m_pendingVisualName = className;
        m_pendingVisualOwner = owner;
        return;
    }
    Enqueue([this, kind, className, owner](const uint8_t*) { m_device.SetDrawVisual(kind, className, owner); });
}

void ThreadedDevice::SetSceneLights(const Device::SceneLight* lights, uint32_t count)
{
    Enqueue([this, count](const uint8_t* data) {
        m_device.SetSceneLights(reinterpret_cast<const Device::SceneLight*>(data), count);
    }, lights, uint32_t(count * sizeof(Device::SceneLight)));
}

void ThreadedDevice::DrawPrimitiveSkinned(uint32_t primitive, uint32_t fvf, const SkinJob& job, uint32_t startVertex,
                                          uint32_t vertexCount)
{
    if (!job || size_t(startVertex) + vertexCount > job->source->vertices.size())
        return;
    m_drawStats.skinned++;
    EnqueueDraw([this, primitive, fvf, job, startVertex, vertexCount](const uint8_t*) {
        m_device.DrawSkinned(primitive, fvf, *job, startVertex, vertexCount, nullptr, 0);
    });
}

void ThreadedDevice::DrawIndexedPrimitiveSkinned(uint32_t primitive, uint32_t fvf, const SkinJob& job,
                                                 uint32_t startVertex, uint32_t vertexCount, const uint16_t* indices,
                                                 uint32_t indexCount)
{
    if (!job || size_t(startVertex) + vertexCount > job->source->vertices.size())
        return;
    m_drawStats.skinned++;
    // The piece's own triangles (as almost always): the job's copy of them, no copy per draw.
    const skin::Source& source = *job->source;
    if (indices == source.gameIndices && indexCount <= source.indices.size()) {
        EnqueueDraw([this, primitive, fvf, job, startVertex, vertexCount, indexCount](const uint8_t*) {
            m_device.DrawSkinned(primitive, fvf, *job, startVertex, vertexCount, job->source->indices.data(),
                                 indexCount);
        });
        return;
    }
    m_drawStats.indexBytes += indexCount * 2;
    EnqueueDraw([this, primitive, fvf, job, startVertex, vertexCount, indexCount](const uint8_t* idx) {
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
    if (HasPending())
        FlushPending();                                // a record of its own: what waited goes first (as Enqueue)
    CountRecord("UpdateTexture");
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
