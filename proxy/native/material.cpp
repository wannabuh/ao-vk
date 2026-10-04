// Materials natively (part of [Native] Device=on): RDeltaState (a set of render state / texture stage state /
// texture changes, which RViewPort_t turns into a StateBlob_c), RMaterial_t and DefaultMaterial_t.
//
// RDeltaState (0x16C bytes, an RResource_t; DisplaySystem allocates them): +0x2C per stage {RTexture_t*, surface_t*}
// x8, +0x6C textures set, +0x70 std::list<{type, value, 0}> render states, +0x7C 8 x std::list texture stage states
// (one per stage), +0xDC stages with states, +0xE0 highest stage used, +0xE4 StateBlob_c, +0x168 changed since the
// blob was made. The lists stay VS2010 lists: RViewPort_t reads them.
// RMaterial_t (0xC0 bytes, an RResource_t): +0x2C diffuse, +0x38 specular, +0x44 ambient, +0x50 emissive (RGB_t),
// +0x5C environment texture, +0x60 power, +0x64 specular strength, +0x68 opacity, +0x6C 0, +0x70 RDeltaState*,
// +0x74 alpha means transparency, +0xBC named "op1_...", +0xBD transparent (drawn in the second pass).
#include "native/material.h"
#include "native/orig_api.gen.h"
#include "native/serialize.h"
#include "native/vc10.h"

#include <cstdio>
#include <cstring>

namespace rnative::material {

namespace {

HMODULE g_orig;
void* const* g_randy;                               // Randy_t::s_pcRandy

template <typename T>
T& Field(void* object, uint32_t offset) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(object) + offset); }
template <typename F>
F Internal(uint32_t rva) { return reinterpret_cast<F>(reinterpret_cast<uint8_t*>(g_orig) + rva); }

const serialize::Api& S() { return serialize::Get(); }
void SetVtable(void* o, uint32_t rva) { Field<uintptr_t>(o, 0) = reinterpret_cast<uintptr_t>(g_orig) + rva; }
void DestroyResource(void* o) { Internal<void(__fastcall*)(void*)>(0x46283)(o); }   // RResource_t::~RResource_t

// ---- RDeltaState ----

constexpr uint32_t kDeltaVtable = 0x94168;

struct State {
    uint32_t type, value, extra;
};
using StateList = vc10::List<State>;
using StateNode = vc10::ListNode<State>;

struct Delta {
    uint8_t resource[0x2C];
    struct Slot {
        void* texture;                              // RTexture_t
        void* surface;                              // surface_t
    } slots[8];
    uint32_t textureCount;                          // +0x6C
    StateList renderStates;                         // +0x70
    StateList stageStates[8];                       // +0x7C
    uint32_t stageCount;                            // +0xDC
    uint32_t highestStage;                          // +0xE0
    uint8_t blob[0x84];                             // +0xE4 StateBlob_c
    uint8_t changed;                                // +0x168
    uint8_t pad[3];
};
static_assert(sizeof(Delta) == 0x16C && offsetof(Delta, textureCount) == 0x6C && offsetof(Delta, renderStates) == 0x70 &&
                  offsetof(Delta, stageStates) == 0x7C && offsetof(Delta, stageCount) == 0xDC &&
                  offsetof(Delta, blob) == 0xE4 && offsetof(Delta, changed) == 0x168, "RDeltaState");

void Reset(Delta* d)                                // FUN_1002f380
{
    for (int i = 0; i < 8; ++i) {
        d->slots[i] = {nullptr, nullptr};
        d->stageStates[i].clear();
    }
    d->textureCount = 0;
    d->stageCount = 0;
    d->highestStage = 0;
    d->renderStates.clear();
    orig::StateBlob_c_Clear(d->blob);
    d->changed = 1;
}

void InitDelta(Delta* d)                            // the members, after the RResource_t part
{
    SetVtable(d, kDeltaVtable);
    d->renderStates.init();
    for (StateList& l : d->stageStates) l.init();
    orig::StateBlob_c_StateBlob_c(d->blob);
    Reset(d);
}

Delta* __fastcall DeltaConstruct(Delta* d, void*, const char* name)
{
    orig::RResource_t_RResource_t_38(d, name);
    InitDelta(d);
    return d;
}

Delta* NewDelta()
{
    return DeltaConstruct(static_cast<Delta*>(vc10::Allocate(0x16C)), nullptr, "noname");
}

// FUN_1002f8a8: a copy (its own blob, made again).
Delta* __fastcall DeltaCopy(Delta* d, void*, const Delta* from)
{
    Internal<void*(__fastcall*)(void*, void*, const void*)>(0x462AD)(d, nullptr, from);   // RResource_t's copy
    InitDelta(d);
    for (int i = 0; i < 8; ++i) {
        d->slots[i].texture = from->slots[i].texture;
        if (d->slots[i].texture) orig::RTexture_t_AddRefRTexture(d->slots[i].texture);
        d->stageStates[i].assign(from->stageStates[i]);
    }
    d->textureCount = from->textureCount;
    d->stageCount = from->stageCount;
    d->highestStage = from->highestStage;
    d->renderStates.assign(from->renderStates);
    return d;
}

