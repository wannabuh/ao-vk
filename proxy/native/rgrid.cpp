// RGrid natively (randy-vk.ini [Native] Scene=on). RGrid is an RVisual_t (0x19C bytes) that draws a centred grid of
// lines: Build makes the vertex list (position + a diffuse colour, 0x10 bytes each) and the index list (uint16 pairs)
// from hcellcnt / vcellcnt / cellwidth / cellheight, SetColor packs one RGB_t into every vertex, and a BVolume_t bounds
// them. Render sets the world transform, disables z / lighting, draws the line list and puts the states back. Its own
// functions here; the RVisual_t base and the archive reader are already native.
#include "native/rgrid.h"

#include "native/color.h"
#include "native/orig_api.gen.h"
#include "native/serialize.h"
#include "native/vc10.h"

#include <cstring>

namespace rnative::rgrid {

namespace {

HMODULE g_orig;
void* const* g_renderInstance;                       // render_t::m_pcInstance: -> the render_t

template <typename F>
F Internal(uint32_t rva) { return reinterpret_cast<F>(reinterpret_cast<uint8_t*>(g_orig) + rva); }
template <typename F>
F Export(const char* name) { return reinterpret_cast<F>(GetProcAddress(g_orig, name)); }
template <typename T>
T& Field(void* object, uint32_t offset) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(object) + offset); }
void SetVtable(void* o, uint32_t rva) { Field<uintptr_t>(o, 0) = reinterpret_cast<uintptr_t>(g_orig) + rva; }
void* Renderer() { return *g_renderInstance; }
const serialize::Api& S() { return serialize::Get(); }

// RGrid: RVisual_t (0x178), then its grid, its built geometry and its bounds.
constexpr uint32_t kVtable = 0x8A544, kSubjectVtable = 0x8A530, kBVolumeVtable = 0x8A5CC;
constexpr uint32_t kHCells = 0x178, kVCells = 0x17C, kCellWidth = 0x180, kCellHeight = 0x184;
constexpr uint32_t kLineCount = 0x188, kIndexCount = 0x18C, kIndices = 0x190, kVertices = 0x194, kBounds = 0x198;
constexpr uint32_t kSize = 0x19C;

// RGrid::SetColor(RGB_t const&) - FUN_100125d7. The original builds the colour with the shared Color_t(r,g,b,a) helper
// at 0x12332 (alpha 0.5) and writes it into every vertex at +0xC; our Color_t::Init does the same packing.
void __fastcall SetColor(void* self, void*, const float* rgb)
{
    color::Color packed;
    color::InitF(&packed, nullptr, rgb[0], rgb[1], rgb[2], 0.5f);
    uint32_t value;
    std::memcpy(&value, &packed, sizeof(value));
    uint8_t* vertices = Field<uint8_t*>(self, kVertices);
    const uint32_t count = Field<uint32_t>(self, kLineCount);
    for (uint32_t i = 0; i < count; ++i)
        *reinterpret_cast<uint32_t*>(vertices + size_t(i) * 0x10 + 0xC) = value;
}

// FUN_10012ac1: a BVolume_t from points (the same body as BVolume_t::BVolume_t(points), FUN_10017fee).
void* __fastcall BoundsFrom(void* self, void*, const float* points, uint32_t count)
{
    S().construct(self, nullptr);
    SetVtable(self, kBVolumeVtable);
    std::memset(static_cast<uint8_t*>(self) + 0x08, 0, 12);    // the centre
    std::memset(static_cast<uint8_t*>(self) + 0x18, 0, 24);    // the box
    Internal<void(__fastcall*)(void*, void*, const float*, uint32_t, uint32_t)>(0x29E51)(self, nullptr, points, count,
                                                                                        0x10);   // BVolume_t::Init
    return self;
}

