#include "game_addresses.h"
// Pedestrian detail and pedestrian limit: when, and how many, distant stand-in pedestrians become full ones.
//
// Far pedestrians are cheap LowPed.XB2 stand-ins kept in a list on the crowd manager ([0x3A8260] + 0x80, count
// at +0x84, 0x60-byte entries, added by 0x1B4730). The crowd update 0x1B4F20 walks that list each frame. For an
// entry in front of the camera it calls 0x1B1C60 (from 0x1B50E7) and then compares the entry's distance, kept in
// the caller's frame at [esp+0x60], against 7.8125 (the shared constant at 0x2D3C74):
//   0x1B50F2  distance < 7.8125, not being removed ([e+0x30] & 4 clear), no full pedestrian yet ([e+0x3C] == 0)
//   0x1B510D  and [0x2F01B4] set
//   0x1B511D  and fewer than 15 full characters ([[0x360E00]+0xBC]; +1 in 0xF9B49, -1 in 0xFB775)
//             -> 0x1B3C00 builds the full pedestrian (its allocations fail cleanly: it checks for null).
//   When [0x2F01B4] or the cap blocks the swap, the game removes the stand-in instead; beyond 46.875
//   (0x2D75B4) a stand-in is marked for removal.
//
// Neither the constant (shared with unrelated code) nor the immediate 15 can be patched in place. On return to
// 0x1B50EC, when every other condition for the swap holds:
//   ped_detail  lowers the stored distance just under 7.8125 when it lies inside a wider radius, so the game's
//               own swap runs. The radius stays at or below 46.875, the stand-ins' own range.
//   ped_limit   when the count is at or above 15 but under the chosen limit, the count is lowered just for the
//               compare at 0x1B511D and put back on entry to 0x1B3C00, the very next call on that path.
// Entries that would not swap are left untouched, so removal and the original cap behave as before.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "app/app.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

namespace {

constexpr uint32_t kSwapCallReturn = TC_ADDR(0x1B50EC);
constexpr uint32_t kDistanceSlot = 0x60;     // caller esp + 0x60 once 0x1B1C60 has returned (ret 4)
constexpr float kGameSwap = 7.8125f;
constexpr uint32_t kGame = TC_ADDR(0x360E00), kFullPedCount = 0xBC, kGameLimit = 15;
constexpr uint32_t kSwapAllowed = TC_ADDR(0x2F01B4);

float Radius() {
    const uint32_t level = std::atomic_ref(app::Get().ped_detail).load(std::memory_order_relaxed);
    constexpr float kRadii[] = {kGameSwap, kGameSwap * 2.0f, kGameSwap * 4.0f, 46.875f};
    return kRadii[level < 4 ? level : 0];
}

int32_t Limit();
// extra ambient types over the game's 5: tied to Pedestrian limit for the experiment (20/25/30 -> 1/2/3)
// 20/25/30 -> 1/2/3 extra, 40 and up -> 4 (9 types). 10 types plus a script-requested one ran the asset table
// out and crashed while loading a type at Pedestrian limit 75.
int32_t VarietyExtra() { return std::clamp((Limit() - 15) / 5, 0, 4); }

int32_t Limit() {
    const uint32_t level = std::atomic_ref(app::Get().ped_limit).load(std::memory_order_relaxed);
    constexpr int32_t kLimits[] = {15, 20, 25, 30, 40, 50, 75};
    return kLimits[level < 7 ? level : 0];
}

// game thread only: how much the count was lowered for the cap compare, and where
int32_t g_lowered = 0;
uint32_t g_lowered_at = 0;

void RestoreCount(uint8_t *base) {
    if (!g_lowered) return;
    X86_MEM32(g_lowered_at) = uint32_t(int32_t(X86_MEM32(g_lowered_at)) + g_lowered);
    g_lowered = 0;
}

} // namespace

