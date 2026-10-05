// RKeyFrameAnimation_t natively (part of [Native] Scene=on): a frame's keyframe animation - translations, rotations,
// visibility and texture (UV) keys - read from archives (and FAFAnim_t, the same kept from FAF files), written to
// them, and evaluated at a time for RRefFrame_t (into its matrix and visibility; the UV scale and offset kept). The
// arithmetic is the original's x87 code in its order (xmath.h: double, rounded to float where it stores a float), so
// the results are the same to the bit.
//
// RKeyFrameAnimation_t (an RResource_t, 0x94 bytes): +0x2C the current translation / rotation / visibility / UV key
// (indices, kept from one evaluation to the next), +0x3C std::vector of translation keys (x y z, time), +0x4C rotation
// keys (quaternion, time), +0x5C visibility keys (time, visible), +0x6C UV keys (scale u v, offset u v, time,
// interpolate), +0x7C its length, +0x80 looping, +0x84 / +0x88 the UV scale, +0x8C / +0x90 the UV offset.
#include "native/keyframe.h"
#include "native/orig_api.gen.h"
#include "native/serialize.h"
#include "native/vc10.h"
#include "native/xmath.h"

#include <cstring>
#include <initializer_list>

namespace rnative::keyframe {

namespace {

HMODULE g_orig;

template <typename T>
T& Field(void* object, uint32_t offset) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(object) + offset); }
template <typename F>
F Internal(uint32_t rva) { return reinterpret_cast<F>(reinterpret_cast<uint8_t*>(g_orig) + rva); }

struct TransKey {
    float p[3], time;
};
struct RotKey {
    float q[4], time;
};
struct VisKey {
    float time;
    uint8_t visible, pad[3];
};
struct UvKey {
    float scale[2], offset[2], time;
    uint32_t interpolate;                           // (tested as bits: -0.0 counts)
};
static_assert(sizeof(TransKey) == 0x10 && sizeof(RotKey) == 0x14 && sizeof(VisKey) == 8 && sizeof(UvKey) == 0x18,
              "keys");

constexpr uint32_t kVtable = 0x93E58, kFafVtable = 0x8A7CC;
constexpr uint32_t kIndex = 0x2C, kTrans = 0x3C, kRot = 0x4C, kVis = 0x5C, kUv = 0x6C, kTotal = 0x7C, kLoop = 0x80,
                   kScaleU = 0x84, kScaleV = 0x88, kOffsetU = 0x8C, kOffsetV = 0x90;

template <typename K>
vc10::Vector<K>& Keys(uint8_t* a, uint32_t at) { return Field<vc10::Vector<K>>(a, at); }
int32_t& Index(uint8_t* a, int list) { return Field<int32_t>(a, kIndex + 4 * uint32_t(list)); }

// FUN_10028c3d: a + (b - a) t as the original weighs it.
void Lerp(const float* a, const float* b, float t, float* out)
{
    const float s = float(1.0 - (double)t);
    const float a0 = a[0], a1 = a[1], a2 = a[2];
    out[0] = float((double)a0 * s + (double)b[0] * t);
    out[1] = float((double)a1 * s + (double)b[1] * t);
    out[2] = float((double)a2 * s + (double)b[2] * t);
}

// How far `time` is from `from` to `to` (each step as the original rounds it).
float Fraction(float time, float from, float to)
{
    const float d = float((double)time - (double)from);
    return float((double)d / ((double)to - (double)from));
}

float Clamped(float time, float last) { return last < time ? last : time; }

// Walking a list's index back (while its key is later) or on (while the next is earlier). `doWhile`: the original
// steps once before looking (rotations, visibility, UV); translations look first.
template <typename K>
void Walk(int32_t& i, const K* keys, float t0, float time, bool doWhile)
{
    if (doWhile) {
        if (t0 > time) do --i; while (keys[i].time > time);
    } else {
        while (keys[i].time > time) --i;
    }
    if (t0 < time)
        while (keys[i + 1].time < time) ++i;
}

}  // namespace

