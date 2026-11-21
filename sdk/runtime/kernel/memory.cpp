#include "kernel/memory.h"
#include "core/dev_env.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include "core/log.h"
#include "kernel/kernel.h"

#include <array>
#include <unordered_map>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace mem {

static uint8_t* g_base = nullptr;
static std::recursive_mutex g_lock;
// bumped under g_lock by every call that changes what AllocationSize answers; its per-thread cache checks it
static std::atomic<uint64_t> g_layout{1};
static void LayoutChanged() { g_layout.fetch_add(1, std::memory_order_release); }

uint8_t* Base() { return g_base; }

// physical memory shows at 0x80000000 and again at 0xF0000000 (write combined view)
static bool InPhysical(uint32_t va) {
    return (va >= kContiguousBase && va < kContiguousBase + kPhysicalSize) ||
           (va >= kWriteCombineBase && va < kWriteCombineBase + kPhysicalSize);
}

static bool OverlapsPhysical(uint32_t va, uint64_t end) {
    return (va < kContiguousBase + kPhysicalSize && end > kContiguousBase) ||
           (va < kWriteCombineBase + kPhysicalSize && end > kWriteCombineBase);
}

bool Init() {
    // Guest VAs are 32-bit. Leave slack beyond the two mapped windows when probing the host address space.
    const uint64_t size = (1ull << 32) + kPhysicalSize;
#ifdef _WIN32
    HANDLE ram = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, kPhysicalSize, nullptr);
    // a fixed base keeps crash dumps comparable between runs
    for (uint64_t hint : {0x100000000ull, 0x200000000ull, 0x300000000ull, 0ull}) {
        uint8_t* b = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(hint), size, MEM_RESERVE, PAGE_NOACCESS));
        if (!b) continue;
        if (!VirtualFree(b, 0, MEM_RELEASE)) continue;
        // Reserve all non-aliased guest addresses, then map the same memory into both physical windows.
        const uint64_t cuts[][2] = {{0, kContiguousBase},
                                    {kContiguousBase + kPhysicalSize, kWriteCombineBase},
                                    {kWriteCombineBase + kPhysicalSize, 1ull << 32}};
        bool ok = ram != nullptr;
        std::array<void*, std::size(cuts)> reservations{};
        for (size_t ci = 0; ok && ci < reservations.size(); ++ci) {
            const auto& c = cuts[ci];
            if (c[1] <= c[0]) continue;
            reservations[ci] = VirtualAlloc(b + c[0], size_t(c[1] - c[0]), MEM_RESERVE, PAGE_NOACCESS);
            ok = reservations[ci] == b + c[0];
        }
        ok = ok && MapViewOfFileEx(ram, FILE_MAP_ALL_ACCESS, 0, 0, kPhysicalSize, b + kContiguousBase) == b + kContiguousBase;
        ok = ok && MapViewOfFileEx(ram, FILE_MAP_ALL_ACCESS, 0, 0, kPhysicalSize, b + kWriteCombineBase) == b + kWriteCombineBase;
        if (!ok) LOGE("memory: candidate base %p failed (mapping %p), err %lu", static_cast<void*>(b),
                      static_cast<void*>(ram), GetLastError());
        if (ok) {
            g_base = b;
            break;
        }
        for (void* region : reservations)
            if (region) VirtualFree(region, 0, MEM_RELEASE);
        UnmapViewOfFile(b + kContiguousBase);
        UnmapViewOfFile(b + kWriteCombineBase);
    }
    if (ram) CloseHandle(ram);
