// Rules for the Graphics rows added to the native Options > Display tab
// (native_graphics.cpp): cursor movement, value changes and row text. No
// runtime dependencies, so scripts/test_graphics_choices.py can compile it.
#pragma once

#include <cstdint>
#include <cstdio>
#include <iterator>

#include "common/graphics_choices.h"

namespace game::display_rows {

// MenuPauseInterface focus (+0x7C) on an Options tab
inline constexpr uint32_t kTabBarFocus = 1, kRowListFocus = 4;
// dispatcher input bits (event 9); A is 0x100 and Back (cancel) 0x20
inline constexpr uint32_t kUp = 0x01, kDown = 0x02, kLeft = 0x04, kRight = 0x08;

enum class Option { RenderScale, Anisotropic, VSync, FrameCap };
inline constexpr Option kOptions[] = {Option::RenderScale, Option::Anisotropic, Option::VSync, Option::FrameCap};
inline constexpr uint32_t kNativeRows = 1; // SUBTITLES, the game's own row
inline constexpr uint32_t kExtraRows = uint32_t(std::size(kOptions));
inline constexpr uint32_t kRows = kNativeRows + kExtraRows;
inline constexpr size_t kTextBytes = 64;

// the settings the rows show and change
struct Values {
    uint32_t render_percent, aniso, fps_cap;
    bool vsync;
};

struct Step {
    bool handled = false; // false: the game handles the input
    uint32_t row = 0, focus = kRowListFocus;
    int change = 0;    // -1 or 1: change the option on this row
};

// One input in the Display tab's row list. The game's own row count for the
// tab is fixed at one, so movement over every row is handled here, as on the
// other tabs: down wraps, up from the first row returns to the tab bar. Left
// and right change an added row; on SUBTITLES they stay the game's toggle.
inline Step Input(uint32_t row, uint32_t input) {
    Step s;
    s.row = row;
    if (input & kDown) {
        s.handled = true;
        s.row = row + 1 < kRows ? row + 1 : 0;
    } else if (input & kUp) {
        s.handled = true;
        if (row == 0) s.focus = kTabBarFocus;
        else s.row = row - 1;
    } else if ((input & (kLeft | kRight)) && row >= kNativeRows && row < kRows) {
        s.handled = true;
        s.change = input & kRight ? 1 : -1;
    }
    return s;
}

inline void Change(Values &v, Option option, int direction) {
    switch (option) {
    case Option::RenderScale:
        v.render_percent = choices::ClampRenderPercent(choices::Step(choices::kRenderPercents, v.render_percent,
                                                                     direction));
        break;
    case Option::Anisotropic: v.aniso = choices::Step(choices::kAnisoLevels, v.aniso, direction); break;
    case Option::VSync: v.vsync = !v.vsync; break;
    case Option::FrameCap:
        v.fps_cap = choices::Step(choices::kFrameCaps, choices::EffectiveFrameCap(v.fps_cap), direction);
        break;
    }
}

// upper case like the game's rows; its font has no '%'
inline void Text(const Values &v, Option option, char (&text)[kTextBytes]) {
    switch (option) {
    case Option::RenderScale:
        std::snprintf(text, kTextBytes, "RENDER SCALE %u.%02uX", v.render_percent / 100, v.render_percent % 100);
        break;
    case Option::Anisotropic:
        if (v.aniso) std::snprintf(text, kTextBytes, "ANISOTROPIC %uX", v.aniso);
        else std::snprintf(text, kTextBytes, "ANISOTROPIC OFF");
        break;
    case Option::VSync: std::snprintf(text, kTextBytes, "V-SYNC %s", v.vsync ? "ON" : "OFF"); break;
    case Option::FrameCap:
        std::snprintf(text, kTextBytes, "FRAME LIMIT %u", choices::EffectiveFrameCap(v.fps_cap));
        break;
    }
}

} // namespace game::display_rows
