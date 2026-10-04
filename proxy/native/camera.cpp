// RCamera_t and RLight_t natively (randy-vk.ini [Native] Scene=on): the camera's projection, view and frustum (set up
// once a frame by the viewport, then asked by every frame of the scene whether a sphere can be seen), and the lights -
// their D3DLIGHT7, collected into the scene's light list as the scene is processed.
//
// RCamera_t (0x1A8 bytes, an RRefFrame_t, vtable 0x93EE4): +0xA4..+0xB0 the view plane window (left, top, right,
// bottom at distance 1), +0xB4 front plane, +0xB8 back plane, +0xBC the back plane in use (the fog's end when nearer),
// +0xC0 aspect (right / top), +0xC4 a sphere around the frustum (centre, +0xD0 radius), +0xD4 the planes' normals in
// world space (forward, left, right, top, bottom, back), +0x11C HMOccluder_t, +0x120 view * projection, +0x160 view,
// +0x1A0 the viewport it was set up for, +0x1A4 1.
//
// RLight_t (0x11C bytes, an RRefFrame_t, vtable 0x953D8): +0xA4 D3DLIGHT7 (position and direction in world space once
// processed), +0x10C its type (1 directional, 2 point, 4 spot), +0x110 its index in the light list, +0x118 enabled.
#include "native/camera.h"
#include "native/orig_api.gen.h"
#include "native/serialize.h"
#include "native/vc10.h"
#include "native/xmath.h"

#include <cmath>
#include <cstring>

namespace rnative::camera {

namespace {

HMODULE g_orig;
void* const* g_randy;

template <typename T>
T& Field(void* object, uint32_t offset) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(object) + offset); }
template <typename T>
T& Global(uint32_t rva) { return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(g_orig) + rva); }
template <typename F>
F Internal(uint32_t rva) { return reinterpret_cast<F>(reinterpret_cast<uint8_t*>(g_orig) + rva); }

using Frame = uint8_t;

constexpr uint32_t kCameraVtable = 0x93EE4, kLightVtable = 0x953D8;
constexpr uint32_t kCurrentCamera = 0x17D338, kDebuggerMode = 0xB7500, kLightsChanged = 0xB7728,
                   kLightList = 0x17D290, kLightsMoved = 0x17D28C, kRender = 0x16BED0;
constexpr uint32_t kRotation = 0x2C, kChanged = 0x9D;

// RCamera_t
constexpr uint32_t kLeft = 0xA4, kTop = 0xA8, kRight = 0xAC, kBottom = 0xB0, kFront = 0xB4, kBack = 0xB8,
                   kBackInUse = 0xBC, kAspect = 0xC0, kSphere = 0xC4, kSphereRadius = 0xD0, kForward = 0xD4,
                   kLeftPlane = 0xE0, kBackPlane = 0x110, kOccluder = 0x11C, kViewProjection = 0x120, kView = 0x160,
                   kViewport = 0x1A0, kOne = 0x1A4;
// RLight_t
constexpr uint32_t kLight = 0xA4, kLightBytes = 0x68, kType = 0x10C, kIndex = 0x110, kEnabled = 0x118;
constexpr uint32_t kPosition = kLight + 0x34, kDirection = kLight + 0x40, kRange = kLight + 0x4C,
                   kTheta = kLight + 0x60, kPhi = kLight + 0x64;

void SetVtable(Frame* f, uint32_t vtable) { Field<uintptr_t>(f, 0) = reinterpret_cast<uintptr_t>(g_orig) + vtable; }
const float* World(void* f) { return static_cast<const float*>(orig::RRefFrame_t_GetWorldMatrix(f)); }
void* __fastcall DestroyFrame(Frame* f) { return Internal<void*(__fastcall*)(void*)>(0x45471)(f), f; }

