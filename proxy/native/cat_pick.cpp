// Picking characters natively (randy-vk.ini [Native] Scene=on): RCATMesh_t::IsLineIntersecting (a segment, e.g. the
// mouse ray: its bounding sphere, its pose's box, then every front-facing triangle of the skinned buffers) and the
// visual's IsRayIntersecting (vtable slot 12, a ray in units of its direction), with the geometry they use - a segment
// against a box (FUN_1004fefe), a ray into a box (FUN_10050358, Woo's), a point in a box (FUN_1004fe93).
#include "native/cat_pick.h"
#include "native/cat.h"
#include "native/cat_skin.h"
#include "native/orig_api.gen.h"
#include "native/xmath.h"

#include <cmath>
#include <cstring>

namespace rnative::catpick {

namespace {

HMODULE g_orig;

template <typename T>
T& Field(void* object, uint32_t offset) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(object) + offset); }
template <typename F>
F Internal(uint32_t rva) { return reinterpret_cast<F>(reinterpret_cast<uint8_t*>(g_orig) + rva); }
template <typename R = void, typename... A>
R Virtual(void* object, uint32_t slot, A... args)
{
    return reinterpret_cast<R(__fastcall*)(void*, void*, A...)>((*static_cast<void***>(object))[slot])(object, nullptr,
                                                                                                      args...);
}

using cat::RenderGroup;
constexpr uint32_t kVisual = 0x3C, kBoxMin = 0x1FC, kBoxMax = 0x208, kPickable = 0x220, kExtents = 0x1CC;

void Sub(const float* a, const float* b, float* out) { out[0] = a[0] - b[0], out[1] = a[1] - b[1], out[2] = a[2] - b[2]; }

// FUN_1004fe93: false when p is inside the box (bounds included).
bool __cdecl Outside(const float* p, const float* lo, const float* hi)
{
    return !(lo[0] <= p[0] && lo[1] <= p[1] && lo[2] <= p[2] && p[0] <= hi[0] && p[1] <= hi[1] && p[2] <= hi[2]);
}

// FUN_1004fefe: false when the segment a-b touches the box: an end inside, or crossing one of its six planes within
// the other two extents (y planes, x planes, z planes, in that order).
bool __cdecl Separated(const float* a, const float* b, const float* lo, const float* hi)
{
    if (!Outside(a, lo, hi) || !Outside(b, lo, hi)) return false;
    // Crossing the plane `axis` = v: where, and is it inside on axes u, w.
    auto crosses = [&](int axis, float v, int u, int w) {
        if (!((a[axis] <= v && v <= b[axis]) || (v <= a[axis] && v >= b[axis]))) return false;
        const double t = (double(v) - a[axis]) / (double(b[axis]) - a[axis]);
        const double pu = (double(b[u]) - a[u]) * t + a[u];
        if (!(lo[u] <= pu && hi[u] >= pu)) return false;
        const double pw = (double(b[w]) - a[w]) * t + a[w];
        return lo[w] <= pw && hi[w] >= pw;
    };
    if (b[1] != a[1] && (crosses(1, lo[1], 0, 2) || crosses(1, hi[1], 0, 2))) return false;
    if (b[0] != a[0] && (crosses(0, lo[0], 1, 2) || crosses(0, hi[0], 1, 2))) return false;
    if (b[2] != a[2] && (crosses(2, lo[2], 1, 0) || crosses(2, hi[2], 1, 0))) return false;
    return true;
}

