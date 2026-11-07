// controllers through SDL: Xbox, PlayStation and Switch pads read into the console controller's report
#include "core/input.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <mutex>
#include <utility>

#define NOMINMAX
#ifdef _WIN32
#include <windows.h>
#endif
#include <SDL3/SDL.h>

#include "app/app.h"
#include "core/log.h"

namespace input {

uint32_t g_mouse_sensitivity_percent = 100;
uint32_t g_mouse_vertical_sensitivity_percent = 100;
// PC defaults: R reload, E use/action, G climb (the port's own action), and no
// configurable heavy key because Left Alt and mouse side button 2 are the defaults.
uint32_t g_reload_key = 'R', g_heavy_attack_key = 0, g_use_key = 'E', g_climb_key = 'G';
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
    // a short window: the camera calls this every frame, so the pad fallback
    // should take over quickly when a transition camera stops updating
    return g_direct_mouse_aim && g_direct_confirmed.load(std::memory_order_relaxed) &&
           now - g_direct_at.load(std::memory_order_relaxed) < 100;
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
// last fallback stick value, held briefly so a mouse flick is not a one-frame
// pulse to the transition cameras
int16_t g_hold_rx = 0, g_hold_ry = 0;
uint64_t g_hold_at = 0;
// mouse wheel notches waiting to be turned into D-pad taps (weapon select / camera)
int g_wheel_steps = 0;
// raw counts waiting for the camera, taken straight from WM_INPUT so a fast
// flick is not limited by the right-stick path's small clamp; the camera clears
// them once per frame
float g_aim_x = 0, g_aim_y = 0;
uint64_t g_aim_at = 0; // last raw mouse event
// movement made while no camera is updating (zone load, car exit). It is
// released over the first camera frames after the transition instead of being
// dropped, matching a pad that is still deflected when control returns.
float g_pending_aim_x = 0, g_pending_aim_y = 0;
constexpr float kPendingAimLimit = 150.0f;
constexpr float kPendingAimStep = 20.0f;

// A camera can stop just after its heartbeat. Counts already queued during
// that 100 ms grace period belong to the transition too, not the discard path.
// Caller holds g_mouse_lock.
void DeferMouseAim() {
    g_pending_aim_x = std::clamp(g_pending_aim_x + g_aim_x, -kPendingAimLimit, kPendingAimLimit);
    g_pending_aim_y = std::clamp(g_pending_aim_y + g_aim_y, -kPendingAimLimit, kPendingAimLimit);
    g_aim_x = g_aim_y = 0;
}

// the clock the camera and the mouse sample use
uint64_t NowMs() {
#ifdef _WIN32
    return GetTickCount64();
#else
    return SDL_GetTicks();
#endif
}

// keyboard and mouse state: XInput-style virtual keys on Windows, SDL scancodes
// elsewhere; both feed the same PC mapping below
#ifdef _WIN32
bool HeldCode(int code) { return (GetAsyncKeyState(code) & 0x8000) != 0; }
constexpr int kKeyUp = VK_UP, kKeyDown = VK_DOWN, kKeyLeft = VK_LEFT, kKeyRight = VK_RIGHT;
constexpr int kKeyEscape = VK_ESCAPE, kKeyTab = VK_TAB, kKeyReturn = VK_RETURN, kKeyBack = VK_BACK;
constexpr int kKeySpace = VK_SPACE, kKeyShift = VK_SHIFT, kKeyRShift = VK_RSHIFT, kKeyLCtrl = VK_LCONTROL,
              kKeyRCtrl = VK_RCONTROL, kKeyLAlt = VK_LMENU, kKeyRAlt = VK_RMENU;
bool HeldChar(char c) { return HeldCode(int(uint8_t(c))); }
#else
bool HeldCode(int code) {
    int count = 0;
    const bool* keys = SDL_GetKeyboardState(&count);
    return keys && code >= 0 && code < count && keys[code];
}
constexpr int kKeyUp = SDL_SCANCODE_UP, kKeyDown = SDL_SCANCODE_DOWN, kKeyLeft = SDL_SCANCODE_LEFT,
              kKeyRight = SDL_SCANCODE_RIGHT;
constexpr int kKeyEscape = SDL_SCANCODE_ESCAPE, kKeyTab = SDL_SCANCODE_TAB, kKeyReturn = SDL_SCANCODE_RETURN,
              kKeyBack = SDL_SCANCODE_BACKSPACE, kKeySpace = SDL_SCANCODE_SPACE, kKeyShift = SDL_SCANCODE_LSHIFT,
              kKeyRShift = SDL_SCANCODE_RSHIFT, kKeyLCtrl = SDL_SCANCODE_LCTRL, kKeyRCtrl = SDL_SCANCODE_RCTRL,
              kKeyLAlt = SDL_SCANCODE_LALT, kKeyRAlt = SDL_SCANCODE_RALT;
bool HeldChar(char c) { return HeldCode(SDL_GetScancodeFromKey(SDL_Keycode(uint8_t(c) | 0x20), nullptr)); }
#endif

// 0 left, 1 middle, 2 right, 3 the first side button, 4 the second
bool MouseHeld(int button) {
#ifdef _WIN32
    constexpr int keys[] = {VK_LBUTTON, VK_MBUTTON, VK_RBUTTON, VK_XBUTTON1, VK_XBUTTON2};
    return HeldCode(keys[button]);
#else
    const SDL_MouseButtonFlags state = SDL_GetMouseState(nullptr, nullptr);
    constexpr SDL_MouseButtonFlags masks[] = {SDL_BUTTON_LMASK, SDL_BUTTON_MMASK, SDL_BUTTON_RMASK,
                                              SDL_BUTTON_X1MASK, SDL_BUTTON_X2MASK};
    return (state & masks[button]) != 0;
#endif
}

bool WindowFocused() {
#ifdef _WIN32
    const auto window = static_cast<HWND>(app::Window());
    return window && GetForegroundWindow() == window;
#else
    const auto window = static_cast<SDL_Window*>(app::Window());
    return window && (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
#endif
}

bool ReadKeyboardMouse(Pad& pad) {
    static std::atomic<bool> had_focus{true};
    const bool focused = WindowFocused();
    if (had_focus.exchange(focused, std::memory_order_relaxed) != focused)
        LOGR("input: game window %s focus", focused ? "regained" : "lost");
    if (!focused) {
        std::lock_guard lock(g_mouse_lock);
        g_mouse_x = g_mouse_y = g_mouse_rx = g_mouse_ry = 0;
        g_hold_rx = g_hold_ry = 0;
        g_aim_x = g_aim_y = 0;
        g_pending_aim_x = g_pending_aim_y = 0;
        g_aim_at = 0;
        g_direct_used_at.store(0, std::memory_order_relaxed);
        return false;
    }
    auto held = [](int key) { return HeldCode(key); };
    // When an ImGui overlay owns the keyboard, the arrows and Enter/Escape
    // already reach it as keys. Do not also mirror them into the pad: the menu
    // reads both paths and would step twice. Controller input is unaffected.
    const bool ui = g_ui_active.load(std::memory_order_relaxed);
    // Keyboard movement is digital, and a diagonal is normalized to the same
    // length as a straight push (full / sqrt(2)), like the PC's keyboard input.
    // A real controller keeps its own analog values in Poll().
    const int move_x = int(HeldChar('D')) - int(HeldChar('A'));
    const int move_y = int(HeldChar('W')) - int(HeldChar('S'));
    const int move_scale = move_x && move_y ? 23170 : 32767;
    pad.lx = int16_t(move_x * move_scale);
    pad.ly = int16_t(move_y * move_scale);
    if (!ui) {
        const int digital[] = {kKeyUp, kKeyDown, kKeyLeft, kKeyRight, kKeyEscape, kKeyTab, 0, 0};
        const bool stick_click = HeldChar(kLeftStickClickKey);
        for (int i = 0; i < 8; ++i) {
            if (i == 6 ? stick_click : (digital[i] && held(digital[i]))) pad.buttons |= uint16_t(1 << i);
        }
    }
    // Preserve the user's preferred aim/lock layout.
    if (held(kKeyShift) || held(kKeyRShift)) pad.buttons |= uint16_t(1 << 7);
    if (HeldChar('Q')) pad.buttons |= uint16_t(1 << 5); // Back: badge / horn / siren

    auto press = [&](int button, bool on) {
        if (on) pad.analog[button] = 255;
    };
    const bool driving = app::kGame.keyboard_driving && app::kGame.keyboard_driving();
    if (!ui) {
        press(0, held(kKeyReturn)); // A: menu confirmation
        press(1, held(kKeyBack));   // B: menu cancel
    }
    if (driving) {
        press(0, HeldChar('W')); // A: accelerate
        press(2, HeldChar('S')); // X: brake / reverse
        press(1, held(kKeySpace)); // B: handbrake
        pad.ly = 0; // steering uses the horizontal stick axis
    } else {
        press(0, held(kKeyLCtrl) || held(kKeyRCtrl) || MouseHeld(3)); // A: light attack
        press(2, held(kKeyLAlt) || held(kKeyRAlt) || MouseHeld(4)); // X: heavy attack
        if (char key = LetterKey(g_heavy_attack_key, 0)) press(2, HeldChar(key));
        press(1, HeldChar('V')); // B: grapple / human shield
        press(3, held(kKeySpace)); // Y: jump
        if (char key = LetterKey(g_climb_key, 'G')) press(3, HeldChar(key)); // Y: jump / climb
    }
    if (char key = LetterKey(g_reload_key, 'R')) press(4, HeldChar(key)); // Black: reload / block
    if (char key = LetterKey(g_use_key, 'E')) press(5, HeldChar(key)); // White: use / enter vehicle
    press(6, MouseHeld(1) || MouseHeld(2));
    press(7, MouseHeld(0));
    bool mouse_active;
    {
        std::lock_guard lock(g_mouse_lock);
        const uint64_t now = NowMs();
        const bool ui = g_ui_active.load(std::memory_order_relaxed);
        const bool direct = DirectAimLive(now);
        static bool was_direct = false;
        if (direct != was_direct && !ui) {
            LOGR("input: mouse aim %s", direct ? "direct" : "right stick (direct camera not updating)");
            was_direct = direct;
        }
        if (now - g_mouse_sample_at >= 16) {
            g_mouse_sample_at = now;
            // The guest's look controllers cube the stick, so a linear mouse
            // sample (a few counts per frame) lands near zero and the camera
            // appears dead until the mouse is thrown a long way. Shape the
            // sample with the inverse curve and a small floor so a normal move
            // produces the same stick deflection a pad does, including while a
            // cutscene or a zone transition owns the camera.
            const float gain = float(std::clamp(g_mouse_sensitivity_percent, 10u, 400u)) * 0.01f;
            auto shape = [](int counts, float gain) {
                // ignore one-count sensor jitter, then keep every real move
                // above the guest's stick dead zone
                if (counts > -2 && counts < 2) return int16_t(0);
                const float t = std::min(std::fabs(float(counts)) / 100.0f, 1.0f);
                const float s = std::min(std::max(std::cbrt(t), 0.45f), 1.0f) * gain;
                return int16_t(std::clamp((counts < 0 ? -s : s) * 32767.0f, -32767.0f, 32767.0f));
            };
            g_mouse_rx = shape(g_mouse_x, gain);
            g_mouse_ry = shape(-g_mouse_y, gain * float(std::clamp(g_mouse_vertical_sensitivity_percent, 10u, 400u)) * 0.01f);
            g_mouse_x = g_mouse_y = 0;
        }
        if (ui) g_aim_x = g_aim_y = 0;
        else if (!direct && g_direct_mouse_aim) DeferMouseAim();
        // A pad holds its deflection for as long as the stick is pushed, while
        // a mouse flick only produces one 16 ms sample. Hold the fallback stick
        // briefly so a quick move gives the transition camera the same
        // sustained input a controller would.
        if (g_mouse_rx || g_mouse_ry) {
            g_hold_rx = g_mouse_rx;
            g_hold_ry = g_mouse_ry;
            g_hold_at = now;
        } else if (direct || ui || now - g_hold_at > 150) {
            g_hold_rx = g_hold_ry = 0;
        }
        // Even a one-count direct sample must select mouse input. The stick
        // fallback's jitter threshold must not keep a connected pad in charge.
        mouse_active = g_mouse_rx || g_mouse_ry || g_hold_rx || g_hold_ry || g_aim_x || g_aim_y ||
                       (g_direct_mouse_aim && g_direct_confirmed.load(std::memory_order_relaxed) &&
                        g_aim_at && now - g_aim_at < 150);
        // the camera applies the pixels itself while direct aim is live
        pad.rx = (direct || ui) ? 0 : g_hold_rx;
        pad.ry = (direct || ui) ? 0 : g_hold_ry;
        // The PC wheel changes weapon on foot and camera in a vehicle; the pad
        // reaches both through the D-pad.
        if (g_wheel_steps > 0) {
            pad.buttons |= uint16_t(1 << 0);
            g_wheel_steps--;
        } else if (g_wheel_steps < 0) {
            pad.buttons |= uint16_t(1 << 1);
            g_wheel_steps++;
        }
    }
    const bool active = pad.lx || pad.ly || mouse_active || pad.buttons ||
                        std::any_of(std::begin(pad.analog), std::end(pad.analog), [](uint8_t value) { return value != 0; });
    return active;
}

void AddPad(SDL_JoystickID id) {
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

// once a millisecond at most, however many ports and threads read.
// Linux owns the SDL event queue on the window thread (GamepadAdded/Removed), so only the
// Windows host pumps it here; SDL_PumpEvents must not run on guest threads.
void UpdatePads() {
#ifdef _WIN32
    static uint64_t last = 0;
    uint64_t now = SDL_GetTicks();
    if (!g_sdl || now == last) return;
    last = now;
    SDL_PumpEvents();
    SDL_Event e;
    while (SDL_PeepEvents(&e, 1, SDL_GETEVENT, SDL_EVENT_GAMEPAD_ADDED, SDL_EVENT_GAMEPAD_REMOVED) == 1) {
        if (e.type == SDL_EVENT_GAMEPAD_ADDED) AddPad(e.gdevice.which);
        else if (e.type == SDL_EVENT_GAMEPAD_REMOVED) RemovePad(e.gdevice.which);
    }
#else
    (void)0;
#endif
}

}  // namespace

// Window thread, before the first SDL_Init: these hints shape the joystick subsystem.
void SetHints() {
    static std::once_flag once;
    std::call_once(once, [] {
        SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "1");
        SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
        SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "0");
        SDL_SetHint(SDL_HINT_JOYSTICK_DIRECTINPUT, "0");
    });
}