void Sub(const float* a, const float* b, float* out) { out[0] = a[0] - b[0], out[1] = a[1] - b[1], out[2] = a[2] - b[2]; }
void Add(const float* a, const float* b, float* out) { out[0] = b[0] + a[0], out[1] = b[1] + a[1], out[2] = b[2] + a[2]; }
void Scale(const float* a, float s, float* out) { out[0] = a[0] * s, out[1] = a[1] * s, out[2] = a[2] * s; }
void Copy3(const float* a, float* out) { out[0] = a[0], out[1] = a[1], out[2] = a[2]; }

// ---- RCamera_t ----

// FUN_1006e108: the inverse of an affine matrix (its 3x3 part by cofactors, then the translation); a singular one
// keeps 0 for 1 / determinant.
void __fastcall InvertAffine(float* m)
{
    xm::M4 r = xm::Identity();
    float* o = r.m;
    o[0] = m[10] * m[5] - m[9] * m[6];
    o[1] = m[2] * m[9] - m[10] * m[1];
    o[2] = m[6] * m[1] - m[2] * m[5];
    float d = m[8] * o[2] + m[0] * o[0] + m[4] * o[1];
    d = d == 0.0f ? 0.0f : 1.0f / d;
    o[0] = d * o[0];
    o[1] = d * o[1];
    o[2] = d * o[2];
    o[4] = d * (m[8] * m[6] - m[4] * m[10]);
    o[5] = d * (m[0] * m[10] - m[2] * m[8]);
    o[6] = d * (m[2] * m[4] - m[6] * m[0]);
    o[8] = d * (m[4] * m[9] - m[8] * m[5]);
    o[9] = d * (m[8] * m[1] - m[9] * m[0]);
    o[10] = d * (m[0] * m[5] - m[4] * m[1]);
    o[12] = -(m[14] * o[8] + m[13] * o[4] + m[12] * o[0]);
    o[13] = -(m[14] * o[9] + m[13] * o[5] + m[12] * o[1]);
    o[14] = -(m[14] * o[10] + m[13] * o[6] + m[12] * o[2]);
    std::memcpy(m, o, sizeof(r.m));
}

void SetWindow(Frame* c, float fov, float aspect)
{
    const float half = float(double(fov) * 0.5);
    const float t = float(std::tan(double(half)));
    Field<float>(c, kLeft) = -t;
    Field<float>(c, kRight) = t;
    const float top = t / aspect;
    Field<float>(c, kTop) = top;
    Field<float>(c, kBottom) = -t / aspect;
    Field<float>(c, kAspect) = t / top;
}

void InitCameraPart(Frame* c)                      // what every constructor sets after RRefFrame_t's
{
    SetVtable(c, kCameraVtable);
    std::memset(c + kSphere, 0, 12);
    std::memset(c + kForward, 0, 6 * 12);
    std::memcpy(c + kViewProjection, xm::Identity().m, 64);
    std::memcpy(c + kView, xm::Identity().m, 64);
}

void BecomeCurrent(Frame* c)
{
    if (!Global<void*>(kCurrentCamera)) Global<void*>(kCurrentCamera) = c;
}

void* __fastcall ConstructCamera(Frame* c, void*, float fov, float aspect, float front, float back, void* parent,
                                 void* animation)
{
    orig::RRefFrame_t_RRefFrame_t_35(c, parent, animation);
    InitCameraPart(c);
    Field<void*>(c, kOccluder) = nullptr;
    Field<void*>(c, kViewport) = nullptr;
    c[kOne] = 1;
    SetWindow(c, fov, aspect);
    Field<float>(c, kFront) = front;
    Field<float>(c, kBack) = back;
    Field<float>(c, kBackInUse) = back;
    BecomeCurrent(c);
    return c;
}