// FUN_10012634: the grid's vertices, indices and bounds. A centred grid: the x lines step from -w/2 to +w/2, the z lines
// from -h/2 to +h/2. The extents are one x87 product (h * cellwidth), rounded once when stored, as the original.
void __fastcall Build(void* self)
{
    const uint32_t h = Field<uint32_t>(self, kHCells);
    const uint32_t v = Field<uint32_t>(self, kVCells);
    const float cellW = Field<float>(self, kCellWidth);
    const float cellH = Field<float>(self, kCellHeight);

    const uint32_t lines = (h + v) * 2 + 4;
    Field<uint32_t>(self, kLineCount) = lines;
    uint8_t* vertices = static_cast<uint8_t*>(vc10::AllocateArray(size_t(lines) * 0x10));
    Field<void*>(self, kVertices) = vertices;
    const uint32_t indexCount = lines >> 1;
    Field<uint32_t>(self, kIndexCount) = indexCount;
    uint16_t* indices = static_cast<uint16_t*>(vc10::AllocateArray(size_t(indexCount) * 4));
    Field<void*>(self, kIndices) = indices;

    const float halfX = float(double(h) * double(cellW) * -0.5);
    const float posX = float(double(h) * double(cellW) * 0.5);
    const float halfZ = float(double(v) * double(cellH) * -0.5);
    const float posZ = float(double(v) * double(cellH) * 0.5);

    float x = halfX;
    for (uint32_t i = 0; i <= h; ++i) {                          // a line at each x, z from halfZ to posZ
        float* a = reinterpret_cast<float*>(vertices + size_t(i) * 0x10);
        a[0] = x; a[1] = 0.0f; a[2] = halfZ;
        float* b = reinterpret_cast<float*>(vertices + size_t(h + 1 + i) * 0x10);
        b[0] = x; b[1] = 0.0f; b[2] = posZ;
        indices[i * 2] = uint16_t(i);
        indices[i * 2 + 1] = uint16_t(h + i + 1);
        x = cellW + x;
    }
    float z = halfZ;
    for (uint32_t c = 0; c <= v; ++c) {                          // a line at each z, x from halfX to posX
        const uint32_t ia = h * 2 + 2 + c, ib = h * 2 + 3 + v + c;
        float* a = reinterpret_cast<float*>(vertices + size_t(ia) * 0x10);
        a[0] = halfX; a[1] = 0.0f; a[2] = z;
        float* b = reinterpret_cast<float*>(vertices + size_t(ib) * 0x10);
        b[0] = posX; b[1] = 0.0f; b[2] = z;
        indices[(h + 1 + c) * 2] = uint16_t(ia);
        indices[(h + 1 + c) * 2 + 1] = uint16_t(ib);
        z = cellH + z;
    }

    const float darkGreen[3] = {0.0f, 0.5f, 0.0f};               // the original's default colour
    SetColor(self, nullptr, darkGreen);

    void* bounds = vc10::Allocate(0x30);
    Field<void*>(self, kBounds) = bounds ? BoundsFrom(bounds, nullptr, reinterpret_cast<const float*>(vertices), lines)
                                         : nullptr;
}

// FUN_1001242a: the scalar destructor (the vertices, the indices, the bounds, then RVisual_t).
void __fastcall Destroy(void* self)
{
    SetVtable(self, kVtable);
    SetVtable(static_cast<uint8_t*>(self) + 0xA4, kSubjectVtable);
    vc10::FreeArray(Field<void*>(self, kVertices));
    vc10::FreeArray(Field<void*>(self, kIndices));
    if (void* bounds = Field<void*>(self, kBounds)) {
        using DeletingDtor = void*(__fastcall*)(void*, void*, uint8_t);
        reinterpret_cast<DeletingDtor>((*static_cast<void***>(bounds))[0])(bounds, nullptr, 1);
    }
    Internal<void(__fastcall*)(void*, void*)>(0x4D7D3)(self, nullptr);   // RVisual_t::~RVisual_t
}

// FUN_10012b74: deleting destructor (vtable slot 0).
void* __fastcall Delete(void* self, void*, uint8_t flags)
{
    Destroy(self);
    if (flags & 1) vc10::Free(self);
    return self;
}

