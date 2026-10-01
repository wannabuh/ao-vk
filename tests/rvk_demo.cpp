// rvk test scenes. Draws a 3x3 grid of tiles, each exercising part of the D3D7 feature set the game uses,
// through rvk's D3D7-shaped API only.
//
//   rvk_demo.exe [--window] [--frames N] [--shot out.bmp]
// Headless by default: renders N frames (default 3) and writes the last one to --shot (default rvk_demo.bmp).
// --window opens a window and animates until it is closed.
#include "rvk.h"
#include "threaded.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace rvk;
using namespace rvk::d3d;

namespace {

constexpr uint32_t kTile = 320, kCols = 3, kRows = 3;
constexpr uint32_t kWidth = kTile * kCols, kHeight = kTile * kRows;
constexpr float kPi = 3.14159265f;

// ---- D3D-style matrix helpers (row vectors, left-handed) ----
Matrix Mul(const Matrix& a, const Matrix& b)
{
    Matrix r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k)
                r.m[i][j] += a.m[i][k] * b.m[k][j];
    return r;
}

Matrix Identity()
{
    Matrix m{};
    for (int i = 0; i < 4; ++i) m.m[i][i] = 1;
    return m;
}

Matrix Translate(float x, float y, float z)
{
    Matrix m = Identity();
    m.m[3][0] = x; m.m[3][1] = y; m.m[3][2] = z;
    return m;
}

Matrix RotateY(float a)
{
    Matrix m = Identity();
    m.m[0][0] = std::cos(a); m.m[0][2] = -std::sin(a);
    m.m[2][0] = std::sin(a); m.m[2][2] = std::cos(a);
    return m;
}

Matrix RotateX(float a)
{
    Matrix m = Identity();
    m.m[1][1] = std::cos(a); m.m[1][2] = std::sin(a);
    m.m[2][1] = -std::sin(a); m.m[2][2] = std::cos(a);
    return m;
}

Matrix RotateZ(float a)
{
    Matrix m = Identity();
    m.m[0][0] = std::cos(a); m.m[0][1] = std::sin(a);
    m.m[1][0] = -std::sin(a); m.m[1][1] = std::cos(a);
    return m;
}

