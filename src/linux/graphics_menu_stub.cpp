// Linux stand-in for the Windows/ImGui Graphics menu (src/common/graphics_options.cpp)
// for the headless harness, which builds without ImGui. The native SDL3/Vulkan build
// compiles the real menu instead (see platform/linux/CMakeLists.txt).
#ifdef __linux__

#include <atomic>
#include <cstdint>
#include <vector>

#include "app/app.h"
#include "common/graphics_menu.h"

namespace game {

bool g_show_fps = false;
uint32_t g_fov_percent = 100;
std::atomic<float> g_camera_fov_degrees{0};

bool ShowFps() {
    return std::atomic_ref(g_show_fps).load(std::memory_order_relaxed);
}

void SetShowFps(bool show) {
    std::atomic_ref(g_show_fps).store(show, std::memory_order_relaxed);
}

std::vector<app::Mode> DisplayModes() {
    return {app::Desktop()};
}

void DrawGraphicsMenu(double) {}

void FilterMenuPad(int, input::Pad&) {}

bool GraphicsMenuMessage(void*, uint32_t, uintptr_t, intptr_t, intptr_t*) {
    return false;
}

} // namespace game

#endif
