// controllers through SDL: Xbox, PlayStation and Switch pads read into the console controller's report
#include "core/input.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iterator>
#include <mutex>
#include <utility>

#define NOMINMAX
#include <windows.h>
#include <SDL3/SDL.h>

#include "app/app.h"
#include "core/log.h"

namespace input {

uint32_t g_mouse_sensitivity_percent = 100;
uint32_t g_mouse_vertical_sensitivity_percent = 100;
uint32_t g_reload_key = 'R', g_heavy_attack_key = 'F', g_use_key = 'G', g_climb_key = 'E';
bool g_direct_mouse_aim = true;

namespace {

// a controller takes the lowest free port when it is plugged in and keeps it until it is unplugged
std::mutex g_lock;
bool g_sdl = false;
SDL_Gamepad* g_pads[4] = {};
std::atomic<PromptStyle> g_prompt_style{PromptStyle::Original};
std::atomic<bool> g_keyboard_mouse_active{false};
// until the camera takes a direct sample the mouse keeps the old stick path,
// so an unsupported camera mode never loses aiming
std::atomic<bool> g_direct_confirmed{false};
std::atomic<uint64_t> g_direct_at{0};      // last frame the direct controller ran
std::atomic<uint64_t> g_direct_used_at{0}; // last time the camera took a sample
// the Graphics menu (or another overlay) is using the pointer
std::atomic<bool> g_ui_active{false};

// direct aim is live only while its controller is actually updating; a
// different camera mode falls back to the right stick instead of losing the
// mouse for the rest of the session
bool DirectAimLive(uint64_t now) {
    return g_direct_mouse_aim && g_direct_confirmed.load(std::memory_order_relaxed) &&
           now - g_direct_at.load(std::memory_order_relaxed) < 250;
}

void UpdatePromptType() {
    const SDL_GamepadType type = g_pads[0] ? SDL_GetRealGamepadType(g_pads[0]) : SDL_GAMEPAD_TYPE_UNKNOWN;
    PromptStyle style = PromptStyle::Original;
    if (type == SDL_GAMEPAD_TYPE_PS3 || type == SDL_GAMEPAD_TYPE_PS4 || type == SDL_GAMEPAD_TYPE_PS5)
        style = PromptStyle::PlayStation;
    else if (type == SDL_GAMEPAD_TYPE_XBOX360 || type == SDL_GAMEPAD_TYPE_XBOXONE)
        style = PromptStyle::Xbox;
    g_prompt_style.store(style, std::memory_order_relaxed);
}

// Read relative mouse motion from the window thread and hold the sample for a
// short interval because the guest can poll controller state several times.
std::mutex g_mouse_lock;
int g_mouse_x = 0, g_mouse_y = 0;
uint64_t g_mouse_sample_at = 0;
int16_t g_mouse_rx = 0, g_mouse_ry = 0;
// raw counts waiting for the camera, taken straight from WM_INPUT so a fast
// flick is not limited by the right-stick path's small clamp; the camera clears
// them once per frame
float g_aim_x = 0, g_aim_y = 0;
uint64_t g_aim_at = 0; // last raw mouse event

bool ReadKeyboardMouse(Pad& pad) {
    const auto window = static_cast<HWND>(app::Window());
    static std::atomic<bool> had_focus{true};
    const bool focused = window && GetForegroundWindow() == window;
    if (had_focus.exchange(focused, std::memory_order_relaxed) != focused)
        LOGR("input: game window %s focus", focused ? "regained" : "lost");
    if (!focused) {
        std::lock_guard lock(g_mouse_lock);
        g_mouse_x = g_mouse_y = g_mouse_rx = g_mouse_ry = 0;
        g_aim_x = g_aim_y = 0;
        return false;
    }
    auto held = [](int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; };
    // Keyboard movement is digital, and a diagonal is normalized to the same
    // length as a straight push (full / sqrt(2)), like the PC's keyboard input.
    // A real controller keeps its own analog values in Poll().
    const int move_x = int(held('D')) - int(held('A'));
    const int move_y = int(held('W')) - int(held('S'));
    const int move_scale = move_x && move_y ? 23170 : 32767;
    pad.lx = int16_t(move_x * move_scale);
    pad.ly = int16_t(move_y * move_scale);
    constexpr int digital[] = {VK_UP,     VK_DOWN,   VK_LEFT,          VK_RIGHT,
                               VK_ESCAPE, VK_TAB,    kLeftStickClickKey, kRightStickClickKey};
    const int analog[] = {VK_SPACE, 'Q', LetterKey(g_reload_key, 'R'), LetterKey(g_heavy_attack_key, 'F'),
                          LetterKey(g_use_key, 'G'), LetterKey(g_climb_key, 'E'), VK_RBUTTON, VK_LBUTTON};
    for (int i = 0; i < 8; ++i) {
        if (held(digital[i])) pad.buttons |= uint16_t(1 << i);
        if (held(analog[i])) pad.analog[i] = 255;
    }
    if (held(VK_RETURN)) pad.analog[0] = 255;
    if (held(VK_BACK)) pad.analog[1] = 255;
    bool mouse_active;
    {
        std::lock_guard lock(g_mouse_lock);
        const uint64_t now = GetTickCount64();
        const bool ui = g_ui_active.load(std::memory_order_relaxed);
        const bool direct = DirectAimLive(now);
        static bool was_direct = false;
        if (direct != was_direct && !ui) {
            LOGR("input: mouse aim %s", direct ? "direct" : "right stick (direct camera not updating)");
            was_direct = direct;
        }
        if (now - g_mouse_sample_at >= 16) {
            g_mouse_sample_at = now;
            const int scale = 18 * int(std::clamp(g_mouse_sensitivity_percent, 10u, 400u));
            g_mouse_rx = int16_t(std::clamp(g_mouse_x * scale, -32767, 32767));
            g_mouse_ry = int16_t(std::clamp(-g_mouse_y * scale, -32767, 32767));
            g_mouse_x = g_mouse_y = 0;
        }
        // raw samples belong to the direct camera; another mode falls back to
        // the stick, so discard anything the direct camera did not take
        if (!direct || ui) g_aim_x = g_aim_y = 0;
        mouse_active = g_mouse_rx || g_mouse_ry;
        // the camera applies the pixels itself while direct aim is live
        pad.rx = (direct || ui) ? 0 : g_mouse_rx;
        pad.ry = (direct || ui) ? 0 : g_mouse_ry;
    }
    return pad.lx || pad.ly || mouse_active || pad.buttons ||
           std::any_of(std::begin(pad.analog), std::end(pad.analog), [](uint8_t value) { return value != 0; });
}

void AddPad(SDL_JoystickID id) {
    // XBE_NO_GAMEPADS=1: ignore controllers entirely, for keyboard/mouse only
    // capture and comparison runs.
    static const bool no_gamepads = std::getenv("XBE_NO_GAMEPADS") != nullptr;
    if (no_gamepads) return;
    for (SDL_Gamepad* pad : g_pads)
        if (pad && SDL_GetGamepadID(pad) == id) return;
    SDL_Gamepad** slot = std::find(std::begin(g_pads), std::end(g_pads), nullptr);
    if (slot == std::end(g_pads)) return;
    *slot = SDL_OpenGamepad(id);
    if (slot == g_pads) UpdatePromptType();
    const char* name = *slot ? SDL_GetGamepadName(*slot) : SDL_GetError();
    LOGI("input: controller %d %s: %s", int(slot - g_pads) + 1, *slot ? "plugged in" : "failed to open", name ? name : "");
}

void RemovePad(SDL_JoystickID id) {
    for (int i = 0; i < 4; i++) {
        if (!g_pads[i] || SDL_GetGamepadID(g_pads[i]) != id) continue;
        SDL_CloseGamepad(g_pads[i]);
        g_pads[i] = nullptr;
        if (i == 0) UpdatePromptType();
        LOGI("input: controller %d unplugged", i + 1);
    }
}

// once a millisecond at most, however many ports and threads read
void UpdatePads() {
    static uint64_t last = 0;
    uint64_t now = SDL_GetTicks();
    if (!g_sdl || now == last) return;
    last = now;
    SDL_PumpEvents();
    SDL_Event e;
    while (SDL_PeepEvents(&e, 1, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) == 1) {
        if (e.type == SDL_EVENT_GAMEPAD_ADDED) AddPad(e.gdevice.which);
        else if (e.type == SDL_EVENT_GAMEPAD_REMOVED) RemovePad(e.gdevice.which);
    }
}

}  // namespace

PromptStyle ControllerPromptStyle() {
    if (g_keyboard_mouse_active.load(std::memory_order_relaxed)) return PromptStyle::KeyboardMouse;
    return g_prompt_style.load(std::memory_order_relaxed);
}

void RegisterMouse(void* window) {
    const RAWINPUTDEVICE mouse{0x01, 0x02, 0, static_cast<HWND>(window)};
    if (!RegisterRawInputDevices(&mouse, 1, sizeof(mouse))) LOGE("input: could not register raw mouse");
}

void ReadMouse(void* raw_input) {
    RAWINPUT input{};
    UINT size = sizeof(input);
    if (GetRawInputData(static_cast<HRAWINPUT>(raw_input), RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER)) !=
            sizeof(input) ||
        input.header.dwType != RIM_TYPEMOUSE || (input.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE))
        return;
    // the menu moves the pointer, not the camera: do not queue that motion
    if (g_ui_active.load(std::memory_order_relaxed)) return;
    std::lock_guard lock(g_mouse_lock);
    const LONG dx = input.data.mouse.lLastX, dy = input.data.mouse.lLastY;
    // the stick path keeps its small clamp
    g_mouse_x = std::clamp(g_mouse_x + dx, -100L, 100L);
    g_mouse_y = std::clamp(g_mouse_y + dy, -100L, 100L);
    // direct aim takes the full raw delta, like the PC fix's raw input path
    g_aim_x = std::clamp(g_aim_x + float(dx), -10000.0f, 10000.0f);
    g_aim_y = std::clamp(g_aim_y + float(dy), -10000.0f, 10000.0f);
    g_aim_at = GetTickCount64();
}

