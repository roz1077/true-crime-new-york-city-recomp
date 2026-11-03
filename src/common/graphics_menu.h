// The in-game Graphics menu (graphics_options.cpp): F10 or Back + Start.
#pragma once

#include <cstdint>

#include "core/input.h"

namespace game {

extern bool g_show_fps; // settings file: show_fps

void DrawGraphicsMenu(double fps);         // GameInfo::ui
void FilterMenuPad(int port, input::Pad &pad); // after the pad tools, from GameInfo::pad
bool GraphicsMenuMessage(void *hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t *result); // GameInfo::message

} // namespace game
