// RSprite and RSpriteAnim natively (part of [Native] Scene=on): a textured quad at a frame - turned to the camera
// (modes 1 and 2), around an axis (3) or as the frame is (4) - with an optional grid of animation frames, additive
// blending, and its archives. DisplaySystem makes them (RSprite::RSprite, EnableAdditiveRendering,
// InitVertexColor). Arithmetic in the original's x87 order (xmath.h), so to the bit.
//
// RSprite (an RVisual_t, 0x294 bytes; a second vtable at +0xA4): +0x178 / +0x17C half width / height, +0x180 / +0x184
// the centre's offset, +0x188 rotation (always 3.14: the corners use a fixed 3.14 radians), +0x18C colour, +0x190 the
// texture rectangle (u0 v0 u1 v1), +0x1A0 four vertices (x y z, colour, u v: XYZ | DIFFUSE | TEX1), +0x200
// RSpriteAnim, +0x204 RMaterial_t, +0x208 mode, +0x20C additive, +0x210 StateBlob_c.
// RSpriteAnim (a Serializable_c, 0x30 bytes): +0x08 the first frame's rectangle, +0x18 / +0x1C a frame's step in u / v,
// +0x20 frames in a row, +0x24 frames, +0x28 a frame's time, +0x2C looping.
#include "native/sprite.h"
#include "native/cat_render.h"
#include "native/orig_api.gen.h"
#include "native/serialize.h"
#include "native/vc10.h"
#include "native/xmath.h"

#include <cmath>
#include <cstring>
#include <initializer_list>

namespace rnative::sprite {

namespace {

HMODULE g_orig;

template <typename T>
T& Field(void* object, uint32_t offset) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(object) + offset); }
template <typename T>
T& Global(uint32_t rva) { return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(g_orig) + rva); }
template <typename F>
F Internal(uint32_t rva) { return reinterpret_cast<F>(reinterpret_cast<uint8_t*>(g_orig) + rva); }

constexpr uint32_t kVtable = 0x8A644, kVtable2 = 0x8A634, kAnimVtable = 0x8A624;
constexpr uint32_t kHalfW = 0x178, kHalfH = 0x17C, kOffX = 0x180, kOffY = 0x184, kRot = 0x188, kColor = 0x18C,
                   kRect = 0x190, kVerts = 0x1A0, kAnim = 0x200, kMaterial = 0x204, kMode = 0x208, kAdditive = 0x20C,
                   kBlob = 0x210, kDelta = 0xB8, kObservers = 0xB0;
constexpr uint32_t kSerializableTd = 0xB60D4, kMaterialTd = 0xB617C, kAnimTd = 0xB6148;
constexpr uint32_t kFacing = 0xB773C, kUp = 0xB7748, kD3dMaterial = 0xB9660;

uint32_t Addr(uint32_t rva) { return uint32_t(reinterpret_cast<uintptr_t>(g_orig)) + rva; }
float& F(uint8_t* s, uint32_t at) { return Field<float>(s, at); }
float* Vertex(uint8_t* s, int i) { return &Field<float>(s, kVerts + uint32_t(i) * 0x18); }   // x y z, colour, u v

void* DynamicCast(void* object, uint32_t td)
{
    using Fn = void*(__cdecl*)(void*, long, void*, void*, int);
    static const auto cast = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleA("msvcr100.dll"), "__RTDynamicCast"));
    return cast(object, 0, &Global<uint8_t>(kSerializableTd), &Global<uint8_t>(td), 0);
}

// ArchiveStream_c::FindObject<T> (FUN_10013d19 / FUN_10013d62): `out` set if found and a T (or none).
int32_t FindObject(void* stream, const char* name, uint32_t td, void** out)
{
    void* object = nullptr;
    int32_t error = serialize::Get().findObject(stream, nullptr, name, &object, 0);
    if (error) return error;
    void* cast = object ? DynamicCast(object, td) : nullptr;
    if (object && !cast) return 1;
    *out = cast;
    return 0;
}

