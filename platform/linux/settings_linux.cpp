// Native Linux settings: the same keys as runtime/app/settings.cpp, stored under the
// XDG config directory instead of beside the executable.
#include <algorithm>
#include "core/dev_env.h"
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <toml++/toml.hpp>

#include "app/app.h"
#include "app/settings_store.h"
#include "core/log.h"

namespace app {

namespace {

toml::table Table();
toml::table g_loaded_settings;
std::mutex g_save_lock;

const char* kModes[] = {"fullscreen", "borderless", "windowed"};

std::filesystem::path ConfigDir() {
    if (const char* override = DevEnv("TCNYC_SETTINGS"); override && *override) {
        std::filesystem::path p(override);
        return p.has_filename() ? p.parent_path() : p;
    }
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return std::filesystem::path(xdg) / "TrueCrimeNYC";
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home) / ".config" / "TrueCrimeNYC";
    return ExeDir();
}

} // namespace

std::filesystem::path SettingsPath() {
    if (const char* override = DevEnv("TCNYC_SETTINGS"); override && *override) {
        std::filesystem::path p(override);
        if (p.has_filename()) return p;
        return p / SettingsFile();
    }
    return ConfigDir() / SettingsFile();
}

uint32_t AudioMode() {
    return std::atomic_ref(Get().audio_mode).load(std::memory_order_relaxed);
}

bool ForceStereoOutput() {
    return std::atomic_ref(Get().force_stereo_output).load(std::memory_order_relaxed);
}

void LogSystem() {
    LOGR("Linux: SDL3 host");
}