// From an archive: "rst_*" render states, "tstv_count" stages of "tst_*" states, "tch_*" textures.
Delta* __fastcall DeltaConstructFrom(Delta* d, void*, void* archive)
{
    orig::RResource_t_RResource_t_37(d, archive);
    InitDelta(d);
    void* stream = S().getStream(archive, nullptr);
    int32_t count = 0, v = 0;
    if (S().findInt32(stream, nullptr, "rst_count", &v, 0) == 0) count = v;
    State s{0, 0, 0};
    for (int32_t i = 0; i < count; ++i) {
        if (S().findInt32(stream, nullptr, "rst_type", &v, i) == 0) s.type = uint32_t(v);
        if (S().findInt32(stream, nullptr, "rst_value", &v, i) == 0) s.value = uint32_t(v);
        d->renderStates.push_back(s);
    }
    int32_t stages = 0;
    if (S().findInt32(stream, nullptr, "tstv_count", &v, 0) == 0) stages = v;
    int32_t index = 0;                              // the "tst_*" entries are numbered across all stages
    for (int32_t stage = 0; stage < stages; ++stage) {
        int32_t n = 0;
        if (S().findInt32(stream, nullptr, "tstm_count", &n, stage) != 0 || n <= 0) continue;
        for (int32_t k = 0; k < n; ++k, ++index) {
            if (S().findInt32(stream, nullptr, "tst_type", &v, index) == 0) s.type = uint32_t(v);
            if (S().findInt32(stream, nullptr, "tst_value", &v, index) == 0) s.value = uint32_t(v);
            d->stageStates[stage].push_back(s);
            if (int32_t(d->highestStage) < stage) d->highestStage = uint32_t(stage);
        }
    }
    d->stageCount = uint32_t(stages);
    int32_t textures = 0;
    if (S().findInt32(stream, nullptr, "tch_count", &v, 0) == 0) textures = v;
    for (int32_t i = 0; i < textures; ++i) {
        int32_t stage = 0;
        if (S().findInt32(stream, nullptr, "tch_type", &v, i) == 0) stage = v;
        void* texture = nullptr;
        Internal<void(__fastcall*)(void*, void*, const char*, void**, intptr_t)>(0x2F989)(stream, nullptr, "tch_text",
                                                                                         &texture, i);
        if (d->slots[stage].texture) {
            orig::RTexture_t_ReleaseRTexture(d->slots[stage].texture);
            --d->textureCount;
        }
        d->slots[stage].texture = texture;
        if (texture) {
            ++d->textureCount;
            if (int32_t(d->highestStage) < stage) d->highestStage = uint32_t(stage);
        }
    }
    return d;
}

void* __cdecl DeltaInstantiate(void* archive)
{
    return DeltaConstructFrom(static_cast<Delta*>(vc10::Allocate(0x16C)), nullptr, archive);
}

void __fastcall DeltaDestroy(Delta* d)              // FUN_1002f4c6
{
    SetVtable(d, kDeltaVtable);
    for (Delta::Slot& s : d->slots) {
        if (s.texture) orig::RTexture_t_ReleaseRTexture(s.texture);
        if (s.surface) orig::surface_t_ReleaseDXSurface(s.surface);
    }
    Internal<void(__fastcall*)(void*)>(0x255B5)(d->blob);   // StateBlob_c::~StateBlob_c
    for (StateList& l : d->stageStates) l.destroy();
    d->renderStates.destroy();
    DestroyResource(d);
}

void __fastcall DeltaSetRenderState(Delta* d, void*, uint32_t type, uint32_t value)
{
    d->changed = 1;
    for (StateNode* n = d->renderStates.begin(); n != d->renderStates.end(); n = n->next)
        if (n->value.type == type) {
            n->value.value = value;
            return;
        }
    d->renderStates.push_back(State{type, value, 0});
}

void __fastcall DeltaSetStageState(Delta* d, void*, uint32_t stage, uint32_t type, uint32_t value)
{
    d->changed = 1;
    if (d->highestStage < stage) d->highestStage = stage;
    StateList& list = d->stageStates[stage];
    for (StateNode* n = list.begin(); n != list.end(); n = n->next)
        if (n->value.type == type) {
            n->value.value = value;
            return;
        }
    list.push_back(State{type, value, 0});
    if (d->stageCount <= stage) d->stageCount = stage + 1;
}

void __fastcall DeltaSetTexture(Delta* d, void*, uint32_t stage, void* texture)
{
    void*& slot = d->slots[stage].texture;
    if (slot == texture) return;
    if (d->highestStage < stage) d->highestStage = stage;
    if (texture) {
        orig::RTexture_t_AddRefRTexture(texture);
        ++d->textureCount;
    }
    if (slot) {
        orig::RTexture_t_ReleaseRTexture(slot);
        --d->textureCount;
    }
    slot = texture;
}

void* __fastcall DeltaGetTexture(Delta* d, void*, uint32_t stage) { return d->slots[stage].texture; }   // FUN_1002f0a0

bool __fastcall DeltaGetRenderState(Delta* d, void*, uint32_t type, uint32_t* out)   // FUN_1002f356
{
    for (StateNode* n = d->renderStates.begin(); n != d->renderStates.end(); n = n->next)
        if (n->value.type == type) {
            *out = n->value.value;
            return true;
        }
    return false;
}

