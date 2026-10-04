// Character skinning on the CPU with SSE (randy31's original: x87, one value at a time). Same results as its
// FUN_1005470d, including its quirks: vertices with a bone out of range keep their old contents, and the box grows the
// original's way (a value that lowers the minimum doesn't also raise the maximum - so the first vertex never counts
// towards the maximum).
#include "skin.h"

#include <emmintrin.h>
#include <malloc.h>

#include <algorithm>
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

void Job::Prefetch(const std::shared_ptr<Job>& job)
{
    Pool& pool = Pool::Get();
    if (pool.Enabled())
        pool.Push(job);
}

}  // namespace rvk::skin
