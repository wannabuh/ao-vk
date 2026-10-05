// The Funcom FC_* / SL_* library classes linked into Randy (randy-vk.ini [Native] Scene=on): the config base
// (FC_Base_t: its destructor, deleting destructor, its name string and writing itself to a variable) and the stream
// classes (SL_Chunk_t / SL_StreamControl_t: vtables and destructors), a Funcom hash constant, a "Container" name and
// a 4x4 matrix multiply. Nothing here draws; the scene reader (FAF) uses them.
#include "native/funcom.h"

#include "native/vc10.h"

#include <cstring>

namespace rnative::funcom {

namespace {

HMODULE g_orig;

template <typename T>
T& Field(void* object, uint32_t offset) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(object) + offset); }
void* At(uint32_t rva) { return reinterpret_cast<uint8_t*>(g_orig) + rva; }
template <typename F>
F Internal(uint32_t rva) { return reinterpret_cast<F>(AddressFor(rva)); }

// A virtual call: `obj`'s vtable entry at byte `offset`, with `obj` as this and one stack argument.
void* Virtual(void* obj, uint32_t offset, void* arg)
{
    using Fn = void*(__fastcall*)(void*, void*, void*);
    return reinterpret_cast<Fn>((*static_cast<void***>(obj))[offset / 4])(obj, nullptr, arg);
}

// FUN_1006e302: a 4x4 matrix multiply, `out = this->matrix * other`. The sums are the original's x87 order (the
// products and each addition in double, as the FPU's precision control rounds them).
float* __fastcall MatrixMul(void* self, void*, float* out, const float* other)
{
    float* result = out;
    const float* row = static_cast<const float*>(self) + 2;   // this + 8, reading back to this + 0
    for (int i = 0; i < 4; ++i, row += 4) {
        const float* o = other + 8;
        for (int j = 0; j < 4; ++j, ++o, ++out) {
            const float a = o[-8], b = o[-4], c = o[0], d = o[4];
            *out = float(double(d) * double(row[1]) + double(c) * double(row[0]) + double(b) * double(row[-1])
                         + double(a) * double(row[-2]));
        }
    }
    return result;
}

uint32_t __cdecl Hash() { return 0x5A4AA89; }                     // FUN_1006f058
void __stdcall CopyContainer(void* out)                           // FUN_1006f05e (one argument, it cleans)
{
    std::memcpy(out, At(0x9B178), 10);                             // "Container\0"
}
void __fastcall SetStreamControl(void* p, void*) { Field<uintptr_t>(p, 0) = reinterpret_cast<uintptr_t>(At(0x9B188)); }
void* __fastcall StreamControlDelete(void* p, void*, uint8_t flags)   // FUN_10070927
{
    Field<uintptr_t>(p, 0) = reinterpret_cast<uintptr_t>(At(0x9B188));
    if (flags & 1) vc10::Free(p);
    return p;
}

// ---- FC_Base_t (its name string at +0x1C) ----

void __fastcall FCBaseSetName(void* self, void*, const char* name)   // FUN_10071833
{
    if (void* old = Field<void*>(self, 0x1C)) vc10::Free(old);
    char* copy = static_cast<char*>(vc10::AllocateArray(std::strlen(name) + 1));
    Field<void*>(self, 0x1C) = copy;
    std::strcpy(copy, name);
}
void* __fastcall FCBaseGetName(void* self, void*)   // FUN_10071873
{
    void* name = Field<void*>(self, 0x1C);
    return name ? name : At(0x8A700);                // "none"
}
void __fastcall ChunkDestroy(void* self, void*);     // FUN_10072f58, below

void __fastcall FCBaseDestroy(void* self, void*)   // FUN_10071814
{
    Field<uintptr_t>(self, 0) = reinterpret_cast<uintptr_t>(At(0x9B14C));
    if (void* name = Field<void*>(self, 0x1C)) vc10::Free(name);
    ChunkDestroy(self, nullptr);
}
void* __fastcall FCBaseDelete(void* self, void*, uint8_t flags)   // FUN_1007386d
{
    FCBaseDestroy(self, nullptr);
    if (flags & 1) vc10::Free(self);
    return self;
}

