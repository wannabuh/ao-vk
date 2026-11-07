// RVK_WebNoSpare: the in-game browser (the market) starts Chromium without a spare page process. PRK's Awesomium.dll
// runs the browser on CEF (Chromium, cef_helper.exe for its other processes). Chromium starts a renderer process ahead
// of time for the next page; under Wine that spare one can't connect and ends itself after 15 seconds ("Terminating
// current process after 15 seconds with no connection" in cef/cef-<pid>.log), and once the market's window has been
// closed, every new one stays empty until the game is restarted. Without the spare process each page gets a renderer
// started when it is needed.
// Awesomium.dll's import of cef_initialize comes here: the CefApp it passes gets its OnBeforeCommandLineProcessing
// wrapped, so after the shim's own switches the browser process's command line also says
// --disable-features=SpareRendererForSitePerProcess. Chromium reads its switches once, so the setting takes effect at
// the next start.
#include "rvk_backend.h"
#include "rvk_settings.h"

#include <windows.h>

#include <cwchar>
#include <string>

namespace rvkproxy {

namespace {

// CEF's C API as of CEF 146 (the include/capi structs generated from include/cef_app.h and cef_command_line.h): a
// struct starts with cef_base_ref_counted_t (its size and four calls), then one function pointer per method in the
// header's order. Only what is used here.
struct CefString {                                // cef_string_t: UTF-16 on Windows
    const wchar_t* str;
    size_t length;
    void (*dtor)(wchar_t*);
};
struct CefCommandLine;
using AddRefFn = void(__stdcall*)(void* self);
using ReleaseFn = int(__stdcall*)(void* self);
using HasSwitchFn = int(__stdcall*)(CefCommandLine* self, const CefString* name);
using GetSwitchValueFn = CefString*(__stdcall*)(CefCommandLine* self, const CefString* name);   // userfree, or null
using AppendSwitchWithValueFn = void(__stdcall*)(CefCommandLine* self, const CefString* name, const CefString* value);
using CommandLineFn = void(__stdcall*)(void* app, const CefString* processType, CefCommandLine* commandLine);
using InitializeFn = int(__cdecl*)(const void* args, const void* settings, void* app, void* sandboxInfo);
using UserfreeFreeFn = void(__cdecl*)(CefString* string);

// Pointer-sized slots from the struct's start (slot 0: its size).
constexpr int kAddRef = 1, kRelease = 2;          // cef_base_ref_counted_t
constexpr int kAppCommandLine = 5;                // cef_app_t::on_before_command_line_processing
constexpr int kHasSwitch = 16, kGetSwitchValue = 17, kAppendSwitchWithValue = 20;
// cef_command_line_t's size: 5 slots of base and 20 methods, 21 with remove_switch (CEF API 14100 on).
constexpr size_t kCommandLineSizes[] = {25 * sizeof(void*), 26 * sizeof(void*)};

InitializeFn g_initialize = nullptr;
CommandLineFn g_shimCommandLine = nullptr;
UserfreeFreeFn g_userfreeFree = nullptr;

template <class Fn>
Fn Slot(void* object, int slot)
{
    return reinterpret_cast<Fn>(static_cast<void**>(object)[slot]);
}

CefString Str(const wchar_t* s) { return {s, std::wcslen(s), nullptr}; }

void DisableSpareRenderer(CefCommandLine* commandLine)
{
    const size_t size = *reinterpret_cast<const size_t*>(commandLine);
    CefString gpu = Str(L"disable-gpu"), madeUp = Str(L"randy-vk-no-such-switch");
    auto has = Slot<HasSwitchFn>(commandLine, kHasSwitch);
    // The layout as expected: the size of CEF 146's struct, the shim's own switch there and a made-up one not.
    if ((size != kCommandLineSizes[0] && size != kCommandLineSizes[1]) || !has(commandLine, &gpu) ||
        has(commandLine, &madeUp)) {
        RvkLog("browser: CEF's command line isn't laid out as expected (size %u) - RVK_WebNoSpare not applied",
               unsigned(size));
        return;
    }
    CefString name = Str(L"disable-features");
    std::wstring value;
    if (CefString* old = Slot<GetSwitchValueFn>(commandLine, kGetSwitchValue)(commandLine, &name)) {
        if (old->str)
            value.assign(old->str, old->length);
        if (g_userfreeFree)
            g_userfreeFree(old);
    }
    if (!value.empty())
        value += L',';
    value += L"SpareRendererForSitePerProcess";
    CefString v = {value.c_str(), value.size(), nullptr};
    Slot<AppendSwitchWithValueFn>(commandLine, kAppendSwitchWithValue)(commandLine, &name, &v);
    RvkLog("browser: Chromium starts with --disable-features=%ls (no spare page process)", value.c_str());
}

void __stdcall OnBeforeCommandLine(void* app, const CefString* processType, CefCommandLine* commandLine)
{
    if (!commandLine) {
        if (g_shimCommandLine)
            g_shimCommandLine(app, processType, commandLine);
        return;
    }
    // A struct passed to a callback is the callee's: the shim's wrapper releases the command line as it returns, so a
    // reference of our own keeps it for after.
    Slot<AddRefFn>(commandLine, kAddRef)(commandLine);
    if (g_shimCommandLine)
        g_shimCommandLine(app, processType, commandLine);
    else
        Slot<ReleaseFn>(commandLine, kRelease)(commandLine);   // no shim's callback: the one passed in is ours too
    if (!processType || !processType->length)
        DisableSpareRenderer(commandLine);        // the browser process's own command line only
    Slot<ReleaseFn>(commandLine, kRelease)(commandLine);
}

int __cdecl CefInitialize(const void* args, const void* settings, void* app, void* sandboxInfo)
{
    if (app && *static_cast<const size_t*>(app) >= (kAppCommandLine + 1) * sizeof(void*)) {
        void** slot = static_cast<void**>(app) + kAppCommandLine;
        g_shimCommandLine = reinterpret_cast<CommandLineFn>(*slot);
        DWORD protect;
        VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &protect);
        *slot = reinterpret_cast<void*>(&OnBeforeCommandLine);
        VirtualProtect(slot, sizeof(void*), protect, &protect);
    } else {
        RvkLog("browser: cef_initialize got no CefApp to wrap - RVK_WebNoSpare not applied");
    }
    return g_initialize(args, settings, app, sandboxInfo);
}

}  // namespace