#else
    void* p = mmap(reinterpret_cast<void*>(0x100000000ull), size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    g_base = p == MAP_FAILED ? nullptr : static_cast<uint8_t*>(p);
    int fd = g_base ? memfd_create("ram", 0) : -1;
    if (fd < 0 || ftruncate(fd, kPhysicalSize) ||
        mmap(g_base + kContiguousBase, kPhysicalSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED ||
        mmap(g_base + kWriteCombineBase, kPhysicalSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED) {
        if (g_base) munmap(g_base, size);
        g_base = nullptr;
    }
    if (fd >= 0) close(fd);
#endif
    if (!g_base) {
        LOGE("memory: could not reserve the 4 GB guest space");
        return false;
    }
    LOGI("memory: guest space at %p", static_cast<void*>(g_base));
    return true;
}

// page commit map, one byte per 4 KB page; read off the lock by crash/watch paths
static std::vector<uint8_t> g_committed(0x100000, 0);

static uint8_t CommittedAt(size_t page) {
    return std::atomic_ref<uint8_t>(g_committed[page]).load(std::memory_order_relaxed);
}

bool Commit(uint32_t va, uint32_t size) {
    if (!size) return true;
    uint32_t first = va & ~(kPage - 1);
    uint64_t end = (uint64_t(va) + size + kPage - 1) & ~uint64_t(kPage - 1);
    if (!g_base || end > (1ull << 32)) return false;
    if (OverlapsPhysical(va, end))
        return InPhysical(va) && end <= uint64_t(va >= kWriteCombineBase ? kWriteCombineBase : kContiguousBase) +
                                           kPhysicalSize;
    std::lock_guard<std::recursive_mutex> lock(g_lock);
#ifdef _WIN32
    if (!VirtualAlloc(g_base + first, size_t(end - first), MEM_COMMIT, PAGE_READWRITE)) {
        LOGE("memory: commit %08X+%X failed", first, uint32_t(end - first));
        return false;
    }
#else
    if (mprotect(g_base + first, size_t(end - first), PROT_READ | PROT_WRITE)) {
        LOGE("memory: commit %08X+%X failed", first, uint32_t(end - first));
        return false;
    }
#endif
    for (uint64_t p = first; p < end; p += kPage)
        std::atomic_ref<uint8_t>(g_committed[size_t(p >> 12)]).store(1, std::memory_order_relaxed);
    return true;
}

namespace {
// XBE_TRACE_FREE: remember the guest return addresses behind the last decommit of each page, so an access
// to freed memory can name who freed it (DescribeFree, called from the crash handler).
// Lock-free: one fixed slot per page below 256 MB (thread id + 5 return addresses), plain relaxed stores,
// so tracing barely changes timing.
constexpr uint32_t kTracePages = 0x10000000u >> 12;
std::atomic<uint32_t> (*g_free_sites)[6] = nullptr;

void RecordFree(uint32_t va, uint32_t size) {
    static const bool on = [] {
        if (!DevEnv("XBE_TRACE_FREE")) return false;
        g_free_sites = new std::atomic<uint32_t>[kTracePages][6]();
        return true;
    }();
    if (!on) return;
    uint32_t rets[6] = {};
    kernel::Thread *t = kernel::Current();
    if (t && t->ctx) {
        rets[0] = t->id;
        // Walk the ebp frame chain (real callers), not a raw stack scan (stale return addresses).
        auto rd = [](uint32_t a) { return *reinterpret_cast<const uint32_t *>(g_base + a); };
        if (IsCommitted(t->ctx->esp)) rets[1] = rd(t->ctx->esp);
        uint32_t ebp = t->ctx->ebp;
        for (size_t n = 2; n < 6; ++n) {
            if (ebp < 0x10000u || (ebp & 3u) || !IsCommitted(ebp) || !IsCommitted(ebp + 7)) break;
            rets[n] = rd(ebp + 4);
            const uint32_t next = rd(ebp);
            if (next <= ebp) break;
            ebp = next;
        }
    }
    for (uint64_t p = va >> 12; p < (uint64_t(va) + size + 0xFFFu) >> 12 && p < kTracePages; ++p)
        for (int i = 0; i < 6; ++i) g_free_sites[p][i].store(rets[i], std::memory_order_relaxed);
}
} // namespace

void DescribeFree(uint32_t va) {
    const uint32_t p = va >> 12;
    if (!g_free_sites || p >= kTracePages || !g_free_sites[p][1].load()) {
        LOGE("  page %08X: no recorded free", va & ~0xFFFu);
        return;
    }
    auto &r = g_free_sites[p];
    LOGE("  page %08X last freed by thread %X from %08X %08X %08X %08X %08X", va & ~0xFFFu, r[0].load(),
         r[1].load(), r[2].load(), r[3].load(), r[4].load(), r[5].load());
}

void Decommit(uint32_t va, uint32_t size) {
    if (!g_base || !size) return;
    uint32_t first = va & ~(kPage - 1);
    uint64_t end = (uint64_t(va) + size + kPage - 1) & ~uint64_t(kPage - 1);
    if (end > (1ull << 32) || OverlapsPhysical(va, end)) return;
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    RecordFree(va, size);
#ifdef _WIN32
    if (!VirtualFree(g_base + first, size_t(end - first), MEM_DECOMMIT)) return;
#else
    if (mprotect(g_base + first, size_t(end - first), PROT_NONE)) return;
    madvise(g_base + first, size_t(end - first), MADV_DONTNEED);
#endif
    for (uint64_t p = first; p < end; p += kPage)
        std::atomic_ref<uint8_t>(g_committed[size_t(p >> 12)]).store(0, std::memory_order_relaxed);
    // what a range describes changed: AllocationSize's and the texture descriptor cache's answers are stale
    LayoutChanged();
}

bool IsCommitted(uint32_t va) { return InPhysical(va) || CommittedAt(va >> 12) != 0; }

// first fit allocator over a guest range
class Ranges {
public:
    void Init(uint32_t start, uint32_t size) { free_[start] = size; }

    void Remove(uint32_t start, uint32_t size) {
        // carve a fixed range out of the free list
        for (auto it = free_.begin(); it != free_.end(); ++it) {
            uint32_t fs = it->first, fz = it->second;
            if (start >= fs && uint64_t(start) + size <= uint64_t(fs) + fz) {
                free_.erase(it);
                if (start > fs) free_[fs] = start - fs;
                if (uint64_t(start) + size < uint64_t(fs) + fz) free_[start + size] = uint32_t(uint64_t(fs) + fz - start - size);
                return;
            }
        }
    }

    uint32_t Alloc(uint32_t size, uint32_t align, uint32_t lo, uint32_t hi, bool top_down) {
        if (!align) align = 1;
        if (top_down) {
            for (auto it = free_.rbegin(); it != free_.rend(); ++it) {
                uint64_t fs = it->first, fe = fs + it->second;
                uint64_t e = std::min<uint64_t>(fe, uint64_t(hi) + 1);
                if (e < size) continue;
                uint64_t a = (e - size) & ~uint64_t(align - 1);
                if (a < fs || a < lo) continue;
                Take(uint32_t(a), size);
                return uint32_t(a);
            }
            return 0;
        }
        for (auto& [fs, fz] : free_) {
            uint64_t a = std::max<uint64_t>(fs, lo);
            a = (a + align - 1) & ~uint64_t(align - 1);
            if (a + size <= uint64_t(fs) + fz && a + size - 1 <= hi) {
                Take(uint32_t(a), size);
                return uint32_t(a);
            }
        }
        return 0;
    }

    bool IsFree(uint32_t start, uint32_t size) const {
        auto it = free_.upper_bound(start);
        if (it == free_.begin()) return false;
        --it;
        return uint64_t(start) + size <= uint64_t(it->first) + it->second;
    }

    bool Free(uint32_t a, uint32_t* size_out = nullptr) {
        auto it = used_.find(a);
        if (it == used_.end()) return false;
        uint32_t size = it->second;
        used_.erase(it);
        if (size_out) *size_out = size;
        Release(a, size);
        return true;
    }

    void Release(uint32_t a, uint32_t size) {
        auto n = free_.emplace(a, size).first;
        auto next = std::next(n);
        if (next != free_.end() && n->first + n->second == next->first) {
            n->second += next->second;
            free_.erase(next);
        }
        if (n != free_.begin()) {
            auto prev = std::prev(n);
            if (prev->first + prev->second == n->first) {
                prev->second += n->second;
                free_.erase(n);
            }
        }
    }

    void Forget(uint32_t a) { used_.erase(a); }

    uint32_t SizeOf(uint32_t a) const {
        auto it = used_.find(a);
        return it == used_.end() ? 0 : it->second;
    }

    uint64_t Used() const {
        uint64_t n = 0;
        for (auto& u : used_) n += u.second;
        return n;
    }

private:
    void Take(uint32_t a, uint32_t size) {
        Remove(a, size);
        used_[a] = size;
    }
    std::map<uint32_t, uint32_t> free_;
    std::map<uint32_t, uint32_t> used_;
};

static Ranges g_contig, g_system, g_stacks;
static std::map<uint32_t, uint32_t> g_stack_tops;
static bool g_ranges_ready = false;

struct Reservation {
    uint32_t size;
    uint32_t protect;
    std::vector<uint8_t> commit;  // per page
};
static std::map<uint32_t, Reservation> g_reserved;
static Ranges g_user;
// committed pages over every reservation, maintained with the maps above. mem::Query used to walk a byte
// per page of the whole user address space (about 1 MB) under the allocator lock.
static uint64_t g_virtual_committed_pages = 0;

static void EnsureRanges() {
    if (g_ranges_ready) return;
    g_ranges_ready = true;
    g_contig.Init(kContiguousBase + kKernelDataSize, kPhysicalSize - kKernelDataSize);
    g_system.Init(kSystemBase, kSystemSize);
    g_stacks.Init(kStackBase, kStackSize);
    g_user.Init(kUserLow, kUserHigh - kUserLow);
    Commit(kKernelDataBase, kKernelDataSize);
}

bool ReserveImage(uint32_t base, uint32_t size) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    LayoutChanged();
    const uint64_t end = (uint64_t(base) + size + 0xFFFF) & ~0xFFFFull;
    if (!size || base < kUserLow || end > kUserHigh) return false;
    EnsureRanges();
    const uint32_t b = base & ~0xFFFFu;
    const uint32_t e = uint32_t(end);
    if (!Commit(b, e - b)) return false;
    g_user.Remove(b, e - b);
    Reservation r{e - b, 4, std::vector<uint8_t>((e - b) / kPage, 1)};  // 4 is PAGE_READWRITE
    if (const auto it = g_reserved.find(b); it != g_reserved.end())
        g_virtual_committed_pages -= uint64_t(std::count(it->second.commit.begin(), it->second.commit.end(), uint8_t(1)));
    g_virtual_committed_pages += uint64_t(r.commit.size());  // every page of an image is committed
    g_reserved[b] = std::move(r);
    return true;
}

static Reservation* FindReservation(uint32_t addr, uint32_t* start) {
    auto it = g_reserved.upper_bound(addr);
    if (it == g_reserved.begin()) return nullptr;
    --it;
    if (addr >= it->first + it->second.size) return nullptr;
    *start = it->first;
    return &it->second;
}

uint32_t AllocateVirtual(uint32_t& base, uint32_t& size, uint32_t type, uint32_t protect) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    LayoutChanged();
    EnsureRanges();
    if (!size || size > 0xFFFFFFFFu - 0xFFFFu) return 0xC000000D;  // STATUS_INVALID_PARAMETER
    bool reserve = type & kMemReserve;
    bool commit = type & kMemCommit;
    if (!base && commit) reserve = true;
    if (reserve) {
        uint32_t sz = (size + 0xFFFF) & ~0xFFFFu;
        uint32_t a;
        if (base) {
            if (uint64_t(base) + size + 0xFFFF > 0xFFFFFFFFull) return 0xC000000D;
            a = base & ~0xFFFFu;
            sz = ((base + size + 0xFFFF) & ~0xFFFFu) - a;
            if (!g_user.IsFree(a, sz)) return 0xC0000018;  // STATUS_CONFLICTING_ADDRESSES
            g_user.Remove(a, sz);
        } else {
            a = g_user.Alloc(sz, 0x10000, kUserLow, kUserHigh - 1, type & kMemTopDown);
            if (!a) return 0xC0000017;  // STATUS_NO_MEMORY
            g_user.Forget(a);
        }
        g_reserved[a] = Reservation{sz, protect, std::vector<uint8_t>(sz / kPage, 0)};
        base = a;
        size = sz;
    }
    if (commit) {
        uint32_t start;
        Reservation* r = FindReservation(base, &start);
        if (!r) return 0xC0000045;  // STATUS_INVALID_PAGE_PROTECTION stands in for not reserved
        const uint64_t first = uint64_t(base) & ~uint64_t(kPage - 1);
        const uint64_t end = (uint64_t(base) + size + kPage - 1) & ~uint64_t(kPage - 1);
        if (end > uint64_t(start) + r->size || end > 0x100000000ull) return 0xC0000018;  // STATUS_CONFLICTING_ADDRESSES
        if (!Commit(uint32_t(first), uint32_t(end - first))) return 0xC0000017;  // STATUS_NO_MEMORY
        for (uint32_t p = uint32_t(first); p < uint32_t(end); p += kPage) {
            uint8_t& c = r->commit[(p - start) / kPage];
            if (!c) {
                c = 1;
                ++g_virtual_committed_pages;
            }
        }
        base = uint32_t(first);
        size = uint32_t(end - first);
    }
    return 0;
}

