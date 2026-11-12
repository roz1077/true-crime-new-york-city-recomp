// Graphics rows in the native Options > Display tab.
//
// The tab's rows come from a MenuDuplicator in PauseMenu.tag (0x42A44) that
// lays out as many rows as the MenuPauseInterface getter (0xC7B80, thiscall
// get(uint32_t* out, uint32_t port)) reports, and asks it for each row's text
// and colour by port hash, base + row. On Xbox the game reports one row,
// SUBTITLES. This reports rows after it, answers their ports, moves the cursor
// over them in the event dispatcher (0xCB400), whose row counts are fixed per
// tab, and follows the tab's cancel (Back restores what the tab had on entry).
// The tab's frame holds five rows, so with more the slots show a window of the
// rows that scrolls with the cursor. The game's row field (+0x34) keeps the
// slot, 0 to 4, which its highlight frame follows; the logical row is
// g_first + slot.
// Its HDR SETTINGS row swaps the rows for an HDR page (g_page).
// The rules are in display_rows.h.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "app/app.h"
#include "common/graphics_menu.h"
#include "core/log.h"
#include "display_rows.h"
#include "gpu/backend.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

namespace app {
// the window thread applies a display change and reports whether it worked (main.cpp; the SDL
// hosts answer at once, graphics_options.cpp)
uint64_t RequestDisplayApply(DisplayMode mode, uint32_t width, uint32_t height);
bool DisplayApplyResult(uint64_t request, bool &ok);
} // namespace app

