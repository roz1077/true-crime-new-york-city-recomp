// Read-only vehicle spawn diagnostics for investigating the Xbox Copland bug.
#include <algorithm>
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
    int weight = 0;    // ref+8, the model's traffic weight
    int used = 0;      // ref+0xC, subtracted from the weight by the selector
    int effective = 0; // max(weight - used, 0), the real selection weight
    bool has_data = false;
    bool operator==(const SpawnRequest &) const = default;
};

SpawnRequest ReadResource(uint8_t *base, uint32_t resource) {
    SpawnRequest request;
    if (!resource) return request;
    request.slot = int16_t(X86_MEM16(resource + 6));
    if (request.slot < 0 || request.slot >= 256) return request;
    const uint32_t entry = 0x37493C + uint32_t(request.slot) * 0x74;
    request.model = int16_t(X86_MEM16(entry + 0x60));
    request.flags = X86_MEM16(entry + 0x62);
    request.weight = int16_t(X86_MEM16(resource + 8));
    request.used = int32_t(X86_MEM32(resource + 0xC));
    request.effective = std::max(request.weight - request.used, 0);
    request.has_data = X86_MEM32(entry) != 0;
    return request;
}

SpawnRequest ReadRequest(X86Context &ctx, uint8_t *base) {
    auto request = ReadResource(base, X86_MEM32(ctx.esp + 8));
    request.caller = X86_MEM32(ctx.esp);
    return request;
}

const char *ModelKind(int model) {
    switch (model) {
    case 7:
        return "crownwig";
    case 62:
        return "police";
    case 63:
        return "taxi";
    case 89:
        return "training-police";
    default:
        return "other";
    }
}

void TraceSpawn(const char *path, X86Context &ctx, uint8_t *base, const SpawnRequest &request) {
    const int model = ctx.eax ? int16_t(X86_MEM16(ctx.eax + 0x448)) + 1 : -1;
    LOGR("traffic: %s caller=%08X slot=%d requested=%d flags=%04X resident=%u actual=%d kind=%s vehicle=%08X", path,
         request.caller, request.slot, request.model, request.flags, unsigned(request.has_data), model,
         ModelKind(model), ctx.eax);
}

void TraceSelection(uint8_t *base, uint32_t caller, int selected) {
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
    uint32_t selectable = 0;
    for (uint32_t i = 0; i < pool.size(); ++i)
        if (pool[i].effective > 0) selectable |= 1u << i;
    static Pool previous;
    static uint32_t previous_world = 0, previous_excluded = 0, previous_selectable = 0xFFFFFFFFu;
    static int previous_selection = -2;
    if (world != previous_world || pool != previous || excluded != previous_excluded) {
        LOGR("traffic: candidate pool world=%08X excluded=%04X", world, excluded);
        for (uint32_t i = 0; i < pool.size(); ++i) {
            const auto &entry = pool[i];
            LOGR("traffic: candidate=%u slot=%d model=%d flags=%04X weight=%d used=%d effective=%d resident=%u", i,
                 entry.slot, entry.model, entry.flags, entry.weight, entry.used, entry.effective,
                 unsigned(entry.has_data));
        }
        previous_world = world;
        previous = pool;
        previous_excluded = excluded;
        previous_selection = -2;
    }
    if (selectable != previous_selectable) {
        int police = 0, others = 0;
        for (uint32_t i = 0; i < pool.size(); ++i) {
            if (pool[i].effective <= 0) continue;
            if (pool[i].model == 62)
                ++police;
            else
                ++others;
        }
        LOGR("traffic: selectable=%04X police=%d others=%d", selectable, police, others);
        previous_selectable = selectable;
        previous_selection = -2;
    }
    if (selected != previous_selection) {
        const int model = selected >= 0 && selected < int(pool.size()) ? pool[selected].model : -1;
        LOGR("traffic: selection caller=%08X candidate=%d model=%d kind=%s", caller, selected, model, ModelKind(model));
        previous_selection = selected;
    }
}

// world+0x27A0 holds 16 model spawn records. 0x14170 picks one for the
// category of the traffic ref the selector chose; slot +2, flags +0, weight
// +0xC (int16) and used +0x10 (int32). An entry with weight - used <= 0 is
// skipped, so a leaked used count can leave only the police entry selectable.
struct ModelEntry {
    int slot = -1, flags = 0, weight = 0, used = 0, effective = 0;
    bool operator==(const ModelEntry &) const = default;
};

