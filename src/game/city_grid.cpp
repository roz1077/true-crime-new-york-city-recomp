#include "game_addresses.h"
#include "core/dev_env.h"
// Phase 2 city tile store: an NÃ—N shadow of the game's fixed 3Ã—3 arrays.
//
// Nothing here changes the game yet. The game's arrays remain authoritative; this store is populated from them
// so Phase 3 can make it the source and mirror the centre 3Ã—3 back. XBE_CITY_GRID=3|5|7|9 enables it.
// XBE_CITY_MIRROR_TEST=1 runs a copy gameâ†’storeâ†’game round trip at 10, 20 and 30 seconds and restores the game
// arrays afterwards, so a mismatch cannot affect play.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <vector>

#include "app/app.h"
#include "core/log.h"
#include "game/city_grid.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

#include <cstdio>
void NodeGuard(uint8_t *base, const char *tag);
std::atomic<uint64_t> &XbeDrawCalls();
extern int g_ring_draw_idx;

namespace game::city_grid {

namespace {

constexpr uint32_t kSlotsCount = 9;
constexpr uint32_t kSlotBytes = 0x8B0, kCellBytes = 0x9C, kRequestBytes = 0x80, kTableBytes = 0x10;
// Offsets from the tile manager's `this` (city + 0xD0).
constexpr uint32_t kGameSlots = 0x10, kGameTable = 0x4E40, kGameCells = 0x4ED4, kGameRequests = 0x54F0;
constexpr int32_t kEmpty = int32_t(0x40000000);

struct Store {
    uint32_t base = 0, slots = 0, cells = 0, requests = 0, table = 0;
    uint32_t bytes = 0, manager = 0, test_slot = 0, test_cell = 0;
    uint32_t slot_template = 0, cell_template = 0;
    uint32_t slot_tmpl_buf = 0, cell_tmpl_buf = 0;
    bool templates_copied = false;
    int n = 0;
    bool ready = false, test = false, attempted = false, cell_test = false, cell_test_done = false;
    std::vector<uint32_t> orphan_requests;  // stream requests to release at the next DriveRing
    bool from_setting = false;  // allocated for app::Settings::city_range (no XBE_CITY_GRID); the setting drives the ring
    int last_radius = 0;
    bool ring = false, ring_init[128] = {}, ring_cell[128] = {}, ring_pending[128] = {}, ring_blocked[128] = {},
         ring_freeze = false, ring_center = false, ring_nodraw = false, ring_slotonly = false,
         ring_cellonly = false, ring_nodrive = false, keep_objects = false, scanned = false, no_unlink = false;
    int32_t ring_cx = 0, ring_cy = 0;
    bool ring_requested[128] = {};
    int32_t ring_target_x[128] = {}, ring_target_y[128] = {};
    int ring_pending_idx = -1;  // the cell whose tile load is in flight: no new request until it lands
    int ring_pending_wait = 0;  // drives spent waiting for it
    int ring_rr = 0;
    // Last tile each of the nine game cells held, kept while the cell still owns its object. A tile is
    // locked to the game through the unload window (tile cleared, object not yet released). Slots and
    // request records are locked too: they name the next tile before the cell updates, so the ring must
    // not load a tile the game has already committed to.
    int32_t game_x[27] = {}, game_y[27] = {};
    bool game_lock[27] = {}, game_tracked = false;
    uint32_t game_city = 0;
    std::chrono::steady_clock::time_point start, last, last_log, last_drive, last_draw, last_move;
    uint32_t syncs = 0, tests = 0, mismatches = 0;
};

Store g;

uint32_t Align16(uint32_t v) { return (v + 15u) & ~15u; }

int SideIndex(int32_t x, int32_t y, int n) {
    const int sx = int((x % n + n) % n);
    const int sy = int((y % n + n) % n);
    return sx * n + sy;
}

bool Valid(int32_t x, int32_t y) { return x != kEmpty && y != kEmpty; }

// +0x50 is the loaded tile's 32-bit root pointer, not a flag byte. A pointer
// whose low byte happens to be zero still means the slot is loaded.
bool SlotHasRoot(uint8_t *base, uint32_t slot) { return X86_MEM32(slot + 0x50) != 0; }

int32_t SlotX(uint8_t *base, uint32_t manager, int i) {
    return int32_t(X86_MEM32(manager + kGameSlots + uint32_t(i) * kSlotBytes + 0x04));
}
int32_t SlotY(uint8_t *base, uint32_t manager, int i) {
    return int32_t(X86_MEM32(manager + kGameSlots + uint32_t(i) * kSlotBytes + 0x08));
}

void SyncFromGame(uint8_t *base) {
    for (int i = 0; i < int(kSlotsCount); ++i) {
        const int32_t x = SlotX(base, g.manager, i), y = SlotY(base, g.manager, i);
        if (!Valid(x, y)) continue;
        const int idx = SideIndex(x, y, g.n);
        const uint32_t slot = g.manager + kGameSlots + uint32_t(i) * kSlotBytes;
        std::memcpy(base + g.slots + uint32_t(idx) * kSlotBytes, base + slot, kSlotBytes);
        std::memcpy(base + g.cells + uint32_t(idx) * kCellBytes,
                    base + g.manager + kGameCells + uint32_t(i) * kCellBytes, kCellBytes);

        const uint32_t req = g.manager + kGameRequests + uint32_t(i) * kRequestBytes;
        const int32_t rx = int32_t(X86_MEM32(req + 0x04)), ry = int32_t(X86_MEM32(req + 0x08));
        const int ridx = Valid(rx, ry) ? SideIndex(rx, ry, g.n) : idx;
        std::memcpy(base + g.requests + uint32_t(ridx) * kRequestBytes, base + req, kRequestBytes);
    }
}

void StoreToGame(uint8_t *base) {
    for (int i = 0; i < int(kSlotsCount); ++i) {
        const int32_t x = SlotX(base, g.manager, i), y = SlotY(base, g.manager, i);
        if (!Valid(x, y)) continue;
        const int idx = SideIndex(x, y, g.n);
        const uint32_t slot = g.manager + kGameSlots + uint32_t(i) * kSlotBytes;
        std::memcpy(base + slot, base + g.slots + uint32_t(idx) * kSlotBytes, kSlotBytes);
        std::memcpy(base + g.manager + kGameCells + uint32_t(i) * kCellBytes,
                    base + g.cells + uint32_t(idx) * kCellBytes, kCellBytes);

        const uint32_t req = g.manager + kGameRequests + uint32_t(i) * kRequestBytes;
        const int32_t rx = int32_t(X86_MEM32(req + 0x04)), ry = int32_t(X86_MEM32(req + 0x08));
        const int ridx = Valid(rx, ry) ? SideIndex(rx, ry, g.n) : idx;
        std::memcpy(base + req, base + g.requests + uint32_t(ridx) * kRequestBytes, kRequestBytes);
    }
}

void MirrorTest(uint8_t *base) {
    const size_t slot_bytes = size_t(kSlotsCount) * kSlotBytes;
    const size_t cell_bytes = size_t(kSlotsCount) * kCellBytes;
    const size_t req_bytes = size_t(kSlotsCount) * kRequestBytes;
    std::vector<uint8_t> slot_snap(slot_bytes), cell_snap(cell_bytes), req_snap(req_bytes);
    std::memcpy(slot_snap.data(), base + g.manager + kGameSlots, slot_bytes);
    std::memcpy(cell_snap.data(), base + g.manager + kGameCells, cell_bytes);
    std::memcpy(req_snap.data(), base + g.manager + kGameRequests, req_bytes);

    StoreToGame(base);

    int bad = 0;
    uint32_t first = 0;
    for (uint32_t i = 0; i < kSlotsCount; ++i) {
        if (std::memcmp(base + g.manager + kGameSlots + i * kSlotBytes, slot_snap.data() + i * kSlotBytes,
                        kSlotBytes)) {
            if (!bad) first = kGameSlots + i * kSlotBytes;
            ++bad;
        }
        if (std::memcmp(base + g.manager + kGameCells + i * kCellBytes, cell_snap.data() + i * kCellBytes,
                        kCellBytes)) {
            if (!bad) first = kGameCells + i * kCellBytes;
            ++bad;
        }
        if (std::memcmp(base + g.manager + kGameRequests + i * kRequestBytes, req_snap.data() + i * kRequestBytes,
                        kRequestBytes)) {
            if (!bad) first = kGameRequests + i * kRequestBytes;
            ++bad;
        }
    }

    std::memcpy(base + g.manager + kGameSlots, slot_snap.data(), slot_bytes);
    std::memcpy(base + g.manager + kGameCells, cell_snap.data(), cell_bytes);
    std::memcpy(base + g.manager + kGameRequests, req_snap.data(), req_bytes);
    g.mismatches += uint32_t(bad);
    if (bad)
        LOGR("city: mirror test %u: %d mismatches, first at %08X", g.tests, bad, first);
    else
        LOGR("city: mirror test %u: 0 mismatches", g.tests);
}

} // namespace

void TestOuter(uint8_t *base, X86Context &ctx, uint32_t city, uint32_t manager) {
    if (!g.ready || !g.cell_test || g.cell_test_done) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - g.start < std::chrono::seconds(5)) return;
    g.cell_test_done = true;

