// view_distance.cpp - characters and full-quality ground further out, placed objects further out.
//
// Ported from the AOReloaded fork (wannabuh/AOReloaded, src/hooks/view_distance.cpp), its patches now made through
// AOPlugins (AopHost.patch: checked bytes, one owner).
//
// How the client limits them:
//   - SetDValue (Utils.dll) clamps every value to its DValue's min/max; LoginPrefs.xml gives DisplayCharViewDistance
//     5..80 and DisplayGroundFullQualityRadius 7..44.
//   - N3.dll FUN_1001f964 copies DisplayCharViewDistance into n3Camera_t +0x174, replacing anything below 5 or above
//     80 with 70. On ground playfields n3VisualDynel_t::Run hides every character further than that from the player.
//   - DisplayGroundFullQualityRadius goes to AnarchyGround_t::SetHQRadius: ground triangles within it get their own
//     high-resolution texture, the rest share the low-resolution one - the visible ring in the distance.
//   - Past 44 the ground corrupts the heap (crash, 2026-10-09): see the ground index patch.
//   - Outdoors, placed objects (statels: buildings' props, lamps, crates) come and go with distance in N3's statel
//     grid: see the statel rings.
//
// So: raise both DValues' maximums, jump over N3's upper clamp, fix the ground's index offsets, scale the statel
// rings, and keep the chosen values (AOPlugins.ini [ao-vk]). The client loads its prefs (clamping them to the stock
// maximums) before the ranges can be raised, so the values are set again once, on the game thread, after login.

