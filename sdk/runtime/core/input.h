#pragma once
#include <cstdint>

namespace input {

// the console controller's report, analog buttons are 0-255
struct Pad {
    // compare reports by value; padding is not part of the state
    bool operator==(const Pad&) const = default;
    bool connected = false;
    uint16_t buttons = 0;  // dpad up/down/left/right, start, back, left thumb, right thumb
    uint8_t analog[8] = {};  // A B X Y black white left trigger right trigger
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};

enum class PromptStyle { Original, PlayStation, Xbox, KeyboardMouse };
PromptStyle ControllerPromptStyle();
// keyboard keys that stand in for the two stick clicks, shared by input and
// the keyboard-mouse prompt glyphs
inline constexpr char kLeftStickClickKey = 'C';
inline constexpr char kRightStickClickKey = 'V';
inline char LetterKey(uint32_t value, char fallback) {
    return value >= 'A' && value <= 'Z' ? char(value) : fallback;
}
extern uint32_t g_reload_key, g_heavy_attack_key, g_use_key, g_climb_key;
extern uint32_t g_mouse_sensitivity_percent;
// optional separate vertical scale for direct aim; 100 matches the PC raw path
extern uint32_t g_mouse_vertical_sensitivity_percent;
// when set, mouse motion is not folded into the right stick: the gameplay
// camera reads the same pixels as an angle, see src/game/mouse_aim.cpp
extern bool g_direct_mouse_aim;
// pixels of mouse motion since the last call, false when nothing is pending;
// the angle applies to the gameplay camera instead of the 8-bit stick
bool ConsumeMouseAim(float& dx, float& dy);
// heartbeat from the direct-aim camera controller, per frame; while it is
// fresh the mouse does not also feed the right stick
void UseDirectMouseAim();
// a direct sample was applied recently (the PC fix stops its direct path after 5 s idle)
bool DirectAimRecent();
// an overlay (the Graphics menu) owns the pointer: no look input, no direct aim
void SetUiActive(bool active);
bool UiActive();
void RegisterMouse(void* window);
void ReadMouse(void* raw_input);
// SDL hosts: relative motion and wheel notches from the event loop
void ReadMouseMotion(float dx, float dy);
void ReadMouseWheel(float steps);
// before the host's first SDL_Init: joystick backend hints
void SetHints();
// window thread: hot-plug notices from the host's event loop
void GamepadAdded(int32_t id);
void GamepadRemoved(int32_t id);
void Init();  // window thread, before boot; the game code window starts it earlier
bool Connected(int port);
uint16_t HeldOnAnyController();  // face buttons as XINPUT_GAMEPAD_A/B/X/Y masks, GameInfo::pad left out
// the controller on that port, then GameInfo::pad
Pad Poll(int port);
void Rumble(int port, uint16_t left, uint16_t right);

}  // namespace input