uint32_t FreeVirtual(uint32_t& base, uint32_t& size, uint32_t type) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    LayoutChanged();
    uint32_t start;
    Reservation* r = FindReservation(base, &start);
    if (!r) return 0xC000009F;  // STATUS_MEMORY_NOT_ALLOCATED
    if (type & kMemRelease) {
        // MEM_RELEASE frees the whole reservation and requires its base; a size,
        // when given, must be the reservation's own size.
        if (base != start || (size && size != r->size)) return 0xC000000D;  // STATUS_INVALID_PARAMETER
        uint32_t sz = r->size;
        Decommit(start, sz);
        g_virtual_committed_pages -= uint64_t(std::count(r->commit.begin(), r->commit.end(), uint8_t(1)));
        g_reserved.erase(start);
        g_user.Release(start, sz);
        base = start;
        size = sz;
        return 0;
    }
    uint32_t first = base & ~(kPage - 1);
    uint64_t end64 = size ? (uint64_t(base) + size + kPage - 1) & ~uint64_t(kPage - 1) : uint64_t(start) + r->size;
    if (end64 > uint64_t(start) + r->size) return 0xC000000D;
    uint32_t end = uint32_t(end64);
    Decommit(first, end - first);
    for (uint32_t p = first; p < end; p += kPage) {
        uint8_t& c = r->commit[(p - start) / kPage];
        if (c) {
            c = 0;
            --g_virtual_committed_pages;
        }
    }
    base = first;
    size = end - first;
    return 0;
}

