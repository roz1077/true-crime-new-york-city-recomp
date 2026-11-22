#include "game_addresses.h"
// Mirrors: show the reflection the game renders, not the blurred copy it adds to the mirror.
//
// This deliberately changes the Xbox release's look so it matches the PS2 version. It is not a port bug fix.
//
// A mirror (the locker room's, for one) is drawn in four steps, all on the screen-quad path (DrawVerticesUP):
//  1. the mirror's view is rendered into a 256x256 target: sharp and correct;
//  2. the mirror's backing writes its mask into the frame's alpha;
//  3. the reflection is shrunk to 128x128 (blend CONSTANT_ALPHA, ZERO) and run through the game's soften routine,
//     ping-pong passes through 128x128 and 96x96 targets (CONSTANT_ALPHA, ONE_MINUS_CONSTANT_ALPHA). The game's
//     soft shadows go through the same routine;
//  4. a full-screen quad adds the softened image where the mask is (DST_ALPHA, ONE).
// The Xbox mirror therefore shows a faint, smeared ghost, while the PS2 version shows a solid, sharp reflection.
//
// Step 3's shrink records which full-size target the small one came from. Step 4 then binds that full-size target
// instead, scaling the quad's texel coordinates (linear targets are addressed in texels) to its size. The soften
// passes still run, so the game's state is unchanged; only what the mirror shows differs. Soft shadows never reach
// step 4's blend, so they keep their softening.

#include <cstdint>
#include <cstring>
#include <unordered_map>

#include "core/log.h"
#include "gpu/state.h"
#include "gpu/texture.h"
#include "kernel/memory.h"

using XbeUpDrawFilterFn = void (*)(gpu::State &, uint32_t prim, uint32_t count, uint8_t *vertices, uint32_t stride);
XbeUpDrawFilterFn &XbeUpDrawFilter();  // xdk/d3d8.cpp

namespace {

constexpr uint32_t kZero = 0x0;
constexpr uint32_t kOne = 0x1;
constexpr uint32_t kDstAlpha = 0x304;
constexpr uint32_t kConstantAlpha = 0x8003;
constexpr uint32_t kQuads = 8;            // D3DPT_QUADLIST
constexpr uint32_t kScreenVertex = 24;    // x y z rhw u v
constexpr uint32_t kTexcoordOffset = 16;

constexpr uint32_t kHeaderBytes = 20;     // D3DPixelContainer: Common, Data, Lock, Format, Size

struct Source {
    // a copy of the full-size reflection's texture header, ours to keep: the one the game binds is not still
    // describing the reflection when the mirror is drawn
    uint32_t header = 0;
    uint32_t width = 0, height = 0;
};

// the shrunk target (guest virtual address of its data) -> the reflection it was shrunk from
std::unordered_map<uint32_t, Source> g_sources;

void MirrorFilter(gpu::State &s, uint32_t prim, uint32_t count, uint8_t *vertices, uint32_t stride) {
    if (!gpu::SimpleState(s, gpu::kMBlendEnable) || !s.textures[0]) return;
    const uint32_t src = gpu::SimpleState(s, gpu::kMSrcBlend), dst = gpu::SimpleState(s, gpu::kMDestBlend);
    const bool shrink = src == kConstantAlpha && dst == kZero;
    const bool add = src == kDstAlpha && dst == kOne;
    if (!shrink && !add) return;

    gpu::TextureDesc tex;
    if (!gpu::Describe(s.textures[0], tex) || !tex.linear) return;

    if (shrink) {
        gpu::TextureDesc rt;
        if (!gpu::DescribeSurface(s.rt, rt)) return;
        if (tex.width > rt.width && tex.height > rt.height) {
            Source &from = g_sources[rt.data];
            if (!from.header) from.header = mem::AllocSystem(kHeaderBytes);
            std::memcpy(mem::Base() + from.header, mem::Base() + s.textures[0], kHeaderBytes);
            from.width = tex.width;
            from.height = tex.height;
        } else {
            g_sources.erase(rt.data);
        }
        return;
    }

    const auto it = g_sources.find(tex.data);
    if (it == g_sources.end() || prim != kQuads || count != 4 || stride != kScreenVertex) return;
    const Source &from = it->second;
    const float sx = float(from.width) / float(tex.width), sy = float(from.height) / float(tex.height);
    for (uint32_t i = 0; i < count; i++) {
        float uv[2];
        uint8_t *p = vertices + i * stride + kTexcoordOffset;
        std::memcpy(uv, p, sizeof(uv));
        uv[0] *= sx;
        uv[1] *= sy;
        std::memcpy(p, uv, sizeof(uv));
    }
    s.textures[0] = from.header;
    LOG_FIRST(1, "mirror: reflection drawn from its %ux%u render, not the %ux%u softened copy", from.width,
              from.height, tex.width, tex.height);
}

const bool g_installed = [] {
    ::XbeUpDrawFilter() = MirrorFilter;
    return true;
}();

} // namespace
