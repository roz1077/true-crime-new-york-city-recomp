#include <algorithm>
#include <atomic>
#include "core/dev_env.h"
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <sys/stat.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
// windows.h maps these Win32 API names to their W variants; the kernel exports
// below are the guest's own functions.
#undef CreateSymbolicLink
#undef DeleteSymbolicLink
#else
#include <unistd.h>
#endif

#include "app/app.h"
#include "kernel/disc.h"
#include "kernel/kernel.h"
#include "kernel/exports.h"

namespace fs = std::filesystem;

namespace game {
uint32_t RadioOverrideTrack();  // audio_volume.cpp: the Music Player song being opened, or 0
std::filesystem::path MusicFolder();               // extra_songs.cpp
std::filesystem::path ExtraSongPath(uint32_t track);
}

namespace kernel {

// Opens a host path without narrowing it through the active ANSI code page.
// CreateFileW with FILE_SHARE_DELETE lets a guest rename a save while a handle
// (or the CRT stream behind it) is still open; _wfopen does not share delete.
FILE* OpenHostFile(const fs::path& path, const char* mode) {
#ifdef _WIN32
    DWORD access = GENERIC_READ;
    DWORD disposition = OPEN_EXISTING;
    int flags = _O_RDONLY | _O_BINARY;
    if (std::strcmp(mode, "r+b") == 0 || std::strcmp(mode, "w+b") == 0) {
        access = GENERIC_READ | GENERIC_WRITE;
        if (mode[0] == 'w') disposition = CREATE_ALWAYS;
        flags = _O_RDWR | _O_BINARY;
    } else if (std::strcmp(mode, "wb") == 0) {
        access = GENERIC_WRITE;
        disposition = CREATE_ALWAYS;
        flags = _O_WRONLY | _O_BINARY;
    } else if (std::strcmp(mode, "ab") == 0) {
        access = GENERIC_WRITE;
        disposition = OPEN_ALWAYS;
        flags = _O_WRONLY | _O_APPEND | _O_BINARY;
    }
    HANDLE handle = CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return nullptr;
    const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle), flags);
    if (fd < 0) {
        ::CloseHandle(handle);
        return nullptr;
    }
    FILE* fp = _fdopen(fd, mode);
    if (!fp) _close(fd);
    return fp;
#else
    return std::fopen(path.string().c_str(), mode);
#endif
}

