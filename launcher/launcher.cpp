// The window TrueCrimeNYC.exe shows before the game: pick an ISO, which is copied into the assets folder beside the
// executable, then start the game (RecompMain, the SDK's wWinMain renamed in CMakeLists.txt).

#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <commdlg.h>

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "kernel/disc.h"
#include "xbe/xbe.h"

namespace fs = std::filesystem;

namespace {

constexpr wchar_t kTitle[] = L"True Crime NYC Launcher";
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

// The runtime rejects an image it cannot mount or whose default.xbe cannot be parsed. Check the
// same things here so a bad file never takes the assets slot.
bool ValidateIso(const fs::path &image, std::wstring &error) {
    kernel::Disc disc;
    std::string why;
    if (!disc.Open(image, why)) {
        error = L"This is not a usable Xbox disc image. " + Widen(why);
        return false;
    }
    std::vector<uint8_t> xbe;
    if (!disc.Load("default.xbe", xbe)) {
        error = L"The disc image has no default.xbe.";
        return false;
    }
    xbe::Image parsed;
    if (!parsed.Parse(std::move(xbe), why)) {
        error = L"The image's default.xbe is invalid. " + Widen(why);
        return false;
    }
    return true;
}

struct CopyState {
    HWND window;
};

DWORD CALLBACK CopyProgress(LARGE_INTEGER total, LARGE_INTEGER transferred, LARGE_INTEGER, LARGE_INTEGER, DWORD,
                            DWORD, HANDLE, HANDLE, LPVOID data) {
    auto *state = static_cast<CopyState *>(data);
    // Keep the launcher painting and responding while the image is copied.
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (total.QuadPart > 0) {
        wchar_t title[96];
        swprintf_s(title, L"%s - installing %d%%", kTitle,
                   int(transferred.QuadPart * 100 / total.QuadPart));
        SetWindowTextW(state->window, title);
    }
    return PROGRESS_CONTINUE;
}

int InstallIso(HWND window, const fs::path &source) {
    if (source.empty()) return 0;
    std::vector<wchar_t> path(32768);
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length == path.size()) return Error(L"Could not locate the launcher folder.");
    const auto folder = fs::path(path.data()).parent_path();

    const auto assets = folder / L"assets";

    try {
        if (_wcsicmp(source.extension().c_str(), L".iso") != 0) return Error(L"Select an .iso file.");
        std::wstring why;
        if (!ValidateIso(source, why)) return Error(why.c_str());

        fs::create_directories(assets);
        if (fs::exists(assets / L"default.xbe")) {
            return Error(L"Move the extracted game files out of the local assets folder before launching an ISO.");
        }

        std::vector<fs::path> installed;
        for (const auto &entry : fs::directory_iterator(assets)) {
            if (entry.is_regular_file() && _wcsicmp(entry.path().extension().c_str(), L".iso") == 0)
                installed.push_back(entry.path());
        }
        bool alreadyInstalled = false;
        for (const auto &other : installed) {
            if (fs::equivalent(other, source)) {
                alreadyInstalled = true;
                continue;
            }
            // An image the runtime rejected can be replaced; a valid one must be moved out first.
            if (ValidateIso(other, why)) {
                return Error(L"Move the other ISO out of the local assets folder before selecting a different ISO.");
            }
            std::error_code ec;
            fs::remove(other, ec);
            if (ec) return Error(L"Could not remove the unusable ISO from the assets folder.");
        }
        if (alreadyInstalled) return 0;

        const auto staged = assets / L"launcher-import.tmp";
        const auto destination = assets / source.filename();
        DeleteFileW(staged.c_str());
        CopyState state{window};
        SetWindowTextW(window, L"True Crime NYC Launcher - installing 0%");
        if (!CopyFileExW(source.c_str(), staged.c_str(), CopyProgress, &state, nullptr, 0)) {
            DeleteFileW(staged.c_str());
            SetWindowTextW(window, kTitle);
            return Error(L"Could not copy the ISO. Check free space and write access, and remove any leftover "
                         L"launcher-import.tmp from assets.");
        }
        SetWindowTextW(window, kTitle);
        if (!MoveFileW(staged.c_str(), destination.c_str())) {
            DeleteFileW(staged.c_str());
            return Error(L"Could not install the ISO in assets.");
        }

        return 0;
    } catch (const fs::filesystem_error &) {
        SetWindowTextW(window, kTitle);
        return Error(L"Could not access the selected ISO or the local assets folder.");
    }
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
            HCURSOR previous = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
            g_installing = true;
            const int result = InstallIso(window, selectedIso);
            g_installing = false;
            SetCursor(previous);
            if (result == 0) {
                launchRequested = true;
                DestroyWindow(window);
            }
        }
        return 0;
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
    return launchRequested ? RecompMain(instance, previous, args, show) : 0;
}
