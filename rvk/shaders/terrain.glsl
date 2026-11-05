// The ground's own texturing (rvk terrain.cpp), on both of its passes - the base (its texture) and the lighting pass
// (the lightmap, with the base texture's normal and material maps, F_BUMPBASE / MAT_BASE, and the cliffs' rock relief):
// - Cliffs: the game lays the ground's texture from above (each 4 x 4 cell its own copy of the tile, turned or flipped),
//   so down a steep face it stretches into streaks. Where the ground is steep the face gets its own lay instead, along
//   x or z with the height, in world space - one lay down the whole cliff, no seams between its cells - blended by the
//   slope (FL.terrain.x): rock (FL.terrainRock: a photo of a layered cliff, tinted to the cell's tile's mean colour,
//   D.terrainAt.xyz, so a sand cliff is sand-coloured rock), or without it the cell's own tile at its repeats per unit
//   (D.terrain.x, rvk FitTerrain). Flat ground keeps the game's coordinates and so its layout: its tiles are placed by
//   hand - transitions, edges, patches of another ground in the middle of a tile - and turned per cell against
//   repeats already.
// - Repeats: the faces are read through a grid of hexagons laid on the world, each with its own offset into the
//   texture, blended by their weights and the texture's brightness (Mikkelsen, "Practical Real-Time Hex-Tiling",
//   2022), so a cliff isn't one picture repeated in rows (FL.terrain.y). The rock's offsets are only sideways - offset
//   up or down too, its layers were chopped into short dashes, a camouflage pattern - and its layers bend gently up and
//   down along the cliff instead, as strata do, so its bands don't stack into the same rows up a high wall.
// - Variation: big lighter and darker patches over the land and up cliffs (FL.terrain.z).
// - The native ground (rvk native_ground.cpp, D.terrain.y): rvk's own patches, the game's coordinates set 0 the cell
//   coordinates (world / cell size); each pixel's cell's tile and turn from the tile-code texture, read with the
//   continuous coordinates' derivatives (no seams in the mip levels between cells), the game's clamped sampler.
// The base pass and its lighting pass compute the same weights from the same vertices: the relief and highlights stay
// on the texture they belong to. Derivatives are taken once (TerrainMapAt, in uniform control flow); every read
// after it is a textureGrad, so the reads may be skipped where their weight is 0.

const float kTerrainBase = 1.0, kTerrainLight = 2.0;

// The draw is a ground chunk whose base texture lies on the world from above (rvk FillTerrainRecord). (RVK_NO_GROUND:
// the scene shader's build for every other draw - without the ground's code it needs 128 registers, not 168 and a
// spill, so more pixels are in flight on the GPU: rvk Device::m_drawGround picks the build.)
#ifdef RVK_NO_GROUND
bool TerrainMapped() { return false; }
#else
bool TerrainMapped() { return D.terrainAt.w > 0.5; }
#endif

#include "terrain_rock.glsl"

struct TerrainMap {
    vec2 uv[3];         // the game's (from above), a face along x (it faces z: x and the height), along z (faces x)
    vec2 dx[3], dy[3];  // ... their screen derivatives
    vec3 w;             // ... their weights
    vec2 hexOff[9];     // each face's three hexagons' offsets into the texture (3 i + cell; none from above)
    vec3 hexW[3];       // ... their weights (TerrainSettle adds the texture's brightness)
    float hex;          // how much of the hexagons (0: the faces' coordinates as they are)
    vec3 px, py;        // the position's screen derivatives (the normal maps' relief)
    float side;         // how steep (0 .. 1): the cliffs' share
    bool rock;          // the faces are the rock's (else the cell's tile)
    bool clutter;       // the native ground's rocks (TerrainClutter): rock from above too, at top
    bool pebble;        // ... its pebbles (D.terrainAt.y 3): each its own shade of the ground's colour
    vec2 top, topDx, topDy;
};

// The hexagon grid over a lay (st: in repeats of the texture, on the world): the three cells around st, the weights
// of their centres (Mikkelsen's TriangleGrid, sharpened) and each cell's offset into the texture (down: this much of a
// repeat at most). A cell is about two thirds of the texture across; each lay has its own grid (seed).
void TerrainHexGrid(vec2 st, uint seed, float down, out vec3 w, out vec2 off0, out vec2 off1, out vec2 off2)
{
    st *= 2.0 * sqrt(3.0) * 0.45;
    vec2 skewed = vec2(st.x - 0.57735027 * st.y, 1.15470054 * st.y);
    ivec2 base = ivec2(floor(skewed));
    vec3 t = vec3(fract(skewed), 0.0);
    t.z = 1.0 - t.x - t.y;
    float s = step(0.0, -t.z), s2 = 2.0 * s - 1.0;
    w = pow(vec3(-t.z * s2, s - t.y * s2, s - t.x * s2), vec3(7.0));
    w /= w.x + w.y + w.z;
    ivec2 is = ivec2(s);
    uint h0 = TerrainHash(uvec2(base + is + 0x4000) + seed);
    uint h1 = TerrainHash(uvec2(base + ivec2(is.x, 1 - is.y) + 0x4000) + seed);
    uint h2 = TerrainHash(uvec2(base + ivec2(1 - is.x, is.y) + 0x4000) + seed);
    const vec2 scale = vec2(1.0, down) / 65536.0;
    off0 = vec2(float(h0 & 0xFFFFu), float(h0 >> 16)) * scale;
    off1 = vec2(float(h1 & 0xFFFFu), float(h1 >> 16)) * scale;
    off2 = vec2(float(h2 & 0xFFFFu), float(h2 >> 16)) * scale;
}

