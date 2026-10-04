// Living inside the original's objects: its heap (msvcr100.dll - DisplaySystem and Randy allocate and free there, so
// memory that crosses into their hands must come from it too) and its containers' layouts (Visual Studio 2010:
// std::vector = first, last, end; std::string = 16-byte buffer or pointer, size, capacity). Our own STL differs, so
// these fields are handled by the helpers here.
#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

namespace rnative::vc10 {

// msvcr100's operator new / delete / new[] / delete[].
void* Allocate(size_t bytes);
void Free(void* p);
void* AllocateArray(size_t bytes);
void FreeArray(void* p);

// std::vector<T> as VS2010 lays it out (release build: no proxy).
template <typename T>
struct Vector {
    T* first;
    T* last;
    T* end;
    size_t size() const { return size_t(last - first); }
    size_t capacity() const { return size_t(end - first); }
    T& operator[](size_t i) { return first[i]; }
    const T& operator[](size_t i) const { return first[i]; }
    T* begin() { return first; }
    T* finish() { return last; }
    // Trivially copyable elements only (the vectors handled here hold pointers and plain records).
    void reserve(size_t n)
    {
        if (n <= capacity()) return;
        size_t count = size();
        T* p = static_cast<T*>(Allocate(n * sizeof(T)));
        if (count) std::memcpy(p, first, count * sizeof(T));
        if (first) Free(first);
        first = p;
        last = p + count;
        end = p + n;
    }
    void push_back(const T& v)
    {
        if (last == end) reserve(capacity() ? capacity() + capacity() / 2 + 1 : 4);   // VS2010 grows by half
        std::memcpy(static_cast<void*>(last), &v, sizeof(T));
        ++last;
    }
    void erase(T* at)
    {
        std::memmove(static_cast<void*>(at), at + 1, size_t(last - at - 1) * sizeof(T));
        --last;
    }
    void clear() { last = first; }
    void release()
    {
        if (first) Free(first);
        first = last = end = nullptr;
    }
};
static_assert(sizeof(Vector<int>) == 12, "VS2010 std::vector");

// std::string as VS2010 lays it out.
struct String {
    union {
        char buffer[16];
        char* pointer;
    };
    uint32_t size;
    uint32_t capacity;           // 15: in the buffer
    uint32_t allocator;          // (the empty allocator, padded)
    const char* c_str() const { return capacity > 15 ? pointer : buffer; }
    void init() { size = 0; capacity = 15; buffer[0] = 0; }
    void assign(const char* s, size_t n);
    void release();
};
static_assert(sizeof(String) == 0x1C, "VS2010 std::string");

// A copy of a vtable (`slots` entries, with the RTTI locator before it) whose entries can be replaced; never freed.
void** CloneVtable(const void* const* vtable, size_t slots);

}  // namespace rnative::vc10