void __fastcall DeltaRemoveStageState(Delta* d, void*, uint32_t stage, uint32_t type)   // FUN_1002f412
{
    d->changed = 1;
    StateList& list = d->stageStates[stage];
    for (StateNode* n = list.begin(); n != list.end(); n = n->next)
        if (n->value.type == type) {
            list.erase(n);
            return;
        }
}

void __fastcall DeltaRemoveRenderState(Delta* d, void*, uint32_t type)   // FUN_1002f3df
{
    d->changed = 1;
    for (StateNode* n = d->renderStates.begin(); n != d->renderStates.end(); n = n->next)
        if (n->value.type == type) {
            d->renderStates.erase(n);
            return;
        }
}

void __fastcall DeltaClearTexture(Delta* d, void*, uint32_t stage)   // FUN_1002f0f7
{
    if (void* t = d->slots[stage].texture) {
        orig::RTexture_t_ReleaseRTexture(t);
        --d->textureCount;
        d->slots[stage].texture = nullptr;
    }
}

uint32_t __fastcall DeltaCount(const Delta* d)      // FUN_1002f11c
{
    uint32_t n = d->renderStates.size + d->textureCount;
    for (const StateList& l : d->stageStates) n += l.size;
    return n;
}

bool __fastcall DeltaIsEmpty(const Delta* d) { return DeltaCount(d) == 0; }

void __fastcall DeltaArchive(Delta* d, void*, void* archive)
{
    orig::RResource_t_Archive(d, archive);
    void* stream = S().getStream(archive, nullptr);
    S().addInt32(stream, nullptr, "version", 1);
    S().addInt32(stream, nullptr, "rst_count", int32_t(d->renderStates.size));
    for (StateNode* n = d->renderStates.begin(); n != d->renderStates.end(); n = n->next) {
        S().addInt32(stream, nullptr, "rst_type", int32_t(n->value.type));
        S().addInt32(stream, nullptr, "rst_value", int32_t(n->value.value));
    }
    S().addInt32(stream, nullptr, "tstv_count", int32_t(d->stageCount));
    for (uint32_t stage = 0; stage < d->stageCount; ++stage) {
        const StateList& l = d->stageStates[stage];
        S().addInt32(stream, nullptr, "tstm_count", int32_t(l.size));
        for (StateNode* n = l.begin(); n != l.end(); n = n->next) {
            S().addInt32(stream, nullptr, "tst_type", int32_t(n->value.type));
            S().addInt32(stream, nullptr, "tst_value", int32_t(n->value.value));
        }
    }
    S().addInt32(stream, nullptr, "tch_count", int32_t(d->textureCount));
    uint32_t written = 0;
    for (int32_t i = 0; i < 8; ++i) {
        if (!d->slots[i].texture) continue;
        S().addInt32(stream, nullptr, "tch_type", i);
        S().addObject(stream, nullptr, "tch_text", d->slots[i].texture);
        if (++written == d->textureCount) return;
    }
}

// Vtable slot 3 (FUN_1002efe4): before applying, the DeviceState's textures for the stages it sets, kept.
void __fastcall DeltaSave(Delta* d, void*, uint8_t* ds)
{
    for (int32_t stage = 0; stage <= int32_t(d->highestStage); ++stage) {
        Delta::Slot& s = d->slots[stage];
        if (!s.texture) continue;
        if (s.surface) {
            orig::surface_t_ReleaseDXSurface(s.surface);
            s.surface = nullptr;
        }
        s.surface = Field<void*>(ds, 0x1128 + stage * 4);
        if (s.surface) orig::surface_t_AddRefDXSurface(s.surface);
    }
}

// Vtable slot 4 (FUN_1002f275): the blob made again if anything changed, set, and the textures (their surfaces).
uint32_t __fastcall DeltaApply(Delta* d, void*, void* ds, int32_t priority)
{
    if (d->changed) {
        d->changed = 0;
        orig::StateBlob_c_Clear(d->blob);
        for (StateNode* n = d->renderStates.begin(); n != d->renderStates.end(); n = n->next)
            orig::StateBlob_c_SetRenderState(d->blob, int32_t(n->value.type), n->value.value);
        for (int32_t stage = 0; stage <= int32_t(d->highestStage); ++stage)
            for (StateNode* n = d->stageStates[stage].begin(); n != d->stageStates[stage].end(); n = n->next)
                orig::StateBlob_c_SetTextureStageState(d->blob, uint32_t(stage), int32_t(n->value.type), n->value.value);
    }
    orig::StateBlob_c_Validate(d->blob);
    orig::StateBlob_c_Set(d->blob);
    for (int32_t stage = 0; stage <= int32_t(d->highestStage); ++stage) {
        void* texture = d->slots[stage].texture;
        if (!texture) continue;
        void* surface = Field<void*>(texture, 0x30);
        orig::DeviceState_SetTexture(ds, surface, uint32_t(stage), surface ? priority : 10);
    }
    return 0;
}

