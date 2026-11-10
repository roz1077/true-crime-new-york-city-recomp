// the host: window, message loop, crash dumps; the game is described by app::kGame
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#include <timeapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cwchar>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <imgui.h>
#include <backends/imgui_impl_win32.h>

#include "app/app.h"
#include "core/input.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "image_symbols.h"
#include "kernel/kernel.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace app {

namespace {

HWND g_hwnd = nullptr;
constexpr UINT kApplyDisplay = WM_APP + 1, kOpenWindow = WM_APP + 2, kCaptureMouse = WM_APP + 3;
bool g_mode_changed = false;
bool g_apply_posted = false;  // a display apply is waiting in the queue or running
// Display changes the menus asked for (RequestDisplayApply). Each request carries its own values and
// gets its own result; the settings are written only once the window thread has applied them.
struct DisplayRequest {
    DisplayMode mode;
    uint32_t width, height;
};
std::mutex g_requests_lock;
uint64_t g_next_request = 0;
std::map<uint64_t, DisplayRequest> g_requests;  // posted, not yet applied
std::map<uint64_t, bool> g_results;             // applied, not yet collected
constexpr size_t kMaxResults = 64;              // results a caller gave up on are dropped, oldest first
bool g_capture_wanted = true; // gameplay wants the pointer hidden and confined
bool g_captured = false;
bool g_cursor_hidden = false;

void ShowPointer(bool show) {
    if (show == !g_cursor_hidden) return;
    if (show) {
        while (ShowCursor(TRUE) < 0) {
        }
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    } else {
        while (ShowCursor(FALSE) >= 0) {
        }
        SetCursor(nullptr);
    }
    g_cursor_hidden = !show;
}

void ConfinePointer(HWND hwnd) {
    RECT client{};
    if (!GetClientRect(hwnd, &client)) return;
    POINT top_left{client.left, client.top}, bottom_right{client.right, client.bottom};
    ClientToScreen(hwnd, &top_left);
    ClientToScreen(hwnd, &bottom_right);
    const RECT clip{top_left.x, top_left.y, bottom_right.x, bottom_right.y};
    ClipCursor(&clip);
}

// acquire or release the pointer; `active` is the window's activation state
void ApplyCapture(HWND hwnd, bool active) {
    const bool capture = g_capture_wanted && active && IsWindowVisible(hwnd) && !IsIconic(hwnd);
    if (capture) {
        ConfinePointer(hwnd);
        ShowPointer(false);
        g_captured = true;
    } else {
        if (g_captured) ClipCursor(nullptr);
        ShowPointer(true);
        g_captured = false;
    }
}

// Guest pages worth keeping when the guest faults: the pages around the faulting address, the faulting thread's
// stack (the minidump only has host memory) and the nine renderer effect slots at renderer+0x55C0.
void WriteGuestCrashPages(kernel::Thread& thread, EXCEPTION_POINTERS* e) {
    auto* r = e ? e->ExceptionRecord : nullptr;
    if (!r || !thread.ctx) return;
    auto dir = ExeDir() / "CrashDumps";
    CreateDirectoryW(dir.c_str(), nullptr);
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t stamp[64];
    swprintf(stamp, 64, L"%04u.%02u.%02u_%02u.%02u.%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    const std::wstring base = (dir / (std::wstring(L"guest-") + stamp)).wstring();
    auto dump = [&](const wchar_t* suffix, uint32_t va, uint32_t len) {
        const std::filesystem::path p = base + suffix;
        if (kernel::DumpGuestRegion(va, len, p.string().c_str()))
            LOGE("  guest dump %ls at %08X (%u bytes)", p.c_str(), va, len);
    };
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
        const uint32_t guest = uint32_t(r->ExceptionInformation[1] - uintptr_t(kernel::g_base));
        if (guest >= 0x4000) dump(L"-fault.bin", (guest & ~0xFFFu) - 0x3000u, 0x6000u);
    }
    const uint32_t esp_page = thread.ctx->esp & ~0xFFFu;
    if (esp_page >= 0x20000) dump(L"-stack.bin", esp_page - 0x20000u, 0x21000u);
    if (mem::IsCommitted(0x37F8FCu)) {
        const uint32_t renderer = kernel::R32(0x37F8FCu);
        if (renderer && mem::IsCommitted(renderer + 0x55C0u)) {
            dump(L"-renderer55c0.bin", renderer + 0x55C0u, 9 * 0x80u);
            // effect manager fields before the slot table (0x5540 is referenced by a corrupt slot) and the slots
            if (renderer >= 0x5400u && mem::IsCommitted(renderer + 0x5400u))
                dump(L"-effectmanager.bin", renderer + 0x5400u, 0x0C00u);
            // first bytes of the data block each active slot derived its pointers from
            for (int i = 0; i < 9; i++) {
                const uint32_t slot = renderer + 0x55C0u + uint32_t(i) * 0x80u;
                if (!mem::IsCommitted(slot + 0x74u) || kernel::R32(slot + 0x70u) == 0) continue;
                const uint32_t block = kernel::R32(slot + 0x1Cu);
                if (!block || !mem::IsCommitted(block)) continue;
                wchar_t suffix[32];
                swprintf(suffix, 32, L"-block%d.bin", i);
                dump(suffix, block, 0x200u);
            }
        }
    }
}

// crash dump in CrashDumps beside the exe
LONG WINAPI WriteCrashDump(EXCEPTION_POINTERS* e) {
    // the dump holds no guest memory: the guest registers and return addresses go to the log, key pages to .bin files
    if (kernel::Thread* t = kernel::Current(); e && t && t->ctx) {
        auto* r = e->ExceptionRecord;
        LOGE("exception %08lX at %p on guest thread %X", r->ExceptionCode, r->ExceptionAddress, t->id);
        HMODULE mod = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(r->ExceptionAddress), &mod) &&
            mod)
            LOGE("  fault module base %p, offset +%llX", (void*)mod,
                 (unsigned long long)(uintptr_t(r->ExceptionAddress) - uintptr_t(mod)));
        if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
            uint64_t va = r->ExceptionInformation[1], g = uint64_t(reinterpret_cast<uintptr_t>(kernel::g_base));
            LOGE("  %s %llX, guest %08llX", r->ExceptionInformation[0] ? "write" : "read", (unsigned long long)va,
                 (unsigned long long)(va - g));
        }
        kernel::LogGuestStack(*t->ctx, 256, t);
        WriteGuestCrashPages(*t, e);
    }
    auto dir = ExeDir() / "CrashDumps";
    CreateDirectoryW(dir.c_str(), nullptr);
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[64];
    swprintf(name, 64, L"%ls.%04u.%02u.%02u_%02u.%02u.%02u.dmp", kGame.name, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
             t.wSecond);
    HANDLE f = CreateFileW((dir / name).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return EXCEPTION_CONTINUE_SEARCH;
    MINIDUMP_EXCEPTION_INFORMATION info{GetCurrentThreadId(), e, FALSE};
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                      MINIDUMP_TYPE(MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithThreadInfo |
                                    MiniDumpWithUnloadedModules | MiniDumpWithIndirectlyReferencedMemory),
                      e ? &info : nullptr, nullptr, nullptr);
    CloseHandle(f);
    logx::Flush();
    return EXCEPTION_CONTINUE_SEARCH;
}

