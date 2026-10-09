// Pop-in log: see popin.h. A diagnostic, off unless randy-vk.ini [Native] PopLog=on.
//
// A frame here is one RViewPort_t::Process of the main scene root (the root processed every frame; other roots, such
// as an inventory preview, are ignored). Its draws are compared when the next one starts, while the camera still holds
// that frame's position and view * projection (RCamera_t +0x120, rebuilt during Process).
#include "native/popin.h"
#include "native/orig_api.gen.h"
#include "native/scene.h"

#include <windows.h>
#include <intrin.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rnative::popin {

namespace {

constexpr uint32_t kParent = 0x14, kWorld = 0x44, kCameraViewProjection = 0x120, kViewportCamera = 0xC;
constexpr uint32_t kTriMeshData = 0x184;          // RTriMesh_t: its RTriMeshData_t (mesh.cpp kData)
constexpr uint32_t kRecent = 2;                   // a change this many frames before the event caused it
constexpr uint32_t kSummaryFrames = 600, kLinesPerSummary = 40;

bool g_enabled;
float g_minDistance = 15.0f;

uint64_t g_frame;
const void* g_root;
uint64_t g_rootFrame;                             // the last frame the main root was processed
bool g_inMain;
bool g_newRoot;                                   // the frame just ended is the first of its root: nothing to compare

float g_eye[3];
float g_viewProjection[16];
bool g_haveCamera;

std::unordered_set<const void*> g_drawnNow, g_drawnPrev;
std::unordered_map<const void*, uint64_t> g_lastDrawn;   // visual -> the frame it was last drawn
std::unordered_map<const void*, uint64_t> g_created;     // visual -> its frame of birth (pruned)

// A game module's address range, for telling its return addresses on the stack apart.
struct Module {
    uintptr_t begin, end;
};
std::vector<Module> g_modules;

using Callers = std::array<uintptr_t, 3>;

struct Change {
    uint64_t frame;
    const char* what;                             // "created", "attached", "detached", "made visible", ...
    Callers callers;
};
std::unordered_map<const void*, Change> g_changes;  // frame -> its latest change (pruned)

struct Gone {
    uint64_t frame;
    float position[3];
    const char* className;
};
std::unordered_map<const void*, Gone> g_gone;    // visuals destroyed since the last comparison

struct Stats {
    uint32_t count = 0;
    float min = 1e9f, max = 0.0f;
    double sum = 0.0;
};
std::map<std::string, Stats> g_summary;
uint32_t g_lines;
uint64_t g_summaryFrame;

template <typename T>
T Field(const void* object, uint32_t offset) { return *reinterpret_cast<const T*>(static_cast<const uint8_t*>(object) + offset); }

bool Readable(const void* p, size_t bytes)
{
    const uintptr_t a = reinterpret_cast<uintptr_t>(p);
    if (a < 0x10000 || a + bytes < a) return false;
    for (uintptr_t page = a & ~uintptr_t(0xFFF); page < a + bytes; page += 0x1000) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<const void*>(page), &mbi, sizeof(mbi)) != sizeof(mbi) || mbi.State != MEM_COMMIT ||
            (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            return false;
    }
    return true;
}

void FindModules()
{
    for (const char* name : {"DisplaySystem.dll", "N3.dll", "Gamecode.dll", "Interfaces.dll", "GUI.dll", "FXS.dll"}) {
        auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleA(name));
        if (!base) continue;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
        g_modules.push_back({reinterpret_cast<uintptr_t>(base), reinterpret_cast<uintptr_t>(base) + nt->OptionalHeader.SizeOfImage});
    }
}

bool InGame(uintptr_t a)
{
    for (const Module& m : g_modules)
        if (a >= m.begin && a < m.end) return true;
    return false;
}

// The first few return addresses into the game's modules above this call: who asked (a scan, so now and then a stale
// value slips in - good enough to name the code).
Callers GameCallers()
{
    Callers out{};
    const auto* sp = static_cast<const uintptr_t*>(_AddressOfReturnAddress());
    const auto* top = reinterpret_cast<const uintptr_t*>(reinterpret_cast<const NT_TIB*>(NtCurrentTeb())->StackBase);
    size_t n = 0;
    for (const uintptr_t* p = sp; p < top && p < sp + 0x200 && n < out.size(); ++p)
        if (InGame(*p)) out[n++] = *p;
    return out;
}

std::string Describe(uintptr_t a)
{
    char text[96];
    DescribeAddress(a, text, sizeof(text));
    return text;
}

std::string CallerText(const Callers& c)
{
    std::string s;
    for (uintptr_t a : c) {
        if (!a) break;
        if (!s.empty()) s += " < ";
        s += Describe(a);
    }
    return s.empty() ? "?" : s;
}