// Vtable slot 5 (FUN_1002f042): the blob reset, the kept textures back.
void __fastcall DeltaRestore(Delta* d, void*, void* ds)
{
    orig::StateBlob_c_Reset(d->blob);
    for (int32_t stage = 0; stage <= int32_t(d->highestStage); ++stage) {
        Delta::Slot& s = d->slots[stage];
        if (!s.texture) continue;
        orig::DeviceState_SetTexture(ds, s.surface, uint32_t(stage), 10);
        if (s.surface) {
            orig::surface_t_ReleaseDXSurface(s.surface);
            s.surface = nullptr;
        }
    }
}

// ---- RMaterial_t ----

constexpr uint32_t kMaterialVtable = 0x954B8, kDefaultVtable = 0x940B8;
constexpr uint32_t kDiffuse = 0x2C, kSpecular = 0x38, kAmbient = 0x44, kEmissive = 0x50, kEnv = 0x5C, kPower = 0x60,
                   kStrength = 0x64, kOpacity = 0x68, kZero = 0x6C, kDelta = 0x70, kAlphaTransparency = 0x74,
                   kOp1 = 0xBC, kTransparent = 0xBD;
constexpr uint32_t ALPHABLENDENABLE = 0x1B, CULLMODE = 0x16, SPECULARENABLE = 0x1D;

Delta*& DeltaOf(void* m) { return Field<Delta*>(m, kDelta); }
bool TextureHasAlpha(void* texture) { return (Field<uint32_t>(texture, 0xB4) & 1) != 0; }   // FUN_1004761a
uint16_t StageCount() { return orig::Randy_t_GetTextureStageCount(*g_randy); }

void* __fastcall GetTexture(void* m, void*, uint8_t stage)
{
    return DeltaOf(m) ? DeltaOf(m)->slots[stage].texture : nullptr;
}

// FUN_10040b4d: a render state of the material's own (or not, `on` false), the delta made / dropped as needed.
void SetOwnState(void* m, uint32_t type, uint32_t value, bool on)
{
    if (!on) {
        if (Delta* d = DeltaOf(m)) {
            DeltaRemoveRenderState(d, nullptr, type);
            if (DeltaIsEmpty(d)) {
                orig::RResource_t_ReleaseRResource(d);
                DeltaOf(m) = nullptr;
            }
        }
    } else {
        if (!DeltaOf(m)) DeltaOf(m) = NewDelta();
        DeltaSetRenderState(DeltaOf(m), nullptr, type, value);
    }
    if (type == ALPHABLENDENABLE) Field<uint8_t>(m, kTransparent) = value == 1;
}

bool __fastcall IsTwoSided(void* m)
{
    uint32_t v = 0;
    return DeltaOf(m) && DeltaGetRenderState(DeltaOf(m), nullptr, CULLMODE, &v) && v == 1;
}

void __fastcall SetTwoSided(void* m, void*, bool on) { SetOwnState(m, CULLMODE, 1, on); }   // D3DCULL_NONE

void __fastcall UpdateSpecular(void* m)
{
    const float* s = &Field<float>(m, kSpecular);
    const bool coloured = s[0] != 0.0f || s[1] != 0.0f || s[2] != 0.0f;   // RGB_t::black
    const float strength = Field<float>(m, kStrength), power = Field<float>(m, kPower);
    SetOwnState(m, SPECULARENABLE, 1, coloured && strength > 0.0f && power >= 0.0f);
}

// Transparent below full opacity (or with an alpha texture) - unless the delta says otherwise.
void __fastcall SetOpacity(void* m, void*, float opacity)
{
    Field<float>(m, kOpacity) = opacity;
    Field<uint8_t>(m, kZero) = 0;
    bool transparent;
    if (opacity < 0.99609375f) {
        transparent = true;
    } else {
        void* t = GetTexture(m, nullptr, 0);
        transparent = t && TextureHasAlpha(GetTexture(m, nullptr, 0));
    }
    SetOwnState(m, ALPHABLENDENABLE, 1, transparent);
    if (Delta* d = DeltaOf(m)) {
        uint32_t v = 0;
        DeltaGetRenderState(d, nullptr, ALPHABLENDENABLE, &v);
        transparent = v == 1;
    }
    Field<uint8_t>(m, kTransparent) = transparent;
}