namespace {

std::mutex g_links_lock;
std::map<std::string, std::string> g_links;    // upper case "\??\D:" -> "\Device\CdRom0"
std::map<std::string, fs::path> g_devices;     // upper case device -> host directory
std::map<fs::path, fs::path> g_resolved;       // Repeated streaming opens skip the directory scan.
constexpr char kCdRom[] = "\\DEVICE\\CDROM0";

std::string Upper(std::string s) {
    for (char& c : s) c = char(std::toupper(uint8_t(c)));
    return s;
}

std::string Normalize(std::string p) {
    std::replace(p.begin(), p.end(), '/', '\\');
    std::string out;
    for (char c : p) {
        if (c == '\\' && !out.empty() && out.back() == '\\') continue;
        out += c;
    }
    while (out.size() > 1 && out.back() == '\\') out.pop_back();
    return out;
}

// true when p is root or below it, both already resolved
bool Within(const fs::path& root, const fs::path& p) {
    const fs::path rel = p.lexically_relative(root);
    if (rel.empty() || rel.is_absolute()) return false;
    return rel == "." || *rel.begin() != "..";
}

// host lookup that ignores case on file systems that do not. Guest names are untrusted: '..' never
// climbs above the mapped device, and the resolved host path must stay inside it even through a
// symlink or junction.
fs::path MatchCase(const fs::path& dir, const std::vector<std::string>& parts, bool cache) {
    fs::path cache_key = dir;
    for (const std::string& part : parts) cache_key /= part;
    if (cache) {
        if (auto hit = g_resolved.find(cache_key); hit != g_resolved.end()) {
            // R10: the destination may have been replaced by a symlink or junction
            // since it was cached. Revalidate both the containment and that the path
            // still names the same existing file before trusting the hit.
            std::error_code ec;
            const fs::path root = fs::weakly_canonical(dir, ec);
            if (!ec) {
                const fs::path resolved = fs::weakly_canonical(hit->second, ec);
                if (!ec && resolved == hit->second && Within(root, resolved) && fs::exists(resolved, ec) && !ec)
                    return hit->second;
            }
            g_resolved.erase(hit);
        }
    }
    fs::path cur = dir;
    size_t depth = 0;
    for (auto& part : parts) {
        if (part.empty() || part == ".") continue;
        if (part == "..") {
            if (depth) {
                cur = cur.parent_path();
                --depth;
            }
            continue;
        }
        fs::path direct = cur / part;
        std::error_code ec;
        if (fs::exists(direct, ec)) {
            cur = direct;
            ++depth;
            continue;
        }
        bool found = false;
        std::string want = Upper(part);
        for (auto& e : fs::directory_iterator(cur, ec)) {
            if (Upper(e.path().filename().string()) == want) {
                cur = e.path();
                found = true;
                break;
            }
        }
        if (!found) cur = direct;
        ++depth;
    }
    std::error_code ec;
    const fs::path root = fs::weakly_canonical(dir, ec);
    if (ec) return {};
    const fs::path resolved = fs::weakly_canonical(cur, ec);
    if (ec || !Within(root, resolved)) {
        LOGW("files: refusing path outside %s: %s", dir.string().c_str(), cur.string().c_str());
        g_resolved.erase(cache_key);
        return {};
    }
    if (cache && fs::exists(resolved, ec) && !ec) g_resolved.emplace(std::move(cache_key), resolved);
    return resolved;
}

bool PathUnchanged(const fs::path& path) {
    if (path.empty()) return false;
    std::error_code ec;
    return fs::weakly_canonical(path, ec) == path && !ec;
}

struct File : Object {
    fs::path host;
    std::string xbox;
    FILE* fp = nullptr;
    const Disc::Entry* entry = nullptr;  // on the disc image instead of a host path
    bool dir = false;
    bool writable = false;  // opened for writing, so flush/truncate has data to commit
    bool read_only = false; // CDROM0 content (disc or extracted assets)
    uint64_t pos = 0;
    std::vector<fs::directory_entry> entries;
    std::vector<const Disc::Entry*> disc_entries;
    size_t next = 0;
    bool listed = false;
    bool delete_on_close = false;
    bool host_decoded = false;  // an MP3, OGG, FLAC or WAV radio replacement: the game's WMA decoder is bypassed
    uint32_t options = 0;       // the create/open options, for TCNYC_IO_TRACE (sync I/O is 0x30)
#if TCNYC_DEV_TOOLS
    bool trace_wma = false;
#endif
    std::mutex io;  // threads sharing a handle: seek and transfer stay together
    File() : Object(ObjType::File) {}
    ~File() override {
        if (fp && std::fclose(fp) != 0) LOGW("files: close failed for %s", host.string().c_str());
        // File bodies are allocated by ObReferenceObjectByHandle. Their address
        // lookup must not keep a closed file (and its CRT stream) alive forever.
        if (guest) {
            Unbind(guest);
            mem::FreeSystem(guest);
        }
        if (delete_on_close && PathUnchanged(host)) {
            std::error_code ec;
            fs::remove(host, ec);
        }
    }
};

#if TCNYC_DEV_TOOLS
void TraceWmaOpen(File& file) {
    static const bool enabled = [] {
        const char* flag = DevEnv("TCNYC_TEST_AUDIO_TRACE");
        return flag && std::strcmp(flag, "1") == 0;
    }();
    if (!enabled || file.dir) return;
    const std::string path = Upper(file.xbox);
    file.trace_wma = path.find("\\DATA\\SOUNDS\\") != std::string::npos && path.ends_with(".WMA");
    if (file.trace_wma) LOGR("audio trace: opened %s", file.xbox.c_str());
}
#endif

namespace {
// TCNYC_IO_TRACE: file reads that cost long enough to show as a hitch, the options each handle was
// opened with, and a summary at exit. std::getenv rather than DevEnv, because measuring a release
// build (the one a handheld runs) is the point, as with TCNYC_SAMPLE.
bool IoTrace() {
    static const bool on = std::getenv("TCNYC_IO_TRACE") != nullptr;
    return on;
}

struct IoStats {
    std::atomic<uint64_t> reads{0}, bytes{0}, writes{0}, write_bytes{0};
    std::atomic<uint64_t> slow{0}, slow_bytes{0}, max_us{0};
    std::atomic<uint64_t> events{0}, apcs{0}, sync{0};
};
IoStats g_io_stats;
std::atomic<int> g_io_slow_logged{0}, g_io_open_logged{0};

void TraceOpen(const File& f, uint32_t options) {
    if (!IoTrace() || f.dir) return;
    if (g_io_open_logged.fetch_add(1, std::memory_order_relaxed) >= 60) return;
    // LOGR, not LOGI: a release build (NDEBUG) logs only Report and Error, and measuring a release build is
    // the point of the switch.
    LOGR("io: open %s options 0x%X", f.xbox.c_str(), options);
}

void NoteIo(bool write, const std::string& name, uint64_t pos, uint32_t n, uint32_t event, uint32_t apc,
            uint32_t options, std::chrono::steady_clock::time_point start) {
    if (!IoTrace()) return;
    const auto us = uint64_t(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
    if (write) {
        g_io_stats.writes.fetch_add(1, std::memory_order_relaxed);
        g_io_stats.write_bytes.fetch_add(n, std::memory_order_relaxed);
    } else {
        g_io_stats.reads.fetch_add(1, std::memory_order_relaxed);
        g_io_stats.bytes.fetch_add(n, std::memory_order_relaxed);
        if (event) g_io_stats.events.fetch_add(1, std::memory_order_relaxed);
        if (apc) g_io_stats.apcs.fetch_add(1, std::memory_order_relaxed);
        if (options & 0x30) g_io_stats.sync.fetch_add(1, std::memory_order_relaxed);
        if (us >= 3000) {
            g_io_stats.slow.fetch_add(1, std::memory_order_relaxed);
            g_io_stats.slow_bytes.fetch_add(n, std::memory_order_relaxed);
            if (g_io_slow_logged.fetch_add(1, std::memory_order_relaxed) < 40)
                LOGR("io: read %s %u bytes at %llu in %.1f ms (event %d apc %d sync %d)", name.c_str(), n,
                     static_cast<unsigned long long>(pos), double(us) / 1000.0, event ? 1 : 0, apc ? 1 : 0,
                     (options & 0x30) ? 1 : 0);
        }
    }
    uint64_t max_us = g_io_stats.max_us.load(std::memory_order_relaxed);
    while (us > max_us && !g_io_stats.max_us.compare_exchange_weak(max_us, us, std::memory_order_relaxed)) {
    }
}
}  // namespace


struct Link : Object {
    std::string target;
    Link() : Object(ObjType::SymLink) {}
};

// Flushes a CRT stream through the host filesystem. fflush alone only reaches the
// CRT buffer; the commit is what tells us a full or failing disk actually failed.
bool SyncStream(FILE* fp) {
    if (!fp) return true;
    if (std::fflush(fp) != 0) return false;
#ifdef _WIN32
    const int fd = _fileno(fp);
    return fd >= 0 && _commit(fd) == 0;
#else
    const int fd = fileno(fp);
    return fd >= 0 && fsync(fd) == 0;
#endif
}

// Shutdown can end the process with _Exit before File destructors run. Weak
// entries let FlushOpenFiles commit every writable stream without keeping a
// closed file (or its CRT stream) alive.
std::mutex g_open_lock;
std::vector<std::weak_ptr<File>> g_open_files;

void RegisterFile(const std::shared_ptr<File>& f) {
    std::lock_guard<std::mutex> lock(g_open_lock);
    if (g_open_files.size() > 64) std::erase_if(g_open_files, [](const auto& w) { return w.expired(); });
    g_open_files.push_back(f);
}

uint64_t ToFileTime(fs::file_time_type t) {
    using namespace std::chrono;
    auto sys = time_point_cast<nanoseconds>(t - fs::file_time_type::clock::now() + system_clock::now());
    return uint64_t(sys.time_since_epoch().count() / 100 + 116444736000000000ll);
}

void FillNetworkInfo(uint32_t out, uint64_t t, uint64_t size, bool dir) {
    for (int i = 0; i < 4; i++) W64(out + i * 8, t);
    W64(out + 0x20, (size + 0xFFF) & ~0xFFFull);
    W64(out + 0x28, size);
    W32(out + 0x30, dir ? 0x10 : 0x80);  // FILE_ATTRIBUTE_DIRECTORY or NORMAL
}

void FillNetworkInfo(uint32_t out, const fs::path& p) {
    std::error_code ec;
    bool dir = fs::is_directory(p, ec);
    const auto written = fs::last_write_time(p, ec);
    const uint64_t time = ec ? 0 : ToFileTime(written);
    const uint64_t size = dir || ec ? 0 : fs::file_size(p, ec);
    FillNetworkInfo(out, time, size, dir);
}

void FillNetworkInfo(uint32_t out, const Disc::Entry& e) {
    FillNetworkInfo(out, MountedDisc()->Time(), e.dir ? 0 : e.size, e.dir);
}

bool WildMatch(const std::string& pat, const std::string& s) {
    size_t p = 0, i = 0, star = std::string::npos, mark = 0;
    while (i < s.size()) {
        if (p < pat.size() && (pat[p] == '?' || std::toupper(uint8_t(pat[p])) == std::toupper(uint8_t(s[i])))) {
            p++;
            i++;
        } else if (p < pat.size() && pat[p] == '*') {
            star = p++;
            mark = i;
        } else if (star != std::string::npos) {
            p = star + 1;
            i = ++mark;
        } else {
            return false;
        }
    }
    while (p < pat.size() && pat[p] == '*') p++;
    return p == pat.size();
}

// past 2 GB a long offset wraps on Windows
bool ResizeStream(FILE* fp, uint64_t size, bool shrink_only) {
    if (size > INT64_MAX) return false;
#ifdef _WIN32
    const int fd = _fileno(fp);
    struct _stat64 info{};
    if (shrink_only && _fstat64(fd, &info) != 0) return false;
    return (shrink_only && size >= uint64_t(info.st_size)) || _chsize_s(fd, size) == 0;
#else
    const int fd = fileno(fp);
    struct stat info{};
    if (shrink_only && fstat(fd, &info) != 0) return false;
    return (shrink_only && size >= uint64_t(info.st_size)) || ftruncate(fd, off_t(size)) == 0;
#endif
}

bool Seek(FILE* fp, uint64_t pos) {
    if (pos > INT64_MAX) return false;
#ifdef _WIN32
    return _fseeki64(fp, int64_t(pos), SEEK_SET) == 0;
#else
    return fseeko(fp, off_t(pos), SEEK_SET) == 0;
#endif
}

void IoStatus(uint32_t iosb, uint32_t status, uint32_t info) {
    if (!iosb) return;
    if (!mem::IsCommitted(iosb) || !mem::IsCommitted(iosb + 4)) return;
    W32(iosb, status);
    W32(iosb + 4, info);
}

std::shared_ptr<File> FileFromHandle(uint32_t h) {
    ObjectRef o = FromHandle(h);
    if (!o || o->type != ObjType::File) return nullptr;
    return std::static_pointer_cast<File>(o);
}

std::string ObjectName(uint32_t attrs, uint32_t* root = nullptr) {
    if (root) *root = R32(attrs);
    return ReadAnsiString(R32(attrs + 4));
}

void Complete(uint32_t event, uint32_t apc, uint32_t apc_ctx, uint32_t iosb) {
    if (event)
        if (ObjectRef e = FromHandle(event)) SetEvent(*e);
    if (apc && Current()) QueueApc(*Current(), apc, apc_ctx, iosb, 0);
}

}  // namespace

// Best-effort save durability at process exit: commit every writable stream that
// is still open before _Exit bypasses the File destructors.
void LogIoStats() {
    if (!IoTrace()) return;
    const auto load = [](const std::atomic<uint64_t>& v) { return v.load(std::memory_order_relaxed); };
    LOGR("io: %llu reads of %.1f MB, %llu slow (%.1f MB); %llu writes of %.1f MB; max %.1f ms; "
         "event %llu, apc %llu, sync %llu",
         (unsigned long long)load(g_io_stats.reads), double(load(g_io_stats.bytes)) / 1e6,
         (unsigned long long)load(g_io_stats.slow), double(load(g_io_stats.slow_bytes)) / 1e6,
         (unsigned long long)load(g_io_stats.writes), double(load(g_io_stats.write_bytes)) / 1e6,
         double(load(g_io_stats.max_us)) / 1000.0, (unsigned long long)load(g_io_stats.events),
         (unsigned long long)load(g_io_stats.apcs), (unsigned long long)load(g_io_stats.sync));
}

void FlushOpenFiles() {
    std::vector<std::shared_ptr<File>> files;
    {
        std::lock_guard<std::mutex> lock(g_open_lock);
        std::erase_if(g_open_files, [](const auto& w) { return w.expired(); });
        for (auto& w : g_open_files)
            if (auto f = w.lock()) files.push_back(std::move(f));
    }
    for (auto& f : files) {
        std::lock_guard<std::mutex> io(f->io);
        if (f->fp && f->writable && !SyncStream(f->fp))
            LOGW("files: shutdown flush failed for %s", f->host.string().c_str());
    }
}

std::string ReadAnsiString(uint32_t str) {
    if (!str || !mem::IsCommitted(str) || !mem::IsCommitted(str + 3)) return {};
    uint16_t len = R16(str);
    uint32_t buf = R32(str + 4);
    if (!buf || !len || uint64_t(buf) + len > 0x100000000ull) return {};
    if (!mem::IsCommitted(buf) || !mem::IsCommitted(buf + len - 1)) return {};
    for (uint32_t off = 0; off < len; off += 0x1000)
        if (!mem::IsCommitted(buf + off)) return {};
    return std::string(reinterpret_cast<const char*>(HostPtr(buf)), len);
}

bool GuestWritable(uint32_t va, uint32_t len) {
    if (!len) return true;
    if (!va || uint64_t(va) + len > 0x100000000ull) return false;
    const uint64_t end = uint64_t(va) + len;
    for (uint64_t p = uint64_t(va) & ~uint64_t(mem::kPage - 1); p < end; p += mem::kPage)
        if (!mem::IsCommitted(uint32_t(p))) return false;
    return true;
}

std::string ReadCString(uint32_t va, size_t max) {
    std::string s;
    if (!va) return s;
    for (size_t i = 0; i < max; i++) {
        if (!mem::IsCommitted(va + uint32_t(i))) break;
        char c = char(G<uint8_t>(va + uint32_t(i)));
        if (!c) break;
        s += c;
    }
    return s;
}

void MountDefaults(const fs::path& game_dir, const fs::path& data_dir) {
    std::lock_guard<std::mutex> lock(g_links_lock);
    g_resolved.clear();
    fs::path d = data_dir;
    g_devices[kCdRom] = game_dir;
    g_devices["\\DEVICE\\HARDDISK0\\PARTITION1"] = d / "E";
    g_devices["\\DEVICE\\HARDDISK0\\PARTITION2"] = d / "C";
    g_devices["\\DEVICE\\HARDDISK0\\PARTITION3"] = d / "X";
    g_devices["\\DEVICE\\HARDDISK0\\PARTITION4"] = d / "Y";
    g_devices["\\DEVICE\\HARDDISK0\\PARTITION5"] = d / "Z";
    for (auto& [k, v] : g_devices) {
        std::error_code ec;
        fs::create_directories(v, ec);
    }
    g_links["\\??\\D:"] = "\\Device\\CdRom0";
    const Disc* disc = MountedDisc();
    LOGI("files: D: is %s, hard disk under %s", disc ? disc->Path().string().c_str() : game_dir.string().c_str(),
         data_dir.string().c_str());
}

bool CreateSymbolicLink(const std::string& link, const std::string& target) {
    std::lock_guard<std::mutex> lock(g_links_lock);
    g_resolved.clear();
    g_links[Upper(Normalize(link))] = Normalize(target);
    LOGI("files: link %s -> %s", link.c_str(), target.c_str());
    return true;
}

bool DeleteSymbolicLink(const std::string& link) {
    std::lock_guard<std::mutex> lock(g_links_lock);
    g_resolved.clear();
    return g_links.erase(Upper(Normalize(link))) != 0;
}

std::string QuerySymbolicLink(const std::string& link) {
    std::lock_guard<std::mutex> lock(g_links_lock);
    auto it = g_links.find(Upper(Normalize(link)));
    return it == g_links.end() ? std::string() : it->second;
}

namespace {

struct Target {
    fs::path host;                       // empty on the disc image or unmapped
    bool disc = false;                   // on the mounted disc image
    const Disc::Entry* entry = nullptr;  // there: the file or folder, null when missing
    bool read_only = false;              // CDROM0 with extracted files: game data must not be written
};

Target Resolve(const std::string& xbox_path, uint32_t root_handle) {
    std::string p = Normalize(xbox_path);
    if (root_handle) {
        if (auto f = FileFromHandle(root_handle)) p = Normalize(f->xbox + "\\" + p);
    }
    if (p.size() >= 2 && p[1] == ':') p = "\\??\\" + p;
    std::lock_guard<std::mutex> lock(g_links_lock);
    for (int depth = 0; depth < 8; depth++) {
        std::string up = Upper(p);
        bool changed = false;
        for (auto& [k, v] : g_links) {
            if (up.compare(0, k.size(), k) == 0 && (up.size() == k.size() || up[k.size()] == '\\')) {
                p = v + p.substr(k.size());
                changed = true;
                break;
            }
        }
        if (!changed) break;
    }
    std::string up = Upper(p);
    const std::pair<const std::string, fs::path>* best = nullptr;
    for (auto& e : g_devices)
        if (up.compare(0, e.first.size(), e.first) == 0 && (up.size() == e.first.size() || up[e.first.size()] == '\\'))
            if (!best || e.first.size() > best->first.size()) best = &e;
    if (!best) return {};
    std::vector<std::string> parts;
    std::string rest = p.substr(best->first.size());
    size_t s = 0;
    while (s < rest.size()) {
        size_t e = rest.find('\\', s);
        if (e == std::string::npos) e = rest.size();
        if (e > s) parts.push_back(rest.substr(s, e - s));
        s = e + 1;
    }
    // no Xbox name holds ':', and fs::path would take "D:" as a host drive
    for (auto& part : parts)
        if (part.find(':') != std::string::npos) return {};
    if (best->first == kCdRom) {
        if (const Disc* disc = MountedDisc()) return {{}, true, disc->Find(parts), true};
        return {MatchCase(best->second, parts, true), false, nullptr, true};
    }
    return {MatchCase(best->second, parts, true)};
}

// the disc image is read only: nothing on it is created, replaced or written
uint32_t OpenOnDisc(const Disc::Entry* e, const std::string& name, uint32_t handle_out, uint32_t iosb, uint32_t disposition,
                    uint32_t options) {
    if (!e && (disposition == 1 || disposition == 4)) {  // FILE_OPEN or FILE_OVERWRITE need an existing file
        LOG_FIRST(200, "missing %s", name.c_str());
        IoStatus(iosb, kObjectNameNotFound, 5);
        return kObjectNameNotFound;
    }
    if (!e || (disposition != 1 && disposition != 3)) {
        IoStatus(iosb, kAccessDenied, 0);
        return kAccessDenied;
    }
    if (e->dir && !(options & 1) && (options & 0x40)) {  // options 1 FILE_DIRECTORY_FILE, 0x40 FILE_NON_DIRECTORY_FILE
        IoStatus(iosb, kFileIsADirectory, 0);
        return kFileIsADirectory;
    }
    auto f = std::make_shared<File>();
    f->xbox = Normalize(name);
    f->entry = e;
    f->dir = e->dir;
    f->read_only = true;
    f->options = options;
#if TCNYC_DEV_TOOLS
    TraceWmaOpen(*f);
#endif
    RegisterFile(f);
    TraceOpen(*f, options);
    W32(handle_out, NewHandle(f));
    IoStatus(iosb, kSuccess, 1);
    LOG_FIRST(400, "open %s", name.c_str());
    return kSuccess;
}

// WMA first (the game decodes it), then the formats decoded by the port
constexpr int kNoAudio = 5;
int AudioRank(const std::string& ext) {
    return ext == ".WMA" ? 0 : ext == ".MP3" ? 1 : ext == ".OGG" ? 2 : ext == ".FLAC" ? 3 : ext == ".WAV" ? 4 : kNoAudio;
}

// The best replacement per track stem ("6012"): <track>.wma, .mp3, .ogg, .flac or .wav in any letter
// case, WMA first since the game decodes it itself. The folder is listed once and listed again only when
// its modification time changes (a file added, removed or renamed), not on every song start.
struct Replacement {
    fs::path path;
    int rank = kNoAudio;
};

std::optional<Replacement> TrackReplacement(const fs::path& folder, const std::string& stem) {
    static std::mutex lock;
    static fs::path listed_folder;
    static fs::file_time_type listed_time;
    static std::unordered_map<std::string, Replacement> by_stem;
    std::error_code ec;
    const auto time = fs::last_write_time(folder, ec);
    if (ec) return std::nullopt;
    std::lock_guard guard(lock);
    if (folder != listed_folder || time != listed_time) {
        by_stem.clear();
        for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
            const fs::path& candidate = it->path();
            // u8string: other songs in the folder may have names the ANSI code page cannot hold
            const std::u8string stem8 = candidate.stem().u8string(), ext8 = candidate.extension().u8string();
            const int rank = AudioRank(Upper(std::string(ext8.begin(), ext8.end())));
            if (rank == kNoAudio) continue;
            std::error_code type_ec;
            if (!fs::is_regular_file(candidate, type_ec)) continue;
            Replacement& best = by_stem[Upper(std::string(stem8.begin(), stem8.end()))];
            if (rank < best.rank) best = {candidate, rank};
        }
        listed_folder = folder;
        listed_time = time;
    }
    const auto found = by_stem.find(stem);
    if (found == by_stem.end()) return std::nullopt;
    return found->second;
}

bool OpenRadioOverride(const std::string& name, uint32_t handle_out, uint32_t iosb) {
    const uint32_t track = game::RadioOverrideTrack();
    if (!track) return false;
    const std::string filename = std::to_string(track) + ".WMA";
    if (!Upper(Normalize(name)).ends_with("\\DATA\\SOUNDS\\" + filename)) return false;

    const fs::path folder = game::MusicFolder();
    if (folder.empty()) return false;

    // An extra song (a track past the game's own) is the file the song list was built from. Only the
    // game's own tracks are replaced by name, so a file called e.g. 6095.mp3 cannot take over the extra
    // song that happens to get track 6095.
    fs::path path = game::ExtraSongPath(track);
    int best = kNoAudio;
    if (!path.empty()) {
        const std::u8string ext8 = path.extension().u8string();
        best = AudioRank(Upper(std::string(ext8.begin(), ext8.end())));
    }

    if (path.empty()) {
        if (const auto found = TrackReplacement(folder, std::to_string(track))) path = found->path, best = found->rank;
    }
    if (path.empty()) return false;
    FILE* fp = OpenHostFile(path, "rb");
    if (!fp) return false;
    if (best == 0) {
        constexpr uint8_t kAsfHeader[] = {
            0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11, 0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C,
        };
        uint8_t header[sizeof(kAsfHeader)];
        if (std::fread(header, 1, sizeof(header), fp) != sizeof(header) ||
            std::memcmp(header, kAsfHeader, sizeof(header)) != 0 || std::fseek(fp, 0, SEEK_SET) != 0) {
            std::fclose(fp);
            LOGW("radio: invalid ASF header in replacement for track %u", track);
            return false;
        }
    }

    auto f = std::make_shared<File>();
    f->xbox = Normalize(name);
    f->host = path;
    f->fp = fp;
    f->read_only = true;
    f->host_decoded = best != 0;
#if TCNYC_DEV_TOOLS
    TraceWmaOpen(*f);
#endif
    RegisterFile(f);
    W32(handle_out, NewHandle(f));
    IoStatus(iosb, kSuccess, 1);
    LOGR("radio: opened local replacement %s for track %u", reinterpret_cast<const char*>(path.filename().u8string().c_str()), track);
    return true;
}

}  // namespace

