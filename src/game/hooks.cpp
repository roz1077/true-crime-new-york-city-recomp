// Compatibility hooks for default.xbe (True Crime: New York City, XDK 5849).
// A strong definition replaces the recompiled function at that address.
// The cutscene clock and replay hooks are in timing.cpp.

#include <atomic>
#include <cstring>

#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

// 0x26381D: XAPI helper (HANDLE file), stdcall. References the file object,
// asks IoQueryVolumeInformation for the file system name and, for "FATX" or
// "GDFX", reads a position from the kernel's device object (FileObject+8),
// which host file objects do not have (read at 0x1F, 113 s). Its only caller,
// the game's stream scheduler 0x24BD7D, shifts the result by 11 (2 KiB sectors)
// and orders pending reads by distance from it: a drive head position. A PC has
// no head to save seeks for, so every request is equally near.
X86_FUNC(sub_0026381D) {
    (void)base;
    ::kernel::Return(ctx, 0, 1);
}

// 0x29366D: CDirectSoundStream 3D method (this, a, b, apply), after
// SetVelocity, through the stub 0x2938A6: in XDK order SetConeAngles(inside,
// outside, apply). Forwards to voice method 0x292FF6, which reads hardware
// voice state the host's software voices lack (read at 0xB4, 33 s in, once the
// world loads). The host mixer has no sound cones.
X86_FUNC(sub_0029366D) {
    (void)base;
    ::kernel::Return(ctx, 0, 4);
}

// 0x2BEDE0: D3DDevice_SetBackBufferScale(float x, float y), stdcall. Stores the
// scale at CDevice+0x96C/+0x970 (the 5849 layout's CDevice_BackBufferScale),
// then 0x2BA880 rebuilds the hardware render target state, which reads surfaces
// the host never made (read near null at 5.7 s). The game calls it with
// (1.0, 1.0) at 0x241293; the host renders at 1.0 already, so keep the stored
// values for the game to read back and skip the hardware part.
X86_FUNC(sub_002BEDE0) {
    constexpr uint32_t kDevice = 0x2C8F50, kScale = 0x96C;
    uint32_t x, y;
    std::memcpy(&x, base + ctx.esp + 4, 4);
    std::memcpy(&y, base + ctx.esp + 8, 4);
    std::memcpy(base + kDevice + kScale, &x, 4);
    std::memcpy(base + kDevice + kScale + 4, &y, 4);
    // as the library: a scale other than 1 sets flag 0x4000, and its Swap then
    // draws the back buffer onto the front buffer (the host's Swap presents the
    // back buffer surface then); 1 by 1 clears it
    uint32_t flags;
    std::memcpy(&flags, base + kDevice + 8, 4);
    if (x != 0x3F800000u || y != 0x3F800000u)
        flags |= 0x4000;
    else
        flags &= ~0x4000u;
    std::memcpy(base + kDevice + 8, &flags, 4);
    LOG_FIRST(8, "tcnyc: SetBackBufferScale %08X %08X, flags %08X", x, y, flags);
    ::kernel::Return(ctx, 0, 2);
}

// 0x293541: CDirectSoundStream 3D method (this, value, apply), between
// SetMinDistance and SetPosition; the game calls it as (stream, 0, 1) through
// the stub at 0x29384F, i.e. SetMode(DS3DMODE_NORMAL, DS3D_DEFERRED). It
// forwards to the voice method 0x292F1F, which the buffer method 0x293273 (the
// config calls it IDirectSoundBuffer_SetI3DL2Source) also uses. The SDK has no
// host version, so the original ran against the host's software voices and read
// near null. The host mixer has no 3D modes: accept it and return DS_OK.
X86_FUNC(sub_00293541) {
    (void)base;
    ::kernel::Return(ctx, 0, 3);
}

