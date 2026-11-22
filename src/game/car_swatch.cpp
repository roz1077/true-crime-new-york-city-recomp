#include "game_addresses.h"
#include "core/dev_env.h"
// Car paint swatches: make the panels drawn with a paint texture match the rest of the body.
//
// This corrects the original game, not a port bug. The Xbox release shows the same patches, only less clearly.
//
// A car's body is drawn with paint materials of two kinds (one stage record each: +0 flags, +2 texture
// slot, +4 colour):
//  - colour paint: flags 0x4001, +4 is an offset into the model's colour palette. The mesh draw 0x23DC60
//    reads [model] + offset + 4 * colour index (0x23E1D5), the index being word 0 of the car's colour
//    record, and sends it to the vertex shader as c[187] (0x23E294). No texture.
//  - textured paint: colour white, slot 0x8000 | base << 4. The texture lookup 0x23DAD0 picks the model's
//    texture base + colour index: one flat 256x256 DXT1 "swatch" per palette entry, in palette order.
// Two things make the textured panels differ from the body around them:
//  1. Each swatch only approximates its palette entry: (96,115,142) for (101,118,142), (212,178,46) for
//     (212,182,62), (24,47,76) for (29,49,76).
//  2. The car vertex shader writes min(light * c[187], 1). With c[187] white the light is clamped before
//     the swatch colour is applied, so wherever the light is above 1 (sunlit, or a dark paint lit by the
//     game's lights) the panel is darker than the body: (19,18,21) against (20,21,23) on a black car.
// So a swatch panel is drawn as colour paint: its swatch is rewritten to white once, and when the draw
// sends the material colour (c[187] at 0x23E29F or 0x23E2E6, or the texture factor at 0x23EBC8 on the
// fixed-function path) the car's palette entry goes in place of white. The panel then gets exactly the
// body's colour and lighting.
//
// Only a swatch is changed: a texture of one repeated block, whose colour is within kMaxDrift of the palette
// entry, on a white material. The palette offset comes from the model's colour paint, which goes through
// the same texture lookup. XBE_CAR_SWATCH=0 keeps the game's swatches.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "core/log.h"
#include "gpu/texture.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

X86_EXTERN_FUNC(__imp__sub_0023DAD0);
X86_EXTERN_FUNC(__imp__sub_002BBB10);

using XbeTextureFactorFilterFn = uint32_t (*)(X86Context &, uint32_t value);
XbeTextureFactorFilterFn &XbeTextureFactorFilter();  // xdk/d3d8.cpp