    uint32_t slot_template = 0, cell_template = 0;
    for (uint32_t i = 0; i < kSlotsCount; ++i) {
        const uint32_t slot = manager + kGameSlots + i * kSlotBytes;
        if (!SlotHasRoot(base, slot) && int32_t(X86_MEM32(slot + 0x04)) == kEmpty) {
            slot_template = slot;
            break;
        }
    }
    for (uint32_t i = 0; i < kSlotsCount; ++i) {
        const uint32_t cell = city + 0x4FA4 + i * kCellBytes;
        if (!X86_MEM8(cell + 0x88) && int32_t(X86_MEM32(cell + 0x04)) == kEmpty) {
            cell_template = cell;
            break;
        }
    }
    if (!slot_template || !cell_template) {
        LOGR("city: outer test: no empty slot/cell template (%08X/%08X)", slot_template, cell_template);
        return;
    }

    int32_t min_x = 0, max_x = 0, min_y = 0, max_y = 0;
    bool have = false;
    for (uint32_t i = 0; i < kSlotsCount; ++i) {
        const uint32_t slot = manager + kGameSlots + i * kSlotBytes;
        if (!SlotHasRoot(base, slot)) continue;
        const int32_t x = int32_t(X86_MEM32(slot + 0x04)), y = int32_t(X86_MEM32(slot + 0x08));
        if (!have) {
            min_x = max_x = x;
            min_y = max_y = y;
            have = true;
        } else {
            min_x = std::min(min_x, x);
            max_x = std::max(max_x, x);
            min_y = std::min(min_y, y);
            max_y = std::max(max_y, y);
        }
    }
    if (!have) {
        LOGR("city: outer test: no loaded game slot");
        return;
    }
    const int32_t cx = (min_x + max_x) / 2, cy = (min_y + max_y) / 2;
    const int32_t x = cx + 2, y = cy;
    const int idx = SideIndex(x, y, g.n);

    const uint32_t slot = g.slots + uint32_t(idx) * kSlotBytes;
    const uint32_t cell = g.cells + uint32_t(idx) * kCellBytes;
    g.test_slot = slot;
    g.test_cell = cell;
    std::memcpy(base + slot, base + slot_template, kSlotBytes);
    std::memcpy(base + cell, base + cell_template, kCellBytes);
    for (uint32_t off = 0; off + 4 <= kSlotBytes; off += 4) {
        const uint32_t v = X86_MEM32(slot + off);
        if (v == slot_template) X86_MEM32(slot + off) = slot;
        if (v == slot_template + 0x70) X86_MEM32(slot + off) = slot + 0x70;
    }
    for (uint32_t off = 0; off + 4 <= kCellBytes; off += 4) {
        if (X86_MEM32(cell + off) == cell_template) X86_MEM32(cell + off) = cell;
    }
    // 0x137600 only accepts a new target while +0x2C is set; an empty template has it clear.
    X86_MEM32(slot + 0x2C) = 1;
    X86_MEM32(cell + 0x2C) = 1;

    auto call = [&](uint32_t object, int32_t tx, int32_t ty) {
        const uint32_t saved = ctx.ecx;
        ctx.ecx = object;
        kernel::CallGuest(ctx, TC_ADDR(0x137600), {city, uint32_t(tx), uint32_t(ty)});
        ctx.ecx = saved;
    };
    call(slot, x, y);
    call(cell, x, y + 6);
    LOGR("city: outer test centre %d,%d target %d,%d slot %08X cell %08X", cx, cy, x, y, slot, cell);
}