#include "view_distance.h"

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace aovk::viewdist {

namespace {

const AopHost* g_host;

constexpr char kCharVar[] = "DisplayCharViewDistance";
constexpr char kGroundVar[] = "DisplayGroundFullQualityRadius";
constexpr char kCharSetting[] = "AOVK_CharDist";      // the chosen values (0: never changed - the client's stands)
constexpr char kGroundSetting[] = "AOVK_GroundHQ";
constexpr char kObjSetting[] = "AOVK_ObjDist";        // the statel rings, percent of the stock distances

// Stock minimums (LoginPrefs.xml); maximums raised from 80 and 44.
constexpr int32_t kCharMin = 5, kCharMax = 300;
constexpr int32_t kGroundMin = 7, kGroundMax = 150, kGroundStockMax = 44;
constexpr int32_t kObjMin = 100, kObjMax = 400;

// N3.dll FUN_1001f964:
//   1001f9a6  83 7D F0 50   cmp dword ptr [ebp-0x10], 0x50
//   1001f9aa  7E 07         jle 1001f9b3          ; the fild that stores it
//   1001f9ac  C7 45 F0 46.. mov dword ptr [ebp-0x10], 0x46
// becomes a jmp to 1001f9b3, so only the lower clamp (5) is left.
constexpr uint32_t kClampRva = 0x1f9a6;
constexpr uint8_t kClampOriginal[] = {0x83, 0x7D, 0xF0, 0x50, 0x7E, 0x07};
constexpr uint8_t kClampPatched[] = {0xEB, 0x0B, 0x90, 0x90, 0x90, 0x90};

// DisplaySystem.dll FUN_10034658 (AnarchyGround_t, laying the tessellated ground out into pooled vertex buffers):
// each high-quality texture group gets a range of its buffer's index array at an offset from [ebp-0x40]. That counter
// runs on over every group, never going back to 0 when the layout moves on to the next buffer, so later buffers are
// written past the end of their arrays once there are enough high-quality triangles - never at the stock radius.
// The draw reads each group's indices from its own buffer's array at that offset, so restarting the counter for every
// buffer is right. Every move to the next buffer goes through 100347c5 (the only jump into it):
//   100347c5  2B 8E AC 05 00 00   sub ecx, [esi+0x5ac]
// becomes a jmp to a stub doing "and dword ptr [ebp-0x40], 0" first.
constexpr uint32_t kNextBufferRva = 0x347c5;
constexpr uint8_t kNextBufferOriginal[] = {0x2B, 0x8E, 0xAC, 0x05, 0x00, 0x00};

// N3.dll statel grid (built per playfield, FUN_10028ebd): cells in rings by the camera's distance to their centre,
// against 0.5 x the view cone length (~500 m) x five factors: 0.1, 0.15, 0.3, 0.4, 0.55 -> 50 / 75 / 150 / 200 /
// 275 m; the further out the ring, the fewer size classes shown (small things only within 50-75 m). The grid copies
// the factors from shared constants (also used elsewhere), so its five "fld [constant]" are pointed at our own,
// scaled factors instead. Read when a playfield's grid is built: changes apply from the next zone.
constexpr uint32_t kRingLoadRvas[5] = {0x28f92, 0x28f9b, 0x28fa4, 0x28fad, 0x28fb6};
constexpr uint32_t kRingConstRvas[5] = {0x3d61c, 0x3e674, 0x3e29c, 0x3e670, 0x3e66c};
constexpr float kRingFactors[5] = {0.1f, 0.15f, 0.3f, 0.4f, 0.55f};
float g_rings[5] = {0.1f, 0.15f, 0.3f, 0.4f, 0.55f};

constexpr uint32_t kCameraCharDistance = 0x174;   // n3Camera_t, float metres

using FnGetEngineInstance = void*(__cdecl*)();
using FnGetActiveCamera = void*(__thiscall*)(void* engine);
FnGetEngineInstance g_getEngine;
FnGetActiveCamera g_getActiveCamera;

bool g_clampPatched, g_groundPatched, g_ringsPatched;
volatile bool g_reapplyPending;

int32_t Clamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

void AddSetting(const char* name, int32_t value, int32_t min, int32_t max) {
    AopSetting s = {sizeof(AopSetting), name, AOP_SETTING_INT, value, min, max, 0};
    g_host->addSetting(g_host, &s);
}

int32_t Setting(const char* name) {
    int32_t v = 0;
    g_host->getSetting(g_host, name, &v);
    return v;
}

// The camera reads the setting only when it is built; set the active one too. Game thread.
void SetCameraCharDistance(int32_t metres) {
    if (!g_getEngine || !g_getActiveCamera) return;
    void* engine = g_getEngine();
    void* camera = engine ? g_getActiveCamera(engine) : nullptr;
    if (camera) *reinterpret_cast<float*>(static_cast<uint8_t*>(camera) + kCameraCharDistance) = float(metres);
}

bool PatchClamp(uint8_t* n3) {
    return g_host->patch(g_host, n3 + kClampRva, kClampOriginal, kClampPatched, sizeof(kClampPatched)) != 0;
}

bool PatchGroundIndexOffsets(uint8_t* ds) {
    uint8_t* at = ds + kNextBufferRva;
    auto* stub = static_cast<uint8_t*>(VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!stub) return false;
    uint8_t code[] = {
        0x83, 0x65, 0xC0, 0x00,                 // and dword ptr [ebp-0x40], 0
        0x2B, 0x8E, 0xAC, 0x05, 0x00, 0x00,     // sub ecx, [esi+0x5ac]   (the original)
        0xE9, 0, 0, 0, 0,                       // jmp 100347cb
    };
    const int32_t back = static_cast<int32_t>((at + sizeof(kNextBufferOriginal)) - (stub + sizeof(code)));
    std::memcpy(code + 11, &back, 4);
    std::memcpy(stub, code, sizeof(code));
    FlushInstructionCache(GetCurrentProcess(), stub, sizeof(code));

    uint8_t jump[6] = {0xE9, 0, 0, 0, 0, 0x90};  // jmp stub; nop
    const int32_t rel = static_cast<int32_t>(stub - (at + 5));
    std::memcpy(jump + 1, &rel, 4);
    if (g_host->patch(g_host, at, kNextBufferOriginal, jump, sizeof(jump))) return true;
    VirtualFree(stub, 0, MEM_RELEASE);
    return false;
}

// Each "fld dword ptr [constant]" (D9 05 imm32) pointed at our factor - all five checked before any is patched, so
// another client build is left as it is rather than half done.
bool PatchStatelRings(uint8_t* n3) {
    uint8_t original[5][6], patched[5][6];
    for (int i = 0; i < 5; ++i) {
        const uint32_t constant = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(n3 + kRingConstRvas[i]));
        const uint32_t ours = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_rings[i]));
        original[i][0] = patched[i][0] = 0xD9;
        original[i][1] = patched[i][1] = 0x05;
        std::memcpy(original[i] + 2, &constant, 4);
        std::memcpy(patched[i] + 2, &ours, 4);
        if (std::memcmp(n3 + kRingLoadRvas[i], original[i], 6) != 0) {
            g_host->log(g_host, "N3 statel ring loads differ - not patched, objects keep the stock distances");
            return false;
        }
    }
    bool all = true;
    for (int i = 0; i < 5; ++i)
        all &= g_host->patch(g_host, n3 + kRingLoadRvas[i], original[i], patched[i], 6) != 0;
    return all;
}

