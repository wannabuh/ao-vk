// Small shared helpers (randy-vk.ini [Native] Scene=on). CrossProduct / ScaleVector / CrossProductTo are the Vector3
// operations the mesh and camera code use; Identity is a TMatrix4_t set to the identity (used widely, from the shadow
// to the character code); NextNode walks an RRefFrame_t hierarchy (a node's first child at +0x1c, its next sibling at
// +0x18 and its parent at +0x14) until `end`; the two Type* functions are debug-descriptor stubs.
#include "native/helpers.h"

#include <cstring>

namespace rnative::helpers {

void __fastcall CrossProduct(float* self, void*, const float* other)
{
    // In double, as the original's x87 does: a float times a float is exact in double, so the only rounding is the
    // one to the stored float (the same as the original's extended computation).
    const double a0 = self[0], a1 = self[1], a2 = self[2];
    self[0] = float(other[2] * a1 - other[1] * a2);
    self[1] = float(a2 * other[0] - a0 * other[2]);
    self[2] = float(other[1] * a0 - other[0] * a1);
}

void __fastcall ScaleVector(const float* self, void*, float* out, float scale)
{
    out[0] = self[0] * scale;
    out[1] = self[1] * scale;
    out[2] = self[2] * scale;
}

void __fastcall CrossProductTo(const float* self, void*, float* out, const float* other)
{
    const double a0 = self[0], a1 = self[1], a2 = self[2];
    out[0] = float(other[2] * a1 - other[1] * a2);
    out[1] = float(a2 * other[0] - a0 * other[2]);
    out[2] = float(other[1] * a0 - other[0] * a1);
}

void __fastcall Identity(void* self, void*)
{
    float* m = static_cast<float*>(self);
    std::memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

void __cdecl NextNode(void** node, const void* end)
{
    uint8_t* at = static_cast<uint8_t*>(*node);
    if (!at) return;
    if (uint8_t* child = *reinterpret_cast<uint8_t**>(at + 0x1c)) {
        *node = child;
        return;
    }
    if (at == end) {
        *node = nullptr;
        return;
    }
    for (;;) {
        if (*reinterpret_cast<uint32_t*>(at + 0x18) != 0) {
            *node = *reinterpret_cast<uint8_t**>(at + 0x18);
            return;
        }
        at = *reinterpret_cast<uint8_t**>(at + 0x14);
        *node = at;
        if (at == end) {
            *node = nullptr;
            return;
        }
    }
}

void __stdcall TypeStore(void* descriptor, int32_t value)
{
    *static_cast<uint16_t*>(descriptor) = 3;
    *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(descriptor) + 8) = value;
}

int32_t __cdecl TypeSize() { return 8; }

void Install(HMODULE orig)
{
    if (GetMode("Scene", Mode::Off) != Mode::On) return;
    struct Entry {
        uint32_t rva;
        void* target;
        const char* what;
    };
#define FN(f) reinterpret_cast<void*>(&f)
    const Entry entries[] = {
        {0x1B63B, FN(TypeStore), "a Type* descriptor store (FUN_1001b63b)"},
        {0x1B662, FN(TypeSize), "a Type* descriptor size (FUN_1001b662)"},
        {0x2A358, FN(CrossProduct), "Vector3 cross product (FUN_1002a358)"},
        {0x2A39F, FN(ScaleVector), "Vector3 scale (FUN_1002a39f)"},
        {0x2A3DB, FN(CrossProductTo), "Vector3 cross product to (FUN_1002a3db)"},
        {0x2A406, FN(Identity), "TMatrix4_t identity (FUN_1002a406)"},
        {0x45289, FN(NextNode), "NextNode (the frame hierarchy)"},
    };
#undef FN
    int installed = 0;
    for (const Entry& e : entries) installed += Replace(orig, e.rva, e.target, e.what) ? 1 : 0;
    Log("helpers: %d of %d functions native", installed, int(sizeof(entries) / sizeof(entries[0])));
}

}  // namespace rnative::helpers