namespace {

bool EnsureTemplates(uint8_t *base, uint32_t city, uint32_t manager) {
    if (g.templates_copied) return true;
    if (!g.slot_template)
        for (uint32_t i = 0; i < kSlotsCount; ++i) {
            const uint32_t slot = manager + kGameSlots + i * kSlotBytes;
            if (!SlotHasRoot(base, slot) && int32_t(X86_MEM32(slot + 0x04)) == kEmpty &&
                X86_MEM32(slot) != 0) {
                g.slot_template = slot;
                break;
            }
        }
    if (!g.cell_template)
        for (uint32_t i = 0; i < kSlotsCount; ++i) {
            const uint32_t cell = city + 0x4FA4 + i * kCellBytes;
            if (!X86_MEM8(cell + 0x88) && int32_t(X86_MEM32(cell + 0x04)) == kEmpty &&
                X86_MEM32(cell) != 0) {
                g.cell_template = cell;
                break;
            }
        }
    if (!g.slot_template || !g.cell_template) return false;
    // Snapshot the template contents. The game reinitialises its slot/cell objects on recentre and level
    // changes, so copying from the live object later would build ring objects from zeroed memory.
    std::memcpy(base + g.slot_tmpl_buf, base + g.slot_template, kSlotBytes);
    std::memcpy(base + g.cell_tmpl_buf, base + g.cell_template, kCellBytes);
    g.templates_copied = true;
    LOGR("city: ring templates snapshot slot %08X -> %08X cell %08X -> %08X", g.slot_template,
         g.slot_tmpl_buf, g.cell_template, g.cell_tmpl_buf);
    return true;
}

void InitRingObject(uint8_t *base, int idx) {
    const uint32_t slot = g.slots + uint32_t(idx) * kSlotBytes;
    const uint32_t cell = g.cells + uint32_t(idx) * kCellBytes;
    std::memcpy(base + slot, base + g.slot_tmpl_buf, kSlotBytes);
    std::memcpy(base + cell, base + g.cell_tmpl_buf, kCellBytes);
    for (uint32_t off = 0; off + 4 <= kSlotBytes; off += 4) {
        const uint32_t v = X86_MEM32(slot + off);
        if (v == g.slot_template) X86_MEM32(slot + off) = slot;
        if (v == g.slot_template + 0x70) X86_MEM32(slot + off) = slot + 0x70;
    }
    for (uint32_t off = 0; off + 4 <= kCellBytes; off += 4) {
        if (X86_MEM32(cell + off) == g.cell_template) X86_MEM32(cell + off) = cell;
    }
    // Clear stale resource pointers carried by the empty template; keep the vtable, self-pointer,
    // callback and size fields. Without this, several copies can point at one game resource.
    X86_MEM32(slot + 0x0C) = 0xFFFFFFFFu;
    X86_MEM32(slot + 0x10) = 0xFFFFFFFFu;
    for (uint32_t off : {0x14u, 0x18u, 0x1Cu, 0x24u, 0x30u, 0x34u, 0x50u, 0x54u, 0x60u})
        X86_MEM32(slot + off) = 0;
    X86_MEM32(cell + 0x1C) = 0;
    for (uint32_t off : {0x30u, 0x34u, 0x38u, 0x40u, 0x54u, 0x80u})
        X86_MEM32(cell + off) = 0;
    X86_MEM8(cell + 0x88) = 0;
    X86_MEM32(cell + 0x90) = 0xFFFFFFFFu;
    // +0x70/+0x74 is the cell's node in the [0x2E7C5C] list (cb +0x7C). The template was snapshotted from a
    // live game cell, so never inherit its next/prev: unlinking later would rewire the game's neighbours.
    X86_MEM32(cell + 0x70) = 0;
    X86_MEM32(cell + 0x74) = 0;
    X86_MEM32(slot + 0x2C) = 1;
    X86_MEM32(cell + 0x2C) = 1;
    g.ring_init[idx] = true;
}

} // namespace

uint32_t g_slot_clears = 0, g_slot_busy = 0;

void ClearRingCell(uint8_t *base, X86Context &ctx, uint32_t city, int idx) {
    const uint32_t cell = g.cells + uint32_t(idx) * kCellBytes;
    const uint32_t saved = ctx.ecx;
    ctx.ecx = cell;
    // Follow the game's own unload sequence (0x137600 state 4): reset the cell, release the stream
    // request, then unlink. Reset even when +0x88 is already clear: the game can unlink a cell first and
    // leave its +0x38 instance group behind, and the next load would then decrement a dead group.
    if (!g.keep_objects && (X86_MEM8(cell + 0x88) || X86_MEM32(cell + 0x38) || X86_MEM32(cell + 0x1C))) {
        kernel::CallGuest(ctx, TC_ADDR(0x13C1D0), {city});  // cell reset: drops the section/instance arrays
    }
    const uint32_t request = X86_MEM32(cell + 0x1C);
    if (request && !g.keep_objects) {
        kernel::CallGuest(ctx, TC_ADDR(0x24BAB0), {request});  // release the stream request and its object
        X86_MEM32(cell + 0x1C) = 0;
    }
    if (X86_MEM8(cell + 0x88) && !g.no_unlink) {
        kernel::CallGuest(ctx, TC_ADDR(0x13C180), {city});  // unlink from the loaded-cell list
    }
    ctx.ecx = saved;
    // 0x13C180 only unlinks while +0x88 is set, and the reset above can clear it first. Never zero a linked
    // node: splice it out ourselves, exactly as 0x13C180 does, so no neighbour keeps a pointer into the store.
    if (const uint32_t next = X86_MEM32(cell + 0x70)) {
        const uint32_t prev = X86_MEM32(cell + 0x74);
        // A null neighbour is the list head, not guest address 0.
        if (next) X86_MEM32(next + 4) = prev;
        if (prev) X86_MEM32(prev) = next;
    }
    X86_MEM32(cell + 0x80) = 0;
    X86_MEM32(cell + 0x0C) = 0xFFFFFFFFu;
    X86_MEM32(cell + 0x70) = 0;
    X86_MEM32(cell + 0x74) = 0;
    X86_MEM8(cell + 0x88) = 0;
    X86_MEM32(cell + 0x90) = 0xFFFFFFFFu;
    X86_MEM32(cell + 0x2C) = 1;
    g.ring_cell[idx] = false;
    g.ring_pending[idx] = false;
    g.ring_requested[idx] = false;

    // The tile's slot is deliberately left loaded. Releasing it here (tried 2026-11-15) left game slots linked to an
    // empty ring slot: the portal pass 0x13E7F0 then walked into it and crashed on its null portal table (+0x54) at
    // 0x13EB1E. The ring re-targets the slot through 0x137600, which unloads and unlinks it the game's own way.
}