void* __fastcall CopyCamera(Frame* c, void*, Frame* from)      // FUN_1002a785
{
    orig::RRefFrame_t_RRefFrame_t_34(c, from);
    InitCameraPart(c);
    std::memcpy(c + kLeft, from + kLeft, kSphere - kLeft);     // the window, planes and aspect
    Field<void*>(c, kOccluder) = nullptr;
    Field<void*>(c, kViewport) = nullptr;
    c[kOne] = 1;
    BecomeCurrent(c);
    return c;
}

const char* const kWindowNames[] = {"w_left", "w_top", "w_right", "w_bottom", "front_plane", "back_plane"};

void* __fastcall ConstructCameraFrom(Frame* c, void*, void* archive)   // FUN_1002a949 (no occluder, not current)
{
    orig::RRefFrame_t_RRefFrame_t_36(c, archive);
    InitCameraPart(c);
    const serialize::Api& s = serialize::Get();
    void* stream = s.getStream(archive, nullptr);
    for (int i = 0; i < 6; ++i) s.findFloat(stream, nullptr, kWindowNames[i], &Field<float>(c, kLeft + 4 * i), 0);
    Field<float>(c, kBackInUse) = Field<float>(c, kBack);
    c[kOne] = 1;
    Field<float>(c, kAspect) = Field<float>(c, kRight) / Field<float>(c, kTop);
    return c;
}

void* __cdecl InstantiateCamera(void* archive)
{
    return ConstructCameraFrom(static_cast<Frame*>(vc10::Allocate(0x1A8)), nullptr, archive);
}

void* __fastcall CloneCamera(Frame* c)              // vtable slot 3 (FUN_1002bbb5)
{
    return CopyCamera(static_cast<Frame*>(vc10::Allocate(0x1A8)), nullptr, c);
}

void __fastcall DestroyCamera(Frame* c)              // FUN_1002a869
{
    SetVtable(c, kCameraVtable);
    if (Global<void*>(kCurrentCamera) == c) Global<void*>(kCurrentCamera) = nullptr;
    DestroyFrame(c);
}

void* __fastcall DeleteCamera(Frame* c, void*, uint8_t flags)   // vtable slot 0 (FUN_1002bbef)
{
    DestroyCamera(c);
    if (flags & 1) vc10::Free(c);
    return c;
}

void __fastcall ArchiveCamera(Frame* c, void*, void* archive)   // vtable slot 1 (FUN_1002aa66)
{
    orig::RRefFrame_t_Archive(c, archive);
    const serialize::Api& s = serialize::Get();
    void* stream = s.getStream(archive, nullptr);
    for (int i = 0; i < 6; ++i) s.addFloat(stream, nullptr, kWindowNames[i], Field<float>(c, kLeft + 4 * i));
}

void __fastcall GetViewPlaneWindow(Frame* c, void*, float* left, float* top, float* right, float* bottom)
{
    *left = Field<float>(c, kLeft);
    *top = Field<float>(c, kTop);
    *right = Field<float>(c, kRight);
    *bottom = Field<float>(c, kBottom);
}

void __fastcall SetViewPlaneWindow(Frame* c, void*, float fov, float aspect) { SetWindow(c, fov, aspect); }

void __fastcall GetViewMatrix(Frame* c, void*, float* out)
{
    std::memcpy(out, World(c), 64);
    InvertAffine(out);
}

// The projection: the view plane window onto -1..1, depth front..back onto 0..1.
void __fastcall GetTransformationMatrix(Frame* c, void*, float* out)
{
    const float w = Field<float>(c, kRight) - Field<float>(c, kLeft);
    const float h = Field<float>(c, kTop) - Field<float>(c, kBottom);
    const float back = Field<float>(c, kBack), front = Field<float>(c, kFront);
    const float q = back / (back - front);
    xm::M4 p = xm::Identity();
    p.m[0] = float(2.0 / double(w));
    p.m[5] = float(2.0 / double(h));
    p.m[10] = q;
    p.m[14] = -front * q;
    p.m[11] = 1.0f;
    p.m[15] = 0.0f;
    xm::M4 offset = xm::Identity();
    offset.m[12] = float(-1.0 - (double(Field<float>(c, kLeft)) / double(w)) * 2.0);
    offset.m[13] = float(1.0 - (double(Field<float>(c, kTop)) / double(h)) * 2.0);
    p = xm::Mul(p, offset);                          // FUN_1006e293
    std::memcpy(out, p.m, 64);
}