void GamepadAdded(int32_t id) {
    std::lock_guard lock(g_lock);
    AddPad(SDL_JoystickID(id));
}

void GamepadRemoved(int32_t id) {
    std::lock_guard lock(g_lock);
    RemovePad(SDL_JoystickID(id));
}

bool Connected(int port) {
    std::lock_guard lock(g_lock);
    UpdatePads();
#ifndef _WIN32
    // The title only polls a controller port that it has seen connect, and keyboard
    // and scripted input are merged into port 0 by Poll. Without a real pad the
    // port therefore stayed absent and every input path was ignored; report port 0
    // present so a keyboard-only Linux host (or the Deck's Desktop Mode) works.
    if (port == 0) return true;
#endif
    return port >= 0 && port < 4 && g_pads[port];
}

PromptStyle ControllerPromptStyle() {
    if (g_keyboard_mouse_active.load(std::memory_order_relaxed)) return PromptStyle::KeyboardMouse;
    return g_prompt_style.load(std::memory_order_relaxed);
}

// relative motion and wheel notches from the window thread; the Windows path
// takes them from WM_INPUT, the SDL path from MOUSE_MOTION / MOUSE_WHEEL events
void ApplyMouseDelta(int dx, int dy, int wheel) {
    // the menu moves the pointer, not the camera: do not queue that motion
    if (g_ui_active.load(std::memory_order_relaxed)) return;
    std::lock_guard lock(g_mouse_lock);
    if (wheel) g_wheel_steps = std::clamp(g_wheel_steps + wheel, -8, 8);
    // the stick path keeps its small clamp
    g_mouse_x = std::clamp(g_mouse_x + dx, -100, 100);
    g_mouse_y = std::clamp(g_mouse_y + dy, -100, 100);
    const uint64_t now = NowMs();
    g_aim_at = now;
    if (DirectAimLive(now)) {
        // direct aim takes the full raw delta, like the PC fix's raw input path
        g_aim_x = std::clamp(g_aim_x + float(dx), -10000.0f, 10000.0f);
        g_aim_y = std::clamp(g_aim_y + float(dy), -10000.0f, 10000.0f);
    } else if (g_direct_mouse_aim && g_direct_confirmed.load(std::memory_order_relaxed)) {
        // no camera is updating: keep a bounded amount so the view catches up
        // when the transition ends instead of dropping the movement
        g_aim_x += float(dx);
        g_aim_y += float(dy);
        DeferMouseAim();
    }
}

