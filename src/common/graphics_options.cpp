// The in-game Graphics menu: F10, or Back + Start on controller 1.
//
// Rows are driven by the keyboard (arrows, Enter, Esc), controller 1 (D-pad or
// left stick, A, B) or the mouse. Controller input reaches the menu through
// GameInfo::pad, which polls SDL, so every controller type works; the game gets
// a pad with nothing pressed while the menu is open. Changes apply at once,
// except display mode and resolution, which wait for Apply. The settings file
// is written when the menu closes.

#include "graphics_menu.h"

#include "app/app.h"
#include "graphics_choices.h"
#include "menu_input.h"

#include <imgui.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace game {

bool g_show_fps = false;
uint32_t g_fov_percent = 100;
std::atomic<float> g_camera_fov_degrees{0};

bool ShowFps() { return std::atomic_ref(g_show_fps).load(std::memory_order_relaxed); }

void SetShowFps(bool show) { std::atomic_ref(g_show_fps).store(show, std::memory_order_relaxed); }

namespace {

using namespace menu_input;

// shared between the pad thread and the UI thread
std::atomic<bool> g_open{false};
std::atomic<bool> g_toggle{false};
std::atomic<uint64_t> g_pad_nav{0};   // nav bits << 32 | GetTickCount64 of the poll
constexpr uint32_t kPadStaleMs = 250; // a game that stopped polling leaves no held direction behind

enum class Row {
    DisplayMode,
    Resolution,
    ApplyDisplay,
    AspectRatio,
    RenderScale,
    FieldOfView,
    Anisotropic,
    Msaa,
    Smaa,
    CityShadowFill,
    Bloom,
    DepthOfField,
    PcContrast,
    VSync,
    FrameCap,
    ShowFps,
    TextureReplacement,
    TextureDumping,
    DumpFormat,
    Defaults,
    Close,
};
constexpr Row kRows[] = {Row::DisplayMode,
                         Row::Resolution,
                         Row::ApplyDisplay,
                         Row::AspectRatio,
                         Row::RenderScale,
                         Row::FieldOfView,
                         Row::Anisotropic,
                         Row::Msaa,
                         Row::Smaa,
                         Row::CityShadowFill,
                         Row::Bloom,
                         Row::DepthOfField,
                         Row::PcContrast,
                         Row::VSync,
                         Row::FrameCap,
                         Row::ShowFps,
                         Row::TextureReplacement,
                         Row::TextureDumping,
                         Row::DumpFormat,
                         Row::Defaults,
                         Row::Close};
constexpr int kRowCount = int(std::size(kRows));

enum class Kind { Choice, Toggle, Action };

// UI thread only, under UiLock
struct Menu {
    bool open = false;
    int selected = 0;
    bool dirty = false; // settings changed since the menu opened or last saved
    app::DisplayMode mode{};
    app::Mode resolution{};
    std::vector<app::Mode> resolutions; // index 0 is Desktop (0 x 0)
    app::Mode desktop{};
    uint32_t output_height = 0;
    Repeater repeater;
} g_menu;

bool SameMode(app::Mode a, app::Mode b) { return a.width == b.width && a.height == b.height; }

} // namespace

std::vector<app::Mode> DisplayModes() {
    std::vector<app::Mode> modes{{0, 0}};
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
    std::sort(modes.begin() + 1, modes.end(),
              [](app::Mode a, app::Mode b) { return std::pair{a.width, a.height} < std::pair{b.width, b.height}; });
    return modes;
}

