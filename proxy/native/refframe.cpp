// RRefFrame_t natively, first part (randy-vk.ini [Native] Scene=on): frames of the scene graph - construction,
// children, the world matrix, change flags, animation time / loop, visibility, group mask, colour overrides,
// relative position / rotation, sphere culling. Pointing at targets / directions, connectors, archives and debug
// drawing are still the original's.
//
// RRefFrame_t (0xA4 bytes; DisplaySystem / Gamecode derive from it and read its fields): +0x00 vtable, +0x04
// Serializable_c's, +0x08 0, +0x0C / +0x10 1.0, +0x14 parent, +0x18 next sibling, +0x1C first child, +0x20 position,
// +0x2C rotation (quaternion), +0x3C scale, +0x40 {RAnimation_t*, matrix} or null, +0x44 world matrix, +0x84 world
// scale, +0x88 transparency, +0x8C / +0x90 emissive / specular override (RGB_t*, new'd), +0x94 visible, +0x98
// connector, +0x9C changes this frame, +0x9D last frame's, +0x9E world matrix out of date (bits), +0xA0 group mask.
#include "native/refframe.h"
#include "native/orig_api.gen.h"
#include "native/serialize.h"
#include "native/vc10.h"
#include "native/xmath.h"

#include <cmath>
#include <cstring>
#include <initializer_list>

namespace rnative::refframe {

namespace {

HMODULE g_orig;

template <typename T>
T& Field(void* object, uint32_t offset) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(object) + offset); }
template <typename T>
T& Global(uint32_t rva) { return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(g_orig) + rva); }
template <typename F>
F Internal(uint32_t rva) { return reinterpret_cast<F>(reinterpret_cast<uint8_t*>(g_orig) + rva); }

constexpr uint32_t kVtable = 0x955DC, kCurrentCamera = 0x17D338;
constexpr uint32_t kParent = 0x14, kNext = 0x18, kChild = 0x1C, kPosition = 0x20, kRotation = 0x2C, kScale = 0x3C,
                   kAnim = 0x40, kWorld = 0x44, kWorldScale = 0x84, kTransparency = 0x88, kEmissive = 0x8C,
                   kSpecular = 0x90, kVisible = 0x94, kConnector = 0x98, kChanged = 0x9C, kLastChanged = 0x9D,
                   kDirty = 0x9E, kGroupMask = 0xA0;

using Frame = uint8_t;

template <typename R = void, typename... A>
R Virtual(void* object, uint32_t slot, A... args)
{
    return reinterpret_cast<R(__fastcall*)(void*, void*, A...)>((*static_cast<void***>(object))[slot])(object, nullptr,
                                                                                                      args...);
}
// Its vtable's slots: 0 deleting destructor, 4 AddChild, 5 RemoveChild, 8 Process, 11 SetChangeFlags.
void SetChangeFlagsV(void* f, uint32_t flags, bool children) { Virtual(f, 11, flags, children); }

struct Anim {                                       // the +0x40 record
    void* animation;                                // RAnimation_t (an RResource_t)
    float matrix[16];
};

Anim* NewAnim(void* animation)                      // FUN_10044c49
{
    auto* a = static_cast<Anim*>(vc10::Allocate(sizeof(Anim)));
    a->animation = animation;
    xm::M4 id = xm::Identity();
    std::memcpy(a->matrix, id.m, 64);
    if (animation) orig::RResource_t_AddRefRResource(animation);
    return a;
}

Frame* Parent(Frame* f) { return Field<Frame*>(f, kParent); }
Frame* FirstChild(Frame* f) { return Field<Frame*>(f, kChild); }
Frame* Next(Frame* f) { return Field<Frame*>(f, kNext); }

