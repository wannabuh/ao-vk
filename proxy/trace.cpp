// Frame inspector: transparent trace thunks for the exports listed in proxy/trace.txt.
//
// Each traced export points at trace_thunk_N, which pushes N and jumps to trace_common. That saves all
// registers, calls OnCall() with the saved registers, the caller's return address and its stack
// arguments, then restores everything and jumps to the original export with the stack exactly as the
// caller left it. No signatures are needed, so any export can be traced.
//
// Normally OnCall() only watches for frame boundaries (Randy_t::Flip). Ctrl+Shift+F12, every 60 s, or
// frame number RANDYVK_CAPTURE_FRAME arms a capture of the next whole frame: the sequence of passes, render targets and draw calls
// (grouped by calling module), per-export call counts, the contents of the render lists, and (with
// RANDYVK_DDRAW=trace) the Direct3D 7 COM calls Randy made during the frame.
// Output: %RANDYVK_LOG%, else randy-vk.log in the current directory (the client folder).

#include "ddraw/com_trace.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

struct Export {
    const char* mangled;
    const char* name;
};

const Export kExports[] = {
#define TRACE_EXPORT(i, mangled, name) {mangled, name},
#include "trace_table.inc"
#undef TRACE_EXPORT
};
constexpr unsigned kExportCount = sizeof(kExports) / sizeof(kExports[0]);

// Registers as saved by pushad, then what was on the stack when the thunk was entered.
struct CallFrame {
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t index;
    uint32_t ret;
    uint32_t args[8];
};

// randy31 0x101CEDE8: RVisual_t* g_renderLists[11][1800], linked through RVisual_t+0xE8.
constexpr uint32_t kRenderListsRva = 0x1CEDE8;
constexpr int kLists = 11, kBuckets = 1800;
constexpr uint32_t kNextOffset = 0xE8;

HMODULE g_orig;
int g_flip = -1, g_viewportRender = -1, g_viewportRefraction = -1;

// --- capture state (main thread only; other threads are ignored while capturing) ---
enum class Capture { Idle, Armed, Recording };
Capture g_capture = Capture::Idle;
DWORD g_captureThread;
DWORD g_lastAuto;
unsigned g_frame;
std::vector<std::string> g_events;
std::map<std::string, unsigned> g_counts;   // "export <- module" -> calls
std::string g_runModule;                     // current run of consecutive draw calls
std::string g_runDraw;
unsigned g_runCount;
unsigned g_captureFrame;                     // RANDYVK_CAPTURE_FRAME: capture this frame number (1-based)
std::vector<uint32_t> g_comStart;            // COM call counters when the capture started

FILE* Log()
{
    static FILE* f = [] {
        char path[MAX_PATH] = "randy-vk.log";
        GetEnvironmentVariableA("RANDYVK_LOG", path, sizeof(path));
        FILE* file = std::fopen(path, "a");
        if (file)
            std::fprintf(file, "\n==== randy-vk frame inspector, pid %lu, %u traced exports\n",
                         GetCurrentProcessId(), kExportCount);
        return file;
    }();
    return f;
}

std::string ModuleOf(uint32_t address)
{
    static std::map<HMODULE, std::string> names;
    HMODULE module = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(address), &module))
        return "?";
    auto it = names.find(module);
    if (it != names.end())
        return it->second;
    char path[MAX_PATH];
    GetModuleFileNameA(module, path, sizeof(path));
    const char* base = std::strrchr(path, '\\');
    return names[module] = base ? base + 1 : path;
}