void OnGameMove(uint8_t *base, X86Context &ctx, uint32_t city, uint32_t game_object, int32_t target_x,
                int32_t target_y) {
    if (!g.ready || !g.ring) return;
    const int32_t old_x = int32_t(X86_MEM32(game_object + 0x04));
    const int32_t old_y = int32_t(X86_MEM32(game_object + 0x08));
    const bool old_valid = old_x != kEmpty && old_y != kEmpty;
    const bool new_valid = target_x != kEmpty && target_y != kEmpty;
    // Cells already use draw coordinates; slots and their targets are six tiles
    // further along y. Applying +6 to a cell missed every ring tile around it.
    const bool game_cell = game_object >= city + 0x4FA4 && game_object < city + 0x4FA4 + 9 * kCellBytes;
    const int32_t old_cell_y = old_y + (game_cell ? 0 : 6);
    const int32_t new_cell_y = target_y + (game_cell ? 0 : 6);
    for (int idx = 0; idx < g.n * g.n && idx < 128; ++idx) {
        if (!g.ring_init[idx]) continue;
        const uint32_t cell = g.cells + uint32_t(idx) * kCellBytes;
        const int32_t cx = int32_t(X86_MEM32(cell + 0x04)), cy = int32_t(X86_MEM32(cell + 0x08));
        if ((old_valid && cx == old_x && cy == old_cell_y) || (new_valid && cx == target_x && cy == new_cell_y))
            g.ring_pending[idx] = true;  // unload from the draw boundary, not inside the game's state machine
    }
}

void EnforceNoOverlap(uint8_t *base, X86Context &ctx, uint32_t city) {
    for (int idx = 0; idx < g.n * g.n && idx < 128; ++idx) {
        if (!g.ring_init[idx] || !g.ring_cell[idx]) continue;
        const uint32_t cell = g.cells + uint32_t(idx) * kCellBytes;
        const int32_t cx = int32_t(X86_MEM32(cell + 0x04)), cy = int32_t(X86_MEM32(cell + 0x08));
        bool blocked = false;
        for (int i = 0; i < 27; ++i)
            if (g.game_lock[i] && cx == g.game_x[i] && cy == g.game_y[i]) {
                blocked = true;
                break;
            }
        if (blocked) ClearRingCell(base, ctx, city, idx);
    }
}

// Remember what each game cell, slot and request record holds. The tile is locked from the moment the game
// first names it until the game drops its object, so the ring cannot grab a tile the game is using or about
// to use. Slots and requests are important: they name the next tile before the draw cell updates, and that
// window is enough for the ring to share the game's tile/instance group.
void UpdateGameTiles(uint8_t *base, uint32_t city, uint32_t manager) {
    if (g.game_city != city) {
        const bool first = g.game_city == 0;
        g.game_city = city;
        g.game_tracked = false;
        if (!first) {
            // A different city object means the store cells point at the old one. Reset the store before
            // driving anything; Init keeps the grid size and re-snapshots the templates.
            LOGR("city: ring city changed to %08X; resetting grid store", city);
            Init(base, city + 0xD0);
        }
    }
    for (int i = 0; i < 9; ++i) {
        const uint32_t cell = city + 0x4FA4 + uint32_t(i) * kCellBytes;
        const int32_t x = int32_t(X86_MEM32(cell + 0x04));
        const int32_t y = int32_t(X86_MEM32(cell + 0x08));
        const uint32_t obj = X86_MEM32(cell + 0x80);
        if (x != kEmpty && y != kEmpty) {
            g.game_x[i] = x;
            g.game_y[i] = y;
            g.game_lock[i] = true;
        } else if (obj) {
            g.game_lock[i] = true;  // unloading: the tile is cleared but the object is still owned
        } else {
            g.game_lock[i] = false;
        }
    }
    for (int i = 0; i < 9; ++i) {
        // Slot targets are slot coordinates; the draw cell is six tiles further along y.
        const uint32_t slot = manager + kGameSlots + uint32_t(i) * kSlotBytes;
        const int32_t x = int32_t(X86_MEM32(slot + 0x04));
        const int32_t y = int32_t(X86_MEM32(slot + 0x08));
        const int at = 9 + i;
        if (x != kEmpty && y != kEmpty) {
            g.game_x[at] = x;
            g.game_y[at] = y + 6;
            g.game_lock[at] = true;
        } else if (SlotHasRoot(base, slot)) {
            g.game_lock[at] = true;  // still loaded while the target is cleared
        } else {
            g.game_lock[at] = false;
        }
    }
    for (int i = 0; i < 9; ++i) {
        // Request records already use draw-cell coordinates.
        const uint32_t req = manager + kGameRequests + uint32_t(i) * kRequestBytes;
        const int32_t x = int32_t(X86_MEM32(req + 0x04));
        const int32_t y = int32_t(X86_MEM32(req + 0x08));
        const int at = 18 + i;
        if (x != kEmpty && y != kEmpty) {
            g.game_x[at] = x;
            g.game_y[at] = y;
            g.game_lock[at] = true;
        } else {
            g.game_lock[at] = false;
        }
    }
    g.game_tracked = true;
}

bool GameOwnsTile(int32_t x, int32_t y) {
    for (int i = 0; i < 27; ++i)
        if (g.game_lock[i] && g.game_x[i] == x && g.game_y[i] == y) return true;
    return false;
}

bool GameBusy(uint8_t *base, uint32_t city, uint32_t manager) {
    // The tile loader uses shared scratch streams (ids 0x40/0x41). While any game slot or cell is
    // actively loading or unloading, the ring must not drive. State 3 is the steady update state and is
    // not a transition.
    auto loading = [](uint32_t state) { return state == 1 || state == 2 || state == 4; };
    for (int i = 0; i < 9; ++i) {
        const uint32_t cell = city + 0x4FA4 + uint32_t(i) * kCellBytes;
        if (loading(X86_MEM32(cell + 0x24))) return true;
    }
    for (int i = 0; i < 9; ++i) {
        const uint32_t slot = manager + kGameSlots + uint32_t(i) * kSlotBytes;
        if (loading(X86_MEM32(slot + 0x24))) return true;
    }
    return false;
}

