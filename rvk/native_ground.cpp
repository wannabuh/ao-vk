// The ground drawn from the playfield's own data instead of the game's (docs/native-ground-plan.md, RVK_GroundOn).
//
// The game's ground (DisplaySystem.dll AnarchyGround_t) rebuilds its triangles on the game's thread as the camera
// moves and bakes everything past its near ring into blurry pictures. Here the playfield's heights and tiles (handed
// over by the proxy, rvk_ground.cpp) become static patches once, drawn in the game's own two passes - the base pass
// (each cell's own tile: terrain.glsl's native mode, through a tile-code texture) and the lighting pass (the hour's
// baked lightmap) - with the game's render state for each: the first ground draw of a pass in a frame draws all the
// patches in view for it, and every ground draw of the game's is skipped. Everything that reads the ground's draws
// (cliff faces, shadows, hill shade, mist, wet ground, fog) reads these the same way.
#include "internal.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace rvk {

using namespace detail;

namespace {

// The game's ground format: position, normal, two coordinate sets (IsTerrain).
constexpr uint32_t kFvf = d3d::FVF_XYZ | d3d::FVF_NORMAL | (2u << 8);
struct Vertex { float x, y, z, nx, ny, nz, u0, v0, u1, v1; };
static_assert(sizeof(Vertex) == 40, "vertex layout");

// The game's lightmap palette (AnarchyGround_t::SetShadowMap): 16 greys, (i / 15 x 188 + 4) x (1 - shade).
uint32_t PaletteGrey(uint32_t i, float shade)
{
    const float v = (float(i) / 15.0f * 188.0f + 4.0f) * std::clamp(1.0f - shade, 0.0f, 1.0f);
    const uint32_t g = uint32_t(std::clamp(v, 0.0f, 255.0f));
    return 0xFF000000u | g << 16 | g << 8 | g;
}

}  // namespace

// How much of the sky each height sample sees, from the heights alone: the horizon in 12 directions out to 64 cells
// (256 units), each direction's share 1 - sin(its horizon's elevation). A canyon's foot sees little of it, a ridge all.
// On a worker thread (about a third of a second for a zone of 800,000 samples); the result as texels (grey, 0..255).
static std::shared_ptr<std::vector<uint32_t>> SkyVisibility(std::shared_ptr<const NativeGround> g)
{
    const int W = int(g->cellsX) + 1, H = int(g->cellsZ) + 1;
    auto out = std::make_shared<std::vector<uint32_t>>(size_t(W) * H);
    // Each direction's steps: their offsets in samples and 1 / their distance (made once - the samples' loop only
    // reads, subtracts, multiplies).
    constexpr int kDirs = 12, kSteps = 12;
    static const int steps[kSteps] = {1, 2, 3, 4, 6, 8, 11, 16, 22, 32, 45, 64};
    int ox[kDirs][kSteps], oz[kDirs][kSteps];
    float inv[kDirs][kSteps];
    const float cell = 0.5f * (g->cellX + g->cellZ);
    for (int k = 0; k < kDirs; ++k) {
        const float a = float(k) * 6.2831853f / float(kDirs) + 0.13f;
        for (int i = 0; i < kSteps; ++i) {
            ox[k][i] = int(std::lround(std::cos(a) * steps[i]));
            oz[k][i] = int(std::lround(std::sin(a) * steps[i]));
            inv[k][i] = 1.0f / (std::sqrt(float(ox[k][i] * ox[k][i] + oz[k][i] * oz[k][i])) * cell);
        }
    }
    const float* h = g->heights.data();
    for (int r = 0; r < H; ++r)
        for (int c = 0; c < W; ++c) {
            const float h0 = h[size_t(r) * W + c];
            float vis = 0.0f;
            for (int k = 0; k < kDirs; ++k) {
                float best = 0.0f;                          // tangent of the horizon's elevation
                for (int i = 0; i < kSteps; ++i) {
                    const int x = c + ox[k][i], z = r + oz[k][i];
                    if (unsigned(x) >= unsigned(W) || unsigned(z) >= unsigned(H)) break;
                    best = std::max(best, (h[size_t(z) * W + x] - h0) * inv[k][i]);
                }
                vis += 1.0f - best / std::sqrt(1.0f + best * best);
            }
            const uint32_t v = uint32_t(std::clamp(vis / float(kDirs), 0.0f, 1.0f) * 255.0f + 0.5f);
            (*out)[size_t(r) * W + c] = 0xFF000000u | v << 16 | v << 8 | v;
        }
    return out;
}

