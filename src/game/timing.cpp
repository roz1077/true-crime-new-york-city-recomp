#include "game_addresses.h"
#include "core/dev_env.h"
// 60 FPS main-loop timing and opt-in deterministic replay for default.xbe.
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
// PAL has a different loop driven by the performance counter. Its timing
// recordings store simulation ticks ("t") at the shared simulation hook.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "app/app.h"
#include "common/guest_values.h"
#include "common/graphics_choices.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xdk/hle.h"
#if TCNYC_DEV_TOOLS
#include "core/input.h"
#include "dev/dev.h"
#endif

namespace {
// guest data
constexpr uint32_t kRandomState = TC_ADDR(0x3376C8); // 8 bytes
constexpr uint32_t kSimTicks = TC_ADDR(0x363D0C);
constexpr uint32_t kLastVblank = TC_ADDR(0x363D38);
constexpr uint32_t kVblank = TC_ADDR(0x420468);
constexpr uint32_t kAudioClockMode = TC_ADDR(0x360E0C);
constexpr uint32_t kGameSession = TC_ADDR(0x360E00);
// The mission setter (US 0x7F160) stores the current 32-byte LevelInfo
// mission entry here. The first word of the entry is its name hash.
constexpr uint32_t kCurrentMissionEntry = 0x2A94;
constexpr uint32_t kEscapeCrazy = guest::Crc32("BC3_M6");
constexpr uint32_t kScriptOwnerDepth = TC_ADDR(0x2E78C0);
constexpr uint32_t kScriptOwnerStack = TC_ADDR(0x37BE28);
constexpr uint32_t kScriptState = 0x424;
constexpr uint32_t kCallingMarcus = guest::Crc32("callingMarcus");
constexpr uint32_t kShowerOrderlyId = 777;
constexpr uint32_t kUnlockShower = guest::Crc32("UnlockShower!");
constexpr uint32_t kUnlockingShower = guest::Crc32("unlockingShower");
constexpr uint32_t kObjectId = 0x1C;         // int16, the ID QueryID looks up (0x21FB20)
constexpr uint32_t kRequestedState = 0x490;  // written by SetState (0xFC090)
constexpr uint32_t kStateTable = 0x478;      // tag node: u16 count, u16 flags, 16-byte entries (name +4)
#if !TCNYC_PAL
constexpr uint32_t kPreviousAudioClockMode = TC_ADDR(0x360DE0);
constexpr uint32_t kCutsceneClockStart = TC_ADDR(0x360DD0);
constexpr uint32_t kCutsceneState = TC_ADDR(0x348E10);
constexpr uint32_t kTicksPerSample = 0x2D5DC8; // float, -60 / 48000

// return addresses of the main loop's calls
constexpr uint32_t kMainLoopSampleTime = 0xA786F;
constexpr uint32_t kMainLoopVblank = 0xA78BD;
#endif
constexpr uint32_t kMainLoopSimulate = TC_ADDR(0xA7910);
constexpr uint32_t kMainLoopRender = TC_ADDR(0xA794B);
constexpr uint32_t kMainLoopPresent = TC_ADDR(0xA7952);
constexpr uint32_t kSimulateVblank = TC_ADDR(0xA5221); // the simulation's own vblank read

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
    static const bool enabled = DevEnv("XBE_DETERMINISTIC_REPLAY") != nullptr;
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
    if (const char *r = DevEnv("XBE_PAD_RECORD")) g_out = std::fopen((std::string(r) + ".timing").c_str(), "w");
    if (const char *r = DevEnv("XBE_PAD_REPLAY")) {
        if (FILE *f = std::fopen((std::string(r) + ".timing").c_str(), "r")) {
            char k;
            unsigned v;
            while (std::fscanf(f, " %c %u", &k, &v) == 2) {
                g_in.push_back({k, v});
                if (k == (TCNYC_PAL ? 't' : 'v') || k == 's' || k == 'r')
                    g_index[k == 's' ? 1 : k == 'r' ? 2 : 0].push_back(g_in.size() - 1);
            }
            std::fclose(f);
            LOGR("tcnyc: replay timing %zu %s steps, %zu sample steps, %zu seeds", g_index[0].size(),
                 TCNYC_PAL ? "frame" : "vblank", g_index[1].size(), g_index[2].size());
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
    case TC_ADDR(0x16E2A):
    case TC_ADDR(0x173D5):
    case TC_ADDR(0x240D22):
    case TC_ADDR(0x240E62):
    case TC_ADDR(0x240F74):
    case TC_ADDR(0x240F8E):
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
thread_local uint32_t guest_renders = 0;
thread_local uint32_t guest_swaps = 0;

bool IsEscapeCrazy(uint8_t *base) {
    const uint32_t session = X86_MEM32(kGameSession);
    if (!session || !mem::IsCommitted(session + kCurrentMissionEntry + 3)) return false;
    const uint32_t mission = X86_MEM32(session + kCurrentMissionEntry);
    return mission && mem::IsCommitted(mission + 3) && X86_MEM32(mission) == kEscapeCrazy;
}

#if TCNYC_DEV_TOOLS
constexpr uint32_t kSessionMenuActive = 0x2A54; // also checked by the guest render path
constexpr uint32_t kExperimentalRenderCap = 120;
constexpr uint32_t kRenderBeginFromMainRender = TC_ADDR(0xA75DD);
constexpr uint32_t kGuestRenderQueueDepth = TC_ADDR(0x4C632C);
constexpr uint32_t kVehicleStepCall = TC_ADDR(0x1CF101);
constexpr uint32_t kWorldStep = TC_ADDR(0x37F630);
thread_local uint32_t split_motion_vehicle = 0;
thread_local uint32_t motion_calls = 0;
std::atomic<bool> request_drive_test{false};
thread_local uint32_t drive_test_vehicle = 0, drive_test_tick = 0;
thread_local bool drive_test_simulating = false;
constexpr uint32_t kThrottleTicks = 240, kBrakeTicks = 240;
thread_local bool double_render = false;
thread_local bool native_gameplay = false;
using NativeClock = std::chrono::steady_clock;
thread_local NativeClock::time_point native_last_swap;
thread_local double native_sim_ms = 0, native_render_ms = 0;
thread_local X86Context render_entry;

uint32_t NativeGameplayCap() {
    static const uint32_t cap = []() -> uint32_t {
        if (DevEnv("XBE_PAD_RECORD") || DevEnv("XBE_PAD_REPLAY")) return 0;
        if (const char *option = DevEnv("XBE_NATIVE_FPS")) {
            char *end = nullptr;
            const long value = std::strtol(option, &end, 10);
            return end != option && !*end && value >= 61 && value <= 120 ? uint32_t(value) : 0;
        }
        const char *legacy = DevEnv("XBE_NATIVE_90FPS");
        return legacy && std::strcmp(legacy, "1") == 0 ? 90 : 0;
    }();
    return cap;
}

bool NativeGameplayEnabled(uint8_t *base) {
    return NativeGameplayCap() && dev::GameSpeed() == 4 && dev::InGameplay() && !kernel::MovieOpen() &&
           !X86_MEM8(X86_MEM32(kGameSession) + kSessionMenuActive);
}

int VehicleStepProbe() {
    static const int mode = [] {
        const char *option = DevEnv("XBE_VEHICLE_STEP_PROBE");
        if (!option || DevEnv("XBE_PAD_RECORD") || DevEnv("XBE_PAD_REPLAY")) return 0;
        if (std::strcmp(option, "full") == 0) return 1;
        if (std::strcmp(option, "split") == 0) return 2;
        if (std::strcmp(option, "motion") == 0) return 3;
        if (std::strcmp(option, "drive-full") == 0) return 4;
        if (std::strcmp(option, "drive-motion") == 0) return 5;
        if (std::strcmp(option, "straight-full") == 0) return 6;
        if (std::strcmp(option, "straight-motion") == 0) return 7;
        return 0;
    }();
    return mode;
}

bool PlayerDrivesVehicle(uint8_t *base, uint32_t vehicle) {
    if (!dev::InGameplay()) return false;
    const uint32_t session = X86_MEM32(kGameSession);
    const uint32_t camera = X86_MEM32(session + 0xAC);
    if (X86_MEM8(session + kSessionMenuActive) || X86_MEM8(camera + 0x19) != 13) return false;
    // The script's GetMyVehicle follows the pawn's parent chain at +0x28.
    uint32_t object = X86_MEM32(TC_ADDR(0x37D284));
    for (unsigned depth = 0; depth < 8 && object && mem::IsCommitted(object + 0x2B); ++depth) {
        if (object == vehicle) return true;
        object = X86_MEM32(object + 0x28);
    }
    return X86_MEM32(camera + 0x17C) == vehicle;
}

bool ShouldDoubleRender(uint8_t *base) {
    static const bool enabled = [] {
        const char *option = DevEnv("XBE_EXPERIMENTAL_120FPS");
        // Pad recordings use the guest swap count as their input clock.
        return option && std::strcmp(option, "1") == 0 && !DevEnv("XBE_PAD_RECORD") &&
               !DevEnv("XBE_PAD_REPLAY");
    }();
    if (!enabled || native_gameplay || VehicleStepProbe() || !fast_gameplay || frame_ticks != 1 || dev::GameSpeed() != 4 ||
        game::choices::EffectiveFrameCap(app::FrameCap()) != 60 || !dev::InGameplay() || kernel::MovieOpen())
        return false;
    return !X86_MEM8(X86_MEM32(kGameSession) + kSessionMenuActive);
}
#endif

void TraceFrame(uint32_t ticks, bool audio_clock, uint32_t steps) {
    static const bool enabled = DevEnv("XBE_TRACE_FRAME_TIMING") != nullptr;
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
        frames = guest_renders = guest_swaps = 0;
        zero_tick_frames = 0;
        previous_audio_clock = audio_clock;
    }
    const double seconds = std::chrono::duration<double>(now - start).count();
    if (seconds >= 2) {
        LOGR("tcnyc: %s %.1f updates/s, %.1f ticks/s, %.1f renders/s, %.1f swaps/s, %u zero-tick updates",
             audio_clock ? "cutscene" : "gameplay", frames / seconds, (ticks - start_ticks) / seconds,
             guest_renders / seconds, guest_swaps / seconds, zero_tick_frames);
        start = now;
        start_ticks = ticks;
        frames = guest_renders = guest_swaps = 0;
        zero_tick_frames = 0;
    }
    ++frames;
    if (!steps) ++zero_tick_frames;
}
} // namespace

namespace game {
uint32_t MainLoopSwapInterval(uint32_t interval) { return fast_present && interval == 2 ? 1 : interval; }
#if TCNYC_DEV_TOOLS
uint32_t MainLoopSwapCapOverride() {
    if (!fast_present) return 0;
    if (native_gameplay) return NativeGameplayCap();
    return double_render ? kExperimentalRenderCap : 0;
}
bool RequestStraightDrive() {
    if (VehicleStepProbe() < 6) return false;
    request_drive_test = true;
    return true;
}

void ApplyStraightDrivePad(int port, input::Pad &pad) {
    if (port != 0 || !drive_test_simulating || !drive_test_vehicle) return;
    const uint32_t elapsed = kernel::R32(kSimTicks) - drive_test_tick;
    if (elapsed >= kThrottleTicks + kBrakeTicks) return;
    pad = {};
    pad.connected = true;
    // The title uses A for throttle and X for brake/reverse.
    if (elapsed < kThrottleTicks) pad.analog[0] = 255;
    else pad.analog[2] = 255;
}
#endif
} // namespace game

#if TCNYC_DEV_TOOLS
X86_EXTERN_FUNC(__imp__sub_002219E0);
X86_FUNC(sub_002219E0) {
    const uint32_t caller = X86_MEM32(ctx.esp);
    if (!split_motion_vehicle || ctx.ecx != split_motion_vehicle || (caller != TC_ADDR(0x1CCC46) && caller != TC_ADDR(0x1CCC72)))
        return __imp__sub_002219E0(ctx, base);

    const X86Context entry = ctx;
    const uint32_t force = X86_MEM32(ctx.esp + 4);
    const uint32_t torque = X86_MEM32(ctx.esp + 8);
    const uint32_t step_arg = ctx.esp + 12;
    const float step = X86_MEMF32(step_arg);
    X86_MEMF32(step_arg) = step * 0.5f;
    __imp__sub_002219E0(ctx, base);
    ctx = entry;
    X86_MEM32(ctx.esp) = caller;
    X86_MEM32(ctx.esp + 4) = force;
    X86_MEM32(ctx.esp + 8) = torque;
    X86_MEMF32(step_arg) = step * 0.5f;
    __imp__sub_002219E0(ctx, base);
    X86_MEMF32(step_arg) = step;
    ++motion_calls;
}

X86_EXTERN_FUNC(__imp__sub_001CA180);
X86_FUNC(sub_001CA180) {
    const int mode = VehicleStepProbe();
    if (!mode || X86_MEM32(ctx.esp) != kVehicleStepCall || !fast_gameplay || dev::GameSpeed() != 4)
        return __imp__sub_001CA180(ctx, base);

    const uint32_t vehicle = ctx.ecx;
    const float step = X86_MEMF32(kWorldStep);
    const auto speed = [&] {
        return std::hypot(X86_MEMF32(vehicle + 0x80), X86_MEMF32(vehicle + 0x88));
    };
    static thread_local uint32_t tracked = 0, samples = 0;
    static thread_local std::chrono::steady_clock::time_point drive_start;
    static thread_local bool drive_complete = false;
    const bool straight = mode >= 6;
    const bool driving = mode >= 4;
    if (straight && !drive_test_vehicle && request_drive_test.load() && PlayerDrivesVehicle(base, vehicle)) {
        request_drive_test = false;
        if (speed() <= 0.005f) {
            drive_test_vehicle = vehicle;
            drive_test_tick = X86_MEM32(kSimTicks) + 1;
            LOGR("straight drive: armed vehicle %08X model=%d", vehicle, int(X86_MEM16(vehicle + 0x448)));
        } else {
            LOGR("straight drive: stop the vehicle before pressing F7");
        }
    }
    if (straight) {
        if (!drive_test_vehicle || vehicle != drive_test_vehicle || !PlayerDrivesVehicle(base, vehicle))
            return __imp__sub_001CA180(ctx, base);
        const uint32_t elapsed = X86_MEM32(kSimTicks) - drive_test_tick;
        if (elapsed >= kThrottleTicks + kBrakeTicks)
            return __imp__sub_001CA180(ctx, base);
    }
    if (driving && tracked && !drive_complete && std::chrono::steady_clock::now() - drive_start >= std::chrono::minutes(1)) {
        drive_complete = true;
        LOGR("vehicle drive: complete, %u samples", samples);
    }
    if (!straight && ((driving ? drive_complete || !PlayerDrivesVehicle(base, vehicle) : samples >= 60) ||
        (tracked && vehicle != tracked) || step < 0.99f || step > 1.01f || X86_MEM32(vehicle + 0x28) ||
        (!tracked && speed() < 0.05f) || (!driving && speed() < 0.05f)))
        return __imp__sub_001CA180(ctx, base);

    if (driving && !straight && !tracked) {
        drive_start = std::chrono::steady_clock::now();
        LOGR("vehicle drive: started %s capture for vehicle %08X", mode == 5 ? "motion" : "baseline", vehicle);
    }
    tracked = vehicle;
    const float x = X86_MEMF32(vehicle + 0x70);
    const float z = X86_MEMF32(vehicle + 0x78);
    const float before = speed();
    if (mode == 2) {
        const X86Context entry = ctx;
        X86_MEMF32(kWorldStep) = step * 0.5f;
        __imp__sub_001CA180(ctx, base);
        ctx = entry;
        X86_MEM32(ctx.esp) = kVehicleStepCall;
    }
    motion_calls = 0;
    if (mode == 3 || mode == 5 || mode == 7) split_motion_vehicle = vehicle;
    __imp__sub_001CA180(ctx, base);
    split_motion_vehicle = 0;
    X86_MEMF32(kWorldStep) = step;
    const char *label = mode == 3 || mode == 5 || mode == 7 ? "motion" : mode == 2 ? "split" : "full";
    LOGR("vehicle step: %s sample=%u tick=%u id=%08X model=%d dx=%.6f dz=%.6f speed=%.6f->%.6f flags=%08X motion=%u",
         label, ++samples, X86_MEM32(kSimTicks), vehicle, int(X86_MEM16(vehicle + 0x448)),
         X86_MEMF32(vehicle + 0x70) - x, X86_MEMF32(vehicle + 0x78) - z, before, speed(), X86_MEM32(vehicle + 0x14),
         motion_calls);
    if (straight && X86_MEM32(kSimTicks) - drive_test_tick == kThrottleTicks + kBrakeTicks - 1)
        LOGR("straight drive: complete, %u samples", samples);
}
#endif

namespace {
void PrepareGameplayTiming(uint8_t *base) {
    fast_gameplay = !X86_MEM8(kAudioClockMode) && AtGameFrameRate() && !timing::Enabled();
#if TCNYC_DEV_TOOLS
    const bool native = fast_gameplay && !VehicleStepProbe() && NativeGameplayEnabled(base);
    if (native != native_gameplay)
        LOGR("tcnyc: native %u FPS gameplay %s", NativeGameplayCap(), native ? "active" : "inactive");
    native_gameplay = native;
#endif
    if (!fast_gameplay) return;
    const uint32_t last = X86_MEM32(kLastVblank);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
    sampled_vblank = X86_MEM32(kVblank);
    // A frame can arrive before the next guest vblank on a fast host.
    while (sampled_vblank == last && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        sampled_vblank = X86_MEM32(kVblank);
    }
}
} // namespace

namespace {
bool HasState(uint8_t *base, uint32_t script, uint32_t state) {
    const uint32_t table = X86_MEM32(script + kStateTable);
    if (!table || !mem::IsCommitted(table + 3)) return false;
    const uint32_t count = std::min<uint32_t>(X86_MEM16(table), 256);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t entry = table + 4 + i * 16;
        if (!mem::IsCommitted(entry + 7)) return false;
        if (X86_MEM32(entry + 4) == state) return true;
    }
    return false;
}

// The object of the character whose scripts are running (0xFCFD0 reads it
// the same way): the owner stack's top slot holds the owner, +0 its object.
uint32_t ScriptObject(uint8_t *base) {
    const int32_t depth = int32_t(X86_MEM32(kScriptOwnerDepth));
    const uint32_t owner = depth >= 0 && depth < 8 ? X86_MEM32(kScriptOwnerStack + uint32_t(depth) * 4) : 0;
    return owner ? X86_MEM32(owner) : 0;
}

// Zeke: the object whose script has a callingMarcus state, and that script's state.
thread_local uint32_t zeke_object = 0;
thread_local uint32_t zeke_state = 0;
// Set while a FollowWaypoints script instance updates during Escape Crazy.
thread_local bool in_follow_waypoints = false;
constexpr uint32_t kFollowState = guest::Crc32("follow");
} // namespace

