// Value lists and stepping rules for the in-game Graphics menu. No runtime or
// ImGui dependencies, so scripts/test_graphics_choices.py can compile it alone.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace game::choices {

inline constexpr uint32_t kRenderPercents[] = {25, 50, 67, 75, 100, 125, 150, 175, 200};
inline constexpr uint32_t kMinRenderPercent = 20, kMaxRenderPercent = 200;
inline constexpr uint32_t kAnisoLevels[] = {0, 2, 4, 8, 16}; // 0: as the game asks
inline constexpr uint32_t kMsaaSamples[] = {0, 2, 4, 8};     // 0: off
inline constexpr uint32_t kAoModes[] = {0, 1, 2};            // off, half resolution, full resolution
// The game's Swap limits pacing to 60 / presentation interval (d3d8.cpp), so
// higher caps have no effect and are not offered.
inline constexpr uint32_t kFrameCaps[] = {30, 60};
inline constexpr uint32_t kGameFrameLimit = 60;
// Field of view, as a percentage of the gameplay camera's tan(fov / 2): 100 is the game's own.
inline constexpr uint32_t kFovPercents[] = {80, 90, 100, 110, 120, 130, 140, 150};
inline constexpr uint32_t kMinFovPercent = 50, kMaxFovPercent = 200;
// Frame shapes, width / height x 1000 (app::Settings::aspect): 0 matches the display, then 4:3, 16:10, 16:9, 21:9
// (64:27) and 32:9. 3D widens to the shape; HUD, menus and movies stay 16:9 in the middle.
inline constexpr uint32_t kAspects[] = {0, 1333, 1600, 1778, 2370, 3556};

// "4:3" and so on for a listed shape, "" for others
inline const char *AspectName(uint32_t aspect) {
    switch (aspect) {
    case 1333:
        return "4:3";
    case 1600:
        return "16:10";
    case 1778:
        return "16:9";
    case 2370:
        return "21:9";
    case 3556:
        return "32:9";
    default:
        return "";
    }
}

// The value one step from current in a sorted list. A current value between
// entries (hand-edited settings file) steps to its neighbour in that
// direction. Ends do not wrap.
inline uint32_t Step(std::span<const uint32_t> values, uint32_t current, int direction) {
    if (values.empty() || direction == 0) return current;
    if (direction > 0) {
        for (uint32_t v : values)
            if (v > current) return v;
        return current < values.back() ? values.back() : current;
    }
    for (size_t i = values.size(); i-- > 0;)
        if (values[i] < current) return values[i];
    return current > values.front() ? values.front() : current;
}

// The frame cap the game runs at for a stored fps_cap: 0 (unlimited), 1
// (monitor refresh) and anything above the game's limit run at that limit.
// Refresh rates below 60 Hz are not considered here.
inline uint32_t EffectiveFrameCap(uint32_t stored) {
    if (stored <= 1 || stored > kGameFrameLimit) return kGameFrameLimit;
    return stored;
}

inline uint32_t ClampFovPercent(uint32_t percent) {
    if (percent < kMinFovPercent) return kMinFovPercent;
    if (percent > kMaxFovPercent) return kMaxFovPercent;
    return percent;
}

// The field of view in degrees when a camera of base_degrees has its tan(fov / 2) scaled by percent.
inline float ScaledFovDegrees(float base_degrees, uint32_t percent) {
    constexpr float kPi = 3.14159265358979f;
    const float half = std::tan(base_degrees * kPi / 360.0f) * float(percent) / 100.0f;
    return std::atan(half) * 360.0f / kPi;
}

inline uint32_t ClampRenderPercent(uint32_t percent) {
    if (percent < kMinRenderPercent) return kMinRenderPercent;
    if (percent > kMaxRenderPercent) return kMaxRenderPercent;
    return percent;
}

// The internal render height for an output height at a render percentage,
// as app::RenderScale computes it.
inline uint32_t RenderHeight(uint32_t output_height, uint32_t percent) {
    return uint32_t(uint64_t(output_height) * percent / 100);
}

} // namespace game::choices
