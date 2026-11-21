#include "updater.h"
#include "launcher.h"
#include "release_json.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <optional>
#include <ranges>
#include <regex>
#include <set>
#include <sstream>
#include <vector>

#include <SDL3/SDL.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

#ifndef TCNYC_VERSION
#define TCNYC_VERSION "0.0.0"
#endif

namespace launcher {
namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

constexpr std::string_view kRepo = "roz1077/true-crime-new-york-city-recomp";
#ifdef _WIN32
constexpr char kAsset[] = "TrueCrimeNYC.zip";
constexpr char kExecutable[] = "TrueCrimeNYC.exe";
constexpr char kPalExecutable[] = "regions/TrueCrimeNYCPAL.exe";
constexpr char kPalEuExecutable[] = "regions/TrueCrimeNYCPALEU.exe";
#else
constexpr char kAsset[] = "TrueCrimeNYC-linux-x86_64.tar.gz";
constexpr char kExecutable[] = "TrueCrimeNYC";
constexpr char kPalExecutable[] = "regions/TrueCrimeNYCPAL";
constexpr char kPalEuExecutable[] = "regions/TrueCrimeNYCPALEU";
#endif
constexpr uint64_t kArchiveLimit = 1ull << 30;

fs::path PathFromUtf8(std::string_view text) {
    return fs::path(std::u8string(text.begin(), text.end()));
}

std::string Utf8(const fs::path& path) {
    const auto text = path.generic_u8string();
    return {text.begin(), text.end()};
}

fs::path BaseDir() {
    const char* path = SDL_GetBasePath();
    if (!path || !*path) throw std::runtime_error("Could not locate the installation.");
    // A PAL game executable in regions/ works on the install folder above it.
    return app::InstallRootFor(fs::canonical(PathFromUtf8(path)));
}

fs::path ExecutablePath() {
#ifdef _WIN32
    std::wstring path(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
    if (!length || length >= path.size()) throw std::runtime_error("Could not locate the executable.");
    path.resize(length);
    return fs::canonical(path);
#else
    return fs::canonical("/proc/self/exe");
#endif
}

bool Protected(std::string_view name) {
    constexpr std::string_view keep[] = {"settings.toml", "settings-pal.toml", "launcher.toml",
        "launcher-pal.toml", "launcher-pal-eu.toml", "settings-pal-eu.toml",
        "assets", "assets-pal", "assets-pal-eu", "textures", "music", "logs", "update", "shader-cache", "crashdumps"};
    if (std::ranges::find(keep, name) != std::end(keep)) return true;
    for (std::string_view prefix : {"hdd-pal-eu", "hdd-pal", "hdd"}) {
        if (name.starts_with(prefix))
            return std::ranges::all_of(name.substr(prefix.size()), [](char c) { return c >= '0' && c <= '9'; });
    }
    return false;
}

std::string AsciiLower(std::string text) {
    for (char& ch : text) if (ch >= 'A' && ch <= 'Z') ch = char(ch + ('a' - 'A'));
    return text;
}

bool SafeComponent(std::string_view part) {
    if (part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ') return false;
    if (!std::ranges::all_of(part, [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
               c == '.' || c == '_' || c == '-' || c == ' ';
    })) return false;
    const auto stem = AsciiLower(std::string(part.substr(0, part.find('.'))));
    if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul") return false;
    return !(stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) &&
             stem.back() >= '0' && stem.back() <= '9');
}

bool SafeRelative(const fs::path& path) {
    if (path.empty() || path.has_root_path()) return false;
    for (const auto& part : path) if (!SafeComponent(Utf8(part))) return false;
    return true;
}

fs::file_status CheckedStatus(const fs::path& path) {
    std::error_code ec;
    const auto status = fs::symlink_status(path, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) throw fs::filesystem_error("reading update path", path, ec);
    if (fs::is_symlink(status)) throw std::runtime_error("A symbolic link is present in the update path.");
#ifdef _WIN32
    if (fs::exists(status) && (GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::runtime_error("A reparse point is present in the update path.");
#endif
    return status;
}

fs::file_status CheckTarget(const fs::path& root, const fs::path& relative) {
    if (!SafeRelative(relative)) throw std::runtime_error("Invalid path in the update.");
    auto current = root;
    auto status = CheckedStatus(root);
    for (const auto& part : relative) {
        if (fs::exists(status) && !fs::is_directory(status))
            throw std::runtime_error("A file is blocking an update directory.");
        current /= part;
        status = CheckedStatus(current);
    }
    return status;
}

struct InstallLock {
#ifdef _WIN32
    HANDLE file = INVALID_HANDLE_VALUE;
#else
    int file = -1;
#endif
    explicit InstallLock(const fs::path& root, const char* name = "install.lock", bool shared = false) {
        const auto folder = root / "update";
        if (const auto status = CheckedStatus(folder); fs::exists(status) && !fs::is_directory(status))
            throw std::runtime_error("The update directory is blocked.");
        fs::create_directory(folder);
        const auto path = folder / name;
        CheckedStatus(path);
#ifdef _WIN32
        file = CreateFileW(path.c_str(), shared ? GENERIC_READ : GENERIC_READ | GENERIC_WRITE,
                           shared ? FILE_SHARE_READ : 0, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("Another launcher is using the update folder.");
#else
        file = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (file < 0 || flock(file, (shared ? LOCK_SH : LOCK_EX) | LOCK_NB) != 0) {
            if (file >= 0) close(file);
            throw std::runtime_error("Another launcher is using the update folder.");
        }
#endif
    }
    InstallLock(const InstallLock&) = delete;
    ~InstallLock() {
#ifdef _WIN32
        CloseHandle(file);
#else
        close(file);
#endif
    }
};

std::string ReadFile(const fs::path& path, size_t limit) {
    if (!fs::is_regular_file(CheckedStatus(path))) throw std::runtime_error("An update file is missing.");
    const auto size = fs::file_size(path);
    if (size > limit) throw std::runtime_error("An update file is too large.");
    std::string text(size_t(size), '\0');
    std::ifstream in(path, std::ios::binary);
    if (!in.read(text.data(), std::streamsize(text.size()))) throw std::runtime_error("Could not read an update file.");
    return text;
}

void SyncFile(const fs::path& path) {
#ifdef _WIN32
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("Could not flush an update file.");
    const bool ok = FlushFileBuffers(file);
    CloseHandle(file);
#else
    const int file = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (file < 0) throw std::runtime_error("Could not flush an update file.");
    const bool ok = fsync(file) == 0;
    close(file);
#endif
    if (!ok) throw std::runtime_error("Could not flush an update file.");
}

void SyncDirectory(const fs::path& path) {
#ifndef _WIN32
    const int dir = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dir < 0) throw std::runtime_error("Could not flush an update directory.");
    const bool ok = fsync(dir) == 0;
    close(dir);
    if (!ok) throw std::runtime_error("Could not flush an update directory.");
#else
    (void)path;  // Windows uses write-through renames for publication.
#endif
}

void Replace(const fs::path& from, const fs::path& to) {
#ifdef _WIN32
    if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw fs::filesystem_error("replacing an update file", from, to,
                                   std::error_code(GetLastError(), std::system_category()));
#else
    fs::rename(from, to);
    SyncDirectory(to.parent_path());
    if (from.parent_path() != to.parent_path()) SyncDirectory(from.parent_path());
#endif
}

void PublishText(const fs::path& target, const std::string& text) {
    const auto temp = target.parent_path() / "writing";
    if (fs::is_regular_file(CheckedStatus(temp))) fs::remove(temp);
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    out << text;
    out.close();
    if (!out) throw std::runtime_error("Could not write the update journal.");
    SyncFile(temp);
    Replace(temp, target);
}

#ifdef _WIN32
std::wstring Quote(std::wstring_view value) {
    std::wstring quoted(1, L'"');
    size_t slashes = 0;
    for (wchar_t c : value) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        quoted.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        quoted += c;
    }
    quoted.append(slashes * 2, L'\\');
    return quoted + L'"';
}

bool Spawn(const fs::path& tool, const std::vector<std::string>& args, PROCESS_INFORMATION& process, DWORD flags,
           HANDLE output = INVALID_HANDLE_VALUE) {
    std::wstring command = Quote(tool.native());
    for (const auto& arg : args) command += L" " + Quote(PathFromUtf8(arg).native());
    STARTUPINFOW startup{sizeof(startup)};
    if (output != INVALID_HANDLE_VALUE) {
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput = output;
        startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    }
    return CreateProcessW(tool.c_str(), command.data(), nullptr, nullptr, output != INVALID_HANDLE_VALUE, flags,
                          nullptr, nullptr, &startup, &process);
}
#else
bool Spawn(const fs::path& tool, const std::vector<std::string>& args, pid_t& pid, const fs::path& output = {}) {
    std::vector<char*> argv{const_cast<char*>(tool.c_str())};
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    if (output.empty()) return posix_spawn(&pid, tool.c_str(), nullptr, nullptr, argv.data(), environ) == 0;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, output.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    const int result = posix_spawn(&pid, tool.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    return result == 0;
}
#endif

fs::path SystemTool(std::string_view name) {
#ifdef TCNYC_UPDATER_TEST_TOOLS
    if (name == "curl") {
#ifdef _WIN32
        return fs::path(TCNYC_UPDATER_TEST_TOOLS) / "curl.exe";
#else
        return fs::path(TCNYC_UPDATER_TEST_TOOLS) / "curl";
#endif
    }
#endif
#ifdef _WIN32
    wchar_t dir[MAX_PATH];
    const auto length = GetSystemDirectoryW(dir, MAX_PATH);
    if (!length || length >= MAX_PATH) throw std::runtime_error("Could not locate Windows System32.");
    return fs::path(dir) / (std::string(name) + ".exe");
#else
    // The usual system directories first, then PATH: NixOS, Guix and some immutable distributions
    // keep curl and tar elsewhere. Only absolute PATH entries count, never the working directory.
    for (const char* dir : {"/usr/bin", "/bin"}) {
        const auto candidate = fs::path(dir) / name;
        if (access(candidate.c_str(), X_OK) == 0) return candidate;
    }
    if (const char* path = std::getenv("PATH")) {
        std::string_view rest(path);
        while (!rest.empty()) {
            const size_t colon = rest.find(':');
            const std::string_view dir = rest.substr(0, colon);
            rest = colon == std::string_view::npos ? std::string_view{} : rest.substr(colon + 1);
            if (dir.empty() || dir.front() != '/') continue;
            const auto candidate = fs::path(std::string(dir)) / name;
            if (access(candidate.c_str(), X_OK) == 0) return candidate;
        }
    }
    return fs::path("/usr/bin") / name;
#endif
}

bool RunTool(std::string_view name, const std::vector<std::string>& args, const std::atomic<bool>& cancel,
             const auto& tick, const fs::path* captured = nullptr) {
    if (cancel) return false;
    const auto timeout = Clock::now() + 10min;
    const auto tool = SystemTool(name);
    CheckedStatus(captured ? *captured : fs::path{});
#ifdef _WIN32
    PROCESS_INFORMATION process{};
    HANDLE output = INVALID_HANDLE_VALUE;
    if (captured) {
        SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
        output = CreateFileW(captured->c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
        if (output == INVALID_HANDLE_VALUE) return false;
    }
    const bool started = Spawn(tool, args, process, CREATE_NO_WINDOW, output);
    if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
    if (!started) return false;
    CloseHandle(process.hThread);
    DWORD code = 1;
    for (;;) {
        const DWORD wait = WaitForSingleObject(process.hProcess, 100);
        if (wait == WAIT_OBJECT_0) {
            GetExitCodeProcess(process.hProcess, &code);
            break;
        }
        if (wait != WAIT_TIMEOUT || cancel || Clock::now() >= timeout) {
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, 5000);
            break;
        }
        tick();
    }
    CloseHandle(process.hProcess);
    return code == 0 && !cancel;
#else
    pid_t pid = 0;
    if (!Spawn(tool, args, pid, captured ? *captured : fs::path{})) return false;
    int status = 0;
    for (;;) {
        const pid_t waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid) return WIFEXITED(status) && WEXITSTATUS(status) == 0 && !cancel;
        if (waited < 0 && errno != EINTR) return false;
        if (cancel || Clock::now() >= timeout) {
            kill(pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            return false;
        }
        tick();
        SDL_Delay(100);
    }
#endif
}

// Validates the whole archive before tar extracts anything: every member must
// be a regular file or a directory, and every name must be a safe relative path
// with no absolute, "." or ".." component. A symlink followed by a file would
// otherwise be able to write outside the unpack directory before the package
// inspection sees it.
void ValidateArchive(const fs::path& archive, const fs::path& dir, const std::atomic<bool>& cancel) {
    const auto names = dir / "archive-names.txt";
    const auto types = dir / "archive-types.txt";
    struct Cleanup {
        fs::path names, types;
        ~Cleanup() {
            std::error_code ec;
            fs::remove(names, ec);
            fs::remove(types, ec);
        }
    } cleanup{names, types};
    if (!RunTool("tar", {"-tf", Utf8(archive)}, cancel, [] {}, &names))
        throw std::runtime_error("Could not read the update archive.");
    const auto listing = ReadFile(names, 8 << 20);
    std::istringstream name_lines(listing);
    uint32_t members = 0;
    for (std::string line; std::getline(name_lines, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        while (line.size() > 1 && line.back() == '/') line.pop_back();
        if (line.empty()) continue;
        if (!SafeRelative(PathFromUtf8(line)))
            throw std::runtime_error("The update archive contains an unsafe filename.");
        members++;
    }
    if (!members) throw std::runtime_error("The update archive is empty.");
    if (!RunTool("tar", {"-tvf", Utf8(archive)}, cancel, [] {}, &types))
        throw std::runtime_error("Could not read the update archive.");
    const auto details = ReadFile(types, 32 << 20);
    std::istringstream type_lines(details);
    for (std::string line; std::getline(type_lines, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line[0] != '-' && line[0] != 'd')
            throw std::runtime_error("The update archive contains a link or special file.");
    }
}

bool Fetch(const std::string& url, const fs::path& out, uint64_t limit,
           const std::atomic<bool>& cancel, const auto& tick) {
    CheckedStatus(out);
    return RunTool("curl", {"-q", "-fsSL", "--proto", "=https", "--proto-redir", "=https", "--retry", "2",
                            "--retry-all-errors", "--connect-timeout", "10", "--max-time", "1800",
                            "--max-filesize", std::to_string(limit), "-C", "-",
                            "-A", "TrueCrimeNYC-Launcher", "-o", Utf8(out), "--", url}, cancel, tick);
}

// FIPS 180-4 SHA-256 of a file, as lowercase hex
std::string Sha256(const fs::path &path) {
    static constexpr uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
        0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
        0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
        0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    const auto block = [&](const uint8_t *p) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) w[i] = uint32_t(p[i * 4]) << 24 | p[i * 4 + 1] << 16 | p[i * 4 + 2] << 8 | p[i * 4 + 3];
        for (int i = 16; i < 64; ++i)
            w[i] = w[i - 16] + (rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3)) + w[i - 7] +
                   (rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10));
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
    };
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("Could not open the update for hashing.");
    std::vector<uint8_t> buffer(1 << 20);
    uint64_t length = 0;
    size_t got = 0;
    while (in.read(reinterpret_cast<char *>(buffer.data()), std::streamsize(buffer.size())), (got = size_t(in.gcount())) > 0) {
        length += got;
        size_t i = 0;
        for (; i + 64 <= got; i += 64) block(buffer.data() + i);
        if (got - i || got < buffer.size()) {  // the last read: pad what is left
            uint8_t tail[128] = {};
            const size_t rest = got - i;
            std::memcpy(tail, buffer.data() + i, rest);
            tail[rest] = 0x80;
            const size_t total = rest + 9 <= 64 ? 64 : 128;
            for (int b = 0; b < 8; ++b) tail[total - 1 - b] = uint8_t((length * 8) >> (8 * b));
            block(tail);
            if (total == 128) block(tail + 64);
            length = ~0ull;  // padded
            break;
        }
    }
    if (in.bad()) throw std::runtime_error("Could not read the update for hashing.");
    if (length != ~0ull) {  // the file was a whole number of reads
        uint8_t tail[64] = {0x80};
        for (int b = 0; b < 8; ++b) tail[63 - b] = uint8_t((length * 8) >> (8 * b));
        block(tail);
    }
    char hex[65];
    for (int i = 0; i < 8; ++i) std::snprintf(hex + i * 8, 9, "%08x", h[i]);
    return hex;
}


struct Version {
    std::array<uint32_t, 3> core{};
    std::vector<std::string> prerelease;
};

bool Numeric(std::string_view text) {
    return !text.empty() && std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; });
}

