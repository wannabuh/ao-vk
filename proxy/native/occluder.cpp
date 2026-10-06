// HMOccluder_t natively (randy-vk.ini [Native] Scene=on): the playfield's heightmap as an occluder. Each frame the
// camera sweeps the terrain in depth slices and keeps, per slice, a horizon - for every column of the screen the
// highest projected terrain so far; a sphere whose screen rectangle stays below the horizon of the slice before it is
// hidden. Static meshes raise the heightmap where they stand (FUN_1003f78d, still the original's), and a few
// playfields have hand-made corrections (PreProcessPlayfield, still the original's).
//
// HMOccluder_t (0x208 bytes, one, 0x1017D284): +0x00 heights (u16, centimetres-ish: times +0x10), +0x04 a byte per
// cell (0xFF: settled by hand or by a mesh), +0x08 / +0x0C width / depth in cells, +0x10 height scale, +0x14 cell
// size, +0x18 no horizon this frame, +0x1C the camera's position on the ground (y: the terrain under it), +0x28 horizon
// columns, +0x2C slices (124: +0 dirty, +4 distance, +0xC the column heights), +0x30 slices swept (49), +0x34 the
// slices, +0x38 / +0x3C half the viewport, +0x40 the camera's up, +0x4C view * projection, +0x8C the camera, +0x90
// culling on, +0x94 the playfield, +0x98 the data path, +0x9C 91 values of 1 - sqrt(1 - (i / 90)^2) (a sphere's
// outline across its rectangle).
#include "native/occluder.h"
#include "native/orig_api.gen.h"
#include "native/vc10.h"
#include "native/xmath.h"

#include <cmath>
#include <initializer_list>
#include <cstdio>
#include <cstring>

namespace rnative::occluder {

namespace {

HMODULE g_orig;

template <typename T>
T& Field(void* object, uint32_t offset) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(object) + offset); }
template <typename T>
T& Global(uint32_t rva) { return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(g_orig) + rva); }
template <typename F>
F Internal(uint32_t rva) { return reinterpret_cast<F>(AddressFor(rva)); }

using Occ = uint8_t;

constexpr uint32_t kInstance = 0x17D284, kDebuggerMode = 0xB7500;
constexpr uint32_t kHeights = 0x00, kFlags = 0x04, kWidth = 0x08, kDepth = 0x0C, kHeightScale = 0x10, kCell = 0x14,
                   kNoHorizon = 0x18, kEye = 0x1C, kColumns = 0x28, kSliceCount = 0x2C, kSlicesSwept = 0x30,
                   kSlices = 0x34, kHalfWidth = 0x38, kHalfHeight = 0x3C, kUp = 0x40, kViewProjection = 0x4C,
                   kCamera = 0x8C, kEnabled = 0x90, kPlayfield = 0x94, kDataPath = 0x98, kOutline = 0x9C;

struct Slice {
    int32_t dirty;
    float distance;
    int32_t unused;
    float* columns;
};
Slice* Slices(Occ* o) { return Field<Slice*>(o, kSlices); }
uint16_t* Heights(Occ* o) { return Field<uint16_t*>(o, kHeights); }
uint32_t DebuggerMode() { return Global<uint32_t>(kDebuggerMode); }

// The rounding helpers (x87 fistp: to nearest, ties to even).
int32_t __cdecl RoundDown(float x)                  // FUN_1002fc74: x - 0.5 (x + 0.5 below 0), rounded
{
    const float y = 0.0f <= x ? float(double(x) - 0.5) : float(double(x) + 0.5);
    return int32_t(Lrint(y));
}
int32_t __cdecl Round(float x) { return int32_t(Lrint(x)); }   // FUN_1002fcae
int32_t __cdecl RoundNudged(float x)                // FUN_1002fc3a: 1e-7 away from 0, rounded
{
    const float y = 0.0f <= x ? float(double(x) + 1.0000000116860974e-07) : float(double(x) - 1.0000000116860974e-07);
    return int32_t(Lrint(y));
}

bool HasCache(int32_t playfield)                    // the playfields whose heightmaps are cached on disk
{
    switch (playfield) {
    case 0x21C: case 0x221: case 0x236: case 0x280: case 0x2BC: case 0x2C1: case 0x2DA: case 0x2DF: case 0x2E4:
    case 0x320: return true;
    default: return false;
    }
}