void ShutdownRing(uint8_t *base, X86Context &ctx, uint32_t city) {
    int cleared = 0;
    for (int idx = 0; idx < g.n * g.n && idx < 128; ++idx) {
        if (!g.ring_init[idx] && !g.ring_cell[idx] && !g.ring_pending[idx]) continue;
        ClearRingCell(base, ctx, city, idx);
        ++cleared;
    }
    g.ring = false;
    g.ring_center = false;
    g.last_radius = 0;
    g.ring_rr = 0;
    LOGR("city: ring shut down, %d cell(s) cleared", cleared);
}

void DriveRing(uint8_t *base, X86Context &ctx, uint32_t city, uint32_t manager) {
    if (!g.orphan_requests.empty()) {  // requests dropped by a city reset (see Init)
        for (const uint32_t req : g.orphan_requests) kernel::CallGuest(ctx, TC_ADDR(0x24BAB0), {req});
        LOGR("city: released %zu ring stream requests left by a city reset", g.orphan_requests.size());
        g.orphan_requests.clear();
    }
    if (g.ready && g.ring && !Ring()) {
        // The City draw distance went back to Normal: remove every ring cell and
        // link instead of leaving stale ones behind.
        ShutdownRing(base, ctx, city);
        return;
    }
    if (!g.ready || !g.ring) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - g.start < std::chrono::seconds(5)) return;
    UpdateGameTiles(base, city, manager);
    for (int idx = 0; idx < g.n * g.n && idx < 128; ++idx)
        if (g.ring_pending[idx]) ClearRingCell(base, ctx, city, idx);
    EnforceNoOverlap(base, ctx, city);

    if (now - g.last_drive < std::chrono::milliseconds(10)) return;
    g.last_drive = now;
    static const bool static_ring = DevEnv("XBE_CITY_RING_STATIC") != nullptr;
    if (static_ring && now - g.start > std::chrono::seconds(20)) return;
    if (GameBusy(base, city, manager)) return;
    if (!EnsureTemplates(base, city, manager)) return;
    static bool locks_logged = false;
    static const bool diag = DevEnv("XBE_CITY_DIAG") != nullptr;
    if (!locks_logged && diag) {
        locks_logged = true;
        for (int i = 0; i < 27; ++i)
            if (g.game_lock[i])
                LOGR("ring: lock %d tile %d,%d", i, g.game_x[i], g.game_y[i]);
        LOGR("ring: center slot %d,%d", g.ring_cx, g.ring_cy);
    }
    // The game can walk every store slot/cell, not just the ones the ring drives. Initialise all of them so
    // any stale reference still has a valid vtable and object layout.
    for (int i = 0; i < g.n * g.n && i < 128; ++i)
        if (!g.ring_init[i]) InitRingObject(base, i);

    int32_t min_x = 0, max_x = 0, min_y = 0, max_y = 0;
    bool have = false;
    for (uint32_t i = 0; i < kSlotsCount; ++i) {
        const uint32_t slot = manager + kGameSlots + i * kSlotBytes;
        if (!SlotHasRoot(base, slot)) continue;
        const int32_t x = int32_t(X86_MEM32(slot + 0x04)), y = int32_t(X86_MEM32(slot + 0x08));
        if (!have) {
            min_x = max_x = x;
            min_y = max_y = y;
            have = true;
        } else {
            min_x = std::min(min_x, x);
            max_x = std::max(max_x, x);
            min_y = std::min(min_y, y);
            max_y = std::max(max_y, y);
        }
    }
    if (!have) return;
    if (!g.ring_freeze || !g.ring_center) {
        g.ring_cx = (min_x + max_x) / 2;
        g.ring_cy = (min_y + max_y) / 2;
        g.ring_center = true;
    }
    const int32_t cx = g.ring_cx, cy = g.ring_cy;

    // 2 for 5x5, 3 for 7x7, 4 for 9x9: from the store, or live from the City draw distance setting
    int radius = (g.n - 1) / 2;
    if (g.from_setting) {
        const int range = int(std::atomic_ref(app::Get().city_range).load(std::memory_order_relaxed));
        radius = std::min(radius, range + 1);
    }
    if (radius != g.last_radius) {  // the setting changed: release the ring cells and drive the new size
        if (g.last_radius)
            for (int i = 0; i < g.n * g.n && i < 128; ++i)
                if (g.ring_init[i]) ClearRingCell(base, ctx, city, i);
        LOGR("city: ring radius %d -> %d", g.last_radius, radius);
        g.last_radius = radius;
    }
    const int max_outer = g.n * g.n - 9;
    static const int requested = [] {
        const char *value = DevEnv("XBE_CITY_RING_LIMIT");
        return value ? std::atoi(value) : 0;
    }();
    const int drive_limit = requested > 0 ? std::min(requested, max_outer) : max_outer;
    auto call = [&](uint32_t object, int32_t tx, int32_t ty) {
        const uint32_t saved = ctx.ecx;
        ctx.ecx = object;
        kernel::CallGuest(ctx, TC_ADDR(0x137600), {city, uint32_t(tx), uint32_t(ty)});
        ctx.ecx = saved;
    };
    std::memset(g.ring_cell, 0, sizeof(g.ring_cell));
    int driven = 0;
    int new_tiles = 0;
    // One tile load in flight at a time: clear the pending mark once its cell reports loaded, or give up
    // after a few seconds so a load that never lands cannot stall the ring.
    if (g.ring_pending_idx >= 0) {
        const int p = g.ring_pending_idx;
        const bool valid = p < g.n * g.n && g.ring_init[p];
        const bool loaded = valid && X86_MEM8(g.cells + uint32_t(p) * kCellBytes + 0x88);
        if (!valid || loaded || ++g.ring_pending_wait > 180) {
            g.ring_pending_idx = -1;
            g.ring_pending_wait = 0;
        }
    }
    // Two new tiles a drive (the drive runs at most once a frame), and only after the previous one has
    // landed: a recentre used to ask for eight per drive, which filled the ring in ~9 frames but cost a
    // burst of ~80 MB of reads and construction inside ~150 ms — the hitch when a boundary is crossed or
    // the draw distance is raised. Slower fill only costs detail on far cells for a second or so.
    constexpr int kNewTilesPerDrive = 2;
    // Walk the closest shell first. Keep polling already requested tiles even
    // after the new-request budget has been used for this update.
    for (int shell = 2; shell <= radius && driven < drive_limit; ++shell) {
        for (int dx = -shell; dx <= shell && driven < drive_limit; ++dx) {
            for (int dy = -shell; dy <= shell && driven < drive_limit; ++dy) {
                if (std::max(std::abs(dx), std::abs(dy)) != shell) continue;
                const int32_t x = cx + dx, y = cy + dy;
                const int idx = SideIndex(x, y, g.n);
                if (GameOwnsTile(x, y + 6)) {
                    if (g.ring_init[idx]) ClearRingCell(base, ctx, city, idx);
                    continue;
                }
                if (g.ring_blocked[idx]) continue;
                static const int min_x_allowed = [] {
                    const char* value = DevEnv("XBE_CITY_RING_MINX");
                    return value ? std::atoi(value) : INT32_MIN;
                }();
                if (x < min_x_allowed) {
                    if (g.ring_init[idx]) ClearRingCell(base, ctx, city, idx);
                    continue;
                }
                if (!g.ring_init[idx]) InitRingObject(base, idx);
                const uint32_t slot = g.slots + uint32_t(idx) * kSlotBytes;
                const uint32_t cell = g.cells + uint32_t(idx) * kCellBytes;
                if (!g.ring_requested[idx] || g.ring_target_x[idx] != x || g.ring_target_y[idx] != y) {
                    if (new_tiles >= kNewTilesPerDrive || g.ring_pending_idx >= 0) continue;
                    ++new_tiles;
                    g.ring_requested[idx] = true;
                    g.ring_pending_idx = idx;
                    g.ring_pending_wait = 0;
                    g.ring_target_x[idx] = x;
                    g.ring_target_y[idx] = y;
                }
                X86_MEM32(slot + 0x2C) = 1;
                X86_MEM32(cell + 0x2C) = 1;
                if (!g.ring_cellonly && !g.ring_nodrive) call(slot, x, y);
                if (!g.ring_slotonly && !g.ring_nodrive) call(cell, x, y + 6);
                if (g.ring_nodrive) continue;
                g.ring_cell[idx] = true;
                ++driven;
            }
        }
    }
}
void NodeGuard(uint8_t *base, const char *tag);