namespace {

bool DisplayPending() {
    app::SettingsLock lock;
    const app::Settings &s = app::Get();
    return g_menu.mode != s.display_mode || !SameMode(g_menu.resolution, {s.width, s.height});
}

void OpenMenu() {
    app::SettingsLock lock;
    const app::Settings &s = app::Get();
    g_menu.open = true;
    g_menu.dirty = false;
    g_menu.mode = s.display_mode;
    g_menu.resolution = {s.width, s.height};
    g_menu.resolutions = DisplayModes();
    if (std::ranges::none_of(g_menu.resolutions, [](app::Mode m) { return SameMode(m, g_menu.resolution); }))
        g_menu.resolutions.push_back(g_menu.resolution); // a size the monitor no longer lists stays selectable
    g_menu.desktop = app::Desktop();
    g_menu.output_height = app::Output().height;
    g_menu.repeater = {};
    g_open.store(true);
    input::SetUiActive(true);
    app::CaptureMouse(false); // the menu needs the pointer
}

void CloseMenu() {
    if (g_menu.dirty) app::Save();
    g_menu.open = false;
    g_open.store(false);
    input::SetUiActive(false);
    app::CaptureMouse(true);
}

void ApplyDisplayChanges() {
    app::SettingsLock lock;
    app::Settings &s = app::Get();
    s.display_mode = g_menu.mode;
    s.width = g_menu.resolution.width;
    s.height = g_menu.resolution.height;
    app::Save();
    app::ApplyDisplay();
    g_menu.dirty = false;
    g_menu.output_height = app::Output().height;
}

void RestoreDefaults() {
    app::SettingsLock lock;
    const app::Settings d;
    app::Settings &s = app::Get();
    s.render_percent = d.render_percent;
    s.aspect = d.aspect;
    app::SetFrameCap(d.fps_cap);
    s.vsync = d.vsync;
    s.aniso = d.aniso;
    app::SetMsaaSamples(d.msaa);
    s.smaa = d.smaa;
    s.city_shadow_fill = d.city_shadow_fill;
    s.bloom = d.bloom;
    s.dof = d.dof;
    s.pc_contrast = d.pc_contrast;
    s.custom_textures = d.custom_textures;
    s.dump_textures = d.dump_textures;
    s.texture_dump_format = d.texture_dump_format;
    SetShowFps(false);
    g_fov_percent = 100;
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
    case Row::FieldOfView:
    case Row::Anisotropic:
    case Row::FrameCap:
    case Row::DumpFormat:
    case Row::Msaa:
        return Kind::Choice;
    case Row::Smaa:
    case Row::CityShadowFill:
    case Row::Bloom:
    case Row::DepthOfField:
    case Row::PcContrast:
    case Row::VSync:
    case Row::ShowFps:
    case Row::TextureReplacement:
    case Row::TextureDumping:
        return Kind::Toggle;
    default:
        return Kind::Action;
    }
}

