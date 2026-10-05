// Character animations natively (randy-vk.ini [Native] Scene=on): CATKeyframeAnimData_t loaded from the game's data
// stream - plain, or compressed (delta-coded zlib channels: ints, key times, quaternions, position ranges, positions);
// CATKeyframeAnim_t (a playing instance of it: time, layers) and CATAnimBlend_t (two blended), with their CATAnim_t
// base. The sampling itself is cat_anim.cpp's (Anim mode).
//
// CATAnim_t (an RResource_t, vtable 0x95B10): +0x2C its version (changes), +0x30 a name (std::string).
// CATKeyframeAnimData_t (an RResource_t, vtable 0x95B94): +0x2C length, +0x30 radius, +0x34 tracks {count, 0x10 each:
// rotation keys {count, 0x14 each: time, x y z w}, position keys {count, 0x10 each: time, x y z}}, +0x3C {count,
// layer masks per track}, +0x44 skeleton id.
// CATKeyframeAnim_t (a CATAnim_t, vtable 0x95BA4): +0x4C its data, +0x50 time, +0x54 layers, +0x58 {count, a byte per
// track: on in the layers}.
// CATAnimBlend_t (a CATAnim_t, vtable 0x95B40): +0x50 / +0x58 the two, +0x5C {count, which of them has the track:
// 1 / 2 / 3}, +0x64 {count, a byte per track}, +0x6C the blend.
#include "native/cat_anim_data.h"
#include "native/orig_api.gen.h"
#include "native/vc10.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace rnative::cat_anim_data {

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
void SetVtable(void* o, uint32_t rva) { Field<uintptr_t>(o, 0) = reinterpret_cast<uintptr_t>(g_orig) + rva; }

constexpr uint32_t kAnimVtable = 0x95B10, kDataVtable = 0x95B94, kKeyframeVtable = 0x95BA4, kBlendVtable = 0x95B40;
constexpr uint32_t kChanges = 0x2C, kName = 0x30;

struct Array {
    int32_t count;
    void* data;
};

template <size_t Size, typename Init>
void ResizePlain(Array* a, int32_t n, Init init)
{
    if (a->count == n) return;
    vc10::FreeArray(a->data);
    a->count = n;
    if (!n) {
        a->data = nullptr;
        return;
    }
    auto* p = static_cast<uint8_t*>(vc10::AllocateArray(size_t(uint32_t(n)) * Size));
    if (p)
        for (int32_t i = 0; i < n; ++i) init(p + i * Size);
    a->data = p;
}

void ResizeInts(Array* a, int32_t n) { Internal<void(__fastcall*)(void*, void*, int32_t)>(0x52816)(a, nullptr, n); }

// ---- CATAnim_t ----

void* __fastcall AnimConstruct(uint8_t* a, void*, const char* name)   // FUN_100509f2
{
    Internal<void*(__fastcall*)(void*, void*, const char*)>(0x46311)(a, nullptr, name);
    SetVtable(a, kAnimVtable);
    vc10::String& s = Field<vc10::String>(a, kName);
    s.size = 0;
    s.capacity = 15;
    s.buffer[0] = 0;
    Field<int32_t>(a, kChanges) = 0;
    return a;
}

void __fastcall AnimDestroy(uint8_t* a)                // FUN_10050a1f
{
    SetVtable(a, kAnimVtable);
    Field<vc10::String>(a, kName).release();
    Internal<void(__fastcall*)(void*)>(0x46283)(a);    // ~RResource_t
}

void* __fastcall AnimDelete(uint8_t* a, void*, uint8_t flags)   // FUN_10050a3c
{
    AnimDestroy(a);
    if (flags & 1) vc10::Free(a);
    return a;
}

void __fastcall SetName(uint8_t* a, void*, const char* name)   // FUN_100504d2
{
    if (!name) name = "";
    Field<vc10::String>(a, kName).assign(name, std::strlen(name));
}

// ---- CATKeyframeAnimData_t ----