// FUN_10050358: where a ray from `o` along `d` enters the box (Woo's: the farthest of the candidate planes).
bool __cdecl RayBox(const float* o, const float* d, const float* lo, const float* hi, float* hit)
{
    int quadrant[3];
    float candidate[3];
    bool inside = true;
    for (int i = 0; i < 3; ++i) {
        if (!(lo[i] < o[i]) && !(lo[i] == o[i])) {
            quadrant[i] = 1;
            candidate[i] = lo[i];
            inside = false;
        } else if (hi[i] < o[i]) {
            quadrant[i] = 0;
            candidate[i] = hi[i];
            inside = false;
        } else {
            quadrant[i] = 2;
        }
    }
    if (inside) {
        hit[0] = o[0], hit[1] = o[1], hit[2] = o[2];
        return true;
    }
    float maxT[3];
    for (int i = 0; i < 3; ++i)
        maxT[i] = (quadrant[i] != 2 && d[i] != 0.0f) ? (candidate[i] - o[i]) / d[i] : -1.0f;
    int plane = 0;
    for (int i = 1; i < 3; ++i)
        if (maxT[plane] < maxT[i]) plane = i;
    if (maxT[plane] < 0.0f) return false;
    for (int i = 0; i < 3; ++i) {
        if (i == plane) {
            hit[i] = candidate[i];
            continue;
        }
        const float h = d[i] * maxT[plane] + o[i];
        hit[i] = h;
        if (lo[i] > h || hi[i] < h) return false;
    }
    return true;
}

float __fastcall Distance(const float* a, void*, const float* b)   // FUN_10058382 (thiscall on a)
{
    float d[3];
    Sub(a, b, d);
    return float(std::sqrt(double(xm::LengthSquared(d))));
}

void PoseUpdate(uint8_t* mesh)
{
    Internal<void(__fastcall*)(void*)>(0x55D52)(mesh);   // bones
    Internal<void(__fastcall*)(void*)>(0x55C1C)(mesh);   // skinning
}

float AnimRadius(void* anim) { return Virtual<float>(anim, 7); }

// A segment / ray from `a` along `d` against the triangles of every piece's skinned buffer: the first front-facing hit
// with its parameter at most `limit`.
template <typename Box>
bool Triangles(uint8_t* mesh, const float* a, const float* d, float limit, float* at, Box&& boxSkip)
{
    for (int32_t gi = 0; gi < Field<int32_t>(mesh, 0x0C); ++gi) {
        RenderGroup* g = Field<RenderGroup*>(mesh, 0x10) + gi;
        uint8_t* group = Field<uint8_t*>(g->mesh, cat::kMeshGroups) + gi * cat::kGroupSize;
        for (int32_t pi = 0; pi < Field<int32_t>(group, cat::kGroupPieceCount); ++pi) {
            uint8_t* piece = Field<uint8_t*>(group, cat::kGroupPieces) + pi * cat::kPieceSize;
            RenderGroup::Slot& slot = g->slots[pi];
            if (!(0 < Field<int32_t>(piece, cat::kPieceActiveTris)) || !slot.vertexCount) continue;
            float* v = static_cast<float*>(orig::VertexBuffer_c_Lock(slot.buffer, 0, 0x10));
            if (!boxSkip(v, slot.vertexCount)) {
                const int32_t count = Field<int32_t>(piece, cat::kPieceActiveTris);
                const uint16_t* idx = Field<uint16_t*>(piece, cat::kPieceIndices);
                for (int32_t k = 0; k < count; k += 3, idx += 3) {
                    const float* p0 = v + idx[0] * 8;
                    const float* p1 = v + idx[1] * 8;
                    const float* p2 = v + idx[2] * 8;
                    float e0[3], e1[3], n[3];
                    Sub(p2, p1, e1);
                    Sub(p1, p0, e0);
                    xm::Cross(e0, e1, n);
                    float t = n[2] * d[2] + n[0] * d[0] + n[1] * d[1];
                    if (!(t < 0.0f) && !std::isnan(t)) continue;   // facing away (or edge on)
                    float w[3];
                    Sub(p0, a, w);
                    const float num = w[2] * n[2] + n[0] * w[0] + w[1] * n[1];
                    t = num / t;
                    float dt[3] = {d[0] * t, d[1] * t, d[2] * t}, p[3];
                    p[0] = dt[0] + a[0], p[1] = dt[1] + a[1], p[2] = dt[2] + a[2];
                    float u[3], q[3], c0[3], c1[3], c2[3];
                    Sub(p, p0, q);
                    Sub(p1, p0, u);
                    xm::Cross(u, q, c0);
                    Sub(p, p1, q);
                    Sub(p2, p1, u);
                    xm::Cross(u, q, c1);
                    if (!(0.0f <= c0[2] * c1[2] + c1[0] * c0[0] + c0[1] * c1[1])) continue;
                    Sub(p, p2, q);
                    Sub(p0, p2, u);
                    xm::Cross(u, q, c2);
                    if (0.0f <= c2[2] * c0[2] + c2[0] * c0[0] + c2[1] * c0[1] && t <= limit) {
                        if (at) *at = t;
                        orig::VertexBuffer_c_Unlock(slot.buffer);
                        return true;
                    }
                }
            }
            orig::VertexBuffer_c_Unlock(slot.buffer);
        }
    }
    return false;
}

