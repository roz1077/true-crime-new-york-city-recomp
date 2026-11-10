// Cutscene sample clock, 60 FPS main-loop timing and opt-in deterministic replay
// for default.xbe.
//
// 0x2914D2: DirectSoundGetSampleTime reads the APU's free-running 48 kHz
// counter at FE80200C after checking the original DirectSound singleton. HLE
// sound creates neither that singleton nor the MMIO counter, so the original
// always returned zero. The main loop at 0xA7810 uses this clock during
// cutscenes (60 / 48000 ticks per sample); with a stopped clock it falls back
// to one update per present instead of catching up to audio.
//
// Replay timing: the main loop runs "vblanks since the last frame" ticks
// (0x243000, the vblank counter 0x420468, called at 0xA78B8) and, in cutscenes,
// ticks from the audio sample clock (0x2914D2, called at 0xA786A); both are
// real time, so a replay of the same inputs drifted. With
// XBE_DETERMINISTIC_REPLAY enabled, XBE_PAD_RECORD=<file> writes the steps in
// call order to <file>.timing ("v <vblank step>", "s <sample step>"); with
// XBE_PAD_REPLAY=<file> the main loop gets those steps back in the same order
// (real time again once the file runs out).

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "app/app.h"
#include "common/graphics_choices.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "xdk/hle.h"
#if TCNYC_DEV_TOOLS
#include "dev/dev.h"
#endif

namespace {
// guest data
constexpr uint32_t kRandomState = 0x3376C8; // 8 bytes
constexpr uint32_t kSimTicks = 0x363D0C;
constexpr uint32_t kLastVblank = 0x363D38;
constexpr uint32_t kVblank = 0x420468;
constexpr uint32_t kAudioClockMode = 0x360E0C;
constexpr uint32_t kPreviousAudioClockMode = 0x360DE0;
constexpr uint32_t kCutsceneClockStart = 0x360DD0;
constexpr uint32_t kCutsceneState = 0x348E10;
constexpr uint32_t kTicksPerSample = 0x2D5DC8; // float, -60 / 48000

// return addresses of the main loop's calls
constexpr uint32_t kMainLoopSampleTime = 0xA786F;
constexpr uint32_t kMainLoopVblank = 0xA78BD;
constexpr uint32_t kMainLoopSimulate = 0xA7910;
constexpr uint32_t kMainLoopRender = 0xA794B;
constexpr uint32_t kMainLoopPresent = 0xA7952;
constexpr uint32_t kSimulateVblank = 0xA5221; // the simulation's own vblank read

// The main loop's fast path only matches the game's own 60 Hz cadence. The
// stored cap can be 0 (unlimited), 1 (monitor refresh) or above 60; the
// effective limit in all of those cases is still the game's 60.
bool AtGameFrameRate() { return game::choices::EffectiveFrameCap(app::FrameCap()) == 60; }
} // namespace

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
    std::memcpy(saved, base + kRandomState, 8);
    __imp__sub_00240CC0(ctx, base);
    std::memcpy(base + kRandomState, saved, 8);
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
// Shared by this main-loop iteration's simulation and rendering calls.
thread_local bool fast_gameplay = false;
thread_local bool fast_cutscene = false;
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
        LOGR("tcnyc: %s %.1f frames/s, %.1f ticks/s, %u zero-tick frames", audio_clock ? "cutscene" : "gameplay",
             frames / seconds, (ticks - start_ticks) / seconds, zero_tick_frames);
        start = now;
        start_ticks = ticks;
        frames = 0;
        zero_tick_frames = 0;
    }
    ++frames;
    if (!steps) ++zero_tick_frames;
}
} // namespace

namespace game {
uint32_t MainLoopSwapInterval(uint32_t interval) { return fast_present && interval == 2 ? 1 : interval; }
} // namespace game

// The main loop's simulation step uses elapsed vblanks in 60 FPS gameplay.
// Cutscenes keep the title's audio-clock step.
X86_EXTERN_FUNC(__imp__sub_000A5200);
X86_FUNC(sub_000A5200) {
    if (X86_MEM32(ctx.esp) != kMainLoopSimulate) return __imp__sub_000A5200(ctx, base);
#if TCNYC_DEV_TOOLS
    dev::OnFrame(ctx);
#endif
    const uint32_t arg = ctx.esp + 4;
    const uint32_t original_ticks = X86_MEM32(arg);
    frame_ticks = original_ticks;
    if (fast_gameplay) {
        const uint32_t last = X86_MEM32(kLastVblank);
        const uint32_t elapsed = sampled_vblank - last;
        frame_ticks = std::clamp(elapsed, 1u, 4u);
        use_sampled_vblank = elapsed != 0;
    }
#if TCNYC_DEV_TOOLS
    frame_ticks = dev::AdjustTicks(frame_ticks); // debug menu: pause, slow motion, fast forward
#endif
    TraceFrame(X86_MEM32(kSimTicks), X86_MEM8(kAudioClockMode) != 0, frame_ticks);
    X86_MEM32(arg) = frame_ticks;
    __imp__sub_000A5200(ctx, base);
    use_sampled_vblank = false;
    X86_MEM32(arg) = original_ticks;
}