void UseDirectMouseAim() {
    g_direct_at.store(GetTickCount64(), std::memory_order_relaxed);
    g_direct_confirmed.store(true, std::memory_order_relaxed);
}

void SetUiActive(bool active) {
    g_ui_active.store(active, std::memory_order_relaxed);
    if (!active) return;
    std::lock_guard lock(g_mouse_lock);
    g_mouse_x = g_mouse_y = g_mouse_rx = g_mouse_ry = 0;
    g_aim_x = g_aim_y = 0; // the menu moved the pointer, not the camera
}

bool ConsumeMouseAim(float& dx, float& dy) {
    std::lock_guard lock(g_mouse_lock);
    if (g_ui_active.load(std::memory_order_relaxed) || !DirectAimLive(GetTickCount64())) {
        g_aim_x = g_aim_y = 0;
        return false;
    }
    // a sample the camera never looked at is stale: drop it rather than turn
    // the view by a jump when gameplay resumes
    if (GetTickCount64() - g_aim_at > 200) {
        g_aim_x = g_aim_y = 0;
        return false;
    }
    if (g_aim_x == 0 && g_aim_y == 0) return false;
    dx = g_aim_x;
    dy = g_aim_y;
    g_aim_x = g_aim_y = 0;
    g_direct_used_at.store(GetTickCount64(), std::memory_order_relaxed);
    return true;
}