// FUN_1002fd6d: everything back to its start (no heightmap, 124 slices 4 m apart, the outline table).
void __fastcall Reset(Occ* o)
{
    Field<float>(o, kHeightScale) = 0.0f;
    Field<float>(o, kCell) = 0.0f;
    std::memset(o + kEye, 0, 12);
    Field<float>(o, kHalfWidth) = 1.0f;
    Field<float>(o, kHalfHeight) = 1.0f;
    Field<float>(o, kUp) = 0.0f;
    Field<float>(o, kUp + 4) = 1.0f;
    Field<void*>(o, kHeights) = nullptr;
    Field<void*>(o, kFlags) = nullptr;
    Field<int32_t>(o, kWidth) = 0;
    Field<int32_t>(o, kDepth) = 0;
    Field<int32_t>(o, kNoHorizon) = 1;
    Field<int32_t>(o, kColumns) = 0;
    Field<int32_t>(o, kSliceCount) = 0x7C;
    Field<void*>(o, kSlices) = nullptr;
    Field<float>(o, kUp + 8) = 0.0f;
    std::memcpy(o + kViewProjection, xm::Identity().m, 64);
    Field<void*>(o, kCamera) = nullptr;
    Field<int32_t>(o, kPlayfield) = 0;
    Field<int32_t>(o, kEnabled) = 1;
    Field<Slice*>(o, kSlices) = static_cast<Slice*>(vc10::AllocateArray(0x7C0));
    for (int32_t i = 0; i < Field<int32_t>(o, kSliceCount); ++i) {
        Slice& s = Slices(o)[i];
        s.columns = nullptr;
        s.distance = float(i) * 4.0f + 4.0f;
        s.dirty = 1;
        s.unused = 0;
    }
    float* outline = &Field<float>(o, kOutline);
    for (int i = 0; i < 0x5B; ++i) {
        if (i == 0x5A) {
            outline[i] = 1.0f;
        } else {
            const float t = float(i) / 90.0f;
            outline[i] = 1.0f - float(std::sqrt(double(1.0f - t * t)));
        }
    }
}

void* __fastcall Construct(Occ* o)                    // FUN_1003ed8b
{
    std::memset(o + kEye, 0, 12);
    std::memset(o + kUp, 0, 12);
    std::memcpy(o + kViewProjection, xm::Identity().m, 64);
    Field<int32_t>(o, kSlicesSwept) = 0x31;
    Reset(o);
    return o;
}

// FUN_1002feaf: a cached playfield's heightmap and settled cells written back to disk, then everything freed.
void __fastcall Release(Occ* o)
{
    const int32_t playfield = Field<int32_t>(o, kPlayfield);
    const int32_t cells = Field<int32_t>(o, kWidth) * Field<int32_t>(o, kDepth);
    if (HasCache(playfield)) {
        const char* path = Field<const char*>(o, kDataPath);
        char name[1024];
        std::sprintf(name, "%s\\OCC\\OCCHM_%d.i", path, playfield);
        if (FILE* f = std::fopen(name, "wb")) {
            std::fwrite(Heights(o), 2, size_t(cells), f);
            std::fclose(f);
        }
        std::sprintf(name, "%s\\OCC\\OCCBL_%d.i", path, Field<int32_t>(o, kPlayfield));
        if (FILE* f = std::fopen(name, "wb")) {
            uint8_t* flags = Field<uint8_t*>(o, kFlags);
            for (uint32_t i = 0; i < uint32_t(cells); ++i)
                if (flags[i]) flags[i] = 0xFF;
            std::fwrite(flags, 1, size_t(cells), f);
            std::fclose(f);
        }
    }
    if (Slice* s = Slices(o)) {
        for (int32_t i = 0; i < Field<int32_t>(o, kSliceCount); ++i)
            if (s[i].columns) {
                vc10::FreeArray(s[i].columns);
                s[i].columns = nullptr;
            }
        vc10::FreeArray(s);
        Field<void*>(o, kSlices) = nullptr;
    }
    if (void* h = Field<void*>(o, kHeights)) {
        vc10::FreeArray(h);
        Field<void*>(o, kHeights) = nullptr;
    }
    if (void* f = Field<void*>(o, kFlags)) {
        vc10::FreeArray(f);
        Field<void*>(o, kFlags) = nullptr;
    }
}

Occ* __cdecl Get()
{
    Occ*& instance = Global<Occ*>(kInstance);
    if (!instance) instance = static_cast<Occ*>(Construct(static_cast<Occ*>(vc10::Allocate(0x208))));
    return instance;
}

void __cdecl Shutdown()
{
    Occ*& instance = Global<Occ*>(kInstance);
    if (!instance) return;
    Release(instance);
    vc10::Free(instance);
    instance = nullptr;
}

void __fastcall SetHeightmapSize(Occ* o, void*, int32_t width, int32_t depth)
{
    Release(o);
    Reset(o);
    Field<int32_t>(o, kWidth) = width;
    Field<int32_t>(o, kDepth) = depth;
    const size_t cells = size_t(uint32_t(width * depth));
    Field<void*>(o, kHeights) = vc10::AllocateArray(cells * 2);
    std::memset(Heights(o), 0, cells * 2);
    Field<void*>(o, kFlags) = vc10::AllocateArray(cells);
    std::memset(Field<void*>(o, kFlags), 0, cells);
}

void __fastcall SetHeightmapScale(Occ* o, void*, float cell, float, float height)
{
    Field<float>(o, kHeightScale) = height;
    Field<float>(o, kCell) = cell;
}