// Where and how the ground's texture lies at this pixel. uv: the game's coordinates; n: the interpolated normal.
TerrainMap TerrainMapAt(vec3 posW, vec2 uv, vec3 n)
{
    TerrainMap m;
    m.px = dFdx(posW);
    m.py = dFdy(posW);
    m.uv[0] = uv;
    m.dx[0] = dFdx(uv);
    m.dy[0] = dFdy(uv);
    // The faces: along x or z, and down - in world space, the rock's size or the tile's repeats per unit.
    m.rock = TerrainRockOn();
    const float s = m.rock ? 1.0 / FL.terrainRock.w : D.terrain.x;
    m.uv[1] = s * vec2(posW.x, -posW.y);
    m.dx[1] = s * vec2(m.px.x, -m.px.y);
    m.dy[1] = s * vec2(m.py.x, -m.py.y);
    m.uv[2] = s * vec2(posW.z, -posW.y);
    m.dx[2] = s * vec2(m.px.z, -m.px.y);
    m.dy[2] = s * vec2(m.py.z, -m.py.y);
    if (m.rock) {                       // (the strata bend gently enough to leave the derivatives as they are)
        const vec4 lays = TerrainRockLays(posW);
        m.uv[1] = lays.xy;
        m.uv[2] = lays.zw;
    }
    // How steep: the triangle's own slope (from the position's derivatives), not the interpolated normal - the game's
    // cells are 4 units across, a cliff one or two of them high, and its corners' normals, shared with the flat ground
    // round it, make the face look far less steep than it is; the stretch is the triangle's.
    vec3 face = cross(m.px, m.py);
    float len = length(face);
    vec3 nu = len > 1e-12 ? face / len : vec3(0.0, 1.0, 0.0);
    // Steeper than ~32 degrees starts the faces (the texture from above already a fifth longer down the slope); past
    // ~57 they have it all.
    m.side = smoothstep(0.85, 0.55, abs(nu.y)) * clamp(FL.terrain.x, 0.0, 1.0);
    // Which way it faces (the lay along x or along z): the interpolated normal, smooth from one triangle to the next -
    // each triangle's own made the lays change at its edges, seams across a wall at an angle. (Its own where the
    // interpolated one has no length.)
    vec3 nf = dot(n, n) > 0.0 ? normalize(n) : nu;
    // The native ground: how steep from the interpolated normal too - each triangle's own switched to the rock at its
    // edges, a row of straight-edged teeth along a cliff's rim. Its normals come from the heights over two cells, a
    // one-cell cliff's corners read less steep than its face: the rock starts a little earlier (~23 degrees, all of it
    // by ~45), and the line where it starts wanders with noise (a metre or two across) rather than following the grid.
    if (floatBitsToUint(D.terrain.y) != 0u && D.terrainAt.y < 1.5) {   // (TerrainNative; not its rocks)
        const float wander = 0.09 * (TerrainNoise(posW.xz * 0.35 + vec2(4.3, 1.9)) - 0.5)
                           + 0.05 * (TerrainNoise(posW.xz * 1.3 + vec2(8.7, 2.6)) - 0.5);
        m.side = smoothstep(0.92, 0.7, abs(nf.y) + wander) * clamp(FL.terrain.x, 0.0, 1.0);
    }
    vec2 across = pow(abs(nf.zx) + 1e-4, vec2(4.0));
    across /= across.x + across.y;
    m.w = vec3(1.0 - m.side, m.side * across);
    m.hex = clamp(FL.terrain.y, 0.0, 1.0);
    // The native ground's rocks (rvk ground_clutter.cpp; D.terrainAt.y 2): the rock from every side, from above too
    // (at its own size: top), by the interpolated normal (they are shaded smooth: by each face's own, the lays changed
    // at its edges and showed its triangles) - no hexagons (a rock is smaller than one).
    m.clutter = floatBitsToUint(D.terrain.y) != 0u && D.terrainAt.y > 1.5 && m.rock;
    m.pebble = m.clutter && D.terrainAt.y > 2.5;
    m.top = s * posW.xz;
    m.topDx = s * m.px.xz;
    m.topDy = s * m.py.xz;
    if (m.clutter) {
        vec3 a = pow(abs(nf) + 1e-4, vec3(4.0));
        a /= a.x + a.y + a.z;
        m.w = vec3(a.y, a.z, a.x);
        m.side = 1.0 - a.y;
        m.hex = 0.0;
        // (A pebble, a few pixels across: the rock from the one side it faces most - one read, not three.)
        if (m.pebble) m.w = m.w.x >= max(m.w.y, m.w.z) ? vec3(1.0, 0.0, 0.0) : m.w.y >= m.w.z ? vec3(0.0, 1.0, 0.0) : vec3(0.0, 0.0, 1.0);
    }
    // A lay with less than 3% of the pixel is left out and its share given to the others: a face turned within ~25
    // degrees of x or z reads one lay, not two, and the foot and the top of a slope read no faces or no top. Every
    // lay costs reads of each map (through its hexagons: up to three each); a few percent of one doesn't show.
    m.w = mix(m.w, vec3(0.0), lessThan(m.w, vec3(0.03)));
    m.w /= m.w.x + m.w.y + m.w.z;               // (at least one is a third or more)
    m.hexW[0] = vec3(0.0);
    for (int i = 1; i < 3; ++i) {
        m.hexW[i] = vec3(0.0);
        if (m.hex > 0.0 && m.w[i] > 0.002)
            TerrainHexGrid(m.uv[i], uint(i) * 0x9E3779B9u, m.rock ? 0.0 : 1.0, m.hexW[i], m.hexOff[3 * i],
                           m.hexOff[3 * i + 1], m.hexOff[3 * i + 2]);
    }
    return m;
}

// ---- the native ground: each cell's own tile (docs/native-ground-plan.md)

bool TerrainNative() { return floatBitsToUint(D.terrain.y) != 0u; }

// The cell's tile's bindless image and its turn: the tile-code texture's texel (R, G the image, B the turn).
uvec2 NativeCell(vec2 cell)
{
    const uint codes = floatBitsToUint(D.terrain.y) - 1u;
    const ivec2 size = textureSize(sampler2D(texImages[codes], bindlessSamplers[D.sampIdx.w]), 0);
    const vec4 t = texelFetch(sampler2D(texImages[codes], bindlessSamplers[D.sampIdx.w]),
                              clamp(ivec2(floor(cell)), ivec2(0), size - 1), 0);
    const uvec4 b = uvec4(t * 255.0 + 0.5);
    return uvec2(b.r | b.g << 8, b.b);
}