fs::path HostDecodedRadioPath(uint32_t handle) {
    const std::shared_ptr<File> f = FileFromHandle(handle);
    return f && f->host_decoded ? f->host : fs::path{};
}

fs::path ResolvePath(const std::string& xbox_path, uint32_t root_handle) { return Resolve(xbox_path, root_handle).host; }

bool MovieOpen() {
    std::lock_guard<std::mutex> lock(g_open_lock);
    for (const auto& w : g_open_files) {
        auto f = w.lock();
        if (!f || f->xbox.size() < 4) continue;
        std::string ext = f->xbox.substr(f->xbox.size() - 4);
        for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
        if (ext == ".bik") return true;
    }
    return false;
}

bool ReadGameFile(const std::string& xbox_path, std::vector<uint8_t>& out) {
    constexpr uint64_t kMaxGameFileBytes = 64ull << 20;  // DSP images are small; keep a malformed image from OOMing
    Target t = Resolve(xbox_path, 0);
    if (t.disc) {
        if (!t.entry || t.entry->dir || t.entry->size > kMaxGameFileBytes) return false;
        out.resize(t.entry->size);
        return MountedDisc()->Read(*t.entry, 0, out.data(), t.entry->size) == t.entry->size;
    }
    std::error_code ec;
    const uintmax_t size = fs::file_size(t.host, ec);
    if (ec || size > kMaxGameFileBytes) return false;
    std::ifstream f(t.host, std::ios::binary);
    if (!f) return false;
    out.resize(size_t(size));
    return f.read(reinterpret_cast<char*>(out.data()), std::streamsize(out.size())).good();
}

