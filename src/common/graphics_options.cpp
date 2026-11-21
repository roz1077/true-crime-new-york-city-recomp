// The in-game Graphics menu: F10, or Back + Start on controller 1.
//
// Rows are driven by the keyboard (arrows, Enter, Esc), controller 1 (D-pad or
// left stick, A, B) or the mouse. Controller input reaches the menu through
// GameInfo::pad, which polls SDL, so every controller type works; the game gets
// a pad with nothing pressed while the menu is open. Changes apply at once,
// except display mode and resolution, which wait for Apply. The settings file
// is written when the menu closes.

#include "graphics_menu.h"
#include "core/dev_env.h"

#include "app/app.h"
#include "game/city_grid.h"
#include "controller_look.h"
#include "core/log.h"
#include "graphics_choices.h"
#include "keycode_map.h"
#include "gpu/backend.h"
#include "menu_input.h"

#include <imgui.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

namespace app {
// main.cpp (app_sdl.cpp on Linux): the window thread applies the requested mode, writes it to the settings if it worked
// and reports the result, so the frame thread does not hold UiLock across a SendMessage.
uint64_t RequestDisplayApply(DisplayMode mode, uint32_t width, uint32_t height);
bool DisplayApplyResult(uint64_t request, bool &ok);
} // namespace app

namespace game {

bool g_show_fps = false;
uint32_t g_fov_percent = 100;
std::atomic<float> g_camera_fov_degrees{0};

bool ShowFps() { return std::atomic_ref(g_show_fps).load(std::memory_order_relaxed); }

void SetShowFps(bool show) { std::atomic_ref(g_show_fps).store(show, std::memory_order_relaxed); }

namespace {

using namespace menu_input;

uint64_t NowMs() {
#ifdef _WIN32
    return GetTickCount64();
#else
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
#endif
}

// shared between the pad thread and the UI thread
std::atomic<bool> g_open{false};
std::atomic<bool> g_toggle{false};
std::atomic<uint64_t> g_pad_nav{0};   // nav bits << 32 | NowMs of the poll
constexpr uint32_t kPadStaleMs = 250; // a game that stopped polling leaves no held direction behind

enum class Row {
    DisplayMode,
    Resolution,
    ApplyDisplay,
    AspectRatio,
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
    CityRange,
    CityInnerDetail,
    CityOuterDetail,
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
    Hdr,
    HdrPaperWhite,
    HdrPeak,
    HdrHighlights,
    VSync,
    LowLatency,
    FrameCap,
    ShowFps,
    TextureReplacement,
    TextureDumping,
    DumpFormat,
    ControllerAimResponse,
    ControllerAimSensitivity,
    MouseSensitivity,
    MouseVerticalSensitivity,
    Controls,
    Defaults,
    Close,
};
constexpr Row kRows[] = {Row::DisplayMode,
                         Row::Resolution,
                         Row::ApplyDisplay,
                         Row::AspectRatio,
                         Row::CutsceneBars,
                         Row::RenderScale,
                         Row::Upscaler,
                         Row::Sharpening,
                         Row::FieldOfView,
                         Row::ObjectDetail,
                         Row::PedDetail,
                         Row::PedLimit,
                         Row::PedDensity,
                         Row::TrafficDensity,
                         Row::TrafficDistance,
                         // Row::CityRange: hidden; the ring code stays, the setting stays at the game's 3 x 3.
                         Row::CityInnerDetail,
                         Row::CityOuterDetail,
                         Row::Anisotropic,
                         Row::Msaa,
                         Row::Smaa,
                         Row::Taa,
                         Row::Bloom,
                         Row::BloomStrength,
                         Row::DepthOfField,
                         Row::AmbientOcclusion,
                         Row::AoQuality,
                         Row::ShadowQuality,
                         Row::ShadowSoftness,
                         Row::BuildingSeamFix,
                         Row::PcContrast,
                         Row::Gamma,
                         Row::Brightness,
                         Row::Contrast,
                         Row::Saturation,
                         Row::Hdr,
                         Row::HdrPaperWhite,
                         Row::HdrPeak,
                         Row::HdrHighlights,
                         Row::VSync,
                         Row::LowLatency,
                         Row::FrameCap,
                         Row::ShowFps,
                         Row::TextureReplacement,
                         Row::TextureDumping,
                         Row::DumpFormat,
                         Row::ControllerAimResponse,
                         Row::ControllerAimSensitivity,
                         Row::MouseSensitivity,
                         Row::MouseVerticalSensitivity,
                         Row::Controls,
                         Row::Defaults,
                         Row::Close};
constexpr int kRowCount = int(std::size(kRows));

enum class Kind { Choice, Toggle, Action };

// UI thread only, under UiLock
struct Menu {
    bool open = false;
    int selected = 0;
    bool dirty = false; // settings changed since the menu opened or last saved
    bool apply_pending = false; // the window thread has not reported the display apply yet
    bool save_failed = false;
    bool apply_failed = false;  // the last display apply was refused
    uint64_t apply_seq = 0;
    app::DisplayMode mode{};
    app::Mode resolution{};
    std::vector<app::Mode> resolutions; // index 0 is Desktop (0 x 0)
    app::Mode desktop{};
    uint32_t output_height = 0;
    Repeater repeater;
    bool controls = false;  // the Controls page is showing
} g_menu;

// The Controls page: one row per keyboard action and controller input, then the resets and Back.
struct ControlsPage {
    input::KeyBinding keys[input::kKeyBindingCount];
    input::PadBinding pad[input::kPadBindingCount];
    int selected = 0;
    int capture = -1;          // the row waiting for a key or button
    bool pad_released = false; // a controller capture waits for every button to be let go first
    int first_frame = 0;       // the frame the capture started: its own Enter/click is not the answer
} g_controls;

constexpr int kKeyRows = input::kKeyBindingCount - 1;  // Start stays on Escape
constexpr int kPadRows = input::kPadBindingCount;
constexpr int kControlRows = kKeyRows + kPadRows + 3;  // + Reset keyboard, Reset controller, Back
constexpr const char *kKeyActions[kKeyRows] = {
    "Move forward", "Move back", "Move left", "Move right",
    "Light attack", "Grapple / human shield", "Heavy attack", "Jump / climb",
    "Car: accelerate", "Car: handbrake", "Car: brake / reverse", "Car: Y action",
    "Reload / block", "Use / enter vehicle", "Aim / lock on", "Fire",
    "Badge / horn / siren", "Left stick click", "Right stick click"};
constexpr const char *kPadActions[kPadRows] = {
    "D-pad up", "D-pad down", "D-pad left", "D-pad right", "Pause", "Badge / horn / siren",
    "Left stick click", "Right stick click", "A: light attack / accelerate", "B: grapple / handbrake",
    "X: heavy attack / brake", "Y: jump / climb", "Black: reload / block", "White: use / enter vehicle",
    "Left trigger: aim", "Right trigger: fire"};

void OpenControls() {
    input::KeyBindings(g_controls.keys);
    input::PadBindings(g_controls.pad);
    g_controls.selected = 0;
    g_controls.capture = -1;
    g_menu.controls = true;
}

void CommitControls() {
    input::SetBindings(g_controls.keys, g_controls.pad);
    g_menu.dirty = true;  // saved with the other settings when the menu closes
}

// ImGui's keys as SDL keycodes (input::KeyBinding), for the ones a binding can use
uint32_t KeycodeOf(ImGuiKey key) { return common::keys::SdlKeycode(int(key)); }

// a capture in progress: true while it owns the input
bool CaptureControls() {
    auto &c = g_controls;
    if (c.capture < 0) return false;
    if (ImGui::GetFrameCount() == c.first_frame) return true;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        c.capture = -1;
        return true;
    }
    if (c.capture < kKeyRows) {
        uint32_t key = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
            std::fill(std::begin(c.keys[c.capture].keys), std::end(c.keys[c.capture].keys), 0u);
            c.capture = -1;
            CommitControls();
            return true;
        }
        for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END && !key; ++k)
            if (ImGui::IsKeyPressed(ImGuiKey(k), false)) key = KeycodeOf(ImGuiKey(k));
        for (int b = 0; b < 5 && !key; ++b)
            if (ImGui::IsMouseClicked(b)) key = input::MouseKey(b == 1 ? 2 : b == 2 ? 1 : b);  // ImGui: 1 right, 2 middle
        if (!input::CanBindKey(key)) return true;
        // the new key becomes the main one (the one the prompts show); the others stay
        auto &slots = c.keys[c.capture].keys;
        const auto end = std::remove(std::begin(slots), std::end(slots), key);
        std::fill(end, std::end(slots), 0u);
        std::move_backward(std::begin(slots), std::end(slots) - 1, std::end(slots));
        slots[0] = key;
    } else {
        const int source = input::HeldPadSource();
        if (!c.pad_released) {
            c.pad_released = source < 0;
            return true;
        }
        if (source < 0) return true;
        auto &target = c.pad[c.capture - kKeyRows];
        // a swap: the input that had this button takes the old one, so nothing is left unbound
        for (auto &binding : c.pad)
            if (binding.source == source) binding.source = target.source;
        target.source = source;
    }
    c.capture = -1;
    CommitControls();
    return true;
}

