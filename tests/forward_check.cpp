// Loads the proxy randy31.dll from the client folder and checks that every export resolves to the
// same address as in randy31_orig.dll. Run from Wine: wine forward_check.exe C:\path\to\client
#include <windows.h>
#include <cstdio>
#include <string>

int main(int argc, char** argv)
{
    std::string dir = argc > 1 ? argv[1] : ".";
    HMODULE proxy = LoadLibraryExA((dir + "\\randy31.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    // Forwarder targets load on first lookup, not with the proxy itself.
    if (proxy)
        GetProcAddress(proxy, "??0BVolume_t@@QAE@XZ");
    HMODULE orig = GetModuleHandleA("randy31_orig.dll");
    if (!proxy || !orig) {
        std::printf("load failed: proxy=%p orig=%p err=%lu\n", (void*)proxy, (void*)orig, GetLastError());
        return 1;
    }
    auto* base = reinterpret_cast<BYTE*>(proxy);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    auto& dirent = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    auto* exp = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(base + dirent.VirtualAddress);
    auto* names = reinterpret_cast<DWORD*>(base + exp->AddressOfNames);
    int ok = 0, bad = 0;
    for (DWORD i = 0; i < exp->NumberOfNames; ++i) {
        const char* name = reinterpret_cast<const char*>(base + names[i]);
        FARPROC a = GetProcAddress(proxy, name), b = GetProcAddress(orig, name);
        if (a && a == b) ++ok;
        else { ++bad; std::printf("MISMATCH %s %p %p\n", name, (void*)a, (void*)b); }
    }
    std::printf("proxy=%p orig=%p exports ok=%d bad=%d\n", (void*)proxy, (void*)orig, ok, bad);
    return bad != 0;
}
