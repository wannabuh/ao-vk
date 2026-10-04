// Character animation on the game's thread (see cat_anim.h). Same arithmetic as the original, in single precision
// where it kept x87 extended precision (differences in the last bits).
#include "native/cat_anim.h"
#include "native/cat.h"

#include <cmath>

namespace rnative::anim {

namespace {

constexpr float kSnap = 0.01f;                      // _DAT_10095b88 (a double)
constexpr double kLinear = 1e-6;                    // _DAT_10095db0

// The keys before / after `time` (as the original's binary search: past the last key or a single key: the first).
template <typename Key>
const Key* Find(const Key* keys, int32_t count, float time, const Key** next, float* t)
{
    *t = 0.0f;
    int32_t last = count - 1, lo = 0;
    *next = keys;
    if (last != 0 && time <= keys[last].time) {
        int32_t hi = last;
        while (hi - lo > 1) {
            int32_t mid = (hi + lo) / 2;
            if (keys[mid].time <= time) lo = mid;
            else hi = mid;
        }
        *t = (time - keys[lo].time) / (keys[hi].time - keys[lo].time);
        *next = keys + hi;
    }
    return keys + lo;
}

}  // namespace

void Slerp(const float a[4], const float b[4], float t, float out[4])
{
    float dot = a[3] * b[3] + a[2] * b[2] + a[1] * b[1] + a[0] * b[0];
    bool flip = dot < 0.0f;
    if (flip) dot = -dot;
    float wa, wb;
    if (double(1.0f - dot) >= kLinear) {
        float theta = float(std::acos(double(dot)));
        float inv = 1.0f / float(std::sin(double(theta)));
        wa = float(std::sin(double(theta - t * theta))) * inv;
        wb = float(std::sin(double(t * theta))) * inv;
    } else {
        wa = 1.0f - t;
        wb = t;
    }
    if (flip) wb = -wb;
    out[0] = wa * a[0] + wb * b[0];
    out[1] = wb * b[1] + wa * a[1];
    out[2] = wb * b[2] + wa * a[2];
    out[3] = wa * a[3] + b[3] * wb;
}

void SampleRotation(const Track& track, float time, float out[4])
{
    const RotationKey* next;
    float t;
    const RotationKey* key = Find(track.rotations, track.rotationCount, time, &next, &t);
    if (t >= kSnap) {
        Slerp(key->q, next->q, t, out);
    } else {
        out[0] = key->q[0], out[1] = key->q[1], out[2] = key->q[2], out[3] = key->q[3];
    }
}

void SamplePosition(const Track& track, float time, float out[3])
{
    const PositionKey* next;
    float t;
    const PositionKey* key = Find(track.positions, track.positionCount, time, &next, &t);
    if (t >= kSnap) {
        float s = 1.0f - t;
        out[0] = key->p[0] * s + t * next->p[0];
        out[1] = t * next->p[1] + key->p[1] * s;
        out[2] = key->p[2] * s + next->p[2] * t;
    } else {
        out[0] = key->p[0], out[1] = key->p[1], out[2] = key->p[2];
    }
}

void BoneMatrix(const float parent[12], const float q[4], const float position[3], float scale, float out[12])
{
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float x2 = x * 2.0f, y2 = y * 2.0f;
    const float xx = x2 * x, xy = x2 * y, xz = x2 * z, xw = w * x2;
    const float yy = y2 * y, yz = z * y2, yw = y2 * w;
    const float zz = z * z * 2.0f, zw = z * 2.0f * w;
    // The local rotation's rows, each through the parent's 3x3.
    const float r[3][3] = {{(1.0f - yy) - zz, zw + xy, xz - yw},
                           {xy - zw, (1.0f - xx) - zz, yz + xw},
                           {yw + xz, yz - xw, (1.0f - xx) - yy}};
    const float* p = parent;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            out[i * 3 + j] = p[6 + j] * r[i][2] + p[j] * r[i][0] + p[3 + j] * r[i][1];
    const float tx = position[0] * scale, ty = position[1] * scale, tz = position[2] * scale;
    for (int j = 0; j < 3; ++j)
        out[9 + j] = p[6 + j] * tz + p[j] * tx + p[3 + j] * ty + p[9 + j];
}

namespace {

using AnimGetFn = void(__fastcall*)(void* anim, void* edx, float* out, int32_t bone);   // CATAnim_t +0x14 / +0x18
using Controller1Fn = void(__cdecl*)(void* render, float* parent, float* q, float* position, void* user);
using Controller2Fn = void(__cdecl*)(float* out, void* render, float* parent, float* q, float* position, void* user);

uintptr_t g_keyframeVtable;                         // CATKeyframeAnim_t's: sampled here directly
void __fastcall HierarchyHook(void* render, void*, float* parent, int32_t bone, float scale);

void __fastcall RotationHook(const void* data, void*, float* out, int32_t bone, float time)
{
    SampleRotation(At<const Track*>(data, kDataTracks)[bone], time, out);
}

void __fastcall PositionHook(const void* data, void*, float* out, int32_t bone, float time)
{
    SamplePosition(At<const Track*>(data, kDataTracks)[bone], time, out);
}

// CATRender_t: +0x04 mesh, +0x08 animation, +0x18 bone matrices, +0x20 bone controllers (0x10 each: type, user, fn1,
// fn2). CATMesh_t +0x44: bones (0x28 each: +0x1C child scale, +0x20 child count, +0x24 children).
void __fastcall HierarchyHook(void* render, void*, float* parent, int32_t bone, float scale)
{
    float q[4] = {0.0f, 0.0f, 0.0f, 1.0f}, position[3] = {0.0f, 0.0f, 0.0f};
    void* animation = At<void*>(render, cat::kRenderAnim);
    void** vtable = *static_cast<void***>(animation);
    if (reinterpret_cast<uintptr_t>(vtable) == g_keyframeVtable) {
        const void* data = At<const void*>(animation, 0x4C);
        const float time = At<float>(animation, 0x50);
        const Track& track = At<const Track*>(data, kDataTracks)[bone];
        SampleRotation(track, time, q);
        SamplePosition(track, time, position);
    } else {
        reinterpret_cast<AnimGetFn>(vtable[0x14 / 4])(animation, nullptr, q, bone);
        reinterpret_cast<AnimGetFn>(vtable[0x18 / 4])(animation, nullptr, position, bone);
    }
    float* out = At<float*>(render, cat::kRenderBones) + bone * 12;
    auto* controller = At<uint8_t*>(render, 0x20) + bone * 0x10;
    const int32_t type = At<int32_t>(controller, 0);
    if (type == 2) {                                 // the controller makes the matrix
        for (float& v : position) v *= scale;
        At<Controller2Fn>(controller, 0xC)(out, render, parent, q, position, At<void*>(controller, 4));
    } else {
        if (type == 1)                               // the controller adjusts rotation / position first
            At<Controller1Fn>(controller, 8)(render, parent, q, position, At<void*>(controller, 4));
        if (type == 0 || type == 1)
            BoneMatrix(parent, q, position, scale, out);
    }
    const uint8_t* meshBone = At<const uint8_t*>(At<void*>(render, cat::kRenderMesh), 0x44) + bone * 0x28;
    const float childScale = At<float>(meshBone, 0x1C) * scale;
    const int32_t children = At<int32_t>(meshBone, 0x20);
    const int32_t* child = At<const int32_t*>(meshBone, 0x24);
    for (int32_t i = 0; i < children && child[i] >= 0; ++i)
        HierarchyHook(render, nullptr, out, child[i], childScale);
}

}  // namespace

void Hierarchy(void* render, float* parent, int32_t bone, float scale)
{
    HierarchyHook(render, nullptr, parent, bone, scale);
}

void SetKeyframeVtable(uintptr_t vtable)
{
    g_keyframeVtable = vtable;
}

namespace {

constexpr uint32_t kRotationRva = 0x51D2A, kPositionRva = 0x51DF4, kHierarchyRva = 0x540A5;
constexpr uint32_t kKeyframeVtableRva = 0x95BA4;
constexpr uint8_t kRotationPrologue[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x10};    // sub esp, 10h
constexpr uint8_t kPositionPrologue[] = {0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x0C};    // mov eax, [ebp+0Ch]
constexpr uint8_t kHierarchyPrologue[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x48};   // sub esp, 48h

}  // namespace

void Install(HMODULE orig)
{
    if (GetMode("Anim", Mode::Off) != Mode::On)
        return;
    g_keyframeVtable = reinterpret_cast<uintptr_t>(orig) + kKeyframeVtableRva;
    bool ok = HookEntry(orig, kRotationRva, kRotationPrologue, sizeof(kRotationPrologue),
                        reinterpret_cast<void*>(&RotationHook), "keyframe rotations (FUN_10051d2a)") &&
              HookEntry(orig, kPositionRva, kPositionPrologue, sizeof(kPositionPrologue),
                        reinterpret_cast<void*>(&PositionHook), "keyframe positions (FUN_10051df4)") &&
              HookEntry(orig, kHierarchyRva, kHierarchyPrologue, sizeof(kHierarchyPrologue),
                        reinterpret_cast<void*>(&HierarchyHook), "bone hierarchy (FUN_100540a5)");
    Log("animation: %s", ok ? "on" : "partly installed (unknown client build)");
}

}  // namespace rnative::anim