constexpr uint32_t kLength = 0x2C, kRadius = 0x30, kTracks = 0x34, kMasks = 0x3C, kSkeleton = 0x44;
constexpr uint32_t kTrackSize = 0x10, kRotKey = 0x14, kPosKey = 0x10;

void* __fastcall TrackConstruct(uint8_t* t) { return std::memset(t, 0, kTrackSize), t; }   // FUN_100529a4
void __fastcall TrackDestroy(uint8_t* t)                // FUN_10052943
{
    vc10::FreeArray(Field<void*>(t, 0xC));
    vc10::FreeArray(Field<void*>(t, 4));
}

void* __fastcall TracksDelete(void* a, void*, uint8_t flags)   // FUN_1005295a (vector deleting destructor)
{
    if (!(flags & 2)) {
        TrackDestroy(static_cast<uint8_t*>(a));
        if (flags & 1) vc10::Free(a);
        return a;
    }
    uint32_t* block = static_cast<uint32_t*>(a) - 1;
    for (uint32_t i = block[0]; i-- > 0;) TrackDestroy(static_cast<uint8_t*>(a) + i * kTrackSize);
    if (flags & 1) vc10::FreeArray(block);
    return block;
}

void __fastcall ResizeTracks(Array* a, void*, int32_t n)   // FUN_100529d9
{
    if (a->count == n) return;
    void* fresh = nullptr;
    if (a->data) TracksDelete(a->data, nullptr, 3);
    a->count = n;
    if (n) {
        uint32_t* block = static_cast<uint32_t*>(vc10::AllocateArray(size_t(uint32_t(n)) * kTrackSize + 4));
        block[0] = uint32_t(n);
        for (int32_t i = 0; i < n; ++i) TrackConstruct(reinterpret_cast<uint8_t*>(block + 1) + i * kTrackSize);
        fresh = block + 1;
    }
    a->data = fresh;
}

void __fastcall ResizeRotKeys(Array* a, void*, int32_t n)   // FUN_10052859: x y z 0, w 1
{
    ResizePlain<kRotKey>(a, n, [](uint8_t* k) {
        Field<float>(k, 4) = Field<float>(k, 8) = Field<float>(k, 0xC) = 0.0f;
        Field<float>(k, 0x10) = 1.0f;
    });
}
void __fastcall ResizePosKeys(Array* a, void*, int32_t n)   // FUN_100528c5
{
    ResizePlain<kPosKey>(a, n, [](uint8_t* k) { Field<float>(k, 4) = Field<float>(k, 8) = Field<float>(k, 0xC) = 0.0f; });
}

struct Reader {
    void* io;
    int32_t Int() { return Virtual<int32_t>(io, 5); }
    int8_t Char() { return Virtual<int8_t>(io, 9); }
    float Float() { return Virtual<float>(io, 11); }
    void Bytes(void* to, int32_t n) { Virtual(io, 0, to, n); }
};

// FUN_1005bb82 / FUN_1005b96a: values `bits` wide over `channels` zlib-compressed byte streams, read round-robin,
// each channel's values big-endian deltas onto its running sum.
class Decoder {
public:
    Decoder(int32_t bits, Reader& in, uint32_t channels)
        : bits_(bits), mask_(bits == 32 ? 0xFFFFFFFFu : (1u << bits) - 1), bytes_(uint32_t(bits + 7) >> 3)
    {
        for (uint32_t i = 0; i < channels; ++i) {
            const int32_t packed = in.Int();
            uint32_t size = uint32_t(in.Int());
            std::vector<uint8_t> source(static_cast<size_t>(uint32_t(packed)));
            in.Bytes(source.data(), packed);
            Channel c;
            c.data.assign(size, 0);
            // zlib's uncompress (statically linked into the original: FUN_1005c270)
            Internal<int(__cdecl*)(void*, uint32_t*, const void*, int32_t)>(0x5C270)(c.data.data(), &size,
                                                                                    source.data(), packed);
            channels_.push_back(std::move(c));
        }
    }
    uint32_t Next()
    {
        Channel& c = channels_[channel_];
        const uint8_t* p = c.data.data() + offset_;
        uint32_t v;
        switch (bytes_) {
        case 1: v = p[0]; break;
        case 2: v = uint32_t(p[0]) << 8 | p[1]; break;
        case 3: v = (uint32_t(p[0]) << 8 | p[1]) << 8 | p[2]; break;
        case 4: v = ((uint32_t(p[0]) << 8 | p[1]) << 8 | p[2]) << 8 | p[3]; break;
        default: v = 0; break;
        }
        c.sum += v;
        const uint32_t r = c.sum & mask_;
        if (++channel_ == channels_.size()) {
            channel_ = 0;
            offset_ += bytes_;
        }
        return r;
    }

private:
    struct Channel {
        std::vector<uint8_t> data;
        uint32_t sum = 0;
    };
    int32_t bits_;
    uint32_t mask_, bytes_;
    uint32_t channel_ = 0, offset_ = 0;
    std::vector<Channel> channels_;
};