// Obstacle navigation can steer Zeke through the window at 60 FPS. His
// starting position and waiting waypoint are on the same side, so he can use
// the normal movement controller directly for this leg.
X86_EXTERN_FUNC(__imp__sub_001785A0); // EnableObstacleNav
X86_EXTERN_FUNC(__imp__sub_001785D0); // DisableObstacleNav
X86_FUNC(sub_001785A0) {
    if (fast_gameplay && zeke_object && zeke_state == kCallingMarcus && ScriptObject(base) == zeke_object)
        return __imp__sub_001785D0(ctx, base);
    __imp__sub_001785A0(ctx, base);
}

// The mission sends this query when the last shower enemy is defeated. The
// orderly's OnQuery answers it with SetState(unlockingShower); that state's
// init unlocks the four shower doors, walks him through and then chases
// Marcus. At 60 FPS the request can be lost, leaving the doors locked and the
// orderly in his previous state. Keep the request pending until his own
// script has entered the state, so the doors open the way the game does it.
thread_local bool shower_unlock_pending = false;

X86_EXTERN_FUNC(__imp__sub_00182830); // QueryID
X86_FUNC(sub_00182830) {
    const uint32_t vm = X86_MEM32(ctx.esp + 4);
    if (fast_gameplay && X86_MEM32(vm - 4) == kUnlockShower && X86_MEM32(vm - 8) == kShowerOrderlyId &&
        IsEscapeCrazy(base))
        shower_unlock_pending = true;
    __imp__sub_00182830(ctx, base);
}