void RotatedBy(const float* v, const float* q, float* out)   // FUN_1002bb8a
{
    Copy3(v, out);
    xm::RotateVector(out, q);
}

// FUN_1002adf7: the matrices, the sphere around the frustum, the planes, the debugger's view of it and the occluder.
void __fastcall SetupFrustum(Frame* c, void*, void* viewport)
{
    Global<void*>(kCurrentCamera) = c;
    xm::M4 projection;
    GetTransformationMatrix(c, nullptr, projection.m);
    float* view = &Field<float>(c, kView);
    GetViewMatrix(c, nullptr, view);
    const xm::M4 vp = xm::Mul(xm::Load(view), projection);
    std::memcpy(c + kViewProjection, vp.m, 64);

    const float fog = Field<float>(*g_randy, 4);
    float& back = Field<float>(c, kBackInUse);
    back = fog;
    if (fog <= 0.0f || Field<float>(c, kBack) < fog) back = Field<float>(c, kBack);
    const float front = Field<float>(c, kFront);
    const float h = Field<float>(c, kTop) - Field<float>(c, kBottom);
    const float w = Field<float>(c, kRight) - Field<float>(c, kLeft);
    const float halfH = float(double(h) * 0.5);
    const float nearH = front * halfH, farH = back * halfH;
    const float mid = float(0.5 * double(back - front)) + front;
    const float farCorner[3] = {(farH * w) / h, farH, back};
    const float middle[3] = {0.0f, 0.0f, mid};
    float d[3];
    Sub(middle, farCorner, d);
    Field<float>(c, kSphereRadius) = float(std::sqrt(double(xm::LengthSquared(d))));
    const float* world = World(c);
    xm::Transform(middle, world, &Field<float>(c, kSphere));
    const float nearMiddle[3] = {0.0f, 0.0f, front};
    float nearPoint[3];
    xm::Transform(nearMiddle, world, nearPoint);

    const float* q = &Field<float>(c, kRotation);
    const float yAxis[3] = {0.0f, 1.0f, 0.0f};
    float up[3];
    RotatedBy(yAxis, q, up);
    const float* position = World(c) + 12;
    float back3[3];                                  // from the near plane back to the eye
    Sub(position, nearPoint, back3);
    xm::Normalize(back3);
    float right[3];
    xm::Cross(up, back3, right);
    xm::Normalize(right);
    xm::Cross(back3, right, up);
    const float nearW = (nearH * w) / h;

    float t[3], u[3], n[3];
    Sub(nearPoint, position, n);                     // forward
    xm::Normalize(n);
    Copy3(n, &Field<float>(c, kForward));

    float* planes = &Field<float>(c, kLeftPlane);    // left, right, top, bottom (normals pointing in)
    Scale(up, nearH, t);                             // top
    Add(nearPoint, t, u);
    Sub(u, position, n);
    xm::Normalize(n);
    xm::Cross(n, right, planes + 6);
    Scale(up, nearH, t);                             // bottom
    Sub(nearPoint, t, u);
    Sub(u, position, n);
    xm::Normalize(n);
    xm::Cross(right, n, planes + 9);
    Scale(right, nearW, t);                          // left
    Sub(nearPoint, t, u);
    Sub(u, position, n);
    xm::Normalize(n);
    xm::Cross(n, up, planes + 0);
    Scale(right, nearW, t);                          // right
    Add(nearPoint, t, u);
    Sub(u, position, n);
    xm::Normalize(n);
    xm::Cross(up, n, planes + 3);
    Sub(nearPoint, position, n);                     // back (along forward)
    xm::Normalize(n);
    Copy3(n, &Field<float>(c, kBackPlane));

    const float xAxis[3] = {1.0f, 0.0f, 0.0f};
    float worldX[3], worldY[3];
    RotatedBy(xAxis, q, worldX);
    RotatedBy(yAxis, q, worldY);
    const float aspect = Field<float>(c, kTop) / Field<float>(c, kRight);
    float* debugger = static_cast<float*>(orig::Debugger_t_Get());   // FUN_1002bf29
    Field<void*>(debugger, 0) = c + kViewProjection;
    Copy3(worldY, debugger + 1);
    Copy3(worldX, debugger + 4);
    debugger[7] = aspect;

    void* occluder = orig::HMOccluder_t_Get();
    Field<void*>(c, kViewport) = viewport;
    Field<void*>(c, kOccluder) = occluder;
    Internal<void(__fastcall*)(void*, void*, void*)>(0x3EDC1)(occluder, nullptr, viewport);
}