// The main loop's render step gets the same tick count as the simulation.
X86_EXTERN_FUNC(__imp__sub_000A73F0);
X86_FUNC(sub_000A73F0) {
#if TCNYC_DEV_TOOLS
    const bool dev_speed = dev::GameSpeed() != 4;
#else
    constexpr bool dev_speed = false;
#endif
    if (X86_MEM32(ctx.esp) != kMainLoopRender || (!fast_gameplay && !fast_cutscene && !dev_speed))
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
    if (X86_MEM32(ctx.esp) != kMainLoopPresent || (!fast_gameplay && !fast_cutscene))
        return __imp__sub_00242F30(ctx, base);
    fast_present = true;
    __imp__sub_00242F30(ctx, base);
    fast_present = false;
}

// The vblank counter read.
X86_EXTERN_FUNC(__imp__sub_00243000);
X86_FUNC(sub_00243000) {
    uint32_t ret;
    std::memcpy(&ret, base + ctx.esp, 4);
    if (ret == kSimulateVblank && use_sampled_vblank) {
        ::kernel::Return(ctx, sampled_vblank, 0);
        return;
    }
    if (ret == kMainLoopVblank) {
        fast_gameplay = !X86_MEM8(kAudioClockMode) && AtGameFrameRate() && !timing::Enabled();
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
    if (!timing::Enabled() || ret != kMainLoopVblank) {
        __imp__sub_00243000(ctx, base);
        return;
    }
    timing::Load();
    uint32_t last, real;
    std::memcpy(&last, base + kLastVblank, 4);
    std::memcpy(&real, base + kVblank, 4);
    static int64_t offset = 0; // replayed counter minus real counter
    const int64_t step = timing::Next(0);
    uint32_t value = step >= 0 ? last + uint32_t(step) : uint32_t(real + offset);
    if (step >= 0) offset = int64_t(value) - int64_t(real);
    timing::Write('v', value - last);
    // check line (not replayed): random state and sim tick count, to find where two runs part
    uint32_t rng, ticks;
    std::memcpy(&rng, base + kRandomState, 4);
    std::memcpy(&ticks, base + kSimTicks, 4);
    timing::Write('c', rng ^ (ticks * 2654435761u));
    ::kernel::Return(ctx, value, 0);
}

X86_FUNC(sub_002914D2) {
    using clock = std::chrono::steady_clock;
    static const auto start = clock::now();
    // Split whole seconds to keep the multiplication safe, including uint32_t wrap.
    const auto sample_time = [] {
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - start).count();
        return uint32_t(uint64_t(ns / 1000000000) * 48000 + uint64_t(ns % 1000000000) * 48000 / 1000000000);
    };
    uint32_t value = sample_time();
    uint32_t ret;
    std::memcpy(&ret, base + ctx.esp, 4);
    if (ret == kMainLoopSampleTime) {
        fast_gameplay = false;
        fast_cutscene = X86_MEM8(kAudioClockMode) && AtGameFrameRate() && !timing::Enabled();
        // The title's render passes expect the simulation to update before each
        // picture. Wait for the audio clock when we are early instead of
        // rendering a zero-tick frame, which can flash. Entry and clock rebase
        // use their own timing. Bound the wait to roughly one tick so an
        // unexpected clock offset falls back to the title's own step.
        if (fast_cutscene && X86_MEM8(kPreviousAudioClockMode) && !X86_MEM8(ctx.esp + 0x1E)) {
            const uint32_t start_sample = X86_MEM32(kCutsceneClockStart);
            const uint32_t state = X86_MEM32(kCutsceneState);
            if (state) {
                const int32_t tick = int32_t(X86_MEM32(state + 0x9C));
                const float ticks_per_sample = -X86_MEMF32(kTicksPerSample);
                const auto caught_up = [&] {
                    const uint32_t elapsed = value - start_sample - uint32_t(value < start_sample);
                    return int32_t(float(int32_t(elapsed)) * ticks_per_sample) <= tick;
                };
                const auto deadline = clock::now() + std::chrono::milliseconds(20);
                while (caught_up() && clock::now() < deadline) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    value = sample_time();
                }
            }
        }
    }
    if (timing::Enabled() && ret == kMainLoopSampleTime) {
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