void* __fastcall Construct(Frame* f, void*, Frame* parent, void* animation)
{
    serialize::Get().construct(f, nullptr);
    Field<float>(f, 0xC) = 1.0f;
    Field<float>(f, 0x10) = 1.0f;
    Field<uintptr_t>(f, 0) = reinterpret_cast<uintptr_t>(g_orig) + kVtable;
    f[8] = 0;
    std::memset(f + kParent, 0, kRotation + 12 - kParent);   // parent, sibling, child, position, rotation x y z
    Field<float>(f, kRotation + 12) = 1.0f;          // w
    Field<void*>(f, kAnim) = nullptr;
    Field<float>(f, kScale) = 1.0f;
    xm::M4 id = xm::Identity();
    std::memcpy(f + kWorld, id.m, 64);
    Field<int32_t>(f, kGroupMask) = -1;
    Field<float>(f, kTransparency) = 1.0f;
    Field<void*>(f, kEmissive) = nullptr;
    Field<void*>(f, kSpecular) = nullptr;
    f[kVisible] = 1;
    Field<void*>(f, kConnector) = nullptr;
    Field<uint16_t>(f, kChanged) = 0x0F0F;
    f[kDirty] = 0xF;
    if (animation) Field<Anim*>(f, kAnim) = NewAnim(animation);
    if (parent) Virtual(parent, 4, static_cast<void*>(f));   // AddChild
    return f;
}

void __fastcall Destroy(Frame* f)
{
    Field<uintptr_t>(f, 0) = reinterpret_cast<uintptr_t>(g_orig) + kVtable;
    if (Anim* a = Field<Anim*>(f, kAnim)) {
        if (a->animation) orig::RResource_t_ReleaseRResource(a->animation);
        vc10::Free(a);
    }
    if (void* c = Field<void*>(f, kConnector)) {
        Virtual(c, 5, static_cast<void*>(nullptr));
        orig::RResource_t_ReleaseRResource(c);
    }
    if (Frame* p = Parent(f)) Virtual(p, 5, static_cast<void*>(f));   // RemoveChild
    while (Frame* child = FirstChild(f)) Virtual<void*>(child, 0, uint32_t(1));   // deleted; leaves the list itself
    vc10::Free(Field<void*>(f, kEmissive));
    vc10::Free(Field<void*>(f, kSpecular));
    serialize::Get().destroy(f, nullptr);
}

void __fastcall AddChild(Frame* f, void*, Frame* child)
{
    if (Frame* old = Parent(child)) Virtual(old, 5, static_cast<void*>(child));
    Field<Frame*>(child, kParent) = f;
    Field<Frame*>(child, kNext) = FirstChild(f);
    Field<Frame*>(f, kChild) = child;
    SetChangeFlagsV(f, 8, false);
    SetChangeFlagsV(child, 8, false);
}

void __fastcall RemoveChild(Frame* f, void*, Frame* child)
{
    Frame** link = &Field<Frame*>(f, kChild);
    while (*link && *link != child) link = &Field<Frame*>(*link, kNext);
    Frame* found = *link;
    if (found) {
        *link = Next(found);
        Field<Frame*>(found, kParent) = nullptr;
        Field<Frame*>(found, kNext) = nullptr;
    }
    SetChangeFlagsV(f, 8, false);
    if (found) SetChangeFlagsV(found, 8, false);    // (the original calls through null when it isn't a child)
}

void __fastcall Process(Frame* f)
{
    f[kLastChanged] = f[kChanged];
    f[kChanged] = 0;
    for (Frame* c = FirstChild(f); c; c = Next(c)) Virtual(c, 8);
}

void __fastcall SetChangeFlags(Frame* f, void*, uint32_t flags, bool children)
{
    f[kDirty] |= uint8_t(flags);
    f[kChanged] |= uint8_t(flags);
    if (children)
        for (Frame* c = FirstChild(f); c; c = Next(c)) SetChangeFlagsV(c, flags, true);
}

// The world matrix: rotation, scale, position, the animation's matrix before, the parent's after.
void __fastcall UpdateWorldMatrix(Frame* f)
{
    if (f[kDirty] & 0xF) {
        float* m = &Field<float>(f, kWorld);
        const xm::M4 r = xm::FromQuaternion(&Field<float>(f, kRotation));
        for (int i : {0, 1, 2, 4, 5, 6, 8, 9, 10}) m[i] = r.m[i];   // (FUN_1006e393 writes only these)
        const float s = Field<float>(f, kScale);
        for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 3; ++col) m[row * 4 + col] *= s;
        m[12] = Field<float>(f, kPosition);
        m[13] = Field<float>(f, kPosition + 4);
        m[14] = Field<float>(f, kPosition + 8);
        m[3] = m[7] = m[11] = 0.0f;
        if (Anim* a = Field<Anim*>(f, kAnim)) {
            const xm::M4 product = xm::Mul(xm::Load(a->matrix), xm::Load(m));
            std::memcpy(m, product.m, 64);
        }
        Field<float>(f, kWorldScale) = s;
        if (Frame* p = Parent(f)) {
            if (p[kDirty]) UpdateWorldMatrix(p);
            const xm::M4 product = xm::Mul(xm::Load(m), xm::Load(p + kWorld));
            std::memcpy(m, product.m, 64);
            Field<float>(f, kWorldScale) = Field<float>(p, kWorldScale) * Field<float>(f, kWorldScale);
        }
    }
    f[kDirty] = 0;
}