float AsFloat(uint32_t bits)
{
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}
float Unsigned(uint32_t v) { return float(double(v)); }   // fild, +2^32 when "negative"

[[noreturn]] void ThrowCatError(const char* text)
{
    vc10::String message;
    message.init();
    message.allocator = 0;
    message.assign(text, std::strlen(text));
    alignas(8) uint8_t error[0x10];
    using CtorFn = void*(__fastcall*)(void*, void*, const vc10::String*);
    reinterpret_cast<CtorFn>(GetProcAddress(
        g_orig, "??0CATError_t@@QAE@ABV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z"))(
        error, nullptr, &message);
    message.release();
    using ThrowFn = void(__stdcall*)(void*, void*);
    reinterpret_cast<ThrowFn>(GetProcAddress(GetModuleHandleA("msvcr100.dll"), "_CxxThrowException"))(
        error, reinterpret_cast<uint8_t*>(g_orig) + 0xA7530);
    __builtin_unreachable();
}

void __fastcall DataDestroy(uint8_t* d);

uint8_t* TrackAt(uint8_t* d, int32_t i) { return static_cast<uint8_t*>(Field<Array>(d, kTracks).data) + i * kTrackSize; }

void* __fastcall DataConstruct(uint8_t* d, void*, void* io)   // CATKeyframeAnimData_t::CATKeyframeAnimData_t
{
    Internal<void*(__fastcall*)(void*, void*, const char*)>(0x46311)(d, nullptr, "CATKeyframeAnimData_t");
    SetVtable(d, kDataVtable);
    Field<Array>(d, kTracks) = {0, nullptr};
    ResizeTracks(&Field<Array>(d, kTracks), nullptr, 0);
    Field<Array>(d, kMasks) = {0, nullptr};
    Reader in{io};
    if (in.Int() != 3) {
        DataDestroy(d);
        ThrowCatError("file is not a CATAnim");
    }
    int32_t version = in.Int();
    const bool compressed = (version & 0x1000000) != 0;
    version &= ~0x1000000;
    if (!(0x104 < version)) {
        DataDestroy(d);
        ThrowCatError("file is not a supported version");
    }
    const bool oldTimes = version < 0x106;
    Field<float>(d, kLength) = oldTimes ? float(in.Int()) : in.Float();
    Field<int32_t>(d, kSkeleton) = in.Int();
    Field<float>(d, kRadius) = in.Float();
    const int32_t tracks = in.Int();
    ResizeTracks(&Field<Array>(d, kTracks), nullptr, tracks);
    ResizeInts(&Field<Array>(d, kMasks), tracks);
    int32_t* masks = static_cast<int32_t*>(Field<Array>(d, kMasks).data);
    if (!compressed) {
        for (int32_t t = 0; t < Field<int32_t>(d, kTracks); ++t) {
            const int32_t index = in.Int();
            uint8_t* track = TrackAt(d, index);
            in.Int();
            masks[index] = in.Int();
            const int32_t keys = in.Int();
            ResizeRotKeys(&Field<Array>(track, 0), nullptr, keys);
            ResizePosKeys(&Field<Array>(track, 8), nullptr, keys);
            for (int32_t k = 0; k < keys; ++k) {
                float* rot = reinterpret_cast<float*>(Field<uint8_t*>(track, 4) + k * kRotKey);
                float* pos = reinterpret_cast<float*>(Field<uint8_t*>(track, 0xC) + k * kPosKey);
                const float time = oldTimes ? float(in.Int()) : in.Float();
                rot[0] = time;
                for (int j = 1; j < 5; ++j) rot[j] = in.Float();
                pos[0] = time;
                for (int j = 1; j < 4; ++j) pos[j] = in.Float();
            }
        }
    } else {
        const int32_t rotBits = in.Char(), posBits = in.Char();
        Decoder ints(32, in, 1), times(oldTimes ? 20 : 32, in, 1), rots(rotBits, in, 4), ranges(32, in, 1),
            positions(posBits, in, 3);
        auto time = [&] {
            const uint32_t v = times.Next();
            return oldTimes ? Unsigned(v) : AsFloat(v);
        };
        for (int32_t t = 0; t < Field<int32_t>(d, kTracks); ++t) {
            const uint32_t index = ints.Next();
            uint8_t* track = TrackAt(d, int32_t(index));
            ints.Next();
            masks[index] = int32_t(ints.Next());
            ResizeRotKeys(&Field<Array>(track, 0), nullptr, int32_t(ints.Next()));
            const float half = float(int32_t(1u << ((rotBits - 1) & 31)));
            for (int32_t k = 0; k < Field<int32_t>(track, 0); ++k) {
                float* rot = reinterpret_cast<float*>(Field<uint8_t*>(track, 4) + k * kRotKey);
                rot[0] = time();
                float q[4];
                for (float& c : q) c = float(double(rots.Next()) - double(half));
                const float sum = float(((double(q[3]) * q[3] + double(q[2]) * q[2]) + double(q[0]) * q[0]) +
                                        double(q[1]) * q[1]);
                const float length = float(std::sqrt(double(sum)));
                const float inv = float(1.0 / double(length));
                for (int j = 0; j < 4; ++j) rot[j + 1] = inv * q[j];
            }
            float r[6];
            for (float& v : r) v = AsFloat(ranges.Next());   // min x, max x, min y, max y, min z, max z
            ResizePosKeys(&Field<Array>(track, 8), nullptr, int32_t(ints.Next()));
            const float steps = float(int32_t((1u << (posBits & 31)) - 1));
            for (int32_t k = 0; k < Field<int32_t>(track, 8); ++k) {
                float* pos = reinterpret_cast<float*>(Field<uint8_t*>(track, 0xC) + k * kPosKey);
                pos[0] = time();
                const float a = Unsigned(positions.Next()), b = Unsigned(positions.Next());
                const uint32_t c = positions.Next();
                float f = float(double(a) / double(steps));
                pos[1] = float((double(r[1]) - r[0]) * f + r[0]);
                f = float(double(b) / double(steps));
                pos[2] = float((double(r[3]) - r[2]) * f + r[2]);
                f = float(double(c) / double(steps));         // (not rounded to a float first)
                pos[3] = float((double(r[5]) - r[4]) * f + r[4]);
            }
        }
    }
    if (oldTimes) {                                    // times strictly increasing, as the original repairs them
        for (int32_t t = 0; t != Field<int32_t>(d, kTracks); ++t) {
            uint8_t* track = TrackAt(d, t);
            float previous = -1.0f;
            for (int32_t k = 0; k != Field<int32_t>(track, 0); ++k) {
                float& time = *reinterpret_cast<float*>(Field<uint8_t*>(track, 4) + k * kRotKey);
                if (time <= previous) {
                    std::fprintf(stderr, "broken keyframe data in stream, time for rot-key %d must be > %f, but was %f\n",
                                 k, double(previous), double(time));
                    time = previous + 1.0f;
                }
                previous = time;
            }
            previous = -1.0f;
            for (int32_t k = 0; k != Field<int32_t>(track, 8); ++k) {
                float& time = *reinterpret_cast<float*>(Field<uint8_t*>(track, 0xC) + k * kPosKey);
                if (time <= previous) {
                    std::fprintf(stderr,
                                 "broken keyframe data in stream, time for trans-key %d must be > %f, but was %f\n", k,
                                 double(previous), double(time));
                    time = previous + 1.0f;
                }
                previous = time;
            }
        }
    }
    return d;
}

