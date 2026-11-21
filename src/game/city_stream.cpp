#include "game_addresses.h"
#include "core/dev_env.h"
#include "game/matrix_mul.h"
// City streaming probe (Phase 0 of docs/city-streaming-plan.md).
//
// Nothing here changes game behaviour. With XBE_TRACE_CITY=1 it logs, for the nine fixed city tile slots:
//   - each load request made through 0x1378F0 (grid x/y, size, stream handle);
//   - every change of a slot's grid position, loaded flag or handle, with the physical-memory use at the time;
//   - a five-second summary of the nine slots and the physical/virtual/system memory use.
// The city object is [0x37F8FC]; its slots start at +0xE0, stride 0x8B0 (x at +0x04, y at +0x08, stream handle
// at +0x0C, size at +0x28, loaded flag at +0x50). 0x13D880 initialises them; the draw loop at 0x13F1B0 calls
// 0x13C4C0 nine times a frame, which is used as the frame tick for sampling.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <array>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "guest_exports.h"

#include "app/app.h"
#include "core/log.h"
#include "game/city_grid.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

namespace {

constexpr uint32_t kCityPtr = TC_ADDR(0x37F8FC);
constexpr uint32_t kSlots = 0xE0, kSlotStride = 0x8B0, kSlotCount = 9;
constexpr uint32_t kX = 0x04, kY = 0x08, kHandle = 0x0C, kSize = 0x28, kLoaded = 0x50;

bool Trace() {
    static const bool on = DevEnv("XBE_TRACE_CITY") != nullptr;
    return on;
}

uint64_t PhysicalUsed();
uint64_t PhysicalFree();

// Phase 1 call trace: counts the selected outside-streamer functions and records the first caller. One line
// per order of magnitude keeps the log short while showing whether a pass runs per frame, per recentre, or
// only on a specific game path.
struct TraceEntry {
    uint32_t addr;
    std::atomic<uint32_t> count{0}, first{0}, bucket{0};
};

TraceEntry g_trace[] = {
    {TC_ADDR(0x146CA0)}, {TC_ADDR(0x146CC8)}, {TC_ADDR(0x146D66)}, {TC_ADDR(0x146FC0)}, {TC_ADDR(0x146FE8)}, {TC_ADDR(0x1472E0)}, {TC_ADDR(0x147308)},
    {TC_ADDR(0x137600)}, {TC_ADDR(0x13D750)}, {TC_ADDR(0x13D350)},
    {TC_ADDR(0x13A1B0)}, {TC_ADDR(0x13A480)}, {TC_ADDR(0x13FBE0)}, {TC_ADDR(0x1400DA)}, {TC_ADDR(0x145350)}, {TC_ADDR(0x145AC6)},
    {TC_ADDR(0x13CF40)}, {TC_ADDR(0x13E7F0)}, {TC_ADDR(0x13E91D)}, {TC_ADDR(0x13EB08)}, {TC_ADDR(0x13F840)},
    {TC_ADDR(0x138B90)}, {TC_ADDR(0x13ABB0)}, {TC_ADDR(0x13A850)}, {TC_ADDR(0x1420A0)}, {TC_ADDR(0x14A240)},
    {TC_ADDR(0x13CB50)}, {TC_ADDR(0x144250)}, {TC_ADDR(0x14F460)}, {TC_ADDR(0x142C90)},
};

// Step 1 diagnostic: who holds which tile object, and do two cells ever hold the same one?
struct DiagCell {
    uint32_t addr = 0;
    int32_t x = 0, y = 0;
    uint32_t state = 0, loaded = 0, obj = 0;
    bool valid = false;
};
DiagCell g_diag[96];
struct DiagPair {
    uint32_t a = 0, b = 0;
    bool valid = false;
};
DiagPair g_diag_pairs[256];
uint64_t g_diag_same = 0, g_diag_diff = 0;
std::chrono::steady_clock::time_point g_diag_last;

const char *DiagRole(uint32_t object, uint32_t city, uint32_t manager) {
    if (object >= manager + 0x10 && object < manager + 0x10 + 9u * 0x8B0) return "game-slot";
    if (object >= city + 0x4FA4 && object < city + 0x4FA4 + 9u * 0x9C) return "game-cell";
    if (game::city_grid::Ready()) {
        const uint32_t count = uint32_t(game::city_grid::N()) * uint32_t(game::city_grid::N());
        const uint32_t slots = game::city_grid::Slots(), cells = game::city_grid::Cells();
        if (object >= slots && object < slots + count * 0x8B0) return "store-slot";
        if (object >= cells && object < cells + count * 0x9C) return "store-cell";
    }
    return "other";
}

void DiagTransition(uint8_t *base, uint32_t object, uint32_t city, uint32_t manager, int32_t target_x,
                    int32_t target_y, uint32_t caller) {
    const char *role = DiagRole(object, city, manager);
    if (role[0] == 'o') return;
    const bool cell = std::strcmp(role, "game-cell") == 0 || std::strcmp(role, "store-cell") == 0;
    const int32_t x = int32_t(X86_MEM32(object + 0x04)), y = int32_t(X86_MEM32(object + 0x08));
    const uint32_t state = X86_MEM32(object + 0x24);
    const uint32_t loaded = cell ? (X86_MEM8(object + 0x88) ? 1u : 0u) : X86_MEM32(object + 0x50);
    const uint32_t obj = cell ? X86_MEM32(object + 0x80) : X86_MEM32(object + 0x30);

    DiagCell *slot = nullptr, *free_slot = nullptr;
    for (DiagCell &e : g_diag) {
        if (e.valid && e.addr == object) {
            slot = &e;
            break;
        }
        if (!e.valid && !free_slot) free_slot = &e;
    }
    if (!slot) slot = free_slot;
    if (!slot) return;
    const bool changed = !slot->valid || slot->x != x || slot->y != y || slot->state != state ||
                         slot->loaded != loaded || slot->obj != obj;
    if (!changed) return;
    LOGR("diag: %s %08X tile %d,%d target %d,%d state %u loaded %u obj %08X caller %08X", role, object, x, y,
         target_x, target_y, state, loaded, obj, caller);
    *slot = {object, x, y, state, loaded, obj, true};
}

bool DiagNewPair(uint32_t a, uint32_t b) {
    for (DiagPair &p : g_diag_pairs) {
        if (p.valid && p.a == a && p.b == b) return false;
        if (!p.valid && !p.a) {
            p = {a, b, true};
            return true;
        }
    }
    return false;
}

void DiagDuplicates(uint8_t *base, uint32_t city) {
    const auto now = std::chrono::steady_clock::now();
    if (now - g_diag_last < std::chrono::milliseconds(500)) return;
    g_diag_last = now;

    uint32_t addrs[64], objs[64], groups[64];
    int32_t xs[64], ys[64];
    const char *roles[64];
    int n = 0;
    for (int i = 0; i < 9; ++i) {
        const uint32_t cell = city + 0x4FA4 + uint32_t(i) * 0x9C;
        const uint32_t obj = X86_MEM32(cell + 0x80);
        if (!obj) continue;
        addrs[n] = cell;
        objs[n] = obj;
        groups[n] = X86_MEM32(cell + 0x38);
        xs[n] = int32_t(X86_MEM32(cell + 0x04));
        ys[n] = int32_t(X86_MEM32(cell + 0x08));
        roles[n] = "game";
        ++n;
    }
    if (game::city_grid::Ready()) {
        const int count = game::city_grid::N() * game::city_grid::N();
        const uint32_t cells = game::city_grid::Cells();
        for (int i = 0; i < count && n < 64; ++i) {
            if (!game::city_grid::RingCell(i)) continue;
            const uint32_t cell = cells + uint32_t(i) * 0x9C;
            const uint32_t obj = X86_MEM32(cell + 0x80);
            if (!obj) continue;
            addrs[n] = cell;
            objs[n] = obj;
            groups[n] = X86_MEM32(cell + 0x38);
            xs[n] = int32_t(X86_MEM32(cell + 0x04));
            ys[n] = int32_t(X86_MEM32(cell + 0x08));
            roles[n] = "store";
            ++n;
        }
    }
    for (int i = 0; i < n; ++i) {
        for (int j = i + 1; j < n; ++j) {
            if (objs[i] != objs[j] && groups[i] != groups[j]) continue;
            if (groups[i] == groups[j] && groups[i] && DiagNewPair(addrs[i] ^ 0x80000000u, addrs[j])) {
                LOGR("diag: SHARED group %08X %s %08X tile %d,%d and %s %08X tile %d,%d", groups[i], roles[i],
                     addrs[i], xs[i], ys[i], roles[j], addrs[j], xs[j], ys[j]);
            }
            if (objs[i] != objs[j]) continue;
            const bool same = xs[i] == xs[j] && ys[i] == ys[j];
            if (DiagNewPair(addrs[i], addrs[j])) {
                if (same) ++g_diag_same; else ++g_diag_diff;
                LOGR("diag: DUPLICATE obj %08X %s %08X tile %d,%d and %s %08X tile %d,%d same_tile %d",
                     objs[i], roles[i], addrs[i], xs[i], ys[i], roles[j], addrs[j], xs[j], ys[j], same);
            }
        }
    }

    static auto last_summary = now;
    if (now - last_summary >= std::chrono::seconds(5)) {
        last_summary = now;
        int loaded = 0;
        if (game::city_grid::Ready()) {
            const int count = game::city_grid::N() * game::city_grid::N();
            const uint32_t cells = game::city_grid::Cells();
            for (int i = 0; i < count; ++i)
                if (game::city_grid::RingCell(i) && X86_MEM8(cells + uint32_t(i) * 0x9C + 0x88)) ++loaded;
        }
        LOGR("diag: unique duplicate pairs same_tile %llu different_tile %llu, ring %d/%d loaded, physical %llu/%llu KB",
             static_cast<unsigned long long>(g_diag_same), static_cast<unsigned long long>(g_diag_diff),
             loaded, game::city_grid::Ready() ? game::city_grid::N() * game::city_grid::N() : 0,
             static_cast<unsigned long long>(PhysicalUsed() / 1024),
             static_cast<unsigned long long>(PhysicalFree() / 1024));
    }
}

// Effect-list diagnostic: 0x13EF50 walks the list whose head sits at [0x2E7C5C] and calls each node's
// [+0x0C]. Walk the same list safely (committed pages, 4-byte alignment) and report the first node that is
// not a valid list entry. Logging is edge-triggered so the list can be watched every call without flooding.
// Node guard: effect-list nodes are mostly static objects registered once by the initialisers at 0x2730xx
// (0x2EA780.., 0x386660, 0x3866E0), so their callbacks never legitimately change. Snapshot the static node
// blocks and every node's callback on first sight; report the first change with the bytes just before it,
// so the overflowing neighbour can be identified.
} // namespace
void NodeGuard(uint8_t *base, const char *tag) {
    static const bool enabled = DevEnv("XBE_CITY_NODEGUARD") != nullptr;
    if (!enabled) return;
    // Static nodes registered by the 0x2730xx initialisers; guard their arg (+8) and callback (+0xC).
    static const uint32_t nodes[] = {TC_ADDR(0x2EA780), TC_ADDR(0x2EA794), TC_ADDR(0x2EA7B8), TC_ADDR(0x2EA7CC), TC_ADDR(0x2EA7E0), TC_ADDR(0x2EA7F4), TC_ADDR(0x2EA808),
                                     TC_ADDR(0x2EA81C), TC_ADDR(0x2EA840), TC_ADDR(0x2EA854), TC_ADDR(0x2EA868), TC_ADDR(0x2EA87C), TC_ADDR(0x2EA890), TC_ADDR(0x2EA8A4),
                                     TC_ADDR(0x2EA8B8), TC_ADDR(0x2EA8CC), TC_ADDR(0x2EA8E0), TC_ADDR(0x2EA8F4), TC_ADDR(0x2EA908), TC_ADDR(0x2EA920), TC_ADDR(0x2EA934),
                                     TC_ADDR(0x2EA980), TC_ADDR(0x2EA9B8), TC_ADDR(0x2EA9CC), TC_ADDR(0x2EA9F0)};
    static std::vector<uint32_t> snap;
    static std::unordered_map<uint32_t, uint32_t> cbs;
    static bool armed = false, fired = false;
    if (fired) return;
    if (!armed) {
        for (uint32_t n : nodes) {
            snap.push_back(X86_MEM32(n + 8));
            snap.push_back(X86_MEM32(n + 0xC));
        }
        armed = true;
        LOGR("nodeguard armed (%s)", tag);
        return;
    }
    size_t i = 0;
    for (uint32_t n : nodes) {
        for (uint32_t a = n + 8; a <= n + 0xC; a += 4, ++i) {
            const uint32_t now = X86_MEM32(a), was = snap[i];
            if (now == was) continue;
            fired = true;
            LOGR("nodeguard %s STATIC %08X: %08X -> %08X", tag, a, was, now);
            for (uint32_t d = (a - 0x40u) & ~3u; d < a + 0x10u; d += 0x10u)
                LOGR("nodeguard   %08X: %08X %08X %08X %08X", d, X86_MEM32(d), X86_MEM32(d + 4), X86_MEM32(d + 8),
                     X86_MEM32(d + 12));
            return;
        }
    }
    const uint32_t sentinel = X86_MEM32(TC_ADDR(0x2E7C5Cu));
    if (sentinel < 0x10000u || (sentinel & 3u) || !mem::IsCommitted(sentinel)) return;
    uint32_t node = X86_MEM32(sentinel);
    for (int n = 0; node && n < 512; ++n) {
        if (node < 0x10000u || (node & 3u) || !mem::IsCommitted(node + 0x10u)) {
            fired = true;
            LOGR("nodeguard %s WILD next %08X at #%d", tag, node, n);
            return;
        }
        const uint32_t cb = X86_MEM32(node + 0x0C);
        auto [it, inserted] = cbs.emplace(node, cb);
        if (!inserted && it->second != cb) {
            fired = true;
            LOGR("nodeguard %s NODE %08X cb %08X -> %08X (arg %08X key %08X)", tag, node, it->second, cb,
                 X86_MEM32(node + 8), X86_MEM32(node + 0x10));
            for (uint32_t d = (node - 0x40u) & ~3u; d < node + 0x20u; d += 0x10u)
                LOGR("nodeguard   %08X: %08X %08X %08X %08X", d, X86_MEM32(d), X86_MEM32(d + 4), X86_MEM32(d + 8),
                     X86_MEM32(d + 12));
            return;
        }
        node = X86_MEM32(node);
    }
}