void ActivateControl(int row) {
    auto &c = g_controls;
    if (row < kKeyRows + kPadRows) {
        if (row == kKeyRows + int(input::PadInput::Start)) return;  // Pause stays on Start
        c.capture = row;
        c.pad_released = false;
        c.first_frame = ImGui::GetFrameCount();
    } else if (row == kKeyRows + kPadRows) {
        input::DefaultKeys(c.keys);
        CommitControls();
    } else if (row == kKeyRows + kPadRows + 1) {
        input::DefaultPadBindings(c.pad);
        CommitControls();
    } else {
        g_menu.controls = false;
    }
}

std::string ControlValue(int row) {
    char name[32];
    if (row < kKeyRows) {
        std::string value;
        for (uint32_t key : g_controls.keys[row].keys) {
            if (!key) continue;
            input::KeyDisplayName(key, name);
            value += (value.empty() ? "" : ", ") + std::string(name);
        }
        return value.empty() ? "-" : value;
    }
    if (row < kKeyRows + kPadRows) {
        input::PadSourceDisplayName(g_controls.pad[row - kKeyRows].source, name);
        return name;
    }
    return "";
}

bool SameMode(app::Mode a, app::Mode b) { return a.width == b.width && a.height == b.height; }

} // namespace

std::vector<app::Mode> DisplayModes() {
    std::vector<app::Mode> modes{{0, 0}};
#ifdef _WIN32
    MONITORINFOEXW monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfoW(MonitorFromWindow(static_cast<HWND>(app::Window()), MONITOR_DEFAULTTOPRIMARY), &monitor))
        return modes;
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    for (DWORD i = 0; EnumDisplaySettingsW(monitor.szDevice, i, &dm); ++i) {
        const app::Mode m{dm.dmPelsWidth, dm.dmPelsHeight};
        if (m.width < 640 || m.height < 480) continue;
        if (std::ranges::none_of(modes, [&](app::Mode e) { return SameMode(e, m); })) modes.push_back(m);
    }
#else
    for (app::Mode m : app::DisplayModes()) {
        if (m.width < 640 || m.height < 480) continue;
        if (std::ranges::none_of(modes, [&](app::Mode e) { return SameMode(e, m); })) modes.push_back(m);
    }
#endif
    std::sort(modes.begin() + 1, modes.end(),
              [](app::Mode a, app::Mode b) { return std::pair{a.width, a.height} < std::pair{b.width, b.height}; });
    return modes;
}

namespace {

bool DisplayPending() {
    // the settings hold the applied mode; they change only once the window thread confirms
    app::SettingsLock lock;
    const app::Settings &s = app::Get();
    return g_menu.mode != s.display_mode || !SameMode(g_menu.resolution, {s.width, s.height});
}

void OpenMenu() {
    app::SettingsLock lock;
    const app::Settings &s = app::Get();
    g_menu.open = true;
    if (!g_menu.apply_pending) g_menu.apply_failed = false;
    g_menu.mode = s.display_mode;
    g_menu.resolution = {s.width, s.height};
    g_menu.resolutions = DisplayModes();
    if (std::ranges::none_of(g_menu.resolutions, [](app::Mode m) { return SameMode(m, g_menu.resolution); }))
        g_menu.resolutions.push_back(g_menu.resolution); // a size the monitor no longer lists stays selectable
    g_menu.desktop = app::Desktop();
    g_menu.output_height = app::Output().height;
    g_menu.repeater = {};
    g_menu.controls = false;
    g_pad_nav.store(0); // drop any keyboard-derived bits from before the overlay owned the keys
    g_open.store(true);
    input::SetUiActive(true);
    app::CaptureMouse(false); // the menu needs the pointer
}

void CloseMenu() {
    if (g_menu.apply_pending) return; // keep the menu up until the mode apply is answered
    if (g_menu.dirty) {
        const bool already_notified = g_menu.save_failed;
        g_menu.dirty = !app::Save();
        g_menu.save_failed = g_menu.dirty;
        if (g_menu.save_failed && !already_notified) return;
    }
    g_menu.open = false;
    g_open.store(false);
    input::SetUiActive(false);
    app::CaptureMouse(true);
}

// WM_CLOSE/WM_DESTROY bypass CloseMenu. Save live changes; the settings only ever hold a display
// mode the window thread applied, so one still in flight is simply not saved.
#ifdef _WIN32
void SaveOnExit() {
    // The window handler runs outside UiLock; the menu state is shared with
    // the frame that draws it.
    std::lock_guard lock(app::UiLock());
    if (!g_menu.dirty && !g_menu.apply_pending) return;
    static bool saved = false;
    if (saved) return;
    saved = true;
    g_menu.apply_pending = false;
    g_menu.dirty = !app::Save();
}
#endif

void ApplyDisplayChanges() {
    if (g_menu.apply_pending) return;
    // the window thread writes the settings once the change works; the file is saved after that
    // (PollDisplayApply)
    const uint64_t request = app::RequestDisplayApply(g_menu.mode, g_menu.resolution.width, g_menu.resolution.height);
    if (!request) {
        g_menu.apply_failed = true;
        LOGE("Graphics: could not request a display apply for mode %u, %ux%u", uint32_t(g_menu.mode),
             g_menu.resolution.width, g_menu.resolution.height);
        return;
    }
    g_menu.apply_pending = true;
    g_menu.apply_seq = request;
    g_menu.apply_failed = false;
}

// Runs on the frame that owns the menu, after the window thread has answered.
void PollDisplayApply() {
    if (!g_menu.apply_pending) return;
    bool ok = false;
    if (!app::DisplayApplyResult(g_menu.apply_seq, ok)) return;
    g_menu.apply_pending = false;
    if (ok) {
        g_menu.save_failed = !app::Save();
        g_menu.dirty = g_menu.save_failed;
        g_menu.output_height = app::Output().height;
        g_menu.apply_failed = false;
        return;
    }
    // the window thread already put the previous mode back; the settings never changed
    g_menu.output_height = app::Output().height;
    g_menu.apply_failed = true;
    LOGE("Graphics: display mode %u at %ux%u was refused; kept the previous settings", uint32_t(g_menu.mode),
         g_menu.resolution.width, g_menu.resolution.height);
}

} // namespace

void RestoreGraphicsDefaults() {
    app::SettingsLock lock;
    const app::Settings d;
    app::Settings &s = app::Get();
    s.render_percent = d.render_percent;
    s.aspect = d.aspect;
    s.remove_cutscene_bars = d.remove_cutscene_bars;
    app::SetFrameCap(d.fps_cap);
    s.vsync = d.vsync;
    s.low_latency = d.low_latency;
    s.taa = d.taa;
    s.upscaler = d.upscaler;
    s.sharpening = d.sharpening;
    s.aniso = d.aniso;
    app::SetMsaaSamples(d.msaa);
    s.smaa = d.smaa;
    s.bloom = d.bloom;
    s.bloom_strength = d.bloom_strength;
    s.shadow_resolution = d.shadow_resolution;
    s.shadow_softness = d.shadow_softness;
    s.dof = d.dof;
    s.ao = d.ao;
    s.ao_method = d.ao_method;
    s.ao_quality = d.ao_quality;
    s.building_seam_fix = d.building_seam_fix;
    std::atomic_ref(s.object_detail).store(d.object_detail, std::memory_order_relaxed);  // read by lod.cpp
    std::atomic_ref(s.ped_detail).store(d.ped_detail, std::memory_order_relaxed);  // read by ped_detail.cpp
    std::atomic_ref(s.ped_limit).store(d.ped_limit, std::memory_order_relaxed);
    std::atomic_ref(s.ped_density).store(d.ped_density, std::memory_order_relaxed);
    std::atomic_ref(s.traffic_density).store(d.traffic_density, std::memory_order_relaxed);
    std::atomic_ref(s.traffic_distance).store(d.traffic_distance, std::memory_order_relaxed);
    std::atomic_ref(s.city_range).store(d.city_range, std::memory_order_relaxed);
    std::atomic_ref(s.city_inner_detail).store(d.city_inner_detail, std::memory_order_relaxed);
    std::atomic_ref(s.city_outer_detail).store(d.city_outer_detail, std::memory_order_relaxed);
    s.pc_contrast = d.pc_contrast;
    s.gamma = d.gamma;
    s.brightness = d.brightness;
    s.contrast = d.contrast;
    s.saturation = d.saturation;
    s.custom_textures = d.custom_textures;
    s.dump_textures = d.dump_textures;
    s.texture_dump_format = d.texture_dump_format;
    s.hdr = d.hdr;
    s.hdr_paper_white = d.hdr_paper_white;
    s.hdr_peak = d.hdr_peak;
    s.hdr_highlights = d.hdr_highlights;
    SetShowFps(false);
    g_fov_percent = 100;
}

