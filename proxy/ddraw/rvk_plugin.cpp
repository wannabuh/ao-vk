// The interface plugin (docs/ui-dll.md, include/aovk_plugin.h): a DLL in ao-vk-plugins\ next to randy31.dll that
// exports AoVkPluginAttach, loaded when randy-vk.ini [Plugins] Load is on, given the host table, and called once a
// frame and from GUI.dll's drawing (native/ui_layer.cpp). One at a time: the first (by file name) that attaches.
// Without one ao-vk runs on its own: the game's interface, RVK_UiRate, no glass.
#include "ddraw/rvk_backend.h"
#include "ddraw/rvk_settings.h"
#include "native/device.h"
#include "native/native.h"
#include "native/ui_layer.h"

#include "aovk_plugin.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace rvkproxy {

namespace {

AoVkPlugin g_plugin;
bool g_attached;

void HostLog(const char* fmt, ...)
{
    char line[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    RvkLog("plugin: %s", line);
}

int32_t HostLookAllowed() { return rvk_settings::GetEffective("RVK_UiSkin") != 0.0f; }

void HostSetGlass(const AoVkGlass* glass)
{
    AoVkGlass none{};
    rvk_settings::SetPluginGlass(glass ? *glass : none);
}

void HostWindow(float x0, float y0, float x1, float y1) { rnative::device::InterfaceWindow(x0, y0, x1, y1); }

void HostShapes(const AoVkUiShape* shapes, uint32_t count, const AoVkUiMeshes* meshes)
{
    rnative::device::InterfaceShapes(shapes, count, meshes);
}

void* HostImage(const void* bgra, uint32_t width, uint32_t height)
{
    return rnative::device::InterfaceImage(bgra, width, height);
}

void HostImageFree(void* image) { rnative::device::InterfaceImageFree(image); }

const AoVkHost kHost{AOVK_PLUGIN_VERSION, sizeof(AoVkHost), &HostLog, &HostLookAllowed, &HostSetGlass,
                     &HostWindow, &HostShapes, &HostImage, &HostImageFree};

// randy31.dll's folder (with the trailing backslash).
std::string OwnFolder()
{
    HMODULE self = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<const char*>(&OwnFolder), &self))
        return {};
    char path[MAX_PATH];
    const DWORD n = GetModuleFileNameA(self, path, sizeof(path));
    if (!n || n >= sizeof(path))
        return {};
    char* slash = std::strrchr(path, '\\');
    if (!slash)
        return {};
    slash[1] = 0;
    return path;
}

// The plugin at `path`, attached - or unloaded again (false).
bool Attach(const std::string& path, const char* name)
{
    HMODULE module = LoadLibraryA(path.c_str());
    if (!module) {
        RvkLog("plugins: %s not loaded (error %lu)", path.c_str(), GetLastError());
        return false;
    }
    auto attach = reinterpret_cast<AoVkPluginAttachFn>(GetProcAddress(module, AOVK_PLUGIN_ATTACH));
    if (!attach) {                                  // (not a plugin: some other DLL in the folder)
        RvkLog("plugins: %s has no " AOVK_PLUGIN_ATTACH " - not a plugin", path.c_str());
        FreeLibrary(module);
        return false;
    }
    rnative::WatchModule(name);                     // (its faults in the crash log, from its attach on)
    AoVkPlugin plugin{};
    if (!attach(&kHost, &plugin) || plugin.size < sizeof(uint32_t) * 2) {
        RvkLog("plugins: %s refused to attach - unloaded", path.c_str());
        FreeLibrary(module);
        return false;
    }
    // (A plugin built against an older header: the entries it doesn't know stay null.)
    std::memcpy(&g_plugin, &plugin, std::min<size_t>(plugin.size, sizeof(g_plugin)));
    RvkLog("plugins: %s attached (API version %u, the plugin's %u)", path.c_str(), AOVK_PLUGIN_VERSION,
           g_plugin.version);
    return true;
}

}  // namespace

void LoadPlugins()
{
    static bool done;
    if (done)
        return;
    done = true;
    const char* ini = rvk_settings::IniPath();
    char on[16] = "";
    GetPrivateProfileStringA("Plugins", "Load", "", on, sizeof(on), ini);
    if (!on[0]) {
        WritePrivateProfileStringA("Plugins", "Load", "on", ini);
        std::strcpy(on, "on");
    }
    if (_stricmp(on, "on") != 0 && std::strcmp(on, "1") != 0) {
        RvkLog("plugins: off (randy-vk.ini [Plugins] Load)");
        return;
    }
    const std::string folder = OwnFolder() + AOVK_PLUGIN_FOLDER "\\";
    std::vector<std::string> names;
    WIN32_FIND_DATAA found;
    HANDLE find = FindFirstFileA((folder + "*.dll").c_str(), &found);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                names.push_back(found.cFileName);
        } while (FindNextFileA(find, &found));
        FindClose(find);
    }
    if (names.empty()) {
        RvkLog("plugins: none in %s - the game's own interface", folder.c_str());
        return;
    }
    std::sort(names.begin(), names.end());
    for (const std::string& name : names) {
        if (g_attached) {
            RvkLog("plugins: %s%s not loaded - one interface plugin at a time", folder.c_str(), name.c_str());
            continue;
        }
        g_attached = Attach(folder + name, name.c_str());
    }
    if (g_attached)
        rnative::uilayer::SetPlugin(&g_plugin);
}

bool PluginAttached() { return g_attached; }

void PluginFrame()
{
    if (g_attached && g_plugin.frame)
        g_plugin.frame();
}

}  // namespace rvkproxy