std::optional<Version> ParseVersion(const std::string& text) {
    static const std::regex pattern(R"(^v?(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?$)");
    std::smatch match;
    if (text.size() > 128 || !std::regex_match(text, match, pattern)) return std::nullopt;
    Version version;
    for (size_t i = 0; i < version.core.size(); ++i) {
        const auto value = match[i + 1].str();
        if (std::from_chars(value.data(), value.data() + value.size(), version.core[i]).ec != std::errc{})
            return std::nullopt;
    }
    std::istringstream ids(match[4].str());
    for (std::string id; std::getline(ids, id, '.');) {
        if (Numeric(id) && id.size() > 1 && id.front() == '0') return std::nullopt;
        version.prerelease.push_back(std::move(id));
    }
    return version;
}

bool OlderVersion(const Version& a, const Version& b) {
    if (a.core != b.core) return a.core < b.core;
    if (a.prerelease.empty() != b.prerelease.empty()) return !a.prerelease.empty();
    const auto less = [](const std::string& lhs, const std::string& rhs) {
        const bool left_numeric = Numeric(lhs), right_numeric = Numeric(rhs);
        if (left_numeric != right_numeric) return left_numeric;
        if (left_numeric && lhs.size() != rhs.size()) return lhs.size() < rhs.size();
        return lhs < rhs;
    };
    return std::lexicographical_compare(a.prerelease.begin(), a.prerelease.end(),
                                        b.prerelease.begin(), b.prerelease.end(), less);
}

