// RVisual_t natively, first part (randy-vk.ini [Native] Scene=on): what every drawable thing of the scene does
// besides drawing itself - getting into the render lists (by distance), its per-frame Process (fading in / out, data
// restored after a lost device), choosing the lights that light it, its render priority and state changes, being
// built and torn down. Rasterize / RenderWithTransparency (drawing static meshes) come with SimpleMesh.
//
// RVisual_t (0x178 bytes, an RRefFrame_t; DisplaySystem's ~120 visual classes derive from it): +0x0C its fade (1 =
// fully there), +0x10 the fade it moves to, +0xA4 SubjectImpl<VisualEvents::Rendering> (vtable, then the observer
// std::set), +0xB0 material hook, +0xB8 RDeltaState*, +0xBC 0, +0xC0 the lights lighting it (8), +0xE0 lit, +0xE1
// environment mapping allowed, +0xE4 its render list (-1: not drawn), +0xE8 next in its bucket, +0xF0 the restore
// count its data is from, +0xF4 StateBlob_c.
#include "native/visual.h"
#include "native/orig_api.gen.h"
#include "native/serialize.h"
#include "native/vc10.h"
#include "native/xmath.h"

#include <cmath>

namespace rnative::visual {

namespace {

HMODULE g_orig;
void* const* g_randy;

template <typename T>
T& Field(void* object, uint32_t offset) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(object) + offset); }
template <typename T>
T& Global(uint32_t rva) { return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(g_orig) + rva); }
template <typename F>
F Internal(uint32_t rva) { return reinterpret_cast<F>(reinterpret_cast<uint8_t*>(g_orig) + rva); }

template <typename R = void, typename... A>
R Virtual(void* object, uint32_t slot, A... args)
{
    return reinterpret_cast<R(__fastcall*)(void*, void*, A...)>((*static_cast<void***>(object))[slot])(object, nullptr,
                                                                                                      args...);
}

constexpr uint32_t kVtable = 0x959B4, kSubjectVtable = 0x959A0, kSubjectBaseVtable = 0x95990;
constexpr uint32_t kFade = 0x0C, kFadeTarget = 0x10, kGroupMask = 0xA0, kVisible = 0x94, kSubject = 0xA4,
                   kDelta = 0xB8, kLights = 0xC0, kLit = 0xE0, kEnvAllowed = 0xE1, kList = 0xE4, kNext = 0xE8,
                   kRestored = 0xF0, kBlob = 0xF4;
constexpr uint32_t kRenderLists = 0x1CEDE8, kBucketsPerList = 0x708, kLightsBegin = 0x17D290, kLightsEnd = 0x17D294,
                   kMaxLights = 0xB7A64, kSun = 0x1CEDE0, kRestoreCount = 0x17D334, kCurrentCamera = 0x17D338;

using Visual = uint8_t;

const float* World(void* frame) { return static_cast<const float*>(orig::RRefFrame_t_GetWorldMatrix(frame)); }

void __cdecl SetMaxActiveLightCount(uint32_t count) { Global<uint32_t>(kMaxLights) = count > 7 ? 8 : count; }

// Into render list `list`, bucket `bucket` - or (bucket out of range) by distance from the camera: 5 buckets a metre
// up to 200 m, one a metre after that.
void __fastcall AddToRenderList(Visual* v, void*, int32_t list, int32_t bucket)
{
    void* camera = Global<void*>(kCurrentCamera);
    if (!camera) return;
    if (uint32_t(bucket) > 0x707) {
        const float* c = World(camera);
        const float* m = World(v);
        float d[3] = {m[12] - c[12], m[13] - c[13], m[14] - c[14]};
        float distance = xm::LengthSquared(d);
        if (0.0f < distance) distance = std::sqrt(distance);
        if (200.0f <= distance) {
            const float whole = float(std::lrint(double(distance) - 0.49999));
            bucket = int32_t(whole) + 800;
        } else {
            const float scaled = float(double(distance) * 5.0);
            bucket = int32_t(std::lrint(double(scaled) - 0.49999));
        }
        if (bucket < 0) bucket = 0;
        else if (bucket > 0x707) bucket = 0x707;
    }
    if (list < 0) return;
    Visual*& head = (&Global<Visual*>(kRenderLists))[list * int32_t(kBucketsPerList) + bucket];
    if (head == v) {
        OutputDebugStringA("error, trying to add an object to the renderlist twice! this is pretty bad.\n");
        return;
    }
    Field<Visual*>(v, kNext) = head;
    head = v;
}