// MSVC RTTI: vftable[-1] -> Complete Object Locator, +12 -> TypeDescriptor, +8 -> ".?AVName@@".
// Plain C so __try is allowed (no objects that need unwinding).
bool RawClassName(const void* object, char* buf, size_t size)
{
    __try {
        auto vtable = *reinterpret_cast<const uint32_t* const*>(object);
        auto col = reinterpret_cast<const uint32_t*>(vtable[-1]);
        std::strncpy(buf, reinterpret_cast<const char*>(col[3] + 8), size - 1);
        buf[size - 1] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::string ClassOf(const void* object)
{
    char buf[128];
    if (!RawClassName(object, buf, sizeof(buf)))
        return "<no rtti>";
    std::string s = buf;
    if (s.rfind(".?AV", 0) == 0 || s.rfind(".?AU", 0) == 0)
        s = s.substr(4);
    if (auto at = s.find("@@"); at != std::string::npos)
        s.resize(at);
    return s;
}

bool IsDraw(unsigned index)
{
    return std::strncmp(kExports[index].name, "render_t::Render", 16) == 0;
}

bool IsStructural(unsigned index)
{
    static const char* names[] = {"RViewPort_t::Render", "RViewPort_t::RenderRefraction", "RViewPort_t::Open",
                                  "RViewPort_t::Close", "RViewPort_t::Clear", "RViewPort_t::Process",
                                  "Randy_t::SetRenderTarget", "Randy_t::PushRenderTarget",
                                  "Randy_t::PopRenderTarget", "Randy_t::SetRenderTargetAsTexture",
                                  "Randy_t::Flip", "render_t::Blt", "DynamicVB_c::Reset"};
    for (const char* n : names)
        if (std::strcmp(kExports[index].name, n) == 0)
            return true;
    return false;
}

void FlushDrawRun()
{
    if (g_runCount) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "  %u x %s from %s", g_runCount, g_runDraw.c_str(), g_runModule.c_str());
        g_events.push_back(buf);
    }
    g_runCount = 0;
}

std::string DescribeLists(const CallFrame& f)
{
    // RViewPort_t::Render(listFrom, listTo, flags, bucketFrom, bucketTo)
    int l0 = static_cast<int>(f.args[0]), l1 = static_cast<int>(f.args[1]);
    unsigned b0 = f.args[3], b1 = f.args[4];
    if (l0 > l1) std::swap(l0, l1);
    if (b0 > b1) std::swap(b0, b1);
    if (l0 < 0 || l1 >= kLists || b1 >= kBuckets)
        return "    (list arguments out of range)";
    auto table = reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(g_orig) + kRenderListsRva);
    std::string out;
    for (int list = l0; list <= l1; ++list) {
        std::map<std::string, unsigned> classes;
        unsigned total = 0, minBucket = kBuckets, maxBucket = 0;
        for (unsigned b = b0; b <= b1; ++b)
            for (uint32_t v = table[list * kBuckets + b]; v && total < 100000;
                 v = *reinterpret_cast<uint32_t*>(v + kNextOffset)) {
                ++classes[ClassOf(reinterpret_cast<void*>(v))];
                ++total;
                if (b < minBucket) minBucket = b;
                if (b > maxBucket) maxBucket = b;
            }
        char buf[128];
        std::snprintf(buf, sizeof(buf), "    list %d: %u visuals", list, total);
        out += buf;
        if (total) {
            std::snprintf(buf, sizeof(buf), " (buckets %u..%u):", minBucket, maxBucket);
            out += buf;
            for (auto& [name, n] : classes)
                out += " " + name + " x" + std::to_string(n);
        }
        out += "\n";
    }
    if (!out.empty()) out.pop_back();
    return out;
}

void EndCapture()
{
    FlushDrawRun();
    FILE* f = Log();
    if (!f)
        return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::fprintf(f, "\n--- frame %u captured %02d:%02d:%02d ---\nsequence:\n", g_frame, t.wHour, t.wMinute, t.wSecond);
    for (auto& e : g_events)
        std::fprintf(f, "%s\n", e.c_str());
    std::fprintf(f, "calls (export <- caller module):\n");
    for (auto& [k, n] : g_counts)
        std::fprintf(f, "  %6u  %s\n", n, k.c_str());
    if (rvkproxy::ComTraceActive() && g_comStart.size() == rvkproxy::ComMethodCount()) {
        std::vector<std::pair<uint32_t, unsigned>> com;
        for (unsigned i = 0; i < g_comStart.size(); ++i)
            if (uint32_t d = rvkproxy::ComCallCount(i) - g_comStart[i])
                com.push_back({d, i});
        std::sort(com.rbegin(), com.rend());
        std::fprintf(f, "Direct3D 7 COM calls by Randy:\n");
        for (auto& [n, i] : com)
            std::fprintf(f, "  %6u  %s\n", n, rvkproxy::ComMethodName(i));
        std::fprintf(f, "Direct3D 7 COM calls since start (incl. initialisation):\n");
        for (unsigned i = 0; i < rvkproxy::ComMethodCount(); ++i)
            if (uint32_t n = rvkproxy::ComCallCount(i))
                std::fprintf(f, "  %6u  %s\n", n, rvkproxy::ComMethodName(i));
    }
    std::fflush(f);
    g_events.clear();
    g_counts.clear();
}

