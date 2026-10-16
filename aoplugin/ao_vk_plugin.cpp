// ao_vk_plugin.cpp - ao-vk's AOPlugins plugin (ao-vk.dll): the renderer's settings in the options window (F10).
//
// randy31.dll is the renderer and stays independent of AOPlugins: it exports its settings table as a small C
// interface (RvkSettings_* / RvkPresets_*, proxy/ddraw/rvk_settings.h) and saves its settings itself (randy-vk.ini).
// This plugin turns that table into two tabs: each setting a DValue (bool, or int - float settings count their
// steps) bound to a widget, changes passed on to the renderer. "Renderer" has the preset buttons, the client's frame
// rate cap (AFCM.dll's Timer_t::m_vMaximumFramerate, kept by AOPlugins in AOPlugins.ini) and the on / off switches;
// "Advanced" has every slider and choice.
//
// Ported from the Renderer tab of the AOReloaded fork (wannabuh/AOReloaded, src/core/renderer_settings.cpp and
// fps_cap.cpp), where it lived inside version.dll.

#include "aop_plugin.h"
#include "camp_on_disconnect.h"
#include "view_distance.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

const AopHost* g_host;

// ---- randy-vk's settings interface (proxy/ddraw/rvk_settings.h), version 2 ----

struct RvkSettingInfo {
    uint32_t size;
    const char* name;
    const char* label;
    const char* section;
    uint32_t type;              // 0 bool, 1 int, 2 float, 3 choice (an int from `choices`)
    float min, max, step;
    float value, defaultValue;
    const char* parent;         // the bool switching the feature this setting belongs to (null: none)
    const char* choices;        // choice: the allowed values separated by spaces
};
enum : uint32_t { kBool = 0, kInt = 1, kFloat = 2, kChoice = 3 };

uint32_t(__cdecl* g_count)();
int(__cdecl* g_get)(uint32_t, RvkSettingInfo*);
int(__cdecl* g_set)(const char*, float);
uint32_t(__cdecl* g_presetCount)();             // presets: 0 Original, 1 Classic+, 2 Modern, 3 Ultra
const char*(__cdecl* g_presetName)(uint32_t);
int(__cdecl* g_presetApply)(uint32_t);
int(__cdecl* g_presetCurrent)();
bool g_available;
std::string g_missing = "ao-vk's renderer (randy31.dll) isn't installed";   // why there's no renderer tab content
std::vector<std::string> g_dnames;              // each setting's DValue, by table index (empty: none)

constexpr char kPresetVar[] = "RVK_Preset";     // the preset buttons; kCustom: none matches
constexpr int32_t kCustom = 99;

// The frame rate cap: the client's own limiter divides by it, so never zero.
constexpr char kFpsVar[] = "AOVK_FpsCap";
constexpr int32_t kStockFps = 100, kMinFps = 30, kMaxFps = 500;
float* g_maxFramerate;

bool GetInfo(uint32_t i, RvkSettingInfo& info) {
    std::memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    return g_get(i, &info) != 0 && info.name;
}

// The DValue of setting `i`: its name, cut to the game's 15 characters (RVK_SandContrast: RVK_SandContras).
// Null when it has none (no name, or the cut name is another setting's).
const char* DName(uint32_t i) { return i < g_dnames.size() && !g_dnames[i].empty() ? g_dnames[i].c_str() : nullptr; }

void NameSettings() {
    const uint32_t count = g_count();
    g_dnames.assign(count, std::string());
    for (uint32_t i = 0; i < count; ++i) {
        RvkSettingInfo s;
        if (!GetInfo(i, s) || !*s.name) continue;
        std::string name(s.name, std::min<size_t>(std::strlen(s.name), 15));
        bool taken = false;
        for (uint32_t j = 0; j < i; ++j) taken |= g_dnames[j] == name;
        if (taken) {
            g_host->log(g_host, "%s: DValue %s is another setting's - not in the tabs", s.name, name.c_str());
            continue;
        }
        g_dnames[i] = std::move(name);
    }
}