void Load() {
    SettingsLock guard;
    Settings& s = Get();
    toml::table t;
    try {
        t = toml::parse_file(SettingsPath().u8string());
    } catch (...) {
        LOGR("Settings: no readable file at %s, using defaults", SettingsPath().string().c_str());
        g_loaded_settings = Table();
        return;
    }
    std::string mode = t["display_mode"].value_or(std::string(kModes[uint32_t(s.display_mode)]));
    for (uint32_t i = 0; i < 3; i++)
        if (mode == kModes[i]) s.display_mode = DisplayMode(i);
    s.width = t["width"].value_or(s.width);
    s.height = t["height"].value_or(s.height);
    s.render_percent = t["render_percent"].value_or(s.render_percent);
    s.aspect = t["aspect_ratio"].value_or(s.aspect);
    if (s.width > 16384 || s.height > 16384 || !s.width || !s.height) s.width = s.height = 0;
    s.render_percent = std::clamp(s.render_percent, 25u, 200u);
    if (s.aspect && (s.aspect < 500 || s.aspect > 8000)) s.aspect = 0;
    if (uint32_t code = LanguageCode(t["language"].value_or(std::string(LanguageName(s.language))))) s.language = code;

    s.audio_mode = uint32_t(std::clamp<int64_t>(t["audio_mode"].value_or(int64_t(s.audio_mode)), 0, 2));
    s.force_stereo_output = t["force_stereo_output"].value_or(s.force_stereo_output);
    s.force_volume_mix = t["force_volume_mix"].value_or(s.force_volume_mix);
    const auto volume = [&](const char* key, uint32_t value) {
        return uint32_t(std::clamp<int64_t>(t[key].value_or(int64_t(value)), 0, 100));
    };
    s.sound_volume = volume("sound_volume", s.sound_volume);
    s.music_volume = volume("music_volume", s.music_volume);
    s.sfx_volume = volume("sfx_volume", s.sfx_volume);
    s.ambient_volume = volume("ambient_volume", s.ambient_volume);
    s.voice_volume = volume("voice_volume", s.voice_volume);
    s.cinema_volume = volume("cinema_volume", s.cinema_volume);

    SetFrameCap(t["fps_cap"].value_or(FrameCap()));
    s.vsync = t["vsync"].value_or(s.vsync);
    s.low_latency = t["low_latency"].value_or(s.low_latency);
    s.taa = t["taa"].value_or(s.taa);
    // 0 bilinear, 1 FSR 1, 2 FSR 3.1, 3 DLSS; without DLSS (3) uses FSR 3.1; FSR 4.1 (4, Windows only) is FSR 3.1 here
    s.upscaler = uint32_t(std::clamp<int64_t>(t["upscaler"].value_or(int64_t(s.upscaler)), 0, 4));
    if (s.upscaler == 4) s.upscaler = 2;
#ifndef TCNYC_HAVE_DLSS
    if (s.upscaler == 3) s.upscaler = 2;
#endif
    s.sharpening = uint32_t(std::clamp<int64_t>(t["sharpening"].value_or(int64_t(s.sharpening)), 0, 100));
    s.aniso = t["anisotropic_filtering"].value_or(s.aniso);
    // as settings.cpp: a file without the key keeps the built-in default (Settings::msaa); older
    // files stored msaa = true for 4x
    SetMsaaSamples(MsaaSamplesFromTable(t, Settings{}.msaa));
    s.smaa = t["smaa"].value_or(s.smaa);
    s.bloom = t["bloom"].value_or(s.bloom);
    s.bloom_strength = uint32_t(std::clamp<int64_t>(t["bloom_strength"].value_or(int64_t(s.bloom_strength)), 50, 400));
    s.shadow_resolution = uint32_t(std::clamp<int64_t>(t["shadow_resolution"].value_or(int64_t(s.shadow_resolution)), 0, 2));
    s.shadow_softness = uint32_t(std::clamp<int64_t>(t["shadow_softness"].value_or(int64_t(s.shadow_softness)), 0, 2));
    s.dof = t["dof"].value_or(s.dof);
    LoadOcclusionSettings(s, t);
    // read by the city ring on the game thread (city_grid.cpp)
    std::atomic_ref(s.city_range).store(0, std::memory_order_relaxed);
    std::atomic_ref(s.city_inner_detail).store(
        uint32_t(std::clamp<int64_t>(t["city_inner_detail"].value_or(int64_t(s.city_inner_detail)), 1, 3)),
        std::memory_order_relaxed);
    std::atomic_ref(s.city_outer_detail).store(
        uint32_t(std::clamp<int64_t>(t["city_outer_detail"].value_or(int64_t(s.city_outer_detail)), 0, 3)),
        std::memory_order_relaxed);
    s.building_seam_fix = t["building_seam_fix"].value_or(s.building_seam_fix);
    // the LOD hooks read it on the game thread (lod.cpp)
    std::atomic_ref(s.object_detail)
        .store(uint32_t(std::clamp<int64_t>(t["object_detail"].value_or(int64_t(s.object_detail)), 0, 3)),
               std::memory_order_relaxed);
    std::atomic_ref(s.traffic_density)
        .store(uint32_t(std::clamp<int64_t>(t["traffic_density"].value_or(int64_t(s.traffic_density)), 0, 5)),
               std::memory_order_relaxed);
    std::atomic_ref(s.traffic_distance)
        .store(uint32_t(std::clamp<int64_t>(t["traffic_distance"].value_or(int64_t(s.traffic_distance)), 0, 2)),
               std::memory_order_relaxed);
    std::atomic_ref(s.ped_density)
        .store(uint32_t(std::clamp<int64_t>(t["ped_density"].value_or(int64_t(s.ped_density)), 0, 3)),
               std::memory_order_relaxed);
    std::atomic_ref(s.ped_limit)
        .store(uint32_t(std::clamp<int64_t>(t["ped_limit"].value_or(int64_t(s.ped_limit)), 0, 6)),
               std::memory_order_relaxed);
    // read on the game thread (ped_detail.cpp)
    std::atomic_ref(s.ped_detail)
        .store(uint32_t(std::clamp<int64_t>(t["ped_detail"].value_or(int64_t(s.ped_detail)), 0, 3)),
               std::memory_order_relaxed);
    s.pc_contrast = t["pc_contrast"].value_or(s.pc_contrast);
    s.gamma = uint32_t(std::clamp<int64_t>(t["gamma"].value_or(int64_t(s.gamma)), 50, 250));
    s.brightness = uint32_t(std::clamp<int64_t>(t["brightness"].value_or(int64_t(s.brightness)), 50, 150));
    s.contrast = uint32_t(std::clamp<int64_t>(t["contrast"].value_or(int64_t(s.contrast)), 50, 150));
    s.saturation = uint32_t(std::clamp<int64_t>(t["saturation"].value_or(int64_t(s.saturation)), 0, 200));
    s.custom_textures = t["custom_textures"].value_or(s.custom_textures);
    s.dump_textures = t["dump_textures"].value_or(s.dump_textures);
    s.texture_dump_format = std::min(t["texture_dump_format"].value_or(s.texture_dump_format), 1u);
    s.hdr = t["hdr"].value_or(s.hdr);
    s.hdr_paper_white = std::clamp<uint32_t>(t["hdr_paper_white"].value_or(s.hdr_paper_white), 80u, 400u);
    s.hdr_peak = std::clamp<uint32_t>(t["hdr_peak"].value_or(s.hdr_peak), 0u, 2000u);
    if (s.hdr_peak && s.hdr_peak < 400) s.hdr_peak = 400;
    s.hdr_highlights = std::min(t["hdr_highlights"].value_or(s.hdr_highlights), 100u);
    s.join_ip = t["join_ip"].value_or(s.join_ip);
    s.music_folder = t["music_folder"].value_or(s.music_folder);
    for (const Option& o : kGame.options) {
        if (o.flag) {
            auto flag = std::atomic_ref(*o.flag);
            flag.store(t[o.key].value_or(flag.load(std::memory_order_relaxed)), std::memory_order_relaxed);
        } else {
            *o.value = t[o.key].value_or(*o.value);
            if (std::strcmp(o.key, "fov_percent") == 0) *o.value = std::clamp(*o.value, 50u, 200u);
            if (std::strcmp(o.key, "controller_aim_response") == 0) *o.value = std::min(*o.value, 1u);
            if (std::strcmp(o.key, "controller_aim_sensitivity_percent") == 0)
                *o.value = std::clamp(*o.value, 100u, 400u);
        }
    }
    if (kGame.load) kGame.load(&t);
    g_loaded_settings = Table();
}

