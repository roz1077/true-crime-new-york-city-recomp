#include "launcher.h"
#include "core/dev_env.h"
#include "updater.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_sdlrenderer3.h>
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <toml++/toml.hpp>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include <stb_image.h>

#include "app/app.h"
#include "app/settings_store.h"
#include "common/graphics_choices.h"
#include "core/input.h"

namespace launcher {
namespace {

namespace fs = std::filesystem;
using namespace game::choices;

// Dear ImGui 1.91.1 renamed the child border flag; Windows and Linux pin different versions.
#if IMGUI_VERSION_NUM >= 19110
constexpr ImGuiChildFlags kChildBorder = ImGuiChildFlags_Borders;
#else
constexpr ImGuiChildFlags kChildBorder = ImGuiChildFlags_Border;
#endif

constexpr char kTitle[] = "True Crime: New York City";
constexpr unsigned char kBanner[] = {
#include "launcher_banner.inc"
};

std::string Utf8(const fs::path &path) {
    auto bytes = path.u8string();
    return {bytes.begin(), bytes.end()};
}

fs::path FromUtf8(std::string_view text) {
    return fs::path(std::u8string(text.begin(), text.end()));
}

fs::path PreferencesPath() {
#if TCNYC_PAL_EU
    constexpr char name[] = "launcher-pal-eu.toml";
#elif defined(TCNYC_PAL)
    constexpr char name[] = "launcher-pal.toml";
#else
    constexpr char name[] = "launcher.toml";
#endif
    return app::SettingsPath().parent_path() / name;
}

struct Picker {
    std::mutex mutex;
    bool pending = false;
    std::string path;
    std::string error;
};

// The dialog callback can run on a different thread.
void SDLCALL Picked(void *userdata, const char *const *files, int) {
    auto &picker = *static_cast<Picker *>(userdata);
    std::lock_guard lock(picker.mutex);
    if (!files)
        picker.error = std::string("Could not open the file picker. ") + SDL_GetError();
    else if (*files)
        picker.path = *files;
    picker.pending = false;
}

void Browse(SDL_Window *window, Picker &picker) {
    static constexpr SDL_DialogFileFilter filters[] = {{"Xbox disc images", "iso;ISO"}};
    {
        std::lock_guard lock(picker.mutex);
        picker.pending = true;
    }
    SDL_ShowOpenFileDialog(Picked, &picker, window, filters, 1, nullptr, false);
}

struct Graphics {
    app::Settings settings;
    toml::table edits;
    SDL_Window *window = nullptr;
    Picker music_picker;  // the custom music folder
    std::vector<app::Mode> resolutions{{0, 0}};
    bool languages = false;  // the Fr/Es/It disc is selected: the only one with other text languages

    void Save() {
        if (edits.empty()) return;
        toml::table full;
        app::FullSettingsTable(full);
        app::MergeSettingsTable(app::SettingsPath(), edits, &full);
        edits.clear();
    }

    void FindResolutions(SDL_Window *window) {
        int count = 0;
        auto modes = SDL_GetFullscreenDisplayModes(SDL_GetDisplayForWindow(window), &count);
        for (int i = 0; i < count; ++i) {
            const auto &mode = *modes[i];
            if (mode.w >= 640 && mode.h >= 480) resolutions.push_back({uint32_t(mode.w), uint32_t(mode.h)});
        }
        SDL_free(modes);
        if (settings.width && settings.height) resolutions.push_back({settings.width, settings.height});
        const auto dimensions = [](app::Mode mode) { return std::pair{mode.width, mode.height}; };
        std::ranges::sort(resolutions, {}, dimensions);
        auto duplicates = std::ranges::unique(resolutions, {}, dimensions);
        resolutions.erase(duplicates.begin(), duplicates.end());
    }

    void Toggle(const char *label, const char *key, bool &value) {
        ImGui::TableNextColumn();
        if (ImGui::Checkbox(label, &value)) edits.insert_or_assign(key, value);
    }

    // A labelled drop-down filling one grid cell.
    template <class Label>
    bool Choice(const char *label, const char *key, uint32_t &value, std::span<const uint32_t> options, Label name) {
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", label);
        ImGui::SetNextItemWidth(-1);
        ImGui::PushID(key);
        bool changed = false;
        if (ImGui::BeginCombo("##choice", name(value).c_str())) {
            for (uint32_t option : options) {
                if (ImGui::Selectable(name(option).c_str(), value == option)) {
                    value = option;
                    edits.insert_or_assign(key, value);
                    changed = true;
                }
                if (value == option) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::PopID();
        return changed;
    }

    static bool Section(const char *title) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(IM_COL32(243, 178, 79, 255)));
        ImGui::SeparatorText(title);
        ImGui::PopStyleColor();
        return ImGui::BeginTable(title, 3, ImGuiTableFlags_SizingStretchSame);
    }