// Called before each script instance's update (0xFD220, ecx = instance),
// whichever character update runs it. Tracks Zeke's state for the nav hook
// above, and re-issues the orderly's pending state change until his own
// script has entered it.
namespace game {
void BeforeScriptUpdate(uint8_t *base, uint32_t instance) {
    in_follow_waypoints = false;
    if (!IsEscapeCrazy(base)) {
        shower_unlock_pending = false;
        zeke_object = zeke_state = 0;
        return;
    }
    const uint32_t object = ScriptObject(base);
    if (!object) return;
    const uint32_t state = X86_MEM32(instance + kScriptState);
    // FollowWaypoints (AI_Global) has a single state, follow
    if (state == kFollowState) {
        const uint32_t table = X86_MEM32(instance + kStateTable);
        in_follow_waypoints = table && mem::IsCommitted(table + 1) && X86_MEM16(table) == 1;
    }
    if (HasState(base, instance, kCallingMarcus)) {
        zeke_object = object;
        zeke_state = state;
    }
    if (shower_unlock_pending && int16_t(X86_MEM16(object + kObjectId)) == int16_t(kShowerOrderlyId) &&
        HasState(base, instance, kUnlockingShower)) {
        if (state == kUnlockingShower) {
            shower_unlock_pending = false;
            return;
        }
        if (X86_MEM32(instance + kRequestedState) != kUnlockingShower)
            LOGR("tcnyc: Escape Crazy shower orderly missed UnlockShower! (state %08X, next %08X)", state,
                 X86_MEM32(instance + kRequestedState));
        X86_MEM32(instance + kRequestedState) = kUnlockingShower;
    }
}
} // namespace game