// Each child that is a visual draws its shadow too (vtable slot 20).
void __fastcall RenderShadow(Visual* v, void*, void* viewport, void* shadow)
{
    using DynamicCastFn = void*(__cdecl*)(void*, long, void*, void*, int);
    static const auto dynamicCast =
        reinterpret_cast<DynamicCastFn>(GetProcAddress(GetModuleHandleA("msvcr100.dll"), "__RTDynamicCast"));
    for (void* child = Field<void*>(v, 0x1C); child; child = Field<void*>(child, 0x18)) {
        void* visual = dynamicCast(child, 0, &Global<uint8_t>(0xB60B8), &Global<uint8_t>(0xB60A0), 0);   // to RVisual_t
        if (visual) Virtual(visual, 20, viewport, shadow);
    }
}

void __fastcall SetRenderPriority(Visual* v, void*, int32_t list) { Field<int32_t>(v, kList) = list; }

void __fastcall SetDeltaState(Visual* v, void*, void* delta)
{
    if (delta) orig::RResource_t_AddRefRResource(delta);
    if (void* old = Field<void*>(v, kDelta)) orig::RResource_t_ReleaseRResource(old);
    Field<void*>(v, kDelta) = delta;
}

// Its delta state's save / apply (priority 8) / restore on the viewport's DeviceState.
void __fastcall StoreStateChanges(Visual* v, void*, void* viewport)
{
    if (void* d = Field<void*>(v, kDelta)) Virtual(d, 3, Field<void*>(viewport, 8));
}

void __fastcall ApplyStateChanges(Visual* v, void*, void* viewport)
{
    if (void* d = Field<void*>(v, kDelta)) Virtual<uint32_t>(d, 4, Field<void*>(viewport, 8), int32_t(8));
}

void __fastcall RestoreStateChanges(Visual* v, void*, void* viewport)
{
    if (void* d = Field<void*>(v, kDelta)) Virtual(d, 5, Field<void*>(viewport, 8));
}

void __fastcall RestoreData(Visual* v) { Field<uint32_t>(v, kRestored) = Global<uint32_t>(kRestoreCount); }

void __fastcall Process(Visual* v)
{
    orig::RRefFrame_t_Process(v);
    if (Field<uint32_t>(v, kRestored) != Global<uint32_t>(kRestoreCount) && !orig::Randy_t_IsDeviceLost(*g_randy))
        Virtual(v, 19);                              // RestoreData
    float& fade = Field<float>(v, kFade);
    const float target = Field<float>(v, kFadeTarget);
    const float step = 0.05f;
    if (target <= fade) {
        if (target < fade) {
            fade -= step;
            if (fade < 0.0f) fade = 0.0f;
        }
    } else {
        fade += step;
        if (fade > 1.0f) fade = 1.0f;
    }
    int32_t list = Field<int32_t>(v, kList);
    if (list != -1 && v[kVisible]) {
        if (fade < 1.0f) list = 6;                  // fading: drawn with the transparent things
        AddToRenderList(v, nullptr, list, -1);
    }
}

// FUN_1004c849 / FUN_1004a8c0: how much a light matters - a directional light's brightness (+1), else 0.
float LightWeight(void* light)
{
    if (Field<int32_t>(light, 0x10C) != 1) return 0.0f;
    const float* diffuse = &Field<float>(light, 0xA8);
    const float luminance = float(double(diffuse[1]) * 0.5870000123977661 + double(diffuse[0]) * 0.29899999499320984 +
                                  double(diffuse[2]) * 0.11400000005960464);
    return float(double(luminance) + 1.0);
}

// FUN_1004caf7: no room left - `light` takes the place of a weaker one, or stays off.
void ReplaceLight(void** lights, uint32_t count, void* light)
{
    float weakest = LightWeight(light);
    uint32_t at = count;
    for (uint32_t i = 0; 0.0f < weakest && i != count; ++i) {
        const float w = LightWeight(lights[i]);
        if (w < weakest) {
            at = i;
            weakest = w;
        }
    }
    if (at != count) {
        orig::RLight_t_Enable(lights[at], false);
        lights[at] = light;
        return;
    }
    if (Field<uint8_t>(light, 0x118)) orig::RLight_t_Enable(light, false);
}