// A setting, with its DValue, for writing the tabs.
struct Entry {
    RvkSettingInfo s;
    const char* dname;
};

std::vector<Entry> AllSettings() {
    std::vector<Entry> all;
    const uint32_t count = g_count();
    for (uint32_t i = 0; i < count; ++i) {
        Entry e;
        if (GetInfo(i, e.s) && (e.dname = DName(i)) != nullptr) all.push_back(e);
    }
    return all;
}

int32_t ToSteps(const RvkSettingInfo& s, float v) { return static_cast<int32_t>(std::lround(v / s.step)); }

int32_t AsDValue(const RvkSettingInfo& s, float v) {
    if (s.type == kBool) return v != 0.0f ? 1 : 0;
    if (s.type == kFloat) return ToSteps(s, v);
    return static_cast<int32_t>(std::lround(v));
}

int32_t CurrentPreset() {
    const int p = g_presetCount ? g_presetCurrent() : -1;
    return p < 0 ? kCustom : p;
}

bool Resolve() {
    HMODULE randy = GetModuleHandleA("randy31.dll");
    if (!randy) return false;
    auto version = reinterpret_cast<uint32_t(__cdecl*)()>(GetProcAddress(randy, "RvkSettings_Version"));
    g_count = reinterpret_cast<decltype(g_count)>(GetProcAddress(randy, "RvkSettings_Count"));
    g_get = reinterpret_cast<decltype(g_get)>(GetProcAddress(randy, "RvkSettings_Get"));
    g_set = reinterpret_cast<decltype(g_set)>(GetProcAddress(randy, "RvkSettings_Set"));
    if (!version || !g_count || !g_get || !g_set) {
        g_missing = "randy31.dll is the game's own renderer, not ao-vk's";
        return false;
    }
    if (version() < 2) {
        g_missing = "ao-vk's renderer is too old for this plugin (settings interface 1, 2 needed)";
        return false;
    }
    g_presetCount = reinterpret_cast<decltype(g_presetCount)>(GetProcAddress(randy, "RvkPresets_Count"));
    g_presetName = reinterpret_cast<decltype(g_presetName)>(GetProcAddress(randy, "RvkPresets_Name"));
    g_presetApply = reinterpret_cast<decltype(g_presetApply)>(GetProcAddress(randy, "RvkPresets_Apply"));
    g_presetCurrent = reinterpret_cast<decltype(g_presetCurrent)>(GetProcAddress(randy, "RvkPresets_Current"));
    if (!g_presetCount || !g_presetName || !g_presetApply || !g_presetCurrent) g_presetCount = nullptr;
    return true;
}

void AddSetting(const char* name, uint32_t type, int32_t value, int32_t min, int32_t max, uint32_t flags) {
    AopSetting s = {sizeof(AopSetting), name, type, value, min, max, flags};
    g_host->addSetting(g_host, &s);   // a refusal is in the log
}

// Each renderer setting a DValue. The renderer keeps them (randy-vk.ini): AOPlugins doesn't save them.
void RegisterRendererSettings() {
    uint32_t added = 0;
    for (const Entry& e : AllSettings()) {
        const RvkSettingInfo& s = e.s;
        if (s.type == kBool)
            AddSetting(e.dname, AOP_SETTING_BOOL, s.value != 0.0f, 0, 1, AOP_SETTING_NOT_SAVED);
        else if (s.type == kFloat)
            AddSetting(e.dname, AOP_SETTING_INT, ToSteps(s, s.value), ToSteps(s, s.min), ToSteps(s, s.max),
                       AOP_SETTING_NOT_SAVED);
        else
            AddSetting(e.dname, AOP_SETTING_INT, static_cast<int32_t>(std::lround(s.value)),
                       static_cast<int32_t>(s.min), static_cast<int32_t>(s.max), AOP_SETTING_NOT_SAVED);
        ++added;
    }
    if (g_presetCount) AddSetting(kPresetVar, AOP_SETTING_INT, CurrentPreset(), 0, kCustom, AOP_SETTING_NOT_SAVED);
    g_host->log(g_host, "renderer settings: %u (presets: %s)", added, g_presetCount ? "yes" : "no");
}

