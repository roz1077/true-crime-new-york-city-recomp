// Developer-only game control: mission jumps and time of day. Built only with
// TCNYC_DEV_TOOLS (see CMakeLists.txt); release builds leave src/dev/ out.
//
// The dev menu (F9, dev_menu.cpp) queues requests; OnFrame applies them on the
// game thread, from the main loop's simulation hook (timing.cpp).
//
// For automated runs, environment variables do the same without the menu:
//   XBE_DEV_MISSION=<name>    jump to that mission (e.g. BC1_M3), on F8 or after the delay
//   XBE_DEV_CLOCK=<hh:mm>     set the time of day, the same way
//   XBE_DEV_DELAY=<frames>    run them once by themselves, that many frames after
//                             the game object exists
// See docs/reverse-engineering-notes.md for the addresses.

#include "dev/dev.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

#include "common/graphics_menu.h"
#include "core/log.h"
#include "dev/level_names.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace {

constexpr uint32_t kGame = 0x360E00;          // global game object pointer
constexpr uint32_t kGameScripts = 0x2A8C;     // script manager inside it
constexpr uint32_t kGameScriptOwner = 0x1B4;  // checked before scripts run
constexpr uint32_t kGameCase = 0x2A84;        // current case index
constexpr uint32_t kGameMission = 0x2A88;     // current mission index in that case
constexpr uint32_t kSetClock = 0x7EAC0;       // (hour, minute, ?), ret 0x0C
constexpr uint32_t kClock = 0x37F940;         // (hour * 60 + minute) * 3600
// story table from LevelInfo.tag (masterCaseInfoTable), built at 0x83CC0:
// count, then 16-byte cases {name hash +0, mission count +8, missions +0xC},
// 32-byte missions {name hash +0, .., scene handler +0x18}
constexpr uint32_t kCaseCount = 0x35C30C;
constexpr uint32_t kCases = 0x35C310;
// leaving the pause menu (0xA56DE) sets the pending case/mission and calls
// 0x7F510(4); 0x7F370 applies the pending mission at the next level load
constexpr uint32_t kPendingCase = 0x2AA4;
constexpr uint32_t kPendingMission = 0x2AA8;
constexpr uint32_t kPendingFlag = 0x19D;
constexpr uint32_t kStartTransition = 0x7F510; // thiscall int(int mode)

std::atomic<int> g_request_case{-1}, g_request_mission{-1}, g_request_clock{-1};

const char *Env(const char *name) {
    const char *v = std::getenv(name);
    return v && *v ? v : nullptr;
}

