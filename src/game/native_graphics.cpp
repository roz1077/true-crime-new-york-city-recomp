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

// MenuPauseInterface fields
constexpr uint32_t kPage = 0x30;  // 0x0F to 0x12: the Options tabs
constexpr uint32_t kRow = 0x34;   // selected row
constexpr uint32_t kFocus = 0x7C; // kTabBarFocus or kRowListFocus
constexpr uint32_t kDisplayPage = 0x11;
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

// the window thread's answer to the display change in flight: keep and save it, or put the
// previous mode back
void PollDisplayApply() {
    if (!g_display_request) return;
    bool ok = false;
    if (!app::DisplayApplyResult(g_display_request, ok)) return;
    g_display_request = 0;
    // a refused change never reached the settings, and the window thread put the previous mode back
    if (!ok) LOGE("Display tab: the display change was refused; kept the previous mode");
    g_save_dirty.store(true, std::memory_order_release);
    FlushSettings();
}

void MaybeSave() {
    if (g_save_dirty.load(std::memory_order_acquire) && NowMs() >= g_save_due.load(std::memory_order_relaxed))
        FlushSettings();
}

bool OnDisplayRows(uint8_t* base, uint32_t self) {
    return X86_MEM32(self + kPage) == kDisplayPage && X86_MEM32(self + kFocus) == kRowListFocus;
}

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

// Cancel: put back the mode the tab was entered with. The window thread answers requests in order,
// so this one also lands after a change still in flight.
void RestoreDisplayChoice(uint32_t mode, uint64_t size) {
    g_pending = false;
    PollDisplayApply();
    const bool in_flight = g_display_request != 0;
    const uint32_t w = uint32_t(size >> 32), h = uint32_t(size);
    {
        app::SettingsLock lock;
        const app::Settings &s = app::Get();
        if (!in_flight && s.width == w && s.height == h && uint32_t(s.display_mode) == mode) return;
    }
    if (const uint64_t request = app::RequestDisplayApply(app::DisplayMode(mode), w, h)) {
        g_display_request = request;  // the earlier request's answer is dropped; this one is saved
        return;
    }
    g_display_request = 0;
    LOGE("Display tab: could not request the entry display mode %u, %ux%u back", mode, w, h);
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

} // namespace

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
    const bool text = port >= kDisplayText && port < kDisplayText + kShownRows;
    const bool colour = port >= kDisplayColour && port < kDisplayColour + kShownRows;
    if (out && (text || colour)) {
        // slot on screen -> logical row
        const uint32_t slot = port - (text ? kDisplayText : kDisplayColour), row = g_first + slot;
        if (colour) {
            const bool lit = slot == X86_MEM32(self + kRow) && X86_MEM32(self + kFocus) == kRowListFocus;
            X86_MEM32(out) = lit ? kColourSelected : kColourNormal;
            ::kernel::Return(ctx, 0, 2);
            return;
        }
        if (row >= NativeRows(g_page) && row < RowCount(g_page)) {
            const Option option = OptionAt(g_page, row);
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
    if (port == kDisplayCount) X86_MEM32(out) = kShownRows;
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
    if (Tracing() && event == kInputEvent && page >= 0x0F && page <= 0x12) {
        const uint32_t row = X86_MEM32(self + kRow), focus = X86_MEM32(self + kFocus);
        static thread_local uint32_t last[4] = {UINT32_MAX};
        const uint32_t now[4] = {input, page, row, focus};
        if (std::memcmp(now, last, sizeof(now))) {
            std::memcpy(last, now, sizeof(now));
            LOGR("Native Options dispatch: object=%08x page=%u row=%u focus=%u input=%08x", self, page - 0x0F, row,
                 focus, input);
        }
    }
    if (event == kInputEvent && page == kDisplayPage && X86_MEM32(self + kFocus) == kRowListFocus) {
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
    __imp__sub_000CB400(ctx, base);
    if (event == kInputEvent && page == kDisplayPage && !OnDisplayRows(base, self)) FlushSettings();
}