    void Draw() {
        constexpr const char *display_names[] = {"Fullscreen", "Borderless", "Windowed"};
        constexpr const char *display_keys[] = {"fullscreen", "borderless", "windowed"};
        constexpr const char *upscaler_names[] = {"Bilinear", "AMD FSR 1", "AMD FSR 3.1", "NVIDIA DLSS", "AMD FSR 4.1"};
        constexpr const char *renderer_names[] = {"Direct3D 12", "Vulkan"};
        const auto percent = [](uint32_t v) { return std::to_string(v) + "%"; };
        const auto multiple = [](uint32_t v) { return v ? std::to_string(v) + "x" : std::string("Off"); };
        const auto level = [](uint32_t v) {
            constexpr const char *names[] = {"Original", "Sharper", "Sharpest"};
            return std::string(names[v]);
        };
        const auto softness = [](uint32_t v) {
            constexpr const char *names[] = {"Original", "Soft", "Softer"};
            return std::string(names[v]);
        };
        const auto resolution_name = [](app::Mode mode) {
            return mode.width ? std::to_string(mode.width) + " x " + std::to_string(mode.height)
                              : std::string("Desktop");
        };

        if (Section("DISPLAY")) {
            ImGui::TableNextColumn();
            ImGui::TextDisabled("Display mode");
            ImGui::SetNextItemWidth(-1);
            int mode = int(settings.display_mode);
            if (ImGui::Combo("##display", &mode, display_names, int(std::size(display_names)))) {
                settings.display_mode = app::DisplayMode(mode);
                edits.insert_or_assign("display_mode", display_keys[mode]);
            }
            ImGui::TableNextColumn();
            ImGui::TextDisabled("Resolution");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##resolution", resolution_name({settings.width, settings.height}).c_str())) {
                for (auto option : resolutions) {
                    const bool selected = option.width == settings.width && option.height == settings.height;
                    if (ImGui::Selectable(resolution_name(option).c_str(), selected)) {
                        settings.width = option.width;
                        settings.height = option.height;
                        edits.insert_or_assign("width", option.width);
                        edits.insert_or_assign("height", option.height);
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            Choice("Aspect ratio", "aspect_ratio", settings.aspect, kAspects, [](uint32_t v) {
                if (!v) return std::string("Match display");
                const std::string name = AspectName(v);
                return name.empty() ? std::to_string(v / 1000.0).substr(0, 5) + ":1" : name;
            });
#ifdef _WIN32
            Choice("Renderer", "renderer", settings.renderer, kRenderers,
                   [&](uint32_t v) { return std::string(renderer_names[v]); });
#endif
            Toggle("Remove cutscene bars", "remove_cutscene_bars", settings.remove_cutscene_bars);
            Choice("Frame cap", "fps_cap", settings.fps_cap, kFrameCaps,
                   [](uint32_t v) { return std::to_string(EffectiveFrameCap(v)) + " FPS"; });
            Toggle("VSync", "vsync", settings.vsync);
            Toggle("Low latency", "low_latency", settings.low_latency);
            ImGui::EndTable();
        }

        if (Section("IMAGE QUALITY")) {
            Choice("Render scale", "render_percent", settings.render_percent, kRenderPercents, percent);
            Choice("Upscaler", "upscaler", settings.upscaler, kUpscalers,
                   [&](uint32_t v) { return std::string(upscaler_names[v]); });
            Choice("Sharpening", "sharpening", settings.sharpening, kSharpenPercents,
                   [](uint32_t v) { return v ? std::to_string(v) + "%" : std::string("Off"); });
            Choice("Multisample AA", "msaa", settings.msaa, kMsaaSamples, multiple);
            Choice("Anisotropic filtering", "anisotropic_filtering", settings.aniso, kAnisoLevels,
                   [](uint32_t v) { return v ? std::to_string(v) + "x" : std::string("Game default"); });
            ImGui::TableNextColumn();
            Toggle("SMAA", "smaa", settings.smaa);
            Toggle("Temporal AA (experimental)", "taa", settings.taa);
            ImGui::EndTable();
        }
        if (settings.upscaler >= kFsr3 && !TemporalScale(settings.render_percent))
            ImGui::TextDisabled("Temporal upscaling needs 50%%, 58%%, 67%% or 100%% render scale.");

        if (Section("LIGHTING & EFFECTS")) {
            uint32_t ao_choice = AoMethodChoice(settings.ao, settings.ao_method);
            if (Choice("Ambient occlusion", "ao_choice", ao_choice, kAoMethodChoices, [](uint32_t v) {
                    constexpr const char *names[] = {"Off", "SSAO", "GTAO"};
                    return std::string(names[v]);
                })) {
                edits.erase("ao_choice");
                settings.ao = ApplyAoMethodChoice(ao_choice, settings.ao_quality, settings.ao_method);
                edits.insert_or_assign("ao", settings.ao);
                edits.insert_or_assign("ao_method", settings.ao_method);
            }
            if (Choice("Occlusion quality", "ao_quality", settings.ao_quality, kAoQualities,
                       [](uint32_t v) { return std::string(v == 1 ? "Half resolution" : "Full resolution"); }) &&
                settings.ao) {
                settings.ao = settings.ao_quality;
                edits.insert_or_assign("ao", settings.ao);
            }
            Choice("Bloom strength", "bloom_strength", settings.bloom_strength, kBloomStrengths, percent);
            Choice("Shadow resolution", "shadow_resolution", settings.shadow_resolution, kShadowLevels, level);
            Choice("Shadow softness", "shadow_softness", settings.shadow_softness, kShadowLevels, softness);
            ImGui::TableNextColumn();
            Toggle("Bloom", "bloom", settings.bloom);
            Toggle("Depth of field", "dof", settings.dof);
            Toggle("PC contrast", "pc_contrast", settings.pc_contrast);
            ImGui::EndTable();
        }

        if (Section("COLOUR")) {
            Choice("Gamma", "gamma", settings.gamma, kGammaPercents, percent);
            Choice("Brightness", "brightness", settings.brightness, kBrightnessPercents, percent);
            Choice("Contrast", "contrast", settings.contrast, kContrastPercents, percent);
            Choice("Saturation", "saturation", settings.saturation, kSaturationPercents, percent);
            ImGui::EndTable();
        }

        if (Section("HDR")) {
            Toggle("HDR output", "hdr", settings.hdr);
            ImGui::TableNextRow();
            ImGui::BeginDisabled(!settings.hdr);
            Choice("Paper white", "hdr_paper_white", settings.hdr_paper_white, kHdrPaperWhiteNits,
                   [](uint32_t v) { return std::to_string(v) + " nits"; });
            Choice("Peak brightness", "hdr_peak", settings.hdr_peak, kHdrPeakNits,
                   [](uint32_t v) { return v ? std::to_string(v) + " nits" : std::string("Display"); });
            Choice("Highlights", "hdr_highlights", settings.hdr_highlights, kHdrHighlightPercents, percent);
            ImGui::EndDisabled();
            ImGui::EndTable();
        }

        if (Section("THE CITY")) {
            Choice("Object detail", "object_detail", settings.object_detail, kObjectDetails,
                   [](uint32_t v) { return std::string(ObjectDetailName(v)); });
            Choice("Traffic density", "traffic_density", settings.traffic_density, kTrafficDensities,
                   [](uint32_t v) { return std::string(TrafficDensityName(v)); });
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("How many ambient cars there are. Raise it with Traffic distance to keep streets busy.");
            Choice("Traffic distance", "traffic_distance", settings.traffic_distance, kTrafficDistances,
                   [](uint32_t v) { return std::string(TrafficDistanceName(v)); });
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("How far away cars appear. The same cars spread over a wider area, "
                                  "so a longer distance looks emptier unless Traffic density is raised too.");
            Choice("Pedestrian limit", "ped_limit", settings.ped_limit, kPedLimits,
                   [](uint32_t v) { return std::string(PedLimitName(v)); });
            Choice("Crowd density", "ped_density", settings.ped_density, kPedDensities,
                   [](uint32_t v) { return std::string(PedDensityName(v)); });
            Choice("Pedestrian detail", "ped_detail", settings.ped_detail, kPedDetails,
                   [](uint32_t v) { return std::string(PedDetailName(v)); });
            Toggle("Building seam fix", "building_seam_fix", settings.building_seam_fix);
            ImGui::EndTable();
        }

        if (Section("TEXTURES")) {
            Toggle("Custom textures", "custom_textures", settings.custom_textures);
            Toggle("Dump textures", "dump_textures", settings.dump_textures);
            ImGui::BeginDisabled(!settings.dump_textures);
            Choice("Dump format", "texture_dump_format", settings.texture_dump_format, std::array<uint32_t, 2>{0, 1},
                   [](uint32_t v) { return std::string(v ? "DDS" : "PNG"); });
            ImGui::EndDisabled();
            ImGui::EndTable();
        }

        // text and subtitles only, voices stay English; the Fr/Es/It disc is the only one with other languages
        if (languages && Section("LANGUAGE")) {
            static constexpr std::array<uint32_t, 4> kLanguageCodes{1, 4, 5, 6};
            if (Choice("Text and subtitles", "language", settings.language, kLanguageCodes, [](uint32_t v) {
                    std::string name = app::LanguageName(v);
                    name[0] = char(std::toupper(static_cast<unsigned char>(name[0])));
                    return name;
                }))
                edits.insert_or_assign("language", std::string(app::LanguageName(settings.language)));
            ImGui::EndTable();
        }
    }

    // The Music tab: where replacement and extra songs come from.
    void DrawMusic() {
        if (Section("CUSTOM MUSIC")) ImGui::EndTable();
        ImGui::TextWrapped("Drop songs into the music folder (mp3, ogg, flac, wav or wma).");
        ImGui::BulletText("A file named after a Music Player song, 6000 to 6092, replaces that song.");
        ImGui::BulletText("Any other file is added as a new song on the CUSTOM MUSIC tab (up to 35).");
        ImGui::BulletText("Name files \"Band - Title.mp3\" to show the band.");
        ImGui::Spacing();

        if (Section("FOLDER")) ImGui::EndTable();
        const fs::path default_folder = app::ExeDir() / "music";
        const bool custom = !settings.music_folder.empty();
        ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Browse...Open folderUse default").x -
                                ImGui::GetStyle().ItemSpacing.x * 3 - ImGui::GetStyle().FramePadding.x * 6);
        const std::string hint = reinterpret_cast<const char *>(default_folder.u8string().c_str());
        if (ImGui::InputTextWithHint("##music_folder", hint.c_str(), &settings.music_folder))
            edits.insert_or_assign("music_folder", settings.music_folder);
        ImGui::SameLine();
        bool picking;
        {
            std::lock_guard lock(music_picker.mutex);
            picking = music_picker.pending;
        }
        ImGui::BeginDisabled(picking);
        if (ImGui::Button("Browse...")) {
            {
                std::lock_guard lock(music_picker.mutex);
                music_picker.pending = true;
            }
            SDL_ShowOpenFolderDialog(Picked, &music_picker, window, custom ? settings.music_folder.c_str() : hint.c_str(),
                                     false);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Open folder")) {
            fs::path folder = custom ? fs::path(reinterpret_cast<const char8_t *>(settings.music_folder.c_str()))
                                     : default_folder;
            if (folder.is_relative()) folder = app::ExeDir() / folder;
            std::error_code ec;
            fs::create_directories(folder, ec);
            const std::u8string path = folder.generic_u8string();
            std::u8string url = u8"file://";
            if (path.empty() || path.front() != u8'/') url += u8'/';
            url += path;
            SDL_OpenURL(reinterpret_cast<const char *>(url.c_str()));
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!custom);
        if (ImGui::Button("Use default")) {
            settings.music_folder.clear();
            edits.insert_or_assign("music_folder", settings.music_folder);
        }
        ImGui::EndDisabled();
        ImGui::TextDisabled(custom ? "Using your own folder." : "Using the music folder beside the game.");
    }

    void Volume(const char *label, const char *key, uint32_t &value) {
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", label);
        ImGui::SetNextItemWidth(-1);
        ImGui::PushID(key);
        int percent = int(value);
        if (ImGui::SliderInt("##volume", &percent, 0, 100, "%d%%")) {
            value = uint32_t(percent);
            edits.insert_or_assign(key, value);
        }
        ImGui::PopID();
    }

    // The Audio tab: the host's output mix and the game's volume categories.
    void DrawAudio() {
        constexpr const char *mode_names[] = {"Mono", "Stereo", "Dolby Pro Logic II"};
        if (Section("OUTPUT")) {
#ifdef _WIN32
            ImGui::TableNextColumn();
            ImGui::Checkbox("Force stereo output (2 ch, 48 kHz)", &settings.force_stereo_output);
            if (ImGui::IsItemEdited()) edits.insert_or_assign("force_stereo_output", settings.force_stereo_output);
            ImGui::TableNextColumn();
            ImGui::TextWrapped("If audio is very quiet or cuts out on a TV or soundbar, leave this on and choose "
                               "Stereo. Turn it off to let the TV or receiver handle Dolby.");
            ImGui::TableNextRow();
#endif
            ImGui::TableNextColumn();
            ImGui::TextDisabled("Output mode");
            ImGui::SetNextItemWidth(-1);
            int mode = int(settings.audio_mode);
            if (ImGui::Combo("##audio_mode", &mode, mode_names, int(std::size(mode_names)))) {
                settings.audio_mode = uint32_t(mode);
                edits.insert_or_assign("audio_mode", mode);
            }
            ImGui::TableNextColumn();
            ImGui::TextWrapped("Mono and Dolby Pro Logic II are encoded by the port from the game's 5.1 mix. "
                               "Dolby output needs a Pro Logic II decoder on the TV, soundbar or receiver.");
            ImGui::EndTable();
        }

        if (Section("VOLUME MIX")) {
            ImGui::TableNextColumn();
            ImGui::Checkbox("Force this mix", &settings.force_volume_mix);
            if (ImGui::IsItemEdited()) edits.insert_or_assign("force_volume_mix", settings.force_volume_mix);
            ImGui::TableNextColumn();
            ImGui::TextWrapped("Replaces the volume settings in the game's Audio menu. 100%% is full volume.");
            ImGui::EndTable();
        }

        if (Section("CATEGORIES")) {
            Volume("Master", "sound_volume", settings.sound_volume);
            Volume("Music", "music_volume", settings.music_volume);
            Volume("SFX", "sfx_volume", settings.sfx_volume);
            Volume("Ambient", "ambient_volume", settings.ambient_volume);
            Volume("Voice", "voice_volume", settings.voice_volume);
            Volume("Cinema", "cinema_volume", settings.cinema_volume);
            ImGui::EndTable();
        }
        ImGui::TextDisabled("These values are applied when the game starts.");
    }
};

// The Controls tab: keyboard and controller bindings, captured by pressing the new key or button.
struct Controls {
    input::KeyBinding keys[input::kKeyBindingCount];
    input::PadBinding pad[input::kPadBindingCount];
    // what is being captured: a keyboard row and slot, or a controller row; -1 none
    int key_row = -1, key_slot = 0, pad_row = -1;
    std::vector<SDL_Gamepad *> gamepads;

