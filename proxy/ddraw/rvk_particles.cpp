// Game side of the GPU particles (rvk::Device::ParticleEmitter): which of the game's sprite effects get particles, and
// their sprites. Two virtual functions are hooked by patching their vtable slots (checked against the known client):
//
//   GfxVisualDiaBill::Render (DisplaySystem.dll) - the sprite batch most effects draw through (up to 128 camera-facing
//     sprites: position, size, colour, atlas frame, alive). Announces the effect with its sprites in world space, then
//     lets the game draw - with its sprites dimmed (RVK_PartCore) - and rvk draws the particles right after.
//   _GfxControlStars_t::Update (Gamecode.dll) - the effect behind most sparkles. Knows its template id (gfxtweak.bin) and
//     its sprite batch, so effects can be chosen by template: randy-vk.ini [Particles] Templates=id,id,first-last.
//
// Layouts from the decompiled client (randy-vk/re/ds_diabill.c, gc_stars*.c).
#include "rvk_backend.h"
#include "rvk_settings.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace rvkproxy {

namespace {

// GfxVisualDiaBill (DisplaySystem.dll, image base 0x10000000).
constexpr uint32_t kDiaBillVtable = 0x1008A6DC - 0x10000000, kDiaBillRenderSlot = 13;
constexpr uint32_t kDiaBillRender = 0x1001105E - 0x10000000;
constexpr uint32_t kDiaBillHidden = 0x178, kDiaBillMaterial = 0x17C, kDiaBillColumns = 0x184, kDiaBillRows = 0x188;
constexpr uint32_t kDiaBillCount = 0x1A8, kDiaBillSprites = 0x1AC, kDiaBillPackedUv = 0x1B0;
// DiaBill_n::Sprite_t, 32 bytes.
struct GameSprite {
    float pos[3];              // effect space
    float width, height;
    uint32_t color;            // D3DCOLOR
    uint32_t frame;            // atlas cell, or (packed) u v du dv bytes
    uint8_t alive;
    uint8_t pad[3];
};
static_assert(sizeof(GameSprite) == 32, "DiaBill_n::Sprite_t");

// _GfxControlStars_t (Gamecode.dll, image base 0x10000000).
constexpr uint32_t kStarsVtable = 0x1016CD6C - 0x10000000, kStarsUpdateSlot = 1;
constexpr uint32_t kStarsUpdate = 0x100F7F3C - 0x10000000;
constexpr uint32_t kControlTemplate = 0x4, kStarsVisual = 0x40;

using RenderFn = uint32_t(__thiscall*)(void* self, void* viewport);
using UpdateFn = uint32_t(__thiscall*)(void* self);
RenderFn g_render;
UpdateFn g_update;
using WorldMatrixFn = const float*(__thiscall*)(const void* self);   // RRefFrame_t::GetWorldMatrix (randy31_orig)
WorldMatrixFn g_worldMatrix;

bool g_installed, g_gaveUp;
uint32_t g_frame = 1;
struct Range { uint32_t first, last; };
std::vector<Range> g_templates;
bool g_allTemplates;
// Sprite batches of chosen effects, with the frame their effect last updated them.
std::unordered_map<const void*, uint32_t> g_chosen;
// Per batch: the frame a sprite was last alive (particles keep being drawn a while after the last one dies).
std::unordered_map<const void*, uint32_t> g_lastAlive;

template <typename T>
T At(const void* object, uint32_t offset)
{
    T v;
    std::memcpy(&v, static_cast<const uint8_t*>(object) + offset, sizeof(T));
    return v;
}

// "43299, 98700000-98700099" or "all".
void LoadTemplates()
{
    char buf[1024] = "";
    GetPrivateProfileStringA("Particles", "Templates", "", buf, sizeof(buf), rvk_settings::IniPath());
    if (!buf[0]) {
        // Lesser Controlled Rage (its sparkles and the clones gfxtweak.py boost makes of them).
        std::strcpy(buf, "43299,98700000-98700099");
        WritePrivateProfileStringA("Particles", "Templates", buf, rvk_settings::IniPath());
    }
    g_allTemplates = std::strstr(buf, "all") != nullptr;
    for (char* p = buf; *p;) {
        char* end;
        unsigned long a = std::strtoul(p, &end, 10);
        if (end == p) { ++p; continue; }
        unsigned long b = a;
        if (*end == '-') b = std::strtoul(end + 1, &end, 10);
        g_templates.push_back({uint32_t(a), uint32_t(b)});
        p = end;
    }
    RvkLog("particles: %s templates %s", g_allTemplates ? "all" : "chosen", buf);
}

bool Chosen(uint32_t id)
{
    if (g_allTemplates) return true;
    for (const Range& r : g_templates)
        if (id >= r.first && id <= r.last) return true;
    return false;
}

bool PatchSlot(HMODULE module, uint32_t vtable, uint32_t slot, uint32_t expected, void* hook, void** original,
               const char* what)
{
    auto base = reinterpret_cast<uintptr_t>(module);
    auto* entry = reinterpret_cast<uintptr_t*>(base + vtable + 4 * slot);
    if (*entry != base + expected) {
        RvkLog("particles: %s vtable slot is %p, expected %p - unknown client version, particles off", what,
               (void*)*entry, (void*)(base + expected));
        return false;
    }
    DWORD protect;
    if (!VirtualProtect(entry, sizeof(*entry), PAGE_READWRITE, &protect))
        return false;
    *original = reinterpret_cast<void*>(*entry);
    *entry = reinterpret_cast<uintptr_t>(hook);
    VirtualProtect(entry, sizeof(*entry), protect, &protect);
    return true;
}

uint32_t __fastcall StarsUpdate(void* self, void* /*edx*/)
{
    uint32_t r = g_update(self);
    const void* visual = At<const void*>(self, kStarsVisual);
    if (visual && Chosen(At<uint32_t>(self, kControlTemplate)))
        g_chosen[visual] = g_frame;
    return r;
}

uint32_t __fastcall DiaBillRender(void* self, void* /*edx*/, void* viewport)
{
    auto it = g_chosen.find(self);
    rvk::ThreadedDevice* device = g_rvk.device;
    if (it == g_chosen.end() || it->second + 30 < g_frame || !device || !device->GetParticleParams().enable ||
        At<uint32_t>(self, kDiaBillHidden) != 0)
        return g_render(self, viewport);

    auto* sprites = At<GameSprite*>(self, kDiaBillSprites);
    uint32_t count = std::min<uint32_t>(std::max(At<int32_t>(self, kDiaBillCount), 0), rvk::Device::kParticleSlots);
    if (!sprites || !count)
        return g_render(self, viewport);
    const float* m = g_worldMatrix(self);           // row vectors: p' = p * M
    bool atlas = At<const void*>(self, kDiaBillMaterial) != nullptr;
    bool packed = At<uint8_t>(self, kDiaBillPackedUv) != 0;
    int32_t columns = std::max(At<int32_t>(self, kDiaBillColumns), 1), rows = std::max(At<int32_t>(self, kDiaBillRows), 1);

    rvk::Device::ParticleSprite out[rvk::Device::kParticleSlots];
    float center[3] = {0, 0, 0};
    uint32_t alive = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const GameSprite& s = sprites[i];
        rvk::Device::ParticleSprite& o = out[i];
        for (int c = 0; c < 3; ++c)
            o.pos[c] = s.pos[0] * m[c] + s.pos[1] * m[4 + c] + s.pos[2] * m[8 + c] + m[12 + c];
        o.size = s.width;
        o.color = s.color;
        o.alive = s.alive ? 1u : 0u;
        if (packed) {                              // GfxVisualDiaBill::Render's packed rectangle (1/128, 1/64 steps)
            o.uv[0] = float((s.frame >> 24) & 0xFF) / 128.0f;
            o.uv[1] = float((s.frame >> 16) & 0xFF) / 64.0f;
            o.uv[2] = float((s.frame >> 8) & 0xFF) / 128.0f;
            o.uv[3] = float(s.frame & 0xFF) / 64.0f;
        } else if (atlas) {
            int32_t column = int32_t(s.frame) % columns, row = int32_t(s.frame) / columns;
            o.uv[0] = float(column) / float(columns);
            o.uv[1] = float(row) / float(rows);
            o.uv[2] = 1.0f / float(columns);
            o.uv[3] = 1.0f / float(rows);
        } else {
            o.uv[0] = o.uv[1] = 0.0f;
            o.uv[2] = o.uv[3] = 1.0f;
        }
        if (s.alive) {
            for (int c = 0; c < 3; ++c) center[c] += o.pos[c];
            ++alive;
        }
    }
    // The swirl and the pull centre on the effect's live sprites (its origin may be at the character's feet).
    if (alive) {
        for (float& c : center) c /= float(alive);
        g_lastAlive[self] = g_frame;
    } else {
        std::memcpy(center, m + 12, sizeof(center));
    }