// Distance native (0x1806B0): from the running character to a point, in 3D.
// FollowWaypoints counts a waypoint as reached within its arrival range. One
// of Escape Crazy's patrol waypoints sits at the upper floor's height, 1.56 m
// above the corridor its inmate walks, so he stops under it and turns on the
// spot for good (on the Xbox too). For FollowWaypoints in this mission,
// measure on the ground plane so he reaches it and carries on.
X86_EXTERN_FUNC(__imp__sub_001806B0);
X86_FUNC(sub_001806B0) {
    if (in_follow_waypoints) {
        const uint32_t object = ScriptObject(base);
        if (object && !X86_MEM32(object + 0x28)) // no parent: +0x70 is the world position
            X86_MEM32(X86_MEM32(ctx.esp + 4) - 8) = X86_MEM32(object + 0x74); // the point's height
    }
    __imp__sub_001806B0(ctx, base);
}

#if !TCNYC_DEV_TOOLS
// Developer builds hook this update in dev_scripts.cpp, which calls BeforeScriptUpdate.
X86_EXTERN_FUNC(__imp__sub_000FD220);
X86_FUNC(sub_000FD220) {
    game::BeforeScriptUpdate(base, ctx.ecx);
    __imp__sub_000FD220(ctx, base);
}
#endif