// FUN_10012b69: the SubjectImpl subobject's deleting destructor (its vtable slot 0), which adjusts to the RVisual_t.
void* __fastcall SubjectDelete(void* self, void*, uint8_t flags)
{
    return Delete(static_cast<uint8_t*>(self) - 0xA4, nullptr, flags);
}

// FUN_10012a82: RVisual_t's Clone (vtable slot 3, shared by every visual): a plain 0xA4 RRefFrame_t copy.
void* __fastcall Clone(void* self, void*)
{
    uint8_t* copy = static_cast<uint8_t*>(vc10::Allocate(0xA4));
    if (!copy) return nullptr;
    Export<void*(__fastcall*)(void*, void*, const void*)>("??0RRefFrame_t@@QAE@ABV0@@Z")(copy, nullptr, self);
    return copy;
}

// FUN_10012abc: RVisual_t's default vtable slot 12 (four stack arguments, always false).
bool __fastcall AlwaysFalse(void*, void*, uint32_t, uint32_t, uint32_t, uint32_t) { return false; }

// RGrid::RGrid(hcellcnt, vcellcnt, cellwidth, cellheight, parent, animation) - FUN_100128a1.
void* __fastcall Construct(void* self, void*, uint32_t h, uint32_t v, float cellW, float cellH, void* parent,
                           void* animation)
{
    orig::RVisual_t_RVisual_t_49(self, parent, animation);
    Field<uint32_t>(self, kHCells) = h;
    Field<uint32_t>(self, kVCells) = v;
    Field<float>(self, kCellWidth) = cellW;
    Field<float>(self, kCellHeight) = cellH;
    SetVtable(self, kVtable);
    SetVtable(static_cast<uint8_t*>(self) + 0xA4, kSubjectVtable);
    Build(self);
    orig::RVisual_t_SetRenderPriority(self, 5);
    return self;
}

// FUN_1001298e: RGrid(archive) - the base archive ctor, our vtable, the four fields from the stream, then Build.
void* __fastcall ConstructFrom(void* self, void*, void* archive)
{
    Internal<void*(__fastcall*)(void*, void*, void*)>(0x4D6D3)(self, nullptr, archive);   // RVisual_t(archive)
    SetVtable(self, kVtable);
    SetVtable(static_cast<uint8_t*>(self) + 0xA4, kSubjectVtable);
    void* stream = S().getStream(archive, nullptr);
    int32_t value = 0;
    if (S().findInt32(stream, nullptr, "hcellcnt", &value, 0) == 0) Field<uint32_t>(self, kHCells) = uint32_t(value);
    if (S().findInt32(stream, nullptr, "vcellcnt", &value, 0) == 0) Field<uint32_t>(self, kVCells) = uint32_t(value);
    S().findFloat(stream, nullptr, "cellwidth", &Field<float>(self, kCellWidth), 0);
    S().findFloat(stream, nullptr, "cellheight", &Field<float>(self, kCellHeight), 0);
    Build(self);
    return self;
}

// RGrid::Instantiate(ObjectArchive_c*) - the exported factory (0x12a4a).
void* __cdecl Instantiate(void* archive)
{
    uint8_t* self = static_cast<uint8_t*>(vc10::Allocate(kSize));
    return self ? ConstructFrom(self, nullptr, archive) : nullptr;
}

// RGrid::Archive (vtable slot 1) - FUN_1001248f.
void __fastcall Archive(void* self, void*, void* archive)
{
    orig::RVisual_t_Archive(self, archive);
    void* stream = S().getStream(archive, nullptr);
    S().addInt32(stream, nullptr, "hcellcnt", int32_t(Field<uint32_t>(self, kHCells)));
    S().addInt32(stream, nullptr, "vcellcnt", int32_t(Field<uint32_t>(self, kVCells)));
    S().addFloat(stream, nullptr, "cellwidth", Field<float>(self, kCellWidth));
    S().addFloat(stream, nullptr, "cellheight", Field<float>(self, kCellHeight));
}