// Misses the sphere around the origin of radius sqrt(r2), from `a` along `d` (|d| not 1): the closest approach.
bool MissesSphere(const float* a, const float* d, float c2, float r2, bool zxy)
{
    if (!(r2 < c2)) return false;                     // starts inside
    const float b2 = float(double(a[2] * d[2] + a[0] * d[0] + a[1] * d[1]) * 2.0);
    const float dd = zxy ? d[2] * d[2] + d[0] * d[0] + d[1] * d[1] : d[1] * d[1] + d[0] * d[0] + d[2] * d[2];
    const float t = -b2 / float(double(dd) * 2.0);
    if (t <= 0.0f) return true;
    const float closest = (t * b2 + c2 + dd * t * t) - r2;
    return 0.0f < closest;
}

// RCATMesh_t::IsLineIntersecting: from `from` to `to`; with `sphere` only as far as the pose's box (the distance to
// it in *at).
bool LineTest(uint8_t* mesh, const float* from, const float* to, float* at, bool sphere)
{
    if (!Field<void*>(mesh, 4) || !Field<void*>(mesh, 8) || !mesh[kPickable]) return false;
    float a[3] = {from[0], from[1], from[2]}, b[3] = {to[0], to[1], to[2]};
    float inverse[16];
    std::memcpy(inverse, orig::RRefFrame_t_GetWorldMatrix(mesh + kVisual), sizeof(inverse));
    Internal<void(__fastcall*)(float*)>(0x6E108)(inverse);
    float t[3];
    xm::Transform(a, inverse, t), std::memcpy(a, t, 12);
    xm::Transform(b, inverse, t), std::memcpy(b, t, 12);
    float d[3];
    Sub(b, a, d);
    const float length = float(std::sqrt(double(xm::LengthSquared(d))));
    if (length == 0.0f) return false;
    const float inv = 1.0f / length;
    d[0] = d[0] * inv, d[1] = d[1] * inv, d[2] = d[2] * inv;
    const float radius = float((double(AnimRadius(Field<void*>(mesh, 8))) + Field<float>(mesh, kExtents)) *
                               double(Field<float>(mesh, kExtents + 4)));
    const float c2 = a[2] * a[2] + a[0] * a[0] + a[1] * a[1];
    if (MissesSphere(a, d, c2, radius * radius, false)) return false;
    PoseUpdate(mesh);
    float hit[3] = {0.0f, 0.0f, 0.0f};
    bool found = RayBox(a, d, &Field<float>(mesh, kBoxMin), &Field<float>(mesh, kBoxMax), hit);
    float distance = 0.0f;
    if (found) {
        distance = Distance(a, nullptr, hit);
        if (length < distance) found = false;
    }
    if (sphere) {
        if (at) *at = distance;
        return found;
    }
    if (!found) return false;
    orig::RRefFrame_t_GetWorldMatrix(mesh + kVisual);
    return Triangles(mesh, a, d, length, at, [&](const float* v, uint32_t n) {
        float hi[3] = {v[0], v[1], v[2]}, lo[3] = {v[0], v[1], v[2]};
        for (uint32_t i = 1; i < n; ++i) {
            const float* p = v + i * 8;
            for (int k = 0; k < 3; ++k) {
                if (lo[k] <= p[k]) {
                    if (hi[k] < p[k]) hi[k] = p[k];
                } else {
                    lo[k] = p[k];
                }
            }
        }
        return Separated(a, b, lo, hi);
    });
}