// FUN_10040645: how a material's first texture is used - "[n]" at the start of its name, else by its alpha.
void TextureStates(void* m, int32_t mode)
{
    Delta* d = DeltaOf(m);
    void* t0 = d->slots[0].texture;
    if (!t0) return;
    const char* name = orig::RResource_t_GetName(m);
    if (mode < 0) {
        if (name && name[0] == '[') {
            const int len = int(std::strlen(name));
            for (int i = 1; i < len; ++i)
                if (name[i] < '0' || name[i] > '9') {
                    if (name[i] == ']' && i > 1) std::sscanf(name, "[%d]", &mode);
                    break;
                }
        }
        if (mode < 0) {
            if (!TextureHasAlpha(t0)) return;
            if (!Field<uint8_t>(m, kAlphaTransparency)) {
                if (StageCount() < 2) return;
                mode = 5;
            } else {
                mode = 0;
            }
        }
    }
    auto rs = [d](uint32_t type, uint32_t value) { DeltaSetRenderState(d, nullptr, type, value); };
    auto tss = [d](uint32_t stage, uint32_t type, uint32_t value) { DeltaSetStageState(d, nullptr, stage, type, value); };
    switch (mode) {
    case 0:                                         // alpha blended
        if (!TextureHasAlpha(t0)) return;
        Field<uint8_t>(m, kAlphaTransparency) = 1;
        Field<uint8_t>(m, kTransparent) = 1;
        rs(0x1B, 1);
        return;
    case 1:                                         // alpha tested at 0x80
        Field<uint8_t>(m, kTransparent) = 0;
        rs(0x1B, 0);
        rs(0x0F, 1);
        rs(0x19, 5);
        rs(0x18, 0x80);
        return;
    case 2:                                         // blended and tested at 0x1E, no z writes
        Field<uint8_t>(m, kTransparent) = 1;
        rs(0x1B, 1);
        rs(0x0F, 1);
        rs(0x19, 5);
        rs(0x18, 0x1E);
        rs(0x0E, 0);
        return;
    case 3:                                         // blended, no z writes
        Field<uint8_t>(m, kTransparent) = 1;
        rs(0x1B, 1);
        rs(0x0E, 0);
        return;
    case 4:                                         // additive, no fog
        Field<uint8_t>(m, kTransparent) = 1;
        rs(0x1B, 1);
        rs(0x0E, 0);
        rs(0x22, 0);
        rs(0x0E, 0);
        rs(0x13, 2);
        rs(0x14, 2);
        tss(0, 2, 2);
        tss(0, 1, 2);
        return;
    case 5:                                         // alpha as a second texture stage's blend
        if (!TextureHasAlpha(t0)) return;
        Field<uint8_t>(m, kAlphaTransparency) = 0;
        Field<uint8_t>(m, kTransparent) = 0;
        rs(0x1B, 0);
        DeltaSetTexture(d, nullptr, 1, t0);
        tss(0, 2, 0x22);
        tss(0, 1, 7);
        tss(0, 3, 0);
        tss(1, 2, 2);
        tss(1, 1, 4);
        tss(1, 3, 1);
        tss(0, 4, 1);
        return;
    default:
        return;
    }
}

void DropSecondStage(Delta* d)                      // the two-stage alpha setup undone
{
    DeltaRemoveStageState(d, nullptr, 0, 2);
    DeltaRemoveStageState(d, nullptr, 0, 3);
    DeltaRemoveStageState(d, nullptr, 0, 1);
    DeltaRemoveStageState(d, nullptr, 1, 2);
    DeltaRemoveStageState(d, nullptr, 1, 3);
    DeltaRemoveStageState(d, nullptr, 1, 1);
    DeltaRemoveStageState(d, nullptr, 0, 4);
}

void __fastcall SetTexture(void* m, void*, void* texture, uint8_t stage, int32_t mode)
{
    if (texture == GetTexture(m, nullptr, stage)) return;
    Delta* d = DeltaOf(m);
    if (!texture) {
        if (!d) return;
        if (stage == 0 && d->slots[0].texture && TextureHasAlpha(d->slots[0].texture)) {
            if (!Field<uint8_t>(m, kAlphaTransparency) && StageCount() > 1) {
                DropSecondStage(d);
                if (d->slots[0].texture == d->slots[1].texture) DeltaClearTexture(d, nullptr, 1);
            } else if (Field<float>(m, kOpacity) == 1.0f) {
                DeltaRemoveRenderState(d, nullptr, ALPHABLENDENABLE);
                Field<uint8_t>(m, kTransparent) = 0;
            }
        }
        DeltaClearTexture(d, nullptr, stage);
        if (DeltaIsEmpty(d)) {
            orig::RResource_t_ReleaseRResource(d);
            DeltaOf(m) = nullptr;
        }
        return;
    }
    if (!d) {
        d = DeltaOf(m) = NewDelta();
    } else if (stage == 0 && d->slots[0].texture && TextureHasAlpha(d->slots[0].texture) && !TextureHasAlpha(texture)) {
        if (!Field<uint8_t>(m, kAlphaTransparency) && StageCount() > 1) {
            DropSecondStage(d);
            DeltaClearTexture(d, nullptr, 1);
        } else if (Field<float>(m, kOpacity) == 1.0f) {
            DeltaRemoveRenderState(d, nullptr, ALPHABLENDENABLE);
            Field<uint8_t>(m, kTransparent) = 0;
        }
    }
    DeltaSetTexture(d, nullptr, stage, texture);
    if (stage == 0) TextureStates(m, mode);
}

void __fastcall SetEnvTexture(void* m, void*, void* texture)
{
    if (texture) orig::RTexture_t_AddRefRTexture(texture);
    if (void* old = Field<void*>(m, kEnv)) orig::RTexture_t_ReleaseRTexture(old);
    Field<void*>(m, kEnv) = texture;
}

void __fastcall SetDeltaState(void* m, void*, Delta* d)
{
    if (d) orig::RResource_t_AddRefRResource(d);
    if (DeltaOf(m)) orig::RResource_t_ReleaseRResource(DeltaOf(m));
    DeltaOf(m) = d;
}

void __fastcall UseAlphaAsTransparency(void* m, void*, bool on) { Field<uint8_t>(m, kAlphaTransparency) = on; }
bool __fastcall IsTransparent(void* m) { return Field<uint8_t>(m, kTransparent) != 0; }

