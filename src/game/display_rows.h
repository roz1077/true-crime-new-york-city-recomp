// Rules for the Graphics rows added to the native Options > Display tab
// (native_graphics.cpp): cursor movement, value changes and row text. No
// runtime dependencies, so scripts/test_display_rows.py can compile it.
#pragma once

#include <cstdint>
#include <cstdio>
#include <iterator>
#include <span>

#include "common/graphics_choices.h"

namespace game::display_rows {

// MenuPauseInterface focus (+0x7C) on an Options tab
inline constexpr uint32_t kTabBarFocus = 1, kRowListFocus = 4;
// dispatcher input bits (event 9); Back (cancel) is 0x20 and B 0x200
inline constexpr uint32_t kUp = 0x01, kDown = 0x02, kLeft = 0x04, kRight = 0x08, kAccept = 0x100;

enum class Option { Resolution, Aspect, RenderScale, FieldOfView, Anisotropic, Msaa, Smaa, VSync, FrameCap, ShowFps };
inline constexpr Option kOptions[] = {Option::Resolution,  Option::Aspect,      Option::RenderScale,
                                      Option::FieldOfView, Option::Anisotropic, Option::Msaa,
                                      Option::Smaa,
                                      Option::VSync,       Option::FrameCap,    Option::ShowFps};
inline constexpr uint32_t kNativeRows = 1; // SUBTITLES, the game's own row
inline constexpr uint32_t kExtraRows = uint32_t(std::size(kOptions));
inline constexpr uint32_t kRows = kNativeRows + kExtraRows;
inline constexpr size_t kTextBytes = 64;

// the option on a row after SUBTITLES
inline constexpr Option OptionAt(uint32_t row) { return kOptions[row - kNativeRows]; }
inline constexpr uint32_t RowOf(Option option) {
    for (uint32_t i = 0; i < kExtraRows; ++i)
        if (kOptions[i] == option) return kNativeRows + i;
    return 0;
}

// a resolution as the list holds it: width << 32 | height, 0 for the desktop's
inline constexpr uint64_t Size(uint32_t width, uint32_t height) { return uint64_t(width) << 32 | height; }

// the settings the rows show and change. The resolution row edits size and applies it on A (a fullscreen mode
// switch per step would flash the screen); applied is the one in use.
struct Values {
    uint32_t render_percent, fov_percent, aniso, fps_cap, aspect, msaa;
    bool vsync, smaa, show_fps;
    uint64_t size, applied;
};

struct Step {
    bool handled = false; // false: the game handles the input
    uint32_t row = 0, focus = kRowListFocus;
    int change = 0;     // -1 or 1: change the option on this row
    bool apply = false; // A on the resolution row
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
        if (row == 0)
            s.focus = kTabBarFocus;
        else
            s.row = row - 1;
    } else if ((input & (kLeft | kRight)) && row >= kNativeRows && row < kRows) {
        s.handled = true;
        s.change = input & kRight ? 1 : -1;
    } else if ((input & kAccept) && row == RowOf(Option::Resolution)) {
        s.handled = true;
        s.apply = true;
    }
    return s;
}

// The tab's frame holds five rows. With more, the list shows a window of them
// that follows the selected row; first is the logical row in the top slot.
inline constexpr uint32_t kVisibleRows = 5;
inline constexpr uint32_t kShownRows = kRows < kVisibleRows ? kRows : kVisibleRows;

inline uint32_t Scroll(uint32_t row, uint32_t first) {
    if (row < first) return row;
    if (row >= first + kShownRows) return row + 1 - kShownRows;
    return first;
}

