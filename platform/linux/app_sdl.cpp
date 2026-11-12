// Native Linux host: the app layer on SDL3. Replaces app/main.cpp and app/settings.cpp's
// Win32 display code for the native build. Settings persistence is the next step; the
// first windowed build uses the defaults and the same display/query contract.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#include <unistd.h>

#include <SDL3/SDL.h>

#include "app/app.h"
#include "core/log.h"

// app_sdl.cpp -> platform/linux/backend_sw.cpp
namespace app {
void SubmitFrame(const uint8_t* rgba, int width, int height);
}

namespace app {

namespace {

std::recursive_mutex g_settings_lock;
SDL_Window* g_window = nullptr;
std::atomic<bool> g_open_pending{false};
std::atomic<bool> g_apply_pending{false};
std::atomic<bool> g_capture_wanted{true};
std::atomic<uint32_t> g_desktop_w{1280}, g_desktop_h{720};
std::atomic<uint32_t> g_window_w{1280}, g_window_h{720};
std::atomic<float> g_refresh{0.0f};
std::mutex g_modes_lock;
std::vector<Mode> g_modes;

// Display changes from the menus (graphics_options.cpp, native_graphics.cpp): queued with their
// own values, applied by the window loop, written to the settings only if SDL accepted them.
struct DisplayRequest {
    DisplayMode mode;
    uint32_t width, height;
};
std::mutex g_display_lock;
uint64_t g_next_request = 0;
std::map<uint64_t, DisplayRequest> g_requests;  // waiting for the window loop
std::map<uint64_t, bool> g_results;             // applied, not yet collected
constexpr size_t kMaxResults = 64;

// Puts the window into a mode; false if SDL refused any step.
bool ApplyWindowMode(DisplayMode mode, uint32_t width, uint32_t height) {
    if (!g_window) return false;
    bool ok = true;
    if (mode == DisplayMode::Borderless) {
        ok &= SDL_SetWindowFullscreen(g_window, false);
        ok &= SDL_SetWindowBordered(g_window, false);
        const Mode desktop = Desktop();
        if (desktop.width && desktop.height)
            ok &= SDL_SetWindowSize(g_window, int(desktop.width), int(desktop.height));
        ok &= SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    } else {
        const bool fullscreen = mode == DisplayMode::Fullscreen;
        if (!fullscreen) ok &= SDL_SetWindowFullscreen(g_window, false);
        ok &= SDL_SetWindowBordered(g_window, true);
        if (width && height) ok &= SDL_SetWindowSize(g_window, int(width), int(height));
        if (fullscreen) {
            SDL_DisplayMode closest{};
            ok &= SDL_GetClosestFullscreenDisplayMode(SDL_GetPrimaryDisplay(), int(width), int(height), 0.0f,
                                                      true, &closest) &&
                  SDL_SetWindowFullscreenMode(g_window, &closest);
            ok &= SDL_SetWindowFullscreen(g_window, true);
        }
    }
    if (!ok) LOGE("app: display change refused by SDL: %s", SDL_GetError());
    return ok;
}

// Window loop: applies each queued request; a refused one puts the saved mode back.
void ApplyDisplayRequests() {
    std::map<uint64_t, DisplayRequest> requests;
    {
        // copied, not moved: each stays queued until its result is stored
        std::lock_guard<std::mutex> guard(g_display_lock);
        requests = g_requests;
    }
    for (const auto& [id, r] : requests) {
        const bool ok = ApplyWindowMode(r.mode, r.width, r.height);
        if (ok) {
            std::lock_guard<std::recursive_mutex> settings(g_settings_lock);
            Settings& s = Get();
            s.display_mode = r.mode;
            s.width = r.width;
            s.height = r.height;
        } else if (g_window) {
            const Settings s = Snapshot();
            ApplyWindowMode(s.display_mode, s.width, s.height);
        }
        std::lock_guard<std::mutex> guard(g_display_lock);
        g_requests.erase(id);
        g_results[id] = ok;
        while (g_results.size() > kMaxResults) g_results.erase(g_results.begin());
    }
}

// newest finished frame from the render thread, shown by UpdateWindow on the window thread
std::mutex g_frame_lock;
std::vector<uint8_t> g_frame;
int g_frame_w = 0, g_frame_h = 0;
bool g_frame_new = false;

} // namespace

std::atomic<bool> g_suspended{false};

// Steam Deck suspend/resume: stop pacing at full speed and release the mouse.
// The guest keeps running slowly; the renderer recreates its swapchain on resume.
void SetSuspended(bool suspended) {
    g_suspended.store(suspended, std::memory_order_relaxed);
    if (suspended) g_capture_wanted.store(false, std::memory_order_relaxed);
}

bool Suspended() { return g_suspended.load(std::memory_order_relaxed); }

void SubmitFrame(const uint8_t* rgba, int width, int height) {
    if (!rgba || width <= 0 || height <= 0) return;
    std::lock_guard<std::mutex> lock(g_frame_lock);
    const size_t bytes = size_t(width) * size_t(height) * 4;
    g_frame.assign(rgba, rgba + bytes);
    g_frame_w = width;
    g_frame_h = height;
    g_frame_new = true;
}

// window thread: the desktop mode and the display's full-screen mode list
void RefreshDesktop() {
    if (const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay())) {
        g_desktop_w.store(uint32_t(mode->w), std::memory_order_relaxed);
        g_desktop_h.store(uint32_t(mode->h), std::memory_order_relaxed);
        g_refresh.store(mode->refresh_rate, std::memory_order_relaxed);
    }
    std::vector<Mode> modes;
    int count = 0;
    if (SDL_DisplayMode** listed = SDL_GetFullscreenDisplayModes(SDL_GetPrimaryDisplay(), &count)) {
        modes.reserve(size_t(count));
        for (int i = 0; i < count; i++)
            if (listed[i]) modes.push_back({uint32_t(listed[i]->w), uint32_t(listed[i]->h)});
        SDL_free(listed);
    }
    std::lock_guard<std::mutex> lock(g_modes_lock);
    g_modes = std::move(modes);
}

