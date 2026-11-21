// the host side without a window: settings at their defaults, the run folder as the exe folder
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <thread>

#include "app/app.h"

namespace app {

Settings& Get() {
    static Settings s;
    return s;
}
void Load() {}
bool Save() { return true; }
void ApplyDisplay() {}
Mode Desktop() { return {1280, 720}; }
Mode Output() { return Desktop(); }
float RenderScale(uint32_t) { return 1.0f; }
bool Widescreen() { return true; }

std::filesystem::path ExeDir() {
    static const std::filesystem::path dir = std::filesystem::current_path();
    return dir;
}
int Instance() {
    static const int n = [] {
        const char* e = std::getenv("INSTANCE");
        return e ? std::atoi(e) : 0;
    }();
    return n;
}
void* Window() { return nullptr; }

bool OpenSplash() { return false; }
void SplashFirstFrame() {}
void CloseSplash() {}
void OpenGameWindow(bool) {}
bool StartHandover() { return false; }
bool HandingOver() { return false; }
void FinishHandover() {}

// no window to switch or close: a display change is refused straight away
uint64_t RequestDisplayApply(DisplayMode, uint32_t, uint32_t) { return 0; }
bool DisplayApplyResult(uint64_t, bool& ok) {
    ok = false;
    return true;
}
bool RequestQuit() { return false; }
std::string HddName() { return "hdd"; }
uint32_t AudioMode() { return 0; }

std::recursive_mutex& UiLock() {
    static std::recursive_mutex m;
    return m;
}

Settings Snapshot() { return Get(); }
SettingsLock::SettingsLock() {}
SettingsLock::~SettingsLock() {}
float AspectRatio() { return 16.0f / 9.0f; }
uint32_t FrameCap() { return 60; }
void SetFrameCap(uint32_t) {}
uint32_t MsaaSamples() { return 0; }
void SetMsaaSamples(uint32_t) {}
void CaptureMouse(bool) {}

void Pace(uint32_t, uint32_t) {}

}  // namespace app
