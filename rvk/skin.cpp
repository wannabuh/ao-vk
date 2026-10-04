// Character skinning on the CPU with SSE (randy31's original: x87, one value at a time). Same results as its
// FUN_1005470d, including its quirks: vertices with a bone out of range keep their old contents, and the box grows the
// original's way (a value that lowers the minimum doesn't also raise the maximum - so the first vertex never counts
// towards the maximum).
#include "skin.h"

#include "internal.h"

#include <emmintrin.h>
#include <malloc.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <condition_variable>
#include <deque>
#include <thread>

namespace rvk::skin {

Palette::~Palette()
{
    _aligned_free(bones);
}

void Palette::Set(const Bone* source, uint32_t n)
{
    if (n > capacity) {
        _aligned_free(bones);
        capacity = n + 16;
        bones = static_cast<Columns*>(_aligned_malloc(sizeof(Columns) * capacity, 16));
    }
    count = n;
    for (uint32_t i = 0; i < n; ++i) {
        const float* m = source[i].m;
        for (int c = 0; c < 4; ++c) {
            bones[i].c[c][0] = m[c * 3 + 0];
            bones[i].c[c][1] = m[c * 3 + 1];
            bones[i].c[c][2] = m[c * 3 + 2];
            bones[i].c[c][3] = 0.0f;
        }
    }
}

namespace {

// p * M (x' = m0 x + m3 y + m6 z + m9, ...), and the rotation alone for normals.
inline __m128 Transform(const Palette::Columns& b, const float* p)
{
    __m128 r = _mm_add_ps(_mm_mul_ps(_mm_load_ps(b.c[0]), _mm_set1_ps(p[0])),
                          _mm_mul_ps(_mm_load_ps(b.c[1]), _mm_set1_ps(p[1])));
    return _mm_add_ps(r, _mm_add_ps(_mm_mul_ps(_mm_load_ps(b.c[2]), _mm_set1_ps(p[2])), _mm_load_ps(b.c[3])));
}

inline __m128 Rotate(const Palette::Columns& b, const float* n)
{
    __m128 r = _mm_add_ps(_mm_mul_ps(_mm_load_ps(b.c[0]), _mm_set1_ps(n[0])),
                          _mm_mul_ps(_mm_load_ps(b.c[1]), _mm_set1_ps(n[1])));
    return _mm_add_ps(r, _mm_mul_ps(_mm_load_ps(b.c[2]), _mm_set1_ps(n[2])));
}

// Position and normal into the vertex, leaving its texture coordinates.
inline void Store(Vertex* v, __m128 pos, __m128 normal)
{
    __m128 zx = _mm_shuffle_ps(pos, normal, _MM_SHUFFLE(0, 0, 2, 2));            // pos.z, pos.z, n.x, n.x
    _mm_storeu_ps(v->pos, _mm_shuffle_ps(pos, zx, _MM_SHUFFLE(2, 0, 1, 0)));      // pos.x, pos.y, pos.z, n.x
    _mm_storel_pi(reinterpret_cast<__m64*>(&v->normal[1]), _mm_shuffle_ps(normal, normal, _MM_SHUFFLE(3, 3, 2, 1)));
}

inline __m128 Load3(const float* p)
{
    return _mm_set_ps(0.0f, p[2], p[1], p[0]);
}

inline void StorePosition(Vertex* v, __m128 pos)
{
    alignas(16) float t[4];
    _mm_store_ps(t, pos);
    v->pos[0] = t[0], v->pos[1] = t[1], v->pos[2] = t[2];
}

// The skinned position of one vertex (and its normal, when `normal` is given); false if a bone is out of range.
inline bool SkinOne(const TriVertex& v, const Palette& bones, bool rest, __m128* pos, __m128* normal)
{
    const auto boneCount = uint32_t(bones.count);
    if (rest) {
        *pos = Load3(v.bind);
        if (normal) *normal = Load3(v.normal);
        return true;
    }
    if (v.weightA <= 0.99f) {                       // the original: <= (double)0.99f
        if (uint32_t(v.boneA) >= boneCount || uint32_t(v.boneB) >= boneCount)
            return false;
        const Palette::Columns& a = bones.bones[v.boneA];
        const Palette::Columns& b = bones.bones[v.boneB];
        __m128 w = _mm_set1_ps(v.weightA), w1 = _mm_set1_ps(1.0f - v.weightA);
        *pos = _mm_add_ps(_mm_mul_ps(w, Transform(a, v.posA)), _mm_mul_ps(w1, Transform(b, v.posB)));
        if (normal) *normal = Rotate(a, v.normal);
        return true;
    }
    if (uint32_t(v.boneA) >= boneCount)
        return false;
    const Palette::Columns& a = bones.bones[v.boneA];
    *pos = Transform(a, v.posA);
    if (normal) *normal = Rotate(a, v.normal);
    return true;
}

}  // namespace

void SkinVertices(const TriVertex* in, uint32_t count, Vertex* out, const Palette& bones, bool rest, float* boxMin,
                  float* boxMax)
{
    __m128 mn = boxMin ? Load3(boxMin) : _mm_set1_ps(FLT_MAX);
    __m128 mx = boxMax ? Load3(boxMax) : _mm_set1_ps(-FLT_MAX);
    for (uint32_t i = 0; i < count; ++i) {
        __m128 pos, normal;
        if (SkinOne(in[i], bones, rest, &pos, &normal))
            Store(out + i, pos, normal);
        else
            pos = Load3(out[i].pos);
        // The original: if (!(min <= p)) min = p; else if (max < p) max = p - per coordinate.
        if (boxMin) {
            __m128 lower = _mm_cmpnle_ps(mn, pos);
            if (boxMax)
                mx = _mm_or_ps(_mm_and_ps(lower, mx), _mm_andnot_ps(lower, _mm_max_ps(mx, pos)));
            mn = _mm_or_ps(_mm_and_ps(lower, pos), _mm_andnot_ps(lower, mn));
        } else if (boxMax) {
            mx = _mm_max_ps(mx, pos);
        }
    }
    alignas(16) float t[4];
    if (boxMin) {
        _mm_store_ps(t, mn);
        boxMin[0] = t[0], boxMin[1] = t[1], boxMin[2] = t[2];
    }
    if (boxMax) {
        _mm_store_ps(t, mx);
        boxMax[0] = t[0], boxMax[1] = t[1], boxMax[2] = t[2];
    }
}

void SkinPositions(const TriVertex* in, uint32_t count, uint32_t step, Vertex* out, const Palette& bones, bool rest)
{
    for (uint32_t i = 0; i < count; i += step) {
        __m128 pos;
        if (SkinOne(in[i], bones, rest, &pos, nullptr))
            StorePosition(out + i, pos);
    }
}

void Source::Finish()
{
    indexHash = indices.empty() ? 0 : detail::HashBytes(indices.data(), indices.size() * 2, indices.size());
    int32_t maxBone = -1;
    for (const TriVertex& v : vertices) maxBone = std::max({maxBone, v.boneA, v.boneB});
    boneBoxes.assign(size_t(maxBone + 1) * 6, 0.0f);
    for (int32_t b = 0; b <= maxBone; ++b)
        for (int j = 0; j < 3; ++j) boneBoxes[b * 6 + j] = FLT_MAX, boneBoxes[b * 6 + 3 + j] = -FLT_MAX;
    for (int j = 0; j < 3; ++j) bindMin[j] = FLT_MAX, bindMax[j] = -FLT_MAX;
    auto grow = [](float* box, const float* p) {
        for (int j = 0; j < 3; ++j) { box[j] = std::min(box[j], p[j]); box[3 + j] = std::max(box[3 + j], p[j]); }
    };
    for (const TriVertex& v : vertices) {
        for (int j = 0; j < 3; ++j) bindMin[j] = std::min(bindMin[j], v.bind[j]), bindMax[j] = std::max(bindMax[j], v.bind[j]);
        if (v.boneA >= 0) grow(&boneBoxes[size_t(v.boneA) * 6], v.posA);
        if (v.weightA <= 0.99f && v.boneB >= 0) grow(&boneBoxes[size_t(v.boneB) * 6], v.posB);
    }
}

void Job::ComputeBounds()
{
    const Source* s = source.get();
    BoundsOf(&s, 1, *bones, rest, boundsMin, boundsMax);
}

void Job::BoundsOf(const Source* const* sources, size_t count, const Palette& bones, bool rest, float boxMin[3],
                   float boxMax[3])
{
    for (int j = 0; j < 3; ++j) boxMin[j] = FLT_MAX, boxMax[j] = -FLT_MAX;
    auto add = [&](const float* p) {
        for (int j = 0; j < 3; ++j) boxMin[j] = std::min(boxMin[j], p[j]), boxMax[j] = std::max(boxMax[j], p[j]);
    };
    // A vertex is in the rest pose, or a blend of its bones' transforms of its positions in their spaces - inside
    // the union of the bones' boxes, transformed (a vertex with a bone out of range: the rest pose, so that too).
    // The pieces' boxes per bone are merged first: each bone's corners are transformed once.
    static thread_local std::vector<float> merged;
    size_t boxes = 0;
    for (size_t i = 0; i < count; ++i) boxes = std::max(boxes, sources[i]->boneBoxes.size() / 6);
    merged.assign(boxes * 6, 0.0f);
    for (size_t b = 0; b < boxes; ++b)
        for (int j = 0; j < 3; ++j) merged[b * 6 + j] = FLT_MAX, merged[b * 6 + 3 + j] = -FLT_MAX;
    bool restToo = rest;
    for (size_t i = 0; i < count; ++i) {
        const std::vector<float>& own = sources[i]->boneBoxes;
        for (size_t k = 0; k < own.size(); k += 6)
            for (int j = 0; j < 3; ++j) {
                merged[k + j] = std::min(merged[k + j], own[k + j]);
                merged[k + 3 + j] = std::max(merged[k + 3 + j], own[k + 3 + j]);
            }
        if (rest) {
            add(sources[i]->bindMin);
            add(sources[i]->bindMax);
        }
    }
    for (size_t b = 0; b < boxes && !rest; ++b) {
        const float* box = &merged[b * 6];
        if (box[0] > box[3])
            continue;                            // no vertex in this bone's space
        if (b >= bones.count) {
            restToo = true;
            continue;
        }
        const Palette::Columns& m = bones.bones[b];
        for (int corner = 0; corner < 8; ++corner) {
            float p[3] = {box[(corner & 1) ? 3 : 0], box[(corner & 2) ? 4 : 1], box[(corner & 4) ? 5 : 2]};
            float q[3];
            for (int j = 0; j < 3; ++j)
                q[j] = m.c[0][j] * p[0] + m.c[1][j] * p[1] + m.c[2][j] * p[2] + m.c[3][j];
            add(q);
        }
    }
    if (restToo && !rest)
        for (size_t i = 0; i < count; ++i) {
            add(sources[i]->bindMin);
            add(sources[i]->bindMax);
        }
    if (boxMin[0] > boxMax[0])                   // no vertices
        for (int j = 0; j < 3; ++j) boxMin[j] = boxMax[j] = 0.0f;
}

const Vertex* Job::Skinned()
{
    std::call_once(m_once, [this] {
        const std::vector<TriVertex>& in = source->vertices;
        m_out.resize(in.size());
        for (size_t i = 0; i < in.size(); ++i) {
            Vertex& o = m_out[i];
            o.uv[0] = in[i].uv[0];
            o.uv[1] = in[i].uv[1];
            __m128 pos, normal;
            if (SkinOne(in[i], *bones, rest, &pos, &normal)) {
                Store(&o, pos, normal);
            } else {                                // a bone out of range: the rest pose (the original keeps old data)
                for (int k = 0; k < 3; ++k) o.pos[k] = in[i].bind[k], o.normal[k] = in[i].normal[k];
            }
        }
    });
    return m_out.data();
}

namespace {

// Threads skinning jobs ahead of their draws: all but two of the processor's (the game's and the renderer's), at most 4.
class Pool {
public:
    static Pool& Get()
    {
        static Pool* pool = new Pool;                // never destroyed (threads may still run at process exit)
        return *pool;
    }
    bool Enabled() const { return !m_threads.empty(); }
    void Push(std::shared_ptr<Job> job)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_queue.push_back(std::move(job));
        }
        m_wake.notify_one();
    }

private:
    Pool()
    {
        unsigned n = std::min(4u, std::max(std::thread::hardware_concurrency(), 2u) - 2u);
        for (unsigned i = 0; i < n; ++i)
            m_threads.emplace_back([this] { Run(); });
        for (std::thread& t : m_threads)
            t.detach();
    }
    void Run()
    {
        for (;;) {
            std::shared_ptr<Job> job;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_wake.wait(lock, [this] { return !m_queue.empty(); });
                job = std::move(m_queue.front());
                m_queue.pop_front();
            }
            job->Skinned();
        }
    }
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<std::shared_ptr<Job>> m_queue;
    std::vector<std::thread> m_threads;
};

}  // namespace

std::atomic<bool> g_prefetch{true};

void Job::SetPrefetch(bool enable)
{
    g_prefetch = enable;
}

void Job::Prefetch(const std::shared_ptr<Job>& job)
{
    if (!g_prefetch)
        return;
    Pool& pool = Pool::Get();
    if (pool.Enabled())
        pool.Push(job);
}

}  // namespace rvk::skin