Matrix LookAtLH(Vector eye, Vector at, Vector up)
{
    auto sub = [](Vector a, Vector b) { return Vector{a.x - b.x, a.y - b.y, a.z - b.z}; };
    auto cross = [](Vector a, Vector b) { return Vector{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; };
    auto dot = [](Vector a, Vector b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
    auto norm = [&](Vector a) { float l = std::sqrt(dot(a, a)); return Vector{a.x / l, a.y / l, a.z / l}; };
    Vector z = norm(sub(at, eye)), x = norm(cross(up, z)), y = cross(z, x);
    Matrix m = Identity();
    m.m[0][0] = x.x; m.m[0][1] = y.x; m.m[0][2] = z.x;
    m.m[1][0] = x.y; m.m[1][1] = y.y; m.m[1][2] = z.y;
    m.m[2][0] = x.z; m.m[2][1] = y.z; m.m[2][2] = z.z;
    m.m[3][0] = -dot(x, eye); m.m[3][1] = -dot(y, eye); m.m[3][2] = -dot(z, eye);
    return m;
}

Matrix PerspectiveLH(float fovY, float aspect, float zn, float zf)
{
    float ys = 1.0f / std::tan(fovY / 2), xs = ys / aspect;
    Matrix m{};
    m.m[0][0] = xs;
    m.m[1][1] = ys;
    m.m[2][2] = zf / (zf - zn);
    m.m[2][3] = 1;
    m.m[3][2] = -zn * zf / (zf - zn);
    return m;
}

uint32_t FloatBits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }

// ---- vertex formats ----
struct VtxRhwDiffuseTex { float x, y, z, rhw; uint32_t diffuse; float u, v; };        // 0x144
constexpr uint32_t kFvfRhwDiffuseTex = FVF_XYZRHW | FVF_DIFFUSE | (1 << 8);
struct VtxRhwDiffuse { float x, y, z, rhw; uint32_t diffuse; };                       // 0x044
constexpr uint32_t kFvfRhwDiffuse = FVF_XYZRHW | FVF_DIFFUSE;
struct VtxMesh { float x, y, z, nx, ny, nz; uint32_t diffuse; float u, v; };          // 0x152
constexpr uint32_t kFvfMesh = FVF_XYZ | FVF_NORMAL | FVF_DIFFUSE | (1 << 8);
struct VtxDiffuseTex { float x, y, z; uint32_t diffuse; float u, v; };                // 0x142
constexpr uint32_t kFvfDiffuseTex = FVF_XYZ | FVF_DIFFUSE | (1 << 8);
struct VtxDiffuse { float x, y, z; uint32_t diffuse; };                               // 0x042
constexpr uint32_t kFvfDiffuse = FVF_XYZ | FVF_DIFFUSE;

// ---- textures ----
std::vector<uint32_t> Checker(uint32_t size, uint32_t cells, uint32_t a, uint32_t b)
{
    std::vector<uint32_t> p(size * size);
    for (uint32_t y = 0; y < size; ++y)
        for (uint32_t x = 0; x < size; ++x)
            p[y * size + x] = ((x * cells / size + y * cells / size) & 1) ? a : b;
    return p;
}

std::vector<uint32_t> SoftDot(uint32_t size)        // white, alpha falls off from the centre
{
    std::vector<uint32_t> p(size * size);
    for (uint32_t y = 0; y < size; ++y)
        for (uint32_t x = 0; x < size; ++x) {
            float dx = (x + 0.5f) / size * 2 - 1, dy = (y + 0.5f) / size * 2 - 1;
            float a = std::fmax(0.0f, 1.0f - std::sqrt(dx * dx + dy * dy));
            p[y * size + x] = (uint32_t(a * 255) << 24) | 0xFFFFFF;
        }
    return p;
}

std::vector<uint32_t> Stripes(uint32_t size)        // horizontal colour stripes
{
    static const uint32_t colors[4] = {0xFFFF4040, 0xFF40FF40, 0xFF4040FF, 0xFFFFFF40};
    std::vector<uint32_t> p(size * size);
    for (uint32_t y = 0; y < size; ++y)
        for (uint32_t x = 0; x < size; ++x)
            p[y * size + x] = colors[(y * 8 / size) & 3];
    return p;
}

// Format test textures, 8x8 each (drawn magnified). Expected look:
//   R5G6B5 red->blue gradient | A1R5G5B5 green, right half transparent | A4R4G4B4 white, alpha ramp
//   X8R8G8B8 yellow (alpha byte 0 ignored) | L8 grey ramp | A8 black with alpha ramp (over red)
//   A8L8 grey ramp, bottom half transparent | DXT1 red/green/blue/white blocks | DXT5 white, alpha ramp
template <typename D>
std::vector<Texture*> MakeFormatTextures(D& dev)
{
    std::vector<Texture*> out;
    auto make = [&](Format f, const void* data, uint32_t pitch) {
        Texture* t = dev.CreateTexture(8, 8, f, 1);
        if (t) dev.UpdateTexture(t, 0, 0, 0, 8, 8, data, pitch);
        out.push_back(t);
    };
    uint16_t p16[64];
    for (int i = 0; i < 64; ++i) { int x = i % 8; p16[i] = uint16_t(((7 - x) * 31 / 7) << 11 | (x * 31 / 7)); }
    make(Format::R5G6B5, p16, 16);
    for (int i = 0; i < 64; ++i) p16[i] = uint16_t(((i % 8) < 4 ? 0x8000 : 0) | 0x03E0);
    make(Format::A1R5G5B5, p16, 16);
    for (int i = 0; i < 64; ++i) p16[i] = uint16_t((((i % 8) * 15 / 7) << 12) | 0x0FFF);
    make(Format::A4R4G4B4, p16, 16);
    uint32_t p32[64];
    for (int i = 0; i < 64; ++i) p32[i] = 0x00FFFF00;
    make(Format::X8R8G8B8, p32, 32);
    uint8_t p8[64];
    for (int i = 0; i < 64; ++i) p8[i] = uint8_t((i % 8) * 255 / 7);
    make(Format::L8, p8, 8);
    make(Format::A8, p8, 8);
    for (int i = 0; i < 64; ++i) p16[i] = uint16_t(((i / 8) < 4 ? 0xFF00 : 0) | ((i % 8) * 255 / 7));
    make(Format::A8L8, p16, 16);
    // DXT1: 2x2 blocks, each solid (color0 = colour, color1 = 0, all indices 0).
    uint8_t dxt1[4 * 8] = {};
    const uint16_t colors[4] = {0xF800, 0x07E0, 0x001F, 0xFFFF};
    for (int b = 0; b < 4; ++b) std::memcpy(dxt1 + b * 8, &colors[b], 2);
    make(Format::DXT1, dxt1, 16);                     // pitch = one row of blocks (2 blocks x 8 bytes)
    // DXT5: white, alpha 255..0 across x via the 8-value alpha palette (alpha0 = 255 > alpha1 = 0).
    uint8_t dxt5[4 * 16] = {};
    for (int b = 0; b < 4; ++b) {
        uint8_t* blk = dxt5 + b * 16;
        blk[0] = 255;
        blk[1] = 0;
        uint64_t bits = 0;
        static const int colIndex[2][4] = {{0, 2, 3, 4}, {5, 6, 7, 1}};   // left/right block: 255 ... 0
        for (int px = 0; px < 16; ++px)
            bits |= uint64_t(colIndex[b % 2][px % 4]) << (3 * px);
        std::memcpy(blk + 2, &bits, 6);
        uint16_t white = 0xFFFF;
        std::memcpy(blk + 8, &white, 2);
    }
    make(Format::DXT5, dxt5, 32);
    return out;
}

// 64x64 texture with 7 levels; level 0 a white/grey checker, then solid red, green, blue, yellow, magenta, cyan.
template <typename D>
Texture* MakeMipTexture(D& dev)
{
    Texture* t = dev.CreateTexture(64, 64, Format::A8R8G8B8, 7);
    static const uint32_t colors[7] = {0, 0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFF00, 0xFFFF00FF, 0xFF00FFFF};
    for (uint32_t level = 0; level < 7; ++level) {
        uint32_t size = 64 >> level;
        std::vector<uint32_t> pixels = level == 0 ? Checker(64, 8, 0xFFFFFFFF, 0xFF808080)
                                                  : std::vector<uint32_t>(size * size, colors[level]);
        dev.UpdateTexture(t, level, 0, 0, size, size, pixels.data(), size * 4);
    }
    return t;
}

template <typename D>
struct Scene {
    D& dev;
    Texture* checker;
    Texture* dot;
    Texture* stripes;
    Texture* gray;
    Texture* target;        // 128x128 render target
    std::vector<Texture*> formats;
    Texture* mips;
    VertexBuffer* vb;
    float time;

    void Tile(uint32_t col, uint32_t row, uint32_t clearColor)
    {
        dev.SetViewport({col * kTile, row * kTile, kTile, kTile, 0.0f, 1.0f});
        dev.Clear(CLEAR_TARGET | CLEAR_ZBUFFER, clearColor, 1.0f);
    }

    void ResetState()
    {
        dev.SetRenderState(RS_ZENABLE, 1);
        dev.SetRenderState(RS_ZWRITEENABLE, 1);
        dev.SetRenderState(RS_ZFUNC, CMP_LESSEQUAL);
        dev.SetRenderState(RS_CULLMODE, CULL_CCW);
        dev.SetRenderState(RS_ALPHABLENDENABLE, 0);
        dev.SetRenderState(RS_ALPHATESTENABLE, 0);
        dev.SetRenderState(RS_FOGENABLE, 0);
        dev.SetRenderState(RS_LIGHTING, 0);
        dev.SetRenderState(RS_SPECULARENABLE, 0);
        for (uint32_t s = 0; s < 2; ++s) {
            dev.SetTextureStageState(s, TSS_COLOROP, s == 0 ? TOP_MODULATE : TOP_DISABLE);
            dev.SetTextureStageState(s, TSS_COLORARG1, TA_TEXTURE);
            dev.SetTextureStageState(s, TSS_COLORARG2, TA_DIFFUSE);
            dev.SetTextureStageState(s, TSS_ALPHAOP, s == 0 ? TOP_MODULATE : TOP_DISABLE);
            dev.SetTextureStageState(s, TSS_ALPHAARG1, TA_TEXTURE);
            dev.SetTextureStageState(s, TSS_ALPHAARG2, TA_DIFFUSE);
            dev.SetTextureStageState(s, TSS_TEXCOORDINDEX, s);
            dev.SetTextureStageState(s, TSS_TEXTURETRANSFORMFLAGS, TTFF_DISABLE);
            dev.SetTextureStageState(s, TSS_MAGFILTER, TFG_LINEAR);
            dev.SetTextureStageState(s, TSS_MINFILTER, TFN_LINEAR);
            dev.SetTextureStageState(s, TSS_ADDRESS, TADDRESS_WRAP);
            dev.SetTexture(s, nullptr);
        }
        dev.SetTransform(World, Identity());
        dev.SetTransform(View, LookAtLH({0, 1.5f, -4}, {0, 0, 0}, {0, 1, 0}));
        dev.SetTransform(Projection, PerspectiveLH(kPi / 3, 1.0f, 0.1f, 100.0f));
    }

    // Tile 1: screen-space (XYZRHW) quads like the UI: textured, vertex colours, alpha blending.
    void Ui()
    {
        Tile(0, 0, 0xFF203040);
        ResetState();
        dev.SetRenderState(RS_ZENABLE, 0);
        dev.SetRenderState(RS_CULLMODE, CULL_NONE);
        dev.SetTextureStageState(0, TSS_MAGFILTER, TFG_POINT);
        dev.SetTextureStageState(0, TSS_MINFILTER, TFN_POINT);
        dev.SetTexture(0, checker);
        auto quad = [&](float x0, float y0, float x1, float y1, uint32_t c0, uint32_t c1, uint32_t c2, uint32_t c3) {
            VtxRhwDiffuseTex v[4] = {{x0, y0, 0, 1, c0, 0, 0}, {x1, y0, 0, 1, c1, 1, 0},
                                     {x0, y1, 0, 1, c2, 0, 1}, {x1, y1, 0, 1, c3, 1, 1}};
            dev.DrawPrimitive(TriangleStrip, kFvfRhwDiffuseTex, v, 4);
        };
        quad(20, 20, 300, 150, 0xFFFFFFFF, 0xFFFF0000, 0xFF00FF00, 0xFF0000FF);     // gradient-tinted checker
        dev.SetRenderState(RS_ALPHABLENDENABLE, 1);
        dev.SetRenderState(RS_SRCBLEND, BLEND_SRCALPHA);
        dev.SetRenderState(RS_DESTBLEND, BLEND_INVSRCALPHA);
        dev.SetTexture(0, nullptr);
        dev.SetTextureStageState(0, TSS_COLOROP, TOP_SELECTARG2);    // vertex colour only
        dev.SetTextureStageState(0, TSS_ALPHAOP, TOP_SELECTARG2);
        quad(80, 100, 240, 300, 0x80FFFF00, 0x80FFFF00, 0x00FFFF00, 0x00FFFF00);     // fading yellow panel
        // 1-pixel white frame: checks the D3D pixel-centre mapping (should be crisp, not blurred over 2 px).
        VtxRhwDiffuse frame[5] = {{10, 10, 0, 1, 0xFFFFFFFF}, {309, 10, 0, 1, 0xFFFFFFFF}, {309, 309, 0, 1, 0xFFFFFFFF},
                                  {10, 309, 0, 1, 0xFFFFFFFF}, {10, 10, 0, 1, 0xFFFFFFFF}};
        dev.SetRenderState(RS_ALPHABLENDENABLE, 0);
        dev.DrawPrimitive(LineStrip, kFvfRhwDiffuse, frame, 5);
    }

    // Tile 2: lit, textured, depth-tested cube (FVF 0x152 like character meshes).
    void LitCube()
    {
        Tile(1, 0, 0xFF101010);
        ResetState();
        dev.SetRenderState(RS_LIGHTING, 1);
        dev.SetRenderState(RS_SPECULARENABLE, 1);
        dev.SetRenderState(RS_AMBIENT, 0xFF202020);
        dev.SetRenderState(RS_DIFFUSEMATERIALSOURCE, MCS_MATERIAL);
        Material mat{{1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 1}, {0, 0, 0, 0}, 20.0f};
        dev.SetMaterial(mat);
        Light sun{};
        sun.type = LIGHT_DIRECTIONAL;
        sun.diffuse = {0.8f, 0.8f, 0.8f, 1};
        sun.specular = {1, 1, 1, 1};
        sun.direction = {-0.5f, -1.0f, 0.7f};
        dev.SetLight(0, sun);
        dev.LightEnable(0, true);
        Light red{};
        red.type = LIGHT_POINT;
        red.diffuse = {1.0f, 0.1f, 0.1f, 1};
        red.position = {-1.6f, 0.2f, -1.2f};
        red.range = 10;
        red.attenuation0 = 0.2f;
        red.attenuation1 = 0.3f;
        dev.SetLight(1, red);
        dev.LightEnable(1, true);
        dev.SetTexture(0, checker);

        std::vector<VtxMesh> v;
        std::vector<uint16_t> idx;
        struct Face { Vector n, u, w; } faces[6] = {
            {{0, 0, -1}, {1, 0, 0}, {0, 1, 0}}, {{0, 0, 1}, {-1, 0, 0}, {0, 1, 0}}, {{1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
            {{-1, 0, 0}, {0, 0, -1}, {0, 1, 0}}, {{0, 1, 0}, {1, 0, 0}, {0, 0, 1}}, {{0, -1, 0}, {1, 0, 0}, {0, 0, -1}}};
        for (auto& f : faces) {
            uint16_t base = uint16_t(v.size());
            for (int k = 0; k < 4; ++k) {
                float su = (k == 1 || k == 2) ? 1.f : -1.f, sw = k >= 2 ? -1.f : 1.f;   // corners: TL, TR, BR, BL
                v.push_back({f.n.x + su * f.u.x + sw * f.w.x, f.n.y + su * f.u.y + sw * f.w.y, f.n.z + su * f.u.z + sw * f.w.z,
                             f.n.x, f.n.y, f.n.z, 0xFFFFFFFF, (su + 1) / 2, (1 - sw) / 2});
            }
            uint16_t q[6] = {0, 1, 2, 0, 2, 3};          // clockwise seen from outside (D3D front faces)
            for (uint16_t i : q) idx.push_back(base + i);
        }
        Matrix world = Mul(Mul(RotateY(0.6f + time), RotateX(0.4f)), Translate(0, 0, 0));
        Matrix scale = Identity();
        scale.m[0][0] = scale.m[1][1] = scale.m[2][2] = 0.9f;
        dev.SetTransform(World, Mul(scale, world));
        dev.DrawIndexedPrimitive(TriangleList, kFvfMesh, v.data(), uint32_t(v.size()), idx.data(), uint32_t(idx.size()));
        dev.LightEnable(0, false);
        dev.LightEnable(1, false);
    }

    // Tile 3: additive particles (ZWRITE off) over an alpha-tested cutout.
    void Effects()
    {
        Tile(2, 0, 0xFF000818);
        ResetState();
        dev.SetRenderState(RS_CULLMODE, CULL_NONE);
        // Alpha-tested checker cutout: transparent cells dropped by ALPHAFUNC GREATER 0x80.
        dev.SetTexture(0, checker);
        dev.SetRenderState(RS_ALPHATESTENABLE, 1);
        dev.SetRenderState(RS_ALPHAREF, 0x80);
        dev.SetRenderState(RS_ALPHAFUNC, CMP_GREATER);
        VtxDiffuseTex wall[4] = {{-1.5f, -1.0f, 1, 0xFF8080FF, 0, 1}, {-1.5f, 1.5f, 1, 0xFF8080FF, 0, 0},
                                 {1.5f, -1.0f, 1, 0xFF8080FF, 1, 1}, {1.5f, 1.5f, 1, 0xFF8080FF, 1, 0}};
        dev.DrawPrimitive(TriangleStrip, kFvfDiffuseTex, wall, 4);
        dev.SetRenderState(RS_ALPHATESTENABLE, 0);
        // Additive glows.
        dev.SetTexture(0, dot);
        dev.SetRenderState(RS_ALPHABLENDENABLE, 1);
        dev.SetRenderState(RS_SRCBLEND, BLEND_SRCALPHA);
        dev.SetRenderState(RS_DESTBLEND, BLEND_ONE);
        dev.SetRenderState(RS_ZWRITEENABLE, 0);
        std::vector<VtxDiffuseTex> tris;
        for (int i = 0; i < 12; ++i) {
            float a = i * kPi * 2 / 12 + time, r = 1.1f, s = 0.45f;
            float cx = std::cos(a) * r, cy = std::sin(a) * r * 0.6f + 0.2f, cz = std::sin(a) * 0.5f;
            uint32_t c = (i % 3 == 0) ? 0xFFFF6020 : (i % 3 == 1) ? 0xFF20A0FF : 0xFF60FF60;
            VtxDiffuseTex q[6] = {{cx - s, cy - s, cz, c, 0, 1}, {cx - s, cy + s, cz, c, 0, 0}, {cx + s, cy + s, cz, c, 1, 0},
                                  {cx - s, cy - s, cz, c, 0, 1}, {cx + s, cy + s, cz, c, 1, 0}, {cx + s, cy - s, cz, c, 1, 1}};
            tris.insert(tris.end(), q, q + 6);
        }
        dev.DrawPrimitive(TriangleList, kFvfDiffuseTex, tris.data(), uint32_t(tris.size()));
    }

    // Tile 4: linear fog over a row of receding quads.
    void Fog()
    {
        Tile(0, 1, 0xFF8090A0);
        ResetState();
        dev.SetRenderState(RS_CULLMODE, CULL_NONE);
        dev.SetRenderState(RS_FOGENABLE, 1);
        dev.SetRenderState(RS_FOGCOLOR, 0xFF8090A0);
        dev.SetRenderState(RS_FOGTABLEMODE, FOG_LINEAR);
        dev.SetRenderState(RS_FOGSTART, FloatBits(3.0f));
        dev.SetRenderState(RS_FOGEND, FloatBits(14.0f));
        dev.SetTexture(0, checker);
        dev.SetTransform(View, LookAtLH({0, 1.0f, -2}, {0, 0.6f, 4}, {0, 1, 0}));
        for (int i = 0; i < 8; ++i) {
            float z = i * 2.0f, x = (i & 1) ? 0.9f : -0.9f;
            VtxDiffuseTex q[4] = {{x - 0.6f, 0.0f, z, 0xFFFFC080, 0, 1}, {x - 0.6f, 1.2f, z, 0xFFFFC080, 0, 0},
                                  {x + 0.6f, 0.0f, z, 0xFFFFC080, 1, 1}, {x + 0.6f, 1.2f, z, 0xFFFFC080, 1, 0}};
            dev.DrawPrimitive(TriangleStrip, kFvfDiffuseTex, q, 4);
        }
        VtxDiffuse ground[4] = {{-6, 0, -1, 0xFF406030}, {-6, 0, 20, 0xFF406030}, {6, 0, -1, 0xFF406030}, {6, 0, 20, 0xFF406030}};
        dev.SetTexture(0, nullptr);
        dev.SetTextureStageState(0, TSS_COLOROP, TOP_SELECTARG2);
        dev.SetTextureStageState(0, TSS_ALPHAOP, TOP_SELECTARG2);
        dev.DrawPrimitive(TriangleStrip, kFvfDiffuse, ground, 4);
    }

    // Tile 5: two texture stages (MODULATE2X), a texture matrix, camera-space texture coordinate generation.
    void Stages()
    {
        Tile(1, 1, 0xFF181818);
        ResetState();
        dev.SetRenderState(RS_ZENABLE, 0);
        dev.SetRenderState(RS_CULLMODE, CULL_NONE);
        // Left quad: checker * stripes * 2, stripes scrolled/rotated through the stage 1 texture matrix.
        dev.SetTexture(0, checker);
        dev.SetTexture(1, stripes);
        dev.SetTextureStageState(1, TSS_COLOROP, TOP_MODULATE2X);
        dev.SetTextureStageState(1, TSS_COLORARG1, TA_TEXTURE);
        dev.SetTextureStageState(1, TSS_COLORARG2, TA_CURRENT);
        dev.SetTextureStageState(1, TSS_TEXCOORDINDEX, 0);
        dev.SetTextureStageState(1, TSS_TEXTURETRANSFORMFLAGS, TTFF_COUNT2);
        Matrix tex = Mul(RotateZ(0.5f), Identity());
        tex.m[2][0] = 0.25f + time * 0.1f;            // 2D translation lives in row 2 (coords padded with 1)
        dev.SetTransform(Texture1, tex);
        const float x0 = kTile, y0 = kTile;            // screen-space vertices: tile 5 starts at (320, 320)
        VtxRhwDiffuseTex a[4] = {{x0 + 15, y0 + 30, 0, 1, 0xFF808080, 0, 0}, {x0 + 150, y0 + 30, 0, 1, 0xFF808080, 1, 0},
                                 {x0 + 15, y0 + 290, 0, 1, 0xFF808080, 0, 1}, {x0 + 150, y0 + 290, 0, 1, 0xFF808080, 1, 1}};
        dev.DrawPrimitive(TriangleStrip, kFvfRhwDiffuseTex, a, 4);
        dev.SetTexture(1, nullptr);
        dev.SetTextureStageState(1, TSS_COLOROP, TOP_DISABLE);
        dev.SetTextureStageState(1, TSS_TEXTURETRANSFORMFLAGS, TTFF_DISABLE);
        dev.SetTextureStageState(1, TSS_TEXCOORDINDEX, 1);

        // Right: a 3D quad whose stage 0 coordinates come from its camera-space position.
        dev.SetRenderState(RS_ZENABLE, 1);
        dev.SetViewport({kTile + 160, kTile, 160, kTile, 0.0f, 1.0f});
        dev.SetTransform(Projection, PerspectiveLH(kPi / 3, 0.5f, 0.1f, 100.0f));
        dev.SetTexture(0, stripes);
        dev.SetTextureStageState(0, TSS_TEXCOORDINDEX, TCI_CAMERASPACEPOSITION);
        dev.SetTextureStageState(0, TSS_TEXTURETRANSFORMFLAGS, TTFF_COUNT2);
        Matrix gen = Identity();
        gen.m[0][0] = gen.m[1][1] = 0.5f;
        dev.SetTransform(Texture0, gen);
        dev.SetTransform(World, RotateY(0.8f));
        VtxDiffuse q[4] = {{-1, -1.5f, 0, 0xFFFFFFFF}, {-1, 1.5f, 0, 0xFFFFFFFF}, {1, -1.5f, 0, 0xFFFFFFFF}, {1, 1.5f, 0, 0xFFFFFFFF}};
        dev.DrawPrimitive(TriangleStrip, kFvfDiffuse, q, 4);
        dev.SetTextureStageState(0, TSS_TEXCOORDINDEX, 0);
        dev.SetTextureStageState(0, TSS_TEXTURETRANSFORMFLAGS, TTFF_DISABLE);
    }

    // Tile 6: line list, triangle fan, and culling: a clockwise triangle must appear, a counter-clockwise one must not.
    void Primitives()
    {
        Tile(2, 1, 0xFF202020);
        ResetState();
        dev.SetRenderState(RS_ZENABLE, 0);
        dev.SetTexture(0, nullptr);
        dev.SetTextureStageState(0, TSS_COLOROP, TOP_SELECTARG2);
        dev.SetTextureStageState(0, TSS_ALPHAOP, TOP_SELECTARG2);
        uint32_t x0 = 2 * kTile, y0 = kTile;
        // Fan: a circle, centre white, rim rainbow.
        std::vector<VtxRhwDiffuse> fan = {{x0 + 90.f, y0 + 90.f, 0, 1, 0xFFFFFFFF}};
        for (int i = 0; i <= 24; ++i) {
            float a = i * 2 * kPi / 24;
            uint32_t c = 0xFF000000 | (uint32_t(127 + 127 * std::cos(a)) << 16) |
                         (uint32_t(127 + 127 * std::cos(a + 2.1f)) << 8) | uint32_t(127 + 127 * std::cos(a + 4.2f));
            // Clockwise on screen (y down) so it survives CULL_CCW.
            fan.push_back({x0 + 90 + 70 * std::cos(a), y0 + 90 + 70 * std::sin(a), 0, 1, c});
        }
        dev.DrawPrimitive(TriangleFan, kFvfRhwDiffuse, fan.data(), uint32_t(fan.size()));
        // Culling check (CULL_CCW): green CW triangle visible, red CCW triangle culled.
        VtxRhwDiffuse cw[3] = {{x0 + 190.f, y0 + 30.f, 0, 1, 0xFF00FF00}, {x0 + 300.f, y0 + 140.f, 0, 1, 0xFF00FF00},
                               {x0 + 190.f, y0 + 140.f, 0, 1, 0xFF00FF00}};
        VtxRhwDiffuse ccw[3] = {{x0 + 190.f, y0 + 170.f, 0, 1, 0xFFFF0000}, {x0 + 190.f, y0 + 290.f, 0, 1, 0xFFFF0000},
                                {x0 + 300.f, y0 + 290.f, 0, 1, 0xFFFF0000}};
        dev.DrawPrimitive(TriangleList, kFvfRhwDiffuse, cw, 3);
        dev.DrawPrimitive(TriangleList, kFvfRhwDiffuse, ccw, 3);
        // 3D line list: coordinate axes (x red, y green, z blue).
        dev.SetViewport({x0, y0 + 160, 180, 160, 0.0f, 1.0f});
        dev.SetTransform(View, LookAtLH({2.0f, 1.5f, -2.5f}, {0, 0, 0}, {0, 1, 0}));
        dev.SetTransform(Projection, PerspectiveLH(kPi / 3, 180.0f / 160.0f, 0.1f, 100.0f));
        VtxDiffuse axes[6] = {{0, 0, 0, 0xFFFF0000}, {1.5f, 0, 0, 0xFFFF0000}, {0, 0, 0, 0xFF00FF00},
                              {0, 1.5f, 0, 0xFF00FF00}, {0, 0, 0, 0xFF4080FF}, {0, 0, 1.5f, 0xFF4080FF}};
        dev.DrawPrimitive(LineList, kFvfDiffuse, axes, 6);
    }

    // Tile 7: render to texture mid-frame, then sample it. The tiles already drawn must survive the switch.
    void RenderToTexture()
    {
        dev.SetRenderTarget(target);              // resets the viewport to the whole 128x128 target
        dev.Clear(CLEAR_TARGET | CLEAR_ZBUFFER, 0xFF102040, 1.0f);
        ResetState();
        dev.SetRenderState(RS_ZENABLE, 0);
        dev.SetTexture(0, nullptr);
        dev.SetTextureStageState(0, TSS_COLOROP, TOP_SELECTARG2);
        dev.SetTextureStageState(0, TSS_ALPHAOP, TOP_SELECTARG2);
        std::vector<VtxRhwDiffuse> fan = {{64, 64, 0, 1, 0xFFFFFFFF}};
        for (int i = 0; i <= 6; ++i) {
            float a = i * 2 * kPi / 6 + time;
            fan.push_back({64 + 56 * std::cos(a), 64 + 56 * std::sin(a), 0, 1, (i & 1) ? 0xFFFF8000u : 0xFF00C0FFu});
        }
        dev.DrawPrimitive(TriangleFan, kFvfRhwDiffuse, fan.data(), uint32_t(fan.size()));
        VtxRhwDiffuse marker[4] = {{4, 4, 0, 1, 0xFFFF0000}, {20, 4, 0, 1, 0xFFFF0000}, {4, 20, 0, 1, 0xFFFF0000},
                                   {20, 20, 0, 1, 0xFFFF0000}};   // red square marks the target's top-left
        dev.DrawPrimitive(TriangleStrip, kFvfRhwDiffuse, marker, 4);

        dev.SetRenderTarget(nullptr);
        Tile(0, 2, 0xFF303030);
        ResetState();
        dev.SetRenderState(RS_ZENABLE, 0);
        dev.SetRenderState(RS_CULLMODE, CULL_NONE);
        dev.SetTexture(0, target);
        dev.SetTextureStageState(0, TSS_COLOROP, TOP_SELECTARG1);
        dev.SetTextureStageState(0, TSS_ALPHAOP, TOP_SELECTARG1);
        const float y0 = 2 * kTile;
        auto quad = [&](float x0, float yy, float size) {
            VtxRhwDiffuseTex v[4] = {{x0, yy, 0, 1, 0xFFFFFFFF, 0, 0}, {x0 + size, yy, 0, 1, 0xFFFFFFFF, 1, 0},
                                     {x0, yy + size, 0, 1, 0xFFFFFFFF, 0, 1}, {x0 + size, yy + size, 0, 1, 0xFFFFFFFF, 1, 1}};
            dev.DrawPrimitive(TriangleStrip, kFvfRhwDiffuseTex, v, 4);
        };
        quad(16, y0 + 16, 128);                  // 1:1
        quad(150, y0 + 100, 160);                // magnified
        // Also through the 3D path, on a tilted quad.
        dev.SetViewport({0, 2 * kTile + 160, 150, 160, 0.0f, 1.0f});
        dev.SetRenderState(RS_ZENABLE, 1);
        dev.SetTransform(Projection, PerspectiveLH(kPi / 3, 150.0f / 160.0f, 0.1f, 100.0f));
        dev.SetTransform(World, RotateY(0.7f));
        VtxDiffuseTex q[4] = {{-1, -1, 0, 0xFFFFFFFF, 0, 1}, {-1, 1, 0, 0xFFFFFFFF, 0, 0},
                              {1, -1, 0, 0xFFFFFFFF, 1, 1}, {1, 1, 0, 0xFFFFFFFF, 1, 0}};
        dev.DrawPrimitive(TriangleStrip, kFvfDiffuseTex, q, 4);
        dev.SetTexture(0, nullptr);
    }

    // Tile 8: one quad per texture format (see MakeFormatTextures for what each should look like).
    void Formats()
    {
        Tile(1, 2, 0xFF800000);                  // red background shows through alpha formats
        ResetState();
        dev.SetRenderState(RS_ZENABLE, 0);
        dev.SetRenderState(RS_CULLMODE, CULL_NONE);
        dev.SetRenderState(RS_ALPHABLENDENABLE, 1);
        dev.SetRenderState(RS_SRCBLEND, BLEND_SRCALPHA);
        dev.SetRenderState(RS_DESTBLEND, BLEND_INVSRCALPHA);
        dev.SetTextureStageState(0, TSS_COLOROP, TOP_SELECTARG1);
        dev.SetTextureStageState(0, TSS_ALPHAOP, TOP_SELECTARG1);
        dev.SetTextureStageState(0, TSS_MAGFILTER, TFG_POINT);
        dev.SetTextureStageState(0, TSS_MINFILTER, TFN_POINT);
        for (size_t i = 0; i < formats.size(); ++i) {
            if (!formats[i]) continue;
            dev.SetTexture(0, formats[i]);
            float x0 = kTile + 10 + (i % 3) * 102.0f, y0 = 2 * kTile + 10 + (i / 3) * 102.0f;
            VtxRhwDiffuseTex v[4] = {{x0, y0, 0, 1, 0xFFFFFFFF, 0, 0}, {x0 + 96, y0, 0, 1, 0xFFFFFFFF, 1, 0},
                                     {x0, y0 + 96, 0, 1, 0xFFFFFFFF, 0, 1}, {x0 + 96, y0 + 96, 0, 1, 0xFFFFFFFF, 1, 1}};
            dev.DrawPrimitive(TriangleStrip, kFvfRhwDiffuseTex, v, 4);
        }
        dev.SetTexture(0, nullptr);
    }

    // Tile 9: mip levels on a receding plane (each level a different colour, so selection shows as bands),
    // and vertex buffers: a VB drawn, rewritten, drawn again (the first draw keeps the old contents).
    void MipsAndBuffers()
    {
        Tile(2, 2, 0xFF000000);
        ResetState();
        dev.SetRenderState(RS_CULLMODE, CULL_NONE);
        dev.SetViewport({2 * kTile, 2 * kTile, kTile, 200, 0.0f, 1.0f});
        dev.SetTransform(View, LookAtLH({0, 1.0f, -1}, {0, 0.2f, 6}, {0, 1, 0}));
        dev.SetTransform(Projection, PerspectiveLH(kPi / 3, kTile / 200.0f, 0.1f, 100.0f));
        dev.SetTexture(0, mips);
        dev.SetTextureStageState(0, TSS_COLOROP, TOP_SELECTARG1);
        dev.SetTextureStageState(0, TSS_MAGFILTER, TFG_POINT);
        dev.SetTextureStageState(0, TSS_MINFILTER, TFN_POINT);
        dev.SetTextureStageState(0, TSS_MIPFILTER, TFP_POINT);
        VtxDiffuseTex plane[4] = {{-3, 0, 0, 0xFFFFFFFF, 0, 0}, {-3, 0, 40, 0xFFFFFFFF, 0, 20},
                                  {3, 0, 0, 0xFFFFFFFF, 3, 0}, {3, 0, 40, 0xFFFFFFFF, 3, 20}};
        dev.DrawPrimitive(TriangleStrip, kFvfDiffuseTex, plane, 4);
        dev.SetTextureStageState(0, TSS_MIPFILTER, TFP_NONE);
        dev.SetTexture(0, nullptr);

        // Vertex buffer: green quad drawn, then the same buffer rewritten red at another position.
        dev.SetViewport({2 * kTile, 2 * kTile, kTile, kTile, 0.0f, 1.0f});
        dev.SetRenderState(RS_ZENABLE, 0);
        dev.SetTextureStageState(0, TSS_COLOROP, TOP_SELECTARG2);
        dev.SetTextureStageState(0, TSS_ALPHAOP, TOP_SELECTARG2);
        auto fill = [&](float x0, float y0, uint32_t color) {
            auto* v = static_cast<VtxRhwDiffuse*>(dev.Lock(vb));
            VtxRhwDiffuse q[4] = {{x0, y0, 0, 1, color}, {x0 + 80, y0, 0, 1, color}, {x0, y0 + 80, 0, 1, color},
                                  {x0 + 80, y0 + 80, 0, 1, color}};
            std::memcpy(v, q, sizeof(q));
            dev.Unlock(vb);
        };
        const float x0 = 2 * kTile, y0 = 2 * kTile;
        fill(x0 + 20, y0 + 220, 0xFF00FF00);
        dev.DrawPrimitiveVB(TriangleStrip, vb, 0, 4);
        fill(x0 + 120, y0 + 220, 0xFFFF0000);
        dev.DrawPrimitiveVB(TriangleStrip, vb, 0, 4);
        // Indexed draw from vertex 4 on: indices are relative to startVertex (blue quad).
        auto* v = static_cast<VtxRhwDiffuse*>(dev.Lock(vb));
        VtxRhwDiffuse q[4] = {{x0 + 220, y0 + 220, 0, 1, 0xFF0080FF}, {x0 + 300, y0 + 220, 0, 1, 0xFF0080FF},
                              {x0 + 220, y0 + 300, 0, 1, 0xFF0080FF}, {x0 + 300, y0 + 300, 0, 1, 0xFF0080FF}};
        std::memcpy(v + 4, q, sizeof(q));
        dev.Unlock(vb);
        uint16_t idx[6] = {0, 1, 2, 2, 1, 3};
        dev.DrawIndexedPrimitiveVB(TriangleList, vb, 4, 4, idx, 6);
    }
};

// CPU benchmark shaped like a crowded scene in the game (frame inspector, 2026-09-29): ~2450 indexed draws
// per frame of FVF 0x152 pieces (~67 vertices), a new world matrix per draw, texture / material / light changes
// every few draws. Returns the average CPU time per frame spent recording (BeginFrame .. EndFrame).
double g_benchRecordMs = 0;     // time spent issuing calls, excluding EndFrame (pacing waits)
double g_benchGameMs = 0;       // simulated game work per frame (busy loop), --game-ms

template <typename D>
double Benchmark(D& dev, int frames)
{
    std::vector<Texture*> textures;
    for (int i = 0; i < 16; ++i) {
        auto px = Checker(64, 4 + i % 4, 0xFF000000u | (0x101010u * (i + 4)), 0xFF404040);
        textures.push_back(dev.CreateTexture(64, 64, px.data()));
    }
    // One "piece": a 67-vertex strip-like patch, 32 triangles.
    std::vector<VtxMesh> verts(67);
    for (int i = 0; i < 67; ++i)
        verts[i] = {float(i % 8) * 0.1f, float(i / 8) * 0.1f, 0.0f, 0, 0, -1, 0xFFFFFFFF, (i % 8) / 7.0f, (i / 8) / 8.0f};
    std::vector<uint16_t> idx;
    for (int t = 0; t < 32; ++t) { idx.push_back(uint16_t(t)); idx.push_back(uint16_t(t + 1)); idx.push_back(uint16_t(t + 8)); }
    Light l{};
    l.type = LIGHT_POINT;
    l.diffuse = {1, 0.9f, 0.8f, 1};
    l.range = 50;
    l.attenuation0 = 1;
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    double total = 0, record = 0;
    for (int f = 0; f < frames; ++f) {
        QueryPerformanceCounter(&t0);
        if (g_benchGameMs > 0) {                      // the game's own work before it renders
            LARGE_INTEGER s, n;
            QueryPerformanceCounter(&s);
            do QueryPerformanceCounter(&n); while (double(n.QuadPart - s.QuadPart) * 1000.0 / double(freq.QuadPart) < g_benchGameMs);
        }
        LARGE_INTEGER r0, r1;
        QueryPerformanceCounter(&r0);
        dev.BeginFrame();
        dev.SetViewport({0, 0, kWidth, kHeight, 0, 1});
        dev.Clear(CLEAR_TARGET | CLEAR_ZBUFFER, 0xFF102030, 1.0f);
        dev.SetRenderState(RS_LIGHTING, 1);
        dev.SetTransform(View, LookAtLH({0, 2, -10}, {0, 0, 0}, {0, 1, 0}));
        dev.SetTransform(Projection, PerspectiveLH(kPi / 3, float(kWidth) / kHeight, 0.1f, 100.0f));
        for (int i = 0; i < 4; ++i) {
            l.position = {float(i) * 3 - 4, 2, -2};
            dev.SetLight(i, l);
            dev.LightEnable(i, true);
        }
        for (int d = 0; d < 2450; ++d) {
            if (d % 2 == 0) dev.SetTexture(0, textures[(d / 2) % textures.size()]);
            if (d % 7 == 0) {
                Material m{{1, 1, 1, 1}, {0.4f, 0.4f, 0.4f, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}, 0};
                dev.SetMaterial(m);
            }
            if (d % 13 == 0) dev.SetRenderState(RS_ALPHABLENDENABLE, (d / 13) & 1);
            dev.SetTransform(World, Translate(float(d % 50) * 0.2f - 5, float(d / 50) * 0.2f - 5, float(d % 7)));
            dev.DrawIndexedPrimitive(TriangleList, kFvfMesh, verts.data(), 67, idx.data(), uint32_t(idx.size()));
        }
        QueryPerformanceCounter(&r1);
        dev.EndFrame();
        QueryPerformanceCounter(&t1);
        if (f >= 10) {                                 // skip warm-up
            total += double(t1.QuadPart - t0.QuadPart) / double(freq.QuadPart);
            record += double(r1.QuadPart - r0.QuadPart) / double(freq.QuadPart);
        }
    }
    for (Texture* t : textures) dev.DestroyTexture(t);
    g_benchRecordMs = record * 1000.0 / std::max(frames - 10, 1);
    return total * 1000.0 / std::max(frames - 10, 1);
}

template <typename D>
void RunDemo(D& dev, bool windowed, bool stress, int frames, const std::string& shot)
{
    auto checkerPixels = Checker(64, 8, 0xFFE0E0E0, 0x00404040);   // dark cells are transparent (alpha test)
    auto dotPixels = SoftDot(64);
    auto stripePixels = Stripes(64);
    uint32_t grayPixel = 0xFF808080;
    Scene<D> scene{dev, dev.CreateTexture(64, 64, checkerPixels.data()), dev.CreateTexture(64, 64, dotPixels.data()),
                dev.CreateTexture(64, 64, stripePixels.data()), dev.CreateTexture(1, 1, &grayPixel),
                dev.CreateRenderTarget(128, 128), MakeFormatTextures(dev), MakeMipTexture(dev),
                dev.CreateVertexBuffer(kFvfRhwDiffuse, 8), 0.0f};

    for (int frame = 0; windowed || frame < frames; ++frame) {
        if (windowed) {
            MSG msg;
            bool quit = false;
            while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) quit = true;
                TranslateMessage(&msg);
                DispatchMessageA(&msg);
            }
            if (quit) break;
            scene.time = GetTickCount() / 1000.0f;
        }
        if (!windowed && frame == frames - 1)
            dev.RequestScreenshot(shot);
        dev.BeginFrame();
        if (stress && frame == 0) {
            // ~100 MB of texture uploads inside one frame: more than the 64 MB ring, so rvk must flush.
            std::vector<uint32_t> pixels(256 * 256, 0xFF00FF00);
            std::vector<Texture*> many;
            for (int i = 0; i < 400; ++i)
                many.push_back(dev.CreateTexture(256, 256, pixels.data()));
            for (Texture* t : many)
                dev.DestroyTexture(t);
            std::printf("stress: uploaded %zu textures (%zu MB) in one frame\n", many.size(), many.size() * 256 * 256 * 4 >> 20);
        }
        scene.Ui();
        scene.LitCube();
        scene.Effects();
        scene.Fog();
        scene.Stages();
        scene.Primitives();
        scene.RenderToTexture();
        scene.Formats();
        scene.MipsAndBuffers();
        Texture* shortLived = nullptr;
        if (stress) {
            // Drawn with in this frame and destroyed right after submission, while the GPU may still be
            // running the frame: rvk must defer the free until that work has completed.
            uint32_t px[16] = {};
            shortLived = dev.CreateTexture(4, 4, px);
            dev.SetTexture(0, shortLived);
            VtxRhwDiffuseTex q[3] = {{0, 0, 0, 1, 0x01FFFFFF, 0, 0}, {1, 0, 0, 1, 0x01FFFFFF, 1, 0}, {0, 1, 0, 1, 0x01FFFFFF, 0, 1}};
            dev.DrawPrimitive(TriangleList, kFvfRhwDiffuseTex, q, 3);
            dev.SetTexture(0, nullptr);
        }
        dev.EndFrame();
        if (shortLived)
            dev.DestroyTexture(shortLived);
        if (stress && frame == 1) {
            // ~100 MB of uploads *between* frames (texture streaming while the game isn't inside a frame).
            std::vector<uint32_t> pixels(256 * 256, 0xFF0000FF);
            std::vector<Texture*> many;
            for (int i = 0; i < 400; ++i)
                many.push_back(dev.CreateTexture(256, 256, pixels.data()));
            for (Texture* t : many)
                dev.DestroyTexture(t);
            std::printf("stress: uploaded %zu textures between frames\n", many.size());
        }
    }
    for (Texture* t : {scene.checker, scene.dot, scene.stripes, scene.gray, scene.target, scene.mips})
        dev.DestroyTexture(t);
    for (Texture* t : scene.formats)
        dev.DestroyTexture(t);
    dev.DestroyVertexBuffer(scene.vb);
}

float g_cameraYaw = 0.0f;  // --camera-yaw: the shadow test's camera turns this much a frame (motion blur)
float g_movingCube = 0.0f; // --moving-cube: the shadow test's big right cube moves this far a frame along x
float g_deformCube = 0.0f; // --deform-cube: its top vertices move this far a frame along x (like CPU skinning)

// Sun shadow test (--shadow-test): cubes and an alpha-tested fence on a ground of two halves - lit by the sun
// (left) and unlit like Anarchy Online's ground base pass (right) - from a camera above and behind.
void AddCube(std::vector<VtxMesh>& v, std::vector<uint16_t>& idx, float cx, float cy, float cz, float h)
{
    struct Face { Vector n, u, w; } faces[6] = {
        {{0, 0, -1}, {1, 0, 0}, {0, 1, 0}}, {{0, 0, 1}, {-1, 0, 0}, {0, 1, 0}}, {{1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
        {{-1, 0, 0}, {0, 0, -1}, {0, 1, 0}}, {{0, 1, 0}, {1, 0, 0}, {0, 0, 1}}, {{0, -1, 0}, {1, 0, 0}, {0, 0, -1}}};
    for (auto& f : faces) {
        uint16_t base = uint16_t(v.size());
        for (int k = 0; k < 4; ++k) {
            float su = (k == 1 || k == 2) ? 1.f : -1.f, sw = k >= 2 ? -1.f : 1.f;
            v.push_back({cx + h * (f.n.x + su * f.u.x + sw * f.w.x), cy + h * (f.n.y + su * f.u.y + sw * f.w.y),
                         cz + h * (f.n.z + su * f.u.z + sw * f.w.z), f.n.x, f.n.y, f.n.z, 0xFFFFFFFF, (su + 1) / 2, (1 - sw) / 2});
        }
        uint16_t q[6] = {0, 1, 2, 0, 2, 3};
        for (uint16_t i : q) idx.push_back(base + i);
    }
}

template <typename D>
void RunShadowTest(D& dev, int frames, const std::string& shot, int cacheTest, int frameMs, const std::string& dump)
{
    auto groundPixels = Checker(64, 8, 0xFFC8C0B0, 0xFFA09888);
    auto fencePixels = Checker(64, 8, 0xFF806040, 0x00000000);
    Texture* ground = dev.CreateTexture(64, 64, groundPixels.data());
    Texture* fence = dev.CreateTexture(64, 64, fencePixels.data());
    std::vector<uint32_t> blobPixels(64, 0xFFFFFFFF);
    std::vector<uint32_t> lightmapPixels(64, 0xFFC0C0C0);
    std::vector<uint32_t> labelPixels(64 * 16, 0xFF20FF20);
    Texture* labelTex = dev.CreateTexture(64, 16, labelPixels.data());
    Texture* lightmap = dev.CreateTexture(8, 8, lightmapPixels.data());
    Texture* blobTex = dev.CreateTexture(8, 8, blobPixels.data());
    for (int frame = 0; frame < frames; ++frame) {
        if (frame == frames - 1) {
            dev.RequestScreenshot(shot);
            if (!dump.empty()) dev.RequestFrameDump(dump);     // the shadow test dumps its last frame
        }
        dev.BeginFrame();
        dev.SetViewport({0, 0, kWidth, kHeight, 0.0f, 1.0f});
        dev.Clear(CLEAR_TARGET | CLEAR_ZBUFFER, 0xFF6080A0, 1.0f);
        // --cache-test K: after K frames, look straight down past the big right cube (which the "game" then no
        // longer draws, as it is out of view) at the tip of its shadow, which must stay if rvk remembered it.
        bool topDown = cacheTest > 0 && frame >= cacheTest;
        if (topDown) {
            dev.SetTransform(View, LookAtLH({7.5f, 4, 6.5f}, {7.5f, 0, 6.5f}, {0, 0, 1}));
            dev.SetTransform(Projection, PerspectiveLH(kPi / 4.5f, float(kWidth) / kHeight, 0.5f, 200.0f));
        } else if (g_cameraYaw != 0.0f) {
            // --camera-yaw r: the camera turns r radians a frame around the scene (motion blur test).
            float a = g_cameraYaw * float(frame), ex = 16.6f * std::sin(a), ez = 2.0f - 16.6f * std::cos(a);
            dev.SetTransform(View, LookAtLH({ex, 9, ez}, {0, 0, 2}, {0, 1, 0}));
            dev.SetTransform(Projection, PerspectiveLH(kPi / 3, float(kWidth) / kHeight, 0.5f, 200.0f));
        } else {
            dev.SetTransform(View, LookAtLH({0, 9, -14}, {0, 0, 2}, {0, 1, 0}));
            dev.SetTransform(Projection, PerspectiveLH(kPi / 3, float(kWidth) / kHeight, 0.5f, 200.0f));
        }
        dev.SetTransform(World, Identity());
        dev.SetRenderState(RS_ZENABLE, 1);
        dev.SetRenderState(RS_ZWRITEENABLE, 1);
        dev.SetRenderState(RS_ZFUNC, CMP_LESSEQUAL);
        dev.SetRenderState(RS_CULLMODE, CULL_NONE);
        dev.SetRenderState(RS_ALPHABLENDENABLE, 0);
        dev.SetRenderState(RS_ALPHATESTENABLE, 0);
        dev.SetRenderState(RS_AMBIENT, 0xFF505050);
        dev.SetRenderState(RS_DIFFUSEMATERIALSOURCE, MCS_MATERIAL);
        Material mat{{1, 1, 1, 1}, {1, 1, 1, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}, 0.0f};
        dev.SetMaterial(mat);
        Light sun{};
        sun.type = LIGHT_DIRECTIONAL;
        sun.diffuse = {0.9f, 0.85f, 0.75f, 1};
        sun.direction = {0.55f, -0.7f, 0.45f};
        dev.SetLight(0, sun);
        dev.LightEnable(0, true);
        dev.SetTextureStageState(0, TSS_COLOROP, TOP_MODULATE);
        dev.SetTextureStageState(0, TSS_COLORARG1, TA_TEXTURE);
        dev.SetTextureStageState(0, TSS_COLORARG2, TA_DIFFUSE);
        dev.SetTextureStageState(0, TSS_ALPHAOP, TOP_SELECTARG1);
        dev.SetTextureStageState(0, TSS_ALPHAARG1, TA_TEXTURE);
        dev.SetTexture(0, ground);
        {   // left: lit ground
            VtxMesh q[4] = {{-20, 0, -10, 0, 1, 0, 0xFFFFFFFF, 0, 0}, {0, 0, -10, 0, 1, 0, 0xFFFFFFFF, 5, 0},
                            {0, 0, 20, 0, 1, 0, 0xFFFFFFFF, 5, 7.5f}, {-20, 0, 20, 0, 1, 0, 0xFFFFFFFF, 0, 7.5f}};
            dev.SetRenderState(RS_LIGHTING, 1);
            dev.DrawPrimitive(TriangleFan, kFvfMesh, q, 4);
        }
        // right: like AnarchyGround_t - world-space terrain (FVF 0x212), an unlit base pass, then a multiplying
        // pass of lightmap + local lights (ZERO/SRCCOLOR, depth EQUAL) with a point light inside a cube's shadow.
        struct VtxTerrain { float x, y, z, nx, ny, nz, u0, v0, u1, v1; };
        const uint32_t kFvfTerrain = FVF_XYZ | FVF_NORMAL | (2 << 8);
        std::vector<VtxTerrain> terrain;
        std::vector<uint16_t> tidx;
        const int cells = 16;
        for (int j = 0; j <= cells; ++j)
            for (int i = 0; i <= cells; ++i) {
                float x = 20.0f * i / cells, z = -10.0f + 30.0f * j / cells;
                terrain.push_back({x, 0, z, 0, 1, 0, x / 4, z / 4, 0.5f, 0.5f});
            }
        for (int j = 0; j < cells; ++j)
            for (int i = 0; i < cells; ++i) {
                uint16_t a = uint16_t(j * (cells + 1) + i), b = uint16_t(a + 1), c = uint16_t(a + cells + 1), d = uint16_t(c + 1);
                uint16_t q[6] = {a, c, b, b, c, d};
                tidx.insert(tidx.end(), q, q + 6);
            }
        dev.SetRenderState(RS_LIGHTING, 0);
        dev.SetTextureStageState(0, TSS_COLOROP, TOP_SELECTARG1);
        dev.DrawIndexedPrimitive(TriangleList, kFvfTerrain, terrain.data(), uint32_t(terrain.size()), tidx.data(), uint32_t(tidx.size()));
        Light lamp{};
        lamp.type = LIGHT_POINT;
        lamp.diffuse = {1.0f, 0.8f, 0.4f, 1};
        lamp.position = {6.5f, 0.6f, 6.0f};
        lamp.range = 4.0f;
        lamp.attenuation1 = 0.15f;                    // bright enough to fill the shadow in near it
        dev.SetLight(1, lamp);
        // The sun stays enabled, as the game leaves it when there are 8 lights or fewer; rvk keeps it off terrain.
        // (No lamp in the cache test: it would fill in the shadow that test looks at.)
        dev.LightEnable(1, cacheTest == 0);
        dev.SetRenderState(RS_LIGHTING, 1);
        dev.SetRenderState(RS_AMBIENT, 0);
        dev.SetRenderState(RS_ALPHABLENDENABLE, 1);
        dev.SetRenderState(RS_SRCBLEND, BLEND_ZERO);
        dev.SetRenderState(RS_DESTBLEND, BLEND_SRCCOLOR);
        dev.SetRenderState(RS_ZFUNC, CMP_EQUAL);
        dev.SetTexture(0, lightmap);
        dev.SetTextureStageState(0, TSS_COLOROP, TOP_ADD);
        dev.SetTextureStageState(0, TSS_TEXCOORDINDEX, 1);
        dev.DrawIndexedPrimitive(TriangleList, kFvfTerrain, terrain.data(), uint32_t(terrain.size()), tidx.data(), uint32_t(tidx.size()));
        dev.SetTextureStageState(0, TSS_TEXCOORDINDEX, 0);
        dev.SetRenderState(RS_ZFUNC, CMP_LESSEQUAL);
        dev.SetRenderState(RS_ALPHABLENDENABLE, 0);
        dev.SetRenderState(RS_AMBIENT, 0xFF505050);
        dev.LightEnable(1, false);
        dev.SetTexture(0, ground);
        dev.SetTextureStageState(0, TSS_COLOROP, TOP_MODULATE);
        dev.SetRenderState(RS_LIGHTING, 1);
        dev.SetTexture(0, nullptr);
        dev.SetTextureStageState(0, TSS_COLOROP, TOP_SELECTARG2);
        const float cubes[4][4] = {{-5, 1, 2, 1}, {-2, 2, 6, 2}, {4, 1.5f, 3, 1.5f}, {7, 0.5f, -2, 0.5f}};
        for (int k = 0; k < 4; ++k) {                // one draw per object, like the game
            if (k == 2 && topDown)
                continue;
            std::vector<VtxMesh> v;
            std::vector<uint16_t> idx;
            AddCube(v, idx, cubes[k][0], cubes[k][1], cubes[k][2], cubes[k][3]);
            if (k == 2 && g_deformCube != 0.0f)      // animated vertices, the world matrix still (CPU skinning)
                for (VtxMesh& m : v)
                    if (m.y > 1.0f) m.x += g_deformCube * float(frame);
            if (k == 2 && g_movingCube != 0.0f) {    // moving through the world by its world matrix (motion blur)
                Matrix move = Identity();
                move.m[3][0] = g_movingCube * float(frame) - 2.0f * g_movingCube;
                dev.SetTransform(World, move);
            }
            if (k == 2 && cacheTest > 0) {           // swaying like a plant: vertices and world matrix change
                for (VtxMesh& m : v)
                    if (m.y > 1.0f) m.x += 0.15f * std::sin(frame * 0.7f);
                Matrix sway = RotateZ(0.04f * std::sin(frame * 0.5f));   // tilts; the translation stays
                dev.SetTransform(World, sway);
            }
            dev.DrawIndexedPrimitive(TriangleList, kFvfMesh, v.data(), uint32_t(v.size()), idx.data(), uint32_t(idx.size()));
            dev.SetTransform(World, Identity());
        }
        // Alpha-tested fence: its shadow must have holes.
        dev.SetTextureStageState(0, TSS_COLOROP, TOP_MODULATE);
        dev.SetTexture(0, fence);
        dev.SetRenderState(RS_ALPHATESTENABLE, 1);
        dev.SetRenderState(RS_ALPHAREF, 0x80);
        dev.SetRenderState(RS_ALPHAFUNC, CMP_GREATEREQUAL);
        VtxMesh f[4] = {{0, 0, 9, 0, 0, -1, 0xFFFFFFFF, 0, 1}, {0, 3, 9, 0, 0, -1, 0xFFFFFFFF, 0, 0},
                        {4, 3, 9, 0, 0, -1, 0xFFFFFFFF, 1, 0}, {4, 0, 9, 0, 0, -1, 0xFFFFFFFF, 1, 1}};
        dev.DrawPrimitive(TriangleFan, kFvfMesh, f, 4);
        dev.SetRenderState(RS_ALPHATESTENABLE, 0);
        // A static object drawn the way Anarchy Online draws most of them: alpha-blended but writing depth. Its
        // texture's transparent cells must cut holes into its shadow.
        {
            std::vector<VtxMesh> sv;
            std::vector<uint16_t> si;
            AddCube(sv, si, -8, 1.2f, -3, 1.2f);
            dev.SetRenderState(RS_ALPHABLENDENABLE, 1);
            dev.SetRenderState(RS_SRCBLEND, BLEND_SRCALPHA);
            dev.SetRenderState(RS_DESTBLEND, BLEND_INVSRCALPHA);
            dev.DrawIndexedPrimitive(TriangleList, kFvfMesh, sv.data(), uint32_t(sv.size()), si.data(), uint32_t(si.size()));
            dev.SetRenderState(RS_ALPHABLENDENABLE, 0);
        }
        // A name label over the small cube (unlit, blended, depth-writing quad with a wide text texture) and a 3D
        // preview drawn with its own camera like an interface window's: neither may cast a shadow.
        {
            dev.SetRenderState(RS_LIGHTING, 0);
            dev.SetRenderState(RS_ALPHABLENDENABLE, 1);
            dev.SetRenderState(RS_SRCBLEND, BLEND_SRCALPHA);
            dev.SetRenderState(RS_DESTBLEND, BLEND_INVSRCALPHA);
            dev.SetTexture(0, labelTex);
            dev.SetTextureStageState(0, TSS_COLOROP, TOP_SELECTARG1);
            VtxDiffuseTex label[4] = {{5.5f, 2.6f, -2, 0xFFFFFFFF, 0, 1}, {5.5f, 3.4f, -2, 0xFFFFFFFF, 0, 0},
                                      {8.5f, 2.6f, -2, 0xFFFFFFFF, 1, 1}, {8.5f, 3.4f, -2, 0xFFFFFFFF, 1, 0}};
            dev.DrawPrimitive(TriangleStrip, kFvfDiffuseTex, label, 4);
            dev.SetRenderState(RS_ALPHABLENDENABLE, 0);
            dev.SetRenderState(RS_LIGHTING, 1);
            dev.SetTexture(0, nullptr);
            dev.SetTextureStageState(0, TSS_COLOROP, TOP_SELECTARG2);
            Matrix saveView = LookAtLH({0, 9, -14}, {0, 0, 2}, {0, 1, 0});
            dev.SetTransform(View, LookAtLH({0, 0, -6}, {0, 0, 0}, {0, 1, 0}));
            dev.SetTransform(Projection, PerspectiveLH(kPi / 4, 1.0f, 0.5f, 50.0f));   // a window's own projection
            dev.SetViewport({kWidth - 200, 20, 180, 180, 0.0f, 1.0f});
            std::vector<VtxMesh> pv;
            std::vector<uint16_t> pi;
            AddCube(pv, pi, 0, 0, 0, 1.5f);
            dev.DrawIndexedPrimitive(TriangleList, kFvfMesh, pv.data(), uint32_t(pv.size()), pi.data(), uint32_t(pi.size()));
            dev.SetViewport({0, 0, kWidth, kHeight, 0.0f, 1.0f});
            if (!topDown) {
                dev.SetTransform(View, saveView);
                dev.SetTransform(Projection, PerspectiveLH(kPi / 3, float(kWidth) / kHeight, 0.5f, 200.0f));
            } else {
                dev.SetTransform(View, LookAtLH({7.5f, 4, 6.5f}, {7.5f, 0, 6.5f}, {0, 0, 1}));
                dev.SetTransform(Projection, PerspectiveLH(kPi / 4.5f, float(kWidth) / kHeight, 0.5f, 200.0f));
            }
        }
        // Anarchy Online's blob shadow (GfxVisualSimpleShadow_c, 8 segments) under the small cube: rvk hides it
        // once sun shadows are available (from the second frame).
        dev.SetRenderState(RS_LIGHTING, 0);
        dev.SetRenderState(RS_ZWRITEENABLE, 0);
        dev.SetRenderState(RS_ALPHABLENDENABLE, 1);
        dev.SetRenderState(RS_SRCBLEND, BLEND_SRCALPHA);
        dev.SetRenderState(RS_DESTBLEND, BLEND_INVSRCALPHA);
        dev.SetTexture(0, blobTex);
        dev.SetTextureStageState(0, TSS_COLOROP, TOP_SELECTARG2);
        dev.SetTextureStageState(0, TSS_ALPHAOP, TOP_SELECTARG2);
        dev.SetTextureStageState(0, TSS_ALPHAARG2, TA_DIFFUSE);
        {
            const int n = 8;
            VtxDiffuseTex blob[2 * n + 1];
            blob[0] = {7, 0.02f, -2, 0x5F000000, 0.5f, 0.5f};
            for (int i = 0; i < n; ++i) {
                float a = 2 * kPi * i / n;
                blob[1 + i] = {7 + 0.9f * std::cos(a), 0.02f, -2 + 0.9f * std::sin(a), 0x5F000000, 0.5f, 0.5f};
                blob[1 + n + i] = {7 + 1.6f * std::cos(a), 0.02f, -2 + 1.6f * std::sin(a), 0x00000000, 0.5f, 0.5f};
            }
            uint16_t fan[n + 2], strip[2 * n + 2];
            fan[0] = 0;
            for (int i = 0; i <= n; ++i) fan[1 + i] = uint16_t(1 + i % n);
            for (int i = 0; i <= n; ++i) { strip[2 * i] = uint16_t(1 + i % n); strip[2 * i + 1] = uint16_t(1 + n + i % n); }
            dev.DrawIndexedPrimitive(TriangleFan, kFvfDiffuseTex, blob, 2 * n + 1, fan, n + 2);
            dev.DrawIndexedPrimitive(TriangleStrip, kFvfDiffuseTex, blob, 2 * n + 1, strip, 2 * n + 2);
        }
        dev.SetRenderState(RS_ALPHABLENDENABLE, 0);
        dev.SetRenderState(RS_ZWRITEENABLE, 1);
        dev.SetTexture(0, nullptr);
        dev.LightEnable(0, false);
        dev.EndFrame();
        if (frameMs > 0)
            Sleep(DWORD(frameMs));
    }
    dev.DestroyTexture(ground);
    dev.DestroyTexture(fence);
    dev.DestroyTexture(blobTex);
    dev.DestroyTexture(lightmap);
    dev.DestroyTexture(labelTex);
}

bool g_slabTest = false;   // --point-shadow-slab
float g_off[3] = {};        // --world-offset x y z: the scene far from the origin, like Anarchy Online's world
Matrix Offset()
{
    Matrix m = Identity();
    m.m[3][0] = g_off[0]; m.m[3][1] = g_off[1]; m.m[3][2] = g_off[2];
    return m;
}

// Point light shadow test (--point-shadow-test): night, a lamp among cubes. Left half: lit ground (per-pixel); right
// half: the ground the way Anarchy Online draws it (unlit base pass + lightmap and lights multiplying pass). A small
// cube around the lamp (its housing) must not cast; the cubes around it must, in every direction (all cube faces).
template <typename D>
void RunPointShadowTest(D& dev, int frames, const std::string& shot, const std::string& dump, bool sun)
{
    auto groundPixels = Checker(64, 8, 0xFFC8C0B0, 0xFFA09888);
    Texture* ground = dev.CreateTexture(64, 64, groundPixels.data());
    // --point-shadow-sun: daytime - a sun (with its shadows) and a brighter lightmap with the sun baked in.
    std::vector<uint32_t> lightmapPixels(64, sun ? 0xFFE8E8E8 : 0xFF202020);
    Texture* lightmap = dev.CreateTexture(8, 8, lightmapPixels.data());
    struct VtxTerrain { float x, y, z, nx, ny, nz, u0, v0, u1, v1; };
    const uint32_t kFvfTerrain = FVF_XYZ | FVF_NORMAL | (2 << 8);
    std::vector<VtxTerrain> terrain;
    std::vector<uint16_t> tidx;
    const int cells = 16;
    for (int j = 0; j <= cells; ++j)
        for (int i = 0; i <= cells; ++i) {
            float x = 12.0f * i / cells, z = -8.0f + 20.0f * j / cells;
            terrain.push_back({x, 0, z, 0, 1, 0, x / 4, z / 4, 0.5f, 0.5f});
        }
    for (int j = 0; j < cells; ++j)
        for (int i = 0; i < cells; ++i) {
            uint16_t a = uint16_t(j * (cells + 1) + i), b = uint16_t(a + 1), c = uint16_t(a + cells + 1), d = uint16_t(c + 1);
            uint16_t q[6] = {a, c, b, b, c, d};
            tidx.insert(tidx.end(), q, q + 6);
        }
    for (int frame = 0; frame < frames; ++frame) {
        if (frame == frames - 1) {
            dev.RequestScreenshot(shot);
            if (!dump.empty()) dev.RequestFrameDump(dump);
        }
        dev.BeginFrame();
        dev.SetViewport({0, 0, kWidth, kHeight, 0.0f, 1.0f});
        dev.Clear(CLEAR_TARGET | CLEAR_ZBUFFER, 0xFF101828, 1.0f);
        dev.SetTransform(View, LookAtLH({g_off[0], g_off[1] + 13, g_off[2] - 11}, {g_off[0], g_off[1], g_off[2] + 1.5f}, {0, 1, 0}));
        dev.SetTransform(Projection, PerspectiveLH(kPi / 3, float(kWidth) / kHeight, 0.5f, 200.0f));
        dev.SetTransform(World, Offset());
        dev.SetRenderState(RS_ZENABLE, 1);
        dev.SetRenderState(RS_ZWRITEENABLE, 1);
        dev.SetRenderState(RS_ZFUNC, CMP_LESSEQUAL);
        dev.SetRenderState(RS_CULLMODE, CULL_NONE);
        dev.SetRenderState(RS_ALPHABLENDENABLE, 0);
        dev.SetRenderState(RS_ALPHATESTENABLE, 0);
        dev.SetRenderState(RS_AMBIENT, 0xFF181818);
        dev.SetRenderState(RS_DIFFUSEMATERIALSOURCE, MCS_MATERIAL);
        Material mat{{1, 1, 1, 1}, {1, 1, 1, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}, 0.0f};
        dev.SetMaterial(mat);
        Light lamp{};
        lamp.type = LIGHT_POINT;
        lamp.diffuse = {1.0f, 0.85f, 0.6f, 1};
        lamp.position = {0.0f, 1.2f, 1.0f};
        lamp.range = 9.0f;
        lamp.attenuation0 = 0.3f;
        lamp.attenuation1 = 0.08f;
        if (g_slabTest) {                      // the player's light: 2 above the floor, 1 / (0.163 d), range 6.13
            lamp.position = {-3.5f, 2.2f, 1.0f};
            lamp.diffuse = {0.25f, 0.23f, 0.16f, 1};   // dimmed: the floor under it must not clip
            lamp.range = 6.13f;
            lamp.attenuation0 = 0.0f;
            lamp.attenuation1 = 0.1631f;
        }
        lamp.position = {lamp.position.x + g_off[0], lamp.position.y + g_off[1], lamp.position.z + g_off[2]};
        dev.SetLight(0, lamp);
        dev.LightEnable(0, true);
        Light sunLight{};
        sunLight.type = LIGHT_DIRECTIONAL;
        sunLight.diffuse = {0.6f, 0.57f, 0.5f, 1};
        sunLight.direction = {-0.5f, -0.6f, 0.62f};
        dev.SetLight(1, sunLight);
        dev.LightEnable(1, sun);
        dev.SetTextureStageState(0, TSS_COLOROP, TOP_MODULATE);
        dev.SetTextureStageState(0, TSS_COLORARG1, TA_TEXTURE);
        dev.SetTextureStageState(0, TSS_COLORARG2, TA_DIFFUSE);
        dev.SetTextureStageState(0, TSS_ALPHAOP, TOP_SELECTARG1);
        dev.SetTextureStageState(0, TSS_ALPHAARG1, TA_TEXTURE);
        dev.SetTexture(0, ground);
        {   // left: lit ground
            VtxMesh q[4] = {{-12, 0, -8, 0, 1, 0, 0xFFFFFFFF, 0, 0}, {0, 0, -8, 0, 1, 0, 0xFFFFFFFF, 3, 0},
                            {0, 0, 12, 0, 1, 0, 0xFFFFFFFF, 3, 5}, {-12, 0, 12, 0, 1, 0, 0xFFFFFFFF, 0, 5}};
            dev.SetRenderState(RS_LIGHTING, 1);
            dev.DrawPrimitive(TriangleFan, kFvfMesh, q, 4);
        }
        // right: terrain base pass, then lightmap + lights multiplying it
        dev.SetRenderState(RS_LIGHTING, 0);
        dev.SetTextureStageState(0, TSS_COLOROP, TOP_SELECTARG1);
        dev.DrawIndexedPrimitive(TriangleList, kFvfTerrain, terrain.data(), uint32_t(terrain.size()), tidx.data(), uint32_t(tidx.size()));
        dev.SetRenderState(RS_LIGHTING, 1);
        dev.SetRenderState(RS_AMBIENT, 0);
        dev.SetRenderState(RS_ALPHABLENDENABLE, 1);
        dev.SetRenderState(RS_SRCBLEND, BLEND_ZERO);
        dev.SetRenderState(RS_DESTBLEND, BLEND_SRCCOLOR);
        dev.SetRenderState(RS_ZFUNC, CMP_EQUAL);
        dev.SetTexture(0, lightmap);
        dev.SetTextureStageState(0, TSS_COLOROP, TOP_ADD);
        dev.SetTextureStageState(0, TSS_TEXCOORDINDEX, 1);
        dev.DrawIndexedPrimitive(TriangleList, kFvfTerrain, terrain.data(), uint32_t(terrain.size()), tidx.data(), uint32_t(tidx.size()));
        dev.SetTextureStageState(0, TSS_TEXCOORDINDEX, 0);
        dev.SetRenderState(RS_ZFUNC, CMP_LESSEQUAL);
        dev.SetRenderState(RS_ALPHABLENDENABLE, 0);
        dev.SetRenderState(RS_AMBIENT, 0xFF181818);
        if (g_slabTest) {
            // Like the city floors: a static platform (XYZ | NORMAL | TEX1) blended SRCALPHA/INVSRCALPHA with depth
            // writes, its top 1 unit below the lamp, lit by material (ambient 0x6F).
            struct VtxSlab { float x, y, z, nx, ny, nz, u, v; };
            const uint32_t kFvfSlab = FVF_XYZ | FVF_NORMAL | (1 << 8);
            std::vector<VtxSlab> sv;
            std::vector<uint16_t> si;
            const float mn[3] = {-9, -18, -6}, mx[3] = {2, 0.2f, 8};
            for (int axis = 0; axis < 3; ++axis)
                for (int side = 0; side < 2; ++side) {
                    int u = (axis + 1) % 3, w = (axis + 2) % 3;
                    uint16_t base = uint16_t(sv.size());
                    for (int k = 0; k < 4; ++k) {
                        float p[3], n[3] = {0, 0, 0};
                        p[axis] = side ? mx[axis] : mn[axis];
                        p[u] = (k == 1 || k == 2) ? mx[u] : mn[u];
                        p[w] = k >= 2 ? mx[w] : mn[w];
                        n[axis] = side ? 2.0f : -2.0f;    // the city platforms' normals are 2 long
                        sv.push_back({p[0], p[1], p[2], n[0], n[1], n[2], p[u] / 3, p[w] / 3});
                    }
                    uint16_t q[6] = {0, 1, 2, 0, 2, 3};
                    for (uint16_t i : q) si.push_back(uint16_t(base + i));
                }
            dev.SetRenderState(RS_AMBIENT, 0xFF6F6F6F);
            dev.SetRenderState(RS_ALPHABLENDENABLE, 1);
            dev.SetRenderState(RS_SRCBLEND, BLEND_SRCALPHA);
            dev.SetRenderState(RS_DESTBLEND, BLEND_INVSRCALPHA);
            dev.SetTexture(0, ground);
            dev.SetTextureStageState(0, TSS_COLOROP, TOP_MODULATE);
            dev.DrawIndexedPrimitive(TriangleList, kFvfSlab, sv.data(), uint32_t(sv.size()), si.data(), uint32_t(si.size()));
            dev.SetRenderState(RS_ALPHABLENDENABLE, 0);
            dev.SetRenderState(RS_AMBIENT, 0xFF181818);
            // A character carrying the lamp at head height: its origin 2 below the light. Its own light neither
            // lights it nor makes it cast.
            std::vector<VtxMesh> cv;
            std::vector<uint16_t> ci;
            AddCube(cv, ci, 0.0f, 0.9f, 0.0f, 0.4f);
            Matrix carrier = Offset();
            carrier.m[3][0] += lamp.position.x - g_off[0] + 0.4f;   // the game puts the light 0.4 off while moving
            carrier.m[3][1] += lamp.position.y - g_off[1] - 2.0f;
            carrier.m[3][2] += lamp.position.z - g_off[2];
            dev.SetTexture(0, nullptr);
            dev.SetTextureStageState(0, TSS_COLOROP, TOP_SELECTARG2);
            dev.SetTransform(World, carrier);
            dev.DrawIndexedPrimitive(TriangleList, kFvfMesh, cv.data(), uint32_t(cv.size()), ci.data(), uint32_t(ci.size()));
            dev.SetTransform(World, Offset());
            dev.SetTexture(0, ground);
            dev.SetTextureStageState(0, TSS_COLOROP, TOP_MODULATE);
        }
        // Cubes all around the lamp, one draw each; the last is the lamp's housing (contains the light).
        dev.SetTexture(0, nullptr);
        dev.SetTextureStageState(0, TSS_COLOROP, TOP_SELECTARG2);
        const float cubes[8][4] = {{-3, 0.6f, 1, 0.6f}, {3, 0.6f, 1, 0.6f}, {0, 0.5f, 4, 0.5f}, {0, 0.5f, -2.5f, 0.5f},
                                   {-2, 0.4f, -1, 0.4f}, {1.8f, 1.0f, 3.2f, 0.3f}, {0, 1.2f, 1, 0.25f},
                                   {sun ? 5.0f : 100.0f, 1.5f, -5.5f, 1.5f}};   // a pillar's top: its sun shadow crosses the lamp's
        for (auto& c : cubes) {
            std::vector<VtxMesh> v;
            std::vector<uint16_t> idx;
            AddCube(v, idx, c[0], c[1], c[2], c[3]);
            dev.DrawIndexedPrimitive(TriangleList, kFvfMesh, v.data(), uint32_t(v.size()), idx.data(), uint32_t(idx.size()));
        }
        dev.EndFrame();
    }
    dev.DestroyTexture(ground);
    dev.DestroyTexture(lightmap);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l)
{
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(hwnd, msg, w, l);
}

}  // namespace

std::string g_particleMotion = "ring";   // --particle-motion ring | orbit-cw | orbit-ccw | burst | rise
bool g_particleEndFree = false;          // --particle-end-free: the effect's texture is destroyed when it ends
bool g_particleOffscreen = false;        // --particle-offscreen: a render target pass (3D + pre-transformed) comes first
float g_particleBright = 1.0f;           // --particle-bright X: the particles' colour multiplier
float g_particleTrail = 0.0f;            // --particle-trail S: motion trails (seconds)
bool g_particleWater = false;            // --particle-water: water the game's way (ProcessVertices output, FVF 0x1C4) mid-scene

// Particle test (--particle-test): an effect like the game's sparkle auras - a ring of soft additive sprites (FVF 0x142,
// drawn the way GfxVisualDiaBill draws them) that twinkle on and off - announced with ParticleEmitter, so each live
// sprite sheds GPU particles. --particles-off draws the sprites alone for comparison. --particle-end F: the effect ends
// at frame F (no longer announced or drawn); its particles must fade out on their own. An interface quad follows the 3D.
template <typename D>
void RunParticleTest(D& dev, int frames, int frameMs, const std::string& shot, const std::string& dump, bool particles,
                     int endFrame, int effects)
{
    std::vector<uint32_t> soft(32 * 32);
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) {
            float dx = (x - 15.5f) / 16.0f, dy = (y - 15.5f) / 16.0f;
            float a = std::exp(-6.0f * (dx * dx + dy * dy));
            soft[y * 32 + x] = (uint32_t(a * 255.0f + 0.5f) << 24) | 0xFFFFFF;
        }
    Texture* tex = dev.CreateTexture(32, 32, soft.data());
    Texture* offscreen = g_particleOffscreen ? dev.CreateRenderTarget(256, 256) : nullptr;
    Device::ParticleParams params;
    params.enable = particles;
    params.brightness = g_particleBright;
    params.trail = g_particleTrail;
    dev.SetParticleParams(params);
    struct VtxSprite { float x, y, z; uint32_t color; float u, v; };
    struct VtxGround { float x, y, z; uint32_t color; };
    // --particle-effects N (stress): N effects on a grid, every one of their 128 sprites alive.
    const uint32_t kN = effects > 1 ? 128 : 24;
    const int side = int(std::ceil(std::sqrt(float(effects))));
    LARGE_INTEGER freq, start, stop;
    QueryPerformanceFrequency(&freq);
    for (int frame = 0; frame < frames; ++frame) {
        if (frame == frames - 1) {
            dev.RequestScreenshot(shot);
            if (!dump.empty()) dev.RequestFrameDump(dump);
        }
        dev.BeginFrame();
        if (frame == endFrame && g_particleEndFree && tex) {   // the game releases the effect's material with it
            dev.DestroyTexture(tex);
            tex = nullptr;
        }
        if (offscreen) {                         // like a refraction pass: 3D, then a pre-transformed quad, elsewhere
            dev.SetRenderTarget(offscreen);
            dev.Clear(CLEAR_TARGET | CLEAR_ZBUFFER, 0xFF000000, 1.0f);
            dev.SetTransform(View, LookAtLH({0, 2.2f, -4.5f}, {0, 0.9f, 0}, {0, 1, 0}));
            dev.SetTransform(Projection, PerspectiveLH(kPi / 3, 1.0f, 0.1f, 100.0f));
            dev.SetTransform(World, Identity());
            dev.SetTexture(0, nullptr);
            struct V3 { float x, y, z; uint32_t c; } tri[3] = {{-1, 0, 0, 0xFF808080}, {1, 0, 0, 0xFF808080}, {0, 1, 0, 0xFF808080}};
            dev.DrawPrimitive(TriangleList, FVF_XYZ | FVF_DIFFUSE, tri, 3);
            struct V2 { float x, y, z, rhw; uint32_t c; } q[3] = {{0, 0, 0, 1, 0xFF000000}, {10, 0, 0, 1, 0xFF000000}, {0, 10, 0, 1, 0xFF000000}};
            dev.DrawPrimitive(TriangleList, FVF_XYZRHW | FVF_DIFFUSE, q, 3);
            dev.SetRenderTarget(nullptr);
        }
        dev.SetViewport({0, 0, kWidth, kHeight, 0.0f, 1.0f});
        dev.Clear(CLEAR_TARGET | CLEAR_ZBUFFER, 0xFF0C1018, 1.0f);
        Matrix view = LookAtLH({0, 2.2f, -4.5f}, {0, 0.9f, 0}, {0, 1, 0});
        dev.SetTransform(View, view);
        dev.SetTransform(Projection, PerspectiveLH(kPi / 3, float(kWidth) / kHeight, 0.1f, 100.0f));
        dev.SetTransform(World, Identity());
        dev.SetRenderState(RS_LIGHTING, 0);
        dev.SetRenderState(RS_CULLMODE, CULL_NONE);
        dev.SetRenderState(RS_ZENABLE, 1);
        dev.SetRenderState(RS_ZFUNC, CMP_LESSEQUAL);
        // Ground (depth writing: the world camera the particles face).
        dev.SetRenderState(RS_ZWRITEENABLE, 1);
        dev.SetRenderState(RS_ALPHABLENDENABLE, 0);
        dev.SetTexture(0, nullptr);
        dev.SetTextureStageState(0, TSS_COLOROP, TOP_SELECTARG1);
        dev.SetTextureStageState(0, TSS_COLORARG1, TA_DIFFUSE);
        dev.SetTextureStageState(0, TSS_ALPHAOP, TOP_SELECTARG1);
        dev.SetTextureStageState(0, TSS_ALPHAARG1, TA_DIFFUSE);
        VtxGround ground[4] = {{-4, 0, -3, 0xFF303440}, {4, 0, -3, 0xFF303440}, {-4, 0, 5, 0xFF202430}, {4, 0, 5, 0xFF202430}};
        uint16_t gi[6] = {0, 1, 2, 1, 2, 3};
        dev.DrawIndexedPrimitive(TriangleList, FVF_XYZ | FVF_DIFFUSE, ground, 4, gi, 6);
        if (g_particleWater) {                   // pre-transformed water between the 3D draws, as VisualLiquid_t does
            struct VW { float x, y, z, rhw; uint32_t c, s; float u, v; } water[4] = {
                {0, 700, 0.9f, 1, 0x80204060, 0, 0, 0}, {float(kWidth), 700, 0.9f, 1, 0x80204060, 0, 1, 0},
                {0, float(kHeight), 0.9f, 1, 0x80204060, 0, 0, 1}, {float(kWidth), float(kHeight), 0.9f, 1, 0x80204060, 0, 1, 1}};
            dev.SetRenderState(RS_ZWRITEENABLE, 0);
            dev.DrawIndexedPrimitive(TriangleList, FVF_XYZRHW | FVF_DIFFUSE | FVF_SPECULAR | (1u << 8), water, 4, gi, 6);
            dev.SetRenderState(RS_ZWRITEENABLE, 1);
        }
        if (frame == 10)
            QueryPerformanceCounter(&start);

        for (int e = 0; e < effects; ++e) {
        // The effect: its frame of reference moves (a walking character), sprites twinkle in its ring.
        float t = frame * 0.016f;
        float gx = effects > 1 ? (float(e % side) - 0.5f * float(side - 1)) * 0.9f : 0.0f;
        float gz = effects > 1 ? float(e / side) * 0.9f : 0.0f;
        float cx = std::sin(t * 0.7f) * 0.8f + gx, cz = gz;
        Device::ParticleSprite sprites[128] = {};
        std::vector<VtxSprite> verts;
        std::vector<uint16_t> idx;
        float right[3] = {view.m[0][0], view.m[1][0], view.m[2][0]}, up[3] = {view.m[0][1], view.m[1][1], view.m[2][1]};
        for (uint32_t i = 0; i < kN; ++i) {
            float a = 2 * kPi * i / kN + t * 0.5f;
            Device::ParticleSprite& sp = sprites[i];
            sp.pos[0] = cx + std::cos(a) * 1.0f;
            sp.pos[1] = 1.0f + 0.3f * std::sin(a * 3.0f);
            sp.pos[2] = cz + std::sin(a) * 1.0f;
            sp.size = 0.6f;
            sp.uv[0] = 0; sp.uv[1] = 0; sp.uv[2] = 1; sp.uv[3] = 1;
            sp.color = 0xC0B080FF;
            sp.alive = effects > 1 || ((frame / 12 + i) % 3) != 0;
            const std::string& mo = g_particleMotion;
            if (mo == "orbit-cw" || mo == "orbit-ccw") {          // a fast ring, all alive
                float b = 2 * kPi * i / kN + (mo == "orbit-cw" ? -2.5f : 2.5f) * t;
                sp.pos[0] = cx + std::cos(b);
                sp.pos[1] = 1.0f;
                sp.pos[2] = cz + std::sin(b);
                sp.alive = 1;
            } else if (mo == "burst") {                            // flung out from the middle every 1.2 s
                float phase = std::fmod(t, 1.2f), r = 0.1f + phase * 2.5f, el = 0.6f * std::sin(float(i) * 2.3f);
                sp.pos[0] = cx + std::cos(a - t * 0.5f) * std::cos(el) * r;
                sp.pos[1] = 1.0f + std::sin(el) * r;
                sp.pos[2] = cz + std::sin(a - t * 0.5f) * std::cos(el) * r;
                sp.alive = phase < 0.9f;
            } else if (mo == "rise") {                             // a fountain column, rising 2 units a second
                float phase = std::fmod(t * 0.9f + float(i) / kN, 1.0f), b = 2 * kPi * i / kN;
                sp.pos[0] = cx + std::cos(b) * 0.5f;
                sp.pos[1] = phase * 2.2f;
                sp.pos[2] = cz + std::sin(b) * 0.5f;
                sp.alive = 1;
            }
            if (!sp.alive) continue;
            uint16_t base = uint16_t(verts.size());
            float h = sp.size * 0.5f;
            for (int k = 0; k < 4; ++k) {
                float sx = (k & 1) ? h : -h, sy = (k & 2) ? h : -h;
                verts.push_back({sp.pos[0] + right[0] * sx + up[0] * sy, sp.pos[1] + right[1] * sx + up[1] * sy,
                                 sp.pos[2] + right[2] * sx + up[2] * sy, sp.color, (k & 1) ? 1.0f : 0.0f, (k & 2) ? 0.0f : 1.0f});
            }
            uint16_t q[6] = {base, uint16_t(base + 1), uint16_t(base + 2), uint16_t(base + 1), uint16_t(base + 2), uint16_t(base + 3)};
            idx.insert(idx.end(), q, q + 6);
        }
        // As the game side does: the centre = the live sprites' middle, the origin = the effect's frame of reference.
        float center[3] = {0, 0, 0}, origin[3] = {cx, 0.0f, cz};
        uint32_t live = 0;
        for (uint32_t i = 0; i < kN; ++i)
            if (sprites[i].alive) { for (int c = 0; c < 3; ++c) center[c] += sprites[i].pos[c]; ++live; }
        for (int c = 0; c < 3; ++c) center[c] = live ? center[c] / float(live) : origin[c];
        bool effect = endFrame < 0 || frame < endFrame;
        if (effect) dev.ParticleEmitter(0x5EED + uint64_t(e), center, origin, sprites, kN);
        dev.SetRenderState(RS_ZWRITEENABLE, 0);
        dev.SetRenderState(RS_ALPHABLENDENABLE, 1);
        dev.SetRenderState(RS_SRCBLEND, BLEND_SRCALPHA);
        dev.SetRenderState(RS_DESTBLEND, BLEND_ONE);
        dev.SetTexture(0, tex);
        dev.SetTextureStageState(0, TSS_COLOROP, TOP_MODULATE);
        dev.SetTextureStageState(0, TSS_COLORARG1, TA_TEXTURE);
        dev.SetTextureStageState(0, TSS_COLORARG2, TA_DIFFUSE);
        dev.SetTextureStageState(0, TSS_ALPHAOP, TOP_MODULATE);
        dev.SetTextureStageState(0, TSS_ALPHAARG1, TA_TEXTURE);
        dev.SetTextureStageState(0, TSS_ALPHAARG2, TA_DIFFUSE);
        if (effect && !verts.empty())
            dev.DrawIndexedPrimitive(TriangleList, Device::kParticleFvf, verts.data(), uint32_t(verts.size()), idx.data(),
                                     uint32_t(idx.size()));
        dev.EndParticleEmitter();
        }
        // Interface: a pre-transformed quad (the end of the 3D scene).
        struct VtxUi { float x, y, z, rhw; uint32_t color; };
        VtxUi ui[4] = {{20, 20, 0, 1, 0xFF4080C0}, {120, 20, 0, 1, 0xFF4080C0}, {20, 60, 0, 1, 0xFF4080C0}, {120, 60, 0, 1, 0xFF4080C0}};
        dev.SetRenderState(RS_ZENABLE, 0);
        dev.SetRenderState(RS_ALPHABLENDENABLE, 0);
        dev.SetTexture(0, nullptr);
        dev.SetTextureStageState(0, TSS_COLOROP, TOP_SELECTARG1);
        dev.SetTextureStageState(0, TSS_COLORARG1, TA_DIFFUSE);
        dev.SetTextureStageState(0, TSS_ALPHAOP, TOP_SELECTARG1);
        dev.SetTextureStageState(0, TSS_ALPHAARG1, TA_DIFFUSE);
        dev.DrawIndexedPrimitive(TriangleList, FVF_XYZRHW | FVF_DIFFUSE, ui, 4, gi, 6);
        dev.EndFrame();
        if (frameMs > 0)
            Sleep(DWORD(frameMs));
    }
    if constexpr (requires { dev.Sync(); }) dev.Sync();     // the threaded device: its work done
    QueryPerformanceCounter(&stop);
    if (frames > 10)
        std::printf("particle test: %d effects, %.2f ms/frame over the last %d frames\n", effects,
                    1000.0 * double(stop.QuadPart - start.QuadPart) / double(freq.QuadPart) / (frames - 10), frames - 10);
    if (tex) dev.DestroyTexture(tex);
    if (offscreen) dev.DestroyTexture(offscreen);
}

int main(int argc, char** argv)
{
    bool windowed = false, stress = false, threaded = false, pixelLighting = false, lightingDebug = false, lightOverride = false, shadows = false, shadowTest = false, pointShadowTest = false, pointShadowSun = false;
    int cacheTest = 0, frameMs = 0;
    bool particleTest = false, particlesOff = false;
    int particleEnd = -1, particleEffects = 1;
    uint32_t pointShadows = 4;
    float headroom = 1.0f;
    double fadeIn = 0.0;
    bool hdr = false;
    float bloom = 0.0f, effectGlow = 1.0f, ao = 0.0f, aoRadius = 1.5f, bump = 0.0f;
    uint32_t anisotropy = 1;
    float motionBlur = 0.0f, dof = 0.0f, dofFocus = 0.0f;
    bool dofBokeh = true, dofFar = true;
    uint32_t motionMode = 0;
    float knee = 0.85f;
    int bench = 0;
    int frames = 3;
    std::string shot = "rvk_demo.bmp", dump;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--window") windowed = true;
        else if (a == "--stress") stress = true;
        else if (a == "--threaded") threaded = true;
        else if (a == "--pixel-lighting") pixelLighting = true;
        else if (a == "--lighting-debug") lightingDebug = true;
        else if (a == "--light-override") lightOverride = true;
        else if (a == "--shadows") shadows = true;
        else if (a == "--shadow-test") shadowTest = shadows = true;
        else if (a == "--point-shadow-test") pointShadowTest = true;
        else if (a == "--world-offset" && i + 3 < argc) { for (int k = 0; k < 3; ++k) g_off[k] = float(std::atof(argv[++i])); }
        else if (a == "--point-shadow-slab") g_slabTest = pointShadowTest = pointShadowSun = shadows = true;
        else if (a == "--point-shadow-sun") pointShadowTest = pointShadowSun = shadows = true;
        else if (a == "--frame-ms" && i + 1 < argc) frameMs = std::atoi(argv[++i]);
        else if (a == "--cache-test" && i + 1 < argc) { cacheTest = std::atoi(argv[++i]); shadowTest = shadows = true; }
        else if (a == "--dump" && i + 1 < argc) dump = argv[++i];
        else if (a == "--bench" && i + 1 < argc) bench = std::atoi(argv[++i]);
        else if (a == "--game-ms" && i + 1 < argc) g_benchGameMs = std::atof(argv[++i]);
        else if (a == "--frames" && i + 1 < argc) frames = std::atoi(argv[++i]);
        else if (a == "--hdr") hdr = true;
        else if (a == "--dof" && i + 1 < argc) { dof = float(std::atof(argv[++i])); hdr = true; }
        else if (a == "--dof-smooth") dofBokeh = false;
        else if (a == "--dof-no-far") dofFar = false;
        else if (a == "--dof-focus" && i + 1 < argc) dofFocus = float(std::atof(argv[++i]));
        else if (a == "--motion-blur" && i + 1 < argc) { motionBlur = float(std::atof(argv[++i])); hdr = true; }
        else if (a == "--motion-mode" && i + 1 < argc) motionMode = uint32_t(std::atoi(argv[++i]));
        else if (a == "--deform-cube" && i + 1 < argc) g_deformCube = float(std::atof(argv[++i]));
        else if (a == "--moving-cube" && i + 1 < argc) g_movingCube = float(std::atof(argv[++i]));
        else if (a == "--camera-yaw" && i + 1 < argc) g_cameraYaw = float(std::atof(argv[++i]));
        else if (a == "--aniso" && i + 1 < argc) anisotropy = uint32_t(std::atoi(argv[++i]));
        else if (a == "--bump" && i + 1 < argc) bump = float(std::atof(argv[++i]));
        else if (a == "--ao" && i + 1 < argc) { ao = float(std::atof(argv[++i])); hdr = true; }
        else if (a == "--ao-radius" && i + 1 < argc) aoRadius = float(std::atof(argv[++i]));
        else if (a == "--effect-glow" && i + 1 < argc) effectGlow = float(std::atof(argv[++i]));
        else if (a == "--bloom" && i + 1 < argc) { bloom = float(std::atof(argv[++i])); hdr = true; }
        else if (a == "--tonemap-knee" && i + 1 < argc) knee = float(std::atof(argv[++i]));
        else if (a == "--point-shadow-fade" && i + 1 < argc) fadeIn = std::atof(argv[++i]);
        else if (a == "--light-headroom" && i + 1 < argc) headroom = float(std::atof(argv[++i]));
        else if (a == "--point-shadows" && i + 1 < argc) pointShadows = uint32_t(std::atoi(argv[++i]));
        else if (a == "--shot" && i + 1 < argc) shot = argv[++i];
        else if (a == "--particle-test") particleTest = true;
        else if (a == "--particles-off") particlesOff = true;
        else if (a == "--particle-end" && i + 1 < argc) particleEnd = std::atoi(argv[++i]);
        else if (a == "--particle-effects" && i + 1 < argc) particleEffects = std::atoi(argv[++i]);
        else if (a == "--particle-motion" && i + 1 < argc) g_particleMotion = argv[++i];
        else if (a == "--particle-end-free") g_particleEndFree = true;
        else if (a == "--particle-offscreen") g_particleOffscreen = true;
        else if (a == "--particle-water") g_particleWater = true;
        else if (a == "--particle-bright" && i + 1 < argc) g_particleBright = float(std::atof(argv[++i]));
        else if (a == "--particle-trail" && i + 1 < argc) g_particleTrail = float(std::atof(argv[++i]));
    }

    HWND hwnd = nullptr;
    if (windowed) {
        WNDCLASSA wc{};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.lpszClassName = "rvk_demo";
        wc.hCursor = LoadCursorA(nullptr, (LPCSTR)IDC_ARROW);
        RegisterClassA(&wc);
        RECT r{0, 0, LONG(kWidth), LONG(kHeight)};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        hwnd = CreateWindowA("rvk_demo", "rvk demo", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    }

    Device dev;
    std::string error;
    if (!dev.Init(hwnd, kWidth, kHeight, &error)) {
        std::printf("rvk init failed: %s\n", error.c_str());
        return 1;
    }
    dev.SetPixelLighting(pixelLighting);
    dev.SetLightingDebug(lightingDebug);
    dev.SetLightOverride(lightOverride);
    dev.SetShadows(shadows);
    dev.SetLightHeadroom(headroom);
    dev.SetHdr(hdr);
    dev.SetTonemap(knee, 1.0f);
    dev.SetBloom(bloom, 1.0f);
    dev.SetEffectGlow(effectGlow);
    dev.SetAo(ao, aoRadius);
    dev.SetBump(bump);
    dev.SetAnisotropy(anisotropy);
    dev.SetMotionBlur(motionBlur, 8.0f);
    dev.SetMotionBlurMode(motionMode);
    dev.SetDof(dof > 0.0f, dofBokeh, true, dof, 16.0f, dofFocus, 0.2f, dofFar, 3.0f);
    dev.SetPointShadowFadeIn(fadeIn);    // frames here are milliseconds apart: no fade-in unless asked
    if (pointShadowTest) {               // needs per-pixel lighting with the light override
        dev.SetPixelLighting(true);
        dev.SetLightOverride(true);
        dev.SetPointShadows(pointShadows);
        RunPointShadowTest(dev, frames, shot, dump, pointShadowSun);
        std::printf("rendered; screenshot %s\n", shot.c_str());
        return 0;
    }
    if (particleTest) {
        if (threaded) {
            ThreadedDevice tdev;
            if (!tdev.Init(nullptr, kWidth, kHeight, &error)) { std::printf("init failed: %s\n", error.c_str()); return 1; }
            tdev.SetHdr(hdr);
            tdev.SetBloom(bloom, 1.0f);
            RunParticleTest(tdev, frames, frameMs, shot, dump, !particlesOff, particleEnd, particleEffects);
            tdev.Sync();
        } else {
            RunParticleTest(dev, frames, frameMs, shot, dump, !particlesOff, particleEnd, particleEffects);
        }
        std::printf("rendered; screenshot %s\n", shot.c_str());
        return 0;
    }
    if (!dump.empty() && !shadowTest) dev.RequestFrameDump(dump);
    std::printf("GPU: %s (Vulkan %u.%u), driver %s\n", dev.Info().gpu.c_str(), VK_API_VERSION_MAJOR(dev.Info().apiVersion),
                VK_API_VERSION_MINOR(dev.Info().apiVersion), dev.Info().driver.c_str());

    if (bench && threaded) {
        // The benchmark through rvk::ThreadedDevice: the time measured is what stays on the calling thread.
        ThreadedDevice tdev;
        std::string err;
        if (!tdev.Init(nullptr, kWidth, kHeight, &err)) { std::printf("init failed: %s\n", err.c_str()); return 1; }
        double ms = Benchmark(tdev, bench);
        std::printf("bench threaded: frame %.3f ms, of which recording %.3f ms (%.2f us per draw), game work %.1f ms\n", ms,
                    g_benchRecordMs, g_benchRecordMs * 1000.0 / 2450, g_benchGameMs);
        return 0;
    }
    if (bench) {
        double ms = Benchmark(dev, bench);
        std::printf("bench direct:   frame %.3f ms, of which recording %.3f ms (%.2f us per draw), game work %.1f ms\n", ms,
                    g_benchRecordMs, g_benchRecordMs * 1000.0 / 2450, g_benchGameMs);
        return 0;
    }
    if (threaded) {
        ThreadedDevice tdev;
        if (!tdev.Init(hwnd, kWidth, kHeight, &error)) { std::printf("init failed: %s\n", error.c_str()); return 1; }
        tdev.SetPixelLighting(pixelLighting);
        tdev.SetLightingDebug(lightingDebug);
        tdev.SetLightOverride(lightOverride);
        tdev.SetShadows(shadows);
        if (!dump.empty() && !shadowTest) tdev.RequestFrameDump(dump);
        if (shadowTest) RunShadowTest(tdev, frames, shot, cacheTest, frameMs, dump);
        else RunDemo(tdev, windowed, stress, frames, shot);
    } else if (shadowTest) {
        RunShadowTest(dev, frames, shot, cacheTest, frameMs, dump);
    } else {
        RunDemo(dev, windowed, stress, frames, shot);
    }
    std::printf("rendered; screenshot %s\n", windowed ? "(none)" : shot.c_str());
    return 0;
}