void DumpThreads() {
    gpu::g_capture = true;
    for (auto& t : kernel::Threads())
        if (t->ctx) {
            LOGI("thread %X", t->id);
            kernel::LogGuestStack(*t->ctx, 32, t.get());
        }
}

// Window thread only. Applies the current settings, or the requested values when request is
// given. A requested change is written to the settings only if it worked; a refused full-screen
// mode returns false and the window goes back to the current settings.
bool ApplyNow(const DisplayRequest* request = nullptr) {
    Settings s = Snapshot();
    if (request) {
        s.display_mode = request->mode;
        s.width = request->width;
        s.height = request->height;
    }
    bool ok = true;
    HMONITOR mon = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    if (g_mode_changed) {
        ChangeDisplaySettingsExW(mi.szDevice, nullptr, nullptr, 0, nullptr);
        g_mode_changed = false;
    }
    const Mode out = s.width && s.height ? Mode{s.width, s.height} : Desktop();
    LONG_PTR visible = IsWindowVisible(g_hwnd) ? WS_VISIBLE : 0;  // a window held back for a splash or handover stays so
    DEVMODEW desktop{};
    desktop.dmSize = sizeof(desktop);
    EnumDisplaySettingsW(mi.szDevice, ENUM_REGISTRY_SETTINGS, &desktop);
    // the desktop's own size is no mode change: switching to it anyway blanks the screen for nothing
    if (s.display_mode == DisplayMode::Fullscreen && s.width && s.height &&
        (s.width != desktop.dmPelsWidth || s.height != desktop.dmPelsHeight)) {
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        dm.dmPelsWidth = s.width;
        dm.dmPelsHeight = s.height;
        dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
        const LONG change = ChangeDisplaySettingsExW(mi.szDevice, &dm, nullptr, CDS_FULLSCREEN, nullptr);
        g_mode_changed = change == DISP_CHANGE_SUCCESSFUL;
        ok = g_mode_changed;
        if (!ok) {
            LOGE("app: fullscreen mode %ux%u refused (%ld)", s.width, s.height, change);
            if (request) {
                ApplyNow();  // back to the mode in the settings
                return false;
            }
        }
        GetMonitorInfoW(mon, &mi);
    }
    DisplayRequest previous{};
    if (request) {
        // before the window is resized, so the renderer sees the new output size with its WM_SIZE
        SettingsLock lock;
        Settings& live = Get();
        previous = {live.display_mode, live.width, live.height};
        live.display_mode = s.display_mode;
        live.width = s.width;
        live.height = s.height;
    }
    BOOL placed;
    if (s.display_mode == DisplayMode::Windowed) {
        RECT r{0, 0, LONG(out.width), LONG(out.height)};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        int w = r.right - r.left, h = r.bottom - r.top;
        const RECT& a = mi.rcWork;
        SetWindowLongPtrW(g_hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | visible);
        placed = SetWindowPos(g_hwnd, HWND_NOTOPMOST, a.left + (a.right - a.left - w) / 2,
                              a.top + (a.bottom - a.top - h) / 2, w, h, SWP_FRAMECHANGED);
    } else {
        const RECT& m = mi.rcMonitor;
        SetWindowLongPtrW(g_hwnd, GWL_STYLE, WS_POPUP | visible);
        placed = SetWindowPos(g_hwnd, HWND_NOTOPMOST, m.left, m.top, m.right - m.left, m.bottom - m.top,
                              SWP_FRAMECHANGED);
    }
    if (!placed) {
        LOGE("app: SetWindowPos failed (%lu)", GetLastError());
        ok = false;
        if (request) {
            // the request is refused: put the saved mode back in the settings and on screen
            {
                SettingsLock lock;
                Settings& live = Get();
                live.display_mode = previous.mode;
                live.width = previous.width;
                live.height = previous.height;
            }
            ApplyNow();
        }
    }
    return ok;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    {
        std::lock_guard lock(UiLock());
        if (ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
    }
    if (intptr_t result = 0; kGame.message && kGame.message(hwnd, msg, wp, lp, &result)) return result;
    switch (msg) {
        case WM_INPUT:
            input::ReadMouse(reinterpret_cast<void*>(lp));
            break;
        case WM_KEYDOWN:
            if (wp == VK_F3) DumpThreads();
            return 0;
        case WM_SETCURSOR:
            if (g_captured && LOWORD(lp) == HTCLIENT) {
                SetCursor(nullptr);
                return TRUE;
            }
            break;
        case WM_SIZE:
        case WM_MOVE:
            if (g_captured) ConfinePointer(hwnd);
            break;
        case WM_DISPLAYCHANGE:
            gpu::g_display_changed.store(true, std::memory_order_relaxed);
            break;
        case kCaptureMouse:
            g_capture_wanted = wp != 0;
            ApplyCapture(hwnd, GetForegroundWindow() == hwnd);
            return 0;
        case WM_ACTIVATE:
            ApplyCapture(hwnd, LOWORD(wp) != WA_INACTIVE);
            if (LOWORD(wp) == WA_INACTIVE) {
                // a changed display mode steps aside for the desktop, minimized, and comes back on return; not while
                // handing over to the next instance, which runs in the same mode
                if (g_mode_changed && !kernel::g_relaunching) {
                    MONITORINFOEXW mi{};
                    mi.cbSize = sizeof(mi);
                    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
                    ChangeDisplaySettingsExW(mi.szDevice, nullptr, nullptr, 0, nullptr);
                    g_mode_changed = false;
                    ShowWindow(hwnd, SW_MINIMIZE);
                }
            } else if (const Settings s = Snapshot();
                       s.display_mode == DisplayMode::Fullscreen && s.width && s.height && !g_mode_changed &&
                       IsWindowVisible(hwnd) && !g_apply_posted) {
                // once: restoring the window activates it again from inside the apply, and a second apply switched
                // the mode back and forth, after which presents were not taken for a while (the game stood still
                // until the next alt-tab)
                g_apply_posted = true;
                PostMessageW(hwnd, kApplyDisplay, 0, 0);
            }
            break;
        case kApplyDisplay:
            if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
            if (wp) {
                DisplayRequest request{};
                bool found = false;
                {
                    std::lock_guard lock(g_requests_lock);
                    if (auto it = g_requests.find(uint64_t(wp)); it != g_requests.end()) {
                        request = it->second;
                        g_requests.erase(it);
                        found = true;
                    }
                }
                const bool ok = found && ApplyNow(&request);
                std::lock_guard lock(g_requests_lock);
                g_results[uint64_t(wp)] = ok;
                while (g_results.size() > kMaxResults) g_results.erase(g_results.begin());
            } else {
                ApplyNow();
            }
            g_apply_posted = false;
            return 0;
        case kOpenWindow:
            CloseSplash();
            if (wp) ApplyNow();
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
            BringWindowToTop(hwnd);
            SetActiveWindow(hwnd);
            SetFocus(hwnd);
            ApplyCapture(hwnd, GetForegroundWindow() == hwnd);
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY: {
            // changes still waiting are answered as refused, so no menu waits on a closed window
            std::lock_guard lock(g_requests_lock);
            for (const auto& [id, request] : g_requests) g_results[id] = false;
            g_requests.clear();
        }
            ClipCursor(nullptr);
            ShowPointer(true);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

std::filesystem::path ExeDir() {
    std::vector<wchar_t> p(MAX_PATH);
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, p.data(), DWORD(p.size()));
        if (!n) return {};
        if (n < p.size()) return std::filesystem::path(std::wstring(p.data(), n)).parent_path();
        p.resize(p.size() * 2);
    }
}

// Hardware write watchpoint diagnostic (XBE_EFFECT_WATCH). The exception carries the host instruction that
// performed the write; logging the module offset lets it be symbolized against the shipped PDB. The guest
// registers are the current thread's context. Read-only: no game state is changed.
LONG CALLBACK GuestWatchHandler(EXCEPTION_POINTERS* e) {
    auto* r = e ? e->ExceptionRecord : nullptr;
    auto* c = e ? e->ContextRecord : nullptr;
    if (!r || !c || r->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    if (!kernel::GuestWatchActive()) return EXCEPTION_CONTINUE_SEARCH;
    // DR0 is the only breakpoint this project sets: ignore unrelated single-step exceptions and
    // leave their status bits for whoever owns them.
    if (!(c->Dr6 & 0x1ull)) return EXCEPTION_CONTINUE_SEARCH;
    const uint32_t watched = kernel::GuestWatchAddress();
    const uint32_t value = mem::IsCommitted(watched) ? kernel::R32(watched) : 0;
    static std::atomic<int> hits{0};
    if (hits.fetch_add(1, std::memory_order_relaxed) < 32) {
        HMODULE mod = nullptr;
        uint64_t rva = 0;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(c->Rip), &mod) &&
            mod)
            rva = uint64_t(c->Rip) - uint64_t(mod);
        if (kernel::Thread* t = kernel::Current(); t && t->ctx) {
            auto& g = *t->ctx;
            LOGE("watch: hit +%llX value %08X guest eax %08X ecx %08X edx %08X ebx %08X esi %08X edi %08X esp %08X",
                 (unsigned long long)rva, value, g.eax, g.ecx, g.edx, g.ebx, g.esi, g.edi, g.esp);
            // the writer's frame: the grid routines keep the coordinate and remainders in the first locals
            if (mem::IsCommitted(g.esp) && mem::IsCommitted(g.esp + 0x5C)) {
                uint32_t w[24];
                for (int i = 0; i < 24; i++) w[i] = kernel::R32(g.esp + uint32_t(i) * 4);
                LOGE("  stack+00 %08X %08X %08X %08X %08X %08X %08X %08X", w[0], w[1], w[2], w[3], w[4], w[5], w[6],
                     w[7]);
                LOGE("  stack+20 %08X %08X %08X %08X %08X %08X %08X %08X", w[8], w[9], w[10], w[11], w[12], w[13],
                     w[14], w[15]);
                LOGE("  stack+40 %08X %08X %08X %08X %08X %08X %08X %08X", w[16], w[17], w[18], w[19], w[20], w[21],
                     w[22], w[23]);
            }
        } else {
            LOGE("watch: hit +%llX value %08X (no guest thread)", (unsigned long long)rva, value);
        }
    }
    c->Dr6 &= ~0x1ull;
    return EXCEPTION_CONTINUE_EXECUTION;
}