bool HexDigest(std::string_view digest) {
    return digest.size() == 64 && digest.find_first_not_of("0123456789abcdefABCDEF") == std::string_view::npos;
}

struct PackageFile {
    fs::path relative;
    bool existed = false;
};

void CheckRequiredFiles(const fs::path& staged, const std::string& release) {
#ifdef _WIN32
    constexpr std::string_view required[] = {kExecutable, "BUILD.txt", "SHA256SUMS", "compiler/clang.exe",
        "compiler/lld-link.exe", "compiler/xbe_recomp.exe", "compiler/TrueCrimeNYC.lib", "compiler/config/game.toml",
#ifdef TCNYC_PAL_SHA1
        kPalExecutable, "compiler/TrueCrimeNYCPAL.lib", "compiler/config/game-pal.toml",
#endif
#ifdef TCNYC_PAL_EU_SHA1
        kPalEuExecutable, "compiler/TrueCrimeNYCPALEU.lib", "compiler/config/game-pal-eu.toml",
#endif
    };
#else
    constexpr std::string_view required[] = {kExecutable, "BUILD.txt", "lib/libshaderc.so.1",
#ifdef TCNYC_PAL_SHA1
        kPalExecutable,
#endif
#ifdef TCNYC_PAL_EU_SHA1
        kPalEuExecutable,
#endif
    };
#endif
    if (ReadFile(staged / "UPDATE-PROTOCOL", 160) != "tcnyc-update-1\n" + release + "\n")
        throw std::runtime_error("This package does not match the requested update version. Install it from its release page.");
    for (const auto file : required) {
        const auto path = staged / file;
        if (!fs::is_regular_file(CheckedStatus(path)) || fs::file_size(path) == 0)
            throw std::runtime_error("The update archive is missing " + std::string(file) + ".");
    }
}