// FUN_100718bf: FC_Base_t writes itself into a variable; the variable's byte at +0x1D chooses the terse or the
// document form (SL_Variable_t / SL_EndLine_t on the stack, their vtable calls as the original makes them).
void __fastcall FCBaseWriteTo(void* self, void*, void* arg)   // FUN_100718bf
{
    uint8_t* var = static_cast<uint8_t*>(arg);
    if (var[0x1D] == 0) {
        uint8_t name[0x1C];
        Virtual(var, 0x60, name);
        FCBaseSetName(self, nullptr, reinterpret_cast<const char*>(name));
        return;
    }
    uint8_t variable[8];
    *reinterpret_cast<uintptr_t*>(variable + 0) = reinterpret_cast<uintptr_t>(At(0x9B1A0));
    *reinterpret_cast<uintptr_t*>(variable + 4) = reinterpret_cast<uintptr_t>(At(0x9B3D4));
    void* obj = Virtual(var, 0x0C, variable);
    void* obj2 = Virtual(obj, 0x24, FCBaseGetName(self, nullptr));
    uint8_t endLine[4];
    *reinterpret_cast<uintptr_t*>(endLine) = reinterpret_cast<uintptr_t>(At(0x9B194));
    Virtual(obj2, 0x0C, endLine);
}

// ---- SL_Chunk_t (a list of objects at +0xC, each freed through its own vtable) ----

void __fastcall ChunkDestroy(void* self, void*)   // FUN_10072f58
{
    uint8_t* node = Field<uint8_t*>(self, 0x0C);
    Field<uintptr_t>(self, 0) = reinterpret_cast<uintptr_t>(At(0x9B794));
    while (node) {
        uint8_t* next = Field<uint8_t*>(node, 8);
        using Delete = void*(__fastcall*)(void*, void*, uint8_t);
        void** vtable = *reinterpret_cast<void***>(node);
        reinterpret_cast<Delete>(vtable[0])(node, nullptr, 1);
        node = next;
    }
}
void* __fastcall ChunkDelete(void* self, void*, uint8_t flags)   // FUN_1007809f
{
    ChunkDestroy(self, nullptr);
    if (flags & 1) vc10::Free(self);
    return self;
}

void __cdecl NoOp() {}                                            // FUN_1007194f
uint32_t __cdecl ReturnZero() { return 0; }                       // FUN_10072247
uint32_t __cdecl ReturnOne() { return 1; }                        // FUN_100756f2
void __stdcall ClearByte(uint8_t* p) { *p = 0; }                  // FUN_10074f95 (one argument, it cleans)

}  // namespace

void Install(HMODULE orig)
{
    if (GetMode("Scene", Mode::Off) != Mode::On) return;
    g_orig = orig;
    struct Entry {
        uint32_t rva;
        void* target;
        const char* what;
    };
#define FN(f) reinterpret_cast<void*>(&f)
    const Entry entries[] = {
        {0x6E302, FN(MatrixMul), "Funcom 4x4 matrix multiply (FUN_1006e302)"},
        {0x6F058, FN(Hash), "Funcom hash constant (FUN_1006f058)"},
        {0x6F05E, FN(CopyContainer), "Funcom \"Container\" name (FUN_1006f05e)"},
        {0x7080F, FN(SetStreamControl), "SL_StreamControl_t vtable (FUN_1007080f)"},
        {0x70927, FN(StreamControlDelete), "SL_StreamControl_t deleting destructor (FUN_10070927)"},
        {0x71814, FN(FCBaseDestroy), "FC_Base_t::~FC_Base_t (FUN_10071814)"},
        {0x71833, FN(FCBaseSetName), "FC_Base_t name (FUN_10071833)"},
        {0x71873, FN(FCBaseGetName), "FC_Base_t name read (FUN_10071873)"},
        {0x718BF, FN(FCBaseWriteTo), "FC_Base_t write to a variable (FUN_100718bf)"},
        {0x7194F, FN(NoOp), "Funcom no-op (FUN_1007194f)"},
        {0x72247, FN(ReturnZero), "Funcom returns 0 (FUN_10072247)"},
        {0x72F58, FN(ChunkDestroy), "SL_Chunk_t destructor (FUN_10072f58)"},
        {0x7386D, FN(FCBaseDelete), "FC_Base_t deleting destructor (FUN_1007386d)"},
        {0x74F95, FN(ClearByte), "Funcom clears a byte (FUN_10074f95)"},
        {0x756F2, FN(ReturnOne), "Funcom returns 1 (FUN_100756f2)"},
        {0x7809F, FN(ChunkDelete), "SL_Chunk_t deleting destructor (FUN_1007809f)"},
    };
#undef FN
    int installed = 0;
    for (const Entry& e : entries) installed += Replace(orig, e.rva, e.target, e.what) ? 1 : 0;
    Log("funcom: %d of %d functions native", installed, int(sizeof(entries) / sizeof(entries[0])));
}

}  // namespace rnative::funcom
