// mixer output through XAudio2
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <xaudio2.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "app/app.h"
#include "audio/audio.h"
#include "core/log.h"

namespace audio {
namespace {

struct Output : IXAudio2EngineCallback {
    IXAudio2* engine = nullptr;
    IXAudio2MasteringVoice* master = nullptr;
    IXAudio2SourceVoice* voice = nullptr;
    std::atomic<HRESULT> error{S_OK};
    bool registered = false;

    void STDMETHODCALLTYPE OnProcessingPassStart() noexcept override {}
    void STDMETHODCALLTYPE OnProcessingPassEnd() noexcept override {}
    void STDMETHODCALLTYPE OnCriticalError(HRESULT hr) noexcept override { error.store(hr); }

    ~Output() {
        if (registered) engine->UnregisterForCallbacks(this);
        if (voice) voice->DestroyVoice();
        if (master) master->DestroyVoice();
        if (engine) engine->Release();
    }
};

// The default render endpoint's id; empty when Core Audio cannot be read.
std::wstring DefaultRenderDevice() {
    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))))
        return {};
    std::wstring id;
    IMMDevice* device = nullptr;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))) {
        LPWSTR text = nullptr;
        if (SUCCEEDED(device->GetId(&text))) {
            id = text;
            CoTaskMemFree(text);
        }
        device->Release();
    }
    enumerator->Release();
    return id;
}

}  // namespace

void StartOutput(std::function<void(float*, uint32_t)> render) {
    std::thread([render] {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(com)) {
            LOGE("audio: COM initialization failed (0x%08X)", unsigned(com));
            return;
        }
        WAVEFORMATEX wf{};
        wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        wf.nChannels = 2;
        wf.nSamplesPerSec = kRate;
        wf.wBitsPerSample = 32;
        wf.nBlockAlign = 8;
        wf.nAvgBytesPerSec = kRate * 8;
        // The mingw toolchains carry no XAudio2 import library; the DLL is part of
        // Windows 8 and later, so bind it at run time.
        using CreateFn = HRESULT(WINAPI*)(IXAudio2**, UINT32, XAUDIO2_PROCESSOR);
        using CreateVersionFn = HRESULT(WINAPI*)(IXAudio2**, UINT32, XAUDIO2_PROCESSOR, DWORD);
        HMODULE dll = LoadLibraryW(L"XAudio2_8.dll");
        CreateFn create = dll ? reinterpret_cast<CreateFn>(reinterpret_cast<void*>(
                                   GetProcAddress(dll, "XAudio2Create"))) : nullptr;
        CreateVersionFn create_version = nullptr;
        if (!create && dll)
            create_version = reinterpret_cast<CreateVersionFn>(
                reinterpret_cast<void*>(GetProcAddress(dll, "XAudio2CreateWithVersionInfo")));
        if (!create && !create_version) {
            LOGE("audio: XAudio2 could not be loaded");
            if (dll) FreeLibrary(dll);
            CoUninitialize();
            return;
        }
        constexpr uint32_t kFrames = kRate / 100;
        constexpr uint32_t kMinQueued = 3;
        constexpr uint32_t kMaxQueued = 10;
        // one slot more than can be queued, so the one rendered is never one XAudio2 still reads
        constexpr uint32_t kSlots = kMaxQueued + 1;
        std::vector<float> bufs(kSlots * kFrames * 2);
        bool recovering = false;
        DWORD retry_delay = 250;
        std::wstring device = DefaultRenderDevice();
        DWORD device_checked = GetTickCount();
        for (;;) {
            {
                Output output;
                const char* operation = "create engine";
                HRESULT hr = create ? create(&output.engine, 0, XAUDIO2_DEFAULT_PROCESSOR)
                                    : create_version(&output.engine, 0, XAUDIO2_DEFAULT_PROCESSOR, 0x0A000000);
                if (SUCCEEDED(hr)) {
                    operation = "register engine callback";
                    hr = output.engine->RegisterForCallbacks(&output);
                    output.registered = SUCCEEDED(hr);
                }
                if (SUCCEEDED(hr)) {
                    operation = "open default output";
                    // The mixer always renders stereo 48 kHz. Some HDMI/TV
                    // endpoints renegotiate and invalidate a non-stereo
                    // mastering voice continuously, making audio cut in and
                    // out; the launcher option asks for stereo in that case.
                    // Otherwise use the endpoint's own format so a TV or
                    // receiver can take over Dolby processing.
                    if (app::ForceStereoOutput())
                        hr = output.engine->CreateMasteringVoice(&output.master, 2, kRate);
                    else
                        hr = output.engine->CreateMasteringVoice(&output.master);
                }
                if (SUCCEEDED(hr)) {
                    operation = "create source voice";
                    hr = output.engine->CreateSourceVoice(&output.voice, &wf);
                }
                auto submit = [&](uint64_t slot) {
                    float* b = &bufs[(slot % kSlots) * kFrames * 2];
                    render(b, kFrames);
                    XAUDIO2_BUFFER xb{};
                    xb.AudioBytes = kFrames * 8;
                    xb.pAudioData = reinterpret_cast<const BYTE*>(b);
                    return output.voice->SubmitSourceBuffer(&xb);
                };
                uint64_t submitted = 0;
                if (SUCCEEDED(hr)) {
                    operation = "prime source voice";
                    for (; submitted < kMinQueued && SUCCEEDED(hr); submitted++) hr = submit(submitted);
                }
                if (SUCCEEDED(hr)) {
                    operation = "start source voice";
                    hr = output.voice->Start();
                }
                if (SUCCEEDED(hr)) {
                    if (recovering) LOGR("audio: XAudio2 output resumed");
                    recovering = false;
                    retry_delay = 250;
                }
                uint32_t depth = kMinQueued;
                DWORD stable_since = GetTickCount();
                DWORD last_report = stable_since - 1000;
                while (SUCCEEDED(hr)) {
                    operation = "engine callback";
                    hr = output.error.load();
                    if (FAILED(hr)) break;
                    // Follow a change of Windows' default output device.
                    if (GetTickCount() - device_checked >= 1000) {
                        device_checked = GetTickCount();
                        std::wstring now = DefaultRenderDevice();
                        if (!now.empty() && !device.empty() && now != device) {
                            LOGI("audio: default output changed, reopening XAudio2");
                            hr = XAUDIO2_E_DEVICE_INVALIDATED;
                            break;
                        }
                        if (!now.empty()) device = std::move(now);
                    }
                    XAUDIO2_VOICE_STATE state{};
                    output.voice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
                    const DWORD now = GetTickCount();
                    if (!state.BuffersQueued) {
                        if (depth < kMaxQueued) depth++;
                        stable_since = now;
                        if (now - last_report >= 1000) {
                            LOGR("audio: XAudio2 output underrun, queue target %u ms", depth * 10);
                            last_report = now;
                        }
                    } else if (depth > kMinQueued && now - stable_since > 5000) {
                        depth--;
                        stable_since = now;
                    }
                    if (state.BuffersQueued >= depth) {
                        Sleep(2);
                        continue;
                    }
                    operation = "submit source buffer";
                    hr = submit(submitted++);
                }
                const HRESULT critical = output.error.load();
                if (FAILED(critical)) {
                    operation = "engine callback";
                    hr = critical;
                }
                if (!recovering || retry_delay == 250)
                    LOGE("audio: XAudio2 %s failed (0x%08X), reopening output", operation, unsigned(hr));
            }
            recovering = true;
            Sleep(retry_delay);
            if (retry_delay < 2000) retry_delay *= 2;
        }
    }).detach();
}

}  // namespace audio