namespace {

void RestoreDefaults() {
    RestoreGraphicsDefaults();
    std::atomic_ref(input::g_controller_aim_response).store(0, std::memory_order_relaxed);
    std::atomic_ref(input::g_controller_aim_sensitivity_percent).store(100, std::memory_order_relaxed);
    std::atomic_ref(input::g_mouse_sensitivity_percent).store(100, std::memory_order_relaxed);
    std::atomic_ref(input::g_mouse_vertical_sensitivity_percent).store(100, std::memory_order_relaxed);
    const app::Settings d;
    g_menu.mode = d.display_mode; // display changes still wait for Apply
    g_menu.resolution = {d.width, d.height};
    g_menu.dirty = true;
}

Kind KindOf(Row row) {
    switch (row) {
    case Row::DisplayMode:
    case Row::Resolution:
    case Row::AspectRatio:
    case Row::RenderScale:
    case Row::ShadowQuality:
    case Row::ShadowSoftness:
    case Row::BloomStrength:
    case Row::Upscaler:
    case Row::Sharpening:
    case Row::FieldOfView:
    case Row::ObjectDetail:
    case Row::PedDetail:
    case Row::PedLimit:
    case Row::PedDensity:
    case Row::TrafficDensity:
    case Row::TrafficDistance:
    case Row::CityRange:
    case Row::CityInnerDetail:
    case Row::CityOuterDetail:
    case Row::Anisotropic:
    case Row::FrameCap:
    case Row::DumpFormat:
    case Row::Msaa:
    case Row::AmbientOcclusion:
    case Row::AoQuality:
    case Row::HdrPaperWhite:
    case Row::HdrPeak:
    case Row::HdrHighlights:
    case Row::Gamma:
    case Row::Brightness:
    case Row::Contrast:
    case Row::Saturation:
    case Row::ControllerAimResponse:
    case Row::ControllerAimSensitivity:
    case Row::MouseSensitivity:
    case Row::MouseVerticalSensitivity:
        return Kind::Choice;
    case Row::Smaa:
    case Row::Taa:
    case Row::Bloom:
    case Row::DepthOfField:
    case Row::BuildingSeamFix:
    case Row::PcContrast:
    case Row::Hdr:
    case Row::VSync:
    case Row::LowLatency:
    case Row::ShowFps:
    case Row::TextureReplacement:
    case Row::TextureDumping:
    case Row::CutsceneBars:
        return Kind::Toggle;
    default:
        return Kind::Action;
    }
}

bool Enabled(Row row) {
    app::SettingsLock lock;  // the native Display tab can write these from the game thread
    if (row == Row::ApplyDisplay) return !g_menu.apply_pending && DisplayPending();
    if (row == Row::DumpFormat) return app::Get().dump_textures;
    if (row == Row::Upscaler) return app::Get().render_percent <= 100;
    if (row == Row::BloomStrength) return app::Get().bloom;
    if (row == Row::ControllerAimSensitivity)
        return std::atomic_ref(input::g_controller_aim_response).load(std::memory_order_relaxed) == 0;
    if (row == Row::CityOuterDetail) return game::city_grid::OuterDetailAvailable();
#ifndef _WIN32
    if (row == Row::LowLatency) return false;  // Direct3D 12 only
#endif
    // The HDR toggle stays reachable on an SDR display so a failed detection can be retried and
    // the fallback is visible; its sliders only matter once HDR is actually active.
    if (row == Row::HdrPaperWhite || row == Row::HdrPeak || row == Row::HdrHighlights)
        return app::Get().hdr && gpu::g_hdr_available.load(std::memory_order_relaxed);
    return true;
}

const char *Label(Row row) {
    switch (row) {
    case Row::DisplayMode:
        return "Display mode";
    case Row::Resolution:
        return "Resolution";
    case Row::ApplyDisplay:
        return "Apply display changes";
    case Row::AspectRatio:
        return "Aspect ratio";
    case Row::CutsceneBars:
        return "Remove cutscene bars";
    case Row::RenderScale:
        return "Render scale";
    case Row::FieldOfView:
        return "Field of view";
    case Row::ObjectDetail:
        return "Object detail";
    case Row::PedDetail:
        return "Pedestrian detail";
    case Row::PedLimit:
        return "Pedestrian limit";
    case Row::PedDensity:
        return "Crowd density";
    case Row::TrafficDensity:
        return "Traffic density";
    case Row::TrafficDistance:
        return "Traffic distance";
    case Row::CityRange:
        return "City draw distance";
    case Row::CityInnerDetail:
        return "Inner city detail distance";
    case Row::CityOuterDetail:
        return "Outer city detail distance";
    case Row::Anisotropic:
        return "Anisotropic filtering";
    case Row::Msaa:
        return "MSAA";
    case Row::Taa:
        return "TAA";
    case Row::Smaa:
        return "SMAA";
    case Row::Bloom:
        return "Bloom";
    case Row::DepthOfField:
        return "Depth of field";
    case Row::AmbientOcclusion:
        return "Ambient occlusion";
    case Row::AoQuality:
        return "Occlusion quality";
    case Row::BuildingSeamFix:
        return "Building seam fix (Experimental)";
    case Row::PcContrast:
        return "PC contrast";
    case Row::Gamma:
        return "Gamma";
    case Row::Brightness:
        return "Brightness";
    case Row::Contrast:
        return "Contrast";
    case Row::Saturation:
        return "Saturation";
    case Row::Hdr:
        return "HDR";
    case Row::HdrPaperWhite:
        return "HDR paper white";
    case Row::HdrPeak:
        return "HDR peak brightness";
    case Row::HdrHighlights:
        return "HDR highlights";
    case Row::ShadowQuality:
        return "Shadow quality";
    case Row::ShadowSoftness:
        return "Shadow softness";
    case Row::BloomStrength:
        return "Bloom strength";
    case Row::Upscaler:
        return "Upscaler";
    case Row::Sharpening:
        return "Sharpening";
    case Row::LowLatency:
        return "Low latency";
    case Row::VSync:
        return "V-Sync";
    case Row::FrameCap:
        return "Frame rate limit";
    case Row::ShowFps:
        return "FPS counter";
    case Row::TextureReplacement:
        return "Texture replacement";
    case Row::TextureDumping:
        return "Texture dumping";
    case Row::DumpFormat:
        return "Dump format";
    case Row::ControllerAimResponse:
        return "Controller aim response";
    case Row::ControllerAimSensitivity:
        return "Controller aim sensitivity";
    case Row::MouseSensitivity:
        return "Mouse sensitivity";
    case Row::MouseVerticalSensitivity:
        return "Mouse vertical sensitivity";
    case Row::Controls:
        return "Controls...";
    case Row::Defaults:
        return "Restore defaults";
    case Row::Close:
        return "Close";
    }
    return "";
}

// a heading drawn above the row; "" draws a plain separator
const char *Section(Row row) {
    switch (row) {
    case Row::DisplayMode:
        return "Display";
    case Row::RenderScale:
        return "Rendering";
    case Row::Hdr:
        return "HDR output";
    case Row::TextureReplacement:
        return "Textures";
    case Row::ControllerAimResponse:
        return "Controller aim";
    case Row::MouseSensitivity:
        return "Mouse aim";
    case Row::Controls:
        return "CONTROLS";
    case Row::Defaults:
        return "";
    default:
        return nullptr;
    }
}

const char *Help(Row row) {
    switch (row) {
    case Row::DisplayMode:
        return "Borderless fills the desktop. Fullscreen switches the monitor to the chosen resolution. "
               "Takes effect with Apply display changes.";
    case Row::Resolution:
        return "Output resolution. In Borderless the window always fills the desktop and this sets the base "
               "for render scale. Takes effect with Apply display changes.";
    case Row::ApplyDisplay:
        return "Applies the display mode and resolution above.";
    case Row::AspectRatio:
        return "The picture's shape; Auto matches the display. The 3D view widens to it, while HUD and menus "
               "stay 16:9 in the middle and the intro movies stretch to fill it. 4:3 and the wide shapes switch after a restart.";
    case Row::CutsceneBars:
        return "Hides the black bars above and below cutscenes and the title screen, showing the whole 3D picture. "
               "Off keeps the game's cinematic framing.";
    case Row::RenderScale:
        return "Internal resolution relative to the output height. Above 100% renders more pixels for a "
               "sharper image; below 100% is faster.";
    case Row::FieldOfView:
        return "Widens or narrows the gameplay camera. 100% is the game's own view; aiming still zooms "
               "relative to it. Menus and cutscene previews are unchanged.";
    case Row::PedDetail:
        return "How close a distant low-detail pedestrian must get before the game swaps in its full "
               "model. Original is the game's own; higher levels swap them in farther away; Maximum also "
               "loads characters from their single full-detail files, so they no longer step through a middle "
               "level. Applies to characters loaded after the change. The game still "
               "keeps at most Pedestrian limit full pedestrians at once.";
    case Row::CityRange:
        return "Experimental: loads and draws real city tiles beyond the game's own 3 x 3 around the player. Normal is "
               "the game's; 5 x 5, 7 x 7 and 9 x 9 show more distant streets and cost memory and performance. "
               "The city detail controls set the range of detailed models across this grid.";
    case Row::CityInnerDetail:
        return "Changes how far detailed buildings and props appear in the central tiles. On an expanded city grid, this covers 5 x 5.";
    case Row::CityOuterDetail:
        return "Draws detailed buildings and props beyond the inner 5 x 5 when the experimental city ring is active.";
    case Row::TrafficDistance:
        return "How far away ambient cars appear and stay. Original, the game's own, is the default. "
               "This spreads the same cars over a wider area, so higher distances look emptier nearby "
               "unless Traffic density is raised too.";
    case Row::TrafficDensity:
        return "How many cars drive around. Original is the game's own; higher levels allow more ambient "
               "traffic in each area. Traffic distance spreads these cars further out, so raise both together "
               "to keep streets busy. Experimental: busier roads cost performance.";
    case Row::PedDensity:
        return "How many pedestrians fill the streets. Original is the game's own; higher levels spawn more "
               "people at each spawn point. 3x also raises the game's crowd budget and is experimental. Busier "
               "streets cost performance, and Pedestrian limit decides how many of them get full detail.";
    case Row::PedLimit:
        return "Experimental. How many full-detail pedestrians the game may keep at once. 15 is the game's "
               "own; more lets Pedestrian detail upgrade more of a crowd but may stutter or fail in busy streets.";
    case Row::ObjectDetail:
        return "How far buildings and props keep their detailed models before switching to "
               "simpler ones. Original is the game's own; higher levels look better at a distance but cost "
               "performance. Maximum draws every object at full detail at any distance and is much slower "
               "in busy streets. Moving cars follow it too; parked cars keep the game's own detail, and distant pedestrians "
               "follow Pedestrian detail.";
    case Row::Anisotropic:
        return "Keeps textures sharp at glancing angles, such as roads into the distance. Game default uses "
               "the filtering the game asks for.";
    case Row::Msaa:
        return "Smooths geometry edges. Higher counts require more GPU memory.";
    case Row::Taa:
        return "Temporal anti-aliasing in place of SMAA on the 3D scene. Smooths edges and shimmer over several "
               "frames and needs no extra DLLs. The FSR and DLSS upscalers replace it when selected.";
    case Row::Smaa:
        return "Smooths jagged edges on the finished frame (SMAA 1x). The HUD is smoothed with it.";
    case Row::Bloom:
        return "Adds extra bright-pass bloom over the game's own lighting.";
    case Row::DepthOfField:
        return "Adds distance blur to the finished 3D frame.";
    case Row::AmbientOcclusion:
        return "Depth-based ambient occlusion. SSAO is a little cheaper but grainier; GTAO is the higher quality. "
               "Set the resolution with Occlusion quality.";
    case Row::AoQuality:
        return "Resolution of the ambient occlusion. Half is cheaper and softer; Full is sharper. Kept while occlusion is off.";
    case Row::BuildingSeamFix:
        return "Experimental: keeps detailed buildings visible where simplified city meshes overlap. Takes effect immediately.";
    case Row::PcContrast:
        return "Applies a PC-inspired contrast curve to the finished 3D frame.";
    case Row::Gamma:
        return "Adjusts midtones without moving black or white. 100% is unchanged; raise it if dark areas "
               "are hard to see.";
    case Row::Brightness:
        return "Scales the whole picture. 100% is unchanged; black stays black, so letterbox bars and "
               "fades are not lifted.";
    case Row::Contrast:
        return "Expands or compresses tones around mid-grey. 100% is unchanged and separate from PC contrast.";
    case Row::Saturation:
        return "Colour strength. 0% is greyscale, 100% is unchanged and 200% doubles colour.";
    case Row::Hdr:
        return gpu::g_hdr_available.load(std::memory_order_relaxed)
                   ? "Expands the finished frame's highlights for an HDR display. Off is bit-identical to SDR output."
                   : "The display does not report HDR; turning this on keeps SDR output and logs the reason.";
    case Row::HdrPaperWhite:
        return "Brightness of ordinary SDR white on the HDR display, in nits.";
    case Row::HdrPeak:
        return "Peak highlight brightness. Display uses the panel's reported peak; otherwise 1000 nits.";
    case Row::HdrHighlights:
        return "How far bright neon, lamps and headlights are pushed above paper white.";
    case Row::ShadowQuality:
        return "Resolution of the shadow image. Original follows Render scale; High and Ultra draw it at 1024 and 2048 "
               "pixels a side.";
    case Row::ShadowSoftness:
        return "Filters shadow edges. Off is the game's hard edge; Soft and Softer blur it over more of the shadow map.";
    case Row::BloomStrength:
        return "How bright the bloom glow is. 100% is the original, very subtle look.";
    case Row::Upscaler:
#ifdef _WIN32
        return "FSR 1 sharpens when scaling up. Experimental FSR 3.1, FSR 4.1 and DLSS run at Render scale "
               "50% (Performance), 58% (Balanced), 67% (Quality) or 100% (Native AA). FSR 4.1 needs AMD's SDK 2.x loader and upscaler DLLs beside the "
               "game; it uses FSR 3.1 when the driver exposes only 3.1. DLSS is experimental and needs an "
               "NVIDIA RTX adapter and NVIDIA's nvngx_dlss.dll beside the game.";
#elif defined(TCNYC_HAVE_DLSS)
        return "FSR 1 keeps edges sharper than Bilinear below 100% Render scale. FSR 3.1 and experimental DLSS "
               "use 50% (Performance), 58% (Balanced), 67% (Quality) or 100% (Native AA). "
               "DLSS requires an NVIDIA RTX adapter and the NGX runtime.";
#else
        return "How the picture is scaled up. AMD FSR 1 keeps edges sharper than Bilinear below 100% Render scale. "
               "Experimental FSR 3.1 runs at Render scale 50% (Performance), 58% (Balanced), 67% (Quality) or 100% "
               "(Native AA).";
#endif
    case Row::Sharpening:
        return "Sharpens the finished 3D frame (AMD FidelityFX RCAS). Offsets the softness of SMAA and of render "
               "scales below 100%. The HUD is not sharpened.";
    case Row::LowLatency:
#ifdef _WIN32
        return "Lets only one frame wait for the display instead of three, so the controls respond sooner. "
               "Can lower the frame rate on a slow GPU.";
#else
        return "Windows only.";
#endif
    case Row::VSync:
        return "Waits for the monitor's refresh before showing a frame, which prevents tearing.";
    case Row::FrameCap:
        return "Sets the frame limit for gameplay and real-time cutscenes.";
    case Row::ShowFps:
        return "Shows the frame rate in the top-left corner.";
    case Row::TextureReplacement:
        return "Uses images in the textures/replacement folder beside the executable in place of the game's textures.";
    case Row::TextureDumping:
        return "Saves each texture the game uses into the textures/dumps folder beside the executable, once. Slows "
               "loading while on.";
    case Row::DumpFormat:
        return "File format for dumped textures.";
    case Row::ControllerAimResponse:
        return "Refined uses one 15% dead zone for the whole stick, then a quadratic curve: a diagonal push "
               "turns the camera as far as a straight push at the same deflection. Original keeps the Xbox "
               "per-axis dead zone and cubic response.";
    case Row::ControllerAimSensitivity:
        return "Scales controller turn speed after the refined curve. 100% keeps the original full-stick "
               "turn speed; higher values help if full-stick turns still feel slow.";
    case Row::MouseSensitivity:
        return "How far the camera turns per mouse movement. 100% matches the PC version's default.";
    case Row::MouseVerticalSensitivity:
        return "Up and down speed as a share of the mouse sensitivity. 100% turns up and down as fast as left "
               "and right.";
    case Row::Controls:
        return "Rebind the keyboard, mouse and controller. The button prompts follow your choices.";
    case Row::Defaults:
        return "Resets every option in this menu. Display changes still need Apply.";
    case Row::Close:
        return DisplayPending() ? "Saves the settings and closes the menu. Unapplied display changes are "
                                  "discarded."
                                : "Saves the settings and closes the menu.";
    }
    return "";
}

std::string Value(Row row) {
    app::SettingsLock lock;
    const app::Settings &s = app::Get();
    char text[64];
    switch (row) {
    case Row::DisplayMode: {
        constexpr const char *kNames[] = {"Fullscreen", "Borderless", "Windowed"};
        return kNames[std::min<uint32_t>(uint32_t(g_menu.mode), 2)];
    }
    case Row::Resolution:
        if (!g_menu.resolution.width || !g_menu.resolution.height)
            std::snprintf(text, sizeof(text), "Desktop (%u x %u)", g_menu.desktop.width, g_menu.desktop.height);
        else
            std::snprintf(text, sizeof(text), "%u x %u", g_menu.resolution.width, g_menu.resolution.height);
        return text;
    case Row::AspectRatio:
        if (!s.aspect) {
            // the desktop mode was read when the menu opened; Output() would query Windows every frame
            const app::Mode o = s.width && s.height ? app::Mode{s.width, s.height} : g_menu.desktop;
            std::snprintf(text, sizeof(text), "Auto (%.2f:1)", o.height ? float(o.width) / float(o.height) : 0.0f);
        } else if (*choices::AspectName(s.aspect)) {
            std::snprintf(text, sizeof(text), "%s", choices::AspectName(s.aspect));
        } else {
            std::snprintf(text, sizeof(text), "%.2f:1", float(s.aspect) / 1000.0f);
        }
        return text;
    case Row::CutsceneBars:
        return s.remove_cutscene_bars ? "On" : "Off";
    case Row::RenderScale:
        std::snprintf(text, sizeof(text), "%u%% (%up)", s.render_percent,
                      choices::RenderHeight(g_menu.output_height, s.render_percent));
        return text;
    case Row::FieldOfView: {
        const float base = g_camera_fov_degrees.load();
        if (base > 0)
            std::snprintf(text, sizeof(text), "%u%% (%.0f deg)", g_fov_percent,
                          choices::ScaledFovDegrees(base, g_fov_percent));
        else
            std::snprintf(text, sizeof(text), "%u%%", g_fov_percent);
        return text;
    }
    case Row::ObjectDetail:
        return choices::ObjectDetailName(s.object_detail);
    case Row::PedDetail:
        return choices::PedDetailName(s.ped_detail);
    case Row::PedLimit:
        return choices::PedLimitName(s.ped_limit);
    case Row::PedDensity:
        return choices::PedDensityName(s.ped_density);
    case Row::TrafficDensity:
        return choices::TrafficDensityName(s.traffic_density);
    case Row::TrafficDistance:
        return choices::TrafficDistanceName(s.traffic_distance);
    case Row::CityRange: {
        constexpr const char* kNames[] = {"Normal", "5 x 5", "7 x 7", "9 x 9"};
        const uint32_t r = std::atomic_ref(s.city_range).load(std::memory_order_relaxed);
        return kNames[r < 4 ? r : 0];
    }
    case Row::CityInnerDetail:
    case Row::CityOuterDetail: {
        const uint32_t scale = row == Row::CityInnerDetail
            ? std::atomic_ref(s.city_inner_detail).load(std::memory_order_relaxed)
            : std::atomic_ref(s.city_outer_detail).load(std::memory_order_relaxed);
        if (!scale) return "Off";
        std::snprintf(text, sizeof(text), "%ux", scale);
        return text;
    }
    case Row::Anisotropic:
        if (!s.aniso) return "Game default";
        std::snprintf(text, sizeof(text), "%ux", s.aniso);
        return text;
    case Row::FrameCap:
        std::snprintf(text, sizeof(text), "%u FPS", choices::EffectiveFrameCap(app::FrameCap()));
        return text;
    case Row::DumpFormat:
        return s.texture_dump_format ? "DDS" : "PNG";
    case Row::ControllerAimResponse:
        return choices::ControllerAimResponseName(
            std::atomic_ref(input::g_controller_aim_response).load(std::memory_order_relaxed));
    case Row::ControllerAimSensitivity:
        std::snprintf(text, sizeof(text), "%u%%",
                      std::clamp(std::atomic_ref(input::g_controller_aim_sensitivity_percent)
                                     .load(std::memory_order_relaxed),
                                 controller_look::kMinSensitivityPercent,
                                 controller_look::kMaxSensitivityPercent));
        return text;
    case Row::MouseSensitivity:
    case Row::MouseVerticalSensitivity:
        std::snprintf(text, sizeof(text), "%u%%",
                      std::clamp(std::atomic_ref(row == Row::MouseSensitivity
                                                     ? input::g_mouse_sensitivity_percent
                                                     : input::g_mouse_vertical_sensitivity_percent)
                                     .load(std::memory_order_relaxed),
                                 choices::kMinMouseSensitivityPercent, choices::kMaxMouseSensitivityPercent));
        return text;
    case Row::Msaa: {
        const auto samples = app::MsaaSamples();
        return samples ? std::to_string(samples) + "x" : "Off";
    }
    case Row::Taa:
        return s.taa ? "On" : "Off";
    case Row::Smaa:
        return s.smaa ? "On" : "Off";
    case Row::Bloom:
        return s.bloom ? "On" : "Off";
    case Row::DepthOfField:
        return s.dof ? "On" : "Off";
    case Row::AmbientOcclusion:
        {
            constexpr const char* kNames[] = {"Off", "SSAO", "GTAO"};
            return kNames[choices::AoMethodChoice(s.ao, s.ao_method)];
        }
    case Row::AoQuality:
        return s.ao_quality == 1 ? "Half" : "Full";
    case Row::BuildingSeamFix:
        return s.building_seam_fix ? "On" : "Off";
    case Row::PcContrast:
        return s.pc_contrast ? "On" : "Off";
    case Row::Gamma:
        std::snprintf(text, sizeof(text), "%u%%", s.gamma);
        return text;
    case Row::Brightness:
        std::snprintf(text, sizeof(text), "%u%%", s.brightness);
        return text;
    case Row::Contrast:
        std::snprintf(text, sizeof(text), "%u%%", s.contrast);
        return text;
    case Row::Saturation:
        std::snprintf(text, sizeof(text), "%u%%", s.saturation);
        return text;
    case Row::Hdr:
        if (!s.hdr) return "Off";
        return gpu::g_hdr_available.load(std::memory_order_relaxed) ? "On" : "On (no HDR display)";
    case Row::HdrPaperWhite:
        std::snprintf(text, sizeof(text), "%u nits", s.hdr_paper_white);
        return text;
    case Row::HdrPeak: {
        const uint32_t reported = gpu::g_hdr_display_peak_nits.load(std::memory_order_relaxed);
        const uint32_t peak = s.hdr_peak ? s.hdr_peak : (reported ? reported : 1000);
        if (!s.hdr_peak) std::snprintf(text, sizeof(text), "Display (%u nits)", peak);
        else std::snprintf(text, sizeof(text), "%u nits", peak);
        return text;
    }
    case Row::HdrHighlights:
        std::snprintf(text, sizeof(text), "%u%%", s.hdr_highlights);
        return text;
    case Row::ShadowQuality: {
        constexpr const char *kNames[] = {"Original", "High (1024)", "Ultra (2048)"};
        return kNames[std::min<uint32_t>(s.shadow_resolution, 2)];
    }
    case Row::ShadowSoftness: {
        constexpr const char *kNames[] = {"Off", "Soft", "Softer"};
        return kNames[std::min<uint32_t>(s.shadow_softness, 2)];
    }
    case Row::BloomStrength:
        std::snprintf(text, sizeof(text), "%u%%", s.bloom_strength);
        return text;
    case Row::Upscaler:
        if (s.upscaler == choices::kFsr3 || s.upscaler == choices::kFsr4) {
            const char* name = s.upscaler == choices::kFsr4 ? "FSR 4.1" : "FSR 3.1";
            const gpu::FsrState state = gpu::g_fsr_state.load(std::memory_order_relaxed);
            if (state == gpu::FsrState::NoDll) return std::string(name) + " (AMD DLLs missing)";
            const char* preset = choices::TemporalPresetName(s.render_percent);
            if (!preset) return std::string(name) + " needs 50, 58, 67 or 100%";
            std::string label = std::string(name) + " " + preset;
            if (s.upscaler == choices::kFsr4 && state == gpu::FsrState::No4) label += " (using 3.1)";
            return label;
        }
        if (s.upscaler == choices::kDlss) {
#if !defined(TCNYC_HAVE_DLSS)
            return "DLSS (not built)";
#else
#ifdef _WIN32
            static const bool forced = [] {
                const char* e = DevEnv("TCNYC_DLSS_FORCE");
                return e && *e && std::atoi(e) != 0;
            }();
#else
            constexpr bool forced = false;
#endif
            const gpu::DlssState state = gpu::g_dlss_state.load(std::memory_order_relaxed);
            if (state == gpu::DlssState::NotBuilt) return "DLSS (not built)";
            if (state == gpu::DlssState::Unavailable && !forced) return "DLSS (unavailable)";
            const char* preset = choices::TemporalPresetName(s.render_percent);
            if (!preset) return "DLSS needs 50, 58, 67 or 100%";
#ifdef _WIN32
            const char* suffix = state == gpu::DlssState::Unavailable ? " (forced test)" : "";
            return std::string("DLSS ") + preset + suffix;
#else
            return std::string("DLSS ") + preset + " (experimental)";
#endif
#endif
        }
#ifdef _WIN32
        if (s.render_percent == 100) return s.upscaler == 1 ? "AMD FSR 1 (inactive)" : "Bilinear";
#endif
        if (s.render_percent >= 100) return "Not needed at 100%";
        return s.upscaler == 1 ? "AMD FSR 1" : "Bilinear";
    case Row::Sharpening:
        if (!s.sharpening) return "Off";
        std::snprintf(text, sizeof(text), "%u%%", s.sharpening);
        return text;
    case Row::LowLatency:
        return s.low_latency ? "On" : "Off";
    case Row::VSync:
        return s.vsync ? "On" : "Off";
    case Row::ShowFps:
        return ShowFps() ? "On" : "Off";
    case Row::TextureReplacement:
        return s.custom_textures ? "On" : "Off";
    case Row::TextureDumping:
        return s.dump_textures ? "On" : "Off";
    default:
        return "";
    }
}

// Left or right on a choice; true when the value changed.
bool Change(Row row, int direction) {
    app::SettingsLock lock;
    app::Settings &s = app::Get();
    // atomic stores: some of these (object_detail) are read on the game thread through atomic_ref
    auto set = [](uint32_t &field, uint32_t value) {
        if (field == value) return false;
        std::atomic_ref(field).store(value, std::memory_order_relaxed);
        return true;
    };
    bool changed = false;
    switch (row) {
    case Row::DisplayMode: {
        const auto mode = uint32_t(std::clamp(int(g_menu.mode) + direction, 0, 2));
        changed = mode != uint32_t(g_menu.mode);
        g_menu.mode = app::DisplayMode(mode);
        return changed; // pending until Apply
    }
    case Row::Resolution: {
        const auto &list = g_menu.resolutions;
        const auto it = std::ranges::find_if(list, [](app::Mode m) { return SameMode(m, g_menu.resolution); });
        const int index = it == list.end() ? 0 : int(it - list.begin());
        const int next = std::clamp(index + direction, 0, int(list.size()) - 1);
        g_menu.resolution = list[next];
        return next != index; // pending until Apply
    }
    case Row::RenderScale:
        // the renderer picks up the new scale after the frame (D3D12Backend::ApplySettings)
        changed =
            set(s.render_percent,
                choices::ClampRenderPercent(choices::Step(choices::kRenderPercents, s.render_percent, direction)));
        break;
    case Row::AspectRatio:
        // read by the renderer and the camera hook each frame: a single aligned 32-bit store
        changed = set(s.aspect, choices::Step(choices::kAspects, s.aspect, direction));
        break;
    case Row::FieldOfView:
        // read by the camera hooks on the game thread: a single aligned 32-bit store
        changed = set(g_fov_percent,
                      choices::ClampFovPercent(choices::Step(choices::kFovPercents, g_fov_percent, direction)));
        break;
    case Row::ObjectDetail:
        // read by the LOD hooks on the game thread: a single aligned 32-bit store
        changed = set(s.object_detail, choices::Step(choices::kObjectDetails, s.object_detail, direction));
        break;
    case Row::PedDetail:
        // read by the pedestrian hook on the game thread: a single aligned 32-bit store
        changed = set(s.ped_detail, choices::Step(choices::kPedDetails, s.ped_detail, direction));
        break;
    case Row::PedLimit:
        changed = set(s.ped_limit, choices::Step(choices::kPedLimits, s.ped_limit, direction));
        break;
    case Row::PedDensity:
        // read by the spawner hooks on the game thread: a single aligned 32-bit store
        changed = set(s.ped_density, choices::Step(choices::kPedDensities, s.ped_density, direction));
        break;
    case Row::TrafficDensity:
        // read by the traffic hook on the game thread: a single aligned 32-bit store
        changed = set(s.traffic_density, choices::Step(choices::kTrafficDensities, s.traffic_density, direction));
        break;
    case Row::TrafficDistance:
        // read by the traffic hooks on the game thread: a single aligned 32-bit store
        changed = set(s.traffic_distance, choices::Step(choices::kTrafficDistances, s.traffic_distance, direction));
        break;
    case Row::CityRange: {
        // read by the city ring on the game thread: a single aligned 32-bit store
        const uint32_t r = std::atomic_ref(s.city_range).load(std::memory_order_relaxed);
        const uint32_t next = choices::Step(choices::kCityRanges, r, direction);
        changed = next != r;
        std::atomic_ref(s.city_range).store(next, std::memory_order_relaxed);
        break;
    }
    case Row::CityInnerDetail:
    case Row::CityOuterDetail: {
        const bool inner = row == Row::CityInnerDetail;
        std::atomic_ref scale(inner ? s.city_inner_detail : s.city_outer_detail);
        const uint32_t old = scale.load(std::memory_order_relaxed);
        const uint32_t next = inner ? choices::Step(choices::kCityInnerDetail, old, direction)
                                    : choices::Step(choices::kCityOuterDetail, old, direction);
        changed = next != old;
        if (changed) scale.store(next, std::memory_order_relaxed);
        break;
    }
    case Row::ShadowQuality:
        changed = set(s.shadow_resolution, choices::Step(choices::kShadowLevels, s.shadow_resolution, direction));
        break;
    case Row::ShadowSoftness:
        changed = set(s.shadow_softness, choices::Step(choices::kShadowLevels, s.shadow_softness, direction));
        break;
    case Row::BloomStrength:
        changed = set(s.bloom_strength, choices::Step(choices::kBloomStrengths, s.bloom_strength, direction));
        break;
    case Row::Gamma:
        changed = set(s.gamma, choices::Step(choices::kGammaPercents, s.gamma, direction));
        break;
    case Row::Brightness:
        changed = set(s.brightness, choices::Step(choices::kBrightnessPercents, s.brightness, direction));
        break;
    case Row::Contrast:
        changed = set(s.contrast, choices::Step(choices::kContrastPercents, s.contrast, direction));
        break;
    case Row::Saturation:
        changed = set(s.saturation, choices::Step(choices::kSaturationPercents, s.saturation, direction));
        break;
    case Row::Upscaler:
        changed = set(s.upscaler, choices::Step(choices::kUpscalers, s.upscaler, direction));
        break;
    case Row::Sharpening:
        changed = set(s.sharpening, choices::Step(choices::kSharpenPercents, s.sharpening, direction));
        break;
    case Row::Anisotropic:
        changed = set(s.aniso, choices::Step(choices::kAnisoLevels, s.aniso, direction));
        break;
    case Row::AmbientOcclusion:
        {
            const uint32_t c = choices::Step(choices::kAoMethodChoices, choices::AoMethodChoice(s.ao, s.ao_method), direction);
            uint32_t method = s.ao_method;
            const uint32_t ao = choices::ApplyAoMethodChoice(c, s.ao_quality, method);
            const bool m = set(s.ao_method, method);
            changed = set(s.ao, ao) || m;
        }
        break;
    case Row::AoQuality:
        {
            const bool q = set(s.ao_quality, choices::Step(choices::kAoQualities, s.ao_quality, direction));
            changed = (s.ao ? set(s.ao, s.ao_quality) : false) || q;
        }
        break;
    case Row::Msaa: {
        const auto current = app::MsaaSamples();
        const auto next = choices::Step(choices::kMsaaSamples, current, direction);
        changed = current != next;
        if (changed) app::SetMsaaSamples(next);
        break;
    }
    case Row::FrameCap: {
        const auto current = app::FrameCap();
        const auto next = choices::Step(choices::kFrameCaps, choices::EffectiveFrameCap(current), direction);
        changed = current != next;
        if (changed) app::SetFrameCap(next);
        break;
    }
    case Row::DumpFormat:
        changed = set(s.texture_dump_format, direction > 0 ? 1u : 0u);
        break;
    case Row::ControllerAimResponse: {
        const uint32_t current =
            std::atomic_ref(input::g_controller_aim_response).load(std::memory_order_relaxed);
        const uint32_t next = choices::Step(choices::kControllerAimResponses, current, direction);
        changed = next != current;
        if (changed) std::atomic_ref(input::g_controller_aim_response).store(next, std::memory_order_relaxed);
        break;
    }
    case Row::ControllerAimSensitivity: {
        const uint32_t current = std::clamp(
            std::atomic_ref(input::g_controller_aim_sensitivity_percent).load(std::memory_order_relaxed),
            controller_look::kMinSensitivityPercent, controller_look::kMaxSensitivityPercent);
        const uint32_t next = choices::Step(choices::kControllerAimSensitivities, current, direction);
        changed = next != current;
        if (changed)
            std::atomic_ref(input::g_controller_aim_sensitivity_percent).store(next, std::memory_order_relaxed);
        break;
    }
    case Row::MouseSensitivity:
    case Row::MouseVerticalSensitivity: {
        uint32_t &setting = row == Row::MouseSensitivity ? input::g_mouse_sensitivity_percent
                                                         : input::g_mouse_vertical_sensitivity_percent;
        const uint32_t current =
            std::clamp(std::atomic_ref(setting).load(std::memory_order_relaxed), choices::kMinMouseSensitivityPercent,
                       choices::kMaxMouseSensitivityPercent);
        const uint32_t next = choices::Step(choices::kMouseSensitivities, current, direction);
        changed = next != current;
        if (changed) std::atomic_ref(setting).store(next, std::memory_order_relaxed);
        break;
    }
    case Row::HdrPaperWhite:
        changed = set(s.hdr_paper_white, choices::Step(choices::kHdrPaperWhiteNits, s.hdr_paper_white, direction));
        break;
    case Row::HdrPeak:
        changed = set(s.hdr_peak, choices::Step(choices::kHdrPeakNits, s.hdr_peak, direction));
        break;
    case Row::HdrHighlights:
        changed = set(s.hdr_highlights, choices::Step(choices::kHdrHighlightPercents, s.hdr_highlights, direction));
        break;
    default:
        break;
    }
    g_menu.dirty |= changed;
    return changed;
}

void Activate(Row row) {
    app::SettingsLock lock;
    app::Settings &s = app::Get();
    switch (KindOf(row)) {
    case Kind::Choice:
        // steps forward and wraps to the first value, so every choice is reachable with one button
        if (!Change(row, 1)) {
            while (Change(row, -1)) {
            }
        }
        return;
    case Kind::Toggle:
        switch (row) {
        case Row::Taa:
            s.taa = !s.taa;
            break;
        case Row::Smaa:
            s.smaa = !s.smaa;
            break;
        case Row::Bloom:
            s.bloom = !s.bloom;
            break;
        case Row::DepthOfField:
            s.dof = !s.dof;
            break;
        case Row::BuildingSeamFix:
            s.building_seam_fix = !s.building_seam_fix;
            break;
        case Row::PcContrast:
            s.pc_contrast = !s.pc_contrast;
            break;
        case Row::CutsceneBars:
            s.remove_cutscene_bars = !s.remove_cutscene_bars;
            break;
        case Row::Hdr:
            s.hdr = !s.hdr;
            break;
        case Row::LowLatency:
            s.low_latency = !s.low_latency;
            break;
        case Row::VSync:
            s.vsync = !s.vsync;
            break;
        case Row::ShowFps:
            SetShowFps(!ShowFps());
            break;
        case Row::TextureReplacement:
            s.custom_textures = !s.custom_textures;
            break;
        case Row::TextureDumping:
            s.dump_textures = !s.dump_textures;
            break;
        default:
            break;
        }
        g_menu.dirty = true;
        return;
    case Kind::Action:
        if (row == Row::ApplyDisplay)
            ApplyDisplayChanges();
        else if (row == Row::Controls)
            OpenControls();
        else if (row == Row::Defaults)
            RestoreDefaults();
        else if (row == Row::Close)
            CloseMenu();
        return;
    }
}

// next enabled row, wrapping
void MoveSelection(int direction) {
    int row = g_menu.selected;
    for (int i = 0; i < kRowCount; ++i) {
        row = (row + direction + kRowCount) % kRowCount;
        if (Enabled(kRows[row])) break;
    }
    g_menu.selected = row;
}

// keyboard and controller 1, as presses with repeat
uint32_t NavPresses() {
    uint32_t held = 0;
    const uint64_t pad = g_pad_nav.load();
    if (uint32_t(NowMs()) - uint32_t(pad) <= kPadStaleMs) held = uint32_t(pad >> 32);
    uint32_t pressed = g_menu.repeater.Update(held, ImGui::GetTime());
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) pressed |= kUp;
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) pressed |= kDown;
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) pressed |= kLeft;
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) pressed |= kRight;
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
        ImGui::IsKeyPressed(ImGuiKey_Space, false))
        pressed |= kAccept;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) pressed |= kBack;
    return pressed;
}