// Whether the cell's tile has a height (its normal map's alpha: parallax) - the code's alpha 0x7F (native_ground.cpp).
bool NativeCellHeight(vec2 cell)
{
    const uint codes = floatBitsToUint(D.terrain.y) - 1u;
    const ivec2 size = textureSize(sampler2D(texImages[codes], bindlessSamplers[D.sampIdx.w]), 0);
    return texelFetch(sampler2D(texImages[codes], bindlessSamplers[D.sampIdx.w]), clamp(ivec2(floor(cell)), ivec2(0), size - 1),
                      0).a < 0.75;
}

// Within a cell (a along x, b along z, 0..1) its tile's coordinates as the game lays them, by the turn
// (docs/ground-format.md): 0 (a, 1 - b), 1 (1 - b, 1 - a), 2 (1 - a, b), 3 (b, a). And a step's.
vec2 NativeTurn(vec2 f, uint turn)
{
    return turn == 0u ? vec2(f.x, 1.0 - f.y) : turn == 1u ? vec2(1.0 - f.y, 1.0 - f.x)
         : turn == 2u ? vec2(1.0 - f.x, f.y) : f.yx;
}
vec2 NativeTurnStep(vec2 d, uint turn)
{
    return turn == 0u ? vec2(d.x, -d.y) : turn == 1u ? vec2(-d.y, -d.x) : turn == 2u ? vec2(-d.x, d.y) : d.yx;
}

// The ground from above: this pixel's cell's tile. smp: the game's (clamped).
// Where two cells' tiles differ (gravel beside dirt, with no transition tile between them) the game's edge is a hard
// straight line along the grid. With FL.terrain3.x (RVK_GroundBlend) the tiles of the cells around the nearest corner
// blend over a band about each edge (0.3 of a cell each side at 1), the edge wandering with noise, each
// neighbour's tile read mirrored about the shared edge (any tile meets itself there: no seam, whatever its layout) and
// the weights taking each tile's brightness in, so the brighter one's stones break into the other rather than a smear.
// Only edges whose two tiles disagree there blend (NativeEdgeOff): tiles laid to carry on across an edge - a road's
// slabs, a transition into the grass beside it - keep the game's edge, which no one sees. The band and weights are
// functions of the 2 x 2 cells and the world position alone, the same on both sides of an edge: no seam along it.
vec3 NativeEdgeColour(uvec2 n, vec2 f, sampler smp)
{
    const float lod = max(float(textureQueryLevels(sampler2D(texImages[nonuniformEXT(n.x)], smp))) - 3.0, 0.0);
    return textureLod(sampler2D(texImages[nonuniformEXT(n.x)], smp), NativeTurn(f, n.y), lod).rgb;
}
// How much two neighbouring cells' tiles disagree along their shared edge (a below b along x or, along z, alongZ):
// their colours there, each averaged over about a quarter of the tile in from it. 0 the same.
float NativeEdgeOff(uvec2 a, uvec2 b, bool alongZ, sampler smp)
{
    if (a == b) return 0.0;
    const vec3 ca = NativeEdgeColour(a, alongZ ? vec2(0.5, 1.0) : vec2(1.0, 0.5), smp);
    const vec3 cb = NativeEdgeColour(b, alongZ ? vec2(0.5, 0.0) : vec2(0.0, 0.5), smp);
    return smoothstep(0.05, 0.15, length(ca - cb));
}

vec4 NativeTop(vec2 cell, vec2 dx, vec2 dy, sampler smp)
{
    const uvec2 c = NativeCell(cell);
    const vec4 own = textureGrad(sampler2D(texImages[nonuniformEXT(c.x)], smp), NativeTurn(fract(cell), c.y),
                                 NativeTurnStep(dx, c.y), NativeTurnStep(dy, c.y));
    const float blend = 0.3 * clamp(FL.terrain3.x, 0.0, 1.0);
    if (blend <= 0.0) return own;
    // The 2 x 2 cells about the nearest corner: t 0.5 on the edge between base and base + 1, wandered by noise (at most
    // 0.16; band + it stays inside 0..1, so the weights are whole one cell's well before t wraps at a cell's middle).
    const vec2 q = cell - 0.5, base = floor(q);
    vec2 t = q - base;
    if (all(greaterThan(abs(t - 0.5), vec2(blend + 0.16)))) return own;
    uvec2 n[4];
    for (int k = 0; k < 4; ++k) n[k] = NativeCell(base + vec2(k & 1, k >> 1) + 0.5);
    // Each direction's band: its two edges' disagreement, the larger.
    const vec2 band = blend * vec2(max(NativeEdgeOff(n[0], n[1], false, smp), NativeEdgeOff(n[2], n[3], false, smp)),
                                   max(NativeEdgeOff(n[0], n[2], true, smp), NativeEdgeOff(n[1], n[3], true, smp)));
    if (max(band.x, band.y) <= 0.0) return own;
    t += 0.22 * (vec2(TerrainNoise(cell * 0.9 + vec2(3.1, 7.7)), TerrainNoise(cell * 0.9 + vec2(11.3, 5.9))) - 0.5)
       + 0.1 * (vec2(TerrainNoise(cell * 3.1 + vec2(1.7, 9.2)), TerrainNoise(cell * 3.1 + vec2(6.4, 2.3))) - 0.5);
    const vec2 w1 = smoothstep(0.5 - max(band, 1e-4), 0.5 + max(band, 1e-4), t), w0 = 1.0 - w1;
    if (max(min(w0.x, w1.x), min(w0.y, w1.y)) <= 0.002) return own;
    const ivec2 mine = ivec2(floor(cell)) - ivec2(base);
    vec4 col[4];
    float w[4], top = -1.0;
    for (int k = 0; k < 4; ++k) {
        const ivec2 o = ivec2(k & 1, k >> 1);
        w[k] = (o.x == 1 ? w1.x : w0.x) * (o.y == 1 ? w1.y : w0.y);
        col[k] = own;
        if (o == mine || w[k] <= 0.002 || n[k] == c) continue;
        const vec2 f = 1.0 - abs(1.0 - abs(cell - base - vec2(o)));   // (mirrored into the cell about its edge)
        col[k] = textureGrad(sampler2D(texImages[nonuniformEXT(n[k].x)], smp), NativeTurn(f, n[k].y),
                             NativeTurnStep(dx, n[k].y), NativeTurnStep(dy, n[k].y));
    }
    for (int k = 0; k < 4; ++k) top = max(top, w[k] + 0.6 * Luma(col[k].rgb));
    vec4 sum = vec4(0.0);
    float total = 0.0;
    for (int k = 0; k < 4; ++k) {
        const float h = max(w[k] + 0.6 * Luma(col[k].rgb) - top + 0.25, 0.0) * step(0.002, w[k]);
        sum += col[k] * h;
        total += h;
    }
    return total > 0.0 ? sum / total : own;
}