void __fastcall DataDestroy(uint8_t* d)                // FUN_10051f68
{
    SetVtable(d, kDataVtable);
    vc10::FreeArray(Field<void*>(d, kMasks + 4));
    if (void* t = Field<void*>(d, kTracks + 4)) TracksDelete(t, nullptr, 3);
    Internal<void(__fastcall*)(void*)>(0x46283)(d);
}

void* __fastcall DataDelete(uint8_t* d, void*, uint8_t flags)   // FUN_10052a73
{
    DataDestroy(d);
    if (flags & 1) vc10::Free(d);
    return d;
}

// ---- CATKeyframeAnim_t ----

constexpr uint32_t kData = 0x4C, kTime = 0x50, kLayers = 0x54, kOn = 0x58;

void __fastcall ResizeBytes(Array* a, void*, int32_t n)   // FUN_100509bf
{
    if (a->count == n) return;
    vc10::FreeArray(a->data);
    a->count = n;
    a->data = n ? vc10::AllocateArray(size_t(uint32_t(n))) : nullptr;
}

void __fastcall UpdateLayers(uint8_t* a)               // FUN_10051f19: tracks on in the layers
{
    uint8_t* data = Field<uint8_t*>(a, kData);
    ResizeBytes(&Field<Array>(a, kOn), nullptr, Field<int32_t>(data, kTracks));
    for (int32_t i = 0; i < Field<int32_t>(Field<uint8_t*>(a, kData), kTracks); ++i)
        Field<uint8_t*>(a, kOn + 4)[i] =
            (Field<uint32_t>(a, kLayers) & static_cast<uint32_t*>(Field<Array>(Field<uint8_t*>(a, kData), kMasks).data)[i]) != 0;
}