bool DebugCull() { return (Global<uint8_t>(kDebuggerMode) & 1) != 0; }

// A culling test's mark on the debugger's top-down view: where the sphere is, a thousandth of its distance.
void DebugMark(Frame* c, const float* center, float r, float g, float b)
{
    float d[3];
    Sub(center, World(c) + 12, d);
    const float x = d[0] / 1000.0f, z = d[2] / 1000.0f;
    Internal<void(__fastcall*)(void*, void*, float, float, float, float, float, float, bool)>(0x2CBAA)(
        orig::Debugger_t_Get(), nullptr, x, z, 0.1f, r, g, b, true);
}

// FUN_1002abe7: within the sphere around the frustum.
bool InSphere(Frame* c, const float* center, float radius)
{
    float d[3];
    Sub(center, &Field<float>(c, kSphere), d);
    const double r = double(Field<float>(c, kSphereRadius) + radius);
    if (!(r * r < double(xm::LengthSquared(d)))) return true;
    if (DebugCull()) DebugMark(c, center, 1.0f, 0.5f, 0.5f);
    return false;
}

// FUN_1002acaa: inside the four side planes and before the back plane.
bool InsidePlanes(Frame* c, const float* center, float radius)
{
    float d[3];
    Sub(center, World(c) + 12, d);
    const float limit = -radius;
    bool inside = true;
    for (int i = 0; i < 4 && inside; ++i) {
        const float* n = &Field<float>(c, kLeftPlane + 12 * i);
        const float distance = n[2] * d[2] + n[0] * d[0] + n[1] * d[1];
        if (limit > distance) inside = false;
    }
    if (inside) {
        const float* n = &Field<float>(c, kBackPlane);
        const float distance = n[2] * d[2] + n[0] * d[0] + n[1] * d[1];
        if (!(Field<float>(c, kBackInUse) + radius < distance)) return true;
    }
    if (DebugCull()) DebugMark(c, center, 1.0f, 0.5f, 0.5f);
    return false;
}

// FUN_1002b2bb: the sphere's screen rectangle not hidden behind the terrain.
bool NotOccluded(Frame* c, const float* center, float radius)
{
    float p[3];
    xm::Transform(center, &Field<float>(c, kViewProjection), p);
    const float aspect = Field<float>(c, kAspect);
    const float invZ = 1.0f / p[2];
    const float invA = 1.0f / aspect;
    const float ra = radius * aspect;
    const float bottom = (invZ * (p[1] - ra)) * invA;
    const float top = ((p[1] + ra) * invZ) * invA;
    const float left = (p[0] - ra) * invZ;
    const float right = (p[0] + ra) * invZ;
    if (Internal<bool(__fastcall*)(void*, void*, void*, float, float, float, float, float)>(0x3E8F6)(
            Field<void*>(c, kOccluder), nullptr, Field<void*>(c, kViewport), left, right, bottom, top, p[2]))
        return true;
    if (DebugCull()) DebugMark(c, center, 1.0f, 0.0f, 0.0f);
    return false;
}