// The visual's IsRayIntersecting (vtable slot 12, FUN_1005632d): a ray (hits up to 1 along it).
bool RayTest(uint8_t* visual, const float* from, const float* along, float* at)
{
    uint8_t* mesh = visual - kVisual;
    if (!Field<void*>(mesh, 4) || !Field<void*>(mesh, 8)) return false;
    float o[3] = {from[0], from[1], from[2]}, d[3] = {along[0], along[1], along[2]};
    const float* w = static_cast<const float*>(orig::RRefFrame_t_GetWorldMatrix(visual));
    {                                                 // FUN_10044ae1: into the frame (rotation transposed)
        o[0] = o[0] - w[12], o[1] = o[1] - w[13];
        const float z = o[2] - w[14];
        o[2] = z;
        const float x = o[0], y = o[1];
        o[0] = w[2] * z + x * w[0] + w[1] * y;
        o[1] = w[6] * z + w[4] * x + w[5] * y;
        o[2] = w[10] * z + w[8] * x + w[9] * y;
    }
    {                                                 // FUN_10055975: the direction likewise
        const float x = d[0], y = d[1], z = d[2];
        d[0] = w[2] * z + x * w[0] + w[1] * y;
        d[1] = w[6] * z + w[5] * y + w[4] * x;
        d[2] = w[10] * z + w[9] * y + w[8] * x;
    }
    const double radius = (double(AnimRadius(Field<void*>(mesh, 8))) + Field<float>(mesh, kExtents)) *
                          double(Field<float>(mesh, kExtents + 4));
    float scale;
    {
        if (visual[0x9E]) orig::RRefFrame_t_UpdateWorldMatrix(visual);
        scale = Field<float>(visual, 0x84);
    }
    const float c2 = o[2] * o[2] + o[0] * o[0] + o[1] * o[1];
    const float r = float(double(scale) * radius);
    if (MissesSphere(o, d, c2, r * r, true)) return false;
    PoseUpdate(mesh);
    return Triangles(mesh, o, d, 1.0f, at, [](const float*, uint32_t) { return false; });
}

// Both skin the character exactly first (deferred skinning only hands the renderer jobs; picking reads the vertices
// and the box) - the scope the skinning's own wraps of these functions gave them.
struct Exact {
    bool on;
    explicit Exact(void* render) : on(skin::PickingBegin(render)) {}
    ~Exact()
    {
        if (on) skin::PickingEnd();
    }
};

bool __fastcall IsLineIntersecting(uint8_t* mesh, void*, const float* from, const float* to, float* at, bool sphere)
{
    Exact exact(mesh);
    return LineTest(mesh, from, to, at, sphere);
}

bool __fastcall IsRayIntersecting(uint8_t* visual, void*, const float* from, const float* along, float* at, bool)
{
    Exact exact(visual - kVisual);
    return RayTest(visual, from, along, at);
}

}  // namespace

void Install(HMODULE orig)
{
    if (GetMode("Scene", Mode::Off) != Mode::On) return;
    g_orig = orig;
    struct Entry {
        uint32_t rva;
        void* target;
        const char* what;
    };
#define FN(f) reinterpret_cast<void*>(&f)
    const Entry entries[] = {
        {0x4FE93, FN(Outside), "point outside a box (FUN_1004fe93)"},
        {0x4FEFE, FN(Separated), "segment against a box (FUN_1004fefe)"},
        {0x50358, FN(RayBox), "ray into a box (FUN_10050358)"},
        {0x58382, FN(Distance), "distance (FUN_10058382)"},
        {0x56786, FN(IsLineIntersecting), "RCATMesh_t::IsLineIntersecting"},
        {0x5632D, FN(IsRayIntersecting), "RCATMesh_t ray test (FUN_1005632d)"},
    };
#undef FN
    int installed = 0;
    for (const Entry& e : entries) installed += Replace(orig, e.rva, e.target, e.what) ? 1 : 0;
    Log("picking: %d of %d functions native", installed, int(sizeof(entries) / sizeof(entries[0])));
}

}  // namespace rnative::catpick
