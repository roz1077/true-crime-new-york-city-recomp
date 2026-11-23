#include "game_addresses.h"

#include <cmath>
#include <cstdint>

#include "kernel/kernel.h"
#include "xdk/hle.h"

// Planar reflection fixes (USA 0x606F0):
//
// 1. Alpha-clear quad (0x247270): before each reflective surface, the game
//    clears framebuffer alpha (color write mask = alpha only, full-screen quad
//    with alpha = 0). 0x247270 checks a local cached blend mode at [ecx+4] and
//    skips disabling D3DRS_ALPHABLENDENABLE when that cache is 0. If the
//    preceding reflection scene left (SRC_ALPHA, INV_SRC_ALPHA) active, the
//    quad evaluates 0*0 + dst.a*(1-0) = dst.a and leaves earlier receivers'
//    alpha masks intact, stacking reflections up to 4x in the police garage.
//    Also, if [ecx+4] == 4, 0x247270 resets the color write mask to RGBA right
//    after setting it to alpha-only. Forcing [ecx+4] = 1 makes 0x247270 always
//    disable alpha blending while keeping the alpha-only write mask intact.
//
// 2. Mid-reflection depth loss (0x244080 / 0x2440F0): while rendering a planar
//    reflection scene into an offscreen 256x256 target with depth, a nested
//    offscreen pass (e.g. a 128x64 vehicle plate/decal target) switches render
//    targets and restores the reflection target through 0x244080. Because
//    0x244080 only attaches a depth surface when binding the backbuffer, the
//    rest of the reflection scene renders without depth testing and dark walls
//    overwrite earlier pillars and lights, making floor reflections flicker.
//    While inside 0x606F0 or the wet-road renderer (0x144250), re-binding the
//    active reflection colour target through 0x244080 restores its depth
//    surface via 0x2440F0.
//
// 3. Sharp vertical mirrors (0x606F0): the Xbox release blurs vertical mirrors
//    (like the locker room mirror) using [receiver+0x98] > 0, whereas PS2 shows
//    a sharp reflection. 0x606F0 already has a native blur==0 fast path that
//    skips the soften passes and composites the full 256x256 reflection. Zero
//    [receiver+0x98] temporarily only for vertical mirrors (|normal.y| < 0.5),
//    leaving horizontal floors and the wet-road renderer (0x144250) untouched.

namespace {

thread_local uint32_t g_refl_nesting = 0;
thread_local uint32_t g_last_rt_data = 0;
thread_local uint32_t g_refl_rt_data = 0;
thread_local uint32_t g_refl_ds_struct = 0;

bool IsVerticalReceiver(const X86Context &ctx, uint8_t *base, uint32_t receiver) {
    uint32_t node = receiver;
    if (!X86_MEM32(node + 0x2C)) {
        const uint32_t parent = X86_MEM32(node + 0x24);
        if (parent) node = parent;
    }
    uint32_t matrix = node + 0x40;
    if (X86_MEM32(node + 0x28)) {
        X86Context transform_ctx = ctx;
        matrix = (ctx.esp & ~0xFu) - 0x40;
        transform_ctx.esp = matrix;
        transform_ctx.ecx = node;
        kernel::CallGuest(transform_ctx, TC_ADDR(0x21EC70), {matrix});
    }
    const float ny = X86_MEMF32(matrix + 0x14);
    return std::fabs(ny) < 0.5f;
}

} // namespace

X86_EXTERN_FUNC(__imp__sub_00247270);
X86_FUNC(sub_00247270) {
    const uint32_t cached_blend = X86_MEM32(ctx.ecx + 4);
    if (cached_blend == 0 || cached_blend == 4) {
        X86_MEM32(ctx.ecx + 4) = 1;
    }
    __imp__sub_00247270(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_002440F0);
X86_FUNC(sub_002440F0) {
    if (g_refl_nesting > 0) {
        const uint32_t ds_struct = X86_MEM32(ctx.esp + 4);
        const uint8_t enable = X86_MEM8(ctx.esp + 12);
        if (enable && ds_struct && g_last_rt_data) {
            g_refl_rt_data = g_last_rt_data;
            g_refl_ds_struct = ds_struct;
        }
    }
    __imp__sub_002440F0(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_00244080);
X86_FUNC(sub_00244080) {
    const uint32_t rt_struct = X86_MEM32(ctx.esp + 4);
    const uint32_t rt_data = rt_struct ? X86_MEM32(rt_struct + 8) : 0;
    g_last_rt_data = rt_data;
    __imp__sub_00244080(ctx, base);
    if (g_refl_nesting > 0 && g_refl_ds_struct && rt_data && rt_data == g_refl_rt_data) {
        X86Context ds_ctx = ctx;
        ds_ctx.esp -= 16;
        X86_MEM32(ds_ctx.esp + 0) = 0;
        X86_MEM32(ds_ctx.esp + 4) = g_refl_ds_struct;
        X86_MEM32(ds_ctx.esp + 8) = 0;
        X86_MEM32(ds_ctx.esp + 12) = 1;
        __imp__sub_002440F0(ds_ctx, base);
    }
}

X86_EXTERN_FUNC(__imp__sub_000606F0);
X86_FUNC(sub_000606F0) {
    const uint32_t receiver = ctx.ecx;
    const bool vertical = receiver && IsVerticalReceiver(ctx, base, receiver);
    float saved_blur = 0.0f;
    if (vertical) {
        saved_blur = X86_MEMF32(receiver + 0x98);
        X86_MEMF32(receiver + 0x98) = 0.0f;
    }
    const uint32_t prev_rt = g_refl_rt_data;
    const uint32_t prev_ds = g_refl_ds_struct;
    g_refl_rt_data = 0;
    g_refl_ds_struct = 0;
    ++g_refl_nesting;
    __imp__sub_000606F0(ctx, base);
    --g_refl_nesting;
    g_refl_rt_data = prev_rt;
    g_refl_ds_struct = prev_ds;
    if (vertical) {
        X86_MEMF32(receiver + 0x98) = saved_blur;
    }
}

namespace tcnyc {
void ReflectionDepthEnter(uint32_t &prev_rt, uint32_t &prev_ds) {
    prev_rt = g_refl_rt_data;
    prev_ds = g_refl_ds_struct;
    g_refl_rt_data = 0;
    g_refl_ds_struct = 0;
    ++g_refl_nesting;
}

void ReflectionDepthLeave(uint32_t prev_rt, uint32_t prev_ds) {
    --g_refl_nesting;
    g_refl_rt_data = prev_rt;
    g_refl_ds_struct = prev_ds;
}
} // namespace tcnyc