namespace {
void DiagEffectLists(uint8_t *base, const char *tag, bool always) {
    NodeGuard(base, tag);
    static const bool enabled = DevEnv("XBE_CITY_RING_LISTDIAG") != nullptr;
    if (!enabled) return;
    const auto now = std::chrono::steady_clock::now();
    static auto last = now;
    static int last_count = -1, last_bad = -1;
    const uint32_t sentinel = X86_MEM32(TC_ADDR(0x2E7C5Cu));
    uint32_t node = 0, prev = 0, bad_node = 0, bad_cb = 0, bad_prev = 0;
    int count = 0, bad = 0;
    bool corrupt = false;
    if (sentinel < 0x10000u || sentinel >= 0x7FFE0000u || (sentinel & 3u) || !mem::IsCommitted(sentinel)) {
        corrupt = true;
        bad_node = sentinel;
    } else {
        node = X86_MEM32(sentinel);
        prev = sentinel;
    }
    while (!corrupt && node && count < 512) {
        if (node < 0x10000u || node >= 0x7FFE0000u || (node & 3u) || !mem::IsCommitted(node + 0x10u)) {
            corrupt = true;
            bad_node = node;
            bad_prev = prev;
            break;
        }
        const uint32_t cb = X86_MEM32(node + 0x0C);
        if (cb != 0u && (cb < 0x1000u || cb >= TC_ADDR(0x00400000u))) {
            ++bad;
            if (!bad_node) {
                bad_node = node;
                bad_cb = cb;
                bad_prev = prev;
            }
        }
        prev = node;
        node = X86_MEM32(node);
        ++count;
    }
    if (corrupt || bad || count != last_count || always) {
        if (corrupt)
            LOGR("effwatch %s CORRUPT next %08X prev %08X after %d nodes", tag, bad_node, bad_prev, count);
        else if (bad)
            LOGR("effwatch %s BADCB node %08X cb %08X prev %08X after %d nodes", tag, bad_node, bad_cb,
                 bad_prev, count);
        else if (always || now - last >= std::chrono::seconds(5))
            LOGR("effwatch %s ok %d nodes", tag, count);
        last_count = count;
        last_bad = bad;
        last = now;
    }
}

// Draw-time guard removed: skipping a cell in 0x13C4C0 leaves the camera/fade state half-updated and causes
// worse failures later. Diagnostics stay read-only.

void TraceHit(int i, X86Context &ctx, uint8_t *base) {
    TraceEntry &e = g_trace[i];
    const uint32_t n = e.count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1) {
        e.first.store(X86_MEM32(ctx.esp), std::memory_order_relaxed);
        LOGR("city: trace %08X first from %08X", e.addr, X86_MEM32(ctx.esp));
    }
    const uint32_t bucket = n < 10 ? n : n < 100 ? 10 : n < 1000 ? 100 : n < 10000 ? 1000
                            : n < 100000 ? 10000 : n < 1000000 ? 100000 : 1000000;
    if (bucket != e.bucket.exchange(bucket, std::memory_order_relaxed))
        LOGR("city: trace %08X calls %u (first ret %08X)", e.addr, bucket,
             e.first.load(std::memory_order_relaxed));
}

struct Slot {
    int32_t x = 0, y = 0;
    uint32_t handle = 0, size = 0, loaded = 0;
};

Slot ReadSlot(uint8_t *base, uint32_t city, int i) {
    const uint32_t slot = city + kSlots + uint32_t(i) * kSlotStride;
    Slot s;
    s.x = int32_t(X86_MEM32(slot + kX));
    s.y = int32_t(X86_MEM32(slot + kY));
    s.handle = X86_MEM32(slot + kHandle);
    s.size = X86_MEM32(slot + kSize);
    s.loaded = X86_MEM8(slot + kLoaded) ? 1u : 0u;
    return s;
}

bool Same(const Slot &a, const Slot &b) {
    return a.x == b.x && a.y == b.y && a.handle == b.handle && a.size == b.size && a.loaded == b.loaded;
}

uint64_t PhysicalUsed() { return uint64_t(mem::Query().contiguous_used) * mem::kPage; }
uint64_t PhysicalFree() {
    const uint64_t usable = mem::kPhysicalSize - mem::kKernelDataSize;
    const uint64_t used = PhysicalUsed();
    return used >= usable ? 0 : usable - used;
}

struct State {
    Slot slots[kSlotCount];
    bool have = false;
    uint64_t changes = 0, loads = 0, unloads = 0;
    uint64_t peak_used = 0, low_free = 0;
    uint64_t last_used = 0;
    int loaded = 0;
    std::chrono::steady_clock::time_point checked, summary, changed;
};

State g_state;

// Simulates the physical cost of the outer ring: hold XBE_CITY_RESERVE_KB of the physical window from 45 s into
// the run, after the city is up, so the game's own streaming has to live with the same reduced headroom that a
// 5Ã—5 or 7Ã—7 grid would leave. Purely a measurement: the block is never used.
uint32_t g_reserved[64];
int g_reserved_n = 0;

void MaybeReserve() {
    static const uint32_t kb = [] {
        const char *value = DevEnv("XBE_CITY_RESERVE_KB");
        return value ? uint32_t(std::atoi(value)) : 0u;
    }();
    static const uint32_t blocks = [] {
        const char *value = DevEnv("XBE_CITY_RESERVE_BLOCKS");
        return value ? std::max(1u, uint32_t(std::atoi(value))) : 1u;
    }();
    static bool done = false;
    static const auto start = std::chrono::steady_clock::now();
    if (!kb || done) return;
    if (std::chrono::steady_clock::now() - start < std::chrono::seconds(45)) return;
    done = true;
    const uint32_t each = kb * 1024 / blocks;
    for (uint32_t i = 0; i < blocks && g_reserved_n < 64; ++i) {
        const uint32_t at = mem::AllocContiguous(each, 0, mem::kPhysicalSize - 1, mem::kPage);
        if (!at) {
            LOGR("city: outer-ring simulation could not reserve block %u/%u (%u KB); %llu KB physical in use", i + 1,
                 blocks, each / 1024, static_cast<unsigned long long>(PhysicalUsed() / 1024));
            break;
        }
        g_reserved[g_reserved_n++] = at;
    }
    LOGR("city: outer-ring simulation reserved %d/%u blocks, %u KB total; %llu KB physical in use", g_reserved_n,
         blocks, uint32_t(g_reserved_n) * each / 1024,
         static_cast<unsigned long long>(PhysicalUsed() / 1024));
}

void LogSlots(uint8_t *base, uint32_t city) {
    char text[512];
    int at = 0;
    for (int i = 0; i < kSlotCount && at < int(sizeof text); ++i) {
        const Slot s = ReadSlot(base, city, i);
        at += std::snprintf(text + at, sizeof text - size_t(at), "%s%d:(%d,%d)%s%08X", i ? " " : "",
                            i, s.x, s.y, s.loaded ? "L" : "-", s.handle);
    }
    LOGR("city: slots %s", text);
}

void Sample(uint8_t *base, bool force) {
    const uint32_t city = X86_MEM32(kCityPtr);
    if (!city) return;
    MaybeReserve();
    const auto now = std::chrono::steady_clock::now();
    if (!force && now - g_state.checked < std::chrono::milliseconds(50)) return;
    g_state.checked = now;

    const uint64_t used = PhysicalUsed();
    const uint64_t free = PhysicalFree();
    g_state.peak_used = used > g_state.peak_used ? used : g_state.peak_used;
    if (!g_state.low_free || free < g_state.low_free) g_state.low_free = free;

    Slot cur[kSlotCount];
    for (int i = 0; i < kSlotCount; ++i) cur[i] = ReadSlot(base, city, i);

    if (!g_state.have) {
        for (int i = 0; i < kSlotCount; ++i) g_state.slots[i] = cur[i];
        g_state.have = true;
        g_state.last_used = used;
        g_state.summary = now;
        LOGR("city: probe on, city %08X, physical %llu KB used, %llu KB free", city,
             static_cast<unsigned long long>(used / 1024), static_cast<unsigned long long>(free / 1024));
        LogSlots(base, city);
        return;
    }

    int loaded = 0;
    for (int i = 0; i < kSlotCount; ++i) loaded += cur[i].loaded ? 1 : 0;
    const int64_t delta = int64_t(used) - int64_t(g_state.last_used);
    const int64_t quiet_ms = g_state.changed.time_since_epoch().count()
                                 ? std::chrono::duration_cast<std::chrono::milliseconds>(now - g_state.changed).count()
                                 : -1;
    int transitions = 0;
    for (int i = 0; i < kSlotCount; ++i)
        if (!Same(g_state.slots[i], cur[i])) ++transitions;

    for (int i = 0; i < kSlotCount; ++i) {
        if (Same(g_state.slots[i], cur[i])) continue;
        const Slot &old = g_state.slots[i];
        if (cur[i].loaded && !old.loaded) ++g_state.loads;
        if (!cur[i].loaded && old.loaded) ++g_state.unloads;
        ++g_state.changes;
        LOGR("city: slot %d at %08X (%d,%d %s %08X size %u) -> (%d,%d %s %08X size %u); loaded %d -> %d, %d "
             "transitions in %lld ms, physical %llu KB (%+lld KB), free %llu KB",
             i, city + kSlots + uint32_t(i) * kSlotStride,
             old.x, old.y, old.loaded ? "loaded" : "empty", old.handle, old.size, cur[i].x, cur[i].y,
             cur[i].loaded ? "loaded" : "empty", cur[i].handle, cur[i].size, g_state.loaded, loaded, transitions,
             static_cast<long long>(quiet_ms), static_cast<unsigned long long>(used / 1024),
             static_cast<long long>(delta / 1024), static_cast<unsigned long long>(free / 1024));
    }
    g_state.last_used = used;
    if (transitions) g_state.changed = now;
    g_state.loaded = loaded;

    if (now - g_state.summary >= std::chrono::seconds(5)) {
        g_state.summary = now;
        const mem::Stats q = mem::Query();
        LOGR("city: 5 s: %llu changes (%llu loaded, %llu emptied), %d tiles loaded now, city %08X, physical "
             "%llu/%llu KB used, peak %llu KB, low free %llu KB, virtual %llu KB, system %llu KB",
             static_cast<unsigned long long>(g_state.changes), static_cast<unsigned long long>(g_state.loads),
             static_cast<unsigned long long>(g_state.unloads), loaded, city,
             static_cast<unsigned long long>(used / 1024),
             static_cast<unsigned long long>((mem::kPhysicalSize - mem::kKernelDataSize) / 1024),
             static_cast<unsigned long long>(g_state.peak_used / 1024),
             static_cast<unsigned long long>(g_state.low_free / 1024),
             static_cast<unsigned long long>(q.virtual_committed) * mem::kPage / 1024,
             static_cast<unsigned long long>(q.system_used) * mem::kPage / 1024);
        LogSlots(base, city);
    }

    for (int i = 0; i < kSlotCount; ++i) g_state.slots[i] = cur[i];
}

} // namespace

