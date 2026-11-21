// Value lists and stepping rules for the in-game Graphics menu. No runtime or
// ImGui dependencies, so a test can compile it alone.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace game::choices {

inline constexpr uint32_t kRenderPercents[] = {25, 50, 58, 67, 75, 100, 125, 150, 175, 200};
inline constexpr uint32_t kMinRenderPercent = 20, kMaxRenderPercent = 200;
inline constexpr uint32_t kAnisoLevels[] = {0, 2, 4, 8, 16}; // 0: as the game asks
inline constexpr uint32_t kMsaaSamples[] = {0, 2, 4, 8};     // 0: off
inline constexpr uint32_t kAoModes[] = {0, 1, 2};            // off, half resolution, full resolution
// Two menu rows. Ambient occlusion: 0 off, 1 SSAO (listed first: the lighter one), 2 GTAO; it sets
// app::Settings::ao_method (0 GTAO, 1 SSAO) and turns ao on at ao_quality. Occlusion quality: 1 half, 2 full.
inline constexpr uint32_t kAoMethodChoices[] = {0, 1, 2};
inline constexpr uint32_t kAoQualities[] = {1, 2};
inline constexpr uint32_t AoMethodChoice(uint32_t ao, uint32_t method) { return ao == 0 ? 0 : method == 1 ? 1 : 2; }
// applies a method choice: returns the new ao, writes the new method
inline constexpr uint32_t ApplyAoMethodChoice(uint32_t choice, uint32_t quality, uint32_t &method) {
    if (choice == 0) return 0;
    method = choice == 1 ? 1 : 0;
    return quality == 1 ? 1 : 2;
}
// City draw distance (app::Settings::city_range): 0 the game's 3x3 tiles, then a 5x5, 7x7 or 9x9 ring.
inline constexpr uint32_t kCityRanges[] = {0, 1, 2, 3};
inline constexpr uint32_t kCityInnerDetail[] = {1, 2, 3};
inline constexpr uint32_t kCityOuterDetail[] = {0, 1, 2, 3};
inline constexpr uint32_t kFsr3 = 2;
// Keep DLSS's setting value even when the optional backend was omitted.
inline constexpr uint32_t kDlss = 3;
inline constexpr uint32_t kFsr4 = 4;
// The temporal upscalers (FSR 3.1, FSR 4.1, DLSS) run at their standard presets' render scales only.
inline constexpr bool TemporalScale(uint32_t percent) {
    return percent == 50 || percent == 58 || percent == 67 || percent == 100;
}
// "Quality" and so on for a temporal render scale, nullptr for others
inline constexpr const char *TemporalPresetName(uint32_t percent) {
    switch (percent) {
    case 50:
        return "Performance";
    case 58:
        return "Balanced";
    case 67:
        return "Quality";
    case 100:
        return "Native AA";
    default:
        return nullptr;
    }
}
#if defined(_WIN32) && defined(TCNYC_HAVE_DLSS)
inline constexpr uint32_t kUpscalers[] = {0, 1, kFsr3, kDlss, kFsr4};
#elif defined(_WIN32)
inline constexpr uint32_t kUpscalers[] = {0, 1, kFsr3, kFsr4};
#elif defined(TCNYC_HAVE_DLSS)
inline constexpr uint32_t kUpscalers[] = {0, 1, kFsr3, kDlss};
#else
inline constexpr uint32_t kUpscalers[] = {0, 1, kFsr3};  // FSR 3.1 through AMD's open Vulkan backend
#endif
// Controller aim response: 0 is the port's radial dead zone + quadratic curve,
// 1 keeps the original Xbox per-axis dead zone and cubic.
inline constexpr uint32_t kControllerAimResponses[] = {0, 1};
inline const char *ControllerAimResponseName(uint32_t response) {
    static const char *const names[] = {"Refined", "Original"};
    return names[response < 2 ? response : 0];
}
// Applied after the curve. 100 keeps the original full-stick turn speed; higher
// values let players who still find full-stick turns slow raise it.
inline constexpr uint32_t kControllerAimSensitivities[] = {
    100, 110, 120, 130, 140, 150, 160, 170, 180, 190, 200,
    220, 240, 260, 280, 300, 325, 350, 375, 400};
// Mouse look speed and vertical speed relative to it, in percent (settings
// mouse_sensitivity_percent / mouse_vertical_sensitivity_percent, 10 to 400).
inline constexpr uint32_t kMinMouseSensitivityPercent = 10, kMaxMouseSensitivityPercent = 400;
inline constexpr uint32_t kMouseSensitivities[] = {
    10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120, 130, 140, 150,
    175, 200, 225, 250, 300, 350, 400};
