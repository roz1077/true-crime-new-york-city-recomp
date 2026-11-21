#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include "core/dev_env.h"
#include <shellapi.h>

#include <cstdlib>
#include <cwchar>
#include <string>
#include <string_view>
#include <utility>

#include "app/app.h"
#include "launcher.h"
#include "updater.h"

extern "C" int WINAPI RecompMain(HINSTANCE, HINSTANCE, PWSTR, int);

namespace {

const char* RegionName(app::GameRegion region) {
    switch (region) {
    case app::GameRegion::Ntsc: return "US NTSC";
    case app::GameRegion::Pal: return "European PAL";
    case app::GameRegion::PalEu: return "European PAL (other)";
    default: return "unknown";
    }
}

std::string Utf8(const std::filesystem::path& path) {
    const auto bytes = path.u8string();
    return {bytes.begin(), bytes.end()};
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hinstance, HINSTANCE previous, PWSTR args, int show) {
    // Install the crash filter before the launcher runs too. A fault while the
    // launcher closes its window then leaves a dump instead of a silent exit.
    app::InstallCrashHandler();

    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::string update_argument;
    if (argc > 1) {
        const std::wstring_view first(argv[1]);
        if (first.starts_with(L"--apply-update=") || first.starts_with(L"--recover-update="))
            update_argument.assign(first.begin(), first.end());
    }
    launcher::LogLine("launcher: start v%s (%d arguments)", launcher::Updater::Current(), argc > 0 ? argc - 1 : 0);
    if (const auto handled = launcher::Updater::HandleStartup(update_argument)) {
        launcher::LogLine("launcher: update handling returned %d", *handled);
        if (argv) LocalFree(argv);
        return *handled;
    }
    bool skip = false;
    int instance = 0;
    app::GameSource source;
#if TCNYC_DEV_TOOLS
    skip = DevWEnv(L"XBE_DEV_SKIP_LAUNCHER") != nullptr;
#endif
    for (int i = 1; i < argc; ++i) {
        if (std::wcscmp(argv[i], L"--game-source") == 0 && i + 1 < argc) source.path = argv[++i];
        else if (std::wcscmp(argv[i], L"--instance") == 0 && i + 1 < argc) instance = _wtoi(argv[++i]);
        else if (std::wcscmp(argv[i], L"--skip-launcher") == 0 || std::wcscmp(argv[i], L"--relaunched") == 0) skip = true;
    }
    if (argv) LocalFree(argv);
    launcher::LogLine("launcher: skip=%d instance=%d source=%s", skip ? 1 : 0, instance,
                      source.path.empty() ? "(local assets)" : Utf8(source.path).c_str());

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    if (!skip) {
        if (source.path.empty()) {
            launcher::LogLine("launcher: opening the launcher window");
            if (!launcher::Run(source)) {
                launcher::LogLine("launcher: closed without starting the game");
                return 0;
            }
            // HIT THE STREETS already started the game in its own process, before
            // the launcher began shutting SDL down.
            launcher::LogLine("launcher: window closed after starting the game");
            return 0;
        }
        // A source on the command line: no launcher window. Keep running in this
        // process so wrappers such as RecompLauncher see the game's own process.
        launcher::LogLine("launcher: no window (game source on the command line)");
    }

    // A game process: started with --skip-launcher (or by the updater). Run in-process.
#ifndef TCNYC_PAL
    if (source.region == app::GameRegion::Unknown) {
        const auto detected = app::DetectGameSource(source.path);
        if (detected.region != app::GameRegion::Unknown) source = detected;
    }
    if (app::IsPal(source.region)) {
        launcher::LogLine("handoff: %s disc, starting the region executable", RegionName(source.region));
        return launcher::LaunchPal(source) ? 0 : 1;
    }
#endif
    launcher::LogLine("handoff: holding the game files");
    const auto game_files = launcher::Updater::HoldGameFiles();
    if (!game_files) {
        launcher::LogLine("handoff: the game files are unavailable");
        return 1;
    }
    launcher::LogLine("handoff: entering the game main (%s)", RegionName(source.region));
    app::SetStartupGameLocation(std::move(source.path));
    return RecompMain(hinstance, previous, args, show);
}