std::vector<PackageFile> InspectPackage(const fs::path& root, const fs::path& staged, const std::string& release) {
    std::set<std::string> seen, files;
    std::vector<PackageFile> result;
    uint64_t bytes = 0;
    for (auto it = fs::recursive_directory_iterator(staged); it != fs::recursive_directory_iterator(); ++it) {
        const auto relative = it->path().lexically_relative(staged);
        if (!SafeRelative(relative)) throw std::runtime_error("An update filename is invalid.");
        const auto status = CheckedStatus(it->path());
        const auto name = Utf8(relative);
        if (!seen.insert(AsciiLower(name)).second) throw std::runtime_error("Duplicate filenames in the update.");
        if (fs::is_directory(status)) continue;
        if (!fs::is_regular_file(status) || fs::hard_link_count(it->path()) != 1)
            throw std::runtime_error("The update contains an unsupported filesystem entry.");
        const auto ext = AsciiLower(Utf8(relative.extension()));
        if (ext == ".xbe" || ext == ".iso") throw std::runtime_error("Game assets cannot be installed by the updater.");
        const auto size = fs::file_size(it->path());
        if (size > kArchiveLimit || bytes > 2 * kArchiveLimit - size || files.size() >= 4096)
            throw std::runtime_error("The unpacked update exceeds its size limit.");
        bytes += size;
        files.insert(name);
        if (Protected(AsciiLower(Utf8(*relative.begin())))) continue;

        const auto target = CheckTarget(root, relative);
        if (fs::exists(target) && !fs::is_regular_file(target))
            throw std::runtime_error("The update would replace a directory.");
        result.push_back({relative, fs::exists(target)});
    }

    CheckRequiredFiles(staged, release);
    if (!files.erase("SHA256SUMS")) throw std::runtime_error("The update manifest is missing.");
    {
        std::istringstream manifest(ReadFile(staged / "SHA256SUMS", 1 << 20));
        for (std::string line; std::getline(manifest, line);) {
            if (line.ends_with('\r')) line.pop_back();
            const auto digest = line.substr(0, 64);
            const auto name = line.size() > 66 ? line.substr(66) : std::string{};
            if (!HexDigest(digest) || line.substr(64, 2) != "  " || !files.erase(name) ||
                Sha256(staged / PathFromUtf8(name)) != AsciiLower(digest))
                throw std::runtime_error("A file does not match the update manifest.");
        }
        if (!files.empty()) throw std::runtime_error("The update manifest does not cover every file.");
    }
    // Move the launch entry point last, after all its dependencies have been placed.
    std::stable_partition(result.begin(), result.end(), [](const auto& file) { return file.relative != kExecutable; });
    return result;
}

void EnsureDirectories(const fs::path& root, const fs::path& relative, std::vector<fs::path>* created = nullptr) {
    auto dir = root;
    for (const auto& component : relative) {
        if (component.empty()) continue;
        const auto next = dir / component;
        CheckedStatus(next);
        if (fs::create_directory(next)) {
            if (created) created->push_back(next);
            SyncDirectory(dir);
        }
        dir = next;
    }
}

void PublishFile(const fs::path& source, const fs::path& root, const fs::path& relative, const fs::path& temp,
                 std::vector<fs::path>* created = nullptr) {
    CheckTarget(root, relative);
    EnsureDirectories(root, relative.parent_path(), created);
    fs::copy_file(source, temp);
    SyncFile(temp);
    CheckTarget(root, relative);
    Replace(temp, root / relative);
}

