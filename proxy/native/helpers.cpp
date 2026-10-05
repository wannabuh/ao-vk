// Small shared helpers (randy-vk.ini [Native] Scene=on). CrossProduct / ScaleVector / CrossProductTo are the Vector3
// operations the mesh and camera code use; Identity is a TMatrix4_t set to the identity (used widely, from the shadow
// to the character code); NextNode walks an RRefFrame_t hierarchy (a node's first child at +0x1c, its next sibling at
// +0x18 and its parent at +0x14) until `end`; the two Type* functions are debug-descriptor stubs.
#include "native/helpers.h"

#include <cmath>
#include <cstring>

namespace rnative::helpers {

namespace {

HMODULE g_orig;

template <typename F>
F Internal(uint32_t rva) { return reinterpret_cast<F>(AddressFor(rva)); }

}  // namespace

void SetModule(HMODULE orig) { g_orig = orig; }

// The original's 1/sqrt(v.v) (0x17f2b) returns an x87 float10; it stays the original's, called here so ours matches.
float* __fastcall NormalizeScale(float* self, void*, float scale)
{
    const long double reciprocal = Internal<long double(__fastcall*)(const float*, void*)>(0x17F2B)(self, nullptr);
    const float f = float(reciprocal * (long double)scale);
    self[0] *= f;
    self[1] *= f;
    self[2] *= f;
    return self;
}

void __fastcall NormalizeScaleTo(const float* self, void*, float* out, float scale)
{
    float local[3] = {self[0], self[1], self[2]};
    NormalizeScale(local, nullptr, scale);
    out[0] = local[0];
    out[1] = local[1];
    out[2] = local[2];
}

void __fastcall TranslateAdd(float* self, void*, const float* v)   // a matrix's origin += v (FUN_1002cdb2)
{
    self[12] += v[0];
    self[13] += v[1];
    self[14] += v[2];
}

void __fastcall MatrixMove(float* self, void*, float a, float b)   // FUN_10046518
{
    // In double, as the original's x87 does: a float times a float is exact in double, so only the store rounds.
    self[0] = float(double(a) * self[1] + self[0]);
    self[2] = float(double(b) * self[1] + self[2]);
    self[4] = float(double(a) * self[5] + self[4]);
    self[6] = float(double(b) * self[5] + self[6]);
    self[8] = float(double(a) * self[9] + self[8]);
    self[10] = float(double(b) * self[9] + self[10]);
    self[12] = float(double(a) * self[13] + self[12]);
    self[14] = float(double(b) * self[13] + self[14]);
}

float* __fastcall Subtract(const float* self, void*, float* out, const float* other)   // FUN_10012cf0
{
    out[0] = self[0] - other[0];
    out[1] = self[1] - other[1];
    out[2] = self[2] - other[2];
    return out;
}

void __fastcall CrossProduct(float* self, void*, const float* other)
{
    // In double, as the original's x87 does: a float times a float is exact in double, so the only rounding is the
    // one to the stored float (the same as the original's extended computation).
    const double a0 = self[0], a1 = self[1], a2 = self[2];
    self[0] = float(other[2] * a1 - other[1] * a2);
    self[1] = float(a2 * other[0] - a0 * other[2]);
    self[2] = float(other[1] * a0 - other[0] * a1);
}

float* __fastcall ScaleVector(const float* self, void*, float* out, float scale)
{
    out[0] = self[0] * scale;
    out[1] = self[1] * scale;
    out[2] = self[2] * scale;
    return out;
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
        {0x12CF0, FN(Subtract), "Vector3 subtract (FUN_10012cf0)"},
        {0x18833, FN(NormalizeScale), "Vector3 normalize and scale (FUN_10018833)"},
        {0x18864, FN(NormalizeScaleTo), "Vector3 normalize and scale to (FUN_10018864)"},
        {0x2A358, FN(CrossProduct), "Vector3 cross product (FUN_1002a358)"},
        {0x2CDB2, FN(TranslateAdd), "a matrix's origin + a Vector3 (FUN_1002cdb2)"},
        {0x46518, FN(MatrixMove), "a matrix moved along its axes (FUN_10046518)"},
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
