// Read-only tracing of the native Options tab renderer. Off unless
// XBE_TRACE_OPTIONS_PAGE is set; the hook calls the original unchanged.

#include <cstdint>
#include <cstdlib>

#include "core/log.h"
#include "kernel/kernel.h"
#include "xdk/hle.h"

// The Options value getter (0xC7B80) and event dispatcher (0xCB400) are hooked in
// native_graphics.cpp, which also traces them.

// Trace explicit numTabs updates to identify the renderer used by Options.
X86_EXTERN_FUNC(__imp__sub_00250170);
X86_FUNC(sub_00250170) {
    static const bool trace = std::getenv("XBE_TRACE_OPTIONS_PAGE") != nullptr;
    const uint32_t renderer = trace && X86_MEM32(ctx.esp + 12) == 0xEF15D709u ? ctx.ecx : 0;
    const uint32_t caller = renderer ? X86_MEM32(ctx.esp) : 0;
    // The forwarding wrapper pushes three arguments before calling this setter.
    const uint32_t wrapper_caller = caller == 0x00251138u ? X86_MEM32(ctx.esp + 16) : 0;
    // The dispatcher has four outgoing arguments, two saved registers, and 12 local bytes.
    const uint32_t dispatch_caller = wrapper_caller == 0x00250E7Fu ? X86_MEM32(ctx.esp + 56) : 0;
    __imp__sub_00250170(ctx, base);

    if (renderer && ctx.eax == 0) {
        const uint32_t count = X86_MEM32(renderer + 0xA8);
        static thread_local uint32_t last_renderer = 0;
        static thread_local uint32_t last_count = UINT32_MAX;
        if (renderer != last_renderer || count != last_count) {
            last_renderer = renderer;
            last_count = count;
            LOGR("Native tab renderer: object=%08x count=%u return=%08x wrapper_return=%08x dispatch_return=%08x",
                 renderer, count, caller, wrapper_caller, dispatch_caller);
        }
    }
}