void RemoveTree(const fs::path& root, const fs::path& relative) {
    CheckTarget(root, relative);
    fs::remove_all(root / relative);
}

void Rollback(const fs::path& root) {
    const auto update = root / "update";
    std::vector<PackageFile> files;
    std::istringstream journal(ReadFile(update / "active", 1 << 20));
    for (std::string line; std::getline(journal, line);) {
        if (line.size() < 3 || (line[0] != '0' && line[0] != '1') || line[1] != '\t')
            throw std::runtime_error("Invalid update recovery journal.");
        const auto relative = PathFromUtf8(std::string_view(line).substr(2));
        if (!SafeRelative(relative) || Protected(AsciiLower(Utf8(*relative.begin()))))
            throw std::runtime_error("Invalid filename in update recovery journal.");
        if (line[0] == '1' && !fs::is_regular_file(CheckTarget(update / "backup", relative)))
            throw std::runtime_error("An update backup is missing.");
        files.push_back({relative, line[0] == '1'});
    }
    for (auto it = files.rbegin(); it != files.rend(); ++it) {
        const auto& file = *it;
        const auto temp = update / "publishing";
        if (fs::is_regular_file(CheckedStatus(temp))) fs::remove(temp);
        if (file.existed) {
            const auto backup = update / "backup" / file.relative;
            const auto original = root / file.relative;
            if (fs::is_regular_file(CheckTarget(root, file.relative)) &&
                fs::file_size(original) == fs::file_size(backup) && Sha256(original) == Sha256(backup)) continue;
            PublishFile(backup, root, file.relative, temp);
        } else if (fs::is_regular_file(CheckTarget(root, file.relative))) {
            fs::remove(root / file.relative);
            SyncDirectory((root / file.relative).parent_path());
        }
    }
    fs::remove(update / "active");
    SyncDirectory(update);
}

void ApplyPackage(const fs::path& root) {
    const auto update = root / "update";
    const auto staged = update / "staged";
    if (fs::exists(CheckedStatus(update / "active"))) Rollback(root);
    const auto release = ReadFile(update / "pending", 128);
    if (release != TCNYC_VERSION) throw std::runtime_error("The installer does not match the staged update version.");
    // Files the previous package installed, so ones removed from this package can go.
    std::set<std::string> previous;
    try {
        std::istringstream manifest(ReadFile(update / "installed", 1 << 20));
        for (std::string line; std::getline(manifest, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) previous.insert(line);
        }
    } catch (const std::exception&) {
    }
    bool journal = false;
    std::vector<fs::path> created_directories;
    try {
        const auto files = InspectPackage(root, staged, release);
        // Files the previous package installed and this one no longer contains. They
        // are backed up and journaled like replaced files, so an interrupted or failed
        // apply restores them with the rest of the install.
        std::map<std::string, fs::path> current;
        std::ostringstream installed;
        for (const auto& file : files) {
            const std::string name = Utf8(file.relative);
            current.emplace(AsciiLower(name), file.relative);
            installed << name << '\n';
        }
        std::vector<fs::path> dropped;
        for (const auto& name : previous) {
            const fs::path relative = PathFromUtf8(name);
            if (!SafeRelative(relative) || Protected(AsciiLower(Utf8(*relative.begin())))) continue;
            const auto replacement = current.find(AsciiLower(Utf8(relative)));
            if (replacement != current.end() && replacement->second == relative) continue;
            if (!fs::is_regular_file(CheckTarget(root, relative))) continue;
            if (replacement != current.end()) {
                // A case-only rename: on a case-insensitive install both names are the same
                // file, which the package replaces. On a case-sensitive install the new
                // name does not exist yet, or is a different file, and the old one goes.
                std::error_code ec;
                const auto other = root / replacement->second;
                const bool other_exists = fs::exists(other, ec);
                if (ec) continue;
                if (other_exists && (fs::equivalent(root / relative, other, ec) || ec)) continue;
            }
            dropped.push_back(relative);
        }
        RemoveTree(update, "backup");
        fs::create_directory(update / "backup");
        std::ostringstream journal_text;
        auto back_up = [&](const fs::path& relative) {
            EnsureDirectories(update / "backup", relative.parent_path());
            const auto backup = update / "backup" / relative;
            fs::copy_file(root / relative, backup);
            SyncFile(backup);
            SyncDirectory(backup.parent_path());
        };
        for (const auto& file : files) {
            journal_text << file.existed << '\t' << Utf8(file.relative) << '\n';
            if (file.existed) back_up(file.relative);
        }
        for (const auto& relative : dropped) {
            journal_text << "1\t" << Utf8(relative) << '\n';
            back_up(relative);
        }
        const auto temp = update / "publishing";
        if (fs::is_regular_file(CheckedStatus(temp))) fs::remove(temp);
        PublishText(update / "active", journal_text.str());
        journal = true;
        // Dropped files go first, under the journal: a failure from here on restores them.
        std::string kept;  // dropped files that could not be removed, for the next update to retry
        for (const auto& relative : dropped) {
            std::error_code ec;
            if (fs::remove(root / relative, ec)) SyncDirectory((root / relative).parent_path());
            else if (ec) kept += Utf8(relative) + '\n';
        }
        for (const auto& file : files) PublishFile(staged / file.relative, root, file.relative, temp, &created_directories);
        Replace(update / "active", update / "committed");
        try {
            PublishText(update / "installed", installed.str() + kept);
        } catch (const std::exception& failure) {
            std::fprintf(stderr, "update: could not record the package files: %s\n", failure.what());
        }
    } catch (...) {
        // A failure after the journal starts is rolled back. A failure before it (a
        // missing or invalid package) must also clear pending/staged, or every
        // launch offers the same update and fails the same way.
        if (journal) {
            // Preserve the journal and backups if any part of restoration fails.
            Rollback(root);
            // Directories this apply created may now be empty; remove those.
            for (auto it = created_directories.rbegin(); it != created_directories.rend(); ++it) {
                std::error_code ec;
                fs::remove(*it, ec);
            }
        } else {
            std::error_code ec;
            fs::remove_all(update / "staged", ec);
            fs::remove_all(update / "backup", ec);
            fs::remove(update / "pending", ec);
            fs::remove(update / "unpacked", ec);
            SyncDirectory(update);
        }
        throw;
    }
}