// Every renderer setting's DValue (and the preset buttons) to the renderer's values - a preset changes many at once.
// setSetting: the options window follows, without reporting the change back here.
void SyncAll() {
    for (const Entry& e : AllSettings()) g_host->setSetting(g_host, e.dname, AsDValue(e.s, e.s.value));
    if (g_presetCount) g_host->setSetting(g_host, kPresetVar, CurrentPreset());
}

void TryRenderer() {
    if (g_available) return;
    if (!Resolve()) {
        g_host->log(g_host, "no renderer settings: %s", g_missing.c_str());
        return;
    }
    NameSettings();
    RegisterRendererSettings();
    g_available = true;                         // only now: the tab writers and settingChanged read the names
}

// ---- The frame rate cap ----

void ApplyFps(int32_t fps) {
    if (!g_maxFramerate) return;
    fps = fps < kMinFps ? kMinFps : fps > kMaxFps ? kMaxFps : fps;
    *g_maxFramerate = static_cast<float>(fps);   // an aligned 4-byte store: the game thread never sees half of it
    g_host->log(g_host, "maximum frame rate %d", fps);
}

// AFCM.dll is a static import of AnarchyOnline.exe: loaded by gameReady.
void InitFpsCap() {
    HMODULE afcm = GetModuleHandleA("AFCM.dll");
    g_maxFramerate = afcm ? reinterpret_cast<float*>(GetProcAddress(afcm, "?m_vMaximumFramerate@Timer_t@@2MA")) : nullptr;
    if (!g_maxFramerate) {
        g_host->log(g_host, "no frame rate cap: AFCM.dll's Timer_t::m_vMaximumFramerate not found");
        return;
    }
    int32_t fps = kStockFps;
    g_host->getSetting(g_host, kFpsVar, &fps);
    g_host->log(g_host, "client frame rate cap was %.0f", static_cast<double>(*g_maxFramerate));
    ApplyFps(fps);
}


// ---- The Renderer tab ----

void Out(AopBuffer* out, const std::string& text) { out->append(out, text.data(), static_cast<uint32_t>(text.size())); }

std::string Escape(const char* text) {
    std::string r;
    for (const char* p = text ? text : ""; *p; ++p) {
        switch (*p) {
        case '&': r += "&amp;"; break;
        case '<': r += "&lt;"; break;
        case '>': r += "&gt;"; break;
        case '"': r += "&quot;"; break;
        default: r += *p;
        }
    }
    return r;
}

bool Is(const char* a, const char* b) { return a && b && std::strcmp(a, b) == 0; }

// A setting in the "Experimental" section: shown in its own block under the switches, off by default (the defaults
// and the section are randy-vk's).
bool IsExperimental(const RvkSettingInfo& s) { return Is(s.section, "Experimental"); }

// The settings whose `parent` is `p`, of the requested kind (bool switches or values).
std::vector<const Entry*> ChildrenOf(const std::vector<Entry>& all, const Entry& p, bool bools) {
    std::vector<const Entry*> list;
    for (const Entry& c : all)
        if (Is(c.s.parent, p.s.name) && (c.s.type == kBool) == bools) list.push_back(&c);
    return list;
}

void Heading(std::string& x, const char* text, int top, const char* font = "HUGE") {
    char attrs[96];
    std::snprintf(attrs, sizeof(attrs), "\" font=\"%s\" layout_borders=\"Rect(0,%d,0,3)\" />\n", font, top);
    x += "\n        <TextView value=\"" + Escape(text) + attrs;
}

void Note(std::string& x, const char* text) {
    x += "        <TextView value=\"" + Escape(text) + "\" font=\"LARGE\" layout_borders=\"Rect(10,0,0,3)\" />\n";
}