// ---- RSpriteAnim ----

// FUN_10012eb0: the texture rectangle of the frame at `time` (held at the last or looped).
void __fastcall AnimRect(uint8_t* a, void*, float time, float* rect)
{
    const float q = float((double)time / (double)Field<float>(a, 0x28));
    const float whole = float(std::floor((double)q));
    uint32_t frame = uint32_t(int64_t(whole));      // _ftol2: its low 32 bits
    const uint32_t count = Field<uint32_t>(a, 0x24);
    if (Field<bool>(a, 0x2C)) frame %= count;
    else if (frame >= count) frame = count - 1;
    const uint32_t columns = Field<uint32_t>(a, 0x20);
    const float du = float((double)Field<float>(a, 0x18) * (double)(frame % columns));
    const float dv = float((double)Field<float>(a, 0x1C) * (double)(frame / columns));
    rect[0] = float((double)Field<float>(a, 0x08) + du);
    rect[1] = float((double)Field<float>(a, 0x0C) + dv);
    rect[2] = float((double)Field<float>(a, 0x10) + du);
    rect[3] = float((double)Field<float>(a, 0x14) + dv);
}

uint8_t* __fastcall AnimCopy(uint8_t* a, void*, const uint8_t* from)   // FUN_10012e56
{
    serialize::Get().construct(a, nullptr);
    Field<uint32_t>(a, 0) = Addr(kAnimVtable);
    std::memcpy(a + 8, from + 8, 0x24);
    a[0x2C] = from[0x2C];
    return a;
}

// FUN_10012f53: from an archive - which holds none of its fields (they are left as they were).
uint8_t* __fastcall AnimFromArchive(uint8_t* a, void*, void* archive)
{
    serialize::Get().constructFrom(a, nullptr, archive);
    Field<uint32_t>(a, 0) = Addr(kAnimVtable);
    return a;
}

void* __cdecl AnimInstantiate(void* archive)        // ?Instantiate@RSpriteAnim@@SAPAV1@PAVObjectArchive_c@fun@@@Z
{
    return AnimFromArchive(static_cast<uint8_t*>(vc10::Allocate(0x30)), nullptr, archive);
}

void __fastcall AnimDestroy(uint8_t* a)             // FUN_10012ea4
{
    Field<uint32_t>(a, 0) = Addr(kAnimVtable);
    serialize::Get().destroy(a, nullptr);
}

void* __fastcall AnimDeletingDestroy(uint8_t* a, void*, uint32_t flags)   // FUN_10013db6 (slot 0)
{
    AnimDestroy(a);
    if (flags & 1) vc10::Free(a);
    return a;
}

void __fastcall AnimArchive(uint8_t* a, void*, void* archive)   // FUN_10012f91 (slot 1): its base's, nothing more
{
    using Fn = void(__fastcall*)(void*, void*, void*);
    static const auto archiveBase = reinterpret_cast<Fn>(GetProcAddress(
        GetModuleHandleA("serialize.dll"), "?Archive@Serializable_c@fun@@UBEXPAVObjectArchive_c@2@@Z"));
    archiveBase(a, nullptr, archive);
}

// ---- RSprite ----

