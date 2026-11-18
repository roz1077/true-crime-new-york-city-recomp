#include "game_addresses.h"
#include "core/dev_env.h"
// A non-positive mix-bin volume can prevent a stream from starting.
//
// 0x25DA90 starts a voice/music/SFX stream. Before starting it, it compares
// the mix-bin volume from 0x2291F0 (a float table at 0x337640, indexed by the
// stream's bin) against zero. The old music fade could overshoot into
// negative values and leave later tracks unable to start. Missions
// that wait for a phone call to finish then never advance (no informant
// missions, no "Precinct Cleaned" call).
//
// The table holds the player's own settings, so it is left alone: the getter's
// result is changed instead, and only while 0x25DA90 is starting a stream.
// A non-positive value becomes a tiny positive value for the start check.
// The stream can then start silently, run its completion callback, and
// become audible if the mix-bin gain is restored during playback.

#include "app/app.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "xdk/hle.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#if TCNYC_DEV_TOOLS
#include <array>
#include <chrono>
#include <mutex>
#include <thread>
#endif

namespace {
constexpr double kSilentButPlaying = 1.0e-6;
constexpr uint32_t kMusicBin = 1;
constexpr uint32_t kVolumeTable = TCNYC_PAL_EU ? 0x337D00u : TCNYC_PAL ? 0x337E00u : 0x337640u;
thread_local int t_stream_start = 0;  // inside 0x25DA90 on this thread

// The launcher's forced mix writes the game's volume table directly. The
// music bin is held back while a music fade is active, so radio ducking and
// fades still work.
void ApplyForcedVolumes(bool music) {
    const app::Settings& s = app::Get();
    if (!s.force_volume_mix) return;
    const float master = float(std::clamp<uint32_t>(s.sound_volume, 0, 100)) / 100.0f;
    const auto set = [&](uint32_t bin, uint32_t percent) {
        const float value = master * float(std::clamp<uint32_t>(percent, 0, 100)) / 100.0f;
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        kernel::W32(kVolumeTable + bin * 4, bits);
    };
    set(0, s.sfx_volume);
    set(2, s.ambient_volume);
    set(3, s.voice_volume);
    set(4, s.cinema_volume);
    if (music) set(kMusicBin, s.music_volume);
}
} // namespace

#if TCNYC_DEV_TOOLS
namespace game {
namespace {
struct RefillEvent {
    uint32_t voice, result, decoder_flags;
    const char* step;
    int64_t start_ms, end_ms;
    size_t thread;
};

std::mutex refill_lock;
std::array<RefillEvent, 256> refill_events{};
size_t refill_next = 0;

int64_t RefillClock() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
}

bool TraceAudioRefillCall(X86Func* fn, X86Context& ctx, uint8_t* base) {
    // Return addresses inside the title's streamed-audio refill loop.
    const uint32_t caller = kernel::R32(ctx.esp);
    constexpr uint32_t pal_offset = TCNYC_PAL_EU ? 0x5D0u : TCNYC_PAL ? 0x6A0u : 0u;
    const char* step;
    switch (caller - pal_offset) {
        case 0x2499AA: step = "poll observed free slot / check decoder"; break;
        case 0x249A11:
        case 0x249A80: step = "decode"; break;
        case 0x249A47:
        case 0x249B02: step = "submit"; break;
        default: return false;
    }

    const uint32_t stream = ctx.esi;
    const uint32_t voice = kernel::R32(stream);
    const int64_t start = RefillClock();
    fn(ctx, base);
    const int64_t end = RefillClock();
    const uint32_t decoder_flags = caller == 0x2499AAu + pal_offset ? kernel::R32(stream + 0x11C) : 0;
    std::lock_guard lock(refill_lock);
    refill_events[refill_next++ % refill_events.size()] = {
        voice, ctx.eax, decoder_flags, step, start, end, std::hash<std::thread::id>{}(std::this_thread::get_id())};
    return true;
}

void ReportAudioRefill(uint32_t voice, int64_t completion_ms, int64_t submission_ms) {
    std::lock_guard lock(refill_lock);
    const size_t oldest = refill_next > refill_events.size() ? refill_next - refill_events.size() : 0;
    bool found = false;
    for (size_t i = oldest; i < refill_next; i++) {
        const RefillEvent& event = refill_events[i % refill_events.size()];
        if (event.voice != voice || event.end_ms < completion_ms - 100 || event.start_ms > submission_ms) continue;
        found = true;
        LOGI("audio trace: refill %08X: %+lld ms from last packet completion: %s took %lld ms, "
             "result %08X, decoder flags %08X, thread %zx",
             voice, static_cast<long long>(event.start_ms - completion_ms), event.step,
             static_cast<long long>(event.end_ms - event.start_ms), event.result, event.decoder_flags, event.thread);
    }
    if (!found) LOGI("audio trace: refill %08X: no guest refill calls captured", voice);
}
} // namespace game

#endif

namespace game {
uint32_t LastMusicTrack();  // extra_songs.cpp
namespace {
thread_local uint32_t t_radio_track;  // the Music Player song being started on this thread
}
uint32_t RadioOverrideTrack() { return t_radio_track; }
}

