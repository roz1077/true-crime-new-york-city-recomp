// Cutscene sample clock and opt-in deterministic replay for default.xbe.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"
#include "kernel/kernel.h"
#include "xdk/hle.h"

// 0x2914D2: DirectSoundGetSampleTime reads the APU's free-running 48 kHz
// counter at FE80200C after checking the original DirectSound singleton. HLE
// sound creates neither that singleton nor the MMIO counter, so the original
// always returned zero. The main loop at A7810 uses this clock during cutscenes
// (60 / 48000 ticks per sample); with a stopped clock it falls back to one
// update per present instead of catching up to audio. Replay timing. The main
// loop 0xA7810 runs "vblanks since the last frame" ticks (0x243000, the vblank
// counter 0x420468, called at 0xA78B8) and, in cutscenes, ticks from the audio
// sample clock (0x2914D2, called at 0xA786A); both are real time, so a replay
// of the same inputs drifted. With XBE_DETERMINISTIC_REPLAY enabled,
// XBE_PAD_RECORD=<file> writes the steps in call order to <file>.timing
// ("v <vblank step>", "s <sample step>"); with XBE_PAD_REPLAY=<file> the
// main loop gets those steps back in the same order (real time again once the
// file runs out).
namespace timing {
// Clock/seed overrides and RNG isolation change title behavior. They are useful
// for controlled replay experiments, but ordinary input recording must not turn
// them on during normal gameplay.
bool Enabled() {
    static const bool enabled = std::getenv("XBE_DETERMINISTIC_REPLAY") != nullptr;
    return enabled;
}
FILE *g_out = nullptr;
std::vector<std::pair<char, uint32_t>> g_in;
size_t g_next[3] = {0, 0, 0}; // per kind: v, s, r
std::vector<size_t> g_index[3];
bool g_loaded = false;
void Load() {
    if (g_loaded) return;
    g_loaded = true;
    if (!Enabled()) return;
    if (const char *r = std::getenv("XBE_PAD_RECORD")) g_out = std::fopen((std::string(r) + ".timing").c_str(), "w");
    if (const char *r = std::getenv("XBE_PAD_REPLAY")) {
        if (FILE *f = std::fopen((std::string(r) + ".timing").c_str(), "r")) {
            char k;
            unsigned v;
            while (std::fscanf(f, " %c %u", &k, &v) == 2) {
                g_in.push_back({k, v});
                if (k == 'v' || k == 's' || k == 'r')
                    g_index[k == 's' ? 1 : k == 'r' ? 2 : 0].push_back(g_in.size() - 1);
            }
            std::fclose(f);
            LOGR("tcnyc: replay timing %zu vblank steps, %zu sample steps, %zu seeds", g_index[0].size(),
                 g_index[1].size(), g_index[2].size());
        }
    }
}
// recorded step or -1
int64_t Next(int kind) {
    if (g_next[kind] >= g_index[kind].size()) return -1;
    return g_in[g_index[kind][g_next[kind]++]].second;
}
void Write(char kind, uint32_t v) {
    if (g_out) std::fprintf(g_out, "%c %u\n", kind, v);
}
} // namespace timing

// Draws world objects with random variation (seeded per object, then the old
// state is put back slightly moved). How many objects get drawn follows
// streaming, which finishes in real time. Only explicit deterministic-replay
// experiments isolate this draw's RNG effects; normal gameplay retains the
// original random-state changes.
X86_EXTERN_FUNC(__imp__sub_00240CC0);
X86_FUNC(sub_00240CC0) {
    if (!timing::Enabled()) {
        __imp__sub_00240CC0(ctx, base);
        return;
    }
    uint8_t saved[8];
    std::memcpy(saved, base + 0x3376C8, 8);
    __imp__sub_00240CC0(ctx, base);
    std::memcpy(base + 0x3376C8, saved, 8);
}

// srand: the game seeds its random numbers from a timer, so a replay records
// and reuses every seed
X86_EXTERN_FUNC(__imp__sub_00229D50);
X86_FUNC(sub_00229D50) {
    if (!timing::Enabled()) {
        __imp__sub_00229D50(ctx, base);
        return;
    }
    uint32_t ret, seed;
    std::memcpy(&ret, base + ctx.esp, 4);
    // these seed from an object position or put back a saved random number: they
    // repeat by themselves and run thousands of times a frame, so only the other
    // (timer-based) seeds are kept
    switch (ret) {
    case 0x16E2A:
    case 0x173D5:
    case 0x240D22:
    case 0x240E62:
    case 0x240F74:
    case 0x240F8E:
        return __imp__sub_00229D50(ctx, base);
    }
    timing::Load();
    std::memcpy(&seed, base + ctx.esp + 4, 4);
    const int64_t rec = timing::Next(2);
    if (rec >= 0) {
        seed = uint32_t(rec);
        std::memcpy(base + ctx.esp + 4, &seed, 4);
    }
    timing::Write('r', seed);
    __imp__sub_00229D50(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_00243000);
X86_FUNC(sub_00243000) {
    uint32_t ret;
    std::memcpy(&ret, base + ctx.esp, 4);
    if (!timing::Enabled() || ret != 0xA78BD) {
        __imp__sub_00243000(ctx, base);
        return;
    }
    timing::Load();
    uint32_t last, real;
    std::memcpy(&last, base + 0x363D38, 4);
    std::memcpy(&real, base + 0x420468, 4);
    static int64_t offset = 0; // replayed counter minus real counter
    const int64_t step = timing::Next(0);
    uint32_t value = step >= 0 ? last + uint32_t(step) : uint32_t(real + offset);
    if (step >= 0) offset = int64_t(value) - int64_t(real);
    timing::Write('v', value - last);
    uint32_t rng, ticks; // check line (not replayed): random state and sim tick
                         // count, to find where two runs part
    std::memcpy(&rng, base + 0x3376C8, 4);
    std::memcpy(&ticks, base + 0x363D0C, 4);
    timing::Write('c', rng ^ (ticks * 2654435761u));
    ::kernel::Return(ctx, value, 0);
}

X86_FUNC(sub_002914D2) {
    using clock = std::chrono::steady_clock;
    static const auto start = clock::now();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - start).count();
    // Split whole seconds to keep the multiplication safe, including uint32_t
    // wrap.
    const uint64_t samples = uint64_t(ns / 1000000000) * 48000 + uint64_t(ns % 1000000000) * 48000 / 1000000000;
    uint32_t value = uint32_t(samples);
    uint32_t ret;
    std::memcpy(&ret, base + ctx.esp, 4);
    if (timing::Enabled() && ret == 0xA786F) { // the main loop's call
        timing::Load();
        static uint32_t last_real = value, last_value = value;
        static bool first = true;
        const int64_t step = timing::Next(1);
        const uint32_t real_step = first ? 0 : value - last_real;
        first = false;
        last_real = value;
        value = last_value + (step >= 0 ? uint32_t(step) : real_step);
        timing::Write('s', value - last_value);
        last_value = value;
    }
    ::kernel::Return(ctx, value, 0);
}