#ifdef _WIN32
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
    int wheel = 0;
    if (input.data.mouse.usButtonFlags & RI_MOUSE_WHEEL)
        wheel = int16_t(input.data.mouse.usButtonData) > 0 ? 1 : -1;
    ApplyMouseDelta(int(input.data.mouse.lLastX), int(input.data.mouse.lLastY), wheel);
}
#else
void RegisterMouse(void*) {}
void ReadMouse(void*) {}

void ReadMouseMotion(float dx, float dy) {
    ApplyMouseDelta(int(std::lround(dx)), int(std::lround(dy)), 0);
}

void ReadMouseWheel(float steps) {
    const int n = int(std::lround(steps));
    if (n) ApplyMouseDelta(0, 0, n > 0 ? 1 : -1);
}
#endif

void UseDirectMouseAim() {
    g_direct_at.store(NowMs(), std::memory_order_relaxed);
    g_direct_confirmed.store(true, std::memory_order_relaxed);
}

void SetUiActive(bool active) {
    g_ui_active.store(active, std::memory_order_relaxed);
    if (!active) return;
    std::lock_guard lock(g_mouse_lock);
    g_mouse_x = g_mouse_y = g_mouse_rx = g_mouse_ry = 0;
    g_hold_rx = g_hold_ry = 0;

    g_aim_x = g_aim_y = 0; // the menu moved the pointer, not the camera
    g_pending_aim_x = g_pending_aim_y = 0;
    g_aim_at = 0;
    g_direct_used_at.store(0, std::memory_order_relaxed);
}