void Position(const void* frame, float* out)
{
    const float* world = reinterpret_cast<const float*>(static_cast<const uint8_t*>(frame) + kWorld);
    out[0] = world[12], out[1] = world[13], out[2] = world[14];
}

float Distance(const float* p)
{
    const float d[3] = {p[0] - g_eye[0], p[1] - g_eye[1], p[2] - g_eye[2]};
    return std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}

bool InView(const float* p)
{
    const float* m = g_viewProjection;
    const float x = p[0] * m[0] + p[1] * m[4] + p[2] * m[8] + m[12];
    const float y = p[0] * m[1] + p[1] * m[5] + p[2] * m[9] + m[13];
    const float w = p[0] * m[3] + p[1] * m[7] + p[2] * m[11] + m[15];
    return w > 0.1f && std::fabs(x) <= 1.05f * w && std::fabs(y) <= 1.05f * w;
}

std::string MeshName(const void* visual, const scene::VisualInfo& info)
{
    if (info.kind != scene::Kind::Static || !Readable(visual, kTriMeshData + 4)) return "";
    const void* data = Field<const void*>(visual, kTriMeshData);
    if (!data || !Readable(data, 0x40)) return "";
    const char* name = orig::RResource_t_GetName(const_cast<void*>(data));
    return name && Readable(name, 1) ? name : "";
}

// The latest change on the visual or a frame above it, within kRecent frames of `frame`.
const Change* RecentChange(const void* visual, uint64_t frame, bool appearing)
{
    const void* f = visual;
    for (int depth = 0; f && depth < 12; ++depth) {
        auto found = g_changes.find(f);
        if (found != g_changes.end() && frame - found->second.frame <= kRecent) {
            const char* w = found->second.what;
            const bool showing = !std::strcmp(w, "created") || !std::strcmp(w, "attached") || !std::strcmp(w, "made visible");
            if (showing == appearing) return &found->second;
        }
        if (!Readable(f, kParent + 4)) break;
        f = Field<const void*>(f, kParent);
    }
    return nullptr;
}

void Record(bool appear, const void* visual, const float* p, const char* className, const std::string& name,
            const std::string& cause, uint64_t gap)
{
    const float distance = Distance(p);
    const std::string key = std::string(appear ? "appear   " : "vanish   ") + className + "  " + cause;
    Stats& s = g_summary[key];
    ++s.count;
    s.sum += distance;
    if (distance < s.min) s.min = distance;
    if (distance > s.max) s.max = distance;
    if (g_lines++ >= kLinesPerSummary) return;
    char gapText[32] = "first time";
    if (gap) std::snprintf(gapText, sizeof(gapText), "after %llu frames", (unsigned long long)gap);
    Log("pop-in: %s %.1f m %s%s%s%s at (%.0f, %.0f, %.0f): %s (%s)", appear ? "appeared" : "vanished", distance, className,
        name.empty() ? "" : " '", name.c_str(), name.empty() ? "" : "'", p[0], p[1], p[2], cause.c_str(),
        appear ? gapText : "");
}

std::string Cause(const void* visual, bool appear, uint64_t frame)
{
    if (const Change* c = RecentChange(visual, frame, appear)) {
        std::string s = c->what;
        if (c->callers[0]) s += " by " + CallerText(c->callers);
        return s;
    }
    // Nothing changed around it: its own Process (or one above it) chose to (not) add it to a render list.
    const void* vtable = Readable(visual, 4) ? Field<const void*>(visual, 0) : nullptr;
    const void* process = vtable && Readable(vtable, 9 * 4) ? Field<const void*>(vtable, 8 * 4) : nullptr;
    return std::string("own Process ") + (process ? Describe(reinterpret_cast<uintptr_t>(process)) : "?");
}