void TraceModelPick(uint8_t *base, uint32_t world, uint32_t category, int result) {
    (void)base;  // X86_MEM* read through base
    std::array<ModelEntry, kTrafficRefCount> table;
    uint32_t usable = 0;
    for (uint32_t i = 0; i < table.size(); ++i) {
        const uint32_t entry = world + 0x27A0 + i * kTrafficRefStride;
        auto &m = table[i];
        m.slot = int8_t(X86_MEM8(entry + 2));
        m.flags = int(X86_MEM16(entry));
        m.weight = int16_t(X86_MEM16(entry + 0xC));
        m.used = int32_t(X86_MEM32(entry + 0x10));
        m.effective = std::max(m.weight - m.used, 0);
        if (m.slot >= 0 && m.effective > 0) usable |= 1u << i;
    }
    static decltype(table) previous;
    static uint32_t previous_world = 0, previous_usable = 0xFFFFFFFFu, previous_category = 0xFFFFFFFFu;
    if (world == previous_world && usable == previous_usable && category == previous_category) return;
    LOGR("traffic: pick table world=%08X category=%08X usable=%04X result=%d", world, category, usable, result);
    for (uint32_t i = 0; i < table.size(); ++i) {
        const auto &m = table[i];
        if (m.slot < 0 && m.flags == 0 && m.weight == 0 && m.used == 0) continue;
        LOGR("traffic: pick=%u slot=%d flags=%04X weight=%d used=%d effective=%d", i, m.slot, m.flags, m.weight,
             m.used, m.effective);
    }
    previous_world = world;
    previous_usable = usable;
    previous_category = category;
    previous = table;
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

// 0x14170 picks a model from world+0x27A0 for the chosen traffic ref's category.
X86_EXTERN_FUNC(__imp__sub_00014170);
X86_FUNC(sub_00014170) {
    if (!TraceEnabled()) return __imp__sub_00014170(ctx, base);
    const uint32_t world = X86_MEM32(kWorld);
    const uint32_t category = X86_MEM32(ctx.esp + 8);  // arg1: the ref's category table
    __imp__sub_00014170(ctx, base);
    if (world) TraceModelPick(base, world, category, int32_t(ctx.eax));
}

// 0x7DBD0 adds one record to the model table at world+0x27A0. The caller tells
// us which zone/profile setup is choosing the models.
//
// The scripted/random-crime vehicle queue (return 0x1E492) reserves traffic
// model records by re-adding them with weight 0. That overwrites the zone
// profile's civilian weights, so the availability mask no longer matches the
// civilian traffic refs and only police can spawn (the "Copland" state).
X86_EXTERN_FUNC(__imp__sub_0007DBD0);
X86_FUNC(sub_0007DBD0) {
    const bool trace = TraceEnabled();
    const uint32_t caller = X86_MEM32(ctx.esp);
    const int index = int32_t(X86_MEM32(ctx.esp + 4));
    __imp__sub_0007DBD0(ctx, base);
    const uint32_t world = X86_MEM32(kWorld);
    if (!world || index < 0 || index >= int(kTrafficRefCount)) return;
    uint8_t *entry = base + world + 0x27A0 + uint32_t(index) * kTrafficRefStride;
    // The scripted/random-crime vehicle queue reserves a model record at
    // weight 0. Left at 0, the availability mask no longer matches the
    // civilian refs and only police can spawn (the "Copland" state). The
    // queue's own vehicle still spawns from its ref; give the record the
    // weight of that ref so normal traffic keeps using the model.
    int16_t weight = int16_t(X86_MEM16(entry + 0xC));
    if (weight <= 0 && (caller == 0x1CC6Cu || caller == 0x1E492u)) {
        const int8_t slot = int8_t(X86_MEM8(entry + 2));
        for (uint32_t i = 0; i < kTrafficRefCount; i++) {
            const uint8_t *ref = base + world + kTrafficRefs + i * kTrafficRefStride;
            if (int16_t(X86_MEM16(ref + 6)) != slot) continue;
            const int16_t ref_weight = int16_t(X86_MEM16(ref + 8));
            if (ref_weight <= 0) continue;
            const uint16_t flags = uint16_t(X86_MEM16(entry) & ~0x0100u);
            X86_MEM16(entry) = flags;
            X86_MEM16(entry + 0xC) = uint16_t(ref_weight);
            break;
        }
    }
    if (!trace) return;
    LOGR("traffic: add caller=%08X index=%d slot=%d flags=%04X weight=%d used=%d", caller, index,
         int8_t(X86_MEM8(entry + 2)), X86_MEM16(entry), int16_t(X86_MEM16(entry + 0xC)),
         int32_t(X86_MEM32(entry + 0x10)));
}

// 0x83240 removes/clears one model record.
X86_EXTERN_FUNC(__imp__sub_00083240);
X86_FUNC(sub_00083240) {
    if (!TraceEnabled()) return __imp__sub_00083240(ctx, base);
    const uint32_t caller = X86_MEM32(ctx.esp);
    const uint32_t entry = ctx.ecx;
    const uint32_t world = X86_MEM32(kWorld);
    int index = -1;
    if (world && entry >= world + 0x27A0 && entry < world + 0x27A0 + kTrafficRefCount * kTrafficRefStride)
        index = int((entry - world - 0x27A0) / kTrafficRefStride);
    LOGR("traffic: clear caller=%08X index=%d slot=%d flags=%04X weight=%d used=%d", caller, index,
         int8_t(X86_MEM8(entry + 2)), X86_MEM16(entry), int16_t(X86_MEM16(entry + 0xC)),
         int32_t(X86_MEM32(entry + 0x10)));
    __imp__sub_00083240(ctx, base);
}
