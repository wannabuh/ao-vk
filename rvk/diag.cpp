// Frame dump: one line per 3D draw of a frame with the state that decides how it is lit, its world
// position and its screen rectangle, plus the lights it uses. For finding out why a surface is (not) lit.
#include "internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace rvk {

using namespace detail;

void Device::RequestFrameDump(const std::string& path)
{
    m_dumpPath = path;
}

void Device::BeginFrameDump()
{
    m_dumpFile = std::fopen(m_dumpPath.c_str(), "w");
    m_dumpPath.clear();
    if (!m_dumpFile) return;
    m_dumpDraw = 0;
    m_dumpedLights.assign(m_lights.size(), {});
    m_dumpedLightValid.assign(m_lights.size(), false);
    const auto& v = m_view.m;
    float eye[3];
    for (int i = 0; i < 3; ++i) eye[i] = -(v[3][0] * v[i][0] + v[3][1] * v[i][1] + v[3][2] * v[i][2]);
    std::fprintf(m_dumpFile, "# rvk frame dump; eye (%.2f %.2f %.2f); pixel lighting %d\n"
                             "# D draw: prim fvf verts idx | lit lights(idx:nearest vertex distance) | material diffuse/ambient/emissive,"
                             " sources d/a/s/e, colorvertex, global ambient | stages op/arg1/arg2 | tex | blend src/dst | z test/write/func |"
                             " world translation | screen rect\n"
                             "# L index: type position direction range atten0/1/2 diffuse ambient\n",
                 eye[0], eye[1], eye[2], m_pixelLighting ? 1 : 0);
}

void Device::EndFrameDump()
{
    if (!m_dumpFile) return;
    std::fprintf(m_dumpFile, "# end: %u draws\n", m_dumpDraw);
    std::fclose(m_dumpFile);
    m_dumpFile = nullptr;
}

static void Mul(const d3d::Matrix& m, const float in[4], float out[4])
{
    for (int c = 0; c < 4; ++c)
        out[c] = in[0] * m.m[0][c] + in[1] * m.m[1][c] + in[2] * m.m[2][c] + in[3] * m.m[3][c];
}