// The tab's title and the line under it.
void Title(std::string& x, const char* title, const char* text) {
    x += "        <TextView value=\"" + Escape(title) + "\" font=\"HUGE\" layout_borders=\"Rect(0,0,0,5)\" />\n";
    x += "        <TextView value=\"" + Escape(text) + "\" font=\"LARGE\" layout_borders=\"Rect(0,0,0,10)\" />\n";
}

void CheckBox(std::string& x, const Entry& e, int indent) {
    char attrs[128];
    std::snprintf(attrs, sizeof(attrs),
                  "\" font=\"LARGE\" layout_borders=\"Rect(%d,0,0,0)\" opt_type=\"variant\" opt_variable=\"", indent);
    x += "        <OptionCheckBox label=\"" + Escape(e.s.label) + attrs + e.dname + "\"/>\n";
}

// A slider (int: the value; float: steps, shown scaled back) or, for a choice, radio buttons of its values.
void Value(std::string& x, const Entry& e) {
    const RvkSettingInfo& s = e.s;
    if (s.type == kChoice && s.choices) {
        x += "        <OptionRadioButtonGroup label=\"" + Escape(s.label) +
             ":\" font=\"LARGE\" layout_borders=\"Rect(10,0,0,3)\" opt_type=\"variant\" opt_variable=\"" + e.dname +
             "\">\n";
        for (const char* c = s.choices; *c;) {
            while (*c == ' ') ++c;
            const char* end = c;
            while (*end && *end != ' ') ++end;
            if (end == c) break;
            const std::string value(c, end);
            x += "          <RadioButton label=\"" + Escape(value.c_str()) + "\" value=\"" + Escape(value.c_str()) +
                 "\"/>\n";
            c = end;
        }
        x += "        </OptionRadioButtonGroup>\n";
        return;
    }
    char attrs[192];
    if (s.type == kFloat)
        std::snprintf(attrs, sizeof(attrs),
                      "\" font=\"LARGE\" value_fmt=\"&lt;font color=#70C4D0&gt;%%.2f&lt;/font&gt;\" value_scale=\"%g\"/>\n",
                      static_cast<double>(s.step));
    else
        std::snprintf(attrs, sizeof(attrs),
                      "\" font=\"LARGE\" value_fmt=\"&lt;font color=#70C4D0&gt;%%.0f&lt;/font&gt;\" value_scale=\"1\"/>\n");
    x += "        <OptionSlider label=\"" + Escape(s.label) +
         ":\" layout_borders=\"Rect(10,0,0,3)\" opt_type=\"variant\" opt_variable=\"" + e.dname + attrs;
}

void FrameRate(std::string& x, int top) {
    Heading(x, "Frame rate", top);
    x += std::string("        <OptionSlider label=\"Maximum frame rate (default: 100):\" font=\"LARGE\""
                     " layout_borders=\"Rect(10,0,0,3)\" opt_type=\"variant\" opt_variable=\"") +
         kFpsVar + "\" value_fmt=\"&lt;font color=#70C4D0&gt;%.0f&lt;/font&gt;\" value_scale=\"1\"/>\n";
}

void Presets(std::string& x) {
    Heading(x, "Preset", 0);
    x += std::string("        <OptionRadioButtonGroup label=\"Look:\" font=\"LARGE\" layout_borders=\"Rect(10,0,0,3)\""
                     " opt_type=\"variant\" opt_variable=\"") + kPresetVar + "\">\n";
    const uint32_t n = g_presetCount();
    for (uint32_t i = 0; i < n; ++i)
        x += "          <RadioButton label=\"" + Escape(g_presetName(i)) + "\" value=\"" + std::to_string(i) + "\"/>\n";
    x += "          <RadioButton label=\"Custom\" value=\"" + std::to_string(kCustom) + "\"/>\n";
    x += "        </OptionRadioButtonGroup>\n";
    Note(x, "To keep a setup of your own, back up randy-vk.ini.");
}