// The main loop's simulation step uses elapsed vblanks in 60 FPS gameplay.
// Cutscenes keep the clock calculation in their region's guest loop.
X86_EXTERN_FUNC(__imp__sub_000A5200);
X86_FUNC(sub_000A5200) {
    if (X86_MEM32(ctx.esp) != kMainLoopSimulate) return __imp__sub_000A5200(ctx, base);
#if TCNYC_PAL
    // PAL gameplay computes its step inline from the performance counter,
    // with no per-frame call to the vblank reader before simulation.
    PrepareGameplayTiming(base);
    fast_cutscene = X86_MEM8(kAudioClockMode) && AtGameFrameRate() && !timing::Enabled();
#endif
#if TCNYC_DEV_TOOLS
    static const bool trace_audio = [] {
        const char* flag = DevEnv("TCNYC_TEST_AUDIO_TRACE");
        return flag && std::strcmp(flag, "1") == 0;
    }();
    const auto native_start = native_gameplay || trace_audio ? NativeClock::now() : NativeClock::time_point{};
    if (trace_audio) {
        thread_local NativeClock::time_point last_update;
        if (last_update != NativeClock::time_point{}) {
            const double ms = std::chrono::duration<double, std::milli>(native_start - last_update).count();
            if (ms >= 35.0)
                LOGI("audio trace: game update interval %.1f ms, thread %zx", ms,
                     std::hash<std::thread::id>{}(std::this_thread::get_id()));
        }
        last_update = native_start;
    }
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
#if TCNYC_PAL
    if (timing::Enabled()) {
        timing::Load();
        const int64_t recorded = timing::Next(0);
        if (recorded >= 0) frame_ticks = uint32_t(recorded);
        timing::Write('t', frame_ticks);
        timing::Write('c', X86_MEM32(kRandomState) ^ (X86_MEM32(kSimTicks) * 2654435761u));
    }
#endif
#if TCNYC_DEV_TOOLS
    frame_ticks = dev::AdjustTicks(frame_ticks); // debug menu: pause, slow motion, fast forward
#endif
    TraceFrame(X86_MEM32(kSimTicks), X86_MEM8(kAudioClockMode) != 0, frame_ticks);
    X86_MEM32(arg) = frame_ticks;
#if TCNYC_DEV_TOOLS
    drive_test_simulating = drive_test_vehicle && fast_gameplay && PlayerDrivesVehicle(base, drive_test_vehicle);
#endif
    __imp__sub_000A5200(ctx, base);
#if TCNYC_DEV_TOOLS
    drive_test_simulating = false;
    if (native_gameplay || trace_audio) {
        const double ms = std::chrono::duration<double, std::milli>(NativeClock::now() - native_start).count();
        if (native_gameplay) native_sim_ms = ms;
        if (trace_audio && ms >= 25.0)
            LOGI("audio trace: game update took %.1f ms, thread %zx", ms,
                 std::hash<std::thread::id>{}(std::this_thread::get_id()));
    }
#endif
    use_sampled_vblank = false;
    X86_MEM32(arg) = original_ticks;
}