// FUN_1002b3e4: can a sphere be seen.
bool __fastcall Sees(Frame* c, void*, const float* center, float radius)
{
    if (!InSphere(c, center, radius) || !InsidePlanes(c, center, radius) || !NotOccluded(c, center, radius))
        return false;
    if (DebugCull()) DebugMark(c, center, 1.0f, 1.0f, 1.0f);
    return true;
}

// ---- RLight_t ----

uint32_t D3DType(int32_t type) { return type == 2 ? 1 : type == 4 ? 2 : 3; }   // FUN_1003fcbe
int32_t TypeOf(uint32_t d3dType) { return d3dType == 1 ? 2 : d3dType == 2 ? 4 : 1; }               // FUN_1003fca1

void LightsChanged() { Global<uint8_t>(kLightsChanged) = 1; }

void InitLightPart(Frame* l)
{
    SetVtable(l, kLightVtable);
    Field<uint32_t>(l, kIndex) = 0;
    Field<uint32_t>(l, 0x114) = 0;
    l[kEnabled] = 0;
}

void* __fastcall ConstructLight(Frame* l, void*, void* parent, const float* rgb, int32_t type, void* animation)
{
    orig::RRefFrame_t_RRefFrame_t_35(l, parent, animation);
    InitLightPart(l);
    std::memset(l + kLight, 0, kLightBytes);
    float* d = &Field<float>(l, kLight);
    Field<uint32_t>(l, kLight) = D3DType(type);
    d[1] = rgb[0], d[2] = rgb[1], d[3] = rgb[2], d[4] = 1.0f;     // diffuse
    d[5] = rgb[0], d[6] = rgb[1], d[7] = rgb[2], d[8] = 1.0f;     // specular
    d[9] = d[10] = d[11] = d[12] = 0.0f;                          // ambient
    d[13] = d[14] = d[15] = 0.0f;                                 // position
    d[16] = d[17] = 0.0f, d[18] = 1.0f;                           // direction
    d[19] = float(std::sqrt(3.4028234663852886e+38));             // range: sqrt(FLT_MAX)
    d[20] = 1.0f;                                                 // falloff
    d[21] = 0.0f, d[22] = 1.0f, d[23] = 0.0f;                     // attenuation
    Field<uint32_t>(l, kTheta) = 0x3EB2A191;                      // 0.3488889 (_DAT_10095438)
    Field<uint32_t>(l, kPhi) = 0x3F32A191;                        // 0.6977778 (_DAT_10095434)
    LightsChanged();
    Field<int32_t>(l, kType) = type;
    return l;
}

void* __fastcall CopyLight(Frame* l, void*, Frame* from)       // FUN_1003fb6b
{
    orig::RRefFrame_t_RRefFrame_t_34(l, from);
    SetVtable(l, kLightVtable);
    std::memcpy(l + kLight, from + kLight, kLightBytes);
    InitLightPart(l);
    LightsChanged();
    Field<int32_t>(l, kType) = Field<int32_t>(from, kType);
    return l;
}

void* __fastcall ConstructLightFrom(Frame* l, void*, void* archive)
{
    orig::RRefFrame_t_RRefFrame_t_36(l, archive);
    InitLightPart(l);
    const serialize::Api& s = serialize::Get();
    void* stream = s.getStream(archive, nullptr);
    std::memset(l + kLight, 0, kLightBytes);
    LightsChanged();
    const void* data = nullptr;
    int32_t bytes = 0;
    s.findData(stream, nullptr, "light_info", &data, &bytes, 0);
    std::memcpy(l + kLight, data, kLightBytes);
    Field<int32_t>(l, kType) = TypeOf(Field<uint32_t>(l, kLight));
    return l;
}