// FUN_10028fde (vtable slot 3): the animation at `time` (looped or held at its end): the matrix's rotation and
// position, the visibility, the UV scale and offset.
void __fastcall Evaluate(uint8_t* a, void*, float time, float* matrix, uint8_t* visible)
{
    const float total = Field<float>(a, kTotal);
    if (total < time) time = Field<bool>(a, kLoop) ? float(xm::CrtFmod(time, total)) : total;
    auto& trans = Keys<TransKey>(a, kTrans);
    auto& rot = Keys<RotKey>(a, kRot);
    auto& vis = Keys<VisKey>(a, kVis);
    auto& uv = Keys<UvKey>(a, kUv);
    const float transTime = Clamped(time, trans.last[-1].time), rotTime = Clamped(time, rot.last[-1].time),
                visTime = Clamped(time, vis.last[-1].time), uvTime = Clamped(time, uv.last[-1].time);
    int32_t &ti = Index(a, 0), &ri = Index(a, 1), &vi = Index(a, 2), &ui = Index(a, 3);
    const float t0 = trans.first[ti].time, r0 = rot.first[ri].time, v0 = vis.first[vi].time, u0 = uv.first[ui].time;
    if (trans.size() > 1) Walk(ti, trans.first, t0, transTime, false);
    if (rot.size() > 1) Walk(ri, rot.first, r0, rotTime, true);
    if (vis.size() > 1) Walk(vi, vis.first, v0, visTime, true);
    if (visible) *visible = vis.first[vi].visible;
    if (uv.size() > 1) Walk(ui, uv.first, u0, uvTime, true);

    if (matrix) {
        const TransKey& t = trans.first[ti];
        float p[3] = {t.p[0], t.p[1], t.p[2]};
        if (t.time != transTime) Lerp(p, trans.first[ti + 1].p, Fraction(transTime, t.time, trans.first[ti + 1].time), p);
        matrix[12] = p[0];
        matrix[13] = p[1];
        matrix[14] = p[2];
        const RotKey& r = rot.first[ri];
        float q[4] = {r.q[0], r.q[1], r.q[2], r.q[3]};
        if (r.time != rotTime) xm::SlerpX87(q, rot.first[ri + 1].q, Fraction(rotTime, r.time, rot.first[ri + 1].time), q);
        xm::RotationX87(matrix, q);
    }
    if (uv.size() < 2) return;
    const UvKey& k = uv.first[ui];
    const UvKey& n = uv.first[ui + 1];
    const bool at = k.time == uvTime;
    if (!at && k.interpolate) {
        const float u = Fraction(uvTime, k.time, n.time);
        const double r = 1.0 - (double)u;
        Field<float>(a, kOffsetU) = float((double)n.offset[0] * u + (double)k.offset[0] * r);
        Field<float>(a, kOffsetV) = float((double)n.offset[1] * u + (double)k.offset[1] * r);
        Field<float>(a, kScaleU) = float((double)n.scale[0] * u + (double)k.scale[0] * r);
        Field<float>(a, kScaleV) = float((double)n.scale[1] * u + (double)k.scale[1] * r);
        return;
    }
    Field<float>(a, kOffsetU) = k.offset[0];
    Field<float>(a, kOffsetV) = k.offset[1];
    if (at) {
        Field<float>(a, kScaleU) = k.scale[0];
        Field<float>(a, kScaleV) = k.scale[1];
        return;
    }
    const float u = Fraction(uvTime, k.time, n.time);
    const double r = 1.0 - (double)u;
    Field<float>(a, kScaleU) = float((double)n.scale[0] * u + (double)k.scale[0] * r);
    Field<float>(a, kScaleV) = float((double)n.scale[1] * u + (double)k.scale[1] * r);
}

namespace {

// FUN_100294e0 (slot 4): the UV scale and offset; whether it has UV keys to animate.
bool __fastcall GetUv(uint8_t* a, void*, float* scaleU, float* scaleV, float* offsetU, float* offsetV)
{
    if (scaleU) *scaleU = Field<float>(a, kScaleU);
    if (scaleV) *scaleV = Field<float>(a, kScaleV);
    if (offsetU) *offsetU = Field<float>(a, kOffsetU);
    if (offsetV) *offsetV = Field<float>(a, kOffsetV);
    return Keys<UvKey>(a, kUv).size() > 1;
}

float __fastcall TotalTime(uint8_t* a) { return Field<float>(a, kTotal); }          // FUN_10016485 (slot 5)
bool __fastcall Looping(uint8_t* a) { return Field<bool>(a, kLoop); }              // FUN_1001647e (slot 6)
void __fastcall SetLooping(uint8_t* a, void*, bool loop) { Field<bool>(a, kLoop) = loop; }   // FUN_1001646e (slot 7)

// FUN_1001643e: the key lists, then RResource_t.
void __fastcall Destroy(uint8_t* a)
{
    Keys<UvKey>(a, kUv).release();
    Keys<VisKey>(a, kVis).release();
    Keys<RotKey>(a, kRot).release();
    Keys<TransKey>(a, kTrans).release();
    Internal<void(__fastcall*)(void*)>(0x46283)(a);  // RResource_t::~RResource_t
}

void* __fastcall DeletingDestroy(uint8_t* a, void*, uint32_t flags)   // FUN_1001665f (slot 0)
{
    Destroy(a);
    if (flags & 1) vc10::Free(a);
    return a;
}

template <typename K>
void Read(const serialize::Api& s, void* stream, const char* countName, const char* name, vc10::Vector<K>& out)
{
    int32_t count = 0;
    const void* data = nullptr;
    int32_t bytes = 0;
    s.findInt32(stream, nullptr, countName, &count, 0);
    s.findData(stream, nullptr, name, &data, &bytes, 0);
    for (int32_t i = 0; i < count; ++i) out.push_back(static_cast<const K*>(data)[i]);
}

}  // namespace

