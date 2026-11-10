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
#include <cstring>
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
    s.ao = g.ao;
    s.building_seam_fix = g.building_seam_fix;
    s.object_detail = std::atomic_ref(Get().object_detail).load(std::memory_order_relaxed);  // stored atomically
    s.pc_contrast = g.pc_contrast;
    s.custom_textures = g.custom_textures;
    s.dump_textures = g.dump_textures;
    s.texture_dump_format = g.texture_dump_format;
    s.hdr = g.hdr;
    s.hdr_paper_white = g.hdr_paper_white;
    s.hdr_peak = g.hdr_peak;
    s.hdr_highlights = g.hdr_highlights;
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
    if (s.width > 16384 || s.height > 16384 || !s.width || !s.height) s.width = s.height = 0;
    s.render_percent = std::clamp(s.render_percent, 25u, 200u);
    if (s.aspect && (s.aspect < 500 || s.aspect > 8000)) s.aspect = 0;

    SetFrameCap(t["fps_cap"].value_or(FrameCap()));
    s.vsync = t["vsync"].value_or(s.vsync);
    s.aniso = t["anisotropic_filtering"].value_or(s.aniso);
    // a sample count; older settings files stored msaa = true for 4x. A file
    // without the key keeps the built-in default (Settings::msaa).
    const auto msaa = t["msaa"];
    if (!msaa) {
        SetMsaaSamples(Settings{}.msaa);
    } else {
        const auto samples = msaa.value_or(msaa.value_or(false) ? 4u : 0u);
        SetMsaaSamples(samples == 2 || samples == 4 || samples == 8 ? samples : 0);
    }
    s.smaa = t["smaa"].value_or(s.smaa);
    s.bloom = t["bloom"].value_or(s.bloom);
    s.dof = t["dof"].value_or(s.dof);
    if (t["ao"].is_boolean()) {
        s.ao = t["ao"].value_or(false) ? 2u : 0u;  // older settings stored a boolean
    } else {
        s.ao = uint32_t(std::clamp<int64_t>(t["ao"].value_or(int64_t(s.ao)), 0, 2));
    }
    s.building_seam_fix = t["building_seam_fix"].value_or(s.building_seam_fix);
    // the LOD hooks read it on the game thread (lod.cpp)
    std::atomic_ref(s.object_detail)
        .store(uint32_t(std::clamp<int64_t>(t["object_detail"].value_or(int64_t(s.object_detail)), 0, 3)),
               std::memory_order_relaxed);
    s.pc_contrast = t["pc_contrast"].value_or(s.pc_contrast);
    s.custom_textures = t["custom_textures"].value_or(s.custom_textures);
    s.dump_textures = t["dump_textures"].value_or(s.dump_textures);
    s.texture_dump_format = std::min(t["texture_dump_format"].value_or(s.texture_dump_format), 1u);
    s.hdr = t["hdr"].value_or(s.hdr);
    s.hdr_paper_white = std::clamp<uint32_t>(t["hdr_paper_white"].value_or(s.hdr_paper_white), 80u, 400u);
    s.hdr_peak = std::clamp<uint32_t>(t["hdr_peak"].value_or(s.hdr_peak), 0u, 2000u);
    if (s.hdr_peak && s.hdr_peak < 400) s.hdr_peak = 400;
    s.hdr_highlights = std::min(t["hdr_highlights"].value_or(s.hdr_highlights), 100u);
    s.join_ip = t["join_ip"].value_or(s.join_ip);
    for (const Option& o : kGame.options) {
        if (o.flag) {
            auto flag = std::atomic_ref(*o.flag);
            flag.store(t[o.key].value_or(flag.load(std::memory_order_relaxed)), std::memory_order_relaxed);
        } else {
            *o.value = t[o.key].value_or(*o.value);
            // The field of view is a percentage with a fixed range; a
            // hand-edited file must not push the camera outside it.
            if (std::strcmp(o.key, "fov_percent") == 0) *o.value = std::clamp(*o.value, 50u, 200u);
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
                  {"object_detail", int64_t(std::atomic_ref(Get().object_detail).load(std::memory_order_relaxed))},
                  {"pc_contrast", s.pc_contrast},
                  {"custom_textures", s.custom_textures},
                  {"dump_textures", s.dump_textures},
                  {"texture_dump_format", int64_t(s.texture_dump_format)},
                  {"hdr", s.hdr},
                  {"hdr_paper_white", int64_t(s.hdr_paper_white)},
                  {"hdr_peak", int64_t(s.hdr_peak)},
                  {"hdr_highlights", int64_t(s.hdr_highlights)},
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
    // Separate game instances share this file: every save uses its own exclusively created temporary
    // file and publications are serialized by a named mutex. The last writer wins.
    // Build the snapshot under the settings lock, then write without holding it:
    // a slow or cross-process-locked save must not stall render-thread snapshots.
    std::string text;
    std::filesystem::path path;
    {
        SettingsLock guard;
        std::ostringstream out;
        out << Table() << "\n";
        text = out.str();
        path = File();
    }
    const std::wstring path_text = path.wstring();

    // Serialize publication between processes in this session.
    uint32_t hash = 2166136261u;
    for (wchar_t c : path_text) hash = (hash ^ uint32_t(c)) * 16777619u;
    const std::wstring mutex_name = L"Local\\TrueCrimeNYCSettings-" + std::to_wstring(hash);
    HANDLE mutex = CreateMutexW(nullptr, FALSE, mutex_name.c_str());
    const DWORD wait = mutex ? WaitForSingleObject(mutex, 5000) : WAIT_FAILED;
    // WAIT_ABANDONED also grants ownership: the previous owner died while holding the mutex.
    const bool locked = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
    if (mutex && !locked) LOGE("Settings: could not serialize publishing %s", path.string().c_str());

    std::filesystem::path temp;
    HANDLE file = INVALID_HANDLE_VALUE;
    static std::atomic<uint32_t> serial{0};
    const DWORD pid = GetCurrentProcessId();
    for (int attempt = 0; attempt < 64 && file == INVALID_HANDLE_VALUE; attempt++) {
        temp = path_text + L".tmp." + std::to_wstring(pid) + L"." + std::to_wstring(serial.fetch_add(1));
        file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_EXISTS) break;
    }
    if (file == INVALID_HANDLE_VALUE) {
        LOGE("Settings: could not create a temporary file for %s (error %lu)", path.string().c_str(), GetLastError());
        if (mutex) {
            if (locked) ReleaseMutex(mutex);
            CloseHandle(mutex);
        }
        return;
    }
    DWORD written = 0;
    const bool wrote = WriteFile(file, text.data(), DWORD(text.size()), &written, nullptr) &&
                       written == DWORD(text.size()) && FlushFileBuffers(file);
    CloseHandle(file);
    if (!wrote) {
        LOGE("Settings: could not write %s (error %lu)", temp.string().c_str(), GetLastError());
        DeleteFileW(temp.c_str());
        if (mutex) {
            if (locked) ReleaseMutex(mutex);
            CloseHandle(mutex);
        }
        return;
    }
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        LOGE("Settings: could not replace %s (error %lu)", path.string().c_str(), GetLastError());
        DeleteFileW(temp.c_str());
    }
    if (mutex) {
        if (locked) ReleaseMutex(mutex);
        CloseHandle(mutex);
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
    {
        std::lock_guard<std::mutex> guard(lock);
        if (now == key && mode.height) return mode;
    }
    const Mode out = s.width && s.height ? Mode{s.width, s.height} : Desktop();
    // the settings may have changed while Desktop() ran: keep that result out of the cache
    const Settings after = Snapshot();
    if (after.width != s.width || after.height != s.height) return out;
    std::lock_guard<std::mutex> guard(lock);
    key = now;
    mode = out;
    return out;
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
