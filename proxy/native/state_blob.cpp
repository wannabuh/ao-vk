// StateBlob_c natively (part of [Native] Device=on): a set of render states, texture stage states, textures and
// transforms a visual puts on the DeviceState around its drawing (Set / Reset), checked once with ValidateDevice.
//
// StateBlob_c (0x84 bytes): +0x00 set, +0x01 the three fixed states below set, +0x02 validated, +0x03 valid,
// +0x04 uses a texture stage the device lacks, +0x05 every texture present; +0x08 render target, +0x0C blueprint,
// +0x10 material (opaque to Randy); VS2010 vectors: +0x14 render states, +0x24 texture stage states, +0x64 textures,
// +0x74 transforms; +0x34 three fixed render states (FILLMODE, LIGHTING, ALPHABLENDENABLE), 0x10 bytes each.
#include "native/state_blob.h"
#include "native/orig_api.gen.h"
#include "native/vc10.h"

#include <cstdio>
#include <cstring>

namespace rnative::stateblob {

namespace {

HMODULE g_orig;
void* const* g_render;                              // render_t::m_pcInstance
const int32_t* g_alwaysValidate;                    // 0x100B72F4: validate on every call

template <typename T>
T& Field(void* object, uint32_t offset) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(object) + offset); }

struct RenderState {
    uint32_t state, value, old;
    uint8_t set, pad[3];
};
struct StageState {
    uint32_t stage, type, value, old;
    uint8_t set, pad[3];
};
struct Texture {
    uint32_t stage;
    char* name;
    void* surface;                                  // surface_t
    void* old;
    uint8_t set, pad[3];
};
struct Transform {
    uint32_t type;
    void* state;
    float matrix[16];
};
static_assert(sizeof(RenderState) == 0x10 && sizeof(StageState) == 0x14 && sizeof(Texture) == 0x14 &&
                  sizeof(Transform) == 0x48, "StateBlob_c entries");

struct Blob {
    uint8_t isSet, fixedSet, validated, valid, unsupported, texturesComplete, pad[2];
    uint32_t renderTarget;
    void* blueprint;
    void* material;
    vc10::Vector<RenderState> renderStates;         // +0x14
    vc10::Vector<StageState> stageStates;           // +0x24
    RenderState fixed[3];                           // +0x34
    vc10::Vector<Texture> textures;                 // +0x64
    vc10::Vector<Transform> transforms;             // +0x74
};
static_assert(sizeof(Blob) == 0x84 && offsetof(Blob, renderStates) == 0x14 && offsetof(Blob, stageStates) == 0x24 &&
                  offsetof(Blob, fixed) == 0x34 && offsetof(Blob, textures) == 0x64 &&
                  offsetof(Blob, transforms) == 0x74, "StateBlob_c");

void* Devicestate() { return reinterpret_cast<void*(__cdecl*)()>(reinterpret_cast<uint8_t*>(g_orig) + 0x1BDF2)(); }

