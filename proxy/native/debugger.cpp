// Debugger_t natively (part of [Native] Device=on): Randy's debug drawing. DisplaySystem (and the camera and occluder
// in their debug modes) add lines, screen lines, points and spheres; RViewPort_t::Render draws them at its end and
// they are gone (FUN_1002c082).
//
// Debugger_t (0x20 bytes, one: 0x1017D23C): +0x04 / +0x10 the axes spheres are drawn in, +0x1C the scale of screen y.
// Its lists are globals: lines 0x1017D240 (0x7FFF x 0x24 bytes: from, to, r g b) with their count 0x1017D244, screen
// lines 0x1017D248 / 0x1017D24C (the same; z 1), points 0x1017D250 / 0x1017D254 (0xFFFF x 0x18: position, r g b).
// Debugger_t::m_nDebuggerMode (0x100B7500): 0 draws none of them.
#include "native/debugger.h"
#include "native/orig_api.gen.h"
#include "native/vc10.h"

#include <cstring>
#include <vector>

namespace rnative::debugger {

namespace {

HMODULE g_orig;

template <typename T>
T& Global(uint32_t rva) { return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(g_orig) + rva); }
template <typename T>
T& Field(void* object, uint32_t offset) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(object) + offset); }
template <typename F>
F Internal(uint32_t rva) { return reinterpret_cast<F>(AddressFor(rva)); }

constexpr uint32_t kInstance = 0x17D23C, kLines = 0x17D240, kLineCount = 0x17D244, kScreenLines = 0x17D248,
                   kScreenLineCount = 0x17D24C, kPoints = 0x17D250, kPointCount = 0x17D254, kMode = 0xB7500,
                   kRender = 0x16BED0, kViewMatrix = 0xB79C8;
constexpr uint32_t kMaxLines = 0x7FFF, kMaxPoints = 0xFFFF;

struct Line {
    float from[3], to[3], color[3];
};
static_assert(sizeof(Line) == 0x24, "debug line");
struct Point {
    float position[3], color[3];
};
static_assert(sizeof(Point) == 0x18, "debug point");

void* Render() { return Global<void*>(kRender); }

template <typename T>
T* List(uint32_t at, uint32_t count)                 // made on first use, positions zeroed
{
    T*& list = Global<T*>(at);
    if (!list) {
        list = static_cast<T*>(vc10::AllocateArray(count * sizeof(T)));
        for (uint32_t i = 0; i < count; ++i) std::memset(&list[i], 0, sizeof(T) - 12);
    }
    return list;
}

// msvcr100's sin / cos, as the original's _CIsin / _CIcos (its SSE2 path) - the same results to the bit.
double CrtSin(double x)
{
    static const auto fn = reinterpret_cast<double(__cdecl*)(double)>(GetProcAddress(GetModuleHandleA("msvcr100.dll"), "sin"));
    return fn(x);
}
double CrtCos(double x)
{
    static const auto fn = reinterpret_cast<double(__cdecl*)(double)>(GetProcAddress(GetModuleHandleA("msvcr100.dll"), "cos"));
    return fn(x);
}

// A render state / texture stage state for a while (FUN_1001237d / FUN_1002be44 and their ends).
struct State {
    uint32_t state, old;
    bool set;
    State(uint32_t s, uint32_t value)
        : state(s), old(Field<uint32_t>(DeviceState(), 0x4C8 + s * 4)),
          set(orig::DeviceState_SetRenderState(DeviceState(), int32_t(s), value, 10))
    {
    }
    ~State()
    {
        if (set) orig::DeviceState_SetRenderState(DeviceState(), int32_t(state), old, 2);
    }
    static void* DeviceState() { return Field<void*>(Global<void*>(0x17D2EC), 0x27C); }   // Randy_t's
};
struct StageState {
    uint32_t stage, type, old;
    bool set;
    StageState(uint32_t st, uint32_t t, uint32_t value)
        : stage(st), type(t), old(Field<uint32_t>(State::DeviceState(), 0xD84 + (st * 0x19 + t) * 4)),
          set(orig::DeviceState_SetTextureStageState(State::DeviceState(), st, int32_t(t), value, 10))
    {
    }
    ~StageState()
    {
        if (set) orig::DeviceState_SetTextureStageState(State::DeviceState(), stage, int32_t(type), old, 2);
    }
};