bool QueryVirtual(uint32_t addr, RegionInfo& out) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t start;
    Reservation* r = FindReservation(addr, &start);
    std::memset(&out, 0, sizeof(out));
    if (!r) {
        out.base = addr & ~(kPage - 1);
        auto it = g_reserved.upper_bound(addr);
        uint32_t next = it == g_reserved.end() ? kUserHigh : it->first;
        out.size = next > out.base ? next - out.base : kPage;
        out.state = 0x10000;  // MEM_FREE
        out.protect = 1;      // PAGE_NOACCESS
        return addr < kUserHigh;
    }
    uint32_t page = (addr - start) / kPage;
    uint8_t c = r->commit[page];
    uint32_t n = page;
    while (n < r->commit.size() && r->commit[n] == c) n++;
    out.base = start + page * kPage;
    out.alloc_base = start;
    out.alloc_protect = r->protect;
    out.size = (n - page) * kPage;
    out.state = c ? kMemCommit : kMemReserve;
    out.protect = c ? r->protect : 0;
    out.type = 0x20000;  // MEM_PRIVATE
    return true;
}

// Phase 0 city-streaming probe: with XBE_TRACE_CITY=1, log the larger physical allocations and frees so tile
// loads can be attributed from their timestamp. Off otherwise, so normal runs are unaffected.
static bool CityTrace() {
    static const bool on = DevEnv("XBE_TRACE_CITY") != nullptr;
    return on;
}

