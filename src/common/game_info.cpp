#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "app/app.h"
#include "graphics_menu.h"
#include "core/input.h"

namespace {

// XBE_PAD_SCRIPT=<seconds>:<button>[,<seconds>:<button>...] presses a button on controller 1 for 0.2 s at each time
// (from the first poll), to reach a scene without someone at the controller. Buttons: a b x y start back up down left
// right. Controller 1 also counts as plugged in while a script is set.
struct Press {
    double at;
    uint16_t buttons;
    int analog;  // -1 for none
};

std::vector<Press> LoadScript() {
    std::vector<Press> out;
    const char* v = std::getenv("XBE_PAD_SCRIPT");
    if (!v) return out;
    struct Name { const char* name; uint16_t buttons; int analog; };
    constexpr Name kNames[] = {{"up", 1, -1},    {"down", 2, -1}, {"left", 4, -1}, {"right", 8, -1},
                               {"start", 16, -1}, {"back", 32, -1}, {"a", 0, 0},     {"b", 0, 1},
                               {"x", 0, 2},       {"y", 0, 3}};
    std::string s = v;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t end = s.find(',', pos);
        if (end == std::string::npos) end = s.size();
        std::string item = s.substr(pos, end - pos);
        size_t colon = item.find(':');
        if (colon != std::string::npos) {
            double at = std::atof(item.c_str());
            std::string b = item.substr(colon + 1);
            for (const Name& n : kNames)
                if (b == n.name) out.push_back({at, n.buttons, n.analog});
        }
        pos = end + 1;
    }
    return out;
}

// XBE_PAD_RECORD=<file>: writes each button pressed on controller 1 as "<seconds>:<button>," to the file, in the
// XBE_PAD_SCRIPT format, so a way through the menus can be played back later
void Record(const input::Pad& p, double t) {
    static FILE* f = [] {
        const char* v = std::getenv("XBE_PAD_RECORD");
        return v ? std::fopen(v, "w") : nullptr;
    }();
    static uint16_t last_buttons = 0;
    static uint8_t last_analog[4] = {};
    if (!f) return;
    constexpr const char* kButtons[] = {"up", "down", "left", "right", "start", "back"};
    constexpr const char* kAnalog[] = {"a", "b", "x", "y"};
    for (int i = 0; i < 6; i++)
        if ((p.buttons & (1u << i)) && !(last_buttons & (1u << i))) std::fprintf(f, "%.2f:%s,", t, kButtons[i]);
    for (int i = 0; i < 4; i++)
        if (p.analog[i] > 0x80 && last_analog[i] <= 0x80) std::fprintf(f, "%.2f:%s,", t, kAnalog[i]);
    last_buttons = p.buttons;
    std::memcpy(last_analog, p.analog, 4);
    std::fflush(f);
}

void ScriptedPad(int port, input::Pad& p) {
    static const std::vector<Press> script = LoadScript();
    static const auto start = std::chrono::steady_clock::now();
    if (port != 0) return;
    if (!script.empty()) p.connected = true;
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (p.connected) Record(p, t);
    if (script.empty()) return;
    for (const Press& press : script) {
        if (t < press.at || t >= press.at + 0.2) continue;
        p.buttons |= press.buttons;
        if (press.analog >= 0) p.analog[press.analog] = 0xFF;
    }
}

// the script first, so the Graphics menu sees what the game would
void FilterPad(int port, input::Pad& p) {
    ScriptedPad(port, p);
    game::FilterMenuPad(port, p);
}

}  // namespace

// what the runtime asks of the game; strings must be set, hooks may stay null
const app::GameInfo app::kGame = {
    .title = L"True Crime: New York City",             // window and splash
    .short_title = L"True Crime NYC",             // message boxes
    .name = L"TrueCrimeNYC",                    // window classes, crash dumps, handover event
    .log = "tcnyc",                      // log file prefix
    .settings = "settings.toml",          // settings file beside the exe
    .splash = L"splash",                  // optional splash.png or splash.bmp beside the exe
    .missing = L"Put the game in the assets folder: its disc image (.iso), or default.xbe with its folders beside it.",
    .images = {{"default.xbe", "TrueCrimeNYC"}},  // each recompiled xbe and its exe, for title launches
    .options = {{"show_fps", &game::g_show_fps, nullptr}},  // game settings kept in the settings file
    .load = nullptr,                      // reads more from the settings file's toml::table
    .save = nullptr,                      // writes more into it
    .init = nullptr,                      // after the settings load, before the window and boot
    .loaded = nullptr,                    // image in memory, before its entry point runs
    .window = nullptr,                    // the game window is up
    .message = game::GraphicsMenuMessage, // window messages first; true takes one
    .ui = game::DrawGraphicsMenu,         // ImGui drawing each frame: the Graphics menu (F10)
    .pad = FilterPad,                     // changes a polled pad
    .rumble = nullptr,                    // changes a rumble before it is set
};