bool Launch(const fs::path& exe, const std::vector<std::string>& args) {
#ifdef _WIN32
    PROCESS_INFORMATION process{};
    if (!Spawn(exe, args, process, 0)) return false;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
#else
    pid_t pid;
    return Spawn(exe, args, pid);
#endif
}

bool StartInstaller(const fs::path& root, const char* mode) {
    const auto path = fs::path("update") / "staged" / kExecutable;
    if (!fs::is_regular_file(CheckTarget(root, path))) return false;
#ifdef _WIN32
    const auto pid = GetCurrentProcessId();
#else
    const auto pid = getpid();
#endif
    return Launch(root / path, {std::string(mode) + std::to_string(pid)});
}

bool WaitForParent(uint32_t pid) {
#ifdef _WIN32
    HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!parent) return GetLastError() == ERROR_INVALID_PARAMETER;
    const bool exited = WaitForSingleObject(parent, 30000) == WAIT_OBJECT_0;
    CloseHandle(parent);
    return exited;
#else
    const auto deadline = Clock::now() + 30s;
    while (uint32_t(getppid()) == pid) {
        if (Clock::now() >= deadline) return false;
        SDL_Delay(50);
    }
    return true;
#endif
}

} // namespace

void LogLine(const char* format, ...) {
    try {
        // The launcher runs before the game log is open, so it keeps its own short
        // record of the handoff. It also survives a crash during SDL shutdown.
        static const auto start = std::chrono::steady_clock::now();
        char message[1024];
        va_list args;
        va_start(args, format);
        std::vsnprintf(message, sizeof(message), format, args);
        va_end(args);

        const auto dir = app::InstallRootFor(ExecutablePath().parent_path()) / "logs";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::ofstream out(dir / "launcher.log", std::ios::binary | std::ios::app);
        if (!out) return;

        const auto now = std::chrono::system_clock::now();
        const std::time_t time = std::chrono::system_clock::to_time_t(now);
        std::tm local{};
#ifdef _WIN32
        localtime_s(&local, &time);
#else
        localtime_r(&time, &local);
#endif
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        char line[1200];
        std::snprintf(line, sizeof(line), "[%s +%.3fs] %s\n", stamp, elapsed, message);
        out << line;
        out.flush();
    } catch (...) {
        // A diagnostic must never take the launcher down.
    }
}

bool OlderVersion(const std::string& a, const std::string& b) {
    const auto left = ParseVersion(a), right = ParseVersion(b);
    return left && right && OlderVersion(*left, *right);
}

const char* Updater::Current() {
    static const char* version = [] {
// The version override exists for the updater tests and developer builds only.
#if (defined(TCNYC_DEV_TOOLS) && TCNYC_DEV_TOOLS) || defined(TCNYC_UPDATER_TEST_TOOLS)
        const char* test = std::getenv("TCNYC_UPDATE_TEST_VERSION");
#else
        const char* test = nullptr;
#endif
        return test && *test ? test : TCNYC_VERSION;
    }();
    return version;
}

Updater::~Updater() {
    cancel_ = true;
    if (thread_.joinable()) thread_.join();
}

std::string Updater::latest() const {
    std::lock_guard lock(mutex_);
    return latest_;
}

std::string Updater::error() const {
    std::lock_guard lock(mutex_);
    return error_;
}

void Updater::Fail(std::string message) {
    std::lock_guard lock(mutex_);
    error_ = std::move(message);
    state_ = State::Failed;
}

void Updater::Run(void (Updater::*step)()) {
    if (thread_.joinable()) thread_.join();
    cancel_ = false;
    try {
        thread_ = std::thread([this, step] {
            try {
                (this->*step)();
            } catch (const std::exception& failure) {
                Fail(failure.what());
            }
        });
    } catch (const std::exception& failure) {
        Fail(failure.what());
    }
}

void Updater::Check() {
#ifdef TCNYC_PAL
    return;
#endif
    state_ = State::Checking;
    Run(&Updater::DoCheck);
}

void Updater::Download() {
    if (state_ != State::Available && state_ != State::Failed) return;
    bool have_release;
    {
        std::lock_guard lock(mutex_);
        have_release = !archive_url_.empty();
    }
    if (!have_release) {
        Check();
        return;
    }
    state_ = State::Downloading;
    progress_ = 0;
    Run(&Updater::DoDownload);
}

void Updater::Cancel() { cancel_ = true; }

bool Updater::UseStagedRelease(const fs::path& root) {
    const auto pending = root / "update" / "pending";
    if (!fs::is_regular_file(CheckedStatus(pending))) return false;
    const auto staged_version = ReadFile(pending, 128);
    if (!OlderVersion(Current(), staged_version) ||
        !fs::is_regular_file(CheckTarget(root, fs::path("update/staged") / kExecutable))) return false;
    const auto protocol = root / "update" / "staged" / "UPDATE-PROTOCOL";
    if (!fs::is_regular_file(CheckedStatus(protocol)) ||
        ReadFile(protocol, 160) != "tcnyc-update-1\n" + staged_version + "\n") return false;
    std::lock_guard guard(mutex_);
    latest_ = staged_version;
    state_ = State::Ready;
    return true;
}

bool Updater::AdoptStaged() {
#ifdef TCNYC_PAL
    return false;
#endif
    try {
        const auto root = BaseDir();
        InstallLock lock(root);
        return UseStagedRelease(root);
    } catch (const std::exception&) {
        return false;
    }
}