bool DirectAimRecent() {
    const uint64_t used = g_direct_used_at.load(std::memory_order_relaxed);
    return used && GetTickCount64() - used < 5000;
}

void Init() {
    if (g_sdl) return;  // started already for the game code window
    // the old right-stick path stays available as a fallback
    if (std::getenv("TCNYC_MOUSE_STICK")) g_direct_mouse_aim = false;
    // WGI reads the triggers apart and HIDAPI takes PlayStation and Switch pads. Reads come from the guest's threads,
    // so SDL's own thread takes the device notices.
    SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
    SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_DIRECTINPUT, "0");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        LOGE("input: SDL failed to start, no controllers: %s", SDL_GetError());
        return;
    }
    int v = SDL_GetVersion();
    LOGI("input: SDL %d.%d.%d", SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v));
    std::lock_guard lock(g_lock);
    g_sdl = true;
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
        for (int i = 0; i < count; i++) AddPad(ids[i]);
        SDL_free(ids);
    }
}

Pad Poll(int port) {
    Pad p;
    {
        std::lock_guard lock(g_lock);
        UpdatePads();
        if (SDL_Gamepad* pad = port >= 0 && port < 4 ? g_pads[port] : nullptr) {
            p.connected = true;
            constexpr SDL_GamepadButton kButtons[] = {
                SDL_GAMEPAD_BUTTON_DPAD_UP,   SDL_GAMEPAD_BUTTON_DPAD_DOWN,  SDL_GAMEPAD_BUTTON_DPAD_LEFT,
                SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_START,     SDL_GAMEPAD_BUTTON_BACK,
                SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK};
            for (int i = 0; i < 8; i++) p.buttons |= uint16_t(SDL_GetGamepadButton(pad, kButtons[i]) << i);
            // A B X Y black white
            constexpr SDL_GamepadButton kAnalog[] = {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST,
                                                     SDL_GAMEPAD_BUTTON_WEST,  SDL_GAMEPAD_BUTTON_NORTH,
                                                     SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER};
            for (int i = 0; i < 6; i++) p.analog[i] = SDL_GetGamepadButton(pad, kAnalog[i]) ? 0xFF : 0;
            auto axis = [&](SDL_GamepadAxis a) { return int(SDL_GetGamepadAxis(pad, a)); };
            // 0 to 32767 into 0 to 255
            p.analog[6] = uint8_t(std::max(axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER), 0) >> 7);
            p.analog[7] = uint8_t(std::max(axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER), 0) >> 7);
            // SDL's sticks have y down
            // not plain negation: -32768 has no positive
            p.lx = int16_t(axis(SDL_GAMEPAD_AXIS_LEFTX));
            p.ly = int16_t(-1 - axis(SDL_GAMEPAD_AXIS_LEFTY));
            p.rx = int16_t(axis(SDL_GAMEPAD_AXIS_RIGHTX));
            p.ry = int16_t(-1 - axis(SDL_GAMEPAD_AXIS_RIGHTY));
        }
    }
    if (port == 0) {
        Pad keyboard_mouse;
        const bool active = ReadKeyboardMouse(keyboard_mouse);
        const bool controller_active = p.buttons || std::abs(p.lx) > 8000 || std::abs(p.ly) > 8000 ||
                                       std::abs(p.rx) > 8000 || std::abs(p.ry) > 8000 ||
                                       std::any_of(std::begin(p.analog), std::end(p.analog),
                                                   [](uint8_t value) { return value > 32; });
        if (active || !p.connected) g_keyboard_mouse_active.store(true, std::memory_order_relaxed);
        else if (controller_active) g_keyboard_mouse_active.store(false, std::memory_order_relaxed);
        if (active || !p.connected) {
            keyboard_mouse.connected = true;
            p = keyboard_mouse;
        }
    }
    if (app::kGame.pad) app::kGame.pad(port, p);
    return p;
}

uint16_t HeldOnAnyController() {
    constexpr std::pair<SDL_GamepadButton, uint16_t> kFaceButtons[] = {
        {SDL_GAMEPAD_BUTTON_SOUTH, 0x1000}, {SDL_GAMEPAD_BUTTON_EAST, 0x2000},
        {SDL_GAMEPAD_BUTTON_WEST, 0x4000},  {SDL_GAMEPAD_BUTTON_NORTH, 0x8000}};
    std::lock_guard lock(g_lock);
    UpdatePads();
    uint16_t held = 0;
    for (SDL_Gamepad* pad : g_pads)
        for (auto [button, mask] : kFaceButtons)
            if (pad && SDL_GetGamepadButton(pad, button)) held |= mask;
    return held;
}

// the motors run until set again
void Rumble(int port, uint16_t left, uint16_t right) {
    if (app::kGame.rumble) app::kGame.rumble(port, left, right);
    std::lock_guard lock(g_lock);
    if (port >= 0 && port < 4 && g_pads[port]) SDL_RumbleGamepad(g_pads[port], left, right, 0);
}

}  // namespace input