// sizes: the resolutions to step through, the desktop's (0) first
inline void Change(Values &v, Option option, int direction, std::span<const uint64_t> sizes) {
    switch (option) {
    case Option::Resolution: {
        size_t i = 0;
        while (i < sizes.size() && sizes[i] != v.size)
            ++i;
        if (i == sizes.size()) i = 0; // a size the monitor no longer lists: from the desktop's
        if (direction > 0 && i + 1 < sizes.size()) ++i;
        if (direction < 0 && i > 0) --i;
        if (!sizes.empty()) v.size = sizes[i];
        break;
    }
    case Option::Aspect:
        v.aspect = choices::Step(choices::kAspects, v.aspect, direction);
        break;
    case Option::RenderScale:
        v.render_percent =
            choices::ClampRenderPercent(choices::Step(choices::kRenderPercents, v.render_percent, direction));
        break;
    case Option::FieldOfView:
        v.fov_percent = choices::ClampFovPercent(choices::Step(choices::kFovPercents, v.fov_percent, direction));
        break;
    case Option::Anisotropic:
        v.aniso = choices::Step(choices::kAnisoLevels, v.aniso, direction);
        break;
    case Option::Msaa:
        v.msaa = choices::Step(choices::kMsaaSamples, v.msaa, direction);
        break;
    case Option::Smaa:
        v.smaa = !v.smaa;
        break;
    case Option::VSync:
        v.vsync = !v.vsync;
        break;
    case Option::ShowFps:
        v.show_fps = !v.show_fps;
        break;
    case Option::FrameCap:
        v.fps_cap = choices::Step(choices::kFrameCaps, choices::EffectiveFrameCap(v.fps_cap), direction);
        break;
    }
}

// upper case like the game's rows; its font has no '%'
inline void Text(const Values &v, Option option, char (&text)[kTextBytes]) {
    switch (option) {
    case Option::Resolution: {
        const uint32_t w = uint32_t(v.size >> 32), h = uint32_t(v.size);
        const char *apply = v.size != v.applied ? " - PRESS A" : "";
        if (!v.size)
            std::snprintf(text, kTextBytes, "RESOLUTION DESKTOP%s", apply);
        else
            std::snprintf(text, kTextBytes, "RESOLUTION %uX%u%s", w, h, apply);
        break;
    }
    case Option::Aspect:
        if (!v.aspect)
            std::snprintf(text, kTextBytes, "ASPECT RATIO AUTO");
        else if (*choices::AspectName(v.aspect))
            std::snprintf(text, kTextBytes, "ASPECT RATIO %s", choices::AspectName(v.aspect));
        else
            std::snprintf(text, kTextBytes, "ASPECT RATIO %u.%02u", v.aspect / 1000, v.aspect % 1000 / 10);
        break;
    case Option::RenderScale:
        std::snprintf(text, kTextBytes, "RENDER SCALE %u.%02uX", v.render_percent / 100, v.render_percent % 100);
        break;
    case Option::FieldOfView:
        std::snprintf(text, kTextBytes, "FIELD OF VIEW %u.%02uX", v.fov_percent / 100, v.fov_percent % 100);
        break;
    case Option::Anisotropic:
        if (v.aniso)
            std::snprintf(text, kTextBytes, "ANISOTROPIC %uX", v.aniso);
        else
            std::snprintf(text, kTextBytes, "ANISOTROPIC OFF");
        break;
    case Option::Msaa:
        if (v.msaa)
            std::snprintf(text, kTextBytes, "MSAA %uX", v.msaa);
        else
            std::snprintf(text, kTextBytes, "MSAA OFF");
        break;
    case Option::Smaa:
        std::snprintf(text, kTextBytes, "SMAA %s", v.smaa ? "ON" : "OFF");
        break;
    case Option::VSync:
        std::snprintf(text, kTextBytes, "V-SYNC %s", v.vsync ? "ON" : "OFF");
        break;
    case Option::ShowFps:
        std::snprintf(text, kTextBytes, "SHOW FPS %s", v.show_fps ? "ON" : "OFF");
        break;
    case Option::FrameCap:
        std::snprintf(text, kTextBytes, "FRAME LIMIT %u", choices::EffectiveFrameCap(v.fps_cap));
        break;
    }
}

} // namespace game::display_rows
