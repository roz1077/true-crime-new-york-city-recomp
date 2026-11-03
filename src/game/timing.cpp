// Cutscene sample clock and opt-in deterministic replay for default.xbe.

#include <algorithm>
#include <chrono>
#include <thread>

#include "app/app.h"
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

namespace {
constexpr uint32_t kLastVblank = 0x363D38;
constexpr uint32_t kVblank = 0x420468;
constexpr uint32_t kAudioClockMode = 0x360E0C;
constexpr uint32_t kPreviousAudioClockMode = 0x360DE0;
constexpr uint32_t kCutsceneClockStart = 0x360DD0;
constexpr uint32_t kCutsceneState = 0x348E10;

// Shared by this main-loop iteration's simulation and rendering calls.
thread_local bool fast_gameplay = false;
thread_local bool fast_cutscene = false;
thread_local bool cutscene_tick_not_due = false;
thread_local bool use_sampled_vblank = false;
thread_local uint32_t sampled_vblank = 0;
thread_local uint32_t frame_ticks = 0;
thread_local bool fast_present = false;

void TraceFrame(uint32_t ticks, bool audio_clock, uint32_t steps) {
    static const bool enabled = std::getenv("XBE_TRACE_FRAME_TIMING") != nullptr;
    if (!enabled) return;
    using clock = std::chrono::steady_clock;
    static auto start = clock::now();
    static uint32_t frames = 0, start_ticks = ticks, zero_tick_frames = 0;
    static bool previous_audio_clock = audio_clock;
    const auto now = clock::now();
    if (audio_clock != previous_audio_clock || ticks < start_ticks) {
        if (audio_clock != previous_audio_clock)
            LOGR("tcnyc: %s", audio_clock ? "entered audio-clock cutscene" : "left audio-clock cutscene");
        start = now;
        start_ticks = ticks;
        frames = 0;
        zero_tick_frames = 0;
        previous_audio_clock = audio_clock;
    }
    const double seconds = std::chrono::duration<double>(now - start).count();
    if (seconds >= 2) {
        LOGR("tcnyc: %s %.1f frames/s, %.1f ticks/s, %u zero-tick frames",
             audio_clock ? "cutscene" : "gameplay", frames / seconds, (ticks - start_ticks) / seconds,
             zero_tick_frames);
        start = now;
        start_ticks = ticks;
        frames = 0;
        zero_tick_frames = 0;
    }
    ++frames;
    if (!steps) ++zero_tick_frames;
}
}

namespace game {
uint32_t MainLoopSwapInterval(uint32_t interval) {
    return fast_present && interval == 2 ? 1 : interval;
}
}

X86_EXTERN_FUNC(__imp__sub_000A5200);
X86_FUNC(sub_000A5200) {
    if (X86_MEM32(ctx.esp) != 0xA7910) return __imp__sub_000A5200(ctx, base);
    const uint32_t arg = ctx.esp + 4;
    const uint32_t original_ticks = X86_MEM32(arg);
    frame_ticks = original_ticks;
    if (fast_gameplay) {
        const uint32_t last = X86_MEM32(kLastVblank);
        const uint32_t elapsed = sampled_vblank - last;
        frame_ticks = std::clamp(elapsed, 1u, 4u);
        use_sampled_vblank = elapsed != 0;
    } else if (fast_cutscene && cutscene_tick_not_due && original_ticks == 1) {
        frame_ticks = 0;
    }
    TraceFrame(X86_MEM32(0x363D0C), X86_MEM8(kAudioClockMode) != 0, frame_ticks);
    X86_MEM32(arg) = frame_ticks;
    __imp__sub_000A5200(ctx, base);
    use_sampled_vblank = false;
    X86_MEM32(arg) = original_ticks;
}

X86_EXTERN_FUNC(__imp__sub_000A73F0);
X86_FUNC(sub_000A73F0) {
    if (X86_MEM32(ctx.esp) != 0xA794B || (!fast_gameplay && !fast_cutscene))
        return __imp__sub_000A73F0(ctx, base);
    const uint32_t arg = ctx.esp + 4;
    const uint32_t original_ticks = X86_MEM32(arg);
    X86_MEM32(arg) = frame_ticks;
    __imp__sub_000A73F0(ctx, base);
    X86_MEM32(arg) = original_ticks;
}

// The main loop presents after its render function returns.
X86_EXTERN_FUNC(__imp__sub_00242F30);
X86_FUNC(sub_00242F30) {
    if (X86_MEM32(ctx.esp) != 0xA7952 || (!fast_gameplay && !fast_cutscene))
        return __imp__sub_00242F30(ctx, base);
    fast_present = true;
    __imp__sub_00242F30(ctx, base);
    fast_present = false;
}

X86_EXTERN_FUNC(__imp__sub_00243000);
X86_FUNC(sub_00243000) {
    uint32_t ret;
    std::memcpy(&ret, base + ctx.esp, 4);
    if (ret == 0xA5221 && use_sampled_vblank) {
        ::kernel::Return(ctx, sampled_vblank, 0);
        return;
    }
    if (ret == 0xA78BD) {
        fast_gameplay = !X86_MEM8(kAudioClockMode) && app::FrameCap() == 60 && !timing::Enabled();
        if (fast_gameplay) {
            const uint32_t last = X86_MEM32(kLastVblank);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
            sampled_vblank = X86_MEM32(kVblank);
            // A frame can arrive before the next guest vblank on a fast host.
            while (sampled_vblank == last && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                sampled_vblank = X86_MEM32(kVblank);
            }
            ::kernel::Return(ctx, sampled_vblank, 0);
            return;
        }
    }
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
    if (ret == 0xA786F) {
        fast_gameplay = false;
        fast_cutscene = X86_MEM8(kAudioClockMode) && app::FrameCap() == 60 && !timing::Enabled();
        cutscene_tick_not_due = false;
        // Only the steady audio-clock branch forces a tick when the scene is
        // already caught up. Leave the entry and clock rebase paths alone.
        if (fast_cutscene && X86_MEM8(kPreviousAudioClockMode) && !X86_MEM8(ctx.esp + 0x1E)) {
            const uint32_t start_sample = X86_MEM32(kCutsceneClockStart);
            const uint32_t elapsed = value - start_sample - uint32_t(value < start_sample);
            const float due = float(int32_t(elapsed)) * -X86_MEMF32(0x2D5DC8);
            const uint32_t state = X86_MEM32(kCutsceneState);
            cutscene_tick_not_due = state && int32_t(due) <= int32_t(X86_MEM32(state + 0x9C));
        }
    }
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
