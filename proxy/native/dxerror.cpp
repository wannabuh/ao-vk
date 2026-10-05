// fun::DXError::GetErrorString natively: Randy's render_t throws a DXError when a D3D call fails, DisplaySystem
// catches it and asks for the text - the message, then the HRESULT's name and description from Randy's table
// (FUN_1001d619, a 169-value switch: dxerror.gen.h).
//
// DXError (std::exception, 0x4C bytes): +0x0C HRESULT, +0x10 std::string message, +0x2C std::string source file,
// +0x48 line.
#include "native/dxerror.h"
#include "native/dxerror.gen.h"
#include "native/vc10.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace rnative::dxerror {

namespace {

HMODULE g_orig;

template <typename T>
T& Field(void* object, uint32_t offset) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(object) + offset); }
void* At(uint32_t rva) { return reinterpret_cast<uint8_t*>(g_orig) + rva; }

// the original's static DXError (vtable in its .rdata) and msvcr100's std::exception it derives from
constexpr uint32_t kDXErrorVtable = 0x9296C;
void ExceptionCtor(void* self)
{
    static const auto ctor = reinterpret_cast<void(__fastcall*)(void*, void*)>(
        GetProcAddress(GetModuleHandleA("msvcr100.dll"), "??0exception@std@@QAE@XZ"));
    ctor(self, nullptr);
}
void ExceptionDtor(void* self)
{
    static const auto dtor = reinterpret_cast<void(__fastcall*)(void*, void*)>(
        GetProcAddress(GetModuleHandleA("msvcr100.dll"), "??1exception@std@@UAE@XZ"));
    dtor(self, nullptr);
}

void Append(vc10::String& s, const char* text)
{
    const size_t n = std::strlen(text);
    char stack[512];
    char* joined = s.size + n < sizeof(stack) ? stack : static_cast<char*>(vc10::Allocate(s.size + n + 1));
    std::memcpy(joined, s.c_str(), s.size);
    std::memcpy(joined + s.size, text, n + 1);
    s.assign(joined, s.size + n);
    if (joined != stack) vc10::Free(joined);
}

}  // namespace

vc10::String* __fastcall GetErrorString(const uint8_t* error, void*, vc10::String* out)
{
    const auto& message = *reinterpret_cast<const vc10::String*>(error + 0x10);
    out->init();
    out->assign(message.c_str(), message.size);
    Append(*out, "\r\n");
    const int32_t hr = *reinterpret_cast<const int32_t*>(error + 0x0C);
    const char *description, *name;
    if (Describe(hr, &description, &name)) {
        Append(*out, name);
        Append(*out, ":");
        Append(*out, description);
    } else {
        char text[32];
        std::sprintf(text, "0x%08X: unknown", unsigned(hr));
        Append(*out, text);
    }
    return out;
}

bool Describe(int32_t hr, const char** description, const char** name)
{
    const uint32_t v = uint32_t(hr);
    const Entry* e = std::lower_bound(std::begin(kErrors), std::end(kErrors), v,
                                      [](const Entry& a, uint32_t b) { return a.hr < b; });
    if (e == std::end(kErrors) || e->hr != v) return false;
    if (description) *description = e->description;
    if (name) *name = e->name;
    return true;
}

// fun::DXError's members (DisplaySystem imports them): the constructor / destructor and the three accessors.
void* __fastcall Construct(void* self, void*, int32_t hr, const vc10::String* message, const vc10::String* file,
                           int line)
{
    ExceptionCtor(self);
    *reinterpret_cast<uintptr_t*>(self) = reinterpret_cast<uintptr_t>(At(kDXErrorVtable));
    vc10::String& text = Field<vc10::String>(self, 0x10);
    vc10::String& source = Field<vc10::String>(self, 0x2C);
    text.init();
    source.init();
    Field<int32_t>(self, 0x0C) = hr;
    text.assign(message->c_str(), message->size);
    source.assign(file->c_str(), file->size);
    Field<int32_t>(self, 0x48) = line;
    return self;
}

void __fastcall Destroy(void* self, void*)
{
    *reinterpret_cast<uintptr_t*>(self) = reinterpret_cast<uintptr_t>(At(kDXErrorVtable));
    Field<vc10::String>(self, 0x2C).release();
    Field<vc10::String>(self, 0x10).release();
    ExceptionDtor(self);
}

int32_t __fastcall GetErrorNo(void* self, void*) { return Field<int32_t>(self, 0x0C); }
int32_t __fastcall GetLineNo(void* self, void*) { return Field<int32_t>(self, 0x48); }
void* __fastcall GetFilename(void* self, void*) { return static_cast<uint8_t*>(self) + 0x2C; }

[[noreturn]] void Throw(int32_t hr, const char* message, const char* file, int line)
{
    vc10::String text, where;
    text.init();
    where.init();
    text.assign(message, std::strlen(message));
    where.assign(file, std::strlen(file));
    alignas(8) uint8_t error[0x4C];
    Construct(error, nullptr, hr, &text, &where, line);
    text.release();
    where.release();
    using ThrowFn = void(__stdcall*)(void*, void*);
    static const auto cxxThrow =
        reinterpret_cast<ThrowFn>(GetProcAddress(GetModuleHandleA("msvcr100.dll"), "_CxxThrowException"));
    cxxThrow(error, reinterpret_cast<uint8_t*>(g_orig) + 0xA45D4);
    __builtin_unreachable();
}

void Install(HMODULE orig)
{
    g_orig = orig;
    struct Entry {
        uint32_t rva;
        void* target;
        const char* what;
    };
    const Entry entries[] = {
        {0x1E51A, reinterpret_cast<void*>(&GetErrorString), "fun::DXError::GetErrorString"},
        {0x1E4A3, reinterpret_cast<void*>(&Construct), "fun::DXError::DXError (FUN_1001e4a3)"},
        {0x1E479, reinterpret_cast<void*>(&Destroy), "fun::DXError::~DXError"},
        {0x1D615, reinterpret_cast<void*>(&GetErrorNo), "fun::DXError::GetErrorNo"},
        {0x1D611, reinterpret_cast<void*>(&GetLineNo), "fun::DXError::GetLineNo"},
        {0x1D60D, reinterpret_cast<void*>(&GetFilename), "fun::DXError::GetFilename"},
    };
    int installed = 0;
    for (const Entry& e : entries) installed += Replace(orig, e.rva, e.target, e.what) ? 1 : 0;
    Log("DXError: %d of %d functions native", installed, int(sizeof(entries) / sizeof(entries[0])));
}

}  // namespace rnative::dxerror