void Identity(float* m)
{
    std::memset(m, 0, 64);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

Blob* __fastcall Construct(Blob* b)
{
    std::memset(b, 0, sizeof(Blob));
    b->validated = 1;
    b->valid = 1;
    b->texturesComplete = 1;
    b->fixed[0] = RenderState{8, 2, 0, 0, {}};     // D3DRENDERSTATE_FILLMODE, D3DFILL_WIREFRAME
    b->fixed[1] = RenderState{0x89, 0, 0, 0, {}};  // LIGHTING
    b->fixed[2] = RenderState{0x1B, 0, 0, 0, {}};  // ALPHABLENDENABLE
    return b;
}

bool __fastcall NeedTextures(Blob* b) { return b->texturesComplete == 0; }
void __fastcall SetBlueprint(Blob* b, void*, void* p) { b->blueprint = p; }
void __fastcall SetMaterial(Blob* b, void*, void* p) { b->material = p; }
void __fastcall SetRenderTarget(Blob* b, void*, uint32_t t) { b->renderTarget = t; }

// Back to what was there before Set (priority 2, the default).
void __fastcall Reset(Blob* b)
{
    void* ds = Devicestate();
    if (b->isSet) {
        for (Transform* t = b->transforms.first; t != b->transforms.last; ++t) {
            float identity[16];
            Identity(identity);
            orig::render_t_SetTransformMatrix(*g_render, int32_t(t->type), identity);
        }
        if (b->texturesComplete)
            for (Texture* t = b->textures.first; t != b->textures.last; ++t)
                if (t->set) orig::DeviceState_SetTexture(ds, t->old, t->stage, 2);
        for (RenderState* r = b->renderStates.first; r != b->renderStates.last; ++r)
            if (r->set) orig::DeviceState_SetRenderState(ds, int32_t(r->state), r->old, 2);
        for (StageState* s = b->stageStates.first; s != b->stageStates.last; ++s)
            if (s->set) orig::DeviceState_SetTextureStageState(ds, s->stage, int32_t(s->type), s->old, 2);
        b->isSet = 0;
    }
    if (b->fixedSet) {
        for (RenderState& r : b->fixed)
            if (r.set && r.state) orig::DeviceState_SetRenderState(ds, int32_t(r.state), r.old, 2);
        b->fixedSet = 0;
    }
}

// Puts the blob's states on the DeviceState (priority 10), remembering what they replace.
void __fastcall Set(Blob* b)
{
    if (b->isSet || b->fixedSet) {
        std::printf("ERROR!!! Setting states while states are already set. Please reproduce in client debug mode and "
                    "contact Viggo!\n");
        Reset(b);
    }
    uint8_t* ds = static_cast<uint8_t*>(Devicestate());
    for (Transform* t = b->transforms.first; t != b->transforms.last; ++t)
        orig::render_t_SetTransformMatrix(*g_render, int32_t(t->type), t->matrix);
    if (b->texturesComplete)
        for (Texture* t = b->textures.first; t != b->textures.last; ++t) {
            t->old = Field<void*>(ds, 0x1128 + t->stage * 4);
            t->set = orig::DeviceState_SetTexture(ds, t->surface, t->stage, 10) ? 1 : 0;
        }
    for (RenderState* r = b->renderStates.first; r != b->renderStates.last; ++r) {
        r->old = Field<uint32_t>(ds, 0x4C8 + r->state * 4);
        r->set = orig::DeviceState_SetRenderState(ds, int32_t(r->state), r->value, 10) ? 1 : 0;
    }
    for (StageState* s = b->stageStates.first; s != b->stageStates.last; ++s) {
        s->old = Field<uint32_t>(ds, 0xD84 + (s->stage * 0x19 + s->type) * 4);
        s->set = orig::DeviceState_SetTextureStageState(ds, s->stage, int32_t(s->type), s->value, 10) ? 1 : 0;
    }
    b->isSet = 1;
}

// Whether the device draws the blob in one pass (IDirect3DDevice7::ValidateDevice), worked out once.
bool __fastcall Validate(Blob* b)
{
    if (*g_alwaysValidate == 0 && b->validated) return b->valid != 0;
    const uint8_t wasSet = b->isSet;
    b->validated = 1;
    b->valid = 1;
    if (b->unsupported) {
        b->valid = 0;
        return false;
    }
    void* ds = Devicestate();
    if (!wasSet) {
        Set(b);
        orig::DeviceState_UpdateDevice(ds);
    }
    DWORD passes = 0;
    void* device = *static_cast<void**>(*g_render);
    HRESULT hr = 0;
    if (device)
        hr = reinterpret_cast<HRESULT(__stdcall*)(void*, DWORD*)>((*static_cast<void***>(device))[0x98 / 4])(device,
                                                                                                         &passes);
    if (!wasSet) {
        Reset(b);
        orig::DeviceState_UpdateDevice(ds);
    }
    if (passes - 2 <= 0xD) {                        // 2 to 15 passes: not drawable as one
        std::printf("Failed with %lxd = \"%s\", %lxd passes\n", (unsigned long)hr, "", (unsigned long)passes);
        b->valid = 0;
        return false;
    }
    return true;
}

void __fastcall SetRenderState(Blob* b, void*, uint32_t state, uint32_t value)
{
    for (RenderState* r = b->renderStates.first; r != b->renderStates.last; ++r)
        if (r->state == state) {
            r->value = value;
            return;
        }
    b->renderStates.push_back(RenderState{state, value, 0, 0, {}});
}

void __fastcall SetTextureStageState(Blob* b, void*, uint32_t stage, uint32_t type, uint32_t value)
{
    if (Field<uint16_t>(*g_render, 0x246) <= stage) b->unsupported = 1;   // the device's texture stages
    for (StageState* s = b->stageStates.first; s != b->stageStates.last; ++s)
        if (s->stage == stage && s->type == type) {
            if (s->value == value) return;
            b->validated = 0;
            s->value = value;
            return;
        }
    b->stageStates.push_back(StageState{stage, type, value, 0, 0, {}});
    b->validated = 0;
}

const char* __fastcall GetTextureName(Blob* b, void*, uint32_t stage)
{
    for (Texture* t = b->textures.first; t != b->textures.last; ++t)
        if (t->stage == stage) return t->name;
    return nullptr;
}

void* __fastcall GetTexture(Blob* b, void*, uint32_t stage)
{
    for (Texture* t = b->textures.first; t != b->textures.last; ++t)
        if (t->stage == stage) return t->surface;
    return nullptr;
}

// The texture for a stage named earlier; texturesComplete once every named stage has one.
void __fastcall SetTexture(Blob* b, void*, uint32_t stage, void* surface)
{
    b->texturesComplete = 1;
    for (Texture* t = b->textures.first; t != b->textures.last; ++t) {
        if (t->stage == stage) {
            if (t->surface) {
                orig::surface_t_ReleaseDXSurface(t->surface);
                t->surface = nullptr;
            }
            t->surface = surface;
            orig::surface_t_AddRefDXSurface(surface);
        } else if (!t->surface) {
            b->texturesComplete = 0;
        }
    }
}

void __fastcall SetTextureName(Blob* b, void*, uint32_t stage, const char* name)
{
    b->texturesComplete = 0;
    const size_t n = std::strlen(name) + 1;
    char* copy = static_cast<char*>(vc10::AllocateArray(n));
    std::memcpy(copy, name, n);
    for (Texture* t = b->textures.first; t != b->textures.last; ++t)
        if (t->stage == stage) {
            if (t->name) vc10::FreeArray(t->name);
            t->name = copy;
            if (t->surface) {
                orig::surface_t_ReleaseDXSurface(t->surface);
                t->surface = nullptr;
            }
            return;
        }
    b->textures.push_back(Texture{stage, copy, nullptr, nullptr, 0, {}});
}

uint32_t __fastcall GetNumTransformStates(Blob* b) { return uint32_t(b->transforms.size()); }
void* __fastcall GetTransformState(Blob* b, void*, uint32_t i) { return b->transforms[i].state; }

void __fastcall SetTransformMatrix(Blob* b, void*, const float* m, uint32_t i)
{
    std::memcpy(b->transforms[i].matrix, m, 64);
}

// A transform (identity) for `type`; one already there is overwritten - and a new one added all the same.
void __fastcall SetTransformState(Blob* b, void*, uint32_t type, void* state)
{
    Transform t{type, state, {}};
    Identity(t.matrix);
    for (Transform* e = b->transforms.first; e != b->transforms.last; ++e)
        if (e->type == type) *e = t;
    b->transforms.push_back(t);
}

void ReleaseTextures(Blob* b)                       // FUN_10024fa4
{
    for (Texture* t = b->textures.first; t != b->textures.last; ++t)
        if (t->surface) {
            orig::surface_t_ReleaseDXSurface(t->surface);
            t->surface = nullptr;
        }
    b->texturesComplete = 0;
}

void __fastcall Clear(Blob* b)
{
    if (b->isSet || b->fixedSet) Reset(b);
    ReleaseTextures(b);
    for (Texture* t = b->textures.first; t != b->textures.last; ++t)
        if (t->name) vc10::FreeArray(t->name);
    b->renderStates.clear();
    b->stageStates.clear();
    b->textures.clear();
    b->transforms.clear();
    b->validated = 1;
    b->valid = 1;
    b->unsupported = 0;
    b->texturesComplete = 1;
}

void __fastcall Destroy(Blob* b)
{
    Clear(b);
    b->transforms.release();
    b->textures.release();
    b->stageStates.release();
    b->renderStates.release();
}

void* __fastcall GetBlueprint(Blob* b, void*) { return Field<void*>(b, 0x0C); }        // StateBlob_c::GetBlueprint
void* __fastcall GetMaterial(Blob* b, void*) { return Field<void*>(b, 0x10); }        // StateBlob_c::GetMaterial
uint32_t __fastcall GetRenderTarget(Blob* b, void*) { return Field<uint32_t>(b, 0x08); }   // StateBlob_c::GetRenderTarget

}  // namespace

