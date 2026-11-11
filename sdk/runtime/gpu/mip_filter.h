#pragma once
// Mipmaps for texture-pack images that come with only their top level (texturedumper.cpp and the Linux copy).
//
// Each level is a 2x2 box of the one above averaged in linear light, not on the stored sRGB values: averaging
// gamma-encoded values darkens fine bright detail (window lights, signs) and makes distant surfaces dimmer than
// up close. Alpha is averaged as stored.
//
// Cut-out textures (fences, leaves, wires: alpha nearly all clear or opaque) lose coverage at each level, because
// averaging pulls thin opaque parts under the alpha test and they vanish with distance. Their smaller levels have
// alpha scaled so the share of texels at or above half alpha stays what the top level has.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace gpu::mips {

inline const std::array<float, 256>& SrgbToLinearTable() {
    static const std::array<float, 256> table = [] {
        std::array<float, 256> t{};
        for (int i = 0; i < 256; i++) {
            const float c = float(i) / 255.0f;
            t[size_t(i)] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        return t;
    }();
    return table;
}

inline uint8_t LinearToSrgb(float l) {
    l = std::clamp(l, 0.0f, 1.0f);
    const float c = l <= 0.0031308f ? l * 12.92f : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
    return uint8_t(std::lround(c * 255.0f));
}

// The half-size level of a BGRA8 (or RGBA8) image, rows pitch bytes apart; its far row and column are clamped to
// the edge for odd sizes.
inline std::vector<uint8_t> HalfLevel(const uint8_t* src, uint32_t pitch, uint32_t w, uint32_t h, uint32_t& out_w,
                                      uint32_t& out_h) {
    const auto& lin = SrgbToLinearTable();
    out_w = std::max(w / 2, 1u);
    out_h = std::max(h / 2, 1u);
    std::vector<uint8_t> out(size_t(out_w) * out_h * 4);
    for (uint32_t y = 0; y < out_h; y++) {
        const uint8_t* row0 = src + size_t(std::min(y * 2, h - 1)) * pitch;
        const uint8_t* row1 = src + size_t(std::min(y * 2 + 1, h - 1)) * pitch;
        uint8_t* dst = &out[size_t(y) * out_w * 4];
        for (uint32_t x = 0; x < out_w; x++) {
            const uint32_t x0 = std::min(x * 2, w - 1) * 4, x1 = std::min(x * 2 + 1, w - 1) * 4;
            for (uint32_t c = 0; c < 3; c++) {
                const float sum = lin[row0[x0 + c]] + lin[row0[x1 + c]] + lin[row1[x0 + c]] + lin[row1[x1 + c]];
                dst[x * 4 + c] = LinearToSrgb(sum * 0.25f);
            }
            const uint32_t a = row0[x0 + 3] + row0[x1 + 3] + row1[x0 + 3] + row1[x1 + 3];
            dst[x * 4 + 3] = uint8_t((a + 2) / 4);
        }
    }
    return out;
}

// share of texels whose alpha times scale reaches 128
inline float Coverage(const uint8_t* px, uint32_t pitch, uint32_t w, uint32_t h, float scale) {
    uint64_t n = 0;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) n += float(px[size_t(y) * pitch + x * 4 + 3]) * scale >= 127.5f;
    return float(n) / float(uint64_t(w) * h);
}

// a cut-out: some clear and some opaque texels, and nearly nothing between
inline bool IsCutout(const uint8_t* px, uint32_t pitch, uint32_t w, uint32_t h) {
    uint64_t clear = 0, solid = 0, mid = 0;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t a = px[size_t(y) * pitch + x * 4 + 3];
            if (a < 16) clear++;
            else if (a > 239) solid++;
            else mid++;
        }
    const uint64_t total = uint64_t(w) * h;
    return clear && solid && mid * 10 < total;
}

// scales a level's alpha so its coverage matches target (a binary search on the scale)
inline void KeepCoverage(uint8_t* px, uint32_t pitch, uint32_t w, uint32_t h, float target) {
    float lo = 0.0f, hi = 4.0f;
    for (int i = 0; i < 12; i++) {
        const float mid = (lo + hi) * 0.5f;
        (Coverage(px, pitch, w, h, mid) < target ? lo : hi) = mid;
    }
    const float scale = hi;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint8_t& a = px[size_t(y) * pitch + x * 4 + 3];
            a = uint8_t(std::min(255.0f, std::round(float(a) * scale)));
        }
}

// Fills levels (each with px and pitch, the first the top level of width x height) down to 1x1.
template <class LevelT>
void BuildChain(std::vector<LevelT>& levels, uint32_t width, uint32_t height) {
    if (levels.empty()) return;
    const bool cutout = IsCutout(levels[0].px.data(), levels[0].pitch, width, height);
    const float coverage = cutout ? Coverage(levels[0].px.data(), levels[0].pitch, width, height, 1.0f) : 0.0f;
    uint32_t w = width, h = height;
    while (w > 1 || h > 1) {
        uint32_t nw = 0, nh = 0;
        LevelT next;
        next.px = HalfLevel(levels.back().px.data(), levels.back().pitch, w, h, nw, nh);
        next.pitch = nw * 4;
        if (cutout) KeepCoverage(next.px.data(), next.pitch, nw, nh, coverage);
        levels.push_back(std::move(next));
        w = nw;
        h = nh;
    }
}

}  // namespace gpu::mips