namespace {

toml::table Table() {
    const Settings& s = Get();
    toml::table t{{"display_mode", kModes[uint32_t(s.display_mode)]},
                  {"width", int64_t(s.width)},
                  {"height", int64_t(s.height)},
                  {"render_percent", int64_t(s.render_percent)},
                  {"aspect_ratio", int64_t(s.aspect)},
                  {"language", LanguageName(s.language)},
                  {"audio_mode", int64_t(s.audio_mode)},
                  {"force_stereo_output", s.force_stereo_output},
                  {"force_volume_mix", s.force_volume_mix},
                  {"sound_volume", int64_t(s.sound_volume)},
                  {"music_volume", int64_t(s.music_volume)},
                  {"sfx_volume", int64_t(s.sfx_volume)},
                  {"ambient_volume", int64_t(s.ambient_volume)},
                  {"voice_volume", int64_t(s.voice_volume)},
                  {"cinema_volume", int64_t(s.cinema_volume)},
                  {"fps_cap", int64_t(FrameCap())},
                  {"vsync", s.vsync},
                  {"low_latency", s.low_latency},
                  {"taa", s.taa},
                  {"upscaler", int64_t(s.upscaler)},
                  {"sharpening", int64_t(s.sharpening)},
                  {"anisotropic_filtering", int64_t(s.aniso)},
                  {"msaa", MsaaSamples()},
                  {"smaa", s.smaa},
                  {"bloom", s.bloom},
                  {"bloom_strength", int64_t(s.bloom_strength)},
                  {"shadow_resolution", int64_t(s.shadow_resolution)},
                  {"shadow_softness", int64_t(s.shadow_softness)},
                  {"dof", s.dof},
                  {"ao", int64_t(s.ao)},
                  {"ao_method", int64_t(s.ao_method)},
                  {"ao_quality", int64_t(s.ao_quality)},
                  {"building_seam_fix", s.building_seam_fix},
                  {"object_detail", int64_t(s.object_detail)},
                  {"ped_detail", int64_t(s.ped_detail)},
                  {"ped_limit", int64_t(s.ped_limit)},
                  {"ped_density", int64_t(s.ped_density)},
                  {"traffic_density", int64_t(s.traffic_density)},
                  {"traffic_distance", int64_t(s.traffic_distance)},
                  {"pc_contrast", s.pc_contrast},
                  {"gamma", int64_t(s.gamma)},
                  {"brightness", int64_t(s.brightness)},
                  {"contrast", int64_t(s.contrast)},
                  {"saturation", int64_t(s.saturation)},
                  {"custom_textures", s.custom_textures},
                  {"dump_textures", s.dump_textures},
                  {"texture_dump_format", int64_t(s.texture_dump_format)},
                  {"hdr", s.hdr},
                  {"hdr_paper_white", int64_t(s.hdr_paper_white)},
                  {"hdr_peak", int64_t(s.hdr_peak)},
                  {"hdr_highlights", int64_t(s.hdr_highlights)},
                  {"join_ip", s.join_ip},
                  {"music_folder", s.music_folder}};
    for (const Option& o : kGame.options) {
        if (o.flag) t.insert(o.key, std::atomic_ref(*o.flag).load(std::memory_order_relaxed));
        else t.insert(o.key, int64_t(*o.value));
    }
    if (kGame.save) kGame.save(&t);
    return t;
}

} // namespace

void FullSettingsTable(toml::table& out) {
    SettingsLock guard;
    out = Table();
}

bool Save() {
    try {
        const std::lock_guard save_guard(g_save_lock);
        const auto path = SettingsPath();
        SettingsLock guard;
        PublishSettingsAsync(path, g_loaded_settings, Table());
        return true;
    } catch (const std::exception& failure) {
        LOGE("Settings: %s", failure.what());
        return false;
    }
}

void LogSettings() {
    std::ostringstream text;
    text << Table();
    std::istringstream lines(text.str());
    LOGR("Settings: %s", SettingsPath().string().c_str());
    for (std::string line; std::getline(lines, line);)
        if (!line.empty()) LOGR("  %s", line.c_str());
}

} // namespace app
