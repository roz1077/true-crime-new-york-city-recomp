#include "app/app.h"
#include "core/input.h"
#include "core/log.h"
#include "graphics_menu.h"
#include "pad_tools.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#if TCNYC_DEV_TOOLS
#include "dev/dev.h"
#endif

#include <chrono>
#include <filesystem>
#include <string>

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

// A missing profile header means a new install/profile. The title starts with
// vibration on, but if a new profile applies an off default during the first
// seconds, force it on once before the menu is usable. Existing profiles keep
// the player's choice.
bool ProfileHeaderExists() {
    std::error_code error;
    const std::filesystem::path udata =
        std::filesystem::path(app::ExeDir()) / "hdd" / "E" / "UDATA" / "41560045";
    for (const auto& entry : std::filesystem::directory_iterator(udata, error)) {
        if (!entry.is_directory(error)) continue;
        const std::string name = entry.path().filename().string();
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".bak") == 0) continue;
        if (std::filesystem::exists(entry.path() / "TCNYC", error)) return true;
    }
    return false;
}

uint64_t NowMs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
}

bool g_new_profile = false;
bool g_vibration_defaulted = false;
uint64_t g_vibration_deadline = 0;

void InitVibrationProfile() {
    g_new_profile = !ProfileHeaderExists();
    g_vibration_deadline = NowMs() + 30000;
}

// scripted or replayed input first, so the Graphics menu sees what the game would
void FilterPad(int port, input::Pad &pad) {
    game::ApplyPadTools(port, pad);
    game::FilterMenuPad(port, pad);
    // A new profile can apply an off default after the game object exists;
    // force it on once, inside the short window before the menu is usable.
    if (port == 0 && g_new_profile && !g_vibration_defaulted && NowMs() <= g_vibration_deadline &&
        mem::Base() && mem::IsCommitted(0x360E00) && mem::IsCommitted(0x2ECDB0)) {
        const uint32_t game = kernel::R32(0x360E00);
        if (game && mem::IsCommitted(game + 0xFED) && kernel::G<uint8_t>(game + 0xFED) == 0) {
            kernel::G<uint8_t>(game + 0xFED) = 1;
            kernel::G<uint8_t>(0x2ECDB0) = 1;
            g_vibration_defaulted = true;
        }
    }
}

} // namespace

// The gameplay camera follows a pawn; the attract/menu flythrough and most
// cutscenes do not. Used to keep the deferred AO path out of the front end.
namespace game {
bool GameplayCameraActive() {
    if (!mem::Base() || !mem::IsCommitted(0x360E00)) return false;
    const uint32_t game = kernel::R32(0x360E00);
    if (!game || !mem::IsCommitted(game + 0xAC)) return false;
    const uint32_t camera = kernel::R32(game + 0xAC);
    if (!camera || !mem::IsCommitted(camera + 0x17C) || !mem::IsCommitted(camera + 0x18C)) return false;
    const uint8_t active = kernel::G<uint8_t>(camera + 0x18C);
    const uint8_t mode = kernel::G<uint8_t>(camera + 0x19);
    const uint32_t target = kernel::R32(camera + 0x17C);
    static int last_active = -1, last_mode = -1;
    if (int(active) != last_active || int(mode) != last_mode) {
        last_active = active;
        last_mode = mode;
        LOGR("game camera: active %u mode %u target %08X", active, mode, target);
    }
    const bool gameplay_mode = mode == 0 || mode == 5 || mode == 6 || mode == 11 || mode == 13;
    return active != 0 && target != 0 && gameplay_mode;
}
} // namespace game

namespace {
// ImGui each frame: the Graphics menu (F10), and in dev builds the debug menu (F9)
void DrawUi(double fps) {
    game::DrawGraphicsMenu(fps);
#if TCNYC_DEV_TOOLS
    dev::DrawMenu();
#endif
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
                {"prompt_style", nullptr, &input::g_prompt_override},
                {"reload_key", nullptr, &input::g_reload_key}, {"heavy_attack_key", nullptr, &input::g_heavy_attack_key},
                {"use_key", nullptr, &input::g_use_key}, {"climb_key", nullptr, &input::g_climb_key}},
    .load = nullptr,                      // reads more from the settings file's toml::table
    .save = nullptr,                      // writes more into it
    .init = InitVibrationProfile,         // after the settings load, before the window and boot
    .loaded = nullptr,                    // image in memory, before its entry point runs
    .window = nullptr,                    // the game window is up
    .message = game::GraphicsMenuMessage, // window messages first; true takes one
    .ui = DrawUi,                         // ImGui drawing each frame: the Graphics menu (F10)
    .pad = FilterPad,                     // changes a polled pad
    .rumble = nullptr,                    // changes a rumble before it is set
    .swap_interval = game::MainLoopSwapInterval,
    .keyboard_driving = KeyboardDriving,
};