void* __fastcall KeyframeConstruct(uint8_t* a, void*, uint8_t* data, uint32_t layers)
{
    AnimConstruct(a, nullptr, "CATKeyframeAnim_t");
    SetVtable(a, kKeyframeVtable);
    Field<Array>(a, kOn) = {0, nullptr};
    Field<uint8_t*>(a, kData) = data;
    orig::RResource_t_AddRefRResource(data);
    Field<float>(a, kTime) = 0.0f;
    Field<uint32_t>(a, kLayers) = layers;
    UpdateLayers(a);
    Field<int32_t>(a, kChanges) = 0;
    SetName(a, nullptr, "CATAnimBlend_t");             // (the original's name for both)
    return a;
}

void __fastcall KeyframeDestroy(uint8_t* a)            // FUN_10052021
{
    SetVtable(a, kKeyframeVtable);
    orig::RResource_t_ReleaseRResource(Field<void*>(a, kData));
    vc10::FreeArray(Field<void*>(a, kOn + 4));
    AnimDestroy(a);
}

void* __fastcall KeyframeDelete(uint8_t* a, void*, uint8_t flags)   // FUN_10052a92
{
    KeyframeDestroy(a);
    if (flags & 1) vc10::Free(a);
    return a;
}

void __fastcall SetTime(uint8_t* a, void*, float time)
{
    ++Field<int32_t>(a, kChanges);
    Field<float>(a, kTime) = time;
}

void __fastcall SetLayers(uint8_t* a, void*, uint32_t layers)
{
    if (Field<uint32_t>(a, kLayers) == layers) return;
    Field<uint32_t>(a, kLayers) = layers;
    UpdateLayers(a);
}