int Instance() {
    static const int n = [] {
        const wchar_t* p = std::wcsstr(GetCommandLineW(), L"--instance");
        return p ? std::max(0, std::min(8, _wtoi(p + 10))) : 0;
    }();
    return n;
}

void* Window() { return g_hwnd; }

std::recursive_mutex& UiLock() {
    static std::recursive_mutex m;
    return m;
}

void ApplyDisplay() { PostMessageW(g_hwnd, kApplyDisplay, 0, 0); }

// Asks the window thread to switch to this mode and size, and returns a request number, or 0 when
// the window would not accept the message. The settings change only if the switch works; the caller
// polls DisplayApplyResult for this request's own answer and saves after a success.
uint64_t RequestDisplayApply(DisplayMode mode, uint32_t width, uint32_t height) {
    uint64_t id;
    {
        std::lock_guard lock(g_requests_lock);
        id = ++g_next_request;
        g_requests[id] = {mode, width, height};
    }
    if (g_hwnd && PostMessageW(g_hwnd, kApplyDisplay, WPARAM(id), 0)) return id;
    std::lock_guard lock(g_requests_lock);
    g_requests.erase(id);
    return 0;
}

// true once the window thread has answered this request; ok says whether the mode was applied
bool DisplayApplyResult(uint64_t request, bool &ok) {
    std::lock_guard lock(g_requests_lock);
    auto it = g_results.find(request);
    if (it == g_results.end()) return false;
    ok = it->second;
    g_results.erase(it);
    return true;
}
void OpenGameWindow(bool apply) { SendMessageW(g_hwnd, kOpenWindow, apply, 0); }
void CaptureMouse(bool capture) {
    if (g_hwnd) PostMessageW(g_hwnd, kCaptureMouse, capture ? 1 : 0, 0);
}