// FUN_100132b3: the corners, from the size and the centre's offset, turned by 3.14 radians (the rotation itself is
// set to 3.14, never read).
void __fastcall Corners(uint8_t* s)
{
    const float cx = F(s, kOffX), cy = F(s, kOffY), w = F(s, kHalfW), h = F(s, kHalfH);
    const float a = float((1.0 - cx) * w), b = float((1.0 - cy) * h);
    const float c = float((-1.0 - cx) * w), d = float((-1.0 - cy) * h);
    F(s, kRot) = 3.14f;
    const double angle = (double)3.14f;              // (0x1008a708)
    const float cs = float(xm::CrtCos(angle)), sn = float(xm::CrtSin(angle));
    const float corner[4][2] = {
        {float((double)cs * a + (double)sn * b), float((double)cs * b - (double)sn * a)},
        {float((double)cs * a + (double)sn * d), float((double)cs * d - (double)sn * a)},
        {float((double)cs * c + (double)sn * b), float((double)cs * b - (double)sn * c)},
        {float((double)cs * c + (double)sn * d), float((double)cs * d - (double)sn * c)}};
    const bool flat = Field<int32_t>(s, kMode) == 3;  // around y: in the xz plane
    for (int i = 0; i < 4; ++i) {
        float* v = Vertex(s, i);
        v[0] = corner[i][0];
        v[1] = flat ? 0.0f : corner[i][1];
        v[2] = flat ? -corner[i][1] : 0.0f;
    }
}

void __fastcall Uvs(uint8_t* s)                     // FUN_100134a1: the corners' u v from the texture rectangle
{
    const float* r = &F(s, kRect);
    const float uv[4][2] = {{r[0], r[3]}, {r[0], r[1]}, {r[2], r[3]}, {r[2], r[1]}};
    for (int i = 0; i < 4; ++i) {
        Vertex(s, i)[4] = uv[i][0];
        Vertex(s, i)[5] = uv[i][1];
    }
}

}  // namespace

// ?InitVertexColor@RSprite@@IAEXXZ: every corner its colour.
void __fastcall InitVertexColor(uint8_t* s)
{
    for (int i = 0; i < 4; ++i) Field<uint32_t>(Vertex(s, i), 12) = Field<uint32_t>(s, kColor);
}

namespace {

// FUN_10013575: corners, u v, colours, and its states: no lighting, Z on, alpha test (greater, 0x1E), modulate.
void __fastcall Setup(uint8_t* s)
{
    Corners(s);
    Uvs(s);
    InitVertexColor(s);
    void* blob = s + kBlob;
    orig::StateBlob_c_SetRenderState(blob, 0x89, 0);
    orig::StateBlob_c_SetRenderState(blob, 0xE, 1);
    orig::StateBlob_c_SetRenderState(blob, 0xF, 1);
    orig::StateBlob_c_SetRenderState(blob, 0x19, 5);
    orig::StateBlob_c_SetRenderState(blob, 0x18, 0x1E);
    orig::StateBlob_c_SetTextureStageState(blob, 0, 1, 4);
    orig::StateBlob_c_SetTextureStageState(blob, 0, 2, 2);
    orig::StateBlob_c_SetTextureStageState(blob, 0, 3, 0);
}

void Common(uint8_t* s)                             // the vtables, colour, vertices of every constructor
{
    Field<uint32_t>(s, 0) = Addr(kVtable);
    Field<uint32_t>(s, 0xA4) = Addr(kVtable2);
    orig::Color_t_Color_t_16(s + kColor);
    for (int i = 0; i < 4; ++i) {                   // FUN_1002ed38: position 0, a default colour
        float* v = Vertex(s, i);
        v[0] = v[1] = v[2] = 0.0f;
        orig::Color_t_Color_t_16(v + 3);
    }
}

}  // namespace

// ??0RSprite@@QAE@PBVRMaterial_t@@PBVRSpriteAnim@@MMPAVRRefFrame_t@@W4SpriteMode_e@0@@Z
uint8_t* __fastcall Construct(uint8_t* s, void*, void* material, void* anim, float width, float height, void* parent,
                              int32_t mode)
{
    orig::RVisual_t_RVisual_t_49(s, parent, nullptr);
    Common(s);
    Field<void*>(s, kAnim) = anim;
    Field<void*>(s, kMaterial) = material;
    Field<int32_t>(s, kMode) = mode;
    s[kAdditive] = 0;
    orig::StateBlob_c_StateBlob_c(s + kBlob);
    orig::RResource_t_AddRefRResource(material);
    if (Field<uint8_t>(material, 0xBD)) orig::RVisual_t_SetRenderPriority(s, 6);   // a transparent material
    F(s, kHalfW) = float((double)width * 0.5);
    F(s, kHalfH) = float(0.5 * (double)height);
    F(s, kOffX) = F(s, kOffY) = 0.0f;
    float* r = &F(s, kRect);
    r[0] = r[1] = 0.0f;
    r[2] = r[3] = 1.0f;
    Setup(s);
    return s;
}