void Device::DumpDraw(uint32_t primitive, uint32_t fvf, const void* vertices, uint32_t vertexCount, uint32_t indexCount)
{
    uint32_t n = m_dumpDraw++;
    if ((fvf & d3d::FVF_POSITION_MASK) == d3d::FVF_XYZRHW) return;     // 2D
    FILE* f = m_dumpFile;
    FvfLayout layout = DecodeFvf(fvf);
    bool lit = m_rs[d3d::RS_LIGHTING] != 0;

    // Lights first, when new or changed since they were last printed. (SetLight may grow m_lights mid-frame.)
    if (m_dumpedLights.size() < m_lights.size()) {
        m_dumpedLights.resize(m_lights.size());
        m_dumpedLightValid.resize(m_lights.size(), false);
    }
    for (size_t i = 0; lit && i < m_lights.size(); ++i) {
        const LightSlot& s = m_lights[i];
        if (!s.enabled) continue;
        if (m_dumpedLightValid[i] && std::memcmp(&m_dumpedLights[i], &s.light, sizeof(d3d::Light)) == 0) continue;
        m_dumpedLights[i] = s.light;
        m_dumpedLightValid[i] = true;
        const d3d::Light& l = s.light;
        std::fprintf(f, "L %zu: type %u pos (%.2f %.2f %.2f) dir (%.2f %.2f %.2f) range %.2f att %.3f/%.4f/%.5f"
                        " diffuse (%.2f %.2f %.2f) ambient (%.2f %.2f %.2f)\n",
                     i, uint32_t(l.type), l.position.x, l.position.y, l.position.z, l.direction.x, l.direction.y,
                     l.direction.z, l.range, l.attenuation0, l.attenuation1, l.attenuation2, l.diffuse.r, l.diffuse.g,
                     l.diffuse.b, l.ambient.r, l.ambient.g, l.ambient.b);
    }

    // World positions -> nearest distance to each local light, and the screen rectangle.
    float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
    std::vector<float> nearest(m_lights.size(), 1e30f);
    const uint8_t* base = static_cast<const uint8_t*>(vertices);
    uint32_t step = std::max(1u, vertexCount / 4096);
    for (uint32_t i = 0; i < vertexCount; i += step) {
        const float* p = reinterpret_cast<const float*>(base + size_t(i) * layout.stride + layout.offset[0]);
        float o[4] = {p[0], p[1], p[2], 1.0f}, w[4], e[4], c[4];
        Mul(m_world, o, w);
        for (size_t k = 0; lit && k < m_lights.size(); ++k) {
            const LightSlot& s = m_lights[k];
            if (!s.enabled || s.light.type == d3d::LIGHT_DIRECTIONAL) continue;
            float dx = w[0] - s.light.position.x, dy = w[1] - s.light.position.y, dz = w[2] - s.light.position.z;
            nearest[k] = std::min(nearest[k], std::sqrt(dx * dx + dy * dy + dz * dz));
        }
        Mul(m_view, w, e);
        Mul(m_proj, e, c);
        if (c[3] <= 1e-4f) continue;
        float sx = m_viewport.x + (c[0] / c[3] * 0.5f + 0.5f) * m_viewport.width;
        float sy = m_viewport.y + (0.5f - c[1] / c[3] * 0.5f) * m_viewport.height;
        minX = std::min(minX, sx); maxX = std::max(maxX, sx);
        minY = std::min(minY, sy); maxY = std::max(maxY, sy);
    }

    std::fprintf(f, "D %u: prim %u fvf 0x%X v %u i %u | lit %d", n, primitive, fvf, vertexCount, indexCount, lit ? 1 : 0);
    if (lit) {
        std::fprintf(f, " lights");
        for (size_t k = 0; k < m_lights.size(); ++k) {
            if (!m_lights[k].enabled) continue;
            if (m_lights[k].light.type == d3d::LIGHT_DIRECTIONAL) std::fprintf(f, " %zu:dir", k);
            else std::fprintf(f, " %zu:%.1f", k, nearest[k]);
        }
        const d3d::Material& m = m_material;
        std::fprintf(f, " | mat d(%.2f %.2f %.2f %.2f) a(%.2f %.2f %.2f) e(%.2f %.2f %.2f) src %u/%u/%u/%u cv %u amb %08X",
                     m.diffuse.r, m.diffuse.g, m.diffuse.b, m.diffuse.a, m.ambient.r, m.ambient.g, m.ambient.b,
                     m.emissive.r, m.emissive.g, m.emissive.b, m_rs[d3d::RS_DIFFUSEMATERIALSOURCE],
                     m_rs[d3d::RS_AMBIENTMATERIALSOURCE], m_rs[d3d::RS_SPECULARMATERIALSOURCE],
                     m_rs[d3d::RS_EMISSIVEMATERIALSOURCE], m_rs[d3d::RS_COLORVERTEX], m_rs[d3d::RS_AMBIENT]);
    }
    std::fprintf(f, " | st0 %u/%X/%X st1 %u/%X/%X", m_tss[0][d3d::TSS_COLOROP], m_tss[0][d3d::TSS_COLORARG1],
                 m_tss[0][d3d::TSS_COLORARG2], m_tss[1][d3d::TSS_COLOROP], m_tss[1][d3d::TSS_COLORARG1],
                 m_tss[1][d3d::TSS_COLORARG2]);
    for (int s = 0; s < 2; ++s)
        if (m_textures[s]) std::fprintf(f, " tex%d %ux%u", s, m_textures[s]->Width(), m_textures[s]->Height());
    if (m_rs[d3d::RS_ALPHABLENDENABLE]) std::fprintf(f, " | blend %u/%u", m_rs[d3d::RS_SRCBLEND], m_rs[d3d::RS_DESTBLEND]);
    std::fprintf(f, " | z %u/%u/%u | at (%.1f %.1f %.1f)", m_rs[d3d::RS_ZENABLE], m_rs[d3d::RS_ZWRITEENABLE],
                 m_rs[d3d::RS_ZFUNC], m_world.m[3][0], m_world.m[3][1], m_world.m[3][2]);
    if (IsShadowCaster(primitive, fvf) || ShadowReceiver(fvf))
        std::fprintf(f, " | shadow %s%s", IsShadowCaster(primitive, fvf) ? "C" : "", ShadowReceiver(fvf) ? "R" : "");
    if (minX <= maxX) std::fprintf(f, " | rect %.0f,%.0f-%.0f,%.0f", minX, minY, maxX, maxY);
    else std::fprintf(f, " | offscreen");
    std::fprintf(f, "\n");
}

}  // namespace rvk