X86_EXTERN_FUNC(__imp__sub_0025DE90);
X86_FUNC(sub_0025DE90) {
    const uint32_t caller = X86_MEM32(ctx.esp);
    const uint32_t track = X86_MEM32(ctx.esp + 4);
    const uint32_t flags = X86_MEM32(ctx.esp + 8);
#if TCNYC_DEV_TOOLS
    static const bool trace = [] {
        const char* flag = DevEnv("TCNYC_TEST_RADIO_TRACE");
        return flag && std::strcmp(flag, "1") == 0;
    }();
    if (trace) {
        LOGR("radio trace: caller %08X stream %08X track %u flags %u",
             caller, ctx.ecx, track, flags);
    }
#endif
    // The Music Player previews a selection, restarts a song, or starts radio playback here.
    // Other callers start scripted music and can use the same song IDs.
    const bool music_player = TCNYC_PAL
        ? caller == 0x79AF1 || caller == 0x79B5A || caller == 0x7ACD4 || caller == 0x7AF86
        : caller == 0x799D1 || caller == 0x79A3A || caller == 0x7ABB4 || caller == 0x7AE66;
    const uint32_t previous = game::t_radio_track;
    game::t_radio_track = music_player && flags == 0 && track >= 6000 && track <= game::LastMusicTrack() ? track : 0;
    __imp__sub_0025DE90(ctx, base);
    game::t_radio_track = previous;
}

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
#if TCNYC_DEV_TOOLS
    const uint32_t caller = X86_MEM32(ctx.esp);
#endif
    __imp__sub_002291F0(ctx, base);
    if (t_stream_start && X87_ST(0) <= 0.0) {
        LOG_FIRST(8, "tcnyc: mix bin %u gain %.6g, allowing silent stream startup", bin, X87_ST(0));
        X87_ST(0) = kSilentButPlaying;
    }
#if TCNYC_DEV_TOOLS
    // Only the playback volume update for music streams, after the game's
    // volume and ducking are combined. Leave the saved table and startup check alone.
    constexpr uint32_t kStreamVolumeUpdate = TCNYC_PAL_EU ? 0x0024AC59u : TCNYC_PAL ? 0x24AD29u : 0x24A689u;
    if (bin == kMusicBin && caller == kStreamVolumeUpdate) {
        static const bool trace = [] {
            const char* flag = DevEnv("TCNYC_TEST_AUDIO_TRACE");
            return flag && std::strcmp(flag, "1") == 0;
        }();
        if (trace) {
            thread_local double previous = -1.0;
            const double gain = X87_ST(0);
            if (std::isfinite(gain) && (previous < 0.0 || (gain <= 0.0) != (previous <= 0.0) ||
                                        gain > previous * 1.122 || gain < previous / 1.122)) {
                LOG_FIRST(256, "audio trace: streamed music gain %.5f before test boost", gain);
                previous = gain;
            }
        }
        static const bool boost = [] {
            const char* flag = DevEnv("TCNYC_TEST_MUSIC_BOOST");
            return flag && std::strcmp(flag, "1") == 0;
        }();
        if (boost) {
            constexpr double kFiveDecibels = 1.7782794100;
            LOG_FIRST(1, "tcnyc: +5 dB test boost active for streamed music");
            X87_ST(0) *= kFiveDecibels;
        }
    }
#endif
}

X86_EXTERN_FUNC(__imp__sub_00229120);
X86_EXTERN_FUNC(__imp__sub_00250580);
X86_FUNC(sub_00250580) {
    constexpr uint32_t kUpdate = 0;  // event 0 in both XBE jump tables
    constexpr uint32_t kAudioManager = TCNYC_PAL_EU ? 0x0041F0C0u : TCNYC_PAL ? 0x41F1C0u : 0x41E8F0u;
    const uint32_t fade = ctx.ecx;
    if (X86_MEM32(ctx.esp + 4) != kUpdate) {
        __imp__sub_00250580(ctx, base);
        return;
    }
    if (!X86_MEM8(fade + 0x28)) {
        __imp__sub_00250580(ctx, base);
        ApplyForcedVolumes(true);
        return;
    }

    // The guest compares the target against 1.0 on every tick. A fade down
    // to a target below 1.0 therefore keeps subtracting past the target.
    const float target = X86_MEMF32(fade + 0x30);
    const float step = X86_MEMF32(fade + 0x38);
    const float current = X86_MEMF32(fade + 0x3C);
    const float next = current + step;
    const bool finished = !std::isfinite(next) || next == current ||
                          (step < 0.0f ? next <= target : next >= target);
    X86_MEMF32(fade + 0x3C) = finished ? target : next;
    if (finished) X86_MEM8(fade + 0x28) = 0;

    X86_PUSH32(X86_MEM32(fade + 0x3C));
    X86_PUSH32(kMusicBin);
    X86_PUSH32(TCNYC_PAL_EU ? 0x00250CCEu : TCNYC_PAL ? 0x250D6Eu : 0x25069Eu);  // return address after the guest setter call
    ctx.ecx = kAudioManager;
    __imp__sub_00229120(ctx, base);
    ApplyForcedVolumes(false);  // leave the fade's music level alone
    ctx.eax = 0;
    ctx.esp += 16;  // ret 0x0C
}
