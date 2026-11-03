// The in-game Graphics menu (graphics_options.cpp): F10 or Back + Start.
#pragma once

#include <atomic>
#include <cstdint>

#include <vector>

#include "app/app.h"
#include "core/input.h"

namespace game {

extern bool g_show_fps; // settings file: show_fps
bool ShowFps();
void SetShowFps(bool show);
extern uint32_t g_fov_percent; // settings file: fov_percent, see graphics_choices.h
// the gameplay camera's horizontal field of view before fov_percent, from its last projection (0 before one)
extern std::atomic<float> g_camera_fov_degrees;

// Desktop (0 x 0), then the sizes the game window's monitor lists from 640 x 480, smallest first
std::vector<app::Mode> DisplayModes();

void DrawGraphicsMenu(double fps);             // GameInfo::ui
void FilterMenuPad(int port, input::Pad &pad); // after the pad tools, from GameInfo::pad
bool GraphicsMenuMessage(void *hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t *result); // GameInfo::message

} // namespace game