// At startup (the first export call, before the game starts its browser): points Awesomium.dll's import of
// cef_initialize here.
void InstallBrowserHooks()
{
    if (rvk_settings::Get("RVK_WebNoSpare") == 0.0f)
        return;
    HMODULE awesomium = GetModuleHandleA("Awesomium.dll"), cef = GetModuleHandleA("libcef.dll");
    if (!awesomium || !cef) {
        RvkLog("browser: Awesomium.dll or libcef.dll not loaded at startup - RVK_WebNoSpare not applied");
        return;
    }
    g_userfreeFree = reinterpret_cast<UserfreeFreeFn>(GetProcAddress(cef, "cef_string_userfree_utf16_free"));
    g_initialize = reinterpret_cast<InitializeFn>(GetProcAddress(cef, "cef_initialize"));
    void* previous = g_initialize ? PatchImport(awesomium, "libcef.dll", "cef_initialize",
                                                reinterpret_cast<void*>(&CefInitialize))
                                  : nullptr;
    if (!previous) {
        RvkLog("browser: Awesomium's import of cef_initialize not found - RVK_WebNoSpare not applied");
        return;
    }
    g_initialize = reinterpret_cast<InitializeFn>(previous);
    RvkLog("browser: Awesomium's cef_initialize hooked (RVK_WebNoSpare)");
}

}  // namespace rvkproxy