// The lighting pass: the cells' tiles' normal maps (D.terrain.z: their texture's bindless image + 1; R, G each cell's
// tile's normal map's image, 0xFFFF none).
bool TerrainNativeMaps() { return TerrainNative() && floatBitsToUint(D.terrain.z) != 0u; }
uint NativeNormalImage(vec2 cell)
{
    const uint maps = floatBitsToUint(D.terrain.z) - 1u;
    const ivec2 size = textureSize(sampler2D(texImages[maps], bindlessSamplers[D.sampIdx.w]), 0);
    const vec4 t = texelFetch(sampler2D(texImages[maps], bindlessSamplers[D.sampIdx.w]),
                              clamp(ivec2(floor(cell)), ivec2(0), size - 1), 0);
    const uvec2 b = uvec2(t.rg * 255.0 + 0.5);
    return b.r | b.g << 8;
}

// ... and the cell's tile's material map (B, A; 0xFFFF none).
uint NativeOrmImage(vec2 cell)
{
    const uint maps = floatBitsToUint(D.terrain.z) - 1u;
    const ivec2 size = textureSize(sampler2D(texImages[maps], bindlessSamplers[D.sampIdx.w]), 0);
    const vec4 t = texelFetch(sampler2D(texImages[maps], bindlessSamplers[D.sampIdx.w]),
                              clamp(ivec2(floor(cell)), ivec2(0), size - 1), 0);
    const uvec2 b = uvec2(t.ba * 255.0 + 0.5);
    return b.x | b.y << 8;
}

// How much of the sky this point of the native ground sees (rvk native_ground.cpp SkyVisibility: the horizon all round
// from the heights), as a factor on its light: 1 - strength x the hidden part (FL.terrain2.x, RVK_GroundAo). 1 without.
float NativeSky(vec3 posW)
{
    const uint sky = floatBitsToUint(D.terrain.w);
    if (!TerrainNative() || sky == 0u || FL.terrain2.x <= 0.0) return 1.0;
    const ivec2 size = textureSize(sampler2D(texImages[sky - 1u], bindlessSamplers[D.sampIdx.w]), 0);
    const vec2 cell = posW.xz / vec2(1.0 / max(D.terrain.x, 1e-6));   // (D.terrain.x: a cell's repeats a unit)
    const float v = textureLod(sampler2D(texImages[sky - 1u], bindlessSamplers[D.sampIdx.w]), (cell + 0.5) / vec2(size), 0.0).r;
    return 1.0 - FL.terrain2.x * (1.0 - v);
}

// The cells' tiles' mean colours filtered between cells at this point: what the rock on a cliff is tinted to - per
// cell, a cliff not quite upright crossed cells corner to corner and the tint jumped at each (grass tiles: green
// wedges up the face). From the means' texture (D.terrainAt.x on the native base pass), else the cell's tile's
// smallest level.
vec3 NativeMean(vec2 cell)
{
    const uint means = floatBitsToUint(D.terrainAt.x);
    if (means != 0u && means < 65537u) {
        const vec2 size = vec2(textureSize(sampler2D(texImages[means - 1u], bindlessSamplers[D.sampIdx.w]), 0));
        return textureLod(sampler2D(texImages[means - 1u], bindlessSamplers[D.sampIdx.w]), cell / size, 0.0).rgb;
    }
    const uvec2 c = NativeCell(cell);
    return textureLod(sampler2D(texImages[nonuniformEXT(c.x)], bindlessSamplers[D.sampIdx.w]), vec2(0.5), 14.0).rgb;
}

// How much of the native ground at this point is loose sand (0..1: rvk ground_pebbles.cpp's verdict of its cell's tile,
// the means' alpha), filtered between cells so it fades over a cell's edge. 0 off the native ground or without it.
float NativeSand(vec2 cell)
{
    const uint means = floatBitsToUint(D.terrainAt.x);
    if (!TerrainNative() || means == 0u || means >= 65537u) return 0.0;
    const vec2 size = vec2(textureSize(sampler2D(texImages[means - 1u], bindlessSamplers[D.sampIdx.w]), 0));
    return textureLod(sampler2D(texImages[means - 1u], bindlessSamplers[D.sampIdx.w]), cell / size, 0.0).a;
}

#define TERRAIN_READ(img, smp, uv, dx, dy) textureGrad(sampler2D(img, smp), uv, dx, dy)
// The faces read past the tile's 0..1 (world space, offsets): tiled, whatever the draw's own sampler - the game reads
// each cell's tile clamped, which smears its edge into stripes there. (The normal maps' sampler: tiled, mipmapped,
// anisotropic; every draw has it.) The game's coordinates from above keep the draw's: tiled, a transition tile's
// filtering would bleed its far edge along the cell's borders.
#define TERRAIN_FACE_READ(img, uv, dx, dy) textureGrad(sampler2D(img, bindlessSamplers[D.sampIdx.w]), uv, dx, dy)

