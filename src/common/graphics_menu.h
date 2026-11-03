// The in-game Graphics menu (graphics_options.cpp): F10 or Back + Start.
#pragma once

#include <atomic>
#include <cstdint>

#include "core/input.h"

namespace game {

extern bool g_show_fps;        // settings file: show_fps
extern uint32_t g_fov_percent; // settings file: fov_percent, see graphics_choices.h
// the gameplay camera's horizontal field of view before fov_percent, from its last projection (0 before one)
extern std::atomic<float> g_camera_fov_degrees;

void DrawGraphicsMenu(double fps);             // GameInfo::ui
void FilterMenuPad(int port, input::Pad &pad); // after the pad tools, from GameInfo::pad
bool GraphicsMenuMessage(void *hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t *result); // GameInfo::message

} // namespace game