    Controls() {
        input::KeyBindings(keys);
        input::PadBindings(pad);
    }
    ~Controls() {
        for (auto *gamepad : gamepads) SDL_CloseGamepad(gamepad);
    }
    bool Capturing() const { return key_row >= 0 || pad_row >= 0; }

    // pushes the tables into the input module and the settings edits
    void Commit(toml::table &edits) {
        input::SetBindings(keys, pad);
        toml::table written;
        input::SaveBindings(&written);
        if (auto *controls = written["controls"].as_table()) edits.insert_or_assign("controls", *controls);
    }

    void SetKey(uint32_t key, toml::table &edits) {
        auto &slots = keys[key_row].keys;
        if (!key) {
            std::fill(std::begin(slots), std::end(slots), 0u);
            key_row = -1;
            Commit(edits);
            return;
        }
        for (auto& binding : slots) if (binding == key) binding = 0;
        // the new key becomes the main one (the one the prompts show); the others stay
        const auto end = std::remove(std::begin(slots), std::end(slots), 0u);
        std::fill(end, std::end(slots), 0u);
        std::move_backward(std::begin(slots), std::end(slots) - 1, std::end(slots));
        slots[0] = key;
        key_row = -1;
        Commit(edits);
    }

    // true when the launcher used the event for a capture
    bool Event(const SDL_Event &event, toml::table &edits) {
        if (event.type == SDL_EVENT_GAMEPAD_ADDED) {
            if (auto *gamepad = SDL_OpenGamepad(event.gdevice.which)) gamepads.push_back(gamepad);
            return false;
        }
        if (event.type == SDL_EVENT_GAMEPAD_REMOVED) {
            std::erase_if(gamepads, [&](SDL_Gamepad* gamepad) {
                if (SDL_GetGamepadID(gamepad) != event.gdevice.which) return false;
                SDL_CloseGamepad(gamepad);
                return true;
            });
            return false;
        }
        if (!Capturing()) return false;
        if (event.type == SDL_EVENT_KEY_DOWN) {
            const SDL_Keycode key = event.key.key;
            if (key == SDLK_ESCAPE) {
                key_row = pad_row = -1;
            } else if (key_row >= 0) {
                if (key == SDLK_DELETE) SetKey(0, edits);
                else if (key != SDLK_RETURN && key != SDLK_UP && key != SDLK_DOWN && key != SDLK_LEFT &&
                         key != SDLK_RIGHT && input::CanBindKey(uint32_t(key)))  // menu keys stay fixed
                    SetKey(uint32_t(key), edits);
            }
            return true;
        }
        if (event.type == SDL_EVENT_KEY_UP || event.type == SDL_EVENT_TEXT_INPUT) return true;
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && key_row >= 0) {
            constexpr int mouse[] = {-1, 0, 1, 2, 3, 4};  // SDL's 1-based buttons
            if (event.button.button <= 5) SetKey(input::MouseKey(mouse[event.button.button]), edits);
            return true;
        }
        if (pad_row >= 0) {
            int source = -1;
            if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && event.gbutton.button != SDL_GAMEPAD_BUTTON_GUIDE)
                source = event.gbutton.button;
            if (event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && event.gaxis.value > 20000 &&
                (event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || event.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER))
                source = input::kPadAxis | event.gaxis.axis;
            if (source >= 0) {
                // a swap: the input that had this button takes the old one, so nothing is left unbound
                for (auto &binding : pad)
                    if (binding.source == source) binding.source = pad[pad_row].source;
                pad[pad_row].source = source;
                pad_row = -1;
                Commit(edits);
                return true;
            }
        }
        return false;
    }

