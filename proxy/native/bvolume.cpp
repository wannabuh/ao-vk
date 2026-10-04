// BVolume_t natively (randy-vk.ini [Native] Scene=on): a bounding sphere and box - from points (Ritter's sphere from
// extreme points, then grown; the box from those extreme points), copied, archived.
//
// BVolume_t (0x30 bytes, a Serializable_c, vtable 0x8A5CC): +0x08 centre, +0x14 radius, +0x18 box minimum, +0x24 box
// maximum. The original's extreme-point search tracks, in its "minimum y" and "minimum z" slots, the points of
// minimum x and minimum y (a slip kept: the boxes are what the original makes).
#include "native/bvolume.h"
#include "native/serialize.h"
#include "native/vc10.h"

#include <cmath>
#include <cstring>

namespace rnative::bvolume {

namespace {

HMODULE g_orig;

template <typename T>
T& Field(void* object, uint32_t offset) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(object) + offset); }
void SetVtable(void* o, uint32_t rva) { Field<uintptr_t>(o, 0) = reinterpret_cast<uintptr_t>(g_orig) + rva; }
const serialize::Api& S() { return serialize::Get(); }

constexpr uint32_t kVtable = 0x8A5CC, kCenter = 0x08, kRadius = 0x14, kMin = 0x18, kMax = 0x24, kSize = 0x30;

// |v|^2 as the original's x87 sums it (z, x, y), rounded once.
float LengthSq(const float* v) { return float((double(v[2]) * v[2] + double(v[0]) * v[0]) + double(v[1]) * v[1]); }

// FUN_10029a70: the indices of the extreme points (see the note above).
void __cdecl Extremes(const float* p, uint32_t n, uint32_t* minX, uint32_t* maxX, uint32_t* minX2, uint32_t* maxY,
                      uint32_t* minY, uint32_t* maxZ)
{
    if (n == 0) *maxZ = *minY = *maxY = *minX2 = *maxX = *minX = 0xFFFFFFFFu;   // (overwritten just below)
    *maxZ = *minY = *maxY = *minX2 = *maxX = *minX = 0;
    for (uint32_t i = 1; i < n; ++i) {
        const float* q = p + i * 3;
        if (q[0] < p[*minX * 3]) *minX = i;
        if (p[*maxX * 3] < q[0]) *maxX = i;
        if (q[0] < p[*minX2 * 3]) *minX2 = i;
        if (p[*maxY * 3 + 1] < q[1]) *maxY = i;
        if (q[1] < p[*minY * 3 + 1]) *minY = i;
        if (p[*maxZ * 3 + 2] < q[2]) *maxZ = i;
    }
}

// FUN_10029b7e: Ritter's sphere and the box of `n` points.
void __cdecl Bound(const float* p, uint32_t n, float* center, float* radius, float* boxMin, float* boxMax)
{
    uint32_t i0, i1, i2, i3, i4, i5;
    Extremes(p, n, &i0, &i1, &i2, &i3, &i4, &i5);
    boxMin[0] = p[i0 * 3];
    boxMin[1] = p[i2 * 3 + 1];
    boxMin[2] = p[i4 * 3 + 2];
    boxMax[0] = p[i1 * 3];
    boxMax[1] = p[i3 * 3 + 1];
    boxMax[2] = p[i5 * 3 + 2];
    auto spanSq = [&](uint32_t a, uint32_t b) {
        const float d[3] = {p[b * 3] - p[a * 3], p[b * 3 + 1] - p[a * 3 + 1], p[b * 3 + 2] - p[a * 3 + 2]};
        return LengthSq(d);
    };
    const float sx = spanSq(i0, i1), sy = spanSq(i2, i3), sz = spanSq(i4, i5);
    float widest = sx;
    uint32_t a = i0, b = i1;
    if (sx < sy) widest = sy, a = i2, b = i3;
    if (widest < sz) a = i4, b = i5;
    const float* pb = p + b * 3;
    for (int k = 0; k < 3; ++k) center[k] = (pb[k] + p[a * 3 + k]) / 2.0f;
    float d[3] = {pb[0] - center[0], pb[1] - center[1], pb[2] - center[2]};
    float radiusSq = LengthSq(d);
    *radius = float(std::sqrt(double(radiusSq)));
    for (uint32_t i = 0; i < n; ++i) {
        const float* q = p + i * 3;
        const float e[3] = {q[0] - center[0], q[1] - center[1], q[2] - center[2]};
        const float distSq = LengthSq(e);
        if (!(radiusSq < distSq)) continue;
        const float dist = float(std::sqrt(double(distSq)));
        const float r = float((double(*radius) + dist) * 0.5);
        *radius = r;
        radiusSq = float(double(r) * r);
        const float behind = dist - r;
        for (int k = 0; k < 3; ++k) {
            const float c = center[k] * r, s = q[k] * behind;
            center[k] = (s + c) / dist;
        }
    }
}

void ZeroAllButRadius(uint8_t* v)
{
    std::memset(v + kCenter, 0, 12);
    std::memset(v + kMin, 0, 24);
}

