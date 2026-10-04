// Character animation: replaces randy31_orig's keyframe sampling (FUN_10051d2a rotations, FUN_10051df4 positions,
// FUN_10053d0b slerp), animation blends (CATAnimBlend_t's sampling, radius, version) and the bone hierarchy
// (FUN_100540a5), docs/animation.md.
//
// randy-vk.ini [Native] Anim: off (the original) / on (ours).
#pragma once

#include "native/native.h"

#include <cstdint>

namespace rnative::anim {

// CATKeyframeAnimData_t: its tracks, one per bone (+0x38), 0x10 bytes each.
struct RotationKey { float time, q[4]; };            // quaternion x y z w
struct PositionKey { float time, p[3]; };
struct Track {
    int32_t rotationCount;
    const RotationKey* rotations;
    int32_t positionCount;
    const PositionKey* positions;
};
static_assert(sizeof(RotationKey) == 0x14 && sizeof(PositionKey) == 0x10 && sizeof(Track) == 0x10, "CAT tracks");
constexpr uint32_t kDataTracks = 0x38;

// As the original: the keys around `time` (binary search), interpolated - unless `time` is under 1% of the way to the
// next key (or past the last key: the first key, as the game wraps time itself).
void SampleRotation(const Track& track, float time, float out[4]);
void SamplePosition(const Track& track, float time, float out[3]);
// Spherical interpolation (linear when the two are within 1e-6 of each other), the shorter way.
void Slerp(const float a[4], const float b[4], float t, float out[4]);

// One bone's matrix (3x4, as CATRender_t keeps them) from its rotation and position under the parent's matrix.
void BoneMatrix(const float parent[12], const float q[4], const float position[3], float scale, float out[12]);

void Install(HMODULE orig);
// The hierarchy as installed (tests): needs Install's keyframe vtable or calls the animation's own functions.
void Hierarchy(void* render, float* parent, int32_t bone, float scale);
void SetKeyframeVtable(uintptr_t vtable);
void SetBlendVtable(uintptr_t vtable);
// An animation's slots +0x1C (bounding radius) and +0x20 (version), keyframes and blends natively.
float Radius(void* anim);
int32_t Version(void* anim);

}  // namespace rnative::anim