// 0x13D880: initialises the three nine-slot arrays. Sample once the slots exist.
X86_EXTERN_FUNC(__imp__sub_0013D880);
X86_FUNC(sub_0013D880) {
    const uint32_t manager = ctx.ecx;  // the tile manager's this; the original changes ecx
    __imp__sub_0013D880(ctx, base);
    game::city_grid::Init(base, manager);
    if (!Trace()) return;
    g_state = {};
    Sample(base, true);
}

// 0x1378F0 and 0x138070 build a tile record's .XB2 and .g names and issue its stream requests. ecx is the record.
void LogRequest(X86Context &ctx, uint8_t *base, const char *which, void (*original)(X86Context &, uint8_t *)) {
    const uint32_t tile = ctx.ecx;
    if (!tile) return original(ctx, base);
    const uint32_t ret = X86_MEM32(ctx.esp);
    const int32_t x = int32_t(X86_MEM32(tile + kX)), y = int32_t(X86_MEM32(tile + kY));
    const uint32_t size = X86_MEM32(tile + kSize);
    original(ctx, base);
    LOGR("city: %s from %08X tile %08X grid %d,%d size %u -> handle %08X", which, ret, tile, x, y, size,
         X86_MEM32(tile + kHandle));
}

X86_EXTERN_FUNC(__imp__sub_001378F0);
X86_FUNC(sub_001378F0) {
    if (!Trace()) return __imp__sub_001378F0(ctx, base);
    LogRequest(ctx, base, "XB2", __imp__sub_001378F0);
}

X86_EXTERN_FUNC(__imp__sub_00138070);
X86_FUNC(sub_00138070) {
    if (!Trace()) return __imp__sub_00138070(ctx, base);
    LogRequest(ctx, base, ".g", __imp__sub_00138070);
}

// 0x142CE0 -> 0x13F090: the city draw pass. Run the ring driver here, once per pass, before any cell is
// drawn. Driving from inside 0x13C4C0 (per cell) can leave a frame half-drawn when a tile loads or unloads.
// First argument (render context) the game passed to 0x13C4C0 in the current city draw pass. The ring must
// draw with exactly this: 0x142CE0's ecx is not it, and on some passes points at a small static object.
uint32_t g_game_cell_ctx = 0;

// Move the 16-entry light table (0x349380, count 0x3493CC) to a 256-entry guest array. grid_patch routes all
// nine table sites through xbe_light_table (exported by the game code DLL); lights are addressed by index
// only, so copying the entries and switching the base on the game thread is safe.
// Sector object draw radius (grid_patch kAppendSites): 0x13E7F0 skips an object beyond its own radius; the
// exported xbe_draw_radius_scale multiplies that radius. XBE_DRAW_RADIUS=<scale> for testing.
// XBE_CITY_PORTAL_OWNERS: grid_patch counts the slots whose sector tables the portal pass reads (0x13EB1B: game slot
// vs ring store slot); log and reset every 5 s.
// XBE_CITY_RING_PORTALS: hand the loaded ring slots to the portal pass (grid_patch: past the game's nine slots its
// loop continues with xbe_extra_slots, each a slot + 0x1C), so ring tiles are walked and drawn in full detail.
void FillRingPortalSlots() {
    static const bool on = DevEnv("XBE_CITY_RING_PORTALS") != nullptr;
    if (!on) return;
    static uint32_t *count = nullptr, *slots = nullptr;
    static bool looked = false;
    if (!looked) {
        looked = true;
        count = game::GuestExport<uint32_t>("xbe_extra_slot_count");
        slots = game::GuestExport<uint32_t>("xbe_extra_slots");
        LOGR("ring portals: %s", count && slots ? "ring slots join the portal pass" : "exports missing");
    }
    if (!count || !slots) return;
    uint32_t n = 0;
    if (game::city_grid::Ring() && game::city_grid::Ready()) {
        uint8_t *base = mem::Base();
        const int cells = game::city_grid::N() * game::city_grid::N();
        for (int i = 0; i < cells && n < 128; ++i) {
            if (!game::city_grid::RingCell(i)) continue;
            const uint32_t slot = game::city_grid::Slots() + uint32_t(i) * 0x8B0u;
            if (X86_MEM32(slot + 0x50) && X86_MEM32(slot + 0x1C)) slots[n++] = slot + 0x1C;
        }
    }
    {
        static auto last = std::chrono::steady_clock::now();
        if (std::chrono::steady_clock::now() - last > std::chrono::seconds(5) && game::city_grid::Ready()) {
            last = std::chrono::steady_clock::now();
            uint8_t *base = mem::Base();
            const int cells = game::city_grid::N() * game::city_grid::N();
            int ring = 0, loaded = 0;
            uint32_t sample = 0;
            for (int i = 0; i < cells; ++i) {
                if (!game::city_grid::RingCell(i)) continue;
                ++ring;
                const uint32_t s = game::city_grid::Slots() + uint32_t(i) * 0x8B0u;
                if (X86_MEM32(s + 0x50)) { ++loaded; if (!sample) sample = s; }
            }
            if (sample)
                LOGR("ring portals: %u listed; ring cells %d, slots with root %d; sample slot %08X +0C %08X +14 %08X +1C %08X +50 %08X",
                     n, ring, loaded, sample, X86_MEM32(sample + 0x0C), X86_MEM32(sample + 0x14), X86_MEM32(sample + 0x1C), X86_MEM32(sample + 0x50));
            else
                LOGR("ring portals: %u listed; ring cells %d, none with a root", n, ring);
        }
    }
    *count = n;
}

void LogPortalSlots() {
    static const bool on = DevEnv("XBE_CITY_PORTAL_OWNERS") != nullptr;
    if (!on) return;
    static auto last = std::chrono::steady_clock::now();
    if (std::chrono::steady_clock::now() - last < std::chrono::seconds(5)) return;
    last = std::chrono::steady_clock::now();
    static uint32_t *hits = game::GuestExport<uint32_t>("xbe_portal_slot_hits");
    if (!hits) return;
    LOGR("portal slots: game %u, ring %u (per 5 s)", hits[0], hits[1]);
    hits[0] = hits[1] = 0;
}

void ApplyDrawRadius() {
    static bool done = false;
    if (done) return;
    done = true;
    const char *e = DevEnv("XBE_DRAW_RADIUS");
    if (!e) return;
    auto *scale = game::GuestExport<float>("xbe_draw_radius_scale");
    if (!scale) {
        LOGR("draw radius: xbe_draw_radius_scale not exported");
        return;
    }
    *scale = float(std::atof(e));
    LOGR("draw radius: sector objects drawn to %.2fx their radius", *scale);
}

struct CityInstanceDistances {
    float inner = 1.0f, outer = 0.0f;
} g_instance_distances;
bool g_instance_table_ready = false;
float *g_ring_instance_cutoff = nullptr;

void ApplyCityInstanceDistances() {
    static const auto exports = std::array{game::GuestExport<float>("xbe_inner_instance_scale"),
                                           game::GuestExport<float>("xbe_ring_instance_scale")};
    if (!exports[0] || !exports[1]) return;
    g_ring_instance_cutoff = exports[1];

    // Keep the old environment-driven configuration for diagnostic runs.
    static const CityInstanceDistances override = [] {
        if (!DevEnv("XBE_CITY_RING_INSTANCES")) return CityInstanceDistances{0.0f, 0.0f};
        auto read = [](const char *name, float fallback) {
            const char *value = DevEnv(name);
            if (!value) return fallback;
            char *end;
            const float requested = std::strtof(value, &end);
            return end != value && *end == '\0' && requested >= 1.0f && requested <= 8.0f ? requested : fallback;
        };
        return CityInstanceDistances{read("XBE_CITY_INNER_INSTANCE_DISTANCE", 8.0f),
                                     read("XBE_CITY_RING_INSTANCE_DISTANCE", 4.0f)};
    }();
    auto &settings = app::Get();
    const auto distances = override.inner != 0.0f ? override : CityInstanceDistances{
        float(std::atomic_ref(settings.city_inner_detail).load(std::memory_order_relaxed)),
        float(std::atomic_ref(settings.city_outer_detail).load(std::memory_order_relaxed))};
    if (distances.inner == g_instance_distances.inner && distances.outer == g_instance_distances.outer) return;
    *exports[0] = distances.inner;
    *exports[1] = distances.outer;
    g_instance_distances = distances;
    LOGR("city instances: distance scale %.2fx inner, %.2fx ring", distances.inner, distances.outer);
}

// Move the 300-entry visible instance list (city + 0x5AD0, see grid_patch.h kInstSites) to a 4096-entry guest
// array so the outer ring can add its tiles. The list is rebuilt every frame, so switching at the city draw
// entry is safe; the entries already there are copied for that frame.
uint32_t g_inst_entries = 0;  // relocated list: entry i at g_inst_entries + i * 8 (city + 0x5AD4 before)
void RelocateInstTable(uint32_t city, bool enabled) {
    static uint32_t *count_ref = nullptr;
    if (count_ref) {  // insert/evict reach the count through the list base (XBE_CNT); keep it on this city
        *count_ref = city + 0x6430;
        return;
    }
    static bool done = false;
    if (done || !enabled) return;
    done = true;
    auto *table = game::GuestExport<uint32_t>("xbe_inst_table");
    auto *cap = game::GuestExport<uint32_t>("xbe_inst_cap");
    auto *count = game::GuestExport<uint32_t>("xbe_inst_count");
    if (!table || !cap || !count) {
        LOGR("instances: xbe_inst_table not exported; list stays at 300");
        return;
    }
    constexpr uint32_t kEntries = 4096;
    uint32_t at = 0, size = kEntries * 8;
    if (mem::AllocateVirtual(at, size, mem::kMemReserve | mem::kMemCommit | mem::kMemTopDown, 0x04) != 0 || !at) {
        LOGR("instances: could not allocate the larger list");
        return;
    }
    uint8_t *base = mem::Base();
    std::memset(base + at, 0, size);
    std::memcpy(base + at, base + city + 0x5AD0, 300 * 8);
    *count = city + 0x6430;
    count_ref = count;
    *table = at;
    g_inst_entries = at + 4;
    *cap = kEntries;
    g_instance_table_ready = true;
    LOGR("instances: visible list moved to %08X (%u entries)", at, kEntries);
}