void HandleControlsNavigation(uint32_t pressed) {
    auto &c = g_controls;
    if (pressed & kBack) g_menu.controls = false;
    if (pressed & kUp) c.selected = (c.selected + kControlRows - 1) % kControlRows;
    if (pressed & kDown) c.selected = (c.selected + 1) % kControlRows;
    if (pressed & kAccept) ActivateControl(c.selected);
}

void HandleNavigation() {
    if (g_menu.controls) {
        const uint32_t pressed = NavPresses();  // also keeps the repeater current during a capture
        if (!CaptureControls()) HandleControlsNavigation(pressed);
        return;
    }
    const uint32_t pressed = NavPresses();
    if (pressed & kBack) {
        CloseMenu();
        return;
    }
    if (pressed & kUp) MoveSelection(-1);
    if (pressed & kDown) MoveSelection(1);
    const Row row = kRows[g_menu.selected];
    if (!Enabled(row)) return;
    if (KindOf(row) == Kind::Choice) {
        if (pressed & kLeft) Change(row, -1);
        if (pressed & kRight) Change(row, 1);
    } else if (KindOf(row) == Kind::Toggle && (pressed & (kLeft | kRight))) {
        Activate(row);
    }
    if (pressed & kAccept) Activate(row);
}

void DrawFpsCounter(double fps) {
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos({viewport->WorkPos.x + 12.0f, viewport->WorkPos.y + 12.0f});
    ImGui::SetNextWindowBgAlpha(0.65f);
    constexpr auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs |
                           ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("FPS counter", nullptr, flags)) ImGui::Text("%.1f FPS", fps);
    ImGui::End();
}