namespace {
// XBE_TRACE_CONTIG: live contiguous allocations with their guest caller chain, summarised by caller so a
// growing group (a leak) stands out. Guarded by g_lock like the allocator itself.
struct ContigRec {
    uint32_t size;
    uint32_t chain[4];
};
std::map<uint32_t, ContigRec> g_contig_live;
bool ContigTrace() {
    static const bool on = DevEnv("XBE_TRACE_CONTIG") != nullptr;
    return on;
}
void ContigChain(uint32_t out[4]) {
    kernel::Thread *t = kernel::Current();
    if (!t || !t->ctx) return;
    auto rd = [](uint32_t a) { return *reinterpret_cast<const uint32_t *>(g_base + a); };
    uint32_t ebp = t->ctx->ebp;
    for (int n = 0; n < 4; ++n) {
        if (ebp < 0x10000u || (ebp & 3u) || !IsCommitted(ebp) || !IsCommitted(ebp + 7)) break;
        out[n] = rd(ebp + 4);
        const uint32_t next = rd(ebp);
        if (next <= ebp) break;
        ebp = next;
    }
}
} // namespace

uint32_t AllocContiguous(uint32_t size, uint32_t lowest, uint32_t highest, uint32_t align) {
    std::unique_lock<std::recursive_mutex> lock(g_lock);
    LayoutChanged();
    EnsureRanges();
    if (size > 0xFFFFFFFFu - (kPage - 1)) return 0;
    size = (size + kPage - 1) & ~(kPage - 1);
    if (!size) size = kPage;
    align = std::max<uint32_t>(align, kPage);
    uint32_t lo = kContiguousBase | std::min<uint32_t>(lowest, kPhysicalSize - 1);
    uint32_t hi = highest >= kPhysicalSize - 1 ? kContiguousBase + kPhysicalSize - 1 : kContiguousBase | highest;
    // highest fit first: low memory stays free for requests limited to it (D3D report pages, first 16 MB)
    uint32_t a = g_contig.Alloc(size, align, lo, hi, true);
    if (!a) {
        LOGE("memory: contiguous allocation of %X bytes failed (%X..%X align %X), %llu KB in use", size, lowest, highest, align,
             (unsigned long long)(g_contig.Used() >> 10));
        return 0;
    }
    if (!Commit(a, size)) {
        uint32_t freed = 0;
        g_contig.Free(a, &freed);
        return 0;
    }
    if (ContigTrace()) {
        ContigRec r{size, {}};
        ContigChain(r.chain);
        g_contig_live[a] = r;
    }
    if (CityTrace() && size >= 0x4000)
        LOGR("citymem: alloc %u KB at %08X, %llu KB in use", size >> 10, a,
             static_cast<unsigned long long>(g_contig.Used() >> 10));
    lock.unlock();
    // Zero outside the global lock: a large city store memset used to stop every
    // other guest allocation for its duration.
    std::memset(g_base + a, 0, size);
    return a;
}