struct Vertex {                                     // D3DFVF_XYZ | D3DFVF_DIFFUSE
    float position[3];
    uint32_t color;
};

uint32_t Color(const float* rgb)
{
    uint32_t c;
    orig::Color_t_Init_296(&c, rgb[0], rgb[1], rgb[2], 1.0f);
    return c;
}

// The lines of a list into the dynamic vertex buffer, indexed 0 1, 2 3, ...: the first vertex and the indices.
uint32_t PutLines(const Line* lines, uint32_t count, std::vector<uint16_t>& indices)
{
    Vertex* v = nullptr;
    const uint32_t first = orig::DynamicVB_c_GetVertices(orig::DynamicVB_c_Get(), 0x42, 0x10, count * 2, &v);
    for (uint32_t i = 0; i < count; ++i) {
        std::memcpy(v[i * 2].position, lines[i].from, 12);
        v[i * 2].color = Color(lines[i].color);
        std::memcpy(v[i * 2 + 1].position, lines[i].to, 12);
        v[i * 2 + 1].color = Color(lines[i].color);
    }
    indices.resize(count * 2);
    for (uint32_t i = 0; i < count * 2; ++i) indices[i] = uint16_t(i);
    return first;
}

void DrawLines(void* viewport, uint32_t first, std::vector<uint16_t>& indices)
{
    orig::RViewPort_t_RealizeRenderStates(viewport);
    void* vb = orig::DynamicVB_c_GetVB(orig::DynamicVB_c_Get(), 0x42);
    const uint32_t n = uint32_t(indices.size());
    orig::render_t_RenderLineList_397(Render(), vb, first, n, indices.data(), n, 0);
}

void ViewSpace(float* m)                            // world = the view matrix inverted (FUN_1006e108)
{
    std::memcpy(m, &Global<float>(kViewMatrix), 64);
    Internal<void(__fastcall*)(float*)>(0x6E108)(m);
}

}  // namespace

// ?Get@Debugger_t@@SAPAV1@XZ: the one Debugger_t, made the first time (FUN_1002beb8: the debugger mode 0x200).
void* __cdecl Get()
{
    void*& instance = Global<void*>(kInstance);
    if (!instance) {
        instance = vc10::Allocate(0x20);
        std::memset(static_cast<uint8_t*>(instance) + 4, 0, 0x18);
        Global<uint32_t>(kMode) = 0x200;
    }
    return instance;
}

// ?AddLine@Debugger_t@@QAEXVVector3_t@@0MMM@Z
void __fastcall AddLine(void*, void*, float fx, float fy, float fz, float tx, float ty, float tz, float r, float g,
                        float b)
{
    Line* lines = List<Line>(kLines, kMaxLines);
    uint32_t& count = Global<uint32_t>(kLineCount);
    if (count >= kMaxLines) return;
    lines[count++] = {{fx, fy, fz}, {tx, ty, tz}, {r, g, b}};
}

// ?Add2DLine@Debugger_t@@QAEXVVector3_t@@0MMM_N@Z: a line on the screen (z 1; y scaled by +0x1C if `scale`).
void __fastcall AddScreenLine(uint8_t* self, void*, float fx, float fy, float, float tx, float ty, float, float r,
                              float g, float b, bool scale)
{
    Line* lines = List<Line>(kScreenLines, kMaxLines);
    uint32_t& count = Global<uint32_t>(kScreenLineCount);
    if (count >= kMaxLines) return;
    Line& l = lines[count++];
    l = {{fx, fy, 1.0f}, {tx, ty, 1.0f}, {r, g, b}};
    if (scale) {
        l.from[1] = Field<float>(self, 0x1C) * l.from[1];
        l.to[1] = Field<float>(self, 0x1C) * l.to[1];
    }
}