// The frame that just ended against the one before.
void Compare()
{
    const uint64_t frame = g_frame;
    for (const void* v : g_drawnNow) {
        if (g_drawnPrev.count(v)) continue;
        if (!Readable(v, kWorld + 64)) continue;
        float p[3];
        Position(v, p);
        if (!InView(p) || Distance(p) < g_minDistance) continue;
        const auto seen = g_lastDrawn.find(v);
        const uint64_t last = seen != g_lastDrawn.end() ? seen->second : 0;
        const scene::VisualInfo info = scene::Describe(v);
        std::string cause = Cause(v, true, frame);
        auto born = g_created.find(v);
        if (born != g_created.end() && frame - born->second <= kRecent && cause.compare(0, 7, "created") != 0)
            cause = "created, then " + cause;
        Record(true, v, p, info.className, MeshName(v, info), cause, last ? frame - last : 0);
    }
    for (const void* v : g_drawnPrev) {
        if (g_drawnNow.count(v)) continue;
        auto gone = g_gone.find(v);
        if (gone != g_gone.end()) {
            if (InView(gone->second.position) && Distance(gone->second.position) >= g_minDistance)
                Record(false, v, gone->second.position, gone->second.className, "", "destroyed", 0);
            continue;
        }
        if (!Readable(v, kWorld + 64)) continue;
        float p[3];
        Position(v, p);
        if (!InView(p) || Distance(p) < g_minDistance) continue;
        const scene::VisualInfo info = scene::Describe(v);
        Record(false, v, p, info.className, MeshName(v, info), Cause(v, false, frame), 0);
    }
    for (const void* v : g_drawnNow) g_lastDrawn[v] = frame;
    g_gone.clear();
    for (auto it = g_changes.begin(); it != g_changes.end();)
        it = frame - it->second.frame > kRecent + 1 ? g_changes.erase(it) : ++it;
    for (auto it = g_created.begin(); it != g_created.end();)
        it = frame - it->second > kRecent + 1 ? g_created.erase(it) : ++it;

    if (frame - g_summaryFrame >= kSummaryFrames) {
        if (!g_summary.empty()) {
            Log("pop-in: summary of the last %u frames (beyond %.0f m, in view): count, distance min / mean / max", kSummaryFrames,
                g_minDistance);
            for (const auto& [key, s] : g_summary)
                Log("pop-in:   %5u  %6.1f %6.1f %6.1f  %s", s.count, s.min, s.sum / s.count, s.max, key.c_str());
        }
        g_summary.clear();
        g_lines = 0;
        g_summaryFrame = frame;
    }
}

}  // namespace

void Install(HMODULE)
{
    g_enabled = GetMode("PopLog", Mode::Off) == Mode::On;
    if (!g_enabled) return;
    if (IniPath()[0]) g_minDistance = float(GetPrivateProfileIntA("Native", "PopLogMin", 15, IniPath()));
    Log("pop-in log: on (appearing / vanishing in view beyond %.0f m)", g_minDistance);
}

bool Enabled() { return g_enabled; }

void FrameStart(void* viewport, void* root)
{
    if (!g_enabled) return;
    if (g_modules.empty()) FindModules();
    if (root != g_root) {
        if (g_root && g_frame - g_rootFrame < 30) {   // another root (a preview): not the scene
            g_inMain = false;
            return;
        }
        g_root = root;                            // the first root, or a new one after a zone change
        g_drawnNow.clear();
        g_lastDrawn.clear();
        g_newRoot = true;
    } else if (g_haveCamera && !g_newRoot) {
        Compare();
    } else {
        g_newRoot = false;
        for (const void* v : g_drawnNow) g_lastDrawn[v] = g_frame;   // (what a zone shows when it comes in)
    }
    g_drawnPrev.swap(g_drawnNow);
    g_drawnNow.clear();
    ++g_frame;
    g_rootFrame = g_frame;
    g_inMain = true;
    const void* camera = viewport && Readable(viewport, kViewportCamera + 4) ? Field<const void*>(viewport, kViewportCamera) : nullptr;
    g_haveCamera = camera && Readable(camera, kCameraViewProjection + 64);
    if (g_haveCamera) {
        Position(camera, g_eye);
        std::memcpy(g_viewProjection, static_cast<const uint8_t*>(camera) + kCameraViewProjection, 64);
    }
}

void Drawn(const void* visual)
{
    if (g_enabled && g_inMain) g_drawnNow.insert(visual);
}

void Created(const void* visual, const void* caller)
{
    if (!g_enabled) return;
    g_created[visual] = g_frame;
    Callers c = GameCallers();
    if (!c[0]) c[0] = reinterpret_cast<uintptr_t>(caller);
    g_changes[visual] = {g_frame, "created", c};
}

void Destroyed(const void* visual)
{
    if (!g_enabled) return;
    g_lastDrawn.erase(visual);
    g_created.erase(visual);
    g_changes.erase(visual);
    if (!g_drawnPrev.count(visual) && !g_drawnNow.count(visual)) return;
    Gone g{g_frame, {}, scene::Describe(visual).className};
    Position(visual, g.position);
    g_gone[visual] = g;
    g_drawnNow.erase(visual);
}

void Attached(const void* frame, bool attached, const void* caller)
{
    if (!g_enabled) return;
    Callers c = GameCallers();
    if (!c[0]) c[0] = reinterpret_cast<uintptr_t>(caller);
    g_changes[frame] = {g_frame, attached ? "attached" : "detached", c};
}

void VisibleSet(const void* frame, bool visible, const void* caller)
{
    if (!g_enabled) return;
    if (Readable(frame, 0x98) && (Field<uint8_t>(frame, 0x94) != 0) == visible) return;   // no change
    Callers c = GameCallers();
    if (!c[0]) c[0] = reinterpret_cast<uintptr_t>(caller);
    g_changes[frame] = {g_frame, visible ? "made visible" : "made invisible", c};
}

}  // namespace rnative::popin
