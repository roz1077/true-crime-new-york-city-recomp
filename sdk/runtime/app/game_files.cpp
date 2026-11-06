// The game in assets: its files, or a disc image (.iso) with this executable's xbe on it. The xbe must be the one
// this executable was recompiled from; another release's would run its own data under this code.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <memory>

#include "app/app.h"
#include "core/log.h"
#include "image_symbols.h"
#include "kernel/crypto.h"
#include "kernel/disc.h"

extern const char kImageSha1[];  // the xbe recompiled into this executable, written by CMake

namespace app {

namespace {

namespace fs = std::filesystem;

// assets beside the exe or in any folder above it (the build output)
fs::path FindAssets() {
    auto dir = ExeDir();
    std::error_code ec;
    for (int i = 0; i < 6; i++) {
        if (fs::is_directory(dir / "assets", ec)) return dir / "assets";
        if (!dir.has_parent_path() || dir.parent_path() == dir) break;
        dir = dir.parent_path();
    }
    return {};
}

std::string Narrow(const wchar_t* text) {
    std::string out;
    for (; *text; ++text) out += *text < 128 ? char(*text) : '?';
    return out;
}

// telling the player what to fix; a host without dialog boxes logs it instead
bool Alert(const wchar_t* text, bool question = false) {
#ifdef _WIN32
    const UINT flags = question ? MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2 : MB_ICONERROR;
    return MessageBoxW(nullptr, text, kGame.short_title, flags) == IDYES;
#else
    LOGE("game: %s", Narrow(text).c_str());
    // no dialog on this host: an explicit opt-in is the only way to accept another release
    return question && std::getenv("TCNYC_ALLOW_OTHER_RELEASE") != nullptr;
#endif
}

bool ReadHostFile(const fs::path& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

}  // namespace

std::string Sha1(const std::vector<uint8_t>& data) {
    alignas(8) uint8_t ctx[128];
    uint8_t digest[20];
    crypto::ShaInit(ctx);
    crypto::ShaUpdate(ctx, data.data(), data.size());
    crypto::ShaFinal(ctx, digest);
    std::string hex;
    for (uint8_t b : digest) {
        hex += "0123456789abcdef"[b >> 4];
        hex += "0123456789abcdef"[b & 15];
    }
    return hex;
}

bool FindGame(GameFiles& game, std::string* error, const fs::path& location) {
    const fs::path source = location.empty() ? FindAssets() : location;
    std::error_code ec;
    const bool source_iso = fs::is_regular_file(source, ec) && [&] {
        std::string ext = source.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return ext == ".iso";
    }();
    const fs::path dir = source_iso ? source.parent_path() : source;

    std::vector<fs::path> isos;
    if (source_iso) {
        isos.push_back(source);
    } else if (fs::is_directory(source, ec)) {
        for (auto& e : fs::directory_iterator(source, ec)) {
            std::string ext = e.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            if (ext == ".iso" && e.is_regular_file(ec)) isos.push_back(e.path());
        }
    }
    std::sort(isos.begin(), isos.end());

    // the first copy with this xbe wins, the extracted files before any image; another release is kept to offer
    GameFiles other;
    std::unique_ptr<kernel::Disc> other_disc;
    fs::path other_name;
    auto consider = [&](const fs::path& name, std::vector<uint8_t> xbe, std::unique_ptr<kernel::Disc> disc) {
        std::string sha = Sha1(xbe);
        bool match = sha == kImageSha1;
        LOGR("Game: %s in %s, sha1 %s%s", IMAGE_XBE, name.string().c_str(), sha.c_str(), match ? "" : ", another release");
        if (match) {
            game = {dir, std::move(xbe)};
            kernel::MountDisc(std::move(disc));
            return true;
        }
        if (other_name.empty()) {
            other = {dir, std::move(xbe)};
            other_disc = std::move(disc);
            other_name = name;
        }
        return false;
    };

    std::vector<uint8_t> xbe;
    if (!source_iso && !source.empty() && fs::is_directory(source, ec) &&
        ReadHostFile(source / IMAGE_XBE, xbe) && consider(source, std::move(xbe), nullptr))
        return true;
    for (const fs::path& iso : isos) {
        auto disc = std::make_unique<kernel::Disc>();
        std::string why;
        if (!disc->Open(iso, why)) {
            LOGW("game: %s: %s", iso.string().c_str(), why.c_str());
            continue;
        }
        if (!disc->Load(IMAGE_XBE, xbe)) {
            LOGW("game: %s has no %s", iso.string().c_str(), IMAGE_XBE);
            continue;
        }
        if (consider(iso, std::move(xbe), std::move(disc))) return true;
    }

    const std::string searched = source.empty() ? std::string() : " (" + source.string() + ")";
    if (other_name.empty()) {
        Alert(kGame.missing);
        if (error) *error = Narrow(kGame.missing) + searched;
        return false;
    }
    std::wstring text = other_name.filename().wstring() +
                        L" is a different release of the game than this build was made from, and will most likely "
                        L"crash.\n\nStart it anyway?";
    if (!Alert(text.c_str(), true)) {
        if (error) *error = other_name.filename().string() +
                            " is a different release of the game than this build was made from.";
        return false;
    }
    game = std::move(other);
    kernel::MountDisc(std::move(other_disc));
    return true;
}

}  // namespace app