const float* __fastcall GetWorldMatrix(Frame* f)
{
    if (f[kDirty]) UpdateWorldMatrix(f);
    return &Field<float>(f, kWorld);
}

void* AnimationOf(Frame* f) { return Field<Anim*>(f, kAnim) ? Field<Anim*>(f, kAnim)->animation : nullptr; }

// Its animation to `time` (wrapped to the animation's length; a whole number of loops is the end, not the start),
// and its children's if `children`.
void __fastcall SetAnimationTime(Frame* f, void*, float time, bool children)
{
    if (Anim* a = Field<Anim*>(f, kAnim)) {
        SetChangeFlagsV(f, 3, false);
        if (void* animation = a->animation) {
            const float total = Virtual<float>(animation, 5);
            float t = float(std::fmod(double(time), double(total)));
            if (t == 0.0f && time > 0.0f) t = total;
            Virtual(animation, 3, t, static_cast<void*>(a->matrix), static_cast<void*>(f + kVisible));
        }
    }
    if (children)
        for (Frame* c = FirstChild(f); c; c = Next(c)) SetAnimationTime(c, nullptr, time, true);
}

void __fastcall SetLoop(Frame* f, void*, bool loop)
{
    for (Frame* c = FirstChild(f); c; c = Next(c)) SetLoop(c, nullptr, loop);
    if (void* animation = AnimationOf(f)) Virtual(animation, 7, loop);
}

float __fastcall GetAnimationTotalTime(Frame* f)
{
    void* animation = AnimationOf(f);
    return animation ? Virtual<float>(animation, 5) : 0.0f;
}

void* __fastcall GetAnimation(Frame* f) { return Field<Anim*>(f, kAnim) ? Field<Anim*>(f, kAnim)->animation : nullptr; }

float __fastcall GetAnimationTreeTotalTime(Frame* f)
{
    float longest = GetAnimationTotalTime(f);
    for (Frame* c = FirstChild(f); c; c = Next(c)) {
        const float t = GetAnimationTreeTotalTime(c);
        if (longest < t) longest = t;
    }
    return longest;
}

bool Equal(const float* a, const float* b)          // FUN_1006e353
{
    for (int i = 0; i < 16; ++i)
        if (a[i] != b[i]) return false;
    return true;
}

const float* StaticIdentity(uint32_t flagRva, uint32_t matrixRva)   // a function-local static of the original
{
    uint32_t& flag = Global<uint32_t>(flagRva);
    float* m = &Global<float>(matrixRva);
    if (!(flag & 1)) {
        flag |= 1;
        const xm::M4 id = xm::Identity();
        std::memcpy(m, id.m, 64);
    }
    return m;
}

void __fastcall SetAnimMatrix(Frame* f, void*, const float* m)
{
    Anim*& a = Field<Anim*>(f, kAnim);
    if (!a) {
        if (Equal(m, StaticIdentity(0x17D3B0, 0x17D370))) return;
        a = NewAnim(nullptr);
    } else if (Equal(a->matrix, m)) {
        return;
    }
    std::memcpy(a->matrix, m, 64);
    SetChangeFlagsV(f, 3, true);
}

const float* __fastcall GetAnimMatrix(Frame* f)
{
    const float* identity = StaticIdentity(0x17D3F8, 0x17D3B8);
    return Field<Anim*>(f, kAnim) ? Field<Anim*>(f, kAnim)->matrix : identity;
}

void __fastcall SetVisible(Frame* f, void*, bool visible, bool children)
{
    if (children)
        for (Frame* c = FirstChild(f); c; c = Next(c)) Field<uint8_t>(c, kVisible) = visible;   // one level, as Randy
    f[kVisible] = visible;
}