// The lights (of those `mask` allows, sharing a group) reaching a sphere of `radius` around it: up to the maximum,
// all of them when there are 8 or fewer in the scene; switched on, the others off. Returns how many.
uint32_t __fastcall CullLights(Visual* v, void*, void*, float radius, uint32_t mask)
{
    if (!v[kLit]) return 0;
    void** begin = Global<void**>(kLightsBegin);
    void** end = Global<void**>(kLightsEnd);
    const uint32_t all = uint32_t(end - begin);
    const float* m = World(v);
    void** lights = &Field<void*>(v, kLights);
    uint32_t count = 0;
    const uint32_t max = Global<uint32_t>(kMaxLights);
    for (void** it = begin; it != end; ++it) {
        void* light = *it;
        if (all < 9) {
            lights[count++] = light;
            continue;
        }
        if ((Field<uint32_t>(v, kGroupMask) & Field<uint32_t>(light, kGroupMask)) &&
            (mask & Field<uint32_t>(light, 0x10C))) {
            if (Field<uint32_t>(light, 0x10C) == 1) {        // directional
                if (count < max) lights[count++] = light;
                else ReplaceLight(lights, count, light);
                continue;
            }
            const float range = Field<float>(light, 0xF0);
            if (0.0f < range) {
                const float reach = radius + range;
                const float* l = World(light);
                const float dx = l[12] - m[12];
                if (dx <= reach && -reach <= dx) {
                    const float dz = l[14] - m[14];
                    const float dy = l[13] - m[13];
                    if (dz <= reach && -reach <= dz && dz * dz + dx * dx + dy * dy <= reach * reach) {
                        if (max <= count) ReplaceLight(lights, count, light);
                        else lights[count++] = light;
                        continue;
                    }
                }
            }
        }
        if (Field<uint8_t>(light, 0x118)) orig::RLight_t_Enable(light, false);
    }
    for (uint32_t i = 0; i < count; ++i) orig::RLight_t_Enable(lights[i], true);
    return count;
}

// FUN_1004c895: the state blob every visual starts with (modulate texture by diffuse, linear filtering, blending).
void DefaultBlob(Visual* v)
{
    void* blob = v + kBlob;
    orig::StateBlob_c_SetTextureStageState(blob, 0, 1, 4);
    orig::StateBlob_c_SetTextureStageState(blob, 0, 2, 0);
    orig::StateBlob_c_SetTextureStageState(blob, 0, 3, 2);
    orig::StateBlob_c_SetTextureStageState(blob, 0, 0xB, 0x20000);
    orig::StateBlob_c_SetTextureStageState(blob, 0, 0x18, 3);
    orig::StateBlob_c_SetTextureStageState(blob, 0, 0xC, 1);
    orig::StateBlob_c_SetRenderState(blob, 0x0E, 0);
    orig::StateBlob_c_SetRenderState(blob, 0x17, 3);
    orig::StateBlob_c_SetRenderState(blob, 0x89, 1);
    orig::StateBlob_c_SetRenderState(blob, 0x13, 2);
    orig::StateBlob_c_SetRenderState(blob, 0x14, 2);
    orig::StateBlob_c_SetRenderState(blob, 0x1B, 1);
    orig::StateBlob_c_SetRenderState(blob, 0x22, 0);
    orig::StateBlob_c_SetRenderState(blob, 0x8B, 0);
}

void InitVisualPart(Visual* v)
{
    Internal<void*(__fastcall*)(void*)>(0x4E3B6)(v + kSubject);   // SubjectImpl: its vtable, the observer set
    Field<uintptr_t>(v, 0) = reinterpret_cast<uintptr_t>(g_orig) + kVtable;
    Field<uintptr_t>(v, kSubject) = reinterpret_cast<uintptr_t>(g_orig) + kSubjectVtable;
    orig::StateBlob_c_StateBlob_c(v + kBlob);
    Global<void*>(kSun) = nullptr;
}

void* __fastcall Construct(Visual* v, void*, void* parent, void* animation)
{
    orig::RRefFrame_t_RRefFrame_t_35(v, parent, animation);
    InitVisualPart(v);
    Field<void*>(v, kNext) = nullptr;
    v[kLit] = 1;
    v[kEnvAllowed] = 1;
    Field<void*>(v, kDelta) = nullptr;
    Field<int32_t>(v, kList) = 3;
    v[0xBC] = 0;
    Field<uint32_t>(v, kRestored) = Global<uint32_t>(kRestoreCount);
    DefaultBlob(v);
    return v;
}