void SetStatelRings(int32_t percent) {
    percent = Clamp(percent, kObjMin, kObjMax);
    for (int i = 0; i < 5; ++i) g_rings[i] = kRingFactors[i] * float(percent) / 100.0f;
    g_host->log(g_host, "statel rings at %d%%: %.0f / %.0f / %.0f / %.0f / %.0f m (from the next zone)", percent,
                g_rings[0] * 500.0, g_rings[1] * 500.0, g_rings[2] * 500.0, g_rings[3] * 500.0, g_rings[4] * 500.0);
}

// Put a saved value back (within the raised range), keeping the DValue's type (the sliders store doubles).
void Reapply(const char* var, const char* setting, int32_t lo, int32_t hi) {
    int32_t wanted = Setting(setting);
    if (wanted <= 0) return;
    wanted = Clamp(wanted, lo, hi);
    AopValue current{};
    if (!g_host->dvalueGet(g_host, var, &current) || current.type == AOP_VALUE_BOOL) return;
    if (static_cast<int32_t>(current.value) == wanted) return;
    AopValue v = current;
    v.value = wanted;
    g_host->dvalueSet(g_host, var, &v);
    g_host->log(g_host, "%s %d -> %d", var, static_cast<int32_t>(current.value), wanted);
}

}  // namespace

int32_t AOReloadedIni(const AopHost* host, const char* key, int32_t fallback) {
    char ini[MAX_PATH];
    std::snprintf(ini, sizeof(ini), "%sAOReloaded.ini", host->clientDir(host));
    return static_cast<int32_t>(GetPrivateProfileIntA("AOReloaded", key, fallback, ini));
}

void Attach(const AopHost* host) {
    g_host = host;
    AddSetting(kCharSetting, Clamp(AOReloadedIni(host, "AOR_CharDist", 0), 0, kCharMax), 0, kCharMax);
    AddSetting(kGroundSetting, Clamp(AOReloadedIni(host, "AOR_GroundHQ", 0), 0, kGroundMax), 0, kGroundMax);
    AddSetting(kObjSetting, Clamp(AOReloadedIni(host, "AOR_ObjDist", kObjMin), kObjMin, kObjMax), kObjMin, kObjMax);
    host->watchDValue(host, kCharVar);
    host->watchDValue(host, kGroundVar);
}