// 0x24C020: stream request done (ecx = stream entry, arg = request), called
// from the read completion routine on the streaming thread. A request with a
// callback (+0x40 context, +0x44 function) is written into the next slot of a
// ring (producer 0x4D48A8; nodes: +0 next, +4 context, +8 function) that the
// main thread's stream tick 0x24BEA0 drains once a frame (consumer 0x4D48A4,
// clears +8). The producer never checks the slot: on the console DVD reads are
// slow enough that the ring never fills, but host reads finish at once, so a
// burst of completions laps the ring between two frames and overwrites
// callbacks not yet run. Their resources then stay "loading" forever (the world
// never becomes ready: no 3D behind the menu, endless loading screen). Waiting
// deadlocks (the main thread may be waiting on the streaming side), so the ring
// is made 1024 slots longer while it is empty.
X86_EXTERN_FUNC(__imp__sub_0024C020);
X86_FUNC(sub_0024C020) {
    auto rv = [&](uint32_t a) { return *reinterpret_cast<volatile uint32_t *>(base + a); };
    auto wv = [&](uint32_t a, uint32_t v) { *reinterpret_cast<volatile uint32_t *>(base + a) = v; };
    constexpr uint32_t kProducer = 0x4D48A8, kConsumer = 0x4D48A4, kExtra = 1024, kNode = 12;
    static std::atomic<bool> grown{false};
    uint32_t slot = rv(kProducer);
    // grow once, while the ring is empty: consumer on the producer's node and
    // that slot unused, so the consumer stops there and never walks the nodes
    // being linked in after it
    if (!grown && slot && rv(kConsumer) == slot && !rv(slot + 8)) {
        uint32_t size = 1;
        for (uint32_t n = rv(slot); n && n != slot && size < 100000; n = rv(n))
            size++;
        uint32_t nodes = mem::AllocSystem(kExtra * kNode, 16), old_next = rv(slot);
        for (uint32_t i = 0; i < kExtra; i++) {
            uint32_t n = nodes + i * kNode;
            wv(n + 4, 0);
            wv(n + 8, 0);
            wv(n, i + 1 < kExtra ? n + kNode : old_next);
        }
        std::atomic_thread_fence(std::memory_order_seq_cst);
        wv(slot, nodes);
        grown = true;
        LOGI("tcnyc: stream callback ring grown from %u to %u slots", size, size + kExtra);
    }
    uint32_t req = rv(ctx.esp + 4);
    if (rv(req + 0x44) && slot && rv(slot + 8)) {
        static std::atomic<int> full{0};
        if (full++ < 20) LOGE("tcnyc: stream callback ring full at slot %08X, a callback is lost", slot);
    }
    __imp__sub_0024C020(ctx, base);
}

// The HUD menu loader leaves menuscript.tag in guest memory at object + 8.
// HUDTelephoneAnim starts at x = -64, but its 64-pixel icon is drawn with a
// one-pixel inset. Move only the hidden keyframe so its right edge lands at 0.
X86_EXTERN_FUNC(__imp__sub_00258EB0);
X86_FUNC(sub_00258EB0) {
    constexpr uint32_t kHudMenu = 0x363D50;
    const uint32_t menu = ctx.ecx;
    __imp__sub_00258EB0(ctx, base);
    if (menu != kHudMenu) return;

    constexpr uint32_t kPhoneTag = 0xFD8, kHiddenKey = 0x87C40;
    const uint32_t data = X86_MEM32(menu + 8);
    if (!data || uint64_t(data) + kHiddenKey + 12 > 0x100000000ull ||
        !mem::IsCommitted(data + kPhoneTag) || !mem::IsCommitted(data + kHiddenKey) ||
        !mem::IsCommitted(data + kHiddenKey + 11)) return;

    if (X86_MEM32(data + kPhoneTag) == 0x4BBD4B3E &&  // HUDTelephoneAnim
        X86_MEM32(data + kHiddenKey) == 0x8CDC1683 &&  // x
        X86_MEM32(data + kHiddenKey + 4) == 0x10000 &&
        X86_MEM32(data + kHiddenKey + 8) == uint32_t(-64))
        X86_MEM32(data + kHiddenKey + 8) = uint32_t(-65);
}