// exports

KERNEL_EXPORT(190, NtCreateFile, 9) {
    uint32_t handle_out = a[0], access = a[1], attrs = a[2], iosb = a[3], disposition = a[7], options = a[8];
    uint32_t root;
    std::string name = ObjectName(attrs, &root);
    Target t = Resolve(name, root);
    if (t.read_only && disposition == 1 && (options & 0x1001) == 0 && OpenRadioOverride(name, handle_out, iosb))
        return kSuccess;
    if (t.disc) return OpenOnDisc(t.entry, name, handle_out, iosb, disposition, options);
    const fs::path& host = t.host;
    if (host.empty()) {
        LOGW("NtCreateFile: no mapping for %s", name.c_str());
        IoStatus(iosb, kObjectPathNotFound, 0);
        return kObjectPathNotFound;
    }
    std::error_code ec;
    bool exists = fs::exists(host, ec);
    bool is_dir = exists && fs::is_directory(host, ec);
    // CDROM0 is read-only media, including when its files are extracted.
    if (t.read_only && !exists) {
        IoStatus(iosb, kObjectNameNotFound, 5);
        return kObjectNameNotFound;
    }
    if (t.read_only && ((disposition != 1 && disposition != 3) || (options & 0x1000))) {
        IoStatus(iosb, kAccessDenied, 0);
        return kAccessDenied;
    }
    bool want_dir = options & 1;
    if (want_dir && exists && !is_dir) {
        IoStatus(iosb, kNotADirectory, 0);
        return kNotADirectory;
    }
    uint32_t info = 1;  // FILE_OPENED
    if (want_dir || is_dir) {
        if (!exists) {
            if (disposition == 1 || disposition == 4) {
                IoStatus(iosb, kObjectNameNotFound, 5);
                return kObjectNameNotFound;
            }
            fs::create_directories(host, ec);
            if (ec && !fs::is_directory(host, ec)) {
                IoStatus(iosb, kAccessDenied, 0);
                return kAccessDenied;
            }
            info = 2;
        } else if (disposition == 2) {
            IoStatus(iosb, kObjectNameCollision, 4);
            return kObjectNameCollision;
        }
        if (!want_dir && (options & 0x40)) {
            IoStatus(iosb, kFileIsADirectory, 0);
            return kFileIsADirectory;
        }
        auto f = std::make_shared<File>();
        f->host = host;
        f->xbox = Normalize(name);
        f->dir = true;
        f->read_only = t.read_only;
        RegisterFile(f);
        W32(handle_out, NewHandle(f));
        IoStatus(iosb, kSuccess, info);
        return kSuccess;
    }
    const char* mode = nullptr;
    // A read-only desired access must not ask the host for write permission: a
    // read-only asset would otherwise fail its first open.
    const bool read_request = access && (access & 0x40000000u) == 0 && (access & 0x2u) == 0;
    switch (disposition) {
        case 0:  // supersede
        case 5:  // overwrite if
            mode = "w+b";
            info = exists ? 3 : 2;
            break;
        case 1:  // open
            if (!exists) {
                LOG_FIRST(200, "missing %s", name.c_str());
                IoStatus(iosb, kObjectNameNotFound, 5);
                return kObjectNameNotFound;
            }
            mode = read_request ? "rb" : "r+b";
            break;
        case 2:  // create
            if (exists) {
                IoStatus(iosb, kObjectNameCollision, 4);
                return kObjectNameCollision;
            }
            mode = "w+b";
            info = 2;
            break;
        case 3:  // open if
            mode = !exists ? "w+b" : read_request ? "rb" : "r+b";
            info = exists ? 1 : 2;
            break;
        case 4:  // overwrite
            if (!exists) {
                IoStatus(iosb, kObjectNameNotFound, 5);
                return kObjectNameNotFound;
            }
            mode = "w+b";
            info = 3;
            break;
        default:
            return kInvalidParameter;
    }
    // a file in a missing folder fails as on the console: XAPI probes saves with OPEN_ALWAYS
    if (!exists && !fs::is_directory(host.parent_path(), ec)) {
        IoStatus(iosb, kObjectPathNotFound, 0);
        return kObjectPathNotFound;
    }
    FILE* fp = OpenHostFile(host, mode);
    bool writable = std::strchr(mode, '+') != nullptr;
    if (!fp && std::string(mode) == "r+b") {  // read only media
        fp = OpenHostFile(host, "rb");
        writable = false;
    }
    if (!fp) {
        LOGW("NtCreateFile: cannot open %s", host.string().c_str());
        IoStatus(iosb, kAccessDenied, 0);
        return kAccessDenied;
    }
    auto f = std::make_shared<File>();
    f->host = host;
    f->xbox = Normalize(name);
    f->fp = fp;
    f->writable = writable && fp != nullptr;
    f->read_only = t.read_only;
    f->delete_on_close = options & 0x1000;  // FILE_DELETE_ON_CLOSE
    f->options = options;
#if TCNYC_DEV_TOOLS
    TraceWmaOpen(*f);
#endif
    RegisterFile(f);
    TraceOpen(*f, options);
    W32(handle_out, NewHandle(f));
    IoStatus(iosb, kSuccess, info);
    LOG_FIRST(400, "open %s", name.c_str());
    return kSuccess;
}