void RelocateLightTable() {
    static bool done = false;
    if (done) return;
    done = true;
    auto *table = game::GuestExport<uint32_t>("xbe_light_table");
    if (!table) {
        LOGR("lights: xbe_light_table not exported; table stays at 16");
        return;
    }
    uint32_t at = 0, size = 0x1000;
    if (mem::AllocateVirtual(at, size, mem::kMemReserve | mem::kMemCommit | mem::kMemTopDown, 0x04) != 0 || !at) {
        LOGR("lights: could not allocate the larger table");
        return;
    }
    uint8_t *base = mem::Base();
    std::memset(base + at, 0, size);
    for (uint32_t i = 0; i < 16; ++i) X86_MEM32(at + 4 + i * 4) = X86_MEM32(TC_ADDR(0x349380u) + i * 4);
    *table = at + 4;  // keep one spare word below for the [base-4] read
    LOGR("lights: table moved to %08X (1023 entries)", at + 4);
}
extern int g_ring_draw_idx;
X86_EXTERN_FUNC(__imp__sub_00142CE0);
X86_FUNC(sub_00142CE0) {
    static const bool diag = DevEnv("XBE_CITY_DIAG") != nullptr;
    const uint32_t city = X86_MEM32(kCityPtr);
    if (city && game::city_grid::Ring()) {
        if (!game::city_grid::Ready()) game::city_grid::Init(base, city + 0xD0);
        game::city_grid::DriveRing(base, ctx, city, city + 0xD0);
        if (diag) DiagDuplicates(base, city);
        DiagEffectLists(base, "drive", false);
    }
    if (city && game::city_grid::Ring()) RelocateLightTable();
    if (city) RelocateInstTable(city, game::city_grid::Ring() || g_instance_distances.inner > 1.0f);
    ApplyDrawRadius();
    LogPortalSlots();
    FillRingPortalSlots();
    { void InstallTileDrawProbe(); InstallTileDrawProbe(); }
    static const bool drawprobe = DevEnv("XBE_CITY_DRAWPROBE") != nullptr;
    if (drawprobe && city && game::city_grid::Ready()) {
        // Scene list at city + 0x6B48 (walked by 0x144250 -> 0x13F400): who owns its nodes?
        static auto last = std::chrono::steady_clock::now();
        if (std::chrono::steady_clock::now() - last > std::chrono::seconds(5)) {
            last = std::chrono::steady_clock::now();
            auto owner = [&](uint32_t node) -> int {
                for (uint32_t i = 0; i < 9; ++i) {
                    const uint32_t b = X86_MEM32(city + 0xE0 + i * 0x8B0 + 0x30);
                    if (b && node >= b && node < b + 0x400000u) return 0;
                }
                const int n = game::city_grid::N();
                for (int i = 0; i < n * n; ++i) {
                    const uint32_t b = X86_MEM32(game::city_grid::Slots() + uint32_t(i) * 0x8B0u + 0x30);
                    if (b && node >= b && node < b + 0x400000u) return 1;
                }
                return 2;
            };
            int counts[3] = {}, total = 0;
            const uint32_t head = X86_MEM32(city + 0x6B48);
            for (uint32_t e = head; e && total < 100000; e = X86_MEM32(e), ++total) {
                if (total && e == head) break;
                if (!mem::IsCommitted(e + 8)) break;
                counts[owner(X86_MEM32(e + 8))]++;
            }
            {
                static uint32_t peak = 0;
                const uint32_t cnt = X86_MEM32(city + 0x6430);
                peak = cnt > peak ? cnt : peak;
                LOGR("drawprobe: visible instances %u (peak %u of 300)", cnt, peak);
            }
            LOGR("drawprobe: scene list %d nodes: game slots %d, ring slots %d, other %d", total, counts[0], counts[1], counts[2]);
        }
    }
    { extern bool g_ring_instances_done; g_ring_instances_done = false; }
    g_game_cell_ctx = 0;
    __imp__sub_00142CE0(ctx, base);
    {   // Light table watch: count at 0x3493CC, count at 0x3493CC (no bounds check on add).
        static uint32_t high = 0;
        const uint32_t cnt = X86_MEM32(TC_ADDR(0x3493CCu));
        if (cnt > high && cnt < 0x1000u) {
            high = cnt;
            LOGR("lights high-water %u", cnt);
        }
    }
    NodeGuard(base, "gamedraw");
    static const bool alternate = DevEnv("XBE_CITY_RING_ALTERNATE") != nullptr;  // A/B captures
    static uint32_t passes = 0;
    const bool skip_alt = alternate && (++passes & 1u);
    if (alternate && (passes % 600u) < 2u) LOGR("ring alternate: pass %u ring %s", passes, skip_alt ? "off" : "on");
    // The ring normally draws inside the distant pass (0x13F1B0 hook below), with the game's own render states.
    // XBE_CITY_RING_DRAW_LATE=1 keeps the old draw here, after the pass (states already reset: see-through).
    static const bool late = DevEnv("XBE_CITY_RING_DRAW_LATE") != nullptr;
    if (late && city && game::city_grid::Ring() && !skip_alt && DevEnv("XBE_CITY_RING_NODRAW") == nullptr)
        if (g_game_cell_ctx) game::city_grid::DrawRing(base, ctx, g_game_cell_ctx);
    if (city && game::city_grid::Ring()) DiagEffectLists(base, "drawdone", false);
}

// 0x14B390: the scene submit pass; it calls the effect manager 0x13EF50 at 0x14B5AE. Watch the list at its
// entry so a node corrupted earlier in the pass is reported before the manager dereferences it.
X86_EXTERN_FUNC(__imp__sub_0014B390);
X86_FUNC(sub_0014B390) {
    DiagEffectLists(base, "submit", false);
    __imp__sub_0014B390(ctx, base);
}

// 0x13EF50: the effect manager update. Walk the list immediately before it runs.
X86_EXTERN_FUNC(__imp__sub_0013EF50);
X86_FUNC(sub_0013EF50) {
    DiagEffectLists(base, "update", false);
    __imp__sub_0013EF50(ctx, base);
}

// 0x13C4C0: one draw cell, called nine times a frame by 0x13F1B0. Use it as a cheap frame tick.
X86_EXTERN_FUNC(__imp__sub_0013C4C0);
X86_FUNC(sub_0013C4C0) {
    if (g_ring_draw_idx < 0) g_game_cell_ctx = X86_MEM32(ctx.esp + 4);
    static thread_local int index = 0;
    static thread_local auto last_call = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - last_call > std::chrono::milliseconds(100)) index = 0;
    last_call = now;

    if (index == 0) {
        if (!game::city_grid::Ready()) {
            const uint32_t city = X86_MEM32(kCityPtr);
            if (city) game::city_grid::Init(base, city + 0xD0);
        }
        // Read-only store housekeeping and the gated single-cell test, independent of the ring.
        game::city_grid::MaybeSync(base);
        const uint32_t city = X86_MEM32(kCityPtr);
        if (city) game::city_grid::TestOuter(base, ctx, city, city + 0xD0);
        // DrawRing is not called: the full ring crashes over longer runs (shared objects).
        if (Trace()) Sample(base, false);
    }
    if (Trace()) {
        static int cells = 0;
        if (cells < 27) {
            ++cells;
            const uint32_t cell = ctx.ecx;
            LOGR("city: draw cell %08X xy %d,%d loaded %u node %u obj %08X", cell,
                 int32_t(X86_MEM32(cell + 0x04)), int32_t(X86_MEM32(cell + 0x08)), X86_MEM8(cell + 0x88),
                 X86_MEM32(cell + 0x90), X86_MEM32(cell + 0x80));
        }
    }
    __imp__sub_0013C4C0(ctx, base);
    if (++index >= 9) index = 0;
}