uint32_t Crc32(const char *s) {
    uint32_t crc = 0xFFFFFFFFu;
    for (; *s; ++s) {
        crc ^= uint8_t(*s);
        for (int i = 0; i < 8; ++i) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

uint32_t GameObject() {
    if (!mem::Base() || !mem::IsCommitted(kGame)) return 0;
    const uint32_t game = kernel::R32(kGame);
    if (!game || !mem::IsCommitted(game + kGameScripts)) return 0;
    if (!kernel::R32(game + kGameScripts) || !kernel::R32(game + kGameScriptOwner)) return 0;
    return game;
}

// thiscall: ecx = object, arguments on the stack
uint32_t CallMethod(X86Context &ctx, uint32_t fn, uint32_t self, std::initializer_list<uint32_t> args) {
    const uint32_t ecx = ctx.ecx;
    ctx.ecx = self;
    const uint32_t r = kernel::CallGuest(ctx, fn, args);
    ctx.ecx = ecx;
    return r;
}

bool FindMission(uint32_t hash, int &case_index, int &mission_index) {
    for (const dev::Case &c : dev::StoryTable())
        for (const dev::Mission &m : c.missions)
            if (m.hash == hash) {
                case_index = m.case_index;
                mission_index = m.mission_index;
                return true;
            }
    return false;
}

void ApplyMission(X86Context &ctx, uint32_t game, int c, int m) {
    // as the pause menu's exit: pending case and mission, then the transition
    kernel::W32(game + kPendingCase, uint32_t(c));
    kernel::W32(game + kPendingMission, uint32_t(m));
    kernel::G<uint8_t>(game + kPendingFlag) = 0;
    CallMethod(ctx, kStartTransition, game, {4});
    LOGR("dev: start mission case %d, mission %d", c, m);
}

void ApplyClock(X86Context &ctx, int minutes) {
    kernel::CallGuest(ctx, kSetClock, {uint32_t(minutes / 60), uint32_t(minutes % 60), 0});
    LOGR("dev: clock set to %02d:%02d", minutes / 60, minutes % 60);
}

// the environment variable actions, on F8 or once after XBE_DEV_DELAY frames
void EnvironmentActions(X86Context &ctx, uint32_t game) {
    const char *mission = Env("XBE_DEV_MISSION");
    const char *clock = Env("XBE_DEV_CLOCK");
    if (!mission && !clock) return;
    static bool was_down = false, done = false;
    static int frames = 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    const bool down = pid == GetCurrentProcessId() && (GetAsyncKeyState(VK_F8) & 0x8000);
    const bool hotkey = down && !was_down;
    was_down = down;
    bool timed = false;
    if (!done)
        if (const char *delay = Env("XBE_DEV_DELAY"))
            if (++frames >= std::atoi(delay)) timed = done = true;
    if (!hotkey && !timed) return;
    if (clock) {
        int h = 0, m = 0;
        if (std::sscanf(clock, "%d:%d", &h, &m) == 2) ApplyClock(ctx, h * 60 + m);
    }
    if (mission) {
        int c = -1, m = -1;
        if (!dev::InGameplay())
            LOGR("dev: mission jump ignored, not in gameplay yet");
        else if (FindMission(Crc32(mission), c, m))
            ApplyMission(ctx, game, c, m);
        else
            LOGR("dev: mission %s is not in the story table", mission);
    }
}

} // namespace

namespace dev {

namespace {
const LevelName *Find(uint32_t hash) {
    static const std::unordered_map<uint32_t, const LevelName *> names = [] {
        std::unordered_map<uint32_t, const LevelName *> map;
        for (const LevelName &name : kLevelInfoNames) map.emplace(Crc32(name.name), &name);
        return map;
    }();
    auto it = names.find(hash);
    return it == names.end() ? nullptr : it->second;
}
} // namespace

std::string IdOf(uint32_t hash) {
    if (const LevelName *name = Find(hash)) return name->name;
    char hex[16];
    std::snprintf(hex, sizeof(hex), "%08X", hash);
    return hex;
}

std::string TitleOf(uint32_t hash) {
    const LevelName *name = Find(hash);
    return name && name->title ? name->title : IdOf(hash);
}

bool InGame() { return GameObject() != 0; }

bool InGameplay() { return InGame() && game::GameplayCameraActive(); }

std::vector<Case> StoryTable() {
    std::vector<Case> cases;
    if (!InGame() || !mem::IsCommitted(kCases)) return cases;
    const int count = int(kernel::R32(kCaseCount));
    const uint32_t table = kernel::R32(kCases);
    if (!table || count <= 0 || count > 64 || !mem::IsCommitted(table)) return cases;
    for (int c = 0; c < count; ++c) {
        const uint32_t entry = table + uint32_t(c) * 16;
        const uint32_t case_hash = kernel::R32(entry);
        Case out{case_hash, IdOf(case_hash), TitleOf(case_hash), {}};
        const int missions = int(kernel::R32(entry + 8));
        const uint32_t list = kernel::R32(entry + 12);
        if (list && mem::IsCommitted(list))
            for (int m = 0; m < missions && m < 64; ++m) {
                const uint32_t hash = kernel::R32(list + uint32_t(m) * 32);
                out.missions.push_back({c, m, hash, IdOf(hash), TitleOf(hash)});
            }
        cases.push_back(std::move(out));
    }
    return cases;
}

bool CurrentMission(int &case_index, int &mission_index) {
    const uint32_t game = GameObject();
    if (!game) return false;
    case_index = int(kernel::R32(game + kGameCase));
    mission_index = int(kernel::R32(game + kGameMission));
    return true;
}

int ClockMinutes() {
    if (!InGame()) return -1;
    return int(kernel::R32(kClock) / 3600 % (24 * 60));
}

void RequestMission(int case_index, int mission_index) {
    g_request_mission.store(mission_index);
    g_request_case.store(case_index);
}

void RequestClock(int hour, int minute) { g_request_clock.store(hour * 60 + minute); }

void OnFrame(X86Context &ctx) {
    ScriptsFrame();
    const uint32_t game = GameObject();
    if (!game) return;
    const int clock = g_request_clock.exchange(-1);
    if (clock >= 0) ApplyClock(ctx, clock);
    const int c = g_request_case.exchange(-1);
    if (c >= 0) {
        const int m = g_request_mission.load();
        if (InGameplay())
            ApplyMission(ctx, game, c, m);
        else
            LOGR("dev: mission jump ignored, not in gameplay yet");
    }
    EnvironmentActions(ctx, game);
    if (InGameplay()) PlayerFrame(ctx);
}

} // namespace dev