// Once a frame (the native ground's first draw): the sky's visibility in once its worker is done.
void Device::PollNativeSky()
{
    if (!m_native.skyPending.valid() ||
        m_native.skyPending.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;
    auto px = m_native.skyPending.get();
    if (!px || !m_native.ground) return;
    m_native.sky = CreateTexture(m_native.ground->cellsX + 1, m_native.ground->cellsZ + 1, px->data());
    Log("native ground: the sky's visibility over the land made (RVK_GroundAo)");
}

void Device::DestroyNativeGround()
{
    if (m_native.skyPending.valid()) m_native.skyPending.wait();
    if (m_native.ground && m_native.framesDrawn)
        Log("native ground: playfield %u - %.1f patches drawn a frame over %u frames, %u rocks made", m_native.ground->id,
            double(m_native.patchesDrawn) / m_native.framesDrawn, m_native.framesDrawn, m_native.rocksMade);
    if (m_native.ground && m_native.pebblesMade)
        Log("native ground: playfield %u - %u pebbles made", m_native.ground->id, m_native.pebblesMade);
    m_native.patchesDrawn = m_native.framesDrawn = m_native.rocksMade = m_native.pebblesMade = 0;
    DropNativePebbles(true);
    // (The lightmap stays: it is the hour's, handed over when it changes - perhaps before this playfield's ground.)
    // (Nor does the fade from the last hour's: across playfields, it would be another place's light.)
    for (Texture** t : {&m_native.codes, &m_native.maps, &m_native.means, &m_native.sky, &m_native.lightmapPrev})
        if (*t) { DestroyTexture(*t); *t = nullptr; }
    m_native.lightFit[0] = 0.0f;
    m_native.anyOrm = nullptr;
    m_native.patches.clear();
    m_native.holes.clear();
    m_native.holeCells = 0;
    m_native.ground.reset();
}

// A playfield's ground: its patches (vertices, boxes), the shared indices, the tile-code texture. Null: the game's own.
void Device::SetNativeGround(std::shared_ptr<const NativeGround> ground)
{
    DestroyNativeGround();
    if (!ground || !ground->cellsX || !ground->cellsZ || !ground->patch ||
        ground->heights.size() != size_t(ground->cellsX + 1) * (ground->cellsZ + 1) ||
        ground->codes.size() != size_t(ground->cellsX) * ground->cellsZ) {
        if (ground) Log("native ground: playfield %u's data doesn't add up - the game's ground", ground->id);
        return;
    }
    const NativeGround& g = *ground;
    const uint32_t P = g.patch, W = g.cellsX + 1;
    const uint32_t pw = (g.cellsX + P - 1) / P, ph = (g.cellsZ + P - 1) / P;
    auto heightAt = [&](int c, int r) {
        c = std::clamp(c, 0, int(g.cellsX));
        r = std::clamp(r, 0, int(g.cellsZ));
        return g.heights[size_t(r) * W + c];
    };
    m_native.indexCount = P * P * 6;
    m_native.vertices = (P + 1) * (P + 1);
    for (uint32_t pz = 0; pz < ph; ++pz)
        for (uint32_t px = 0; px < pw; ++px) {
            auto data = std::make_shared<std::vector<uint8_t>>(size_t(m_native.vertices) * sizeof(Vertex));
            Vertex* v = reinterpret_cast<Vertex*>(data->data());
            NativePatch p{};
            p.lo[0] = p.lo[1] = p.lo[2] = 1e30f;
            p.hi[0] = p.hi[1] = p.hi[2] = -1e30f;
            for (uint32_t j = 0; j <= P; ++j)
                for (uint32_t i = 0; i <= P; ++i) {
                    const int c = int(std::min(px * P + i, g.cellsX)), r = int(std::min(pz * P + j, g.cellsZ));
                    Vertex& o = v[j * (P + 1) + i];
                    o.x = float(c) * g.cellX;
                    o.z = float(r) * g.cellZ;
                    o.y = heightAt(c, r);
                    // The normal from the heights round it (central differences).
                    const float dx = (heightAt(c + 1, r) - heightAt(c - 1, r)) / (2.0f * g.cellX);
                    const float dz = (heightAt(c, r + 1) - heightAt(c, r - 1)) / (2.0f * g.cellZ);
                    const float len = std::sqrt(dx * dx + dz * dz + 1.0f);
                    o.nx = -dx / len;
                    o.ny = 1.0f / len;
                    o.nz = -dz / len;
                    o.u0 = float(c);                                  // the cell coordinates (terrain.glsl)
                    o.v0 = float(r);
                    o.u1 = float(c) / float(g.cellsX);                // the playfield's lightmap: a texel a cell
                    o.v1 = float(r) / float(g.cellsZ);
                    const float pos[3] = {o.x, o.y, o.z};
                    for (int k = 0; k < 3; ++k) { p.lo[k] = std::min(p.lo[k], pos[k]); p.hi[k] = std::max(p.hi[k], pos[k]); }
                }
            p.vertices = data;
            // Its indices: P x P cells of two triangles (a patch at the playfield's edge has fewer cells, its
            // vertices still (P + 1)^2, clamped to the edge). Each cell split as NativeCellAcross says: where a cliff
            // runs at a slant to the cells, a split fixed one way crossed its foot and its rim in every other cell -
            // the flat and the steep halves made a row of teeth along it; split along the diagonal whose ends are the
            // nearer in height, the steep halves meet the flat along the cliff. Ground one can stand on keeps the
            // game's split: split the other way, a hillside's cells lay up to a few units below where the game puts
            // characters, and they floated.
            auto indices = std::make_shared<std::vector<uint8_t>>(size_t(P) * P * 6 * 2);
            uint16_t* ix = reinterpret_cast<uint16_t*>(indices->data());
            for (uint32_t j = 0; j < P; ++j)
                for (uint32_t i = 0; i < P; ++i) {
                    const uint16_t a = uint16_t(j * (P + 1) + i), b = uint16_t(a + 1), c = uint16_t(a + P + 1),
                                   d = uint16_t(c + 1);
                    const bool across = NativeCellAcross(v[a].y, v[b].y, v[c].y, v[d].y, std::min(g.cellX, g.cellZ));
                    // (The game's winding: culling is off for the ground anyway.)
                    const uint16_t q[6] = {a, c, across ? d : b, across ? a : b, across ? d : c, across ? b : d};
                    std::memcpy(ix, q, sizeof(q));
                    ix += 6;
                }
            p.indices = p.allIndices = indices;
            p.indexCount = P * P * 6;
            m_native.patches.push_back(std::move(p));
        }
    m_native.ground = ground;
    MakeNativeCodes();
    FitNativeLight();
    m_native.skyPending = std::async(std::launch::async, SkyVisibility, ground);
    if (!m_native.lightmapGrey) {
        const uint32_t grey = PaletteGrey(15, 0.5f);
        m_native.lightmapGrey = CreateTexture(1, 1, &grey);
    }
    m_native.drawnBase = m_native.drawnLight = ~0ull;
    uint32_t missing = 0;
    for (Texture* t : g.tiles) missing += !t || !t->m_image;
    Log("native ground: playfield %u, %u x %u cells of %.1f x %.1f, %zu patches of %u, %zu tiles (%u without a "
        "texture) - drawn by rvk", g.id, g.cellsX, g.cellsZ, g.cellX, g.cellZ, m_native.patches.size(), P, g.tiles.size(),
        missing);
}

// What the tile textures are drawn with now: each tile's albedo map (with RVK_Albedo), normal map (with normal maps
// on, lit per pixel) and material map (with RVK_Pbr) - side-loaded maps come in after the ground, and settings change:
// the per-cell textures are made again when this does.
uint64_t Device::NativeMapsSignature() const
{
    uint64_t h = 1469598103934665603ull ^ uint64_t(m_pbr.albedoMaps) << 1 ^ uint64_t(m_normalMaps && m_pixelLighting) ^
                 uint64_t(m_pbr.enabled && m_pixelLighting) << 2;
    for (Texture* t : m_native.ground->tiles) {
        const uint64_t a = t ? uint64_t(reinterpret_cast<uintptr_t>(t->m_albedoMap)) : 0;
        const uint64_t n = t ? uint64_t(reinterpret_cast<uintptr_t>(t->m_normalMap)) : 0;
        const uint64_t o = t ? uint64_t(reinterpret_cast<uintptr_t>(t->m_ormMap)) : 0;
        h = (h ^ a) * 1099511628211ull;
        h = (h ^ n) * 1099511628211ull;
        h = (h ^ o) * 1099511628211ull;
        h = (h ^ uint64_t(t && t->m_image)) * 1099511628211ull;
        h = (h ^ uint64_t(NativeTileHole(t))) * 1099511628211ull;   // (known once its pixels are in)
    }
    return (h ^ uint64_t(reinterpret_cast<uintptr_t>(m_native.pebbleTable.get()))) * 1099511628211ull;   // (the sand)
}

// The sand's prevailing wind (its ripples' crests lie across it: terrain.glsl), a bearing in radians: fixed for a
// playfield - not the weather's wind (wind.cpp), which turns by the minute; ripples are laid by the winds of months.
float Device::SandWindBearing() const
{
    uint32_t h = m_skyPlayfield * 0x9E3779B1u + 0x7F4A7C15u;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12;
    return float(h >> 8) / 16777216.0f * 6.2831853f;
}

// The per-cell textures: each cell's tile's bindless image - its albedo map if drawn with one - and turn (codes: R, G,
// B), its normal map's (maps: R, G) and material map's (maps: B, A; 0xFFFF none), its tile's mean colour and how much of
// it is sand (means: RGB, A - filtered between cells, so the sand's ripples fade over a cell's edge, not stop at it).
void Device::MakeNativeCodes()
{
    const NativeGround& g = *m_native.ground;
    for (Texture** t : {&m_native.codes, &m_native.maps, &m_native.means})
        if (*t) { DestroyTexture(*t); *t = nullptr; }
    std::vector<uint32_t> image(g.tiles.size(), BindlessImage(nullptr)), normal(g.tiles.size(), 0xFFFFu),
        orm(g.tiles.size(), 0xFFFFu);
    std::vector<bool> height(g.tiles.size(), false);   // (its normal map has a height: the code's alpha 0x7F)
    bool anyNormal = false;
    m_native.anyOrm = nullptr;
    m_native.anyHeight = false;
    for (size_t t = 0; t < g.tiles.size(); ++t) {
        Texture* tex = g.tiles[t];
        if (!tex || !tex->m_image) continue;
        Texture* drawn = tex->m_albedoMap && m_pbr.albedoMaps ? tex->m_albedoMap : tex;
        image[t] = BindlessImage(drawn);
        if (tex->m_normalMap && m_normalMaps && m_pixelLighting) {
            normal[t] = BindlessImage(tex->m_normalMap) & 0xFFFFu;
            anyNormal = true;
            m_native.anyHeight = m_native.anyHeight || tex->m_normalMap->m_alphaHeight;
            height[t] = tex->m_normalMap->m_alphaHeight;
        }
        if (tex->m_ormMap && m_pbr.enabled && m_pixelLighting) {
            orm[t] = BindlessImage(tex->m_ormMap) & 0xFFFFu;
            m_native.anyOrm = tex->m_ormMap;
        }
    }
    const bool anyMaps = anyNormal || m_native.anyOrm;
    std::vector<uint32_t> codes(size_t(g.cellsX) * g.cellsZ), maps(anyMaps ? codes.size() : 0), means(codes.size());
    std::vector<uint32_t> mean(g.tiles.size(), 0x00808080u);   // (each tile's, from its colour grid)
    const PebbleTable* table = m_native.pebbleTable.get();
    auto byte = [](float v) { return uint32_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    for (size_t t = 0; t < g.tiles.size(); ++t) {
        const Texture* tex = g.tiles[t];
        const uint32_t sand = table && t < table->sand.size() ? byte(table->sand[t]) << 24 : 0u;
        if (!tex || tex->m_mean[0] < 0.0f) { mean[t] |= sand; continue; }
        mean[t] = sand | byte(tex->m_mean[0]) << 16 | byte(tex->m_mean[1]) << 8 | byte(tex->m_mean[2]);
    }
    for (size_t k = 0; k < codes.size(); ++k) {
        const uint16_t code = g.codes[k];
        const uint32_t tile = code & 0x3FFFu, turn = code >> 14;
        const uint32_t t = tile < image.size() ? tile : 0;
        const uint32_t img = image.empty() ? 0u : image[t];
        codes[k] = (t < height.size() && height[t] ? 0x7F000000u : 0xFF000000u) | (img & 0xFFu) << 16 |
                   ((img >> 8) & 0xFFu) << 8 | turn;
        means[k] = mean.empty() ? 0x00808080u : mean[t];
        if (anyMaps) {
            const uint32_t nm = normal.empty() ? 0xFFFFu : normal[t], om = orm.empty() ? 0xFFFFu : orm[t];
            maps[k] = (om >> 8 & 0xFFu) << 24 | (nm & 0xFFu) << 16 | ((nm >> 8) & 0xFFu) << 8 | (om & 0xFFu);
        }
    }
    m_native.codes = CreateTexture(g.cellsX, g.cellsZ, codes.data());
    if (anyMaps) m_native.maps = CreateTexture(g.cellsX, g.cellsZ, maps.data());
    m_native.means = CreateTexture(g.cellsX, g.cellsZ, means.data());
    m_native.mapsSignature = NativeMapsSignature();
    ApplyNativeHoles();
}

// A tile that is a hole in the ground: all but transparent (its alpha under 16 of 255 all over). The Shadowlands lay
// one under the void around their floating land - 'shadowbrink', black with "Brink" written on it, alpha 2 - over
// whole playfields (the gardens, 4380-4388) or the floor far below the islands (Adonis, 4873: about half its cells).
// The game's draw lets it through to the sky; drawn opaque, the void was a plain of "Brink" to the horizon (#18).
// Every other ground tile is opaque (all the rdb's terrain tiles checked, 2026-10-28).
bool Device::NativeTileHole(const Texture* t)
{
    if (!t || !t->m_image || t->m_opaque || t->m_alphaMask.empty())
        return false;
    for (uint8_t a : t->m_alphaMask)
        if (a >= 16) return false;
    return true;
}

// The holes' cells left out of each patch's indices (a patch of holes alone isn't drawn): made again with the codes,
// as the tiles' pixels come in.
void Device::ApplyNativeHoles()
{
    const NativeGround& g = *m_native.ground;
    std::vector<uint8_t> holes(g.tiles.size(), 0);
    bool any = false;
    for (size_t t = 0; t < g.tiles.size(); ++t)
        any = (holes[t] = NativeTileHole(g.tiles[t])) || any;
    if (holes == m_native.holes)
        return;
    m_native.holes = holes;
    const uint32_t P = g.patch, pw = (g.cellsX + P - 1) / P;
    uint32_t cells = 0, empty = 0;
    for (size_t k = 0; k < m_native.patches.size(); ++k) {
        NativePatch& p = m_native.patches[k];
        const uint32_t px = uint32_t(k) % pw, pz = uint32_t(k) / pw;
        auto hole = [&](uint32_t i, uint32_t j) {
            const uint32_t c = px * P + i, r = pz * P + j;
            if (!any || c >= g.cellsX || r >= g.cellsZ) return false;   // (past the playfield's edge: flat, unseen)
            const uint32_t tile = g.codes[size_t(r) * g.cellsX + c] & 0x3FFFu;
            return tile < holes.size() && holes[tile] != 0;
        };
        uint32_t cut = 0;
        for (uint32_t j = 0; j < P; ++j)
            for (uint32_t i = 0; i < P; ++i) cut += hole(i, j);
        cells += cut;
        if (!cut) {
            p.indices = p.allIndices;
            p.indexCount = P * P * 6;
            continue;
        }
        auto kept = std::make_shared<std::vector<uint8_t>>();
        kept->reserve(size_t(P * P - cut) * 6 * 2);
        const uint8_t* all = p.allIndices->data();
        for (uint32_t j = 0; j < P; ++j)
            for (uint32_t i = 0; i < P; ++i)
                if (!hole(i, j)) {
                    const uint8_t* q = all + (size_t(j) * P + i) * 6 * 2;
                    kept->insert(kept->end(), q, q + 6 * 2);
                }
        p.indexCount = uint32_t(kept->size() / 2);
        p.indices = p.indexCount ? std::shared_ptr<const std::vector<uint8_t>>(std::move(kept)) : p.allIndices;
        empty += p.indexCount == 0;
    }
    if (cells || m_native.holeCells)
        Log("native ground: playfield %u - %u cells are holes (a see-through tile: the void), %u patches all holes",
            g.id, cells, empty);
    m_native.holeCells = cells;
}

// The hour's baked lightmap, as the game hands it to its ground: unpacked through the game's palette, a texel a cell.
void Device::SetNativeGroundLightmap(std::shared_ptr<const std::vector<uint8_t>> packed, uint32_t w, uint32_t h,
                                     float shade)
{
    // The last hour's lightmap fades into this one (the game swaps them whole: the land's light jumped).
    if (m_native.lightmapPrev) {
        DestroyTexture(m_native.lightmapPrev);
        m_native.lightmapPrev = nullptr;
    }
    if (m_native.lightmap) {
        m_lightmapsUploaded.push_back(m_native.lightmap);   // (the grass's light taken from it: again)
        if (m_native.ground && m_native.lightW == w && m_native.lightH == h)
            m_native.lightmapPrev = m_native.lightmap;
        else
            DestroyTexture(m_native.lightmap);
        m_native.lightmap = nullptr;
    }
    m_native.lightmapSince = std::chrono::steady_clock::now();
    m_native.lightFit[0] = 0.0f;
    m_native.lightPixels.clear();
    m_native.lightW = m_native.lightH = 0;
    const uint32_t rowBytes = (w + 1) / 2;
    if (!packed || !w || !h || packed->size() < size_t(rowBytes) * h)
        return;
    uint32_t palette[16];
    for (uint32_t i = 0; i < 16; ++i) palette[i] = PaletteGrey(i, shade);
    std::vector<uint32_t> px(size_t(w) * h);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t b = (*packed)[size_t(y) * rowBytes + x / 2];
            px[size_t(y) * w + x] = palette[(x & 1) ? (b & 15u) : (b >> 4)];
        }
    m_native.lightmap = CreateTexture(w, h, px.data());
    m_native.lightPixels = std::move(px);
    m_native.lightW = w;
    m_native.lightH = h;
    Log("native ground: the hour's lightmap, %u x %u, shade %.2f", w, h, shade);
    FitNativeLight();
}

// The hour's lightmap as a + b.n of the ground's slope (n the heights' normal at the cell's centre; texels 0..1): the
// game bakes it with the sun where it stood at that hour, and a plane in the normal explains it well (Stret West Bank's
// six: 84 to 92 % of the lit cells' variance). Least squares over the lit cells - those far under the plane (the baked
// shadows of trees, buildings and hills) are left out and the plane fitted again. b's bearing is the baked sun's; the
// light pass turns it to the sun's now (NativeSunTurn). a = 0: no fit (no ground, no lightmap, or the night's flat one).
void Device::FitNativeLight()
{
    m_native.lightFit[0] = 0.0f;
    if (!m_native.ground || m_native.lightPixels.empty()) return;
    const NativeGround& g = *m_native.ground;
    const uint32_t w = std::min(m_native.lightW, g.cellsX), h = std::min(m_native.lightH, g.cellsZ);
    if (w < 8 || h < 8) return;
    const uint32_t W = g.cellsX + 1;
    std::vector<float> n(size_t(w) * h * 3), v(size_t(w) * h);
    for (uint32_t r = 0; r < h; ++r)
        for (uint32_t c = 0; c < w; ++c) {
            auto H = [&](int cc, int rr) {
                cc = std::clamp(cc, 0, int(g.cellsX));
                rr = std::clamp(rr, 0, int(g.cellsZ));
                return g.heights[size_t(rr) * W + cc];
            };
            // (The cell's centre: its four corners' differences.)
            const float dx = (H(c + 1, r) + H(c + 1, r + 1) - H(c, r) - H(c, r + 1)) / (2.0f * g.cellX);
            const float dz = (H(c, r + 1) + H(c + 1, r + 1) - H(c, r) - H(c + 1, r)) / (2.0f * g.cellZ);
            const float len = std::sqrt(dx * dx + dz * dz + 1.0f);
            const size_t i = size_t(r) * w + c;
            n[i * 3] = -dx / len;
            n[i * 3 + 1] = 1.0f / len;
            n[i * 3 + 2] = -dz / len;
            v[i] = float((m_native.lightPixels[size_t(r) * m_native.lightW + c] >> 8) & 0xFFu) / 255.0f;
        }
    std::vector<uint8_t> lit(v.size(), 1);
    double coef[4] = {};
    for (int round = 0; round < 4; ++round) {
        double A[4][5] = {};
        for (size_t i = 0; i < v.size(); ++i) {
            if (!lit[i]) continue;
            const double x[4] = {1.0, n[i * 3], n[i * 3 + 1], n[i * 3 + 2]};
            for (int a = 0; a < 4; ++a) {
                for (int b = 0; b < 4; ++b) A[a][b] += x[a] * x[b];
                A[a][4] += x[a] * v[i];
            }
        }
        // (Gaussian elimination with partial pivoting: 4 x 4.)
        for (int k = 0; k < 4; ++k) {
            int pivot = k;
            for (int r = k + 1; r < 4; ++r)
                if (std::fabs(A[r][k]) > std::fabs(A[pivot][k])) pivot = r;
            if (std::fabs(A[pivot][k]) < 1e-9) return;
            for (int c = 0; c < 5; ++c) std::swap(A[k][c], A[pivot][c]);
            for (int r = 0; r < 4; ++r) {
                if (r == k) continue;
                const double f = A[r][k] / A[k][k];
                for (int c = k; c < 5; ++c) A[r][c] -= f * A[k][c];
            }
        }
        for (int k = 0; k < 4; ++k) coef[k] = A[k][4] / A[k][k];
        // (A tenth of the 4-bit scale's step under the plane: in a baked shadow.)
        for (size_t i = 0; i < v.size(); ++i) {
            const double fit = coef[0] + coef[1] * n[i * 3] + coef[2] * n[i * 3 + 1] + coef[3] * n[i * 3 + 2];
            lit[i] = v[i] - fit > -0.06;
        }
    }
    const double b = std::sqrt(coef[1] * coef[1] + coef[3] * coef[3]);
    if (b < 0.02) {
        Log("native ground: the hour's lightmap is the same on every slope (night) - not turned with the sun");
        return;
    }
    for (int k = 0; k < 4; ++k) m_native.lightFit[k] = float(coef[k]);
    Log("native ground: the hour's lightmap = %.2f + (%.2f %.2f %.2f).n - the sun it was baked with at bearing %.0f",
        coef[0], coef[1], coef[2], coef[3], std::atan2(coef[3], coef[1]) * 57.29578);
}

// The frame block's part (draw.cpp): the last hour's lightmap and how far it has faded, and the lightmap's fit with
// its sun turned to the bearing of the sun now (the baked height of the sun kept: flat ground stays as baked).
void Device::FillNativeLights(detail::FrameLights* fl)
{
    constexpr float kLightmapFade = 8.0f;        // seconds
    const float since = std::chrono::duration<float>(std::chrono::steady_clock::now() - m_native.lightmapSince).count();
    const float fade = m_native.lightmapPrev ? std::clamp(since / kLightmapFade, 0.0f, 1.0f) : 1.0f;
    const uint32_t prev = m_native.lightmapPrev && fade < 1.0f ? BindlessImage(m_native.lightmapPrev) + 1 : 0u;
    std::memcpy(&fl->terrain2[1], &prev, 4);
    fl->terrain2[2] = fade;
    std::memset(fl->terrainLight, 0, sizeof(fl->terrainLight));
    std::memset(fl->terrainLight2, 0, sizeof(fl->terrainLight2));
    const float* f = m_native.lightFit;
    if (f[0] <= 0.0f) return;
    // The fit as baked (the rocks are shaded by it whatever the setting), and turned to the sun now when there is one
    // to turn to: fl->sunDir, the way its light travels as the frame's lights and shadows have it - not the moon's.
    for (int k = 0; k < 3; ++k) fl->terrainLight[k] = fl->terrainLight2[k] = f[k + 1];
    fl->terrainLight[3] = f[0];
    const float sx = -fl->sunDir[0], sz = -fl->sunDir[2], s = std::sqrt(sx * sx + sz * sz);
    if (m_native.sun <= 0.0f || s < 0.05f || fl->moon[3] > 0.5f) return;
    const float b = std::sqrt(f[1] * f[1] + f[3] * f[3]);
    fl->terrainLight[0] = b * sx / s;
    fl->terrainLight[2] = b * sz / s;
    fl->terrainLight2[3] = std::clamp(m_native.sun, 0.0f, 1.0f);
}

bool Device::NativeGroundRgb(float u, float v, uint8_t rgb[3], Texture** tileTex) const
{
    if (tileTex) *tileTex = nullptr;
    if (!m_native.ground) return false;
    const NativeGround& g = *m_native.ground;
    const int c = std::clamp(int(std::floor(u)), 0, int(g.cellsX) - 1), r = std::clamp(int(std::floor(v)), 0, int(g.cellsZ) - 1);
    const uint16_t code = g.codes[size_t(r) * g.cellsX + c];
    const uint32_t tile = code & 0x3FFFu, turn = code >> 14;
    if (tile >= g.tiles.size() || !g.tiles[tile]) return false;
    if (tileTex) *tileTex = g.tiles[tile];
    // (A tile texture of 32 x 32 or less is the game's stand-in while the real one streams in, as for its own ground.)
    if (g.tiles[tile]->Width() * g.tiles[tile]->Height() <= 32u * 32u) return false;
    const float a = std::clamp(u - float(c), 0.0f, 1.0f), b = std::clamp(v - float(r), 0.0f, 1.0f);
    const float tu[4] = {a, 1.0f - b, 1.0f - a, b}, tv[4] = {1.0f - b, 1.0f - a, b, a};   // (terrain.glsl NativeTurn)
    return g.tiles[tile]->TexelRgbAt(tu[turn], tv[turn], rgb);
}

bool Device::NativeLightRgb(float u, float v, uint8_t rgb[3]) const
{
    if (m_native.lightPixels.empty()) return false;
    const float x = std::clamp(u * float(m_native.lightW) - 0.5f, 0.0f, float(m_native.lightW - 1));
    const float y = std::clamp(v * float(m_native.lightH) - 0.5f, 0.0f, float(m_native.lightH - 1));
    const uint32_t x0 = uint32_t(x), y0 = uint32_t(y);
    const uint32_t x1 = std::min(x0 + 1, m_native.lightW - 1), y1 = std::min(y0 + 1, m_native.lightH - 1);
    const float fx = x - float(x0), fy = y - float(y0);
    auto at = [&](uint32_t px, uint32_t py, int ch) {
        return float((m_native.lightPixels[size_t(py) * m_native.lightW + px] >> (16 - 8 * ch)) & 0xFFu);
    };
    for (int ch = 0; ch < 3; ++ch) {
        const float top = at(x0, y0, ch) + (at(x1, y0, ch) - at(x0, y0, ch)) * fx;
        const float bottom = at(x0, y1, ch) + (at(x1, y1, ch) - at(x0, y1, ch)) * fx;
        rgb[ch] = uint8_t(std::clamp(top + (bottom - top) * fy + 0.5f, 0.0f, 255.0f));
    }
    return true;
}

// A draw of the game's: is it its ground, replaced (drawn by this first, for its pass, once a frame)? Skipped then.
bool Device::NativeGroundReplaces(uint32_t fvf)
{
    if (!m_native.on || !m_native.ground || !m_native.codes || m_native.drawing)
        return false;
    if (m_drawVisualKind != uint32_t(VisualKind::Terrain) || !IsTerrain(fvf))
        return false;
    const bool base = !m_rs[d3d::RS_LIGHTING] && !m_rs[d3d::RS_ALPHABLENDENABLE];
    const bool light = m_rs[d3d::RS_LIGHTING] && IsMultiplyPass();
    // The game's ground in one pass, as it draws the Shadowlands' playfields (a frame dump of Nascene, 2026-11-04):
    // lit and opaque, the lightmap and the lights added in stage 0, the tile multiplied in by stage 1. Both passes in
    // one draw. Not taken for either, it was skipped and nothing drawn in its place: no ground at all.
    const auto& s1 = m_tss[1];
    const bool single = m_rs[d3d::RS_LIGHTING] && !m_rs[d3d::RS_ALPHABLENDENABLE] &&
                        s1[d3d::TSS_COLOROP] == d3d::TOP_MODULATE &&
                        ((s1[d3d::TSS_COLORARG1] == d3d::TA_TEXTURE && s1[d3d::TSS_COLORARG2] == d3d::TA_CURRENT) ||
                         (s1[d3d::TSS_COLORARG1] == d3d::TA_CURRENT && s1[d3d::TSS_COLORARG2] == d3d::TA_TEXTURE));
    if (base || light)
        StreetCaptureGroundPass(light);          // (a street being recorded: its ground's passes - portal_street.cpp)
    const bool first = (base || light || single) && m_native.drawnBase != m_frameNumber &&
                       m_native.drawnLight != m_frameNumber;
    if (first)
        EnsurePebbleTable();                     // (which cells are sand: the means' texture, made again once it is)
    if (first && NativeMapsSignature() != m_native.mapsSignature)
        MakeNativeCodes();                       // (the tiles' maps or the settings changed: once a frame at most)
    if (first)
        PollNativeSky();
    if (single) {
        if (first) {
            m_native.drawnBase = m_native.drawnLight = m_frameNumber;
            DrawNativeGroundSingle();
        }
    } else if (base && m_native.drawnBase != m_frameNumber) {
        m_native.drawnBase = m_frameNumber;
        // One pass (RVK_GroundOne): deferred to the lighting pass, which draws the ground once, its texture and its
        // light together - while the ground had a lighting pass last frame (this one's is then all but sure to come
        // right after), per-pixel lit into the HDR scene. The scene's fog and ambient noted now, from this pass.
        if (m_groundOnePass && m_pixelLighting && m_target == m_scene && m_scene && m_target->m_format == Format::RGBA16F &&
            m_native.drawnLight + 1 == m_frameNumber) {
            m_native.basePending = true;
            m_native.baseRs = m_rs;
            m_native.baseTss = m_tss;
            m_native.baseTex1 = m_textures[1];
            m_native.baseWorld = m_world;
            m_native.baseView = m_view;
            m_native.baseProj = m_proj;
            m_native.baseSampler = StageSamplerSlot(0);
            if (m_scenePhase && !m_external) NoteSceneFog();
        } else {
            DrawNativeGround(false);
        }
    } else if (light && m_native.drawnLight != m_frameNumber) {
        m_native.drawnLight = m_frameNumber;
        if (m_native.basePending) DrawNativeGroundOne();
        else DrawNativeGround(true);
    }
    return true;
}

// The ground in one pass (RVK_GroundOne): its lighting pass's state - lights, lightmap, fog - drawn as its base pass
// is, opaque and writing depth (the depth pre-pass takes it), with the base pass's stand-in texture, sampler and fog in
// the draw records' spare words: ffp_main lays the base's colour in the same pixel and multiplies it in, where two
// passes laid the ground's tiles twice and blended the second over the first (~1.5-2 ms of 4.4 at 2560 x 1440 over
// Stret West Bank - rvk_demo, 2026-10-20).
void Device::DrawNativeGroundOne()
{
    m_native.basePending = false;
    const auto rs = m_rs;
    m_rs[d3d::RS_ALPHABLENDENABLE] = m_native.baseRs[d3d::RS_ALPHABLENDENABLE];
    m_rs[d3d::RS_ZWRITEENABLE] = m_native.baseRs[d3d::RS_ZWRITEENABLE];
    m_rs[d3d::RS_ZFUNC] = m_native.baseRs[d3d::RS_ZFUNC];
    m_groundOne = true;
    // The base pass's fog: the game's colour turned to a storm's air (as Draw does for a non-multiplying pass), or our
    // own sky's horizon (bit 24); whether it fogs at all (bit 25).
    const uint32_t argb = m_native.baseRs[d3d::RS_FOGCOLOR];
    float fog[3] = {float((argb >> 16) & 0xFF) / 255.0f, float((argb >> 8) & 0xFF) / 255.0f, float(argb & 0xFF) / 255.0f};
    StormAirColour(fog);
    uint32_t fogBits = 0;
    for (int i = 0; i < 3; ++i)
        fogBits |= uint32_t(std::clamp(fog[i], 0.0f, 1.0f) * 255.0f + 0.5f) << (16 - 8 * i);
    if (OwnSkyFog()) fogBits |= 1u << 24;
    if (m_native.baseRs[d3d::RS_FOGENABLE]) fogBits |= 1u << 25;
    m_recordExtra[0] = BindlessImage(m_blackTexture) + 1;
    m_recordExtra[1] = m_native.baseSampler;
    m_recordExtra[2] = fogBits;
    m_constantsDirty = true;
    DrawNativeGround(true);
    m_recordExtra[0] = m_recordExtra[1] = m_recordExtra[2] = 0;
    m_groundOne = false;
    m_rs = rs;
    m_constantsDirty = true;
}

// The game's ground in one pass (NativeGroundReplaces), drawn as its two passes would be. The lighting pass's state is
// the draw's own with stage 1 off (the native lighting pass lays the lightmap in stage 0, read through set 1). The
// base pass's is made from it: unlit, with the tile's stage as stage 0 (its sampler: the game's clamped one), read
// through set 0. With RVK_GroundOne both go into the one pass (DrawNativeGroundOne); else the base pass is drawn,
// then the lighting pass multiplying it.
void Device::DrawNativeGroundSingle()
{
    const auto rs = m_rs;
    const auto tss = m_tss;
    Texture* tex1 = m_textures[1];
    auto baseRs = rs;
    baseRs[d3d::RS_LIGHTING] = 0;
    auto baseTss = tss;
    baseTss[0] = tss[1];
    baseTss[0][d3d::TSS_COLOROP] = d3d::TOP_SELECTARG1;
    baseTss[0][d3d::TSS_COLORARG1] = d3d::TA_TEXTURE;
    baseTss[0][d3d::TSS_ALPHAOP] = d3d::TOP_SELECTARG1;
    baseTss[0][d3d::TSS_ALPHAARG1] = d3d::TA_TEXTURE;
    baseTss[0][d3d::TSS_TEXCOORDINDEX] = 0;
    baseTss[1][d3d::TSS_COLOROP] = baseTss[1][d3d::TSS_ALPHAOP] = d3d::TOP_DISABLE;
    auto lightTss = tss;
    lightTss[0][d3d::TSS_TEXCOORDINDEX] = 1;
    lightTss[1][d3d::TSS_COLOROP] = lightTss[1][d3d::TSS_ALPHAOP] = d3d::TOP_DISABLE;
    m_textures[1] = nullptr;
    if (m_groundOnePass && m_pixelLighting && m_target == m_scene && m_scene && m_target->m_format == Format::RGBA16F) {
        m_tss = baseTss;
        m_native.baseSampler = StageSamplerSlot(0);
        m_native.baseRs = baseRs;
        m_native.baseTss = baseTss;
        m_native.baseTex1 = nullptr;
        m_native.baseWorld = m_world;
        m_native.baseView = m_view;
        m_native.baseProj = m_proj;
        if (m_scenePhase && !m_external) NoteSceneFog();
        m_tss = lightTss;
        m_constantsDirty = true;
        DrawNativeGroundOne();
    } else {
        m_rs = baseRs;
        m_tss = baseTss;
        m_constantsDirty = true;
        DrawNativeGround(false);
        m_rs = rs;
        m_rs[d3d::RS_ALPHABLENDENABLE] = 1;
        m_rs[d3d::RS_SRCBLEND] = d3d::BLEND_ZERO;
        m_rs[d3d::RS_DESTBLEND] = d3d::BLEND_SRCCOLOR;
        m_rs[d3d::RS_ZWRITEENABLE] = 0;
        m_rs[d3d::RS_ZFUNC] = d3d::CMP_EQUAL;
        m_tss = lightTss;
        m_constantsDirty = true;
        DrawNativeGround(true);
    }
    m_rs = rs;
    m_tss = tss;
    m_textures[1] = tex1;
    m_constantsDirty = true;
}

// The deferred base pass after all (RVK_GroundOne): something other than the ground's lighting pass came next - drawn
// now, as it was to be.
void Device::DrawPendingGroundBase()
{
    m_native.basePending = false;
    const auto rs = m_rs;
    const auto tss = m_tss;
    Texture* tex1 = m_textures[1];
    const d3d::Matrix world = m_world, view = m_view, proj = m_proj;
    m_rs = m_native.baseRs;
    m_tss = m_native.baseTss;
    m_textures[1] = m_native.baseTex1;
    m_world = m_native.baseWorld;
    m_view = m_native.baseView;
    m_proj = m_native.baseProj;
    m_constantsDirty = true;
    DrawNativeGround(false);
    m_rs = rs;
    m_tss = tss;
    m_textures[1] = tex1;
    m_world = world;
    m_view = view;
    m_proj = proj;
    m_constantsDirty = true;
}

// The patches in view for one pass, with the game's state for it: the base pass's stage 0 texture a stand-in (the
// shader reads each cell's tile), the lighting pass's the playfield's lightmap.
void Device::DrawNativeGround(bool lightPass)
{
    Texture* saved = m_textures[0];
    m_textures[0] = lightPass ? (m_native.lightmap ? m_native.lightmap : m_native.lightmapGrey) : m_blackTexture;
    m_native.drawing = true;
    UpdateFrameEye();
    // In view: a patch's box against the camera's clip planes (all eight corners outside one: left out). (Its world
    // matrix: the playfield's ground has none, a street's seen from a shop is moved to its door - portal_street.cpp.)
    const d3d::Matrix vp = MulMatrix(m_world, MulMatrix(m_view, m_proj));
    uint32_t drawn = 0;
    for (const NativePatch& p : m_native.patches) {
        bool outside[6] = {true, true, true, true, true, true};
        for (int k = 0; k < 8; ++k) {
            // (A unit round it: the tessellation's shape and rock move the ground that far at most.)
            const float w[3] = {(k & 1) ? p.hi[0] + 1.0f : p.lo[0] - 1.0f, (k & 2) ? p.hi[1] + 1.0f : p.lo[1] - 1.0f,
                                (k & 4) ? p.hi[2] + 1.0f : p.lo[2] - 1.0f};
            float c[4];
            for (int j = 0; j < 4; ++j)
                c[j] = w[0] * vp.m[0][j] + w[1] * vp.m[1][j] + w[2] * vp.m[2][j] + vp.m[3][j];
            outside[0] = outside[0] && c[0] < -c[3];
            outside[1] = outside[1] && c[0] > c[3];
            outside[2] = outside[2] && c[1] < -c[3];
            outside[3] = outside[3] && c[1] > c[3];
            outside[4] = outside[4] && c[2] < 0.0f;
            outside[5] = outside[5] && c[2] > c[3];
        }
        if (!p.indexCount || outside[0] || outside[1] || outside[2] || outside[3] || outside[4] || outside[5])
            continue;
        // Tessellated only within kNativeTessDistance (its box): beyond it ffp.tesc splits nothing and ffp.tese moves
        // nothing, the very same triangles - but through the tessellation stages every one of the ground's triangles
        // to the horizon cost the base pass ~2.5 ms at 2560 x 1440 (rvk_demo, Newland Desert). Both passes choose alike.
        float d2 = 0.0f;
        for (int k = 0; k < 3; ++k) {
            const float d = std::max({p.lo[k] - m_frameEye[k], m_frameEye[k] - p.hi[k], 0.0f});
            d2 += d * d;
        }
        m_native.tessPatch = d2 < kNativeTessDistance * kNativeTessDistance && !m_streetGround;
        DrawSharedIndexed(d3d::TriangleList, kFvf, p.vertices, 0, m_native.vertices, p.indices, p.indexCount);
        ++drawn;
    }
    DrawNativeClutter(vp);
    DrawNativePebbles(vp);
    m_native.drawing = false;
    m_textures[0] = saved;
    if (!lightPass) {
        m_native.patchesDrawn += drawn;
        ++m_native.framesDrawn;
    }
}

}  // namespace rvk