namespace {

// FUN_100136f9: a copy (its animation copied too).
uint8_t* __fastcall Copy(uint8_t* s, void*, uint8_t* from)
{
    orig::RVisual_t_RVisual_t_48(s, from);
    Common(s);
    Field<void*>(s, kAnim) = AnimCopy(static_cast<uint8_t*>(vc10::Allocate(0x30)), nullptr, Field<uint8_t*>(from, kAnim));
    Field<void*>(s, kMaterial) = Field<void*>(from, kMaterial);
    Field<int32_t>(s, kMode) = Field<int32_t>(from, kMode);
    s[kAdditive] = 0;
    orig::StateBlob_c_StateBlob_c(s + kBlob);
    orig::RResource_t_AddRefRResource(Field<void*>(s, kMaterial));
    std::memcpy(s + kHalfW, from + kHalfW, 8);
    std::memcpy(s + kOffX, from + kOffX, 8);
    std::memcpy(s + kRect, from + kRect, 16);
    Setup(s);
    return s;
}

void* __fastcall Duplicate(uint8_t* s)              // FUN_10013df4 (slot 3)
{
    return Copy(static_cast<uint8_t*>(vc10::Allocate(0x294)), nullptr, s);
}

// FUN_100138cc: from an archive (a missing "mode" leaves the stream's address there, as the original).
uint8_t* __fastcall FromArchive(uint8_t* s, void*, void* archive)
{
    Internal<void*(__fastcall*)(void*, void*, void*)>(0x4D6D3)(s, nullptr, archive);   // RVisual_t(archive)
    Common(s);
    orig::StateBlob_c_StateBlob_c(s + kBlob);
    const serialize::Api& a = serialize::Get();
    void* stream = a.getStream(archive, nullptr);
    Field<void*>(s, kMaterial) = nullptr;
    FindObject(stream, "material", kMaterialTd, &Field<void*>(s, kMaterial));
    FindObject(stream, "anim", kAnimTd, &Field<void*>(s, kAnim));
    a.findFloat(stream, nullptr, "width", &F(s, kHalfW), 0);
    a.findFloat(stream, nullptr, "height", &F(s, kHalfH), 0);
    a.findFloat(stream, nullptr, "cxoff", &F(s, kOffX), 0);
    a.findFloat(stream, nullptr, "cyoff", &F(s, kOffY), 0);
    a.findFloat(stream, nullptr, "rot", &F(s, kRot), 0);
    const void* rect = nullptr;
    int32_t bytes = 0;
    if (a.findData(stream, nullptr, "texturerect", &rect, &bytes, 0) == 0) std::memcpy(s + kRect, rect, 16);
    int32_t mode = 0;
    Field<int32_t>(s, kMode) =
        a.findInt32(stream, nullptr, "mode", &mode, 0) == 0 ? mode : int32_t(reinterpret_cast<uintptr_t>(stream));
    a.findBool(stream, nullptr, "additive", &Field<bool>(s, kAdditive), 0);
    float rgb[3] = {0.0f, 0.0f, 0.0f};
    a.findRgb(stream, nullptr, "diffuse_col", rgb, 0);
    uint32_t color;
    orig::Color_t_Color_t_14(&color, rgb);
    Field<uint32_t>(s, kColor) = color;
    Setup(s);
    return s;
}

void* __cdecl Instantiate(void* archive)            // ?Instantiate@RSprite@@SAPAV1@PAVObjectArchive_c@fun@@@Z
{
    return FromArchive(static_cast<uint8_t*>(vc10::Allocate(0x294)), nullptr, archive);
}

// FUN_10013018 (slot 1).
void __fastcall Archive(uint8_t* s, void*, void* archive)
{
    orig::RVisual_t_Archive(s, archive);
    const serialize::Api& a = serialize::Get();
    void* stream = a.getStream(archive, nullptr);
    a.addObject(stream, nullptr, "material", Field<void*>(s, kMaterial));
    a.addObject(stream, nullptr, "anim", Field<void*>(s, kAnim));
    a.addFloat(stream, nullptr, "width", F(s, kHalfW));
    a.addFloat(stream, nullptr, "height", F(s, kHalfH));
    a.addFloat(stream, nullptr, "cxoff", F(s, kOffX));
    a.addFloat(stream, nullptr, "cyoff", F(s, kOffY));
    a.addFloat(stream, nullptr, "rot", F(s, kRot));
    a.addData(stream, nullptr, "texturerect", s + kRect, 0x10, true, 1);
    a.addInt32(stream, nullptr, "mode", Field<int32_t>(s, kMode));
    a.addBool(stream, nullptr, "additive", Field<bool>(s, kAdditive));
    const uint8_t* c = s + kColor;                  // FUN_10012cb1: b g r a bytes to r g b
    const float scale = float(Global<double>(0x8A618));   // 1 / 255
    const float rgb[3] = {float((double)c[2] * scale), float((double)c[1] * scale), float((double)c[0] * scale)};
    a.addRgb(stream, nullptr, "diffuse_col", rgb);
}

// FUN_10012faa: its material and animation let go, the state blob, RVisual_t.
void __fastcall Destroy(uint8_t* s)
{
    Field<uint32_t>(s, 0) = Addr(kVtable);
    Field<uint32_t>(s, 0xA4) = Addr(kVtable2);
    if (void* m = Field<void*>(s, kMaterial)) orig::RResource_t_ReleaseRResource(m);
    if (void* anim = Field<void*>(s, kAnim))
        reinterpret_cast<void*(__fastcall*)(void*, void*, uint32_t)>((*static_cast<void***>(anim))[0])(anim, nullptr, 1);
    Internal<void(__fastcall*)(void*)>(0x255B5)(s + kBlob);   // StateBlob_c::~StateBlob_c
    Internal<void(__fastcall*)(void*)>(0x4D7D3)(s);          // RVisual_t::~RVisual_t
}

void* __fastcall DeletingDestroy(uint8_t* s, void*, uint32_t flags)   // FUN_10013dd5 (slot 0)
{
    Destroy(s);
    if (flags & 1) vc10::Free(s);
    return s;
}

void* __fastcall DeletingDestroy2(uint8_t* s, void*, uint32_t flags)  // FUN_10013dab: through the second vtable
{
    return DeletingDestroy(s - 0xA4, nullptr, flags);
}

// FUN_10013515 (slot 21): its animation at `time` into the texture rectangle.
void __fastcall Animate(uint8_t* s, void*, float time)
{
    AnimRect(Field<uint8_t*>(s, kAnim), nullptr, time, &F(s, kRect));
    Uvs(s);
}

// ---- drawing ----

// FUN_10012d75: v through the 3x3 of m (row vector).
void Rotate(const float* m, float* v)
{
    const float y = float(((double)m[1] * v[0] + (double)m[5] * v[1]) + (double)m[9] * v[2]);
    const float z = float(((double)m[2] * v[0] + (double)m[6] * v[1]) + (double)m[10] * v[2]);
    v[0] = float(((double)m[4] * v[1] + (double)v[0] * m[0]) + (double)m[8] * v[2]);
    v[1] = y;
    v[2] = z;
}

void CrossX87(float* a, const float* b)             // FUN_1002a358: a = a x b
{
    const float x = float((double)b[2] * a[1] - (double)b[1] * a[2]);
    const float y = float((double)a[2] * b[0] - (double)a[0] * b[2]);
    a[2] = float((double)b[1] * a[0] - (double)b[0] * a[1]);
    a[0] = x;
    a[1] = y;
}

void NormalizeX87(float* v)                         // FUN_10018833(v, 1): FUN_10017f2b's 1 / length, times 1
{
    const float squared = float(((double)v[0] * v[0] + (double)v[1] * v[1]) + (double)v[2] * v[2]);
    const float length = float(std::sqrt((double)squared));
    const float f = float((double)float(1.0 / (double)length) * 1.0);
    v[0] = float((double)v[0] * f);
    v[1] = float((double)v[1] * f);
    v[2] = float((double)v[2] * f);
}

// FUN_1006ec17: m's rows from a direction and an up (x = dir x up, the normalised dir, their cross); identity if both
// are nothing.
void Facing(float* m, const float* dir, const float* up)
{
    float x[3] = {dir[0], dir[1], dir[2]};
    CrossX87(x, up);
    if (x[0] == 0.0f && x[1] == 0.0f && x[2] == 0.0f && dir[0] == 0.0f && dir[1] == 0.0f && dir[2] == 0.0f) {
        const xm::M4 id = xm::Identity();
        std::memcpy(m, id.m, 64);
        return;
    }
    NormalizeX87(x);
    float n[3] = {dir[0], dir[1], dir[2]};
    NormalizeX87(n);
    float y[3] = {n[0], n[1], n[2]};
    CrossX87(y, x);
    std::memcpy(m, x, 12);
    std::memcpy(m + 4, y, 12);
    std::memcpy(m + 8, n, 12);
}

void VirtualViewport(uint8_t* s, uint32_t slot, void* viewport)   // StoreStateChanges / Apply / Restore
{
    reinterpret_cast<void(__fastcall*)(void*, void*, void*)>((*reinterpret_cast<void***>(s))[slot])(s, nullptr, viewport);
}

// FUN_10013ab2 (slot 13): its world matrix by mode, its states, the strip of four.
void __fastcall Render(uint8_t* s, void*, uint8_t* viewport)
{
    if (!orig::StateBlob_c_Validate(s + kBlob)) return;
    const float* world = static_cast<const float*>(orig::RRefFrame_t_GetWorldMatrix(s));
    float m[16];
    const xm::M4 id = xm::Identity();               // FUN_10012d2e: at the frame's position
    std::memcpy(m, id.m, 64);
    m[12] = world[12], m[13] = world[13], m[14] = world[14];
    void* camera = Field<void*>(viewport, 0xC);
    switch (Field<int32_t>(s, kMode)) {
    case 1:                                         // the camera's orientation
        std::memcpy(m, orig::RRefFrame_t_GetWorldMatrix(camera), 64);
        m[12] = world[12], m[13] = world[13], m[14] = world[14];
        break;
    case 2: {                                       // the facing direction turned with the camera
        float dir[3] = {Global<float>(kFacing), Global<float>(kFacing + 4), Global<float>(kFacing + 8)};
        Rotate(static_cast<const float*>(orig::RRefFrame_t_GetWorldMatrix(camera)), dir);
        Facing(m, dir, &Global<float>(kUp));
        break;
    }
    case 3: {                                       // its own facing, up towards the camera
        float dir[3] = {Global<float>(kFacing), Global<float>(kFacing + 4), Global<float>(kFacing + 8)};
        const float* cam = static_cast<const float*>(orig::RRefFrame_t_GetWorldMatrix(camera));
        const float toCamera[3] = {cam[12] - world[12], cam[13] - world[13], cam[14] - world[14]};   // FUN_10012cf0
        Rotate(world, dir);
        Facing(m, dir, toCamera);
        break;
    }
    case 4:
        std::memcpy(m, world, 64);
        break;
    default:
        break;
    }
    void* material = Field<void*>(s, kMaterial);
    void* render = Global<void*>(0x16BED0);
    if (!Field<void*>(s, kObservers)) {
        VirtualViewport(s, 16, viewport);
        VirtualViewport(s, 17, viewport);
        orig::RViewPort_t_SetMaterial(viewport, material);
        orig::render_t_SetTransformMatrix(render, 1, m);
        orig::StateBlob_c_Set(s + kBlob);
        orig::RViewPort_t_RealizeRenderStates(viewport);
        orig::render_t_RenderTriangleStrip_409(render, 0x142, s + kVerts, 4, 0);
        orig::StateBlob_c_Reset(s + kBlob);
        orig::RViewPort_t_ResetMaterial(viewport);
        VirtualViewport(s, 18, viewport);
        return;
    }
    if (catrender::ObserversSkip(s, viewport)) return;
    VirtualViewport(s, 16, viewport);
    VirtualViewport(s, 17, viewport);
    orig::render_t_SetTransformMatrix(render, 1, m);
    orig::RViewPort_t_SetMaterial(viewport, material);
    void* d3dMaterial = &Global<uint8_t>(kD3dMaterial);
    orig::RMaterial_t_InitD3DMaterial(material, d3dMaterial);
    if (!catrender::ObserversMaterial(s, material, viewport, d3dMaterial)) orig::RViewPort_t_SetD3DMaterial(viewport, d3dMaterial);
    catrender::ObserversAfterMaterial(s, material, viewport);
    orig::StateBlob_c_Set(s + kBlob);
    orig::RViewPort_t_RealizeRenderStates(viewport);
    orig::render_t_RenderTriangleStrip_409(render, 0x142, s + kVerts, 4, 0);
    orig::StateBlob_c_Reset(s + kBlob);
    orig::RViewPort_t_ResetMaterial(viewport);
    VirtualViewport(s, 18, viewport);
    catrender::ObserversAfter(s, viewport);
}

}  // namespace