void DrawRing(uint8_t *base, X86Context &ctx, uint32_t camera) {
    if (!g.ready || !g.ring || g.ring_nodraw) return;
    const auto now = std::chrono::steady_clock::now();

    if (now - g.last_draw < std::chrono::milliseconds(10)) return;
    g.last_draw = now;
    const int count = g.n * g.n;
    for (int idx = 0; idx < count && idx < 128; ++idx) {
        if (!g.ring_cell[idx]) continue;
        const uint32_t cell = g.cells + uint32_t(idx) * kCellBytes;
        if (!X86_MEM8(cell + 0x88)) continue;
        if (X86_MEM32(cell + 0x90) == 0xFFFFFFFFu) continue;
        const uint32_t saved = ctx.ecx;
        static const bool trace = DevEnv("XBE_CITY_NODEGUARD") != nullptr;
        const uint32_t p70 = X86_MEM32(cell + 0x70), p74 = X86_MEM32(cell + 0x74);
        static uint32_t pre[kCellBytes / 4 + 4];
        uint32_t guard_before = 0;
        if (trace) {
            for (uint32_t w = 0; w < kCellBytes / 4; ++w) pre[w] = X86_MEM32(cell + w * 4);
            guard_before = X86_MEM32(TC_ADDR(0x2EA7E8u)) ^ X86_MEM32(TC_ADDR(0x2EA860u));
        }
        ::g_ring_draw_idx = idx;
        ctx.ecx = cell;
        kernel::CallGuest(ctx, TC_ADDR(0x13C4C0), {camera});
        ::g_ring_draw_idx = -1;
        ctx.ecx = saved;
        if (!trace) continue;
        char tag[128];
        std::snprintf(tag, sizeof tag, "ringcell %d cell %08X links %08X/%08X -> %08X/%08X arg %08X", idx, cell, p70,
                      p74, X86_MEM32(cell + 0x70), X86_MEM32(cell + 0x74), X86_MEM32(cell + 0x78));
        if ((X86_MEM32(TC_ADDR(0x2EA7E8u)) ^ X86_MEM32(TC_ADDR(0x2EA860u))) != guard_before) {
            for (uint32_t w = 0; w < kCellBytes / 4; w += 4)
                LOGR("ringcell pre +%02X: %08X %08X %08X %08X", w * 4, pre[w], pre[w + 1], pre[w + 2], pre[w + 3]);
            const uint32_t obj = pre[0x80 / 4], req = pre[0x1C / 4];
            const uint32_t gcells = X86_MEM32(TC_ADDR(0x37F8FCu)) + 0x4FA4;
            for (int i = 0; i < 9; ++i) {
                const uint32_t c = gcells + uint32_t(i) * kCellBytes;
                if (X86_MEM32(c + 0x80) == obj || X86_MEM32(c + 0x1C) == req || X86_MEM32(c + 0x38) == pre[0x38 / 4])
                    LOGR("ringcell SHARED with game cell %d %08X (obj %08X req %08X grp %08X tile %d,%d)", i, c,
                         X86_MEM32(c + 0x80), X86_MEM32(c + 0x1C), X86_MEM32(c + 0x38), X86_MEM32(c + 4),
                         X86_MEM32(c + 8));
            }
            for (int j = 0; j < count && j < 128; ++j) {
                const uint32_t c = g.cells + uint32_t(j) * kCellBytes;
                if (j != idx && g.ring_cell[j] && (X86_MEM32(c + 0x80) == obj || X86_MEM32(c + 0x1C) == req))
                    LOGR("ringcell SHARED with ring cell %d", j);
            }
            LOGR("ringcell tile %d,%d req %08X reqword0 %08X obj0 %08X", pre[1], pre[2], req,
                 req ? X86_MEM32(req) : 0, obj ? X86_MEM32(obj) : 0);
        }
        ::NodeGuard(base, tag);
    }
}