void Install(HMODULE orig)
{
    g_orig = orig;
    g_render = reinterpret_cast<void* const*>(GetProcAddress(orig, "?m_pcInstance@render_t@@0PAV1@A"));
    g_alwaysValidate = reinterpret_cast<const int32_t*>(reinterpret_cast<uint8_t*>(orig) + 0xB72F4);
    struct Entry {
        uint32_t rva;
        void* target;
        const char* what;
    };
#define FN(f) reinterpret_cast<void*>(&f)
    const Entry entries[] = {
        {0x25254, FN(Construct), "StateBlob_c::StateBlob_c"},
        {0x255B5, FN(Destroy), "StateBlob_c::~StateBlob_c"},
        {0x25456, FN(Clear), "StateBlob_c::Clear"},
        {0x24CE2, FN(NeedTextures), "StateBlob_c::NeedTextures"},
        {0x24CF8, FN(GetBlueprint), "StateBlob_c::GetBlueprint"},
        {0x24D09, FN(GetMaterial), "StateBlob_c::GetMaterial"},
        {0x24D1A, FN(GetRenderTarget), "StateBlob_c::GetRenderTarget"},
        {0x24CEB, FN(SetBlueprint), "StateBlob_c::SetBlueprint"},
        {0x24CFC, FN(SetMaterial), "StateBlob_c::SetMaterial"},
        {0x24D0D, FN(SetRenderTarget), "StateBlob_c::SetRenderTarget"},
        {0x24D1E, FN(Reset), "StateBlob_c::Reset"},
        {0x24EAF, FN(GetTextureName), "StateBlob_c::GetTextureName"},
        {0x24EEB, FN(GetTexture), "StateBlob_c::GetTexture"},
        {0x24F27, FN(SetTexture), "StateBlob_c::SetTexture"},
        {0x24FF1, FN(GetNumTransformStates), "StateBlob_c::GetNumTransformStates"},
        {0x24FFE, FN(GetTransformState), "StateBlob_c::GetTransformState"},
        {0x25012, FN(SetTransformMatrix), "StateBlob_c::SetTransformMatrix"},
        {0x25032, FN(Set), "StateBlob_c::Set"},
        {0x251AC, FN(Validate), "StateBlob_c::Validate"},
        {0x252C4, FN(SetRenderState), "StateBlob_c::SetRenderState"},
        {0x25315, FN(SetTextureStageState), "StateBlob_c::SetTextureStageState"},
        {0x253A2, FN(SetTextureName), "StateBlob_c::SetTextureName"},
        {0x254EA, FN(SetTransformState), "StateBlob_c::SetTransformState"},
    };
#undef FN
    int installed = 0;
    for (const Entry& e : entries) installed += Replace(orig, e.rva, e.target, e.what) ? 1 : 0;
    Log("state blobs: %d of %d functions native", installed, int(sizeof(entries) / sizeof(entries[0])));
}

}  // namespace rnative::stateblob