    // The game's own sprites, dimmed while it draws them. With none alive, a transparent one keeps the effect drawing
    // (its particles are drawn after its sprite draw) for as long as particles may live.
    float core = rvk_settings::Get("RVK_PartCore");
    uint32_t savedColor[rvk::Device::kParticleSlots];
    uint8_t savedAlive = sprites[0].alive;
    float savedWidth = sprites[0].width, savedHeight = sprites[0].height;
    for (uint32_t i = 0; i < count; ++i) {
        savedColor[i] = sprites[i].color;
        uint32_t a = uint32_t(float(sprites[i].color >> 24) * core + 0.5f);
        sprites[i].color = (sprites[i].color & 0x00FFFFFFu) | (std::min(a, 255u) << 24);
    }
    bool keepAlive = false;
    if (!alive) {
        auto la = g_lastAlive.find(self);
        float life = device->GetParticleParams().life * 1.5f;
        keepAlive = la != g_lastAlive.end() && float(g_frame - la->second) < life * 240.0f;   // frames, at up to 240 fps
        if (keepAlive) {
            sprites[0].alive = 1;
            sprites[0].color &= 0x00FFFFFFu;
            sprites[0].width = sprites[0].height = 0.0f;
        }
    }
    uint32_t r;
    {
        static const unsigned index = ComIndex("IDirect3DDevice7::DrawIndexedPrimitive");
        ComScope scope(index);
        device->ParticleEmitter(uint64_t(uintptr_t(self)), center, out, count);
        r = g_render(self, viewport);
        device->EndParticleEmitter();
    }
    for (uint32_t i = 0; i < count; ++i)
        sprites[i].color = savedColor[i];
    if (keepAlive) {
        sprites[0].alive = savedAlive;
        sprites[0].width = savedWidth;
        sprites[0].height = savedHeight;
    }
    return r;
}

}  // namespace

