// Read-only vehicle spawn diagnostics for investigating the Xbox Copland bug.
#include <cstdint>
#include <cstdlib>
#include <array>

#include "core/log.h"
#include "xdk/hle.h"

namespace {

constexpr uint32_t kWorld = 0x360E00;
constexpr uint32_t kTrafficRefs = 0x2660;
constexpr uint32_t kTrafficRefStride = 0x14;
constexpr uint32_t kTrafficRefCount = 16;

bool TraceEnabled() {
    static const bool enabled = std::getenv("XBE_TRACE_TRAFFIC") != nullptr;
    return enabled;
}

// Vehicle spawns pass a resource reference as the second argument. The signed
// slot at +6 indexes the shared asset loader table. Model IDs in the table
// are one-based, while the constructed vehicle stores a zero-based ID.
struct SpawnRequest {
    uint32_t caller = 0;
    int slot = -1;
    int model = -1;
    uint32_t flags = 0;
    bool has_data = false;
    bool operator==(const SpawnRequest&) const = default;
};

SpawnRequest ReadResource(uint8_t* base, uint32_t resource) {
    SpawnRequest request;
    if (!resource) return request;
    request.slot = int16_t(X86_MEM16(resource + 6));
    if (request.slot < 0 || request.slot >= 256) return request;
    const uint32_t entry = 0x37493C + uint32_t(request.slot) * 0x74;
    request.model = int16_t(X86_MEM16(entry + 0x60));
    request.flags = X86_MEM16(entry + 0x62);
    request.has_data = X86_MEM32(entry) != 0;
    return request;
}

SpawnRequest ReadRequest(X86Context& ctx, uint8_t* base) {
    auto request = ReadResource(base, X86_MEM32(ctx.esp + 8));
    request.caller = X86_MEM32(ctx.esp);
    return request;
}

const char* ModelKind(int model) {
    switch (model) {
    case 7: return "crownwig";
    case 62: return "police";
    case 63: return "taxi";
    case 89: return "training-police";
    default: return "other";
    }
}

void TraceSpawn(const char* path, X86Context& ctx, uint8_t* base, const SpawnRequest& request) {
    const int model = ctx.eax ? int16_t(X86_MEM16(ctx.eax + 0x448)) + 1 : -1;
    LOGR("traffic: %s caller=%08X slot=%d requested=%d flags=%04X resident=%u actual=%d kind=%s vehicle=%08X",
         path, request.caller, request.slot, request.model, request.flags, unsigned(request.has_data),
         model, ModelKind(model), ctx.eax);
}

void TraceSelection(uint8_t* base, uint32_t caller, int selected) {
    const uint32_t world = X86_MEM32(kWorld);
    if (!world) return;
    using Pool = std::array<SpawnRequest, kTrafficRefCount>;
    Pool pool;
    uint32_t excluded = 0;
    for (uint32_t i = 0; i < pool.size(); ++i) {
        const uint32_t ref = world + kTrafficRefs + i * kTrafficRefStride;
        pool[i] = ReadResource(base, ref);
        if (X86_MEM16(ref + 4) & 0x100) excluded |= 1u << i;
    }
    static Pool previous;
    static uint32_t previous_world = 0, previous_excluded = 0;
    static int previous_selection = -2;
    if (world != previous_world || pool != previous || excluded != previous_excluded) {
        LOGR("traffic: candidate pool world=%08X excluded=%04X", world, excluded);
        for (uint32_t i = 0; i < pool.size(); ++i) {
            const auto& entry = pool[i];
            LOGR("traffic: candidate=%u slot=%d model=%d flags=%04X resident=%u",
                 i, entry.slot, entry.model, entry.flags, unsigned(entry.has_data));
        }
        previous_world = world;
        previous = pool;
        previous_excluded = excluded;
        previous_selection = -2;
    }
    if (selected != previous_selection) {
        const int model = selected >= 0 && selected < int(pool.size()) ? pool[selected].model : -1;
        LOGR("traffic: selection caller=%08X candidate=%d model=%d kind=%s",
             caller, selected, model, ModelKind(model));
        previous_selection = selected;
    }
}

} // namespace

X86_EXTERN_FUNC(__imp__sub_000158F0);
X86_FUNC(sub_000158F0) {
    if (!TraceEnabled()) return __imp__sub_000158F0(ctx, base);
    const uint32_t caller = X86_MEM32(ctx.esp);
    __imp__sub_000158F0(ctx, base);
    TraceSelection(base, caller, int32_t(ctx.eax));
}

X86_EXTERN_FUNC(__imp__sub_000823C0);
X86_FUNC(sub_000823C0) {
    if (!TraceEnabled()) return __imp__sub_000823C0(ctx, base);
    const auto request = ReadRequest(ctx, base);
    __imp__sub_000823C0(ctx, base);
    TraceSpawn("823C0", ctx, base, request);
}

X86_EXTERN_FUNC(__imp__sub_0007DD80);
X86_FUNC(sub_0007DD80) {
    if (!TraceEnabled()) return __imp__sub_0007DD80(ctx, base);
    const auto request = ReadRequest(ctx, base);
    __imp__sub_0007DD80(ctx, base);
    TraceSpawn("7DD80", ctx, base, request);
}