// The Renderer tab: the presets, the frame rate cap and view distance, then every on / off option (by section, a
// feature's own options indented under it) and the experimental features' switches in a block of their own.
// Their values (sliders, choices) are in the Advanced tab, so this one stays short.
void __cdecl WriteRendererTab(void* /*user*/, AopBuffer* out) {
    std::string x;
    if (!g_available) {
        Title(x, "ao-vk renderer", (g_missing + ". The frame rate cap and view distance work without it.").c_str());
        FrameRate(x, 0);
        aovk::viewdist::WriteTab(x);
        Out(out, x);
        return;
    }
    Title(x, "ao-vk renderer", "Changes apply at once. Settings are saved to randy-vk.ini.");
    // ao-vk reads the ground as the game's Shadows mode draws it (a base pass and a lightmap pass) for its grass,
    // valley mist and terrain lighting; the mode is the player's to set.
    Note(x, "Set Ground render mode to Shadows in the game's video options: the grass and the terrain's lighting"
            " need it.");
    if (g_presetCount) Presets(x);
    FrameRate(x, g_presetCount ? 12 : 0);
    aovk::viewdist::WriteTab(x);

    const std::vector<Entry> all = AllSettings();
    Heading(x, "Options", 16);
    const char* section = "";
    for (const Entry& e : all) {
        if (e.s.type != kBool || e.s.parent || IsExperimental(e.s)) continue;
        if (!Is(section, e.s.section)) {
            section = e.s.section;
            Heading(x, section, 6);
        }
        CheckBox(x, e, 10);
        for (const Entry* c : ChildrenOf(all, e, true)) CheckBox(x, *c, 30);
    }

    bool any = false;
    for (const Entry& e : all) any |= !e.s.parent && IsExperimental(e.s);
    if (any) {
        Heading(x, "Experimental", 6);
        Note(x, "New and unfinished features. Off by default; they may be unstable or change.");
        for (const Entry& p : all) {
            if (p.s.parent || !IsExperimental(p.s)) continue;
            CheckBox(x, p, 10);
            for (const Entry* c : ChildrenOf(all, p, true)) CheckBox(x, *c, 30);
        }
    }
    Out(out, x);
}

// The Advanced tab: every value (sliders, choices) - a block per feature (its label) or, for values of no feature,
// one per section; then the experimental features' values.
void __cdecl WriteAdvancedTab(void* /*user*/, AopBuffer* out) {
    std::string x;
    if (!g_available) {
        Title(x, "ao-vk renderer: advanced", (g_missing + ".").c_str());
        Out(out, x);
        return;
    }
    Title(x, "ao-vk renderer: advanced", "Every slider and choice. Changes apply at once. Settings are saved to"
                                         " randy-vk.ini.");
    const std::vector<Entry> all = AllSettings();
    const char* heading = "";
    for (const Entry& e : all) {
        if (e.s.parent || IsExperimental(e.s)) continue;
        if (e.s.type == kBool) {
            const std::vector<const Entry*> values = ChildrenOf(all, e, false);
            if (values.empty()) continue;
            Heading(x, e.s.label, 8);
            heading = e.s.label;
            for (const Entry* v : values) Value(x, *v);
        } else {
            if (!Is(heading, e.s.section)) Heading(x, e.s.section, 8);
            heading = e.s.section;
            Value(x, e);
        }
    }
    bool experimental = false;
    for (const Entry& p : all) {
        if (p.s.parent || !IsExperimental(p.s)) continue;
        const std::vector<const Entry*> values = ChildrenOf(all, p, false);
        if (values.empty()) continue;
        if (!experimental) Heading(x, "Experimental", 16);
        experimental = true;
        Heading(x, p.s.label, 8);
        for (const Entry* v : values) Value(x, *v);
    }
    Out(out, x);
}

// ---- Events ----

void __cdecl GameReady() {
    aovk::viewdist::GameReady();
    aovk::camp::GameReady();
    InitFpsCap();
    TryRenderer();
}

// The renderer may be loaded after Utils.dll: one more look. (If it is only found now, the tab the game has already
// read has no settings; they are there from the next start.)
void __cdecl WorldReady() {
    aovk::viewdist::WorldReady();
    if (!g_available) TryRenderer();
}