// Before the game world: the game thread can't be running this code yet.
void GameReady() {
    auto* ds = reinterpret_cast<uint8_t*>(GetModuleHandleA("DisplaySystem.dll"));
    auto* n3 = reinterpret_cast<uint8_t*>(GetModuleHandleA("N3.dll"));
    g_groundPatched = ds && PatchGroundIndexOffsets(ds);
    if (n3) {
        g_getEngine = reinterpret_cast<FnGetEngineInstance>(
            GetProcAddress(reinterpret_cast<HMODULE>(n3), "?GetInstance@n3EngineClient_t@@SAPAV1@XZ"));
        g_getActiveCamera = reinterpret_cast<FnGetActiveCamera>(
            GetProcAddress(reinterpret_cast<HMODULE>(n3), "?GetActiveCamera@n3EngineClient_t@@QBEPAVn3Camera_t@@XZ"));
        g_clampPatched = PatchClamp(n3);
        SetStatelRings(Setting(kObjSetting));    // the grid isn't built yet
        g_ringsPatched = PatchStatelRings(n3);
    }
    g_host->log(g_host, "view distance: characters %s, ground %s, objects %s",
                g_clampPatched ? "up to 300 m" : "stock (not patched)", g_groundPatched ? "up to 150" : "stock (not patched)",
                g_ringsPatched ? "scalable" : "stock (not patched)");
}

void WorldReady() {
    if (g_clampPatched) g_host->dvalueRange(g_host, kCharVar, kCharMin, kCharMax);
    if (g_groundPatched) g_host->dvalueRange(g_host, kGroundVar, kGroundMin, kGroundMax);
    g_reapplyPending = true;
}

void GameFrame() {
    if (!g_reapplyPending) return;
    g_reapplyPending = false;
    Reapply(kCharVar, kCharSetting, kCharMin, g_clampPatched ? kCharMax : 80);
    Reapply(kGroundVar, kGroundSetting, kGroundMin, g_groundPatched ? kGroundMax : kGroundStockMax);
    AopValue v{};
    if (g_host->dvalueGet(g_host, kCharVar, &v)) SetCameraCharDistance(static_cast<int32_t>(v.value));
}

// The game's sliders: remember what was chosen (after the game's clamping: read back).
void DValueChanged(const char* name, const AopValue* value) {
    const bool chars = std::strcmp(name, kCharVar) == 0;
    if (!chars && std::strcmp(name, kGroundVar) != 0) return;
    AopValue kept = *value;
    g_host->dvalueGet(g_host, name, &kept);
    const int32_t v = static_cast<int32_t>(kept.value);
    g_host->setSetting(g_host, chars ? kCharSetting : kGroundSetting, v);
    if (chars) SetCameraCharDistance(v);
}

bool SettingChanged(const char* name, int32_t value) {
    if (std::strcmp(name, kObjSetting) != 0) return std::strcmp(name, kCharSetting) == 0 || std::strcmp(name, kGroundSetting) == 0;
    SetStatelRings(value);
    return true;
}

void WriteTab(std::string& x) {
    x += "\n        <TextView value=\"View distance\" font=\"HUGE\" layout_borders=\"Rect(0,12,0,3)\" />\n";
    if (g_ringsPatched)
        x += std::string("        <OptionSlider label=\"Object detail distance, % of the stock distances (from the next "
                         "zone):\" font=\"LARGE\" layout_borders=\"Rect(10,0,0,3)\" opt_type=\"variant\" opt_variable=\"") +
             kObjSetting + "\" value_fmt=\"&lt;font color=#70C4D0&gt;%.0f%%&lt;/font&gt;\" value_scale=\"1\"/>\n";
    char note[256];
    std::snprintf(note, sizeof(note),
                  "The game's own sliders go further: character view distance up to %d m, ground full quality up to "
                  "%d.",
                  g_clampPatched ? kCharMax : 80, g_groundPatched ? kGroundMax : kGroundStockMax);
    x += std::string("        <TextView value=\"") + note + "\" font=\"LARGE\" layout_borders=\"Rect(10,0,0,3)\" />\n";
}

}  // namespace aovk::viewdist