bool UiActive() { return g_ui_active.load(std::memory_order_relaxed); }

bool ConsumeMouseAim(float& dx, float& dy) {
    std::lock_guard lock(g_mouse_lock);
    if (g_ui_active.load(std::memory_order_relaxed) || !DirectAimLive(NowMs())) {
        g_aim_x = g_aim_y = 0;
        return false;
    }
    // If no pad poll ran during the stall, preserve the grace-period counts
    // here as well. Release them through the same bounded transition buffer.
    if (NowMs() - g_aim_at > 200) DeferMouseAim();

    float px = 0, py = 0;
    if (g_pending_aim_x != 0 || g_pending_aim_y != 0) {
        px = std::clamp(g_pending_aim_x, -kPendingAimStep, kPendingAimStep);
        py = std::clamp(g_pending_aim_y, -kPendingAimStep, kPendingAimStep);
        g_pending_aim_x -= px;
        g_pending_aim_y -= py;
    }
    if (g_aim_x == 0 && g_aim_y == 0 && px == 0 && py == 0) return false;
    dx = g_aim_x + px;
    dy = g_aim_y + py;
    g_aim_x = g_aim_y = 0;
    g_direct_used_at.store(NowMs(), std::memory_order_relaxed);
    return true;
}

bool DirectAimRecent() {
    const uint64_t used = g_direct_used_at.load(std::memory_order_relaxed);
    return used && NowMs() - used < 5000;
}

