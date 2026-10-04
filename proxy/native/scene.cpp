// Which visual is drawing (scene.h).
#include "native/scene.h"
#include "native/cat.h"

#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace rnative::scene {

namespace {

using RenderFn = void(__fastcall*)(void* viewport, void*, int listFrom, int listTo, int type, uint32_t from, uint32_t to);
RenderFn g_render, g_renderRefraction;
void* g_viewport;                                  // the viewport inside Render / RenderRefraction
uint32_t g_renders;                                // Render calls so far (cache refresh clock)
constexpr uint32_t kCurrentVisual = 0x164;         // RViewPort_t: the visual being rendered
constexpr uint32_t kParent = 0x14;                 // RRefFrame_t: its parent frame

void __fastcall RenderHook(void* viewport, void*, int listFrom, int listTo, int type, uint32_t from, uint32_t to)
{
    void* previous = g_viewport;
    g_viewport = viewport;
    ++g_renders;
    g_render(viewport, nullptr, listFrom, listTo, type, from, to);
    g_viewport = previous;
}

void __fastcall RenderRefractionHook(void* viewport, void*, int listFrom, int listTo, int type, uint32_t from,
                                     uint32_t to)
{
    void* previous = g_viewport;
    g_viewport = viewport;
    ++g_renders;
    g_renderRefraction(viewport, nullptr, listFrom, listTo, type, from, to);
    g_viewport = previous;
}

// The RTTI type name of an object (".?AVName@@"), or null. C only (SEH).
const char* RawTypeName(const void* object)
{
    __try {
        auto vtable = *reinterpret_cast<const uintptr_t* const*>(object);
        auto col = reinterpret_cast<const uint32_t*>(vtable[-1]);   // Complete Object Locator
        const char* name = reinterpret_cast<const char*>(col[3] + 8); // TypeDescriptor: vftable, spare, name
        return name[0] == '.' && name[1] == '?' ? name : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

struct Cached {
    const void* vtable = nullptr;
    uint32_t stamp = 0;
    VisualInfo info;
};
std::unordered_map<const void*, Cached> g_visuals;
std::unordered_map<const void*, std::string> g_classNames;   // by vtable; never cleared (names are handed out)

const char* ClassName(const void* object)
{
    const void* vtable = *static_cast<const void* const*>(object);
    auto found = g_classNames.find(vtable);
    if (found != g_classNames.end())
        return found->second.c_str();
    std::string name;
    if (const char* raw = RawTypeName(object)) {
        name = raw + 4;                                 // ".?AV" / ".?AU"
        size_t end = name.find("@@");
        if (end != std::string::npos) name.resize(end);
    }
    return g_classNames.emplace(vtable, name).first->second.c_str();
}

bool StartsWith(const char* s, const char* prefix)
{
    return std::strncmp(s, prefix, std::strlen(prefix)) == 0;
}

Kind KindOf(const char* name)
{
    if (!std::strcmp(name, "RCATMesh_t")) return Kind::Character;
    if (!std::strcmp(name, "AnarchyGround_t")) return Kind::Terrain;
    if (!std::strcmp(name, "VisualRoom_t")) return Kind::Room;
    if (!std::strcmp(name, "VisualLiquid_t")) return Kind::Water;
    if (StartsWith(name, "GfxVisualSimpleShadow")) return Kind::BlobShadow;
    if (StartsWith(name, "GfxVisual")) return Kind::Effect;
    if (std::strstr(name, "Sky")) return Kind::Sky;
    if (!std::strcmp(name, "RTriMesh_t") || !std::strcmp(name, "GenericMeshObject_t") || !std::strcmp(name, "SimpleMesh"))
        return Kind::Static;
    return name[0] ? Kind::Other : Kind::Unknown;
}

// The character (RCATMesh_t frame) somewhere up an object's parents, or null.
const void* CharacterAbove(const void* object)
{
    const void* frame = At<const void*>(object, kParent);
    for (int depth = 0; frame && depth < 12; ++depth, frame = At<const void*>(frame, kParent))
        if (!std::strcmp(ClassName(frame), "RCATMesh_t"))
            return frame;
    return nullptr;
}

// The character carrying a light: one up its parents, or one next to it under its parent (a character's visual hangs
// its light on its own frame) - only under a small parent (not a zone's root, which may hold characters too).
constexpr uint32_t kFirstChild = 0x1C, kNextSibling = 0x18;   // RRefFrame_t children list
const void* CarrierOf(const void* light)
{
    if (const void* character = CharacterAbove(light))
        return character;
    const void* parent = At<const void*>(light, kParent);
    if (!parent)
        return nullptr;
    const void* found = nullptr;
    int children = 0;
    for (const void* child = At<const void*>(parent, kFirstChild); child && children < 64;
         child = At<const void*>(child, kNextSibling), ++children)
        if (!found && !std::strcmp(ClassName(child), "RCATMesh_t"))
            found = child;
    return children <= 8 ? found : nullptr;
}

// ---- lights ----
using ProcessFn = void(__fastcall*)(void* viewport, void*, void* root);
ProcessFn g_process;
LightSinkFn g_lightSink;
const void* const* g_lightsBegin;                  // randy31 0x1017D290: std::vector<RLight_t*> begin, end
constexpr uint32_t kLightListRva = 0x17D290;
constexpr uint32_t kLightData = 0xA4;              // RLight_t: its D3DLIGHT7 (world space after Process)

void __fastcall ProcessHook(void* viewport, void*, void* root)
{
    g_process(viewport, nullptr, root);
    if (!g_lightSink)
        return;
    static std::vector<SceneLight> lights;
    lights.clear();
    const void* const* begin = g_lightsBegin[0] ? static_cast<const void* const*>(g_lightsBegin[0]) : nullptr;
    const void* const* end = static_cast<const void* const*>(g_lightsBegin[1]);
    for (const void* const* it = begin; it && it != end && lights.size() < 1024; ++it) {
        SceneLight l;
        std::memcpy(l.d3dLight, static_cast<const uint8_t*>(*it) + kLightData, sizeof(l.d3dLight));
        l.owner = CarrierOf(*it);
        lights.push_back(l);
    }
    g_lightSink(lights.data(), lights.size());
}

}  // namespace

void SetLightSink(LightSinkFn sink)
{
    g_lightSink = sink;
}

const void* CurrentVisual()
{
    return g_viewport ? At<const void*>(g_viewport, kCurrentVisual) : nullptr;
}

VisualInfo Describe(const void* visual)
{
    if (!visual)
        return {};
    const void* vtable = *static_cast<const void* const*>(visual);
    Cached& c = g_visuals[visual];
    if (c.vtable == vtable && g_renders - c.stamp < 2000)
        return c.info;
    c.vtable = vtable;
    c.stamp = g_renders;
    c.info.className = ClassName(visual);
    c.info.kind = KindOf(c.info.className);
    // Attached to a character (somewhere up its frames): one of its parts.
    c.info.owner = c.info.kind == Kind::Character ? visual : nullptr;
    if (c.info.kind != Kind::Character && c.info.kind != Kind::BlobShadow)
        if (const void* character = CharacterAbove(visual)) {
            c.info.kind = Kind::CharacterPart;
            c.info.owner = character;
        }
    VisualInfo info = c.info;
    if (g_visuals.size() > 20000)                       // visuals come and go: start over now and then
        g_visuals.clear();
    return info;
}

bool Installed()
{
    return g_render != nullptr;
}

void Install(HMODULE orig)
{
    if (GetMode("Visuals", Mode::Off) != Mode::On)
        return;
    // RViewPort_t::Render: mov eax, <handler> (absolute); call _EH_prolog (relative).
    static const uint8_t kRender[] = {0xB8, 0xBC, 0x78, 0x08, 0x10, 0xE8, 0xB7, 0xC7, 0x02, 0x00};
    static const size_t kRenderAbs[] = {1}, kRenderRel[] = {6};
    HookFixups fixups;
    fixups.abs32 = kRenderAbs;
    fixups.abs32Count = 1;
    fixups.rel32 = kRenderRel;
    fixups.rel32Count = 1;
    g_render = reinterpret_cast<RenderFn>(HookEntry(orig, 0x4BFFF, kRender, sizeof(kRender),
                                                    reinterpret_cast<void*>(&RenderHook), "RViewPort_t::Render", fixups));
    static const uint8_t kRefraction[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x10};   // push ebp; mov ebp, esp; sub esp, 10h
    g_renderRefraction = reinterpret_cast<RenderFn>(
        HookEntry(orig, 0x4C4EA, kRefraction, sizeof(kRefraction), reinterpret_cast<void*>(&RenderRefractionHook),
                  "RViewPort_t::RenderRefraction"));
    // RViewPort_t::Process: push ebp; mov ebp, esp; push ecx; fld dword [<constant>] (absolute).
    static const uint8_t kProcess[] = {0x55, 0x8B, 0xEC, 0x51, 0xD9, 0x05, 0x54, 0x3F, 0x09, 0x10};
    static const size_t kProcessAbs[] = {6};
    HookFixups processFixups;
    processFixups.abs32 = kProcessAbs;
    processFixups.abs32Count = 1;
    g_lightsBegin = reinterpret_cast<const void* const*>(reinterpret_cast<uint8_t*>(orig) + kLightListRva);
    g_process = reinterpret_cast<ProcessFn>(HookEntry(orig, 0x4BF39, kProcess, sizeof(kProcess),
                                                      reinterpret_cast<void*>(&ProcessHook), "RViewPort_t::Process",
                                                      processFixups));
    Log("visuals: %s%s", g_render && g_renderRefraction ? "on" : "not installed (unknown client build)",
        g_process ? ", lights from the scene" : "");
}

}  // namespace rnative::scene