    static std::string KeyName(uint32_t key) {
        if (!key) return "";
        if (key & input::kMouseKey) {
            constexpr const char *names[] = {"Mouse left", "Mouse middle", "Mouse right", "Mouse 4", "Mouse 5"};
            return (key & 0xFF) < 5 ? names[key & 0xFF] : "?";
        }
        return SDL_GetKeyName(SDL_Keycode(key));
    }

    static std::string PadName(int source) {
        if (source & input::kPadAxis)
            return (source & 0xFF) == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? "Left trigger (LT / L2)" : "Right trigger (RT / R2)";
        switch (source) {
        case SDL_GAMEPAD_BUTTON_SOUTH: return "Bottom (A / Cross)";
        case SDL_GAMEPAD_BUTTON_EAST: return "Right (B / Circle)";
        case SDL_GAMEPAD_BUTTON_WEST: return "Left (X / Square)";
        case SDL_GAMEPAD_BUTTON_NORTH: return "Top (Y / Triangle)";
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return "Left bumper (LB / L1)";
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return "Right bumper (RB / R1)";
        case SDL_GAMEPAD_BUTTON_BACK: return "View / Share";
        case SDL_GAMEPAD_BUTTON_START: return "Menu / Options";
        case SDL_GAMEPAD_BUTTON_LEFT_STICK: return "Left stick click";
        case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return "Right stick click";
        case SDL_GAMEPAD_BUTTON_DPAD_UP: return "D-pad up";
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return "D-pad down";
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return "D-pad left";
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return "D-pad right";
        default: {
            const char *name = SDL_GetGamepadStringForButton(SDL_GamepadButton(source));
            return name ? name : "?";
        }
        }
    }

    // the same key on another row that can be active at the same time
    bool Clash(int row, uint32_t key) const {
        for (int other = 0; other < input::kKeyBindingCount; ++other) {
            if (other == row) continue;
            const auto a = keys[row].context, b = keys[other].context;
            if (a != input::Context::Any && b != input::Context::Any && a != b) continue;
            if (std::ranges::find(keys[other].keys, key) != std::end(keys[other].keys)) return true;
        }
        return false;
    }