void Record(const CallFrame& f)
{
    unsigned i = f.index;
    std::string module = ModuleOf(f.ret);
    ++g_counts[std::string(kExports[i].name) + " <- " + module];
    if (IsDraw(i)) {
        if (g_runCount && (module != g_runModule || g_runDraw != kExports[i].name))
            FlushDrawRun();
        g_runModule = module;
        g_runDraw = kExports[i].name;
        ++g_runCount;
        return;
    }
    if (!IsStructural(i))
        return;
    FlushDrawRun();
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s(0x%X, 0x%X, 0x%X, 0x%X, 0x%X) this=%08X from %s", kExports[i].name,
                  f.args[0], f.args[1], f.args[2], f.args[3], f.args[4], f.ecx, module.c_str());
    g_events.push_back(buf);
    if (static_cast<int>(i) == g_viewportRender || static_cast<int>(i) == g_viewportRefraction)
        g_events.push_back(DescribeLists(f));
}

void Init()
{
    g_orig = GetModuleHandleA("randy31_orig.dll");
    if (!g_orig)
        g_orig = LoadLibraryA("randy31_orig.dll");
    for (unsigned i = 0; i < kExportCount; ++i) {
        if (!std::strcmp(kExports[i].name, "Randy_t::Flip")) g_flip = i;
        if (!std::strcmp(kExports[i].name, "RViewPort_t::Render")) g_viewportRender = i;
        if (!std::strcmp(kExports[i].name, "RViewPort_t::RenderRefraction")) g_viewportRefraction = i;
    }
    g_lastAuto = GetTickCount();
    char frame[16] = "";
    if (GetEnvironmentVariableA("RANDYVK_CAPTURE_FRAME", frame, sizeof(frame)))
        g_captureFrame = unsigned(std::atoi(frame));
    if (g_orig)
        rvkproxy::InstallDDrawHooks(g_orig);
}

}  // namespace

extern "C" {

void* g_traceTargets[kExportCount];

void __cdecl trace_on_call(CallFrame* f)
{
    static bool initialized = (Init(), true);
    (void)initialized;
    if (!g_traceTargets[f->index])
        g_traceTargets[f->index] = reinterpret_cast<void*>(GetProcAddress(g_orig, kExports[f->index].mangled));

    bool flip = static_cast<int>(f->index) == g_flip;
    if (g_capture == Capture::Recording && GetCurrentThreadId() == g_captureThread) {
        Record(*f);
        if (flip) {
            EndCapture();
            g_capture = Capture::Idle;
        }
    }
    if (!flip)
        return;

    // Frame boundary: the frame after this Flip is the next one to record.
    ++g_frame;
    bool hotkey = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) &&
                  (GetAsyncKeyState(VK_F12) & 1);
    DWORD now = GetTickCount();
    bool requested = g_captureFrame >= 2 && g_frame == g_captureFrame - 1;
    if (g_capture == Capture::Idle && (hotkey || requested || now - g_lastAuto > 60000)) {
        g_lastAuto = now;
        g_capture = Capture::Recording;
        g_captureThread = GetCurrentThreadId();
        g_comStart.assign(rvkproxy::ComMethodCount(), 0);
        for (unsigned i = 0; i < g_comStart.size(); ++i)
            g_comStart[i] = rvkproxy::ComCallCount(i);
    }
}

__declspec(naked) void trace_common()
{
    __asm {
        pushad
        push esp
        call trace_on_call
        add esp, 4
        popad
        xchg eax, [esp]                         // eax = export index, [esp] = caller's eax
        mov eax, dword ptr g_traceTargets[eax * 4]
        xchg eax, [esp]                         // restore eax, [esp] = original export
        ret                                     // jump there; stack is as the caller left it
    }
}

#define TRACE_EXPORT(i, mangled, name) \
    __declspec(naked) void trace_thunk_##i() { __asm { push i } __asm { jmp trace_common } }
#include "trace_table.inc"
#undef TRACE_EXPORT

}  // extern "C"
