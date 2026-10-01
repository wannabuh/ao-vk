// Offline proxy check, run under Wine from the client folder: wine forward_check.exe C:\path\to\client
//  1. every proxy export resolves either to the original's address (forwarder) or to a thunk inside
//     the proxy (traced export);
//  2. traced exports behave like the original: a cdecl static (VertexBuffer_c::GetFormatSize) and a
//     thiscall constructor (BVolume_t::BVolume_t) give identical results through the thunk.
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

using GetFormatSizeFn = unsigned(__cdecl*)(unsigned);
using CtorFn = void*(__fastcall*)(void* self, void* edx);   // thiscall: this in ecx, edx unused

bool InModule(HMODULE module, FARPROC p)
{
    auto base = reinterpret_cast<BYTE*>(module);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    auto a = reinterpret_cast<BYTE*>(p);
    return a >= base && a < base + nt->OptionalHeader.SizeOfImage;
}

}  // namespace

int main(int argc, char** argv)
{
    std::string dir = argc > 1 ? argv[1] : ".";
    HMODULE proxy = LoadLibraryExA((dir + "\\randy31.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    // Forwarder targets load on first lookup, not with the proxy itself.
    if (proxy)
        GetProcAddress(proxy, "?Archive@RTexture_t@@UBEXPAVObjectArchive_c@fun@@@Z");
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
    int forwarded = 0, thunked = 0, bad = 0;
    for (DWORD i = 0; i < exp->NumberOfNames; ++i) {
        const char* name = reinterpret_cast<const char*>(base + names[i]);
        FARPROC a = GetProcAddress(proxy, name), b = GetProcAddress(orig, name);
        if (a && a == b) ++forwarded;
        else if (a && b && InModule(proxy, a)) ++thunked;
        else { ++bad; std::printf("MISMATCH %s %p %p\n", name, (void*)a, (void*)b); }
    }
    std::printf("exports: forwarded=%d thunked=%d bad=%d\n", forwarded, thunked, bad);

    const char* gfs = "?GetFormatSize@VertexBuffer_c@@SAII@Z";
    auto viaThunk = reinterpret_cast<GetFormatSizeFn>(GetProcAddress(proxy, gfs));
    auto direct = reinterpret_cast<GetFormatSizeFn>(GetProcAddress(orig, gfs));
    unsigned fvfs[] = {0x002, 0x112, 0x142, 0x152, 0x1C4, 0x252, 0x312};
    for (unsigned fvf : fvfs) {
        unsigned x = viaThunk(fvf), y = direct(fvf);
        if (x != y) { ++bad; std::printf("GetFormatSize(0x%X): thunk %u, original %u\n", fvf, x, y); }
    }
    std::printf("GetFormatSize(0x152) = %u through thunk\n", viaThunk(0x152));

    const char* ctor = "??0BVolume_t@@QAE@XZ";
    alignas(16) unsigned char a[256], b[256];
    std::memset(a, 0xCD, sizeof(a));
    std::memset(b, 0xCD, sizeof(b));
    void* ra = reinterpret_cast<CtorFn>(GetProcAddress(proxy, ctor))(a, nullptr);
    void* rb = reinterpret_cast<CtorFn>(GetProcAddress(orig, ctor))(b, nullptr);
    // Bytes 4..7 come from the fun::Serializable_c base constructor, a running object number
    // (1 for the first object, 2 for the second), so they differ by design.
    std::memset(a + 4, 0, 4);
    std::memset(b + 4, 0, 4);
    bool same = std::memcmp(a, b, sizeof(a)) == 0 && ra == a && rb == b;
    if (!same) {
        ++bad;
        std::printf("  returns: thunk %p (buf %p), original %p (buf %p)\n", ra, (void*)a, rb, (void*)b);
        for (int i = 0; i < 256; ++i)
            if (a[i] != b[i]) std::printf("  byte %d: thunk %02X original %02X\n", i, a[i], b[i]);
    }
    std::printf("BVolume_t ctor through thunk: %s\n", same ? "identical" : "DIFFERENT");

    std::printf("%s\n", bad ? "FAIL" : "OK");
    return bad != 0;
}