bool Enabled(Row row) {
    if (row == Row::ApplyDisplay) return DisplayPending();
    if (row == Row::DumpFormat) return app::Get().dump_textures;
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
    case Row::RenderScale:
        return "Render scale";
    case Row::FieldOfView:
        return "Field of view";
    case Row::Anisotropic:
        return "Anisotropic filtering";
    case Row::Msaa:
        return "MSAA";
    case Row::Smaa:
        return "SMAA";
    case Row::CityShadowFill:
        return "City shadow fill";
    case Row::Bloom:
        return "Bloom";
    case Row::DepthOfField:
        return "Depth of field";
    case Row::PcContrast:
        return "PC contrast";
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
    case Row::TextureReplacement:
        return "Textures";
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
        return "The picture's shape; Auto matches the display. The 3D view widens to it, while HUD, menus "
               "and movies stay 16:9 in the middle. 4:3 and the wide shapes switch after a restart.";
    case Row::RenderScale:
        return "Internal resolution relative to the output height. Above 100% renders more pixels for a "
               "sharper image; below 100% is faster.";
    case Row::FieldOfView:
        return "Widens or narrows the gameplay camera. 100% is the game's own view; aiming still zooms "
               "relative to it. Menus and cutscene previews are unchanged.";
    case Row::Anisotropic:
        return "Keeps textures sharp at glancing angles, such as roads into the distance. Game default uses "
               "the filtering the game asks for.";
    case Row::Msaa:
        return "Smooths geometry edges. Higher counts require more GPU memory.";
    case Row::Smaa:
        return "Smooths jagged edges on the finished frame (SMAA 1x). The HUD is smoothed with it.";
    case Row::CityShadowFill:
        return "Lifts dark city walls while retaining the game's lights and shadows.";
    case Row::Bloom:
        return "Adds extra bright-pass bloom over the game's own lighting.";
    case Row::DepthOfField:
        return "Adds distance blur to the finished 3D frame when MSAA is enabled.";
    case Row::PcContrast:
        return "Applies a PC-inspired contrast curve to the finished 3D frame.";
    case Row::VSync:
        return "Waits for the monitor's refresh before showing a frame, which prevents tearing.";
    case Row::FrameCap:
        return "Sets the frame limit for gameplay and real-time cutscenes.";
    case Row::ShowFps:
        return "Shows the frame rate in the top-left corner.";
    case Row::TextureReplacement:
        return "Uses images in textures\\replacement beside the exe in place of the game's textures.";
    case Row::TextureDumping:
        return "Saves each texture the game uses into textures\\dumps beside the exe, once. Slows loading "
               "while on.";
    case Row::DumpFormat:
        return "File format for dumped textures.";
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
            const app::Mode o = app::Output();
            std::snprintf(text, sizeof(text), "Auto (%.2f:1)", o.height ? float(o.width) / float(o.height) : 0.0f);
        } else if (*choices::AspectName(s.aspect)) {
            std::snprintf(text, sizeof(text), "%s", choices::AspectName(s.aspect));
        } else {
            std::snprintf(text, sizeof(text), "%.2f:1", float(s.aspect) / 1000.0f);
        }
        return text;
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
    case Row::Anisotropic:
        if (!s.aniso) return "Game default";
        std::snprintf(text, sizeof(text), "%ux", s.aniso);
        return text;
    case Row::FrameCap:
        std::snprintf(text, sizeof(text), "%u FPS", choices::EffectiveFrameCap(app::FrameCap()));
        return text;
    case Row::DumpFormat:
        return s.texture_dump_format ? "DDS" : "PNG";
    case Row::Msaa: {
        const auto samples = app::MsaaSamples();
        return samples ? std::to_string(samples) + "x" : "Off";
    }
    case Row::Smaa:
        return s.smaa ? "On" : "Off";
    case Row::CityShadowFill:
        return s.city_shadow_fill ? "On" : "Off";
    case Row::Bloom:
        return s.bloom ? "On" : "Off";
    case Row::DepthOfField:
        return s.dof ? "On" : "Off";
    case Row::PcContrast:
        return s.pc_contrast ? "On" : "Off";
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
    app::Settings &s = app::Get();
    auto set = [](uint32_t &field, uint32_t value) {
        if (field == value) return false;
        field = value;
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
    case Row::Anisotropic:
        changed = set(s.aniso, choices::Step(choices::kAnisoLevels, s.aniso, direction));
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
        case Row::Smaa:
            s.smaa = !s.smaa;
            break;
        case Row::CityShadowFill:
            s.city_shadow_fill = !s.city_shadow_fill;
            break;
        case Row::Bloom:
            s.bloom = !s.bloom;
            break;
        case Row::DepthOfField:
            s.dof = !s.dof;
            break;
        case Row::PcContrast:
            s.pc_contrast = !s.pc_contrast;
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
    if (uint32_t(GetTickCount64()) - uint32_t(pad) <= kPadStaleMs) held = uint32_t(pad >> 32);
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

void HandleNavigation() {
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

    for (int i = 0; i < kRowCount; ++i) {
        const Row row = kRows[i];
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
        ImGui::TextUnformatted("GRAPHICS");
        char rate[32];
        std::snprintf(rate, sizeof(rate), "%.1f FPS", fps);
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize(rate).x);
        ImGui::TextDisabled("%s", rate);

        DrawRows(scale);

        ImGui::Spacing();
        ImGui::Separator();
        // room for two lines, so the window keeps its height from row to row
        const float help_height = ImGui::GetTextLineHeightWithSpacing() * 2;
        ImGui::BeginChild("help", {0, help_height}, ImGuiChildFlags_None, ImGuiWindowFlags_NoNav);
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", Help(kRows[g_menu.selected]));
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        ImGui::Separator();
        ImGui::TextDisabled("Arrows / D-pad: change   Enter / A: select   Esc / B: close");
    }
    ImGui::End();
    ImGui::PopStyleVar(5);
}

} // namespace

void DrawGraphicsMenu(double fps) {
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
    g_pad_nav.store(uint64_t(NavBits(pad)) << 32 | uint32_t(GetTickCount64()));
    if (gate.Filter(pad, g_open.load())) g_toggle.store(true);
}

bool GraphicsMenuMessage(void *, uint32_t msg, uintptr_t wp, intptr_t, intptr_t *result) {
    // F10 is the Windows menu key: on its release Windows enters menu mode, and in Windowed mode the arrow keys
    // then open the system menu instead of reaching the Graphics menu
    if ((msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) && wp == VK_F10) {
        *result = 0;
        return true;
    }
    return false;
}

} // namespace game
