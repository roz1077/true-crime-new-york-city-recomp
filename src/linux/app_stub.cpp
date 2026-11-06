// Linux/headless stand-ins for the settings accessors in app/settings.cpp.
// The native SDL3 host replaces these with the real settings file and display
// queries; the harness only needs the symbols the game code references.
#ifdef __linux__

#include <atomic>
#include <mutex>

#include "app/app.h"

namespace app {

namespace {
std::recursive_mutex g_lock;
}

Settings Snapshot() {
    std::lock_guard<std::recursive_mutex> guard(g_lock);
    return Get();
}

SettingsLock::SettingsLock() { g_lock.lock(); }
SettingsLock::~SettingsLock() { g_lock.unlock(); }

uint32_t FrameCap() {
    return std::atomic_ref(Get().fps_cap).load(std::memory_order_relaxed);
}

void SetFrameCap(uint32_t cap) {
    std::atomic_ref(Get().fps_cap).store(cap, std::memory_order_relaxed);
}

uint32_t MsaaSamples() {
    return std::atomic_ref(Get().msaa).load(std::memory_order_relaxed);
}

void SetMsaaSamples(uint32_t samples) {
    std::atomic_ref(Get().msaa).store(samples, std::memory_order_relaxed);
}

void LogSettings() {}
void LogSystem() {}

float AspectRatio() {
    return 16.0f / 9.0f;
}

float RenderStretch(uint32_t, uint32_t) {
    return 1.0f;
}

void Pace(uint32_t) {}

} // namespace app

#endif