namespace {

void ScanRange(uint8_t *base, uint32_t from, uint32_t to, uint32_t value, const char *what, uint32_t owner) {
    int found = 0;
    for (uint32_t a = from; a < to; a += 4) {
        if (X86_MEM32(a) != value) continue;
        LOGR("city: %s %08X (owner %08X) referenced at %08X", what, value, owner, a);
        if (++found >= 8) break;
    }
}

// Find where the store slots/cells and their scene-node objects are registered: a hit in a fixed table reveals
// the 16-entry pool that limits the ring.
void ScanReferences(uint8_t *base) {
    for (int idx = 0; idx < g.n * g.n && idx < 128; ++idx) {
        if (!g.ring_init[idx]) continue;
        const uint32_t slot = g.slots + uint32_t(idx) * kSlotBytes;
        const uint32_t cell = g.cells + uint32_t(idx) * kCellBytes;
        const uint32_t object = X86_MEM32(cell + 0x80);
        const uint32_t block = X86_MEM32(slot + 0x30);
        ScanRange(base, 0x002D0000, 0x00580000, slot, "slot", slot);
        ScanRange(base, 0x002D0000, 0x00580000, cell, "cell", cell);
        if (object) ScanRange(base, 0x002D0000, 0x00580000, object, "object", cell);
        if (block) ScanRange(base, 0x002D0000, 0x00580000, block, "block", slot);
    }
}

} // namespace

void Init(uint8_t *base, uint32_t manager) {
    static const char *env = [] {
        const char *value = DevEnv("XBE_CITY_GRID");
        return value;
    }();
    // The in-game City draw distance setting allocates a 9x9 store the first time it is above Normal; the
    // setting then drives the ring live (Ring(), DriveRing). XBE_CITY_GRID keeps the old fixed configuration.
    const uint32_t range = std::atomic_ref(app::Get().city_range).load(std::memory_order_relaxed);
    if (!env && range == 0) return;
    if (g.attempted && !g.ready) return;
    g.attempted = true;
    g.from_setting = !env;
    int n = env ? std::atoi(env) : 9;
    if (n != 3 && n != 5 && n != 7 && n != 9) {
        LOGR("city: grid store: XBE_CITY_GRID=%s is not 3/5/7/9; using 5", env);
        n = 5;
    }

    if (g.ready) {
        if (g.n != n) {
            LOGR("city: grid store already %dx%d, ignoring request for %dx%d", g.n, g.n, n, n);
            return;
        }
        g.manager = manager;
        // Do not memset the store here. The game's lists can still hold pointers to store slots/cells from
        // before the city change; zeroing them makes the next 0x137600 call hit a null vtable. The objects
        // are re-initialised from the new templates when the ring next drives them.
        g.start = g.last = g.last_log = g.last_move = std::chrono::steady_clock::now();
        g.syncs = g.tests = g.mismatches = 0;
        g.templates_copied = false;
        g.slot_template = g.cell_template = 0;
        // The memset zeroed the store objects, so every ring object must be rebuilt from the templates.
        // Without this, ring_init still says "initialized" and the driver feeds zeroed cells (vtable 0)
        // into 0x137600.
        // The rebuild copies the template over every ring slot/cell, which would drop the stream request each
        // one still holds at +0x1C (one leaked request per object per city reset: ~4 MB/min at 9x9 on the
        // attract screen). Collect them here; DriveRing releases them with 0x24BAB0 (Init has no guest context).
        for (int idx = 0; idx < n * n && idx < 128; ++idx) {
            if (!g.ring_init[idx]) continue;
            for (const uint32_t obj : {g.slots + uint32_t(idx) * kSlotBytes, g.cells + uint32_t(idx) * kCellBytes}) {
                if (const uint32_t req = X86_MEM32(obj + 0x1C)) {
                    g.orphan_requests.push_back(req);
                    X86_MEM32(obj + 0x1C) = 0;
                }
            }
        }
        std::memset(g.ring_init, 0, sizeof(g.ring_init));
        std::memset(g.ring_cell, 0, sizeof(g.ring_cell));
        std::memset(g.ring_pending, 0, sizeof(g.ring_pending));
        std::memset(g.ring_requested, 0, sizeof(g.ring_requested));
        std::memset(g.ring_blocked, 0, sizeof(g.ring_blocked));
        std::memset(g.game_lock, 0, sizeof(g.game_lock));
        g.game_tracked = false;
        g.ring_center = false;
        g.ring_cx = g.ring_cy = 0;
        g.ring_rr = 0;
        LOGR("city: grid store %dx%d reset at %08X", n, n, g.base);
        return;
    }

    const uint32_t slots = uint32_t(n * n) * kSlotBytes;
    const uint32_t total = slots + uint32_t(n * n) * (kCellBytes + kRequestBytes + kTableBytes) +
                           kSlotBytes + kCellBytes + 0x40;
    // Allocate the store from the top of user space. A low first-fit address can land inside the game's own
    // heap, which then overwrites the store (and the store overwrites the game) after a minute or two.
    static const uint32_t alloc_flags = [] {
        uint32_t flags = mem::kMemReserve | mem::kMemCommit;
        if (!DevEnv("XBE_CITY_STORE_LOW")) flags |= mem::kMemTopDown;
        return flags;
    }();
    uint32_t at = 0, size = total;
    if (mem::AllocateVirtual(at, size, alloc_flags, 0x04) != 0 || !at) {
        LOGR("city: grid store %dx%d could not allocate %u KB", n, n, total / 1024);
        return;
    }

    g.base = at;
    g.bytes = size;
    g.slots = at;
    g.cells = Align16(g.slots + slots);
    g.requests = Align16(g.cells + uint32_t(n * n) * kCellBytes);
    g.table = Align16(g.requests + uint32_t(n * n) * kRequestBytes);
    g.slot_tmpl_buf = Align16(g.table + uint32_t(n * n) * kTableBytes);
    g.cell_tmpl_buf = Align16(g.slot_tmpl_buf + kSlotBytes);
    g.n = n;
    g.manager = manager;
    g.test = DevEnv("XBE_CITY_MIRROR_TEST") != nullptr;
    g.cell_test = DevEnv("XBE_CITY_CELL_TEST") != nullptr;
    g.ring = DevEnv("XBE_CITY_RING") != nullptr || DevEnv("XBE_CITY_RING_DIAG") != nullptr ||
             DevEnv("XBE_CITY_DIAG") != nullptr || g.from_setting;
    LOGR("city: slot type %08X -> %08X %08X %08X %08X; cell type %08X -> %08X %08X %08X %08X", TC_ADDR(0x2D9C44),
         X86_MEM32(TC_ADDR(0x2D9C44)), X86_MEM32(TC_ADDR(0x2D9C48)), X86_MEM32(TC_ADDR(0x2D9C4C)), X86_MEM32(TC_ADDR(0x2D9C50)), TC_ADDR(0x2D9E0C),
         X86_MEM32(TC_ADDR(0x2D9E0C)), X86_MEM32(TC_ADDR(0x2D9E10)), X86_MEM32(TC_ADDR(0x2D9E14)), X86_MEM32(TC_ADDR(0x2D9E18)));
    g.ring_freeze = DevEnv("XBE_CITY_RING_FREEZE") != nullptr;
    g.ring_nodraw = DevEnv("XBE_CITY_RING_NODRAW") != nullptr;
    g.ring_slotonly = DevEnv("XBE_CITY_RING_SLOTONLY") != nullptr;
    g.ring_cellonly = DevEnv("XBE_CITY_RING_CELLONLY") != nullptr;
    g.ring_nodrive = DevEnv("XBE_CITY_RING_NODRIVE") != nullptr;
    g.keep_objects = DevEnv("XBE_CITY_KEEP_OBJECTS") != nullptr;
    g.no_unlink = DevEnv("XBE_CITY_NO_CLEAR") != nullptr;
    std::memset(base + at, 0, size);
    g.start = g.last = g.last_log = g.last_move = std::chrono::steady_clock::now();
    g.ready = true;
    LOGR("city: grid store %dx%d at %08X (%u KB): slots %08X cells %08X requests %08X table %08X", n, n, at,
         size / 1024, g.slots, g.cells, g.requests, g.table);
    SyncFromGame(base);
}