// Phase 1 trace hooks. Each counts calls and records the first caller; the original body is untouched.
#if TCNYC_DEV_TOOLS
#define TRACE_HOOK(addr, index)                                                                       \
    X86_EXTERN_FUNC(__imp__sub_##addr);                                                               \
    X86_FUNC(sub_##addr) {                                                                            \
        if (Trace()) TraceHit(index, ctx, base);                                                      \
        __imp__sub_##addr(ctx, base);                                                                 \
    }
#else
#define TRACE_HOOK(addr, index)  // trace hooks are developer-build only
#endif

TRACE_HOOK(00146CA0, 0)
TRACE_HOOK(00146CC8, 1)
TRACE_HOOK(00146D66, 2)
TRACE_HOOK(00146FC0, 3)
TRACE_HOOK(00146FE8, 4)
TRACE_HOOK(001472E0, 5)
TRACE_HOOK(00147308, 6)

TRACE_HOOK(0013D750, 8)
TRACE_HOOK(0013D350, 9)
TRACE_HOOK(0013A1B0, 10)
TRACE_HOOK(0013A480, 11)
TRACE_HOOK(0013FBE0, 12)
TRACE_HOOK(001400DA, 13)
TRACE_HOOK(00145350, 14)
TRACE_HOOK(00145AC6, 15)
TRACE_HOOK(0013CF40, 16)
// 0x13E7F0 (trace index 17) is hooked below with the portal trace.
TRACE_HOOK(0013E91D, 18)
TRACE_HOOK(0013EB08, 19)
#if defined(TCNYC_PAL) && TCNYC_PAL
// The PAL body for this hook is the one the USA 0x13F840 function became.
TRACE_HOOK(001400D0, 20)
#elif TCNYC_DEV_TOOLS
void SignPassCensus(uint8_t *base);
X86_EXTERN_FUNC(__imp__sub_0013F840);
X86_FUNC(sub_0013F840) {
    if (Trace()) TraceHit(20, ctx, base);
    SignPassCensus(base);
    __imp__sub_0013F840(ctx, base);
}
#endif
TRACE_HOOK(00138B90, 21)
TRACE_HOOK(0013ABB0, 22)
TRACE_HOOK(0013A850, 23)
TRACE_HOOK(001420A0, 24)
TRACE_HOOK(0014A240, 25)
TRACE_HOOK(0013CB50, 26)
TRACE_HOOK(00144250, 27)
TRACE_HOOK(0014F460, 28)
TRACE_HOOK(00142C90, 29)

// Phase 3 probe: the per-slot state machine's argument order.
X86_EXTERN_FUNC(__imp__sub_00137600);
X86_FUNC(sub_00137600) {
    static const bool freeze_game = DevEnv("XBE_CITY_FREEZE_GAME") != nullptr;
    static const auto freeze_start = std::chrono::steady_clock::now();
    const uint32_t object = ctx.ecx;
    if (freeze_game && std::chrono::steady_clock::now() - freeze_start > std::chrono::seconds(5)) {
        const uint32_t city = X86_MEM32(kCityPtr);
        if (city) {
            const uint32_t manager = city + 0xD0;
            const bool game_slot = object >= manager + 0x10 && object < manager + 0x10 + 9u * 0x8B0;
            const bool game_cell = object >= city + 0x4FA4 && object < city + 0x4FA4 + 9u * 0x9C;
            if (game_slot || game_cell) {
                ctx.eax = 0;
                ctx.esp += 16;  // return address + three arguments (ret 0xC)
                return;
            }
        }
    }
    // One owner per tile: when a game cell changes, drop any ring cell for the same tile before the game
    // unloads or reuses the object. XBE_CITY_RING_NOUNLOAD disables it for debugging.
    static const bool ring_unload = DevEnv("XBE_CITY_RING_NOUNLOAD") == nullptr;
    if (ring_unload && !freeze_game) {
        const uint32_t city = X86_MEM32(kCityPtr);
        if (city) {
            const uint32_t manager = city + 0xD0;
            const bool game_slot = object >= manager + 0x10 && object < manager + 0x10 + 9u * 0x8B0;
            const bool game_cell = object >= city + 0x4FA4 && object < city + 0x4FA4 + 9u * 0x9C;
            if (game_slot || game_cell) {
                const int32_t nx = int32_t(X86_MEM32(ctx.esp + 8)), ny = int32_t(X86_MEM32(ctx.esp + 0xC));
                const int32_t ox = int32_t(X86_MEM32(object + 4)), oy = int32_t(X86_MEM32(object + 8));
                if (nx != ox || ny != oy) game::city_grid::OnGameMove(base, ctx, city, object, nx, ny);
            }
        }
    }
    static const bool diag = DevEnv("XBE_CITY_DIAG") != nullptr;
    if (diag) {
        const uint32_t city = X86_MEM32(kCityPtr);
        if (city)
            DiagTransition(base, object, city, city + 0xD0, int32_t(X86_MEM32(ctx.esp + 8)),
                           int32_t(X86_MEM32(ctx.esp + 0xC)), X86_MEM32(ctx.esp));
    }

    const uint32_t grid_slots = game::city_grid::Slots(), grid_cells = game::city_grid::Cells();
    const uint32_t grid_count = game::city_grid::Ready()
                                    ? uint32_t(game::city_grid::N()) * uint32_t(game::city_grid::N())
                                    : 0;
    if (Trace() && grid_count && object >= grid_slots && object < grid_slots + grid_count * 0x8B0)
        LOGR("city: state store slot %08X xy %d,%d state %u flag %u args %08X %08X %08X", object,
             int32_t(X86_MEM32(object + 4)), int32_t(X86_MEM32(object + 8)), X86_MEM32(object + 0x24),
             X86_MEM32(object + 0x2C), X86_MEM32(ctx.esp + 4), X86_MEM32(ctx.esp + 8),
             X86_MEM32(ctx.esp + 0xC));
    if (Trace() && grid_count && object >= grid_cells && object < grid_cells + grid_count * 0x9C)
        LOGR("city: state store cell %08X xy %d,%d state %u flag %u args %08X %08X %08X", object,
             int32_t(X86_MEM32(object + 4)), int32_t(X86_MEM32(object + 8)), X86_MEM32(object + 0x24),
             X86_MEM32(object + 0x2C), X86_MEM32(ctx.esp + 4), X86_MEM32(ctx.esp + 8),
             X86_MEM32(ctx.esp + 0xC));
    if (Trace()) {
        static int n = 0;
        if (n < 12) {
            ++n;
            const uint32_t slot = ctx.ecx;
            LOGR("city: state slot %08X slotxy %d,%d args %08X %08X %08X", slot,
                 int32_t(X86_MEM32(slot + 4)), int32_t(X86_MEM32(slot + 8)), X86_MEM32(ctx.esp + 4),
                 X86_MEM32(ctx.esp + 8), X86_MEM32(ctx.esp + 0xC));
        }
        TraceHit(7, ctx, base);
    }
    __imp__sub_00137600(ctx, base);
}

// Effect-list insert. Log inserts made while a ring cell is being drawn (DrawRing sets g_ring_draw_idx).
int g_ring_draw_idx = -1;
#if TCNYC_DEV_TOOLS  // developer traces: XBE_CITY_NODEGUARD, XBE_TRACE_HEAP, XBE_TRACE_RELEASE, XBE_CITY_SLOTDRAW_TRACE
X86_EXTERN_FUNC(__imp__sub_0013EFB0);
X86_FUNC(sub_0013EFB0) {
    static int logged = 0;
    static const bool node_guard = DevEnv("XBE_CITY_NODEGUARD") != nullptr;
    if (node_guard && g_ring_draw_idx >= 0 && logged < 40) {
        ++logged;
        LOGR("effinsert ring %d node %08X linked %08X cb %08X key %08X ret %08X", g_ring_draw_idx, ctx.ecx,
             X86_MEM32(ctx.ecx), X86_MEM32(ctx.ecx + 0xC), X86_MEM32(ctx.ecx + 0x10), X86_MEM32(ctx.esp));
    }
    __imp__sub_0013EFB0(ctx, base);
}
X86_EXTERN_FUNC(__imp__sub_0013B460);
X86_FUNC(sub_0013B460) {
    const uint32_t arg0 = X86_MEM32(ctx.esp + 4), self = ctx.ecx, ret = X86_MEM32(ctx.esp);
    __imp__sub_0013B460(ctx, base);
    if (g_ring_draw_idx >= 0) {
        char tag[96];
        std::snprintf(tag, sizeof tag, "after 0013B460 ring %d ecx %08X arg0 %08X ret %08X", g_ring_draw_idx, self, arg0, ret);
        NodeGuard(base, tag);
    }
}
X86_EXTERN_FUNC(__imp__sub_0021C6D0);
X86_FUNC(sub_0021C6D0) {
    const uint32_t arg0 = X86_MEM32(ctx.esp + 4), self = ctx.ecx, ret = X86_MEM32(ctx.esp);
    __imp__sub_0021C6D0(ctx, base);
    if (g_ring_draw_idx >= 0) {
        char tag[96];
        std::snprintf(tag, sizeof tag, "after 0021C6D0 ring %d ecx %08X arg0 %08X ret %08X", g_ring_draw_idx, self, arg0, ret);
        NodeGuard(base, tag);
    }
}
X86_EXTERN_FUNC(__imp__sub_0024C6E0);
X86_FUNC(sub_0024C6E0) {
    const uint32_t arg0 = X86_MEM32(ctx.esp + 4), self = ctx.ecx, ret = X86_MEM32(ctx.esp);
    // the recompiled body ran every SSE op as a four-lane scalar loop: this is the same sequence natively
    game::MatrixMul4x4(ctx, base);
    if (g_ring_draw_idx >= 0) {
        char tag[96];
        std::snprintf(tag, sizeof tag, "after 0024C6E0 ring %d ecx %08X arg0 %08X ret %08X", g_ring_draw_idx, self, arg0, ret);
        NodeGuard(base, tag);
    }
}
X86_EXTERN_FUNC(__imp__sub_0024CEC0);
X86_FUNC(sub_0024CEC0) {
    const uint32_t arg0 = X86_MEM32(ctx.esp + 4), self = ctx.ecx, ret = X86_MEM32(ctx.esp);
    __imp__sub_0024CEC0(ctx, base);
    if (g_ring_draw_idx >= 0) {
        char tag[96];
        std::snprintf(tag, sizeof tag, "after 0024CEC0 ring %d ecx %08X arg0 %08X ret %08X", g_ring_draw_idx, self, arg0, ret);
        NodeGuard(base, tag);
    }
}
X86_EXTERN_FUNC(__imp__sub_00245EF0);
X86_FUNC(sub_00245EF0) {
    if (ctx.ecx >= 0x2E0000u && ctx.ecx < 0x390000u) {
        static int n = 0;
        if (n++ < 4)
            LOGR("badthis 245EF0 ecx %08X ret %08X ring %d esi %08X edi %08X ebx %08X ebp %08X [esp+4] %08X", ctx.ecx,
                 X86_MEM32(ctx.esp), g_ring_draw_idx, ctx.esi, ctx.edi, ctx.ebx, ctx.ebp, X86_MEM32(ctx.esp + 4));
    }
    __imp__sub_00245EF0(ctx, base);
}

// XBE_TRACE_HEAP: pair the release 0x21FC00 with the game heap's own free. 0x268B0F is operator new, 0x268A85 is
// free (block header at ptr-4). Every block remembers the caller chain (ebp frames) that allocated and freed it;
// when 0x21FC00 is handed an object inside a freed block, both chains are logged before the game touches it.
namespace heaptrace {
struct Rec {
    uint32_t size = 0;
    uint32_t alloc[5] = {};
    uint32_t freed[5] = {};
    bool live = true;
};
std::map<uint32_t, Rec> g_blocks;
std::map<uint32_t, std::array<uint32_t, 5>> *g_released = nullptr;
std::mutex g_lock;
bool On() {
    static const bool on = DevEnv("XBE_TRACE_HEAP") != nullptr;
    return on;
}
void Chain(uint8_t *base, const X86Context &ctx, uint32_t out[5]) {
    out[0] = X86_MEM32(ctx.esp);
    uint32_t ebp = ctx.ebp;
    for (int i = 1; i < 5; ++i) {
        if (ebp < 0x10000u || (ebp & 3u) || !mem::IsCommitted(ebp) || !mem::IsCommitted(ebp + 7)) break;
        out[i] = X86_MEM32(ebp + 4);
        const uint32_t next = X86_MEM32(ebp);
        if (next <= ebp) break;
        ebp = next;
    }
}
void Log(const char *what, uint32_t at, const Rec &r) {
    LOGR("heaptrace %s block %08X size %X alloc %08X<%08X<%08X<%08X<%08X free %08X<%08X<%08X<%08X<%08X", what, at,
         r.size, r.alloc[0], r.alloc[1], r.alloc[2], r.alloc[3], r.alloc[4], r.freed[0], r.freed[1], r.freed[2],
         r.freed[3], r.freed[4]);
}
} // namespace heaptrace

X86_EXTERN_FUNC(__imp__sub_00269405);
X86_FUNC(sub_00269405) {
    static const bool release_trace = DevEnv("XBE_TRACE_RELEASE") != nullptr;
    if (!heaptrace::On() && !release_trace) return __imp__sub_00269405(ctx, base);
    heaptrace::Rec r;
    r.size = X86_MEM32(ctx.esp + 4);
    if (heaptrace::On()) heaptrace::Chain(base, ctx, r.alloc);
    __imp__sub_00269405(ctx, base);
    if (!ctx.eax) return;
    std::lock_guard<std::mutex> lock(heaptrace::g_lock);
    if (heaptrace::g_released) {  // a new allocation here: earlier releases of objects inside it no longer count
        auto &m = *heaptrace::g_released;
        m.erase(m.lower_bound(ctx.eax), m.lower_bound(ctx.eax + r.size));
    }
    if (heaptrace::On()) heaptrace::g_blocks[ctx.eax] = r;
}

X86_EXTERN_FUNC(__imp__sub_002688F9);
X86_FUNC(sub_002688F9) {
    if (heaptrace::On()) {
        const uint32_t ptr = X86_MEM32(ctx.esp + 4);
        std::lock_guard<std::mutex> lock(heaptrace::g_lock);
        auto it = heaptrace::g_blocks.find(ptr);
        if (it != heaptrace::g_blocks.end()) {
            if (!it->second.live) heaptrace::Log("DOUBLE-FREE", ptr, it->second);
            it->second.live = false;
            heaptrace::Chain(base, ctx, it->second.freed);
        }
    }
    __imp__sub_002688F9(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_0021FC00);
X86_FUNC(sub_0021FC00) {
    static const bool release_trace = DevEnv("XBE_TRACE_RELEASE") != nullptr;
    if (release_trace) {
        // Release twice without a new allocation at that address in between = double release; log both.
        static std::map<uint32_t, std::array<uint32_t, 5>> released;
        std::array<uint32_t, 5> chain{};
        heaptrace::Chain(base, ctx, chain.data());
        std::lock_guard<std::mutex> g(heaptrace::g_lock);
        heaptrace::g_released = &released;
        auto [it, fresh] = released.emplace(ctx.ecx, chain);
        if (!fresh) {
            static int n = 0;
            if (n++ < 20) {
                const auto &a = it->second;
                LOGR("reltrace DOUBLE RELEASE obj %08X cb %08X refs %04X first %08X<%08X<%08X<%08X<%08X now "
                     "%08X<%08X<%08X<%08X<%08X",
                     ctx.ecx, X86_MEM32(ctx.ecx + 0x10), X86_MEM16(ctx.ecx + 0x82), a[0], a[1], a[2], a[3], a[4],
                     chain[0], chain[1], chain[2], chain[3], chain[4]);
            }
            it->second = chain;
        }
    }
    if (heaptrace::On()) {
        const uint32_t obj = ctx.ecx;
        std::lock_guard<std::mutex> lock(heaptrace::g_lock);
        auto it = heaptrace::g_blocks.upper_bound(obj);
        if (it != heaptrace::g_blocks.begin()) {
            --it;
            if (!it->second.live && obj < it->first + it->second.size) {
                static int n = 0;
                if (n++ < 40) {
                    LOGR("heaptrace RELEASE OF FREED obj %08X (+%X in block) from %08X", obj, obj - it->first,
                         X86_MEM32(ctx.esp));
                    heaptrace::Log("freed", it->first, it->second);
                }
            }
        }
    }
    __imp__sub_0021FC00(ctx, base);
}

// XBE_CITY_SLOTDRAW_TRACE: does the portal pass reach ring slots? 0x13D100 is the per-slot setup it calls;
// count calls on game slots (city + 0xE0..) and on store slots (allocated top-down, >= 0x70000000).
X86_EXTERN_FUNC(__imp__sub_0013D100);
X86_FUNC(sub_0013D100) {
    static const bool on = DevEnv("XBE_CITY_SLOTDRAW_TRACE") != nullptr;
    if (on) {
        static uint32_t game_calls = 0, store_calls = 0;
        static auto last = std::chrono::steady_clock::now();
        (ctx.ecx >= 0x70000000u ? store_calls : game_calls)++;
        const auto now = std::chrono::steady_clock::now();
        if (now - last > std::chrono::seconds(5)) {
            last = now;
            LOGR("slotdraw: 0x13D100 on game slots %u, store slots %u (ret %08X)", game_calls, store_calls,
                 X86_MEM32(ctx.esp));
            // Which slot field leads to this object? Search the nine game slots (city + 0xE0, 0x8B0 each) for
            // the pointer, and one level deeper through each slot pointer field.
            const uint32_t obj = ctx.ecx, city = X86_MEM32(kCityPtr);
            int hits = 0;
            for (uint32_t i = 0; city && i < 9 && hits < 4; ++i) {
                const uint32_t slot = city + 0xE0 + i * 0x8B0;
                for (uint32_t off = 0; off < 0x8B0 && hits < 4; off += 4) {
                    const uint32_t v = X86_MEM32(slot + off);
                    if (v == obj) {
                        LOGR("slotdraw: obj %08X = slot %u +%X", obj, i, off);
                        ++hits;
                    } else if (v >= 0x10000u && (v & 3u) == 0 && mem::IsCommitted(v) && mem::IsCommitted(v + 0x3FC)) {
                        for (uint32_t o2 = 0; o2 < 0x400 && hits < 4; o2 += 4)
                            if (X86_MEM32(v + o2) == obj) {
                                LOGR("slotdraw: obj %08X = [slot %u +%X] +%X", obj, i, off, o2);
                                ++hits;
                            }
                    }
                }
            }
            if (!hits) LOGR("slotdraw: obj %08X (vt %08X) not found in game slots", obj, X86_MEM32(obj));
        }
    }
    __imp__sub_0013D100(ctx, base);
}
#endif  // TCNYC_DEV_TOOLS

// XBE_CITY_DRAWPROBE: which guest code draws tile (.g) geometry? A draw whose vertex buffer header lies inside a
// game slot's .g block (slot + 0x30, the next 4 MB) is tile geometry; log its distinct caller chains.
using XbeDrawProbeFn = void (*)(X86Context &, uint32_t vertex_buffer, uint32_t count, uint32_t texture0);
XbeDrawProbeFn &XbeDrawProbe();
namespace {
void TileDrawProbe(X86Context &ctx, uint32_t vb, uint32_t, uint32_t) {
    uint8_t *base = mem::Base();
    const uint32_t city = X86_MEM32(kCityPtr);
    if (!city || !vb) return;
    for (uint32_t i = 0; i < 9; ++i) {
        const uint32_t block = X86_MEM32(city + 0xE0 + i * 0x8B0 + 0x30);
        if (!block || vb < block || vb >= block + 0x400000u) continue;
        uint32_t chain[10] = {};
        chain[0] = X86_MEM32(ctx.esp);
        for (uint32_t ebp = ctx.ebp, n = 1; n < 10; ++n) {
            if (ebp < 0x10000u || (ebp & 3u) || !mem::IsCommitted(ebp) || !mem::IsCommitted(ebp + 7)) break;
            chain[n] = X86_MEM32(ebp + 4);
            const uint32_t next = X86_MEM32(ebp);
            if (next <= ebp) break;
            ebp = next;
        }
        const uint32_t key = chain[1] ^ (chain[3] << 1) ^ (chain[5] << 2);
        static std::vector<uint32_t> seen;
        static std::mutex lock;
        std::lock_guard<std::mutex> g(lock);
        if (seen.size() >= 16 || std::find(seen.begin(), seen.end(), key) != seen.end()) return;
        seen.push_back(key);
        LOGR("drawprobe: tile draw (slot %u, vb %08X) from %08X<%08X<%08X<%08X<%08X<%08X<%08X<%08X<%08X<%08X", i, vb,
             chain[0], chain[1], chain[2], chain[3], chain[4], chain[5], chain[6], chain[7], chain[8], chain[9]);
        return;
    }
}
} // namespace
namespace {
// XBE_CITY_DRAWPROBE=quads: which guest path submits small (4-vertex) draws such as the façade sign quads?
// Groups 4-vertex draws by caller chain and every 2 s logs each chain's draw count with a sample texture data
// address (the D3D resource's +4), so a walk across a detail transition shows which chain gains the signs.
struct QuadChain { uint32_t chain[8]; uint32_t count; uint32_t tex; };
void QuadDrawProbe(X86Context &ctx, uint32_t, uint32_t count, uint32_t tex0) {
    if (count != 4) return;
    uint8_t *base = mem::Base();
    {
        // Draws reached from 0x1387F0's object branch: 0x21E1A0(ctx, object, matrix) returns to 0x1388AF.
        // Its frame is the ebp frame whose return address is 0x1388AF; log each new object it draws.
        for (uint32_t ebp = ctx.ebp, n = 0; n < 10; ++n) {
            if (ebp < 0x10000u || (ebp & 3u) || !mem::IsCommitted(ebp) || !mem::IsCommitted(ebp + 0x13)) break;
            if (X86_MEM32(ebp + 4) == TC_ADDR(0x1388AF)) {
                const uint32_t obj = X86_MEM32(ebp + 0xC), mtx = X86_MEM32(ebp + 0x10);
                static std::map<uint32_t, uint32_t> objs;
                static std::mutex olock;
                std::lock_guard<std::mutex> g(olock);
                if (objs.size() < 400 && objs.emplace(obj, 0).second) {
                    const bool okm = mtx && mem::IsCommitted(mtx + 0x3F), oko = obj && mem::IsCommitted(obj + 0x3F);
                    LOGR("signobj: obj %08X vtable %08X +4 %08X +8 %08X +0x30 %08X +0x38 %08X pos %.0f,%.0f,%.0f tex %08X",
                         obj, oko ? X86_MEM32(obj) : 0, oko ? X86_MEM32(obj + 4) : 0, oko ? X86_MEM32(obj + 8) : 0,
                         oko ? X86_MEM32(obj + 0x30) : 0, oko ? X86_MEM32(obj + 0x38) : 0,
                         okm ? float(X86_MEMF32(mtx + 0x30)) : 0.f, okm ? float(X86_MEMF32(mtx + 0x34)) : 0.f,
                         okm ? float(X86_MEMF32(mtx + 0x38)) : 0.f,
                         (tex0 && mem::IsCommitted(tex0 + 4)) ? X86_MEM32(tex0 + 4) : 0);
                }
                break;
            }
            const uint32_t next = X86_MEM32(ebp);
            if (next <= ebp) break;
            ebp = next;
        }
    }
    QuadChain q{};
    q.chain[0] = X86_MEM32(ctx.esp);
    for (uint32_t ebp = ctx.ebp, n = 1; n < 8; ++n) {
        if (ebp < 0x10000u || (ebp & 3u) || !mem::IsCommitted(ebp) || !mem::IsCommitted(ebp + 7)) break;
        q.chain[n] = X86_MEM32(ebp + 4);
        const uint32_t next = X86_MEM32(ebp);
        if (next <= ebp) break;
        ebp = next;
    }
    const uint32_t tex = (tex0 && mem::IsCommitted(tex0 + 4)) ? X86_MEM32(tex0 + 4) : 0;
    uint64_t key = 0;
    for (uint32_t c : q.chain) key = key * 0x100000001B3ull ^ c;
    static std::unordered_map<uint64_t, QuadChain> chains;
    static std::mutex lock;
    static auto last = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> g(lock);
    auto it = chains.find(key);
    if (it == chains.end()) {
        if (chains.size() >= 64) return;
        it = chains.emplace(key, q).first;
        it->second.count = 0;
    }
    it->second.count++;
    if (tex) it->second.tex = tex;
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds(2)) return;
    last = now;
    for (auto &[k, c] : chains) {
        if (!c.count) continue;
        LOGR("quadprobe: %6u draws tex %08X from %08X<%08X<%08X<%08X<%08X<%08X<%08X<%08X", c.count, c.tex,
             c.chain[0], c.chain[1], c.chain[2], c.chain[3], c.chain[4], c.chain[5], c.chain[6], c.chain[7]);
        c.count = 0;
    }
    LOGR("quadprobe: ----");
}
} // namespace
void InstallTileDrawProbe() {
    static const char* const probe = DevEnv("XBE_CITY_DRAWPROBE");
    if (!probe || ::XbeDrawProbe()) return;
    ::XbeDrawProbe() = std::strcmp(probe, "quads") == 0 ? QuadDrawProbe : TileDrawProbe;
}

bool g_ring_instances_done = false;

// Ring instances: 0x14A240 calls 0x148D10(this = its visibility context, slot root [slot + 0x50], planes,
// plane count, 0, 0) to add a slot's visible instances to the visible list (moved and enlarged, see
// RelocateInstTable). After the game's own call (return 0x14A73B), run it again once per frame with the same
// context for each loaded ring slot, so their full-detail instances are drawn when enabled in F10.
X86_EXTERN_FUNC(__imp__sub_00148D10);
X86_FUNC(sub_00148D10) {
#if !TCNYC_PAL && TCNYC_DEV_TOOLS
    { extern bool g_ring_walk; extern void CountRingWalk(); if (g_ring_walk) CountRingWalk(); }
#endif
    const uint32_t ret = X86_MEM32(ctx.esp);
    const bool game_slot_walk = ret == TC_ADDR(0x0014A73Bu);
    const uint32_t self = ctx.ecx;
    if (game_slot_walk) ApplyCityInstanceDistances();
    __imp__sub_00148D10(ctx, base);
    static const bool inst_ctx = DevEnv("XBE_CITY_INSTCTX") != nullptr;
    if (inst_ctx) {
        static auto t = std::chrono::steady_clock::now();
        static uint32_t seen[8] = {}, nseen = 0;
        const uint32_t owner = X86_MEM32(self + 0xA54);
        bool known = false;
        for (uint32_t k = 0; k < nseen; ++k) known |= seen[k] == owner;
        if (!known && nseen < 8) {
            seen[nseen++] = owner;
            LOGR("instctx: list owner %08X (city %08X) ret %08X count %u", owner, X86_MEM32(kCityPtr), ret,
                 X86_MEM32(owner + 0x6430));
        }
        if (std::chrono::steady_clock::now() - t > std::chrono::seconds(5)) {
            t = std::chrono::steady_clock::now();
            LOGR("instctx: owner %08X count after walk %u", owner, X86_MEM32(owner + 0x6430));
        }
    }
    if (!game_slot_walk || !g_ring_instance_cutoff || !g_instance_table_ready || !game::city_grid::Ring()) return;
    const auto [inner, outer] = g_instance_distances;
    if (inner == 1.0f && outer == 0.0f) return;
    const uint32_t city = X86_MEM32(kCityPtr);
    if (g_ring_instances_done) return;  // once per frame (reset at the city draw entry)
    g_ring_instances_done = true;
    const uint32_t before = X86_MEM32(city + 0x6430);

    const int n = game::city_grid::N();
    const uint32_t saved_ecx = ctx.ecx, saved_eax = ctx.eax;
    const float saved_cutoff = *g_ring_instance_cutoff;
    int inner_slots = 0, outer_slots = 0;
    // 0x148D10 pops its five arguments, so esp is the caller's frame at 0x14A73B again.
    const uint32_t frame = ctx.esp;
    static const bool dyn_records = DevEnv("XBE_CITY_RING_NO_DYNAMIC") == nullptr;
    uint32_t dyn_total = 0;
    // XBE_TRACE_RING: periodic ring instance logs (every 5 s while the ring runs)
    static const bool ring_trace = DevEnv("XBE_TRACE_RING") != nullptr;
    if (ring_trace) {
        static auto t = std::chrono::steady_clock::now();
        if (std::chrono::steady_clock::now() - t > std::chrono::seconds(5)) {
            t = std::chrono::steady_clock::now();
            const uint32_t rec = X86_MEM32(frame + 0x34);
            LOGR("ring dyn: esp %08X records %08X total %d game-count %d city %08X tile %d first %.0f,%.0f r %.0f",
                 frame, rec, int32_t(X86_MEM32(frame + 0x18)), int32_t(X86_MEM32(frame + 0x2C)), X86_MEM32(frame + 0x24),
                 int32_t(X86_MEM32(X86_MEM32(frame + 0x24) + 0xBC)), rec ? float(X86_MEMF32(rec)) : 0.f,
                 rec ? float(X86_MEMF32(rec + 8)) : 0.f, rec ? float(X86_MEMF32(rec + 12)) : 0.f);
        }
    }
    for (int i = 0; i < n * n; ++i) {
        if (!game::city_grid::RingCell(i)) continue;
        const uint32_t slot = game::city_grid::Slots() + uint32_t(i) * 0x8B0u;
        const bool inner_slot = game::city_grid::InInnerDetailZone(base, slot);
        if (!inner_slot && outer == 0.0f) continue;
        const uint32_t root = X86_MEM32(slot + 0x50);
        const uint32_t instances = X86_MEM32(slot + 0x5C);
        if (!root || !X86_MEM32(slot + 0x54) || X86_MEM32(slot + 0x24) || instances > (0x8B0u - 0x70u) * 8u)
            continue;
        // 0x14A240 clears the nine game slots' per-frame instance bits. Do the same for the ring slot.
        std::memset(base + slot + 0x70, 0, (instances + 7) / 8);
        X86_MEM32(slot + 0x6C) = slot + 0x70;
        // The five pushed arguments move the context from esp+0x580 to esp+0x594. 0x148A00 reads the
        // slot from context+0xA50 (esp+0xFD0 before the pushes) when resolving a leaf's instance indices.
        const uint32_t saved_slot = X86_MEM32(self + 0xA50);
        X86_MEM32(self + 0xA50) = slot;
        *g_ring_instance_cutoff = inner_slot ? inner : outer;
        ctx.ecx = self;
#if !TCNYC_PAL && TCNYC_DEV_TOOLS
        { extern bool g_ring_walk; g_ring_walk = true; }
#endif
        // The game's 0x20-byte records describe dynamic objects selected for its current slot.
        // Walk the ring slot's static instances without registering those unrelated objects again.
        // Dynamic records (signs and other placed objects): 0x14A240 hands each of its slots the records of
        // its frame array ([esp+0x34], [esp+0x18] in total) whose centre lies within half a tile plus the
        // radius of the slot centre (+0x40) in x and z, partitioned to the front. Do the same for the ring slot.
        // The dynamic records overlapping this slot are copied into a scratch list rather than moved to the
        // front of the game's own array, which kept its order only by luck. They are copied back after the
        // walk so anything it records in them still reaches the game's array.
        constexpr uint32_t kRecordSize = 0x20, kMaxRecords = 4096;
        static const uint32_t scratch = mem::AllocSystem(kMaxRecords * kRecordSize);
        static std::vector<uint32_t> picked;
        picked.clear();
        uint32_t dyn = 0, dyn_count = 0;
        if (dyn_records && scratch) {
            const uint32_t records = X86_MEM32(frame + 0x34);
            const int32_t total = int32_t(X86_MEM32(frame + 0x18));
            const float half = float(int32_t(X86_MEM32(X86_MEM32(frame + 0x24) + 0xBC)) / 2);
            const float cx = X86_MEMF32(slot + 0x40), cz = X86_MEMF32(slot + 0x48);
            for (int32_t r = 0; records && r < total && r < int32_t(kMaxRecords); ++r) {
                const uint32_t rec = records + uint32_t(r) * kRecordSize;
                const float lim = half + X86_MEMF32(rec + 0x0C);
                if (!(std::fabs(cx - X86_MEMF32(rec)) < lim && std::fabs(cz - X86_MEMF32(rec + 0x08)) < lim)) continue;
                std::memcpy(base + scratch + dyn_count * kRecordSize, base + rec, kRecordSize);
                picked.push_back(rec);
                ++dyn_count;
            }
            if (dyn_count) dyn = scratch;
            dyn_total += dyn_count;
        }
        kernel::CallGuest(ctx, TC_ADDR(0x148D10), {root, dyn, dyn_count, 0, 0});
        for (uint32_t k = 0; k < dyn_count; ++k)
            std::memcpy(base + picked[k], base + scratch + k * kRecordSize, kRecordSize);
#if !TCNYC_PAL && TCNYC_DEV_TOOLS
        { extern bool g_ring_walk; g_ring_walk = false; }
#endif
        X86_MEM32(self + 0xA50) = saved_slot;
        ++(inner_slot ? inner_slots : outer_slots);
        static const int max_slots = [] { const char *e = DevEnv("XBE_CITY_RING_INSTANCE_SLOTS"); return e ? std::atoi(e) : 72; }();
        if (inner_slots + outer_slots >= max_slots) break;
    }
    *g_ring_instance_cutoff = saved_cutoff;
    ctx.ecx = saved_ecx;
    ctx.eax = saved_eax;
    static uint32_t frames_added = 0, peak_added = 0;
    const uint32_t after = X86_MEM32(city + 0x6430);
    if (after > before) {
        ++frames_added;
        peak_added = std::max(peak_added, after - before);
    }
    static auto last = std::chrono::steady_clock::now();
    if (ring_trace && std::chrono::steady_clock::now() - last > std::chrono::seconds(5)) {
        last = std::chrono::steady_clock::now();
        LOGR("ring instances: %d inner-band slots at %.2fx, %d outer slots at %.2fx, visible list %u -> %u, "
             "growth in %u frames (peak +%u), %u dynamic records", inner_slots, inner, outer_slots, outer, before, after,
             frames_added, peak_added, dyn_total);
        frames_added = peak_added = 0;
#if !TCNYC_PAL && TCNYC_DEV_TOOLS
        { void LogRingWalkStats(); LogRingWalkStats(); }
#endif
    }
}


// Called by the skyline hook (camera.cpp) just before the skyline at 0x13F2BB, inside the distant-city pass
// 0x13F090 and right after the game's nine distant cells: the ring's cells then get exactly the same render
// states (depth, blend, fog) as the game's own. XBE_CITY_RING_DRAW_LATE=1 restores the old draw after the pass.
void DrawRingInDistantPass(uint8_t *base, X86Context &ctx) {
    static const bool late = DevEnv("XBE_CITY_RING_DRAW_LATE") != nullptr;
    static const bool nodraw = DevEnv("XBE_CITY_RING_NODRAW") != nullptr;
    const uint32_t city = X86_MEM32(kCityPtr);
    if (late || nodraw || !game::city_grid::Ring() || !g_game_cell_ctx || !city) return;
    // While one of the game's own nine tiles is still loading, its gap would show the ring's far tiles in front
    // of where nearer buildings belong; hold the ring back until the game's tiles settle (state +0x24 idle).
    static const bool always = DevEnv("XBE_CITY_RING_DRAW_ALWAYS") != nullptr;
    for (uint32_t i = 0; !always && i < 9; ++i)
        if (X86_MEM32(city + 0xE0 + i * 0x8B0 + 0x24) || X86_MEM32(city + 0x4FA4 + i * 0x9C + 0x24)) return;
    const uint32_t eax = ctx.eax, ecx = ctx.ecx, edx = ctx.edx;
    game::city_grid::DrawRing(base, ctx, g_game_cell_ctx);
    ctx.eax = eax;
    ctx.ecx = ecx;
    ctx.edx = edx;
}

#if TCNYC_DEV_TOOLS
// XBE_CITY_PORTALTRACE: the portal pass 0x13E7F0 runs on `this` (stored at [esp+0x44]) and at 0x13ECAD reads the
// city's instance table instead of this->+0x50. Log every distinct `this` that is not the city.
X86_EXTERN_FUNC(__imp__sub_0013E7F0);
X86_FUNC(sub_0013E7F0) {
    if (Trace()) TraceHit(17, ctx, base);
    static const bool on = DevEnv("XBE_CITY_PORTALTRACE") != nullptr;
    if (on) {
        const uint32_t self = ctx.ecx, city = X86_MEM32(kCityPtr);
        static uint32_t seen[16] = {}, n = 0;
        bool known = self == city;
        for (uint32_t i = 0; i < n && !known; ++i) known = seen[i] == self;
        if (!known && n < 16) {
            seen[n++] = self;
            const int ring_n = game::city_grid::Ready() ? game::city_grid::N() : 0;
            const uint32_t store_lo = game::city_grid::Ready() ? game::city_grid::Slots() : 0;
            LOGR("portaltrace: this %08X (city %08X) vt %08X ret %08X [this+0x50] %08X [city+0x50] %08X store slots %08X..+%X",
                 self, city, X86_MEM32(self), X86_MEM32(ctx.esp), X86_MEM32(self + 0x50), X86_MEM32(city + 0x50),
                 store_lo, uint32_t(ring_n * ring_n) * 0x8B0u);
        }
    }
    __imp__sub_0013E7F0(ctx, base);
}
#endif

#if !TCNYC_PAL && TCNYC_DEV_TOOLS
// Diagnostic (XBE_CITY_RING_INSTANCES): what the tile walk 0x148D10 does with a ring slot's tree. Counted only while
// a ring walk runs (g_ring_walk): recursion, subtree accept 0x13E4A0, add 0x148A00, and the two bound tests.
bool g_ring_walk = false;
struct RingWalkStats {
    uint32_t walks = 0, accepts = 0, adds = 0, bound_calls = 0, bound_pass = 0;
    uint32_t distance_calls = 0, distant_subtrees = 0;
    float last_bound = 0.0f;
} g_rws;
X86_EXTERN_FUNC(__imp__sub_0013E4A0);
X86_FUNC(sub_0013E4A0) {
    if (g_ring_walk) ++g_rws.accepts;
    __imp__sub_0013E4A0(ctx, base);
}
X86_EXTERN_FUNC(__imp__sub_00148A00);
X86_FUNC(sub_00148A00) {
    if (g_ring_walk) ++g_rws.adds;
    __imp__sub_00148A00(ctx, base);
}
X86_EXTERN_FUNC(__imp__sub_00136DB0);
X86_FUNC(sub_00136DB0) {
    const uint32_t obj = ctx.ecx, ret = X86_MEM32(ctx.esp);
    __imp__sub_00136DB0(ctx, base);
    static const bool portal_trace = DevEnv("XBE_CITY_PORTAL_OWNERS") != nullptr;
    if (portal_trace && ret >= 0x13E7F0u && ret < 0x13EF50u && game::city_grid::Ready()) {
        // Which tiles does the portal pass visit? Classify the object by the .g block it lies in.
        static uint32_t game_hits = 0, ring_hits = 0, other = 0;
        static auto last = std::chrono::steady_clock::now();
        const uint32_t city = X86_MEM32(kCityPtr);
        int owner = 2;
        for (uint32_t i = 0; city && i < 9 && owner == 2; ++i) {
            const uint32_t b = X86_MEM32(city + 0xE0 + i * 0x8B0 + 0x30);
            if (b && obj >= b && obj < b + 0x400000u) owner = 0;
        }
        const int n = game::city_grid::N();
        for (int i = 0; i < n * n && owner == 2; ++i) {
            const uint32_t b = X86_MEM32(game::city_grid::Slots() + uint32_t(i) * 0x8B0u + 0x30);
            if (b && obj >= b && obj < b + 0x400000u) owner = 1;
        }
        (owner == 0 ? game_hits : owner == 1 ? ring_hits : other)++;
        if (std::chrono::steady_clock::now() - last > std::chrono::seconds(5)) {
            last = std::chrono::steady_clock::now();
            LOGR("portal owners: game tiles %u, ring tiles %u, other %u (per 5 s)", game_hits, ring_hits, other);
            game_hits = ring_hits = other = 0;
        }
    }
    if (!g_ring_walk || ret != 0x148D7Cu) return;
    ++g_rws.bound_calls;
    // The tile walk loads the result from [eax] before comparing it at 0x148D80.
    g_rws.last_bound = X86_MEMF32(ctx.eax);
    if (ctx.xmm[0].f[0] > X86_MEMF32(0x2E1C80)) ++g_rws.bound_pass;
}
X86_EXTERN_FUNC(__imp__sub_00136A70);
X86_FUNC(sub_00136A70) {
    const bool trace = g_ring_walk && X86_MEM32(ctx.esp) == 0x148E05u;
    __imp__sub_00136A70(ctx, base);
    if (trace) {
        ++g_rws.distance_calls;
        const float cutoff = X86_MEMF32(0x2D2AF4) * *g_ring_instance_cutoff;
        if (X86_MEMF32(ctx.esp + 0x34) > cutoff && X86_MEMF32(ctx.esp + 0x3C) > cutoff)
            ++g_rws.distant_subtrees;
    }
}
#endif
#if !TCNYC_PAL && TCNYC_DEV_TOOLS
void LogRingWalkStats() {
    uint8_t *base = mem::Base();
    LOGR("ring walk: %u walks, %u scene-list calls, %u instance leaf calls, bound %u (%u above cutoff, last %.3f vs %.3f), "
         "distance %u (%u early exits)",
         g_rws.walks, g_rws.accepts, g_rws.adds, g_rws.bound_calls, g_rws.bound_pass, g_rws.last_bound,
         X86_MEMF32(0x2E1C80), g_rws.distance_calls, g_rws.distant_subtrees);
    g_rws = {};
}
#endif
#if !TCNYC_PAL && TCNYC_DEV_TOOLS
void CountRingWalk() { ++g_rws.walks; }
#endif

// XBE_CITY_SIGNWATCH=1: who attaches/detaches the per-instance object that 0x1387F0 draws through 0x21E1A0
// (façade signs)? The first visible instance seen with an object gets a hardware write watch on inst+0.
#if !TCNYC_PAL && TCNYC_DEV_TOOLS  // diagnostic only, USA addresses
X86_EXTERN_FUNC(__imp__sub_001387F0);
X86_FUNC(sub_001387F0) {
    static const bool on = [] { const char *v = DevEnv("XBE_CITY_SIGNWATCH"); return v && std::strcmp(v, "1") == 0; }();
    static bool armed = false;
    if (on && !armed && !kernel::GuestWatchActive()) {
        const uint32_t self = ctx.ecx;
        const int32_t n = int32_t(X86_MEM32(self + 0x6430));
        for (int32_t i = 0; i < n && i < 4096; ++i) {
            const uint32_t inst = X86_MEM32((g_inst_entries ? g_inst_entries : self + 0x5AD4) + uint32_t(i) * 8);
            if (!inst || !mem::IsCommitted(inst)) continue;
            const uint32_t obj = X86_MEM32(inst);
            if (!obj) continue;
            LOGR("signwatch: inst %08X obj %08X (entry %d of %d, [inst+4] %08X)", inst, obj, i, n, X86_MEM32(inst + 4));
            kernel::InstallGuestWatch(inst, 4);
            armed = true;
            break;
        }
    }
    static const bool list_watch = [] { const char *v = DevEnv("XBE_CITY_SIGNWATCH");
                                        return v && std::strcmp(v, "list") == 0; }();
    if (list_watch && !kernel::GuestWatchActive()) {
        // Live-object list [0x37F634]: node +0 next, +8 object. Watch the sentinel's next pointer so each
        // head insertion logs its writer's stack.
        const uint32_t head = X86_MEM32(0x37F634u);
        if (head) {
            LOGR("signwatch: live list sentinel %08X first %08X", head, X86_MEM32(head));
            kernel::InstallGuestWatch(head, 4);
        }
    }
    static const bool count = DevEnv("XBE_CITY_SIGNCOUNT") != nullptr;
    if (count) {
        // Visible instances by owning tile: game 3x3, inner ring (5x5 band), outer ring; and how many carry an
        // attached object (the 0x21E1A0 draw: façade signs). The instance lies in its slot's .g block.
        static uint32_t vis[3], obj[3], frames, after_ring, is_city;
        if (g_ring_instances_done) ++after_ring;
        if (ctx.ecx == X86_MEM32(kCityPtr)) ++is_city;
        static auto last = std::chrono::steady_clock::now();
        const uint32_t self = ctx.ecx, city = X86_MEM32(kCityPtr);
        const int32_t n = int32_t(X86_MEM32(self + 0x6430));
        const int grid = game::city_grid::Ring() ? game::city_grid::N() : 0;
        for (int32_t i = 0; i < n && i < 8192; ++i) {
            const uint32_t inst = X86_MEM32((g_inst_entries ? g_inst_entries : self + 0x5AD4) + uint32_t(i) * 8);
            if (!inst) continue;
            int cls = -1;
            for (uint32_t s = 0; s < 9 && city && cls < 0; ++s) {
                const uint32_t b = X86_MEM32(city + 0xE0 + s * 0x8B0 + 0x30);
                if (b && inst >= b && inst < b + 0x400000u) cls = 0;
            }
            for (int s = 0; s < grid * grid && cls < 0; ++s) {
                if (!game::city_grid::RingCell(s)) continue;
                const uint32_t slot = game::city_grid::Slots() + uint32_t(s) * 0x8B0u;
                const uint32_t b = X86_MEM32(slot + 0x30);
                if (b && inst >= b && inst < b + 0x400000u)
                    cls = game::city_grid::InInnerDetailZone(base, slot) ? 1 : 2;
            }
            if (cls < 0) continue;
            static const bool ring_watch = [] { const char *v = DevEnv("XBE_CITY_SIGNWATCH");
                                                 return v && std::strcmp(v, "ring") == 0; }();
            static const bool game_watch = [] { const char *v = DevEnv("XBE_CITY_SIGNWATCH");
                                                 return v && std::strcmp(v, "game") == 0; }();
            if (game_watch && cls == 0 && !X86_MEM32(inst) && X86_MEM32(inst + 4) && !kernel::GuestWatchActive()) {
                LOGR("signwatch: game-tile inst %08X has no object ([inst+4] %08X); watching", inst, X86_MEM32(inst + 4));
                kernel::InstallGuestWatch(inst, 4);
            }
            if (ring_watch && cls == 1 && !X86_MEM32(inst) && !kernel::GuestWatchActive()) {
                LOGR("signwatch: inner-ring inst %08X has no object ([inst+4] %08X); watching", inst, X86_MEM32(inst + 4));
                kernel::InstallGuestWatch(inst, 4);
            }
            ++vis[cls];
            if (const uint32_t o = X86_MEM32(inst)) {
                ++obj[cls];
                // object class census: vtable -> (count, sample object, sample instance)
                if (mem::IsCommitted(o)) {
                    static std::map<uint32_t, std::array<uint32_t, 3>> classes;
                    auto &c = classes[X86_MEM32(o)];
                    if (!c[0]) { c[1] = o; c[2] = inst; }
                    ++c[0];
                    static auto t = std::chrono::steady_clock::now();
                    if (std::chrono::steady_clock::now() - t > std::chrono::seconds(10)) {
                        t = std::chrono::steady_clock::now();
                        for (auto &[vt, v] : classes)
                            LOGR("signclass: vtable %08X seen %u (obj %08X inst %08X, obj+4 %08X +8 %08X +0x38 %08X)", vt,
                                 v[0], v[1], v[2], X86_MEM32(v[1] + 4), X86_MEM32(v[1] + 8), X86_MEM32(v[1] + 0x38));
                        classes.clear();
                    }
                }
            }
        }
        ++frames;
        const auto now = std::chrono::steady_clock::now();
        if (now - last >= std::chrono::seconds(2)) {
            last = now;
            LOGR("signcount: per frame visible/with-object  game3x3 %u/%u  inner-ring %u/%u  outer %u/%u  (%u frames, %u after ring walk, %u self==city, list %d)",
                 vis[0] / frames, obj[0] / frames, vis[1] / frames, obj[1] / frames, vis[2] / frames, obj[2] / frames,
                 frames, after_ring, is_city, n);
            vis[0] = vis[1] = vis[2] = obj[0] = obj[1] = obj[2] = frames = after_ring = is_city = 0;
        }
    }
    __imp__sub_001387F0(ctx, base);
}
#endif

// XBE_CITY_SIGNCOUNT=1: the façade-sign pass 0x13F840 walks the visible list and draws an overlay for each
// instance whose model ([city+0x38] + ([inst+4]>>20)*0x100) has +0x4C == 1 and overlay data +0x1C, if the
// overlay mask test on 0x136E70's flags passes. Count each step per owning tile class at the pass's entry.
void SignPassCensus(uint8_t *base) {
#if TCNYC_PAL
    (void)base;  // diagnostic only, USA addresses
#else
    static const bool on = DevEnv("XBE_CITY_SIGNCOUNT") != nullptr;
    const uint32_t city = X86_MEM32(kCityPtr);
    if (on && city) {
        static uint32_t vis[4], ovl[4], pass[4], frames;
        const int32_t n = int32_t(X86_MEM32(city + 0x6430));
        const int grid = game::city_grid::Ring() ? game::city_grid::N() : 0;
        for (int32_t i = 0; i < n && i < 8192; ++i) {
            const uint32_t inst = X86_MEM32((g_inst_entries ? g_inst_entries : city + 0x5AD4) + uint32_t(i) * 8);
            if (!inst || !mem::IsCommitted(inst + 0x17)) continue;
            int cls = -1;
            for (uint32_t s = 0; s < 9 && cls < 0; ++s) {
                const uint32_t b = X86_MEM32(city + 0xE0 + s * 0x8B0 + 0x30);
                if (b && inst >= b && inst < b + 0x400000u) cls = 0;
            }
            for (int s = 0; s < grid * grid && cls < 0; ++s) {
                if (!game::city_grid::RingCell(s)) continue;
                const uint32_t slot = game::city_grid::Slots() + uint32_t(s) * 0x8B0u;
                const uint32_t b = X86_MEM32(slot + 0x30);
                if (b && inst >= b && inst < b + 0x400000u)
                    cls = game::city_grid::InInnerDetailZone(base, slot) ? 1 : 2;
            }
            if (cls < 0) cls = 3;
            ++vis[cls];
            const uint32_t model = X86_MEM32(city + 0x38) + ((X86_MEM32(inst + 4) >> 20) << 8);
            if (!mem::IsCommitted(model + 0x4F) || X86_MEM16(model + 0x4C) != 1) continue;
            const uint32_t ov = X86_MEM32(model + 0x1C);
            if (!ov || !mem::IsCommitted(ov + 0x1B)) continue;
            ++ovl[cls];
            // 0x136E70 flags (stdcall-free leaf: arg on the stack, cdecl)
            const uint32_t f14 = X86_MEM32(inst + 0x14), f8 = X86_MEM32(inst + 8);
            uint32_t fl = ((f14 >> 22) < X86_MEM16(0x37F0B0u + ((f14 >> 16) & 0x3F) * 2)) ? 0x4000u : 0u;
            fl |= X86_MEM16(0x37F180u + ((f14 >> 8) & 0x0F) * 2);
            fl |= ((f8 & 0xFFFF) < X86_MEM16(0x363D44u)) ? 0x8000u : 0u;
            fl |= (f14 >> 12) & 0x0F;
            if (((fl & X86_MEM16(ov + 0x18)) ^ X86_MEM16(ov + 0x1A)) == 0) {
                ++pass[cls];
                // texture entry as 0x13F9AB picks it: pooled ([0x3F54C4]) when [ov+0x14] has 0x4000, else the
                // model's own table [model+0x10]; 36-byte entries. Log the first sightings per class.
                const uint32_t t = X86_MEM32(ov + 0x14);
                const bool pooled = (t & 0x4000u) != 0;
                const uint32_t idx = pooled ? (t & 0x3FFFu) : t;
                const uint32_t tab = pooled ? X86_MEM32(0x3F54C4u) : X86_MEM32(model + 0x10);
                const uint32_t e = tab + idx * 36u;
                static std::map<uint64_t, int> seen;
                if (seen.size() < 120 && seen.emplace((uint64_t(cls) << 32) | e, 0).second && mem::IsCommitted(e + 35))
                    LOGR("signtex: %s inst %08X pooled %d idx %X entry %08X: %08X %08X %08X %08X %08X %08X",
                         cls == 0 ? "game3x3" : cls == 1 ? "inner" : cls == 2 ? "outer" : "unmatched", inst, pooled,
                         idx, e, X86_MEM32(e), X86_MEM32(e + 4), X86_MEM32(e + 8), X86_MEM32(e + 12),
                         X86_MEM32(e + 16), X86_MEM32(e + 20));
            }
        }
        ++frames;
        static auto last = std::chrono::steady_clock::now();
        if (std::chrono::steady_clock::now() - last >= std::chrono::seconds(3)) {
            last = std::chrono::steady_clock::now();
            LOGR("signpass: per frame visible/overlay/mask-pass  game3x3 %u/%u/%u  inner-ring %u/%u/%u  outer %u/%u/%u  "
                 "unmatched %u/%u/%u (list %d, %u frames)", vis[0] / frames, ovl[0] / frames, pass[0] / frames, vis[1] / frames,
                 ovl[1] / frames, pass[1] / frames, vis[2] / frames, ovl[2] / frames, pass[2] / frames, vis[3] / frames, ovl[3] / frames, pass[3] / frames, n, frames);
            for (int k = 0; k < 4; ++k) vis[k] = ovl[k] = pass[k] = 0;
            frames = 0;
        }
    }
#endif
}
