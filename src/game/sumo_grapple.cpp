// The "sumo" grapple: the shoving match on the cargo container with Tommy in
// Human Cargo (BC4_M5), animations Nick_Grappled_Sumo_* / NPC_Grapple_Sumo_*.
//
// 0x58D90 updates one side of it (thiscall, ecx = grapple): state byte +0x0C,
// timer +0x30, balance +0x40 (+1 when the other side is at the edge), timer
// +0x44. Its timers count down by the frame time [0x37F630], in 1/60 s ticks.
// When a side is recovering (state 1) with the other at the edge, B pushes
// them off; the window closes when this side's own update starts the next
// shove (it posts the other side an event through 0x587B0). Measured at about
// 10 ticks (166 ms), which with the original's frame drops could be a single
// frame on the Xbox. This side gets a slower clock during that window, so the
// window lasts kWindowScale times longer; the rest of the game keeps its time.

#include <cstring>

#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xdk/hle.h"
#if TCNYC_DEV_TOOLS
#include "dev/dev.h"
#endif

namespace {
constexpr uint32_t kFrameTime = 0x37F630;
constexpr uint32_t kState = 0x0C, kBalance = 0x40, kEnd = 0x48;
constexpr uint8_t kRecovering = 1;
constexpr float kAtEdge = 0.999f;
constexpr float kWindowScale = 1.5f; // 166 ms -> about 250 ms

float F32(uint32_t va) {
    const uint32_t bits = kernel::R32(va);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

void W32F(uint32_t va, float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    kernel::W32(va, bits);
}
} // namespace

X86_EXTERN_FUNC(__imp__sub_00058D90);
X86_FUNC(sub_00058D90) {
    const uint32_t grapple = ctx.ecx;
    if (!grapple || !mem::IsCommitted(grapple + kEnd)) return __imp__sub_00058D90(ctx, base);
#if TCNYC_DEV_TOOLS
    dev::SumoBefore(grapple);
#endif
    const bool push_window =
        kernel::G<uint8_t>(grapple + kState) == kRecovering && F32(grapple + kBalance) >= kAtEdge;
    const float frame_time = F32(kFrameTime);
    if (push_window) W32F(kFrameTime, frame_time / kWindowScale);
    __imp__sub_00058D90(ctx, base);
    if (push_window) W32F(kFrameTime, frame_time);
#if TCNYC_DEV_TOOLS
    dev::SumoAfter(grapple, push_window);
#endif
}