int32_t __fastcall KeyframeBones(uint8_t* a) { return Field<int32_t>(Field<uint8_t*>(a, kData), kTracks); }   // slot 3
float __fastcall KeyframeRadius(uint8_t* a) { return Field<float>(Field<uint8_t*>(a, kData), kRadius); }      // slot 7
int32_t __fastcall KeyframeSkeleton(uint8_t* a) { return Field<int32_t>(Field<uint8_t*>(a, kData), kSkeleton); }   // 9

void __fastcall KeyframeRotation(uint8_t* a, void*, float* out, int32_t bone)   // slot 5 (FUN_10051ee1)
{
    Internal<void(__fastcall*)(void*, void*, float*, int32_t, float)>(0x51D2A)(Field<void*>(a, kData), nullptr, out, bone,
                                                                            Field<float>(a, kTime));
}
void __fastcall KeyframePosition(uint8_t* a, void*, float* out, int32_t bone)   // slot 6 (FUN_10051efd)
{
    Internal<void(__fastcall*)(void*, void*, float*, int32_t, float)>(0x51DF4)(Field<void*>(a, kData), nullptr, out, bone,
                                                                            Field<float>(a, kTime));
}

// ---- CATAnimBlend_t ----

constexpr uint32_t kFirst = 0x50, kSecond = 0x58, kWhich = 0x5C, kHas = 0x64, kBlend = 0x6C;

void __fastcall MergeTracks(uint8_t* b)                // FUN_10050742: which of the two has each track
{
    void* first = Field<void*>(b, kFirst);
    void* second = Field<void*>(b, kSecond);
    if (!first && !second) return;
    const Array* on1 = first ? Virtual<const Array*>(first, 4) : nullptr;
    const Array* on2 = nullptr;
    int32_t n = on1 ? on1->count : 0;
    if (second) {
        on2 = Virtual<const Array*>(second, 4);
        if (n < on2->count) n = on2->count;
    }
    ResizeBytes(&Field<Array>(b, kHas), nullptr, n);
    ResizeInts(&Field<Array>(b, kWhich), n);
    for (int32_t i = n - 1; i >= 0; --i) {
        uint32_t which = 0;
        if (on1 && i < on1->count && static_cast<uint8_t*>(on1->data)[i]) which = 1;
        if (on2 && i < on2->count && static_cast<uint8_t*>(on2->data)[i]) which |= 2;
        static_cast<uint32_t*>(Field<void*>(b, kWhich + 4))[i] = which;
        Field<uint8_t*>(b, kHas + 4)[i] = which != 0;
    }
}

template <uint32_t Mine, uint32_t Other>
bool SetChild(uint8_t* b, void* anim)
{
    if (Field<void*>(b, Mine) == anim) return true;
    if (void* other = Field<void*>(b, Other)) {
        if (anim) {
            if (Virtual<int32_t>(anim, 3) != Virtual<int32_t>(other, 3)) return false;
            if (Virtual<int32_t>(anim, 9) != Virtual<int32_t>(other, 9)) return false;
            orig::RResource_t_AddRefRResource(anim);
        }
    } else if (anim) {
        orig::RResource_t_AddRefRResource(anim);
    }
    if (void* old = Field<void*>(b, Mine)) orig::RResource_t_ReleaseRResource(old);
    Field<void*>(b, Mine) = anim;
    MergeTracks(b);
    ++Field<int32_t>(b, kChanges);
    return true;
}
bool __fastcall SetAnim1(uint8_t* b, void*, void* anim) { return SetChild<kFirst, kSecond>(b, anim); }
bool __fastcall SetAnim2(uint8_t* b, void*, void* anim) { return SetChild<kSecond, kFirst>(b, anim); }

void __fastcall SetBlend(uint8_t* b, void*, float blend)
{
    ++Field<int32_t>(b, kChanges);
    Field<float>(b, kBlend) = blend;
}

void* __fastcall BlendConstruct(uint8_t* b, void*, void* first, void* second, float blend)
{
    AnimConstruct(b, nullptr, "CATAnimBlend_t");
    SetVtable(b, kBlendVtable);
    std::memset(b + 0x4C, 0, 0x24);
    if (first) SetAnim1(b, nullptr, first);
    if (second) SetAnim2(b, nullptr, second);
    ++Field<int32_t>(b, kChanges);
    Field<float>(b, kBlend) = blend;
    SetName(b, nullptr, "CATAnimBlend_t");
    return b;
}

