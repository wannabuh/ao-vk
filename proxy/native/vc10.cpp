// msvcr100's heap and VS2010 container helpers (vc10.h).
#include "native/vc10.h"

namespace rnative::vc10 {

namespace {

using NewFn = void*(__cdecl*)(size_t);
using DeleteFn = void(__cdecl*)(void*);

struct Heap {
    NewFn allocate = nullptr, allocateArray = nullptr;
    DeleteFn free = nullptr, freeArray = nullptr;
    Heap()
    {
        HMODULE crt = GetModuleHandleA("msvcr100.dll");
        if (!crt) crt = LoadLibraryA("msvcr100.dll");
        if (crt) {
            allocate = reinterpret_cast<NewFn>(GetProcAddress(crt, "??2@YAPAXI@Z"));
            allocateArray = reinterpret_cast<NewFn>(GetProcAddress(crt, "??_U@YAPAXI@Z"));
            free = reinterpret_cast<DeleteFn>(GetProcAddress(crt, "??3@YAXPAX@Z"));
            freeArray = reinterpret_cast<DeleteFn>(GetProcAddress(crt, "??_V@YAXPAX@Z"));
        }
    }
};

const Heap& TheHeap()
{
    static Heap heap;
    return heap;
}

}  // namespace

void* Allocate(size_t bytes)
{
    void* p = TheHeap().allocate(bytes);
    if (!p) throw std::bad_alloc();
    return p;
}

void Free(void* p)
{
    if (p) TheHeap().free(p);
}

void* AllocateArray(size_t bytes)
{
    void* p = TheHeap().allocateArray(bytes);
    if (!p) throw std::bad_alloc();
    return p;
}

void FreeArray(void* p)
{
    if (p) TheHeap().freeArray(p);
}

void String::assign(const char* s, size_t n)
{
    release();
    if (n <= 15) {
        std::memcpy(buffer, s, n);
        buffer[n] = 0;
        capacity = 15;
    } else {
        pointer = static_cast<char*>(Allocate(n + 1));
        std::memcpy(pointer, s, n);
        pointer[n] = 0;
        capacity = uint32_t(n);
    }
    size = uint32_t(n);
}

void String::release()
{
    if (capacity > 15) Free(pointer);
    init();
}

void** CloneVtable(const void* const* vtable, size_t slots)
{
    auto** copy = static_cast<void**>(VirtualAlloc(nullptr, (slots + 1) * sizeof(void*), MEM_COMMIT | MEM_RESERVE,
                                                   PAGE_READWRITE));
    if (!copy) return nullptr;
    std::memcpy(copy, vtable - 1, (slots + 1) * sizeof(void*));   // the RTTI locator, then the slots
    return copy + 1;
}

}  // namespace rnative::vc10