// One lay: from above, the game's coordinates as they are (top, read with smp); a face (face) through its hexagons
// (their weights m.hexW), blended with its plain coordinates by m.hex.
vec4 TerrainLay(texture2D top, sampler smp, texture2D face, TerrainMap m, int i)
{
    if (i == 0) return TERRAIN_READ(top, smp, m.uv[0], m.dx[0], m.dy[0]);   // (the native ground: TerrainBase)
    vec4 c = vec4(0.0);
    if (m.hex < 1.0) c = TERRAIN_FACE_READ(face, m.uv[i], m.dx[i], m.dy[i]) * (1.0 - m.hex);
    if (m.hex > 0.0)
        for (int k = 0; k < 3; ++k)
            if (m.hexW[i][k] > 0.002)
                c += TERRAIN_FACE_READ(face, m.uv[i] + m.hexOff[3 * i + k], m.dx[i], m.dy[i]) * (m.hexW[i][k] * m.hex);
    return c;
}

// All the lays blended, the faces' part times faceScale, with the weights already settled (TerrainSettle).
vec4 TerrainTexture(texture2D top, sampler smp, texture2D face, TerrainMap m, vec4 faceScale)
{
    vec4 c = m.w.x > 0.002 ? TerrainLay(top, smp, face, m, 0) * m.w.x : vec4(0.0);
    for (int i = 1; i < 3; ++i)
        if (m.w[i] > 0.002) c += TerrainLay(top, smp, face, m, i) * (faceScale * m.w[i]);
    return c;
}

// The faces' hexagons' weights take their texture's brightness in (the brighter cell wins where they meet - a clean
// seam rather than a blur); every map read after it uses the same. face: the faces' colour (the rock's albedo or the
// cell's tile) - both passes settle them from the same texture.
// Only the hexagons with a weight are read (from 0.002, as the lays read them): a pixel is mostly in one or two.
void TerrainSettle(texture2D face, inout TerrainMap m)
{
    if (m.hex <= 0.0) return;
    for (int i = 1; i < 3; ++i) {
        if (m.w[i] <= 0.002) continue;
        vec3 w = vec3(0.0);
        for (int k = 0; k < 3; ++k)
            if (m.hexW[i][k] > 0.002)
                w[k] = mix(1.0, Luma(TERRAIN_FACE_READ(face, m.uv[i] + m.hexOff[3 * i + k], m.dx[i], m.dy[i]).rgb), 0.6) *
                       m.hexW[i][k];
        m.hexW[i] = w / max(w.x + w.y + w.z, 1e-6);
    }
}

// Face i's lay settled from its own reads (TerrainSettle, then TerrainLay, without reading each hexagon twice): its
// colour, and its weights settled in m for the map reads after it.
vec4 TerrainSettleLay(texture2D face, inout TerrainMap m, int i)
{
    vec4 c = m.hex < 1.0 ? TERRAIN_FACE_READ(face, m.uv[i], m.dx[i], m.dy[i]) * (1.0 - m.hex) : vec4(0.0);
    if (m.hex <= 0.0) return c;
    vec4 sum = vec4(0.0);
    vec3 w = vec3(0.0);
    for (int k = 0; k < 3; ++k)
        if (m.hexW[i][k] > 0.002) {
            const vec4 t = TERRAIN_FACE_READ(face, m.uv[i] + m.hexOff[3 * i + k], m.dx[i], m.dy[i]);
            w[k] = mix(1.0, Luma(t.rgb), 0.6) * m.hexW[i][k];
            sum += t * w[k];
        }
    const float total = max(w.x + w.y + w.z, 1e-6);
    m.hexW[i] = w / total;
    return c + sum * (m.hex / total);
}

// The rock's colour made the cell's: its tile's mean colour over the rock's (a sand cliff is sand-coloured rock, a dark
// one dark) - its brightness whole, its hue two thirds (a grass tile on a steep face made it bright green rock: now
// a mossy hint); 1 without the rock or with the tile's colour not known yet.
vec4 TerrainRockTint(TerrainMap m)
{
    if (!m.rock || (!TerrainNative() && D.terrainAt.x < 0.0)) return vec4(1.0);
    const vec3 mean = TerrainNative() ? NativeMean(m.uv[0]) : D.terrainAt.xyz;
    const vec3 tint = clamp(mean / max(FL.terrainRock2.rgb, vec3(0.02)), vec3(0.2), vec3(5.0));
    return vec4(mix(vec3(dot(tint, vec3(0.3, 0.59, 0.11))), tint, 0.66), 1.0);
}

// The base texture as the ground lays it (the base pass's colour; the lighting pass's PBR albedo): settles the weights.
// image: the base texture's bindless image; the faces' is the rock's albedo, or the same. The rock's contrast is a
// photo's - harsher than the game's soft ground beside it: a quarter of the way to its own mean.
// The native ground's rocks: the rock read from above and along x and z by their faces' weights, a quarter of the
// way to its mean, tinted to the ground's colour where they lie.
vec4 TerrainClutterBase(TerrainMap m)
{
    vec4 c = m.w.x > 0.002 ? TERRAIN_FACE_READ(texImages[TerrainRockImage(0)], m.top, m.topDx, m.topDy) * m.w.x : vec4(0.0);
    for (int i = 1; i < 3; ++i)
        if (m.w[i] > 0.002) c += TERRAIN_FACE_READ(texImages[TerrainRockImage(0)], m.uv[i], m.dx[i], m.dy[i]) * m.w[i];
    c.rgb = mix(FL.terrainRock2.rgb, c.rgb, 0.75);
    if (!m.pebble) return c * TerrainRockTint(m);
    // A pebble (rvk ground_pebbles.cpp): the ground's colour under it with the rock's grain in its brightness (not the
    // rock's own hue: brown stones on grey gravel). Set 0 is where it lies, the same at all its corners (the middle of
    // a 1/256 of a cell): its hash makes each stone lighter or darker, and nearer the ground's hue or a plain grey.
    const uint h = TerrainHash(uvec2(floor(m.uv[0] * 256.0)) + uvec2(0x51u, 0x2Fu));
    const float v = float(h & 0xFFFFu) / 65535.0, w = float(h >> 16) / 65535.0;
    const vec3 mean = NativeMean(m.uv[0]);
    // (Never brighter than a ground texture can be: on pale sand the grain and the shade together made stones of
    // twice white, which the lighting pass doubled again - they glowed in the bloom and shimmered as the TAA jittered
    // them; scaled down, not clipped, to keep their hue.)
    const float grain = clamp(Luma(c.rgb) / max(Luma(FL.terrainRock2.rgb), 0.02), 0.7, 1.3);
    // On loose sand the few stones keep the sand's own hue: grey and white ones read as gravel strewn over the dunes.
    const float hue = mix(0.4 + 0.6 * w, 1.0, NativeSand(m.uv[0]));
    vec3 stone = mix(vec3(Luma(mean)), mean, hue) * grain * (0.75 + 0.5 * v);
    stone *= min(1.0, 0.92 / max(max(stone.r, stone.g), max(stone.b, 1e-4)));
    return vec4(stone, c.a);
}