void __fastcall BlendDestroy(uint8_t* b)               // FUN_1005095f
{
    SetVtable(b, kBlendVtable);
    if (void* a = Field<void*>(b, kFirst)) orig::RResource_t_ReleaseRResource(a);
    if (void* a = Field<void*>(b, kSecond)) orig::RResource_t_ReleaseRResource(a);
    vc10::FreeArray(Field<void*>(b, kHas + 4));
    vc10::FreeArray(Field<void*>(b, kWhich + 4));
    AnimDestroy(b);
}

void* __fastcall BlendDelete(uint8_t* b, void*, uint8_t flags)   // FUN_10050aec
{
    BlendDestroy(b);
    if (flags & 1) vc10::Free(b);
    return b;
}

int32_t __fastcall BlendBones(uint8_t* b)              // slot 3 (FUN_1005050a): the first's, or the second's
{
    if (void* a = Field<void*>(b, kFirst)) return Virtual<int32_t>(a, 3);
    if (void* a = Field<void*>(b, kSecond)) return Virtual<int32_t>(a, 3);
    return 0;
}
int32_t __fastcall BlendSkeleton(uint8_t* b)           // slot 9 (FUN_10050a9d)
{
    if (void* a = Field<void*>(b, kFirst)) return Virtual<int32_t>(a, 9);
    if (void* a = Field<void*>(b, kSecond)) return Virtual<int32_t>(a, 9);
    return 0;
}
bool __fastcall BlendRest(uint8_t* b)                  // slot 10 (FUN_10050abc): either in its rest pose
{
    if (void* a = Field<void*>(b, kSecond))
        if (Virtual<uint8_t>(a, 10)) return true;
    if (void* a = Field<void*>(b, kFirst))
        if (Virtual<uint8_t>(a, 10)) return true;
    return false;
}

