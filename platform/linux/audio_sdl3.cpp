// Native Linux audio sink: SDL3 pulls a small pre-rendered ring into the
// desktop's audio stack (PulseAudio, PipeWire or ALSA via SDL). Rendering ahead
// by a few blocks keeps a guest mixer stall from turning into an underrun.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>

#include "audio/audio.h"
#include "core/log.h"

namespace audio {

namespace {

constexpr uint32_t kBlockFrames = kRate / 100;  // 10 ms
constexpr uint32_t kBlocks = 6;                 // 60 ms of slack
constexpr size_t kBlockSamples = size_t(kBlockFrames) * 2;

std::function<void(float*, uint32_t)> g_render;
SDL_AudioStream* g_stream = nullptr;
std::atomic<bool> g_suspended{false};
std::atomic<bool> g_stop{false};

std::mutex g_ring_lock;
std::condition_variable g_ring_cv;
std::vector<float> g_ring(kBlockSamples * kBlocks);
size_t g_read = 0, g_write = 0, g_count = 0;

void RingClear() {
    std::lock_guard<std::mutex> lock(g_ring_lock);
    g_read = g_write = g_count = 0;
    g_ring_cv.notify_all();
}

void RingPush(const float* src, size_t n) {
    for (size_t i = 0; i < n; i++) {
        g_ring[g_write] = src[i];
        g_write = (g_write + 1) % g_ring.size();
    }
    g_count += n;
}

size_t RingPop(float* dst, size_t n) {
    const size_t take = n < g_count ? n : g_count;
    for (size_t i = 0; i < take; i++) {
        dst[i] = g_ring[g_read];
        g_read = (g_read + 1) % g_ring.size();
    }
    g_count -= take;
    return take;
}

void Produce() {
    std::vector<float> block(kBlockSamples);
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(g_ring_lock);
            g_ring_cv.wait(lock, [] {
                return g_stop.load() || (!g_suspended.load() && g_count + kBlockSamples <= g_ring.size());
            });
            if (g_stop.load()) return;
        }
        if (g_suspended.load()) continue;
        if (g_render) g_render(block.data(), kBlockFrames);
        else std::fill(block.begin(), block.end(), 0.0f);
        {
            std::lock_guard<std::mutex> lock(g_ring_lock);
            if (g_stop.load() || g_suspended.load()) continue;
            RingPush(block.data(), block.size());
        }
        g_ring_cv.notify_all();
    }
}

void SDLCALL Pull(void*, SDL_AudioStream* stream, int additional, int) {
    if (additional <= 0) return;
    const size_t frames = size_t(additional) / (2 * sizeof(float));
    static thread_local std::vector<float> buffer;
    if (buffer.size() < frames * 2) buffer.resize(frames * 2);
    size_t got = 0;
    {
        std::lock_guard<std::mutex> lock(g_ring_lock);
        got = RingPop(buffer.data(), frames * 2);
    }
    if (got < frames * 2) std::fill(buffer.begin() + got, buffer.begin() + frames * 2, 0.0f);
    if (got < frames * 2) LOG_FIRST(4, "audio: mixer underrun, filling %zu frames with silence", frames - got / 2);
    g_ring_cv.notify_all();
    SDL_PutAudioStreamData(stream, buffer.data(), int(frames * 2 * sizeof(float)));
}

bool OpenStream() {
    const SDL_AudioSpec spec{SDL_AUDIO_F32, 2, kRate};
    g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, Pull, nullptr);
    if (!g_stream) return false;
    RingClear();
    if (g_suspended.load()) SDL_PauseAudioStreamDevice(g_stream);
    else SDL_ResumeAudioStreamDevice(g_stream);
    LOGI("audio: %s at %u Hz", SDL_GetCurrentAudioDriver(), kRate);
    return true;
}

// no device: still call render at the same pace, the mixer is also the game's clock.
// Runs on its own thread so a host without audio does not block guest boot.
void RenderSilently(std::function<void(float*, uint32_t)> render) {
    std::vector<float> b(size_t(kRate / 100) * 2);
    for (;;) {
        if (g_suspended.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        render(b.data(), kRate / 100);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

} // namespace

void StartOutput(std::function<void(float*, uint32_t)> render) {
    g_render = std::move(render);
    g_stop.store(false);
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        LOGE("audio: SDL audio failed: %s, no sound", SDL_GetError());
        std::thread(RenderSilently, g_render).detach();
        return;
    }
    if (!OpenStream()) {
        LOGE("audio: no output device (%s), no sound", SDL_GetError());
        std::thread(RenderSilently, g_render).detach();
        return;
    }
    std::thread(Produce).detach();
}

// Suspend/resume keeps the stream and producer alive; a device change rebuilds
// the SDL stream while the ring absorbs the gap.
void SetSuspended(bool suspended) {
    g_suspended.store(suspended, std::memory_order_relaxed);
    g_ring_cv.notify_all();
    if (!g_stream) return;
    if (suspended) SDL_PauseAudioStreamDevice(g_stream);
    else SDL_ResumeAudioStreamDevice(g_stream);
}

void DeviceChanged() {
    if (!g_stream) return;
    SDL_DestroyAudioStream(g_stream);
    g_stream = nullptr;
    if (!OpenStream()) LOGE("audio: could not reopen the default output device: %s", SDL_GetError());
}

} // namespace audio
