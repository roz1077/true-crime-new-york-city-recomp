// Rules for the Graphics rows added to the native Options > Display tab
// (native_graphics.cpp): cursor movement, value changes and row text. No
// runtime dependencies, so a test can compile it.
//
// Main follows the game's SUBTITLES row with display and rendering options.
// HDR SETTINGS opens a native subpage. The top-level Exit Game selection
// uses the same row renderer for its confirmation.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <span>
#include <string>

#include "common/graphics_choices.h"

namespace game::display_rows {

// MenuPauseInterface focus (+0x7C) on an Options tab
inline constexpr uint32_t kTabBarFocus = 1, kRowListFocus = 4;
// dispatcher input bits (event 9)
inline constexpr uint32_t kUp = 0x01, kDown = 0x02, kLeft = 0x04, kRight = 0x08, kBack = 0x20, kAccept = 0x100,
                          kCancel = 0x200;

enum class Page { Main, Hdr, Exit };

enum class Option {
    DisplayMode,
    Resolution,
    Aspect,
    CutsceneBars,
    RenderScale,
    Upscaler,
    Sharpening,
    FieldOfView,
    ObjectDetail,
    PedDetail,
    PedLimit,
    PedDensity,
    TrafficDensity,
    TrafficDistance,
    CityInnerDetail,
    CityOuterDetail,
    CityRange,
    Anisotropic,
    Msaa,
    Smaa,
    Taa,
    Bloom,
    BloomStrength,
    DepthOfField,
    AmbientOcclusion,
    AoQuality,
    ShadowQuality,
    ShadowSoftness,
    BuildingSeamFix,
    PcContrast,
    Gamma,
    Brightness,
    Contrast,
    Saturation,
    VSync,
    LowLatency, // D3D12, or Vulkan with VK_KHR_present_wait
    FrameCap,
    ShowFps,
    TextureReplacement,
    TextureDumping,
    DumpFormat,
    Defaults,
    HdrPage, // opens the HDR page
    Hdr,
    HdrPaperWhite,
    HdrPeak,
    HdrHighlights,
    HdrBack, // returns to the main page
    ExitPrompt,
    ExitCancel,
    ExitConfirm,
    Count,
};
inline constexpr uint32_t kOptionCount = uint32_t(Option::Count);

inline constexpr Option kMainOptions[] = {
    Option::DisplayMode, Option::Resolution,       Option::Aspect,          Option::CutsceneBars,
    Option::RenderScale,
    Option::Upscaler,    Option::Sharpening,
    Option::FieldOfView, Option::ObjectDetail,    Option::PedDetail,       Option::PedLimit,
    Option::PedDensity, Option::TrafficDensity,  Option::TrafficDistance,
    Option::CityInnerDetail, Option::CityOuterDetail, Option::Anisotropic, Option::Msaa,
    Option::Smaa,        Option::Taa,              Option::Bloom,            Option::BloomStrength,
    Option::DepthOfField,    Option::AmbientOcclusion, Option::AoQuality, Option::ShadowQuality, Option::ShadowSoftness,
    Option::BuildingSeamFix, Option::PcContrast,   Option::Gamma, Option::Brightness,
    Option::Contrast, Option::Saturation, Option::VSync,
    Option::LowLatency,
    Option::FrameCap,
    Option::ShowFps, Option::TextureReplacement, Option::TextureDumping, Option::DumpFormat,
    Option::HdrPage, Option::Defaults};
inline constexpr auto kMainOptionsWithoutOuterDetail = [] {
    std::array<Option, std::size(kMainOptions) - 1> options{};
    size_t next = 0;
    for (const Option option : kMainOptions)
        if (option != Option::CityOuterDetail) options[next++] = option;
    return options;
}();
inline constexpr Option kHdrOptions[] = {Option::Hdr, Option::HdrPaperWhite, Option::HdrPeak, Option::HdrHighlights,
                                         Option::HdrBack};
inline constexpr Option kExitOptions[] = {Option::ExitPrompt, Option::ExitCancel, Option::ExitConfirm};

inline constexpr std::span<const Option> Options(Page page, bool outer_detail = false) {
    if (page == Page::Hdr) return kHdrOptions;
    if (page == Page::Exit) return kExitOptions;
    if (outer_detail) return kMainOptions;
    return kMainOptionsWithoutOuterDetail;
}
// rows the game draws itself: SUBTITLES on the main page
inline constexpr uint32_t NativeRows(Page page) { return page == Page::Main ? 1 : 0; }
inline constexpr uint32_t RowCount(Page page, bool outer_detail = false) {
    return NativeRows(page) + uint32_t(Options(page, outer_detail).size());
}
inline constexpr size_t kTextBytes = 64;

// the option on a row at or after NativeRows(page)
inline constexpr Option OptionAt(Page page, uint32_t row, bool outer_detail = false) {
    return Options(page, outer_detail)[row - NativeRows(page)];
}
inline constexpr uint32_t RowOf(Page page, Option option, bool outer_detail = false) {
    const auto options = Options(page, outer_detail);
    for (uint32_t i = 0; i < options.size(); ++i)
        if (options[i] == option) return NativeRows(page) + i;
    return 0;
}

// a resolution as the list holds it: width << 32 | height, 0 for the desktop's
inline constexpr uint64_t Size(uint32_t width, uint32_t height) { return uint64_t(width) << 32 | height; }

// the settings the rows show and change. Display mode and resolution are edited here and applied together on A
// (a fullscreen mode switch per step would flash the screen); applied and applied_mode are the ones in use.
struct Values {
    uint32_t render_percent, fov_percent, aniso, fps_cap, aspect, msaa;
    bool vsync, smaa, show_fps;
    uint64_t size, applied;
    uint32_t mode = 1, applied_mode = 1; // app::DisplayMode: 0 fullscreen, 1 borderless, 2 windowed
    uint32_t object_detail = 0, ao = 0, ao_method = 0, ao_quality = 2, city_range = 0;
    uint32_t ped_detail = 0, ped_limit = 0, ped_density = 0, traffic_density = 0, traffic_distance = 0;
    uint32_t city_inner_detail = 1, city_outer_detail = 0;
    bool custom_textures = true, dump_textures = false;
    uint32_t texture_dump_format = 0;
    bool bloom = true, dof = true, building_seam_fix = true, pc_contrast = true, hdr = false;
    uint32_t gamma = 100, brightness = 100, contrast = 100, saturation = 100;  // app::Settings defaults
    bool remove_cutscene_bars = false;
    uint32_t hdr_paper_white = 200, hdr_peak = 0, hdr_highlights = 40;  // app::Settings defaults
    uint32_t display_peak = 0; // the HDR display's reported peak in nits, 0 when unknown (read only)
    uint32_t upscaler = 0, sharpening = 0, bloom_strength = 100, shadow_resolution = 0, shadow_softness = 0;  // app::Settings defaults
    bool low_latency = false, taa = false;
    // the upscalers' availability (read only): gpu::FsrState and gpu::DlssState as numbers, 0 not known yet
    uint32_t fsr_state = 0, dlss_state = 0;
};
inline constexpr uint32_t kFsrNo4 = 2, kFsrNoDll = 3;              // gpu::FsrState
inline constexpr uint32_t kDlssUnavailable = 2, kDlssNotBuilt = 3; // gpu::DlssState

inline bool DisplayPending(const Values &v) { return v.size != v.applied || v.mode != v.applied_mode; }
inline bool Enabled(const Values &v, Option option) { return option != Option::DumpFormat || v.dump_textures; }

struct Step {
    bool handled = false; // false: the game handles the input
    Page page = Page::Main;
    uint32_t row = 0, focus = kRowListFocus;
    int change = 0;     // -1 or 1: change the option on this row
    bool apply = false; // A on the display mode or resolution row
    bool reset = false; // A on Restore Defaults
    bool quit = false;  // A on Exit Game in the confirmation page
};

// One input in the Display tab's row list. The game's own row count for the
// tab is fixed at one, so movement over every row is handled here: down stops
// at the main page's last row (wraps on the HDR page), up from the main page's first row returns to the tab
// bar. Left and right change an added row; on SUBTITLES they stay the game's
// toggle. Back on the HDR page returns to the main page instead of cancelling.
inline Step Input(Page page, uint32_t row, uint32_t input, bool outer_detail = false) {
    Step s;
    s.page = page;
    s.row = row;
    if (page == Page::Exit) {
        s.handled = true;
        const uint32_t cancel = RowOf(page, Option::ExitCancel);
        const uint32_t confirm = RowOf(page, Option::ExitConfirm);
        if (input & (kBack | kCancel)) {
            s.page = Page::Main;
        } else if (input & kUp) {
            s.row = cancel;
        } else if (input & kDown) {
            s.row = confirm;
        } else if (input & kAccept) {
            if (row == cancel) {
                s.page = Page::Main;
            } else {
                s.quit = row == confirm;
            }
        }
        return s;
    }
    const uint32_t rows = RowCount(page, outer_detail);
    row = std::min(row, rows - 1);
    s.row = row;
    const bool added = row >= NativeRows(page) && row < rows;
    if (input & kDown) {
        s.handled = true;
        // the main page stops at its last row; the short HDR page wraps
        s.row = row + 1 < rows ? row + 1 : page == Page::Main ? row : 0;
    } else if (input & kUp) {
        s.handled = true;
        if (row > 0)
            s.row = row - 1;
        else if (page == Page::Main)
            s.focus = kTabBarFocus;
        else
            s.row = rows - 1;
    } else if ((input & (kLeft | kRight)) && added) {
        s.handled = true;
        const Option o = OptionAt(page, row, outer_detail);
        if (o != Option::HdrPage && o != Option::HdrBack && o != Option::Defaults)
            s.change = input & kRight ? 1 : -1;
    } else if ((input & kAccept) && added) {
        const Option o = OptionAt(page, row, outer_detail);
        if (o == Option::DisplayMode || o == Option::Resolution) {
            s.handled = true;
            s.apply = true;
        } else if (o == Option::HdrPage) {
            s.handled = true;
            s.page = Page::Hdr;
            s.row = 0;
        } else if (o == Option::HdrBack) {
            s.handled = true;
            s.page = Page::Main;
            s.row = RowOf(Page::Main, Option::HdrPage, outer_detail);
        } else if (o == Option::Defaults) {
            s.handled = true;
            s.reset = true;
        }
    } else if ((input & (kBack | kCancel)) && page == Page::Hdr) {
        s.handled = true;
        s.page = Page::Main;
        s.row = RowOf(Page::Main, Option::HdrPage, outer_detail);
    }
    return s;
}

// The tab's frame holds five rows. With more, the list shows a window of them
// that follows the selected row; first is the logical row in the top slot.
inline constexpr uint32_t kShownRows = 5;
static_assert(RowCount(Page::Main) >= kShownRows && RowCount(Page::Hdr) >= kShownRows,
              "the settings pages fill the tab's five slots");

inline uint32_t Scroll(uint32_t row, uint32_t first) {
    if (row < first) return row;
    if (row >= first + kShownRows) return row + 1 - kShownRows;
    return first;
}

// sizes: the resolutions to step through, the desktop's (0) first
inline void Change(Values &v, Option option, int direction, std::span<const uint64_t> sizes) {
    if (!Enabled(v, option)) return;
    switch (option) {
    case Option::DisplayMode:
        if (direction > 0 && v.mode < 2) ++v.mode;
        if (direction < 0 && v.mode > 0) --v.mode;
        break;
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
    case Option::CutsceneBars:
        v.remove_cutscene_bars = !v.remove_cutscene_bars;
        break;
    case Option::RenderScale:
        v.render_percent =
            choices::ClampRenderPercent(choices::Step(choices::kRenderPercents, v.render_percent, direction));
        break;
    case Option::ShadowQuality:
        v.shadow_resolution = choices::Step(choices::kShadowLevels, v.shadow_resolution, direction);
        break;
    case Option::ShadowSoftness:
        v.shadow_softness = choices::Step(choices::kShadowLevels, v.shadow_softness, direction);
        break;
    case Option::BloomStrength:
        v.bloom_strength = choices::Step(choices::kBloomStrengths, v.bloom_strength, direction);
        break;
    case Option::Upscaler:
        v.upscaler = choices::Step(choices::kUpscalers, v.upscaler, direction);
        break;
    case Option::Sharpening:
        v.sharpening = choices::Step(choices::kSharpenPercents, v.sharpening, direction);
        break;
    case Option::LowLatency:
        v.low_latency = !v.low_latency;
        break;
    case Option::FieldOfView:
        v.fov_percent = choices::ClampFovPercent(choices::Step(choices::kFovPercents, v.fov_percent, direction));
        break;
    case Option::ObjectDetail:
        v.object_detail = choices::Step(choices::kObjectDetails, v.object_detail, direction);
        break;
    case Option::PedDetail:
        v.ped_detail = choices::Step(choices::kPedDetails, v.ped_detail, direction);
        break;
    case Option::PedLimit:
        v.ped_limit = choices::Step(choices::kPedLimits, v.ped_limit, direction);
        break;
    case Option::PedDensity:
        v.ped_density = choices::Step(choices::kPedDensities, v.ped_density, direction);
        break;
    case Option::TrafficDensity:
        v.traffic_density = choices::Step(choices::kTrafficDensities, v.traffic_density, direction);
        break;
    case Option::TrafficDistance:
        v.traffic_distance = choices::Step(choices::kTrafficDistances, v.traffic_distance, direction);
        break;
    case Option::CityInnerDetail:
        v.city_inner_detail = choices::Step(choices::kCityInnerDetail, v.city_inner_detail, direction);
        break;
    case Option::CityOuterDetail:
        v.city_outer_detail = choices::Step(choices::kCityOuterDetail, v.city_outer_detail, direction);
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
    case Option::Taa:
        v.taa = !v.taa;
        break;
    case Option::Bloom:
        v.bloom = !v.bloom;
        break;
    case Option::DepthOfField:
        v.dof = !v.dof;
        break;
    case Option::AmbientOcclusion: {
        const uint32_t c = choices::Step(choices::kAoMethodChoices, choices::AoMethodChoice(v.ao, v.ao_method), direction);
        v.ao = choices::ApplyAoMethodChoice(c, v.ao_quality, v.ao_method);
        break;
    }
    case Option::AoQuality:
        v.ao_quality = choices::Step(choices::kAoQualities, v.ao_quality, direction);
        if (v.ao) v.ao = v.ao_quality;
        break;
    case Option::CityRange:
        v.city_range = choices::Step(choices::kCityRanges, v.city_range, direction);
        break;
    case Option::BuildingSeamFix:
        v.building_seam_fix = !v.building_seam_fix;
        break;
    case Option::PcContrast:
        v.pc_contrast = !v.pc_contrast;
        break;
    case Option::Gamma:
        v.gamma = choices::Step(choices::kGammaPercents, v.gamma, direction);
        break;
    case Option::Brightness:
        v.brightness = choices::Step(choices::kBrightnessPercents, v.brightness, direction);
        break;
    case Option::Contrast:
        v.contrast = choices::Step(choices::kContrastPercents, v.contrast, direction);
        break;
    case Option::Saturation:
        v.saturation = choices::Step(choices::kSaturationPercents, v.saturation, direction);
        break;
    case Option::VSync:
        v.vsync = !v.vsync;
        break;
    case Option::ShowFps:
        v.show_fps = !v.show_fps;
        break;
    case Option::TextureReplacement:
        v.custom_textures = !v.custom_textures;
        break;
    case Option::TextureDumping:
        v.dump_textures = !v.dump_textures;
        break;
    case Option::DumpFormat:
        v.texture_dump_format = direction > 0 ? 1u : 0u;
        break;
    case Option::FrameCap:
        v.fps_cap = choices::Step(choices::kFrameCaps, choices::EffectiveFrameCap(v.fps_cap), direction);
        break;
    case Option::Hdr:
        v.hdr = !v.hdr;
        break;
    case Option::HdrPaperWhite:
        v.hdr_paper_white = choices::Step(choices::kHdrPaperWhiteNits, v.hdr_paper_white, direction);
        break;
    case Option::HdrPeak:
        v.hdr_peak = choices::Step(choices::kHdrPeakNits, v.hdr_peak, direction);
        break;
    case Option::HdrHighlights:
        v.hdr_highlights = choices::Step(choices::kHdrHighlightPercents, v.hdr_highlights, direction);
        break;
    default:
        break;
    }
}

inline const char *OnOff(bool on) { return on ? "ON" : "OFF"; }

// upper case like the game's rows; its font has no '%'. Kept short to fit the
// tab's row boxes: " - A" marks a display change that A applies.
inline constexpr size_t kMaxTextChars = 19;
inline void Text(const Values &v, Option option, char (&text)[kTextBytes]) {
    const char *apply = DisplayPending(v) ? " - A" : "";
    switch (option) {
    case Option::DisplayMode: {
        constexpr const char *kNames[] = {"FULLSCREEN", "BORDERLESS", "WINDOWED"};
        std::snprintf(text, kTextBytes, "MODE %s%s", kNames[v.mode < 3 ? v.mode : 1], apply);
        break;
    }
    case Option::Resolution: {
        const uint32_t w = uint32_t(v.size >> 32), h = uint32_t(v.size);
        if (!v.size)
            std::snprintf(text, kTextBytes, "RES DESKTOP%s", apply);
        else
            std::snprintf(text, kTextBytes, "RES %uX%u%s", w, h, apply);
        break;
    }
    case Option::Aspect:
        if (!v.aspect)
            std::snprintf(text, kTextBytes, "ASPECT AUTO");
        else if (*choices::AspectName(v.aspect))
            std::snprintf(text, kTextBytes, "ASPECT %s", choices::AspectName(v.aspect));
        else
            std::snprintf(text, kTextBytes, "ASPECT %u.%02u", v.aspect / 1000, v.aspect % 1000 / 10);
        break;
    case Option::CutsceneBars:
        std::snprintf(text, kTextBytes, "CUTSCENE BARS %s", v.remove_cutscene_bars ? "HIDE" : "SHOW");
        break;
    case Option::RenderScale:
        std::snprintf(text, kTextBytes, "RENDER SCALE %u.%02uX", v.render_percent / 100, v.render_percent % 100);
        break;
    case Option::ShadowQuality: {
        constexpr const char *kNames[] = {"ORIGINAL", "HIGH", "ULTRA"};
        std::snprintf(text, kTextBytes, "SHADOWS %s", kNames[v.shadow_resolution < 3 ? v.shadow_resolution : 0]);
        break;
    }
    case Option::ShadowSoftness: {
        constexpr const char *kNames[] = {"OFF", "SOFT", "SOFTER"};
        std::snprintf(text, kTextBytes, "SOFT SHADOWS %s", kNames[v.shadow_softness < 3 ? v.shadow_softness : 0]);
        break;
    }
    case Option::BloomStrength:
        std::snprintf(text, kTextBytes, "BLOOM LEVEL %u", v.bloom_strength);
        break;
    case Option::Upscaler: {
        std::string label = "UPSCALER";
        switch (v.upscaler) {
        case 3: label += " DLSS"; break;
        case 4: label += " FSR4"; break;
        default: label += v.upscaler == 1 ? " FSR1" : v.upscaler == 2 ? " FSR3" : " BILINEAR"; break;
        }
        if (v.upscaler == 3 || v.upscaler == 2 || v.upscaler == 4) {
            const char* preset = v.render_percent == 50 ? "PERF" : v.render_percent == 58 ? "BAL"
                                  : v.render_percent == 67 ? "QUAL" : v.render_percent == 100 ? "NAT" : nullptr;
            if (!preset) {
                label = "UPSCALER SCALE";
            } else if (v.upscaler != 3 && v.fsr_state == kFsrNoDll) {
                label = "UPSCALER FSR3 NODLL";
            } else if (v.upscaler == 3 && v.dlss_state == kDlssNotBuilt) {
                label = "UPSCALER DLSS NONE";
            } else if (v.upscaler == 3 && v.dlss_state == kDlssUnavailable) {
                label = "UPSCALER DLSS N/A";
            } else if (v.upscaler == 4 && v.fsr_state == kFsrNo4) {
                label = std::string("UPSCALER FSR4* ") + preset;
            } else {
                label += ' ';
                label += preset;
            }
        }
        std::snprintf(text, kTextBytes, "%s", label.c_str());
        break;
    }
    case Option::Sharpening:
        if (v.sharpening)
            std::snprintf(text, kTextBytes, "SHARPENING %u", v.sharpening);
        else
            std::snprintf(text, kTextBytes, "SHARPENING OFF");
        break;
    case Option::LowLatency:
        std::snprintf(text, kTextBytes, "LOW LATENCY %s", OnOff(v.low_latency));
        break;
    case Option::FieldOfView:
        std::snprintf(text, kTextBytes, "FOV %u.%02uX", v.fov_percent / 100, v.fov_percent % 100);
        break;
    case Option::ObjectDetail: {
        constexpr const char *kNames[] = {"ORIGINAL", "HIGH", "VERY HIGH", "MAXIMUM"};
        std::snprintf(text, kTextBytes, "DETAIL %s", kNames[v.object_detail < 4 ? v.object_detail : 0]);
        break;
    }
    case Option::PedDetail: {
        constexpr const char *kNames[] = {"ORIG", "HIGH", "V HIGH", "MAX"};
        std::snprintf(text, kTextBytes, "PED DETAIL %s", kNames[v.ped_detail < 4 ? v.ped_detail : 0]);
        break;
    }
    case Option::PedLimit:
        std::snprintf(text, kTextBytes, "PED LIMIT %s",
                      v.ped_limit && v.ped_limit < std::size(choices::kPedLimits) ? choices::PedLimitName(v.ped_limit) : "15");
        break;
    case Option::PedDensity: {
        constexpr const char *kNames[] = {"ORIG", "1.5X", "2X", "3X EXP"};
        std::snprintf(text, kTextBytes, "CROWD %s", kNames[v.ped_density < 4 ? v.ped_density : 0]);
        break;
    }
    case Option::TrafficDensity: {
        constexpr const char *kNames[] = {"ORIG", "1.5X", "2X", "3X", "4X EXP", "5X EXP"};
        std::snprintf(text, kTextBytes, "TRAFFIC %s", kNames[v.traffic_density < 6 ? v.traffic_density : 0]);
        break;
    }
    case Option::TrafficDistance: {
        constexpr const char *kNames[] = {"ORIG", "1.5X", "2X"};
        std::snprintf(text, kTextBytes, "CAR RANGE %s", kNames[v.traffic_distance < 3 ? v.traffic_distance : 0]);
        break;
    }
    case Option::CityInnerDetail:
        std::snprintf(text, kTextBytes, "CITY INNER %uX", std::clamp(v.city_inner_detail, 1u, 3u));
        break;
    case Option::CityOuterDetail:
        if (v.city_outer_detail)
            std::snprintf(text, kTextBytes, "CITY OUTER %uX", std::min(v.city_outer_detail, 3u));
        else
            std::snprintf(text, kTextBytes, "CITY OUTER OFF");
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
        std::snprintf(text, kTextBytes, "SMAA %s", OnOff(v.smaa));
        break;
    case Option::Taa:
        std::snprintf(text, kTextBytes, "TAA %s", OnOff(v.taa));
        break;
    case Option::Bloom:
        std::snprintf(text, kTextBytes, "BLOOM %s", OnOff(v.bloom));
        break;
    case Option::DepthOfField:
        std::snprintf(text, kTextBytes, "DEPTH OF FIELD %s", OnOff(v.dof));
        break;
    case Option::AmbientOcclusion: {
        constexpr const char *kNames[] = {"OFF", "SSAO", "GTAO"};
        std::snprintf(text, kTextBytes, "OCCLUSION %s", kNames[choices::AoMethodChoice(v.ao, v.ao_method)]);
        break;
    }
    case Option::AoQuality:
        std::snprintf(text, kTextBytes, "AO QUALITY %s", v.ao_quality == 1 ? "HALF" : "FULL");
        break;
    case Option::CityRange: {  // experimental outer city ring
        constexpr const char *kNames[] = {"NORMAL", "5X5", "7X7", "9X9"};
        std::snprintf(text, kTextBytes, "CITY RANGE %s", kNames[v.city_range < 4 ? v.city_range : 0]);
        break;
    }
    case Option::BuildingSeamFix:
        std::snprintf(text, kTextBytes, "SEAM FIX %s", OnOff(v.building_seam_fix));
        break;
    case Option::PcContrast:
        std::snprintf(text, kTextBytes, "PC CONTRAST %s", OnOff(v.pc_contrast));
        break;
    case Option::Gamma:
        std::snprintf(text, kTextBytes, "GAMMA %u", v.gamma);
        break;
    case Option::Brightness:
        std::snprintf(text, kTextBytes, "BRIGHTNESS %u", v.brightness);
        break;
    case Option::Contrast:
        std::snprintf(text, kTextBytes, "CONTRAST %u", v.contrast);
        break;
    case Option::Saturation:
        std::snprintf(text, kTextBytes, "SATURATION %u", v.saturation);
        break;
    case Option::VSync:
        std::snprintf(text, kTextBytes, "V-SYNC %s", OnOff(v.vsync));
        break;
    case Option::ShowFps:
        std::snprintf(text, kTextBytes, "SHOW FPS %s", OnOff(v.show_fps));
        break;
    case Option::TextureReplacement:
        std::snprintf(text, kTextBytes, "TEXTURE MODS %s", OnOff(v.custom_textures));
        break;
    case Option::TextureDumping:
        std::snprintf(text, kTextBytes, "DUMP TEXTURES %s", OnOff(v.dump_textures));
        break;
    case Option::DumpFormat:
        std::snprintf(text, kTextBytes, "DUMP FORMAT %s", v.texture_dump_format ? "DDS" : "PNG");
        break;
    case Option::Defaults:
        std::snprintf(text, kTextBytes, "RESTORE DEFAULTS");
        break;
    case Option::FrameCap:
        std::snprintf(text, kTextBytes, "FRAME LIMIT %u", choices::EffectiveFrameCap(v.fps_cap));
        break;
    case Option::HdrPage:
        std::snprintf(text, kTextBytes, "HDR SETTINGS");
        break;
    case Option::Hdr:
        std::snprintf(text, kTextBytes, "HDR %s", OnOff(v.hdr));
        break;
    case Option::HdrPaperWhite:
        std::snprintf(text, kTextBytes, "PAPER WHITE %u", v.hdr_paper_white);
        break;
    case Option::HdrPeak:
        if (v.hdr_peak)
            std::snprintf(text, kTextBytes, "PEAK %u NITS", v.hdr_peak);
        else
            std::snprintf(text, kTextBytes, "PEAK AUTO %u", v.display_peak ? v.display_peak : 1000);
        break;
    case Option::HdrHighlights:
        std::snprintf(text, kTextBytes, "HIGHLIGHTS %u", v.hdr_highlights);
        break;
    case Option::HdrBack:
        std::snprintf(text, kTextBytes, "BACK");
        break;
    case Option::ExitConfirm:
        std::snprintf(text, kTextBytes, "EXIT GAME");
        break;
    case Option::ExitPrompt:
        std::snprintf(text, kTextBytes, "EXIT GAME?");
        break;
    case Option::ExitCancel:
        std::snprintf(text, kTextBytes, "CANCEL");
        break;
    default:
        text[0] = 0;
        break;
    }
}

} // namespace game::display_rows
