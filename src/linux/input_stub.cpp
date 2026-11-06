// Linux/headless stand-ins for the mouse-aim half of core/input.cpp.
// The native SDL3 host supplies the real keyboard/mouse state; the harness
// only needs the symbols the game's camera hooks reference.
#ifdef __linux__

#include <cstdint>

#include "core/input.h"

namespace input {

uint32_t g_mouse_sensitivity_percent = 100;
uint32_t g_mouse_vertical_sensitivity_percent = 100;
uint32_t g_reload_key = 'R', g_heavy_attack_key = 0, g_use_key = 'E', g_climb_key = 'G';
bool g_direct_mouse_aim = true;

void UseDirectMouseAim() {}

bool ConsumeMouseAim(float&, float&) {
    return false;
}

bool DirectAimRecent() {
    return false;
}

} // namespace input

#endif
