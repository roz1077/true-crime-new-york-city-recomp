// Keep the named-object table and its child nodes consistent across rebuilds.
//
// The game keeps a heap table of 0xB4-byte named-object records (base
// 0x351544, count 0x351558).  Each record may own a 0xD0-byte transform node
// at +0xA8; that node points back at its record through +0xC0 and is also
// registered as a message receiver.  On a level change the table is rebuilt
// (sub_000703D0 overwrites the globals with a freshly allocated table) and the
// world reset clears only the forward link (sub_00068300 sets [record+0xA8]
// to 0).  The nodes keep their back-pointer to the old record, which the
// allocator then reuses or frees: a later pad/message dispatch dereferences
// the dead record (the 2026-11-03 crash in sub_00074600).
//
// Detach both ends when either function runs, so a node never outlives its
// record.  The node's own callback handles a null parent by design, so it
// keeps working until the object that owns it is rebuilt.

#include <cstdint>

#include "core/log.h"
#include "kernel/kernel.h"
#include "level_generation.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

namespace {

constexpr uint32_t kTableBase = 0x351544;  // allocated by sub_000703D0
constexpr uint32_t kTableCount = 0x351558; // records in the table
constexpr uint32_t kRecordSize = 0xB4;
constexpr uint32_t kRecordChild = 0xA8;   // record + 0xA8 -> child node
constexpr uint32_t kChildParent = 0xC0;   // child node + 0xC0 -> record

void DetachChildren(uint8_t *base) {
    const int32_t count = int32_t(X86_MEM32(kTableCount));
    const uint32_t table = X86_MEM32(kTableBase);
    if (count <= 0 || !table || !mem::IsCommitted(table)) return;
    for (int32_t i = 0; i < count; i++) {
        const uint32_t record = table + uint32_t(i) * kRecordSize;
        if (!mem::IsCommitted(record + kRecordChild)) break;
        const uint32_t child = X86_MEM32(record + kRecordChild);
        if (child && mem::IsCommitted(child + kChildParent) && X86_MEM32(child + kChildParent) == record)
            X86_MEM32(child + kChildParent) = 0;
        X86_MEM32(record + kRecordChild) = 0;
    }
}

} // namespace

namespace game {
std::atomic<uint32_t> g_level_generation{0};
}

// world reset: clears [record+0xA8] for every record
X86_EXTERN_FUNC(__imp__sub_00068300);
X86_FUNC(sub_00068300) {
    DetachChildren(base);
    game::g_level_generation.fetch_add(1, std::memory_order_relaxed);
    __imp__sub_00068300(ctx, base);
}

// level load: allocates the new table and overwrites base/count
X86_EXTERN_FUNC(__imp__sub_000703D0);
X86_FUNC(sub_000703D0) {
    DetachChildren(base);
    game::g_level_generation.fetch_add(1, std::memory_order_relaxed);
    __imp__sub_000703D0(ctx, base);
}