namespace {

using namespace game::display_rows;

// MenuPauseInterface ports of the Display tab
constexpr uint32_t kDisplayCount = 0x08015D89;  // number of rows
constexpr uint32_t kDisplayText = 0xDBCC9F60;   // + row: the row's text
constexpr uint32_t kDisplayColour = 0xA471F488; // + row: the row's text colour
constexpr uint32_t kOptionsPage = 0x6ADBF5A0;   // OptionsPageController: tab, 0 to 3
constexpr uint32_t kTopTitle = 0xE4BCA648;     // MainStaticTitleRender text

// MenuPauseInterface fields
constexpr uint32_t kTopTab = 0x2C; // Main through Options, 0 to 4
constexpr uint32_t kPage = 0x30;  // 0x0F to 0x12: the Options tabs
constexpr uint32_t kRow = 0x34;   // selected row
constexpr uint32_t kFocus = 0x7C; // kTabBarFocus or kRowListFocus
constexpr uint32_t kDisplayPage = 0x11;
constexpr uint32_t kMainTab = 0, kOptionsTab = 4, kTopBarFocus = 0;
constexpr uint32_t kColourSelected = 0x80808080, kColourNormal = 0x60606060; // as the game's rows
constexpr uint32_t kInputEvent = 9;

bool Tracing() {
    static const bool on = std::getenv("XBE_TRACE_OPTIONS_PAGE") != nullptr;
    return on;
}

// the display mode and resolution rows' choices before A applies them, and the sizes they step through;
// menu thread only
uint64_t g_pending_size = 0;
uint32_t g_pending_mode = 0;
bool g_pending = false;
std::vector<uint64_t> g_sizes;

// Apply changes the live settings at once, but writing and flushing the file
// on every arrow step stalled the tab. Save after the row changes settle, when
// navigation/exit arrives, or when the tab is entered again.
constexpr uint64_t kSaveIdleMs = 250;
std::atomic<bool> g_save_dirty{false};
std::atomic<uint64_t> g_save_due{0};

// A display mode/resolution change the window thread has not answered yet: the requested values
// are in the live settings (the window thread applies from them) but are written to the file only
// once it confirms them. A refused change is put back. Menu thread only.
uint64_t g_display_request = 0;
// A cancel while an apply is still in flight waits for that answer, then asks for the mode the
// tab was entered with. Keeping this separate stops the superseded request's result being saved.
bool g_restore_pending = false;
uint32_t g_restore_mode = 0;
uint64_t g_restore_size = 0;

uint64_t NowMs() {
    return uint64_t(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

void MarkSave() {
    g_save_due.store(NowMs() + kSaveIdleMs, std::memory_order_relaxed);
    g_save_dirty.store(true, std::memory_order_release);
}

void FlushSettings() {
    if (g_display_request) return;  // saved once the display change is answered
    if (!g_save_dirty.exchange(false, std::memory_order_acq_rel)) return;
    app::Save();
}

void PostRestoreDisplayChoice() {
    const uint32_t w = uint32_t(g_restore_size >> 32), h = uint32_t(g_restore_size);
    const uint64_t request = app::RequestDisplayApply(app::DisplayMode(g_restore_mode), w, h);
    if (!request) {
        g_restore_pending = false;
        LOGE("Display tab: could not request the entry display mode %u, %ux%u back", g_restore_mode, w, h);
        return;
    }
    g_display_request = request;
}

// Exit Game: save the settings, then ask the host to close. While a display change is still waiting for
// the window thread's answer, the exit waits for it (PollDisplayApply) so the answer is saved too.
bool g_quit_pending = false;
void RequestGameExit(const char* where) {
    if (g_display_request || g_restore_pending) {
        if (!g_quit_pending) LOGR("%s: exit waits for the display change in progress", where);
        g_quit_pending = true;
        return;
    }
    g_quit_pending = false;
    FlushSettings();
    if (!app::RequestQuit()) LOGE("%s: could not request game exit", where);
}

// the window thread's answer to the display change in flight: keep and save it, or put the
// previous mode back
void PollDisplayApply() {
    if (g_quit_pending && !g_display_request && !g_restore_pending) RequestGameExit("Display tab");
    if (!g_display_request) {
        if (g_restore_pending) {
            g_restore_pending = false;
            PostRestoreDisplayChoice();
        }
        return;
    }
    bool ok = false;
    if (!app::DisplayApplyResult(g_display_request, ok)) return;
    g_display_request = 0;
    // a refused change never reached the settings, and the window thread put the previous mode back
    if (!ok) LOGE("Display tab: the display change was refused; kept the previous mode");
    if (g_restore_pending) {
        // The cancel was requested while this apply was in flight. Wait for its own answer, then
        // put the entry mode back; do not save the superseded request's mode.
        g_restore_pending = false;
        PostRestoreDisplayChoice();
        if (g_display_request) return;
    }
    g_save_dirty.store(true, std::memory_order_release);
    FlushSettings();
    if (g_quit_pending) RequestGameExit("Display tab");
}

void MaybeSave() {
    if (g_save_dirty.load(std::memory_order_acquire) && NowMs() >= g_save_due.load(std::memory_order_relaxed))
        FlushSettings();
}

bool OnDisplayRows(uint8_t* base, uint32_t self) {
    return X86_MEM32(self + kPage) == kDisplayPage && X86_MEM32(self + kFocus) == kRowListFocus;
}

static_assert(kFsrNo4 == uint32_t(gpu::FsrState::No4) && kFsrNoDll == uint32_t(gpu::FsrState::NoDll) &&
                  kDlssUnavailable == uint32_t(gpu::DlssState::Unavailable) &&
                  kDlssNotBuilt == uint32_t(gpu::DlssState::NotBuilt),
              "display_rows.h mirrors the upscaler states");

Values Current() {
    app::SettingsLock lock;
    const app::Settings &s = app::Get();
    const uint64_t applied = Size(s.width, s.height);
    const uint32_t applied_mode = uint32_t(s.display_mode);
    Values v{s.render_percent, game::g_fov_percent,
             s.aniso,          app::FrameCap(),
             s.aspect,         app::MsaaSamples(),
             s.vsync,          s.smaa,
             game::ShowFps(),  g_pending ? g_pending_size : applied,
             applied};
    v.mode = g_pending ? g_pending_mode : applied_mode;
    v.applied_mode = applied_mode;
    v.object_detail = s.object_detail;
    v.ao = s.ao;
    v.bloom = s.bloom;
    v.dof = s.dof;
    v.building_seam_fix = s.building_seam_fix;
    v.pc_contrast = s.pc_contrast;
    v.hdr = s.hdr;
    v.hdr_paper_white = s.hdr_paper_white;
    v.hdr_peak = s.hdr_peak;
    v.hdr_highlights = s.hdr_highlights;
    v.upscaler = s.upscaler;
    v.bloom_strength = s.bloom_strength;
    v.shadow_resolution = s.shadow_resolution;
    v.shadow_softness = s.shadow_softness;
    v.sharpening = s.sharpening;
    v.low_latency = s.low_latency;
    v.taa = s.taa;
    v.fsr_state = uint32_t(gpu::g_fsr_state.load(std::memory_order_relaxed));
    v.dlss_state = uint32_t(gpu::g_dlss_state.load(std::memory_order_relaxed));
    v.display_peak = gpu::g_hdr_display_peak_nits.load(std::memory_order_relaxed);
    return v;
}

// guest thread; the renderer reads these each frame and app::Pace each Swap (aligned 32-bit stores). The
// resolution only changes through ApplySize.
void Apply(const Values &v) {
    app::SettingsLock lock;
    app::Settings &s = app::Get();
    s.render_percent = v.render_percent;
    game::g_fov_percent = v.fov_percent;
    s.aniso = v.aniso;
    app::SetFrameCap(v.fps_cap);
    s.aspect = v.aspect;
    s.vsync = v.vsync;
    app::SetMsaaSamples(v.msaa);
    s.smaa = v.smaa;
    std::atomic_ref(s.object_detail).store(v.object_detail, std::memory_order_relaxed);  // read by lod.cpp
    s.ao = v.ao;
    s.bloom = v.bloom;
    s.dof = v.dof;
    s.building_seam_fix = v.building_seam_fix;
    s.pc_contrast = v.pc_contrast;
    s.hdr = v.hdr;
    s.hdr_paper_white = v.hdr_paper_white;
    s.hdr_peak = v.hdr_peak;
    s.hdr_highlights = v.hdr_highlights;
    s.upscaler = v.upscaler;
    s.bloom_strength = v.bloom_strength;
    s.shadow_resolution = v.shadow_resolution;
    s.shadow_softness = v.shadow_softness;
    s.sharpening = v.sharpening;
    s.low_latency = v.low_latency;
    s.taa = v.taa;
    game::SetShowFps(v.show_fps);
    MarkSave();
}

// sets the display mode and output resolution and has the window thread apply them, as the overlay's Apply does:
// the file is written once the window thread confirms the change (PollDisplayApply)
void ApplyDisplayChoice(uint32_t mode, uint64_t size) {
    PollDisplayApply();
    if (g_display_request) return;  // one change at a time; the choice stays pending for another A
    const uint32_t w = uint32_t(size >> 32), h = uint32_t(size);
    {
        app::SettingsLock lock;
        const app::Settings &s = app::Get();
        if (s.width == w && s.height == h && uint32_t(s.display_mode) == mode) {
            g_pending = false;
            return;
        }
    }
    // the window thread writes the settings only if the change works
    const uint64_t request = app::RequestDisplayApply(app::DisplayMode(mode), w, h);
    if (!request) {
        LOGE("Display tab: could not request a display change to mode %u, %ux%u", mode, w, h);
        return;  // the choice stays pending, so another A retries it
    }
    g_pending = false;
    g_display_request = request;
}

// Cancel: put back the mode the tab was entered with. If a change is still in flight, remember the
// restore target and post it after that request's own answer arrives.
void RestoreDisplayChoice(uint32_t mode, uint64_t size) {
    g_pending = false;
    const uint32_t w = uint32_t(size >> 32), h = uint32_t(size);
    if (!g_display_request) {
        app::SettingsLock lock;
        const app::Settings &s = app::Get();
        if (s.width == w && s.height == h && uint32_t(s.display_mode) == mode) return;
    }
    // Set the restore target before polling: if the in-flight request has just finished, Poll must
    // post the restore instead of saving the superseded request's mode.
    g_restore_mode = mode;
    g_restore_size = size;
    g_restore_pending = true;
    PollDisplayApply();
}

// guest buffers for the row texts, one per option; 0 if they could not be allocated
uint32_t TextBuffer(uint32_t index) {
    static const uint32_t buffers = mem::AllocSystem(kOptionCount * kTextBytes, 16);
    return buffers ? buffers + index * uint32_t(kTextBytes) : 0;
}

// what the rows had when the Display tab was entered, for cancel; menu thread only
Values g_on_entry{};
bool g_entered = false;
// the logical row shown in the top slot (display_rows.h Scroll); menu thread only. Reset
// where the game resets the row: entering the tab and cancel.
uint32_t g_first = 0;
Page g_page = Page::Main;

// The guest indexes a five-element title table with the tab field. Leave the
// real tab on Options to draw the confirmation panel and override its title.
struct ExitSelection {
    uint32_t owner = 0, tab = 0, page = 0, row = 0;
};
ExitSelection g_exit;

// MenuShellInterface owns the New Game / Load Game / Options list after Start.
// The tag already has a fourth row and highlight position for its demo item.
constexpr uint32_t kShellRow = 0x68, kShellState = 0x6C, kShellDemo = 0x70;
constexpr uint32_t kShellChoiceState = 1, kShellExitRow = 3;
constexpr uint32_t kShellCount = 0xFC9CB9A7, kShellText = 0x33384F13, kShellColour = 0xC5B0BD5B;
constexpr uint32_t kShellInputReady = 0x003EE31A, kShellDialogOpen = 0x00373554;
constexpr uint32_t kShellAlternateBack = 0x800;
uint32_t g_shell_exit_owner = 0;

} // namespace

X86_EXTERN_FUNC(__imp__sub_000AD5C0);
X86_FUNC(sub_000AD5C0) {
    const uint32_t self = ctx.ecx, out = X86_MEM32(ctx.esp + 4), port = X86_MEM32(ctx.esp + 8);
    const bool confirming = self && g_shell_exit_owner == self;
    if (confirming) PollDisplayApply();  // a pending Exit Game waits for a display change's answer
    if (out && self && !X86_MEM8(self + kShellDemo)) {
        if (port == kShellCount) {
            X86_MEM32(out) = confirming ? RowCount(Page::Exit) : kShellExitRow + 1;
            ::kernel::Return(ctx, 0, 2);
            return;
        }
        if (confirming && port >= kShellColour && port < kShellColour + RowCount(Page::Exit)) {
            const bool selected = port - kShellColour == X86_MEM32(self + kShellRow);
            X86_MEM32(out) = selected ? kColourSelected : kColourNormal;
            ::kernel::Return(ctx, 0, 2);
            return;
        }
        if ((confirming && port >= kShellText && port < kShellText + RowCount(Page::Exit)) ||
            (!confirming && port == kShellText + kShellExitRow)) {
            const Option option = confirming ? OptionAt(Page::Exit, port - kShellText) : Option::ExitConfirm;
            if (const uint32_t buffer = TextBuffer(uint32_t(option))) {
                char label[kTextBytes];
                Text({}, option, label);
                std::memcpy(base + buffer, label, std::strlen(label) + 1);
                X86_MEM32(out) = buffer;
                ::kernel::Return(ctx, 0, 2);
                return;
            }
        }
    }
    __imp__sub_000AD5C0(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_000B6400);
X86_FUNC(sub_000B6400) {
    const uint32_t self = ctx.ecx, event = X86_MEM32(ctx.esp + 4), input = X86_MEM32(ctx.esp + 8);
    if (self == g_shell_exit_owner && event == 1) g_shell_exit_owner = 0;
    if (self && event == kInputEvent && X86_MEM32(self + kShellState) == kShellChoiceState &&
        !X86_MEM8(self + kShellDemo) && X86_MEM8(kShellInputReady) && !X86_MEM8(kShellDialogOpen)) {
        const uint32_t row = X86_MEM32(self + kShellRow);
        if (g_shell_exit_owner == self) {
            const Step step = Input(Page::Exit, row, input | ((input & kShellAlternateBack) ? kBack : 0));
            X86_MEM32(self + kShellRow) = step.row;
            if (step.page != Page::Exit) {
                g_shell_exit_owner = 0;
                X86_MEM32(self + kShellRow) = kShellExitRow;
            }
            if (step.quit) RequestGameExit("Main menu");
            ::kernel::Return(ctx, 0, 3);
            return;
        }
        if (row == kShellExitRow && (input & (kAccept | kDown))) {
            if (input & kAccept) {
                g_shell_exit_owner = self;
                X86_MEM32(self + kShellRow) = RowOf(Page::Exit, Option::ExitCancel);
            }
            ::kernel::Return(ctx, 0, 3);
            return;
        }
        if (row == kShellExitRow - 1 && (input & kDown) && !(input & (kAccept | kUp))) {
            __imp__sub_000B6400(ctx, base);
            X86_MEM32(self + kShellRow) = kShellExitRow;
            return;
        }
    }
    __imp__sub_000B6400(ctx, base);
}

// 0xB13C0: Display tab setup (thiscall): copies the game's display settings
// into the menu for cancel. Runs on opening Options and on entering the tab.
X86_EXTERN_FUNC(__imp__sub_000B13C0);
X86_FUNC(sub_000B13C0) {
    __imp__sub_000B13C0(ctx, base);
    FlushSettings(); // a previous visit left before its idle save
    g_pending = false;
    g_on_entry = Current();
    g_entered = true;
    g_first = 0;
    g_page = Page::Main;
    g_sizes.clear();
    for (const app::Mode m : game::DisplayModes())
        g_sizes.push_back(Size(m.width, m.height));
}

// 0xB14F0: Display tab cancel (thiscall; Back in the row list): restores the
// settings copied at setup.
X86_EXTERN_FUNC(__imp__sub_000B14F0);
X86_FUNC(sub_000B14F0) {
    __imp__sub_000B14F0(ctx, base);
    if (g_entered) {
        Apply(g_on_entry);
        RestoreDisplayChoice(g_on_entry.applied_mode, g_on_entry.applied);
    }
    FlushSettings(); // Back restores and saves before the tab (or menu) closes
    g_first = 0;
    g_page = Page::Main;
}

X86_EXTERN_FUNC(__imp__sub_000C7B80);
X86_FUNC(sub_000C7B80) {
    PollDisplayApply();
    MaybeSave(); // the row getter runs every frame while the tab is open
    const uint32_t self = ctx.ecx, out = X86_MEM32(ctx.esp + 4), port = X86_MEM32(ctx.esp + 8);
    const bool exiting = self && self == g_exit.owner;
    const Page row_page = exiting ? Page::Exit : g_page;
    if (out && exiting && port == kTopTitle) {
        if (const uint32_t buffer = TextBuffer(uint32_t(Option::ExitConfirm))) {
            std::memcpy(base + buffer, "EXIT GAME", sizeof("EXIT GAME"));
            X86_MEM32(out) = buffer;
            ::kernel::Return(ctx, 0, 2);
            return;
        }
    }
    const bool text = port >= kDisplayText && port < kDisplayText + kShownRows;
    const bool colour = port >= kDisplayColour && port < kDisplayColour + kShownRows;
    if (out && (text || colour)) {
        // slot on screen -> logical row
        const uint32_t slot = port - (text ? kDisplayText : kDisplayColour);
        const uint32_t row = (exiting ? 0 : g_first) + slot;
        if (colour) {
            const bool lit = slot == X86_MEM32(self + kRow) && X86_MEM32(self + kFocus) == kRowListFocus;
            X86_MEM32(out) = lit ? kColourSelected : kColourNormal;
            ::kernel::Return(ctx, 0, 2);
            return;
        }
        if (row >= NativeRows(row_page) && row < RowCount(row_page)) {
            const Option option = OptionAt(row_page, row);
            char buffer_text[kTextBytes];
            Text(Current(), option, buffer_text);
            if (const uint32_t buffer = TextBuffer(uint32_t(option))) {
                std::memcpy(base + buffer, buffer_text, std::strlen(buffer_text) + 1);
                X86_MEM32(out) = buffer;
                ::kernel::Return(ctx, 0, 2);
                return;
            }
            LOG_FIRST(1, "Display tab: no guest memory for the row texts");
        }
        // SUBTITLES in the top slot: the game's own text for that port
    }

    __imp__sub_000C7B80(ctx, base);

    if (ctx.eax != 0 || !out) return;
    if (port == kDisplayCount) {
        const bool awaiting_accept = exiting && X86_MEM32(self + kFocus) == kTopBarFocus;
        X86_MEM32(out) = awaiting_accept ? 1 : std::min(kShownRows, RowCount(row_page));
    }
    if (Tracing() && port == kOptionsPage) {
        static thread_local uint32_t last_page = UINT32_MAX;
        const uint32_t page = X86_MEM32(out);
        if (page != last_page) {
            last_page = page;
            LOGR("Native Options page query: %u", page);
        }
    }
}

// MenuPauseInterface event dispatcher (thiscall: event, two arguments).
X86_EXTERN_FUNC(__imp__sub_000CB400);
X86_FUNC(sub_000CB400) {
    PollDisplayApply();
    MaybeSave();
    const uint32_t self = ctx.ecx, event = X86_MEM32(ctx.esp + 4), input = X86_MEM32(ctx.esp + 8);
    const uint32_t page = self ? X86_MEM32(self + kPage) : 0;
    if (self && self == g_exit.owner && event == 1) g_exit = {};
    if (self && self == g_exit.owner && event == kInputEvent) {
        if (X86_MEM32(self + kFocus) == kRowListFocus) {
            const Step step = Input(Page::Exit, X86_MEM32(self + kRow), input);
            X86_MEM32(self + kRow) = step.row;
            if (step.page != Page::Exit) X86_MEM32(self + kFocus) = kTopBarFocus;
            if (step.quit) RequestGameExit("Pause menu");
            ::kernel::Return(ctx, 0, 3);
            return;
        }

        if (input & (kLeft | kRight | kBack | kCancel)) {
            const uint32_t target = (input & kLeft) ? kOptionsTab : kMainTab;
            const bool back = (input & (kBack | kCancel)) != 0;
            X86_MEM32(self + kTopTab) = g_exit.tab;
            X86_MEM32(self + kPage) = g_exit.page;
            X86_MEM32(self + kRow) = g_exit.row;
            g_exit = {};
            if (back || X86_MEM32(self + kTopTab) != target)
                __imp__sub_000CB400(ctx, base);
            else
                ::kernel::Return(ctx, 0, 3);
            return;
        }
        if (input & kAccept) {
            X86_MEM32(self + kFocus) = kRowListFocus;
            X86_MEM32(self + kRow) = RowOf(Page::Exit, Option::ExitCancel);
        }
        ::kernel::Return(ctx, 0, 3);
        return;
    }
    if (self && Tracing() && event == kInputEvent && page >= 0x0F && page <= 0x12) {
        const uint32_t row = X86_MEM32(self + kRow), focus = X86_MEM32(self + kFocus);
        static thread_local uint32_t last[4] = {UINT32_MAX};
        const uint32_t now[4] = {input, page, row, focus};
        if (std::memcmp(now, last, sizeof(now))) {
            std::memcpy(last, now, sizeof(now));
            LOGR("Native Options dispatch: object=%08x page=%u row=%u focus=%u input=%08x", self, page - 0x0F, row,
                 focus, input);
        }
    }
    if (self && event == kInputEvent && page == kDisplayPage && X86_MEM32(self + kFocus) == kRowListFocus) {
        const Step step = Input(g_page, g_first + X86_MEM32(self + kRow), input);
        if (step.handled && (!step.apply || g_pending)) {
            const Page page = g_page; // the page the input's row belongs to
            if (step.page != g_page) {
                g_page = step.page;
                g_first = 0;
            }
            g_first = Scroll(step.row, g_first);
            X86_MEM32(self + kRow) = step.row - g_first;
            X86_MEM32(self + kFocus) = step.focus;
            if (step.change) {
                Values v = Current();
                const Option option = OptionAt(page, step.row);
                Change(v, option, step.change, g_sizes);
                if (option == Option::Resolution || option == Option::DisplayMode) {
                    g_pending_size = v.size;
                    g_pending_mode = v.mode;
                    g_pending = DisplayPending(v);
                } else {
                    Apply(v);
                }
            }
            if (step.apply) ApplyDisplayChoice(g_pending_mode, g_pending_size);
            // Navigation (no value change) means the arrow steps have settled;
            // leaving the row list/tab also has to save before the hooks stop.
            // A display apply writes the file once the window thread confirms it.
            if ((!step.change && !step.apply) || !OnDisplayRows(base, self)) FlushSettings();
            ::kernel::Return(ctx, 0, 3);
            return;
        }
    }
    const bool top_navigation = self && event == kInputEvent && (input & (kLeft | kRight)) &&
                                X86_MEM32(self + kFocus) == kTopBarFocus;
    const uint32_t old_tab = top_navigation ? X86_MEM32(self + kTopTab) : UINT32_MAX;
    __imp__sub_000CB400(ctx, base);
    if (top_navigation && X86_MEM32(self + kFocus) == kTopBarFocus) {
        const uint32_t tab = X86_MEM32(self + kTopTab);
        const bool wrapped_left = old_tab == kMainTab && tab == kOptionsTab && (input & kLeft);
        const bool wrapped_right = old_tab == kOptionsTab && tab == kMainTab && !(input & kLeft);
        if (wrapped_left || wrapped_right) {
            g_exit = {self, tab, X86_MEM32(self + kPage), X86_MEM32(self + kRow)};
            X86_MEM32(self + kTopTab) = kOptionsTab;
            X86_MEM32(self + kPage) = kDisplayPage;
            X86_MEM32(self + kRow) = 0;
        }
    }
    if (event == kInputEvent && page == kDisplayPage && !OnDisplayRows(base, self)) FlushSettings();
}