void Init() {
    if (g_sdl) return;  // started already for the game code window
    // the old right-stick path stays available as a fallback
    if (std::getenv("TCNYC_MOUSE_STICK")) g_direct_mouse_aim = false;
    // WGI reads the triggers apart and HIDAPI takes PlayStation and Switch pads. Reads come from the guest's threads,
    // so SDL's own thread takes the device notices. Hosts call SetHints before their first SDL_Init.
    SetHints();
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
        const bool ui = g_ui_active.load(std::memory_order_relaxed);
        const bool controller_active = p.buttons || std::abs(p.lx) > 8000 || std::abs(p.ly) > 8000 ||
                                       std::abs(p.rx) > 8000 || std::abs(p.ry) > 8000 ||
                                       std::any_of(std::begin(p.analog), std::end(p.analog),
                                                   [](uint8_t value) { return value > 32; });
        if (active || !p.connected) g_keyboard_mouse_active.store(true, std::memory_order_relaxed);
        else if (controller_active) {
            g_keyboard_mouse_active.store(false, std::memory_order_relaxed);
            // Relinquish held camera angles immediately. The next mouse
            // sample starts from the controller's current heading.
            std::lock_guard lock(g_mouse_lock);
            g_direct_used_at.store(0, std::memory_order_relaxed);
            g_aim_x = g_aim_y = 0; // a flick before the handoff must not turn once
            g_pending_aim_x = g_pending_aim_y = 0;
        }
        // While an ImGui overlay owns the keyboard, do not merge the keyboard
        // pad into port 0: its movement and button mappings would reach
        // NavBits as extra menu presses. ImGui gets the keys directly, and the
        // real controller (if any) stays the pad source.
        if (!ui && (active || !p.connected)) {
            keyboard_mouse.connected = true;
            p = keyboard_mouse;
        } else if (ui) {
            // Keep port 0 present as before. With no controller the pad is
            // already neutral; keyboard overlay controls never enter it.
            p.connected = true;
        }
    }
    // A background window keeps receiving controller state through SDL, but it
    // must not steer, attack or open the menu. Scripted and replayed input is
    // applied by GameInfo::pad just below, after this gate.
    if (!WindowFocused()) {
        const bool connected = p.connected;
        p = Pad{};
        p.connected = connected;
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