vec4 TerrainBase(uint image, sampler smp, inout TerrainMap m)
{
    if (m.clutter) return TerrainClutterBase(m);
    if (TerrainNative()) {
        // The native ground: its cell's tile from above; the faces rock, or the cell's tile laid on them (no
        // hexagons: one cell's tile in each pixel's own).
        const vec4 top = m.w.x > 0.002 ? NativeTop(m.uv[0], m.dx[0], m.dy[0], smp) * m.w.x : vec4(0.0);
        if (m.w.x >= 0.998) return top;
        if (m.rock) {
            vec4 faces = vec4(0.0);
            for (int i = 1; i < 3; ++i)
                if (m.w[i] > 0.002) faces += TerrainSettleLay(texImages[TerrainRockImage(0)], m, i) * m.w[i];
            const float side = m.w.y + m.w.z;
            faces.rgb = mix(FL.terrainRock2.rgb * side, faces.rgb, 0.75);
            return top + faces * TerrainRockTint(m);
        }
        const uvec2 c = NativeCell(m.uv[0]);
        vec4 faces = vec4(0.0);
        for (int i = 1; i < 3; ++i)
            if (m.w[i] > 0.002)
                faces += textureGrad(sampler2D(texImages[nonuniformEXT(c.x)], bindlessSamplers[D.sampIdx.w]), m.uv[i],
                                     m.dx[i], m.dy[i]) * m.w[i];
        return top + faces;
    }
    const uint faceImage = m.rock ? TerrainRockImage(0) : image;
    const vec4 top = m.w.x > 0.002 ? TerrainLay(texImages[image], smp, texImages[faceImage], m, 0) * m.w.x : vec4(0.0);
    vec4 faces = vec4(0.0);
    for (int i = 1; i < 3; ++i)
        if (m.w[i] > 0.002) faces += TerrainSettleLay(texImages[faceImage], m, i) * m.w[i];
    if (!m.rock) return top + faces;
    const float side = m.w.y + m.w.z;
    if (side > 0.002) faces.rgb = mix(FL.terrainRock2.rgb * side, faces.rgb, 0.75);
    return top + faces * TerrainRockTint(m);
}

// The relief of normal maps laid like the texture: from above the base's (top, strength), on the faces the rock's
// (or the base's), each lay's relief through its own coordinates' derivatives (as NormalMapNormal), blended.
vec3 TerrainNormalMap(vec3 n, texture2D top, float topStrength, texture2D face, float faceStrength, TerrainMap m)
{
    vec3 sum = vec3(0.0);
    for (int i = 0; i < 3; ++i) {
        if (m.w[i] <= 0.002) continue;
        vec2 uvx = m.dx[i], uvy = m.dy[i];
        vec3 t;
        if (i == 0 && m.clutter) {
            // (The rocks from above: the rock's own normal map, at its size.)
            uvx = m.topDx;
            uvy = m.topDy;
            t = TERRAIN_FACE_READ(face, m.top, uvx, uvy).xyz * 2.0 - 1.0;
        } else if (i == 0 && TerrainNativeMaps()) {
            // (The native ground from above: the cell's tile's own normal map, turned as its tile is.)
            const uvec2 c = NativeCell(m.uv[0]);
            const uint img = NativeNormalImage(m.uv[0]);
            if (img == 0xFFFFu) { sum += n * m.w[0]; continue; }
            uvx = NativeTurnStep(m.dx[0], c.y);
            uvy = NativeTurnStep(m.dy[0], c.y);
            t = textureGrad(sampler2D(texImages[nonuniformEXT(img)], bindlessSamplers[D.sampIdx.w]),
                            NativeTurn(fract(m.uv[0]), c.y), uvx, uvy).xyz * 2.0 - 1.0;
        } else {
            t = TerrainLay(top, bindlessSamplers[D.sampIdx.w], face, m, i).xyz * 2.0 - 1.0;
        }
        t.z = max(t.z, 0.05);
        vec2 slope = vec2(-t.x, t.y) / t.z * (i == 0 && !m.clutter ? topStrength : faceStrength);
        float worldPerUv = sqrt(max(dot(m.px, m.px), dot(m.py, m.py)) / max(max(dot(uvx, uvx), dot(uvy, uvy)), 1e-12));
        sum += BendNormal(n, m.px, m.py, dot(slope, uvx) * worldPerUv, dot(slope, uvy) * worldPerUv) * m.w[i];
    }
    float len2 = dot(sum, sum);
    return len2 > 0.0 ? sum * (length(n) * inversesqrt(len2)) : n;
}

