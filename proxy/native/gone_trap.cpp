// RANDYVK_GONE_TRAP: an int3 over the first byte of every function port-status calls unreachable (and the ledger's
// dead ones). A vectored handler logs the first run of each, puts the byte back and lets it go on - so a wrong
// "unreachable" shows up in randy-vk.log instead of being trusted. For tests only (the harness, a game session).
#include "native/gone_trap.h"

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_map>

namespace rnative::gonetrap {

namespace {

uintptr_t g_base;
std::unordered_map<uintptr_t, uint8_t>* g_saved;    // trapped address -> its original byte
std::mutex g_lock;

void Put(uintptr_t at, uint8_t byte)
{
    DWORD old;
    VirtualProtect(reinterpret_cast<void*>(at), 1, PAGE_EXECUTE_READWRITE, &old);
    *reinterpret_cast<volatile uint8_t*>(at) = byte;
    VirtualProtect(reinterpret_cast<void*>(at), 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), 1);
}

LONG CALLBACK Trapped(EXCEPTION_POINTERS* e)
{
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT) return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t at = reinterpret_cast<uintptr_t>(e->ExceptionRecord->ExceptionAddress);
    std::lock_guard<std::mutex> hold(g_lock);
    auto it = g_saved->find(at);
    if (it == g_saved->end()) return EXCEPTION_CONTINUE_SEARCH;
    Put(at, it->second);
    g_saved->erase(it);
    const uintptr_t caller = *reinterpret_cast<uintptr_t*>(e->ContextRecord->Esp);
    Log("gone function ran: FUN_%08lx (rva 0x%lx), called from rva 0x%lx", (unsigned long)(0x10000000 + at - g_base),
        (unsigned long)(at - g_base), (unsigned long)(caller - g_base));
    char out[MAX_PATH] = "";                        // RANDYVK_GONE_TRAP_OUT: appended to as well (runs add up)
    if (GetEnvironmentVariableA("RANDYVK_GONE_TRAP_OUT", out, sizeof(out)))
        if (FILE* f = std::fopen(out, "a")) {
            std::fprintf(f, "%lx %lx\n", (unsigned long)(at - g_base), (unsigned long)(caller - g_base));
            std::fclose(f);
        }
    e->ContextRecord->Eip = DWORD(at);
    return EXCEPTION_CONTINUE_EXECUTION;
}

}  // namespace

void Install(HMODULE orig)
{
    char path[MAX_PATH] = "";
    if (!GetEnvironmentVariableA("RANDYVK_GONE_TRAP", path, sizeof(path))) return;
    FILE* f = std::fopen(path, "r");
    if (!f) {
        Log("gone trap: can't read %s", path);
        return;
    }
    g_base = reinterpret_cast<uintptr_t>(orig);
    g_saved = new std::unordered_map<uintptr_t, uint8_t>;
    char line[64];
    while (std::fgets(line, sizeof(line), f)) {
        const unsigned long rva = std::strtoul(line, nullptr, 16);
        if (!rva) continue;
        const uintptr_t at = g_base + rva;
        const uint8_t byte = *reinterpret_cast<const uint8_t*>(at);
        if (byte == 0xE9 || byte == 0xCC) continue;     // replaced (our jump), or padding
        (*g_saved)[at] = byte;
    }
    std::fclose(f);
    AddVectoredExceptionHandler(1, Trapped);
    for (const auto& [at, byte] : *g_saved) Put(at, 0xCC);
    Log("gone trap: %zu functions trapped", g_saved->size());
}

}  // namespace rnative::gonetrap