void __fastcall InitD3DMaterial(void* m, void*, float* out)   // D3DMATERIAL7
{
    const float* diffuse = &Field<float>(m, kDiffuse);
    const float* specular = &Field<float>(m, kSpecular);
    const float* ambient = &Field<float>(m, kAmbient);
    const float* emissive = &Field<float>(m, kEmissive);
    const float strength = Field<float>(m, kStrength);
    out[0] = diffuse[0], out[1] = diffuse[1], out[2] = diffuse[2], out[3] = Field<float>(m, kOpacity);
    out[8] = specular[0] * strength, out[9] = specular[1] * strength, out[10] = specular[2] * strength, out[11] = 1.0f;
    out[4] = ambient[0], out[5] = ambient[1], out[6] = ambient[2], out[7] = 1.0f;
    out[12] = emissive[0], out[13] = emissive[1], out[14] = emissive[2], out[15] = 1.0f;
    out[16] = Field<float>(m, kPower);
}

// FUN_10040aa0 / FUN_10040ac9: a material's own states on the DeviceState (priority 6) and off again - through the
// delta state's virtual slots, as the original calls them.
void __fastcall Apply(void* m, void*, void* ds)
{
    Delta* d = DeltaOf(m);
    if (!d) return;
    void** vt = *reinterpret_cast<void***>(d);
    reinterpret_cast<void(__fastcall*)(void*, void*, void*)>(vt[3])(d, nullptr, ds);
    reinterpret_cast<uint32_t(__fastcall*)(void*, void*, void*, int32_t)>(vt[4])(d, nullptr, ds, 6);
}

void __fastcall Unapply(void* m, void*, void* ds)
{
    Delta* d = DeltaOf(m);
    if (!d) return;
    reinterpret_cast<void(__fastcall*)(void*, void*, void*)>((*reinterpret_cast<void***>(d))[5])(d, nullptr, ds);
}

bool NamedOp1(void* m) { return _strnicmp(orig::RResource_t_GetName(m), "op1_", 4) == 0; }

void* __fastcall Construct(void* m, void*, const char* name, void* texture, const float* diffuse, const float* specular,
                           const float* ambient, const float* emissive, float power, float strength, float opacity,
                           bool twoSided, bool alphaTransparency)
{
    orig::RResource_t_RResource_t_38(m, name);
    SetVtable(m, kMaterialVtable);
    std::memcpy(&Field<float>(m, kDiffuse), diffuse, 12);
    std::memcpy(&Field<float>(m, kSpecular), specular, 12);
    std::memcpy(&Field<float>(m, kAmbient), ambient, 12);
    std::memcpy(&Field<float>(m, kEmissive), emissive, 12);
    Field<void*>(m, kEnv) = nullptr;
    Field<uint8_t>(m, kZero) = 0;
    Field<float>(m, kPower) = power;
    DeltaOf(m) = nullptr;
    Field<uint8_t>(m, kAlphaTransparency) = alphaTransparency;
    Field<float>(m, kStrength) = strength;
    Field<uint8_t>(m, kTransparent) = 0;
    Field<uint8_t>(m, kOp1) = NamedOp1(m);
    SetOpacity(m, nullptr, opacity);
    SetOwnState(m, CULLMODE, 1, twoSided);
    SetTexture(m, nullptr, texture, 0, -1);
    UpdateSpecular(m);
    return m;
}

void* __fastcall Copy(void* m, void*, void* from)
{
    Internal<void*(__fastcall*)(void*, void*, const void*)>(0x462AD)(m, nullptr, from);   // RResource_t's copy
    SetVtable(m, kMaterialVtable);
    std::memcpy(&Field<uint8_t>(m, kDiffuse), &Field<uint8_t>(from, kDiffuse), kPower + 4 - kDiffuse);   // colours, env, power
    DeltaOf(m) = nullptr;
    Field<uint8_t>(m, kZero) = 0;
    Field<float>(m, kStrength) = Field<float>(from, kStrength);
    Field<uint8_t>(m, kAlphaTransparency) = Field<uint8_t>(from, kAlphaTransparency);
    Field<uint8_t>(m, kOp1) = Field<uint8_t>(from, kOp1);
    Field<uint8_t>(m, kTransparent) = Field<uint8_t>(from, kTransparent);
    if (DeltaOf(from)) DeltaOf(m) = DeltaCopy(static_cast<Delta*>(vc10::Allocate(0x16C)), nullptr, DeltaOf(from));
    if (void* env = Field<void*>(m, kEnv)) orig::RTexture_t_AddRefRTexture(env);
    SetOpacity(m, nullptr, Field<float>(from, kOpacity));
    SetOwnState(m, CULLMODE, 1, IsTwoSided(from));
    UpdateSpecular(m);
    return m;
}