void* __cdecl InstantiateLight(void* archive)
{
    return ConstructLightFrom(static_cast<Frame*>(vc10::Allocate(0x11C)), nullptr, archive);
}

void* __fastcall CloneLight(Frame* l)                // vtable slot 3 (FUN_10016c78)
{
    return CopyLight(static_cast<Frame*>(vc10::Allocate(0x11C)), nullptr, l);
}

void __fastcall DestroyLight(Frame* l)               // FUN_1003fbc6
{
    SetVtable(l, kLightVtable);
    LightsChanged();
    DestroyFrame(l);
}

void* __fastcall DeleteLight(Frame* l, void*, uint8_t flags)   // vtable slot 0 (FUN_100170b0)
{
    DestroyLight(l);
    if (flags & 1) vc10::Free(l);
    return l;
}

void __fastcall ArchiveLight(Frame* l, void*, void* archive)
{
    orig::RRefFrame_t_Archive(l, archive);
    const serialize::Api& s = serialize::Get();
    void* stream = s.getStream(archive, nullptr);
    s.addInt32(stream, nullptr, "version", 1);
    s.addData(stream, nullptr, "light_info", l + kLight, kLightBytes, true, 1);
}

void __fastcall AddLightChild(Frame* l, void*, void* child)   // vtable slot 4 (FUN_1003fc21)
{
    orig::RRefFrame_t_AddChild(l, child);
    LightsChanged();
}

void __fastcall RemoveLightChild(Frame* l, void*, void* child)   // vtable slot 12 (FUN_1003fc37)
{
    orig::RRefFrame_t_RemoveChild(l, child);
    LightsChanged();
}

const char* __fastcall TypeName(Frame* l)            // vtable slot 10 (FUN_1003fc4d)
{
    switch (Field<int32_t>(l, kType)) {
    case 1: return "Point";
    case 2: return "Spot";
    case 3: return "Directional";
    default: return "???";
    }
}

void __fastcall Enable(Frame* l, void*, bool enable)
{
    if (uint8_t(enable) == l[kEnabled]) return;
    l[kEnabled] = enable;
    Internal<void(__fastcall*)(void*, void*, uint32_t, uint32_t)>(0x218BE)(Global<void*>(kRender), nullptr,
                                                                         Field<uint32_t>(l, kIndex), enable);
}

void __fastcall SetSpotAngles(Frame* l, void*, float inner, float outer)
{
    Field<float>(l, kTheta) = float(double(inner) * 2.0);
    Field<float>(l, kPhi) = float(2.0 * double(outer));
}

// Vtable slot 8 (FUN_1003ff72): where it is and points to in the world (when that changed), then into the light list.
void __fastcall ProcessLight(Frame* l)
{
    orig::RRefFrame_t_Process(l);
    if (l[kChanged]) {
        const float* m = World(l);
        Copy3(m + 12, &Field<float>(l, kPosition));
        float v[3] = {0.0f, 0.0f, 1.0f};             // FUN_10012d75: through the matrix's 3x3
        const float x = v[0], y = v[1], z = v[2];
        float* dir = &Field<float>(l, kDirection);
        dir[0] = m[8] * z + x * m[0] + m[4] * y;
        dir[1] = m[9] * z + m[5] * y + m[1] * x;
        dir[2] = m[10] * z + m[6] * y + m[2] * x;
        ++Global<uint32_t>(kLightsMoved);
        LightsChanged();
    }
    auto& lights = Global<vc10::Vector<void*>>(kLightList);
    Field<uint32_t>(l, kIndex) = uint32_t(lights.size());
    lights.push_back(l);
}

}  // namespace