    void Draw(float scale, toml::table &edits) {
        constexpr const char *actions[input::kKeyBindingCount] = {
            "Move forward", "Move back", "Move left", "Move right",
            "Light attack", "Grapple / human shield", "Heavy attack", "Jump / climb",
            "Accelerate", "Handbrake", "Brake / reverse", "Vehicle Y action",
            "Reload / block", "Use / enter vehicle", "Aim / lock on", "Fire",
            "Badge / horn / siren", "Left stick click", "Right stick click", "Pause"};
        constexpr struct { const char *title; int first, last; } groups[] = {
            {"KEYBOARD: MOVEMENT", 0, 3}, {"KEYBOARD: ON FOOT", 4, 7},
            {"KEYBOARD: DRIVING", 8, 11}, {"KEYBOARD: EVERYWHERE", 12, 18}};
        // Aim: the controller's right-stick response and speed, and the mouse's speeds. Each one is its
        // own settings.toml key, written when the choice changes.
        const auto percent = [](uint32_t v) { return std::to_string(v) + "%"; };
        const auto aim_choice = [&](const char *label, const char *key, uint32_t &value,
                                    std::span<const uint32_t> options, auto name, uint32_t low, uint32_t high,
                                    bool enabled) {
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", label);
            ImGui::SetNextItemWidth(-1);
            ImGui::PushID(key);
            ImGui::BeginDisabled(!enabled);
            const uint32_t shown = std::clamp(value, low, high);
            if (ImGui::BeginCombo("##aim", name(shown).c_str())) {
                for (const uint32_t option : options) {
                    if (ImGui::Selectable(name(option).c_str(), shown == option)) {
                        value = option;
                        edits.insert_or_assign(key, int64_t(option));
                    }
                    if (shown == option) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        };
        if (Graphics::Section("AIM")) {
            aim_choice("Controller aim response", "controller_aim_response", input::g_controller_aim_response,
                       kControllerAimResponses,
                       [](uint32_t v) { return std::string(ControllerAimResponseName(v)); }, 0, 1, true);
            aim_choice("Controller aim sensitivity", "controller_aim_sensitivity_percent",
                       input::g_controller_aim_sensitivity_percent, kControllerAimSensitivities, percent,
                       kControllerAimSensitivities[0],
                       kControllerAimSensitivities[std::size(kControllerAimSensitivities) - 1],
                       input::g_controller_aim_response == 0);
            ImGui::TableNextColumn();  // the third column stays empty: controller on one line, mouse on the next
            aim_choice("Mouse sensitivity", "mouse_sensitivity_percent", input::g_mouse_sensitivity_percent,
                       kMouseSensitivities, percent, kMinMouseSensitivityPercent,
                       kMaxMouseSensitivityPercent, true);
            aim_choice("Mouse vertical sensitivity", "mouse_vertical_sensitivity_percent",
                       input::g_mouse_vertical_sensitivity_percent, kMouseSensitivities, percent,
                       kMinMouseSensitivityPercent, kMaxMouseSensitivityPercent, true);
            ImGui::EndTable();
            ImGui::TextDisabled("Refined controller aim: one dead zone for the whole stick and a smoother curve. "
                                "Sensitivity applies to Refined only.");
        }

        ImGui::TextDisabled("Click a slot, then press the key or mouse button. Delete clears it, Escape cancels, "
                            "right-click clears. Enter, the arrows and Escape always work in menus.");
        for (const auto &group : groups) {
            if (!Graphics::Section(group.title)) continue;
            ImGui::EndTable();
            if (!ImGui::BeginTable(group.title, 5, ImGuiTableFlags_SizingStretchSame)) continue;
            ImGui::TableSetupColumn("action", ImGuiTableColumnFlags_WidthStretch, 1.6f);
            for (int row = group.first; row <= group.last; ++row) {
                ImGui::PushID(row);
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(actions[row]);
                for (int slot = 0; slot < 4; ++slot) {
                    ImGui::TableNextColumn();
                    ImGui::PushID(slot);
                    const uint32_t key = keys[row].keys[slot];
                    const bool capturing = key_row == row && key_slot == slot;
                    const bool clash = key && Clash(row, key);
                    if (clash) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{1, 0.45f, 0.45f, 1});
                    if (capturing) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.55f, 0.36f, 0.10f, 1});
                    const std::string text = capturing ? "Press a key..." : key ? KeyName(key) : "-";
                    if (ImGui::Button(text.c_str(), {-1, 0})) {
                        key_row = row;
                        key_slot = slot;
                        pad_row = -1;
                    }
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && key) {
                        key_row = row;
                        key_slot = slot;
                        SetKey(0, edits);
                    }
                    if (clash && ImGui::IsItemHovered()) ImGui::SetTooltip("Also bound to another action");
                    if (capturing) ImGui::PopStyleColor();
                    if (clash) ImGui::PopStyleColor();
                    ImGui::PopID();
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (ImGui::Button("Reset keyboard")) {
            input::DefaultKeys(keys);
            key_row = -1;
            Commit(edits);
        }

        constexpr const char *pad_actions[input::kPadBindingCount] = {
            "D-pad up (weapon)", "D-pad down (weapon)", "D-pad left", "D-pad right",
            "Pause", "Badge / horn / siren", "Left stick click", "Right stick click",
            "A: light attack / accelerate", "B: grapple / handbrake", "X: heavy attack / brake", "Y: jump / climb",
            "Black: reload / block", "White: use / enter vehicle", "Left trigger: aim / lock on", "Right trigger: fire"};
        if (Graphics::Section("CONTROLLER")) {
            ImGui::EndTable();
            ImGui::TextDisabled(gamepads.empty() ? "Connect a controller to rebind it."
                                                 : "Click an action, then press the button. Taking a button "
                                                   "from another action swaps the two.");
            if (ImGui::BeginTable("controller", 2, ImGuiTableFlags_SizingStretchSame)) {
                for (int row = 0; row < input::kPadBindingCount; ++row) {
                    ImGui::PushID(100 + row);
                    ImGui::TableNextColumn();
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(pad_actions[row]);
                    ImGui::TableNextColumn();
                    const bool capturing = pad_row == row;
                    if (capturing) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.55f, 0.36f, 0.10f, 1});
                    ImGui::BeginDisabled(gamepads.empty() || row == int(input::PadInput::Start));
                    if (ImGui::Button(capturing ? "Press a button..." : PadName(pad[row].source).c_str(), {-1, 0})) {
                        pad_row = row;
                        key_row = -1;
                    }
                    ImGui::EndDisabled();
                    if (capturing) ImGui::PopStyleColor();
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            if (ImGui::Button("Reset controller")) {
                input::DefaultPadBindings(pad);
                pad_row = -1;
                Commit(edits);
            }
        }
        (void)scale;
    }
};

SDL_Texture *LoadBanner(SDL_Renderer *renderer) {
    int width = 0, height = 0, channels = 0;
    auto *pixels = stbi_load_from_memory(kBanner, int(sizeof(kBanner)), &width, &height, &channels, 4);
    if (!pixels) return nullptr;
    auto *surface = SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_RGBA32, pixels, width * 4);
    auto *texture = surface ? SDL_CreateTextureFromSurface(renderer, surface) : nullptr;
    SDL_DestroySurface(surface);
    stbi_image_free(pixels);
    return texture;
}

ImFont *g_display = nullptr; // Impact-style headline font, matching the banner
ImFont *g_button = nullptr;

ImFont *AddFirst(std::initializer_list<const char *> names, float size) {
    auto &fonts = *ImGui::GetIO().Fonts;
    for (const char *name : names) {
#ifdef _WIN32
        const char *windows = std::getenv("WINDIR");
        if (!windows) break;
        const auto font = fs::path(windows) / "Fonts" / name;
#else
        const fs::path font = name;
#endif
        std::error_code ec;
        if (fs::is_regular_file(font, ec))
            if (auto *added = fonts.AddFontFromFileTTF(font.string().c_str(), size)) return added;
    }
    return nullptr;
}

void LoadFont(float size) {
#ifdef _WIN32
    auto *body = AddFirst({"bahnschrift.ttf", "segoeui.ttf"}, size);
    const std::initializer_list<const char *> display = {"impact.ttf", "bahnschrift.ttf"};
#else
    auto *body = AddFirst({"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
                           "/usr/share/fonts/noto/NotoSans-Regular.ttf"},
                          size);
    const std::initializer_list<const char *> display = {"/usr/share/fonts/truetype/dejavu/DejaVuSansCondensed-Bold.ttf",
                                                         "/usr/share/fonts/TTF/DejaVuSansCondensed-Bold.ttf"};
#endif
    if (!body) {
        ImFontConfig font;
        font.SizePixels = size;
        ImGui::GetIO().Fonts->AddFontDefault(&font);
    }
    g_display = AddFirst(display, size * 1.6f);
    g_button = AddFirst(display, size * 1.25f);
}

// Banner palette: night navy, sodium-lamp amber, cold street blue, siren red/blue.
constexpr ImU32 kAmber = IM_COL32(243, 178, 79, 255);
constexpr ImU32 kNight = IM_COL32(9, 11, 20, 255);

void Headline(const char *text) {
    if (g_display) ImGui::PushFont(g_display);
    ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    if (g_display) ImGui::PopFont();
}

// Hazard stripe, like police caution tape across a crime scene.
void Tape(ImDrawList *draw, ImVec2 a, ImVec2 b, float scale) {
    draw->AddRectFilled(a, b, kAmber);
    draw->PushClipRect(a, b, true);
    const float step = 14 * scale, h = b.y - a.y;
    for (float x = a.x - h; x < b.x + h; x += step * 2)
        draw->AddQuadFilled({x, b.y}, {x + h, a.y}, {x + h + step, a.y}, {x + step, b.y}, kNight);
    draw->PopClipRect();
}

// Night skyline with lit windows and sodium street glow.
void Backdrop(ImDrawList *draw, ImVec2 size, float scale) {
    draw->AddRectFilledMultiColor({0, 0}, size, IM_COL32(24, 33, 58, 255), IM_COL32(24, 33, 58, 255),
                                  IM_COL32(6, 7, 12, 255), IM_COL32(6, 7, 12, 255));
    draw->AddRectFilledMultiColor({0, size.y * 0.55f}, size, IM_COL32(243, 150, 60, 0), IM_COL32(243, 150, 60, 0),
                                  IM_COL32(243, 150, 60, 50), IM_COL32(243, 150, 60, 50));
    uint32_t seed = 0x7c15u;
    const auto rnd = [&] {
        seed = seed * 1664525u + 1013904223u;
        return (seed >> 8) / float(1u << 24);
    };
    for (float x = 0; x < size.x;) {
        const float w = (38 + rnd() * 70) * scale, h = (90 + rnd() * rnd() * 330) * scale;
        const float top = size.y - h;
        draw->AddRectFilled({x, top}, {x + w, size.y}, kNight);
        if (rnd() < 0.25f) draw->AddLine({x + w / 2, top}, {x + w / 2, top - 26 * scale}, kNight, 2 * scale);
        for (float wy = top + 8 * scale; wy < size.y - 6 * scale; wy += 11 * scale)
            for (float wx = x + 6 * scale; wx < x + w - 8 * scale; wx += 9 * scale)
                if (rnd() < 0.16f)
                    draw->AddRectFilled({wx, wy}, {wx + 4 * scale, wy + 5 * scale},
                                        rnd() < 0.8f ? IM_COL32(255, 210, 122, 150) : IM_COL32(159, 211, 255, 120));
        x += w + rnd() * 6 * scale;
    }
}

// Prints the window's logical, pixel and render sizes whenever any of them changes. The launcher runs
// before the game log opens, so this goes to stderr through SDL_Log. It shows scaling mismatches (KDE
// fractional scaling, a compositor resizing the window) that only appear on some desktops.
void LogWindowGeometry(SDL_Window *window, SDL_Renderer *renderer) {
    struct Geometry {
        int w = 0, h = 0, pw = 0, ph = 0, rw = 0, rh = 0;
        float density = 0, display = 0, rsx = 0, rsy = 0;
        bool operator==(const Geometry &) const = default;
    };
    static Geometry last;
    Geometry g;
    SDL_GetWindowSize(window, &g.w, &g.h);
    SDL_GetWindowSizeInPixels(window, &g.pw, &g.ph);
    SDL_GetCurrentRenderOutputSize(renderer, &g.rw, &g.rh);
    SDL_GetRenderScale(renderer, &g.rsx, &g.rsy);
    g.density = SDL_GetWindowPixelDensity(window);
    g.display = SDL_GetWindowDisplayScale(window);
    if (g == last) return;
    last = g;
    const auto &io = ImGui::GetIO();
    SDL_Log("launcher: window %dx%d, pixels %dx%d, render output %dx%d, render scale %.2fx%.2f, pixel density %.2f, "
            "display scale %.2f, ImGui display %.0fx%.0f at %.2fx%.2f, video %s, renderer %s",
            g.w, g.h, g.pw, g.ph, g.rw, g.rh, g.rsx, g.rsy, g.density, g.display, io.DisplaySize.x, io.DisplaySize.y,
            io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y, SDL_GetCurrentVideoDriver(),
            SDL_GetRendererName(renderer));
}

// TCNYC_LAUNCHER_SHOT=<file.bmp>: saves the backbuffer of the 30th frame, to check the layout on a
// desktop whose windows cannot be captured from outside (Wayland over SSH).
void SaveLauncherShot(SDL_Renderer *renderer) {
    static const char *path = DevEnv("TCNYC_LAUNCHER_SHOT");
    static int frame = 0;
    if (!path || ++frame != 30) return;
    if (SDL_Surface *shot = SDL_RenderReadPixels(renderer, nullptr)) {
        SDL_Log("launcher: saved %dx%d frame to %s: %s", shot->w, shot->h, path,
                SDL_SaveBMP(shot, path) ? "ok" : SDL_GetError());
        SDL_DestroySurface(shot);
    }
}

void Theme(float scale) {
    auto &style = ImGui::GetStyle();
    style.WindowPadding = {24, 18};
    style.FramePadding = {12, 8};
    style.ItemSpacing = {12, 10};
    style.CellPadding = {8, 4};
    style.FrameRounding = 2;
    style.ChildRounding = 3;
    style.PopupRounding = 6;
    style.TabRounding = 5;
    style.FrameBorderSize = 1;
    style.ScaleAllSizes(scale);

    auto *c = style.Colors;
    c[ImGuiCol_WindowBg] = {0, 0, 0, 0};
    c[ImGuiCol_ChildBg] = {0.035f, 0.043f, 0.075f, 0.88f};
    c[ImGuiCol_PopupBg] = {0.05f, 0.06f, 0.10f, 0.98f};
    c[ImGuiCol_Text] = {0.93f, 0.95f, 1.0f, 1};
    c[ImGuiCol_TextDisabled] = {0.50f, 0.65f, 0.79f, 1};
    c[ImGuiCol_Border] = c[ImGuiCol_Separator] = {0.95f, 0.70f, 0.31f, 0.35f};
    c[ImGuiCol_FrameBg] = c[ImGuiCol_Button] = c[ImGuiCol_Tab] = {0.094f, 0.13f, 0.23f, 1};
    c[ImGuiCol_FrameBgHovered] = c[ImGuiCol_ButtonHovered] = c[ImGuiCol_TabHovered] = {0.16f, 0.22f, 0.36f, 1};
    c[ImGuiCol_FrameBgActive] = c[ImGuiCol_ButtonActive] = c[ImGuiCol_HeaderActive] = {0.22f, 0.29f, 0.46f, 1};
    c[ImGuiCol_Header] = c[ImGuiCol_HeaderHovered] = {0.95f, 0.70f, 0.31f, 0.28f};
    c[ImGuiCol_TabSelected] = {0.55f, 0.36f, 0.10f, 1};
    c[ImGuiCol_TabSelectedOverline] = {1, 0.82f, 0.48f, 1};
    c[ImGuiCol_CheckMark] = c[ImGuiCol_NavHighlight] = {1.0f, 0.82f, 0.48f, 1};
}

bool DrawLauncher(SDL_Window *window, SDL_Renderer *renderer, float scale, app::GameSource& source) {
    LogLine("launcher: window open (v%s)", Updater::Current());
    const auto preferences = PreferencesPath();
    std::string iso, remembered_iso, notice, skip_version;
    bool error = false, check_updates = true, prefer_pal = false;
    try {
        const auto prefs = app::ReadSettingsTable(preferences);
        iso = remembered_iso = prefs["iso"].value_or(std::string{});
        check_updates = prefs["check_updates"].value_or(true);
        skip_version = prefs["skip_version"].value_or(std::string{});
        prefer_pal = prefs["prefer_pal"].value_or(false);
#ifndef TCNYC_PAL
        if (!prefs.contains("iso")) {
            const auto pal_prefs = app::ReadSettingsTable(preferences.parent_path() / "launcher-pal.toml");
            iso = pal_prefs["iso"].value_or(std::string{});
            prefer_pal = !iso.empty();
        }
#endif
    } catch (const std::exception &) {
        notice = "Could not read the saved disc path.";
        error = true;
    }
    // launcher.toml holds several values: update one and keep the rest
    const auto save_pref = [&](const char *key, auto value) {
        try {
            toml::table edits;
            edits.insert(key, value);
            app::MergeSettingsTable(preferences, edits);
        } catch (const std::exception &failure) {
            notice = failure.what();
            error = true;
        }
    };
    const auto detect = [&] {
        const auto path = FromUtf8(iso);
        return app::DetectGameSource(path, prefer_pal);
    };
    auto detected = detect();
    auto checked_path = iso;
    bool checked_preference = prefer_pal, refresh_source = false;
    auto editing_region = detected.region;
    if (editing_region == app::GameRegion::Unknown && prefer_pal) editing_region = app::GameRegion::Pal;
    toml::table defaults;
    app::FullSettingsTable(defaults);
    app::SelectGameSettings(editing_region);
    app::Load();
    Graphics graphics{app::Snapshot()};
    graphics.languages = editing_region == app::GameRegion::Pal;
    graphics.window = window;
    graphics.FindResolutions(window);

    Updater updater;
    updater.AdoptStaged();
    if (check_updates) updater.Check();

    Picker picker;
    SDL_Texture *banner = LoadBanner(renderer);
    Controls controls;
    bool play = false, quit = false;
    SDL_Event event{};
    bool waited_event = false;
    uint64_t last_activity = SDL_GetTicks();
    while (!play) {
        while (waited_event || SDL_PollEvent(&event)) {
            waited_event = false;
            last_activity = SDL_GetTicks();
            if (controls.Event(event, graphics.edits)) continue;
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) quit = true;
            if (event.type == SDL_EVENT_DROP_FILE && event.drop.data) {
                iso = event.drop.data;
                notice.clear();
            }
        }

        bool picking;
        {
            std::lock_guard lock(picker.mutex);
            picking = picker.pending;
            if (!picker.path.empty()) {
                iso = std::move(picker.path);
                picker.path.clear();
                notice.clear();
            }
            if (!picker.error.empty()) {
                notice = std::move(picker.error);
                picker.error.clear();
                error = true;
            }
        }
        {
            // The music folder picker's callback can outlive the tab and the
            // frame, so its result is collected here, not while drawing.
            std::lock_guard lock(graphics.music_picker.mutex);
            picking = picking || graphics.music_picker.pending;
            if (!graphics.music_picker.path.empty()) {
                graphics.settings.music_folder = std::move(graphics.music_picker.path);
                graphics.music_picker.path.clear();
                graphics.edits.insert_or_assign("music_folder", graphics.settings.music_folder);
            }
            if (!graphics.music_picker.error.empty()) {
                notice = std::move(graphics.music_picker.error);
                graphics.music_picker.error.clear();
                error = true;
            }
        }
        // Keep the callback state and its parent window alive until the native dialog returns.
        if (quit && !picking) {
            LogLine("launcher: closed without starting the game");
            break;
        }
        if (quit) {
            SDL_Delay(16);
            continue;
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        LogWindowGeometry(window, renderer);
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        Backdrop(ImGui::GetBackgroundDrawList(), ImGui::GetIO().DisplaySize, scale);
        ImGui::Begin("Launcher", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        if (banner) {
            float width = ImGui::GetContentRegionAvail().x;
            ImGui::Image((ImTextureID)(intptr_t)banner, {width, width / 4});
        } else {
            ImGui::TextUnformatted(kTitle);
        }

        // Update strip: shown only when there is something to say
        {
            const auto state = updater.state();
            const std::string latest = updater.latest();
            const bool show = (state == Updater::State::Available && latest != skip_version) ||
                              state == Updater::State::Downloading || state == Updater::State::Staging ||
                              state == Updater::State::Ready || state == Updater::State::Failed;
            if (show) {
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{0.95f, 0.70f, 0.31f, 0.16f});
                ImGui::PushStyleColor(ImGuiCol_Border, ImVec4{0.95f, 0.70f, 0.31f, 0.8f});
                ImGui::BeginChild("update", {0, 0}, kChildBorder | ImGuiChildFlags_AutoResizeY |
                                                        ImGuiChildFlags_AlwaysUseWindowPadding);
                switch (state) {
                case Updater::State::Available:
                    ImGui::AlignTextToFramePadding();
                    ImGui::Text("Update available: v%s (you have v%s)", latest.c_str(), Updater::Current());
                    ImGui::SameLine();
                    if (ImGui::Button("Update now")) updater.Download();
                    ImGui::SameLine();
                    if (ImGui::Button("What's new")) SDL_OpenURL(Updater::kReleasesPage);
                    ImGui::SameLine();
                    if (ImGui::Button("Skip this version")) {
                        skip_version = latest;
                        save_pref("skip_version", latest);
                    }
                    break;
                case Updater::State::Downloading: {
                    ImGui::AlignTextToFramePadding();
                    ImGui::Text("Downloading v%s", latest.c_str());
                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4{0.95f, 0.70f, 0.31f, 1});
                    ImGui::ProgressBar(updater.progress(), {ImGui::GetContentRegionAvail().x - 90 * scale, 0});
                    ImGui::PopStyleColor();
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel", {-1, 0})) updater.Cancel();
                    break;
                }
                case Updater::State::Staging:
                    ImGui::AlignTextToFramePadding();
                    ImGui::Text("Verifying and unpacking v%s...", latest.c_str());
                    break;
                case Updater::State::Ready:
                    ImGui::AlignTextToFramePadding();
                    ImGui::Text("v%s is ready to install. Restart the launcher.", latest.c_str());
                    ImGui::SameLine();
                    if (ImGui::Button("Restart now")) {
                        if (updater.Relaunch()) quit = true;
                        else notice = "Could not restart; open the game again to use the update.";
                    }
                    break;
                default:
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextColored({1, 0.49f, 0.51f, 1}, "%s", updater.error().c_str());
                    ImGui::SameLine();
                    if (ImGui::Button("Try again")) updater.Download();
                    ImGui::SameLine();
                    if (ImGui::Button("Releases page")) SDL_OpenURL(Updater::kReleasesPage);
                    break;
                }
                ImGui::EndChild();
                ImGui::PopStyleColor(2);
            }
        }
        const bool updating =
            updater.state() == Updater::State::Downloading || updater.state() == Updater::State::Staging;

        bool graphics_tab = false;
        const float panel_height = std::max(100.0f, ImGui::GetWindowHeight() - ImGui::GetCursorPosY() - 79 * scale);
        const float tape_y = ImGui::GetCursorScreenPos().y;
        Tape(ImGui::GetWindowDrawList(), {0, tape_y}, {ImGui::GetIO().DisplaySize.x, tape_y + 8 * scale}, scale);
        ImGui::Dummy({0, 10 * scale});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {18 * scale, 14 * scale});
        ImGui::BeginChild("Launcher panel", {0, panel_height - 18 * scale},
                          kChildBorder | ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PopStyleVar();
        if (ImGui::BeginTabBar("Launcher tabs")) {
            if (ImGui::BeginTabItem("  PLAY  ")) {
                ImGui::Spacing();
                Headline("CASE FILE: THE DISC");
                ImGui::TextDisabled("Choose or drop an Xbox disc image or extracted game folder.");
                ImGui::Spacing();
                ImGui::BeginDisabled(picking);
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 114 * scale);
                if (ImGui::InputTextWithHint("##iso", "Path to an Xbox ISO or extracted folder", &iso)) notice.clear();
                ImGui::SameLine();
                if (ImGui::Button("Browse...", {102 * scale, 0})) Browse(window, picker);
                if (ImGui::Button("Use local assets")) {
                    iso.clear();
                    refresh_source = true;
                    notice.clear();
                }
#ifndef TCNYC_PAL
                if (iso.empty() && (PalInstalled(app::GameRegion::Pal) || PalInstalled(app::GameRegion::PalEu))) {
                    ImGui::SameLine();
                    ImGui::Checkbox("Prefer PAL from local assets", &prefer_pal);
                }
#endif
                ImGui::EndDisabled();
                if (detected.region == app::GameRegion::Unknown)
                    ImGui::TextDisabled("%s", iso.empty() ? "Choose a supported Xbox ISO or extracted game folder."
                                                            : "This path has no supported Xbox disc.");
                else if (detected.region == app::GameRegion::PalEu)
                    ImGui::TextUnformatted("Detected: European PAL (other disc)");
                else
                    ImGui::Text("Detected: %s", detected.region == app::GameRegion::Pal ? "European PAL (Fr/Es/It)" : "US NTSC");
                ImGui::Spacing();
                if (ImGui::Checkbox("Check for updates when the launcher opens", &check_updates)) {
                    save_pref("check_updates", check_updates);
                    if (check_updates && updater.state() == Updater::State::Idle) updater.Check();
                }
                ImGui::SameLine();
                ImGui::TextDisabled("v%s", Updater::Current());
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("  GRAPHICS  ")) {
                ImGui::Spacing();
                Headline("THE STREETS: GRAPHICS");
                graphics_tab = true;
                ImGui::BeginChild("graphics scroll", {0, 0}, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
                graphics.Draw();
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("  MUSIC  ")) {
                ImGui::Spacing();
                Headline("THE RADIO: MUSIC");
                graphics_tab = true;
                ImGui::BeginChild("music scroll", {0, 0}, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
                graphics.DrawMusic();
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("  AUDIO  ")) {
                ImGui::Spacing();
                Headline("THE MIX: AUDIO");
                graphics_tab = true;
                ImGui::BeginChild("audio scroll", {0, 0}, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
                graphics.DrawAudio();
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("  CONTROLS  ")) {
                ImGui::Spacing();
                Headline("THE BADGE: CONTROLS");
                graphics_tab = true;
                ImGui::BeginChild("controls scroll", {0, 0}, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
                controls.Draw(scale, graphics.edits);
                ImGui::EndChild();
                ImGui::EndTabItem();
            } else {
                controls.key_row = controls.pad_row = -1;
            }
            ImGui::EndTabBar();
        }
        ImGui::EndChild();

        ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), ImGui::GetWindowHeight() - 69 * scale));
        const float width = ImGui::GetContentRegionAvail().x;
        ImGui::BeginGroup();
        const float actions_width = (graphics_tab ? 310 : 164) * scale;
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width - actions_width);
        ImGui::TextColored(error && !notice.empty() ? ImVec4{1, 0.49f, 0.51f, 1}
                                                    : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled),
                           "%s", notice.empty() ? "In-game graphics menu: F10 / Back + Start" : notice.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
        bool save = false;
        if (graphics_tab) {
            ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 296 * scale);
            ImGui::BeginDisabled(graphics.edits.empty());
            save = ImGui::Button("Save settings", {132 * scale, 38 * scale});
            ImGui::EndDisabled();
        }
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 152 * scale);
        ImGui::PushStyleColor(ImGuiCol_Button, {0.72f, 0.08f, 0.13f, 1});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.92f, 0.16f, 0.20f, 1});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, {0.55f, 0.05f, 0.10f, 1});
        ImGui::PushStyleColor(ImGuiCol_Border, {1, 0.82f, 0.48f, 0.9f});
        ImGui::BeginDisabled(picking || updating || detected.region == app::GameRegion::Unknown);
        if (g_button) ImGui::PushFont(g_button);
        const bool launch = ImGui::Button("HIT THE STREETS", {152 * scale, 38 * scale});
        if (g_button) ImGui::PopFont();
        ImGui::EndDisabled();
        ImGui::PopStyleColor(4);
        ImGui::End();

        try {
            if (launch || refresh_source || checked_path != iso || checked_preference != prefer_pal) {
                auto next = detect();
                if (next.region != app::GameRegion::Unknown && next.region != editing_region) {
                    graphics.Save();
                    editing_region = next.region;
                    app::SelectGameSettings(editing_region);
                    graphics.languages = editing_region == app::GameRegion::Pal;
                    app::Get() = app::Settings{};
                    for (const auto& option : app::kGame.options) {
                        if (option.flag) *option.flag = defaults[option.key].value_or(*option.flag);
                        else *option.value = defaults[option.key].value_or(*option.value);
                    }
                    input::LoadBindings(&defaults);
                    app::Load();
                    graphics.settings = app::Snapshot();
                }
                detected = std::move(next);
                refresh_source = false;
                checked_path = iso;
                checked_preference = prefer_pal;
            }
            if (save || launch) {
                graphics.Save();
                if (launch && detected.region != app::GameRegion::Unknown) {
#ifndef TCNYC_PAL
                    if (app::IsPal(detected.region) && !PalInstalled(detected.region))
                        throw std::runtime_error("The PAL executable is missing. Extract the complete release.");
#endif
                    if (iso != remembered_iso) save_pref("iso", iso);
                    save_pref("prefer_pal", app::IsPal(detected.region));
                    LogLine("play: region %d, source %s", int(detected.region), Utf8(detected.path).c_str());
                    bool started = true;
                    // Start the game before the launcher shuts SDL down. On Windows
                    // this is a child process; on Linux execv replaces this image.
                    // Either way, a launcher shutdown fault cannot stop the game.
                    const bool same_region = app::BuildRegion() == detected.region;
                    started = same_region ? LaunchGame(detected, app::Instance()) : LaunchPal(detected);
                    if (!started) {
                        notice = "Could not start the game. Extract the complete release and try again.";
                        error = true;
                    } else {
                        source = detected;
                        play = true;
                        LogLine("play: the game process has started");
                    }
                } else if (save) {
                    notice = "Settings saved.";
                    error = false;
                } else {
                    notice = "The selected game source is no longer available.";
                    error = true;
                }
            }
        } catch (const std::exception& failure) {
            notice = failure.what();
            error = true;
        }

        // TCNYC_LAUNCHER_TIMING (developer builds): the CPU time of each redraw, averaged into launcher.log
        static const bool timing = DevEnv("TCNYC_LAUNCHER_TIMING") != nullptr;
        const uint64_t draw_start = timing ? SDL_GetPerformanceCounter() : 0;
        ImGui::Render();
        // The SDL_Renderer backend leaves vertex positions in window units and expects the renderer scaled
        // to the backbuffer. With desktop scaling (KDE at 150%: a 760x620 window has a 1140x930 backbuffer)
        // an unscaled renderer drew the launcher into the top-left two-thirds of its window.
        const ImVec2 fb_scale = ImGui::GetIO().DisplayFramebufferScale;
        SDL_SetRenderScale(renderer, fb_scale.x > 0 ? fb_scale.x : 1.0f, fb_scale.y > 0 ? fb_scale.y : 1.0f);
        SDL_SetRenderDrawColor(renderer, 6, 7, 12, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SaveLauncherShot(renderer);
        SDL_RenderPresent(renderer);
        if (timing) {
            static double total_ms = 0, max_ms = 0;
            static int frames = 0;
            const double ms = double(SDL_GetPerformanceCounter() - draw_start) * 1000.0 /
                              double(SDL_GetPerformanceFrequency());
            total_ms += ms;
            max_ms = std::max(max_ms, ms);
            if (++frames == 50) {
                int w = 0, h = 0;
                SDL_GetCurrentRenderOutputSize(renderer, &w, &h);
                LogLine("launcher: %s %dx%d, 50 redraws: %.2f ms average, %.2f ms max", SDL_GetRendererName(renderer),
                        w, h, total_ms / frames, max_ms);
                total_ms = max_ms = 0;
                frames = 0;
            }
        }
        const bool busy = updating || updater.state() == Updater::State::Checking || picking || controls.Capturing();
        if (!play && !busy && SDL_GetTicks() - last_activity >= 250)
            waited_event = SDL_WaitEventTimeout(&event, 100);
        else if (!play)
            SDL_Delay(12);
    }
    SDL_DestroyTexture(banner);
    return play;
}

} // namespace