KERNEL_EXPORT(202, NtOpenFile, 6) {
    // same as create with FILE_OPEN
    uint32_t handle_out = a[0], access = a[1], attrs = a[2], iosb = a[3], options = a[5];
    uint32_t root;
    std::string name = ObjectName(attrs, &root);
    Target t = Resolve(name, root);
    if (t.read_only && (options & 0x1001) == 0 && OpenRadioOverride(name, handle_out, iosb)) return kSuccess;
    if (t.disc) return OpenOnDisc(t.entry, name, handle_out, iosb, 1, options);
    const fs::path& host = t.host;
    std::error_code ec;
    if (host.empty() || !fs::exists(host, ec)) {
        LOG_FIRST(200, "missing %s", name.c_str());
        const uint32_t status = host.empty() ? kObjectPathNotFound : kObjectNameNotFound;
        IoStatus(iosb, status, host.empty() ? 0 : 5);
        return status;
    }
    auto f = std::make_shared<File>();
    f->host = host;
    f->xbox = Normalize(name);
    f->dir = fs::is_directory(host, ec);
    f->read_only = t.read_only;
    f->options = options;
    if (!f->dir) {
        const bool read_request = access && (access & 0x40000000u) == 0 && (access & 0x2u) == 0;
        bool write = !t.read_only && !read_request;
        f->fp = OpenHostFile(host, write ? "r+b" : "rb");
        if (!f->fp && write) {
            f->fp = OpenHostFile(host, "rb");
            write = false;
        }
        f->writable = write && f->fp != nullptr;
        if (!f->fp) return kAccessDenied;
    }
#if TCNYC_DEV_TOOLS
    TraceWmaOpen(*f);
#endif
    RegisterFile(f);
    TraceOpen(*f, options);
    (void)options;
    W32(handle_out, NewHandle(f));
    IoStatus(iosb, kSuccess, 1);
    return kSuccess;
}


