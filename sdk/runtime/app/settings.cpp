// the settings file beside the exe
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <mutex>
#include <sstream>

#include <toml++/toml.hpp>

#include "app/app.h"
#include "core/log.h"

namespace app {

namespace {

const char* kModes[] = {"fullscreen", "borderless", "windowed"};

std::filesystem::path File() { return ExeDir() / kGame.settings; }
std::recursive_mutex g_settings_lock;

}  // namespace

uint32_t FrameCap() {
    return std::atomic_ref(Get().fps_cap).load(std::memory_order_relaxed);
}

void SetFrameCap(uint32_t cap) {
    std::atomic_ref(Get().fps_cap).store(cap, std::memory_order_relaxed);
}

uint32_t MsaaSamples() {
    return std::atomic_ref(Get().msaa).load(std::memory_order_relaxed);
}

void SetMsaaSamples(uint32_t samples) {
    std::atomic_ref(Get().msaa).store(samples, std::memory_order_relaxed);
}

Settings& Get() {
    static Settings s;
    return s;
}

Settings Snapshot() {
    std::lock_guard<std::recursive_mutex> guard(g_settings_lock);
    const Settings& g = Get();
    Settings s;
    s.display_mode = g.display_mode;
    s.width = g.width;
    s.height = g.height;
    s.render_percent = g.render_percent;
    s.aspect = g.aspect;
    s.vsync = g.vsync;
    s.aniso = g.aniso;
    s.smaa = g.smaa;
    s.bloom = g.bloom;
    s.dof = g.dof;
    s.pc_contrast = g.pc_contrast;
    s.custom_textures = g.custom_textures;
    s.dump_textures = g.dump_textures;
    s.texture_dump_format = g.texture_dump_format;
    s.join_ip = g.join_ip;
    // these two are written atomically, so read them atomically
    s.fps_cap = FrameCap();
    s.msaa = MsaaSamples();
    return s;
}

SettingsLock::SettingsLock() { g_settings_lock.lock(); }
SettingsLock::~SettingsLock() { g_settings_lock.unlock(); }

void Load() {
    SettingsLock guard;
    Settings& s = Get();
    toml::table t;
    try {
        t = toml::parse_file(File().string());
    } catch (...) {
        return;
    }
    std::string mode = t["display_mode"].value_or(std::string(kModes[uint32_t(s.display_mode)]));
    for (uint32_t i = 0; i < 3; i++)
        if (mode == kModes[i]) s.display_mode = DisplayMode(i);
    s.width = t["width"].value_or(s.width);
    s.height = t["height"].value_or(s.height);
    s.render_percent = t["render_percent"].value_or(s.render_percent);
    s.aspect = t["aspect_ratio"].value_or(s.aspect);
    SetFrameCap(t["fps_cap"].value_or(FrameCap()));
    s.vsync = t["vsync"].value_or(s.vsync);
    s.aniso = t["anisotropic_filtering"].value_or(s.aniso);
    // a sample count; older settings files stored msaa = true for 4x
    const auto samples = t["msaa"].value_or(t["msaa"].value_or(false) ? 4u : 0u);
    SetMsaaSamples(samples == 2 || samples == 4 || samples == 8 ? samples : 0);
    s.smaa = t["smaa"].value_or(s.smaa);
    s.bloom = t["bloom"].value_or(s.bloom);
    s.dof = t["dof"].value_or(s.dof);
    s.pc_contrast = t["pc_contrast"].value_or(s.pc_contrast);
    s.custom_textures = t["custom_textures"].value_or(s.custom_textures);
    s.dump_textures = t["dump_textures"].value_or(s.dump_textures);
    s.texture_dump_format = std::min(t["texture_dump_format"].value_or(s.texture_dump_format), 1u);
    s.join_ip = t["join_ip"].value_or(s.join_ip);
    for (const Option& o : kGame.options) {
        if (o.flag) {
            auto flag = std::atomic_ref(*o.flag);
            flag.store(t[o.key].value_or(flag.load(std::memory_order_relaxed)), std::memory_order_relaxed);
        }
        else *o.value = t[o.key].value_or(*o.value);
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

}  // namespace

void Save() {
    // Two menus can save at once, and a failed write must not leave the live file truncated.
    SettingsLock guard;
    std::ostringstream out;
    out << Table() << "\n";
    const std::string text = out.str();
    const std::filesystem::path path = File();
    const std::filesystem::path temp = path.wstring() + L".tmp";
    {
        std::ofstream f(temp, std::ios::binary | std::ios::trunc);
        if (!f) {
            LOGE("Settings: could not open %s", temp.string().c_str());
            return;
        }
        f.write(text.data(), std::streamsize(text.size()));
        f.flush();
        if (!f) {
            LOGE("Settings: could not write %s", temp.string().c_str());
            f.close();
            DeleteFileW(temp.c_str());
            return;
        }
        f.close();
        if (f.fail()) {
            LOGE("Settings: could not close %s", temp.string().c_str());
            DeleteFileW(temp.c_str());
            return;
        }
    }
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        LOGE("Settings: could not replace %s (error %lu)", path.string().c_str(), GetLastError());
        DeleteFileW(temp.c_str());
    }
}

void LogSettings() {
    std::ostringstream text;
    text << Table();
    std::istringstream lines(text.str());
    LOGR("Settings:");
    for (std::string line; std::getline(lines, line);)
        if (!line.empty()) LOGR("  %s", line.c_str());
}

Mode Desktop() {
    HMONITOR mon = MonitorFromWindow(static_cast<HWND>(Window()), MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (GetMonitorInfoW(mon, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_REGISTRY_SETTINGS, &dm))
        return {dm.dmPelsWidth, dm.dmPelsHeight};
    return {uint32_t(GetSystemMetrics(SM_CXSCREEN)), uint32_t(GetSystemMetrics(SM_CYSCREEN))};
}

Mode Output() {
    const Settings s = Snapshot();
    if (s.width && s.height) return {s.width, s.height};
    return Desktop();
}

// Output for every frame: the desktop mode lookup reads the registry, so it runs again only when the resolution
// setting or the window's monitor changes
Mode CachedOutput() {
    struct Key {
        uint32_t width;
        uint32_t height;
        HMONITOR monitor;
        bool operator==(const Key&) const = default;
    };
    static std::mutex lock;
    static Key key{};
    static Mode mode{};
    const Settings s = Snapshot();
    Key now{s.width, s.height, MonitorFromWindow(static_cast<HWND>(Window()), MONITOR_DEFAULTTOPRIMARY)};
    Mode out = s.width && s.height ? Mode{s.width, s.height} : Desktop();
    std::lock_guard<std::mutex> guard(lock);
    if (!(now == key) || !mode.height) {
        key = now;
        mode = out;
    }
    return mode;
}

float AspectRatio() {
    const Settings s = Snapshot();
    if (uint32_t a = s.aspect) return float(a) / 1000.0f;
    Mode m = CachedOutput();
    return m.width && m.height ? float(m.width) / float(m.height) : 16.0f / 9.0f;
}

// 3:2 or wider: the console reports a widescreen TV
bool Widescreen() { return AspectRatio() >= 1.5f; }

float RenderStretch(uint32_t guest_width, uint32_t guest_height) {
    if (!guest_width || !guest_height) return 1.0f;
    return AspectRatio() * float(guest_height) / float(guest_width);
}

float RenderScale(uint32_t guest_height) {
    // the frame fills the output's height, or its width when the frame is wider than the output
    const Settings s = Snapshot();
    Mode m = CachedOutput();
    float shown = float(m.height);
    if (m.width && float(m.width) < shown * AspectRatio()) shown = float(m.width) / AspectRatio();
    float h = shown * (s.render_percent / 100.0f);
    return guest_height ? std::max(h / guest_height, 0.1f) : 1.0f;
}

}  // namespace app