// The launcher's renderer: SDL's default (Direct3D 11 on Windows). TCNYC_LAUNCHER_RENDERER (developer builds)
// picks another SDL renderer for comparison, with TCNYC_LAUNCHER_TIMING to time the redraws.
//
// RivaTuner Statistics Server (MSI Afterburner) hooks every Direct3D device a process creates. In Beta 4.x the
// game ran on in the launcher's process after that device was gone, and with RTSS's default hooking the game
// never started (issues #17 and #22); the game now runs in its own process (LaunchGame). SDL's "software"
// renderer would give RTSS nothing to hook at all, but it measured 23.7 ms a redraw against 0.24 ms for
// Direct3D 11 at 760x620, and the animated background redraws continuously, so it is not the default.
SDL_Renderer *CreateLauncherRenderer(SDL_Window *window) {
    if (const char *forced = DevEnv("TCNYC_LAUNCHER_RENDERER")) {
        if (SDL_Renderer *renderer = SDL_CreateRenderer(window, forced)) return renderer;
        LogLine("launcher: the %s renderer failed (%s), using SDL's default", forced, SDL_GetError());
    }
    return SDL_CreateRenderer(window, nullptr);
}

bool Run(app::GameSource& source) {
    input::SetHints();
    constexpr auto subsystems = SDL_INIT_VIDEO | SDL_INIT_GAMEPAD;
    if (!SDL_InitSubSystem(subsystems)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, kTitle, SDL_GetError(), nullptr);
        return false;
    }
    SDL_Window *window = SDL_CreateWindow(kTitle, 760, 620, SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE);
    SDL_Renderer *renderer = window ? CreateLauncherRenderer(window) : nullptr;
    bool play = false;
    if (renderer) {
        LogLine("launcher: renderer %s", SDL_GetRendererName(renderer));
#ifdef _WIN32
        PinGraphicsModules();  // before SDL can unload them again (issue #22)
#endif
        const float pixel_density = std::max(1.0f, SDL_GetWindowPixelDensity(window));
        const float scale = std::max(1.0f, SDL_GetWindowDisplayScale(window) / pixel_density);
        if (scale > 1.0f) SDL_SetWindowSize(window, int(760 * scale), int(620 * scale));
        SDL_SetWindowMinimumSize(window, int(620 * scale), int(540 * scale));
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        auto &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        LoadFont(16 * scale);
        Theme(scale);
        ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
        ImGui_ImplSDLRenderer3_Init(renderer);
        play = DrawLauncher(window, renderer, scale, source);
        ImGui_ImplSDLRenderer3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
    } else {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, kTitle, SDL_GetError(), window);
    }
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    SDL_QuitSubSystem(subsystems);
    return play;
}

} // namespace launcher
