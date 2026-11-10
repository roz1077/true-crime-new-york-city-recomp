// Voice volume 0 blocks gameplay phone calls.
//
// 0x25DA90 starts a voice/music/SFX stream. Before starting it, it compares
// the mix-bin volume from 0x2291F0 (a float table at 0x337640, indexed by the
// stream's bin) against zero; a zero volume makes the start fail. Missions
// that wait for a phone call to finish then never advance (no informant
// missions, no "Precinct Cleaned" call).
//
// The table holds the player's own settings, so it is left alone: the getter's
// result is changed instead, and only while 0x25DA90 is starting a stream. An
// exact 0.0 then becomes a tiny positive value, above the start check's
// threshold ([0x2E1C80] = 0.0). The result is inaudible (about -120 dB) but the
// stream starts and its completion callback runs. The getter's other callers
// still see 0, and non-zero volumes are left untouched.

#include "core/log.h"
#include "kernel/kernel.h"
#include "xdk/hle.h"

namespace {
constexpr double kSilentButPlaying = 1.0e-6;
thread_local int t_stream_start = 0;  // inside 0x25DA90 on this thread
} // namespace

X86_EXTERN_FUNC(__imp__sub_0025DA90);
X86_FUNC(sub_0025DA90) {
    ++t_stream_start;
    __imp__sub_0025DA90(ctx, base);
    --t_stream_start;
}

// 0x2291F0 is the mix-bin volume getter: [esp+4] = bin index, returns
// [0x337640 + bin * 4] as a float on the x87 stack.
X86_EXTERN_FUNC(__imp__sub_002291F0);
X86_FUNC(sub_002291F0) {
    const uint32_t bin = X86_MEM32(ctx.esp + 4);
    __imp__sub_002291F0(ctx, base);
    if (t_stream_start && X87_ST(0) == 0.0) {
        X87_ST(0) = kSilentButPlaying;
        LOG_FIRST(8, "tcnyc: mix bin %u is 0, reported as %.1e so phone calls can start", bin, kSilentButPlaying);
    }
}