X86_EXTERN_FUNC(__imp__sub_001B1C60);
X86_FUNC(sub_001B1C60) {
    RestoreCount(base);  // never expected here; keeps the count right if the swap did not follow
    const uint32_t ret = X86_MEM32(ctx.esp);
    const uint32_t entry = ctx.ecx;
    __imp__sub_001B1C60(ctx, base);
    if (ret != kSwapCallReturn) return;
    const float radius = Radius();
    const int32_t limit = Limit();
    if (radius <= kGameSwap && limit <= int32_t(kGameLimit)) return;

    const uint32_t slot = ctx.esp + kDistanceSlot;
    float distance;
    std::memcpy(&distance, base + slot, 4);
    if (!(distance < radius)) return;
    // only where the game's swap would then succeed; otherwise it would remove the stand-in
    if (X86_MEM8(entry + 0x30) & 0x04) return;
    if (X86_MEM32(entry + 0x3C)) return;
    if (!X86_MEM32(kSwapAllowed)) return;
    // Only upgrade early when the stand-in's type ([e+0x34], a slot at [0x360E00] + 0x27A0) still has a quota
    // in the current pedestrian mix. When the game switches mix (district or time of day) it sets outgoing
    // types' quotas to 0; full pedestrians made far away outlive the switch, keep those types loaded, and the
    // street stayed empty until they left. Within the game's own swap distance nothing changes.
    if (distance >= kGameSwap) {
        const uint32_t type = X86_MEM32(entry + 0x34);
        if (!type || int16_t(X86_MEM16(type + 0x0C)) <= 0) return;
    }
    const uint32_t game = X86_MEM32(kGame);
    if (!game) return;
    const uint32_t count_at = game + kFullPedCount;
    const int32_t count = int32_t(X86_MEM32(count_at));
    if (count >= limit) return;
    // Keep places free for stand-ins that walk up close. Upgrades are first come, first served, so far ones
    // filled every place and a stand-in near the player stayed low detail (reduced model and animation) until
    // one freed, then snapped to full.
    // The share allowed falls with distance: the whole limit at the game's own swap distance, half of it at the
    // edge of the chosen radius, so nearer stand-ins always find a place before farther ones take it.
    if (distance >= kGameSwap) {
        const float t = (distance - kGameSwap) / std::max(radius - kGameSwap, 1.0f);
        const int32_t allowed = limit - int32_t(float(limit) * 0.5f * t + 0.5f);
        if (count >= allowed) return;
    }

    if (distance >= kGameSwap) {
        const float swap = kGameSwap - 0.01f;
        std::memcpy(base + slot, &swap, 4);
    }
    if (count >= int32_t(kGameLimit)) {
        g_lowered = count - int32_t(kGameLimit) + 1;
        g_lowered_at = count_at;
        X86_MEM32(count_at) = uint32_t(count - g_lowered);
    }
}

X86_EXTERN_FUNC(__imp__sub_001B3C00);
X86_FUNC(sub_001B3C00) {
    if (g_lowered) {
        RestoreCount(base);
        static bool logged = false;
        if (!logged) {
            logged = true;
            LOGR("pedestrians: building a full pedestrian past the game's limit of %u", kGameLimit);
        }
    }
    __imp__sub_001B3C00(ctx, base);
}

namespace {
// Applies the even quotas for the duration of a call and puts the game's back afterwards.
struct EvenQuotas {
    uint8_t *base;
    uint32_t game = 0;
    int16_t saved[16] = {};
    bool active = false;
    int16_t even_value = 0;
    explicit EvenQuotas(uint8_t *b) : base(b) {
        game = X86_MEM32(kGame);
        if (VarietyExtra() <= 0 || !game) return;
        int32_t sum = 0, n = 0;
        for (uint32_t i = 0; i < 16; ++i) {
            saved[i] = int16_t(X86_MEM16(Quota(i)));
            if (Counted(i)) { sum += saved[i]; ++n; }
        }
        if (n < 2) return;
        const int16_t avg = int16_t(std::max<int32_t>(1, sum / n));
        for (uint32_t i = 0; i < 16; ++i)
            if (Counted(i)) X86_MEM16(Quota(i)) = uint16_t(avg);
        even_value = avg;
        active = true;
    }
    ~EvenQuotas() {
        if (!active) return;
        // a quota the game rewrote during the call (an area or time change) is left as the game set it
        for (uint32_t i = 0; i < 16; ++i)
            if (saved[i] > 0 && int16_t(X86_MEM16(Quota(i))) == even_value) X86_MEM16(Quota(i)) = uint16_t(saved[i]);
    }
    uint32_t Quota(uint32_t i) const { return game + 0x27A0 + i * 0x14 + 0x0C; }
    bool Counted(uint32_t i) const {
        return saved[i] > 0 && int8_t(X86_MEM8(game + 0x27A0 + i * 0x14 + 2)) >= 0;
    }
};
} // namespace