// ?EnableAdditiveRendering@RSprite@@QAEX_NW4SpriteRenderMode_e@1@@Z: additive blending (mode 2: dest colour times the
// sprite, else its colour added), no Z writes, black fog - in its RDeltaState; off: those states taken out again
// (the delta state gone if nothing is left). Additive ones draw later (priority 6, else 3).
void __fastcall EnableAdditiveRendering(uint8_t* s, void*, bool on, int32_t mode)
{
    void*& delta = Field<void*>(s, kDelta);
    if (on) {
        if (!delta) delta = orig::RDeltaState_RDeltaState(vc10::Allocate(0x16C), "none");
        orig::RDeltaState_SetRenderState(delta, 0x13, mode == 2 ? 10u : 3u);
        orig::RDeltaState_SetRenderState(delta, 0x14, 2);
        orig::RDeltaState_SetRenderState(delta, 0x1B, 1);
        orig::RDeltaState_SetRenderState(delta, 0xE, 0);
        orig::RDeltaState_SetRenderState(delta, 0x22, 0);
    } else if (delta && s[kAdditive]) {
        auto remove = Internal<void(__fastcall*)(void*, void*, int32_t)>(0x2F3DF);   // RDeltaState: a state out
        for (int32_t state : {0x13, 0x14, 0x1B, 0xE, 0x22}) remove(delta, nullptr, state);
        if (orig::RDeltaState_IsEmpty(delta)) {
            reinterpret_cast<void*(__fastcall*)(void*, void*, uint32_t)>((*static_cast<void***>(delta))[0])(delta, nullptr, 1);
            delta = nullptr;
        }
    }
    s[kAdditive] = on;
    orig::RVisual_t_SetRenderPriority(s, on ? 6 : 3);
}