// FUN_1002cbaa: a point on the screen (z 1; y scaled by +0x1C if `scale`).
void __fastcall AddPoint(uint8_t* self, void*, float x, float y, float, float r, float g, float b, bool scale)
{
    Point* points = List<Point>(kPoints, kMaxPoints);
    uint32_t& count = Global<uint32_t>(kPointCount);
    if (count >= kMaxPoints) return;
    Point& p = points[count++];
    p = {{x, y, 1.0f}, {r, g, b}};
    if (scale) p.position[1] = Field<float>(self, 0x1C) * p.position[1];
}

// ?AddSphere@Debugger_t@@QAEXVVector3_t@@MMMM@Z: a circle of 32 lines in the plane of the axes at +0x04 and +0x10.
void __fastcall AddSphere(uint8_t* self, void*, float cx, float cy, float cz, float radius, float r, float g, float b)
{
    const float* a = &Field<float>(self, 4);
    const float* c = &Field<float>(self, 0x10);
    float previous[3] = {0.0f, 0.0f, 0.0f};
    for (uint32_t i = 0; i <= 32; ++i) {
        const float angle = float(double(i) * -0.78539816339744828 * 0.25);   // (exact as the x87 does it)
        const float s = float(CrtSin(angle)), co = float(CrtCos(angle));
        float along[3], across[3], p[3];
        for (int k = 0; k < 3; ++k) {
            along[k] = a[k] * s * radius;
            across[k] = c[k] * co * radius;
        }
        const float center[3] = {cx, cy, cz};
        for (int k = 0; k < 3; ++k) p[k] = center[k] + across[k] + along[k];
        if (i) AddLine(self, nullptr, previous[0], previous[1], previous[2], p[0], p[1], p[2], r, g, b);
        std::memcpy(previous, p, sizeof(p));
    }
}