void __fastcall SetGroupMask(Frame* f, void*, uint32_t mask, bool children)
{
    Field<uint32_t>(f, kGroupMask) = mask;
    if (children)
        for (Frame* c = FirstChild(f); c; c = Next(c)) SetGroupMask(c, nullptr, mask, true);
}

void SetColour(Frame* f, uint32_t field, const float* rgb)
{
    vc10::Free(Field<void*>(f, field));
    Field<void*>(f, field) = nullptr;
    if (rgb) {
        auto* copy = static_cast<float*>(vc10::Allocate(12));
        std::memcpy(copy, rgb, 12);
        Field<float*>(f, field) = copy;
    }
}

void __fastcall SetEmissive(Frame* f, void*, const float* rgb)
{
    for (Frame* c = FirstChild(f); c; c = Next(c)) SetEmissive(c, nullptr, rgb);
    SetColour(f, kEmissive, rgb);
}

void __fastcall SetSpecular(Frame* f, void*, const float* rgb)
{
    for (Frame* c = FirstChild(f); c; c = Next(c)) SetSpecular(c, nullptr, rgb);
    SetColour(f, kSpecular, rgb);
}

void __fastcall SetTransparency(Frame* f, void*, float transparency)
{
    for (Frame* c = FirstChild(f); c; c = Next(c)) SetTransparency(c, nullptr, transparency);
    Field<float>(f, kTransparency) = transparency;
}

void* CurrentCamera() { return Global<void*>(kCurrentCamera); }   // RRefFrame_t::m_spcCurrentCamera
bool CameraCulls(void* camera, const float* center, float radius)
{
    return Internal<bool(__fastcall*)(void*, void*, const float*, float)>(0x2B3E4)(camera, nullptr, center, radius);
}

bool __fastcall CullBySphere(Frame*, void*, const float* center, float radius)
{
    void* camera = CurrentCamera();
    return camera && CameraCulls(camera, center, radius);
}

// Also where the center lands (the camera's view-projection) when it isn't culled.
bool __fastcall CullBySphereAt(Frame*, void*, const float* center, float radius, float* out)
{
    void* camera = CurrentCamera();
    if (!camera) return false;
    if (CameraCulls(camera, center, radius)) return true;
    xm::Transform(center, &Field<float>(camera, 0x120), out);
    return false;
}

// The setters that flag changes (FUN_10016877, FUN_100168a6, FUN_100559d2).
void __fastcall SetPosition(Frame* f, void*, const float* p)
{
    float* own = &Field<float>(f, kPosition);
    if (own[0] == p[0] && own[1] == p[1] && own[2] == p[2]) return;
    std::memcpy(own, p, 12);
    SetChangeFlagsV(f, 1, true);
}

void __fastcall SetRotation(Frame* f, void*, const float* q)
{
    float* own = &Field<float>(f, kRotation);
    if (own[3] == q[3] && own[0] == q[0] && own[1] == q[1] && own[2] == q[2]) return;
    std::memcpy(own, q, 16);
    SetChangeFlagsV(f, 2, true);
}

void __fastcall SetScale(Frame* f, void*, float scale)
{
    if (scale == Field<float>(f, kScale)) return;
    Field<float>(f, kScale) = scale;
    SetChangeFlagsV(f, 4, true);
}

// A position given in world space - or in `relativeTo`'s space - made the parent's.
void __fastcall SetRelativePosition(Frame* f, void*, const float* position, Frame* relativeTo)
{
    float p[3] = {position[0], position[1], position[2]};
    if (Frame* parent = Parent(f)) {                 // FUN_10044ae1: into the parent's space (its rotation's inverse)
        const float* m = GetWorldMatrix(parent);
        p[0] -= m[12];
        p[1] -= m[13];
        const float z = p[2] - m[14];
        p[2] = z;
        const float x = p[0], y = p[1];
        p[0] = m[2] * z + x * m[0] + m[1] * y;
        p[1] = m[6] * z + m[4] * x + m[5] * y;
        p[2] = m[10] * z + m[8] * x + m[9] * y;
    }
    if (relativeTo) {                                // FUN_1002cde5
        float out[3];
        xm::Transform(p, GetWorldMatrix(relativeTo), out);
        std::memcpy(p, out, 12);
    }
    SetPosition(f, nullptr, p);
}

