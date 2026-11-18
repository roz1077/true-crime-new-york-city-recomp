#include "game_addresses.h"

// PHamilton's head (BC2_M2 "Club Tadow") is a one-record skinned mesh. Its head loader leaves the
// skin buffer uninitialised, so the four uint16 bone indices are whatever the allocator had (0x1C90)
// and the skin path reads a matrix far outside the 23-bone palette (guest D00ED620).
//
// 0x240003 is the skin pass: the palette is at [ebp+0x14] with its count at [ebp+0x18], the mesh at
// [ebp+0x10], and the records it walks are at [esp+0x20] with the count at [esp+0x24]. The head mesh
// type is 0x43 (body parts are 0x03) and its descriptor names the single bone at +0x1C. Bone 0 is the
// character root and its palette entry is all zeros, so fall back to the first non-zero bone.
//
// Before the original runs, give a head record with no valid influence its declared bone and weight
// 255. Records that already name a bone inside the palette are left untouched, so this only fills in
// the data the loader omitted.

#include <atomic>
#include <cstdint>
#include <cstring>

#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

namespace {

constexpr uint32_t kRecordBytes = 20;
constexpr uint32_t kMaxBones = 64;

uint32_t U32(uint32_t a) {
    uint32_t v;
    std::memcpy(&v, mem::Base() + a, sizeof v);
    return v;
}

float F32(uint32_t a) {
    float v;
    std::memcpy(&v, mem::Base() + a, sizeof v);
    return v;
}

int HeadBone(uint32_t mesh, uint32_t palette, uint32_t bones) {
    const uint32_t declared = U32(mesh + 0x1C);
    if (declared > 0 && declared < bones) return int(declared);
    for (uint32_t i = 1; i < bones; ++i) {
        const uint32_t m = palette + i * 0x40;
        if (F32(m + 0x0C) != 0.0f || F32(m + 0x1C) != 0.0f || F32(m + 0x2C) != 0.0f) return int(i);
    }
    return 0;
}

bool HasValidInfluence(const uint8_t *p, uint32_t bones) {
    for (int slot = 0; slot < 4; ++slot) {
        uint16_t index;
        std::memcpy(&index, p + slot * 2, sizeof index);
        if (p[8 + slot] && index < bones) return true;
    }
    return false;
}

void InitHeadSkin(X86Context &ctx) {
    const uint32_t mesh = U32(ctx.ebp + 0x10);
    if (!mesh || !mem::IsCommitted(mesh + 0x28)) return;
    if (U32(mesh) != 0x43u) return;  // the textured head mesh; body parts are 0x03
    const uint32_t palette = U32(ctx.ebp + 0x14);
    const uint32_t bones = U32(ctx.ebp + 0x18);
    if (!palette || bones < 1 || bones > kMaxBones || !mem::IsCommitted(palette + (bones - 1) * 0x40)) return;
    const uint32_t rec = U32(ctx.esp + 0x20);
    const uint32_t count = U32(ctx.esp + 0x24);
    if (!rec || !count || count > 64 || !mem::IsCommitted(rec + (count - 1) * kRecordBytes)) return;

    const int bone = HeadBone(mesh, palette, bones);
    int fixed = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t *p = mem::Base() + rec + i * kRecordBytes;
        if (HasValidInfluence(p, bones)) continue;
        std::memset(p, 0, kRecordBytes);
        const uint16_t index = uint16_t(bone);
        std::memcpy(p, &index, sizeof index);
        p[8] = 255;
        ++fixed;
    }
    if (fixed) {
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 4)
            LOGR("head: filled %d skin record(s) for mesh %08X with bone %d of %u", fixed, mesh, bone, bones);
    }
}

}  // namespace

X86_EXTERN_FUNC(__imp__sub_00240003);

X86_FUNC(sub_00240003) {
    InitHeadSkin(ctx);
    __imp__sub_00240003(ctx, base);
}