// I/O completes before the call returns: the status block is written, then the file object, the event and the
// APC are signalled (GetOverlappedResult without an event waits on the file handle)
constexpr uint32_t kIoDeviceError = 0xC0000185;
static uint32_t Finish(File& f, uint32_t status, uint32_t n, uint32_t event, uint32_t apc, uint32_t apc_ctx, uint32_t iosb) {
    IoStatus(iosb, status, n);
    SetEvent(f);
    Complete(event, apc, apc_ctx, iosb);
    return status;
}

static uint64_t Offset(File& f, uint32_t off) {
    int64_t o = off ? int64_t(R64(off)) : -1;
    return o >= 0 ? uint64_t(o) : f.pos;
}

// pages is the FILE_SEGMENT_ELEMENT array for scatter and gather, 0 for one buffer
static uint32_t Transfer(const Args& a, bool write, bool scatter) {
    uint32_t h = a[0], event = a[1], apc = a[2], apc_ctx = a[3], iosb = a[4], buf = a[5], len = a[6], off = a[7];
    auto f = FileFromHandle(h);
    if (!f || (!f->fp && !f->entry)) return kInvalidHandle;
    if (write && (f->entry || f->read_only)) return kAccessDenied;
    const bool io_trace = IoTrace();
    const auto io_start = io_trace ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
#if TCNYC_DEV_TOOLS
    const bool trace_read = f->trace_wma && !write;
    const auto read_start = trace_read ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
#endif
    if (iosb && !GuestWritable(iosb, 8)) return kAccessViolation;
    std::lock_guard<std::mutex> lock(f->io);
    ResetEvent(*f);
    const auto finish = [&](uint32_t status, uint32_t count = 0) {
        return Finish(*f, status, count, event, apc, apc_ctx, iosb);
    };
    if (off && !GuestWritable(off, 8)) return finish(kAccessViolation);
    std::vector<uint32_t> segments;
    if (scatter && len) {
        const uint32_t count = (len - 1) / mem::kPage + 1;
        if (!GuestWritable(buf, count * 8)) return finish(kAccessViolation);
        segments.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            // The union is eight bytes wide, but the 32-bit Xbox pointer occupies the low word.
            const uint32_t at = R32(buf + i * 8);
            if (!GuestWritable(at, std::min(len - i * mem::kPage, mem::kPage))) return finish(kAccessViolation);
            segments.push_back(at);
        }
    } else if (!GuestWritable(buf, len)) {
        return finish(kAccessViolation);
    }
    const uint64_t pos = Offset(*f, off);
    if (pos > uint64_t(INT64_MAX) - len) return finish(kInvalidParameter);
    uint32_t n = 0;
    if (write) {
        if (f->fp) {
            std::clearerr(f->fp);
            if (!Seek(f->fp, pos)) return finish(kIoDeviceError);
        }
        while (n < len) {
            const uint32_t at = scatter ? segments[n / mem::kPage] : buf;
            const uint32_t chunk = scatter ? std::min(len - n, mem::kPage) : len;
            const size_t done = f->entry ? MountedDisc()->Read(*f->entry, pos + n, HostPtr(at), chunk)
                                         : std::fwrite(HostPtr(at), 1, chunk, f->fp);
            n += uint32_t(done);
            if (done < chunk) break;
        }
        f->pos = pos + n;
        NoteIo(true, f->xbox, pos, n, event, apc, f->options, io_start);
        return finish(n < len ? (errno == ENOSPC ? kDiskFull : kIoDeviceError) : kSuccess, n);
    }
    if (f->fp) {
        std::clearerr(f->fp);
        if (!Seek(f->fp, pos)) return finish(kIoDeviceError);
    }
    while (n < len) {
        const uint32_t at = scatter ? segments[n / mem::kPage] : buf;
        const uint32_t chunk = scatter ? std::min(len - n, mem::kPage) : len;
        const size_t done = f->entry ? MountedDisc()->Read(*f->entry, pos + n, HostPtr(at), chunk)
                                     : std::fread(HostPtr(at), 1, chunk, f->fp);
        n += uint32_t(done);
        if (done < chunk) break;
    }
    f->pos = pos + n;
    NoteIo(false, f->xbox, pos, n, event, apc, f->options, io_start);
#if TCNYC_DEV_TOOLS
    if (trace_read) {
        const auto elapsed = std::chrono::steady_clock::now() - read_start;
        if (elapsed >= std::chrono::milliseconds(20))
            LOGR("audio trace: %s read %u of %u bytes at %llu in %.1f ms", f->xbox.c_str(), n, len,
                 static_cast<unsigned long long>(pos), std::chrono::duration<double, std::milli>(elapsed).count());
    }
#endif
    if ((f->fp && std::ferror(f->fp)) ||
        (f->entry && pos < f->entry->size && n < std::min<uint64_t>(len, f->entry->size - pos)))
        return finish(kIoDeviceError, n);
    if (len && !n) return finish(kEndOfFile);
    return finish(kSuccess, n);
}

KERNEL_EXPORT(219, NtReadFile, 8) { return Transfer(a, false, false); }
KERNEL_EXPORT(236, NtWriteFile, 8) { return Transfer(a, true, false); }
// the buffer argument is an array of one page per element
KERNEL_EXPORT(220, NtReadFileScatter, 8) { return Transfer(a, false, true); }
KERNEL_EXPORT(237, NtWriteFileGather, 8) { return Transfer(a, true, true); }

KERNEL_EXPORT(211, NtQueryInformationFile, 5) {
    uint32_t h = a[0], iosb = a[1], out = a[2], len = a[3], cls = a[4];
    auto f = FileFromHandle(h);
    if (!f) return kInvalidHandle;
    std::lock_guard<std::mutex> io(f->io);
    std::error_code ec;
    uint64_t size = f->dir ? 0 : f->entry ? f->entry->size : 0;
    if (!f->dir && !f->entry) {
        if (f->fp && std::fflush(f->fp) != 0) return kDiskFull;
        const uintmax_t host_size = fs::file_size(f->host, ec);
        size = ec ? 0 : uint64_t(host_size);  // a vanished/locked file must not report UINT64_MAX
    }
    uint32_t need = cls == 4 ? 0x28 : cls == 5 ? 0x18 : cls == 14 ? 8 : cls == 34 ? 0x38 : 0;
    if (need && len < need) {
        IoStatus(iosb, kBufferTooSmall, need);
        return kBufferTooSmall;
    }
    if (len && !GuestWritable(out, len)) {
        IoStatus(iosb, kAccessViolation, 0);
        return kAccessViolation;
    }
    std::memset(HostPtr(out), 0, len);
    switch (cls) {
        case 4: {  // FileBasicInformation
            uint64_t t = 0;
            if (f->entry) {
                t = MountedDisc()->Time();
            } else {
                const auto written = fs::last_write_time(f->host, ec);
                if (!ec) t = ToFileTime(written);
            }
            for (int i = 0; i < 4; i++) W64(out + i * 8, t);
            W32(out + 0x20, f->dir ? 0x10 : 0x80);
            IoStatus(iosb, kSuccess, 0x28);
            return kSuccess;
        }
        case 5:  // FileStandardInformation
            W64(out, (size + 0xFFF) & ~0xFFFull);
            W64(out + 8, size);
            W32(out + 0x10, 1);  // NumberOfLinks
            G<uint8_t>(out + 0x15) = f->dir;  // Directory, after DeletePending
            IoStatus(iosb, kSuccess, 0x18);
            return kSuccess;
        case 14:  // FilePositionInformation
            W64(out, f->pos);
            IoStatus(iosb, kSuccess, 8);
            return kSuccess;
        case 34:  // FileNetworkOpenInformation
            if (f->entry) FillNetworkInfo(out, *f->entry);
            else FillNetworkInfo(out, f->host);
            IoStatus(iosb, kSuccess, 0x38);
            return kSuccess;
        case 22:  // FileModeInformation
        case 16:
            IoStatus(iosb, kSuccess, 4);
            return kSuccess;
        default:
            LOG_FIRST(8, "NtQueryInformationFile: class %u", cls);
            return kInvalidParameter;
    }
}

