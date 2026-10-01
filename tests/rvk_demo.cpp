// rvk test scenes. Draws a 3x2 grid of tiles, each exercising part of the D3D7 feature set the game uses,
// through rvk's D3D7-shaped API only.
//
//   rvk_demo.exe [--window] [--frames N] [--shot out.bmp]
// Headless by default: renders N frames (default 3) and writes the last one to --shot (default rvk_demo.bmp).
// --window opens a window and animates until it is closed.
#include "rvk.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace rvk;
using namespace rvk::d3d;

namespace {

constexpr uint32_t kTile = 320, kCols = 3, kRows = 2;
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

struct Scene {
    Device& dev;
    Texture* checker;
    Texture* dot;
    Texture* stripes;
    Texture* gray;
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
};

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l)
{
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(hwnd, msg, w, l);
}

}  // namespace

int main(int argc, char** argv)
{
    bool windowed = false;
    int frames = 3;
    std::string shot = "rvk_demo.bmp";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--window") windowed = true;
        else if (a == "--frames" && i + 1 < argc) frames = std::atoi(argv[++i]);
        else if (a == "--shot" && i + 1 < argc) shot = argv[++i];
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
    std::printf("GPU: %s (Vulkan %u.%u), driver %s\n", dev.Info().gpu.c_str(), VK_API_VERSION_MAJOR(dev.Info().apiVersion),
                VK_API_VERSION_MINOR(dev.Info().apiVersion), dev.Info().driver.c_str());

    auto checkerPixels = Checker(64, 8, 0xFFE0E0E0, 0x00404040);   // dark cells are transparent (alpha test)
    auto dotPixels = SoftDot(64);
    auto stripePixels = Stripes(64);
    uint32_t grayPixel = 0xFF808080;
    Scene scene{dev, dev.CreateTexture(64, 64, checkerPixels.data()), dev.CreateTexture(64, 64, dotPixels.data()),
                dev.CreateTexture(64, 64, stripePixels.data()), dev.CreateTexture(1, 1, &grayPixel), 0.0f};

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
        scene.Ui();
        scene.LitCube();
        scene.Effects();
        scene.Fog();
        scene.Stages();
        scene.Primitives();
        dev.EndFrame();
    }
    for (Texture* t : {scene.checker, scene.dot, scene.stripes, scene.gray})
        dev.DestroyTexture(t);
    std::printf("rendered; screenshot %s\n", windowed ? "(none)" : shot.c_str());
    return 0;
}