// Large-scale variation in the ground's colour: patches a few tens to hundreds of units across, a little lighter or
// darker, more or less saturated (x FL.terrain.z). Laid as the texture is: from above on flat ground, on the face
// along x or z and down where it is steep (m.w) - from above only, a cliff took the same value all the way down and
// the patches ran down it in streaks.
vec3 TerrainPatches(vec2 p)    // tone, saturation (-0.5 .. 0.5)
{
    float big = TerrainNoise(p / 190.0 + 31.7) - 0.5, mid = TerrainNoise(p / 47.0 - 11.3) - 0.5;
    return vec3(0.65 * big + 0.35 * mid, TerrainNoise(p / 113.0 + 77.1) - 0.5, 0.0);
}

// The patches at a point, kept for the fragment (k: 0 the top, 1 the faces along x, 2 along z): the one-pass ground
// varies the same point's colour twice - its base, and its material maps' albedo - and each is 3 noises of 4 hashes,
// up to 3 times over on the faces (the profiling sweep: the variation 0.28 ms of a 6.9 ms frame, 2026-10-20).
vec3 gPatches[3];
vec3 gPatchesAt = vec3(3.0e38);
uint gPatchesHave = 0u;
vec3 TerrainPatchesAt(int k, vec3 posW)
{
    if (posW != gPatchesAt) {
        gPatchesAt = posW;
        gPatchesHave = 0u;
    }
    if ((gPatchesHave & (1u << k)) == 0u) {
        gPatchesHave |= 1u << k;
        gPatches[k] = TerrainPatches(k == 0 ? posW.xz : k == 1 ? vec2(posW.x, -posW.y) : vec2(posW.z, -posW.y));
    }
    return gPatches[k];
}

vec3 TerrainVariation(vec3 c, vec3 posW, TerrainMap m)
{
    float k = FL.terrain.z;
    if (k <= 0.0) return c;
    vec3 v = TerrainPatchesAt(0, posW) * m.w.x;
    if (m.w.y > 0.002) v += TerrainPatchesAt(1, posW) * m.w.y;
    if (m.w.z > 0.002) v += TerrainPatchesAt(2, posW) * m.w.z;
    float l = Luma(c);
    c = mix(vec3(l), c, clamp(1.0 + 0.35 * k * v.y, 0.0, 2.0));
    return c * clamp(1.0 + 0.26 * k * v.x, 0.5, 1.5);
}

// Fine grain on the ground up close (FL.terrain2.w, RVK_GroundGrain): a tile's texel is a few centimetres, and at the
// feet the ground was a soft smear. The rock's fine detail at two small sizes laid across each other (0.9 and 1.37
// units, turned apart - neither repeats in rows): each read against the same spot three levels coarser, so only its
// grit is left, not its bands and patches; from above only (the faces have the rock itself), fading out as a repeat
// shrinks to a few dozen pixels.
float TerrainGrit(vec2 uv, vec2 dx, vec2 dy)
{
    const float lod = max(log2(max(length(dx), length(dy)) * 1024.0), 0.0);
    const float fine = Luma(TERRAIN_FACE_READ(texImages[TerrainRockImage(0)], uv, dx, dy).rgb);
    const float coarse = Luma(textureLod(sampler2D(texImages[TerrainRockImage(0)], bindlessSamplers[D.sampIdx.w]), uv,
                                         lod + 3.0).rgb);
    return fine / max(coarse, 0.02);
}

vec3 TerrainGrain(vec3 c, TerrainMap m, vec3 posW)
{
    // (Not on loose sand: its ripples are its detail - parallax.glsl. Nor on a tile with maps of its own - a texture
    // pack's, made for it: the rock's grit lay over it as faint scratches, and over one drawn in depth it didn't move
    // with the relief - the user's athens tile. posW: where the parallax met the ground, so it would.)
    const float k = FL.terrain2.w * m.w.x * (1.0 - NativeSand(m.uv[0]) * clamp(FL.sand.x, 0.0, 1.0));
    if (k <= 0.002 || !TerrainRockOn() || m.clutter) return c;
    if (TerrainNativeMaps() && NativeNormalImage(m.uv[0]) != 0xFFFFu) return c;
    const mat2 turn = mat2(0.8, 0.6, -0.6, 0.8);
    const vec2 a = posW.xz / 0.9, adx = m.px.xz / 0.9, ady = m.py.xz / 0.9;
    const float fade = 1.0 - smoothstep(0.012, 0.04, max(length(adx), length(ady)));
    if (fade <= 0.0) return c;
    const float g = sqrt(TerrainGrit(a, adx, ady) * TerrainGrit(turn * posW.xz / 1.37, turn * m.px.xz / 1.37,
                                                                 turn * m.py.xz / 1.37));
    return c * clamp(1.0 + 0.8 * k * fade * (g - 1.0), 0.75, 1.25);
}

// The native ground's lightmap at lightmap coordinates at: the hour's (stage 0), faded in from the last hour's
// (FL.terrain2.y, z: the game swaps them whole, and the land's light jumped).
vec4 NativeLightmapAt(vec2 at)
{
    const vec4 now = textureLod(TEX0, at, 0.0);
    const uint prev = floatBitsToUint(FL.terrain2.y);
    if (prev == 0u) return now;
    return mix(textureLod(sampler2D(texImages[prev - 1u], bindlessSamplers[D.sampIdx.x]), at, 0.0), now, FL.terrain2.z);
}