void __fastcall SetRelativeRotation(Frame* f, void*, const float* q, Frame* relativeTo)
{
    xm::M4 r = xm::FromQuaternion(q);
    if (Frame* parent = Parent(f)) {                 // the parent's matrix transposed (FUN_1004614a)
        const float* m = GetWorldMatrix(parent);
        xm::M4 t;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) t.m[i * 4 + j] = m[j * 4 + i];
        r = xm::Mul(r, t);
    }
    if (relativeTo) r = xm::Mul(r, xm::Load(GetWorldMatrix(relativeTo)));
    float out[4];
    xm::ToQuaternion(r, out);
    SetRotation(f, nullptr, out);
}

}  // namespace

void Install(HMODULE orig)
{
    if (GetMode("Scene", Mode::Off) != Mode::On) return;
    g_orig = orig;
    if (!serialize::Get().complete) {
        Log("frames: serialize.dll exports missing - not replaced");
        return;
    }
    struct Entry {
        uint32_t rva;
        void* target;
        const char* what;
    };
#define FN(f) reinterpret_cast<void*>(&f)
    const Entry entries[] = {
        {0x44CC8, FN(Construct), "RRefFrame_t::RRefFrame_t(parent, animation)"},
        {0x45471, FN(Destroy), "RRefFrame_t::~RRefFrame_t"},
        {0x44ED1, FN(AddChild), "RRefFrame_t::AddChild"},
        {0x44F10, FN(RemoveChild), "RRefFrame_t::RemoveChild"},
        {0x44F79, FN(Process), "RRefFrame_t::Process"},
        {0x44FA2, FN(UpdateWorldMatrix), "RRefFrame_t::UpdateWorldMatrix"},
        {0x45712, FN(GetWorldMatrix), "RRefFrame_t::GetWorldMatrix"},
        {0x45253, FN(SetChangeFlags), "RRefFrame_t::SetChangeFlags"},
        {0x4506B, FN(SetAnimationTime), "RRefFrame_t::SetAnimationTime"},
        {0x45113, FN(SetLoop), "RRefFrame_t::SetLoop"},
        {0x4541A, FN(GetAnimationTotalTime), "RRefFrame_t::GetAnimationTotalTime"},
        {0x45430, FN(GetAnimation), "RRefFrame_t::GetAnimation"},
        {0x45728, FN(GetAnimationTreeTotalTime), "RRefFrame_t::GetAnimationTreeTotalTime"},
        {0x457E7, FN(SetAnimMatrix), "RRefFrame_t::SetAnimMatrix"},
        {0x45894, FN(GetAnimMatrix), "RRefFrame_t::GetAnimMatrix"},
        {0x4543D, FN(SetVisible), "RRefFrame_t::SetVisible"},
        {0x452F8, FN(SetGroupMask), "RRefFrame_t::SetGroupMask"},
        {0x45329, FN(SetEmissive), "RRefFrame_t::SetEmissive"},
        {0x4538B, FN(SetSpecular), "RRefFrame_t::SetSpecular"},
        {0x453ED, FN(SetTransparency), "RRefFrame_t::SetTransparency"},
        {0x452CE, FN(CullBySphere), "RRefFrame_t::CullBySphere"},
        {0x45769, FN(CullBySphereAt), "RRefFrame_t::CullBySphere(at)"},
        {0x16877, FN(SetPosition), "RRefFrame_t position (FUN_10016877)"},
        {0x168A6, FN(SetRotation), "RRefFrame_t rotation (FUN_100168a6)"},
        {0x559D2, FN(SetScale), "RRefFrame_t scale (FUN_100559d2)"},
        {0x45C85, FN(SetRelativePosition), "RRefFrame_t::SetRelativePosition"},
        {0x45CD9, FN(SetRelativeRotation), "RRefFrame_t::SetRelativeRotation"},
    };
#undef FN
    int installed = 0;
    for (const Entry& e : entries) installed += Replace(orig, e.rva, e.target, e.what) ? 1 : 0;
    Log("frames: %d of %d functions native", installed, int(sizeof(entries) / sizeof(entries[0])));
}

}  // namespace rnative::refframe