// The blend's accessors and clear, and the keyframe data's, that the rest of the class did not cover.
void* __fastcall GetAnim2(void* self, void*) { return Field<void*>(self, 0x58); }               // CATAnimBlend_t::GetAnim2
void* __fastcall BlendAt64(void* self, void*) { return static_cast<uint8_t*>(self) + 0x64; }    // FUN_10050a5b
void __fastcall BlendClear(void* self, void*)                                                   // FUN_10050ff4
{
    if (void* p = Field<void*>(self, 0)) vc10::Free(p);
    Field<void*>(self, 0) = nullptr;
    Field<void*>(self, 4) = nullptr;
    Field<void*>(self, 8) = nullptr;
}
void* __fastcall DataAt58(void* self, void*) { return static_cast<uint8_t*>(self) + 0x58; }     // FUN_10052a5e
bool __fastcall ReturnTrue(void*, void*) { return true; }                                        // FUN_10052a69

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
        {0x509F2, FN(AnimConstruct), "CATAnim_t::CATAnim_t (FUN_100509f2)"},
        {0x50A1F, FN(AnimDestroy), "CATAnim_t::~CATAnim_t (FUN_10050a1f)"},
        {0x50A3C, FN(AnimDelete), "CATAnim_t deleting destructor (FUN_10050a3c)"},
        {0x504D2, FN(SetName), "CATAnim_t name (FUN_100504d2)"},
        {0x529A4, FN(TrackConstruct), "CATKeyframeAnimData_t track (FUN_100529a4)"},
        {0x52943, FN(TrackDestroy), "CATKeyframeAnimData_t track destructor (FUN_10052943)"},
        {0x5295A, FN(TracksDelete), "CATKeyframeAnimData_t tracks deleting (FUN_1005295a)"},
        {0x529D9, FN(ResizeTracks), "CATKeyframeAnimData_t tracks resize (FUN_100529d9)"},
        {0x52859, FN(ResizeRotKeys), "rotation keys resize (FUN_10052859)"},
        {0x528C5, FN(ResizePosKeys), "position keys resize (FUN_100528c5)"},
        {0x52068, FN(DataConstruct), "CATKeyframeAnimData_t::CATKeyframeAnimData_t (the loader)"},
        {0x51F68, FN(DataDestroy), "CATKeyframeAnimData_t::~CATKeyframeAnimData_t (FUN_10051f68)"},
        {0x52A73, FN(DataDelete), "CATKeyframeAnimData_t deleting destructor (FUN_10052a73)"},
        {0x509BF, FN(ResizeBytes), "byte array resize (FUN_100509bf)"},
        {0x51F19, FN(UpdateLayers), "CATKeyframeAnim_t tracks in the layers (FUN_10051f19)"},
        {0x51FB2, FN(KeyframeConstruct), "CATKeyframeAnim_t::CATKeyframeAnim_t"},
        {0x52021, FN(KeyframeDestroy), "CATKeyframeAnim_t::~CATKeyframeAnim_t (FUN_10052021)"},
        {0x52A92, FN(KeyframeDelete), "CATKeyframeAnim_t deleting destructor (FUN_10052a92)"},
        {0x51CFC, FN(SetTime), "CATKeyframeAnim_t::SetTime"},
        {0x51F51, FN(SetLayers), "CATKeyframeAnim_t::SetLayers"},
        {0x52A6C, FN(KeyframeBones), "CATKeyframeAnim_t bone count (FUN_10052a6c)"},
        {0x51D23, FN(KeyframeRadius), "CATKeyframeAnim_t radius (FUN_10051d23)"},
        {0x52A62, FN(KeyframeSkeleton), "CATKeyframeAnim_t skeleton (FUN_10052a62)"},
        {0x51EE1, FN(KeyframeRotation), "CATKeyframeAnim_t rotation (FUN_10051ee1)"},
        {0x51EFD, FN(KeyframePosition), "CATKeyframeAnim_t position (FUN_10051efd)"},
        {0x50742, FN(MergeTracks), "CATAnimBlend_t tracks (FUN_10050742)"},
        {0x507E5, FN(SetAnim1), "CATAnimBlend_t::SetAnim1"},
        {0x5085C, FN(SetAnim2), "CATAnimBlend_t::SetAnim2"},
        {0x504F6, FN(SetBlend), "CATAnimBlend_t::SetBlend"},
        {0x508D3, FN(BlendConstruct), "CATAnimBlend_t::CATAnimBlend_t"},
        {0x5095F, FN(BlendDestroy), "CATAnimBlend_t::~CATAnimBlend_t (FUN_1005095f)"},
        {0x50AEC, FN(BlendDelete), "CATAnimBlend_t deleting destructor (FUN_10050aec)"},
        {0x5050A, FN(BlendBones), "CATAnimBlend_t bone count (FUN_1005050a)"},
        {0x50A9D, FN(BlendSkeleton), "CATAnimBlend_t skeleton (FUN_10050a9d)"},
        {0x50ABC, FN(BlendRest), "CATAnimBlend_t rest pose (FUN_10050abc)"},
        {0x504F2, FN(GetAnim2), "CATAnimBlend_t::GetAnim2"},
        {0x50A5B, FN(BlendAt64), "CATAnimBlend_t field (FUN_10050a5b)"},
        {0x50FF4, FN(BlendClear), "CATAnimBlend_t clear (FUN_10050ff4)"},
        {0x52A5E, FN(DataAt58), "CATKeyframeAnimData_t field (FUN_10052a5e)"},
        {0x52A69, FN(ReturnTrue), "CATKeyframeAnimData_t flag (FUN_10052a69)"},
    };
#undef FN
    int installed = 0;
    for (const Entry& e : entries) installed += Replace(orig, e.rva, e.target, e.what) ? 1 : 0;
    Log("character animations: %d of %d functions native", installed, int(sizeof(entries) / sizeof(entries[0])));
}

}  // namespace rnative::cat_anim_data