void Updater::DoCheck() {
    const auto root = BaseDir();
    InstallLock lock(root);
    if (UseStagedRelease(root)) return;
    const auto json = root / "update" / "latest.json";
    const auto current = ParseVersion(Current());
    if (!current) {
        state_ = State::Idle;
        return;
    }
    std::optional<Version> best;
    std::string best_tag, archive_url, checksum_url;
    uint64_t size = 0;
    bool read_any = false;
    // Read up to five pages (100 releases), newest first, so a release pushed
    // off the first page is still offered.
    for (uint32_t page = 1; page <= 5; ++page) {
        if (fs::is_regular_file(CheckedStatus(json))) fs::remove(json);
        const std::string url = "https://api.github.com/repos/" + std::string(kRepo) +
                                "/releases?per_page=20&page=" + std::to_string(page);
        if (!Fetch(url, json, 2 << 20, cancel_, [] {})) {
            if (!read_any) throw std::runtime_error("Could not check for updates. Check your connection.");
            break;  // keep what the earlier pages found
        }
        read_any = true;
        const auto releases = ReleaseJsonParser(ReadFile(json, 2 << 20)).Parse();
        for (const auto& release : releases.elements) {
            if (release["draft"].True()) continue;
            const std::string tag(release["tag_name"].String());
            const auto version = ParseVersion(tag);
            if (!version || !OlderVersion(*current, *version) || (best && !OlderVersion(*best, *version))) continue;
            if (current->prerelease.empty() && (release["prerelease"].True() || !version->prerelease.empty())) continue;
            const std::string base = "https://github.com/" + std::string(kRepo) + "/releases/download/" + tag + "/";
            const std::string checksum_name = std::string(kAsset) + ".sha256";
            std::string archive, checksum;
            uint64_t bytes = 0;
            for (const auto& asset : release["assets"].elements) {
                const auto name = asset["name"].String(), download = asset["browser_download_url"].String();
                if (name == kAsset && download == base + kAsset) {
                    archive = download;
                    bytes = asset["size"].Size();
                } else if (name == checksum_name && download == base + checksum_name) {
                    checksum = download;
                }
            }
            if (archive.empty() || checksum.empty() || !bytes || bytes > kArchiveLimit) continue;
            best = version;
            best_tag = tag.starts_with('v') ? tag.substr(1) : tag;
            archive_url = std::move(archive);
            checksum_url = std::move(checksum);
            size = bytes;
        }
        if (releases.elements.size() < 20) break;  // the last page
    }
    {
        std::lock_guard lock(mutex_);
        latest_ = std::move(best_tag);
        archive_url_ = std::move(archive_url);
        checksum_url_ = std::move(checksum_url);
        size_ = size;
    }
    state_ = best ? State::Available : State::Idle;
}

void Updater::DoDownload() {
    const auto root = BaseDir();
    InstallLock lock(root);
    const auto dir = root / "update";
    if (fs::exists(CheckedStatus(dir / "active")) || fs::exists(CheckedStatus(dir / "committed")))
        throw std::runtime_error("Restart the game to finish its pending update.");
    if (UseStagedRelease(root)) return;
    const auto archive = dir / kAsset;
    const auto checksum = dir / (std::string(kAsset) + ".sha256");
    for (const auto& path : {archive, checksum}) if (fs::is_regular_file(CheckedStatus(path))) fs::remove(path);
    const auto tick = [&] {
        std::error_code ec;
        const auto got = fs::file_size(archive, ec);
        if (!ec) progress_ = std::min(1.0f, float(double(got) / double(size_)));
    };
    if (!Fetch(archive_url_, archive, size_, cancel_, tick) || !Fetch(checksum_url_, checksum, 4096, cancel_, [] {})) {
        if (cancel_) state_ = State::Available;
        else Fail("The download failed. Check your connection or get it from the releases page.");
        return;
    }
    const auto expected = ReadFile(checksum, 4096).substr(0, 64);
    if (fs::file_size(archive) != size_ || !HexDigest(expected) || Sha256(archive) != AsciiLower(expected))
        throw std::runtime_error("The download did not match its published size or checksum.");

    state_ = State::Staging;
    const auto unpacked = dir / "unpacked";
    RemoveTree(dir, "unpacked");
    fs::create_directory(unpacked);
    ValidateArchive(archive, dir, cancel_);
    if (!RunTool("tar", {"-xf", Utf8(archive), "-C", Utf8(unpacked), "--no-same-owner", "--no-same-permissions"},
                 cancel_, [] {})) {
        std::error_code ec;
        fs::remove(dir / "archive-names.txt", ec);
        fs::remove(dir / "archive-types.txt", ec);
        throw std::runtime_error("Could not unpack the update.");
    }
    std::error_code listing_ec;
    fs::remove(dir / "archive-names.txt", listing_ec);
    fs::remove(dir / "archive-types.txt", listing_ec);
    auto package = unpacked;
    auto it = fs::directory_iterator(unpacked);
    if (it != fs::directory_iterator()) {
        const auto entry = it->path();
        if (++it == fs::directory_iterator() && fs::is_directory(CheckedStatus(entry))) package = entry;
    }
    InspectPackage(root, package, latest());
    RemoveTree(dir, "staged");
    Replace(package, dir / "staged");
    RemoveTree(dir, "unpacked");
    PublishText(dir / "pending", latest());
    fs::remove(archive);
    fs::remove(checksum);
    state_ = State::Ready;
}

namespace {
fs::path PalExecutable(app::GameRegion region) {
    return BaseDir() / PathFromUtf8(region == app::GameRegion::PalEu ? kPalEuExecutable : kPalExecutable);
}
}

bool PalInstalled(app::GameRegion region) {
    std::error_code ec;
    return fs::is_regular_file(PalExecutable(region), ec);
}