#if TCNYC_DEV_TOOLS
X86_EXTERN_FUNC(__imp__sub_00242F20);
X86_FUNC(sub_00242F20) {
    // Render begin waits when this Xbox queue exceeds one entry. The HLE
    // renderer already paces and synchronizes its own queue at Swap.
    if (native_gameplay && X86_MEM32(ctx.esp) == kRenderBeginFromMainRender)
        X86_MEM32(kGuestRenderQueueDepth) = 0;
    __imp__sub_00242F20(ctx, base);
}
#endif

// The main loop's render step gets the same tick count as the simulation.
X86_EXTERN_FUNC(__imp__sub_000A73F0);
X86_FUNC(sub_000A73F0) {
    if (X86_MEM32(ctx.esp) != kMainLoopRender) return __imp__sub_000A73F0(ctx, base);
    ++guest_renders;
#if TCNYC_DEV_TOOLS
    const auto native_start = native_gameplay ? NativeClock::now() : NativeClock::time_point{};
    const bool dev_speed = dev::GameSpeed() != 4;
    double_render = ShouldDoubleRender(base);
    if (double_render) render_entry = ctx;
#else
    constexpr bool dev_speed = false;
#endif
    if (!fast_gameplay && !fast_cutscene && !dev_speed && !(TCNYC_PAL && timing::Enabled()))
        return __imp__sub_000A73F0(ctx, base);
    const uint32_t arg = ctx.esp + 4;
    const uint32_t original_ticks = X86_MEM32(arg);
    X86_MEM32(arg) = frame_ticks;
    __imp__sub_000A73F0(ctx, base);
#if TCNYC_DEV_TOOLS
    if (native_gameplay) native_render_ms = std::chrono::duration<double, std::milli>(NativeClock::now() - native_start).count();
#endif
    X86_MEM32(arg) = original_ticks;
}

