// 0x24C6E0: dst[i] = a rows times b row i, a 4x4 float matrix multiply. The Deck title-thread profile put
// it at ~9.5% of the thread, the largest single entry: the recompiled code runs every SSE op as a four-lane
// scalar loop, while the console's own sequence is a handful of instructions.
//
// This override is the guest's instruction sequence transcribed one-for-one (same shuffles, same add
// grouping, same order of loads and stores, so aliased arguments behave the same), which makes it
// bit-exact rather than merely close: float addition is not associative, and swapping the grouping would
// change results.
#include <immintrin.h>

#include "game_addresses.h"
#include "kernel/kernel.h"

namespace {
// the guest works in console memory; rows may be only 4-byte aligned in theory, so load/store unaligned
inline __m128 LoadRow(uint8_t* base, uint32_t va) {
    return _mm_loadu_ps(reinterpret_cast<const float*>(base + va));
}
inline void StoreRow(uint8_t* base, uint32_t va, __m128 v) {
    _mm_storeu_ps(reinterpret_cast<float*>(base + va), v);
}
}  // namespace

namespace game {
void MatrixMul4x4(X86Context& ctx, uint8_t* base) {
    const uint32_t dst = kernel::R32(ctx.esp + 4);
    const uint32_t a = kernel::R32(ctx.esp + 8);
    const uint32_t b = kernel::R32(ctx.esp + 12);

    const __m128 a0 = LoadRow(base, a + 0x00);
    const __m128 a1 = LoadRow(base, a + 0x10);
    const __m128 a2 = LoadRow(base, a + 0x20);
    const __m128 a3 = LoadRow(base, a + 0x30);

    // row 0: (b0x*a0 + b0y*a1) + (b0z*a2 + b0w*a3)
    {
        const __m128 b0 = LoadRow(base, b + 0x00);
        __m128 x0 = _mm_shuffle_ps(b0, b0, 0x00);
        __m128 x1 = _mm_shuffle_ps(b0, b0, 0x55);
        __m128 x2 = _mm_shuffle_ps(b0, b0, 0xAA);
        __m128 x3 = _mm_shuffle_ps(b0, b0, 0xFF);
        x0 = _mm_mul_ps(x0, a0);
        x1 = _mm_mul_ps(x1, a1);
        x2 = _mm_mul_ps(x2, a2);
        x3 = _mm_mul_ps(x3, a3);
        x0 = _mm_add_ps(x0, x1);
        x2 = _mm_add_ps(x2, x3);
        x0 = _mm_add_ps(x0, x2);
        StoreRow(base, dst + 0x00, x0);
    }
    // row 1: (b1x*a0 + b1y*a1) + (b1z*a2 + b1w*a3)
    {
        const __m128 b1 = LoadRow(base, b + 0x10);
        __m128 x2 = _mm_shuffle_ps(b1, b1, 0x00);
        __m128 x3 = _mm_shuffle_ps(b1, b1, 0x55);
        __m128 x1 = _mm_shuffle_ps(b1, b1, 0xAA);
        __m128 x0 = _mm_shuffle_ps(b1, b1, 0xFF);
        x2 = _mm_mul_ps(x2, a0);
        x3 = _mm_mul_ps(x3, a1);
        x1 = _mm_mul_ps(x1, a2);
        x0 = _mm_mul_ps(x0, a3);
        x2 = _mm_add_ps(x2, x3);
        x1 = _mm_add_ps(x1, x0);
        x2 = _mm_add_ps(x2, x1);
        StoreRow(base, dst + 0x10, x2);
    }
    // row 2: ((b2x*a0 + b2y*a1) + b2w*a3) + b2z*a2
    {
        const __m128 b2 = LoadRow(base, b + 0x20);
        __m128 x2 = _mm_shuffle_ps(b2, b2, 0x00);
        __m128 x3 = _mm_shuffle_ps(b2, b2, 0x55);
        __m128 x1 = _mm_shuffle_ps(b2, b2, 0xAA);
        __m128 x0 = _mm_shuffle_ps(b2, b2, 0xFF);
        x2 = _mm_mul_ps(x2, a0);
        x3 = _mm_mul_ps(x3, a1);
        x1 = _mm_mul_ps(x1, a2);
        x0 = _mm_mul_ps(x0, a3);
        x3 = _mm_add_ps(x3, x2);
        x0 = _mm_add_ps(x0, x3);
        x0 = _mm_add_ps(x0, x1);
        StoreRow(base, dst + 0x20, x0);
    }
    // row 3: (b3y*a1 + b3x*a0) + (b3w*a3 + b3z*a2)
    {
        const __m128 b3 = LoadRow(base, b + 0x30);
        __m128 x1 = _mm_shuffle_ps(b3, b3, 0x00);
        __m128 x3 = _mm_shuffle_ps(b3, b3, 0x55);
        __m128 x0 = _mm_shuffle_ps(b3, b3, 0xAA);
        __m128 x2 = _mm_shuffle_ps(b3, b3, 0xFF);
        x3 = _mm_mul_ps(x3, a1);
        x1 = _mm_mul_ps(x1, a0);
        x2 = _mm_mul_ps(x2, a3);
        x0 = _mm_mul_ps(x0, a2);
        x3 = _mm_add_ps(x3, x1);
        x2 = _mm_add_ps(x2, x0);
        x3 = _mm_add_ps(x3, x2);
        StoreRow(base, dst + 0x30, x3);
    }
    ctx.esp += 4;  // ret
}
}  // namespace game

// Release builds compile no dev-tools wrapper for this function (city_stream.cpp's trace block), so this is
// the only override there; dev builds route their tracing wrapper here too.
#if !TCNYC_DEV_TOOLS
X86_FUNC(sub_0024C6E0) { game::MatrixMul4x4(ctx, base); }
#endif