// ??0RKeyFrameAnimation_t@@QAE@PAVObjectArchive_c@fun@@@Z: from an archive (tot_time, loop and the key lists; a
// single plain UV key if it has none), then evaluated at 0.
uint8_t* __fastcall ConstructFromArchive(uint8_t* a, void*, void* archive)
{
    orig::RResource_t_RResource_t_37(a, archive);
    Field<uint32_t>(a, 0) = uint32_t(reinterpret_cast<uintptr_t>(g_orig)) + kVtable;
    for (uint32_t at : {kTrans, kRot, kVis, kUv}) Field<vc10::Vector<uint8_t>>(a, at) = {};
    Field<float>(a, kScaleU) = 1.0f;
    Field<float>(a, kScaleV) = 1.0f;
    Field<float>(a, kOffsetU) = 0.0f;
    Field<float>(a, kOffsetV) = 0.0f;
    const serialize::Api& s = serialize::Get();
    void* stream = s.getStream(archive, nullptr);
    Field<float>(a, kTotal) = 0.0f;
    Field<bool>(a, kLoop) = false;
    for (int i = 0; i < 4; ++i) Index(a, i) = 0;
    s.findFloat(stream, nullptr, "tot_time", &Field<float>(a, kTotal), 0);
    s.findBool(stream, nullptr, "loop", &Field<bool>(a, kLoop), 0);
    Read(s, stream, "num_rot_keys", "rot_keys", Keys<RotKey>(a, kRot));
    Read(s, stream, "num_trans_keys", "trans_keys", Keys<TransKey>(a, kTrans));
    {
        int32_t count = 0;
        const void* data = nullptr;
        int32_t bytes = 0;
        s.findInt32(stream, nullptr, "num_vis_keys", &count, 0);
        s.findData(stream, nullptr, "vis_keys", &data, &bytes, 0);
        for (int32_t i = 0; i < count; ++i) {
            VisKey k{};
            k.time = static_cast<const VisKey*>(data)[i].time;
            k.visible = static_cast<const VisKey*>(data)[i].visible;
            Keys<VisKey>(a, kVis).push_back(k);
        }
    }
    int32_t count = 0;
    auto& uv = Keys<UvKey>(a, kUv);
    if (s.findInt32(stream, nullptr, "num_uv_keys", &count, 0) == 0) {
        const void* data = nullptr;
        int32_t bytes = 0;
        s.findData(stream, nullptr, "uv_keys", &data, &bytes, 0);
        uv.reserve(size_t(count > 0 ? count : 0));
        for (int32_t i = 0; i < count; ++i) uv.push_back(static_cast<const UvKey*>(data)[i]);
    } else {
        uv.push_back(UvKey{{1.0f, 1.0f}, {0.0f, 0.0f}, 0.0f, 0});
    }
    Evaluate(a, nullptr, 0.0f, nullptr, nullptr);
    return a;
}

namespace {

// Each key list as data (a copy, let go after - as the original, which hands it over uncopied).
template <typename K>
void Write(const serialize::Api& s, void* stream, const char* countName, const char* name, const vc10::Vector<K>& keys)
{
    const int32_t count = int32_t(keys.last - keys.first);
    void* copy = vc10::AllocateArray(size_t(count) * sizeof(K));
    if (count) std::memcpy(copy, keys.first, size_t(count) * sizeof(K));
    s.addInt32(stream, nullptr, countName, count);
    s.addData(stream, nullptr, name, copy, count * int32_t(sizeof(K)), false, 1);
    vc10::FreeArray(copy);
}

}  // namespace