void* __fastcall ConstructFrom(void* m, void*, void* archive)   // FUN_1004132d
{
    orig::RResource_t_RResource_t_37(m, archive);
    SetVtable(m, kMaterialVtable);
    std::memset(&Field<uint8_t>(m, kDiffuse), 0, kEnv - kDiffuse);
    Field<uint8_t>(m, kAlphaTransparency) = 1;
    Field<uint8_t>(m, kTransparent) = 0;
    void* stream = S().getStream(archive, nullptr);
    Field<float>(m, kPower) = 0.0f;
    Field<float>(m, kStrength) = 0.0f;
    Field<float>(m, kOpacity) = 1.0f;
    DeltaOf(m) = nullptr;
    Field<uint8_t>(m, kZero) = 0;
    Field<void*>(m, kEnv) = nullptr;
    Internal<void(__fastcall*)(void*, void*, const char*, void**, void*)>(0x414DF)(stream, nullptr, "delta_state",
                                                                                 reinterpret_cast<void**>(&DeltaOf(m)), nullptr);
    Internal<void(__fastcall*)(void*, void*, const char*, void**, void*)>(0x2F989)(stream, nullptr, "env_texture",
                                                                                 &Field<void*>(m, kEnv), nullptr);
    S().findRgb(stream, nullptr, "diff", &Field<float>(m, kDiffuse), 0);
    S().findRgb(stream, nullptr, "spec", &Field<float>(m, kSpecular), 0);
    S().findRgb(stream, nullptr, "ambi", &Field<float>(m, kAmbient), 0);
    S().findRgb(stream, nullptr, "emis", &Field<float>(m, kEmissive), 0);
    S().findFloat(stream, nullptr, "shin", &Field<float>(m, kPower), 0);
    S().findFloat(stream, nullptr, "shin_str", &Field<float>(m, kStrength), 0);
    S().findFloat(stream, nullptr, "opac", &Field<float>(m, kOpacity), 0);
    Field<uint8_t>(m, kOp1) = NamedOp1(m);
    if (Delta* d = DeltaOf(m)) {
        uint32_t v = 0;
        if (DeltaGetRenderState(d, nullptr, ALPHABLENDENABLE, &v)) {
            Field<uint8_t>(m, kAlphaTransparency) = v != 0;
            Field<uint8_t>(m, kTransparent) = v != 0;
        } else {
            Field<uint8_t>(m, kAlphaTransparency) = 0;
        }
    }
    return m;
}

void* __cdecl Instantiate(void* archive) { return ConstructFrom(vc10::Allocate(0xC0), nullptr, archive); }

void __fastcall Destroy(void* m)                    // FUN_100408cb
{
    SetVtable(m, kMaterialVtable);
    if (Delta* d = DeltaOf(m)) orig::RResource_t_ReleaseRResource(d);
    if (void* env = Field<void*>(m, kEnv)) orig::RTexture_t_ReleaseRTexture(env);
    DestroyResource(m);
}

void __fastcall Archive(void* m, void*, void* archive)
{
    orig::RResource_t_Archive(m, archive);
    void* stream = S().getStream(archive, nullptr);
    S().addObject(stream, nullptr, "delta_state", DeltaOf(m));
    S().addObject(stream, nullptr, "env_texture", Field<void*>(m, kEnv));
    S().addRgb(stream, nullptr, "diff", &Field<float>(m, kDiffuse));
    S().addRgb(stream, nullptr, "spec", &Field<float>(m, kSpecular));
    S().addRgb(stream, nullptr, "ambi", &Field<float>(m, kAmbient));
    S().addRgb(stream, nullptr, "emis", &Field<float>(m, kEmissive));
    S().addFloat(stream, nullptr, "shin", Field<float>(m, kPower));
    S().addFloat(stream, nullptr, "shin_str", Field<float>(m, kStrength));
    S().addFloat(stream, nullptr, "opac", Field<float>(m, kOpacity));
}

// ---- DefaultMaterial_t: white, or a random pastel colour ----

void RandomColour(float* rgb) { Internal<void(__cdecl*)(float*)>(0x2EDE7)(rgb); }   // FUN_1002ede7: the CRT's rand()

void* __fastcall DefaultConstruct(void* m, void*, bool random)
{
    static const float white[3] = {1, 1, 1}, black[3] = {0, 0, 0};
    Construct(m, nullptr, "Default", nullptr, white, black, black, black, 0.0f, 0.0f, 1.0f, false, true);
    SetVtable(m, kDefaultVtable);
    if (random) {
        float c[3];
        RandomColour(c);
        std::memcpy(&Field<float>(m, kDiffuse), c, 12);
        std::memcpy(&Field<float>(m, kAmbient), c, 12);
        Field<uint8_t>(m, kZero) = 0;
    }
    return m;
}

void* __fastcall DefaultConstructFrom(void* m, void*, void* archive)   // FUN_1002eede
{
    ConstructFrom(m, nullptr, archive);
    SetVtable(m, kDefaultVtable);
    S().getStream(archive, nullptr);
    float c[3];
    RandomColour(c);
    std::memcpy(&Field<float>(m, kDiffuse), c, 12);
    std::memcpy(&Field<float>(m, kAmbient), c, 12);
    static const float white[3] = {1, 1, 1};
    std::memcpy(&Field<float>(m, kSpecular), white, 12);
    Field<uint8_t>(m, kZero) = 0;
    UpdateSpecular(m);
    Field<uint8_t>(m, kZero) = 0;
    return m;
}

void* __cdecl DefaultInstantiate(void* archive) { return DefaultConstructFrom(vc10::Allocate(0xC0), nullptr, archive); }

}  // namespace

