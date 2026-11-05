#include "app/app.h"
#include "core/input.h"
#include "graphics_menu.h"
#include "pad_tools.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace game {
uint32_t MainLoopSwapInterval(uint32_t interval);
}

namespace {

bool KeyboardDriving() {
    if (!mem::Base() || !mem::IsCommitted(0x360E00)) return false;
    const uint32_t game = kernel::R32(0x360E00);
    if (!game || !mem::IsCommitted(game + 0xAC)) return false;
    const uint32_t camera = kernel::R32(game + 0xAC);
    if (!camera || !mem::IsCommitted(camera + 0x18C)) return false;
    return kernel::G<uint8_t>(camera + 0x18C) && kernel::G<uint8_t>(camera + 0x19) == 13;
}

// scripted or replayed input first, so the Graphics menu sees what the game would
void FilterPad(int port, input::Pad &pad) {
    game::ApplyPadTools(port, pad);
    game::FilterMenuPad(port, pad);
}

} // namespace

// what the runtime asks of the game; strings must be set, hooks may stay null
const app::GameInfo app::kGame = {
    .title = L"True Crime: New York City", // window and splash
    .short_title = L"True Crime NYC",      // message boxes
    .name = L"TrueCrimeNYC",               // window classes, crash dumps, handover event
    .log = "tcnyc",                        // log file prefix
    .settings = "settings.toml",           // settings file beside the exe
    .splash = L"splash",                   // optional splash.png or splash.bmp beside the exe
    .missing = L"Put the game in the assets folder: its disc image (.iso), or "
               L"default.xbe with its folders beside it.",
    .images = {{"default.xbe", "TrueCrimeNYC"}}, // image and executable for title launches
    .options = {{"show_fps", &game::g_show_fps, nullptr}, {"fov_percent", nullptr, &game::g_fov_percent},
                {"mouse_sensitivity_percent", nullptr, &input::g_mouse_sensitivity_percent},
                {"mouse_vertical_sensitivity_percent", nullptr, &input::g_mouse_vertical_sensitivity_percent},
                {"direct_mouse_aim", &input::g_direct_mouse_aim, nullptr},
                {"reload_key", nullptr, &input::g_reload_key}, {"heavy_attack_key", nullptr, &input::g_heavy_attack_key},
                {"use_key", nullptr, &input::g_use_key}, {"climb_key", nullptr, &input::g_climb_key}},
    .load = nullptr,                      // reads more from the settings file's toml::table
    .save = nullptr,                      // writes more into it
    .init = nullptr,                      // after the settings load, before the window and boot
    .loaded = nullptr,                    // image in memory, before its entry point runs
    .window = nullptr,                    // the game window is up
    .message = game::GraphicsMenuMessage, // window messages first; true takes one
    .ui = game::DrawGraphicsMenu,         // ImGui drawing each frame: the Graphics menu (F10)
    .pad = FilterPad,                     // changes a polled pad
    .rumble = nullptr,                    // changes a rumble before it is set
    .swap_interval = game::MainLoopSwapInterval,
    .keyboard_driving = KeyboardDriving,
};
