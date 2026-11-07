// The window TrueCrimeNYC.exe shows before the game: pick an ISO, which is copied into the assets folder beside the
// executable, then start the game (RecompMain, the SDK's wWinMain renamed in CMakeLists.txt).

#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <commdlg.h>

#include <atomic>
#include <filesystem>
#include <thread>

#include "app/app.h"
#include <string>
#include <utility>
#include <vector>

#include "kernel/disc.h"
#include "xbe/xbe.h"

extern const char kImageSha1[];

namespace fs = std::filesystem;

namespace {

constexpr wchar_t kTitle[] = L"True Crime NYC Launcher";
constexpr UINT kInstallProgress = WM_APP + 1;
constexpr UINT kInstallDone = WM_APP + 2;
std::thread g_install;
std::atomic<bool> g_cancel{false};
std::wstring g_error;
bool g_installing = false;

int Error(const wchar_t *message) {
    MessageBoxW(nullptr, message, kTitle, MB_OK | MB_ICONERROR);
    return 1;
}

std::wstring Widen(const std::string &text) {
    if (text.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), int(text.size()), nullptr, 0);
    std::wstring out(size_t(n), L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, text.c_str(), int(text.size()), out.data(), n);
    return out;
}

std::wstring ValidateIso(const fs::path &image) {
    kernel::Disc disc;
    std::string why;
    if (!disc.Open(image, why)) return L"This is not a usable Xbox disc image. " + Widen(why);

    const auto *entry = disc.Find("default.xbe");
    constexpr uint32_t kMaxXbeSize = 64u << 20;
    std::vector<uint8_t> xbe;
    if (!entry || entry->size > kMaxXbeSize || !disc.Load("default.xbe", xbe))
        return L"The disc image does not contain a readable default.xbe.";
    if (app::Sha1(xbe) != kImageSha1)
        return L"Select the original Xbox disc image that matches this build of True Crime NYC.";
    return {};
}

DWORD CALLBACK CopyProgress(LARGE_INTEGER total, LARGE_INTEGER transferred, LARGE_INTEGER, LARGE_INTEGER, DWORD,
                            DWORD, HANDLE, HANDLE, LPVOID window) {
    if (g_cancel.load()) return PROGRESS_CANCEL;
    if (total.QuadPart > 0)
        PostMessageW(static_cast<HWND>(window), kInstallProgress, transferred.QuadPart * 100 / total.QuadPart, 0);
    return PROGRESS_CONTINUE;
}

std::wstring InstallIso(HWND window, const fs::path &source) {
    std::vector<wchar_t> path(32768);
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length == path.size()) return L"Could not locate the launcher folder.";
    const auto assets = fs::path(path.data()).parent_path() / L"assets";

    if (_wcsicmp(source.extension().c_str(), L".iso") != 0) return L"Select an .iso file.";
    fs::create_directories(assets);
    if (fs::exists(assets / L"default.xbe"))
        return L"Move the extracted game files out of the local assets folder before launching an ISO.";

    bool installed = false;
    for (const auto &entry : fs::directory_iterator(assets)) {
        if (!entry.is_regular_file() || _wcsicmp(entry.path().extension().c_str(), L".iso") != 0) continue;
        if (!fs::equivalent(entry.path(), source))
            return L"Move the existing ISO out of the local assets folder before selecting a different ISO.";
        installed = true;
    }
    if (installed) return ValidateIso(source);

    if (std::wstring error = ValidateIso(source); !error.empty()) return error;
    if (g_cancel.load()) return {};
    const auto destination = assets / source.filename();
    wchar_t temp[MAX_PATH];
    if (!GetTempFileNameW(assets.c_str(), L"iso", 0, temp))
        return L"Could not create a staging file in assets.";
    struct Staged {
        const wchar_t *path;
        ~Staged() {
            if (DeleteFileW(path) || GetLastError() != ERROR_ACCESS_DENIED) return;
            const DWORD attributes = GetFileAttributesW(path);
            if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY)) {
                SetFileAttributesW(path, attributes & ~FILE_ATTRIBUTE_READONLY);
                DeleteFileW(path);
            }
        }
    } staged{temp};

    if (!CopyFileExW(source.c_str(), staged.path, CopyProgress, window, nullptr, 0))
        return L"Could not copy the ISO. Check free space and write access.";
    if (g_cancel.load()) return {};
    if (std::wstring error = ValidateIso(staged.path); !error.empty()) return error;
    if (g_cancel.load()) return {};
    if (!MoveFileW(staged.path, destination.c_str())) return L"Could not install the ISO in assets.";
    return {};
}

