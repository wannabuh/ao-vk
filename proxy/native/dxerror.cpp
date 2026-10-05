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

void Install(HMODULE orig)
{
    Replace(orig, 0x1E51A, reinterpret_cast<void*>(&GetErrorString), "fun::DXError::GetErrorString");
}

}  // namespace rnative::dxerror