void DumpContigLedger() {
    if (!ContigTrace()) return;
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    std::map<std::array<uint32_t, 4>, std::pair<uint32_t, uint64_t>> groups;
    for (const auto &[va, r] : g_contig_live) {
        auto &g = groups[{r.chain[0], r.chain[1], r.chain[2], r.chain[3]}];
        g.first++;
        g.second += r.size;
    }
    std::vector<std::pair<uint64_t, std::array<uint32_t, 4>>> top;
    for (const auto &[k, v] : groups) top.push_back({v.second, k});
    std::sort(top.rbegin(), top.rend());
    for (size_t i = 0; i < top.size() && i < 8; ++i) {
        const auto &g = groups[top[i].second];
        LOGR("contig: %6llu KB in %4u allocs from %08X<%08X<%08X<%08X", (unsigned long long)(top[i].first >> 10),
             g.first, top[i].second[0], top[i].second[1], top[i].second[2], top[i].second[3]);
    }
}

void FreeContiguous(uint32_t va) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    LayoutChanged();
    if (ContigTrace()) g_contig_live.erase(va);
    uint32_t size;
    if (g_contig.Free(va, &size)) {
        Decommit(va, size);
        if (CityTrace() && size >= 0x4000)
            LOGR("citymem: free %u KB at %08X, %llu KB in use", size >> 10, va,
                 static_cast<unsigned long long>(g_contig.Used() >> 10));
    }
}

