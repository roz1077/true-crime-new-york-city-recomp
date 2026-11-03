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
// The rules are in display_rows.h; GRAPHICS_NOTES.md has the research.

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "app/app.h"
#include "common/graphics_menu.h"
#include "core/log.h"
#include "display_rows.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

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

Values Current() {
    const app::Settings &s = app::Get();
    return {s.render_percent, game::g_fov_percent, s.aniso, s.fps_cap, s.vsync, s.smaa};
}

// guest thread; the renderer reads these each frame and app::Pace each Swap (aligned 32-bit stores)
void Apply(const Values &v) {
    app::Settings &s = app::Get();
    s.render_percent = v.render_percent;
    game::g_fov_percent = v.fov_percent;
    s.aniso = v.aniso;
    s.fps_cap = v.fps_cap;
    s.vsync = v.vsync;
    s.smaa = v.smaa;
    app::Save();
}

// guest buffers for the row texts, one per added row
uint32_t TextBuffer(uint32_t index) {
    static const uint32_t buffers = mem::AllocSystem(kExtraRows * kTextBytes, 16);
    return buffers + index * uint32_t(kTextBytes);
}

// what the rows had when the Display tab was entered, for cancel; menu thread only
Values g_on_entry{};
bool g_entered = false;
// the logical row shown in the top slot (display_rows.h Scroll); menu thread only. Reset
// where the game resets the row: entering the tab and cancel.
uint32_t g_first = 0;

} // namespace

// 0xB13C0: Display tab setup (thiscall): copies the game's display settings
// into the menu for cancel. Runs on opening Options and on entering the tab.
X86_EXTERN_FUNC(__imp__sub_000B13C0);
X86_FUNC(sub_000B13C0) {
    __imp__sub_000B13C0(ctx, base);
    g_on_entry = Current();
    g_entered = true;
    g_first = 0;
}

// 0xB14F0: Display tab cancel (thiscall; Back in the row list): restores the
// settings copied at setup.
X86_EXTERN_FUNC(__imp__sub_000B14F0);
X86_FUNC(sub_000B14F0) {
    __imp__sub_000B14F0(ctx, base);
    if (g_entered) Apply(g_on_entry);
    g_first = 0;
}

X86_EXTERN_FUNC(__imp__sub_000C7B80);
X86_FUNC(sub_000C7B80) {
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
        if (row >= kNativeRows) {
            const uint32_t index = row - kNativeRows;
            char buffer_text[kTextBytes];
            Text(Current(), kOptions[index], buffer_text);
            const uint32_t buffer = TextBuffer(index);
            std::memcpy(base + buffer, buffer_text, std::strlen(buffer_text) + 1);
            X86_MEM32(out) = buffer;
            ::kernel::Return(ctx, 0, 2);
            return;
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
        const Step step = Input(g_first + X86_MEM32(self + kRow), input);
        if (step.handled) {
            g_first = Scroll(step.row, g_first);
            X86_MEM32(self + kRow) = step.row - g_first;
            X86_MEM32(self + kFocus) = step.focus;
            if (step.change) {
                Values v = Current();
                Change(v, kOptions[step.row - kNativeRows], step.change);
                Apply(v);
            }
            ::kernel::Return(ctx, 0, 3);
            return;
        }
    }
    __imp__sub_000CB400(ctx, base);
}