X86_EXTERN_FUNC(__imp__sub_001B4F20);
X86_FUNC(sub_001B4F20) {
    EvenQuotas even(base);
    __imp__sub_001B4F20(ctx, base);
}

// Pedestrian variety, part 2: even quotas. 0x14C30 picks a type weighted by quota (+0x0C) minus in use (+0x10).
// The area data often gives one type most of the quota (145 against 6-20 for the rest), so most of the crowd is
// that one type. While variety is raised, every loaded type with a quota gets the average quota for the pick.

// The pick (0x14C30) and the crowd update (0x1B4F20, see below) both see the even quotas: the crowd update
// also checks quotas when a stand-in is upgraded or kept, and with the game's own quotas it removed stand-ins
// of over-quota types as the player approached.
X86_EXTERN_FUNC(__imp__sub_00014C30);
X86_FUNC(sub_00014C30) {
    EvenQuotas even(base);
    __imp__sub_00014C30(ctx, base);
}

// Full character models. Bodies and heads load through 0x7D820 and 0x7DA50; with bit 0x200 of the first
// argument set they build the path from "!LRESCHAR!\BODIES\" (0x2D4A58, 19 bytes copied) or "!LRESCHAR!\HEADS\"
// (0x2D4A7C, 18 bytes), otherwise from "!CHAR!\BODIES\" (0x2D4A48) / "!CHAR!\HEADS\" (0x2D4A6C). LResChar meshes
// carry three detail levels, so a pedestrian built far away shows a middle level first and visibly steps up as
// it nears; the Char files hold one full-detail mesh (about 10% larger; redirecting every character there was
// verified in game on 2026-11-04). While Pedestrian detail is Maximum, the LResChar strings are made to
// read as the Char ones (the copy stops at the earlier null), so only characters loaded after the change move.
namespace {

struct PathSwap {
    uint32_t lres, full, bytes;
    uint8_t original[20];
    bool saved;
};
PathSwap g_paths[] = {{TC_ADDR(0x2D4A58), TC_ADDR(0x2D4A48), 19, {}, false}, {TC_ADDR(0x2D4A7C), TC_ADDR(0x2D4A6C), 18, {}, false}};

void ApplyCharPaths(uint8_t *base) {
    const bool full = std::atomic_ref(app::Get().ped_detail).load(std::memory_order_relaxed) == 3;  // Maximum
    for (PathSwap &p : g_paths) {
        if (!p.saved) {
            std::memcpy(p.original, base + p.lres, p.bytes);
            p.saved = true;
        }
        uint8_t want[20] = {};
        if (full) {
            std::memcpy(want, base + p.full, p.bytes - 4);  // "!CHAR!\..." and its null fit in the first bytes
        } else {
            std::memcpy(want, p.original, p.bytes);
        }
        if (std::memcmp(base + p.lres, want, p.bytes)) std::memcpy(base + p.lres, want, p.bytes);
    }
}

} // namespace

