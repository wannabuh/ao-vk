// rvk test scenes. Draws a 3x3 grid of tiles, each exercising part of the D3D7 feature set the game uses,
// through rvk's D3D7-shaped API only.
//
//   rvk_demo.exe [--window] [--frames N] [--shot out.bmp]
// Headless by default: renders N frames (default 3) and writes the last one to --shot (default rvk_demo.bmp).
// --window opens a window and animates until it is closed.
#include "rvk.h"
#include "threaded.h"
#include "material_maps.h"
#include "portal_file.h"

#include <chrono>
#include <memory>
#include <utility>
#include <thread>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace rvk;
using namespace rvk::d3d;

namespace {

constexpr uint32_t kTile = 320, kCols = 3, kRows = 3;
uint32_t g_width = kTile * kCols, g_height = kTile * kRows;   // --size W H (benchmarks at a game's resolution)
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

// A cloud puff's texture, as the game's single_cloud*.png: white, a cumulus silhouette in the alpha - a few round
// lumps on a flatter base, their edges broken up.
std::vector<uint32_t> CloudPuff(uint32_t size, uint32_t seed)
{
    auto hash = [](int x, int y, uint32_t s) {
        uint32_t h = uint32_t(x) * 73856093u ^ uint32_t(y) * 19349663u ^ s * 83492791u;
        h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
        return float(h & 0xFFFF) / 65535.0f;
    };
    auto value = [&](float x, float y, uint32_t s) {
        int ix = int(std::floor(x)), iy = int(std::floor(y));
        float fx = x - ix, fy = y - iy;
        fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
        float a = hash(ix, iy, s), b = hash(ix + 1, iy, s), c = hash(ix, iy + 1, s), d = hash(ix + 1, iy + 1, s);
        return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
    };
    struct Lump { float x, y, r; };
    Lump lumps[5];
    for (int i = 0; i < 5; ++i)
        lumps[i] = {-0.55f + 0.27f * i + 0.1f * (hash(i, 1, seed) - 0.5f), -0.1f + 0.25f * hash(i, 2, seed),
                    0.22f + 0.18f * hash(i, 3, seed) * (i == 1 || i == 2 ? 1.6f : 1.0f)};
    std::vector<uint32_t> p(size * size);
    for (uint32_t y = 0; y < size; ++y)
        for (uint32_t x = 0; x < size; ++x) {
            float u = (x + 0.5f) / size * 2 - 1, v = 1 - (y + 0.5f) / size * 2;   // v up
            float cover = 0.0f;
            for (const Lump& l : lumps) {
                float d = std::sqrt((u - l.x) * (u - l.x) + (v - l.y) * (v - l.y) * 1.3f) / l.r;
                cover = std::max(cover, 1.0f - d);
            }
            cover = std::min(cover, (v + 0.45f) * 3.0f);                        // a flat base
            float n = value(u * 6 + 11, v * 6 + 7, seed) * 0.6f + value(u * 14, v * 14, seed + 1) * 0.4f;
            float a = std::clamp((cover + (n - 0.5f) * 0.35f) * 3.0f, 0.0f, 1.0f);
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
        dev.SetViewport({0, 0, g_width, g_height, 0, 1});
        dev.Clear(CLEAR_TARGET | CLEAR_ZBUFFER, 0xFF102030, 1.0f);
        dev.SetRenderState(RS_LIGHTING, 1);
        dev.SetTransform(View, LookAtLH({0, 2, -10}, {0, 0, 0}, {0, 1, 0}));
        dev.SetTransform(Projection, PerspectiveLH(kPi / 3, float(g_width) / g_height, 0.1f, 100.0f));
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

bool g_readbackTest = false;    // --readback-test: RunReadbackTest

// --readback-test: texture levels read back from the GPU exactly as they were uploaded (ReadTextureLevel - a game
// texture whose CPU copy was given up is read back so when the game locks it again): every format the game uses,
// compressed too, every mip level; uploaded between frames and inside one, read back both ways.
template <typename D>
bool RunReadbackTest(D& dev)
{
    const Format formats[] = {Format::A8R8G8B8, Format::R5G6B5, Format::A4R4G4B4, Format::A1R5G5B5, Format::L8,
                              Format::A8L8, Format::DXT1, Format::DXT3, Format::DXT5};
    const uint32_t w = 64, h = 32, levels = 7;
    uint32_t seed = 12345, failed = 0, checked = 0;
    auto bytesOf = [&](Format f, uint32_t l) {
        return size_t(FormatRowBytes(f, std::max(1u, w >> l))) * FormatRows(f, std::max(1u, h >> l));
    };
    for (int pass = 0; pass < 2; ++pass)          // 0: uploaded between frames, 1: inside a frame
        for (Format f : formats) {
            Texture* t = dev.CreateTexture(w, h, f, levels);
            std::vector<std::vector<uint8_t>> data(levels);
            if (pass) dev.BeginFrame();
            for (uint32_t l = 0; l < levels; ++l) {
                data[l].resize(bytesOf(f, l));
                for (uint8_t& b : data[l]) b = uint8_t((seed = seed * 1103515245u + 12345u) >> 16);
                dev.UpdateTexture(t, l, 0, 0, std::max(1u, w >> l), std::max(1u, h >> l), data[l].data(),
                                  FormatRowBytes(f, std::max(1u, w >> l)));
            }
            for (int inFrame = 0; inFrame < 2; ++inFrame) {   // read back: as it is (between or in), then the other
                if (inFrame && !pass) dev.BeginFrame();
                if (!inFrame && pass) dev.EndFrame();
                for (uint32_t l = 0; l < levels; ++l) {
                    std::vector<uint8_t> back(data[l].size(), 0xEE);
                    const bool read = dev.ReadTextureLevel(t, l, back.data(), back.size());
                    ++checked;
                    if (!read || back != data[l]) {
                        ++failed;
                        std::printf("readback: FAIL format %u level %u (%s, uploaded %s, read %s)\n", unsigned(f), l,
                                    read ? "differs" : "not read", pass ? "in a frame" : "between frames",
                                    (inFrame == 0) == (pass == 0) ? "the same way" : "the other way");
                    }
                }
            }
            if (!pass) dev.EndFrame();
            dev.DestroyTexture(t);
        }
    std::printf("readback: %u of %u levels read back exactly\n", checked - failed, checked);
    return failed == 0;
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
            // 12000 draws, each its own material: more distinct constants a frame than the arenas start with (a busy
            // zone's), so they grow - and must stay inside the ring (a player's 71 MB of them in 64 MB wrote past it).
            VtxRhwDiffuse dot[3] = {{0, 0, 0, 1, 0x00FFFFFF}, {1, 0, 0, 1, 0x00FFFFFF}, {0, 1, 0, 1, 0x00FFFFFF}};
            for (int i = 0; i < 12000; ++i) {
                Material m{{float(i % 97) / 97.0f, float(i % 89) / 89.0f, 1, 1}, {1, 1, 1, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}, 1};
                dev.SetMaterial(m);
                dev.DrawPrimitive(TriangleList, kFvfRhwDiffuse, dot, 3);
            }
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

// Normal map test (--normal-map-test [strength]): a per-pixel lit wall in four tiles, light from the upper left.
//   top left:   grey, no normal map                     top middle: grey + flat normal map (must match top left)
//   top right:  dome heights, generated normals (F_BUMP)  middle left: dome heights + the normal map computed from
//               the same heights at the same scale (F_NORMALMAP; should look like top right)
//   centre:     grey + that normal map: the domes from the normal map alone, lit on their upper left
template <typename D>
void RunNormalMapTest(D& dev, int frames, const std::string& shot, float bump)
{
    const uint32_t n = 128, cells = 4;
    std::vector<float> height(n * n);
    for (uint32_t y = 0; y < n; ++y)
        for (uint32_t x = 0; x < n; ++x) {
            float cx = (float(x % (n / cells)) + 0.5f) / (n / cells) * 2 - 1, cy = (float(y % (n / cells)) + 0.5f) / (n / cells) * 2 - 1;
            float r2 = (cx * cx + cy * cy) / 0.64f;
            height[y * n + x] = r2 < 1 ? std::sqrt(1 - r2) : 0.0f;
        }
    std::vector<uint32_t> heightPixels(n * n), flatPixels(n * n, 0xFF8080FF), normalPixels(n * n);
    for (uint32_t i = 0; i < n * n; ++i) {
        uint32_t g = uint32_t(std::lround(height[i] * 255));
        heightPixels[i] = 0xFF000000u | g << 16 | g << 8 | g;
    }
    // OpenGL-convention normals of the height field (as quantised to 8 bits, the way F_BUMP sees it), the slope in
    // brightness per texel times the bump strength (= F_BUMP's world height per texel / world size of a texel).
    auto h = [&](int x, int y) { x = (x + n) % n; y = (y + n) % n; return float(heightPixels[y * n + x] & 0xFF) / 255.0f; };
    for (uint32_t y = 0; y < n; ++y)
        for (uint32_t x = 0; x < n; ++x) {
            float du = (h(x + 1, y) - h(x - 1, y)) * 0.5f * bump, dv = (h(x, y + 1) - h(x, y - 1)) * 0.5f * bump;
            float nx = -du, ny = dv, nz = 1, len = std::sqrt(nx * nx + ny * ny + nz * nz);
            auto enc = [](float v) { return uint32_t(std::lround((v * 0.5f + 0.5f) * 255)); };
            normalPixels[y * n + x] = 0xFF000000u | enc(nx / len) << 16 | enc(ny / len) << 8 | enc(nz / len);
        }
    uint32_t grey = 0xFFB0B0B0;
    Texture* plain = dev.CreateTexture(1, 1, &grey);
    Texture* plainMapped = dev.CreateTexture(1, 1, &grey);
    Texture* heights = dev.CreateTexture(n, n, heightPixels.data());
    Texture* heightsMapped = dev.CreateTexture(n, n, heightPixels.data());
    Texture* flat = dev.CreateTexture(n, n, Format::A8R8G8B8, 1);
    dev.UpdateTexture(flat, 0, 0, 0, n, n, flatPixels.data(), n * 4);
    Texture* normals = dev.CreateTexture(n, n, Format::A8R8G8B8, 1);
    dev.UpdateTexture(normals, 0, 0, 0, n, n, normalPixels.data(), n * 4);
    Texture* greyDomes = dev.CreateTexture(1, 1, &grey);
    Texture* domeNormals = dev.CreateTexture(n, n, Format::A8R8G8B8, 1);
    dev.UpdateTexture(domeNormals, 0, 0, 0, n, n, normalPixels.data(), n * 4);
    dev.SetNormalMap(greyDomes, domeNormals);
    dev.SetNormalMap(plainMapped, flat);
    dev.SetNormalMap(heightsMapped, normals);
    dev.SetPixelLighting(true);
    dev.SetNormalMaps(true, 1.0f);

    VtxMesh quad[4] = {{-1.5f, 1.5f, 0, 0, 0, -1, 0xFFFFFFFF, 0, 0}, {1.5f, 1.5f, 0, 0, 0, -1, 0xFFFFFFFF, 1, 0},
                       {-1.5f, -1.5f, 0, 0, 0, -1, 0xFFFFFFFF, 0, 1}, {1.5f, -1.5f, 0, 0, 0, -1, 0xFFFFFFFF, 1, 1}};
    for (int frame = 0; frame < frames; ++frame) {
        if (frame == frames - 1)
            dev.RequestScreenshot(shot);
        dev.BeginFrame();
        auto tile = [&](uint32_t col, uint32_t row, Texture* tex, float tileBump) {
            dev.SetViewport({col * kTile, row * kTile, kTile, kTile, 0.0f, 1.0f});
            dev.Clear(CLEAR_TARGET | CLEAR_ZBUFFER, 0xFF101018, 1.0f);
            dev.SetBump(tileBump);
            dev.SetRenderState(RS_ZENABLE, 1);
            dev.SetRenderState(RS_ZWRITEENABLE, 1);
            dev.SetRenderState(RS_ZFUNC, CMP_LESSEQUAL);
            dev.SetRenderState(RS_CULLMODE, CULL_NONE);
            dev.SetRenderState(RS_LIGHTING, 1);
            dev.SetRenderState(RS_AMBIENT, 0xFF181818);
            dev.SetRenderState(RS_DIFFUSEMATERIALSOURCE, MCS_MATERIAL);
            Material mat{{1, 1, 1, 1}, {1, 1, 1, 1}, {0, 0, 0, 1}, {0, 0, 0, 0}, 0.0f};
            dev.SetMaterial(mat);
            Light sun{};
            sun.type = LIGHT_DIRECTIONAL;
            sun.diffuse = {1, 1, 1, 1};
            sun.direction = {0.6f, -0.6f, 0.5f};             // travels to the lower right, into the wall
            dev.SetLight(0, sun);
            dev.LightEnable(0, true);
            dev.SetTextureStageState(0, TSS_COLOROP, TOP_MODULATE);
            dev.SetTextureStageState(0, TSS_COLORARG1, TA_TEXTURE);
            dev.SetTextureStageState(0, TSS_COLORARG2, TA_DIFFUSE);
            dev.SetTextureStageState(0, TSS_ALPHAOP, TOP_SELECTARG1);
            dev.SetTextureStageState(0, TSS_ALPHAARG1, TA_TEXTURE);
            dev.SetTextureStageState(0, TSS_TEXCOORDINDEX, 0);
            dev.SetTextureStageState(0, TSS_TEXTURETRANSFORMFLAGS, TTFF_DISABLE);
            dev.SetTextureStageState(0, TSS_MAGFILTER, TFG_LINEAR);
            dev.SetTextureStageState(0, TSS_MINFILTER, TFN_LINEAR);
            dev.SetTextureStageState(0, TSS_ADDRESS, TADDRESS_WRAP);
            dev.SetTextureStageState(1, TSS_COLOROP, TOP_DISABLE);
            dev.SetTexture(0, tex);
            dev.SetTransform(World, Identity());
            dev.SetTransform(View, LookAtLH({0, 0, -4}, {0, 0, 0}, {0, 1, 0}));
            dev.SetTransform(Projection, PerspectiveLH(kPi / 3, 1.0f, 0.1f, 100.0f));
            dev.DrawPrimitive(TriangleStrip, kFvfMesh, quad, 4);
        };
        tile(0, 0, plain, 0.0f);
        tile(1, 0, plainMapped, 0.0f);
        tile(2, 0, heights, bump);
        tile(0, 1, heightsMapped, 0.0f);
        tile(1, 1, greyDomes, 0.0f);         // relief from the normal map alone: domes lit from the upper left
        dev.EndFrame();
    }
    for (Texture* t : {plain, plainMapped, heights, heightsMapped, greyDomes})
        dev.DestroyTexture(t);              // the normal maps go with their textures
}

// PBR test (--pbr-test [--hdr] [--ssr 1] [--pbr-debug N]): a grid of spheres, roughness rising to the right
// (0.05 .. 1), lit by a warm sun from the upper left and a blue point light at the lower right, over a floor.
//   row 0: the game's own lighting (no material; Blinn-Phong specular, power 20) on red; last: a grey texture
//          whose albedo map is a checker (SetMaterialMaps albedo, must show the checker); middle: glowing cyan
//          bands (an emissive map, SetEmissiveMap)
//   row 1: red non-metal      row 2: gold metal      row 3: silver metal (last column: occlusion map stripes)
// The floor has a material too: a checker, smooth-ish non-metal (roughness 0.3), reflecting the spheres with --ssr.
uint32_t g_pbrDebug = 0;      // --pbr-debug N: RVK_PbrDebug's views
bool g_pbrOff = false;        // --pbr-off: materials attached but PBR lighting off (RVK_Pbr 0)
float g_pbrProbe = 1.0f;      // --pbr-probe S: the environment probe's strength (0 = the analytic sky only)
bool g_pbrDebugLate = false;  // --pbr-debug-late: the debug view only in the last frames (the probe learns the scene)
float g_pick[2] = {-1.0f, -1.0f};   // --pick X Y: texture picking at (X, Y) of the target (0..1); prints which ball
bool g_pbrRoom = false;       // --pbr-room: unlit coloured walls around (red left, green right, blue ahead), for the
                              // environment probe: mirror balls show red on their left, green on their right
// --pbr-maps DIR: the side-loaded files, through the proxy's loader (rvk/material_maps.h): the floor gets
// floor_n.png, floor_h.png, floor_ao.png / floor_r.png / floor_m.png (separate) and floor_d.png; the bottom row's middle ball
// ball_n.png and ball_orm.png (packed). Missing files are skipped.
std::string g_pbrMaps;
bool g_pbrSelfLit = false;          // --pbr-selflit: the emissive map's ball drawn with an emissive (self-lit) material
extern float g_sunDir[3];               // (--sun-dir, below: also the PBR scene's sun - from under the ground, night)
extern bool g_sunDirSet;
float g_floorRepeats = 8.0f;        // --floor-repeats N: the PBR scene's floor texture's repeats across it (16 units)

template <typename D>
void AttachFileMaps(D& dev, Texture* tex, const std::string& stem)
{
    auto file = [&](const char* suffix) {
        std::string p = g_pbrMaps + "\\" + stem + suffix;
        return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES ? p : std::string();
    };
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    maps::MaterialFiles f;
    f.normal = file("_n.png");
    f.height = file("_h.png");
    f.packed = file("_orm.png");
    f.parts[0] = file("_ao.png");
    f.parts[1] = file("_r.png");
    f.parts[2] = file("_m.png");
    f.albedo = file("_d.png");
    f.emissive = file("_e.png");
    maps::Decoded d = maps::Decode(f);
    maps::Attach(dev, tex, d);
    QueryPerformanceCounter(&t1);
    std::printf("pbr maps: %s: orm from %s, albedo %s, emissive %s (%.1f ms)%s%s\n", stem.c_str(),
                d.ormFrom.empty() ? "(none)" : d.ormFrom.c_str(), d.albedo.Empty() ? "no" : "yes",
                d.emissive.Empty() ? "no" : "yes",
                double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(freq.QuadPart), d.errors.empty() ? "" : " errors: ",
                d.errors.c_str());
}

template <typename D>
void RunPbrTest(D& dev, int frames, const std::string& shot)
{
    const int kSeg = 32, kRing = 24;
    std::vector<VtxMesh> sphere;
    std::vector<uint16_t> sphereIdx;
    for (int r = 0; r <= kRing; ++r)
        for (int sgm = 0; sgm <= kSeg; ++sgm) {
            float th = kPi * r / kRing, ph = 2 * kPi * sgm / kSeg;
            float x = std::sin(th) * std::cos(ph), y = std::cos(th), z = std::sin(th) * std::sin(ph);
            sphere.push_back({x * 0.45f, y * 0.45f, z * 0.45f, x, y, z, 0xFFFFFFFF, float(sgm) / kSeg * 4, float(r) / kRing * 2});
        }
    for (int r = 0; r < kRing; ++r)
        for (int sgm = 0; sgm < kSeg; ++sgm) {
            uint16_t a = uint16_t(r * (kSeg + 1) + sgm), b = uint16_t(a + kSeg + 1);
            for (uint16_t i : {a, b, uint16_t(a + 1), uint16_t(a + 1), b, uint16_t(b + 1)}) sphereIdx.push_back(i);
        }
    auto solid = [&](uint32_t argb) { return dev.CreateTexture(1, 1, &argb); };
    auto orm = [&](float ao, float rough, float metal) {
        uint32_t px = 0xFF000000u | uint32_t(std::lround(ao * 255)) << 16 | uint32_t(std::lround(rough * 255)) << 8 |
                      uint32_t(std::lround(metal * 255));
        return solid(px);
    };
    const float rough[5] = {0.05f, 0.25f, 0.45f, 0.7f, 1.0f};
    const uint32_t albedo[4] = {0xFFB02020, 0xFFB02020, 0xFFE0B050, 0xFFD8D8D8};
    Texture* tex[4][5] = {};
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 5; ++col) {
            tex[row][col] = solid(albedo[row]);
            if (row > 0) {
                Texture* m = orm(1.0f, rough[col], row >= 2 ? 1.0f : 0.0f);
                if (row == 3 && col == 4) {          // occlusion stripes on a half-rough silver ball
                    dev.DestroyTexture(m);
                    std::vector<uint32_t> px(64 * 64);
                    for (uint32_t y = 0; y < 64; ++y)
                        for (uint32_t x = 0; x < 64; ++x)
                            px[y * 64 + x] = 0xFF000000u | ((x / 8) % 2 ? 0x20u : 0xFFu) << 16 | 0x80u << 8 | 0xFFu;
                    m = dev.CreateTexture(64, 64, px.data());
                }
                dev.SetMaterialMaps(tex[row][col], m, nullptr);
            }
        }
    {   // row 0, last: the albedo map replaces the grey texture
        auto checker = Checker(64, 8, 0xFFF0F0F0, 0xFF2040C0);
        dev.SetMaterialMaps(tex[0][4], nullptr, dev.CreateTexture(64, 64, checker.data()));
        std::vector<uint32_t> bands(64 * 64);  // row 0, middle: cyan bands glowing, black between
        for (uint32_t y = 0; y < 64; ++y)
            for (uint32_t x = 0; x < 64; ++x)
                bands[y * 64 + x] = (y / 4) % 4 == 0 ? 0xFF40E0FFu : 0xFF000000u;
        dev.SetEmissiveMap(tex[0][2], dev.CreateTexture(64, 64, bands.data()));
    }
    auto floorPixels = Checker(64, 8, 0xFF909090, 0xFF505050);
    Texture* floor = dev.CreateTexture(64, 64, floorPixels.data());
    dev.SetMaterialMaps(floor, orm(1.0f, 0.3f, 0.0f), nullptr);
    if (!g_pbrMaps.empty()) {
        AttachFileMaps(dev, floor, "floor");
        AttachFileMaps(dev, tex[3][2], "ball");
    }
    dev.SetPixelLighting(true);
    Device::PbrSettings pbr;
    pbr.enabled = !g_pbrOff;
    pbr.debug = g_pbrDebugLate ? 0u : g_pbrDebug;
    pbr.probe = g_pbrProbe;
    dev.SetPbr(pbr);

    Texture* wallTex[3] = {solid(0xFFC03020), solid(0xFF30B040), solid(0xFF3050C0)};
    const VtxMesh walls[3][4] = {
        {{-10, -3, -8, 1, 0, 0, 0xFFFFFFFF, 0, 0}, {-10, 12, -8, 1, 0, 0, 0xFFFFFFFF, 0, 1},
         {-10, -3, 25, 1, 0, 0, 0xFFFFFFFF, 1, 0}, {-10, 12, 25, 1, 0, 0, 0xFFFFFFFF, 1, 1}},
        {{10, -3, -8, -1, 0, 0, 0xFFFFFFFF, 0, 0}, {10, 12, -8, -1, 0, 0, 0xFFFFFFFF, 0, 1},
         {10, -3, 25, -1, 0, 0, 0xFFFFFFFF, 1, 0}, {10, 12, 25, -1, 0, 0, 0xFFFFFFFF, 1, 1}},
        {{-10, -3, 25, 0, 0, -1, 0xFFFFFFFF, 0, 0}, {-10, 12, 25, 0, 0, -1, 0xFFFFFFFF, 0, 1},
         {10, -3, 25, 0, 0, -1, 0xFFFFFFFF, 1, 0}, {10, 12, 25, 0, 0, -1, 0xFFFFFFFF, 1, 1}}};
    const float fr = g_floorRepeats;
    VtxMesh floorQuad[4] = {{-8, 0, -4, 0, 1, 0, 0xFFFFFFFF, 0, 0}, {8, 0, -4, 0, 1, 0, 0xFFFFFFFF, fr, 0},
                            {-8, 0, 12, 0, 1, 0, 0xFFFFFFFF, 0, fr}, {8, 0, 12, 0, 1, 0, 0xFFFFFFFF, fr, fr}};
    for (int frame = 0; frame < frames; ++frame) {
        if (frame == frames - 1)
            dev.RequestScreenshot(shot);
        if (g_pick[0] >= 0.0f && frame == frames - 4)
            dev.RequestPick(g_pick[0], g_pick[1]);
        if (g_pick[0] >= 0.0f && frame == frames - 2) {
            Texture* picked = dev.LastPick().texture;
            std::string what = picked == floor ? "the floor" : picked ? "another texture" : "nothing";
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 5; ++c)
                    if (picked == tex[r][c]) what = "ball row " + std::to_string(r) + " column " + std::to_string(c);
            std::printf("pick: %s (serial %u)\n", what.c_str(), dev.LastPick().serial);
            dev.SetPickHighlight(picked);
        }
        if (g_pbrDebugLate && frame == frames - 3) {
            pbr.debug = g_pbrDebug;
            dev.SetPbr(pbr);
        }
        dev.BeginFrame();
        dev.SetViewport({0, 0, g_width, g_height, 0.0f, 1.0f});
        dev.Clear(CLEAR_TARGET | CLEAR_ZBUFFER, 0xFF8098B8, 1.0f);
        dev.SetRenderState(RS_ZENABLE, 1);
        dev.SetRenderState(RS_ZWRITEENABLE, 1);
        dev.SetRenderState(RS_ZFUNC, CMP_LESSEQUAL);
        dev.SetRenderState(RS_CULLMODE, CULL_NONE);
        dev.SetRenderState(RS_LIGHTING, 1);
        dev.SetRenderState(RS_AMBIENT, 0xFF505860);
        dev.SetRenderState(RS_DIFFUSEMATERIALSOURCE, MCS_MATERIAL);
        dev.SetRenderState(RS_NORMALIZENORMALS, 1);
        dev.SetRenderState(RS_FOGENABLE, 1);
        dev.SetRenderState(RS_FOGTABLEMODE, FOG_LINEAR);
        dev.SetRenderState(RS_FOGCOLOR, 0xFF8098B8);
        float fogStart = 60.0f, fogEnd = 200.0f;
        uint32_t bits;
        std::memcpy(&bits, &fogStart, 4); dev.SetRenderState(RS_FOGSTART, bits);
        std::memcpy(&bits, &fogEnd, 4); dev.SetRenderState(RS_FOGEND, bits);
        Light sun{};
        sun.type = LIGHT_DIRECTIONAL;
        sun.diffuse = {1.0f, 0.95f, 0.85f, 1};
        sun.specular = {1, 1, 1, 1};
        sun.direction = {0.5f, -0.6f, 0.6f};
        if (g_sunDirSet) sun.direction = {g_sunDir[0], g_sunDir[1], g_sunDir[2]};   // (--sun-dir: up from under the ground is night)
        dev.SetLight(0, sun);
        dev.LightEnable(0, true);
        Light lamp{};
        lamp.type = LIGHT_POINT;
        lamp.diffuse = {0.3f, 0.5f, 1.0f, 1};
        lamp.specular = {0.3f, 0.5f, 1.0f, 1};
        lamp.position = {2.6f, -1.6f, -1.2f};
        lamp.range = 6;
        lamp.attenuation0 = 0.2f;
        lamp.attenuation1 = 0.3f;
        dev.SetLight(1, lamp);
        dev.LightEnable(1, true);
        dev.SetTextureStageState(0, TSS_COLOROP, TOP_MODULATE);
        dev.SetTextureStageState(0, TSS_COLORARG1, TA_TEXTURE);
        dev.SetTextureStageState(0, TSS_COLORARG2, TA_DIFFUSE);
        dev.SetTextureStageState(0, TSS_ALPHAOP, TOP_SELECTARG1);
        dev.SetTextureStageState(0, TSS_ALPHAARG1, TA_TEXTURE);
        dev.SetTextureStageState(0, TSS_TEXCOORDINDEX, 0);
        dev.SetTextureStageState(0, TSS_TEXTURETRANSFORMFLAGS, TTFF_DISABLE);
        dev.SetTextureStageState(0, TSS_MAGFILTER, TFG_LINEAR);
        dev.SetTextureStageState(0, TSS_MINFILTER, TFN_LINEAR);
        dev.SetTextureStageState(0, TSS_ADDRESS, TADDRESS_WRAP);
        dev.SetTextureStageState(1, TSS_COLOROP, TOP_DISABLE);
        dev.SetTransform(View, LookAtLH({0, 0.3f, -6.2f}, {0, -0.3f, 0}, {0, 1, 0}));
        dev.SetTransform(Projection, PerspectiveLH(kPi / 3, float(g_width) / g_height, 0.1f, 300.0f));
        Material plain{{1, 1, 1, 1}, {1, 1, 1, 1}, {0, 0, 0, 1}, {0, 0, 0, 0}, 0.0f};
        Material glossy{{1, 1, 1, 1}, {1, 1, 1, 1}, {0.6f, 0.6f, 0.6f, 1}, {0, 0, 0, 0}, 20.0f};
        dev.SetMaterial(plain);
        dev.SetRenderState(RS_SPECULARENABLE, 0);
        dev.SetTexture(0, floor);
        dev.SetTransform(World, Translate(0, -2.4f, 0));
        dev.DrawPrimitive(TriangleStrip, kFvfMesh, floorQuad, 4);
        if (g_pbrRoom) {
            dev.SetRenderState(RS_LIGHTING, 0);
            dev.SetTransform(World, Translate(0, 0, 0));
            for (int w = 0; w < 3; ++w) {
                dev.SetTexture(0, wallTex[w]);
                dev.DrawPrimitive(TriangleStrip, kFvfMesh, walls[w], 4);
            }
            dev.SetRenderState(RS_LIGHTING, 1);
        }
        for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 5; ++col) {
                bool legacy = row == 0 && col < 4;
                dev.SetMaterial(legacy ? glossy : plain);
                // (--pbr-selflit: the emissive map's ball self-lit, as the game's signs - an emissive material: the map
                // only says where the night glow goes, no light of its own.)
                if (g_pbrSelfLit && row == 0 && col == 2) {
                    Material lit = glossy;
                    lit.emissive = {1, 1, 1, 1};
                    dev.SetMaterial(lit);
                }
                dev.SetRenderState(RS_SPECULARENABLE, legacy ? 1 : 0);
                dev.SetTexture(0, tex[row][col]);
                dev.SetTransform(World, Translate((col - 2) * 1.1f, 1.45f - row * 1.1f, 0));
                dev.DrawIndexedPrimitive(TriangleList, kFvfMesh, sphere.data(), uint32_t(sphere.size()), sphereIdx.data(),
                                         uint32_t(sphereIdx.size()));
            }
        dev.EndFrame();
    }
    for (auto& row : tex)
        for (Texture* t : row) dev.DestroyTexture(t);   // their maps go with them
    dev.DestroyTexture(floor);
    for (Texture* t : wallTex) dev.DestroyTexture(t);
}

// Glow occlusion test (--glow-occlude-test, with --bloom S): a bright additive quad drawn first, then a solid wall in
// front of all of it - drawn after it, as the game drew a rock after the Temple of the Three Winds portal. The wall
// must hide the quad's glow too: no bloom on the wall from behind it (the wall stays as grey as its far edge).
template <typename D>
void RunGlowOccludeTest(D& dev, int frames, const std::string& shot)
{
    uint32_t white = 0xFFFFFFFF, grey = 0xFF606060;
    Texture* glowTex = dev.CreateTexture(1, 1, &white);
    Texture* wallTex = dev.CreateTexture(1, 1, &grey);
    VtxDiffuseTex glow[4] = {{-1.5f, -1.5f, 8, 0xFFFFE0A0, 0, 0}, {-1.5f, 1.5f, 8, 0xFFFFE0A0, 0, 1},
                             {1.5f, -1.5f, 8, 0xFFFFE0A0, 1, 0}, {1.5f, 1.5f, 8, 0xFFFFE0A0, 1, 1}};
    // (Vertex alpha just under 255: kept out of the depth pre-pass, as the rock was - pre-passed, its depth hid the
    // quad from the start.)
    VtxDiffuseTex wall[4] = {{-3.0f, -3.0f, 4, 0xFEFFFFFF, 0, 0}, {-3.0f, 3.0f, 4, 0xFEFFFFFF, 0, 1},
                             {3.0f, -3.0f, 4, 0xFEFFFFFF, 1, 0}, {3.0f, 3.0f, 4, 0xFEFFFFFF, 1, 1}};
    for (int frame = 0; frame < frames; ++frame) {
        if (frame == frames - 1)
            dev.RequestScreenshot(shot);
        dev.BeginFrame();
        dev.SetViewport({0, 0, g_width, g_height, 0.0f, 1.0f});
        dev.Clear(CLEAR_TARGET | CLEAR_ZBUFFER, 0xFF101418, 1.0f);
        dev.SetTransform(View, LookAtLH({0, 0, -4}, {0, 0, 0}, {0, 1, 0}));
        dev.SetTransform(Projection, PerspectiveLH(kPi / 3, float(g_width) / g_height, 0.1f, 100.0f));
        dev.SetTransform(World, Identity());
        dev.SetRenderState(RS_LIGHTING, 0);
        dev.SetRenderState(RS_CULLMODE, CULL_NONE);
        dev.SetRenderState(RS_ZENABLE, 1);
        dev.SetRenderState(RS_ZFUNC, CMP_LESSEQUAL);
        dev.SetTextureStageState(0, TSS_COLOROP, TOP_MODULATE);
        dev.SetTextureStageState(0, TSS_COLORARG1, TA_TEXTURE);
        dev.SetTextureStageState(0, TSS_COLORARG2, TA_DIFFUSE);
        dev.SetTextureStageState(0, TSS_ALPHAOP, TOP_SELECTARG1);
        dev.SetTextureStageState(0, TSS_ALPHAARG1, TA_TEXTURE);
        dev.SetTextureStageState(1, TSS_COLOROP, TOP_DISABLE);
        // The glow first: additive, no depth written.
        dev.SetRenderState(RS_ALPHABLENDENABLE, 1);
        dev.SetRenderState(RS_SRCBLEND, BLEND_SRCALPHA);
        dev.SetRenderState(RS_DESTBLEND, BLEND_ONE);
        dev.SetRenderState(RS_ZWRITEENABLE, 0);
        dev.SetTexture(0, glowTex);
        for (int k = 0; k < 4; ++k)                    // (stacked: well above the bloom's threshold)
            dev.DrawPrimitive(TriangleStrip, kFvfDiffuseTex, glow, 4);
        // Then the wall in front of it: solid, writing depth - blended (SRCALPHA / INVSRCALPHA, opaque texture) the way
        // the game draws most static objects, the rock in front of the portal too (so not in the depth pre-pass).
        dev.SetRenderState(RS_SRCBLEND, BLEND_SRCALPHA);
        dev.SetRenderState(RS_DESTBLEND, BLEND_INVSRCALPHA);
        dev.SetRenderState(RS_ZWRITEENABLE, 1);
        dev.SetTextureStageState(0, TSS_ALPHAOP, TOP_MODULATE);
        dev.SetTextureStageState(0, TSS_ALPHAARG2, TA_DIFFUSE);
        dev.SetTexture(0, wallTex);
        dev.DrawPrimitive(TriangleStrip, kFvfDiffuseTex, wall, 4);
        dev.EndFrame();
    }
    dev.DestroyTexture(glowTex);
    dev.DestroyTexture(wallTex);
}

float g_cameraYaw = 0.0f;  // --camera-yaw: the shadow test's camera turns this much a frame (motion blur)
float g_cameraPitch = -1000.0f;   // --camera-pitch DEG: the shadow test's camera looks this far up (the sky: clouds)
bool g_sunView = false;    // --sun-view: the shadow test's camera low, looking into the sun
float g_movingCube = 0.0f; // --moving-cube: the shadow test's big right cube moves this far a frame along x
float g_deformCube = 0.0f; // --deform-cube: its top vertices move this far a frame along x (like CPU skinning)
float g_grassWalk = -100.0f;  // --grass-walk X: a patch of grass and an animated "character" standing at x = X in it
float g_grassWalkSpeed = 0.0f; // --grass-walk-speed: ... walking this far along x a frame
float g_grassSize = 1.0f;      // --grass-size: the tufts' cards this many times bigger (big quads)
bool g_grassFlip = false;
bool g_walkerRound = false;
int g_walkerSkip = 0;
bool g_walkerLight = false;    // --walker-light: the round "character" carries a point light at head height and swings
                               // an arm beside its body (its own light's shadow on itself)          // --walker-skip N: the "character" animates only every N-th frame (crowds: the game skips)    // --walker-round: the "character" a low-polygon smooth-shaded 8-sided column
int g_grassDense = 1;          // --grass-dense K: K x K as many tufts, K times closer (a field, for profiling)      // --grass-flip: the tufts modelled upside down, turned up by their world matrix
bool g_grassBench = false;     // --grass-bench: RunGrassBench (a big terrain, the camera walking)
int g_grassLate = 0;               // --grass-late N: the grass switched on at frame N of the zone ground scene
bool g_grassFieldOn = false;   // --grass-field: the procedural ground grass (RVK_GrassOn) over the shadow test's terrain
float g_grassFieldDist = 25.0f;
float g_grassEven = 1.0f;         // --grass-even E: RVK_GrassEven
bool g_grassShadow = true;
int g_shinePass = -1;             // --shine-pass 0|1: the shadow test with a smooth sphere drawn as a character (tessellated
                                  // with --tess); 1: then an additive red pass over it with depth EQUAL, as the Kyr'Ozch
                                  // armour's shine
bool g_uiWindow = false;          // --ui-window: the shadow test with an interface window (the interface layer) and a
                                  // red "mouse pointer" drawn on the frame after it, over the window
bool g_hideUi = false;            // --hide-ui: the interface hidden (SetHideInterface, Ctrl+Shift+H in game)        // --grass-shadow 0|1: RVK_GrassShadow
float g_grassStyle[5] = {1, 1, 1, 1, 1};   // --grass-style V,F,G,U,T: variety, flowers, glow, gusts, trails (SetGrassStyle)
float g_grassBenchYaw = 0.0f;  // --grass-bench-yaw R: the bench's camera turned R radians (pi: into the sun)
bool g_grassBenchWalker = false;  // --grass-bench-walker: a "character" walking through the grass ahead of the camera
bool g_grassBenchCross = false;   // --grass-bench-cross: ... crossing the view from left to right over the run (with
                                  // --grass-bench-still: the trail it leaves lies across the screenshot)
bool g_grassBenchLamp = false;    // --grass-bench-lamp: a lamp just over the grass ahead (point light shadows)
float g_spotDegrees = 0.0f;       // --point-shadow-spot DEG: the point shadow test's lamp a spot light looking down
                                  // (with --grass-bench-lamp: a floodlight high over the grass, its cone DEG across)
bool g_grassBenchNight = false;   // --grass-bench-night: a dim lightmap and ambient, a faint bluish moon
float g_sunDir[3] = {}, g_sunColour[3] = {};   // --sun-dir X Y Z, --sun-colour R G B: the water scenes' and the grass
bool g_sunDirSet = false, g_sunColourSet = false;   // bench's sun
// --sun-sweep A B N (the water scenes): the sun's elevation from A to B degrees over the run (its azimuth --sun-dir's,
// else ahead of the camera), a shot sweep_NNN.bmp at each of N steps - tools/sun-sweep.py makes them a contact sheet
// and looks for jumps between them (docs/dusk-plan.md). Below the horizon the light points up, as the game's.
float g_sweepFrom = 0.0f, g_sweepTo = 0.0f;
int g_sweepSteps = 0, g_sweepHold = 6;
bool g_grassBenchSteep = false;   // --grass-bench-steep: the bench's hills steep (slopes up to ~56 degrees)
int g_grassBenchLoading = 0;      // --grass-bench-loading N: the ground's texture a plain green stand-in until frame N
bool g_grassBenchLoadingNull = false;   // --grass-bench-loading-null: ... drawn untextured instead
bool g_grassBenchPlatform = false;  // --grass-bench-platform: a flat slab lying on the ground ahead (no grass under it)
bool g_grassBenchStorm = false;   // --grass-bench-storm: AO's sandstorm tint (a depth-writing ZFUNC ALWAYS sprite)
bool g_bareGround = false;        // --bare-ground: the grass bench's ground sandy, no grass (the wet ground, puddles)
bool g_sandStorm = false;         // --sand-storm: the grass bench in a desert sandstorm (sandy ground, the game's tint,
                                  // the new weather without rain: our haze and blowing sand)
float g_sandBlow = 1.0f;          // --sand-blow X: RVK_SandBlow
uint32_t g_sandRes = 0;           // --sand-res N: RVK_SandRes
float g_windStorm = 1.0f;         // --wind-storm X: RVK_WindStorm
float g_sandForce = 0.0f;         // --sand-force X: RVK_SandForce
float g_rainSpeed = 15.0f, g_rainLength = 0.45f;   // --rain-speed S, --rain-length L: RVK_RainSpeed, RVK_RainLength
float g_wetNow = -1.0f;           // --wet W: the ground soaked (and its puddles) this much from the start
float g_stormForce = 0.0f;        // --storm F: RVK_StormForce (a thunderstorm: rain, wind, our own strikes)
float g_stormThick = 1.0f;        // --storm-thick X: RVK_StormThick
float g_fogBankForce = -1.0f;     // --fog-banks X: fog banks of that strength now (RVK_FogBanks)
bool g_heatForce = false;         // --heat-flash: a heat-lightning night (RVK_HeatFlash)
float g_strikeRate = 6.0f;        // --strike-rate R: RVK_StrikeRate (strikes a minute)
int g_lightningAt = -1;           // --lightning-at N: a strike ahead of the camera at frame N (the grass bench)
bool g_sandAsh = false;           // --sand-ash: RVK_SandAsh
bool g_grassBenchStill = false;   // --grass-bench-still: the camera stays where it starts (to watch the wind)
bool g_terrainPbr = false;        // --terrain-pbr: the bench's ground texture gets a PBR material (a glossy path through
                                  // rough grass), per-pixel lighting, no grass field: the ground's PBR (MAT_BASE) path
int g_grassBenchFilm = 0;         // --grass-bench-film N: ~60 frames a second, a screenshot every N (film_NNN.bmp)
float g_grassBenchRise = 0.0f;    // --grass-bench-rise H: the camera H units higher
float g_grassBenchLampAt = 3.0f; // --grass-bench-lamp-at D: the lamp D units ahead (5: behind the walker, which then
                                  // shades the grass between it and the camera)
float g_grassBenchBack = 0.0f;    // --grass-bench-back D: the camera D units back along its view (zoomed out: the
                                  // walker, the target and the grass round them stay where they are)
int g_grassBenchZoomAt = 0;        // --grass-bench-zoom F H: from frame F the camera H units higher instead
float g_grassBenchZoomRise = 0.0f;
float g_grassBenchPitch = -1.0f;  // --grass-bench-pitch R: the camera looking R radians down, raised to match (top-down)
bool g_grassFieldTex = true;   // --grass-field-any: over every ground, not only where the ground texture is green
bool g_grassFieldTan = false;  // --grass-field-tan: leave the terrain's tan ground (a filter that must reject it)

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

// Sand shelter (--sand-obstacles): a wall, a tree (a trunk, a see-through crown) and a boulder ahead of the bench's
// camera, as placed objects (a pack written here and loaded as the game's: statels.cpp, sand_map.cpp) and drawn as
// grey boxes; --sand-wind X Z: the storm's wind that way (the game's weather wind, each frame).
bool g_sandObstacles = false;
bool g_sandArch = false;
float g_sandGusts = 1.0f;            // --sand-gusts S: RVK_SandGusts
float g_sandFog = 1.0f;              // RVK_SandFog (--sand-haze: 0, the flat haze; --sand-fog: on, as by default)
float g_sandClear = 6.0f;            // --sand-clear D: RVK_SandClear
float g_sandContrast = 2.0f;         // --sand-contrast K: RVK_SandContrast             // --sand-arch: an arch in place of the wall
bool g_sandSim = true;               // the sand simulated (RVK_SandProc 0); --sand-proc: procedural
bool g_sandWindSet = false;
float g_sandWind[2] = {1.0f, 0.0f};
struct SandBox { float lo[3], hi[3]; bool seeThrough; };

void AddBox(std::vector<VtxMesh>& v, std::vector<uint16_t>& idx, const float lo[3], const float hi[3], uint32_t colour)
{
    struct Face { int axis; float sign; } faces[6] = {{0, -1}, {0, 1}, {1, -1}, {1, 1}, {2, -1}, {2, 1}};
    for (const Face& f : faces) {
        const uint16_t base = uint16_t(v.size());
        const int a = (f.axis + 1) % 3, b = (f.axis + 2) % 3;
        for (int k = 0; k < 4; ++k) {
            float p[3], n[3] = {0, 0, 0};
            p[f.axis] = f.sign < 0 ? lo[f.axis] : hi[f.axis];
            p[a] = (k == 1 || k == 2) ? hi[a] : lo[a];
            p[b] = k >= 2 ? hi[b] : lo[b];
            n[f.axis] = f.sign;
            v.push_back({p[0], p[1], p[2], n[0], n[1], n[2], colour, 0, 0});
        }
        const uint16_t q[6] = {0, 1, 2, 0, 2, 3};
        for (uint16_t i : q) idx.push_back(uint16_t(base + i));
    }
}

// The boxes as a statel pack (statels.cpp's format, version 2): a mesh of one piece each, placed at the origin.
bool WriteSandPack(const char* path, const std::vector<SandBox>& boxes)
{
    std::vector<uint8_t> out;
    auto put = [&](const void* p, size_t n) { const uint8_t* b = static_cast<const uint8_t*>(p); out.insert(out.end(), b, b + n); };
    auto u32 = [&](uint32_t v) { put(&v, 4); };
    auto f32 = [&](float v) { put(&v, 4); };
    const uint32_t n = uint32_t(boxes.size());
    put("AOSTATEL", 8);
    u32(2); u32(999);
    u32(n); u32(n); u32(n); u32(1); u32(n * 8); u32(n * 36); u32(64);
    u32(1010004); u32(1); uint16_t wh[2] = {8, 8}; put(wh, 4); u32(0);                   // the see-through mask
    for (uint32_t i = 0; i < n; ++i) {                                                     // meshes
        u32(i); u32(i); u32(1);
        for (int k = 0; k < 3; ++k) f32(boxes[i].lo[k]);
        for (int k = 0; k < 3; ++k) f32(boxes[i].hi[k]);
    }
    for (uint32_t i = 0; i < n; ++i) {                                                     // pieces
        for (int r = 0; r < 16; ++r) f32(r % 5 == 0 ? 1.0f : 0.0f);
        u32(i * 8); u32(8); u32(i * 36); u32(36);
        int32_t tex = boxes[i].seeThrough ? 0 : -1;
        put(&tex, 4);
        f32(0.5f);
        for (int k = 0; k < 3; ++k) f32(boxes[i].lo[k]);
        for (int k = 0; k < 3; ++k) f32(boxes[i].hi[k]);
    }
    for (uint32_t i = 0; i < n; ++i) {                                                     // statels
        for (int r = 0; r < 12; ++r) f32(r < 9 && r % 4 == 0 ? 1.0f : 0.0f);
        int16_t ypr[3] = {0, 0, 0};
        put(ypr, 6);
        uint16_t mesh = uint16_t(i);
        put(&mesh, 2);
        f32(1.0f); u32(0); u32(i);
    }
    for (const SandBox& b : boxes)                                                         // vertices
        for (int c = 0; c < 8; ++c) {
            f32(c & 1 ? b.hi[0] : b.lo[0]); f32(c & 2 ? b.hi[1] : b.lo[1]); f32(c & 4 ? b.hi[2] : b.lo[2]);
            f32(0.5f); f32(0.5f);
        }
    static const uint16_t kTris[36] = {0, 1, 3, 0, 3, 2, 4, 6, 7, 4, 7, 5, 0, 4, 5, 0, 5, 1,
                                       2, 3, 7, 2, 7, 6, 0, 2, 6, 0, 6, 4, 1, 5, 7, 1, 7, 3};
    for (uint32_t i = 0; i < n; ++i) put(kTris, sizeof(kTris));                           // indices
    while (out.size() % 4) out.push_back(0);
    for (int k = 0; k < 64; ++k) out.push_back(uint8_t(((k ^ (k >> 3)) & 1) ? 255 : 0));   // texels (half leaves)
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
    return true;
}

// Terrain texturing test (--terrain-test): a canyon drawn like AnarchyGround_t - world-space chunks of 4 x 4 unit cells,
// each with its own four vertices and its own copy of the tile laid from above (0..1 over the cell, turned or flipped
// one of eight ways, as the game lays its near ground), an unlit base pass and a multiplying light pass with a
// lightmap - seen from the canyon floor towards a wall 30 units high. The wall's texture streaks without the
// terrain texturing (rvk terrain.glsl; --terrain-off), the floor's repeats show; a frame dump (terrain-frame.txt) has
// each chunk's fit (its T line).
bool g_terrainTest = false;
bool g_terrainOff = false;          // --terrain-off: RVK_CliffTex, RVK_TileHide, RVK_GroundVary all 0
float g_terrainYaw = 0.0f;          // --terrain-yaw R: the camera turned (radians; 0 looks along +z at the wall)
float g_terrainVary = 1.0f;         // --terrain-vary X: RVK_GroundVary (0 .. 2)
bool g_terrainMaps = false;         // --terrain-maps: the tile gets a normal map (from its brightness: pits sunk) and a
                                    // PBR material, per-pixel lighting - the light pass's relief laid like the texture
template <typename D>
void RunTerrainTest(D& dev, int frames, const std::string& shot)
{
    // The tile: sandstone with grains, a few dark pits and pale flecks - features that show a stretch and a repeat.
    constexpr int kTex = 256;
    std::vector<uint32_t> tile(kTex * kTex);
    auto hash = [](uint32_t x, uint32_t y) {
        uint32_t h = x * 0x8DA6B343u ^ y * 0xD8163841u;
        h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12;
        return float(h & 0xFFFFu) / 65535.0f;
    };
    auto smooth = [&](float x, float y, int period) {   // tileable value noise
        int x0 = int(std::floor(x)), y0 = int(std::floor(y));
        float fx = x - x0, fy = y - y0;
        fx = fx * fx * (3 - 2 * fx);
        fy = fy * fy * (3 - 2 * fy);
        auto h = [&](int a, int b) { return hash(uint32_t((a % period + period) % period), uint32_t((b % period + period) % period)); };
        float a = h(x0, y0) + (h(x0 + 1, y0) - h(x0, y0)) * fx, b = h(x0, y0 + 1) + (h(x0 + 1, y0 + 1) - h(x0, y0 + 1)) * fx;
        return a + (b - a) * fy;
    };
    for (int y = 0; y < kTex; ++y)
        for (int x = 0; x < kTex; ++x) {
            float n = 0.5f * smooth(x / 32.0f, y / 32.0f, 8) + 0.3f * smooth(x / 8.0f, y / 8.0f, 32) +
                      0.2f * hash(uint32_t(x), uint32_t(y));
            float pit = smooth(x / 16.0f + 3.1f, y / 16.0f + 7.7f, 16);
            float k = 0.75f + 0.45f * n - (pit > 0.78f ? 0.45f : 0.0f) + (hash(uint32_t(x) / 3, uint32_t(y) / 3) > 0.985f ? 0.3f : 0.0f);
            auto ch = [&](float c) { return uint32_t(std::clamp(c * k, 0.0f, 255.0f)); };
            tile[size_t(y) * kTex + x] = 0xFF000000u | ch(196) << 16 | ch(124) << 8 | ch(78);
        }
    Texture* ground = dev.CreateTexture(kTex, kTex, tile.data());
    if (g_terrainMaps) {
        auto heightAt = [&](int x, int y) {
            uint32_t p = tile[size_t((y + kTex) % kTex) * kTex + (x + kTex) % kTex];
            return float((p >> 16 & 0xFF) + (p >> 8 & 0xFF) + (p & 0xFF)) / (3.0f * 255.0f);
        };
        std::vector<uint32_t> normalPixels(kTex * kTex), ormPixels(kTex * kTex, 0xFFFFD000u);   // rough, not metal
        for (int y = 0; y < kTex; ++y)
            for (int x = 0; x < kTex; ++x) {
                float dx = (heightAt(x + 1, y) - heightAt(x - 1, y)) * 4.0f, dy = (heightAt(x, y + 1) - heightAt(x, y - 1)) * 4.0f;
                float n[3] = {-dx, dy, 1.0f}, l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);   // (OpenGL: +y up)
                auto ch = [&](float c) { return uint32_t(std::clamp((c / l * 0.5f + 0.5f) * 255.0f, 0.0f, 255.0f)); };
                normalPixels[size_t(y) * kTex + x] = 0xFF000000u | ch(n[0]) << 16 | ch(n[1]) << 8 | ch(n[2]);
            }
        dev.SetNormalMap(ground, dev.CreateTexture(kTex, kTex, normalPixels.data()));
        dev.SetMaterialMaps(ground, dev.CreateTexture(kTex, kTex, ormPixels.data()), nullptr);
        dev.SetPixelLighting(true);
        dev.SetNormalMaps(true, 1.0f);
    }
    std::vector<uint32_t> lightmapPixels(64 * 64, 0xFFC8C8C8u);
    Texture* lightmap = dev.CreateTexture(64, 64, lightmapPixels.data());
    struct VtxTerrain { float x, y, z, nx, ny, nz, u0, v0, u1, v1; };
    const uint32_t kFvfTerrain = FVF_XYZ | FVF_NORMAL | (2 << 8);
    // The canyon: a floor with low swells, a wall rising 30 units over ~5 along z = 40, a plateau above it.
    auto height = [](float x, float z) {
        float t = std::clamp((z - 40.0f) / 5.0f, 0.0f, 1.0f);
        t = t * t * (3 - 2 * t);
        return 1.2f * std::sin(x * 0.11f) * std::cos(z * 0.09f) + 30.0f * t + 1.5f * std::sin(x * 0.31f) * t;
    };
    auto normal = [&](float x, float z, float out[3]) {
        float dx = height(x + 0.25f, z) - height(x - 0.25f, z), dz = height(x, z + 0.25f) - height(x, z - 0.25f);
        float n[3] = {-dx, 0.5f, -dz}, l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        for (int i = 0; i < 3; ++i) out[i] = n[i] / l;
    };
    constexpr int kChunks = 6, kChunkCells = 8;
    constexpr float kChunk = 32.0f, kCell = kChunk / kChunkCells, kOriginX = -kChunk * kChunks / 2, kOriginZ = -64.0f;
    std::vector<std::vector<VtxTerrain>> chunks;
    std::vector<uint16_t> cellIndices;
    for (int k = 0; k < kChunkCells * kChunkCells; ++k) {
        const uint16_t q[6] = {uint16_t(4 * k), uint16_t(4 * k + 1), uint16_t(4 * k + 2), uint16_t(4 * k + 2),
                               uint16_t(4 * k + 1), uint16_t(4 * k + 3)};
        cellIndices.insert(cellIndices.end(), q, q + 6);
    }
    for (int cz = 0; cz < kChunks; ++cz)
        for (int cx = 0; cx < kChunks; ++cx) {
            std::vector<VtxTerrain> v;
            for (int j = 0; j < kChunkCells; ++j)
                for (int i = 0; i < kChunkCells; ++i) {
                    const uint32_t turn = uint32_t(hash(uint32_t(cx * kChunkCells + i), uint32_t(cz * kChunkCells + j)) * 8.0f) & 7u;
                    auto vert = [&](int a, int b) {
                        float x = kOriginX + cx * kChunk + (i + a) * kCell, z = kOriginZ + cz * kChunk + (j + b) * kCell;
                        float u = float(a), w = float(b);              // the cell's own copy of the tile ...
                        if (turn & 1u) std::swap(u, w);                // ... turned or flipped
                        if (turn & 2u) u = 1.0f - u;
                        if (turn & 4u) w = 1.0f - w;
                        VtxTerrain t{x, height(x, z), z, 0, 1, 0, u, w, float(i + a) / kChunkCells, float(j + b) / kChunkCells};
                        normal(x, z, &t.nx);
                        return t;
                    };
                    VtxTerrain q[4] = {vert(0, 0), vert(0, 1), vert(1, 0), vert(1, 1)};
                    v.insert(v.end(), q, q + 4);
                }
            chunks.push_back(std::move(v));
        }
    if (g_terrainOff || g_terrainVary != 1.0f) {
        Device::TerrainSettings ts;
        ts.variation = g_terrainVary;
        if (g_terrainOff) ts.cliff = ts.tiling = ts.variation = 0.0f;
        dev.SetTerrain(ts);
    }
    for (int frame = 0; frame < frames; ++frame) {
        if (frame == frames - 1) {
            dev.RequestScreenshot(shot);
            dev.RequestFrameDump("terrain-frame.txt");
        }
        dev.BeginFrame();
        dev.SetViewport({0, 0, g_width, g_height, 0.0f, 1.0f});
        dev.Clear(CLEAR_TARGET | CLEAR_ZBUFFER, 0xFF8098B8, 1.0f);
        const Vector eye{-6.0f, height(-6.0f, 0.0f) + 4.0f, 0.0f};
        const float yaw = 1.5708f + g_terrainYaw;   // (+z)
        dev.SetTransform(View, LookAtLH(eye, {eye.x + std::cos(yaw), eye.y + 0.25f, eye.z + std::sin(yaw)}, {0, 1, 0}));
        dev.SetTransform(Projection, PerspectiveLH(kPi / 3, float(g_width) / g_height, 0.3f, 400.0f));
        dev.SetTransform(World, Identity());
        dev.SetRenderState(RS_ZENABLE, 1);
        dev.SetRenderState(RS_ZWRITEENABLE, 1);
        dev.SetRenderState(RS_CULLMODE, CULL_NONE);
        dev.SetRenderState(RS_ALPHATESTENABLE, 0);
        Material mat{{1, 1, 1, 1}, {1, 1, 1, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}, 0.0f};
        dev.SetMaterial(mat);
        Light sun{};
        sun.type = LIGHT_DIRECTIONAL;
        sun.diffuse = {0.9f, 0.85f, 0.75f, 1};
        sun.direction = {0.55f, -0.7f, 0.45f};
        dev.SetLight(0, sun);
        dev.LightEnable(0, true);
        dev.SetTextureStageState(0, TSS_COLORARG1, TA_TEXTURE);
        dev.SetTextureStageState(0, TSS_COLORARG2, TA_DIFFUSE);
        dev.SetTextureStageState(0, TSS_ALPHAOP, TOP_SELECTARG1);
        dev.SetTextureStageState(0, TSS_ALPHAARG1, TA_TEXTURE);
        for (const auto& c : chunks) {
            dev.SetDrawVisual(uint32_t(VisualKind::Terrain), "AnarchyGround_t");
            dev.SetRenderState(RS_LIGHTING, 0);      // base pass: the ground's own texture, unlit
            dev.SetRenderState(RS_ALPHABLENDENABLE, 0);
            dev.SetRenderState(RS_ZFUNC, CMP_LESSEQUAL);
            dev.SetTexture(0, ground);
            dev.SetTextureStageState(0, TSS_TEXCOORDINDEX, 0);
            dev.SetTextureStageState(0, TSS_COLOROP, TOP_SELECTARG1);
            dev.SetTextureStageState(0, TSS_ADDRESSU, TADDRESS_CLAMP);   // (each cell's tile clamped, as the game)
            dev.SetTextureStageState(0, TSS_ADDRESSV, TADDRESS_CLAMP);
            dev.DrawIndexedPrimitive(TriangleList, kFvfTerrain, c.data(), uint32_t(c.size()), cellIndices.data(),
                                     uint32_t(cellIndices.size()));
            dev.SetTextureStageState(0, TSS_ADDRESSU, TADDRESS_WRAP);
            dev.SetTextureStageState(0, TSS_ADDRESSV, TADDRESS_WRAP);
            dev.SetRenderState(RS_LIGHTING, 1);      // light pass: lightmap + ambient multiplying it
            dev.SetRenderState(RS_AMBIENT, 0xFF515151);
            dev.SetRenderState(RS_ALPHABLENDENABLE, 1);
            dev.SetRenderState(RS_SRCBLEND, BLEND_ZERO);
            dev.SetRenderState(RS_DESTBLEND, BLEND_SRCCOLOR);
            dev.SetRenderState(RS_ZFUNC, CMP_EQUAL);
            dev.SetTexture(0, lightmap);
            dev.SetTextureStageState(0, TSS_TEXCOORDINDEX, 1);
            dev.SetTextureStageState(0, TSS_COLOROP, TOP_ADD);
            dev.DrawIndexedPrimitive(TriangleList, kFvfTerrain, c.data(), uint32_t(c.size()), cellIndices.data(),
                                     uint32_t(cellIndices.size()));
            dev.SetDrawVisual(0, nullptr);
        }
        dev.SetTextureStageState(0, TSS_TEXCOORDINDEX, 0);
        dev.SetRenderState(RS_ZFUNC, CMP_LESSEQUAL);
        dev.EndFrame();
    }
}

// Native ground test (--native-ground-test): a playfield of 128 x 128 cells of 4 units drawn as the game draws its
// ground - each cell its own four vertices with its tile's coordinates turned by the cell's code (docs/ground-format.md),
// a base-pass draw per patch and tile, a lighting pass per patch with that patch's 64 x 64 lightmap through the game's
// palette - and, with --native-on, the same data handed to rvk's native ground (native_ground.cpp), which draws it
// itself and skips those draws: the two pictures should match. Four tiles, each a colour with a mark in one corner
// (a turn shows), a canyon wall across the view.
bool g_nativeGroundTest = false;
bool g_nativeOn = false;
bool g_groundSingle = false;   // --ground-single: the game's ground in one pass (the Shadowlands'), not two
template <typename D>
void RunNativeGroundTest(D& dev, int frames, const std::string& shot)
{
    constexpr uint32_t kCells = 128, kPatch = 64, kTiles = 4, kTex = 64;
    constexpr float kCell = 4.0f;
    auto hash = [](uint32_t x, uint32_t y) {
        uint32_t h = x * 0x8DA6B343u ^ y * 0xD8163841u;
        h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12;
        return h;
    };
    const uint32_t colours[kTiles] = {0xFFB07840u, 0xFF8C9A5Au, 0xFF9C8C7Cu, 0xFFC8A46Eu};
    std::vector<Texture*> tiles;
    for (uint32_t t = 0; t < kTiles; ++t) {
        std::vector<uint32_t> px(kTex * kTex);
        for (uint32_t y = 0; y < kTex; ++y)
            for (uint32_t x = 0; x < kTex; ++x) {
                uint32_t c = colours[t];
                if (x < 20 && y < 12) c = 0xFF202020u;                    // the mark: top left, wider than high
                if ((hash(x, y + 97 * t) & 15u) == 0) c ^= 0x00101010u;   // (some grain)
                px[size_t(y) * kTex + x] = c;
            }
        tiles.push_back(dev.CreateTexture(kTex, kTex, px.data()));
        if (g_terrainMaps) {                 // (--terrain-maps: a normal map from its brightness: the mark sunk)
            std::vector<uint32_t> nm(kTex * kTex);
            auto lum = [&](int x, int y) {
                const uint32_t c = px[size_t((y + kTex) % kTex) * kTex + (x + kTex) % kTex];
                return float((c >> 16 & 0xFF) + (c >> 8 & 0xFF) + (c & 0xFF)) / 765.0f;
            };
            for (int y = 0; y < int(kTex); ++y)
                for (int x = 0; x < int(kTex); ++x) {
                    const float dx = (lum(x + 1, y) - lum(x - 1, y)) * 3.0f, dy = (lum(x, y + 1) - lum(x, y - 1)) * 3.0f;
                    const float l = std::sqrt(dx * dx + dy * dy + 1.0f);
                    auto ch = [&](float v) { return uint32_t(std::clamp((v / l * 0.5f + 0.5f) * 255.0f, 0.0f, 255.0f)); };
                    nm[size_t(y) * kTex + x] = 0xFF000000u | ch(-dx) << 16 | ch(dy) << 8 | ch(1.0f);
                }
            dev.SetNormalMap(tiles.back(), dev.CreateTexture(kTex, kTex, nm.data()));
        }
    }
    if (g_terrainMaps) {
        dev.SetPixelLighting(true);
        dev.SetNormalMaps(true, 1.0f);
    }
    auto height = [](float x, float z) {
        float t = std::clamp((z - 150.0f) / 12.0f, 0.0f, 1.0f);
        t = t * t * (3 - 2 * t);
        return 2.0f * std::sin(x * 0.05f) * std::cos(z * 0.04f) + 40.0f * t;
    };
    auto ground = std::make_shared<NativeGround>();
    ground->id = 1;
    ground->cellsX = ground->cellsZ = kCells;
    ground->patch = kPatch;
    ground->cellX = ground->cellZ = kCell;
    ground->heights.resize(size_t(kCells + 1) * (kCells + 1));
    for (uint32_t r = 0; r <= kCells; ++r)
        for (uint32_t c = 0; c <= kCells; ++c) ground->heights[size_t(r) * (kCells + 1) + c] = height(c * kCell, r * kCell);
    ground->codes.resize(size_t(kCells) * kCells);
    for (uint32_t r = 0; r < kCells; ++r)
        for (uint32_t c = 0; c < kCells; ++c) {
            const uint32_t h = hash(c, r);
            ground->codes[size_t(r) * kCells + c] = uint16_t((h % kTiles) | ((h >> 8) & 3u) << 14);
        }
    ground->tiles = tiles;
    // The lightmap: a 4-bit value a cell (packed, the high nibble first), and the palette at a shade of 0.45.
    auto packed = std::make_shared<std::vector<uint8_t>>(size_t(kCells / 2) * kCells);
    auto shadeAt = [&](uint32_t c, uint32_t r) { return uint8_t(6 + (hash(c / 3, r / 3) % 10)); };
    for (uint32_t r = 0; r < kCells; ++r)
        for (uint32_t c = 0; c < kCells; c += 2)
            (*packed)[size_t(r) * (kCells / 2) + c / 2] = uint8_t(shadeAt(c, r) << 4 | shadeAt(c + 1, r));
    const float shade = 0.45f;
    auto grey = [&](uint32_t i) {
        const uint32_t g = uint32_t((float(i) / 15.0f * 188.0f + 4.0f) * (1.0f - shade));
        return 0xFF000000u | g << 16 | g << 8 | g;
    };
    // The game's way: per patch, a draw per tile (each cell four vertices, its tile's coordinates turned), and the
    // patch's lightmap.
    struct VtxTerrain { float x, y, z, nx, ny, nz, u0, v0, u1, v1; };
    const uint32_t kFvfTerrain = FVF_XYZ | FVF_NORMAL | (2 << 8);
    struct Chunk { uint32_t tile; std::vector<VtxTerrain> v; std::vector<uint16_t> i; };
    struct GamePatch { std::vector<Chunk> chunks; Texture* lightmap; };
    std::vector<GamePatch> patches;
    auto normalAt = [&](float x, float z, float n[3]) {
        const float dx = (height(x + kCell, z) - height(x - kCell, z)) / (2 * kCell);
        const float dz = (height(x, z + kCell) - height(x, z - kCell)) / (2 * kCell);
        const float l = std::sqrt(dx * dx + dz * dz + 1);
        n[0] = -dx / l; n[1] = 1 / l; n[2] = -dz / l;
    };
    for (uint32_t pz = 0; pz < kCells / kPatch; ++pz)
        for (uint32_t px = 0; px < kCells / kPatch; ++px) {
            GamePatch gp;
            std::vector<uint32_t> lm(kPatch * kPatch);
            for (uint32_t j = 0; j < kPatch; ++j)
                for (uint32_t i = 0; i < kPatch; ++i) lm[j * kPatch + i] = grey(shadeAt(px * kPatch + i, pz * kPatch + j));
            gp.lightmap = dev.CreateTexture(kPatch, kPatch, lm.data());
            for (uint32_t t = 0; t < kTiles; ++t) {
                Chunk ch{t, {}, {}};
                for (uint32_t j = 0; j < kPatch; ++j)
                    for (uint32_t i = 0; i < kPatch; ++i) {
                        const uint32_t c = px * kPatch + i, r = pz * kPatch + j;
                        const uint16_t code = ground->codes[size_t(r) * kCells + c];
                        if ((code & 0x3FFFu) != t) continue;
                        const uint32_t turn = code >> 14;
                        const uint16_t base = uint16_t(ch.v.size());
                        for (int k = 0; k < 4; ++k) {
                            const float a = float(k & 1), b = float(k >> 1);   // (along x, along z)
                            const float uv[4][2] = {{a, 1 - b}, {1 - b, 1 - a}, {1 - a, b}, {b, a}};
                            const float x = (c + a) * kCell, z = (r + b) * kCell;
                            VtxTerrain v{x, height(x, z), z, 0, 1, 0, uv[turn][0], uv[turn][1], (i + a) / kPatch, (j + b) / kPatch};
                            normalAt(x, z, &v.nx);
                            ch.v.push_back(v);
                        }
                        const uint16_t q[6] = {base, uint16_t(base + 2), uint16_t(base + 1), uint16_t(base + 1),
                                               uint16_t(base + 2), uint16_t(base + 3)};
                        ch.i.insert(ch.i.end(), q, q + 6);
                    }
                if (!ch.v.empty()) gp.chunks.push_back(std::move(ch));
            }
            patches.push_back(std::move(gp));
        }
    if (g_nativeOn) {
        dev.SetNativeGround(ground);
        dev.SetNativeGroundLightmap(packed, kCells, kCells, shade);
    }
    for (int frame = 0; frame < frames; ++frame) {
        if (frame == frames - 1) {
            dev.RequestScreenshot(shot);
            dev.RequestFrameDump("native-frame.txt");
        }
        dev.BeginFrame();
        dev.SetViewport({0, 0, g_width, g_height, 0.0f, 1.0f});
        dev.Clear(CLEAR_TARGET | CLEAR_ZBUFFER, 0xFF8098B8, 1.0f);
        const Vector eye{200.0f + g_terrainYaw * 0.0f, height(200.0f, 90.0f) + 14.0f, 90.0f};
        const float yaw = 1.5708f + g_terrainYaw;
        dev.SetTransform(View, LookAtLH(eye, {eye.x + std::cos(yaw), eye.y - 0.25f, eye.z + std::sin(yaw)}, {0, 1, 0}));
        dev.SetTransform(Projection, PerspectiveLH(kPi / 3, float(g_width) / g_height, 0.3f, 600.0f));
        dev.SetTransform(World, Identity());
        dev.SetRenderState(RS_ZENABLE, 1);
        dev.SetRenderState(RS_ZWRITEENABLE, 1);
        dev.SetRenderState(RS_CULLMODE, CULL_NONE);
        dev.SetRenderState(RS_ALPHATESTENABLE, 0);
        Material mat{{1, 1, 1, 1}, {1, 1, 1, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}, 0.0f};
        dev.SetMaterial(mat);
        Light sun{};
        sun.type = LIGHT_DIRECTIONAL;
        sun.diffuse = {0.9f, 0.85f, 0.75f, 1};
        sun.direction = {0.55f, -0.7f, 0.45f};
        dev.SetLight(0, sun);
        dev.LightEnable(0, true);
        dev.SetTextureStageState(0, TSS_COLORARG1, TA_TEXTURE);
        dev.SetTextureStageState(0, TSS_COLORARG2, TA_DIFFUSE);
        dev.SetTextureStageState(0, TSS_ALPHAOP, TOP_SELECTARG1);
        dev.SetTextureStageState(0, TSS_ALPHAARG1, TA_TEXTURE);
        dev.SetDrawVisual(uint32_t(VisualKind::Terrain), "AnarchyGround_t");
        if (g_groundSingle) {
            // (--ground-single: the game's one pass, as on the Shadowlands' playfields - lit and opaque, the lightmap
            // and the lights added in stage 0, the tile multiplied in by stage 1.)
            dev.SetRenderState(RS_LIGHTING, 1);
            dev.SetRenderState(RS_AMBIENT, 0xFF515151);
            dev.SetRenderState(RS_ALPHABLENDENABLE, 0);
            dev.SetRenderState(RS_ZFUNC, CMP_LESSEQUAL);
            dev.SetTextureStageState(0, TSS_TEXCOORDINDEX, 1);
            dev.SetTextureStageState(0, TSS_COLOROP, TOP_ADD);
            dev.SetTextureStageState(1, TSS_TEXCOORDINDEX, 0);
            dev.SetTextureStageState(1, TSS_COLOROP, TOP_MODULATE);
            dev.SetTextureStageState(1, TSS_COLORARG1, TA_TEXTURE);
            dev.SetTextureStageState(1, TSS_COLORARG2, TA_CURRENT);
            dev.SetTextureStageState(1, TSS_ADDRESSU, TADDRESS_CLAMP);
            dev.SetTextureStageState(1, TSS_ADDRESSV, TADDRESS_CLAMP);
            for (const GamePatch& gp : patches) {
                dev.SetTexture(0, gp.lightmap);
                for (const Chunk& ch : gp.chunks) {
                    dev.SetTexture(1, tiles[ch.tile]);
                    dev.DrawIndexedPrimitive(TriangleList, kFvfTerrain, ch.v.data(), uint32_t(ch.v.size()), ch.i.data(),
                                             uint32_t(ch.i.size()));
                }
            }
            dev.SetTexture(1, nullptr);
            dev.SetTextureStageState(1, TSS_COLOROP, TOP_DISABLE);
            dev.SetTextureStageState(1, TSS_TEXCOORDINDEX, 1);
        }
        if (!g_groundSingle) for (const GamePatch& gp : patches) {   // base pass: the tiles, unlit, clamped
            dev.SetRenderState(RS_LIGHTING, 0);
            dev.SetRenderState(RS_ALPHABLENDENABLE, 0);
            dev.SetRenderState(RS_ZFUNC, CMP_LESSEQUAL);
            dev.SetTextureStageState(0, TSS_TEXCOORDINDEX, 0);
            dev.SetTextureStageState(0, TSS_COLOROP, TOP_SELECTARG1);
            dev.SetTextureStageState(0, TSS_ADDRESSU, TADDRESS_CLAMP);
            dev.SetTextureStageState(0, TSS_ADDRESSV, TADDRESS_CLAMP);
            for (const Chunk& ch : gp.chunks) {
                dev.SetTexture(0, tiles[ch.tile]);
                dev.DrawIndexedPrimitive(TriangleList, kFvfTerrain, ch.v.data(), uint32_t(ch.v.size()), ch.i.data(),
                                         uint32_t(ch.i.size()));
            }
        }
        if (!g_groundSingle) for (const GamePatch& gp : patches) {   // lighting pass: the lightmap + the lights, multiplying
            dev.SetRenderState(RS_LIGHTING, 1);
            dev.SetRenderState(RS_AMBIENT, 0xFF515151);
            dev.SetRenderState(RS_ALPHABLENDENABLE, 1);
            dev.SetRenderState(RS_SRCBLEND, BLEND_ZERO);
            dev.SetRenderState(RS_DESTBLEND, BLEND_SRCCOLOR);
            dev.SetRenderState(RS_ZFUNC, CMP_EQUAL);
            dev.SetTexture(0, gp.lightmap);
            dev.SetTextureStageState(0, TSS_TEXCOORDINDEX, 1);
            dev.SetTextureStageState(0, TSS_COLOROP, TOP_ADD);
            for (const Chunk& ch : gp.chunks)
                dev.DrawIndexedPrimitive(TriangleList, kFvfTerrain, ch.v.data(), uint32_t(ch.v.size()), ch.i.data(),
                                         uint32_t(ch.i.size()));
        }
        dev.SetDrawVisual(0, nullptr);
        dev.SetTextureStageState(0, TSS_TEXCOORDINDEX, 0);
        dev.SetTextureStageState(0, TSS_ADDRESSU, TADDRESS_WRAP);
        dev.SetTextureStageState(0, TSS_ADDRESSV, TADDRESS_WRAP);
        dev.SetRenderState(RS_ZFUNC, CMP_LESSEQUAL);
        dev.SetRenderState(RS_ALPHABLENDENABLE, 0);
        dev.EndFrame();
    }
}

// A real playfield's ground (--native-ground-file F, from tools/ground-data.py --export): rvk's native ground drawn
// from it - the game's heights, tiles (their JPEGs, mipmapped) and the hour's lightmap - from --eye X Y Z, looking
// --view-yaw R (radians from +x towards +z) and --view-pitch R down. The scene issues one stand-in ground draw a pass,
// with the ground's state; the native ground draws instead.
std::string g_groundFile;
float g_groundAo = 0.6f;            // --ground-ao X: RVK_GroundAo
float g_groundTess = 0.6f;          // --ground-tess X: RVK_GroundTess
float g_cliffDepth = 0.4f;          // --cliff-depth X: RVK_CliffDepth
float g_groundSun = 1.0f;           // --ground-sun X: RVK_GroundSun
float g_groundRocks = 1.0f;         // --ground-rocks X: RVK_GroundRocks
float g_groundPebbles = 1.0f;       // --ground-pebbles X: RVK_Pebbles
float g_groundGrain = 0.6f;         // --ground-grain X: RVK_GroundGrain
int g_groundOne = 1;                // --ground-one 0|1: RVK_GroundOne, the native ground in one pass (0: the game's two)
float g_groundBlend = 1.0f;         // --ground-blend X: RVK_GroundBlend
float g_sandRipple = 1.0f;          // --sand-ripple X: RVK_SandRipple
float g_parallax = 1.0f;            // --parallax X: RVK_Parallax
float g_parallaxSteps = 1.0f;       // --parallax-steps X: RVK_DepthSteps
float g_duneLight = 0.6f;           // --dune-light X: RVK_DuneLight
float g_sandGlint = 0.5f;           // --sand-glint X: RVK_SandGlint
float g_rippleSmooth = 0.3f;        // --ripple-smooth X: RVK_RippleSoft
float g_sandPrints = 1.0f;          // --sand-prints X: RVK_SandPrints
float g_rippleDepth = 1.0f;         // --ripple-depth X: RVK_RippleDepth
bool g_printWalk = false;
bool g_groundLight = false;         // --ground-light: a character's light (point, range 6) 2 units over the ground ahead           // --print-walk: a walker's footprints laid ahead of the eye (sand_prints.cpp)
float g_desertAir = 1.0f;           // --desert-air X: RVK_DesertAir
float g_groundFog = 0.0f;           // --ground-fog END: the native ground scene fogged as the game does (linear from 0;
                                    // Newland Desert: 1276), its colour the desert's, our sky over it; 0 none
float g_heatShimmer = 0.5f;         // --heat-shimmer X: RVK_HeatShimmer
bool g_tileOrm = false;             // --tile-orm: every other tile a shiny material map (PBR, pixel lighting on)
bool g_statelSkinned = false;   // (--statel-skinned: also the native ground's, RunGroundFile)
int g_shotRun = 0;   // --shot-run K: the last K frames each saved (<shot>-0.bmp ...), to see what changes frame to frame

// A stand-in character skinned on the GPU (--statel-skinned): an 8-sided column 1.8 high on one bone.
std::shared_ptr<rvk::skin::Source> MakeColumn()
{
    auto column = std::make_shared<rvk::skin::Source>();
    for (int k = 0; k < 8; ++k)
        for (int e = 0; e < 2; ++e) {
            const float a = float(k) * kPi / 4.0f, cx = 0.4f * std::cos(a), cz = 0.4f * std::sin(a), cy = e ? 1.8f : 0.0f;
            rvk::skin::TriVertex v{};
            v.posA[0] = v.posB[0] = v.bind[0] = cx;
            v.posA[1] = v.posB[1] = v.bind[1] = cy;
            v.posA[2] = v.posB[2] = v.bind[2] = cz;
            v.normal[0] = std::cos(a);
            v.normal[2] = std::sin(a);
            v.boneA = v.boneB = 0;
            v.weightA = 1.0f;
            column->vertices.push_back(v);
        }
    for (int k = 0; k < 8; ++k) {
        const uint16_t a = uint16_t(2 * k), b = uint16_t(2 * ((k + 1) % 8));
        const uint16_t q[6] = {a, uint16_t(a + 1), b, b, uint16_t(a + 1), uint16_t(b + 1)};
        column->indices.insert(column->indices.end(), q, q + 6);
    }
    column->Finish();
    column->gameIndices = column->indices.data();
    return column;
}

// The column standing at (x, y, z) this frame: a new job, as the game makes one each frame it skins a character.
template <typename D>
void DrawColumn(D& dev, const std::shared_ptr<rvk::skin::Source>& column, float x, float y, float z, uint32_t owner)
{
    if constexpr (requires { dev.DrawSkinned(0u, 0u, std::declval<rvk::skin::Job&>(), 0u, 0u, nullptr, 0u); }) {
        rvk::skin::Bone bone{{1, 0, 0, 0, 1, 0, 0, 0, 1, x, y, z}};
        auto palette = std::make_shared<rvk::skin::Palette>();
        palette->Set(&bone, 1);
        rvk::skin::Job job;
        job.source = column;
        job.bones = palette;
        job.ComputeBounds();
        dev.SetTexture(0, nullptr);
        dev.SetDrawVisual(uint32_t(VisualKind::Character), "column", owner);
        dev.DrawSkinned(TriangleList, rvk::skin::kVertexFvf, job, 0, uint32_t(column->vertices.size()),
                        column->indices.data(), uint32_t(column->indices.size()));
        dev.SetDrawVisual(0, nullptr);
    }
}

float g_eye[3] = {1355.87f, 42.36f, 2861.39f}, g_viewYaw = 1.5708f, g_viewPitch = 0.15f;
uint32_t g_tileSwap[3] = {};   // --tile-swap N A B (RunGroundFile)
float g_walkFrom[3] = {};   // --walk-from X Z N: the eye walks in a straight line from (X, Z) to --eye over the first N
                            // frames (the grass's tiles coming and going as in game), then stays
int g_lookRound = 0;   // --look-round N: the camera turns once round, level, over the first N frames (the environment
                       // probe sees all round: RVK_RtReflect's hits off the screen), then the given view
template <typename D>
void RunGroundFile(D& dev, int frames, const std::string& shot)
{
    FILE* f = std::fopen(g_groundFile.c_str(), "rb");
    if (!f) { std::printf("can't open %s\n", g_groundFile.c_str()); return; }
    std::vector<uint8_t> d;
    {
        std::fseek(f, 0, SEEK_END);
        d.resize(size_t(std::ftell(f)));
        std::fseek(f, 0, SEEK_SET);
        if (std::fread(d.data(), 1, d.size(), f) != d.size()) d.clear();
        std::fclose(f);
    }
    size_t p = 8;
    auto u32 = [&] { uint32_t v; std::memcpy(&v, d.data() + p, 4); p += 4; return v; };
    auto f32 = [&] { float v; std::memcpy(&v, d.data() + p, 4); p += 4; return v; };
    if (d.size() < 32 || (std::memcmp(d.data(), "AOGND1", 6) != 0 && std::memcmp(d.data(), "AOGND2", 6) != 0)) {
        std::printf("not a ground file\n");
        return;
    }
    auto ground = std::make_shared<NativeGround>();
    ground->id = 790;
    ground->cellsX = u32();
    ground->cellsZ = u32();
    ground->patch = u32();
    ground->cellX = f32();
    ground->cellZ = f32();
    const size_t nh = size_t(ground->cellsX + 1) * (ground->cellsZ + 1), nc = size_t(ground->cellsX) * ground->cellsZ;
    ground->heights.resize(nh);
    std::memcpy(ground->heights.data(), d.data() + p, nh * 4);
    p += nh * 4;
    ground->codes.resize(nc);
    std::memcpy(ground->codes.data(), d.data() + p, nc * 2);
    p += nc * 2;
    const uint32_t tiles = u32();
    std::vector<maps::MipChain> chains(tiles);
    for (uint32_t t = 0; t < tiles; ++t) {
        const uint32_t n = u32();
        maps::Image img;
        std::string error;
        if (n && maps::LoadImageFromMemory(d.data() + p, n, &img, &error))
            maps::MakeAlbedoChain(img, &chains[t]);
        p += n;
    }
    // (--tile-swap N A B: tile A shows tile B's picture until frame N, then its own - as the game fills a ground
    // texture again after the grass first saw it.)
    const bool swap = g_tileSwap[0] > 0 && g_tileSwap[1] < tiles && g_tileSwap[2] < tiles &&
                      chains[g_tileSwap[1]].w == chains[g_tileSwap[2]].w && chains[g_tileSwap[1]].h == chains[g_tileSwap[2]].h;
    for (uint32_t t = 0; t < tiles; ++t)
        ground->tiles.push_back(maps::Upload(dev, chains[swap && t == g_tileSwap[1] ? g_tileSwap[2] : t]));
    if (g_tileOrm) {
        const std::vector<uint32_t> shiny(16, 0xFFFF3000u);   // (occlusion 1, roughness 0.19, metal 0)
        for (size_t t = 0; t < ground->tiles.size(); t += 2)
            if (ground->tiles[t]) dev.SetMaterialMaps(ground->tiles[t], dev.CreateTexture(4, 4, shiny.data()), nullptr);
        dev.SetPixelLighting(true);
        Device::PbrSettings pbr;
        pbr.enabled = true;
        dev.SetPbr(pbr);
    }
    const uint32_t lw = u32(), lh = u32();
    const float shade = f32();
    auto packed = std::make_shared<std::vector<uint8_t>>(d.data() + p, d.data() + p + size_t((lw + 1) / 2) * lh);
    p += size_t((lw + 1) / 2) * lh;
    if (d[5] == '2' && p + 4 <= d.size()) {     // (AOGND2: the tiles' surface types - the pebbles)
        const uint32_t n = u32();
        if (p + size_t(n) * 2 <= d.size()) {
            ground->types.resize(n);
            std::memcpy(ground->types.data(), d.data() + p, size_t(n) * 2);
        }
    }
    std::printf("ground: %u x %u cells, %u tiles, lightmap %u x %u\n", ground->cellsX, ground->cellsZ, tiles, lw, lh);
    dev.SetNativeGround(ground);
    dev.SetNativeGroundLightmap(packed, lw, lh, shade);
    dev.SetNativeGroundAo(g_groundAo);
    dev.SetNativeGroundSun(g_groundSun);
    dev.SetNativeGroundRocks(g_groundRocks);
    dev.SetNativeGroundPebbles(g_groundPebbles);
    {
        Device::TerrainSettings t;
        t.round = g_groundTess;
        t.depth = g_cliffDepth;
        t.grain = g_groundGrain;
        t.blend = g_groundBlend;
        t.ripple = g_sandRipple;
        t.parallax = g_parallax;
        t.parallaxSteps = g_parallaxSteps;
        t.dune = g_duneLight;
        t.glint = g_sandGlint;
        t.smooth = g_rippleSmooth;
        t.prints = g_sandPrints;
        t.rippleDepth = g_rippleDepth;
        dev.SetTerrain(t);
    }
    if (g_grassFieldOn && g_grassLate == 0)
        dev.SetGrassField(true, g_grassFieldDist, 5.0f, 0.5f, true);   // (--grass-field: the grass on it)
    struct VtxTerrain { float x, y, z, nx, ny, nz, u0, v0, u1, v1; };
    const uint32_t kFvfTerrain = FVF_XYZ | FVF_NORMAL | (2 << 8);
    const VtxTerrain standIn[3] = {{0, -1000, 0, 0, 1, 0, 0, 0, 0, 0}, {1, -1000, 0, 0, 1, 0, 1, 0, 1, 0},
                                   {0, -1000, 1, 0, 1, 0, 0, 1, 0, 1}};
    // (--statel-skinned: two stand-in characters 8 units ahead of the eye on the ground, one still, one walking a
    // circle - what AMD's indirect light makes of characters, which its model of the world lacks.)
    auto column = MakeColumn();
    auto groundY = [&](float x, float z) {
        const float fc = std::clamp(x / ground->cellX, 0.0f, float(ground->cellsX) - 0.001f);
        const float fr = std::clamp(z / ground->cellZ, 0.0f, float(ground->cellsZ) - 0.001f);
        const uint32_t c = uint32_t(fc), r = uint32_t(fr), w = ground->cellsX + 1;
        const float u = fc - float(c), v = fr - float(r);
        const float* h = ground->heights.data();
        return (h[r * w + c] * (1 - u) + h[r * w + c + 1] * u) * (1 - v) + (h[(r + 1) * w + c] * (1 - u) + h[(r + 1) * w + c + 1] * u) * v;
    };
    for (int frame = 0; frame < frames; ++frame) {
        if (swap && frame == int(g_tileSwap[0]) && ground->tiles[g_tileSwap[1]]) {
            const maps::MipChain& own = chains[g_tileSwap[1]];
            uint32_t w = own.w, h = own.h;
            for (uint32_t level = 0; level < own.levels.size(); ++level) {
                dev.UpdateTexture(ground->tiles[g_tileSwap[1]], level, 0, 0, w, h, own.levels[level].data(), w * 4);
                w = std::max(w / 2, 1u);
                h = std::max(h / 2, 1u);
            }
            std::printf("tile %u: its own picture from frame %d\n", g_tileSwap[1], frame);
        }
        if (g_shotRun > 0 && frame >= frames - g_shotRun) {
            std::string name = shot;
            name.insert(name.size() - 4, "-" + std::to_string(frame - (frames - g_shotRun)));
            dev.RequestScreenshot(name);
        }
        if (g_grassFieldOn && g_grassLate > 0 && frame == g_grassLate)   // (--grass-late N: switched on at frame N,
            dev.SetGrassField(true, g_grassFieldDist, 5.0f, 0.5f, true); // as after zoning: its tiles come later)
        // (The last 300 frames' GPU and CPU times logged - "ground bench:" - once the rocks and pebbles are made.)
        if (frame == std::max(frames - 301, 0)) dev.ProfileWindow(true, "");
        if (frame == frames - 1) {
            dev.ProfileWindow(false, "ground bench: ");
            dev.RequestScreenshot(shot);
            dev.RequestFrameDump("ground-frame.txt");
        }
        dev.BeginFrame();
        // (--print-walk: a walker crossing in front of the eye, a print a step from 3 units ahead, one every 8 frames,
        // and a second one's track across it.)
        if (g_printWalk && frame >= 100 && frame < 100 + 8 * 24 && (frame - 100) % 8 == 0) {
            const int k = (frame - 100) / 8, step = k % 12;
            const float fx = std::cos(g_viewYaw), fz = std::sin(g_viewYaw), side = (k & 1) ? -1.0f : 1.0f;
            // (The first walks away from the eye; the second crosses its way, left to right, 6 units out.)
            const float ahead = k < 12 ? 3.0f + 0.7f * float(step) : 6.0f, right = k < 12 ? 0.0f : -4.0f + 0.7f * float(step);
            const float dx = k < 12 ? fx : -fz, dz = k < 12 ? fz : fx;
            const float cx = g_eye[0] + fx * ahead - fz * right, cz = g_eye[2] + fz * ahead + fx * right;
            dev.AddSandPrint(cx - dz * 0.11f * side, cz + dx * 0.11f * side, dx, dz);
        }
        dev.SetViewport({0, 0, g_width, g_height, 0.0f, 1.0f});
        constexpr uint32_t kDesertFog = 0xFFD6B878u;   // (Newland Desert's fog colour, about)
        dev.Clear(CLEAR_TARGET | CLEAR_ZBUFFER, g_groundFog > 0.0f ? kDesertFog : 0xFF8098B8, 1.0f);
        if (g_groundFog > 0.0f) {
            dev.SetRenderState(RS_FOGENABLE, 1);
            dev.SetRenderState(RS_FOGTABLEMODE, FOG_LINEAR);
            dev.SetRenderState(RS_FOGCOLOR, kDesertFog);
            const float fogStart = 0.0f, fogEnd = g_groundFog;
            uint32_t bits;
            std::memcpy(&bits, &fogStart, 4); dev.SetRenderState(RS_FOGSTART, bits);
            std::memcpy(&bits, &fogEnd, 4); dev.SetRenderState(RS_FOGEND, bits);
        }
        const float walk = g_walkFrom[2] > 0.0f ? std::min(float(frame) / g_walkFrom[2], 1.0f) : 1.0f;
        const Vector eye{g_walkFrom[0] + (g_eye[0] - g_walkFrom[0]) * walk, g_eye[1],
                         g_walkFrom[1] + (g_eye[2] - g_walkFrom[1]) * walk};
        const bool round = frame < g_lookRound;
        const float yaw = round ? g_viewYaw + 2.0f * kPi * float(frame) / float(g_lookRound) : g_viewYaw;
        const float pitch = round ? 0.05f : g_viewPitch;
        const Vector at{eye.x + std::cos(yaw) * std::cos(pitch), eye.y - std::sin(pitch),
                        eye.z + std::sin(yaw) * std::cos(pitch)};
        dev.SetTransform(View, LookAtLH(eye, at, {0, 1, 0}));
        dev.SetTransform(Projection, PerspectiveLH(kPi / 3, float(g_width) / g_height, 0.5f, 2000.0f));
        dev.SetTransform(World, Identity());
        dev.SetRenderState(RS_ZENABLE, 1);
        dev.SetRenderState(RS_ZWRITEENABLE, 1);
        dev.SetRenderState(RS_CULLMODE, CULL_NONE);
        dev.SetRenderState(RS_ALPHATESTENABLE, 0);
        Material mat{{1, 1, 1, 1}, {1, 1, 1, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}, 0.0f};
        dev.SetMaterial(mat);
        if (g_groundLight) {                     // (--ground-light: 5 units ahead, a unit under the eye)
            Light own{};
            own.type = LIGHT_POINT;
            own.diffuse = {1.0f, 0.85f, 0.6f, 1};
            own.position = {eye.x + 5.0f * std::cos(g_viewYaw), eye.y - 1.0f, eye.z + 5.0f * std::sin(g_viewYaw)};
            own.range = 6.1f;
            own.attenuation0 = 0.0f;
            own.attenuation1 = 0.4f;
            dev.SetLight(1, own);
            dev.LightEnable(1, true);
        }
        Light sun{};
        sun.type = LIGHT_DIRECTIONAL;
        sun.diffuse = {0.9f, 0.85f, 0.75f, 1};
        sun.direction = g_sunDirSet ? Vector{g_sunDir[0], g_sunDir[1], g_sunDir[2]} : Vector{0.55f, -0.7f, 0.45f};
        dev.SetLight(0, sun);
        dev.LightEnable(0, true);
        if (g_groundFog > 0.0f) {
            // (The game's dome - a lit, unblended sphere round the eye - which our sky replaces.)
            static std::vector<VtxMesh> dome;
            static std::vector<uint16_t> domeIdx;
            if (dome.empty()) {
                for (int i = 0; i <= 8; ++i)
                    for (int j = 0; j <= 16; ++j) {
                        const float el = (float(i) / 8.0f - 0.5f) * kPi, az = float(j) / 16.0f * 2.0f * kPi;
                        const float x = std::cos(el) * std::cos(az), y = std::sin(el), z = std::cos(el) * std::sin(az);
                        dome.push_back({x * 500.0f, y * 500.0f, z * 500.0f, -x, -y, -z, kDesertFog, 0.0f, 0.0f});
                    }
                for (int i = 0; i < 8; ++i)
                    for (int j = 0; j < 16; ++j) {
                        const uint16_t a = uint16_t(i * 17 + j), b = uint16_t(a + 17);
                        domeIdx.insert(domeIdx.end(), {a, b, uint16_t(a + 1), uint16_t(a + 1), b, uint16_t(b + 1)});
                    }
            }
            dev.SetDrawVisual(uint32_t(VisualKind::Sky), "GenericMeshObject_t");
            dev.SetTransform(World, Translate(eye.x, eye.y, eye.z));
            dev.SetRenderState(RS_LIGHTING, 1);
            dev.SetRenderState(RS_ZWRITEENABLE, 0);
            dev.SetRenderState(RS_FOGENABLE, 0);
            dev.SetTexture(0, nullptr);
            dev.DrawIndexedPrimitive(TriangleList, kFvfMesh, dome.data(), uint32_t(dome.size()), domeIdx.data(),
                                     uint32_t(domeIdx.size()));
            dev.SetDrawVisual(0, nullptr);
            dev.SetRenderState(RS_ZWRITEENABLE, 1);
            dev.SetRenderState(RS_FOGENABLE, 1);
            dev.SetTransform(World, Identity());
        }
        dev.SetTextureStageState(0, TSS_MAGFILTER, TFG_LINEAR);   // (the game's ground: filtered, mipmapped)
        dev.SetTextureStageState(0, TSS_MINFILTER, TFN_LINEAR);
        dev.SetTextureStageState(0, TSS_MIPFILTER, TFP_LINEAR);
        dev.SetTextureStageState(0, TSS_COLORARG1, TA_TEXTURE);
        dev.SetTextureStageState(0, TSS_COLORARG2, TA_DIFFUSE);
        dev.SetTextureStageState(0, TSS_ALPHAOP, TOP_SELECTARG1);
        dev.SetTextureStageState(0, TSS_ALPHAARG1, TA_TEXTURE);
        dev.SetDrawVisual(uint32_t(VisualKind::Terrain), "AnarchyGround_t");
        dev.SetRenderState(RS_LIGHTING, 0);                  // base pass (the game's state for it)
        dev.SetRenderState(RS_ALPHABLENDENABLE, 0);
        dev.SetRenderState(RS_ZFUNC, CMP_LESSEQUAL);
        dev.SetTextureStageState(0, TSS_TEXCOORDINDEX, 0);
        dev.SetTextureStageState(0, TSS_COLOROP, TOP_SELECTARG1);
        dev.SetTextureStageState(0, TSS_ADDRESSU, TADDRESS_CLAMP);
        dev.SetTextureStageState(0, TSS_ADDRESSV, TADDRESS_CLAMP);
        dev.SetTexture(0, ground->tiles[0]);
        dev.DrawPrimitive(TriangleList, kFvfTerrain, standIn, 3);
        dev.SetRenderState(RS_LIGHTING, 1);                  // lighting pass
        dev.SetRenderState(RS_AMBIENT, 0xFF515151);
        dev.SetRenderState(RS_ALPHABLENDENABLE, 1);
        dev.SetRenderState(RS_SRCBLEND, BLEND_ZERO);
        dev.SetRenderState(RS_DESTBLEND, BLEND_SRCCOLOR);
        dev.SetRenderState(RS_ZFUNC, CMP_EQUAL);
        dev.SetTextureStageState(0, TSS_TEXCOORDINDEX, 1);
        dev.SetTextureStageState(0, TSS_COLOROP, TOP_ADD);
        // (The game lights its ground's lighting pass without the sun - the lightmap has it - only the ambient and
        // the local lights: the sun's light kept out by a black diffuse here, still the frame's sun.)
        // (--ground-light: as in game, a white diffuse and the sun's light off - the game's dumps: d 1.00, its lights
        // the local ones.)
        const Material lightMat{{0, 0, 0, 1}, {1, 1, 1, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}, 0.0f};
        dev.SetMaterial(g_groundLight ? mat : lightMat);
        if (g_groundLight) dev.LightEnable(0, false);
        dev.DrawPrimitive(TriangleList, kFvfTerrain, standIn, 3);
        if (g_groundLight) dev.LightEnable(0, true);
        dev.SetMaterial(mat);
        dev.SetDrawVisual(0, nullptr);
        dev.SetTextureStageState(0, TSS_TEXCOORDINDEX, 0);
        if (g_statelSkinned) {
            dev.SetRenderState(RS_ALPHABLENDENABLE, 0);
            dev.SetRenderState(RS_ZFUNC, CMP_LESSEQUAL);
            dev.SetTextureStageState(0, TSS_COLOROP, TOP_MODULATE);
            const float ax = g_eye[0] + 8.0f * std::cos(g_viewYaw), az = g_eye[2] + 8.0f * std::sin(g_viewYaw);
            DrawColumn(dev, column, ax - 1.5f, groundY(ax - 1.5f, az), az, 7);
            const float t = float(frame) * 2.0f * kPi / 240.0f;
            const float wx = ax + 1.5f + std::cos(t), wz = az + std::sin(t);
            DrawColumn(dev, column, wx, groundY(wx, wz), wz, 8);
        }
        dev.SetRenderState(RS_ZFUNC, CMP_LESSEQUAL);
        dev.SetRenderState(RS_ALPHABLENDENABLE, 0);
        dev.EndFrame();
    }
}

// Ground grass benchmark (--grass-bench): a big hilly terrain drawn like AnarchyGround_t - world-space chunks of
// non-indexed triangles, an unlit base pass with a green texture and a multiplying light pass with a lightmap - under
// RVK_GrassOn, the camera walking and turning so tiles keep coming into range. Prints the frame time (mean and worst).
template <typename D>
void RunGrassBench(D& dev, int frames, const std::string& shot)
{
    std::vector<uint32_t> grassPixels(64 * 64);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
            grassPixels[y * 64 + x] = g_sandStorm || g_bareGround ? ((x / 4 + y / 4) & 1 ? 0xFFC8A46E : 0xFFBE9862)
                                    : (x >= 26 && x < 38) ? 0xFF8A6A4A : ((x / 4 + y / 4) & 1 ? 0xFF3C6A2A : 0xFF2E5320);
    // (Loading: a game draws ground whose texture isn't in yet with a stand-in, then uploads the picture.)
    const std::vector<uint32_t> standIn(64 * 64, 0xFF3C6A2A);
    Texture* grassGround = dev.CreateTexture(64, 64, g_grassBenchLoading > 0 ? standIn.data() : grassPixels.data());
    std::vector<uint32_t> lightmapPixels(64 * 64);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            uint32_t l = uint32_t(150 + 60 * std::sin(x * 0.2f) * std::cos(y * 0.15f));
            if (g_grassBenchNight)
                l /= 5;
            lightmapPixels[y * 64 + x] = 0xFF000000u | l << 16 | l << 8 | l;
        }
    Texture* lightmap = dev.CreateTexture(64, 64, lightmapPixels.data());
    struct VtxTerrain { float x, y, z, nx, ny, nz, u0, v0, u1, v1; };
    const uint32_t kFvfTerrain = FVF_XYZ | FVF_NORMAL | (2 << 8);
    // (--grass-bench-steep: steep hills, slopes up to ~56 degrees - where the blades stood in rows.)
    auto height = [](float x, float z) {
        return g_grassBenchSteep ? 6.0f * std::sin(x * 0.25f) * std::cos(z * 0.2f)
                                 : 1.5f * std::sin(x * 0.13f) * std::cos(z * 0.11f);
    };
    constexpr int kChunks = 6, kChunkCells = 16;
    constexpr float kChunk = 32.0f, kCell = kChunk / kChunkCells, kOrigin = -kChunk * kChunks / 2;
    std::vector<std::vector<VtxTerrain>> chunks;
    for (int cz = 0; cz < kChunks; ++cz)
        for (int cx = 0; cx < kChunks; ++cx) {
            std::vector<VtxTerrain> v;
            for (int j = 0; j < kChunkCells; ++j)
                for (int i = 0; i < kChunkCells; ++i) {
                    float xs[2] = {kOrigin + cx * kChunk + i * kCell, kOrigin + cx * kChunk + (i + 1) * kCell};
                    float zs[2] = {kOrigin + cz * kChunk + j * kCell, kOrigin + cz * kChunk + (j + 1) * kCell};
                    auto vert = [&](int a, int b) {
                        float x = xs[a], z = zs[b];
                        return VtxTerrain{x, height(x, z), z, 0, 1, 0, x / 4, z / 4, float(i + a) / kChunkCells,
                                          float(j + b) / kChunkCells};
                    };
                    VtxTerrain q[6] = {vert(0, 0), vert(0, 1), vert(1, 0), vert(1, 0), vert(0, 1), vert(1, 1)};
                    v.insert(v.end(), q, q + 6);
                }
            chunks.push_back(std::move(v));
        }
    LARGE_INTEGER freq, prev, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&prev);
    double sum = 0.0, worst = 0.0;
    int counted = 0, worstFrame = 0;
    std::vector<double> times;
    dev.SetGrassField(!g_terrainPbr, g_grassFieldDist, 5.0f, 0.5f, true);   // the game's default density
    if (g_grassBenchLamp) {    // the lamp's cube shadow map (the grass casts into it): per-pixel lights, overridden
        dev.SetPixelLighting(true);
        dev.SetLightOverride(true);
        dev.SetPointShadows(4);
    }
    if (g_terrainPbr) {
        std::vector<uint32_t> orm(64 * 64);
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x)
                orm[y * 64 + x] = x >= 26 && x < 38 ? 0xFFFF2800u : 0xFFC0D800u;   // path: roughness 0.16; grass 0.85
        dev.SetMaterialMaps(grassGround, dev.CreateTexture(64, 64, orm.data()), nullptr);
        dev.SetPixelLighting(true);
        Device::PbrSettings pbr;
        pbr.enabled = !g_pbrOff;
        pbr.debug = g_pbrDebug;
        dev.SetPbr(pbr);
    }
    std::vector<SandBox> sandBoxes;
    std::vector<VtxMesh> sandVerts;
    std::vector<uint16_t> sandIdx;
    if (g_sandObstacles) {
        auto box = [&](float x, float z, float w, float d, float lo, float hi, bool seeThrough) {
            const float g = height(x, z);
            sandBoxes.push_back({{x - w / 2, g + lo, z - d / 2}, {x + w / 2, g + hi, z + d / 2}, seeThrough});
        };
        if (g_sandArch) {                                          // an arch across the view: its legs, its lintel
            box(-48.0f, -5.5f, 2.0f, 5.0f, -1.0f, 6.0f, false);
            box(-48.0f, 5.5f, 2.0f, 5.0f, -1.0f, 6.0f, false);
            box(-48.0f, 0.0f, 2.0f, 6.0f, 3.5f, 6.0f, false);
        } else {
            box(-48.0f, 0.0f, 1.0f, 16.0f, -1.0f, 4.0f, false);    // a wall across the view, 12 units ahead
        }
        box(-53.0f, 5.0f, 0.8f, 0.8f, -1.0f, 8.0f, false);         // a tree's trunk
        box(-53.0f, 5.0f, 6.0f, 6.0f, 5.0f, 11.0f, true);          // ... its crown
        box(-55.0f, -6.0f, 3.0f, 3.0f, -1.0f, 2.5f, false);        // a boulder
        WriteSandPack("sand-obstacles.bin", sandBoxes);
        dev.LoadStatels("sand-obstacles.bin", 999);
        for (const SandBox& b : sandBoxes)
            if (!b.seeThrough) AddBox(sandVerts, sandIdx, b.lo, b.hi, 0xFF9A948C);
    }
    for (int frame = 0; frame < frames; ++frame) {
        if (frame == frames - 1) {
            dev.RequestScreenshot(shot);
            if (g_sandObstacles || g_sandStorm || g_sandForce > 0.0f)   // (and the sand map: sand-frame-sand.bmp)
                dev.RequestFrameDump("sand-frame.txt");
        } else if (g_grassBenchFilm > 0 && frame >= 60 && frame % g_grassBenchFilm == 0) {
            char name[32];
            std::snprintf(name, sizeof(name), "film_%03d.bmp", (frame - 60) / g_grassBenchFilm);
            dev.RequestScreenshot(name);
        }
        if (g_grassBenchFilm > 0)
            Sleep(16);
        if (g_grassBenchLoading > 0 && frame == g_grassBenchLoading)
            dev.UpdateTexture(grassGround, 0, 0, 0, 64, 64, grassPixels.data(), 64 * 4);
        Texture* const groundNow = g_grassBenchLoadingNull && frame < g_grassBenchLoading ? nullptr : grassGround;
        dev.BeginFrame();
        dev.SetViewport({0, 0, g_width, g_height, 0.0f, 1.0f});
        dev.Clear(CLEAR_TARGET | CLEAR_ZBUFFER, 0xFF6080A0, 1.0f);
        const float t = g_grassBenchStill ? 0.0f : float(frame);
        const float ex = -60.0f + 0.06f * t, ez = 10.0f * std::sin(t * 0.004f);
        const float yaw = 0.6f * std::sin(t * 0.007f) + g_grassBenchYaw;
        const float pitch = g_grassBenchPitch >= 0.0f ? std::min(g_grassBenchPitch, 1.5f) : 0.197f;
        const float rise = g_grassBenchZoomAt > 0 && frame >= g_grassBenchZoomAt ? g_grassBenchZoomRise : g_grassBenchRise;
        const float ey = height(ex, ez) + 1.0f + 6.0f * std::sin(pitch) + rise;
        const float fwd = 6.0f * std::cos(pitch);
        const float bx = -g_grassBenchBack * std::cos(pitch) * std::cos(yaw), bz = -g_grassBenchBack * std::cos(pitch) * std::sin(yaw);
        const float by = g_grassBenchBack * std::sin(pitch);
        dev.SetTransform(View, LookAtLH({ex + bx, ey + by, ez + bz},
                                        {ex + fwd * std::cos(yaw), ey - 6.0f * std::sin(pitch), ez + fwd * std::sin(yaw)},
                                        {0, 1, 0}));
        dev.SetTransform(Projection, PerspectiveLH(kPi / 3, float(g_width) / g_height, 0.3f, 300.0f));
        dev.SetTransform(World, Identity());
        if (frame == g_lightningAt) {
            const float strike[3] = {ex + 120.0f * std::cos(yaw + 0.3f), ey + 20.0f, ez + 120.0f * std::sin(yaw + 0.3f)};
            dev.TriggerLightning(strike);
        }
        dev.SetRenderState(RS_ZENABLE, 1);
        dev.SetRenderState(RS_ZWRITEENABLE, 1);
        dev.SetRenderState(RS_ZFUNC, CMP_LESSEQUAL);
        dev.SetRenderState(RS_CULLMODE, CULL_NONE);
        dev.SetRenderState(RS_ALPHATESTENABLE, 0);
        Material mat{{1, 1, 1, 1}, {1, 1, 1, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}, 0.0f};
        dev.SetMaterial(mat);
        Light sun{};
        sun.type = LIGHT_DIRECTIONAL;
        sun.diffuse = {0.9f, 0.85f, 0.75f, 1};
        sun.direction = {0.55f, -0.7f, 0.45f};
        if (g_sunDirSet) sun.direction = {g_sunDir[0], g_sunDir[1], g_sunDir[2]};
        if (g_sunColourSet) sun.diffuse = {g_sunColour[0], g_sunColour[1], g_sunColour[2], 1};
        if (g_grassBenchNight) {
            // As the game's nights: its light from the sun under the ground, pointing up, dim blue; a sky seen (the
            // moon: opposite the sun, night.cpp).
            sun.diffuse = {0.13f, 0.22f, 0.30f, 1};
            sun.direction = {-0.5f, 0.59f, 0.63f};
            dev.SetDrawVisual(uint32_t(VisualKind::Sky), "sky");
            dev.SetDrawVisual(0, nullptr);
        }
        dev.SetLight(0, sun);
        dev.LightEnable(0, true);
        if (g_grassBenchLamp) {    // a warm lamp just over the grass, 3 units ahead, a little left (its shadows in view)
            const float at = g_grassBenchLampAt, off = at == 3.0f ? 0.8f : 0.0f;   // (behind the walker: in line)
            const float lx = ex + at * std::cos(yaw) + off * std::sin(yaw), lz = ez + at * std::sin(yaw) - off * std::cos(yaw);
            Light lamp{};
            lamp.type = LIGHT_POINT;
            lamp.diffuse = {1.6f, 1.1f, 0.6f, 1};
            lamp.position = {lx, height(lx, lz) + 0.6f, lz};
            lamp.range = 8;
            lamp.attenuation0 = 0.3f;
            lamp.attenuation1 = 0.2f;
            if (g_spotDegrees > 0.0f) {        // a floodlight, as over Borealis's bridges: high, far-reaching, down
                lamp.type = LIGHT_SPOT;
                lamp.direction = {0.05f, -1.0f, 0.03f};
                lamp.phi = g_spotDegrees * kPi / 180.0f;
                lamp.theta = 0.7f * lamp.phi;
                lamp.falloff = 1.0f;
                lamp.position.y += 12.0f;
                lamp.range = 1041.0f;
                lamp.attenuation0 = 0.2f;
                lamp.attenuation1 = 0.0f;
                lamp.attenuation2 = 0.0009f;
            }
            dev.SetLight(1, lamp);
            dev.LightEnable(1, true);
        }
        dev.SetTextureStageState(0, TSS_COLORARG1, TA_TEXTURE);
        dev.SetTextureStageState(0, TSS_COLORARG2, TA_DIFFUSE);
        dev.SetTextureStageState(0, TSS_ALPHAOP, TOP_SELECTARG1);
        dev.SetTextureStageState(0, TSS_ALPHAARG1, TA_TEXTURE);
        for (const auto& c : chunks) {               // base pass: the ground's own texture, unlit
            dev.SetRenderState(RS_LIGHTING, 0);
            dev.SetRenderState(RS_ALPHABLENDENABLE, 0);
            dev.SetRenderState(RS_ZFUNC, CMP_LESSEQUAL);
            dev.SetTexture(0, groundNow);
            dev.SetTextureStageState(0, TSS_TEXCOORDINDEX, 0);
            dev.SetTextureStageState(0, TSS_COLOROP, TOP_SELECTARG1);
            dev.DrawPrimitive(TriangleList, kFvfTerrain, c.data(), uint32_t(c.size()));
            // light pass: lightmap + ambient multiplying it
            dev.SetRenderState(RS_LIGHTING, 1);
            dev.SetRenderState(RS_AMBIENT, g_grassBenchNight ? 0xFF181C28 : 0xFF515151);
            dev.SetRenderState(RS_ALPHABLENDENABLE, 1);
            dev.SetRenderState(RS_SRCBLEND, BLEND_ZERO);
            dev.SetRenderState(RS_DESTBLEND, BLEND_SRCCOLOR);
            dev.SetRenderState(RS_ZFUNC, CMP_EQUAL);
            dev.SetTexture(0, lightmap);
            dev.SetTextureStageState(0, TSS_TEXCOORDINDEX, 1);
            dev.SetTextureStageState(0, TSS_COLOROP, TOP_ADD);
            dev.DrawPrimitive(TriangleList, kFvfTerrain, c.data(), uint32_t(c.size()));
        }
        dev.SetTextureStageState(0, TSS_TEXCOORDINDEX, 0);
        dev.SetRenderState(RS_ZFUNC, CMP_LESSEQUAL);
        if (g_grassBenchWalker) {   // a lit column a few units ahead, its vertices moving each frame (CPU-skinned)
            float wx = ex + 4.0f * std::cos(yaw) + 1.5f * std::sin(t * 0.02f), wz = ez + 4.0f * std::sin(yaw);
            if (g_grassBenchCross) {     // from 4 units left of the view's centre to 1 right, 4 ahead
                const float s = -4.0f + 5.0f * float(frame) / float(std::max(frames - 1, 1));   // ends right of centre
                wx = ex + 4.0f * std::cos(yaw) + s * std::sin(yaw);
                wz = ez + 4.0f * std::sin(yaw) - s * std::cos(yaw);
            }
            const float wy = height(wx, wz);
            std::vector<VtxMesh> cv;
            std::vector<uint16_t> ci;
            const int kSides = 8, kRings = 5;
            for (int r = 0; r < kRings; ++r)
                for (int sd = 0; sd < kSides; ++sd) {
                    const float a = 6.28318f * float(sd) / float(kSides), y = 0.05f + 1.7f * float(r) / float(kRings - 1);
                    const float rad = 0.15f + 0.2f * std::sin(3.14159f * float(r) / float(kRings - 1));
                    const float sway = r > 2 ? 0.02f * std::sin(t * 0.9f) : 0.0f;
                    cv.push_back({wx + rad * std::cos(a) + sway, wy + y, wz + rad * std::sin(a), std::cos(a), 0,
                                  std::sin(a), 0xFFFFFFFF, 0, 0});
                }
            for (int r = 0; r + 1 < kRings; ++r)
                for (int sd = 0; sd < kSides; ++sd) {
                    const uint16_t a = uint16_t(r * kSides + sd), b = uint16_t(r * kSides + (sd + 1) % kSides);
                    const uint16_t q[6] = {a, uint16_t(a + kSides), b, b, uint16_t(a + kSides), uint16_t(b + kSides)};
                    ci.insert(ci.end(), q, q + 6);
                }
            dev.SetRenderState(RS_LIGHTING, 1);
            dev.SetRenderState(RS_ALPHABLENDENABLE, 0);
            dev.SetTexture(0, nullptr);
            dev.SetTextureStageState(0, TSS_COLOROP, TOP_SELECTARG2);
            dev.DrawIndexedPrimitive(TriangleList, kFvfMesh, cv.data(), uint32_t(cv.size()), ci.data(), uint32_t(ci.size()));
        }
        if (g_grassBenchPlatform) {   // a slab 6 x 4 units, its top 0.1 over the ground, 5 units ahead of the start
            const float px = -60.0f + 5.0f, pz = 0.0f, top = height(px, pz) + 0.1f;
            auto v = [&](float x, float z) { return VtxMesh{px + x, top, pz + z, 0, 1, 0, 0xFF808890, 0, 0}; };
            VtxMesh q[6] = {v(-3, -2), v(-3, 2), v(3, 2), v(-3, -2), v(3, 2), v(3, -2)};
            dev.SetRenderState(RS_LIGHTING, 1);
            dev.SetRenderState(RS_ALPHABLENDENABLE, 0);
            dev.SetTexture(0, nullptr);
            dev.SetTextureStageState(0, TSS_COLOROP, TOP_SELECTARG2);
            dev.DrawPrimitive(TriangleList, kFvfMesh, q, 6);
        }
        {   // a blended draw, as the game's foliage: the grass goes in before it
            dev.SetRenderState(RS_LIGHTING, 0);
            dev.SetRenderState(RS_ALPHABLENDENABLE, 1);
            dev.SetRenderState(RS_SRCBLEND, BLEND_SRCALPHA);
            dev.SetRenderState(RS_DESTBLEND, BLEND_INVSRCALPHA);
            dev.SetRenderState(RS_ZWRITEENABLE, 0);
            dev.SetTexture(0, nullptr);
            dev.SetTextureStageState(0, TSS_COLOROP, TOP_SELECTARG2);
            VtxMesh q[4] = {{ex + 30, 40, ez - 2, 0, 1, 0, 0x40FFFFFF, 0, 0}, {ex + 30, 40, ez + 2, 0, 1, 0, 0x40FFFFFF, 1, 0},
                            {ex + 30, 44, ez + 2, 0, 1, 0, 0x40FFFFFF, 1, 1}, {ex + 30, 44, ez - 2, 0, 1, 0, 0x40FFFFFF, 0, 1}};
            dev.DrawPrimitive(TriangleFan, kFvfMesh, q, 4);
            dev.SetRenderState(RS_ZWRITEENABLE, 1);
            dev.SetRenderState(RS_ALPHABLENDENABLE, 0);
        }
        if (g_sandWindSet) {      // (the game's weather wind)
            Device::WeatherState w;
            w.wind[0] = g_sandWind[0];
            w.wind[2] = g_sandWind[1];
            dev.SetWeather(w);
        }
        if (!sandVerts.empty()) {
            dev.SetTexture(0, nullptr);
            dev.SetTextureStageState(0, TSS_COLOROP, TOP_SELECTARG2);
            dev.SetRenderState(RS_LIGHTING, 1);
            dev.DrawIndexedPrimitive(TriangleList, kFvfMesh, sandVerts.data(), uint32_t(sandVerts.size()), sandIdx.data(),
                                     uint32_t(sandIdx.size()));
            dev.SetTextureStageState(0, TSS_COLOROP, TOP_MODULATE);
        }
        if (g_grassBenchStorm) {   // AO's sandstorm tint: a screen-sized blended sprite 5 units ahead, writing depth, ZFUNC ALWAYS
            const float fx = std::cos(yaw), fz = std::sin(yaw), rx = -fz, rz = fx;
            const float cx = ex + 5.0f * fx, cy = ey - 0.5f, cz = ez + 5.0f * fz;
            auto v = [&](float a, float b) { return VtxMesh{cx + a * rx, cy + b, cz + a * rz, 0, 1, 0, 0x60C07830, 0, 0}; };
            VtxMesh q[4] = {v(-12, -8), v(12, -8), v(12, 8), v(-12, 8)};
            dev.SetRenderState(RS_LIGHTING, 0);
            dev.SetRenderState(RS_ALPHABLENDENABLE, 1);
            dev.SetRenderState(RS_SRCBLEND, BLEND_SRCALPHA);
            dev.SetRenderState(RS_DESTBLEND, BLEND_INVSRCALPHA);
            dev.SetRenderState(RS_ZWRITEENABLE, 1);
            dev.SetRenderState(RS_ZFUNC, CMP_ALWAYS);
            static Texture* tintTex = [&] {      // (the game's: an 8 x 8 white texture under the vertex colour)
                std::vector<uint32_t> white(64, 0xFFFFFFFFu);
                return dev.CreateTexture(8, 8, white.data());
            }();
            dev.SetTexture(0, tintTex);
            dev.SetTextureStageState(0, TSS_COLOROP, TOP_SELECTARG2);
            dev.SetTextureStageState(0, TSS_ALPHAOP, TOP_SELECTARG2);   // (the vertex colour's alpha)
            dev.SetTextureStageState(0, TSS_ALPHAARG2, TA_DIFFUSE);
            dev.DrawPrimitive(TriangleFan, kFvfMesh, q, 4);
            dev.SetTextureStageState(0, TSS_ALPHAOP, TOP_SELECTARG1);
            dev.SetRenderState(RS_ZFUNC, CMP_LESSEQUAL);
            dev.SetRenderState(RS_ALPHABLENDENABLE, 0);
        }
        dev.EndFrame();
        QueryPerformanceCounter(&now);
        const double ms = double(now.QuadPart - prev.QuadPart) * 1000.0 / double(freq.QuadPart);
        prev = now;
        if (frame >= 60) {
            sum += ms;
            if (ms > worst) {
                worst = ms;
                worstFrame = frame;
            }
            ++counted;
            times.push_back(ms);
        }
    }
    std::sort(times.begin(), times.end());
    const double p99 = times.empty() ? 0.0 : times[size_t(double(times.size() - 1) * 0.99)];
    std::printf("grass bench: %d frames, mean %.3f ms, 99th percentile %.3f ms, worst %.3f ms (frame %d)\n", counted,
                sum / std::max(counted, 1), p99, worst, worstFrame);
}

// City benchmark (--city-bench, docs/render-batching-plan.md P0): shaped like a frame of West Bank's city in the game
// (frame dump and log of 2026-10-14: ~3,400-3,600 draws, the render thread the limit). Through ThreadedDevice, the
// statics by mesh handle (RegisterMesh / DrawMesh, as the proxy's): ~2,000 objects of 1-2 pieces drawn from 160 small
// mesh kinds (FVF 0x112, ~37 vertices; a few kinds make most draws, as the game's), 85% of them blended
// SRCALPHA/INVSRCALPHA with depth writes (the opaque fast path, as the game draws its statics), 170 fence pieces with
// holes (the foliage path: two-pass edges), 40 additive panes at the end; 160 ground chunks (216 indices into one of 4
// static vertex buffers) in two passes (AnarchyGround_t: base, then the multiplying light pass) with the ground grass; a sun with shadows and 7
// point lights; the camera circling slowly.
// The clock is fixed (1/60 s a frame), so a run of N frames ends on the same picture (--shot; --dump: its draws).
// After --city-warm frames a profile window opens; at the end the render thread's sections are logged ("city bench:"
// lines, stderr) and the calling thread's frame printed.
int g_cityWarm = 60;              // --city-warm N: frames before the measured window
bool g_cityNoGrass = false;       // --city-no-grass: without the ground grass (on by default, as in the game)
bool g_cityNoCull = false;        // --city-no-cull: every object drawn every frame (the game draws what is in view)
std::string g_cityStatels;        // --city-statels PACK: playfield 790's placed objects (their shadows), the scene moved
                                  // to West Bank's city (1380, 0, 2950) where they stand
int g_cityMovers = 0;             // --city-movers N: the first N objects sway to and fro a little each frame (moving
                                  // pieces of meshes placed many times: placements.h's young places)
int g_cityLamps = 0;              // --city-lamps N: N lamps more (a lamp-lit outpost has ~90), set each frame as the game
                                  // sets lights, and handed over as its scene lights: on a jittered grid, 3 to 6 units
                                  // up, ranges 12-28 (the frame lights' per-pixel cost, docs/render-perf-plan.md)

namespace city {
struct Vtx { float x, y, z, nx, ny, nz, u, v; };   // FVF 0x112, the game's statics
constexpr uint32_t kFvf = FVF_XYZ | FVF_NORMAL | (1 << 8);

// A box without its bottom (5 quads: 20 vertices, 30 indices - the game's most drawn static), each side face split
// into s x s quads; its base on y = 0.
void AddBox(std::vector<Vtx>& v, std::vector<uint16_t>& idx, float w, float h, float d, int s)
{
    struct Face { Vector o, u, w, n; };
    const Face faces[5] = {{{-w, 0, -d}, {2 * w, 0, 0}, {0, h, 0}, {0, 0, -1}}, {{w, 0, d}, {-2 * w, 0, 0}, {0, h, 0}, {0, 0, 1}},
                           {{w, 0, -d}, {0, 0, 2 * d}, {0, h, 0}, {1, 0, 0}}, {{-w, 0, d}, {0, 0, -2 * d}, {0, h, 0}, {-1, 0, 0}},
                           {{-w, h, -d}, {2 * w, 0, 0}, {0, 0, 2 * d}, {0, 1, 0}}};
    for (int f = 0; f < 5; ++f) {
        const Face& F = faces[f];
        const int n = f == 4 ? 1 : s;
        const uint16_t base = uint16_t(v.size());
        for (int j = 0; j <= n; ++j)
            for (int i = 0; i <= n; ++i) {
                const float a = float(i) / n, b = float(j) / n;
                v.push_back({F.o.x + a * F.u.x + b * F.w.x, F.o.y + a * F.u.y + b * F.w.y, F.o.z + a * F.u.z + b * F.w.z,
                             F.n.x, F.n.y, F.n.z, a * 2, b * 2});
            }
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) {
                const uint16_t q = uint16_t(base + j * (n + 1) + i);
                const uint16_t t[6] = {q, uint16_t(q + n + 1), uint16_t(q + 1), uint16_t(q + 1), uint16_t(q + n + 1),
                                       uint16_t(q + n + 2)};
                idx.insert(idx.end(), t, t + 6);
            }
    }
}

// A cylinder (k sides, capped), its base on y = 0.
void AddCylinder(std::vector<Vtx>& v, std::vector<uint16_t>& idx, float r, float h, int k)
{
    const uint16_t base = uint16_t(v.size());
    for (int i = 0; i <= k; ++i) {
        const float a = 2 * kPi * i / k, c = std::cos(a), s = std::sin(a);
        v.push_back({r * c, 0, r * s, c, 0, s, float(i) / k, 0});
        v.push_back({r * c, h, r * s, c, 0, s, float(i) / k, 1});
    }
    for (int i = 0; i < k; ++i) {
        const uint16_t q = uint16_t(base + 2 * i);
        const uint16_t t[6] = {q, uint16_t(q + 1), uint16_t(q + 2), uint16_t(q + 2), uint16_t(q + 1), uint16_t(q + 3)};
        idx.insert(idx.end(), t, t + 6);
    }
    const uint16_t centre = uint16_t(v.size());
    v.push_back({0, h, 0, 0, 1, 0, 0.5f, 0.5f});
    for (int i = 0; i <= k; ++i) {
        const float a = 2 * kPi * i / k;
        v.push_back({r * std::cos(a), h, r * std::sin(a), 0, 1, 0, 0.5f + 0.5f * std::cos(a), 0.5f + 0.5f * std::sin(a)});
    }
    for (int i = 0; i < k; ++i) {
        const uint16_t t[3] = {centre, uint16_t(centre + 2 + i), uint16_t(centre + 1 + i)};
        idx.insert(idx.end(), t, t + 3);
    }
}

// Upright panels w wide, h high in the x-y plane: s columns (a fence); with `cross`, a second one turned 90 degrees.
void AddPanel(std::vector<Vtx>& v, std::vector<uint16_t>& idx, float w, float h, int s, bool cross = false)
{
    for (int p = 0; p < (cross ? 2 : 1); ++p) {
        const uint16_t base = uint16_t(v.size());
        for (int i = 0; i <= s; ++i) {
            const float t = -w + 2 * w * i / s;
            if (p == 0) {
                v.push_back({t, 0, 0, 0, 0, -1, float(i), 1});
                v.push_back({t, h, 0, 0, 0, -1, float(i), 0});
            } else {
                v.push_back({0, 0, t, -1, 0, 0, float(i), 1});
                v.push_back({0, h, t, -1, 0, 0, float(i), 0});
            }
        }
        for (int i = 0; i < s; ++i) {
            const uint16_t q = uint16_t(base + 2 * i);
            const uint16_t t[6] = {q, uint16_t(q + 1), uint16_t(q + 2), uint16_t(q + 2), uint16_t(q + 1), uint16_t(q + 3)};
            idx.insert(idx.end(), t, t + 6);
        }
    }
}

uint32_t Hash(uint32_t x)
{
    x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15; x *= 0x846CA68Bu; x ^= x >> 16;
    return x;
}
float Rand(uint32_t seed, uint32_t salt) { return float(Hash(seed * 0x9E3779B9u + salt) & 0xFFFFFF) / float(0x1000000); }

struct Piece { uint32_t mesh; Texture* tex; bool emissive; };
struct Object {
    float x, z, yaw;
    Piece piece[2];
    uint32_t pieces;
    enum Kind { Blended, Solid, Holes, Additive } kind;   // Blended: SRCALPHA/INVSRCALPHA writing depth (most)
    uint32_t visual;
};
}  // namespace city

template <typename D>
void RunCityBench(D& dev, int frames, const std::string& shot, const std::string& dump)
{
    using namespace city;
    Device::SetFixedClock(1.0 / 60.0);
    // Textures: 60 opaque, 4 with holes (fences), 2 for the additive panes.
    std::vector<Texture*> opaque, holes, glow;
    for (uint32_t i = 0; i < 60; ++i) {
        const uint32_t a = 0xFF000000u | (0x40 + 3 * i) << 16 | (0x38 + 11 * (i % 9)) << 8 | (0x30 + 13 * (i % 7));
        auto px = Checker(64, 2 + i % 7, a, 0xFF000000u | ((a & 0xFEFEFE) >> 1));
        opaque.push_back(dev.CreateTexture(64, 64, px.data()));
    }
    for (uint32_t i = 0; i < 4; ++i) {
        auto px = Checker(64, 4 + 2 * i, 0xFF706048u + 0x080808u * i, 0x00000000);
        holes.push_back(dev.CreateTexture(64, 64, px.data()));
    }
    for (uint32_t i = 0; i < 2; ++i) {
        std::vector<uint32_t> px(64 * 64, i ? 0x80F0C060u : 0x8060C0F0u);
        glow.push_back(dev.CreateTexture(64, 64, px.data()));
    }
    // Meshes, registered once (ids from 1, as the proxy hands them out).
    uint32_t nextId = 1;
    size_t vertexBytes = 0;
    auto add = [&](const std::vector<Vtx>& v, const std::vector<uint16_t>& idx) {
        auto vs = std::make_shared<std::vector<uint8_t>>(reinterpret_cast<const uint8_t*>(v.data()),
                                                         reinterpret_cast<const uint8_t*>(v.data() + v.size()));
        auto is = std::make_shared<std::vector<uint8_t>>(reinterpret_cast<const uint8_t*>(idx.data()),
                                                         reinterpret_cast<const uint8_t*>(idx.data() + idx.size()));
        vertexBytes += vs->size();
        const uint32_t id = nextId++;
        dev.RegisterMesh(id, TriangleList, kFvf, ThreadedDevice::SharedVertices(vs), 0, uint32_t(v.size()),
                         ThreadedDevice::SharedVertices(is), uint32_t(idx.size()));
        return id;
    };
    // 160 static kinds, the first ones drawn most (weight 1 / (k + 1)^1.1: kind 0 ~ a fifth of the pieces).
    constexpr uint32_t kKinds = 160;
    std::vector<uint32_t> kinds;
    std::vector<float> cumulative;
    float total = 0.0f;
    for (uint32_t k = 0; k < kKinds; ++k) {
        std::vector<Vtx> v;
        std::vector<uint16_t> idx;
        const float w = 0.4f + 2.6f * Rand(k, 1), h = 0.5f + 5.0f * Rand(k, 2), d = 0.4f + 2.6f * Rand(k, 3);
        switch (k < 4 ? 0 : Hash(k) % 10) {
        case 4: AddPanel(v, idx, w, h, 1); break;                    // 4 / 6
        case 5: AddPanel(v, idx, w, h, 1, true); break;              // 8 / 12
        case 6: AddBox(v, idx, w, h, d, 2); break;                   // 40 / 102
        case 7: AddCylinder(v, idx, 0.5f * w, h, 8); break;          // 28 / 72
        case 8: AddBox(v, idx, w, h, d, 3); break;                   // 68 / 222
        default: AddBox(v, idx, w, h, d, 1); break;                  // 20 / 30
        }
        kinds.push_back(add(v, idx));
        total += 1.0f / std::pow(float(k + 1), 1.1f);
        cumulative.push_back(total);
    }
    auto pickKind = [&](uint32_t seed) {
        const float r = Rand(seed, 77) * total;
        return uint32_t(std::lower_bound(cumulative.begin(), cumulative.end(), r) - cumulative.begin());
    };
    std::vector<uint32_t> fenceKinds, paneKinds;
    for (uint32_t k = 0; k < 8; ++k) {
        std::vector<Vtx> v;
        std::vector<uint16_t> idx;
        AddPanel(v, idx, 2.0f + k * 0.25f, 1.2f + 0.1f * k, 4 + k);
        fenceKinds.push_back(add(v, idx));
    }
    for (uint32_t k = 0; k < 4; ++k) {
        std::vector<Vtx> v;
        std::vector<uint16_t> idx;
        AddPanel(v, idx, 1.0f + 0.3f * k, 2.0f + 0.4f * k, 1);
        paneKinds.push_back(add(v, idx));
    }
    std::vector<Object> objects;
    constexpr int kLotsX = 25, kLotsZ = 24;
    constexpr float kLot = 12.0f;
    uint32_t visual = 0x10000;
    auto lotCentre = [&](uint32_t lot, float& cx, float& cz) {
        cx = (float(lot % kLotsX) - kLotsX / 2.0f + 0.5f) * kLot;
        cz = (float(lot / kLotsX) - kLotsZ / 2.0f + 0.5f) * kLot;
    };
    // (Culled to the view as the game does, ~40% are drawn: ~3,500 draws, as the game's frame.)
    for (uint32_t i = 0; i < 4800; ++i) {        // statics: 1 or 2 pieces (~1.47 a static, as the game's 3,029 / 2,067)
        float cx, cz;
        lotCentre(i % (kLotsX * kLotsZ), cx, cz);
        Object o{};
        o.x = cx + (Rand(i, 10) - 0.5f) * kLot * 0.8f;
        o.z = cz + (Rand(i, 11) - 0.5f) * kLot * 0.8f;
        o.yaw = Rand(i, 12) * 2 * kPi;
        o.pieces = Rand(i, 13) < 0.47f ? 2 : 1;
        for (uint32_t p = 0; p < o.pieces; ++p) {
            const uint32_t k = pickKind(i * 2 + p);
            o.piece[p] = {kinds[k], opaque[(k * 7 + p) % opaque.size()], k % 53 == 7};   // (texture, material: the kind's)
        }
        o.kind = Rand(i, 14) < 0.85f ? Object::Blended : Object::Solid;
        o.visual = visual++;
        objects.push_back(o);
    }
    for (uint32_t i = 0; i < 200; ++i) {         // fences on 200 lots: ~170 pieces with holes in view (the game's foliage)
        float cx, cz;
        lotCentre(Hash(i + 500) % (kLotsX * kLotsZ), cx, cz);
        for (int s = 0; s < 2; ++s) {
            Object o{};
            o.x = s ? cx - kLot * 0.45f : cx;
            o.z = s ? cz : cz - kLot * 0.45f;
            o.yaw = s ? kPi * 0.5f : 0.0f;
            o.pieces = 1;
            o.piece[0] = {fenceKinds[(i + 3 * s) % 8], holes[(i + s) % 4], false};
            o.kind = Object::Holes;
            o.visual = visual++;
            objects.push_back(o);
        }
    }
    std::vector<Object> panes;                   // additive, no depth writes, after the rest (the game's effects)
    for (uint32_t i = 0; i < 95; ++i) {
        float cx, cz;
        lotCentre(Hash(i + 999) % (kLotsX * kLotsZ), cx, cz);
        Object o{};
        o.x = cx + 4.5f;
        o.z = cz + 4.5f;
        o.yaw = Rand(i, 30) * kPi;
        o.pieces = 1;
        o.piece[0] = {paneKinds[i % 4], glow[i % 2], false};
        o.kind = Object::Additive;
        o.visual = visual++;
        panes.push_back(o);
    }
    // The ground: 16 x 10 chunks, base pass + multiplying light pass.
    std::vector<uint32_t> groundPixels(64 * 64), lightmapPixels(64 * 64);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            groundPixels[y * 64 + x] = ((x / 4 + y / 4) & 1) ? 0xFF3C6A2A : 0xFF2E5320;
            const uint32_t l = uint32_t(170 + 50 * std::sin(x * 0.2f) * std::cos(y * 0.15f));
            lightmapPixels[y * 64 + x] = 0xFF000000u | l << 16 | l << 8 | l;
        }
    Texture* groundTex = dev.CreateTexture(64, 64, groundPixels.data());
    Texture* lightmap = dev.CreateTexture(64, 64, lightmapPixels.data());
    struct VtxTerrain { float x, y, z, nx, ny, nz, u0, v0, u1, v1; };
    const uint32_t kFvfTerrain = FVF_XYZ | FVF_NORMAL | (2 << 8);
    // As the game's: big static vertex buffers (a quadrant each here), every chunk drawn with its own few indices.
    constexpr int kChunksX = 16, kChunksZ = 10, kChunkCells = 6, kQuads = 2;
    constexpr float kChunkX = 20.0f, kChunkZ = 30.0f;
    constexpr int kQuadChunksX = kChunksX / kQuads, kQuadChunksZ = kChunksZ / kQuads;
    constexpr int kGridX = kQuadChunksX * kChunkCells + 1, kGridZ = kQuadChunksZ * kChunkCells + 1;
    struct Chunk { int quadrant; std::vector<uint16_t> indices; };
    std::vector<ThreadedDevice::SharedVertices> groundBuffers;
    std::vector<Chunk> chunks;
    for (int qz = 0; qz < kQuads; ++qz)
        for (int qx = 0; qx < kQuads; ++qx) {
            std::vector<VtxTerrain> v;
            const float x0 = -kChunkX * kChunksX / 2 + qx * kQuadChunksX * kChunkX;
            const float z0 = -kChunkZ * kChunksZ / 2 + qz * kQuadChunksZ * kChunkZ;
            for (int j = 0; j < kGridZ; ++j)
                for (int i = 0; i < kGridX; ++i) {
                    const float x = x0 + kChunkX * i / kChunkCells, z = z0 + kChunkZ * j / kChunkCells;
                    v.push_back({x, 0.0f, z, 0, 1, 0, x / 4, z / 4, float(i % kChunkCells) / kChunkCells,
                                 float(j % kChunkCells) / kChunkCells});
                }
            groundBuffers.push_back(std::make_shared<std::vector<uint8_t>>(
                reinterpret_cast<const uint8_t*>(v.data()), reinterpret_cast<const uint8_t*>(v.data() + v.size())));
            for (int cz = 0; cz < kQuadChunksZ; ++cz)
                for (int cx = 0; cx < kQuadChunksX; ++cx) {
                    Chunk c{qz * kQuads + qx, {}};
                    for (int j = cz * kChunkCells; j < (cz + 1) * kChunkCells; ++j)
                        for (int i = cx * kChunkCells; i < (cx + 1) * kChunkCells; ++i) {
                            const uint16_t q = uint16_t(j * kGridX + i);
                            const uint16_t t[6] = {q, uint16_t(q + kGridX), uint16_t(q + 1), uint16_t(q + 1),
                                                   uint16_t(q + kGridX), uint16_t(q + kGridX + 1)};
                            c.indices.insert(c.indices.end(), t, t + 6);
                        }
                    chunks.push_back(std::move(c));
                }
        }
    const uint32_t groundVertices = uint32_t(kGridX * kGridZ);
    if (g_grassFieldOn && !g_cityNoGrass)
        dev.SetGrassField(true, g_grassFieldDist, 5.0f, 0.5f, true);
    // (--city-statels: everything moved to the placed objects' city; its ground buffers too.)
    const float ox = g_cityStatels.empty() ? 0.0f : 1380.0f, oz = g_cityStatels.empty() ? 0.0f : 2950.0f;
    if (!g_cityStatels.empty()) {
        dev.SetStatelMode(2);
        dev.LoadStatels(g_cityStatels.c_str(), 790);
        for (auto& b : groundBuffers) {
            auto moved = std::make_shared<std::vector<uint8_t>>(*b);
            auto* v = reinterpret_cast<VtxTerrain*>(moved->data());
            for (size_t i = 0; i < moved->size() / sizeof(VtxTerrain); ++i) { v[i].x += ox; v[i].z += oz; }
            b = moved;
        }
        for (Object& o : objects) { o.x += ox; o.z += oz; }
        for (Object& o : panes) { o.x += ox; o.z += oz; }
    }
    std::printf("city bench: %u meshes (%.2f MB of vertices), %zu objects + %zu panes, %zu ground chunks, %d frames "
                "(window from %d)\n", nextId - 1, double(vertexBytes) / 1048576.0, objects.size(), panes.size(),
                chunks.size(), frames, g_cityWarm);

    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    double frameSum = 0.0, recordSum = 0.0;
    int counted = 0;
    uint32_t drawsPerFrame = 0;
    QueryPerformanceCounter(&t0);
    for (int frame = 0; frame < frames; ++frame) {
        if (frame == g_cityWarm) dev.ProfileWindow(true, "");
        if (frame == frames - 1 && !shot.empty()) dev.RequestScreenshot(shot);
        if (frame == frames - 1 && !dump.empty()) dev.RequestFrameDump(dump);
        LARGE_INTEGER r0, r1;
        QueryPerformanceCounter(&r0);
        uint32_t draws = 0;
        dev.BeginFrame();
        dev.SetViewport({0, 0, g_width, g_height, 0.0f, 1.0f});
        dev.Clear(CLEAR_TARGET | CLEAR_ZBUFFER, 0xFF6080A0, 1.0f);
        const float a = 0.6f + 0.0015f * float(frame);
        const Vector eye{ox + 70.0f * std::sin(a), 14.0f, oz - 70.0f * std::cos(a)};
        dev.SetTransform(View, LookAtLH(eye, {eye.x - 40.0f * std::sin(a), 2.0f, eye.z + 40.0f * std::cos(a)}, {0, 1, 0}));
        // What the game would draw: in front of the camera, within its view sideways (a margin for big things).
        const float fx = -std::sin(a), fz = std::cos(a), tanHalf = std::tan(kPi / 6) * float(g_width) / float(g_height);
        auto inView = [&](const Object& o) {
            if (g_cityNoCull) return true;
            const float dx = o.x - eye.x, dz = o.z - eye.z;
            const float depth = dx * fx + dz * fz, side = dx * fz - dz * fx;
            return depth > -6.0f && std::fabs(side) < std::max(depth, 0.0f) * tanHalf + 8.0f;
        };
        dev.SetTransform(Projection, PerspectiveLH(kPi / 3, float(g_width) / g_height, 0.5f, 300.0f));
        dev.SetTransform(World, Identity());
        dev.SetRenderState(RS_ZENABLE, 1);
        dev.SetRenderState(RS_ZWRITEENABLE, 1);
        dev.SetRenderState(RS_ZFUNC, CMP_LESSEQUAL);
        dev.SetRenderState(RS_CULLMODE, CULL_NONE);
        dev.SetRenderState(RS_ALPHATESTENABLE, 0);
        dev.SetRenderState(RS_ALPHABLENDENABLE, 0);
        Light sun{};
        sun.type = LIGHT_DIRECTIONAL;
        sun.diffuse = {0.9f, 0.85f, 0.75f, 1};
        sun.direction = {0.55f, -0.7f, 0.45f};
        dev.SetLight(0, sun);
        dev.LightEnable(0, true);
        for (uint32_t i = 1; i < 8; ++i) {       // street lamps
            Light l{};
            l.type = LIGHT_POINT;
            l.diffuse = {1.0f, 0.85f, 0.6f, 1};
            l.position = {ox + (Rand(i, 40) - 0.5f) * 120.0f, 3.0f, oz + (Rand(i, 41) - 0.5f) * 120.0f};
            l.range = 20.0f;
            l.attenuation0 = 1.0f;
            l.attenuation1 = 0.1f;
            dev.SetLight(i, l);
            dev.LightEnable(i, true);
        }
        if (g_cityLamps > 0) {
            static std::vector<Device::SceneLight> lamps;
            if (lamps.empty()) {
                const int side = int(std::ceil(std::sqrt(float(g_cityLamps))));
                for (int i = 0; i < g_cityLamps; ++i) {
                    Light l{};
                    l.type = LIGHT_POINT;
                    l.diffuse = {1.0f, 0.8f + 0.1f * Rand(uint32_t(i), 44), 0.55f, 1};
                    const float gx = (float(i % side) + 0.5f) / float(side) - 0.5f, gz = (float(i / side) + 0.5f) / float(side) - 0.5f;
                    l.position = {ox + (gx + (Rand(uint32_t(i), 42) - 0.5f) / float(side)) * 120.0f,
                                  3.0f + 3.0f * Rand(uint32_t(i), 45),
                                  oz + (gz + (Rand(uint32_t(i), 43) - 0.5f) / float(side)) * 120.0f};
                    l.range = 12.0f + 16.0f * Rand(uint32_t(i), 46);
                    l.attenuation0 = 1.0f;
                    l.attenuation1 = 0.1f;
                    if (g_spotDegrees > 0.0f && i < 3) {   // --point-shadow-spot DEG: three floodlights, as Borealis's
                        l.type = LIGHT_SPOT;
                        l.diffuse = {1, 1, 1, 1};
                        l.position.y = 16.0f;
                        l.direction = {0.03f, -1.0f, 0.02f};
                        l.phi = g_spotDegrees * kPi / 180.0f;
                        l.theta = 0.7f * l.phi;
                        l.falloff = 1.0f;
                        l.range = 1041.0f;
                        l.attenuation0 = 0.2f;
                        l.attenuation1 = 0.0f;
                        l.attenuation2 = 0.00009f;
                    }
                    lamps.push_back({l, 0});
                }
            }
            dev.SetSceneLights(lamps.data(), uint32_t(lamps.size()));
            // (The frame's lights are the ones the game sets while drawing - rvk CaptureLight: each lamp set once, in
            // a slot of its own past the 8 the draws use, and switched off again.)
            for (size_t i = 0; i < lamps.size(); ++i) {
                dev.SetLight(uint32_t(8 + i), lamps[i].light);
                dev.LightEnable(uint32_t(8 + i), true);
                dev.LightEnable(uint32_t(8 + i), false);
            }
        }
        dev.SetTextureStageState(0, TSS_COLORARG1, TA_TEXTURE);
        dev.SetTextureStageState(0, TSS_COLORARG2, TA_DIFFUSE);
        dev.SetTextureStageState(0, TSS_ALPHAOP, TOP_SELECTARG1);
        dev.SetTextureStageState(0, TSS_ALPHAARG1, TA_TEXTURE);
        for (const auto& c : chunks) {           // the ground
            dev.SetRenderState(RS_LIGHTING, 0);
            dev.SetRenderState(RS_ALPHABLENDENABLE, 0);
            dev.SetRenderState(RS_ZFUNC, CMP_LESSEQUAL);
            dev.SetTexture(0, groundTex);
            dev.SetTextureStageState(0, TSS_TEXCOORDINDEX, 0);
            dev.SetTextureStageState(0, TSS_COLOROP, TOP_SELECTARG1);
            dev.DrawIndexedPrimitiveShared(TriangleList, kFvfTerrain, groundBuffers[c.quadrant], 0, groundVertices,
                                           c.indices.data(), uint32_t(c.indices.size()));
            dev.SetRenderState(RS_LIGHTING, 1);
            dev.SetRenderState(RS_AMBIENT, 0xFF515151);
            dev.SetRenderState(RS_ALPHABLENDENABLE, 1);
            dev.SetRenderState(RS_SRCBLEND, BLEND_ZERO);
            dev.SetRenderState(RS_DESTBLEND, BLEND_SRCCOLOR);
            dev.SetRenderState(RS_ZFUNC, CMP_EQUAL);
            dev.SetTexture(0, lightmap);
            dev.SetTextureStageState(0, TSS_TEXCOORDINDEX, 1);
            dev.SetTextureStageState(0, TSS_COLOROP, TOP_ADD);
            dev.DrawIndexedPrimitiveShared(TriangleList, kFvfTerrain, groundBuffers[c.quadrant], 0, groundVertices,
                                           c.indices.data(), uint32_t(c.indices.size()));
            draws += 2;
        }
        dev.SetTextureStageState(0, TSS_TEXCOORDINDEX, 0);
        dev.SetRenderState(RS_ZFUNC, CMP_LESSEQUAL);
        dev.SetRenderState(RS_LIGHTING, 1);
        dev.SetRenderState(RS_AMBIENT, 0xFF616161);
        dev.SetRenderState(RS_DIFFUSEMATERIALSOURCE, MCS_MATERIAL);
        dev.SetTextureStageState(0, TSS_COLOROP, TOP_MODULATE);
        const Material plain{{1, 1, 1, 1}, {1, 1, 1, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}, 0.0f};
        const Material glowing{{1, 1, 1, 1}, {1, 1, 1, 1}, {0, 0, 0, 0}, {1, 1, 1, 1}, 0.0f};
        auto drawObject = [&](const Object& o) {
            dev.SetDrawVisual(uint32_t(VisualKind::Static), "RTriMesh_t", 0, o.visual);
            dev.SetTransform(World, Mul(RotateY(o.yaw), Translate(o.x, 0.0f, o.z)));
            for (uint32_t p = 0; p < o.pieces; ++p) {
                // As the game's pieces: their state set again for each (the hand-off drops repeats).
                dev.SetMaterial(o.piece[p].emissive ? glowing : plain);
                dev.SetTexture(0, o.piece[p].tex);
                switch (o.kind) {
                case Object::Solid:
                    dev.SetRenderState(RS_ALPHABLENDENABLE, 0);
                    dev.SetRenderState(RS_ZWRITEENABLE, 1);
                    break;
                case Object::Blended:
                case Object::Holes:
                    dev.SetRenderState(RS_ALPHABLENDENABLE, 1);
                    dev.SetRenderState(RS_SRCBLEND, BLEND_SRCALPHA);
                    dev.SetRenderState(RS_DESTBLEND, BLEND_INVSRCALPHA);
                    dev.SetRenderState(RS_ZWRITEENABLE, 1);
                    break;
                case Object::Additive:
                    dev.SetRenderState(RS_ALPHABLENDENABLE, 1);
                    dev.SetRenderState(RS_SRCBLEND, BLEND_SRCALPHA);
                    dev.SetRenderState(RS_DESTBLEND, BLEND_ONE);
                    dev.SetRenderState(RS_ZWRITEENABLE, 0);
                    break;
                }
                dev.DrawMesh(o.piece[p].mesh);
                ++draws;
            }
        };
        for (size_t i = 0; i < objects.size(); ++i) {
            if (!inView(objects[i])) continue;
            if (int(i) >= g_cityMovers) { drawObject(objects[i]); continue; }
            Object o = objects[i];
            o.x += 1.5f * std::sin(float(frame) * 0.05f + float(i));
            drawObject(o);
        }
        for (const Object& o : panes) if (inView(o)) drawObject(o);
        dev.SetRenderState(RS_ZWRITEENABLE, 1);
        dev.SetRenderState(RS_ALPHABLENDENABLE, 0);
        dev.SetDrawVisual(0, nullptr);
        QueryPerformanceCounter(&r1);
        dev.EndFrame();
        QueryPerformanceCounter(&t1);
        if (frame >= g_cityWarm) {
            frameSum += double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(freq.QuadPart);
            recordSum += double(r1.QuadPart - r0.QuadPart) * 1000.0 / double(freq.QuadPart);
            ++counted;
        }
        t0 = t1;
        drawsPerFrame = draws;
    }
    dev.ProfileWindow(false, "city bench: ");
    dev.Sync();
    for (uint32_t id = 1; id < nextId; ++id) dev.ReleaseMesh(id);
    for (Texture* t : opaque) dev.DestroyTexture(t);
    for (Texture* t : holes) dev.DestroyTexture(t);
    for (Texture* t : glow) dev.DestroyTexture(t);
    dev.DestroyTexture(groundTex);
    dev.DestroyTexture(lightmap);
    dev.Sync();
    std::printf("city bench: %u draws a frame; calling thread: frame %.3f ms (%.1f fps), recording %.3f ms (avg of %d frames)\n",
                drawsPerFrame, frameSum / std::max(counted, 1), 1000.0 * std::max(counted, 1) / std::max(frameSum, 1e-9),
                recordSum / std::max(counted, 1), counted);
}

// Water test scenes (--water-lake, --water-coast): terrain drawn like AnarchyGround_t (base pass, then the light pass
// that gives the water its sun and ambient), a few lit props standing in and over the water, the game's fog, and the
// water handed over as the proxy does with the game's VisualLiquid_t mesh - a few huge flat triangles (DrawWater).
// The lake: a bowl of ground 200 units across under one big square of water; the coast: a beach sloping into a sea
// that reaches far out (the open-water swell).
int g_waterScene = 0;             // 1 = --water-lake, 2 = --water-coast
float g_waterStyle = 0.5f;        // --water-style S (0 = enhanced AO, 1 = realistic)
float g_waterYaw = 0.0f;          // --water-yaw R: the camera turned R radians from the scene's default
float g_waterTurn = 0.0f;         // --water-turn R: ... and R more each frame (turning)
float g_waterPitch = -10.0f;      // --water-pitch R: looking R radians down (negative: up; default per scene)
float g_waterHeight = -1.0f;      // --water-height H: the eye this high above the water (default per scene)
float g_waterWaves = 1.0f;        // --water-waves W
int g_waterQuality = 2;           // --water-quality 1..3
bool g_waterOff = false;          // --water-off: the game's way instead (no DrawWater: a flat blended quad)
bool g_waterNight = false;        // --water-night
// --game-day (the water scenes): the game's own light by the sun's height, as its dusk log has it - the hour's lightmap
// from the day's ~0.72 grey down to the night's dark blue 303040 between ~6 degrees up and ~3 under, its ambient from
// 0.37 to 0.29, its directional light white at every height (pointing up once the sun is under: the game's night
// light). Without it the sweep kept the day's lightmap and ambient and only dimmed the sun: no real night to measure.
bool g_gameDay = false;
// --rig-props: a static (RTriMesh_t, lit, textured with the ground's own texture) on dry ground near the camera, and
// rig-props.txt beside the shots with the screen rects of its front face, its top and a patch of ground beside it:
// the same albedo, so their brightness ratio is the lighting's (tools/sun-sweep.py: statics vs ground).
bool g_rigProps = false;
// --indoor: a room's playfield - no directional light at all (the renderer sees no sun), a dim ambient, two lamps.
bool g_indoor = false;
float g_waterZ = -1e6f;           // --water-z Z: the camera's z (default per scene)
bool g_waterNoPrepass = false;    // --water-noprepass
bool g_hudSprite = false;         // --hud-sprite: after the 3D, a screen sprite as the game's timer bars draw it (the
                                  // pre-transformed 0x1C4 format, not out of ProcessVertices): interface, so its colour
                                  // must come out exactly (0x30C050 at the top left), untouched by the HDR passes
                                  // (with --grade 0.3 1.3 0.5 0 0.8 --dof 1 a scene draw would come out grey and blurred);
                                  // and an untextured MODULATE(TEXTURE, DIFFUSE) bar below it: 0x8040C0, not black
bool g_waterPool = false;         // --water-pool: the lake's water only a 6 x 6 pool in front (Jobe's harbour: no water
                                  // may spread beyond it - the rest of the bowl stays dry)
bool g_smokeWall = false;         // --smoke-wall: an opaque wall right ahead of the lake's camera (hidden smoke)
bool g_waterWall = false;         // --water-wall: a steep sheet of the water mesh standing in the lake (a fall)
bool g_waterShelf = false;        // --water-shelf: the mesh terraced - the surface with a lower shore shelf in front
                                  // (as Anarchy Online hangs its aprons), which used to render as a "mountain of water"
std::string g_meshIds;             // --mesh-ids FILE: ao-assets' mesh-ids.bin (the mesh ids' table)
std::vector<std::string> g_meshPieces;   // --mesh-piece FILE: a piece dumped from rdb.db (fvf, count, stride, texture
                                  //  offset, index count; vertices; indices), drawn as a static object (a mesh id test)
bool g_clouds = false;            // --clouds: cloud puffs in the sky, as the game's (VisualSingleCloud_t)
bool g_cloudsGame = false;
float g_cloudCover = 0.0f;        // --cloud-cover X: RVK_CloudCover
float g_cloudThick = 1.0f;        // --cloud-thick X: RVK_CloudThick
float g_cloudShade = 1.6f;        // --cloud-shade X: RVK_CloudShade
float g_cloudHeight = 900.0f;     // --cloud-height X: RVK_CloudHeight
float g_cloudLow = 0.0f;          // --cloud-low X: RVK_CloudLow (the renderer's default: none)
int g_cloudQual = 1;              // --cloud-qual N: RVK_CloudQual (0 low .. 3 ultra)
float g_cloudSheet = 1.0f;        // --cloud-sheet X: RVK_CloudSheet
float g_cloudShadow = 0.6f;       // --cloud-shadow X: RVK_CloudShadow
float g_cloudNight = 0.8f;        // --cloud-night X: RVK_CloudNight
float g_skyVary = 0.0f;           // --sky-vary X: RVK_SkyVary (off: the demo's skies as they were)
uint32_t g_skyZone = 0;      // --sky-zone PF: the sky leans by that playfield's climate (sky.cpp)
double g_skyTime = -1.0;          // --sky-time T: the sky weather's clock (seconds)
float g_moonLight = 1.0f;         // --moon-light X: RVK_MoonLight
float g_darkness = 1.0f;          // --darkness X (or --night-dark X): RVK_Darkness (the demo's default: the old nights)
float g_darkFloor = 1.0f;         // --dark-floor X: RVK_DarkFloor
bool g_atmos = true;              // --atmos 0|1: RVK_Atmos
float g_twilight = 0.85f, g_skyGlow = 1.0f;   // --twilight T G: RVK_Twilight, RVK_SkyGlow
float g_duskRays = 1.0f;                      // --dusk-rays X: RVK_DuskRays
// --sky-dome RRGGBB (the water scenes): a dome like the game's - the sky's first draw, a lit unblended mesh round the
// eye - in that colour, which the fog takes too (the zone's sky: 4FE6D9 the user's teal); RVK_ProcSky replaces it.
uint32_t g_skyDome = 0;
bool g_procSky = true;                        // --proc-sky 0|1: RVK_ProcSky
float g_skyZoneHint = 0.25f;                  // --sky-zone-hint X: RVK_SkyZone
rvk::SkyColours g_skyColours;                 // --sky-key I RRGGBB: the sky's colour key I (rvk::SkyKey)
float g_rainbowForce = -1.0f;                 // --rainbow X: a rainbow of that strength now (RVK_Rainbow)
bool g_profileWindow = false;                 // --profile-window: the water scenes' second half's GPU passes timed
rvk::NightSky g_nightSky;                     // --stars 0|1, --star-bright X, --galaxy X, --meteors X, --aurora X
                                              // (forced now: its strength), --twinkle X
float g_dawnLight = 1.0f;         // --dawn-light X: RVK_DawnLight
int g_textureFlood = 0;                         // --texture-flood: textures alive before the shadow test's label
float g_mist = 1.0f;              // --mist X: RVK_Mist
bool g_cloudLayer = true;         // --cloud-puffs: RVK_CloudLayer off (a volume per puff)
bool g_cloudsHalf = true;         // --clouds-full: RVK_CloudHalf off        // --clouds-game: ... drawn as the game draws them (RVK_CloudsOn off)
bool g_fixedClock = false;        // --fixed-clock MS: the renderer's clock steps MS a frame (no sleeping: benchmarks)
bool g_waterWalker = false;       // --water-walker: a "character" standing in the lake with its feet under the surface,
                                  // bobbing along: the surface ripples around it (RVK_WaterSwim)
bool g_waterJump = false;         // --water-jump: the walker jumps out of the water and lands (RVK_WakeSplash)
float g_waterSwim = 1.0f;         // --water-swim S: strength of those ripples (RVK_WaterSwim)
bool g_waterGrid = false;        // --water-grid: the older water surface (a grid over the water map; RVK_WaterMesh off)
std::string g_waterMesh;          // --water-mesh FILE: the water mesh from a dump of the game's (count, index count, the
                                  // vertices as Device::WaterVertex, the 16-bit indices), the part within 150 units of
                                  // --water-mesh-at X Y Z moved to the lake's far side (that point to 0 0 60)
float g_waterMeshAt[3] = {2040.0f, 29.5f, 2340.0f};
float g_waterEye[3] = {0, 0, 0};  // --water-eye X Y Z (the dump's coordinates): the camera there, looking at
bool g_waterEyeSet = false;       // --water-mesh-at
bool g_smoke = false;             // --smoke: a smoke source on the ground in front of the camera (the new smoke on)
bool g_smokeWalker = false;       // --smoke-walker: a "character" walking to and fro through the smoke
bool g_smokeGame = false;         // --smoke-game: the source as the game's barrel smoke hands it over (small, no push
                                  // of its own, 6 s, the game's wind)
float g_smokeDensity = 1.0f;      // --smoke-density X (RVK_SmokeThick)
float g_smokeWindScale = 1.0f;    // --smoke-wind X: the game's wind x X
float g_smokeWisps = 1.0f;        // --smoke-wisps X (RVK_SmokeWisps)
bool g_smokeHalf = true;          // --smoke-full: RVK_SmokeHalf off
float g_smokeWobble = 0.0f;       // --smoke-wobble A: the source jumps about by up to A each frame (the game's moves with
                                  //  its newest sprites)
bool g_smokeTwo = false;          // --smoke-two: a second, bigger source
int g_smokeMany = 0;              // --smoke-many N: N more sources round the scene (the simulation's budget under load)
float g_smokeBudget = -1.0f;      // --smoke-budget MS: RVK_SmokeBudget (default: the setting's)
float g_smokeRes = 0.0f;          // --smoke-res R: RVK_SmokeRes
uint32_t g_smokeQual = 0;         // --smoke-qual Q: RVK_SmokeQual
float g_smokeOpaque = 1.0f;       // --smoke-opaque X (RVK_SmokeOpaque)
float g_rainForce = 0.0f;         // --rain F: rain this hard (0..1; RVK_RainForce), the new weather on
bool g_waterLamp = false;         // --water-lamp: a warm point light by the camera, as a character's lamp (with
                                  // --light-override, so the frame's lights carry it)

// --clouds: the puffs' textures (two shapes) and their sheet: 10 x 10 bulging towards the viewer (+z here, turned to
// face the camera), 2 x 1 across.
template <typename D>
void MakeDemoPuffs(D& dev, Texture** puffTex, std::vector<VtxMesh>& puffMesh, std::vector<uint16_t>& puffIdx)
{
    for (int k = 0; k < 2; ++k) {
        auto px = CloudPuff(256, 7u + uint32_t(k) * 31u);
        puffTex[k] = dev.CreateTexture(256, 256, px.data());
    }
    for (int j = 0; j < 10; ++j)
        for (int i = 0; i < 10; ++i) {
            const float u = i / 9.0f, v = j / 9.0f, x = u * 2 - 1, y = v * 2 - 1;
            puffMesh.push_back({x, y * 0.5f, -0.25f * (1 - x * x) * (1 - y * y), 0, 0, -1, 0xFFFFFFFF, u, 1 - v});
        }
    for (int j = 0; j < 9; ++j)
        for (int i = 0; i < 9; ++i) {
            const uint16_t a = uint16_t(j * 10 + i);
            puffIdx.insert(puffIdx.end(), {a, uint16_t(a + 1), uint16_t(a + 11), a, uint16_t(a + 11), uint16_t(a + 10)});
        }
}

// --clouds: the sky's puffs (as the game: drawn first, the sky, then the scene), each a sheet turned to the camera,
// around `yaw` (the view's direction); `scale` shrinks the sky for small scenes.
template <typename D>
void DrawDemoPuffs(D& dev, Vector eye, float yaw, Texture* const* puffTex, const std::vector<VtxMesh>& puffMesh,
                   const std::vector<uint16_t>& puffIdx, float scale)
{
    // (The game's sun sprites, as stand-ins: four round fans of 32 triangles round a hub, two a sun - one the light's
    // way, one 14 degrees to its side; where the renderer draws its suns, sun.cpp.)
    dev.SetDrawVisual(uint32_t(VisualKind::Sky), "VisualSunRays_t");
    dev.SetTransform(World, Translate(eye.x, eye.y, eye.z));
    {
        auto Normalize = [](Vector v) {
            const float n = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
            return Vector{v.x / n, v.y / n, v.z / n};
        };
        const float* l = g_sunDirSet ? g_sunDir : nullptr;
        const Vector to = Normalize(l ? Vector{-l[0], -l[1], -l[2]} : Vector{-0.35f, 0.55f, -0.75f});
        const Vector side = Normalize(Vector{-to.z, 0.0f, to.x});
        const Vector up = Vector{side.y * to.z - side.z * to.y, side.z * to.x - side.x * to.z, side.x * to.y - side.y * to.x};
        const float c14 = std::cos(0.244f), s14 = std::sin(0.244f);
        const Vector to2 = Normalize(Vector{to.x * c14 + side.x * s14, to.y * c14 + side.y * s14, to.z * c14 + side.z * s14});
        struct Fan { Vector dir; float size; uint32_t colour; };
        // (as the game's, logged 2026-10-13: an orange and a blue sprite a sun, of very different sizes)
        const Fan fans[] = {{to, 180.0f, 0xFF639CFFu}, {to, 540.0f, 0xFFFF9C38u}, {to2, 90.0f, 0xFFFF9C38u},
                            {to2, 270.0f, 0xFF639CFFu}};
        std::vector<VtxDiffuseTex> fan(33);
        std::vector<uint16_t> fanIdx;
        for (uint16_t i = 0; i < 32; ++i) fanIdx.insert(fanIdx.end(), {0, uint16_t(1 + i), uint16_t(1 + (i + 1) % 32)});
        int f = 0;
        for (const Fan& fn : fans) {
            const Vector c{fn.dir.x * 1000.0f, fn.dir.y * 1000.0f, fn.dir.z * 1000.0f};
            fan[0] = VtxDiffuseTex{c.x, c.y, c.z, fn.colour, 0.5f, 0.5f};
            for (int i = 0; i < 32; ++i) {
                const float a = float(i) / 32.0f * 2.0f * kPi;
                const float r = fn.size;         // (round: the rays are in the texture)
                const float x = std::cos(a) * r, y = std::sin(a) * r;
                fan[1 + i] = VtxDiffuseTex{c.x + side.x * x + up.x * y, c.y + side.y * x + up.y * y,
                                           c.z + side.z * x + up.z * y, fn.colour, 0.5f + 0.5f * std::cos(a),
                                           0.5f + 0.5f * std::sin(a)};
            }
            dev.DrawIndexedPrimitive(TriangleList, kFvfDiffuseTex, fan.data(), 33, fanIdx.data(), uint32_t(fanIdx.size()));
            ++f;
        }
    }
    dev.SetDrawVisual(uint32_t(VisualKind::Sky), "VisualSingleCloud_t");
    dev.SetRenderState(RS_LIGHTING, 1);
    dev.SetRenderState(RS_ALPHABLENDENABLE, 1);
    dev.SetRenderState(RS_SRCBLEND, BLEND_SRCALPHA);
    dev.SetRenderState(RS_DESTBLEND, BLEND_INVSRCALPHA);
    dev.SetRenderState(RS_ZWRITEENABLE, 0);
    dev.SetTextureStageState(0, TSS_COLOROP, TOP_MODULATE);
    dev.SetTextureStageState(0, TSS_ALPHAOP, TOP_MODULATE);
    dev.SetTextureStageState(0, TSS_ALPHAARG2, TA_DIFFUSE);
    Material cloudMat{{1, 1, 1, 1}, {1, 1, 1, 1}, {0, 0, 0, 0}, {0.72f, 0.72f, 0.75f, 1}, 0.0f};
    dev.SetMaterial(cloudMat);
    struct Place { float yaw, dist, height, width; };
    const Place places[] = {{-0.55f, 520, 150, 150}, {-0.2f, 700, 210, 220}, {0.12f, 450, 120, 120},
                            {0.45f, 620, 180, 190}, {0.75f, 400, 140, 110}, {-0.85f, 380, 110, 100},
                            {0.3f, 900, 260, 260}, {-0.4f, 950, 300, 240}, {0.05f, 90, 200, 160},
                            {-0.3f, 160, 230, 150}};
    int k = 0;
    for (const Place& pl : places) {
        const float a = yaw + pl.yaw;
        const float px = eye.x + std::cos(a) * pl.dist * scale, pz = eye.z + std::sin(a) * pl.dist * scale;
        const float face = std::atan2(eye.x - px, eye.z - pz);   // its -z towards the camera
        const float w = pl.width * scale;
        Matrix m = Mul(Mul(Matrix{{{w, 0, 0, 0}, {0, w, 0, 0}, {0, 0, w, 0}, {0, 0, 0, 1}}}, RotateY(face + kPi)),
                       Translate(px, eye.y + pl.height * scale, pz));
        dev.SetTransform(World, m);
        dev.SetTexture(0, puffTex[k++ & 1]);
        dev.DrawIndexedPrimitive(TriangleList, kFvfMesh, puffMesh.data(), uint32_t(puffMesh.size()), puffIdx.data(),
                                 uint32_t(puffIdx.size()));
    }
    dev.SetTransform(World, Identity());
    dev.SetRenderState(RS_ZWRITEENABLE, 1);
    dev.SetTextureStageState(0, TSS_ALPHAOP, TOP_SELECTARG1);
    dev.SetDrawVisual(0, nullptr);
}

template <typename D>
void RunWaterScene(D& dev, int frames, const std::string& shot)
{
    const bool coast = g_waterScene == 2;
    // Ground: sand to grass by a pattern (one texture, as the game's terrain tile would be).
    std::vector<uint32_t> groundPixels(64 * 64);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            uint32_t n = uint32_t((x * 7 + y * 13) % 17);
            groundPixels[y * 64 + x] = coast ? (0xFFC2B08A - n * 0x020202u) : (0xFF6E7A4A - n * 0x020202u);
        }
    Texture* ground = dev.CreateTexture(64, 64, groundPixels.data());
    std::vector<uint32_t> lightmapPixels(64 * 64, g_waterNight ? 0xFF303040u : 0xFFB8B8B8u);
    Texture* lightmap = dev.CreateTexture(64, 64, lightmapPixels.data());
    // --game-day: the hour's lightmap by the sun's height (gameDayW: 0 night .. 1 day), 16 steps of it.
    auto gameDayW = [](float hs) {
        const float t = std::clamp((hs + 0.05f) / 0.15f, 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    };
    Texture* dayLightmaps[16] = {};
    if (g_gameDay)
        for (int k = 0; k < 16; ++k) {
            const float w = float(k) / 15.0f;
            const uint32_t rg = uint32_t(0x30 + (0xB8 - 0x30) * w + 0.5f), b = uint32_t(0x40 + (0xB8 - 0x40) * w + 0.5f);
            std::vector<uint32_t> px(64 * 64, 0xFF000000u | rg << 16 | rg << 8 | b);
            dayLightmaps[k] = dev.CreateTexture(64, 64, px.data());
        }
    // The game's water texture stand-in (water5.png: a pale blue mottled pattern).
    std::vector<uint32_t> waterPixels(64 * 64);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            float v = 0.5f + 0.25f * std::sin(x * 0.39f + std::sin(y * 0.2f) * 2.0f) + 0.25f * std::cos(y * 0.33f);
            uint32_t b = uint32_t(140 + 80 * v), g = uint32_t(110 + 70 * v), r = uint32_t(70 + 40 * v);
            waterPixels[y * 64 + x] = 0xFF000000u | r << 16 | g << 8 | b;
        }
    Texture* waterTex = dev.CreateTexture(64, 64, waterPixels.data());
    Texture* puffTex[2] = {nullptr, nullptr};
    std::vector<VtxMesh> puffMesh;
    std::vector<uint16_t> puffIdx;
    if (g_clouds)
        MakeDemoPuffs(dev, puffTex, puffMesh, puffIdx);
    struct VtxTerrain { float x, y, z, nx, ny, nz, u0, v0, u1, v1; };
    const uint32_t kFvfTerrain = FVF_XYZ | FVF_NORMAL | (2 << 8);
    auto height = [&](float x, float z) {
        if (coast) {
            // Land at z < 20, the beach down into the sea, a sea floor at -14 from z ~ 140; a few bumps.
            float h = 5.0f - 0.16f * z + 0.6f * std::sin(x * 0.05f) * std::cos(z * 0.07f);
            return std::max(h, -14.0f + 0.8f * std::sin(x * 0.03f));
        }
        float r = std::sqrt(x * x + (z - 40.0f) * (z - 40.0f) * 1.3f);
        float bowl = -5.0f + 9.0f * std::pow(std::min(r / 75.0f, 1.4f), 2.0f);
        return bowl + 0.8f * std::sin(x * 0.11f) * std::cos(z * 0.09f);
    };
    const float cell = coast ? 5.0f : 2.5f;
    const float x0 = coast ? -250.0f : -110.0f, x1 = -x0;
    const float z0 = coast ? -80.0f : -70.0f, z1 = coast ? 320.0f : 150.0f;
    std::vector<VtxTerrain> terrain;
    for (float z = z0; z < z1; z += cell)
        for (float x = x0; x < x1; x += cell) {
            auto vert = [&](float vx, float vz) {
                float e = 0.5f;
                float nx = height(vx - e, vz) - height(vx + e, vz), nz = height(vx, vz - e) - height(vx, vz + e);
                float l = std::sqrt(nx * nx + 1.0f + nz * nz);
                return VtxTerrain{vx, height(vx, vz), vz, nx / l, 1.0f / l, nz / l, vx / 6, vz / 6, (vx - x0) / (x1 - x0),
                                  (vz - z0) / (z1 - z0)};
            };
            VtxTerrain q[6] = {vert(x, z), vert(x, z + cell), vert(x + cell, z), vert(x + cell, z), vert(x, z + cell),
                               vert(x + cell, z + cell)};
            terrain.insert(terrain.end(), q, q + 6);
        }
    // Props: lit boxes (a pillar and rocks standing in the water, a pier over it).
    std::vector<VtxMesh> props;
    auto box = [&](float cx, float cy, float cz, float sx, float sy, float sz, uint32_t colour) {
        const float c[3] = {cx, cy, cz}, e[3] = {sx * 0.5f, sy * 0.5f, sz * 0.5f};
        for (int axis = 0; axis < 3; ++axis)
            for (int sign = -1; sign <= 1; sign += 2) {
                int a = (axis + 1) % 3, b = (axis + 2) % 3;
                float n[3] = {0, 0, 0};
                n[axis] = float(sign);
                auto corner = [&](float ua, float ub) {
                    float p[3];
                    p[axis] = c[axis] + sign * e[axis];
                    p[a] = c[a] + ua * e[a];
                    p[b] = c[b] + ub * e[b];
                    return VtxMesh{p[0], p[1], p[2], n[0], n[1], n[2], colour, 0, 0};
                };
                VtxMesh q[6] = {corner(-1, -1), corner(1, -1), corner(1, 1), corner(-1, -1), corner(1, 1), corner(-1, 1)};
                props.insert(props.end(), q, q + 6);
            }
    };
    if (coast) {
        for (int i = 0; i < 9; ++i)
            box(-12.0f + float(i) * 0.1f, 1.5f, 30.0f + float(i) * 6.0f, 0.5f, 7.0f, 0.5f, 0xFF6A5038);   // pier posts
        box(-11.6f, 2.6f, 54.0f, 3.0f, 0.3f, 52.0f, 0xFF8A6A48);                                      // the deck
        box(18.0f, -1.0f, 60.0f, 6.0f, 6.0f, 5.0f, 0xFF707070);                                        // a rock
        box(40.0f, -4.0f, 110.0f, 10.0f, 12.0f, 8.0f, 0xFF6A6A70);
    } else {
        box(8.0f, 0.5f, 28.0f, 1.2f, 9.0f, 1.2f, 0xFFB0A898);                                          // a pillar
        box(-15.0f, -0.5f, 45.0f, 5.0f, 3.0f, 4.0f, 0xFF707068);                                       // a rock
        if (g_waterWall)
            box(0.0f, 4.0f, 71.0f, 70.0f, 12.0f, 2.0f, 0xFF7A7468);                                    // a cliff behind
        box(25.0f, -2.0f, 70.0f, 8.0f, 6.0f, 3.0f, 0xFF606060);
        box(-6.0f, 1.2f, 15.0f, 1.0f, 1.0f, 1.0f, 0xFFC04030);                                         // a red crate
    }
    // --rig-props: a static box on the dry slope 11 units ahead, a little left (its shadow falls to +x, away from the
    // ground patch measured on its left), textured with the ground's texture at the same scale (world units / 6).
    std::vector<VtxMesh> rigBox;
    const float rigC[3] = {-4.0f, height(-4.0f, -14.0f) + 1.25f, -14.0f}, rigE[3] = {1.5f, 1.25f, 0.75f};
    if (g_rigProps)
        for (int axis = 0; axis < 3; ++axis)
            for (int sign = -1; sign <= 1; sign += 2) {
                const int a = (axis + 1) % 3, b = (axis + 2) % 3;
                float n[3] = {0, 0, 0};
                n[axis] = float(sign);
                auto corner = [&](float ua, float ub) {
                    float p[3];
                    p[axis] = rigC[axis] + float(sign) * rigE[axis];
                    p[a] = rigC[a] + ua * rigE[a];
                    p[b] = rigC[b] + ub * rigE[b];
                    // (u, v across the face: x / z on the top, x or z with y on the sides)
                    const float u = axis == 0 ? p[2] : p[0], v = axis == 1 ? p[2] : p[1];
                    return VtxMesh{p[0], p[1], p[2], n[0], n[1], n[2], 0xFFFFFFFFu, u / 6.0f, v / 6.0f};
                };
                VtxMesh q[6] = {corner(-1, -1), corner(1, -1), corner(1, 1), corner(-1, -1), corner(1, 1), corner(-1, 1)};
                rigBox.insert(rigBox.end(), q, q + 6);
            }
    // The water, as the game has it: big flat triangles (the lake one square; the sea out to the horizon).
    const uint32_t tint = 0xCC00186A;
    std::vector<Device::WaterVertex> water;
    if (coast)
        water = {{{-3000, 0, 5}, tint}, {{3000, 0, 5}, tint}, {{3000, 0, 4000}, tint}, {{-3000, 0, 4000}, tint}};
    else
        water = {{{-90, 0, -40}, tint}, {{90, 0, -40}, tint}, {{90, 0, 130}, tint}, {{-90, 0, 130}, tint}};
    if (g_waterPool)
        water = {{{-3, 0, 17}, tint}, {{3, 0, 17}, tint}, {{3, 0, 23}, tint}, {{-3, 0, 23}, tint}};
    std::vector<uint16_t> waterIdx;
    if (g_waterShelf) {
        // The surface with a lower shelf in front, sharing no edge with it (z 55): the mesh steps down 4 units, as
        // Anarchy Online's shore aprons do. The lower shelf is deliberately the larger part, so the body's level must
        // come from the top (its surface), not from where most of the area lies.
        water = {{{-90, -4, -40}, tint}, {{90, -4, -40}, tint}, {{90, -4, 55}, tint}, {{-90, -4, 55}, tint},
                 {{-90, 0, 55}, tint},  {{90, 0, 55}, tint},  {{90, 0, 130}, tint},  {{-90, 0, 130}, tint}};
        waterIdx = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};
    } else {
        waterIdx = {0, 1, 2, 0, 2, 3};
    }
    if (!g_waterMesh.empty()) {
        water.clear();
        waterIdx.clear();
        if (FILE* f = std::fopen(g_waterMesh.c_str(), "rb")) {
            uint32_t n = 0, ic = 0;
            std::vector<Device::WaterVertex> v;
            std::vector<uint16_t> idx;
            if (std::fread(&n, 4, 1, f) == 1 && std::fread(&ic, 4, 1, f) == 1) {
                v.resize(n);
                idx.resize(ic);
                if (std::fread(v.data(), sizeof(Device::WaterVertex), n, f) != n || std::fread(idx.data(), 2, ic, f) != ic)
                    v.clear(), idx.clear();
            }
            std::fclose(f);
            const float* c = g_waterMeshAt;
            std::vector<int32_t> remap(v.size(), -1);
            for (size_t t = 0; t + 2 < idx.size(); t += 3) {
                bool close = true;
                for (int k = 0; k < 3; ++k) {
                    const float* p = v[idx[t + k]].pos;
                    close = close && std::hypot(p[0] - c[0], p[2] - c[2]) < 150.0f;
                }
                if (!close)
                    continue;
                for (int k = 0; k < 3; ++k) {
                    int32_t& r = remap[idx[t + k]];
                    if (r < 0) {
                        r = int32_t(water.size());
                        Device::WaterVertex w = v[idx[t + k]];
                        w.pos[0] -= c[0]; w.pos[1] -= c[1]; w.pos[2] += 60.0f - c[2];
                        water.push_back(w);
                    }
                    waterIdx.push_back(uint16_t(r));
                }
            }
            std::printf("water mesh %s: %zu of %u triangles, %zu vertices\n", g_waterMesh.c_str(), waterIdx.size() / 3,
                        ic / 3, water.size());
        } else {
            std::printf("water mesh %s: can't open\n", g_waterMesh.c_str());
        }
    }
    if (g_waterWall) {                    // upright, 8 units tall, with the rock behind it 10 units further back
        const uint16_t base = uint16_t(water.size());
        water.insert(water.end(), {{{-30, 0, 60}, tint}, {{30, 0, 60}, tint}, {{30, 8, 60}, tint}, {{-30, 8, 60}, tint}});
        waterIdx.insert(waterIdx.end(), {base, uint16_t(base + 1), uint16_t(base + 2), base, uint16_t(base + 2),
                                         uint16_t(base + 3)});
    }

    Device::PbrSettings pbr;                 // (the environment probe the water reflects: --pbr-probe)
    pbr.probe = g_pbrProbe;
    dev.SetPbr(pbr);
    Device::WaterParams wp;
    wp.style = g_waterStyle;
    wp.waves = g_waterWaves;
    wp.swimRipples = g_waterSwim;
    wp.quality = uint32_t(g_waterQuality);
    wp.mesh = !g_waterGrid;
    dev.SetWaterParams(wp);
    if (g_waterNoPrepass)
        dev.SetKnob(rvk::Knob::Prepass, false);
    const uint32_t sky = g_waterNight ? 0xFF101828u : 0xFF8FB0CCu;
    if (g_sweepSteps > 0)
        frames = g_sweepSteps * g_sweepHold;
    for (int frame = 0; frame < frames; ++frame) {
        // --profile-window: the GPU's passes timed over the second half of the run ("window: gpu ms ...").
        if (g_profileWindow && frame == frames / 2) dev.ProfileWindow(true, "");
        if (g_profileWindow && frame == frames - 1) dev.ProfileWindow(false, "window: ");
        float sweepDir[3] = {}, sweepDim = 1.0f;
        uint32_t skyNow = sky;
        if (g_sweepSteps > 0) {
            const int step = frame / g_sweepHold;
            const float t = g_sweepSteps > 1 ? float(frame) / float(frames - 1) : 0.0f;
            const float e = (g_sweepFrom + (g_sweepTo - g_sweepFrom) * t) * kPi / 180.0f;
            float az[2] = {0.35f, 0.75f};
            if (g_sunDirSet) { az[0] = g_sunDir[0]; az[1] = g_sunDir[2]; }
            const float al = std::max(std::sqrt(az[0] * az[0] + az[1] * az[1]), 1e-4f);
            sweepDir[0] = std::cos(e) * az[0] / al;
            sweepDir[1] = -std::sin(e);
            sweepDir[2] = std::cos(e) * az[1] / al;
            // (A stand-in for the game's own dusk: its sky and fog darkening from ~6 degrees up to ~15 under, its
            // light going out at the horizon. The game's real curve: the dusk log.)
            const float hs = std::sin(e), k = std::clamp((hs + 0.26f) / 0.36f, 0.0f, 1.0f);
            const float sky01 = 0.12f + 0.88f * k * k * (3.0f - 2.0f * k);
            sweepDim = std::clamp(hs / 0.05f, 0.0f, 1.0f);
            const uint32_t base = g_skyDome ? g_skyDome : 0xFF8FB0CCu;
            const uint32_t r = uint32_t(float((base >> 16) & 0xFF) * sky01), g = uint32_t(float((base >> 8) & 0xFF) * sky01),
                           b = uint32_t(float(base & 0xFF) * std::sqrt(sky01));
            skyNow = 0xFF000000u | r << 16 | g << 8 | b;
            if (frame % g_sweepHold == g_sweepHold - 1) {
                char name[32];
                std::snprintf(name, sizeof(name), "sweep_%03d.bmp", step);
                dev.RequestScreenshot(name);
            }
        } else if (frame == frames - 1)
            dev.RequestScreenshot(shot);
        dev.BeginFrame();
        dev.SetViewport({0, 0, g_width, g_height, 0.0f, 1.0f});
        dev.Clear(CLEAR_TARGET | CLEAR_ZBUFFER, skyNow, 1.0f);
        // Standing on the shore (the eye 1.8 above the ground there, or --water-height above the water), looking out.
        const float ex = coast ? 2.0f : 0.0f, ez = g_waterZ > -1e5f ? g_waterZ : coast ? 22.0f : -25.0f;
        const float eyeY = g_waterHeight >= 0.0f ? g_waterHeight : std::max(height(ex, ez), 0.0f) + 1.8f;
        const float pitch = g_waterPitch > -5.0f ? g_waterPitch : (coast ? 0.1f : 0.16f);
        const float yaw = kPi * 0.5f + g_waterYaw + g_waterTurn * float(frame);   // +z (--water-turn: turning)
        Vector eye{ex, eyeY, ez};
        if (g_waterEyeSet)
            eye = {g_waterEye[0] - g_waterMeshAt[0], g_waterEye[1] - g_waterMeshAt[1], g_waterEye[2] - g_waterMeshAt[2] + 60.0f};
        if (frame == 0)
            std::printf("water scene: eye %.2f %.2f %.2f, ground there %.2f, at z 60: %.2f, at z 150: %.2f, %zu terrain vertices\n",
                        double(ex), double(eyeY), double(ez), double(height(ex, ez)), double(height(0, 60)),
                        double(height(0, 150)), terrain.size());
        Vector at{eye.x + std::cos(yaw) * std::cos(pitch), eye.y - std::sin(pitch), eye.z + std::sin(yaw) * std::cos(pitch)};
        if (g_waterEyeSet)
            at = {0.0f, 9.0f, 60.0f};                       // (the fall's middle, about)

        dev.SetTransform(View, LookAtLH(eye, at, {0, 1, 0}));
        dev.SetTransform(Projection, PerspectiveLH(kPi / 3, float(g_width) / g_height, 0.3f, 2000.0f));
        dev.SetTransform(World, Identity());
        dev.SetRenderState(RS_ZENABLE, 1);
        dev.SetRenderState(RS_ZWRITEENABLE, 1);
        dev.SetRenderState(RS_ZFUNC, CMP_LESSEQUAL);
        dev.SetRenderState(RS_CULLMODE, CULL_NONE);
        dev.SetRenderState(RS_ALPHATESTENABLE, 0);
        dev.SetRenderState(RS_FOGENABLE, 1);
        dev.SetRenderState(RS_FOGCOLOR, skyNow);
        dev.SetRenderState(RS_FOGTABLEMODE, FOG_LINEAR);
        dev.SetRenderState(RS_FOGSTART, FloatBits(coast ? 200.0f : 120.0f));
        dev.SetRenderState(RS_FOGEND, FloatBits(coast ? 1400.0f : 600.0f));
        Material mat{{1, 1, 1, 1}, {1, 1, 1, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}, 0.0f};
        dev.SetMaterial(mat);
        Light sun{};
        sun.type = LIGHT_DIRECTIONAL;
        sun.diffuse = g_waterNight ? Color{0.1f, 0.12f, 0.2f, 1} : Color{1.0f, 0.93f, 0.8f, 1};
        sun.direction = {0.35f, -0.55f, 0.75f};             // the sun ahead of the camera, low: a glint on the water
        if (g_sunDirSet) sun.direction = {g_sunDir[0], g_sunDir[1], g_sunDir[2]};
        if (g_sweepSteps > 0) sun.direction = {sweepDir[0], sweepDir[1], sweepDir[2]};
        if (g_sunColourSet) sun.diffuse = {g_sunColour[0], g_sunColour[1], g_sunColour[2], 1};
        if (g_gameDay)
            sun.diffuse = {1, 1, 1, 1};                  // (the game's: white at every height, the dusk log)
        else if (g_sweepSteps > 0)
            sun.diffuse = {sun.diffuse.r * std::max(sweepDim, 0.15f), sun.diffuse.g * std::max(sweepDim, 0.2f),
                           sun.diffuse.b * std::max(sweepDim, 0.3f), 1};
        dev.SetLight(0, sun);
        dev.LightEnable(0, !g_indoor);
        // The sun's height as the game's light has it (the sine of its elevation: under the horizon, negative).
        const float sunLen = std::max(std::sqrt(sun.direction.x * sun.direction.x + sun.direction.y * sun.direction.y +
                                                sun.direction.z * sun.direction.z), 1e-6f);
        const float sunHs = -sun.direction.y / sunLen, dayW = gameDayW(sunHs);
        uint32_t ambient = g_waterNight ? 0xFF181C28u : 0xFF606468u;
        if (g_gameDay) {
            const float t = std::clamp((sunHs + 0.3f) / 0.6f, 0.0f, 1.0f);
            const uint32_t a = uint32_t(255.0f * (0.29f + 0.08f * t * t * (3.0f - 2.0f * t)) + 0.5f);
            ambient = 0xFF000000u | a << 16 | a << 8 | a;
        }
        if (g_indoor) {
            ambient = 0xFF404040u;
            for (int k = 0; k < 2; ++k) {
                Light lamp{};
                lamp.type = LIGHT_POINT;
                lamp.diffuse = {1.0f, 0.85f, 0.6f, 1};
                lamp.position = {eye.x + (k ? 5.0f : -5.0f), eye.y + 1.0f, eye.z + 10.0f};
                lamp.range = 25.0f;
                lamp.attenuation0 = 0.4f;
                lamp.attenuation1 = 0.08f;
                dev.SetLight(2 + k, lamp);
                dev.LightEnable(2 + k, true);
            }
        }
        if (g_waterLamp) {
            Light lamp{};
            lamp.type = LIGHT_POINT;
            lamp.diffuse = {1.0f, 0.8f, 0.5f, 1};
            lamp.position = {eye.x + 0.6f, eye.y + 0.4f, eye.z + 1.0f};
            lamp.range = 40.0f;
            lamp.attenuation0 = 0.3f;
            lamp.attenuation1 = 0.05f;
            dev.SetLight(1, lamp);
            dev.LightEnable(1, true);
        }
        dev.SetTextureStageState(0, TSS_COLORARG1, TA_TEXTURE);
        dev.SetTextureStageState(0, TSS_COLORARG2, TA_DIFFUSE);
        dev.SetTextureStageState(0, TSS_ALPHAOP, TOP_SELECTARG1);
        dev.SetTextureStageState(0, TSS_ALPHAARG1, TA_TEXTURE);
        if (g_skyDome) {
            // (A UV sphere of 17 x 9 rings round the eye in the fog's colour, lit, unblended: the game's dome.)
            static std::vector<VtxMesh> dome;
            static std::vector<uint16_t> domeIdx;
            if (dome.empty()) {
                for (int i = 0; i <= 8; ++i)
                    for (int j = 0; j <= 16; ++j) {
                        const float el = (float(i) / 8.0f - 0.5f) * kPi, az = float(j) / 16.0f * 2.0f * kPi;
                        const float x = std::cos(el) * std::cos(az), y = std::sin(el), z = std::cos(el) * std::sin(az);
                        dome.push_back({x * 500.0f, y * 500.0f, z * 500.0f, -x, -y, -z, 0xFFFFFFFFu, 0.0f, 0.0f});
                    }
                for (int i = 0; i < 8; ++i)
                    for (int j = 0; j < 16; ++j) {
                        const uint16_t a = uint16_t(i * 17 + j), b = uint16_t(a + 17);
                        domeIdx.insert(domeIdx.end(), {a, b, uint16_t(a + 1), uint16_t(a + 1), b, uint16_t(b + 1)});
                    }
            }
            for (VtxMesh& v : dome) v.diffuse = skyNow;
            dev.SetDrawVisual(uint32_t(VisualKind::Sky), "GenericMeshObject_t");
            dev.SetTransform(World, Translate(eye.x, eye.y, eye.z));
            dev.SetRenderState(RS_LIGHTING, 1);
            dev.SetRenderState(RS_ZWRITEENABLE, 0);
            dev.SetRenderState(RS_FOGENABLE, 0);
            dev.SetTexture(0, nullptr);
            dev.DrawIndexedPrimitive(TriangleList, kFvfMesh, dome.data(), uint32_t(dome.size()), domeIdx.data(),
                                     uint32_t(domeIdx.size()));
            dev.SetDrawVisual(0, nullptr);
            dev.SetRenderState(RS_ZWRITEENABLE, 1);
            dev.SetRenderState(RS_FOGENABLE, 1);
            dev.SetTransform(World, Identity());
        }
        // --clouds: the sky's puffs first (as the game: the sky, then the scene), each a sheet turned to the camera.
        if (g_clouds)
            DrawDemoPuffs(dev, eye, kPi * 0.5f + g_waterYaw, puffTex, puffMesh, puffIdx, 1.0f);
        dev.SetMaterial(mat);
        // --mesh-piece: each a static object of its own (as the game's: one visual each), drawn at the origin.
        for (size_t k = 0; k < g_meshPieces.size(); ++k) {
            static std::vector<std::vector<uint8_t>> pieces;
            if (pieces.size() < g_meshPieces.size()) {
                pieces.resize(g_meshPieces.size());
                for (size_t j = 0; j < g_meshPieces.size(); ++j)
                    if (FILE* f = std::fopen(g_meshPieces[j].c_str(), "rb")) {
                        int c;
                        while ((c = std::fgetc(f)) != EOF) pieces[j].push_back(uint8_t(c));
                        std::fclose(f);
                    }
            }
            const std::vector<uint8_t>& d = pieces[k];
            if (d.size() < 20) continue;
            uint32_t head[5];
            std::memcpy(head, d.data(), 20);
            const uint8_t* verts = d.data() + 20;
            const uint16_t* idx = reinterpret_cast<const uint16_t*>(verts + size_t(head[1]) * head[2]);
            dev.SetDrawVisual(uint32_t(VisualKind::Static), "RTriMesh_t", 0, uint32_t(0x1000 + k));
            dev.DrawIndexedPrimitive(TriangleList, head[0], verts, head[1], idx, head[4]);
            dev.SetDrawVisual(0, nullptr);
        }
        // The ground: base pass, then the light pass (lightmap + ambient, multiplied).
        dev.SetRenderState(RS_LIGHTING, 0);
        dev.SetRenderState(RS_ALPHABLENDENABLE, 0);
        dev.SetTexture(0, ground);
        dev.SetTextureStageState(0, TSS_TEXCOORDINDEX, 0);
        dev.SetTextureStageState(0, TSS_COLOROP, TOP_SELECTARG1);
        const uint32_t kChunkVerts = 1536;               // in chunks, as the game draws its ground
        for (size_t at = 0; at < terrain.size(); at += kChunkVerts)
            dev.DrawPrimitive(TriangleList, kFvfTerrain, terrain.data() + at,
                              uint32_t(std::min<size_t>(kChunkVerts, terrain.size() - at)));
        dev.SetRenderState(RS_LIGHTING, 1);
        dev.SetRenderState(RS_AMBIENT, ambient);
        dev.SetRenderState(RS_ALPHABLENDENABLE, 1);
        dev.SetRenderState(RS_SRCBLEND, BLEND_ZERO);
        dev.SetRenderState(RS_DESTBLEND, BLEND_SRCCOLOR);
        dev.SetRenderState(RS_ZFUNC, CMP_EQUAL);
        dev.SetTexture(0, g_gameDay ? dayLightmaps[std::clamp(int(dayW * 15.0f + 0.5f), 0, 15)] : lightmap);
        dev.SetTextureStageState(0, TSS_TEXCOORDINDEX, 1);
        dev.SetTextureStageState(0, TSS_COLOROP, TOP_ADD);
        for (size_t at = 0; at < terrain.size(); at += kChunkVerts)
            dev.DrawPrimitive(TriangleList, kFvfTerrain, terrain.data() + at,
                              uint32_t(std::min<size_t>(kChunkVerts, terrain.size() - at)));
        dev.SetTextureStageState(0, TSS_TEXCOORDINDEX, 0);
        dev.SetRenderState(RS_ZFUNC, CMP_LESSEQUAL);
        dev.SetRenderState(RS_ALPHABLENDENABLE, 0);
        // The props: lit, coloured by their vertices.
        dev.SetRenderState(RS_LIGHTING, 1);
        dev.SetRenderState(RS_COLORVERTEX, 1);
        dev.SetRenderState(RS_DIFFUSEMATERIALSOURCE, MCS_COLOR1);
        dev.SetRenderState(RS_AMBIENTMATERIALSOURCE, MCS_COLOR1);
        dev.SetTexture(0, nullptr);
        dev.SetTextureStageState(0, TSS_COLOROP, TOP_SELECTARG2);
        dev.DrawPrimitive(TriangleList, kFvfMesh, props.data(), uint32_t(props.size()));
        if (g_rigProps) {
            // Lit as the game's statics: white material, texture x lit colour (stage 0 MODULATE), a static visual.
            dev.SetRenderState(RS_COLORVERTEX, 0);
            dev.SetRenderState(RS_DIFFUSEMATERIALSOURCE, MCS_MATERIAL);
            dev.SetRenderState(RS_AMBIENTMATERIALSOURCE, MCS_MATERIAL);
            dev.SetMaterial(mat);
            dev.SetTexture(0, ground);
            dev.SetTextureStageState(0, TSS_COLOROP, TOP_MODULATE);
            dev.SetDrawVisual(uint32_t(VisualKind::Static), "RTriMesh_t", 0, 0x2000u);
            dev.DrawPrimitive(TriangleList, kFvfMesh, rigBox.data(), uint32_t(rigBox.size()));
            dev.SetDrawVisual(0, nullptr);
            dev.SetTexture(0, nullptr);
            dev.SetTextureStageState(0, TSS_COLOROP, TOP_SELECTARG2);
            dev.SetRenderState(RS_COLORVERTEX, 1);
            dev.SetRenderState(RS_DIFFUSEMATERIALSOURCE, MCS_COLOR1);
            dev.SetRenderState(RS_AMBIENTMATERIALSOURCE, MCS_COLOR1);
            if (frame == 0) {
                // Where they are on the screen (0..1, x right, y down), each shrunk to its middle 60%.
                const Matrix view = LookAtLH(eye, at, {0, 1, 0});
                const Matrix proj = PerspectiveLH(kPi / 3, float(g_width) / g_height, 0.3f, 2000.0f);
                auto rect = [&](const float (*pts)[3], int count, float out[4]) {
                    float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f};
                    for (int k = 0; k < count; ++k) {
                        float v[4], c[4];
                        for (int j = 0; j < 4; ++j)
                            v[j] = pts[k][0] * view.m[0][j] + pts[k][1] * view.m[1][j] + pts[k][2] * view.m[2][j] + view.m[3][j];
                        for (int j = 0; j < 4; ++j)
                            c[j] = v[0] * proj.m[0][j] + v[1] * proj.m[1][j] + v[2] * proj.m[2][j] + v[3] * proj.m[3][j];
                        const float sx = 0.5f * (c[0] / c[3] + 1.0f), sy = 0.5f * (1.0f - c[1] / c[3]);
                        lo[0] = std::min(lo[0], sx); lo[1] = std::min(lo[1], sy);
                        hi[0] = std::max(hi[0], sx); hi[1] = std::max(hi[1], sy);
                    }
                    for (int j = 0; j < 2; ++j) {
                        const float m = 0.5f * (lo[j] + hi[j]), h = 0.3f * (hi[j] - lo[j]);
                        out[j] = std::clamp(m - h, 0.0f, 1.0f);
                        out[2 + j] = std::clamp(m + h, 0.0f, 1.0f);
                    }
                };
                const float x0 = rigC[0] - rigE[0], x1 = rigC[0] + rigE[0], y0 = rigC[1] - rigE[1], y1 = rigC[1] + rigE[1];
                const float z0 = rigC[2] - rigE[2], z1 = rigC[2] + rigE[2];
                const float face[4][3] = {{x0, y0, z0}, {x1, y0, z0}, {x0, y1, z0}, {x1, y1, z0}};
                const float top[4][3] = {{x0, y1, z0}, {x1, y1, z0}, {x0, y1, z1}, {x1, y1, z1}};
                // (The ground just in front of it, nearer the camera: its shadow falls away from the camera.)
                const float gx0 = rigC[0] - 0.5f, gx1 = rigC[0] + 1.5f, gz0 = z0 - 3.0f, gz1 = z0 - 1.0f;
                const float patch[4][3] = {{gx0, height(gx0, gz0), gz0}, {gx1, height(gx1, gz0), gz0},
                                           {gx0, height(gx0, gz1), gz1}, {gx1, height(gx1, gz1), gz1}};
                float r[3][4];
                rect(face, 4, r[0]);
                rect(top, 4, r[1]);
                rect(patch, 4, r[2]);
                if (FILE* f = std::fopen("rig-props.txt", "w")) {
                    const char* names[3] = {"face", "top", "ground"};
                    for (int k = 0; k < 3; ++k)
                        std::fprintf(f, "%s %.4f %.4f %.4f %.4f\n", names[k], double(r[k][0]), double(r[k][1]),
                                     double(r[k][2]), double(r[k][3]));
                    std::fclose(f);
                }
            }
        }
        // A "character" swimming (--water-walker): a tall body in the lake, its feet under the surface, bobbing along.
        // Flagged a character (SetDrawVisual), so the renderer knows where it is - and ripples the water around it.
        if (g_waterWalker) {
            if (!g_fixedClock) std::this_thread::sleep_for(std::chrono::milliseconds(16));   // (the demo renders faster than the game: the
                                                                          //  water's simulation runs on real time)
            std::vector<VtxMesh> cv;
            std::vector<uint16_t> ci;
            // --water-jump: leaps out of the water every 90 frames (a 1.5 s arc) and lands again: a splash.
            float jumpUp = 0.0f;
            if (g_waterJump) {
                const float u = float(frame % 90) / 90.0f;
                jumpUp = 11.0f * u * (1.0f - u);
            }
            const float wx = g_waterJump ? 5.0f : 5.0f + 6.0f * std::sin(float(frame) * 0.012f);   // ~4 m/s at 60 fps
            AddCube(cv, ci, wx, 0.0f, 36.0f, 0.3f);
            for (VtxMesh& m : cv) {
                m.y = m.y * 3.0f + 0.12f * std::sin(float(frame) * 0.4f) + jumpUp;   // 1.8 tall: feet ~0.9 under the water
                if (m.y > 0.2f)
                    m.x += 0.05f * std::sin(frame * 0.7f);
            }
            dev.SetDrawVisual(uint32_t(VisualKind::Character), "swimmer");
            dev.DrawPrimitive(TriangleList, kFvfMesh, cv.data(), uint32_t(cv.size()));
            dev.SetDrawVisual(0, nullptr);
        }
        if (g_smokeWall) {                       // (--smoke-wall: an opaque wall right ahead hides all the smoke)
            const uint32_t grey = 0xFF707070;
            const VtxMesh wall[6] = {{-80, -10, -18, 0, 0, -1, grey, 0, 0}, {80, -10, -18, 0, 0, -1, grey, 1, 0},
                                     {80, 80, -18, 0, 0, -1, grey, 1, 1},   {-80, -10, -18, 0, 0, -1, grey, 0, 0},
                                     {80, 80, -18, 0, 0, -1, grey, 1, 1},   {-80, 80, -18, 0, 0, -1, grey, 0, 1}};
            dev.SetRenderState(RS_LIGHTING, 1);
            dev.SetRenderState(RS_ALPHABLENDENABLE, 0);
            dev.SetRenderState(RS_ZWRITEENABLE, 1);
            dev.SetRenderState(RS_CULLMODE, CULL_NONE);
            dev.SetTexture(0, nullptr);
            dev.SetTextureStageState(0, TSS_COLOROP, TOP_SELECTARG2);
            dev.DrawPrimitive(TriangleList, kFvfMesh, wall, 6);
        }
        // The water, last of the opaque scene (the game draws it from its sorted list after the solid world).
        dev.SetRenderState(RS_LIGHTING, 0);
        dev.SetRenderState(RS_ALPHABLENDENABLE, 1);
        dev.SetRenderState(RS_SRCBLEND, BLEND_SRCALPHA);
        dev.SetRenderState(RS_DESTBLEND, BLEND_INVSRCALPHA);
        dev.SetRenderState(RS_ZWRITEENABLE, 0);
        dev.SetTexture(0, waterTex);
        dev.SetTextureStageState(0, TSS_COLOROP, TOP_MODULATE);
        dev.SetTextureStageState(0, TSS_ALPHAOP, TOP_SELECTARG2);
        dev.SetTextureStageState(0, TSS_ALPHAARG2, TA_DIFFUSE);
        if (g_waterOff) {
            std::vector<VtxMesh> flat;
            for (int k : {0, 1, 2, 0, 2, 3}) {
                const auto& w = water[size_t(k)];
                flat.push_back({w.pos[0], w.pos[1], w.pos[2], 0, 1, 0, tint, w.pos[0] / 40, w.pos[2] / 40});
            }
            dev.DrawPrimitive(TriangleList, kFvfMesh, flat.data(), uint32_t(flat.size()));
        } else {
            dev.DrawWater(water.data(), uint32_t(water.size()), waterIdx.data(), uint32_t(waterIdx.size()));
            if (g_smokeWalker) {                 // (drawn after the water: the bodies are taken from last frame's draws)
                if (!g_fixedClock) std::this_thread::sleep_for(std::chrono::milliseconds(16));
                std::vector<VtxMesh> cv;
                std::vector<uint16_t> ci;
                const float wx = -3.0f + 3.5f * std::sin(float(frame) * 0.01f);   // ~2 m/s at 60 fps
                AddCube(cv, ci, wx, 0.0f, -11.5f, 0.3f);
                const float ground = height(wx, -11.5f);
                for (VtxMesh& m : cv) {
                    m.y = ground + (m.y + 0.3f) * 3.0f;                         // 1.8 tall, on the ground
                    if (m.y > ground + 0.2f) m.x += 0.05f * std::sin(frame * 0.7f);
                }
                dev.SetRenderState(RS_LIGHTING, 1);
                dev.SetRenderState(RS_ALPHABLENDENABLE, 0);
                dev.SetRenderState(RS_ZWRITEENABLE, 1);
                dev.SetTexture(0, nullptr);
                dev.SetTextureStageState(0, TSS_COLOROP, TOP_SELECTARG2);
                dev.SetDrawVisual(uint32_t(VisualKind::Character), "walker");
                dev.DrawPrimitive(TriangleList, kFvfMesh, cv.data(), uint32_t(cv.size()));
                dev.SetDrawVisual(0, nullptr);
                dev.SetRenderState(RS_LIGHTING, 0);
                dev.SetRenderState(RS_ALPHABLENDENABLE, 1);
                dev.SetRenderState(RS_ZWRITEENABLE, 0);
                dev.SetTexture(0, waterTex);
                dev.SetTextureStageState(0, TSS_COLOROP, TOP_MODULATE);
            }
            if (g_smoke) {                       // (--smoke: a smouldering fire on the shore in front)
                Device::SmokeSource src;
                src.pos[0] = -3.0f;
                src.pos[2] = -12.0f;
                src.pos[1] = height(src.pos[0], src.pos[2]);
                src.colour = 0x8A8580;
                if (g_smokeGame) {
                    src.radius = 0.25f; src.speed = 1.0f; src.life = 6.0f; src.size = 5.1f; src.colour = 0;
                    src.wind[0] = 7.65f * g_smokeWindScale; src.wind[1] = 1.36f * g_smokeWindScale; src.wind[2] = -1.92f * g_smokeWindScale;
                }
                if (g_smokeWobble > 0.0f) {
                    src.pos[0] += g_smokeWobble * std::sin(float(frame) * 2.1f);
                    src.pos[2] += g_smokeWobble * std::sin(float(frame) * 1.3f + 1.0f);
                }
                dev.SmokeSourceFrame(1, src);
                if (g_smokeTwo) {                // (--smoke-two: a bigger second one beside it - its own grid size)
                    Device::SmokeSource big;
                    big.pos[0] = 3.0f;
                    big.pos[2] = -10.0f;
                    big.pos[1] = height(big.pos[0], big.pos[2]);
                    big.size = 9.0f;
                    big.colour = 0x8A8580;
                    dev.SmokeSourceFrame(2, big);
                }
                for (int k = 0; k < g_smokeMany; ++k) {   // (--smoke-many: a ring of them, near and far, some behind)
                    Device::SmokeSource m;
                    const float a = 0.9f + float(k) * 2.39996f, r = 12.0f + 14.0f * float(k);
                    m.pos[0] = r * std::sin(a);
                    m.pos[2] = -r * std::cos(a);
                    m.pos[1] = height(m.pos[0], m.pos[2]);
                    m.size = 6.0f + float(k % 3);
                    m.colour = 0x302C28;
                    dev.SmokeSourceFrame(uint64_t(10 + k), m);
                }
            }
        }
        dev.SetTextureStageState(0, TSS_ALPHAOP, TOP_SELECTARG1);
        dev.SetRenderState(RS_ZWRITEENABLE, 1);
        dev.SetRenderState(RS_ALPHABLENDENABLE, 0);
        if (g_hudSprite) {
            struct VtxSprite { float x, y, z, rhw; uint32_t diffuse, specular; float u, v; };
            const uint32_t kFvfSprite = FVF_XYZRHW | FVF_DIFFUSE | FVF_SPECULAR | (1 << 8);
            const uint32_t c = 0xFF30C050;
            VtxSprite bar[4] = {{20, 20, 0, 1, c, 0, 0, 0}, {220, 20, 0, 1, c, 0, 1, 0},
                                {20, 40, 0, 1, c, 0, 0, 1}, {220, 40, 0, 1, c, 0, 1, 1}};
            dev.SetRenderState(RS_ZENABLE, 0);
            dev.SetRenderState(RS_LIGHTING, 0);
            dev.SetTexture(0, nullptr);
            dev.SetTextureStageState(0, TSS_COLOROP, TOP_SELECTARG2);
            dev.SetTextureStageState(0, TSS_COLORARG2, TA_DIFFUSE);
            dev.SetTextureStageState(0, TSS_ALPHAOP, TOP_SELECTARG2);
            dev.SetTextureStageState(0, TSS_ALPHAARG2, TA_DIFFUSE);
            dev.DrawPrimitive(TriangleStrip, kFvfSprite, bar, 4);
            // ... and one as the PF map's wash: no texture, MODULATE(TEXTURE, DIFFUSE) - native Direct3D disables a
            // textureless stage that reads its texture, so it must come out its vertex colour (0x8040C0 at y 60).
            const uint32_t c2 = 0xFF8040C0;
            VtxSprite wash[4] = {{20, 50, 0, 1, c2, 0, 0, 0}, {220, 50, 0, 1, c2, 0, 1, 0},
                                 {20, 70, 0, 1, c2, 0, 0, 1}, {220, 70, 0, 1, c2, 0, 1, 1}};
            dev.SetTextureStageState(0, TSS_COLOROP, TOP_MODULATE);
            dev.SetTextureStageState(0, TSS_COLORARG1, TA_TEXTURE);
            dev.SetTextureStageState(0, TSS_COLORARG2, TA_DIFFUSE);
            dev.DrawPrimitive(TriangleStrip, kFvfSprite, wash, 4);
            dev.SetRenderState(RS_ZENABLE, 1);
            dev.SetTextureStageState(0, TSS_COLOROP, TOP_MODULATE);
            dev.SetTextureStageState(0, TSS_ALPHAOP, TOP_SELECTARG1);
        }
        dev.EndFrame();
    }
}

template <typename D>
void RunShadowTest(D& dev, int frames, const std::string& shot, int cacheTest, int frameMs, const std::string& dump)
{
    auto groundPixels = Checker(64, 8, 0xFFC8C0B0, 0xFFA09888);
    auto fencePixels = Checker(64, 8, 0xFF806040, 0x00000000);
    Texture* ground = dev.CreateTexture(64, 64, groundPixels.data());
    // --grass-field: a green ground with a vertical brick stripe, for the grass filter (and its texture/not-texture
    // edge): grass must grow on the green and leave the stripe bare.
    std::vector<uint32_t> grassPixels(64 * 64);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
            grassPixels[y * 64 + x] = (x >= 26 && x < 38) ? 0xFF8A6A4A : ((x / 4 + y / 4) & 1 ? 0xFF3C6A2A : 0xFF2E5320);
    Texture* grassGround = dev.CreateTexture(64, 64, grassPixels.data());
    Texture* fence = dev.CreateTexture(64, 64, fencePixels.data());
    std::vector<uint32_t> blobPixels(64, 0xFFFFFFFF);
    std::vector<uint32_t> lightmapPixels(64, 0xFFC0C0C0);
    // --texture-flood N: N textures alive before the label's (a big zone has over 4,096: the label must still draw).
    std::vector<Texture*> flood;
    {
        const uint32_t grey = 0xFF808080u;
        for (int i = 0; i < g_textureFlood; ++i) flood.push_back(dev.CreateTexture(1, 1, &grey));
    }
    // A name as the game makes one: 128 x 32, transparent, with white strokes (text) in its middle rows.
    std::vector<uint32_t> labelPixels(128 * 32, 0x00000000u);
    for (uint32_t y = 10; y < 22; ++y)
        for (uint32_t x = 8; x < 120; ++x)
            if ((x / 3) % 3 != 0 && (y < 12 || y > 19 || (x / 3) % 3 == 1))
                labelPixels[y * 128 + x] = 0xFFFFFFFFu;
    Texture* labelTex = dev.CreateTexture(128, 32, labelPixels.data());
    Texture* lightmap = dev.CreateTexture(8, 8, lightmapPixels.data());
    Texture* blobTex = dev.CreateTexture(8, 8, blobPixels.data());
    Texture* puffTex[2] = {nullptr, nullptr};
    std::vector<VtxMesh> puffMesh;
    std::vector<uint16_t> puffIdx;
    if (g_clouds)
        MakeDemoPuffs(dev, puffTex, puffMesh, puffIdx);
    for (int frame = 0; frame < frames; ++frame) {
        if (g_grassFieldOn && frame == 5)
            dev.SetGrassField(true, g_grassFieldDist, 7.0f, 0.5f, g_grassFieldTex);   // a change rebuilds the tiles
        if (frame == frames - 1) {
            dev.RequestScreenshot(shot);
            if (!dump.empty()) dev.RequestFrameDump(dump);     // the shadow test dumps its last frame
        }
        dev.BeginFrame();
        dev.SetViewport({0, 0, g_width, g_height, 0.0f, 1.0f});
        dev.Clear(CLEAR_TARGET | CLEAR_ZBUFFER, 0xFF6080A0, 1.0f);
        // --cache-test K: after K frames, look straight down past the big right cube (which the "game" then no
        // longer draws, as it is out of view) at the tip of its shadow, which must stay if rvk remembered it.
        bool topDown = cacheTest > 0 && frame >= cacheTest;
        if (topDown) {
            dev.SetTransform(View, LookAtLH({7.5f, 4, 6.5f}, {7.5f, 0, 6.5f}, {0, 0, 1}));
            dev.SetTransform(Projection, PerspectiveLH(kPi / 4.5f, float(g_width) / g_height, 0.5f, 200.0f));
        } else if (g_sunView) {
            // --sun-view: low, looking towards the sun past the cubes (volumetric light shafts).
            dev.SetTransform(View, LookAtLH({11.0f, 1.5f, 11.0f}, {2.0f, 5.0f, 3.5f}, {0, 1, 0}));
        } else if (g_cameraPitch > -999.0f) {
            // --camera-pitch d: from the clouds' eye (DrawDemoPuffs) d degrees up, towards +z.
            const float p = g_cameraPitch * kPi / 180.0f;
            dev.SetTransform(View, LookAtLH({0, 9, -14}, {0, 9 + std::sin(p), -14 + std::cos(p)}, {0, 1, 0}));
            dev.SetTransform(Projection, PerspectiveLH(kPi / 3, float(g_width) / g_height, 0.5f, 200.0f));
        } else if (g_cameraYaw != 0.0f) {
            // --camera-yaw r: the camera turns r radians a frame around the scene (motion blur test).
            float a = g_cameraYaw * float(frame), ex = 16.6f * std::sin(a), ez = 2.0f - 16.6f * std::cos(a);
            dev.SetTransform(View, LookAtLH({ex, 9, ez}, {0, 0, 2}, {0, 1, 0}));
            dev.SetTransform(Projection, PerspectiveLH(kPi / 3, float(g_width) / g_height, 0.5f, 200.0f));
        } else {
            if (g_grassWalk >= -10.0f)
                dev.SetTransform(View, LookAtLH({0, 3.0f, -8.5f}, {0, 0.3f, -3.5f}, {0, 1, 0}));
            else
                dev.SetTransform(View, LookAtLH({0, 9, -14}, {0, 0, 2}, {0, 1, 0}));
            dev.SetTransform(Projection, PerspectiveLH(kPi / 3, float(g_width) / g_height, 0.5f, 200.0f));
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
        if (g_clouds) {                          // (--clouds: the sky's puffs, the sky shrunk to the scene)
            DrawDemoPuffs(dev, Vector{0, 9, -14}, kPi * 0.5f, puffTex, puffMesh, puffIdx, 0.03f);
            dev.SetMaterial(mat);
            dev.SetRenderState(RS_ALPHABLENDENABLE, 0);
            dev.SetTextureStageState(0, TSS_COLOROP, TOP_MODULATE);
        }
        if (g_walkerLight && g_grassWalk >= -10.0f) {   // the character's own light, as the game's (range ~6)
            Light own{};
            own.type = LIGHT_POINT;
            own.diffuse = {1.0f, 0.92f, 0.65f, 1};
            own.position = {g_grassWalk + g_grassWalkSpeed * float(frame), 2.1f, -3.2f};
            own.range = 6.0f;
            own.attenuation1 = 0.163f;
            dev.SetLight(1, own);
            dev.LightEnable(1, true);
        }
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
        if (g_grassFieldOn && !g_grassFieldTan)
            dev.SetTexture(0, grassGround);      // --grass-field: green ground, so RVK_GrassTex accepts it
        dev.SetRenderState(RS_LIGHTING, 0);
        dev.SetTextureStageState(0, TSS_COLOROP, TOP_SELECTARG1);
        dev.DrawIndexedPrimitive(TriangleList, kFvfTerrain, terrain.data(), uint32_t(terrain.size()), tidx.data(), uint32_t(tidx.size()));
        dev.SetTexture(0, ground);
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
        if (g_sunView) {                             // --sun-view: tall pillars between the camera and the sun
            const float pillars[3][2] = {{1.0f, 1.0f}, {-1.5f, 4.0f}, {3.5f, -1.5f}};
            for (const auto& pl : pillars) {
                std::vector<VtxMesh> v;
                std::vector<uint16_t> idx;
                for (int y = 0; y < 6; ++y) AddCube(v, idx, pl[0], 0.7f + 1.4f * float(y), pl[1], 0.7f);
                dev.DrawIndexedPrimitive(TriangleList, kFvfMesh, v.data(), uint32_t(v.size()), idx.data(), uint32_t(idx.size()));
            }
        }
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
        if (g_grassWalk >= -10.0f) {                 // --grass-walk: tufts of crossed cut-out quads, one draw each
            // Blended with depth writes and no alpha test, as the game draws its plants.
            dev.SetRenderState(RS_ALPHATESTENABLE, 0);
            dev.SetRenderState(RS_ALPHABLENDENABLE, 1);
            dev.SetRenderState(RS_SRCBLEND, BLEND_SRCALPHA);
            dev.SetRenderState(RS_DESTBLEND, BLEND_INVSRCALPHA);
            for (int gz = 0; gz < 8 * g_grassDense; ++gz)
                for (int gx = 0; gx < 13 * g_grassDense; ++gx) {
                    if (g_grassSize > 1.5f && (gx % 4 != 1 || gz % 4 != 2))
                        continue;                    // big cards: a few, apart
                    float x = -3.0f + (0.5f * float(gx) + 0.11f * float(gz % 3)) / float(g_grassDense),
                          z = -5.0f + 0.4f * float(gz) / float(g_grassDense), s = 0.3f * g_grassSize,
                          gh = 0.7f * g_grassSize;
                    VtxMesh t[8] = {{x - s, 0, z, 0, 0, -1, 0xFF60C060, 0, 1}, {x - s, gh, z, 0, 0, -1, 0xFF60C060, 0, 0},
                                    {x + s, gh, z, 0, 0, -1, 0xFF60C060, 1, 0}, {x + s, 0, z, 0, 0, -1, 0xFF60C060, 1, 1},
                                    {x, 0, z - s, -1, 0, 0, 0xFF60C060, 0, 1}, {x, gh, z - s, -1, 0, 0, 0xFF60C060, 0, 0},
                                    {x, gh, z + s, -1, 0, 0, 0xFF60C060, 1, 0}, {x, 0, z + s, -1, 0, 0, 0xFF60C060, 1, 1}};
                    uint16_t ti[12] = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};
                    dev.SetRenderState(RS_CULLMODE, CULL_NONE);
                    if (g_grassFlip) {               // model y down, around the tuft's origin
                        Matrix flip = Identity();
                        flip.m[1][1] = -1.0f;
                        flip.m[3][0] = x; flip.m[3][2] = z;
                        for (VtxMesh& m : t) { m.x -= x; m.z -= z; m.y = -m.y; }
                        dev.SetTransform(World, flip);
                    }
                    dev.DrawIndexedPrimitive(TriangleList, kFvfMesh, t, 8, ti, 12);
                    dev.SetTransform(World, Identity());
                }
            dev.SetRenderState(RS_CULLMODE, CULL_CCW);
            dev.SetRenderState(RS_ALPHABLENDENABLE, 0);
        }
        dev.SetRenderState(RS_ALPHATESTENABLE, 0);
        if (g_grassWalk >= -10.0f) {                 // the "character": CPU-skinned (its vertices change every frame)
            std::vector<VtxMesh> cv;
            std::vector<uint16_t> ci;
            float wx = g_grassWalk + g_grassWalkSpeed * float(frame);
            if (g_walkerRound) {
                // A rigid head first (as the game draws it): the same vertices every frame, moved by its world matrix.
                std::vector<VtxMesh> hv;
                std::vector<uint16_t> hi;
                const int kSeg = 8, kStack = 5;
                for (int r = 0; r <= kStack; ++r) {
                    float phi = 3.14159f * float(r) / kStack;
                    for (int s = 0; s < kSeg; ++s) {
                        float a = 6.28318f * float(s) / kSeg;
                        float nx = std::sin(phi) * std::cos(a), ny = std::cos(phi), nz = std::sin(phi) * std::sin(a);
                        hv.push_back({0.2f * nx, 0.2f * ny, 0.2f * nz, nx, ny, nz, 0xFFFFFFFF, float(s) / kSeg, float(r) / kStack});
                    }
                }
                for (int r = 0; r < kStack; ++r)
                    for (int s = 0; s < kSeg; ++s) {
                        uint16_t a = uint16_t(r * kSeg + s), b = uint16_t(r * kSeg + (s + 1) % kSeg);
                        uint16_t c = uint16_t(a + kSeg), d = uint16_t(b + kSeg);
                        uint16_t q[6] = {a, b, c, b, d, c};
                        hi.insert(hi.end(), q, q + 6);
                    }
                Matrix head = Identity();
                head.m[3][0] = wx; head.m[3][1] = 1.95f; head.m[3][2] = -3.2f;
                dev.SetTransform(World, head);
                dev.DrawIndexedPrimitive(TriangleList, kFvfMesh, hv.data(), uint32_t(hv.size()), hi.data(), uint32_t(hi.size()));
                dev.SetTransform(World, Identity());
                // Rings of 8 around, radius bulging in the middle; smooth normals.
                const int kSides = 8, kRings = 5;
                for (int r = 0; r < kRings; ++r) {
                    float y = 0.05f + 1.7f * float(r) / float(kRings - 1);
                    float rad = 0.15f + 0.2f * std::sin(3.14159f * float(r) / float(kRings - 1));
                    for (int s = 0; s < kSides; ++s) {
                        float a = 6.28318f * float(s) / float(kSides);
                        float nx = std::cos(a), nz = std::sin(a);
                        cv.push_back({rad * nx, y, rad * nz, nx, 0, nz, 0xFFFFFFFF,
                                      float(s) / kSides, 1.0f - float(r) / (kRings - 1)});
                    }
                }
                for (int r = 0; r + 1 < kRings; ++r)
                    for (int s = 0; s < kSides; ++s) {
                        uint16_t a = uint16_t(r * kSides + s), b = uint16_t(r * kSides + (s + 1) % kSides);
                        uint16_t c = uint16_t(a + kSides), d = uint16_t(b + kSides);
                        uint16_t q[6] = {a, c, b, b, c, d};
                        ci.insert(ci.end(), q, q + 6);
                    }
                int animFrame = g_walkerSkip > 0 ? frame / g_walkerSkip * g_walkerSkip : frame;
                for (VtxMesh& m : cv)
                    if (m.y > 1.0f) m.x += 0.02f * std::sin(animFrame * 0.9f);
                // Placed by its world matrix at the feet, as the game's characters (the carrier test looks there).
                Matrix body = Identity();
                body.m[3][0] = wx; body.m[3][2] = -3.2f;
                dev.SetTransform(World, body);
                if (g_walkerLight) {                 // an arm swinging beside the body: animated, its own draw
                    std::vector<VtxMesh> av;
                    std::vector<uint16_t> ai;
                    AddCube(av, ai, 0.43f, 1.15f, 0.0f, 0.07f);
                    for (VtxMesh& m : av) {
                        m.y = 1.15f + (m.y - 1.15f) * 5.0f;
                        m.x += (m.y - 1.5f) * 0.25f * std::sin(animFrame * 1.3f);
                    }
                    dev.DrawIndexedPrimitive(TriangleList, kFvfMesh, av.data(), uint32_t(av.size()), ai.data(), uint32_t(ai.size()));
                }
            } else {
                AddCube(cv, ci, wx, 0.9f, -3.2f, 0.3f);
                for (VtxMesh& m : cv) {
                    m.y = 0.9f + (m.y - 0.9f) * 3.0f;        // tall: 1.8 units, feet on the ground
                    if (m.y > 1.0f) m.x += 0.02f * std::sin(frame * 0.9f);
                }
            }
            dev.DrawIndexedPrimitive(TriangleList, kFvfMesh, cv.data(), uint32_t(cv.size()), ci.data(), uint32_t(ci.size()));
            dev.SetTransform(World, Identity());
        }
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
            dev.SetTextureStageState(0, TSS_COLOROP, TOP_MODULATE);   // (as the game: texture x diffuse)
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
            dev.SetViewport({g_width - 200, 20, 180, 180, 0.0f, 1.0f});
            std::vector<VtxMesh> pv;
            std::vector<uint16_t> pi;
            AddCube(pv, pi, 0, 0, 0, 1.5f);
            dev.DrawIndexedPrimitive(TriangleList, kFvfMesh, pv.data(), uint32_t(pv.size()), pi.data(), uint32_t(pi.size()));
            dev.SetViewport({0, 0, g_width, g_height, 0.0f, 1.0f});
            if (!topDown) {
                dev.SetTransform(View, saveView);
                dev.SetTransform(Projection, PerspectiveLH(kPi / 3, float(g_width) / g_height, 0.5f, 200.0f));
            } else {
                dev.SetTransform(View, LookAtLH({7.5f, 4, 6.5f}, {7.5f, 0, 6.5f}, {0, 0, 1}));
                dev.SetTransform(Projection, PerspectiveLH(kPi / 4.5f, float(g_width) / g_height, 0.5f, 200.0f));
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
        if (g_shinePass >= 0) {
            std::vector<VtxMesh> sv;
            std::vector<uint16_t> si;
            const int rings = 6, segs = 8;            // (coarse: rounding by tessellation shows)
            const float r = 1.6f, cx = -3.0f, cy = 1.8f, cz = -7.0f;
            for (int i = 0; i <= rings; ++i)
                for (int j = 0; j <= segs; ++j) {
                    const float th = kPi * i / rings, ph = 2 * kPi * j / segs;
                    const float nx = std::sin(th) * std::cos(ph), ny = std::cos(th), nz = std::sin(th) * std::sin(ph);
                    sv.push_back({cx + r * nx, cy + r * ny, cz + r * nz, nx, ny, nz, 0xFF808080, 0, 0});
                }
            for (int i = 0; i < rings; ++i)
                for (int j = 0; j < segs; ++j) {
                    const uint16_t a = uint16_t(i * (segs + 1) + j), b = uint16_t(a + segs + 1);
                    si.insert(si.end(), {a, b, uint16_t(a + 1), uint16_t(a + 1), b, uint16_t(b + 1)});
                }
            dev.LightEnable(0, true);                 // (the sun: switched off for what follows)
            dev.SetRenderState(RS_LIGHTING, 1);       // (off since the blob shadow; characters are lit draws)
            dev.SetDrawVisual(uint32_t(VisualKind::Character), "RCATMesh_t");
            dev.SetTextureStageState(0, TSS_COLOROP, TOP_SELECTARG2);
            dev.DrawIndexedPrimitive(TriangleList, kFvfMesh, sv.data(), uint32_t(sv.size()), si.data(), uint32_t(si.size()));
            if (g_shinePass == 1) {
                for (VtxMesh& v : sv) v.diffuse = 0xFFFF0000;
                dev.SetRenderState(RS_ZWRITEENABLE, 0);
                dev.SetRenderState(RS_ZFUNC, CMP_EQUAL);
                dev.SetRenderState(RS_ALPHABLENDENABLE, 1);
                dev.SetRenderState(RS_SRCBLEND, BLEND_ONE);
                dev.SetRenderState(RS_DESTBLEND, BLEND_ONE);
                dev.DrawIndexedPrimitive(TriangleList, kFvfMesh, sv.data(), uint32_t(sv.size()), si.data(), uint32_t(si.size()));
                dev.SetRenderState(RS_ALPHABLENDENABLE, 0);
                dev.SetRenderState(RS_ZFUNC, CMP_LESSEQUAL);
                dev.SetRenderState(RS_ZWRITEENABLE, 1);
            }
            dev.SetDrawVisual(0, nullptr);
            dev.SetRenderState(RS_LIGHTING, 0);
            dev.LightEnable(0, false);
        }
        if (g_uiWindow) {
            // As the game: its windows into the interface layer, then the pointer straight onto the frame.
            dev.SetRenderState(RS_ZENABLE, 0);
            dev.SetRenderState(RS_ALPHABLENDENABLE, 1);
            dev.SetRenderState(RS_SRCBLEND, BLEND_SRCALPHA);
            dev.SetRenderState(RS_DESTBLEND, BLEND_INVSRCALPHA);
            dev.SetTextureStageState(0, TSS_COLOROP, TOP_SELECTARG2);
            dev.SetTextureStageState(0, TSS_ALPHAOP, TOP_SELECTARG2);
            dev.SetTextureStageState(0, TSS_ALPHAARG2, TA_DIFFUSE);
            // A bar drawn before the interface (AOReloaded's cast bars, the text after zoning): with frame generation
            // into the labels' layer, under the window.
            VtxRhwDiffuse bar[4] = {{250, 420, 0, 1, 0xF020C040}, {650, 420, 0, 1, 0xF020C040},
                                    {250, 440, 0, 1, 0xF020C040}, {650, 440, 0, 1, 0xF020C040}};
            dev.DrawPrimitive(TriangleStrip, kFvfRhwDiffuse, bar, 4);
            dev.InterfaceBegin(true);
            VtxRhwDiffuse window[4] = {{300, 200, 0, 1, 0xE0304060}, {700, 200, 0, 1, 0xE0304060},
                                       {300, 450, 0, 1, 0xE0304060}, {700, 450, 0, 1, 0xE0304060}};
            dev.DrawPrimitive(TriangleStrip, kFvfRhwDiffuse, window, 4);
            // Textured pieces, as the game's windows have (the lean shader's A/B, RVK_LiteShader): a grille alpha-tested
            // through its texture's holes, and a panel blended with its texture tinted by the vertex colour.
            dev.SetTextureStageState(0, TSS_COLOROP, TOP_MODULATE);
            dev.SetTextureStageState(0, TSS_COLORARG1, TA_TEXTURE);
            dev.SetTextureStageState(0, TSS_COLORARG2, TA_DIFFUSE);
            dev.SetTextureStageState(0, TSS_ALPHAOP, TOP_MODULATE);
            dev.SetTextureStageState(0, TSS_ALPHAARG1, TA_TEXTURE);
            dev.SetTextureStageState(0, TSS_ALPHAARG2, TA_DIFFUSE);
            dev.SetTexture(0, fence);
            dev.SetRenderState(RS_ALPHATESTENABLE, 1);
            dev.SetRenderState(RS_ALPHAREF, 0x80);
            dev.SetRenderState(RS_ALPHAFUNC, CMP_GREATEREQUAL);
            VtxRhwDiffuseTex grille[4] = {{320, 220, 0, 1, 0xFFFFE0A0, 0, 0}, {460, 220, 0, 1, 0xFFFFE0A0, 2, 0},
                                          {320, 360, 0, 1, 0xFFFFE0A0, 0, 2}, {460, 360, 0, 1, 0xFFFFE0A0, 2, 2}};
            dev.DrawPrimitive(TriangleStrip, kFvfRhwDiffuseTex, grille, 4);
            dev.SetRenderState(RS_ALPHATESTENABLE, 0);
            dev.SetTexture(0, ground);
            VtxRhwDiffuseTex panel[4] = {{540, 230, 0, 1, 0xC060A0FF, 0, 0}, {680, 230, 0, 1, 0xC0FF8040, 1, 0},
                                         {540, 420, 0, 1, 0x8040FF80, 0, 1}, {680, 420, 0, 1, 0x80FFFFFF, 1, 1}};
            dev.DrawPrimitive(TriangleStrip, kFvfRhwDiffuseTex, panel, 4);
            dev.SetTexture(0, nullptr);
            dev.SetTextureStageState(0, TSS_COLOROP, TOP_SELECTARG2);
            dev.SetTextureStageState(0, TSS_ALPHAOP, TOP_SELECTARG2);
            dev.InterfaceEnd();
            VtxRhwDiffuse pointer[4] = {{480, 300, 0, 1, 0xFFFF2020}, {520, 300, 0, 1, 0xFFFF2020},
                                        {480, 340, 0, 1, 0xFFFF2020}, {520, 340, 0, 1, 0xFFFF2020}};
            dev.DrawPrimitive(TriangleStrip, kFvfRhwDiffuse, pointer, 4);
            dev.SetRenderState(RS_ALPHABLENDENABLE, 0);
            dev.SetRenderState(RS_ZENABLE, 1);
        }
        dev.EndFrame();
        if (frameMs > 0)
            Sleep(DWORD(frameMs));
    }
    dev.DestroyTexture(ground);
    dev.DestroyTexture(fence);
    dev.DestroyTexture(blobTex);
    dev.DestroyTexture(lightmap);
    dev.DestroyTexture(labelTex);
    for (Texture* t : flood) dev.DestroyTexture(t);
}

bool g_slabTest = false;   // --point-shadow-slab
bool g_twoLamps = false;   // --two-lamps: the point shadow test with a second lamp
bool g_lampWobble = false; // --lamp-wobble: lamp positions change in their last bits each frame
bool g_nightSign = false;  // --night-glow: the point shadow test gets an unlit sign of lit windows (night glow)
float g_off[3] = {};        // --world-offset x y z: the scene far from the origin, like Anarchy Online's world
Matrix Offset()
{
    Matrix m = Identity();
    m.m[3][0] = g_off[0]; m.m[3][1] = g_off[1]; m.m[3][2] = g_off[2];
    return m;
}

// Placed objects' shadows (--statel-test PACK PLAYFIELD X Y Z [--statel-mode M]): a playfield's pack (ao-assets
// statels) loaded as in the game, the camera above (X, Y, Z), a flat lit ground at Y and one cube of the "game's"
// (shadows need a caster drawn) - the placed objects' shadows fall on the ground though nothing draws them.
// Mode 2 (the default here: the demo draws none of them to confirm the placement), 0 for the scene without them.
// --statel-skinned: a "character" skinned on the GPU (DrawSkinned, as the game's) walking round the cube - an 8-sided
// column on one bone, the bone moved each frame (a new job each frame, as the game makes them).
// --statel-movers: the cube twice more through a world matrix, one standing and one circling - one mesh at two places
// a frame, one of them new each frame (the ray tracing's and Brixelizer's instances must not pile up: placements.h).
std::string g_statelPack;
bool g_statelMovers = false;
uint32_t g_statelPlayfield = 0, g_statelMode = 2;
float g_statelAt[3] = {};

template <typename D>
void RunStatelTest(D& dev, int frames, const std::string& shot, const std::string& dump)
{
    auto groundPixels = Checker(64, 8, 0xFFC8C0B0, 0xFFA09888);
    Texture* ground = dev.CreateTexture(64, 64, groundPixels.data());
    dev.SetStatelMode(g_statelMode);
    dev.LoadStatels(g_statelPack.c_str(), g_statelPlayfield);
    const float x = g_statelAt[0], y = g_statelAt[1], z = g_statelAt[2];
    std::vector<VtxMesh> cube;
    std::vector<uint16_t> cubeIdx;
    AddCube(cube, cubeIdx, x - 6.0f, y + 0.5f, z - 6.0f, 0.5f);
    auto column = MakeColumn();
    for (int frame = 0; frame < frames; ++frame) {
        if (g_shotRun > 0 && frame >= frames - g_shotRun) {
            std::string name = shot;
            name.insert(name.size() - 4, "-" + std::to_string(frame - (frames - g_shotRun)));
            dev.RequestScreenshot(name);
        }
        if (frame == frames - 1) {
            dev.RequestScreenshot(shot);
            if (!dump.empty()) dev.RequestFrameDump(dump);
        }
        dev.BeginFrame();
        dev.SetViewport({0, 0, g_width, g_height, 0.0f, 1.0f});
        dev.Clear(CLEAR_TARGET | CLEAR_ZBUFFER, 0xFF6080A0, 1.0f);
        dev.SetTransform(View, LookAtLH({x - 22.0f, y + 16.0f, z - 22.0f}, {x, y, z}, {0, 1, 0}));
        dev.SetTransform(Projection, PerspectiveLH(kPi / 3, float(g_width) / g_height, 0.5f, 600.0f));
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
        if (g_sunDirSet) sun.direction = {g_sunDir[0], g_sunDir[1], g_sunDir[2]};   // (--sun-dir)
        dev.SetLight(0, sun);
        dev.LightEnable(0, true);
        dev.SetTextureStageState(0, TSS_COLOROP, TOP_MODULATE);
        dev.SetTextureStageState(0, TSS_COLORARG1, TA_TEXTURE);
        dev.SetTextureStageState(0, TSS_COLORARG2, TA_DIFFUSE);
        dev.SetTextureStageState(0, TSS_ALPHAOP, TOP_SELECTARG1);
        dev.SetTextureStageState(0, TSS_ALPHAARG1, TA_TEXTURE);
        dev.SetTexture(0, ground);
        dev.SetRenderState(RS_LIGHTING, 1);
        const float r = 120.0f;
        VtxMesh q[4] = {{x - r, y, z - r, 0, 1, 0, 0xFFFFFFFF, 0, 0}, {x + r, y, z - r, 0, 1, 0, 0xFFFFFFFF, 60, 0},
                        {x + r, y, z + r, 0, 1, 0, 0xFFFFFFFF, 60, 60}, {x - r, y, z + r, 0, 1, 0, 0xFFFFFFFF, 0, 60}};
        dev.DrawPrimitive(TriangleFan, kFvfMesh, q, 4);
        dev.DrawIndexedPrimitive(TriangleList, kFvfMesh, cube.data(), uint32_t(cube.size()), cubeIdx.data(),
                                 uint32_t(cubeIdx.size()));
        if (g_statelMovers) {
            const float t = float(frame) * 2.0f * kPi / 300.0f;
            const float at[2][2] = {{8.0f, 4.0f}, {4.0f * std::cos(t), 4.0f * std::sin(t)}};
            for (const auto& a : at) {
                Matrix w = Identity();
                w.m[3][0] = a[0];
                w.m[3][2] = a[1];
                dev.SetTransform(World, w);
                dev.DrawIndexedPrimitive(TriangleList, kFvfMesh, cube.data(), uint32_t(cube.size()), cubeIdx.data(),
                                         uint32_t(cubeIdx.size()));
            }
            dev.SetTransform(World, Identity());
        }
        if constexpr (requires { dev.DrawSkinned(0u, 0u, std::declval<rvk::skin::Job&>(), 0u, 0u, nullptr, 0u); }) {
            if (g_statelSkinned) {               // the column walking round the cube, a turn in 240 frames
                const float t = float(frame) * 2.0f * kPi / 240.0f;
                DrawColumn(dev, column, x - 6.0f + 3.0f * std::cos(t), y, z - 6.0f + 3.0f * std::sin(t), 7);
            }
        }
        dev.EndFrame();
    }
}


// A captured interior on its own (--portal-view FILE; shop portals, docs/shop-portal-plan.md): its pieces replayed as
// the game drew them - their textures, material, render and stage states and lights - from just inside its entrance
// looking in, or from --portal-cam R U I YAW: R units to the right, U up and I into the room from the entrance's bottom
// centre, turned YAW degrees (positive: to the right). Checks a capture without the game.
std::string g_portalView;
float g_portalCam[4] = {0.0f, 1.8f, 0.5f, 0.0f};

template <typename D>
void RunPortalView(D& dev, int frames, const std::string& shot)
{
    portal::Capture capture;
    if (!portal::Load(g_portalView.c_str(), capture)) {
        std::printf("portal view: %s isn't a capture\n", g_portalView.c_str());
        return;
    }
    std::vector<Texture*> textures;
    for (const portal::CapturedTexture& t : capture.textures) {
        Texture* tex = dev.CreateTexture(t.width, t.height, t.format, 1);
        if (tex) dev.UpdateTexture(tex, 0, 0, 0, t.width, t.height, t.level0.data(), FormatRowBytes(t.format, t.width));
        textures.push_back(tex);
    }
    // The view: from the entrance (or the pieces' middle), along its way in.
    float base[3] = {}, in[3] = {0.0f, 0.0f, 1.0f};
    if (capture.entranceKnown) {
        std::memcpy(base, capture.entrance, sizeof(base));
        std::memcpy(in, capture.inward, sizeof(in));
    } else if (!capture.pieces.empty()) {
        for (const auto& p : capture.pieces)
            for (int j = 0; j < 3; ++j) base[j] += p.world.m[3][j] / float(capture.pieces.size());
    }
    const float right[3] = {in[2], 0.0f, -in[0]};
    const float yaw = g_portalCam[3] * kPi / 180.0f;
    const float look[3] = {in[0] * std::cos(yaw) + right[0] * std::sin(yaw), 0.0f,
                           in[2] * std::cos(yaw) + right[2] * std::sin(yaw)};
    const Vector eye{base[0] + right[0] * g_portalCam[0] + in[0] * g_portalCam[2], base[1] + g_portalCam[1],
                     base[2] + right[2] * g_portalCam[0] + in[2] * g_portalCam[2]};
    const Vector at{eye.x + look[0], eye.y - 0.15f, eye.z + look[2]};
    std::printf("portal view: playfield %u, %zu pieces, %zu textures, entrance %s (%.2f %.2f %.2f) in (%.2f %.2f)\n",
                capture.playfield, capture.pieces.size(), capture.textures.size(), capture.entranceKnown ? "known" : "not known",
                base[0], base[1], base[2], in[0], in[2]);
    // The states the replay sets (the rest stay the device's): what the pieces' look depends on.
    static const uint32_t kStates[] = {RS_LIGHTING, RS_AMBIENT, RS_ALPHABLENDENABLE, RS_SRCBLEND, RS_DESTBLEND,
                                       RS_ALPHATESTENABLE, RS_ALPHAREF, RS_ALPHAFUNC, RS_CULLMODE, RS_ZENABLE,
                                       RS_ZWRITEENABLE, RS_ZFUNC, RS_COLORVERTEX, RS_DIFFUSEMATERIALSOURCE,
                                       RS_AMBIENTMATERIALSOURCE, RS_SPECULARMATERIALSOURCE, RS_EMISSIVEMATERIALSOURCE,
                                       RS_SPECULARENABLE, RS_NORMALIZENORMALS, RS_TEXTUREFACTOR, RS_ZBIAS};
    for (int frame = 0; frame < frames; ++frame) {
        if (frame == frames - 1)
            dev.RequestScreenshot(shot);
        dev.BeginFrame();
        dev.SetViewport({0, 0, g_width, g_height, 0.0f, 1.0f});
        dev.Clear(CLEAR_TARGET | CLEAR_ZBUFFER, 0xFF000000, 1.0f);
        dev.SetTransform(View, LookAtLH(eye, at, {0, 1, 0}));
        dev.SetTransform(Projection, PerspectiveLH(kPi / 3, float(g_width) / g_height, 0.2f, 400.0f));
        uint32_t lit = 0;
        for (const portal::CapturedPiece& p : capture.pieces) {
            dev.SetTransform(World, p.world);
            dev.SetMaterial(p.material);
            for (uint32_t st : kStates)
                dev.SetRenderState(st, p.rs[st]);
            for (uint32_t stage = 0; stage < 2; ++stage) {
                for (uint32_t t = 1; t < TSS_COUNT; ++t)
                    dev.SetTextureStageState(stage, t, p.tss[stage][t]);
                dev.SetTexture(stage, p.texture[stage] >= 0 ? textures[size_t(p.texture[stage])] : nullptr);
            }
            for (uint32_t i = 0; i < p.lights.size(); ++i) {
                dev.SetLight(i, p.lights[i]);
                dev.LightEnable(i, true);
            }
            for (uint32_t i = uint32_t(p.lights.size()); i < lit; ++i)
                dev.LightEnable(i, false);
            lit = uint32_t(p.lights.size());
            dev.SetDrawVisual(p.kind, p.kind == uint32_t(VisualKind::Room) ? "VisualRoom_t" : "RTriMesh_t");
            if (p.indices.empty())
                dev.DrawPrimitive(p.primitive, p.fvf, p.vertices.data(), p.vertexCount);
            else
                dev.DrawIndexedPrimitive(p.primitive, p.fvf, p.vertices.data(), p.vertexCount, p.indices.data(),
                                         uint32_t(p.indices.size()));
        }
        dev.SetDrawVisual(0, nullptr);
        dev.EndFrame();
    }
    for (Texture* t : textures) dev.DestroyTexture(t);
}

// The capture's and the links' side (--portal-capture-test DIR): a walk into a shop and out. A street (playfield 4100)
// with a wall and a black doorway quad (the black texture, 1010004:6014) and a native ground, the camera in front of it; a teleport after
// 0.4 s; then a small room as the game draws a shop's - walls and floor tagged as rooms, boxes as props - in a playfield
// of its own (4242), left after 0.8 s (before the capture's quiet spell: it saves on leaving); then the street again.
// The capture writes DIR\4242.rvkp (which --portal-view draws) and the link DIR\links.tsv; back in the street the
// doorway is known, shows the room, and its street is recorded (DIR\street-4100-200-203.rvkp); then into the shop again,
// looking back at its entrance: the street through it (the street portal). Shots: -street, -room, -portal, -inside.
std::string g_portalCaptureDir;
std::string g_portalDump;          // (--dump with --portal-capture-test: the last frame's dump, the portal in view)

template <typename D>
void RunPortalCaptureTest(D& dev, int frames, const std::string& shot)
{
    rvk::SetPortalDir(g_portalCaptureDir.c_str());
    dev.SetSkyPlayfield(4100);
    const uint32_t blackPixel[16] = {0xFF000000, 0xFF000000, 0xFF000000, 0xFF000000, 0xFF000000, 0xFF000000,
                                     0xFF000000, 0xFF000000, 0xFF000000, 0xFF000000, 0xFF000000, 0xFF000000,
                                     0xFF000000, 0xFF000000, 0xFF000000, 0xFF000000};
    Texture* black = dev.CreateTexture(4, 4, blackPixel);
    dev.SetTextureRdbId(black, 6014);
    // The street's wall (z = 3) and its doorway (2.5 wide, 2.9 tall, from the ground), facing the camera (-z).
    const VtxMesh wallQuad[4] = {{-6, 0, 3, 0, 0, -1, 0xFFFFFFFF, 0, 0}, {6, 0, 3, 0, 0, -1, 0xFFFFFFFF, 4, 0},
                                 {6, 5, 3, 0, 0, -1, 0xFFFFFFFF, 4, 2}, {-6, 5, 3, 0, 0, -1, 0xFFFFFFFF, 0, 2}};
    const VtxMesh door[4] = {{-1.25f, 0, 2.99f, 0, 0, -1, 0xFFFFFFFF, 0, 0}, {1.25f, 0, 2.99f, 0, 0, -1, 0xFFFFFFFF, 1, 0},
                             {1.25f, 2.9f, 2.99f, 0, 0, -1, 0xFFFFFFFF, 1, 1}, {-1.25f, 2.9f, 2.99f, 0, 0, -1, 0xFFFFFFFF, 0, 1}};
    const uint16_t quadIdx[6] = {0, 1, 2, 0, 2, 3};
    auto wallPixels = Checker(64, 4, 0xFFB09070, 0xFF806050);
    auto boxPixels = Checker(64, 8, 0xFF60A0D0, 0xFF305070);
    Texture* wall = dev.CreateTexture(64, 64, wallPixels.data());
    Texture* box = dev.CreateTexture(64, 64, boxPixels.data());
    std::vector<VtxMesh> room, props;
    std::vector<uint16_t> roomIdx, propIdx;
    AddCube(room, roomIdx, 0.0f, 3.0f, 8.0f, 8.0f);   // (the room: seen from inside)
    for (int i = 0; i < 4; ++i)
        AddCube(props, propIdx, -4.5f + 3.0f * float(i), 0.0f, 12.0f - 2.0f * float(i % 2), 0.8f);
    std::vector<VtxMesh> pillar;
    std::vector<uint16_t> pillarIdx;
    AddCube(pillar, pillarIdx, 0.7f, -3.5f, 4.0f, 0.7f);
    std::vector<VtxMesh> streetBoxes;
    std::vector<uint16_t> streetBoxIdx;
    for (int i = 0; i < 3; ++i)
        AddCube(streetBoxes, streetBoxIdx, -4.0f + 4.0f * float(i), 1.0f, -6.0f - 3.0f * float(i % 2), 1.0f);
    // The street's native ground (the street stands at x, z 200, the ground's cells from 0): 128 x 128 cells of 4,
    // four tiles, flat at the doorway and rolling further out, a lightmap of bands - its cut-out recorded with the
    // street and drawn through the shop's entrance.
    constexpr float kOx = 200.0f, kOz = 200.0f;
    constexpr uint32_t kCells = 128;
    std::shared_ptr<NativeGround> ground;
    std::shared_ptr<std::vector<uint8_t>> packed;
    {
        const uint32_t colours[4] = {0xFF6E8C3Cu, 0xFF8C9A5Au, 0xFF9C8C7Cu, 0xFFB09060u};
        std::vector<Texture*> tiles;
        for (uint32_t t = 0; t < 4; ++t) {
            std::vector<uint32_t> px(64 * 64);
            for (uint32_t y = 0; y < 64; ++y)
                for (uint32_t x = 0; x < 64; ++x)
                    px[size_t(y) * 64 + x] = (x < 20 && y < 12) ? 0xFF303030u : ((x ^ y) & 8u) ? colours[t] : colours[t] - 0x00101010u;
            tiles.push_back(dev.CreateTexture(64, 64, px.data()));
        }
        auto height = [&](float x, float z) {
            const float d = std::sqrt((x - kOx) * (x - kOx) + (z - kOz) * (z - kOz));
            const float t = std::clamp((d - 12.0f) / 30.0f, 0.0f, 1.0f);
            return -0.02f + t * t * (3.0f * std::sin(x * 0.06f) * std::cos(z * 0.045f) + 2.0f);
        };
        ground = std::make_shared<NativeGround>();
        ground->id = 4100;
        ground->cellsX = ground->cellsZ = kCells;
        ground->patch = 64;
        ground->heights.resize(size_t(kCells + 1) * (kCells + 1));
        for (uint32_t r = 0; r <= kCells; ++r)
            for (uint32_t c = 0; c <= kCells; ++c) ground->heights[size_t(r) * (kCells + 1) + c] = height(c * 4.0f, r * 4.0f);
        ground->codes.resize(size_t(kCells) * kCells);
        for (uint32_t r = 0; r < kCells; ++r)
            for (uint32_t c = 0; c < kCells; ++c)
                ground->codes[size_t(r) * kCells + c] = uint16_t(((c / 3 + r / 5) % 4) | ((c * 7 + r) % 4) << 14);
        ground->tiles = tiles;
        packed = std::make_shared<std::vector<uint8_t>>(size_t(kCells / 2) * kCells);
        for (uint32_t r = 0; r < kCells; ++r)
            for (uint32_t c = 0; c < kCells; c += 2) {
                const uint8_t v = uint8_t(8 + (r / 4 % 2) * 6);
                (*packed)[size_t(r) * (kCells / 2) + c / 2] = uint8_t(v << 4 | v);
            }
    }
    auto packedEvening = std::make_shared<std::vector<uint8_t>>(*packed);
    bool groundSet = false;
    Matrix streetWorld = Identity();
    streetWorld.m[3][0] = kOx;
    streetWorld.m[3][2] = kOz;
    frames = std::max(frames, 330);
    for (int frame = 0; frame < frames; ++frame) {
        if (frame == frames - 1 && !g_portalDump.empty())
            dev.RequestFrameDump(g_portalDump);
        if (frame == 20 || frame == 70 || frame == 199 || frame == frames - 1) {
            std::string name = shot;
            name.insert(name.size() - 4, frame == 20 ? "-street" : frame == 70 ? "-room" : frame == 199 ? "-portal" : "-inside");
            dev.RequestScreenshot(name);
        }
        if (frame == 25 || frame == 205)
            dev.ZoneSignal(rvk::kZoneTeleport);  // (at the doorway: StartTeleport)
        if (frame == 30 || frame == 80 || frame == 210)
            dev.SetSkyPlayfield(0);              // (a zone change's blank frames)
        if (frame == 35 || frame == 215)
            dev.SetSkyPlayfield(4242);           // (the shop)
        if (frame == 100)
            dev.SetSkyPlayfield(4100);           // (the street again: its doorway's street recorded)
        const bool street = frame < 30 || (frame >= 100 && frame < 210);
        const bool blank = (frame >= 30 && frame < 35) || (frame >= 80 && frame < 100) || (frame >= 210 && frame < 215);
        const bool lookingBack = frame >= 215;   // (in the shop again, looking at its entrance: the street through it)
        dev.BeginFrame();
        dev.SetViewport({0, 0, g_width, g_height, 0.0f, 1.0f});
        dev.Clear(CLEAR_TARGET | CLEAR_ZBUFFER, 0xFF000000, 1.0f);
        if (blank) {
            dev.EndFrame();
            Sleep(16);
            continue;
        }
        if (street != groundSet) {               // (the street's ground: the game hands it over on arriving)
            groundSet = street;
            dev.SetNativeGround(street ? ground : nullptr);
            if (street) dev.SetNativeGroundLightmap(packed, kCells, kCells, 0.45f);
        }
        // The street's hour: day while its street is recorded, evening from frame 150 (its light, ambient and
        // lightmap): seen from inside the shop afterwards it is the evening's, not the day it was recorded in.
        const bool evening = frame >= 150;
        Light sun{};
        sun.type = LIGHT_DIRECTIONAL;
        sun.diffuse = evening ? Color{0.9f, 0.45f, 0.25f, 1} : Color{0.9f, 0.85f, 0.75f, 1};
        sun.direction = evening ? Vector{0.9f, -0.25f, 0.3f} : Vector{0.55f, -0.7f, 0.45f};
        if (frame == 150)
            dev.SetNativeGroundLightmap(packedEvening, kCells, kCells, 0.75f);
        if (street) {
            dev.SetTransform(View, LookAtLH({kOx, 1.8f, kOz}, {kOx, 1.6f, kOz + 3.0f}, {0, 1, 0}));
            dev.SetTransform(Projection, PerspectiveLH(kPi / 3, float(g_width) / g_height, 0.2f, 400.0f));
            dev.SetTransform(World, Identity());
            dev.SetRenderState(RS_ZENABLE, 1);
            dev.SetRenderState(RS_ZWRITEENABLE, 1);
            dev.SetRenderState(RS_CULLMODE, CULL_NONE);
            {
                // The game's ground passes (one chunk each: the native ground draws its own in their place).
                struct VtxT { float x, y, z, nx, ny, nz, u0, v0, u1, v1; };
                const VtxT chunk[4] = {{kOx - 4, 0, kOz - 4, 0, 1, 0, 0, 0, 0, 0}, {kOx + 4, 0, kOz - 4, 0, 1, 0, 1, 0, 0, 0},
                                       {kOx + 4, 0, kOz + 4, 0, 1, 0, 1, 1, 0, 0}, {kOx - 4, 0, kOz + 4, 0, 1, 0, 0, 1, 0, 0}};
                const uint32_t kFvfT = FVF_XYZ | FVF_NORMAL | (2 << 8);
                Material mat{{1, 1, 1, 1}, {1, 1, 1, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}, 0.0f};
                dev.SetMaterial(mat);
                dev.SetLight(0, sun);
                dev.LightEnable(0, true);
                dev.SetDrawVisual(uint32_t(VisualKind::Terrain), "AnarchyGround_t");
                dev.SetTexture(0, ground->tiles[0]);
                dev.SetRenderState(RS_LIGHTING, 0);
                dev.SetRenderState(RS_ALPHABLENDENABLE, 0);
                dev.SetTextureStageState(0, TSS_COLOROP, TOP_SELECTARG1);
                dev.SetTextureStageState(0, TSS_COLORARG1, TA_TEXTURE);
                dev.DrawIndexedPrimitive(TriangleList, kFvfT, chunk, 4, quadIdx, 6);
                dev.SetRenderState(RS_LIGHTING, 1);
                dev.SetRenderState(RS_AMBIENT, evening ? 0xFF282430 : 0xFF515151);
                dev.SetRenderState(RS_ALPHABLENDENABLE, 1);
                dev.SetRenderState(RS_SRCBLEND, BLEND_ZERO);
                dev.SetRenderState(RS_DESTBLEND, BLEND_SRCCOLOR);
                dev.SetRenderState(RS_ZFUNC, CMP_EQUAL);
                dev.SetTextureStageState(0, TSS_TEXCOORDINDEX, 1);
                dev.SetTextureStageState(0, TSS_COLOROP, TOP_ADD);
                dev.DrawIndexedPrimitive(TriangleList, kFvfT, chunk, 4, quadIdx, 6);
                dev.SetRenderState(RS_ZFUNC, CMP_LESSEQUAL);
                dev.SetRenderState(RS_ALPHABLENDENABLE, 0);
                dev.SetTextureStageState(0, TSS_TEXCOORDINDEX, 0);
                dev.LightEnable(0, false);
            }
            dev.SetTransform(World, streetWorld);
            dev.SetRenderState(RS_LIGHTING, 0);
            dev.SetTextureStageState(0, TSS_COLOROP, TOP_SELECTARG1);
            dev.SetTextureStageState(0, TSS_COLORARG1, TA_TEXTURE);
            dev.SetDrawVisual(uint32_t(VisualKind::Static), "RTriMesh_t");
            dev.SetTexture(0, wall);
            dev.DrawIndexedPrimitive(TriangleList, kFvfMesh, wallQuad, 4, quadIdx, 6);
            dev.SetTexture(0, black);
            dev.DrawIndexedPrimitive(TriangleList, kFvfMesh, door, 4, quadIdx, 6);
            // The street in front of the doorway (behind the camera): boxes the street's capture records, lit by the sun.
            dev.SetTexture(0, box);
            dev.SetRenderState(RS_LIGHTING, 1);
            dev.SetRenderState(RS_AMBIENT, evening ? 0xFF282430 : 0xFF606060);
            dev.SetTextureStageState(0, TSS_COLOROP, TOP_MODULATE);
            dev.SetTextureStageState(0, TSS_COLORARG2, TA_DIFFUSE);
            dev.SetLight(0, sun);
            dev.LightEnable(0, true);
            dev.DrawIndexedPrimitive(TriangleList, kFvfMesh, streetBoxes.data(), uint32_t(streetBoxes.size()), streetBoxIdx.data(),
                                     uint32_t(streetBoxIdx.size()));
            dev.LightEnable(0, false);
            dev.SetRenderState(RS_LIGHTING, 0);
            dev.SetTextureStageState(0, TSS_COLOROP, TOP_SELECTARG1);
            // A solid wall behind the doorway's plane drawn after it (as the building's own insides): hidden by the room.
            const VtxMesh inside[4] = {{-3, 0, 5, 0, 0, -1, 0xFF808080, 0, 0}, {3, 0, 5, 0, 0, -1, 0xFF808080, 1, 0},
                                       {3, 4, 5, 0, 0, -1, 0xFF808080, 1, 1}, {-3, 4, 5, 0, 0, -1, 0xFF808080, 0, 1}};
            dev.SetTexture(0, wall);
            dev.DrawIndexedPrimitive(TriangleList, kFvfMesh, inside, 4, quadIdx, 6);
            // An effect after it, in front of the doorway (as the game's sparks): over the room drawn behind it.
            struct VtxC { float x, y, z; uint32_t argb; };
            const VtxC spark[4] = {{-0.4f, 0.8f, 2.5f, 0x80FF4020}, {0.4f, 0.8f, 2.5f, 0x80FF4020},
                                   {-0.4f, 1.6f, 2.5f, 0x80FF4020}, {0.4f, 1.6f, 2.5f, 0x80FF4020}};
            dev.SetDrawVisual(uint32_t(VisualKind::Effect), "effect");
            dev.SetTexture(0, nullptr);
            dev.SetRenderState(RS_ALPHABLENDENABLE, 1);
            dev.SetRenderState(RS_SRCBLEND, BLEND_SRCALPHA);
            dev.SetRenderState(RS_DESTBLEND, BLEND_INVSRCALPHA);
            dev.SetRenderState(RS_ZWRITEENABLE, 0);
            dev.SetTextureStageState(0, TSS_COLOROP, TOP_SELECTARG2);
            dev.SetTextureStageState(0, TSS_COLORARG2, TA_DIFFUSE);
            dev.SetTextureStageState(0, TSS_ALPHAOP, TOP_SELECTARG2);
            dev.SetTextureStageState(0, TSS_ALPHAARG2, TA_DIFFUSE);
            dev.DrawPrimitive(TriangleStrip, FVF_XYZ | FVF_DIFFUSE, spark, 4);
            dev.SetRenderState(RS_ALPHABLENDENABLE, 0);
            dev.SetRenderState(RS_ZWRITEENABLE, 1);
            dev.SetDrawVisual(0, nullptr);
            dev.EndFrame();
            Sleep(16);
            continue;
        }
        if (lookingBack)
            dev.SetTransform(View, LookAtLH({0.0f, -3.0f, 8.0f}, {0.0f, -3.3f, 0.0f}, {0, 1, 0}));
        else
            dev.SetTransform(View, LookAtLH({0.0f, 1.8f, 0.5f}, {0.0f, 1.65f, 1.5f}, {0, 1, 0}));
        dev.SetTransform(Projection, PerspectiveLH(kPi / 3, float(g_width) / g_height, 0.2f, 400.0f));
        dev.SetTransform(World, Identity());
        dev.SetRenderState(RS_ZENABLE, 1);
        dev.SetRenderState(RS_ZWRITEENABLE, 1);
        dev.SetRenderState(RS_CULLMODE, CULL_NONE);
        dev.SetRenderState(RS_LIGHTING, 1);
        dev.SetRenderState(RS_AMBIENT, 0xFF909090);
        dev.SetRenderState(RS_DIFFUSEMATERIALSOURCE, MCS_MATERIAL);
        Material mat{{1, 1, 1, 1}, {1, 1, 1, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}, 0.0f};
        dev.SetMaterial(mat);
        Light lamp{};
        lamp.type = LIGHT_POINT;
        lamp.diffuse = {1.0f, 0.9f, 0.7f, 1};
        lamp.position = {0.0f, 5.0f, 9.0f};
        lamp.range = 20.0f;
        lamp.attenuation0 = 1.0f;
        dev.SetLight(0, lamp);
        dev.LightEnable(0, true);
        dev.SetTextureStageState(0, TSS_COLOROP, TOP_MODULATE);
        dev.SetTextureStageState(0, TSS_COLORARG1, TA_TEXTURE);
        dev.SetTextureStageState(0, TSS_COLORARG2, TA_DIFFUSE);
        dev.SetTextureStageState(0, TSS_ALPHAOP, TOP_SELECTARG1);
        dev.SetTextureStageState(0, TSS_ALPHAARG1, TA_TEXTURE);
        dev.SetDrawVisual(uint32_t(VisualKind::Room), "VisualRoom_t");
        dev.SetTexture(0, wall);
        dev.DrawIndexedPrimitive(TriangleList, kFvfMesh, room.data(), uint32_t(room.size()), roomIdx.data(),
                                 uint32_t(roomIdx.size()));
        dev.SetDrawVisual(uint32_t(VisualKind::Static), "RTriMesh_t");
        dev.SetTexture(0, box);
        dev.DrawIndexedPrimitive(TriangleList, kFvfMesh, props.data(), uint32_t(props.size()), propIdx.data(),
                                 uint32_t(propIdx.size()));
        // A door in the room's back wall, away from its entrance: recorded with the room and seen from the street.
        {
            const VtxMesh backDoor[4] = {{3.0f, -5.0f, 15.9f, 0, 0, -1, 0xFFFFFFFF, 0, 0}, {5.4f, -5.0f, 15.9f, 0, 0, -1, 0xFFFFFFFF, 1, 0},
                                         {5.4f, -1.8f, 15.9f, 0, 0, -1, 0xFFFFFFFF, 1, 2}, {3.0f, -1.8f, 15.9f, 0, 0, -1, 0xFFFFFFFF, 0, 2}};
            dev.SetDrawVisual(uint32_t(VisualKind::Static), "door_inner");
            dev.SetTexture(0, box);
            dev.DrawIndexedPrimitive(TriangleList, kFvfMesh, backDoor, 4, quadIdx, 6);
            dev.SetDrawVisual(uint32_t(VisualKind::Static), "RTriMesh_t");
            dev.SetTexture(0, box);
        }
        if (lookingBack) {                       // a pillar between the camera and the entrance: the street behind it hidden
            dev.DrawIndexedPrimitive(TriangleList, kFvfMesh, pillar.data(), uint32_t(pillar.size()), pillarIdx.data(),
                                     uint32_t(pillarIdx.size()));
            // The black texture's panel in the entrance's plane (as a shop's, in game): left out while the street shows.
            // As wide as last_ditch_building_medium3's, much more than the door: its corners far from the entrance.
            const VtxMesh panel[4] = {{-6.0f, -5.0f, 0.5f, 0, 0, 1, 0xFFFFFFFF, 0, 0}, {6.0f, -5.0f, 0.5f, 0, 0, 1, 0xFFFFFFFF, 1, 0},
                                      {6.0f, -1.5f, 0.5f, 0, 0, 1, 0xFFFFFFFF, 1, 1}, {-6.0f, -1.5f, 0.5f, 0, 0, 1, 0xFFFFFFFF, 0, 1}};
            dev.SetTexture(0, black);
            dev.DrawIndexedPrimitive(TriangleList, kFvfMesh, panel, 4, quadIdx, 6);
            // The door's leaf, swung 70 degrees open out of the room (hinged at the doorway's right): behind the
            // opening, it stays in front of the street.
            const VtxMesh leaf[4] = {{1.2f, -5.0f, 0.5f, -0.94f, 0, -0.34f, 0xFFFFFFFF, 0, 0},
                                     {0.38f, -5.0f, -1.75f, -0.94f, 0, -0.34f, 0xFFFFFFFF, 2, 0},
                                     {0.38f, -1.9f, -1.75f, -0.94f, 0, -0.34f, 0xFFFFFFFF, 2, 3},
                                     {1.2f, -1.9f, 0.5f, -0.94f, 0, -0.34f, 0xFFFFFFFF, 0, 3}};
            dev.SetDrawVisual(uint32_t(VisualKind::Static), "door_leaf");
            dev.SetTexture(0, wall);
            dev.DrawIndexedPrimitive(TriangleList, kFvfMesh, leaf, 4, quadIdx, 6);
        }
        dev.SetDrawVisual(0, nullptr);
        dev.LightEnable(0, false);
        dev.EndFrame();
        Sleep(16);
    }
    dev.DestroyTexture(wall);
    dev.DestroyTexture(box);
    dev.DestroyTexture(black);
}

// The zone hand-over (--handover-test; handover.cpp, RVK_ZoneFade): a zone change as the game makes one at 60 frames a
// second. 18 frames of a warm scene, 2 of the world already stopped (black, the cubes only - n3EngineClient_t::
// StartTeleport's; its signal comes after the first of them), 40 frames without 3D (a window of the interface only),
// then a blue scene seen from elsewhere (a camera jump). The old scene's picture must stay up over the blank frames and
// the new scene's first ones, then fade into it. Shots (<shot>-<name>.bmp): before (the old zone), hold (the blank
// frames), hidden (the new zone drawn under the held picture), fade, after (the new zone).
bool g_handoverTest = false;

template <typename D>
void RunHandoverTest(D& dev, int frames, const std::string& shot)
{
    auto warmPixels = Checker(64, 8, 0xFFD0A070, 0xFF906040);
    auto bluePixels = Checker(64, 8, 0xFF7090D0, 0xFF405080);
    Texture* warm = dev.CreateTexture(64, 64, warmPixels.data());
    Texture* blue = dev.CreateTexture(64, 64, bluePixels.data());
    std::vector<VtxMesh> cubes;
    std::vector<uint16_t> cubeIdx;
    for (int i = 0; i < 5; ++i)
        AddCube(cubes, cubeIdx, -8.0f + 4.0f * float(i), 1.0f, float(i % 2) * 3.0f, 1.0f);
    frames = std::max(frames, 200);
    const struct { int frame; const char* name; } kShots[] = {{15, "before"}, {45, "hold"}, {70, "hidden"},
                                                               {frames - 1, "after"}};
    int fadeShot = -1;
    for (int frame = 0; frame < frames; ++frame) {
        for (const auto& s : kShots)
            if (frame == s.frame) {
                std::string name = shot;
                name.insert(name.size() - 4, std::string("-") + s.name);
                dev.RequestScreenshot(name);
            }
        if (frame == fadeShot) {
            std::string name = shot;
            name.insert(name.size() - 4, "-fade");
            dev.RequestScreenshot(name);
        }
        if (frame == frames - 1)
            dev.RequestScreenshot(shot);
        if (frame == 19)
            dev.ZoneSignal(rvk::kZoneTeleport);
        const bool stopped = frame >= 18 && frame < 20;
        const bool blank = frame >= 20 && frame < 60;
        const bool second = frame >= 60;
        dev.BeginFrame();
        dev.SetViewport({0, 0, g_width, g_height, 0.0f, 1.0f});
        dev.Clear(CLEAR_TARGET | CLEAR_ZBUFFER, stopped ? 0xFF000000 : second ? 0xFF203050 : 0xFF806040, 1.0f);
        if (!blank) {
            const Vector eye = second ? Vector{14.0f, 9.0f, -16.0f} : Vector{-14.0f, 7.0f, -18.0f};
            dev.SetTransform(View, LookAtLH(eye, {0, 0, 0}, {0, 1, 0}));
            dev.SetTransform(Projection, PerspectiveLH(kPi / 3, float(g_width) / g_height, 0.5f, 600.0f));
            dev.SetTransform(World, Identity());
            dev.SetRenderState(RS_ZENABLE, 1);
            dev.SetRenderState(RS_ZWRITEENABLE, 1);
            dev.SetRenderState(RS_ZFUNC, CMP_LESSEQUAL);
            dev.SetRenderState(RS_CULLMODE, CULL_NONE);
            dev.SetRenderState(RS_ALPHABLENDENABLE, 0);
            dev.SetRenderState(RS_AMBIENT, 0xFF606060);
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
            dev.SetTexture(0, second ? blue : warm);
            dev.SetRenderState(RS_LIGHTING, 1);
            const float r = 60.0f;
            VtxMesh q[4] = {{-r, 0, -r, 0, 1, 0, 0xFFFFFFFF, 0, 0}, {r, 0, -r, 0, 1, 0, 0xFFFFFFFF, 30, 0},
                            {r, 0, r, 0, 1, 0, 0xFFFFFFFF, 30, 30}, {-r, 0, r, 0, 1, 0, 0xFFFFFFFF, 0, 30}};
            if (!stopped)
                dev.DrawPrimitive(TriangleFan, kFvfMesh, q, 4);
            dev.DrawIndexedPrimitive(TriangleList, kFvfMesh, cubes.data(), uint32_t(cubes.size()), cubeIdx.data(),
                                     uint32_t(cubeIdx.size()));
            dev.SetRenderState(RS_LIGHTING, 0);
            dev.LightEnable(0, false);
        }
        // The interface, every frame: a window (its layer), as the game's.
        dev.SetRenderState(RS_ZENABLE, 0);
        dev.SetRenderState(RS_ALPHABLENDENABLE, 1);
        dev.SetRenderState(RS_SRCBLEND, BLEND_SRCALPHA);
        dev.SetRenderState(RS_DESTBLEND, BLEND_INVSRCALPHA);
        dev.SetTexture(0, nullptr);
        dev.SetTextureStageState(0, TSS_COLOROP, TOP_SELECTARG2);
        dev.SetTextureStageState(0, TSS_ALPHAOP, TOP_SELECTARG2);
        dev.SetTextureStageState(0, TSS_ALPHAARG2, TA_DIFFUSE);
        dev.InterfaceBegin(true);
        VtxRhwDiffuse window[4] = {{20, 20, 0, 1, 0xE0304060}, {220, 20, 0, 1, 0xE0304060},
                                   {20, 90, 0, 1, 0xE0304060}, {220, 90, 0, 1, 0xE0304060}};
        dev.DrawPrimitive(TriangleStrip, kFvfRhwDiffuse, window, 4);
        dev.InterfaceEnd();
        dev.SetRenderState(RS_ALPHABLENDENABLE, 0);
        dev.SetRenderState(RS_ZENABLE, 1);
        dev.EndFrame();
        if (fadeShot < 0 && dev.HandoverFading())
            fadeShot = frame + 15;               // (about half way through the half-second fade)
        Sleep(16);
    }
    dev.DestroyTexture(warm);
    dev.DestroyTexture(blue);
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
    auto windowPixels = Checker(64, 16, 0xFFFFD890, 0xFF1A1C24);
    Texture* windows = dev.CreateTexture(64, 64, windowPixels.data());
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
        dev.SetViewport({0, 0, g_width, g_height, 0.0f, 1.0f});
        dev.Clear(CLEAR_TARGET | CLEAR_ZBUFFER, 0xFF101828, 1.0f);
        dev.SetTransform(View, LookAtLH({g_off[0], g_off[1] + 13, g_off[2] - 11}, {g_off[0], g_off[1], g_off[2] + 1.5f}, {0, 1, 0}));
        dev.SetTransform(Projection, PerspectiveLH(kPi / 3, float(g_width) / g_height, 0.5f, 200.0f));
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
        if (g_spotDegrees > 0.0f) {            // --point-shadow-spot DEG: a spot light looking down, its cone DEG across
            lamp.type = LIGHT_SPOT;
            lamp.direction = {0.05f, -1.0f, 0.03f};
            lamp.phi = g_spotDegrees * kPi / 180.0f;
            lamp.theta = 0.7f * lamp.phi;
            lamp.falloff = 1.0f;
            lamp.position.y += 1.5f;
        }
        lamp.position = {lamp.position.x + g_off[0], lamp.position.y + g_off[1], lamp.position.z + g_off[2]};
        // --lamp-wobble: the position moves in its last bits from frame to frame, as the game's lamps do.
        float wobble = g_lampWobble && (frame & 1) ? 1e-4f : 0.0f;
        lamp.position.x += wobble;
        dev.SetLight(0, lamp);
        dev.LightEnable(0, true);
        if (g_twoLamps) {                      // --two-lamps: a second shadowed lamp (cube slots skipping frames)
            Light lamp2 = lamp;
            lamp2.position = {-4.0f + g_off[0] + wobble, 1.5f + g_off[1], 5.0f + g_off[2]};
            lamp2.diffuse = {0.5f, 0.7f, 1.0f, 1};
            dev.SetLight(2, lamp2);
            dev.LightEnable(2, true);
        }
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
        if (g_nightSign) {
            // A building's lit windows: an unlit, textured, opaque quad standing behind the scene.
            struct VtxSign { float x, y, z; uint32_t c; float u, v; };
            VtxSign q[4] = {{-7, 0, 9, 0xFFFFFFFF, 0, 1}, {-1, 0, 9, 0xFFFFFFFF, 3, 1},
                            {-1, 4, 9, 0xFFFFFFFF, 3, 0}, {-7, 4, 9, 0xFFFFFFFF, 0, 0}};
            dev.SetRenderState(RS_LIGHTING, 0);
            dev.SetTexture(0, windows);
            dev.SetTextureStageState(0, TSS_COLOROP, TOP_MODULATE);
            dev.DrawPrimitive(TriangleFan, FVF_XYZ | FVF_DIFFUSE | (1 << 8), q, 4);
            dev.SetRenderState(RS_LIGHTING, 1);
        }
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
    dev.DestroyTexture(windows);
}

// --leaf-tree A.tree[,B.tree...]: real trees (tools/extract-tree.py: each part in the game's space, its texture, how
// it blends) standing in a row on a lit ground, drawn as the game draws them - the canopy blended with depth writes,
// the trunk opaque - for the leaves (leaves.cpp). The camera stands back to see them whole.
std::string g_leafTrees;           // --leaf-tree: the files, comma-separated
Device::LeafSettings g_leafSettings{true};   // --leaf-off, --leaf-density, --leaf-size, --leaf-dist, --leaf-core, --leaf-wind, --leaf-flutter
int g_folEdges = -1;               // --fol-edges 0|1: RVK_FolEdges, plants' soft edges in a second pass (-1: the default)
int g_prepassEq = -1;              // --prepass-eq 0|1: RVK_PrepassEq, pre-passed draws shaded only where their depth is
float g_folDither = -1.0f;         // --fol-dither D: RVK_FolDither, soft edges dithered past D units (0 never; -1: default)
float g_leafCamDist = -1.0f;       // --leaf-cam-dist D: the camera this far from the row (default: from the trees' size)
float g_leafCamHeight = -1.0f;     // --leaf-cam-height H: ... this high (default: a third of the tallest tree)
float g_leafCamYaw = 0.0f;         // --leaf-cam-yaw R: ... turned R radians about the row's middle
float g_leafCamTurn = 0.0f;        // --leaf-cam-turn R: ... looking to and fro, R radians either way (a turn in 2 s)
float g_leafCamOrbit = 0.0f;       // --leaf-cam-orbit R: ... going round the row's middle to and fro, R radians either way
int g_leafFilm = 0;                // --leaf-film N: ~60 frames a second, a screenshot every N frames (film_NNN.bmp)
int g_frameGenToggle = 0;          // --frame-gen-toggle N: frame generation off at frame N, on again at 2N (the leaf
                                   // scenes; the swapchain made again each time)
int g_fgBlankFrom = 0, g_fgHudAt = 0, g_fgHudEnd = 0;   // --fg-loading A H E: from frame A no 3D (a loading screen),
                                        // frames H to E the perf readout's image, redrawn every 15 frames (the leaf
                                        // scenes, with --window --frame-gen: the readout switched on at a loading screen
                                        // hung FSR 3's swapchain)
bool g_leafPond = false;           // --leaf-pond: a pond in front of the trees, drawn before them (as the game draws a
                                   // pond's water before most of a forest: the depth pre-pass's second segment)
int g_leafForest = 0;              // --leaf-forest N: an N x N grid of the trees (12 units apart), seen from eye height
float g_crownShade = 1.0f;         // --crown-shade X: RVK_CrownShade
bool g_freeCam = false;            // --free-cam: the free camera on from frame 10, turning and moving ahead (Device::SetFreeCam)
std::string g_leafArt;             // --leaf-art ATLAS.png[,NORMAL.png]: a tree type's own leaf art (ao-assets leaf-art)
                                   // on every canopy (cut-out part) of the trees - leaves.cpp BakeOwnCanopy

struct TreePart {
    uint32_t flags = 0, alphaRef = 128;
    Matrix world;                  // the part's own (version 2: its node in the mesh; else the identity)
    Texture* texture = nullptr;
    std::vector<float> vertices;   // x y z, nx ny nz, u v
    std::vector<uint16_t> indices;
};
struct Tree {
    std::vector<TreePart> parts;
    float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
};

template <typename D>
bool LoadTree(D& dev, const std::string& path, Tree& tree)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { std::printf("leaf tree: can't open %s\n", path.c_str()); return false; }
    std::vector<uint8_t> data;
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) data.insert(data.end(), buf, buf + n);
    std::fclose(f);
    size_t off = 0;
    auto u32 = [&]() { uint32_t v = 0; if (off + 4 <= data.size()) std::memcpy(&v, &data[off], 4); off += 4; return v; };
    if (data.size() < 12 || std::memcmp(data.data(), "RVKT", 4) != 0) { std::printf("leaf tree: %s is no tree\n", path.c_str()); return false; }
    off = 4;
    const uint32_t version = u32();
    const uint32_t parts = u32();
    for (uint32_t p = 0; p < parts; ++p) {
        TreePart part;
        part.flags = u32();
        part.alphaRef = u32();
        part.world = Identity();
        if (version >= 2) {
            std::memcpy(&part.world, &data[off], 64);
            off += 64;
        }
        const uint32_t w = u32(), h = u32();
        if (off + size_t(w) * h * 4 > data.size()) return false;
        part.texture = dev.CreateTexture(w, h, reinterpret_cast<const uint32_t*>(&data[off]));
        off += size_t(w) * h * 4;
        const uint32_t nv = u32();
        part.vertices.resize(size_t(nv) * 8);
        std::memcpy(part.vertices.data(), &data[off], size_t(nv) * 32);
        off += size_t(nv) * 32;
        const uint32_t ni = u32();
        part.indices.resize(ni);
        std::memcpy(part.indices.data(), &data[off], size_t(ni) * 2);
        off += size_t(ni) * 2 + (ni % 2 ? 2 : 0);
        for (uint32_t v = 0; v < nv; ++v) {
            const float* p = &part.vertices[size_t(v) * 8];
            for (int k = 0; k < 3; ++k) {
                const float x = p[0] * part.world.m[0][k] + p[1] * part.world.m[1][k] + p[2] * part.world.m[2][k] +
                                part.world.m[3][k];
                tree.lo[k] = std::min(tree.lo[k], x);
                tree.hi[k] = std::max(tree.hi[k], x);
            }
        }
        tree.parts.push_back(std::move(part));
    }
    std::printf("leaf tree: %s, %u parts, %.1f units tall\n", path.c_str(), parts, tree.hi[1] - tree.lo[1]);
    return true;
}

template <typename D>
void RunLeafScene(D& dev, int frames, const std::string& shot, const std::string& dump)
{
    std::vector<Tree> trees;
    for (size_t start = 0; start < g_leafTrees.size();) {
        size_t comma = g_leafTrees.find(',', start);
        std::string path = g_leafTrees.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        Tree t;
        if (!path.empty() && LoadTree(dev, path, t)) trees.push_back(std::move(t));
        if (!trees.empty() && !g_leafArt.empty() && path.size()) {   // (on this tree's canopies)
            size_t c = g_leafArt.find(',');
            rvk::maps::MipChain atlas, normal;
            rvk::maps::NormalSpread spread;
            std::string err;
            if (!rvk::maps::DecodeAlbedoMap(g_leafArt.substr(0, c), &atlas, &err))
                std::printf("leaf art: %s\n", err.c_str());
            if (c != std::string::npos && !rvk::maps::DecodeNormalMap(g_leafArt.substr(c + 1), &normal, &spread, &err))
                std::printf("leaf art: %s\n", err.c_str());
            for (TreePart& part : trees.back().parts)
                if (part.flags & 3)
                    dev.SetLeafArt(part.texture, rvk::maps::Upload(dev, atlas), rvk::maps::Upload(dev, normal));
        }
        start = comma == std::string::npos ? g_leafTrees.size() : comma + 1;
    }
    if (trees.empty()) return;
    // In a row along x, each on the ground (y 0) at its own box's middle; or (--leaf-forest) a grid of them.
    struct Placed { size_t tree; float x, z, turn; };
    std::vector<Placed> placed;
    float x = 0.0f, tallest = 0.0f;
    for (size_t i = 0; i < trees.size(); ++i)
        tallest = std::max(tallest, trees[i].hi[1] - trees[i].lo[1]);
    if (g_leafForest > 0) {
        for (int gz = 0; gz < g_leafForest; ++gz)
            for (int gx = 0; gx < g_leafForest; ++gx)
                placed.push_back({size_t(gx * 7 + gz * 3) % trees.size(), 12.0f * float(gx) + 3.0f * float(gz % 2),
                                  12.0f * float(gz), 1.3f * float(gx + 5 * gz)});
        x = 12.0f * float(g_leafForest - 1);
    } else {
        for (size_t i = 0; i < trees.size(); ++i) {
            const float width = trees[i].hi[0] - trees[i].lo[0];
            placed.push_back({i, x + 0.5f * width, 0.0f, 0.0f});
            x += width * 0.9f;
        }
    }
    const float mid = 0.5f * x;
    const float dist = g_leafCamDist > 0.0f ? g_leafCamDist
                       : g_leafForest > 0 ? 14.0f : std::max(1.1f * tallest, 0.75f * x);
    const float camY = g_leafCamHeight >= 0.0f ? g_leafCamHeight : g_leafForest > 0 ? 2.0f : tallest / 3.0f;
    auto groundPixels = Checker(64, 8, 0xFF4A6A30, 0xFF42602A);
    Texture* ground = dev.CreateTexture(64, 64, groundPixels.data());
    dev.SetLeaves(g_leafSettings);
    dev.SetKnob(rvk::Knob::CrownShade, g_crownShade);
    if (g_folEdges >= 0) dev.SetKnob(rvk::Knob::FolEdges, float(g_folEdges));
    if (g_folDither >= 0.0f) dev.SetKnob(rvk::Knob::FolDither, g_folDither);
    if (g_prepassEq >= 0) dev.SetKnob(rvk::Knob::PrepassEq, float(g_prepassEq));
    if (g_pbrDebug) {                            // (--pbr-debug 10: the two-pass foliage's parts in colour)
        Device::PbrSettings pbr;
        pbr.enabled = true;
        pbr.debug = g_pbrDebug;
        dev.SetPbr(pbr);
    }
    for (int frame = 0; frame < frames; ++frame) {
        if (g_frameGenToggle > 0 && (frame == g_frameGenToggle || frame == 2 * g_frameGenToggle))
            dev.SetFrameGen(frame == 2 * g_frameGenToggle);
        if (frame == frames - 1) {
            dev.RequestScreenshot(shot);
            if (!dump.empty()) dev.RequestFrameDump(dump);   // the last frame: the leaves are baked by then
        } else if (g_leafFilm > 0 && frame >= 30 && frame % g_leafFilm == 0) {
            char name[32];
            std::snprintf(name, sizeof(name), "film_%03d.bmp", (frame - 30) / g_leafFilm);
            dev.RequestScreenshot(name);
        }
        if (g_leafFilm > 0)
            Sleep(16);
        dev.BeginFrame();
        dev.SetViewport({0, 0, g_width, g_height, 0.0f, 1.0f});
        dev.Clear(CLEAR_TARGET | CLEAR_ZBUFFER, 0xFF8AB0D8, 1.0f);
        if (g_fgHudEnd > 0 && frame == g_fgHudEnd)
            dev.SetConsoleImage(0, 0, nullptr);
        else if (g_fgHudAt > 0 && frame >= g_fgHudAt && (frame < g_fgHudEnd || !g_fgHudEnd) && (frame - g_fgHudAt) % 15 == 0) {
            std::vector<uint32_t> hud(size_t(g_width) * 120, (frame / 15) % 2 ? 0xB0101820u : 0xB0201008u);
            dev.SetConsoleImage(g_width, 120, hud.data());
        }
        if (g_fgBlankFrom > 0 && frame >= g_fgBlankFrom) {
            dev.EndFrame();
            if (frame % 30 == 0) std::printf("fg-loading: frame %d\n", frame);
            continue;
        }
        const float yaw = g_leafCamYaw + g_leafCamOrbit * std::sin(float(frame) * kPi / 60.0f);
        const float ex = mid + dist * std::sin(yaw), ez = -dist * std::cos(yaw);
        const float lookY = g_leafForest > 0 ? camY + 3.0f : tallest * 0.45f;
        const float lookZ = g_leafForest > 0 ? 30.0f : 0.0f;
        if (g_leafCamTurn != 0.0f) {             // (turning in place: the look's point swung round the eye)
            const float a = g_leafCamTurn * std::sin(float(frame) * kPi / 60.0f), dx = mid - ex, dz = lookZ - ez;
            dev.SetTransform(View, LookAtLH({ex, camY, ez}, {ex + dx * std::cos(a) - dz * std::sin(a), lookY,
                                                             ez + dx * std::sin(a) + dz * std::cos(a)}, {0, 1, 0}));
        } else {
            dev.SetTransform(View, LookAtLH({ex, camY, ez}, {mid, lookY, lookZ}, {0, 1, 0}));
        }
        if (g_freeCam && frame >= 10) {
            Device::FreeCamInput in;
            in.on = true;
            in.look[0] = 0.01f;
            in.look[1] = 0.004f;
            in.move[2] = 1.0f;
            in.dt = 1.0f / 60.0f;
            dev.SetFreeCam(in);
        }
        dev.SetTransform(Projection, PerspectiveLH(kPi / 3, float(g_width) / g_height, 0.5f, 500.0f));
        dev.SetTransform(World, Identity());
        dev.SetRenderState(RS_ZENABLE, 1);
        dev.SetRenderState(RS_ZWRITEENABLE, 1);
        dev.SetRenderState(RS_ZFUNC, CMP_LESSEQUAL);
        dev.SetRenderState(RS_CULLMODE, CULL_NONE);
        dev.SetRenderState(RS_ALPHABLENDENABLE, 0);
        dev.SetRenderState(RS_ALPHATESTENABLE, 0);
        dev.SetRenderState(RS_AMBIENT, 0xFF606060);
        dev.SetRenderState(RS_LIGHTING, 1);
        dev.SetRenderState(RS_DIFFUSEMATERIALSOURCE, MCS_MATERIAL);
        Material mat{{1, 1, 1, 1}, {1, 1, 1, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}, 0.0f};
        dev.SetMaterial(mat);
        Light sun{};
        sun.type = LIGHT_DIRECTIONAL;
        sun.diffuse = {0.9f, 0.85f, 0.75f, 1};
        sun.direction = {0.45f, -0.75f, 0.5f};
        if (g_sunDirSet) sun.direction = {g_sunDir[0], g_sunDir[1], g_sunDir[2]};   // (--sun-dir)
        dev.SetLight(0, sun);
        dev.LightEnable(0, true);
        dev.SetTextureStageState(0, TSS_COLOROP, TOP_MODULATE);
        dev.SetTextureStageState(0, TSS_COLORARG1, TA_TEXTURE);
        dev.SetTextureStageState(0, TSS_COLORARG2, TA_DIFFUSE);
        dev.SetTextureStageState(0, TSS_ALPHAOP, TOP_SELECTARG1);
        dev.SetTextureStageState(0, TSS_ALPHAARG1, TA_TEXTURE);
        dev.SetTexture(0, ground);
        {
            const float r = 200.0f;
            VtxMesh q[4] = {{-r + mid, 0, -r, 0, 1, 0, 0xFFFFFFFF, 0, 0}, {r + mid, 0, -r, 0, 1, 0, 0xFFFFFFFF, 40, 0},
                            {r + mid, 0, r, 0, 1, 0, 0xFFFFFFFF, 40, 40}, {-r + mid, 0, r, 0, 1, 0, 0xFFFFFFFF, 0, 40}};
            dev.DrawPrimitive(TriangleFan, kFvfMesh, q, 4);
        }
        if (g_leafPond) {
            const uint32_t tint = 0xCC00186A;
            const Device::WaterVertex pond[4] = {{{mid - 12, 0.2f, -11}, tint}, {{mid + 12, 0.2f, -11}, tint},
                                                 {{mid + 12, 0.2f, -2}, tint}, {{mid - 12, 0.2f, -2}, tint}};
            const uint16_t pondIdx[6] = {0, 1, 2, 0, 2, 3};
            dev.SetRenderState(RS_LIGHTING, 0);
            dev.SetRenderState(RS_ALPHABLENDENABLE, 1);
            dev.SetRenderState(RS_SRCBLEND, BLEND_SRCALPHA);
            dev.SetRenderState(RS_DESTBLEND, BLEND_INVSRCALPHA);
            dev.SetRenderState(RS_ZWRITEENABLE, 0);
            dev.SetTexture(0, nullptr);
            dev.DrawWater(pond, 4, pondIdx, 6);
            dev.SetRenderState(RS_LIGHTING, 1);
            dev.SetRenderState(RS_ALPHABLENDENABLE, 0);
            dev.SetRenderState(RS_ZWRITEENABLE, 1);
        }
        const uint32_t kFvfTree = FVF_XYZ | FVF_NORMAL | (1 << 8);   // 0x112, as the game's statics
        for (const Placed& pl : placed) {
            const Tree& tree = trees[pl.tree];
            // (The model's origin is its foot on the ground, as the game places it; the trunk reaches a little below.)
            Matrix w = Mul(Translate(-0.5f * (tree.lo[0] + tree.hi[0]), 0.0f, -0.5f * (tree.lo[2] + tree.hi[2])),
                           Mul(RotateY(pl.turn), Translate(pl.x, 0.0f, pl.z)));
            for (const TreePart& part : tree.parts) {
                dev.SetTransform(World, Mul(part.world, w));
                dev.SetTexture(0, part.texture);
                dev.SetRenderState(RS_ALPHABLENDENABLE, (part.flags & 1) ? 1 : 0);
                dev.SetRenderState(RS_SRCBLEND, BLEND_SRCALPHA);
                dev.SetRenderState(RS_DESTBLEND, BLEND_INVSRCALPHA);
                dev.SetRenderState(RS_ALPHATESTENABLE, (part.flags & 2) ? 1 : 0);
                dev.SetRenderState(RS_ALPHAREF, part.alphaRef);
                dev.SetRenderState(RS_ALPHAFUNC, CMP_GREATEREQUAL);
                dev.DrawIndexedPrimitive(TriangleList, kFvfTree, part.vertices.data(),
                                         uint32_t(part.vertices.size() / 8), part.indices.data(),
                                         uint32_t(part.indices.size()));
            }
        }
        dev.SetRenderState(RS_ALPHABLENDENABLE, 0);
        dev.SetRenderState(RS_ALPHATESTENABLE, 0);
        dev.SetTransform(World, Identity());
        dev.EndFrame();
    }
    for (Tree& t : trees)
        for (TreePart& part : t.parts) dev.DestroyTexture(part.texture);
    dev.DestroyTexture(ground);
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
uint32_t g_particleColor = 0xC0B080FF;   // --particle-color AARRGGBB: the sprites' colour
float g_particleUniform = 0.0f;          // --particle-uniform U: same size for every effect (0..1)
float g_spriteScale = 1.0f;              // --sprite-scale X: the effect's sprite size
bool g_particleWater = false;            // --particle-water: water the game's way (ProcessVertices output, FVF 0x1C4) mid-scene

// Particle test (--particle-test): an effect like the game's sparkle auras - a ring of soft additive sprites (FVF 0x142,
// Our own effects (--fx FILE.rvkfx; rvk fx.cpp): a cast played over and over - charged at the caster's hand for 0.5 s,
// flying to the target in an arc for 0.8 s, its impact for 0.7 s, then 0.6 s of nothing (its particles living out) -
// the effect announced every frame as the proxy does for the game's Spell1. Shots of the first cast: -charge (0.3 s),
// -flight (0.9 s), -impact (1.5 s), and the last frame's as --shot. --fx-still: an effect on a character instead (a
// sparkle: its one phase, "play", for 2 s at the middle post's top, then 0.6 s of nothing).
bool g_fxStill = false;

template <typename D>
void RunFxTest(D& dev, int frames, const std::string& file, const std::string& shot)
{
    std::string text;
    if (FILE* f = std::fopen(file.c_str(), "rb")) {
        char buf[4096];
        for (size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) > 0;) text.append(buf, n);
        std::fclose(f);
    }
    auto effect = std::make_shared<rvk::fx::Effect>();
    std::string error;
    if (!rvk::fx::Parse(text, *effect, &error)) {
        std::printf("fx: %s: %s\n", file.c_str(), text.empty() ? "can't read it" : error.c_str());
        return;
    }
    std::printf("fx: %s: \"%s\", %zu emitters\n", file.c_str(), effect->name.c_str(), effect->emitters.size());
    dev.FxSetEffect(1, effect);
    struct VtxGround { float x, y, z; uint32_t color; };
    const float hand[3] = {-3.0f, 1.4f, 0.0f}, chest[3] = {3.0f, 1.1f, 0.0f};
    auto named = [&](const char* tag) {
        std::string n = shot;
        n.insert(n.size() > 4 ? n.size() - 4 : n.size(), tag);
        return n;
    };
    for (int frame = 0; frame < frames; ++frame) {
        if (frame == 19) dev.RequestScreenshot(named("-charge"));
        if (frame == 56) dev.RequestScreenshot(named("-flight"));
        if (frame == 94) dev.RequestScreenshot(named("-impact"));
        if (frame == frames - 1) dev.RequestScreenshot(shot);
        dev.BeginFrame();
        dev.SetViewport({0, 0, g_width, g_height, 0.0f, 1.0f});
        dev.Clear(CLEAR_TARGET | CLEAR_ZBUFFER, 0xFF0A0E16, 1.0f);
        dev.SetTransform(View, LookAtLH({0, 2.6f, -7.5f}, {0, 1.0f, 0}, {0, 1, 0}));
        dev.SetTransform(Projection, PerspectiveLH(kPi / 3, float(g_width) / g_height, 0.1f, 100.0f));
        dev.SetTransform(World, Identity());
        dev.SetRenderState(RS_LIGHTING, 0);
        dev.SetRenderState(RS_CULLMODE, CULL_NONE);
        dev.SetRenderState(RS_ZENABLE, 1);
        dev.SetRenderState(RS_ZWRITEENABLE, 1);
        dev.SetRenderState(RS_ZFUNC, CMP_LESSEQUAL);
        dev.SetRenderState(RS_ALPHABLENDENABLE, 0);
        dev.SetTexture(0, nullptr);
        dev.SetTextureStageState(0, TSS_COLOROP, TOP_SELECTARG1);
        dev.SetTextureStageState(0, TSS_COLORARG1, TA_DIFFUSE);
        dev.SetTextureStageState(0, TSS_ALPHAOP, TOP_SELECTARG1);
        dev.SetTextureStageState(0, TSS_ALPHAARG1, TA_DIFFUSE);
        // The ground, and a post for each of the caster and the target.
        std::vector<VtxGround> v = {{-8, 0, -4, 0xFF2A2E38}, {8, 0, -4, 0xFF2A2E38}, {-8, 0, 6, 0xFF1C2028}, {8, 0, 6, 0xFF1C2028}};
        std::vector<uint16_t> idx = {0, 1, 2, 1, 2, 3};
        for (float x : {hand[0], chest[0], 0.0f}) {
            if (x == 0.0f && !g_fxStill) continue;
            const uint16_t b = uint16_t(v.size());
            v.push_back({x - 0.25f, 0.0f, 0.0f, 0xFF505868});
            v.push_back({x + 0.25f, 0.0f, 0.0f, 0xFF505868});
            v.push_back({x - 0.25f, 1.8f, 0.0f, 0xFF606878});
            v.push_back({x + 0.25f, 1.8f, 0.0f, 0xFF606878});
            for (uint16_t k : {0, 1, 2, 1, 2, 3}) idx.push_back(uint16_t(b + k));
        }
        dev.DrawIndexedPrimitive(TriangleList, FVF_XYZ | FVF_DIFFUSE, v.data(), uint32_t(v.size()), idx.data(),
                                 uint32_t(idx.size()));
        // The cast: where its head is, and which phase it is in.
        const float t = std::fmod(float(frame) * 0.016f, 2.6f);
        const uint64_t key = uint64_t(float(frame) * 0.016f / 2.6f) + 1;
        if (t < 2.0f && g_fxStill) {
            Device::FxAnchors a;
            const float at[3] = {0.0f, 1.0f, 0.0f};
            std::memcpy(a.head, at, sizeof(at));
            std::memcpy(a.source, at, sizeof(at));
            std::memcpy(a.target, at, sizeof(at));
            a.phase = rvk::fx::kFlight;
            dev.FxInstance(key, 1, a);
        } else if (t < 2.0f) {
            Device::FxAnchors a;
            std::memcpy(a.source, hand, sizeof(hand));
            std::memcpy(a.target, chest, sizeof(chest));
            if (t < 0.5f) {
                a.phase = rvk::fx::kCharge;
                std::memcpy(a.head, hand, sizeof(hand));
            } else if (t < 1.3f) {
                a.phase = rvk::fx::kFlight;
                const float s = (t - 0.5f) / 0.8f;
                for (int i = 0; i < 3; ++i) a.head[i] = hand[i] + (chest[i] - hand[i]) * s;
                a.head[1] += std::sin(s * kPi) * 0.6f;
            } else {
                a.phase = rvk::fx::kImpact;
                std::memcpy(a.head, chest, sizeof(chest));
            }
            dev.FxInstance(key, 1, a);
        }
        dev.EndFrame();
        Sleep(16);
    }
}

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
    params.uniformSize = g_particleUniform;
    params.fixedSize = 0.09f;
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
        dev.SetViewport({0, 0, g_width, g_height, 0.0f, 1.0f});
        dev.Clear(CLEAR_TARGET | CLEAR_ZBUFFER, 0xFF0C1018, 1.0f);
        Matrix view = LookAtLH({0, 2.2f, -4.5f}, {0, 0.9f, 0}, {0, 1, 0});
        dev.SetTransform(View, view);
        dev.SetTransform(Projection, PerspectiveLH(kPi / 3, float(g_width) / g_height, 0.1f, 100.0f));
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
                {0, 700, 0.9f, 1, 0x80204060, 0, 0, 0}, {float(g_width), 700, 0.9f, 1, 0x80204060, 0, 1, 0},
                {0, float(g_height), 0.9f, 1, 0x80204060, 0, 0, 1}, {float(g_width), float(g_height), 0.9f, 1, 0x80204060, 0, 1, 1}};
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
            sp.size = 0.6f * g_spriteScale;
            sp.uv[0] = 0; sp.uv[1] = 0; sp.uv[2] = 1; sp.uv[3] = 1;
            sp.color = g_particleColor;
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

// A crash prints a line and exits (3): no Wine crash dialog on the desktop.
LONG WINAPI Crashed(EXCEPTION_POINTERS* e)
{
    std::printf("rvk_demo crashed: exception %08lx at %p\n", (unsigned long)e->ExceptionRecord->ExceptionCode,
                e->ExceptionRecord->ExceptionAddress);
    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 3);
    return EXCEPTION_EXECUTE_HANDLER;
}

int main(int argc, char** argv)
{
    SetUnhandledExceptionFilter(&Crashed);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    bool windowed = false, stress = false, threaded = false, pixelLighting = false, lightingDebug = false, lightOverride = false, shadows = false, shadowTest = false, pointShadowTest = false, pointShadowSun = false;
    int cacheTest = 0, frameMs = 0;
    bool particleTest = false, particlesOff = false;
    std::string fxFile;
    int particleEnd = -1, particleEffects = 1;
    uint32_t pointShadows = 4;
    float headroom = 1.0f;
    double fadeIn = 0.0;
    bool hdr = false;
    float bloom = 0.0f, effectGlow = 1.0f, ao = 0.0f, aoRadius = 1.5f, gi = 0.0f, giRadius = 4.0f, volume = 0.0f, volHaze = 1.0f, volShafts = 1.0f, ssr = 0.0f, ssrWet = 0.0f, bloomOcc = 0.15f, neonSat = 0.0f, bloomWide = 0.0f, sunSoft = 0.0f, leaf = 0.0f, nightGlow = 0.0f, contact = 0.0f, sway = 0.0f, bump = 0.0f, grassPush = 1.0f, plantDetail = 1.0f, foliageLod = 35.0f;
    std::vector<std::pair<rvk::Knob, float>> knobs;   // --knob
    int sunRes = 4096, ptRes = 1024;
    float ptLight = 1.0f, tess = 0.0f;
    int tessLevel = 4;
    float saturation = 1.0f, contrast = 1.0f, warmth = 0.0f, nightTint = 0.0f, vignette = 0.0f;
    bool lutSepia = false, taa = false, fxaa = false;
    float sharpen = 0.4f;
    int renderScale = 100;                       // --render-scale N: RVK_RenderScale (needs --hdr --taa)
    int upscaler = 0;                            // --upscaler N: RVK_Upscaler (1: FSR 2, 2: FSR 3)
    bool frameGen = false;                       // --frame-gen: RVK_FrameGen (needs --window)
    bool normalMapTest = false, pbrTest = false, glowOccludeTest = false;
    uint32_t anisotropy = 1;
    float motionBlur = 0.0f, dof = 0.0f, dofFocus = 0.0f;
    bool dofBokeh = true, dofFar = true;
    uint32_t motionMode = 0;
    float knee = 0.85f;
    int bench = 0;
    bool cityBench = false;
    int frames = 3;
    std::string shot = "rvk_demo.bmp", dump;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--window") windowed = true;
        else if (a == "--stress") stress = true;
        else if (a == "--threaded") threaded = true;
        else if (a == "--pixel-lighting") pixelLighting = true;
        else if (a == "--lighting-debug") lightingDebug = true;
        else if (a == "--hide-ui") g_hideUi = true;
        else if (a == "--handover-test") { g_handoverTest = true; hdr = true; }
        else if (a == "--portal-view" && i + 1 < argc) { g_portalView = argv[++i]; hdr = true; }
        else if (a == "--portal-capture-test" && i + 1 < argc) { g_portalCaptureDir = argv[++i]; hdr = true; }
        else if (a == "--portal-cam" && i + 4 < argc) { for (float& x : g_portalCam) x = float(std::atof(argv[++i])); }
        else if (a == "--light-override") lightOverride = true;
        else if (a == "--shadows") shadows = true;
        else if (a == "--shadow-test") shadowTest = shadows = true;
        else if (a == "--texture-flood" && i + 1 < argc) g_textureFlood = std::atoi(argv[++i]);
        else if (a == "--statel-test" && i + 5 < argc) {
            g_statelPack = argv[++i];
            g_statelPlayfield = uint32_t(std::atoi(argv[++i]));
            for (int k = 0; k < 3; ++k) g_statelAt[k] = float(std::atof(argv[++i]));
            shadows = true;
        }
        else if (a == "--statel-mode" && i + 1 < argc) g_statelMode = uint32_t(std::atoi(argv[++i]));
        else if (a == "--statel-skinned") g_statelSkinned = true;
        else if (a == "--statel-movers") g_statelMovers = true;
        else if (a == "--shot-run" && i + 1 < argc) g_shotRun = std::atoi(argv[++i]);
        else if (a == "--point-shadow-test") pointShadowTest = true;
        else if (a == "--point-shadow-spot" && i + 1 < argc) g_spotDegrees = float(std::atof(argv[++i]));
        else if (a == "--world-offset" && i + 3 < argc) { for (int k = 0; k < 3; ++k) g_off[k] = float(std::atof(argv[++i])); }
        else if (a == "--point-shadow-slab") g_slabTest = pointShadowTest = pointShadowSun = shadows = true;
        else if (a == "--point-shadow-sun") pointShadowTest = pointShadowSun = shadows = true;
        else if (a == "--frame-ms" && i + 1 < argc) frameMs = std::atoi(argv[++i]);
        else if (a == "--size" && i + 2 < argc) { g_width = uint32_t(std::atoi(argv[++i])); g_height = uint32_t(std::atoi(argv[++i])); }
        else if (a == "--clouds") { g_clouds = true; hdr = true; }
        else if (a == "--mesh-ids" && i + 1 < argc) g_meshIds = argv[++i];
        else if (a == "--mesh-piece" && i + 1 < argc) g_meshPieces.push_back(argv[++i]);
        else if (a == "--clouds-game") { g_clouds = g_cloudsGame = true; hdr = true; }
        else if (a == "--clouds-full") g_cloudsHalf = false;
        else if (a == "--cloud-puffs") g_cloudLayer = false;
        else if (a == "--sun-dir" && i + 3 < argc) { for (float& x : g_sunDir) x = float(std::atof(argv[++i])); g_sunDirSet = true; }
        else if (a == "--sun-sweep" && i + 3 < argc) {
            g_sweepFrom = float(std::atof(argv[++i]));
            g_sweepTo = float(std::atof(argv[++i]));
            g_sweepSteps = std::atoi(argv[++i]);
        }
        else if (a == "--sweep-hold" && i + 1 < argc) g_sweepHold = std::max(std::atoi(argv[++i]), 1);
        else if (a == "--sun-colour" && i + 3 < argc) { for (float& x : g_sunColour) x = float(std::atof(argv[++i])); g_sunColourSet = true; }
        else if (a == "--cloud-cover" && i + 1 < argc) g_cloudCover = float(std::atof(argv[++i]));
        else if (a == "--cloud-thick" && i + 1 < argc) g_cloudThick = float(std::atof(argv[++i]));
        else if (a == "--cloud-shade" && i + 1 < argc) g_cloudShade = float(std::atof(argv[++i]));
        else if (a == "--cloud-height" && i + 1 < argc) g_cloudHeight = float(std::atof(argv[++i]));
        else if (a == "--cloud-low" && i + 1 < argc) g_cloudLow = float(std::atof(argv[++i]));
        else if (a == "--cloud-qual" && i + 1 < argc) g_cloudQual = std::atoi(argv[++i]);
        else if (a == "--cloud-sheet" && i + 1 < argc) g_cloudSheet = float(std::atof(argv[++i]));
        else if (a == "--cloud-shadow" && i + 1 < argc) g_cloudShadow = float(std::atof(argv[++i]));
        else if (a == "--cloud-night" && i + 1 < argc) g_cloudNight = float(std::atof(argv[++i]));
        else if (a == "--sky-vary" && i + 1 < argc) g_skyVary = float(std::atof(argv[++i]));
        else if (a == "--sky-time" && i + 1 < argc) { g_skyTime = std::atof(argv[++i]); g_skyVary = 1.0f; }
        else if (a == "--sky-zone" && i + 1 < argc) g_skyZone = uint32_t(std::atoi(argv[++i]));   // (a playfield's climate)
        else if (a == "--fixed-clock" && i + 1 < argc) { g_fixedClock = true; Device::SetFixedClock(std::atof(argv[++i]) * 1e-3); }
        else if (a == "--cache-test" && i + 1 < argc) { cacheTest = std::atoi(argv[++i]); shadowTest = shadows = true; }
        else if (a == "--dump" && i + 1 < argc) dump = argv[++i];
        else if (a == "--bench" && i + 1 < argc) bench = std::atoi(argv[++i]);
        else if (a == "--city-bench") { cityBench = true; hdr = shadows = g_grassFieldOn = true; }
        else if (a == "--city-warm" && i + 1 < argc) g_cityWarm = std::atoi(argv[++i]);
        else if (a == "--city-lamps" && i + 1 < argc) g_cityLamps = std::atoi(argv[++i]);
        else if (a == "--city-movers" && i + 1 < argc) g_cityMovers = std::atoi(argv[++i]);
        else if (a == "--city-no-grass") g_cityNoGrass = true;
        else if (a == "--city-no-cull") g_cityNoCull = true;
        else if (a == "--city-statels" && i + 1 < argc) g_cityStatels = argv[++i];
        else if (a == "--game-ms" && i + 1 < argc) g_benchGameMs = std::atof(argv[++i]);
        else if (a == "--frames" && i + 1 < argc) frames = std::atoi(argv[++i]);
        else if (a == "--hdr") hdr = true;
        else if (a == "--dof" && i + 1 < argc) { dof = float(std::atof(argv[++i])); hdr = true; }
        else if (a == "--dof-smooth") dofBokeh = false;
        else if (a == "--dof-no-far") dofFar = false;
        else if (a == "--dof-focus" && i + 1 < argc) dofFocus = float(std::atof(argv[++i]));
        else if (a == "--motion-blur" && i + 1 < argc) { motionBlur = float(std::atof(argv[++i])); hdr = true; }
        else if (a == "--motion-mode" && i + 1 < argc) motionMode = uint32_t(std::atoi(argv[++i]));
        else if (a == "--grass-walk" && i + 1 < argc) g_grassWalk = float(std::atof(argv[++i]));
        else if (a == "--grass-walk-speed" && i + 1 < argc) g_grassWalkSpeed = float(std::atof(argv[++i]));
        else if (a == "--grass-flip") g_grassFlip = true;
        else if (a == "--grass-late" && i + 1 < argc) g_grassLate = std::atoi(argv[++i]);
        else if (a == "--grass-field") { g_grassFieldOn = true; hdr = true; }
        else if (a == "--grass-bench") { g_grassFieldOn = g_grassBench = true; hdr = true; }
        else if (a == "--readback-test") g_readbackTest = true;
        else if (a == "--terrain-test") g_terrainTest = true;
        else if (a == "--native-ground-test") g_nativeGroundTest = true;
        else if (a == "--native-on") g_nativeOn = true;
        else if (a == "--ground-single") g_groundSingle = true;
        else if (a == "--native-ground-file" && i + 1 < argc) g_groundFile = argv[++i];
        else if (a == "--eye" && i + 3 < argc) { for (int k = 0; k < 3; ++k) g_eye[k] = float(std::atof(argv[++i])); }
        else if (a == "--view-yaw" && i + 1 < argc) g_viewYaw = float(std::atof(argv[++i]));
        else if (a == "--view-pitch" && i + 1 < argc) g_viewPitch = float(std::atof(argv[++i]));
        else if (a == "--ground-ao" && i + 1 < argc) g_groundAo = float(std::atof(argv[++i]));
        else if (a == "--ground-tess" && i + 1 < argc) g_groundTess = float(std::atof(argv[++i]));
        else if (a == "--cliff-depth" && i + 1 < argc) g_cliffDepth = float(std::atof(argv[++i]));
        else if (a == "--ground-sun" && i + 1 < argc) g_groundSun = float(std::atof(argv[++i]));
        else if (a == "--ground-rocks" && i + 1 < argc) g_groundRocks = float(std::atof(argv[++i]));
        else if (a == "--ground-pebbles" && i + 1 < argc) g_groundPebbles = float(std::atof(argv[++i]));
        else if (a == "--tile-orm") g_tileOrm = true;
        else if (a == "--ground-grain" && i + 1 < argc) g_groundGrain = float(std::atof(argv[++i]));
        else if (a == "--ground-one" && i + 1 < argc) g_groundOne = std::atoi(argv[++i]);
        else if (a == "--ground-blend" && i + 1 < argc) g_groundBlend = float(std::atof(argv[++i]));
        else if (a == "--sand-ripple" && i + 1 < argc) g_sandRipple = float(std::atof(argv[++i]));
        else if (a == "--parallax" && i + 1 < argc) g_parallax = float(std::atof(argv[++i]));
        else if (a == "--parallax-steps" && i + 1 < argc) g_parallaxSteps = float(std::atof(argv[++i]));
        else if (a == "--dune-light" && i + 1 < argc) g_duneLight = float(std::atof(argv[++i]));
        else if (a == "--sand-glint" && i + 1 < argc) g_sandGlint = float(std::atof(argv[++i]));
        else if (a == "--ripple-smooth" && i + 1 < argc) g_rippleSmooth = float(std::atof(argv[++i]));
        else if (a == "--sand-prints" && i + 1 < argc) g_sandPrints = float(std::atof(argv[++i]));
        else if (a == "--ripple-depth" && i + 1 < argc) g_rippleDepth = float(std::atof(argv[++i]));
        else if (a == "--print-walk") g_printWalk = true;
        else if (a == "--ground-light") g_groundLight = true;
        else if (a == "--desert-air" && i + 1 < argc) g_desertAir = float(std::atof(argv[++i]));
        else if (a == "--ground-fog" && i + 1 < argc) g_groundFog = float(std::atof(argv[++i]));
        else if (a == "--heat-shimmer" && i + 1 < argc) g_heatShimmer = float(std::atof(argv[++i]));

        else if (a == "--terrain-off") g_terrainOff = true;
        else if (a == "--terrain-yaw" && i + 1 < argc) g_terrainYaw = float(std::atof(argv[++i]));
        else if (a == "--terrain-maps") g_terrainMaps = true;
        else if (a == "--terrain-vary" && i + 1 < argc) g_terrainVary = float(std::atof(argv[++i]));
        else if (a == "--leaf-tree" && i + 1 < argc) g_leafTrees = argv[++i];
        else if (a == "--leaf-off") g_leafSettings.on = false;
        else if (a == "--leaf-art" && i + 1 < argc) g_leafArt = argv[++i];
        else if (a == "--leaf-no-own") g_leafSettings.own = false;
        else if (a == "--crown-shade" && i + 1 < argc) g_crownShade = float(std::atof(argv[++i]));
        else if (a == "--free-cam") g_freeCam = true;
        else if (a == "--leaf-shadow-off") g_leafSettings.shadows = false;
        else if (a == "--leaf-fall" && i + 1 < argc) g_leafSettings.fall = float(std::atof(argv[++i]));
        else if (a == "--leaf-density" && i + 1 < argc) g_leafSettings.density = float(std::atof(argv[++i]));
        else if (a == "--leaf-size" && i + 1 < argc) g_leafSettings.size = float(std::atof(argv[++i]));
        else if (a == "--leaf-dist" && i + 1 < argc) g_leafSettings.distance = float(std::atof(argv[++i]));
        else if (a == "--leaf-core" && i + 1 < argc) g_leafSettings.core = float(std::atof(argv[++i]));
        else if (a == "--leaf-wind" && i + 1 < argc) g_leafSettings.wind = float(std::atof(argv[++i]));
        else if (a == "--leaf-flutter" && i + 1 < argc) g_leafSettings.flutter = float(std::atof(argv[++i]));
        else if (a == "--leaf-cam-dist" && i + 1 < argc) g_leafCamDist = float(std::atof(argv[++i]));
        else if (a == "--fol-edges" && i + 1 < argc) g_folEdges = std::atoi(argv[++i]);
        else if (a == "--prepass-eq" && i + 1 < argc) g_prepassEq = std::atoi(argv[++i]);
        else if (a == "--fol-dither" && i + 1 < argc) g_folDither = float(std::atof(argv[++i]));
        else if (a == "--leaf-cam-height" && i + 1 < argc) g_leafCamHeight = float(std::atof(argv[++i]));
        else if (a == "--leaf-cam-yaw" && i + 1 < argc) g_leafCamYaw = float(std::atof(argv[++i]));
        else if (a == "--leaf-cam-turn" && i + 1 < argc) g_leafCamTurn = float(std::atof(argv[++i]));
        else if (a == "--leaf-cam-orbit" && i + 1 < argc) g_leafCamOrbit = float(std::atof(argv[++i]));
        else if (a == "--leaf-film" && i + 1 < argc) g_leafFilm = std::atoi(argv[++i]);
        else if (a == "--leaf-pond") g_leafPond = true;
        else if (a == "--leaf-forest" && i + 1 < argc) g_leafForest = std::atoi(argv[++i]);
        else if (a == "--water-lake") { g_waterScene = 1; hdr = true; }
        else if (a == "--water-coast") { g_waterScene = 2; hdr = true; }
        else if (a == "--water-wall") g_waterWall = true;
        else if (a == "--smoke-wall") g_smokeWall = true;
        else if (a == "--water-mesh" && i + 1 < argc) g_waterMesh = argv[++i];
        else if (a == "--water-lamp") g_waterLamp = true;
        else if (a == "--smoke") { g_smoke = true; hdr = true; }
        else if (a == "--smoke-walker") { g_smoke = g_smokeWalker = true; hdr = true; }
        else if (a == "--smoke-game") { g_smoke = g_smokeGame = true; hdr = true; }
        else if (a == "--smoke-two") g_smokeTwo = true;
        else if (a == "--smoke-many" && i + 1 < argc) g_smokeMany = std::atoi(argv[++i]);
        else if (a == "--smoke-budget" && i + 1 < argc) g_smokeBudget = float(std::atof(argv[++i]));
        else if (a == "--smoke-res" && i + 1 < argc) g_smokeRes = float(std::atof(argv[++i]));
        else if (a == "--smoke-qual" && i + 1 < argc) g_smokeQual = uint32_t(std::atoi(argv[++i]));
        else if (a == "--smoke-opaque" && i + 1 < argc) g_smokeOpaque = float(std::atof(argv[++i]));
        else if (a == "--smoke-wisps" && i + 1 < argc) g_smokeWisps = float(std::atof(argv[++i]));
        else if (a == "--smoke-full") g_smokeHalf = false;
        else if (a == "--smoke-wobble" && i + 1 < argc) g_smokeWobble = float(std::atof(argv[++i]));
        else if (a == "--smoke-wind" && i + 1 < argc) g_smokeWindScale = float(std::atof(argv[++i]));
        else if (a == "--smoke-density" && i + 1 < argc) g_smokeDensity = float(std::atof(argv[++i]));
        else if (a == "--rain" && i + 1 < argc) { g_rainForce = float(std::atof(argv[++i])); hdr = true; }
        else if (a == "--lightning-at" && i + 1 < argc) g_lightningAt = std::atoi(argv[++i]);
        else if (a == "--rain-speed" && i + 1 < argc) g_rainSpeed = float(std::atof(argv[++i]));
        else if (a == "--rain-length" && i + 1 < argc) g_rainLength = float(std::atof(argv[++i]));
        else if (a == "--wet" && i + 1 < argc) g_wetNow = float(std::atof(argv[++i]));
        else if (a == "--storm" && i + 1 < argc) { g_stormForce = float(std::atof(argv[++i])); hdr = true; }
        else if (a == "--storm-thick" && i + 1 < argc) g_stormThick = float(std::atof(argv[++i]));
        else if (a == "--fog-banks" && i + 1 < argc) g_fogBankForce = float(std::atof(argv[++i]));
        else if (a == "--heat-flash") g_heatForce = true;
        else if (a == "--strike-rate" && i + 1 < argc) g_strikeRate = float(std::atof(argv[++i]));
        else if (a == "--water-mesh-at" && i + 3 < argc) { for (float& x : g_waterMeshAt) x = float(std::atof(argv[++i])); }
        else if (a == "--water-eye" && i + 3 < argc) { for (float& x : g_waterEye) x = float(std::atof(argv[++i])); g_waterEyeSet = true; }
        else if (a == "--water-shelf") g_waterShelf = true;
        else if (a == "--water-pool") g_waterPool = true;
        else if (a == "--water-walker") g_waterWalker = true;
        else if (a == "--water-jump") { g_waterWalker = true; g_waterJump = true; }
        else if (a == "--water-swim" && i + 1 < argc) g_waterSwim = float(std::atof(argv[++i]));
        else if (a == "--water-shadows") shadows = true;   // (the coast's camera then goes wrong from frame 2: not the water's)
        else if (a == "--water-style" && i + 1 < argc) g_waterStyle = float(std::atof(argv[++i]));
        else if (a == "--water-yaw" && i + 1 < argc) g_waterYaw = float(std::atof(argv[++i]));
        else if (a == "--water-turn" && i + 1 < argc) g_waterTurn = float(std::atof(argv[++i]));
        else if (a == "--tile-swap" && i + 3 < argc) { for (uint32_t& x : g_tileSwap) x = uint32_t(std::atoi(argv[++i])); }
        else if (a == "--walk-from" && i + 3 < argc) { for (float& x : g_walkFrom) x = float(std::atof(argv[++i])); }
        else if (a == "--look-round" && i + 1 < argc) g_lookRound = std::atoi(argv[++i]);
        else if (a == "--water-pitch" && i + 1 < argc) g_waterPitch = float(std::atof(argv[++i]));
        else if (a == "--water-height" && i + 1 < argc) g_waterHeight = float(std::atof(argv[++i]));
        else if (a == "--water-waves" && i + 1 < argc) g_waterWaves = float(std::atof(argv[++i]));
        else if (a == "--water-quality" && i + 1 < argc) g_waterQuality = std::atoi(argv[++i]);
        else if (a == "--water-grid") g_waterGrid = true;
        else if (a == "--water-off") g_waterOff = true;
        else if (a == "--water-night") g_waterNight = true;
        else if (a == "--game-day") g_gameDay = true;
        else if (a == "--rig-props") g_rigProps = true;
        else if (a == "--indoor") g_indoor = true;
        else if (a == "--moon-light" && i + 1 < argc) g_moonLight = float(std::atof(argv[++i]));
        else if ((a == "--darkness" || a == "--night-dark") && i + 1 < argc) g_darkness = float(std::atof(argv[++i]));
        else if (a == "--dark-floor" && i + 1 < argc) g_darkFloor = float(std::atof(argv[++i]));
        else if (a == "--atmos" && i + 1 < argc) g_atmos = std::atoi(argv[++i]) != 0;
        else if (a == "--dawn-light" && i + 1 < argc) g_dawnLight = float(std::atof(argv[++i]));
        else if (a == "--twilight" && i + 2 < argc) {
            g_twilight = float(std::atof(argv[++i]));
            g_skyGlow = float(std::atof(argv[++i]));
        }
        else if (a == "--dusk-rays" && i + 1 < argc) g_duskRays = float(std::atof(argv[++i]));
        else if (a == "--sky-dome" && i + 1 < argc) g_skyDome = 0xFF000000u | uint32_t(std::strtoul(argv[++i], nullptr, 16));
        else if (a == "--proc-sky" && i + 1 < argc) g_procSky = std::atoi(argv[++i]) != 0;
        else if (a == "--stars" && i + 1 < argc) g_nightSky.stars = std::atoi(argv[++i]) != 0;
        else if (a == "--star-bright" && i + 1 < argc) g_nightSky.bright = float(std::atof(argv[++i]));
        else if (a == "--galaxy" && i + 1 < argc) g_nightSky.galaxy = float(std::atof(argv[++i]));
        else if (a == "--meteors" && i + 1 < argc) g_nightSky.meteors = float(std::atof(argv[++i]));
        else if (a == "--aurora" && i + 1 < argc) g_nightSky.auroraForce = float(std::atof(argv[++i]));
        else if (a == "--profile-window") g_profileWindow = true;
        else if (a == "--rainbow" && i + 1 < argc) g_rainbowForce = float(std::atof(argv[++i]));
        else if (a == "--twinkle" && i + 1 < argc) g_nightSky.twinkle = float(std::atof(argv[++i]));
        else if (a == "--sky-key" && i + 2 < argc) {
            const int k = std::atoi(argv[++i]);
            const uint32_t v = uint32_t(std::strtoul(argv[++i], nullptr, 16)) & 0xFFFFFFu;
            if (k >= 0 && k < int(rvk::kSkyKeyCount)) g_skyColours.keys[k] = v;
        }
        else if (a == "--sky-zone-hint" && i + 1 < argc) g_skyZoneHint = float(std::atof(argv[++i]));
        else if (a == "--mist" && i + 1 < argc) g_mist = float(std::atof(argv[++i]));
        else if (a == "--water-z" && i + 1 < argc) g_waterZ = float(std::atof(argv[++i]));
        else if (a == "--water-noshadow") shadows = false;
        else if (a == "--water-nohdr") hdr = false;
        else if (a == "--water-noprepass") g_waterNoPrepass = true;
        else if (a == "--hud-sprite") g_hudSprite = true;
        else if (a == "--grass-field-any") g_grassFieldTex = false;
        else if (a == "--grass-field-tan") g_grassFieldTan = true;
        else if (a == "--grass-field-dist" && i + 1 < argc) g_grassFieldDist = float(std::atof(argv[++i]));
        else if (a == "--grass-bench-yaw" && i + 1 < argc) g_grassBenchYaw = float(std::atof(argv[++i]));
        else if (a == "--grass-bench-walker") g_grassBenchWalker = true;
        else if (a == "--grass-bench-steep") g_grassBenchSteep = true;
        else if (a == "--grass-bench-cross") { g_grassBenchCross = true; g_grassBenchWalker = true; }
        else if (a == "--grass-bench-night") g_grassBenchNight = true;
        else if (a == "--grass-bench-lamp") g_grassBenchLamp = true;
        else if (a == "--grass-bench-still") g_grassBenchStill = true;
        else if (a == "--grass-bench-rise" && i + 1 < argc) g_grassBenchRise = float(std::atof(argv[++i]));
        else if (a == "--grass-bench-back" && i + 1 < argc) g_grassBenchBack = float(std::atof(argv[++i]));
        else if (a == "--grass-bench-lamp-at" && i + 1 < argc) g_grassBenchLampAt = float(std::atof(argv[++i]));
        else if (a == "--grass-bench-zoom" && i + 2 < argc) {
            g_grassBenchZoomAt = std::atoi(argv[++i]);
            g_grassBenchZoomRise = float(std::atof(argv[++i]));
        }
        else if (a == "--terrain-pbr") { g_terrainPbr = g_grassBench = g_grassBenchStill = true; hdr = true; }
        else if (a == "--grass-bench-storm") g_grassBenchStorm = true;
        else if (a == "--sand-obstacles") g_sandObstacles = true;
        else if (a == "--sand-arch") g_sandObstacles = g_sandArch = true;
        else if (a == "--sand-sim") g_sandSim = true;
        else if (a == "--sand-proc") g_sandSim = false;
        else if (a == "--sand-contrast" && i + 1 < argc) g_sandContrast = float(std::atof(argv[++i]));
        else if (a == "--sand-clear" && i + 1 < argc) g_sandClear = float(std::atof(argv[++i]));
        else if (a == "--sand-gusts" && i + 1 < argc) g_sandGusts = float(std::atof(argv[++i]));
        else if (a == "--sand-fog") g_sandFog = 1.0f;
        else if (a == "--sand-haze") g_sandFog = 0.0f;
        else if (a == "--sand-wind" && i + 2 < argc) {
            g_sandWind[0] = float(std::atof(argv[++i]));
            g_sandWind[1] = float(std::atof(argv[++i]));
            g_sandWindSet = true;
        }
        else if (a == "--bare-ground") g_bareGround = true;
        else if (a == "--sand-storm") { g_grassFieldOn = g_grassBench = g_grassBenchStorm = g_sandStorm = true; hdr = true; }
        else if (a == "--sand-blow" && i + 1 < argc) g_sandBlow = float(std::atof(argv[++i]));
        else if (a == "--sand-res" && i + 1 < argc) g_sandRes = uint32_t(std::atoi(argv[++i]));
        else if (a == "--wind-storm" && i + 1 < argc) g_windStorm = float(std::atof(argv[++i]));
        else if (a == "--sand-force" && i + 1 < argc) g_sandForce = float(std::atof(argv[++i]));
        else if (a == "--sand-ash") g_sandAsh = true;
        else if (a == "--grass-bench-platform") g_grassBenchPlatform = true;
        else if (a == "--grass-bench-loading" && i + 1 < argc) g_grassBenchLoading = std::atoi(argv[++i]);
        else if (a == "--grass-bench-loading-null") g_grassBenchLoadingNull = true;
        else if (a == "--grass-bench-film" && i + 1 < argc) g_grassBenchFilm = std::atoi(argv[++i]);
        else if (a == "--grass-bench-pitch" && i + 1 < argc) g_grassBenchPitch = float(std::atof(argv[++i]));
        else if (a == "--grass-even" && i + 1 < argc) g_grassEven = float(std::atof(argv[++i]));
        else if (a == "--grass-shadow" && i + 1 < argc) g_grassShadow = std::atoi(argv[++i]) != 0;
        else if (a == "--grass-style" && i + 1 < argc) {
            char* at = argv[++i];
            for (int k = 0; k < 5; ++k) {
                char* end = nullptr;
                g_grassStyle[k] = std::strtof(at, &end);
                if (end == at || *end != ',')
                    break;
                at = end + 1;
            }
        } else if (a == "--walker-round") g_walkerRound = true;
        else if (a == "--walker-light") g_walkerLight = true;
        else if (a == "--walker-skip" && i + 1 < argc) g_walkerSkip = std::atoi(argv[++i]);
        else if (a == "--grass-dense" && i + 1 < argc) g_grassDense = std::atoi(argv[++i]);
        else if (a == "--grass-size" && i + 1 < argc) g_grassSize = float(std::atof(argv[++i]));
        else if (a == "--tess" && i + 1 < argc) tess = float(std::atof(argv[++i]));
        else if (a == "--tess-level" && i + 1 < argc) tessLevel = std::atoi(argv[++i]);
        else if (a == "--pt-light" && i + 1 < argc) ptLight = float(std::atof(argv[++i]));
        else if (a == "--sun-res" && i + 1 < argc) sunRes = std::atoi(argv[++i]);
        else if (a == "--pt-res" && i + 1 < argc) ptRes = std::atoi(argv[++i]);
        else if (a == "--foliage-lod" && i + 1 < argc) foliageLod = float(std::atof(argv[++i]));
        else if (a == "--plant-detail" && i + 1 < argc) plantDetail = float(std::atof(argv[++i]));
        else if (a == "--grass-push" && i + 1 < argc) grassPush = float(std::atof(argv[++i]));
        else if (a == "--deform-cube" && i + 1 < argc) g_deformCube = float(std::atof(argv[++i]));
        else if (a == "--moving-cube" && i + 1 < argc) g_movingCube = float(std::atof(argv[++i]));
        else if (a == "--camera-yaw" && i + 1 < argc) g_cameraYaw = float(std::atof(argv[++i]));
        else if (a == "--camera-pitch" && i + 1 < argc) g_cameraPitch = float(std::atof(argv[++i]));
        else if (a == "--aniso" && i + 1 < argc) anisotropy = uint32_t(std::atoi(argv[++i]));
        else if (a == "--bump" && i + 1 < argc) bump = float(std::atof(argv[++i]));
        else if (a == "--normal-map-test") normalMapTest = true;
        else if (a == "--pbr-test") pbrTest = true;
        else if (a == "--pbr-selflit") { g_pbrSelfLit = true; pbrTest = true; }
        else if (a == "--glow-occlude-test") { glowOccludeTest = true; hdr = true; }
        else if (a == "--pbr-maps" && i + 1 < argc) { g_pbrMaps = argv[++i]; pbrTest = true; }
        else if (a == "--floor-repeats" && i + 1 < argc) g_floorRepeats = float(std::atof(argv[++i]));
        else if (a == "--pbr-off") g_pbrOff = true;
        else if (a == "--pbr-probe" && i + 1 < argc) g_pbrProbe = float(std::atof(argv[++i]));
        else if (a == "--pbr-room") g_pbrRoom = true;
        else if (a == "--pick" && i + 2 < argc) { g_pick[0] = float(std::atof(argv[++i])); g_pick[1] = float(std::atof(argv[++i])); }
        else if (a == "--pbr-debug-late") g_pbrDebugLate = true;
        else if (a == "--pbr-debug" && i + 1 < argc) g_pbrDebug = uint32_t(std::atoi(argv[++i]));
        else if (a == "--ao" && i + 1 < argc) { ao = float(std::atof(argv[++i])); hdr = true; }
        else if (a == "--ao-radius" && i + 1 < argc) aoRadius = float(std::atof(argv[++i]));
        else if (a == "--gi" && i + 1 < argc) { gi = float(std::atof(argv[++i])); hdr = true; }
        else if (a == "--gi-radius" && i + 1 < argc) giRadius = float(std::atof(argv[++i]));
        else if (a == "--volume" && i + 1 < argc) { volume = float(std::atof(argv[++i])); hdr = true; }
        else if (a == "--vol-haze" && i + 1 < argc) volHaze = float(std::atof(argv[++i]));
        else if (a == "--vol-shafts" && i + 1 < argc) volShafts = float(std::atof(argv[++i]));
        else if (a == "--ssr" && i + 1 < argc) { ssr = float(std::atof(argv[++i])); hdr = true; }
        else if (a == "--ssr-wet" && i + 1 < argc) ssrWet = float(std::atof(argv[++i]));
        else if (a == "--knob" && i + 1 < argc) {   // RVK_Name=value: any plain setting (rvk.h RVK_*_KNOBS)
            std::string kv = argv[++i];
            size_t eq = kv.find('=');
            int k = eq == std::string::npos ? -1 : rvk::KnobByName(kv.substr(0, eq).c_str());
            if (k < 0) { std::fprintf(stderr, "--knob %s: not a knob\n", kv.c_str()); return 2; }
            knobs.emplace_back(rvk::Knob(k), float(std::atof(kv.c_str() + eq + 1)));
        }
        else if (a == "--bloom-occ" && i + 1 < argc) bloomOcc = float(std::atof(argv[++i]));
        else if (a == "--neon-sat" && i + 1 < argc) neonSat = float(std::atof(argv[++i]));
        else if (a == "--bloom-wide" && i + 1 < argc) bloomWide = float(std::atof(argv[++i]));
        else if (a == "--sun-soft" && i + 1 < argc) sunSoft = float(std::atof(argv[++i]));
        else if (a == "--leaf" && i + 1 < argc) leaf = float(std::atof(argv[++i]));
        else if (a == "--night-glow" && i + 1 < argc) { nightGlow = float(std::atof(argv[++i])); g_nightSign = true; }
        else if (a == "--contact" && i + 1 < argc) contact = float(std::atof(argv[++i]));
        else if (a == "--sway" && i + 1 < argc) sway = float(std::atof(argv[++i]));
        else if (a == "--lut-sepia") lutSepia = true;
        else if (a == "--taa") taa = true;
        else if (a == "--fxaa") fxaa = true;
        else if (a == "--sun-view") g_sunView = true;
        else if (a == "--two-lamps") g_twoLamps = true;
        else if (a == "--lamp-wobble") g_lampWobble = true;
        else if (a == "--sharpen" && i + 1 < argc) sharpen = float(std::atof(argv[++i]));
        else if (a == "--render-scale" && i + 1 < argc) renderScale = std::atoi(argv[++i]);
        else if (a == "--upscaler" && i + 1 < argc) upscaler = std::atoi(argv[++i]);
        else if (a == "--frame-gen") frameGen = true;
        else if (a == "--ui-window") g_uiWindow = true;
        else if (a == "--shine-pass" && i + 1 < argc) g_shinePass = std::atoi(argv[++i]);
        else if (a == "--frame-gen-toggle" && i + 1 < argc) g_frameGenToggle = std::atoi(argv[++i]);
        else if (a == "--fg-loading" && i + 3 < argc) {
            g_fgBlankFrom = std::atoi(argv[++i]);
            g_fgHudAt = std::atoi(argv[++i]);
            g_fgHudEnd = std::atoi(argv[++i]);
        }
        else if (a == "--grade" && i + 5 < argc) {   // saturation contrast warmth night-tint vignette
            saturation = float(std::atof(argv[++i])); contrast = float(std::atof(argv[++i]));
            warmth = float(std::atof(argv[++i])); nightTint = float(std::atof(argv[++i])); vignette = float(std::atof(argv[++i]));
        }
        else if (a == "--effect-glow" && i + 1 < argc) effectGlow = float(std::atof(argv[++i]));
        else if (a == "--bloom" && i + 1 < argc) { bloom = float(std::atof(argv[++i])); hdr = true; }
        else if (a == "--tonemap-knee" && i + 1 < argc) knee = float(std::atof(argv[++i]));
        else if (a == "--point-shadow-fade" && i + 1 < argc) fadeIn = std::atof(argv[++i]);
        else if (a == "--light-headroom" && i + 1 < argc) headroom = float(std::atof(argv[++i]));
        else if (a == "--point-shadows" && i + 1 < argc) pointShadows = uint32_t(std::atoi(argv[++i]));
        else if (a == "--shot" && i + 1 < argc) shot = argv[++i];
        else if (a == "--particle-test") particleTest = true;
        else if (a == "--fx" && i + 1 < argc) { fxFile = argv[++i]; hdr = true; }
        else if (a == "--fx-still") g_fxStill = true;
        else if (a == "--particles-off") particlesOff = true;
        else if (a == "--particle-end" && i + 1 < argc) particleEnd = std::atoi(argv[++i]);
        else if (a == "--particle-effects" && i + 1 < argc) particleEffects = std::atoi(argv[++i]);
        else if (a == "--particle-motion" && i + 1 < argc) g_particleMotion = argv[++i];
        else if (a == "--particle-end-free") g_particleEndFree = true;
        else if (a == "--particle-offscreen") g_particleOffscreen = true;
        else if (a == "--particle-water") g_particleWater = true;
        else if (a == "--particle-bright" && i + 1 < argc) g_particleBright = float(std::atof(argv[++i]));
        else if (a == "--particle-trail" && i + 1 < argc) g_particleTrail = float(std::atof(argv[++i]));
        else if (a == "--particle-color" && i + 1 < argc) g_particleColor = uint32_t(std::strtoul(argv[++i], nullptr, 16));
        else if (a == "--particle-uniform" && i + 1 < argc) g_particleUniform = float(std::atof(argv[++i]));
        else if (a == "--sprite-scale" && i + 1 < argc) g_spriteScale = float(std::atof(argv[++i]));
    }

    HWND hwnd = nullptr;
    if (windowed) {
        WNDCLASSA wc{};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.lpszClassName = "rvk_demo";
        wc.hCursor = LoadCursorA(nullptr, (LPCSTR)IDC_ARROW);
        RegisterClassA(&wc);
        RECT r{0, 0, LONG(g_width), LONG(g_height)};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        hwnd = CreateWindowA("rvk_demo", "rvk demo", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    }

    Device dev;
    std::string error;
    if (!dev.Init(hwnd, g_width, g_height, &error)) {
        std::printf("rvk init failed: %s\n", error.c_str());
        return 1;
    }
    dev.SetPixelLighting(pixelLighting);
    {
        Device::TerrainSettings t;               // (--parallax: every scene's side-loaded heights)
        t.parallax = g_parallax;
        t.parallaxSteps = g_parallaxSteps;
        dev.SetTerrain(t);
    }
    dev.SetLightingDebug(lightingDebug);
    dev.SetHideInterface(g_hideUi);
    dev.SetLightOverride(lightOverride);
    dev.SetShadows(shadows);
    dev.SetKnob(rvk::Knob::Headroom, headroom);
    dev.SetHdr(hdr);
    if (!g_meshIds.empty())
        dev.LoadMeshIds(g_meshIds.c_str());
    {
        Device::CloudParams clouds;
        clouds.enable = g_clouds && !g_cloudsGame;
        clouds.half = g_cloudsHalf;
        clouds.layer = g_cloudLayer;
        clouds.cover = g_cloudCover;
        clouds.density = g_cloudThick;
        clouds.shade = g_cloudShade;
        clouds.height = g_cloudHeight;
        clouds.low = g_cloudLow;
        clouds.quality = uint32_t(g_cloudQual);
        clouds.sheet = g_cloudSheet;
        clouds.shadow = g_cloudShadow;
        clouds.night = g_cloudNight;
        clouds.vary = g_skyVary;
        dev.SetSkyTime(g_skyTime);
        dev.SetSkyPlayfield(g_skyZone);
        dev.SetCloudParams(clouds);
    }
    dev.SetDesertAir(g_desertAir, g_heatShimmer);
    if (g_smoke) {
        Device::SmokeParams smoke;
        smoke.enable = true;
        smoke.density = g_smokeDensity;
        smoke.detail = g_smokeWisps;
        smoke.half = g_smokeHalf;
        smoke.opacity = g_smokeOpaque;
        smoke.sandCells = g_sandRes;
        if (g_smokeBudget >= 0.0f) smoke.budget = g_smokeBudget;
        if (g_smokeRes > 0.0f) smoke.resolution = g_smokeRes;
        if (g_smokeQual) smoke.quality = g_smokeQual;
        dev.SetSmokeParams(smoke);
    } else if (g_sandRes) {
        Device::SmokeParams smoke;
        smoke.sandCells = g_sandRes;
        dev.SetSmokeParams(smoke);
    }
    if (g_rainForce > 0.0f || g_stormForce > 0.0f || g_sandStorm || g_sandForce > 0.0f) {
        Device::WeatherParams weather;
        weather.enable = true;
        weather.force = g_rainForce;
        weather.stormForce = g_stormForce;
        weather.strikeRate = g_strikeRate;
        weather.speed = g_rainSpeed;
        weather.length = g_rainLength;
        if (g_wetNow >= 0.0f) dev.SetWetNow(g_wetNow, g_wetNow);
        dev.SetWeatherParams(weather);
    }
    dev.SetKnob(rvk::Knob::StormThick, g_stormThick);
    dev.SetKnob(rvk::Knob::GroundOne, float(g_groundOne));
    dev.SetFogBanks(1.0f, g_fogBankForce);
    dev.SetHeatFlash(1.0f, g_heatForce);
    dev.SetKnob(rvk::Knob::SandBlow, g_sandBlow);
    dev.SetKnob(rvk::Knob::SandProc, !g_sandSim);
    dev.SetKnob(rvk::Knob::SandContrast, g_sandContrast);
    dev.SetKnob(rvk::Knob::SandClear, g_sandClear);
    dev.SetKnob(rvk::Knob::SandGusts, g_sandGusts);
    dev.SetKnob(rvk::Knob::SandFog, g_sandFog);
    dev.SetKnob(rvk::Knob::WindStorm, g_windStorm);
    dev.SetSandForce(g_sandForce, g_sandAsh);
    dev.SetTonemap(knee, 1.0f);
    dev.SetBloom(bloom, 1.0f);
    dev.SetEffectGlow(effectGlow);
    dev.SetKnob(rvk::Knob::BloomOcc, bloomOcc);
    dev.SetKnob(rvk::Knob::NeonSat, neonSat);
    dev.SetKnob(rvk::Knob::BloomWide, bloomWide);
    dev.SetKnob(rvk::Knob::SunSoft, sunSoft);
    dev.SetLeafLight(leaf);
    dev.SetKnob(rvk::Knob::NightGlow, nightGlow);
    dev.SetKnob(rvk::Knob::MoonLight, g_moonLight);
    dev.SetDarkness(g_darkness, g_darkFloor);
    dev.SetKnob(rvk::Knob::Atmos, g_atmos);
    dev.SetTwilight(g_twilight, g_skyGlow);
    dev.SetKnob(rvk::Knob::DuskRays, g_duskRays);
    for (const auto& [k, value] : knobs) dev.SetKnob(k, value);
    dev.SetProcSky(g_procSky, g_skyZoneHint, 1.0f);
    dev.SetSkyColours(g_skyColours);
    dev.SetNightSky(g_nightSky);
    dev.SetRainbow(1.0f, g_rainbowForce);
    dev.SetKnob(rvk::Knob::DawnLight, g_dawnLight);
    dev.SetKnob(rvk::Knob::Mist, g_mist);
    dev.SetKnob(rvk::Knob::Contact, contact);
    dev.SetKnob(rvk::Knob::Sway, sway);
    dev.SetKnob(rvk::Knob::GrassPush, grassPush);
    dev.SetGrassStyle(g_grassStyle[0], g_grassStyle[1], g_grassStyle[2], g_grassStyle[3], g_grassStyle[4] != 0.0f);
    dev.SetGrassEven(g_grassEven);
    dev.SetGrassShadows(g_grassShadow);
    dev.SetKnob(rvk::Knob::PlantDetail, plantDetail);
    dev.SetKnob(rvk::Knob::FoliageLod, foliageLod);
    if (g_grassFieldOn)                  // --grass-field: the procedural ground grass (needs HDR: its pass draws into the scene)
        dev.SetGrassField(true, g_grassFieldDist, 3.0f, 0.5f, g_grassFieldTex);
    dev.SetShadowResolution(uint32_t(sunRes), uint32_t(ptRes));
    dev.SetPointLightIntensity(ptLight, ptLight);
    dev.SetTessellation(tess, 20.0f, uint32_t(tessLevel));
    dev.SetTaa(taa, sharpen);
    dev.SetRenderScale(uint32_t(renderScale));
    dev.SetUpscaler(uint32_t(upscaler));
    dev.SetFrameGen(frameGen);
    dev.SetKnob(rvk::Knob::Fxaa, fxaa);
    dev.SetGrading(saturation, contrast, warmth, 1.0f, nightTint, vignette);
    if (lutSepia) {                      // --lut-sepia: a 16^3 sepia lookup table in the day slot (the 3D LUT path)
        const uint32_t n = 16;
        std::vector<uint8_t> lut(size_t(n) * n * n * 4);
        for (uint32_t b = 0; b < n; ++b)
            for (uint32_t g = 0; g < n; ++g)
                for (uint32_t r = 0; r < n; ++r) {
                    float y = (0.3f * r + 0.59f * g + 0.11f * b) / float(n - 1);
                    uint8_t* p = &lut[((size_t(b) * n + g) * n + r) * 4];
                    p[0] = uint8_t(std::min(1.0f, y * 1.07f) * 255); p[1] = uint8_t(y * 0.74f * 255);
                    p[2] = uint8_t(y * 0.43f * 255); p[3] = 255;
                }
        dev.SetColorLut(0, n, lut.data());
    }
    dev.SetAo(ao, aoRadius);
    dev.SetGi(gi, giRadius);
    dev.SetVolume(volume, volHaze, volShafts);
    dev.SetSsr(ssr, 1.0f, 0.3f, ssrWet);
    dev.SetBump(bump);
    dev.SetAnisotropy(anisotropy);
    dev.SetMotionBlur(motionBlur, 8.0f);
    dev.SetMotionBlurMode(motionMode);
    dev.SetDof(dof > 0.0f, dofBokeh, true, dof, 16.0f, dofFocus, 0.2f, dofFar, 3.0f);
    dev.SetPointShadowFadeIn(fadeIn);    // frames here are milliseconds apart: no fade-in unless asked
    if (glowOccludeTest) {
        RunGlowOccludeTest(dev, frames, shot);
        std::printf("rendered; screenshot %s\n", shot.c_str());
        return 0;
    }
    if (pbrTest) {
        RunPbrTest(dev, frames, shot);
        std::printf("rendered; screenshot %s\n", shot.c_str());
        return 0;
    }
    if (normalMapTest) {
        RunNormalMapTest(dev, frames, shot, bump > 0.0f ? bump : 1.0f);
        std::printf("rendered; screenshot %s\n", shot.c_str());
        return 0;
    }
    if (pointShadowTest) {               // needs per-pixel lighting with the light override
        dev.SetPixelLighting(true);
        dev.SetLightOverride(true);
        dev.SetPointShadows(pointShadows);
        RunPointShadowTest(dev, frames, shot, dump, pointShadowSun);
        std::printf("rendered; screenshot %s\n", shot.c_str());
        return 0;
    }
    if (!fxFile.empty()) {
        if (threaded) {
            ThreadedDevice tdev;
            if (!tdev.Init(nullptr, g_width, g_height, &error)) { std::printf("init failed: %s\n", error.c_str()); return 1; }
            tdev.SetHdr(hdr);
            tdev.SetBloom(bloom, 1.0f);
            RunFxTest(tdev, frames, fxFile, shot);
            tdev.Sync();
        } else {
            RunFxTest(dev, frames, fxFile, shot);
        }
        std::printf("rendered; screenshot %s\n", shot.c_str());
        return 0;
    }
    if (particleTest) {
        if (threaded) {
            ThreadedDevice tdev;
            if (!tdev.Init(nullptr, g_width, g_height, &error)) { std::printf("init failed: %s\n", error.c_str()); return 1; }
            tdev.SetHdr(hdr);
            tdev.SetBloom(bloom, 1.0f);
            tdev.SetHideInterface(g_hideUi);
            RunParticleTest(tdev, frames, frameMs, shot, dump, !particlesOff, particleEnd, particleEffects);
            tdev.Sync();
        } else {
            RunParticleTest(dev, frames, frameMs, shot, dump, !particlesOff, particleEnd, particleEffects);
        }
        std::printf("rendered; screenshot %s\n", shot.c_str());
        return 0;
    }
    if (!dump.empty() && !shadowTest && g_leafTrees.empty() && g_statelPack.empty()) dev.RequestFrameDump(dump);
    std::printf("GPU: %s (Vulkan %u.%u), driver %s\n", dev.Info().gpu.c_str(), VK_API_VERSION_MAJOR(dev.Info().apiVersion),
                VK_API_VERSION_MINOR(dev.Info().apiVersion), dev.Info().driver.c_str());

    if (cityBench) {
        // The city bench (render-batching P0): always through ThreadedDevice, its settings as the main device's.
        ThreadedDevice tdev;
        std::string err;
        if (!tdev.Init(nullptr, g_width, g_height, &err)) { std::printf("init failed: %s\n", err.c_str()); return 1; }
        tdev.SetPixelLighting(pixelLighting);
        tdev.SetHideInterface(g_hideUi);
        tdev.SetShadows(shadows);
        tdev.SetHdr(hdr);
        tdev.SetBloom(bloom, 1.0f);
        for (const auto& [k, value] : knobs) tdev.SetKnob(k, value);
        // (Only as asked: the bench's baselines were made without.)
        if (lightOverride) {
            tdev.SetLightOverride(true);
            tdev.SetPointShadows(pointShadows);   // (--point-shadows N, 4 by default: the nearest lamps' cubes)
        }
        if (taa) {
            tdev.SetTaa(true, sharpen);
            tdev.SetRenderScale(uint32_t(renderScale));
            tdev.SetUpscaler(uint32_t(upscaler));
        }
        if (g_pbrDebug) {
            Device::PbrSettings pbr;
            pbr.enabled = true;
            pbr.debug = g_pbrDebug;
            tdev.SetPbr(pbr);
        }
        RunCityBench(tdev, frames, shot, dump);
        std::printf("rendered; screenshot %s\n", shot.c_str());
        return 0;
    }
    if (bench && threaded) {
        // The benchmark through rvk::ThreadedDevice: the time measured is what stays on the calling thread.
        ThreadedDevice tdev;
        std::string err;
        if (!tdev.Init(nullptr, g_width, g_height, &err)) { std::printf("init failed: %s\n", err.c_str()); return 1; }
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
        if (!tdev.Init(hwnd, g_width, g_height, &error)) { std::printf("init failed: %s\n", error.c_str()); return 1; }
        tdev.SetPixelLighting(pixelLighting);
        tdev.SetLightingDebug(lightingDebug);
        tdev.SetHideInterface(g_hideUi);
        tdev.SetLightOverride(lightOverride);
        tdev.SetShadows(shadows);
        if (!dump.empty() && !shadowTest && g_statelPack.empty()) tdev.RequestFrameDump(dump);
        if (g_readbackTest) RunReadbackTest(tdev);
        else if (!g_statelPack.empty()) RunStatelTest(tdev, frames, shot, dump);
        else if (shadowTest) RunShadowTest(tdev, frames, shot, cacheTest, frameMs, dump);
        else RunDemo(tdev, windowed, stress, frames, shot);
    } else if (g_readbackTest) {
        RunReadbackTest(dev);
    } else if (!g_statelPack.empty()) {
        RunStatelTest(dev, frames, shot, dump);
    } else if (g_handoverTest) {
        RunHandoverTest(dev, frames, shot);
    } else if (!g_portalView.empty()) {
        RunPortalView(dev, frames, shot);
    } else if (!g_portalCaptureDir.empty()) {
        g_portalDump = dump;
        RunPortalCaptureTest(dev, frames, shot);
    } else if (!g_leafTrees.empty()) {
        RunLeafScene(dev, frames, shot, dump);
    } else if (g_grassBench) {
        RunGrassBench(dev, frames, shot);
    } else if (g_terrainTest) {
        RunTerrainTest(dev, frames, shot);
    } else if (g_nativeGroundTest) {
        RunNativeGroundTest(dev, frames, shot);
    } else if (!g_groundFile.empty()) {
        RunGroundFile(dev, frames, shot);
    } else if (g_waterScene) {
        RunWaterScene(dev, frames, shot);
    } else if (shadowTest) {
        if (g_walkerLight) {                     // the character's own light casts point shadows, full strength by day
            dev.SetPointShadows(pointShadows);
            dev.SetPointShadowStrength(1.0f, 1.0f);
        }
        RunShadowTest(dev, frames, shot, cacheTest, frameMs, dump);
    } else {
        RunDemo(dev, windowed, stress, frames, shot);
    }
    std::printf("rendered; screenshot %s\n", windowed ? "(none)" : shot.c_str());
    return 0;
}