void SetWindow(SDL_Window* window) {
    g_window = window;
    if (!window) return;
    int w = 0, h = 0;
    if (SDL_GetWindowSizeInPixels(window, &w, &h) && w > 0 && h > 0) {
        g_window_w.store(uint32_t(w), std::memory_order_relaxed);
        g_window_h.store(uint32_t(h), std::memory_order_relaxed);
    }
    RefreshDesktop();
}

// Window-thread side of the window calls; guest threads only set the flags.
void UpdateWindow() {
    if (g_window) {
        int w = 0, h = 0;
        if (SDL_GetWindowSizeInPixels(g_window, &w, &h) && w > 0 && h > 0) {
            g_window_w.store(uint32_t(w), std::memory_order_relaxed);
            g_window_h.store(uint32_t(h), std::memory_order_relaxed);
        }
    }

    // show the newest frame the render thread finished
    {
        std::vector<uint8_t> frame;
        int fw = 0, fh = 0;
        bool fresh = false;
        {
            std::lock_guard<std::mutex> lock(g_frame_lock);
            if (g_frame_new) {
                frame.swap(g_frame);
                fw = g_frame_w;
                fh = g_frame_h;
                g_frame_new = false;
                fresh = true;
            }
        }
        if (fresh && g_window) {
            SDL_Surface* window_surface = SDL_GetWindowSurface(g_window);
            if (window_surface) {
                SDL_Surface* src = SDL_CreateSurfaceFrom(fw, fh, SDL_PIXELFORMAT_RGBA32, frame.data(), fw * 4);
                if (src) {
                    SDL_BlitSurfaceScaled(src, nullptr, window_surface, nullptr, SDL_SCALEMODE_LINEAR);
                    SDL_DestroySurface(src);
                    SDL_UpdateWindowSurface(g_window);
                }
            }
        }
    }

    if (g_open_pending.exchange(false, std::memory_order_relaxed) && g_window) {
        SDL_ShowWindow(g_window);
        SDL_RaiseWindow(g_window);
    }
    if (g_apply_pending.exchange(false, std::memory_order_relaxed) && g_window) {
        const Settings s = Snapshot();
        ApplyWindowMode(s.display_mode, s.width, s.height);
    }
    ApplyDisplayRequests();
    static bool captured = false;
    const bool want = g_capture_wanted.load(std::memory_order_relaxed);
    if (want != captured && g_window) {
        // only remember a mode the window accepted; a focus gain retries the same request
        if (SDL_SetWindowRelativeMouseMode(g_window, want)) {
            captured = want;
            if (want) SDL_HideCursor();
            else SDL_ShowCursor();
        }
    }
}

Settings& Get() {
    static Settings s;
    return s;
}

Settings Snapshot() {
    std::lock_guard<std::recursive_mutex> guard(g_settings_lock);
    return Get();
}

SettingsLock::SettingsLock() { g_settings_lock.lock(); }
SettingsLock::~SettingsLock() { g_settings_lock.unlock(); }

// These fields are part of Settings, which Snapshot() copies as a whole. Keep every access under
// the settings lock rather than mixing atomic_ref with a non-atomic struct copy.
uint32_t FrameCap() {
    std::lock_guard<std::recursive_mutex> guard(g_settings_lock);
    return Get().fps_cap;
}

void SetFrameCap(uint32_t cap) {
    std::lock_guard<std::recursive_mutex> guard(g_settings_lock);
    Get().fps_cap = cap;
}

uint32_t MsaaSamples() {
    std::lock_guard<std::recursive_mutex> guard(g_settings_lock);
    return Get().msaa;
}