X86_EXTERN_FUNC(__imp__sub_0007D820);
X86_FUNC(sub_0007D820) {
    ApplyCharPaths(base);
    __imp__sub_0007D820(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_0007DA50);
X86_FUNC(sub_0007DA50) {
    ApplyCharPaths(base);
    __imp__sub_0007DA50(ctx, base);
}


// Per-frame character cache. The renderer keeps one 0x1410-byte slot per full character drawn this frame in a
// fixed table at 0x3F58E0: 32 slots, then the slot count at +0x28200 (0x41DAE0). Five routines use it, all
// with ecx = 0x3F58E0 except the reset, which stores to 0x41DAE0 directly:
//   0x21C520  find(key, out*)    slot data (+0x10) for a key, and the slot's +0x04 value through out
//   0x21C570  add(key)           appends a slot with no bounds check and returns its data (+0x10)
//   0x21C5C0  truncate(key)      count = index of key's slot
//   0x21C600  set(key, value)    the slot's +0x04 value
//   0x21C640  reset()            count = 0
// More than 32 full characters on screen (a raised Pedestrian limit plus the player, police and drivers)
// overran it into whatever follows and crashed in 0x25FBB0. The five routines are replaced here by the same
// logic over a larger table allocated in guest memory, with its count kept apart from the slots. Should even
// that fill, add() hands back one spare slot rather than writing past the end.
namespace {

constexpr uint32_t kCacheGame = TC_ADDR(0x3F58E0), kSlotBytes = 0x1410, kCacheSlots = 128;
uint32_t g_cache = 0;        // guest address: kCacheSlots slots plus one spare
int32_t g_cache_count = 0;   // game thread only
bool g_cache_warned = false;
// each slot's key, contiguous: Find runs per character per frame, and walking the 0x1410-byte slots
// touches a different page for every entry
uint32_t g_cache_keys[kCacheSlots];

uint32_t Cache() {
    if (!g_cache) {
        uint32_t at = 0, size = (kCacheSlots + 1) * kSlotBytes;
        // From the top of user space, as the city grid store is: a low first-fit
        // address can land in the game's own heap, which later overwrites it.
        if (mem::AllocateVirtual(at, size, mem::kMemReserve | mem::kMemCommit | mem::kMemTopDown, 0x04) == 0 && at) {
            g_cache = at;
            LOGR("pedestrians: character cache moved to %08X, %u slots", at, kCacheSlots);
        } else {
            LOGR("pedestrians: could not allocate the larger character cache; keeping the game's");
        }
    }
    return g_cache;
}

uint32_t Slot(int32_t i) { return g_cache + uint32_t(i) * kSlotBytes; }

int32_t Find(uint8_t *base, uint32_t key) {
    for (int32_t i = 0; i < g_cache_count; ++i)
        if (g_cache_keys[i] == key) return i;
    return -1;
}

} // namespace

X86_EXTERN_FUNC(__imp__sub_0021C520);
X86_FUNC(sub_0021C520) {
    if (ctx.ecx != kCacheGame || !Cache()) return __imp__sub_0021C520(ctx, base);
    const uint32_t key = X86_MEM32(ctx.esp + 4), out = X86_MEM32(ctx.esp + 8);
    const int32_t i = Find(base, key);
    if (i < 0) {
        ctx.eax = 0;
    } else {
        X86_MEM32(out) = X86_MEM32(Slot(i) + 4);
        ctx.eax = Slot(i) + 0x10;
    }
    ctx.esp += 12;  // ret 8
}

X86_EXTERN_FUNC(__imp__sub_0021C570);
X86_FUNC(sub_0021C570) {
    if (ctx.ecx != kCacheGame || !Cache()) return __imp__sub_0021C570(ctx, base);
    const uint32_t key = X86_MEM32(ctx.esp + 4);
    uint32_t slot;
    if (g_cache_count < int32_t(kCacheSlots)) {
        slot = Slot(g_cache_count);
        g_cache_keys[g_cache_count] = key;
        ++g_cache_count;
    } else {
        slot = Slot(kCacheSlots);  // the spare: shared, but never past the end
        if (!g_cache_warned) {
            g_cache_warned = true;
            LOGR("pedestrians: character cache full (%u slots)", kCacheSlots);
        }
    }
    X86_MEM32(slot) = key;
    X86_MEM32(slot + 4) = 0;
    ctx.eax = slot + 0x10;
    ctx.esp += 8;  // ret 4
}

X86_EXTERN_FUNC(__imp__sub_0021C5C0);
X86_FUNC(sub_0021C5C0) {
    if (ctx.ecx != kCacheGame || !Cache()) return __imp__sub_0021C5C0(ctx, base);
    const int32_t i = Find(base, X86_MEM32(ctx.esp + 4));
    if (i >= 0) g_cache_count = i;
    ctx.eax = uint32_t(i >= 0 ? i : g_cache_count);
    ctx.esp += 8;  // ret 4
}

X86_EXTERN_FUNC(__imp__sub_0021C600);
X86_FUNC(sub_0021C600) {
    if (ctx.ecx != kCacheGame || !Cache()) return __imp__sub_0021C600(ctx, base);
    const int32_t i = Find(base, X86_MEM32(ctx.esp + 4));
    if (i >= 0) X86_MEM32(Slot(i) + 4) = X86_MEM32(ctx.esp + 8);
    ctx.eax = uint32_t(i >= 0 ? i : g_cache_count);
    ctx.esp += 12;  // ret 8
}

X86_EXTERN_FUNC(__imp__sub_0021C640);
X86_FUNC(sub_0021C640) {
    g_cache_count = 0;
    __imp__sub_0021C640(ctx, base);
}

// Pedestrian variety, part 1: more types at once. 0x87350 streams in ambient types for the area: at most
// min([this+0xC4], 5) (the 5 is an immediate at 0x87368) minus those loaded, and also at most
// 5 - (used body entries) + 1 and (5 * 3 / 2 - (used head entries) + 1) / 2, counting entries in the asset table
// at 0x374938 with 0xDB4F0: 12 body entries from id 0x1C (called from 0x87465) and 36 head entries from 0x28
// (from 0x87488). Those terms use the target 5, not the table size, so the type count stops at 5 while the
// table has room for 12 bodies.
// While variety is raised, both counts come back lowered by the extra types (never below 0, and never by more
// than the table's real free entries), and loaded-type slots are hidden from the routine's own count for the
// call, so it streams in up to 5 + extra types.
namespace {
uint32_t g_variety_reduce = 0;  // set only for the duration of a 0x87350 call
}

X86_EXTERN_FUNC(__imp__sub_000DB4F0);
X86_FUNC(sub_000DB4F0) {
    const uint32_t ret = X86_MEM32(ctx.esp);
    const uint32_t first = X86_MEM32(ctx.esp + 4), span = X86_MEM32(ctx.esp + 8);
    __imp__sub_000DB4F0(ctx, base);
    if (!g_variety_reduce || (ret != TC_ADDR(0x8746A) && ret != TC_ADDR(0x8748D))) return;
    const int32_t used = int32_t(ctx.eax);
    const int32_t free_entries = int32_t(span) - used;
    int32_t reduce = int32_t(g_variety_reduce) * (ret == TC_ADDR(0x8748D) ? 2 : 1);  // heads: 3/2 per type, rounded up
    reduce = std::min(reduce, std::max(0, free_entries - 2));  // keep two entries spare
    ctx.eax = uint32_t(std::max(0, used - reduce));
    (void)first;
}

X86_EXTERN_FUNC(__imp__sub_00087350);
X86_FUNC(sub_00087350) {
    const int32_t extra = VarietyExtra();
    const uint32_t game = X86_MEM32(kGame);
    uint32_t hidden[16];
    int count = 0;
    if (extra > 0 && game) {
        for (uint32_t i = 1; i < 16 && count < extra; ++i) {
            const uint32_t slot = game + 0x27A0 + i * 0x14;
            if ((X86_MEM8(slot) & 0x02) && int8_t(X86_MEM8(slot + 2)) >= 0 && !(X86_MEM8(slot) & 0x01)) {
                X86_MEM8(slot) &= uint8_t(~0x02);
                hidden[count++] = slot;
            }
        }
        g_variety_reduce = uint32_t(extra);
    }
    // Rotation guard. After counting, the routine retires the first loaded, ready type that is not protected
    // (flag 0x200, which script-requested types carry) whenever the other loaded types number the same as it is
    // about to load (0x8759E), and unloads it at once if none are in use (0x875D0). With types hidden that fired
    // over and over: new types were swapped out before any pedestrian used them and only ~3 types stayed
    // populated. While variety is raised, loaded types still in the current mix (quota above 0) get flag 0x200
    // for the call; types the area has dropped (quota 0) can still rotate out.
    uint32_t guarded[16];
    int nguarded = 0;
    if (extra > 0 && game) {
        for (uint32_t i = 1; i < 16; ++i) {
            const uint32_t slot = game + 0x27A0 + i * 0x14;
            if (int8_t(X86_MEM8(slot + 2)) < 0 || (X86_MEM8(slot) & 0x01) || (X86_MEM8(slot + 1) & 0x02)) continue;
            if (int16_t(X86_MEM16(slot + 0x0C)) <= 0) continue;
            X86_MEM8(slot + 1) |= 0x02;
            guarded[nguarded++] = slot;
        }
    }
    __imp__sub_00087350(ctx, base);
    g_variety_reduce = 0;
    for (int i = 0; i < nguarded; ++i) X86_MEM8(guarded[i] + 1) &= uint8_t(~0x02);
    for (int i = 0; i < count; ++i) X86_MEM8(hidden[i]) |= 0x02;
}

// Full-pedestrian despawn distance. Each character re-checks itself on a timer: event 0x2DDCA6EF, queued on the
// world event queue (0x37F610) by 0xF9A10 and re-queued by its handler 0xF9C60. The handler deletes the
// character (0x21FC00, returning -2) when its distance from the player is beyond the global at 0x37D33C, or
// when it is on foot, has existed past [0x2D2F5C] ticks and is beyond the global at 0x37D340. Those suit the
// game's own 7.8-unit swap, so pedestrians made full farther out by Pedestrian detail deleted themselves within
// a second and were made full again: ~150 times every 5 s, seen as flicker between stand-in and full model.
// While Pedestrian detail is raised, the first distance is at least the chosen radius plus a margin for the call.
X86_EXTERN_FUNC(__imp__sub_000F9C60);
X86_FUNC(sub_000F9C60) {
    const float radius = Radius();
    if (radius <= kGameSwap) return __imp__sub_000F9C60(ctx, base);
    constexpr uint32_t kFar = TC_ADDR(0x37D33C), kFarOnFoot = TC_ADDR(0x37D340);
    float far_, on_foot;
    std::memcpy(&far_, base + kFar, 4);
    std::memcpy(&on_foot, base + kFarOnFoot, 4);
    static bool logged = false;
    if (!logged) {
        logged = true;
        float other;
        std::memcpy(&other, base + TC_ADDR(0x37D348), 4);
        LOGR("pedestrians: character despawn distances %g / %g on foot (and %g)", far_, on_foot, other);
    }
    const float want = radius + 8.0f;
    // Only the first distance is raised. The second (1.5625 in game) is not a walking distance; raising it to
    // the radius switched off the game's main cleanup, full characters were left behind across the city
    // holding every place, and stand-ins near the player were removed when their upgrade was refused.
    const float new_far = std::max(far_, want);
    std::memcpy(base + kFar, &new_far, 4);
    __imp__sub_000F9C60(ctx, base);
    std::memcpy(base + kFar, &far_, 4);
}

// Crowd density. Each pedestrian spawn point (0x168F0, message handler) keeps adding stand-ins while its count
// ([point+0x82]) is below the global per-point cap at 0x2F01B0 (12; a sixth of it for some points, 0x16A28),
// read only here. Crowd density raises it by 1.5x, 2x or 3x for the call, so the
// crowd is large enough to fill the extra full places. The crowd budget (250, 0x13D90) and the stand-in list
// (400 entries, 0x1B4730) still apply.
X86_EXTERN_FUNC(__imp__sub_000168F0);
X86_FUNC(sub_000168F0) {
    const uint32_t density = std::atomic_ref(app::Get().ped_density).load(std::memory_order_relaxed);
    if (density == 0 || density > 3) return __imp__sub_000168F0(ctx, base);
    constexpr uint32_t kPointCap = TC_ADDR(0x2F01B0);
    const int32_t cap = int32_t(X86_MEM32(kPointCap));
    X86_MEM32(kPointCap) = uint32_t(density == 3 ? cap * 3 : density == 2 ? cap * 2 : cap * 3 / 2);
    __imp__sub_000168F0(ctx, base);
    X86_MEM32(kPointCap) = uint32_t(cap);
}

// Crowd budget. 0x13D90 returns the most stand-ins the spawner keeps (250, compared with [game+0xB8] at 0x16A88,
// 0x16B79 and 0x16BD5). At Crowd density 3x it returns 350, still under the stand-in list's 400 entries
// (0x1B4730 refuses past that).
X86_EXTERN_FUNC(__imp__sub_00013D90);
X86_FUNC(sub_00013D90) {
    __imp__sub_00013D90(ctx, base);
    if (std::atomic_ref(app::Get().ped_density).load(std::memory_order_relaxed) == 3 && int32_t(ctx.eax) == 250)
        ctx.eax = 350;
}

// Crowd range. Pedestrian spawn points only spawn while the player is within 12.5 units of them
// ([0x2D2AB0], compared with the distance 0x14990 writes through its first argument, at 0x16A53 and 0x16B0C;
// 0x14990 is called only from the spawn point handler 0x168F0). So stand-ins appear only near the player, and
// farther ones exist only because the player walked away from them. While Pedestrian detail is Very high or
// Maximum, the reported distance is divided by 2 or 3, so spawn points 25 or 37.5 units away fill too. Stand-ins
// in front of the camera are still removed beyond 46.875 (0x1B5149), and the crowd budget still applies.
X86_EXTERN_FUNC(__imp__sub_00014990);
X86_FUNC(sub_00014990) {
    const uint32_t out = X86_MEM32(ctx.esp + 4);
    const uint32_t level = std::atomic_ref(app::Get().ped_detail).load(std::memory_order_relaxed);
    __imp__sub_00014990(ctx, base);
    if (level < 2 || !out) return;
    float distance;
    std::memcpy(&distance, base + out, 4);
    distance /= level >= 3 ? 3.0f : 2.0f;
    std::memcpy(base + out, &distance, 4);
}