void Install(HMODULE orig)
{
    g_orig = orig;
    g_randy = reinterpret_cast<void* const*>(GetProcAddress(orig, "?s_pcRandy@Randy_t@@1PAV1@A"));
    if (!S().findInt32 || !S().addRgb) {
        Log("materials: serialize.dll exports missing - not replaced");
        return;
    }
    struct Entry {
        uint32_t rva;
        void* target;
        const char* what;
    };
#define FN(f) reinterpret_cast<void*>(&f)
    const Entry entries[] = {
        {0x2F44E, FN(DeltaConstruct), "RDeltaState::RDeltaState(name)"},
        {0x2F539, FN(DeltaConstructFrom), "RDeltaState::RDeltaState(archive) (FUN_1002f539)"},
        {0x2F8A8, FN(DeltaCopy), "RDeltaState copy (FUN_1002f8a8)"},
        {0x2F870, FN(DeltaInstantiate), "RDeltaState::Instantiate"},
        {0x2F4C6, FN(DeltaDestroy), "RDeltaState::~RDeltaState (FUN_1002f4c6)"},
        {0x2F7B0, FN(DeltaSetRenderState), "RDeltaState::SetRenderState"},
        {0x2F7FB, FN(DeltaSetStageState), "RDeltaState::SetTextureStageState"},
        {0x2F0AE, FN(DeltaSetTexture), "RDeltaState::SetTexture"},
        {0x2F0A0, FN(DeltaGetTexture), "RDeltaState texture (FUN_1002f0a0)"},
        {0x2F356, FN(DeltaGetRenderState), "RDeltaState render state (FUN_1002f356)"},
        {0x2F412, FN(DeltaRemoveStageState), "RDeltaState remove stage state (FUN_1002f412)"},
        {0x2F3DF, FN(DeltaRemoveRenderState), "RDeltaState remove render state (FUN_1002f3df)"},
        {0x2F0F7, FN(DeltaClearTexture), "RDeltaState clear texture (FUN_1002f0f7)"},
        {0x2F11C, FN(DeltaCount), "RDeltaState count (FUN_1002f11c)"},
        {0x2F13A, FN(DeltaIsEmpty), "RDeltaState::IsEmpty"},
        {0x2F145, FN(DeltaArchive), "RDeltaState::Archive"},
        {0x2EFE4, FN(DeltaSave), "RDeltaState vtable slot 3 (FUN_1002efe4)"},
        {0x2F275, FN(DeltaApply), "RDeltaState vtable slot 4 (FUN_1002f275)"},
        {0x2F042, FN(DeltaRestore), "RDeltaState vtable slot 5 (FUN_1002f042)"},
        {0x40AA0, FN(Apply), "RMaterial_t apply (FUN_10040aa0)"},
        {0x40AC9, FN(Unapply), "RMaterial_t unapply (FUN_10040ac9)"},
        {0x41043, FN(Construct), "RMaterial_t::RMaterial_t"},
        {0x41146, FN(Copy), "RMaterial_t::RMaterial_t(copy)"},
        {0x4132D, FN(ConstructFrom), "RMaterial_t::RMaterial_t(archive) (FUN_1004132d)"},
        {0x414A7, FN(Instantiate), "RMaterial_t::Instantiate"},
        {0x408CB, FN(Destroy), "RMaterial_t::~RMaterial_t (FUN_100408cb)"},
        {0x40916, FN(Archive), "RMaterial_t::Archive"},
        {0x409C6, FN(InitD3DMaterial), "RMaterial_t::InitD3DMaterial"},
        {0x40A65, FN(SetDeltaState), "RMaterial_t::SetDeltaState"},
        {0x40A8F, FN(UseAlphaAsTransparency), "RMaterial_t::UseAlphaAsTransparency"},
        {0x40ADF, FN(GetTexture), "RMaterial_t::GetTexture"},
        {0x40AFF, FN(IsTwoSided), "RMaterial_t::IsTwoSided"},
        {0x40BEC, FN(SetTexture), "RMaterial_t::SetTexture"},
        {0x40E5F, FN(SetEnvTexture), "RMaterial_t::SetEnvTexture"},
        {0x40F61, FN(SetOpacity), "RMaterial_t::SetOpacity"},
        {0x40FE5, FN(SetTwoSided), "RMaterial_t::SetTwoSided"},
        {0x40FF8, FN(UpdateSpecular), "RMaterial_t::UpdateSpecular"},
        {0x11AAF, FN(IsTransparent), "RMaterial_t::IsTransparent"},
        {0x2EE70, FN(DefaultConstruct), "DefaultMaterial_t::DefaultMaterial_t"},
        {0x2EEDE, FN(DefaultConstructFrom), "DefaultMaterial_t(archive) (FUN_1002eede)"},
        {0x2EF5F, FN(DefaultInstantiate), "DefaultMaterial_t::Instantiate"},
    };
#undef FN
    int installed = 0;
    for (const Entry& e : entries) installed += Replace(orig, e.rva, e.target, e.what) ? 1 : 0;
    Log("materials: %d of %d functions native", installed, int(sizeof(entries) / sizeof(entries[0])));
}

}  // namespace rnative::material