void Install(HMODULE orig)
{
    if (GetMode("Scene", Mode::Off) != Mode::On) return;
    g_orig = orig;
    g_randy = reinterpret_cast<void* const*>(GetProcAddress(orig, "?s_pcRandy@Randy_t@@1PAV1@A"));
    const serialize::Api& s = serialize::Get();
    if (!g_randy || !s.complete || !s.addData || !s.findData) {
        Log("cameras: serialize.dll / randy31 exports missing - not replaced");
        return;
    }
    struct Entry {
        uint32_t rva;
        void* target;
        const char* what;
    };
#define FN(f) reinterpret_cast<void*>(&f)
    const Entry entries[] = {
        {0x6E108, FN(InvertAffine), "affine inverse (FUN_1006e108)"},
        {0x2A68A, FN(ConstructCamera), "RCamera_t::RCamera_t"},
        {0x2A785, FN(CopyCamera), "RCamera_t::RCamera_t(copy) (FUN_1002a785)"},
        {0x2A949, FN(ConstructCameraFrom), "RCamera_t::RCamera_t(archive) (FUN_1002a949)"},
        {0x2ADBF, FN(InstantiateCamera), "RCamera_t::Instantiate"},
        {0x2BBB5, FN(CloneCamera), "RCamera_t clone (FUN_1002bbb5)"},
        {0x2A869, FN(DestroyCamera), "RCamera_t::~RCamera_t (FUN_1002a869)"},
        {0x2BBEF, FN(DeleteCamera), "RCamera_t deleting destructor (FUN_1002bbef)"},
        {0x2AA66, FN(ArchiveCamera), "RCamera_t::Archive (FUN_1002aa66)"},
        {0x2A883, FN(GetViewPlaneWindow), "RCamera_t::GetViewPlaneWindow"},
        {0x2A8E1, FN(SetViewPlaneWindow), "RCamera_t::SetViewPlaneWindow"},
        {0x2AB00, FN(GetViewMatrix), "RCamera_t::GetViewMatrix"},
        {0x2AB22, FN(GetTransformationMatrix), "RCamera_t::GetTransformationMatrix"},
        {0x2ADF7, FN(SetupFrustum), "RCamera_t frustum (FUN_1002adf7)"},
        {0x2B3E4, FN(Sees), "RCamera_t sphere test (FUN_1002b3e4)"},
        {0x3FD4A, FN(ConstructLight), "RLight_t::RLight_t"},
        {0x3FB6B, FN(CopyLight), "RLight_t::RLight_t(copy) (FUN_1003fb6b)"},
        {0x3FE81, FN(ConstructLightFrom), "RLight_t::RLight_t(archive)"},
        {0x3FF26, FN(InstantiateLight), "RLight_t::Instantiate"},
        {0x16C78, FN(CloneLight), "RLight_t clone (FUN_10016c78)"},
        {0x3FBC6, FN(DestroyLight), "RLight_t::~RLight_t (FUN_1003fbc6)"},
        {0x170B0, FN(DeleteLight), "RLight_t deleting destructor (FUN_100170b0)"},
        {0x3FBD8, FN(ArchiveLight), "RLight_t::Archive"},
        {0x3FC21, FN(AddLightChild), "RLight_t::AddChild (FUN_1003fc21)"},
        {0x3FC37, FN(RemoveLightChild), "RLight_t::RemoveChild (FUN_1003fc37)"},
        {0x3FC4D, FN(TypeName), "RLight_t type name (FUN_1003fc4d)"},
        {0x3FC74, FN(Enable), "RLight_t::Enable"},
        {0x3FCDE, FN(SetSpotAngles), "RLight_t::SetSpotAngles"},
        {0x3FF72, FN(ProcessLight), "RLight_t::Process (FUN_1003ff72)"},
    };
#undef FN
    int installed = 0;
    for (const Entry& e : entries) installed += Replace(orig, e.rva, e.target, e.what) ? 1 : 0;
    Log("cameras: %d of %d functions native", installed, int(sizeof(entries) / sizeof(entries[0])));
}

}  // namespace rnative::camera
