#include <algorithm>
#include <atomic>
#include <cstring>
#include <cstdlib>
#include <fstream>

#include "app/app.h"
#include "kernel/kernel.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif
#include "xbe/xbe.h"

namespace dispatch {
void Init();
uint32_t EntryPoint();
uint32_t KernelThunkAddress(uint32_t ordinal);
}  // namespace dispatch

namespace xnet {
void CloseSockets();  // xdk/xnet.cpp
}

namespace kernel {

uint8_t* g_base = nullptr;
std::filesystem::path g_launch_file;

namespace {
uint32_t g_saved_data = 0;
bool g_mark = false, g_kept_marked = false;
std::string g_picture, g_kept_picture;  // the last picture the title showed, raw path string with its terminator
constexpr uint32_t kPersistMagic = 0x50535044;  // "DPSP"

std::filesystem::path PersistFile() { return g_launch_file.parent_path() / "display.bin"; }

// surface header (Common, Data, Lock, Format, Size) then the pixels at +0x40, as the XDK lays out its persisted copy
void LoadPersistedDisplay() {
    std::ifstream f(PersistFile(), std::ios::binary);
    uint32_t head[7] = {};
    if (!f.read(reinterpret_cast<char*>(head), sizeof head) || head[0] != kPersistMagic || !head[4] || head[4] > 0x800000 ||
        head[6] > 1040)
        return;
    uint32_t block = mem::AllocContiguous(0x40 + head[4], 0, 0xFFFFFFFF, 0x1000);
    if (!block || !f.read(static_cast<char*>(HostPtr(block + 0x40)), head[4])) return;
    g_kept_picture.resize(head[6]);
    if (!f.read(g_kept_picture.data(), head[6])) g_kept_picture.clear();
    W32(block, (head[1] & 0xFFFF0000) | 1);
    W32(block + 4, mem::PhysicalFromVirtual(block + 0x40));
    W32(block + 8, 0);
    W32(block + 0xC, head[2]);
    W32(block + 0x10, head[3]);
    g_saved_data = block;
    g_kept_marked = head[5] != 0;
    LOGI("boot: display kept from the previous run, %u bytes%s", head[4], g_kept_marked ? ", marked" : "");
}
}  // namespace

void SavePersistedDisplay(uint32_t common, uint32_t format, uint32_t size, const void* pixels, uint32_t bytes) {
    if (FILE* f = OpenHostFile(PersistFile(), "wb")) {
        uint32_t head[7] = {kPersistMagic, common, format, size, bytes, g_mark, uint32_t(g_picture.size())};
        std::fwrite(head, sizeof head, 1, f);
        std::fwrite(pixels, 1, bytes, f);
        std::fwrite(g_picture.data(), 1, g_picture.size(), f);
        std::fclose(f);
    }
}

void MarkPersistedDisplay() { g_mark = true; }
bool KeptDisplayMarked() { return g_kept_marked; }
void SetShownPicture(std::string path) { g_picture = std::move(path); }
const std::string& KeptPicture() { return g_kept_picture; }

uint32_t SavedDataAddress() { return g_saved_data; }
void SetSavedDataAddress(uint32_t surface) { g_saved_data = surface; }
const xbe::Image* g_image = nullptr;
static xbe::Image g_xbe;

bool Boot(std::vector<uint8_t> xbe, const std::string& xbe_name, const std::filesystem::path& game_dir,
          const std::filesystem::path& data_dir) {
    if (!mem::Init()) return false;
    g_base = mem::Base();

    std::string err;
    if (!g_xbe.Parse(std::move(xbe), err)) {
        LOGE("boot: %s: %s", xbe_name.c_str(), err.c_str());
        return false;
    }
    g_image = &g_xbe;
    const xbe::Image& img = g_xbe;
    if (img.entry != dispatch::EntryPoint()) {
        LOGE("boot: %s entry %08X does not match the recompiled entry %08X, wrong executable", xbe_name.c_str(), img.entry,
             dispatch::EntryPoint());
        return false;
    }
    LOGI("boot: title %08X, entry %08X, %zu sections", img.title_id, img.entry, img.sections.size());
    for (auto& l : img.libraries) LOGI("boot: library %-8s %u.%u.%u", l.name.c_str(), l.major, l.minor, l.build);

    if (!img.image_size || img.header_size > img.file.size() || img.header_size > img.image_size ||
        uint64_t(img.base) + img.image_size > 0x100000000ull) {
        LOGE("boot: %s: image spans are invalid", xbe_name.c_str());
        return false;
    }
    uint64_t tls = 0;
    if (img.tls_addr) tls = ((uint64_t(img.tls.data_end - img.tls.data_start) + img.tls.zero_fill + 15) & ~15ull) + 4;
    const uint32_t stack = std::max<uint32_t>(img.stack_commit, 0x10000);
    if (stack + tls + 0x1000 > mem::kStackSize - 0x10000) {
        LOGE("boot: guest stack and TLS are too large");
        return false;
    }
    if (!mem::ReserveImage(img.base, img.image_size)) {
        LOGE("boot: %s: could not reserve the guest image", xbe_name.c_str());
        return false;
    }
    std::memcpy(g_base + img.base, img.file.data(), std::min<size_t>(img.header_size, img.file.size()));
    // XBE header 0x120 points at the section headers, 56 bytes each, +0x18 is the reference count that marks one loaded
    uint32_t sec_hdr = img.U32(0x120);
    const uint64_t image_end = uint64_t(img.base) + img.image_size;
    if (!img.sections.empty() &&
        (sec_hdr < img.base || uint64_t(sec_hdr) - img.base + img.sections.size() * 56 > img.header_size)) {
        LOGE("boot: %s: section header table is outside the header", xbe_name.c_str());
        return false;
    }
    for (size_t i = 0; i < img.sections.size(); i++) {
        auto& s = img.sections[i];
        if (!(s.flags & xbe::kPreload)) continue;
        if (s.va < img.base || uint64_t(s.raw) + s.rsize > img.file.size() ||
            uint64_t(s.va) + s.rsize > image_end || uint64_t(s.va) + s.vsize > image_end) {
            LOGE("boot: %s: section %zu lies outside the image", xbe_name.c_str(), i);
            return false;
        }
        std::memcpy(g_base + s.va, img.file.data() + s.raw, std::min(s.rsize, s.vsize));
        W32(sec_hdr + uint32_t(i) * 56 + 0x18, 1);
    }
    if (app::kGame.loaded) app::kGame.loaded();

    InitKernelData();
    dispatch::Init();

    uint32_t missing = 0;
    bool terminated = false;
    for (uint32_t slot = img.kernel_thunk; uint64_t(slot) + 4 <= image_end; slot += 4) {
        uint32_t v = R32(slot);
        if (!v) {
            terminated = true;
            break;
        }
        uint32_t ord = v & 0x7FFFFFFF;  // high bit flags an import by ordinal
        uint32_t data = DataAddress(ord);
        W32(slot, data ? data : dispatch::KernelThunkAddress(ord));
        if (!HasExport(ord)) {
            LOGW("boot: kernel import %u (%s) has no implementation", ord, ExportName(ord));
            missing++;
        }
    }
    if (!terminated) {
        LOGE("boot: unterminated kernel import table");
        return false;
    }
    LOGI("boot: kernel imports resolved, %u missing", missing);

    MountDefaults(game_dir, data_dir);

    // a quick reboot into this title left its launch data page behind
    g_launch_file = data_dir / "launch.bin";
    std::ifstream lf(g_launch_file, std::ios::binary);
    if (lf) {
        uint32_t page = mem::AllocContiguous(0x1000, 0, 0xFFFFFFFF, 0x1000);
        lf.read(static_cast<char*>(HostPtr(page)), 0x1000);
        lf.close();
        std::error_code ec;
        std::filesystem::remove(g_launch_file, ec);
        W32(g_launch_data_page_va, page);
        LOGI("boot: launch data from the previous run, type %u", R32(page));
        LoadPersistedDisplay();
    }
    std::error_code ec;
    std::filesystem::remove(PersistFile(), ec);

    ThreadRef t = CreateThread(stack, uint32_t(tls), 0, img.entry, 0, false);
    if (!t || !StartThread(t)) {
        LOGE("boot: could not start the entry thread");
        return false;
    }
    return true;
}

const char* ImageExe(std::string xbe_path) {
    for (char& c : xbe_path) c = char(std::tolower(uint8_t(c)));
    // Xbox paths ignore case; a row keeps the xbe's own name, which the code DLL and config lookups compare exactly
    for (auto& [xbe, exe] : app::kGame.images) {
        std::string name = xbe;
        for (char& c : name) c = char(std::tolower(uint8_t(c)));
        size_t n = name.size();
        if (xbe_path.size() >= n && xbe_path.compare(xbe_path.size() - n, n, name) == 0 &&
            (xbe_path.size() == n || xbe_path[xbe_path.size() - n - 1] == '\\' || xbe_path[xbe_path.size() - n - 1] == ';'))
            return exe;
    }
    return nullptr;
}

std::wstring HandoverEvent(unsigned long process) {
    return L"Local\\" + std::wstring(app::kGame.name) + L"Handover-" + std::to_wstring(process);
}

void Relaunch(const char* exe) {
    FlushOpenFiles();
    logx::Flush();
#ifdef _WIN32
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring path = self;
    path = path.substr(0, path.find_last_of(L'\\') + 1) + std::wstring(exe, exe + std::strlen(exe)) + L".exe";
    // the command line keeps its arguments (--instance) behind the new executable
    std::wstring line = GetCommandLineW();
    size_t args = line[0] == L'"' ? line.find(L'"', 1) + 1 : line.find(L' ');
    line = L"\"" + path + L"\"" + (args == std::wstring::npos ? L"" : line.substr(args));
    if (line.find(L"--relaunched") == std::wstring::npos) line += L" --relaunched";  // no boot splash again
    // this process lives on through the handover: the new one binds the same ports
    xnet::CloseSockets();
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(path.c_str(), line.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) {
        // this window stays up until the new instance shows its first frames (HandoverEvent), so no gap shows
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, HandoverEvent(pi.dwProcessId).c_str());
        AllowSetForegroundWindow(pi.dwProcessId);
        g_relaunching = true;
        ResumeThread(pi.hThread);
        HANDLE wait[] = {ready, pi.hProcess};
        if (ready) WaitForMultipleObjects(2, wait, FALSE, 15000);
        if (ready) ::CloseHandle(ready);
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
    } else {
        LOGE("relaunch: cannot start %s.exe (%lu)", exe, GetLastError());
    }
    std::_Exit(0);
#else
    char self[4096] = {};
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    std::string path = std::string(self, n > 0 ? size_t(n) : 0);
    path = path.substr(0, path.find_last_of('/') + 1) + exe;
    // Preserve --assets/--iso/--instance: the recompiled image switch must not
    // lose the game location, or a relaunch from an explicit path cannot boot.
    std::ifstream cmdline("/proc/self/cmdline", std::ios::binary);
    std::string raw((std::istreambuf_iterator<char>(cmdline)), {});
    std::vector<std::string> args;
    for (size_t at = 0; at < raw.size();) {
        const size_t end = raw.find('\0', at);
        if (end == std::string::npos) break;
        if (end > at) args.emplace_back(raw.substr(at, end - at));
        at = end + 1;
    }
    if (args.empty()) args.emplace_back(path);
    args[0] = path;
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);
    execv(path.c_str(), argv.data());
    std::_Exit(0);
#endif
}

namespace {
std::atomic<void (*)()> g_exit_save{nullptr};
}

void SetExitSave(void (*save)()) { g_exit_save = save; }

void Shutdown(int code) {
    FlushOpenFiles();
    if (auto save = g_exit_save.exchange(nullptr)) save();
    LOGR("Exit (%d)", code);
    logx::Flush();
    std::_Exit(code);
}

}  // namespace kernel