void SetMsaaSamples(uint32_t samples) {
    std::lock_guard<std::recursive_mutex> guard(g_settings_lock);
    Get().msaa = samples;
}

std::filesystem::path ExeDir() {
    static const std::filesystem::path dir = [] {
        std::error_code ec;
        const auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
        return ec ? std::filesystem::current_path() : exe.parent_path();
    }();
    return dir;
}

int Instance() {
    static const int n = [] {
        const char* e = std::getenv("INSTANCE");
        return e ? std::atoi(e) : 0;
    }();
    return n;
}

void* Window() { return g_window; }

bool RequestQuit() {
    SDL_Event event{};
    event.type = SDL_EVENT_QUIT;
    return SDL_PushEvent(&event);
}

Mode Desktop() {
    return {g_desktop_w.load(std::memory_order_relaxed), g_desktop_h.load(std::memory_order_relaxed)};
}

Mode Output() {
    const Settings s = Snapshot();
    if (s.width && s.height) return {s.width, s.height};
    return Desktop();
}

std::vector<Mode> DisplayModes() {
    std::lock_guard<std::mutex> lock(g_modes_lock);
    return g_modes;
}

// the drawable size of the SDL window, in pixels (the Vulkan swapchain's extent)
Mode WindowPixels() {
    return {g_window_w.load(std::memory_order_relaxed), g_window_h.load(std::memory_order_relaxed)};
}

float RenderScale(uint32_t guest_height) {
    // the frame fills the output's height, or its width when the frame is wider than the output
    const Settings s = Snapshot();
    const Mode m = Output();
    const float aspect = AspectRatio();
    float shown = float(m.height);
    if (m.width && float(m.width) < shown * aspect) shown = float(m.width) / aspect;
    const float h = shown * (float(s.render_percent) / 100.0f);
    return guest_height ? std::max(h / float(guest_height), 0.1f) : 1.0f;
}

float AspectRatio() {
    const Settings s = Snapshot();
    if (s.aspect) return float(s.aspect) / 1000.0f;
    const Mode mode = Output();
    return mode.width && mode.height ? float(mode.width) / float(mode.height) : 16.0f / 9.0f;
}

float RenderStretch(uint32_t guest_width, uint32_t guest_height) {
    if (!guest_width || !guest_height) return 1.0f;
    return AspectRatio() * float(guest_height) / float(guest_width);
}

// 3:2 or wider: the console reports a widescreen TV
bool Widescreen() { return AspectRatio() >= 1.5f; }

bool OpenSplash() { return false; }
void SplashFirstFrame() {}
void CloseSplash() {}

void OpenGameWindow(bool) { g_open_pending.store(true, std::memory_order_relaxed); }

void ApplyDisplay() { g_apply_pending.store(true, std::memory_order_relaxed); }

uint64_t RequestDisplayApply(DisplayMode mode, uint32_t width, uint32_t height) {
    std::lock_guard<std::mutex> guard(g_display_lock);
    const uint64_t id = ++g_next_request;
    g_requests[id] = {mode, width, height};
    return id;
}

bool DisplayApplyResult(uint64_t request, bool& ok) {
    if (!request) return false;
    std::lock_guard<std::mutex> guard(g_display_lock);
    auto it = g_results.find(request);
    if (it == g_results.end()) {
        // answered but dropped from the full results list: refused, not still pending
        if (request <= g_next_request && !g_requests.count(request)) {
            ok = false;
            return true;
        }
        return false;
    }
    ok = it->second;
    g_results.erase(it);
    return true;
}

void CaptureMouse(bool capture) { g_capture_wanted.store(capture, std::memory_order_relaxed); }

bool StartHandover() { return false; }
bool HandingOver() { return false; }
void FinishHandover() {}

std::recursive_mutex& UiLock() {
    static std::recursive_mutex m;
    return m;
}

void Pace(uint32_t max_fps) {
    // the presentation interval's rate and the menu's cap both apply; this runs on
    // the guest's swap thread only (the window loop must not pace as well)
    if (g_suspended.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        return;
    }
    uint32_t cap = FrameCap();
    if (cap == kFpsCapRefresh) {
        const float rate = g_refresh.load(std::memory_order_relaxed);
        cap = rate > 1.0f ? uint32_t(rate + 0.5f) : 0;
    }
    if (max_fps && (!cap || cap > max_fps)) cap = max_fps;
    static uint32_t logged = ~0u;
    if (cap != logged) {
        LOGR("linux: frame cap %u", cap);
        logged = cap;
    }
    using clock = std::chrono::steady_clock;
    static clock::time_point next = clock::now();
    const auto now = clock::now();
    if (!cap) {
        next = now;
        return;
    }
    const auto frame = std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(1.0 / cap));
    next += frame;
    if (next > now) std::this_thread::sleep_until(next);
    else next = now;
}

} // namespace app