// A square patch of (size + 1)^2 heights at cell (x, z).
void __fastcall SetHeightmapPatch(Occ* o, void*, const uint16_t* heights, int32_t x, int32_t z, int32_t size)
{
    for (int32_t row = 0; row <= size; ++row, heights += size + 1)
        for (int32_t i = 0; i <= size; ++i) Heights(o)[(row + z) * Field<int32_t>(o, kWidth) + i + x] = heights[i];
}

void __fastcall SetDataPath(Occ* o, void*, const char* path) { Field<const char*>(o, kDataPath) = path; }

// A hand correction (the tool behind PreProcessPlayfield's tables): set and appended to HMOCCDATA_<playfield>.i.
void __fastcall ModifyOcclusion(Occ* o, void*, int32_t x, float height, int32_t z)
{
    if (!Heights(o)) return;
    const int32_t at = Field<int32_t>(o, kWidth) * z + x;
    Field<uint8_t*>(o, kFlags)[at] = 0xFF;
    Heights(o)[at] = uint16_t(int64_t(double(height) / double(Field<float>(o, kHeightScale))));
    char name[1024];
    std::sprintf(name, "..\\common\\randy31\\randy\\HMOCCDATA_%d.i", Field<int32_t>(o, kPlayfield));
    if (FILE* f = std::fopen(name, "a")) {
        std::fprintf(f, "    SETOCC( %d, %f, %d );\n", x, double(height), z);
        std::fclose(f);
    }
}

float __fastcall GetHeight(Occ* o, void*, const float* p)
{
    if (!Heights(o)) return 0.0f;
    const int32_t x = Round(p[0] / Field<float>(o, kCell));
    if (x < 0 || Field<int32_t>(o, kWidth) <= x) return 0.0f;
    const int32_t z = Round(p[2] / Field<float>(o, kCell));
    if (z < 0 || Field<int32_t>(o, kDepth) <= z) return 0.0f;
    return float(Heights(o)[Field<int32_t>(o, kWidth) * z + x]) * Field<float>(o, kHeightScale);
}

bool __fastcall ToggleOcclusionCulling(Occ* o)
{
    Field<int32_t>(o, kEnabled) = 1 - Field<int32_t>(o, kEnabled);
    return Field<int32_t>(o, kEnabled) != 0;
}

// FUN_1002fcbf: a viewport's rectangle as floats (left, top, right, bottom; its unsigned fields).
void __fastcall ViewportRect(uint8_t* vp, void*, float* left, float* top, float* right, float* bottom)
{
    auto u = [](uint32_t v) { return float(double(v)); };
    *left = u(Field<uint32_t>(vp, 0x1C));
    *top = u(Field<uint32_t>(vp, 0x20));
    *right = u(Field<uint32_t>(vp, 0x24) + Field<uint32_t>(vp, 0x1C));
    *bottom = u(Field<uint32_t>(vp, 0x2C) + Field<uint32_t>(vp, 0x20));
}

float __fastcall FieldOfView(uint8_t* camera)        // FUN_1002fd3c: from the view plane window's width
{
    const float half = float(std::atan(double(Field<float>(camera, 0xAC) - Field<float>(camera, 0xA4)) * 0.5));
    return half + half;
}

void Rotated(const float* v, const float* q, float* out)   // FUN_1002bb8a
{
    out[0] = v[0], out[1] = v[1], out[2] = v[2];
    xm::RotateVector(out, q);
}

void DebugPoint(float x, float y, float r, float g, float b)
{
    Internal<void(__fastcall*)(void*, void*, float, float, float, float, float, float, bool)>(0x2CBAA)(
        orig::Debugger_t_Get(), nullptr, x, y, 0.1f, r, g, b, false);
}

