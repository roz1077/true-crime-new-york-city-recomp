#pragma once
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace input {
struct Pad;
}

// the host side: window, settings
namespace app {

enum class DisplayMode : uint32_t { Fullscreen, Borderless, Windowed };
inline constexpr uint32_t kFpsCapRefresh = 1;  // fps_cap: the monitor's refresh rate

struct Settings {
    DisplayMode display_mode = DisplayMode::Borderless;
    uint32_t width = 0, height = 0;  // 0 is the desktop mode
    uint32_t render_percent = 100;   // of the displayed frame's height
    uint32_t aspect = 0;             // the frame's shape, width / height x 1000; 0 the output's
    uint32_t fps_cap = 60;           // 0 unlimited, kFpsCapRefresh the monitor's refresh rate
    bool vsync = true;
    bool low_latency = false;        // one frame queued ahead of the display instead of three (Windows)
    uint32_t aniso = 0;             // 0 as the game asks
    uint32_t msaa = 0;               // 0, 2, 4 or 8 samples; off by default, it is the costliest option
    bool smaa = true;                // SMAA 1x on the finished frame
    bool taa = false;                // experimental temporal AA in place of SMAA on the 3D scene (Windows)
    uint32_t upscaler = 0;           // 0 bilinear, 1 AMD FSR 1, 2 FSR 3.1, 3 DLSS, 4 FSR 4.1
    uint32_t sharpening = 0;         // RCAS sharpening on the finished frame, percent (0 off)
    bool bloom = true;                // bright-pass bloom on the finished frame
    uint32_t bloom_strength = 100;    // bloom brightness, percent of the original
    bool dof = true;                  // distance blur on the finished frame
    uint32_t ao = 0;                  // ambient occlusion: 0 off, 1 half resolution, 2 full resolution
    uint32_t shadow_resolution = 0;   // shadow maps: 0 the game's, 1 twice, 2 four times as sharp
    uint32_t shadow_softness = 0;     // shadow edges: 0 the game's single compare, 1 soft, 2 softer
    bool building_seam_fix = true;   // experimental priority for detailed city geometry
    uint32_t object_detail = 0;      // mesh LOD distance: 0 original, 1 high (2x), 2 very high (4x), 3 maximum
    bool pc_contrast = true;         // PC contrast curve on the finished frame
    bool custom_textures = true;     // images in textures\replacement\ beside the exe replace the game's
    bool dump_textures = false;      // each texture once into textures\dumps\ beside the exe
    uint32_t texture_dump_format = 0;  // 0 PNG, 1 DDS
    bool hdr = false;                // HDR output on the finished frame
    uint32_t hdr_paper_white = 200;  // SDR white in nits, 80 to 400
    uint32_t hdr_peak = 0;           // highlight peak in nits; 0 uses the display's reported peak, else 1000
    uint32_t hdr_highlights = 40;    // inverse tone map strength, percent
    std::string join_ip;                   // co-op: a host past the LAN the system link search also asks
};

Settings& Get();
// a coherent copy for the window and render threads, which share these fields with the menus
Settings Snapshot();
// serialises multi-field settings reads and writes; recursive so a locked menu can save
class SettingsLock {
public:
    SettingsLock();
    ~SettingsLock();
    SettingsLock(const SettingsLock&) = delete;
    SettingsLock& operator=(const SettingsLock&) = delete;
};
uint32_t FrameCap();
void SetFrameCap(uint32_t cap);
uint32_t MsaaSamples();
void SetMsaaSamples(uint32_t samples);
void Load();
void Save();
void LogSettings();  // every setting, as the settings file has them
void LogSystem();    // the machine and the settings, at a run's start
void ApplyDisplay();  // any thread
bool RequestQuit();  // post a close request to the host event loop
struct Mode {
    uint32_t width, height;
};
Mode Desktop();
Mode Output();
// the full-screen sizes the primary display lists, for the Graphics menu
std::vector<Mode> DisplayModes();
// host pixels per console pixel vertically: render_percent of the displayed frame's height
float RenderScale(uint32_t guest_height);
// the frame's display aspect: the aspect setting, or the output's shape
float AspectRatio();
// horizontal host pixels per vertical ones for a console back buffer: the frame is shown at AspectRatio, wider than the
// console's 4:3 buffer, so targets are drawn that much wider rather than stretched
float RenderStretch(uint32_t guest_width, uint32_t guest_height);
// AspectRatio is 3:2 or wider: the console reports a widescreen TV (read by titles at boot)
bool Widescreen();

std::filesystem::path ExeDir();
// the game in assets (game_files.cpp): its folder and this executable's xbe, from the extracted files or a disc
// image (.iso), which it mounts as D:; false after telling the player what is missing, with error set for hosts
// that show it themselves
struct GameFiles {
    std::filesystem::path dir;
    std::vector<uint8_t> xbe;
};
// location names the game's folder or its disc image; empty searches for assets beside the executable
bool FindGame(GameFiles& game, std::string* error = nullptr, const std::filesystem::path& location = {});
std::string Sha1(const std::vector<uint8_t>& data);  // lower case hex
// the recompiled code, <exe>.dll, built from the game in assets when missing or out of date (game_code.cpp) and
// loaded; false after telling the player why the game cannot start, or when they chose not to start it yet
bool PrepareGameCode(const GameFiles& game);
int Instance();  // --instance N on the command line, 0 without: a second copy on this PC for system link tests
void* Window();  // HWND
// boot splash: GameInfo::splash .bmp or .png beside the exe, up before the game window
bool OpenSplash();        // window thread, at launch; false without one, and the game window opens at once
void SplashFirstFrame();  // the game's first Swap: waits out the splash's minimum, then opens the game window
void CloseSplash();       // window thread
void OpenGameWindow(bool apply = true);  // any thread but the window thread's; returns once the window is up
// gameplay: hide and confine the pointer to the window; false for an overlay that needs it (Graphics menu)
void CaptureMouse(bool capture);  // any thread
// relaunched: the previous instance keeps its window up until this one has shown its first frames (handover.cpp)
bool StartHandover();   // window thread, at launch; true: the game window starts hidden
bool HandingOver();
void FinishHandover();  // any thread but the window thread's: opens the game window, lets the old instance go

// held around ImGui: the window thread's messages and the renderer's frame
std::recursive_mutex& UiLock();

// what the game repo defines once
struct Option {  // a setting in the settings file, flag or value
    const char* key;
    bool* flag;
    uint32_t* value;
};
struct GameInfo {
    const wchar_t* title;        // window and splash
    const wchar_t* short_title;  // message boxes
    const wchar_t* name;         // window classes, crash dumps, handover event
    const char* log;             // log prefix
    const char* settings;        // settings file beside the exe
    const wchar_t* splash;       // .bmp or .png beside the exe
    const wchar_t* missing;      // message when neither the game files nor a disc image are found
    std::vector<std::pair<const char*, const char*>> images;  // xbe file name, executable name
    std::vector<Option> options;
    // the settings file's toml::table, for what options cannot hold
    void (*load)(const void* table);
    void (*save)(void* table);
    void (*init)();                // after the settings load, before the window and boot
    void (*loaded)();              // image in memory, before its entry runs
    void (*window)(void* hwnd);    // the game window is up, before boot
    // window thread, each message after ImGui's; true returns result instead of the host's handling
    bool (*message)(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result);
    void (*ui)(double fps);        // each frame inside the ImGui frame, UiLock held
    void (*pad)(int port, input::Pad& pad);  // a polled pad before the game gets it
    void (*rumble)(int port, uint16_t& left, uint16_t& right);
    uint32_t (*swap_interval)(uint32_t interval) = nullptr;
    bool (*keyboard_driving)() = nullptr; // title camera context for keyboard pedals
};
extern const GameInfo kGame;

// sleeps out the rest of the frame for the fps cap
void Pace(uint32_t max_fps = 0);

}  // namespace app
