#include "app/app.h"
#include "graphics_menu.h"
#include "pad_tools.h"

namespace game {
uint32_t MainLoopSwapInterval(uint32_t interval);
}

namespace {

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
    .options = {{"show_fps", &game::g_show_fps, nullptr}, {"fov_percent", nullptr, &game::g_fov_percent}},
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
};