bool LaunchPal(const app::GameSource& source) {
    const auto executable = PalExecutable(source.region);
    LogLine("handoff: starting the PAL executable (%s)", Utf8(executable).c_str());
#ifdef _WIN32
    if (Launch(executable, {"--skip-launcher", "--game-source", Utf8(source.path)})) return true;
#else
    // Keep the process ID for callers such as Steam and command-line wrappers.
    auto exe_arg = executable.native();
    auto source_arg = source.path.native();
    char skip[] = "--skip-launcher";
    char option[] = "--game-source";
    char* argv[] = {exe_arg.data(), skip, option, source_arg.data(), nullptr};
    execv(executable.c_str(), argv);
#endif
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "True Crime NYC", "Could not start the PAL game. Extract the complete release and try again.", nullptr);
    return false;
}

bool LaunchGame(const app::GameSource& source, int instance) {
    // Run the game in its own process, like the PAL builds. The launcher stops
    // taking part once the child exists, so a fault while SDL shuts its window
    // down cannot stop the game from starting.
    bool started = false;
    try {
        const auto executable = ExecutablePath();
        std::vector<std::string> args{"--skip-launcher"};
        if (!source.path.empty()) {
            args.push_back("--game-source");
            args.push_back(Utf8(source.path));
        }
        if (instance > 0) {
            args.push_back("--instance");
            args.push_back(std::to_string(instance));
        }
        LogLine("handoff: starting %s (%zu arguments)", Utf8(executable).c_str(), args.size());
#ifdef _WIN32
        started = Launch(executable, args);
#else
        // Keep the process ID for callers such as Steam and command-line wrappers.
        std::vector<std::string> storage;
        storage.reserve(args.size() + 1);
        storage.push_back(executable.native());
        storage.insert(storage.end(), args.begin(), args.end());
        std::vector<char*> argv;
        argv.reserve(storage.size() + 1);
        for (auto& arg : storage) argv.push_back(arg.data());
        argv.push_back(nullptr);
        execv(storage.front().c_str(), argv.data());
#endif
    } catch (const std::exception& failure) {
        LogLine("handoff: could not start the game: %s", failure.what());
    }
    if (!started) {
        LogLine("handoff: the game process did not start");
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "True Crime NYC",
                                 "Could not start the game. Extract the complete release and try again.", nullptr);
    }
    return started;
}

bool Updater::Relaunch() {
    try {
        const auto root = BaseDir();
        InstallLock lock(root);
        InstallLock game(root, "game.lock");
        if (ReadFile(root / "update" / "pending", 128) != latest()) return false;
        return StartInstaller(root, "--apply-update=");
    } catch (const std::exception& failure) {
        Fail(failure.what());
        return false;
    }
}

std::shared_ptr<void> Updater::HoldGameFiles() {
    try {
        const auto root = BaseDir();
        auto lock = std::make_shared<InstallLock>(root, "game.lock", true);
        if (fs::exists(CheckTarget(root, "update/active")))
            throw std::runtime_error("An interrupted update needs to be recovered.");
        return lock;
    } catch (const std::exception& failure) {
        LogLine("handoff: game files are unavailable: %s", failure.what());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "True Crime NYC",
                                 "The game files are unavailable. Finish any update from the main launcher and try again.", nullptr);
        return {};
    }
}

std::optional<int> Updater::HandleStartup(std::string_view argument) {
#if TCNYC_PAL
    (void)argument;
    return std::nullopt;
#endif
    try {
        const bool apply = argument.starts_with("--apply-update=");
        const bool recover = argument.starts_with("--recover-update=");
        if (apply || recover) {
            const auto self = ExecutablePath();
            const auto staged = self.parent_path();
            const auto update = staged.parent_path();
            const auto root = update.parent_path();
            if (self.filename() != kExecutable || staged.filename() != "staged" || update.filename() != "update")
                throw std::runtime_error("This executable is outside the update staging folder.");
            const auto value = argument.substr(argument.find('=') + 1);
            uint32_t parent = 0;
            const auto result = std::from_chars(value.data(), value.data() + value.size(), parent);
            if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || !parent || !WaitForParent(parent))
                throw std::runtime_error("The previous launcher is still running.");
            {
                InstallLock lock(root);
                InstallLock game(root, "game.lock");
                if (apply) ApplyPackage(root);
                else if (fs::exists(CheckedStatus(update / "active"))) Rollback(root);
            }
            if (!Launch(root / kExecutable, {})) throw std::runtime_error("Could not restart the launcher.");
            return 0;
        }
        const auto root = BaseDir();
        const auto update = root / "update";
        if (!fs::exists(CheckedStatus(update))) return std::nullopt;
        CheckTarget(root, "update/active");
        const bool active = fs::exists(CheckedStatus(update / "active"));
        const bool committed = fs::exists(CheckedStatus(update / "committed"));
        if (!active && !committed) return std::nullopt;
        InstallLock lock(root);
        if (fs::exists(CheckedStatus(update / "active"))) {
            // The staged executable is what would run the recovery. If it is gone,
            // restore the live files here instead of refusing to start the game.
            if (!fs::is_regular_file(CheckedStatus(update / "staged" / kExecutable))) {
                InstallLock game(root, "game.lock");
                Rollback(root);
            } else {
                if (!StartInstaller(root, "--recover-update="))
                    throw std::runtime_error("Could not start the update recovery process.");
                return 0;
            }
        }
        if (fs::exists(CheckedStatus(update / "committed"))) {
            RemoveTree(update, "backup");
            // The staged installer may still be exiting on Windows. A later
            // download can clear its staged files after the process has exited.
            fs::remove(update / "pending");
            fs::remove(update / "committed");
            SyncDirectory(update);
        }
        return std::nullopt;
    } catch (const std::exception& failure) {
        if (!std::getenv("TCNYC_UPDATER_NO_DIALOG"))
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "True Crime NYC update", failure.what(), nullptr);
        return 1;
    }
}

} // namespace launcher