namespace {

bool __fastcall ObserverSkip(void* visual, void*, void* viewport) { return catrender::ObserversSkip(visual, viewport); }
bool __fastcall ObserverMaterial(void* visual, void*, void* material, void* viewport, void* d3dMaterial)
{
    return catrender::ObserversMaterial(visual, material, viewport, d3dMaterial);
}
void __fastcall ObserverAfterMaterial(void* visual, void*, void* material, void* viewport)
{
    catrender::ObserversAfterMaterial(visual, material, viewport);
}
void __fastcall ObserverAfter(void* visual, void*, void* viewport) { catrender::ObserversAfter(visual, viewport); }

}  // namespace

void Install(HMODULE orig)
{
    if (GetMode("Scene", Mode::Off) != Mode::On) return;
    g_orig = orig;
    const serialize::Api& a = serialize::Get();
    if (!a.complete || !a.findData || !a.addData || !a.findObject) {
        Log("sprites: not replaced (serialize.dll exports missing)");
        return;
    }
    struct Entry {
        uint32_t rva;
        void* target;
        const char* what;
    };
#define FN(f) reinterpret_cast<void*>(&f)
    const Entry entries[] = {
        {0x135F1, FN(Construct), "RSprite::RSprite"},
        {0x13128, FN(EnableAdditiveRendering), "RSprite::EnableAdditiveRendering"},
        {0x13482, FN(InitVertexColor), "RSprite::InitVertexColor"},
        {0x13575, FN(Setup), "RSprite setup (FUN_10013575)"},
        {0x132B3, FN(Corners), "RSprite corners (FUN_100132b3)"},
        {0x134A1, FN(Uvs), "RSprite u v (FUN_100134a1)"},
        {0x136F9, FN(Copy), "RSprite copy (FUN_100136f9)"},
        {0x13DF4, FN(Duplicate), "RSprite duplicate (FUN_10013df4)"},
        {0x138CC, FN(FromArchive), "RSprite(archive) (FUN_100138cc)"},
        {0x13A7A, FN(Instantiate), "RSprite::Instantiate"},
        {0x13018, FN(Archive), "RSprite archive (FUN_10013018)"},
        {0x12FAA, FN(Destroy), "RSprite destroyed (FUN_10012faa)"},
        {0x13DD5, FN(DeletingDestroy), "RSprite deleting destructor (FUN_10013dd5)"},
        {0x13DAB, FN(DeletingDestroy2), "RSprite deleting destructor, second vtable (FUN_10013dab)"},
        {0x13515, FN(Animate), "RSprite animated (FUN_10013515)"},
        {0x13AB2, FN(Render), "RSprite drawn (FUN_10013ab2)"},
        {0x12EB0, FN(AnimRect), "RSpriteAnim frame (FUN_10012eb0)"},
        {0x12E56, FN(AnimCopy), "RSpriteAnim copy (FUN_10012e56)"},
        {0x12F53, FN(AnimFromArchive), "RSpriteAnim(archive) (FUN_10012f53)"},
        {0x13540, FN(AnimInstantiate), "RSpriteAnim::Instantiate"},
        {0x12EA4, FN(AnimDestroy), "RSpriteAnim destroyed (FUN_10012ea4)"},
        {0x13DB6, FN(AnimDeletingDestroy), "RSpriteAnim deleting destructor (FUN_10013db6)"},
        {0x12F91, FN(AnimArchive), "RSpriteAnim archive (FUN_10012f91)"},
        {0x13E2E, FN(ObserverSkip), "RVisual_t observers: skip? (FUN_10013e2e)"},
        {0x13E71, FN(ObserverMaterial), "RVisual_t observers: material (FUN_10013e71)"},
        {0x13EBB, FN(ObserverAfterMaterial), "RVisual_t observers: after the material (FUN_10013ebb)"},
        {0x13F00, FN(ObserverAfter), "RVisual_t observers: after drawing (FUN_10013f00)"},
    };
#undef FN
    int installed = 0;
    for (const Entry& e : entries) installed += Replace(orig, e.rva, e.target, e.what) ? 1 : 0;
    Log("sprites: %d of %d functions native", installed, int(sizeof(entries) / sizeof(entries[0])));
}

}  // namespace rnative::sprite
