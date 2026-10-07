// Side-loaded material maps (normal, occlusion/roughness/metallic, albedo) decoded into mipmapped rvk textures:
// shared by the proxy (proxy/ddraw/rvk_materials.cpp, which finds the files) and rvk_demo --pbr-test. Dev is
// rvk::Device or rvk::ThreadedDevice. Needs stb_image (its implementation compiled once elsewhere).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "rvk.h"
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "stb/stb_image.h"

namespace rvk::maps {

struct Image {
    std::vector<uint8_t> rgba;
    int w = 0, h = 0, channels = 0;
};

inline bool LoadImage(const std::string& path, Image* out, std::string* error)
{
    int n = 0;
    stbi_uc* px = stbi_load(path.c_str(), &out->w, &out->h, &n, 4);
    if (!px) {
        *error = "can't read " + path + " (" + stbi_failure_reason() + ")";
        return false;
    }
    out->channels = n;
    out->rgba.assign(px, px + size_t(out->w) * out->h * 4);
    stbi_image_free(px);
    return true;
}

inline uint32_t LevelCount(int w, int h)
{
    uint32_t levels = 1;
    for (int s = std::max(w, h); s > 1; s >>= 1) ++levels;
    return levels;
}

// A float image with c channels halved (2x2 box; odd sizes repeat their last row / column).
inline std::vector<float> Halve(const std::vector<float>& src, int w, int h, int c, int* nw, int* nh)
{
    *nw = std::max(w / 2, 1);
    *nh = std::max(h / 2, 1);
    std::vector<float> next(size_t(*nw) * *nh * c, 0.0f);
    for (int y = 0; y < *nh; ++y)
        for (int x = 0; x < *nw; ++x)
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx) {
                    int sx = std::min(x * 2 + dx, w - 1), sy = std::min(y * 2 + dy, h - 1);
                    for (int k = 0; k < c; ++k)
                        next[(size_t(y) * *nw + x) * c + k] += src[(size_t(sy) * w + sx) * c + k] * 0.25f;
                }
    return next;
}

inline uint32_t Byte(float v) { return uint32_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); }

// How far each mip level's averaged normals fall short of unit length (the normal map's bumps the level can no longer
// show): per level, its size and the lengths. Roughness maps widen their highlights by it.
struct NormalSpread {
    std::vector<std::vector<float>> length;
    std::vector<int> w, h;
    // The averaged normal's length at level `level` around (u, v) in 0..1 (1 = flat / unknown).
    float At(uint32_t level, float u, float v) const
    {
        if (level >= length.size()) return length.empty() ? 1.0f : At(uint32_t(length.size() - 1), u, v);
        int x = std::clamp(int(u * w[level]), 0, w[level] - 1), y = std::clamp(int(v * h[level]), 0, h[level] - 1);
        return length[level][size_t(y) * w[level] + x];
    }
};

// RGBA8 -> mip chain of A8R8G8B8 (BGRA bytes). Normal maps: each level averages the decoded normals of the level
// above and renormalises them, so distant surfaces don't flatten or tilt; the averaged lengths go to `spread`.
template <typename Dev>
Texture* LoadNormalMap(Dev& dev, const std::string& path, NormalSpread* spread, std::string* error)
{
    Image img;
    if (!LoadImage(path, &img, error))
        return nullptr;
    int w = img.w, h = img.h;
    std::vector<float> nrm(size_t(w) * h * 3);
    for (size_t i = 0; i < size_t(w) * h; ++i)
        for (int c = 0; c < 3; ++c)
            nrm[i * 3 + c] = img.rgba[i * 4 + c] / 127.5f - 1.0f;
    uint32_t levels = LevelCount(w, h);
    Texture* tex = dev.CreateTexture(uint32_t(w), uint32_t(h), Format::A8R8G8B8, levels);
    if (!tex)
        return nullptr;
    std::vector<uint32_t> out;
    for (uint32_t level = 0; level < levels; ++level) {
        out.resize(size_t(w) * h);
        std::vector<float> lengths(out.size());
        for (size_t i = 0; i < out.size(); ++i) {
            float x = nrm[i * 3], y = nrm[i * 3 + 1], z = nrm[i * 3 + 2];
            float len = std::sqrt(x * x + y * y + z * z);
            if (len < 1e-6f) { x = y = 0.0f; z = 1.0f; len = 1.0f; }
            lengths[i] = std::min(len, 1.0f);
            auto enc = [&](float v) { return Byte((v / len) * 0.5f + 0.5f); };
            out[i] = 0xFF000000u | enc(x) << 16 | enc(y) << 8 | enc(z);
        }
        if (level == 0)                              // level 0 is the map as painted (its lengths are quantisation)
            std::fill(lengths.begin(), lengths.end(), 1.0f);
        spread->length.push_back(std::move(lengths));
        spread->w.push_back(w);
        spread->h.push_back(h);
        dev.UpdateTexture(tex, level, 0, 0, uint32_t(w), uint32_t(h), out.data(), uint32_t(w) * 4);
        if (level + 1 == levels)
            break;
        int nw, nh;
        nrm = Halve(nrm, w, h, 3, &nw, &nh);
        w = nw;
        h = nh;
    }
    return tex;
}

