// Native Linux entry point: SDL3 window and event loop around the recompiled game.
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <backends/imgui_impl_sdl3.h>
#include <imgui.h>

#include "app/app.h"
#include "core/input.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "image_symbols.h"
#include "kernel/kernel.h"

namespace app {
void SetWindow(SDL_Window* window);
void UpdateWindow();
void RefreshDesktop();
void SetSuspended(bool suspended);
}
namespace audio {
void SetSuspended(bool suspended);
void DeviceChanged();
}
namespace input {
void Init();
}

namespace {
volatile std::sig_atomic_t g_quit = 0;
void OnSignal(int) { g_quit = 1; }
} // namespace

int main(int argc, char** argv) {
    std::string location;  // the game's folder or its disc image; empty: assets beside the executable
    for (int i = 1; i < argc; ++i) {
        if ((!std::strcmp(argv[i], "--assets") || !std::strcmp(argv[i], "--iso")) && i + 1 < argc) location = argv[++i];
        else if (!std::strcmp(argv[i], "--instance") && i + 1 < argc) setenv("INSTANCE", argv[++i], 1);
        else if (argv[i][0] != '-') location = argv[i];
    }

    // the settings decide the window, so the log and settings load come first
    std::string log_name = app::kGame.log;
    if (app::Instance() > 1) log_name += "-" + std::to_string(app::Instance());
    logx::Init((app::ExeDir() / "logs").string(), log_name);
    app::Load();
    if (app::kGame.init) app::kGame.init();
    app::LogSystem();
    app::LogSettings();

    input::SetHints();
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD)) {
        LOGE("linux: SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    app::RefreshDesktop();
    LOGR("linux: SDL video driver %s", SDL_GetCurrentVideoDriver());
    // timeout(1) and the desktop's close button both end the run cleanly
    std::signal(SIGTERM, OnSignal);
    std::signal(SIGINT, OnSignal);

    app::GameFiles game;
    std::string game_error;
    if (!app::FindGame(game, &game_error, location)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "True Crime NYC", game_error.c_str(), nullptr);
        SDL_Quit();
        return 1;
    }

    const app::Settings settings = app::Snapshot();
    const app::Mode desktop = app::Desktop();
    int width = int(settings.width ? settings.width : desktop.width);
    int height = int(settings.height ? settings.height : desktop.height);
    if (app::Instance() > 1) {
        width = 960;
        height = 540;
    }
    SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY;
    const char* renderer = std::getenv("TCNYC_RENDERER");
    if (!renderer || std::strcmp(renderer, "software") != 0) flags |= SDL_WINDOW_VULKAN;
    switch (settings.display_mode) {
        case app::DisplayMode::Fullscreen: flags |= SDL_WINDOW_FULLSCREEN; break;
        case app::DisplayMode::Borderless: flags |= SDL_WINDOW_BORDERLESS; break;
        case app::DisplayMode::Windowed: flags |= SDL_WINDOW_RESIZABLE; break;
    }

    std::string title;
    for (const wchar_t* p = app::kGame.short_title; p && *p; ++p)
        title += *p < 128 ? char(*p) : '?';
    if (app::Instance()) title += " [" + std::to_string(app::Instance()) + "]";
    SDL_Window* window = SDL_CreateWindow(title.c_str(), width, height, flags);
    if (!window) {
        LOGE("linux: SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    app::SetWindow(window);
    // apply the configured fullscreen/borderless mode before the first frame
    app::ApplyDisplay();
    app::UpdateWindow();
    SDL_ShowWindow(window);
    SDL_RaiseWindow(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::StyleColorsDark();
    if (!ImGui_ImplSDL3_InitForVulkan(window)) {
        LOGE("linux: ImGui SDL3 backend failed to start");
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    if (app::kGame.window) app::kGame.window(window);
    input::Init();
    input::RegisterMouse(window);

    const std::string hdd = app::Instance() > 1 ? "hdd" + std::to_string(app::Instance()) : "hdd";
    if (!kernel::Boot(std::move(game.xbe), IMAGE_XBE, game.dir, app::ExeDir() / hdd)) {
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    LOGR("linux: booted, %s", game.dir.string().c_str());

    bool running = true;
    while (running && !g_quit) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            {
                std::lock_guard<std::recursive_mutex> lock(app::UiLock());
                ImGui_ImplSDL3_ProcessEvent(&event);
            }
            switch (event.type) {
                case SDL_EVENT_QUIT:
                    running = false;
                    break;
                case SDL_EVENT_MOUSE_MOTION:
                    input::ReadMouseMotion(event.motion.xrel, event.motion.yrel);
                    break;
                case SDL_EVENT_MOUSE_WHEEL:
                    input::ReadMouseWheel(event.wheel.y);
                    break;
                case SDL_EVENT_GAMEPAD_ADDED:
                    input::GamepadAdded(int32_t(event.gdevice.which));
                    break;
                case SDL_EVENT_GAMEPAD_REMOVED:
                    input::GamepadRemoved(int32_t(event.gdevice.which));
                    break;
                case SDL_EVENT_KEY_DOWN:
                    if (event.key.scancode == SDL_SCANCODE_F3) {
                        gpu::g_capture.store(true);
                        LOGR("linux: frame trace requested");
                    }
                    break;
                case SDL_EVENT_WINDOW_FOCUS_GAINED:
                    LOGR("linux: window focus gained");
                    if (!input::UiActive()) app::CaptureMouse(true);
                    break;
                case SDL_EVENT_WINDOW_FOCUS_LOST:
                    LOGR("linux: window focus lost");
                    app::CaptureMouse(false);
                    break;
                case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
                    app::RefreshDesktop();
                    break;
                case SDL_EVENT_WILL_ENTER_BACKGROUND:
                case SDL_EVENT_DID_ENTER_BACKGROUND:
                    app::SetSuspended(true);
                    audio::SetSuspended(true);
                    LOGR("linux: entering background, game paused");
                    break;
                case SDL_EVENT_WILL_ENTER_FOREGROUND:
                case SDL_EVENT_DID_ENTER_FOREGROUND:
                    app::SetSuspended(false);
                    audio::SetSuspended(false);
                    app::RefreshDesktop();
                    LOGR("linux: returning to foreground, game resumed");
                    break;
                case SDL_EVENT_AUDIO_DEVICE_REMOVED:
                    audio::DeviceChanged();
                    break;
                default:
                    break;
            }
        }
        app::UpdateWindow();
        SDL_Delay(2);  // the guest's swap paces the game; this loop only pumps events and blits software frames
    }

    kernel::Shutdown(0);
}