inline constexpr uint32_t kBloomStrengths[] = {50, 100, 150, 200, 300, 400};  // percent of the original
inline constexpr uint32_t kShadowLevels[] = {0, 1, 2};  // shadow resolution and softness: off or original, then two steps
inline constexpr uint32_t kSharpenPercents[] = {0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100};
// Display colour adjustments applied after PC contrast and before the HDR encode. 100 is neutral for all
// four; brightness is a gain so black (letterbox bars, fades) stays black.
inline constexpr uint32_t kGammaPercents[] = {50, 60, 70, 80, 90, 100, 110, 120, 130, 140, 150, 175, 200, 225, 250};
inline constexpr uint32_t kBrightnessPercents[] = {50, 60, 70, 80, 90, 100, 110, 120, 130, 140, 150};
inline constexpr uint32_t kContrastPercents[] = {50, 60, 70, 80, 90, 100, 110, 120, 130, 140, 150};
inline constexpr uint32_t kSaturationPercents[] = {0, 25, 50, 75, 100, 125, 150, 175, 200};
inline float GradeGamma(uint32_t percent) { return float(std::clamp(percent, 50u, 250u)) / 100.0f; }
inline float GradeBrightness(uint32_t percent) { return float(std::clamp(percent, 50u, 150u)) / 100.0f; }
inline float GradeContrast(uint32_t percent) { return float(std::clamp(percent, 50u, 150u)) / 100.0f; }
inline float GradeSaturation(uint32_t percent) { return float(std::min(percent, 200u)) / 100.0f; }
inline bool GradeActive(uint32_t gamma, uint32_t brightness, uint32_t contrast, uint32_t saturation) {
    return gamma != 100 || brightness != 100 || contrast != 100 || saturation != 100;
}
// The game's Swap limits pacing to 60 / presentation interval (d3d8.cpp), so
// higher caps have no effect and are not offered.
inline constexpr uint32_t kFrameCaps[] = {30, 60};
inline constexpr uint32_t kGameFrameLimit = 60;
// Field of view, as a percentage of the gameplay camera's tan(fov / 2): 100 is the game's own.
inline constexpr uint32_t kFovPercents[] = {80, 90, 100, 110, 120, 130, 140, 150};
inline constexpr uint32_t kMinFovPercent = 50, kMaxFovPercent = 200;
// Object detail (app::Settings::object_detail): how far meshes keep each detail level. 0 is the game's own,
// then 2x, 4x and always the highest detail.
inline constexpr uint32_t kObjectDetails[] = {0, 1, 2, 3};
inline const char *ObjectDetailName(uint32_t level) {
    static const char *const names[] = {"Original", "High", "Very high", "Maximum"};
    return names[level < 4 ? level : 0];
}
// Pedestrian detail (app::Settings::ped_detail): how close a distant stand-in pedestrian must be before the
// game swaps in the full model. 0 is the game's own, then 2x, 4x and the stand-ins' whole range.
inline constexpr uint32_t kPedDetails[] = {0, 1, 2, 3};
inline const char *PedDetailName(uint32_t level) {
    static const char *const names[] = {"Original", "High", "Very high", "Maximum"};
    return names[level < 4 ? level : 0];
}
// Pedestrian limit (app::Settings::ped_limit): full pedestrians at once. 0 is the game's 15. Experimental.
inline constexpr uint32_t kPedLimits[] = {0, 1, 2, 3, 4, 5, 6};
inline const char *PedLimitName(uint32_t level) {
    static const char *const names[] = {"15 (Original)", "20", "25", "30", "40", "50", "75"};
    return names[level < 7 ? level : 0];
}
// Crowd density (app::Settings::ped_density): how many pedestrians each spawn point keeps. 0 is the game's own.
inline constexpr uint32_t kPedDensities[] = {0, 1, 2, 3};
inline const char *PedDensityName(uint32_t level) {
    static const char *const names[] = {"Original", "1.5x", "2x", "3x (Experimental)"};
    return names[level < 4 ? level : 0];
}
// Traffic distance (app::Settings::traffic_distance): how far out ambient cars spawn. Cars are removed at 4/3 of
// that range so a new car is not removed straight away (Original keeps the game's own ranges).
inline constexpr uint32_t kTrafficDistances[] = {0, 1, 2};
inline const char *TrafficDistanceName(uint32_t level) {
    static const char *const names[] = {"Original", "1.5x", "2x"};
    return names[level < 3 ? level : 0];
}
inline float TrafficSpawnScale(uint32_t level) {
    constexpr float kScales[] = {1.0f, 1.5f, 2.0f};
    return kScales[level < 3 ? level : 1];
}
inline float TrafficDespawnScale(uint32_t level) { return level == 0 ? 1.0f : TrafficSpawnScale(level) * 4.0f / 3.0f; }

// Traffic density (app::Settings::traffic_density): multiplier on the ambient car budget.
inline constexpr uint32_t kTrafficDensities[] = {0, 1, 2, 3, 4, 5};
inline const char *TrafficDensityName(uint32_t level) {
    static const char *const names[] = {"Original", "1.5x", "2x", "3x", "4x (Experimental)", "5x (Experimental)"};
    return names[level < 6 ? level : 0];
}
inline int32_t TrafficScale(int32_t value, uint32_t level) {
    switch (level) {
    case 1: return value * 3 / 2;
    case 2: return value * 2;
    case 3: return value * 3;
    case 4: return value * 4;
    case 5: return value * 5;
    default: return value;
    }
}
// HDR output: SDR white in nits, the highlight peak (0: the display's reported peak), and the
// inverse tone map strength in percent.
inline constexpr uint32_t kHdrPaperWhiteNits[] = {
    80,  90,  100, 110, 120, 130, 140, 150, 160, 170, 180, 190, 200, 210, 220, 230, 240,
    250, 260, 270, 280, 290, 300, 310, 320, 330, 340, 350, 360, 370, 380, 390, 400};
inline constexpr uint32_t kHdrPeakNits[] = {0,   400, 500, 600, 700, 800, 900, 1000, 1100,
                                            1200, 1300, 1400, 1500, 1600, 1700, 1800, 1900, 2000};
inline constexpr uint32_t kHdrHighlightPercents[] = {0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100};
// Frame shapes, width / height x 1000 (app::Settings::aspect): 0 matches the display, then 4:3, 16:10, 16:9, 21:9
// (64:27) and 32:9. 3D widens to the shape; HUD and menus stay 16:9 in the middle; movies stretch to fill.
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
