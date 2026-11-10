// Voice volume 0 blocks gameplay phone calls.
//
// 0x25DA90 starts a voice/music/SFX stream. Before starting it, it compares
// the mix-bin volume from 0x2291F0 (a float table at 0x337640, indexed by the
// stream's bin) against zero; a zero volume makes the start fail. Missions
// that wait for a phone call to finish then never advance (no informant
// missions, no "Precinct Cleaned" call).
//
// The table is the game's own mix-bin volume table, so instead of bypassing
// the check in the stream code, replace an exact 0.0 with a tiny positive
// value when it is read. The result is inaudible (about -120 dB) but the
// stream starts and its completion callback runs. Non-zero volumes are left
// untouched.

#include <cstring>

#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

namespace {
constexpr uint32_t kVolumeTable = 0x337640;
constexpr float kSilentButPlaying = 1.0e-6f;

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

// 0x2291F0 is the mix-bin volume getter: [esp+4] = bin index, returns a float on the x87 stack.
X86_EXTERN_FUNC(__imp__sub_002291F0);
X86_FUNC(sub_002291F0) {
    const uint32_t bin = X86_MEM32(ctx.esp + 4);
    const uint32_t slot = kVolumeTable + bin * 4;
    if (mem::IsCommitted(slot) && F32(slot) == 0.0f) {
        W32F(slot, kSilentButPlaying);
        LOG_FIRST(8, "tcnyc: mix bin %u was 0, raised to %.1e so phone calls can start", bin, kSilentButPlaying);
    }
    __imp__sub_002291F0(ctx, base);
}