void DrawValue(Row row, float x, float width) {
    const std::string value = Value(row);
    ImGui::SameLine(x + std::max(0.0f, (width - ImGui::CalcTextSize(value.c_str()).x) / 2));
    const bool pending = (row == Row::DisplayMode || row == Row::Resolution) && DisplayPending();
    if (pending) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.35f, 1.0f));
    ImGui::AlignTextToFramePadding(); // level with the row label and arrow buttons
    ImGui::TextUnformatted(value.c_str());
    if (pending) ImGui::PopStyleColor();
}

void DrawRows(float scale) {
    const float arrow = ImGui::GetFrameHeight();
    const float value_width = 230.0f * scale;
    // window-local x of the left arrow, the value text and the right arrow
    const float right_x = ImGui::GetWindowContentRegionMax().x - arrow;
    const float value_x = right_x - value_width;
    const float left_x = value_x - arrow;
    const ImVec2 mouse_delta = ImGui::GetIO().MouseDelta;
    const bool mouse_moved = mouse_delta.x != 0 || mouse_delta.y != 0;
    // the row the keyboard or pad last moved to, kept in view when the list is taller than the screen
    static int scrolled_to = -1;
    const bool follow = g_menu.selected != scrolled_to;

    for (int i = 0; i < kRowCount; ++i) {
        const Row row = kRows[i];
        if (row == Row::CityOuterDetail && !game::city_grid::OuterDetailAvailable()) continue;
        if (const char *section = Section(row)) {
            ImGui::Spacing();
            if (*section)
                ImGui::SeparatorText(section);
            else
                ImGui::Separator();
        }
        const bool enabled = Enabled(row);
        const Kind kind = KindOf(row);
        ImGui::PushID(i);
        ImGui::BeginDisabled(!enabled);

        const bool clicked =
            ImGui::Selectable(Label(row), g_menu.selected == i, ImGuiSelectableFlags_AllowOverlap, {0, arrow});
        if (follow && g_menu.selected == i && !(mouse_moved && ImGui::IsItemHovered())) {
            // the first row takes its section heading with it; others only move as far as they must
            if (i == 0)
                ImGui::SetScrollY(0);
            else if (!ImGui::IsItemVisible() || ImGui::GetItemRectMax().y > ImGui::GetWindowPos().y + ImGui::GetWindowHeight() ||
                     ImGui::GetItemRectMin().y < ImGui::GetWindowPos().y)
                ImGui::SetScrollHereY(ImGui::GetItemRectMin().y < ImGui::GetWindowPos().y ? 0.0f : 1.0f);
        }
        if (enabled && mouse_moved && ImGui::IsItemHovered()) g_menu.selected = i;
        if (clicked && enabled) {
            g_menu.selected = i;
            if (kind != Kind::Choice) Activate(row);
        }
        if (kind == Kind::Choice) {
            ImGui::SameLine(left_x);
            if (ImGui::ArrowButton("##less", ImGuiDir_Left)) {
                g_menu.selected = i;
                Change(row, -1);
            }
            DrawValue(row, value_x, value_width);
            ImGui::SameLine(right_x);
            if (ImGui::ArrowButton("##more", ImGuiDir_Right)) {
                g_menu.selected = i;
                Change(row, 1);
            }
        } else if (kind == Kind::Toggle) {
            DrawValue(row, value_x, value_width);
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    scrolled_to = g_menu.selected;
}

void DrawControls(float scale) {
    auto &c = g_controls;
    const float value_x = ImGui::GetWindowContentRegionMax().x - 260.0f * scale;
    for (int row = 0; row < kControlRows; ++row) {
        if (row == 0) ImGui::SeparatorText("KEYBOARD AND MOUSE");
        if (row == kKeyRows) ImGui::SeparatorText("CONTROLLER");
        if (row == kKeyRows + kPadRows) ImGui::Separator();
        const char *label = row < kKeyRows             ? kKeyActions[row]
                            : row < kKeyRows + kPadRows ? kPadActions[row - kKeyRows]
                            : row == kKeyRows + kPadRows ? "Reset keyboard"
                            : row == kKeyRows + kPadRows + 1 ? "Reset controller"
                                                             : "Back";
        ImGui::PushID(row);
        if (ImGui::Selectable(label, c.selected == row, ImGuiSelectableFlags_AllowOverlap) && c.capture < 0) {
            c.selected = row;
            ActivateControl(row);
        }
        if (c.selected == row && !ImGui::IsItemVisible()) ImGui::SetScrollHereY(0.5f);
        const std::string value = c.capture == row ? (row < kKeyRows ? "Press a key..." : "Press a button...")
                                                   : ControlValue(row);
        if (!value.empty()) {
            ImGui::SameLine(value_x);
            if (c.capture == row) ImGui::TextColored({1.0f, 0.8f, 0.35f, 1.0f}, "%s", value.c_str());
            else ImGui::TextUnformatted(value.c_str());
        }
        ImGui::PopID();
    }
}

void DrawMenu(double fps) {
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    const float scale = std::clamp(viewport->Size.y / 720.0f, 1.0f, 4.0f);

    // dim the game behind the menu
    ImGui::GetBackgroundDrawList()->AddRectFilled(
        viewport->Pos, {viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y},
        IM_COL32(0, 0, 0, 110));

    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({620.0f * scale, 0}); // height fits the rows
    ImGui::SetNextWindowBgAlpha(0.92f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {18 * scale, 14 * scale});
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {6 * scale, 4 * scale});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {8 * scale, 4 * scale});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6 * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, {0.0f, 0.5f});
    constexpr auto flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                           ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("Graphics", nullptr, flags)) {
        ImGui::SetWindowFontScale(scale);
        ImGui::TextUnformatted(g_menu.controls ? "CONTROLS" : "GRAPHICS");
        char rate[32];
        std::snprintf(rate, sizeof(rate), "%.1f FPS", fps);
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize(rate).x);
        ImGui::TextDisabled("%s", rate);

        // The rows scroll when the whole menu would be taller than the screen (a low resolution or a
        // handheld), leaving room for the heading above and the help and key lines below.
        const float line = ImGui::GetTextLineHeightWithSpacing();
        const float rows_max = std::max(line * 4, viewport->Size.y * 0.92f - line * 7 - 40 * scale);
        ImGui::SetNextWindowSizeConstraints({0, 0}, {FLT_MAX, rows_max});
        ImGui::BeginChild("rows", {0, 0}, ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoNav);
        if (g_menu.controls) DrawControls(scale);
        else DrawRows(scale);
        ImGui::EndChild();

        ImGui::Spacing();
        ImGui::Separator();
        // room for two lines, so the window keeps its height from row to row
        const float help_height = ImGui::GetTextLineHeightWithSpacing() * 2;
        ImGui::BeginChild("help", {0, help_height}, ImGuiChildFlags_None, ImGuiWindowFlags_NoNav);
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", g_menu.controls ? "Enter / A on a row, then press the new key or button. A new key "
                                                    "becomes the main one; Delete clears the row, Escape cancels. "
                                                    "Taking a controller button from another action swaps them."
                                                  : Help(kRows[g_menu.selected]));
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        if (g_menu.save_failed)
            ImGui::TextColored({1.0f, 0.45f, 0.35f, 1.0f}, "Could not save settings. Close again to return to the game.");
        if (g_menu.apply_failed)
            ImGui::TextColored({1.0f, 0.45f, 0.35f, 1.0f}, "Display change failed; the previous mode is still in use.");
        ImGui::Separator();
        ImGui::TextDisabled("Arrows / D-pad: change   Enter / A: select   Esc / B: close");
    }
    ImGui::End();
    ImGui::PopStyleVar(5);
}

} // namespace