void __cdecl GameFrame() { aovk::viewdist::GameFrame(); }

void __cdecl DValueChanged(const char* name, const AopValue* value) { aovk::viewdist::DValueChanged(name, value); }

void __cdecl SettingChanged(const char* name, int32_t value) {
    if (aovk::viewdist::SettingChanged(name, value)) return;
    if (std::strcmp(name, kFpsVar) == 0) {
        ApplyFps(value);
        return;
    }
    if (!g_available) return;
    if (g_presetCount && std::strcmp(name, kPresetVar) == 0) {
        if (value != kCustom && g_presetApply(static_cast<uint32_t>(value)))
            g_host->log(g_host, "preset %s", g_presetName(static_cast<uint32_t>(value)));
        SyncAll();                              // the window shows the preset's values
        return;
    }
    const uint32_t count = g_count();
    for (uint32_t i = 0; i < count; ++i) {
        RvkSettingInfo s;
        if (!Is(DName(i), name) || !GetInfo(i, s)) continue;
        float setting = s.type == kFloat ? static_cast<float>(value) * s.step : static_cast<float>(value);
        if (s.type == kBool) setting = value ? 1.0f : 0.0f;
        if (s.min <= s.max) setting = std::clamp(setting, s.min, s.max);   // (std::clamp: undefined otherwise)
        if (setting == s.value) return;         // (sliders send their value every frame while dragged)
        g_set(s.name, setting);                 // the renderer applies and saves it
        g_host->log(g_host, "%s = %g", s.name, static_cast<double>(setting));
        if (g_presetCount) g_host->setSetting(g_host, kPresetVar, CurrentPreset());   // by hand: Custom, likely
        return;
    }
}

const AopPluginInfo kInfo = {
    sizeof(AopPluginInfo), AOP_API_VERSION,
    "ao-vk", "ao-vk", AOP_VERSION(1, 1, 0),
    AOP_PLUGIN_EXCLUSIVE,   // a project: the renderer changes how the whole game looks
    nullptr, 0,
};

const AopPlugin kPlugin = {
    AOP_API_VERSION, sizeof(AopPlugin), &GameReady, &WorldReady, &SettingChanged, &DValueChanged, nullptr, &GameFrame,
};

}  // namespace

AOP_EXPORT const AopPluginInfo* __cdecl AopPluginQuery(void) {
    return &kInfo;
}

AOP_EXPORT const AopPlugin* __cdecl AopPluginAttach(const AopHost* host) {
    // API 2: patches, DValue ranges, the game frame (view distance).
    if (host->version < 2 || host->size < sizeof(AopHost)) {
        host->log(host, "AOPlugins is too old for this plugin (plugin API %u, 2 needed)", host->version);
        return nullptr;
    }
    g_host = host;
    aovk::viewdist::Attach(host);
    aovk::camp::Attach(host);
    // The cap is AOPlugins' to keep (AOPlugins.ini [ao-vk]); the renderer's settings come in gameReady, once
    // randy31.dll can be asked.
    // The AOReloaded fork kept the cap as AOR_FpsCap in AOReloaded.ini: its value is the default here, once.
    const int32_t fps = aovk::viewdist::AOReloadedIni(host, "AOR_FpsCap", kStockFps);
    AddSetting(kFpsVar, AOP_SETTING_INT, fps < kMinFps ? kMinFps : fps > kMaxFps ? kMaxFps : fps, kMinFps, kMaxFps, 0);
    // "Renderer" also replaces a static copy of the tab in a GUI skin (PRK-VK's Root.xml has one). Its values (the
    // sliders and choices) are in a tab of their own, right after it.
    if (!host->addOptionsTab(host, "Renderer", &WriteRendererTab, nullptr)) return nullptr;
    host->addOptionsTab(host, "Advanced", &WriteAdvancedTab, nullptr);   // a refusal is in the log
    return &kPlugin;
}
