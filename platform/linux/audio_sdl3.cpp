// Native Linux audio sink: SDL3 pulls the game's mixer output straight into the
// desktop's audio stack (PulseAudio, PipeWire or ALSA via SDL).
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>

#include "audio/audio.h"
#include "core/log.h"

namespace audio {

namespace {

std::function<void(float*, uint32_t)> g_render;

void SDLCALL Pull(void*, SDL_AudioStream* stream, int additional, int) {
    if (additional <= 0) return;
    const int frames = additional / int(2 * sizeof(float));
    static thread_local std::vector<float> buffer;
    if (buffer.size() < size_t(frames) * 2) buffer.resize(size_t(frames) * 2);
    if (g_render) g_render(buffer.data(), uint32_t(frames));
    else std::fill(buffer.begin(), buffer.end(), 0.0f);
    SDL_PutAudioStreamData(stream, buffer.data(), frames * int(2 * sizeof(float)));
}

// no device: still call render at the same pace, the mixer is also the game's clock.
// Runs on its own thread so a host without audio does not block guest boot.
void RenderSilently(std::function<void(float*, uint32_t)> render) {
    std::vector<float> b(size_t(kRate / 100) * 2);
    for (;;) {
        render(b.data(), kRate / 100);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

} // namespace

void StartOutput(std::function<void(float*, uint32_t)> render) {
    g_render = std::move(render);
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        LOGE("audio: SDL audio failed: %s, no sound", SDL_GetError());
        std::thread(RenderSilently, g_render).detach();
        return;
    }
    const SDL_AudioSpec spec{SDL_AUDIO_F32, 2, kRate};
    SDL_AudioStream* stream =
        SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, Pull, nullptr);
    if (!stream) {
        LOGE("audio: no output device (%s), no sound", SDL_GetError());
        std::thread(RenderSilently, g_render).detach();
        return;
    }
    SDL_ResumeAudioStreamDevice(stream);
    LOGI("audio: %s at %u Hz", SDL_GetCurrentAudioDriver(), kRate);
}

} // namespace audio