// Each presented frame: installs the hooks once both modules are loaded (Gamecode.dll comes after the renderer).
void ParticleFrame()
{
    ++g_frame;
    if (g_frame % 600 == 0) {                         // forget batches of effects that ended
        for (auto it = g_chosen.begin(); it != g_chosen.end();)
            it = it->second + 600 < g_frame ? g_chosen.erase(it) : std::next(it);
        for (auto it = g_lastAlive.begin(); it != g_lastAlive.end();)
            it = it->second + 2400 < g_frame ? g_lastAlive.erase(it) : std::next(it);
    }
    if (g_installed || g_gaveUp)
        return;
    HMODULE ds = GetModuleHandleA("DisplaySystem.dll"), gc = GetModuleHandleA("Gamecode.dll");
    HMODULE orig = GetModuleHandleA("randy31_orig.dll");
    if (!ds || !gc || !orig)
        return;
    g_worldMatrix = reinterpret_cast<WorldMatrixFn>(
        GetProcAddress(orig, "?GetWorldMatrix@RRefFrame_t@@QBEABVTMatrix4_t@@XZ"));
    LoadTemplates();
    void* render = nullptr;
    void* update = nullptr;
    if (!g_worldMatrix ||
        !PatchSlot(ds, kDiaBillVtable, kDiaBillRenderSlot, kDiaBillRender, reinterpret_cast<void*>(&DiaBillRender),
                   &render, "GfxVisualDiaBill::Render")) {
        g_gaveUp = true;
        return;
    }
    g_render = reinterpret_cast<RenderFn>(render);
    if (!PatchSlot(gc, kStarsVtable, kStarsUpdateSlot, kStarsUpdate, reinterpret_cast<void*>(&StarsUpdate), &update,
                   "_GfxControlStars_t::Update")) {
        g_gaveUp = true;                              // the render hook stays in, but no effect is ever chosen
        return;
    }
    g_update = reinterpret_cast<UpdateFn>(update);
    g_installed = true;
    RvkLog("particles: hooks installed (DisplaySystem %p, Gamecode %p)", (void*)ds, (void*)gc);
}

}  // namespace rvkproxy