namespace {

constexpr uint16_t kColourPaint = 0x4001;     // stage flags: palette colour, colour on
constexpr uint16_t kSlotPerColour = 0x8000;   // texture slot: chosen through the car's colour record
constexpr uint16_t kSlotGlobal = 0x4000;      // texture slot: global table instead of the model's
constexpr uint32_t kDxt1 = 0x0C;              // NV2A texture format
constexpr uint32_t kEntryBytes = 36;          // the model's texture table entries ([model + 0x10])
constexpr int kMaxDrift = 24;                 // per channel, swatch to palette entry
constexpr uint32_t kMaterialColour = 0xBB;    // c[187]
constexpr uint64_t kWhiteBlock = 0x00000000FFFFFFFFull;  // c0 = c1 = 0xFFFF, index 0

const uint32_t kColourSent = TC_ADDR(0x23E2A4);    // return of the c[187] write for a stage with colour
const uint32_t kNoColourSent = TC_ADDR(0x23E2EB);  // return of the c[187] write for a stage without
const uint32_t kFactorSent = TC_ADDR(0x23EBCD);    // return of the texture factor write (fixed function)

bool Enabled() {
    static const bool on = [] {
        const char *e = DevEnv("XBE_CAR_SWATCH");
        return !(e && e[0] == '0');
    }();
    return on;
}

uint32_t R32(uint32_t a) {
    uint32_t v;
    std::memcpy(&v, mem::Base() + a, 4);
    return v;
}

uint16_t R16(uint32_t a) {
    uint16_t v;
    std::memcpy(&v, mem::Base() + a, 2);
    return v;
}

int Expand5(int v) { return (v << 3) | (v >> 2); }
int Expand6(int v) { return (v << 2) | (v >> 4); }

// what a DXT1 block of one index decodes to, false if the block mixes indices or is transparent
bool BlockColour(const uint8_t *b, float rgb[3]) {
    uint16_t c0, c1;
    uint32_t idx;
    std::memcpy(&c0, b, 2);
    std::memcpy(&c1, b + 2, 2);
    std::memcpy(&idx, b + 4, 4);
    const uint32_t i = idx & 3;
    if (idx != i * 0x55555555u) return false;
    const float a[3] = {float(Expand5(c0 >> 11)), float(Expand6((c0 >> 5) & 63)), float(Expand5(c0 & 31))};
    const float z[3] = {float(Expand5(c1 >> 11)), float(Expand6((c1 >> 5) & 63)), float(Expand5(c1 & 31))};
    for (int ch = 0; ch < 3; ++ch) {
        switch (i) {
        case 0: rgb[ch] = a[ch]; break;
        case 1: rgb[ch] = z[ch]; break;
        case 2: rgb[ch] = c0 > c1 ? (2 * a[ch] + z[ch]) / 3 : (a[ch] + z[ch]) / 2; break;
        default:
            if (c0 <= c1) return false;  // transparent black
            rgb[ch] = (a[ch] + 2 * z[ch]) / 3;
        }
    }
    return true;
}

std::mutex g_lock;
std::unordered_map<uint32_t, uint32_t> g_palette;  // model -> palette offset of its colour paint
std::unordered_map<uint32_t, bool> g_white;        // swatch data that this file made white

// the palette colour (ARGB) a stage's swatch stands for, with the swatch made white; false for anything else
bool SwatchPaint(uint32_t model, uint32_t stage, uint32_t colour, uint32_t &argb) {
    if (!model || !stage || !colour || !mem::IsCommitted(model) || !mem::IsCommitted(stage) ||
        !mem::IsCommitted(colour))
        return false;
    const uint16_t slot = R16(stage + 2);
    if (slot == 0xFFFF || (slot & (kSlotPerColour | kSlotGlobal)) != kSlotPerColour || (slot & 0xF) != 0)
        return false;
    std::lock_guard<std::mutex> lock(g_lock);
    const auto pal = g_palette.find(model);
    if (pal == g_palette.end()) return false;  // its colour paint has not been looked up yet
    const uint32_t index = R16(colour), texture = index + ((slot >> 4) & 0x3FF);
    if (int(texture) >= int16_t(R16(model + 0x30))) return false;
    const uint32_t res = R32(model + 0x10) + texture * kEntryBytes + 4;  // 0x243DF0 binds entry + 4
    if (!mem::IsCommitted(res + 0x10)) return false;
    const uint32_t format = R32(res + 0x0C);
    if (((format >> 8) & 0xFF) != kDxt1 || ((format >> 4) & 0xF) != 2) return false;
    const uint32_t data = mem::VirtualFromPhysical(R32(res + 4));
    if (!data || !mem::IsCommitted(data)) return false;
    argb = R32(R32(model) + pal->second + 4u * index);
    uint8_t *p = mem::Base() + data;
    uint64_t first;
    std::memcpy(&first, p, 8);
    if (first == kWhiteBlock && g_white.count(data)) return true;

    const uint32_t levels = std::max(1u, (format >> 16) & 0xF);
    uint32_t w = 1u << ((format >> 20) & 0xF), h = 1u << ((format >> 24) & 0xF), blocks = 0;
    for (uint32_t l = 0; l < levels; ++l, w = std::max(1u, w / 2), h = std::max(1u, h / 2))
        blocks += std::max(1u, w / 4) * std::max(1u, h / 4);
    if (blocks < 64 || !mem::IsCommitted(data + blocks * 8 - 1)) return false;
    for (uint32_t b = 1; b < blocks; ++b)
        if (std::memcmp(p + b * 8, p, 8) != 0) return false;  // a real texture, not a swatch
    float have[3];
    if (!BlockColour(p, have)) return false;
    const int want[3] = {int(argb >> 16 & 0xFF), int(argb >> 8 & 0xFF), int(argb & 0xFF)};
    for (int ch = 0; ch < 3; ++ch)
        if (std::fabs(have[ch] - float(want[ch])) > kMaxDrift) return false;  // not this palette entry

    for (uint32_t b = 0; b < blocks; ++b) std::memcpy(p + b * 8, &kWhiteBlock, 8);
    g_white[data] = true;
    gpu::MarkDirty(res);
    LOG_FIRST(32, "car_swatch: %08X (%.0f,%.0f,%.0f) drawn as palette (%d,%d,%d)", data, have[0], have[1], have[2],
              want[0], want[1], want[2]);
    return true;
}

// the stage and car of the mesh draw 0x23DC60 at its colour writes: edi = stage, [ebp+0xC] model,
// [ebp+0x10] colour record
bool DrawSwatch(const X86Context &ctx, uint32_t &argb) {
    const uint32_t frame = ctx.ebp;
    if (!mem::IsCommitted(frame + 0x10)) return false;
    return SwatchPaint(R32(frame + 0x0C), ctx.edi, R32(frame + 0x10), argb);
}

uint32_t SwatchFactor(X86Context &ctx, uint32_t value) {
    if (!Enabled() || (value & 0xFFFFFF) != 0xFFFFFF) return value;
    uint8_t *base = mem::Base();
    if (X86_MEM32(ctx.esp) != kFactorSent) return value;
    uint32_t argb;
    if (!DrawSwatch(ctx, argb)) return value;
    return (value & 0xFF000000u) | (argb & 0xFFFFFF);
}

const bool g_installed = [] {
    ::XbeTextureFactorFilter() = SwatchFactor;
    return true;
}();

} // namespace

// 0x23DAD0: texture of one material stage, cdecl (model, stage, colour record), returns the texture entry or 0
X86_FUNC(sub_0023DAD0) {
    const uint32_t model = X86_MEM32(ctx.esp + 4);
    const uint32_t stage = X86_MEM32(ctx.esp + 8);
    __imp__sub_0023DAD0(ctx, base);
    if (!Enabled() || !model || !stage) return;
    if ((R16(stage) & kColourPaint) == kColourPaint) {
        std::lock_guard<std::mutex> lock(g_lock);
        g_palette[model] = R32(stage + 4);
    }
}

// 0x2BBB10: the D3D library's one-constant write, fastcall (ecx = register, edx = 4 floats)
X86_FUNC(sub_002BBB10) {
    if (ctx.ecx == kMaterialColour && Enabled()) {
        const uint32_t ret = X86_MEM32(ctx.esp);
        if (ret == kColourSent || ret == kNoColourSent) {
            float c[4];
            std::memcpy(c, base + ctx.edx, 16);
            uint32_t argb;
            if (c[0] >= 0.999f && c[1] >= 0.999f && c[2] >= 0.999f && DrawSwatch(ctx, argb)) {
                c[0] = float(argb >> 16 & 0xFF) / 255.0f;
                c[1] = float(argb >> 8 & 0xFF) / 255.0f;
                c[2] = float(argb & 0xFF) / 255.0f;
                std::memcpy(base + ctx.edx, c, 12);
            }
        }
    }
    __imp__sub_002BBB10(ctx, base);
}