KERNEL_EXPORT(226, NtSetInformationFile, 5) {
    uint32_t h = a[0], iosb = a[1], in = a[2], cls = a[4];
    auto f = FileFromHandle(h);
    if (!f) return kInvalidHandle;
    std::lock_guard<std::mutex> io(f->io);
    if ((f->entry || f->read_only) && cls != 14 && cls != 4) return kAccessDenied;
    std::error_code ec;
    switch (cls) {
        case 14:  // position
            f->pos = R64(in);
            break;
        case 19:  // allocation: only a size below the end of file cuts it
        case 20:  // end of file
            if (f->fp) {
                // The function already holds f->io; taking it again here deadlocks
                // the first time a save truncates a file.
                if (std::fflush(f->fp) != 0) return kDiskFull;
                if (!GuestWritable(in, 8)) return kAccessViolation;
                if (!ResizeStream(f->fp, R64(in), cls == 19)) return kDiskFull;
                // Truncation is metadata; commit it before reporting success.
                if (!SyncStream(f->fp)) return kDiskFull;
            }
            break;
        case 13:  // disposition
            f->delete_on_close = G<uint8_t>(in) != 0;
            break;
        case 10: {  // rename: ReplaceIfExists, RootDirectory, FileName (ANSI_STRING inline)
            uint32_t root = R32(in + 4);
            std::string target = ReadAnsiString(in + 8);
            Target resolved = Resolve(target, root);
            if (resolved.read_only) return kAccessDenied;
            fs::path host = resolved.host;
            if (host.empty()) return kObjectPathNotFound;
            if (!PathUnchanged(f->host) || !PathUnchanged(host)) return kAccessDenied;
            // ReplaceIfExists: without it, an existing target is a collision.
            if (!G<uint8_t>(in) && fs::exists(host)) return kObjectNameCollision;
            if (f->fp && std::fflush(f->fp) != 0) return kDiskFull;
            fs::rename(f->host, host, ec);
            if (ec) return kAccessDenied;
            f->host = host;
            break;
        }
        case 4:  // basic, times and attributes are not kept
            break;
        default:
            LOG_FIRST(8, "NtSetInformationFile: class %u", cls);
            return kInvalidParameter;
    }
    IoStatus(iosb, kSuccess, 0);
    return kSuccess;
}

// fills a volume information class into out; returns the status and the byte count written
static uint32_t QueryVolume(uint32_t out, uint32_t len, uint32_t cls, uint32_t& bytes, const char* who) {
    bytes = 0;
    uint32_t need = 0;
    switch (cls) {
        case 3: need = 0x18; break;  // FileFsSizeInformation
        case 7: need = 0x20; break;  // FileFsFullSizeInformation
        case 1: need = 0x18; break;  // FileFsVolumeInformation
        case 5: need = 16; break;    // FileFsAttributeInformation
        default:
            LOG_FIRST(8, "%s: class %u", who, cls);
            return kInvalidParameter;
    }
    if (len < need) return kBufferTooSmall;
    if (!GuestWritable(out, need)) return kAccessViolation;
    std::memset(HostPtr(out), 0, need);
    switch (cls) {
        case 3:  // FileFsSizeInformation: 16 GiB total, 8 GiB free in 16 KiB units
            W64(out, 0x100000);
            W64(out + 8, 0x80000);
            W32(out + 0x10, 32);
            W32(out + 0x14, 512);
            bytes = 0x18;
            return kSuccess;
        case 7:  // FileFsFullSizeInformation
            W64(out, 0x100000);
            W64(out + 8, 0x80000);
            W64(out + 0x10, 0x80000);
            W32(out + 0x18, 32);
            W32(out + 0x1C, 512);
            bytes = 0x20;
            return kSuccess;
        case 1:  // FileFsVolumeInformation
            W32(out + 8, 0x12345678);  // VolumeSerialNumber
            bytes = 0x18;
            return kSuccess;
        case 5:  // FileFsAttributeInformation: attributes, max name, name length, "FATX"
            W32(out, 0);
            W32(out + 4, 42);
            W32(out + 8, 4);
            std::memcpy(HostPtr(out + 12), "FATX", 4);
            bytes = 16;
            return kSuccess;
        default:
            LOG_FIRST(8, "%s: class %u", who, cls);
            return kInvalidParameter;
    }
}

KERNEL_EXPORT(218, NtQueryVolumeInformationFile, 5) {
    uint32_t iosb = a[1], out = a[2], len = a[3], cls = a[4], bytes;
    uint32_t status = QueryVolume(out, len, cls, bytes, "NtQueryVolumeInformationFile");
    if (status == kSuccess) IoStatus(iosb, kSuccess, bytes);
    return status;
}

// (FileObject, FsInformationClass, Length, FsInformation, ReturnedLength): the same classes for a referenced file
// object; every host volume answers alike. True Crime NYC asks for class 5 (XAPI 0x26381D).
KERNEL_EXPORT(76, IoQueryVolumeInformation, 5) {
    uint32_t cls = a[1], len = a[2], out = a[3], ret = a[4], bytes;
    uint32_t status = QueryVolume(out, len, cls, bytes, "IoQueryVolumeInformation");
    if (ret) W32(ret, bytes);
    return status;
}