void MaybeSync(uint8_t *base) {
    if (!g.ready) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - g.last < std::chrono::milliseconds(100)) return;
    g.last = now;
    if (!g.cell_test && !g.ring && !g.from_setting) {  // the mirror aliases ring indices: never with a ring store
        SyncFromGame(base);
        ++g.syncs;
    }

    static const bool scan_diag = DevEnv("XBE_CITY_DIAG") != nullptr;
    if (scan_diag && g.ring && !g.scanned && now - g.start > std::chrono::milliseconds(5500)) {
        g.scanned = true;
        ScanReferences(base);
    }
    if (g.test && g.tests < 3 && now - g.start >= std::chrono::seconds(10 * (g.tests + 1))) {
        ++g.tests;
        MirrorTest(base);
    }
    // The 5 s summary scans the whole commit bitmap (mem::Query), so it is diagnostic only: an
    // unconditional line every 5 s in a release build is a stall nobody asked for.
    static const bool log_city = DevEnv("XBE_TRACE_CITY") != nullptr;
    if (log_city && now - g.last_log >= std::chrono::seconds(5)) {
        g.last_log = now;
        const mem::Stats stats = mem::Query();
        {
            static uint64_t last_draws = 0;
            const uint64_t draws = ::XbeDrawCalls().load(std::memory_order_relaxed);
            LOGR("city: draw calls %llu per 5 s", static_cast<unsigned long long>(draws - last_draws));
            last_draws = draws;
        }
        static int dumps = 0;
        if (++dumps % 12 == 0) mem::DumpContigLedger();
        LOGR("city: grid store %dx%d: %u syncs, %u tests, %u mismatches, physical %u/%u KB, ring %d", g.n, g.n,
             g.syncs, g.tests, g.mismatches, uint32_t(uint64_t(stats.contiguous_used) * mem::kPage / 1024),
             uint32_t(mem::kPhysicalSize / 1024), g.n * g.n - 9);
        if (g.ring) {
            int loaded = 0;
            for (int idx = 0; idx < g.n * g.n && idx < 128; ++idx)
                if (g.ring_cell[idx] &&
                    X86_MEM8(g.cells + uint32_t(idx) * kCellBytes + 0x88))
                    ++loaded;
            int slots = 0, slot_req = 0, cell_req = 0;
            for (int idx = 0; idx < g.n * g.n && idx < 128; ++idx) {
                const uint32_t s = g.slots + uint32_t(idx) * kSlotBytes, c = g.cells + uint32_t(idx) * kCellBytes;
                slots += SlotHasRoot(base, s) ? 1 : 0;
                slot_req += X86_MEM32(s + 0x1C) ? 1 : 0;
                cell_req += X86_MEM32(c + 0x1C) ? 1 : 0;
            }
            LOGR("city: ring %d/%d cells loaded; store slots loaded %d, slot req %d, cell req %d; slot clears %u "
                 "busy %u",
                 loaded, std::max(1, g.n * g.n - 9), slots, slot_req, cell_req, g_slot_clears, g_slot_busy);
        }
        if (g.cell_test && g.test_slot) {
            const uint32_t slot = g.test_slot, cell = g.test_cell;
            LOGR("city: outer test slot loaded %u xy %d,%d state %u vt %08X cell loaded %u xy %d,%d obj %08X",
                 SlotHasRoot(base, slot), int32_t(X86_MEM32(slot + 0x04)), int32_t(X86_MEM32(slot + 0x08)),
                 X86_MEM32(slot + 0x24), X86_MEM32(slot), X86_MEM8(cell + 0x88),
                 int32_t(X86_MEM32(cell + 0x04)), int32_t(X86_MEM32(cell + 0x08)), X86_MEM32(cell + 0x80));
        }
    }
}

bool Ready() { return g.ready; }
int N() { return g.n; }
uint32_t Base() { return g.base; }
uint32_t Slots() { return g.slots; }
uint32_t Cells() { return g.cells; }
uint32_t Requests() { return g.requests; }
uint32_t Table() { return g.table; }
bool RingCell(int idx) { return idx >= 0 && idx < 128 && g.ring_cell[idx]; }
bool InInnerDetailZone(uint8_t *base, uint32_t slot) {
    const int64_t dx = int64_t(int32_t(X86_MEM32(slot + 0x04))) - g.ring_cx;
    const int64_t dy = int64_t(int32_t(X86_MEM32(slot + 0x08))) - g.ring_cy;
    return g.ring_center && dx >= -2 && dx <= 2 && dy >= -2 && dy <= 2;
}
bool OuterDetailAvailable() {
    static const bool fixed_outer_ring = [] {
        const char* grid = DevEnv("XBE_CITY_GRID");
        return grid && (std::atoi(grid) == 7 || std::atoi(grid) == 9) &&
               (DevEnv("XBE_CITY_RING") || DevEnv("XBE_CITY_RING_DIAG") || DevEnv("XBE_CITY_DIAG"));
    }();
    return fixed_outer_ring || std::atomic_ref(app::Get().city_range).load(std::memory_order_relaxed) >= 2;
}

bool Ring() {
    if (!g.ready) return false;
    if (!g.from_setting) return g.ring;
    return std::atomic_ref(app::Get().city_range).load(std::memory_order_relaxed) > 0;
}

} // namespace game::city_grid