// ?Archive@RKeyFrameAnimation_t@@UBEXPAVObjectArchive_c@fun@@@Z (slot 1).
void __fastcall Archive(uint8_t* a, void*, void* archive)
{
    orig::RResource_t_Archive(a, archive);
    const serialize::Api& s = serialize::Get();
    void* stream = s.getStream(archive, nullptr);
    s.addInt32(stream, nullptr, "version", 1);
    s.addFloat(stream, nullptr, "tot_time", Field<float>(a, kTotal));
    s.addBool(stream, nullptr, "loop", Field<bool>(a, kLoop));
    Write(s, stream, "num_rot_keys", "rot_keys", Keys<RotKey>(a, kRot));
    Write(s, stream, "num_trans_keys", "trans_keys", Keys<TransKey>(a, kTrans));
    Write(s, stream, "num_vis_keys", "vis_keys", Keys<VisKey>(a, kVis));
    if (Keys<UvKey>(a, kUv).size() > 1) Write(s, stream, "num_uv_keys", "uv_keys", Keys<UvKey>(a, kUv));
}

namespace {

// FAFAnim_t: the same object under another vtable (FUN_10015686; its Archive, FUN_1001566e, adds nothing).
uint8_t* __fastcall FafConstruct(uint8_t* a, void*, void* archive)
{
    ConstructFromArchive(a, nullptr, archive);
    Field<uint32_t>(a, 0) = uint32_t(reinterpret_cast<uintptr_t>(g_orig)) + kFafVtable;
    return a;
}

void* __cdecl FafInstantiate(void* archive)         // ?Instantiate@FAFAnim_t@@SAPAV1@PAVObjectArchive_c@fun@@@Z
{
    return FafConstruct(static_cast<uint8_t*>(vc10::Allocate(0x94)), nullptr, archive);
}

}  // namespace

void Install(HMODULE orig)
{
    if (GetMode("Scene", Mode::Off) != Mode::On) return;
    g_orig = orig;
    if (!serialize::Get().complete || !serialize::Get().addData || !serialize::Get().findData) {
        Log("keyframe animations: not replaced (serialize.dll exports missing)");
        return;
    }
    struct Entry {
        uint32_t rva;
        void* target;
        const char* what;
    };
#define FN(f) reinterpret_cast<void*>(&f)
    const Entry entries[] = {
        {0x295AB, FN(ConstructFromArchive), "RKeyFrameAnimation_t::RKeyFrameAnimation_t(archive)"},
        {0x28C87, FN(Archive), "RKeyFrameAnimation_t::Archive"},
        {0x28FDE, FN(Evaluate), "RKeyFrameAnimation_t evaluated (FUN_10028fde)"},
        {0x294E0, FN(GetUv), "RKeyFrameAnimation_t UV (FUN_100294e0)"},
        {0x1647E, FN(Looping), "RKeyFrameAnimation_t looping (FUN_1001647e)"},
        {0x1646E, FN(SetLooping), "RKeyFrameAnimation_t set looping (FUN_1001646e)"},
        {0x1643E, FN(Destroy), "RKeyFrameAnimation_t destroyed (FUN_1001643e)"},
        {0x1665F, FN(DeletingDestroy), "RKeyFrameAnimation_t deleting destructor (FUN_1001665f)"},
        {0x156C3, FN(FafInstantiate), "FAFAnim_t::Instantiate"},
        {0x15686, FN(FafConstruct), "FAFAnim_t(archive) (FUN_10015686)"},
        {0x1566E, FN(Archive), "FAFAnim_t::Archive (FUN_1001566e)"},
    };
#undef FN
    int installed = 0;
    for (const Entry& e : entries) installed += Replace(orig, e.rva, e.target, e.what) ? 1 : 0;
    // Slot 5 (its length) is too short to patch: both vtables point at ours instead.
    installed += HookSlot(orig, kVtable, 5, 0x16485, reinterpret_cast<void*>(&TotalTime), "RKeyFrameAnimation_t length") ? 1 : 0;
    installed += HookSlot(orig, kFafVtable, 5, 0x16485, reinterpret_cast<void*>(&TotalTime), "FAFAnim_t length") ? 1 : 0;
    Log("keyframe animations: %d of %d functions native", installed, int(sizeof(entries) / sizeof(entries[0])) + 2);
}

}  // namespace rnative::keyframe