// A render state for a scope (FUN_1001237d / FUN_100123c0, native in mesh.cpp): set, and put back when it went through.
struct StateGuard {
    int32_t state;
    uint32_t before;
    bool changed;
};

// RGrid's draw (vtable slot 13) - FUN_10012501: the world transform, z and lighting off, the line list, then back.
void __fastcall Render(void* self, void*, void* viewport)
{
    orig::RViewPort_t_SetMaterial(viewport, nullptr);
    const void* world = orig::RRefFrame_t_GetWorldMatrix(self);
    orig::render_t_SetTransformMatrix(Renderer(), 1, const_cast<void*>(world));   // D3DTRANSFORMSTATE_WORLD
    using GuardOpen = StateGuard*(__fastcall*)(StateGuard*, void*, int32_t, uint32_t, int32_t);
    using GuardClose = void(__fastcall*)(StateGuard*);
    StateGuard zTest, light, lit;
    Internal<GuardOpen>(0x1237D)(&zTest, nullptr, 0x0E, 0, 10);   // D3DRS_ZENABLE off
    Internal<GuardOpen>(0x1237D)(&light, nullptr, 0x88, 1, 10);
    Internal<GuardOpen>(0x1237D)(&lit, nullptr, 0x89, 0, 10);     // D3DRS_LIGHTING off
    orig::RViewPort_t_RealizeRenderStates(viewport);
    orig::render_t_RenderLineList_396(Renderer(), 0x42, Field<void*>(self, kVertices), Field<uint32_t>(self, kLineCount),
                                      Field<void*>(self, kIndices), int32_t(Field<uint32_t>(self, kIndexCount)) * 2, 0);
    orig::RViewPort_t_ResetMaterial(viewport);
    Internal<GuardClose>(0x123C0)(&lit);
    Internal<GuardClose>(0x123C0)(&light);
    Internal<GuardClose>(0x123C0)(&zTest);
}

}  // namespace

void Install(HMODULE orig)
{
    if (GetMode("Scene", Mode::Off) != Mode::On) return;
    g_orig = orig;
    if (!S().complete) return;
    g_renderInstance = reinterpret_cast<void* const*>(GetProcAddress(orig, "?m_pcInstance@render_t@@0PAV1@A"));
    if (!g_renderInstance) return;
    struct Entry {
        uint32_t rva;
        void* target;
        const char* what;
    };
#define FN(f) reinterpret_cast<void*>(&f)
    const Entry entries[] = {
        {0x1242A, FN(Destroy), "RGrid::~RGrid (FUN_1001242a)"},
        {0x1248F, FN(Archive), "RGrid::Archive (FUN_1001248f)"},
        {0x12501, FN(Render), "RGrid::Render (FUN_10012501)"},
        {0x125D7, FN(SetColor), "RGrid::SetColor"},
        {0x12634, FN(Build), "RGrid::Build (FUN_10012634)"},
        {0x128A1, FN(Construct), "RGrid::RGrid"},
        {0x1298E, FN(ConstructFrom), "RGrid::RGrid(archive) (FUN_1001298e)"},
        {0x12A4A, FN(Instantiate), "RGrid::Instantiate"},
        {0x12A82, FN(Clone), "RVisual_t::Clone (FUN_10012a82)"},
        {0x12ABC, FN(AlwaysFalse), "RVisual_t slot 12 default (FUN_10012abc)"},
        {0x12AC1, FN(BoundsFrom), "BVolume_t from points (FUN_10012ac1)"},
        {0x12B69, FN(SubjectDelete), "RGrid subject deleting destructor (FUN_10012b69)"},
        {0x12B74, FN(Delete), "RGrid deleting destructor (FUN_10012b74)"},
    };
#undef FN
    int installed = 0;
    for (const Entry& e : entries) installed += Replace(orig, e.rva, e.target, e.what) ? 1 : 0;
    Log("grid: %d of %d functions native", installed, int(sizeof(entries) / sizeof(entries[0])));
}

}  // namespace rnative::rgrid