// Occlusion / roughness / metallic -> mip chain (R occlusion, G roughness, B metallic). Occlusion and metallic
// average; roughness averages as GGX alpha squared (roughness^4), widened by what the normal map's bumps lose at the
// level (Toksvig-style, from the averaged normal's length via the vMF lobe: alpha^2 += 2 / kappa), so far away a bumpy
// glossy surface turns satin instead of sparkling.
template <typename Dev>
Texture* MakeOrmMap(Dev& dev, std::vector<float> orm, int w, int h, const NormalSpread& spread)
{
    for (size_t i = 0; i < size_t(w) * h; ++i) {          // work in alpha^2
        float r = orm[i * 3 + 1];
        orm[i * 3 + 1] = r * r * r * r;
    }
    uint32_t levels = LevelCount(w, h);
    Texture* tex = dev.CreateTexture(uint32_t(w), uint32_t(h), Format::A8R8G8B8, levels);
    if (!tex)
        return nullptr;
    // The normal map's level matching each of these levels: by size (the two maps' sizes needn't match).
    int normalW0 = spread.w.empty() ? w : spread.w[0], ormW0 = w;
    std::vector<uint32_t> out;
    for (uint32_t level = 0; level < levels; ++level) {
        out.resize(size_t(w) * h);
        uint32_t normalLevel = 0;
        while (normalLevel + 1 < spread.w.size() && spread.w[normalLevel] > w * normalW0 / ormW0)
            ++normalLevel;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                size_t i = size_t(y) * w + x;
                float a2 = orm[i * 3 + 1];
                float len = spread.At(normalLevel, (x + 0.5f) / w, (y + 0.5f) / h);
                if (len < 0.9999f) {
                    float kappa = (3.0f * len - len * len * len) / (1.0f - len * len);
                    a2 = std::min(a2 + 2.0f / kappa, 1.0f);
                }
                float rough = std::sqrt(std::sqrt(a2));
                out[i] = 0xFF000000u | Byte(orm[i * 3]) << 16 | Byte(rough) << 8 | Byte(orm[i * 3 + 2]);
            }
        dev.UpdateTexture(tex, level, 0, 0, uint32_t(w), uint32_t(h), out.data(), uint32_t(w) * 4);
        if (level + 1 == levels)
            break;
        int nw, nh;
        orm = Halve(orm, w, h, 3, &nw, &nh);
        w = nw;
        h = nh;
    }
    return tex;
}

// The material's maps: one packed occlusion/roughness/metallic image, or separate greyscale ones (parts: occlusion,
// roughness, metallic; empty = 1, 1, 0), resampled to the largest. `from` names what was read.
template <typename Dev>
Texture* LoadOrmMap(Dev& dev, const std::string& packed, const std::string parts[3], const NormalSpread& spread,
                    std::string* from, std::string* error)
{
    if (!packed.empty()) {
        Image img;
        if (!LoadImage(packed, &img, error))
            return nullptr;
        std::vector<float> orm(size_t(img.w) * img.h * 3);
        for (size_t i = 0; i < size_t(img.w) * img.h; ++i)
            for (int c = 0; c < 3; ++c)
                orm[i * 3 + c] = img.rgba[i * 4 + c] / 255.0f;
        *from = packed;
        return MakeOrmMap(dev, std::move(orm), img.w, img.h, spread);
    }
    const float defaults[3] = {1.0f, 1.0f, 0.0f};
    Image images[3];
    int w = 0, h = 0;
    for (int c = 0; c < 3; ++c) {
        if (parts[c].empty())
            continue;
        if (!LoadImage(parts[c], &images[c], error))
            return nullptr;
        w = std::max(w, images[c].w);
        h = std::max(h, images[c].h);
        *from += (from->empty() ? "" : " + ") + parts[c];
    }
    if (!w)
        return nullptr;
    std::vector<float> orm(size_t(w) * h * 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c) {
                const Image& p = images[c];
                float v = defaults[c];
                if (p.w)                             // nearest texel; the red channel (greyscale)
                    v = p.rgba[(size_t(y * p.h / h) * p.w + size_t(x * p.w / w)) * 4] / 255.0f;
                orm[(size_t(y) * w + x) * 3 + c] = v;
            }
    return MakeOrmMap(dev, std::move(orm), w, h, spread);
}

// Albedo: RGBA mip chain (box filtered, colour weighted by alpha so see-through texels don't darken the edges).
template <typename Dev>
Texture* LoadAlbedoMap(Dev& dev, const std::string& path, std::string* error)
{
    Image img;
    if (!LoadImage(path, &img, error))
        return nullptr;
    int w = img.w, h = img.h;
    std::vector<float> px(size_t(w) * h * 4);
    for (size_t i = 0; i < size_t(w) * h; ++i) {
        float a = img.rgba[i * 4 + 3] / 255.0f;
        for (int c = 0; c < 3; ++c) px[i * 4 + c] = img.rgba[i * 4 + c] / 255.0f * a;
        px[i * 4 + 3] = a;
    }
    uint32_t levels = LevelCount(w, h);
    Texture* tex = dev.CreateTexture(uint32_t(w), uint32_t(h), Format::A8R8G8B8, levels);
    if (!tex)
        return nullptr;
    std::vector<uint32_t> out;
    for (uint32_t level = 0; level < levels; ++level) {
        out.resize(size_t(w) * h);
        for (size_t i = 0; i < out.size(); ++i) {
            float a = px[i * 4 + 3], k = a > 1e-6f ? 1.0f / a : 0.0f;
            out[i] = Byte(a) << 24 | Byte(px[i * 4] * k) << 16 | Byte(px[i * 4 + 1] * k) << 8 | Byte(px[i * 4 + 2] * k);
        }
        dev.UpdateTexture(tex, level, 0, 0, uint32_t(w), uint32_t(h), out.data(), uint32_t(w) * 4);
        if (level + 1 == levels)
            break;
        int nw, nh;
        px = Halve(px, w, h, 4, &nw, &nh);
        w = nw;
        h = nh;
    }
    return tex;
}

}  // namespace rvk::maps