KERNEL_EXPORT(207, NtQueryDirectoryFile, 10) {
    uint32_t h = a[0], event = a[1], apc = a[2], apc_ctx = a[3], iosb = a[4], out = a[5], len = a[6], cls = a[7],
             mask = a[8], restart = a[9];
    auto f = FileFromHandle(h);
    if (!f || !f->dir) return kInvalidHandle;
    std::lock_guard<std::mutex> io(f->io);
    if (cls != 1) {
        LOG_FIRST(8, "NtQueryDirectoryFile: class %u", cls);
        return kInvalidParameter;
    }
    std::string pat = mask ? ReadAnsiString(mask) : "*";
    if (!f->listed || (restart & 0xFF)) {
        f->entries.clear();
        f->disc_entries.clear();
        if (f->entry) {
            for (const Disc::Entry& e : f->entry->children)
                if (WildMatch(pat, e.name)) f->disc_entries.push_back(&e);
        } else {
            std::error_code ec;
            for (auto& e : fs::directory_iterator(f->host, ec))
                if (WildMatch(pat, e.path().filename().string())) f->entries.push_back(e);
            std::sort(f->entries.begin(), f->entries.end());
        }
        f->next = 0;
        f->listed = true;
    }
    if (f->next >= (f->entry ? f->disc_entries.size() : f->entries.size())) {
        IoStatus(iosb, kNoMoreFiles, 0);
        return f->next == 0 ? kNoSuchFile : kNoMoreFiles;
    }
    size_t i = f->next;
    std::string fname = f->entry ? f->disc_entries[i]->name : f->entries[i].path().filename().string();
    uint32_t need = 0x40 + uint32_t(fname.size());
    if (len < need) {
        IoStatus(iosb, kBufferTooSmall, 0);
        return kBufferTooSmall;  // keep the entry for the retry with a larger buffer
    }
    if (!GuestWritable(out, need)) {
        IoStatus(iosb, kAccessViolation, 0);
        return kAccessViolation;
    }
    f->next++;
    std::memset(HostPtr(out), 0, need);
    // times, sizes and attributes sit at the same place
    if (f->entry) FillNetworkInfo(out + 8, *f->disc_entries[i]);
    else FillNetworkInfo(out + 8, f->entries[i].path());
    // FILE_DIRECTORY_INFORMATION has EndOfFile before AllocationSize, the reverse of the network info
    W32(out + 0x38, R32(out + 8 + 0x30));
    uint64_t alloc = R64(out + 8 + 0x20), eof = R64(out + 8 + 0x28);
    W64(out + 0x28, eof);
    W64(out + 0x30, alloc);
    W32(out + 0x3C, uint32_t(fname.size()));
    std::memcpy(HostPtr(out + 0x40), fname.data(), fname.size());
    IoStatus(iosb, kSuccess, need);
    Complete(event, apc, apc_ctx, iosb);
    return kSuccess;
}

KERNEL_EXPORT(210, NtQueryFullAttributesFile, 2) {
    std::string name = ReadAnsiString(R32(a[0] + 4));
    Target t = Resolve(name, R32(a[0]));
    std::error_code ec;
    if (t.disc ? !t.entry : t.host.empty() || !fs::exists(t.host, ec)) {
        LOG_FIRST(200, "missing %s", name.c_str());
        return kObjectNameNotFound;
    }
    if (!GuestWritable(a[1], 0x38)) return kAccessViolation;
    if (t.disc) FillNetworkInfo(a[1], *t.entry);
    else FillNetworkInfo(a[1], t.host);
    return kSuccess;
}

KERNEL_EXPORT(195, NtDeleteFile, 1) {
    Target target = Resolve(ReadAnsiString(R32(a[0] + 4)), R32(a[0]));
    if (target.read_only) return kAccessDenied;
    std::error_code ec;
    if (target.host.empty() || !fs::remove(target.host, ec)) return kObjectNameNotFound;
    return kSuccess;
}

KERNEL_EXPORT(198, NtFlushBuffersFile, 2) {
    uint32_t status = kSuccess;
    if (auto f = FileFromHandle(a[0])) {
        std::lock_guard<std::mutex> io(f->io);
        // A full or failing disk is visible only when the CRT buffer reaches the
        // host file system, so report it instead of acknowledging lost data.
        if (f->fp && f->writable && !SyncStream(f->fp)) status = kDiskFull;
    }
    IoStatus(a[1], status, 0);
    return status;
}

KERNEL_EXPORT(200, NtFsControlFile, 10) {
    IoStatus(a[4], kSuccess, 0);
    return kSuccess;
}

// IOCTL_SCSI_PASS_THROUGH_DIRECT: the reply goes to the data buffer the SCSI_PASS_THROUGH_DIRECT names
constexpr uint32_t kScsiPassThroughDirect = 0x4D014;
constexpr uint32_t kSptdTransferLength = 0xC;
constexpr uint32_t kSptdDataBuffer = 0x14;
constexpr uint32_t kSptdCdb = 0x1C;
constexpr uint8_t kModeSense10 = 0x5A;
constexpr uint8_t kXboxSecurityPage = 0x3E;
constexpr uint32_t kSecurityPageBytes = 13;  // mode header, page code and length, then the three bytes XAPI checks

// XAPI starts a title whose media must be an Xbox DVD only once the drive's security page reports an
// authenticated disc (page bytes 2 and 4 set, byte 3 one); the game files stand in for that disc
static void ScsiPassThrough(uint32_t sptd) {
    uint32_t cdb = sptd + kSptdCdb;
    if (G<uint8_t>(cdb) != kModeSense10 || (G<uint8_t>(cdb + 2) & 0x3F) != kXboxSecurityPage) return;
    uint32_t data = R32(sptd + kSptdDataBuffer), n = R32(sptd + kSptdTransferLength);
    if (!data || n < kSecurityPageBytes || n > 0xFF) return;
    std::memset(HostPtr(data), 0, n);
    G<uint8_t>(data + 1) = uint8_t(n - 2);  // mode data length, big endian
    G<uint8_t>(data + 8) = kXboxSecurityPage;
    G<uint8_t>(data + 9) = uint8_t(n - 10);
    G<uint8_t>(data + 10) = 1;
    G<uint8_t>(data + 11) = 1;
    G<uint8_t>(data + 12) = 1;
}

KERNEL_EXPORT(196, NtDeviceIoControlFile, 10) {
    uint32_t iosb = a[4], code = a[5], in = a[6], out = a[8], out_len = a[9];
    LOG_FIRST(16, "NtDeviceIoControlFile: code %08X", code);
    if (out && out_len) std::memset(HostPtr(out), 0, out_len);
    if (code == 0x70000 && out_len >= 0x18) {  // IOCTL_DISK_GET_DRIVE_GEOMETRY
        W64(out, 1000);
        W32(out + 8, 12);
        W32(out + 12, 255);
        W32(out + 16, 63);
        W32(out + 20, 512);
    }
    if (code == kScsiPassThroughDirect && in) ScsiPassThrough(in);
    IoStatus(iosb, kSuccess, out_len);
    return kSuccess;
}

KERNEL_EXPORT(67, IoCreateSymbolicLink, 2) {
    CreateSymbolicLink(ReadAnsiString(a[0]), ReadAnsiString(a[1]));
    return kSuccess;
}

KERNEL_EXPORT(69, IoDeleteSymbolicLink, 1) {
    return DeleteSymbolicLink(ReadAnsiString(a[0])) ? kSuccess : kObjectNameNotFound;
}

KERNEL_EXPORT(203, NtOpenSymbolicLinkObject, 2) {
    std::string name = ObjectName(a[1]);
    std::string target = QuerySymbolicLink(name);
    if (target.empty()) return kObjectNameNotFound;
    auto l = std::make_shared<Link>();
    l->target = target;
    W32(a[0], NewHandle(l));
    return kSuccess;
}

KERNEL_EXPORT(215, NtQuerySymbolicLinkObject, 3) {
    ObjectRef o = FromHandle(a[0]);
    if (!o || o->type != ObjType::SymLink) return kInvalidHandle;
    auto& t = static_cast<Link&>(*o).target;
    uint32_t str = a[1];
    uint16_t maxlen = R16(str + 2);
    if (a[2]) W32(a[2], uint32_t(t.size()));
    if (t.size() > maxlen) return kBufferTooSmall;
    std::memcpy(HostPtr(R32(str + 4)), t.data(), t.size());
    W16(str, uint16_t(t.size()));
    return kSuccess;
}

}  // namespace kernel