// The native ground's slopes lit from where the sun stands now: the lightmap is a + b.n of the slope n as the game
// baked it with the sun of its hour (rvk native_ground.cpp FitNativeLight: FL.terrainLight2.xyz b), and with b turned
// to the sun's bearing now (FL.terrainLight.xyz; its height as baked) the ratio of the two is how much lighter or
// darker this slope is now - flat ground 1, as baked; a slope that turns towards the sun through the hour brightens,
// one that turns away darkens (by FL.terrainLight2.w, RVK_GroundSun). Only as far as the sun reached the texel (lit:
// its value, as the fit read it): in a baked shadow what is left is the sky's light, which comes from no one side.
// (In the 0..1 of the texels; floored at the palette's darkest, the ratio clamped.)
// How much of the baked sun a texel has: 0 in a baked shadow .. 1 lit as the fit says the slope n (ground: the flat
// ground under them) is.
float NativeSunShare(vec3 n, bool ground, float lit)
{
    if (FL.terrainLight.w <= 0.0 || dot(n, n) <= 1e-8) return 1.0;
    const vec3 b = FL.terrainLight2.xyz;
    const float baked = FL.terrainLight.w + (ground ? b.y : dot(b, normalize(n)));
    return smoothstep(0.55, 0.85, lit / max(baked, 0.06));
}
float NativeSunTurn(vec3 n, float lit)
{
    if (FL.terrainLight.w <= 0.0 || dot(n, n) <= 1e-8 || FL.terrainLight2.w <= 0.0) return 1.0;
    n = normalize(n);
    const vec3 b = FL.terrainLight2.xyz, now = mix(b, FL.terrainLight.xyz, FL.terrainLight2.w);
    const float baked = FL.terrainLight.w + dot(b, n);
    const float turned = clamp(max(FL.terrainLight.w + dot(now, n), 0.06) / max(baked, 0.06), 0.4, 2.0);
    return mix(1.0, turned, NativeSunShare(n, false, lit));
}

// The rocks on the native ground (TerrainClutter) take the ground's light where they lie - the flat ground's - shaped
// by their own faces: where the bake says the sun reached, part of it is the sun's (kRockSun), lit by the face's
// smooth normal against the frame's sun (Lambert, over the flat ground's), its live shadow (vis) taking only that
// part; the rest the sky's, a little darker on their sides and undersides. Lit from the lightmap's fit instead they
// had almost no shape of their own (the fit's slopes all face up: it can't say how a face turned down or away is
// lit, and one hour's lit their undersides three times their tops) - their only shading the shadow map's, which at a
// low sun shadowed a rock's own faces barely turned to it a triangle at a time: hard-edged facets.
float NativeRockLight(vec3 n, float lit, float vis)
{
    const float kRockSun = 0.6;
    if (dot(n, n) <= 1e-8) return 1.0;
    n = normalize(n);
    const float sky = clamp(0.8 + 0.25 * n.y, 0.55, 1.05);
    const vec3 S = dot(FL.sunDir.xyz, FL.sunDir.xyz) > 0.0 ? -normalize(FL.sunDir.xyz) : vec3(0.0);
    if (S.y <= 0.02) return sky;
    const float sun = clamp(max(dot(n, S), 0.0) / max(S.y, 0.3), 0.0, 2.0) * vis;
    const float k = kRockSun * NativeSunShare(n, true, lit);
    return (1.0 - k) * sky + k * sun;
}

// The native ground's loose sand (NativeSand) lit by the sun now on its dunes' own slopes (FL.sand.y, RVK_DuneLight):
// the game's bake has a large flat share of sky in it, so a dune's lee was barely darker than its sunny face at noon -
// the dunes a smooth felt. Here, as for the rocks, part of the light where the bake says the sun reached (kSandSun) is
// the sun's, by the slope's Lambert against the flat ground's (flat sand stays as baked); the lee falls towards the
// sky's level, a little cooler (the blue sky lights it). A factor on the lightmap; n the smooth (interpolated) normal.
vec3 NativeSandLight(vec3 n, float lit)
{
    const float kSandSun = 0.75;
    if (dot(n, n) <= 1e-8 || dot(FL.sunDir.xyz, FL.sunDir.xyz) <= 0.0) return vec3(1.0);
    n = normalize(n);
    const vec3 S = -normalize(FL.sunDir.xyz);
    if (S.y <= 0.02) return vec3(1.0);
    const float k = kSandSun * NativeSunShare(n, true, lit) * smoothstep(0.02, 0.12, S.y);   // (gone as the sun sets)
    const float sun = clamp(dot(n, S) / max(S.y, 0.25), 0.0, 1.6);
    const float f = (1.0 - k) + k * sun;
    return f * mix(vec3(1.0), vec3(0.92, 0.97, 1.1), clamp(1.0 - f, 0.0, 1.0) * 0.8);
}

// The native ground's material (occlusion, roughness, metal) laid as its colour is: from above the cell's tile's own
// map, turned as the tile is; on the faces the rock's (or the cell's tile's, laid on them); the rocks on it the
// rock's from every side. w: how much of the point has a material at all (a cell whose tile has no map: none there -
// it is lit as before, without PBR).
vec3 NativeOrm(TerrainMap m, out float w)
{
    vec3 sum = vec3(0.0);
    w = 0.0;
    if (!TerrainNativeMaps()) return vec3(1.0, 1.0, 0.0);
    const uint rockOrm = m.rock ? TerrainRockImage(2) : 0xFFFFu;
    if (m.w.x > 0.002) {
        if (m.clutter) {
            sum += TERRAIN_FACE_READ(texImages[rockOrm], m.top, m.topDx, m.topDy).rgb * m.w.x;
            w += m.w.x;
        } else {
            const uint img = NativeOrmImage(m.uv[0]);
            if (img != 0xFFFFu) {
                const uvec2 c = NativeCell(m.uv[0]);
                sum += textureGrad(sampler2D(texImages[nonuniformEXT(img)], bindlessSamplers[D.sampIdx.w]),
                                   NativeTurn(fract(m.uv[0]), c.y), NativeTurnStep(m.dx[0], c.y),
                                   NativeTurnStep(m.dy[0], c.y)).rgb * m.w.x;
                w += m.w.x;
            }
        }
    }
    const uint faceOrm = m.rock ? rockOrm : NativeOrmImage(m.uv[0]);
    if (faceOrm != 0xFFFFu)
        for (int i = 1; i < 3; ++i)
            if (m.w[i] > 0.002) {
                sum += TERRAIN_FACE_READ(texImages[nonuniformEXT(faceOrm)], m.uv[i], m.dx[i], m.dy[i]).rgb * m.w[i];
                w += m.w[i];
            }
    return w > 0.0 ? sum / w : vec3(1.0, 1.0, 0.0);
}

#include "parallax.glsl"