// FUN_1003edc1: this frame's horizons, as seen from `vp`'s camera standing on the ground.
void __fastcall BuildHorizons(Occ* o, void*, uint8_t* vp)
{
    Field<int32_t>(o, kNoHorizon) = 1;
    if (!Heights(o) || !Field<int32_t>(o, kEnabled) || !vp) return;
    uint8_t* camera = Field<uint8_t*>(vp, 0x0C);
    Field<uint8_t*>(o, kCamera) = camera;
    if (!camera) return;
    const float yAxis[3] = {0.0f, 1.0f, 0.0f}, zAxis[3] = {0.0f, 0.0f, 1.0f};
    const float* rotation = &Field<float>(camera, 0x2C);
    Rotated(yAxis, rotation, &Field<float>(o, kUp));
    if (!(0.5f <= Field<float>(o, kUp + 4))) return;   // looking too far up or down
    xm::M4 view, projection;
    orig::RCamera_t_GetViewMatrix(camera, view.m);
    orig::RCamera_t_GetTransformationMatrix(camera, projection.m);
    const xm::M4 vpm = xm::Mul(view, projection);
    std::memcpy(o + kViewProjection, vpm.m, 64);
    const float front = Field<float>(camera, 0xB4), aspect = Field<float>(camera, 0xC0);
    const float* world = static_cast<const float*>(orig::RRefFrame_t_GetWorldMatrix(camera));
    float* eye = &Field<float>(o, kEye);
    eye[0] = world[12], eye[1] = world[13], eye[2] = world[14];
    eye[1] = GetHeight(o, nullptr, eye);
    float left, top, right, bottom;
    ViewportRect(vp, nullptr, &left, &top, &right, &bottom);
    Field<float>(o, kHalfWidth) = (right - left) * 0.5f;
    Field<float>(o, kHalfHeight) = (bottom - top) * 0.5f;
    const int32_t columns = (RoundDown(right) - RoundDown(left)) / Round(8.0f);
    if (columns != Field<int32_t>(o, kColumns)) {
        Field<int32_t>(o, kColumns) = columns;
        for (int32_t i = 0; i < Field<int32_t>(o, kSliceCount); ++i) {
            Slice& s = Slices(o)[i];
            if (s.columns) vc10::FreeArray(s.columns);
            s.columns = static_cast<float*>(vc10::AllocateArray(size_t(uint32_t(columns)) * 4));
            s.dirty = 1;
            s.unused = 0;
        }
    }
    // The left and right edges of the view on the ground, and the direction from one to the other.
    float q[4], t[3], leftDir[3], rightDir[3], across[3];
    xm::AxisAngle(yAxis, float(double(-FieldOfView(camera)) * 0.5), q);
    Rotated(zAxis, q, t);
    Rotated(t, rotation, leftDir);
    xm::AxisAngle(yAxis, float(double(FieldOfView(camera)) * 0.5), q);
    Rotated(zAxis, q, t);
    Rotated(t, rotation, rightDir);
    leftDir[1] = 0.0f;
    rightDir[1] = 0.0f;
    xm::Normalize(leftDir);
    xm::Normalize(rightDir);
    const float columnWidth = float(2.0 / double(Field<int32_t>(o, kColumns)));
    float d[3] = {rightDir[0] - leftDir[0], rightDir[1] - leftDir[1], rightDir[2] - leftDir[2]};
    xm::Normalize(d);
    std::memcpy(across, d, sizeof(across));

    const float cell = Field<float>(o, kCell);
    static float points[2048];                       // (screen x, screen y) of the terrain along the slice
    int32_t start = 0;                               // the column the fill after the points starts at
    for (int32_t s = 0; s < Field<int32_t>(o, kSlicesSwept); ++s) {
        Slice& slice = Slices(o)[s];
        const Slice* before = s ? &Slices(o)[s - 1] : nullptr;
        const float distance = slice.distance;
        float a[3], b[3], u[3], w[3];
        for (int k = 0; k < 3; ++k) {                // one cell beyond each edge
            u[k] = across[k] * cell;
            w[k] = eye[k] + leftDir[k] * distance;
            a[k] = w[k] - u[k];
            w[k] = eye[k] + rightDir[k] * distance;
            b[k] = w[k] + u[k];
        }
        float ab[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const float length = float(std::sqrt(double(xm::LengthSquared(ab)))) / cell;
        const int32_t steps = RoundDown(length);
        const float stepsF = float(steps);
        const float step[3] = {ab[0] / stepsF, ab[1] / stepsF, ab[2] / stepsF};
        float p[3] = {a[0], a[1], a[2]};
        start = 0;
        float low = -1.0f;
        int32_t count = 0;
        for (int32_t k = steps + 1; k > 0; --k) {
            const int32_t ix = RoundNudged(p[0] / cell), iz = RoundNudged(p[2] / cell);
            float sample[3] = {float(ix) * cell, 0.0f, float(iz) * cell};
            if (iz >= 0 && ix >= 0 && ix < Field<int32_t>(o, kWidth) && iz < Field<int32_t>(o, kDepth))
                sample[1] = float(int32_t(Heights(o)[Field<int32_t>(o, kWidth) * iz + ix])) * Field<float>(o, kHeightScale);
            float sp[3];
            xm::Transform(sample, &Field<float>(o, kViewProjection), sp);
            if (-3.6893488147419103e+19f <= sp[2]) {
                const float z = sp[2] + front;
                const float sx = sp[0] / z;
                auto screenY = [&] {
                    const float y = (sp[1] / z) / aspect;
                    return y <= 1.0f ? (-1.0f <= y ? y : -1.0f) : 1.0f;
                };
                if (count == 0) {
                    points[0] = -1.0f;
                    points[1] = screenY();
                    count = 1;
                } else if (sx != points[count * 2 - 2]) {
                    points[count * 2] = sx;
                    points[count * 2 + 1] = screenY();
                    ++count;
                }
            } else if (count == 0) {
                points[0] = -1.0f;
                points[1] = -1.0f;
                count = 1;
            }
            p[0] = step[0] + p[0], p[1] = step[1] + p[1], p[2] = step[2] + p[2];
        }
        // The points into columns: each segment's columns the higher of it and the slice before.
        const int32_t cols = Field<int32_t>(o, kColumns);
        int32_t i = 1;
        bool ended = false;
        while (i < count) {
            const int32_t first = i;
            float* previous = &points[(first - 1) * 2];
            if (points[i * 2] == previous[0]) {
                do {
                    if (count <= i) {
                        ended = true;
                        break;
                    }
                    ++i;
                    if (previous[1] < points[i * 2 + 1]) previous[1] = points[i * 2 + 1];
                } while (points[i * 2] == previous[0]);
                if (ended) break;
            }
            if (count <= i) break;
            int32_t c0 = RoundDown(float((double(previous[0]) + 1.0) / double(columnWidth)));
            if (c0 < 0) c0 = 0;
            else if (cols <= c0) c0 = cols - 1;
            int32_t c1 = RoundDown(float((double(points[i * 2]) + 1.0) / double(columnWidth)));
            if (c1 < 0) c1 = 0;
            else if (cols <= c1) c1 = cols;
            start = c0;
            if (c0 != c1) {
                float y0 = previous[1];
                float y1 = points[i * 2 + 1];
                if (y0 < low) y0 = low;
                if (before && y0 < before->columns[c0]) y0 = before->columns[c0];
                const float kept = y0;
                if (c0 < 0 || cols <= c0) return;
                slice.columns[c0] = 1.0f <= y0 ? 10000.0f : y0;
                start = c0 + 1;
                if (start <= c1) {
                    if (before && y1 < before->columns[c1 - 1]) y1 = before->columns[c1 - 1];
                    const float raw = y1;
                    slice.columns[c1 - 1] = 1.0f <= y1 ? 10000.0f : y1;
                    low = raw < kept ? raw : kept;
                    for (; start < c1; ++start) {
                        float v;
                        if (before && low < before->columns[start]) v = before->columns[start];
                        else v = low < 1.0f ? low : 10000.0f;
                        slice.columns[start] = v;
                    }
                }
            }
            ++i;
        }
        for (; start < cols; ++start)                // past the last point: the slice before, or the bottom
            slice.columns[start] = (before && -1.0f < before->columns[start]) ? before->columns[start] : -1.0f;
    }
    for (int32_t s = 0; s < Field<int32_t>(o, kSlicesSwept); ++s) {
        Slices(o)[s].dirty = 0;
        Field<int32_t>(o, kNoHorizon) = 0;
    }
    if ((DebuggerMode() & 2) && 0 < Field<int32_t>(o, kSlicesSwept)) {   // the horizons drawn in 2D
        using Add2DLineFn = void(__fastcall*)(void*, void*, float, float, float, float, float, float, float, float,
                                              float, bool);
        static const auto add2DLine =
            reinterpret_cast<Add2DLineFn>(GetProcAddress(g_orig, "?Add2DLine@Debugger_t@@QAEXVVector3_t@@0MMM_N@Z"));
        for (int32_t s = 0; s < Field<int32_t>(o, kSlicesSwept); ++s) {
            const int32_t cols = Field<int32_t>(o, kColumns);
            for (int32_t c = 1; c < cols; ++c) {
                const float* h = Slices(o)[s].columns + c;
                const float y1 = 1.0f <= h[0] ? 1.0f : h[0];
                const float y0 = h[-1] < 1.0f ? h[-1] : 1.0f;
                const float x1 = (float(c) / float(cols)) * 2.0f - 1.0f;
                const float x0 = (float(c - 1) / float(cols)) * 2.0f - 1.0f;
                add2DLine(orig::Debugger_t_Get(), nullptr, x0, y0, 0.0f, x1, y1, 0.0f, 0.5f, 0.5f, 0.5f, false);
            }
        }
    }
}

// FUN_1003e8f6: is a screen rectangle (-1..1) at depth `z` above the horizon of its slice anywhere - checked from its
// middle out, the top lowered along a sphere's outline towards the sides.
bool __fastcall Visible(Occ* o, void*, void*, float left, float right, float bottom, float top, float z)
{
    if (!Heights(o) || !Field<int32_t>(o, kEnabled) || Field<int32_t>(o, kNoHorizon) == 1) return true;
    const float depth = float(((double(z) - 8.0) - 4.0) * 0.25);
    int32_t s = int32_t(Lrint(double(depth) - 0.49999));
    if (s < 0) return true;
    if (Field<int32_t>(o, kSlicesSwept) <= s) s = Field<int32_t>(o, kSlicesSwept) - 1;
    const int32_t cols = Field<int32_t>(o, kColumns);
    const float* horizon = Slices(o)[s].columns;
    const float* outline = &Field<float>(o, kOutline);
    const int32_t a = int32_t(Lrint(double(float(((double(left) + 1.0) * 0.5) * double(cols))) - 0.49999));
    const int32_t b = int32_t(Lrint(double(float(((double(right) + 1.0) * 0.5) * double(cols))) - 0.49999)) + 1;
    const int32_t mid = (b - a) / 2 + a;
    const float halfHeight = float((double(top) - double(bottom)) * 0.5);
    const bool debug = (DebuggerMode() & 4) != 0;
    auto screenX = [&](int32_t c) { return float((double(c) / double(cols)) * 2.0 - 1.0); };
    auto heightAt = [&](int32_t k, int32_t n) { return float(double(top) - double(outline[(k * 0x5A) / n]) * halfHeight); };
    // Leftwards from the middle.
    const int32_t n1 = mid - a + 1;
    const int32_t from = a < 1 ? 0 : a;
    int32_t c = mid < cols ? mid : cols - 1;
    for (int32_t k = mid - c; from <= c; --c, ++k) {
        const float y = heightAt(k, n1);
        if (y >= horizon[c]) {
            if (debug) DebugPoint(screenX(c), heightAt(mid - c, n1), 1.0f, 1.0f, 1.0f);
            return true;
        }
        if (debug) DebugPoint(screenX(c), y, 1.0f, 0.0f, 0.0f);
    }
    // Rightwards.
    const int32_t n2 = b - mid + 1;
    const int32_t to = cols <= b ? cols - 1 : b;
    c = mid + 1 < 0 ? 0 : mid + 1;
    for (int32_t k = c - mid; c <= to; ++c, ++k) {
        const float y = heightAt(k, n2);
        if (y >= horizon[c]) {
            if (debug) DebugPoint(screenX(c), heightAt(c - mid, n2), 1.0f, 1.0f, 1.0f);
            return true;
        }
        if (debug) DebugPoint(screenX(c), y, 1.0f, 0.0f, 0.0f);
    }
    return false;
}

// FUN_1003ecab: a mesh name that isn't scenery to occlude with (trees, heads, doors, ...): none of these in it.
// The original's matching, kept: per word a count of characters matched, reset (to 0 or 1) on a mismatch.
bool __cdecl OccludingName(const char* name)
{
    static const char* const kSkipped[] = {
        "tree", "leaves", "grass", "head", "hair", "weapon", "stardome", "sphere", "bird", "monster", "ship", "face",
        "hemet", "scenery", "tent", "simplecity", "simple_", "bridge", "line", "stotte", "lightcone", "soppel",
        "drone", "paper", "helmet", "shop", "door", "horizon", "snutt", "dafdas", "skil", "stenger", "sign",
        "chamfer", "fork", "serving", "mongomeat", "gunner", "small", "papir", "minibronto", "dinigg_b", "seats",
        "torus", "snackbar", "cockroach", "containe", "_can", "camera", "trans", "fly", "logo", "beetle", "garbage",
        "rod", "moth"};
    constexpr size_t kWords = sizeof(kSkipped) / sizeof(kSkipped[0]);
    const size_t length = std::strlen(name);
    if (!length) return false;
    int32_t matched[kWords] = {};
    for (size_t i = 0; i < length; ++i) {
        char c = name[i];
        if (uint8_t(c - 'A') < 26) c = char(c + 32);
        for (size_t w = 0; w < kWords; ++w) {
            const char* word = kSkipped[w];
            if (word[matched[w]] == c) {
                if (word[++matched[w]] == 0) return false;
            } else {
                matched[w] = word[0] == c ? 1 : 0;
            }
        }
    }
    return true;
}

float WorldScale(uint8_t* frame)                     // FUN_1002fd23
{
    if (frame[0x9E]) orig::RRefFrame_t_UpdateWorldMatrix(frame);
    return Field<float>(frame, 0x84);
}

int32_t Truncate(double x) { return int32_t(x); }   // _ftol2

// FUN_1003f78d: an RTriMesh_t standing on the playfield raises the heightmap to its top, cell by cell under its
// bounding sphere - where a ray down from twice the radius above the ground hits it and one up from the ground
// doesn't (not overhanging). On the hand-corrected playfields only (or for an "[OCC]" occluder mesh, which may also
// lower settled cells' neighbours); a cell is settled after 255 tries or one raise.
void __fastcall RegisterMesh(Occ* o, void*, uint8_t* mesh)
{
    if (!Heights(o)) return;
    const char* name = orig::RResource_t_GetName(Field<void*>(mesh, 0x184));
    const bool occluder = std::strncmp(name, "[OCC]", 5) == 0;
    if (!occluder && !HasCache(Field<int32_t>(o, kPlayfield))) return;
    if (!OccludingName(name)) return;
    const uint8_t* volume = Field<uint8_t*>(Field<uint8_t*>(mesh, 0x184), 0x6C);
    float center[3];
    xm::Transform(reinterpret_cast<const float*>(volume + 8), static_cast<const float*>(orig::RRefFrame_t_GetWorldMatrix(mesh)), center);
    const float radius = float(double(Field<float>(const_cast<uint8_t*>(volume), 0x14)) * double(WorldScale(mesh)));
    const float cell = Field<float>(o, kCell);
    const int32_t x0 = Truncate(double(float(center[0] - radius)) / double(cell));
    const int32_t x1 = Truncate(double(float(center[0] + radius)) / double(cell));
    const int32_t z0 = Truncate(double(float(center[2] - radius)) / double(cell));
    const int32_t z1 = Truncate(double(float(center[2] + radius)) / double(cell));
    const float firstX = float(double(x0) * double(cell));
    const float radiusSq = float(double(radius) * double(radius));
    const float twice = radius + radius;
    const float upward[3] = {0.0f, 1.0f, 0.0f}, downward[3] = {0.0f, -1.0f, 0.0f};
    const int32_t width = Field<int32_t>(o, kWidth);
    uint8_t* flags = Field<uint8_t*>(o, kFlags);
    using RayFn = bool(__fastcall*)(void*, void*, const float*, const float*, float*, bool);
    float rowZ = float(double(z0) * double(cell));
    int32_t rowStart = width * z0;
    for (int32_t z = z0; z <= z1; ++z, rowStart += width, rowZ = cell + rowZ) {
        if (z < 0 || Field<int32_t>(o, kDepth) <= z) continue;
        float x = firstX;
        int32_t at = rowStart + x0;
        for (int32_t column = x0; column <= x1; ++column, ++at, x = cell + x) {
            if (column < 0 || width <= column) continue;
            if (!occluder) {
                if (flags[at] >= 0xFF) continue;
                if (++flags[at] == 0xFF) continue;
            }
            float p[3] = {x, float(int32_t(Heights(o)[at])) * Field<float>(o, kHeightScale), rowZ};
            if (!occluder) {
                float d[3] = {p[0] - center[0], p[1] - center[1], p[2] - center[2]};
                if (radiusSq < xm::LengthSquared(d)) continue;
            }
            p[1] = p[1] + twice;
            const float down[3] = {downward[0] * twice, downward[1] * twice, downward[2] * twice};
            float hit = 0.0f;
            const auto ray = reinterpret_cast<RayFn>((*reinterpret_cast<void***>(mesh))[12]);
            if (!ray(mesh, nullptr, p, down, &hit, false)) continue;
            p[1] = p[1] - twice;
            const float up[3] = {upward[0] * twice, upward[1] * twice, upward[2] * twice};
            if (ray(mesh, nullptr, p, up, nullptr, false)) continue;
            const double raise = (double(twice) - double(hit) * double(twice)) / double(Field<float>(o, kHeightScale));
            int32_t h = Truncate(raise) + int32_t(Heights(o)[at]);
            if (h > 0xFFFF || h < 0) h = 0xFFFF;
            Heights(o)[at] = uint16_t(h);
            if (!occluder) flags[at] = 0xFF;
            ++Global<int32_t>(0x17D288);
        }
    }
}

// PreProcessPlayfield's hand corrections: generated from the original (tools/gen_occlusion_data.py) into a git-ignored
// file - the game's data. Without it the original's function stays.
struct HandCell {
    int32_t x;                                       // -1: the playfield in z, its cells follow
    double height;
    int32_t z;
};
#if __has_include("native/private/occlusion_data.inc")
#define OCC_PLAYFIELD(p) {-1, 0.0, p},
#define SETOCC(x, h, z) {x, h, z},
#define OCC_END()
constexpr HandCell kHandCells[] = {
#include "native/private/occlusion_data.inc"
};
#undef OCC_PLAYFIELD
#undef SETOCC
#undef OCC_END
constexpr bool kHaveHandCells = true;
#else
constexpr HandCell kHandCells[] = {{-1, 0.0, 0}};
constexpr bool kHaveHandCells = false;
#endif

// A cached heightmap or flag file read when its size matches.
void ReadCache(const char* name, void* to, size_t bytes)
{
    FILE* f = std::fopen(name, "rb");
    if (!f) return;
    std::fseek(f, 0, SEEK_END);
    if (_ftelli64(f) == int64_t(bytes)) {
        std::fseek(f, 0, SEEK_SET);
        std::fread(to, 1, bytes, f);
    }
    std::fclose(f);
}

// HMOccluder_t::PreProcessPlayfield: the playfield's hand corrections, then (for the cached playfields) its
// preprocessed heightmap and settled cells from <data>\OCC - all caches dropped when version.id changed.
void __fastcall PreProcessPlayfield(Occ* o, void*, int32_t playfield)
{
    if (!Heights(o)) return;
    Field<int32_t>(o, kPlayfield) = playfield;
    const int32_t width = Field<int32_t>(o, kWidth);
    bool in = false;
    for (const HandCell& c : kHandCells) {
        if (c.x < 0) {
            in = c.z == playfield;
            continue;
        }
        if (!in) continue;
        Field<uint8_t*>(o, kFlags)[width * c.z + c.x] = 0xFF;
        Heights(o)[width * c.z + c.x] = uint16_t(int64_t(c.height / double(Field<float>(o, kHeightScale))));
    }
    if (!HasCache(Field<int32_t>(o, kPlayfield))) return;
    const char* path = Field<const char*>(o, kDataPath);
    char name[1024], current[1024], cached[1024];
    std::sprintf(name, "%s\\OCC", path);
    CreateDirectoryA(name, nullptr);
    std::sprintf(name, "version.id");
    if (FILE* f = std::fopen(name, "rb")) {
        std::fscanf(f, "%s", current);
        std::fclose(f);
    } else {
        current[0] = 0;
    }
    std::sprintf(name, "%s\\OCC\\OCCV.i", path);
    if (FILE* f = std::fopen(name, "rb")) {
        std::fscanf(f, "%s", cached);
        std::fclose(f);
    } else {
        cached[0] = '1', cached[1] = 0;
    }
    if (std::strcmp(current, cached) != 0) {
        if (FILE* f = std::fopen(name, "wb")) {
            std::fprintf(f, "%s\n", current);
            std::fclose(f);
        }
        for (int32_t id : {540, 545, 566, 640, 700, 705, 730, 735, 740, 800})
            for (const char* kind : {"HM", "BL"}) {
                std::sprintf(name, "%s\\OCC\\OCC%s_%d.i", path, kind, id);
                DeleteFileA(name);
            }
    }
    const size_t cells = size_t(uint32_t(Field<int32_t>(o, kDepth) * width));
    std::sprintf(name, "%s\\OCC\\OCCHM_%d.i", path, Field<int32_t>(o, kPlayfield));
    ReadCache(name, Heights(o), cells * 2);
    std::sprintf(name, "%s\\OCC\\OCCBL_%d.i", path, Field<int32_t>(o, kPlayfield));
    ReadCache(name, Field<uint8_t*>(o, kFlags), cells);
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
        {0x2FC74, FN(RoundDown), "rounding, half down (FUN_1002fc74)"},
        {0x2FCAE, FN(Round), "rounding (FUN_1002fcae)"},
        {0x2FC3A, FN(RoundNudged), "rounding, nudged (FUN_1002fc3a)"},
        {0x2FD6D, FN(Reset), "HMOccluder_t reset (FUN_1002fd6d)"},
        {0x3ED8B, FN(Construct), "HMOccluder_t::HMOccluder_t (FUN_1003ed8b)"},
        {0x2FEAF, FN(Release), "HMOccluder_t save and free (FUN_1002feaf)"},
        {0x3FB03, FN(Get), "HMOccluder_t::Get"},
        {0x3FB49, FN(Shutdown), "HMOccluder_t::ShutdownHMOccluder"},
        {0x30043, FN(SetHeightmapSize), "HMOccluder_t::SetHeightmapSize"},
        {0x300B4, FN(SetHeightmapScale), "HMOccluder_t::SetHeightmapScale"},
        {0x300C7, FN(SetHeightmapPatch), "HMOccluder_t::SetHeightmapPatch"},
        {0x3011C, FN(SetDataPath), "HMOccluder_t::SetDataPath"},
        {0x3E85B, FN(ModifyOcclusion), "HMOccluder_t::ModifyOcclusion"},
        {0x3EC30, FN(GetHeight), "HMOccluder_t::GetHeight"},
        {0x3ED78, FN(ToggleOcclusionCulling), "HMOccluder_t::ToggleOcclusionCulling"},
        {0x2FCBF, FN(ViewportRect), "RViewPort_t rectangle as floats (FUN_1002fcbf)"},
        {0x2FD3C, FN(FieldOfView), "RCamera_t field of view (FUN_1002fd3c)"},
        {0x3EDC1, FN(BuildHorizons), "HMOccluder_t horizons (FUN_1003edc1)"},
        {0x3E8F6, FN(Visible), "HMOccluder_t visibility (FUN_1003e8f6)"},
        {0x3ECAB, FN(OccludingName), "HMOccluder_t mesh name filter (FUN_1003ecab)"},
        {0x3F78D, FN(RegisterMesh), "HMOccluder_t mesh registration (FUN_1003f78d)"},
    };
#undef FN
    int installed = 0;
    for (const Entry& e : entries) installed += Replace(orig, e.rva, e.target, e.what) ? 1 : 0;
    if (kHaveHandCells)
        installed += Replace(orig, 0x3012C, reinterpret_cast<void*>(&PreProcessPlayfield),
                             "HMOccluder_t::PreProcessPlayfield (hand corrections generated)") ? 1 : 0;
    else
        Log("occluder: no proxy/native/private/occlusion_data.inc (tools/gen_occlusion_data.py) - PreProcessPlayfield stays");
    Log("occluder: %d of %d functions native", installed, int(sizeof(entries) / sizeof(entries[0])) + (kHaveHandCells ? 1 : 0));
}

}  // namespace rnative::occluder