constexpr int kBrowse = 101;
constexpr int kLaunch = 102;
HWND isoField;

fs::path selectedIso;
bool launchRequested = false;

void Browse(HWND window) {
    std::vector<wchar_t> path(32768);
    OPENFILENAMEW picker{};
    picker.lStructSize = sizeof(picker);
    picker.hwndOwner = window;
    picker.lpstrFilter = L"Xbox disc images (*.iso)\0*.iso\0";
    picker.lpstrFile = path.data();
    picker.nMaxFile = static_cast<DWORD>(path.size());
    picker.lpstrTitle = L"Select your True Crime NYC Xbox ISO";
    picker.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&picker)) {
        selectedIso = path.data();
        SetWindowTextW(isoField, selectedIso.c_str());

    } else if (CommDlgExtendedError()) {
        Error(L"Could not open the ISO picker.");
    }
}

HWND AddControl(HWND parent, const wchar_t *type, const wchar_t *text, DWORD style, int x, int y, int width, int height,
                int id = 0) {
    HWND control = CreateWindowW(type, text, WS_CHILD | WS_VISIBLE | style, x, y, width, height, parent,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    return control;
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        AddControl(window, L"STATIC", L"Select your original Xbox disc image (.iso).", 0, 16, 16, 440, 20);
        isoField = AddControl(window, L"EDIT", L"", WS_BORDER | ES_READONLY | ES_AUTOHSCROLL, 16, 44, 350, 25);
        AddControl(window, L"BUTTON", L"&Browse...", WS_TABSTOP, 376, 43, 80, 27, kBrowse);
        AddControl(window, L"STATIC", L"Launch uses assets if no ISO is selected.", 0, 16, 84, 330, 20);
        AddControl(window, L"BUTTON", L"&Launch", WS_TABSTOP, 376, 79, 80, 29, kLaunch);
        return 0;
    case WM_COMMAND:
        if (g_installing) return 0;  // the copy's message pump must not start a second install
        if (LOWORD(wParam) == kBrowse) Browse(window);
        if (LOWORD(wParam) == kLaunch) {
            if (selectedIso.empty()) {
                launchRequested = true;
                DestroyWindow(window);
                return 0;
            }
            g_cancel.store(false);
            g_installing = true;
            SetWindowTextW(window, L"True Crime NYC Launcher - installing");
            try {
                g_install = std::thread([window, source = selectedIso] {
                    try {
                        g_error = InstallIso(window, source);
                    } catch (const std::exception &) {
                        g_error = L"Could not access the selected ISO or the local assets folder.";
                    }
                    PostMessageW(window, kInstallDone, 0, 0);
                });
            } catch (const std::system_error &) {
                g_installing = false;
                SetWindowTextW(window, kTitle);
                Error(L"Could not start the ISO installer.");
            }
        }
        return 0;
    case kInstallProgress: {
        wchar_t title[96];
        swprintf_s(title, L"%s - installing %u%%", kTitle, static_cast<unsigned>(wParam));
        if (!g_cancel.load()) SetWindowTextW(window, title);
        return 0;
    }
    case kInstallDone:
        g_install.join();
        g_installing = false;
        if (g_cancel.load() || g_error.empty()) {
            launchRequested = !g_cancel.load();
            DestroyWindow(window);
        } else {
            SetWindowTextW(window, kTitle);
            Error(g_error.c_str());
        }
        return 0;
    case WM_CLOSE:
        if (g_installing) {
            g_cancel.store(true);
            SetWindowTextW(window, L"True Crime NYC Launcher - cancelling");
            return 0;
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

extern "C" int WINAPI RecompMain(HINSTANCE, HINSTANCE, PWSTR, int);

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR args, int show) {
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    windowClass.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    windowClass.lpszClassName = L"TrueCrimeNYCLauncher";
    if (!RegisterClassW(&windowClass)) return Error(L"Could not initialize the launcher window.");

    constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT bounds{0, 0, 472, 125};
    AdjustWindowRect(&bounds, style, FALSE);
    HWND window =
        CreateWindowW(windowClass.lpszClassName, L"True Crime NYC Launcher", style, CW_USEDEFAULT, CW_USEDEFAULT,
                      bounds.right - bounds.left, bounds.bottom - bounds.top, nullptr, nullptr, instance, nullptr);
    if (!window) return Error(L"Could not open the launcher window.");

    ShowWindow(window, show);
    SetFocus(GetDlgItem(window, kBrowse));
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (g_install.joinable()) {
        g_cancel.store(true);
        g_install.join();
    }
    return launchRequested ? RecompMain(instance, previous, args, show) : 0;
}