// The main loop presents after its render function returns.
X86_EXTERN_FUNC(__imp__sub_00242F30);
X86_FUNC(sub_00242F30) {
    if (X86_MEM32(ctx.esp) != kMainLoopPresent) return __imp__sub_00242F30(ctx, base);
    ++guest_swaps;
    if (!fast_gameplay && !fast_cutscene)
        return __imp__sub_00242F30(ctx, base);
    fast_present = true;
#if TCNYC_DEV_TOOLS
    if (double_render) {
        const uint32_t swap_flags = X86_MEM32(ctx.esp + 4);
        __imp__sub_00242F30(ctx, base);
        const X86Context after_swap = ctx;

        // The swap argument overwrites the render return slot. The render
        // arguments and matrix are still in the caller frame.
        ctx = render_entry;
        X86_MEM32(ctx.esp) = kMainLoopRender;
        ++guest_renders;
        __imp__sub_000A73F0(ctx, base);
        ctx.esp -= 4;
        X86_MEM32(ctx.esp) = swap_flags;
        ctx.esp -= 4;
        X86_MEM32(ctx.esp) = kMainLoopPresent;
        ++guest_swaps;
        __imp__sub_00242F30(ctx, base);
        ctx = after_swap;
        double_render = false;
        fast_present = false;
        return;
    }
#endif
#if TCNYC_DEV_TOOLS
    const auto native_start = native_gameplay ? NativeClock::now() : NativeClock::time_point{};
#endif
    __imp__sub_00242F30(ctx, base);
#if TCNYC_DEV_TOOLS
    if (native_gameplay) {
        const auto end = NativeClock::now();
        static thread_local unsigned frames = 0;
        if (native_last_swap != NativeClock::time_point{} && ++frames % 60 == 0) {
            const auto ms = [](auto duration) { return std::chrono::duration<double, std::milli>(duration).count(); };
            LOGR("native pacing: frame %.3f ms, sim %.3f, render %.3f, swap %.3f",
                 ms(end - native_last_swap), native_sim_ms, native_render_ms, ms(end - native_start));
        }
        native_last_swap = end;
    } else {
        native_last_swap = {};
    }
#endif
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
#if !TCNYC_PAL
    if (ret == kMainLoopVblank) {
        PrepareGameplayTiming(base);
        if (fast_gameplay) {
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
#else
    __imp__sub_00243000(ctx, base);
#endif
}

#if !defined(TCNYC_PAL) || !TCNYC_PAL
// US cutscenes use the DirectSound sample counter. PAL uses QPC instead.
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
#if TCNYC_DEV_TOOLS
        if (X86_MEM8(kAudioClockMode)) native_gameplay = false;
#endif
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
#endif