void* __fastcall Copy(Visual* v, void*, Visual* from)
{
    orig::RRefFrame_t_RRefFrame_t_34(v, from);
    InitVisualPart(v);
    v[kLit] = from[kLit];
    v[kEnvAllowed] = from[kEnvAllowed];
    void* delta = Field<void*>(from, kDelta);
    Field<void*>(v, kDelta) =
        delta ? Internal<void*(__fastcall*)(void*, void*, void*)>(0x2F8A8)(vc10::Allocate(0x16C), nullptr, delta) : nullptr;
    Field<void*>(v, kNext) = nullptr;
    Field<int32_t>(v, kList) = Field<int32_t>(from, kList);
    v[0xBC] = 0;
    Field<uint32_t>(v, kRestored) = Field<uint32_t>(from, kRestored);
    DefaultBlob(v);
    return v;
}

void __fastcall Destroy(Visual* v)
{
    Field<uintptr_t>(v, 0) = reinterpret_cast<uintptr_t>(g_orig) + kVtable;
    Field<uintptr_t>(v, kSubject) = reinterpret_cast<uintptr_t>(g_orig) + kSubjectVtable;
    if (void* d = Field<void*>(v, kDelta)) orig::RResource_t_ReleaseRResource(d);
    Internal<void(__fastcall*)(void*)>(0x255B5)(v + kBlob);   // StateBlob_c::~StateBlob_c
    Field<uintptr_t>(v, kSubject) = reinterpret_cast<uintptr_t>(g_orig) + kSubjectBaseVtable;
    Internal<void(__fastcall*)(void*)>(0x5B824)(v + kSubject + 4);   // the observer set (std::set at +0xA8)
    Internal<void(__fastcall*)(void*)>(0x45471)(v);               // RRefFrame_t::~RRefFrame_t
}

void __fastcall Archive(Visual* v, void*, void* archive)
{
    orig::RRefFrame_t_Archive(v, archive);
    const serialize::Api& s = serialize::Get();
    void* stream = s.getStream(archive, nullptr);
    s.addInt32(stream, nullptr, "prio", Field<int32_t>(v, kList));
    s.addBool(stream, nullptr, "enable_light", v[kLit] != 0);
    s.addObject(stream, nullptr, "delta_state", Field<void*>(v, kDelta));
}

}  // namespace

void Install(HMODULE orig)
{
    if (GetMode("Scene", Mode::Off) != Mode::On) return;
    g_orig = orig;
    g_randy = reinterpret_cast<void* const*>(GetProcAddress(orig, "?s_pcRandy@Randy_t@@1PAV1@A"));
    if (!serialize::Get().complete || !serialize::Get().addBool) {
        Log("visuals: serialize.dll exports missing - not replaced");
        return;
    }
    struct Entry {
        uint32_t rva;
        void* target;
        const char* what;
    };
#define FN(f) reinterpret_cast<void*>(&f)
    const Entry entries[] = {
        {0x4D544, FN(Construct), "RVisual_t::RVisual_t"},
        {0x4D5E5, FN(Copy), "RVisual_t::RVisual_t(copy)"},
        {0x4D7D3, FN(Destroy), "RVisual_t::~RVisual_t"},
        {0x4C947, FN(Archive), "RVisual_t::Archive"},
        {0x4C9A8, FN(SetMaxActiveLightCount), "RVisual_t::SetMaxActiveLightCount"},
        {0x4C9C2, FN(AddToRenderList), "RVisual_t::AddToRenderList"},
        {0x4CABA, FN(RenderShadow), "RVisual_t::RenderShadow"},
        {0x4CB7E, FN(SetRenderPriority), "RVisual_t::SetRenderPriority"},
        {0x4CB8E, FN(SetDeltaState), "RVisual_t::SetDeltaState"},
        {0x4CBBE, FN(StoreStateChanges), "RVisual_t::StoreStateChanges"},
        {0x4CBE1, FN(ApplyStateChanges), "RVisual_t::ApplyStateChanges"},
        {0x4CC06, FN(RestoreStateChanges), "RVisual_t::RestoreStateChanges"},
        {0x4CC29, FN(RestoreData), "RVisual_t::RestoreData"},
        {0x4CC35, FN(Process), "RVisual_t::Process"},
        {0x4CE96, FN(CullLights), "RVisual_t::CullLights"},
    };
#undef FN
    int installed = 0;
    for (const Entry& e : entries) installed += Replace(orig, e.rva, e.target, e.what) ? 1 : 0;
    Log("visuals (RVisual_t): %d of %d functions native", installed, int(sizeof(entries) / sizeof(entries[0])));
}

}  // namespace rnative::visual