uint32_t AllocSystem(uint32_t size, uint32_t align) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    LayoutChanged();
    EnsureRanges();
    if (size > 0xFFFFFFFFu - 15u) return 0;
    size = (size + 15) & ~15u;
    if (!size) size = 16;
    uint32_t a = g_system.Alloc(size, std::max<uint32_t>(align, 16), kSystemBase, kSystemBase + kSystemSize - 1, false);
    if (!a) return 0;
    if (!Commit(a, size)) {
        uint32_t freed = 0;
        g_system.Free(a, &freed);
        return 0;
    }
    std::memset(g_base + a, 0, size);
    return a;
}

void FreeSystem(uint32_t va) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    LayoutChanged();
    g_system.Free(va);
}

uint32_t AllocStack(uint32_t size) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    LayoutChanged();
    EnsureRanges();
    if (size > kStackSize - 0x10000) return 0;
    size = (size + 0xFFFF) & ~0xFFFFu;
    // one uncommitted guard page below the stack
    uint32_t a = g_stacks.Alloc(size + 0x10000, 0x10000, kStackBase, kStackBase + kStackSize - 1, false);
    if (!a) return 0;
    if (!Commit(a + 0x10000, size)) {
        uint32_t freed = 0;
        g_stacks.Free(a, &freed);
        return 0;
    }
    g_stack_tops[a + 0x10000 + size] = a;
    return a + 0x10000 + size;
}

void FreeStack(uint32_t top) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    LayoutChanged();
    auto it = g_stack_tops.find(top);
    if (it == g_stack_tops.end()) return;
    uint32_t size;
    if (g_stacks.Free(it->second, &size)) Decommit(it->second, size);
    g_stack_tops.erase(it);
}

static uint32_t AllocationSizeLocked(uint32_t va) {
    if (uint32_t s = g_contig.SizeOf(va)) return s;
    if (uint32_t s = g_system.SizeOf(va)) return s;
    uint32_t start;
    if (Reservation* r = FindReservation(va, &start)) return r->size - (va - start);
    return 0;
}

// The renderer asks for the same vertex buffers and textures every draw: answers are kept per thread until any
// allocation or free changes the layout, which saves the lock and the allocator searches.
uint32_t AllocationSize(uint32_t va) {
    struct Entry {
        uint32_t va = 0, size = 0;
        uint64_t layout = 0;
    };
    thread_local Entry cache[256];
    Entry& e = cache[(va >> 4 ^ va >> 12) & 255];
    if (e.layout == g_layout.load(std::memory_order_acquire) && e.va == va) return e.size;
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    e = {va, AllocationSizeLocked(va), g_layout.load(std::memory_order_relaxed)};
    return e.size;
}

uint32_t PhysicalFromVirtual(uint32_t va) {
    if (InPhysical(va)) return va - (va >= kWriteCombineBase ? kWriteCombineBase : kContiguousBase);
    return va;
}

uint32_t VirtualFromPhysical(uint32_t pa) {
    if (pa < kPhysicalSize) return kContiguousBase | pa;
    return pa;
}

uint64_t LayoutGeneration() { return g_layout.load(std::memory_order_acquire); }

Stats Query() {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
#if TCNYC_DEV_TOOLS
    // The count is maintained with the maps; a dev build checks it against them and repairs any drift.
    {
        uint64_t check = 0;
        for (auto& [a, r] : g_reserved)
            for (uint8_t c : r.commit) check += c;
        if (check != g_virtual_committed_pages) {
            LOGE("memory: committed page count drifted (%llu, maps say %llu)", (unsigned long long)g_virtual_committed_pages,
                 (unsigned long long)check);
            g_virtual_committed_pages = check;
        }
    }
#endif
    Stats s{};
    s.total_pages = kPhysicalSize / kPage;
    s.virtual_committed = uint32_t(g_virtual_committed_pages);
    s.contiguous_used = uint32_t(g_contig.Used() / kPage);
    s.system_used = uint32_t(g_system.Used() / kPage);
    uint64_t used = g_virtual_committed_pages + s.contiguous_used + s.system_used;
    s.available_pages = used >= s.total_pages ? 0 : uint32_t(s.total_pages - used);
    return s;
}

}  // namespace mem