// FUN_1002c082 (the end of RViewPort_t::Render): the lines twice - through everything, faint (texture factor alpha
// 0x26), then depth tested - then the screen lines and points (world = the view inverted); the lists emptied.
void __fastcall Draw(void*, void*, void* viewport)
{
    if (Global<uint32_t>(kMode)) {
        if (Global<Line*>(kLines) && Global<uint32_t>(kLineCount)) {
            std::vector<uint16_t> indices;
            const uint32_t first = PutLines(Global<Line*>(kLines), Global<uint32_t>(kLineCount), indices);
            float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            orig::render_t_SetTransformMatrix(Render(), 1, identity);   // D3DTRANSFORMSTATE_WORLD
            {
                State zfunc(0x17, 8), zwrite(0xE, 0), blend(0x1B, 1), fog(0x1C, 0), lighting(0x89, 0), src(0x13, 5),
                    dst(0x14, 6), factor(0x3C, 0x26FFFFFF);
                StageState arg2(0, 3, 0), op(0, 1, 3), alphaArg2(0, 6, 3), alphaOp(0, 4, 3), op1(1, 1, 1),
                    alphaOp1(1, 4, 1);
                DrawLines(viewport, first, indices);
            }
            {
                State zwrite(0xE, 0), blend(0x1B, 0), fog(0x1C, 0), lighting(0x89, 0);
                StageState arg2(0, 3, 0), op(0, 1, 3), alphaOp(0, 4, 1), op1(1, 1, 1), alphaOp1(1, 4, 1);
                DrawLines(viewport, first, indices);
            }
        }
        if (Global<Line*>(kScreenLines) && Global<uint32_t>(kScreenLineCount)) {
            std::vector<uint16_t> indices;
            const uint32_t first = PutLines(Global<Line*>(kScreenLines), Global<uint32_t>(kScreenLineCount), indices);
            float world[16];
            ViewSpace(world);
            orig::render_t_SetTransformMatrix(Render(), 1, world);
            State zfunc(0x17, 8), zwrite(0xE, 0), blend(0x1B, 0), fog(0x1C, 0), lighting(0x89, 0);
            StageState arg2(0, 3, 0), op(0, 1, 3), alphaOp(0, 4, 1), op1(1, 1, 1), alphaOp1(1, 4, 1);
            DrawLines(viewport, first, indices);
        }
        if (Global<Point*>(kPoints) && Global<uint32_t>(kPointCount)) {
            const Point* points = Global<Point*>(kPoints);
            const uint32_t count = Global<uint32_t>(kPointCount);
            Vertex* v = nullptr;
            const uint32_t first = orig::DynamicVB_c_GetVertices(orig::DynamicVB_c_Get(), 0x42, 0x10, count, &v);
            for (uint32_t i = 0; i < count; ++i) {
                std::memcpy(v[i].position, points[i].position, 12);
                v[i].color = Color(points[i].color);
            }
            float world[16];
            ViewSpace(world);
            orig::render_t_SetTransformMatrix(Render(), 1, world);
            State zfunc(0x17, 8), zwrite(0xE, 0), blend(0x1B, 0), fog(0x1C, 0), lighting(0x89, 0);
            StageState arg2(0, 3, 0), op(0, 1, 3), alphaOp(0, 4, 1), op1(1, 1, 1), alphaOp1(1, 4, 1);
            orig::RViewPort_t_RealizeRenderStates(viewport);
            void* vb = orig::DynamicVB_c_GetVB(orig::DynamicVB_c_Get(), 0x42);
            orig::render_t_RenderPointList(Render(), vb, first, count, 0);
        }
    }
    Global<uint32_t>(kPointCount) = 0;
    Global<uint32_t>(kScreenLineCount) = 0;
    Global<uint32_t>(kLineCount) = 0;
}

// FUN_1002cc89 (Randy_t's destructor): the Debugger_t gone, its lists (FUN_1002bed9: the mode back to 0x200). The
// original leaves the list pointers dangling; ours clears them.
void __cdecl Shutdown()
{
    void*& instance = Global<void*>(kInstance);
    if (!instance) return;
    Global<uint32_t>(kMode) = 0x200;
    for (uint32_t list : {kLines, kScreenLines, kPoints}) {
        vc10::FreeArray(Global<void*>(list));
        Global<void*>(list) = nullptr;
        Global<uint32_t>(list + 4) = 0;
    }
    vc10::Free(instance);
    instance = nullptr;
}

void Install(HMODULE orig)
{
    g_orig = orig;
    struct Entry {
        uint32_t rva;
        void* target;
        const char* what;
    };
#define FN(f) reinterpret_cast<void*>(&f)
    const Entry entries[] = {
        {0x2CABF, FN(Get), "Debugger_t::Get"},
        {0x2CAE5, FN(AddLine), "Debugger_t::AddLine"},
        {0x2BF51, FN(AddScreenLine), "Debugger_t::Add2DLine"},
        {0x2CBAA, FN(AddPoint), "Debugger_t: a screen point (FUN_1002cbaa)"},
        {0x2CCAB, FN(AddSphere), "Debugger_t::AddSphere"},
        {0x2C082, FN(Draw), "Debugger_t: drawn (FUN_1002c082)"},
        {0x2CC89, FN(Shutdown), "Debugger_t: shut down (FUN_1002cc89)"},
    };
#undef FN
    int installed = 0;
    for (const Entry& e : entries) installed += Replace(orig, e.rva, e.target, e.what) ? 1 : 0;
    Log("debugger: %d of %d functions native", installed, int(sizeof(entries) / sizeof(entries[0])));
}

}  // namespace rnative::debugger