namespace {

// the game window's monitor's refresh rate, read again each second (a mode change, a move to another monitor); 0
// when unreadable
uint32_t RefreshRate() {
    static uint32_t rate = 0;
    static ULONGLONG next = 0;
    if (ULONGLONG now = GetTickCount64(); now >= next) {
        next = now + 1000;
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        rate = GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi) &&
                       EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1
                   ? mode.dmDisplayFrequency
                   : 0;
    }
    return rate;
}

}  // namespace

void Pace(uint32_t max_fps) {
    using clock = std::chrono::steady_clock;
    static clock::time_point next = clock::now();
    uint32_t cap = FrameCap();
    if (cap == kFpsCapRefresh) cap = RefreshRate();
    if (max_fps && (!cap || cap > max_fps)) cap = max_fps;
    auto now = clock::now();
    if (!cap) {
        next = now;
        return;
    }
    auto period = std::chrono::nanoseconds(1000000000ull / cap);
    next += period;
    if (next < now - period) next = now;
    while (clock::now() + std::chrono::milliseconds(2) < next) Sleep(1);
    // sleep the last short span instead of burning a core with yield()
    if (const auto left = next - clock::now(); left > clock::duration::zero())
        std::this_thread::sleep_for(std::min(left, std::chrono::duration_cast<clock::duration>(
                                                             std::chrono::milliseconds(2))));
}

}  // namespace app

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetUnhandledExceptionFilter(app::WriteCrashDump);
    AddVectoredExceptionHandler(1, app::GuestWatchHandler);
    std::signal(SIGABRT, [](int) { app::WriteCrashDump(nullptr); });
    timeBeginPeriod(1);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // one log a session: a relaunch into another image carries on its predecessor's
    bool relaunched = std::wcsstr(GetCommandLineW(), L"--relaunched") != nullptr;
    std::string log = app::kGame.log;
    if (app::Instance() > 1) log += "-" + std::to_string(app::Instance());
    logx::Init((app::ExeDir() / "logs").string(), log, relaunched);
    app::Load();
    if (app::kGame.init) app::kGame.init();
    if (relaunched && !logx::kVerbose) LOGR("Relaunched: %s", IMAGE_XBE);
    else app::LogSystem();
    LOGR("diagnostics: effect watch %s", std::getenv("XBE_EFFECT_WATCH") ? "enabled" : "off");

    app::GameFiles game;
    if (!app::FindGame(game)) return 1;
    if (!app::PrepareGameCode(game)) return 1;

    bool splash = app::OpenSplash(), handover = app::StartHandover();

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = app::WndProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = app::kGame.name;
    RegisterClassExW(&wc);
    app::g_hwnd = CreateWindowExW(0, wc.lpszClassName, app::kGame.title, WS_POPUP, 0, 0, 1280, 720,
                                  nullptr, nullptr, inst, nullptr);
    // behind a splash the window stays hidden until the game's first frame; handed over, it is set up hidden
    if (handover) {
        app::ApplyNow();
    } else if (!splash) {
        app::ApplyNow();
        ShowWindow(app::g_hwnd, SW_SHOW);
        SetForegroundWindow(app::g_hwnd);
        SetActiveWindow(app::g_hwnd);
        SetFocus(app::g_hwnd);
        app::ApplyCapture(app::g_hwnd, GetForegroundWindow() == app::g_hwnd);
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImGui_ImplWin32_Init(app::g_hwnd);

    input::RegisterMouse(app::Window());
    input::Init();
    if (app::kGame.window) app::kGame.window(app::g_hwnd);
    std::wstring hdd = app::Instance() > 1 ? L"hdd" + std::to_wstring(app::Instance()) : L"hdd";
    if (app::Instance())
        SetWindowTextW(app::g_hwnd, (std::wstring(app::kGame.title) + L" [" + std::to_wstring(app::Instance()) + L"]").c_str());
    if (!kernel::Boot(std::move(game.xbe), IMAGE_XBE, game.dir, app::ExeDir() / hdd)) {
        MessageBoxW(app::g_hwnd, L"The game failed to start, see the log in the logs folder.", app::kGame.short_title,
                    MB_ICONERROR);
        return 1;
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (app::g_mode_changed) ChangeDisplaySettingsW(nullptr, 0);
    logx::Flush();
    kernel::Shutdown(0);
}