void* __fastcall Construct(uint8_t* v)                 // BVolume_t::BVolume_t
{
    S().construct(v, nullptr);
    SetVtable(v, kVtable);
    std::memset(v + kCenter, 0, kSize - kCenter);
    return v;
}

void* __fastcall ConstructFrom(uint8_t* v, void*, const float* points, uint32_t count, uint32_t)   // FUN_10017fee
{
    S().construct(v, nullptr);
    SetVtable(v, kVtable);
    ZeroAllButRadius(v);
    Bound(points, count, &Field<float>(v, kCenter), &Field<float>(v, kRadius), &Field<float>(v, kMin),
          &Field<float>(v, kMax));
    return v;
}

void __fastcall Init(uint8_t* v, void*, const float* points, uint32_t count, uint32_t)   // BVolume_t::Init
{
    Bound(points, count, &Field<float>(v, kCenter), &Field<float>(v, kRadius), &Field<float>(v, kMin),
          &Field<float>(v, kMax));
}

void* __fastcall Copy(uint8_t* v, void*, const uint8_t* from)   // FUN_10029a13
{
    S().construct(v, nullptr);
    SetVtable(v, kVtable);
    std::memcpy(v + kCenter, from + kCenter, kSize - kCenter);
    return v;
}

void* __fastcall ConstructFromArchive(uint8_t* v, void*, void* archive)   // FUN_10029e82
{
    S().construct(v, nullptr);
    SetVtable(v, kVtable);
    ZeroAllButRadius(v);
    void* stream = S().getStream(archive, nullptr);
    Field<float>(v, kRadius) = 0.0f;
    S().findVector3(stream, nullptr, "sph_pos", &Field<float>(v, kCenter), 0);
    S().findFloat(stream, nullptr, "sph_radius", &Field<float>(v, kRadius), 0);
    S().findVector3(stream, nullptr, "min_pos", &Field<float>(v, kMin), 0);
    S().findVector3(stream, nullptr, "max_pos", &Field<float>(v, kMax), 0);
    return v;
}

void* __cdecl Instantiate(void* archive) { return ConstructFromArchive(static_cast<uint8_t*>(vc10::Allocate(kSize)), nullptr, archive); }

void __fastcall Archive(uint8_t* v, void*, void* archive)
{
    void* stream = S().getStream(archive, nullptr);
    S().addInt32(stream, nullptr, "version", 1);
    S().addVector3(stream, nullptr, "sph_pos", &Field<float>(v, kCenter));
    S().addFloat(stream, nullptr, "sph_radius", Field<float>(v, kRadius));
    S().addVector3(stream, nullptr, "min_pos", &Field<float>(v, kMin));
    S().addVector3(stream, nullptr, "max_pos", &Field<float>(v, kMax));
}

void __fastcall Destroy(uint8_t* v)
{
    SetVtable(v, kVtable);
    S().destroy(v, nullptr);
}

void* __fastcall Delete(uint8_t* v, void*, uint8_t flags)   // vtable slot 0 (FUN_10012b1f)
{
    if (!(flags & 2)) {
        Destroy(v);
        if (flags & 1) vc10::Free(v);
        return v;
    }
    uint32_t* block = reinterpret_cast<uint32_t*>(v) - 1;
    for (uint32_t i = block[0]; i-- > 0;) Destroy(v + i * kSize);
    if (flags & 1) vc10::FreeArray(block);
    return block;
}

}  // namespace

void Install(HMODULE orig)
{
    if (GetMode("Scene", Mode::Off) != Mode::On) return;
    g_orig = orig;
    if (!S().complete) return;
    struct Entry {
        uint32_t rva;
        void* target;
        const char* what;
    };
#define FN(f) reinterpret_cast<void*>(&f)
    const Entry entries[] = {
        {0x29A70, FN(Extremes), "BVolume_t extreme points (FUN_10029a70)"},
        {0x29B7E, FN(Bound), "BVolume_t sphere and box (FUN_10029b7e)"},
        {0x299E0, FN(Construct), "BVolume_t::BVolume_t"},
        {0x17FEE, FN(ConstructFrom), "BVolume_t::BVolume_t(points) (FUN_10017fee)"},
        {0x29E51, FN(Init), "BVolume_t::Init"},
        {0x29A13, FN(Copy), "BVolume_t::BVolume_t(copy) (FUN_10029a13)"},
        {0x29E82, FN(ConstructFromArchive), "BVolume_t::BVolume_t(archive) (FUN_10029e82)"},
        {0x29F97, FN(Instantiate), "BVolume_t::Instantiate"},
        {0x29F2D, FN(Archive), "BVolume_t::Archive"},
        {0x29E76, FN(Destroy), "BVolume_t::~BVolume_t"},
        {0x12B1F, FN(Delete), "BVolume_t deleting destructor (FUN_10012b1f)"},
    };
#undef FN
    int installed = 0;
    for (const Entry& e : entries) installed += Replace(orig, e.rva, e.target, e.what) ? 1 : 0;
    Log("bounding volumes: %d of %d functions native", installed, int(sizeof(entries) / sizeof(entries[0])));
}

}  // namespace rnative::bvolume
