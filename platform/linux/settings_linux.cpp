// Native Linux settings: the same keys as runtime/app/settings.cpp, stored under the
// XDG config directory instead of beside the executable.
#include <algorithm>
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
#include "core/log.h"

namespace app {

namespace {

const char* kModes[] = {"fullscreen", "borderless", "windowed"};

std::filesystem::path ConfigDir() {
    if (const char* override = std::getenv("TCNYC_SETTINGS"); override && *override) {
        std::filesystem::path p(override);
        return p.has_filename() ? p.parent_path() : p;
    }
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return std::filesystem::path(xdg) / "TrueCrimeNYC";
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home) / ".config" / "TrueCrimeNYC";
    return ExeDir();
}

std::filesystem::path File() {
    if (const char* override = std::getenv("TCNYC_SETTINGS"); override && *override) {
        std::filesystem::path p(override);
        if (p.has_filename()) return p;
        return p / kGame.settings;
    }
    return ConfigDir() / kGame.settings;
}

} // namespace

void LogSystem() {
    LOGR("Linux: SDL3 host");
}

void Load() {
    SettingsLock guard;
    Settings& s = Get();
    toml::table t;
    try {
        t = toml::parse_file(File().string());
    } catch (...) {
        LOGR("Settings: no file at %s, using defaults", File().string().c_str());
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

    SetFrameCap(t["fps_cap"].value_or(FrameCap()));
    s.vsync = t["vsync"].value_or(s.vsync);
    s.aniso = t["anisotropic_filtering"].value_or(s.aniso);
    const auto samples = t["msaa"].value_or(t["msaa"].value_or(false) ? 4u : 0u);
    SetMsaaSamples(samples == 2 || samples == 4 || samples == 8 ? samples : 0);
    s.smaa = t["smaa"].value_or(s.smaa);
    s.bloom = t["bloom"].value_or(s.bloom);
    s.dof = t["dof"].value_or(s.dof);
    if (t["ao"].is_boolean()) {
        s.ao = t["ao"].value_or(false) ? 2u : 0u;  // older settings stored a boolean
    } else {
        s.ao = uint32_t(std::clamp<int64_t>(t["ao"].value_or(int64_t(s.ao)), 0, 2));
    }
    s.building_seam_fix = t["building_seam_fix"].value_or(s.building_seam_fix);
    s.object_detail = uint32_t(std::clamp<int64_t>(t["object_detail"].value_or(int64_t(s.object_detail)), 0, 3));
    s.pc_contrast = t["pc_contrast"].value_or(s.pc_contrast);
    s.custom_textures = t["custom_textures"].value_or(s.custom_textures);
    s.dump_textures = t["dump_textures"].value_or(s.dump_textures);
    s.texture_dump_format = std::min(t["texture_dump_format"].value_or(s.texture_dump_format), 1u);
    s.join_ip = t["join_ip"].value_or(s.join_ip);
    for (const Option& o : kGame.options) {
        if (o.flag) {
            auto flag = std::atomic_ref(*o.flag);
            flag.store(t[o.key].value_or(flag.load(std::memory_order_relaxed)), std::memory_order_relaxed);
        } else {
            *o.value = t[o.key].value_or(*o.value);
        }
    }
    if (kGame.load) kGame.load(&t);
}

namespace {

toml::table Table() {
    const Settings& s = Get();
    toml::table t{{"display_mode", kModes[uint32_t(s.display_mode)]},
                  {"width", int64_t(s.width)},
                  {"height", int64_t(s.height)},
                  {"render_percent", int64_t(s.render_percent)},
                  {"aspect_ratio", int64_t(s.aspect)},
                  {"fps_cap", int64_t(FrameCap())},
                  {"vsync", s.vsync},
                  {"anisotropic_filtering", int64_t(s.aniso)},
                  {"msaa", MsaaSamples()},
                  {"smaa", s.smaa},
                  {"bloom", s.bloom},
                  {"dof", s.dof},
                  {"ao", int64_t(s.ao)},
                  {"building_seam_fix", s.building_seam_fix},
                  {"object_detail", int64_t(s.object_detail)},
                  {"pc_contrast", s.pc_contrast},
                  {"custom_textures", s.custom_textures},
                  {"dump_textures", s.dump_textures},
                  {"texture_dump_format", int64_t(s.texture_dump_format)},
                  {"join_ip", s.join_ip}};
    for (const Option& o : kGame.options) {
        if (o.flag) t.insert(o.key, std::atomic_ref(*o.flag).load(std::memory_order_relaxed));
        else t.insert(o.key, int64_t(*o.value));
    }
    if (kGame.save) kGame.save(&t);
    return t;
}

} // namespace

void Save() {
    SettingsLock guard;
    std::ostringstream out;
    out << Table() << "\n";
    const std::string text = out.str();
    const std::filesystem::path path = File();

    std::error_code ec;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), ec);

    // a lock beside the settings file serializes instances of the same user
    const std::string lock_path = path.string() + ".lock";
    const int lock_fd = ::open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0666);
    if (lock_fd >= 0) ::flock(lock_fd, LOCK_EX);

    const std::string temp = path.string() + ".tmp." + std::to_string(::getpid());
    bool ok = false;
    if (FILE* f = std::fopen(temp.c_str(), "wb")) {
        ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
        ok = std::fflush(f) == 0 && ok;
        ok = ::fsync(::fileno(f)) == 0 && ok;
        std::fclose(f);
    }
    if (!ok) {
        LOGE("Settings: could not write %s", temp.c_str());
        std::remove(temp.c_str());
    } else if (std::rename(temp.c_str(), path.c_str()) != 0) {
        LOGE("Settings: could not replace %s (%d)", path.c_str(), errno);
        std::remove(temp.c_str());
    }
    if (lock_fd >= 0) {
        ::flock(lock_fd, LOCK_UN);
        ::close(lock_fd);
    }
}

void LogSettings() {
    std::ostringstream text;
    text << Table();
    std::istringstream lines(text.str());
    LOGR("Settings: %s", File().string().c_str());
    for (std::string line; std::getline(lines, line);)
        if (!line.empty()) LOGR("  %s", line.c_str());
}

} // namespace app