void DrawGraphicsMenu(double fps) {
    PollDisplayApply();
    const bool toggle = g_toggle.exchange(false) || ImGui::IsKeyPressed(ImGuiKey_F10, false);
    if (toggle) {
        if (g_menu.open)
            CloseMenu();
        else
            OpenMenu();
    } else if (g_menu.open) {
        HandleNavigation();
    }
    if (g_menu.open)
        DrawMenu(fps);
    else if (ShowFps())
        DrawFpsCounter(fps);
}

void FilterMenuPad(int port, input::Pad &pad) {
    static std::mutex lock;
    static PadGate gate;
    if (port != 0) return;
    std::lock_guard guard(lock);
    g_pad_nav.store(uint64_t(NavBits(pad)) << 32 | uint32_t(NowMs()));
    if (gate.Filter(pad, g_open.load())) g_toggle.store(true);
}

bool GraphicsMenuMessage(void *, uint32_t msg, uintptr_t wp, intptr_t, intptr_t *result) {
#ifdef _WIN32
    // Alt+F4 and the window close button skip CloseMenu; save the live
    // overlay settings here so an applied change is not lost on exit.
    if (msg == WM_CLOSE || msg == WM_DESTROY) SaveOnExit();
    // Left Alt is heavy attack. Prevent its release from entering window-menu mode.
    if (msg == WM_SYSCOMMAND && (wp & 0xFFF0u) == SC_KEYMENU) {
        *result = 0;
        return true;
    }
    // F10 is the Windows menu key: on its release Windows enters menu mode, and in Windowed mode the arrow keys
    // then open the system menu instead of reaching the Graphics menu
    if ((msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) && (wp == VK_F10 || wp == VK_MENU)) {
        *result = 0;
        return true;
    }
#else
    (void)msg;
    (void)wp;
    (void)result;
#endif
    return false;
}

} // namespace game
