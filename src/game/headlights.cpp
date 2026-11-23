#include "game_addresses.h"

#include <cmath>

#include "kernel/kernel.h"
#include "xdk/hle.h"

namespace {

struct Vec3 {
    float x, y, z;
};

Vec3 ReadVec3(uint8_t *base, uint32_t address) {
    return {X86_MEMF32(address), X86_MEMF32(address + 4), X86_MEMF32(address + 8)};
}

float Dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

} // namespace

X86_EXTERN_FUNC(__imp__sub_001B8630);
X86_FUNC(sub_001B8630) {
    // The front-light update keeps the chassis in EBX and passes a lamp marker
    // in ECX. It uses only the returned transform's position and Z axis.
    const bool front = X86_MEM32(ctx.esp) == TC_ADDR(0x1BFFFB);
    const uint32_t chassis = ctx.ebx;
    __imp__sub_001B8630(ctx, base);
    if (!front) return;

    uint32_t chassis_matrix = chassis + 0x40;
    if (X86_MEM32(chassis + 0x28)) {
        // A parented chassis stores a local transform. Resolve its world
        // transform in temporary guest stack space, just as for the lamp.
        X86Context transform_ctx = ctx;
        chassis_matrix = (ctx.esp & ~0xFu) - 0x40;
        transform_ctx.esp = chassis_matrix;
        transform_ctx.ecx = chassis;
        kernel::CallGuest(transform_ctx, TC_ADDR(0x21EC70), {chassis_matrix});
    }

    const uint32_t lamp = ctx.eax;
    const Vec3 direction = ReadVec3(base, lamp + 0x20);
    const Vec3 down = ReadVec3(base, chassis_matrix + 0x10);
    // The Blitzer's front marker Z axes point almost directly up in chassis
    // space. Testing relative to the chassis also works on sloped roads.
    if (!(Dot(direction, down) < -0.95f)) return;

    const Vec3 axis = ReadVec3(base, chassis_matrix + 0x20);
    const Vec3 center = ReadVec3(base, chassis_matrix + 0x30);
    const Vec3 origin = ReadVec3(base, lamp + 0x30);
    const Vec3 offset = {origin.x - center.x, origin.y - center.y, origin.z - center.z};
    // Front segments have negative offsets along the renderer's negated
    // marker Z, so marker Z should point out from the nose of the car.
    const float sign = std::copysign(1.0f, Dot(offset, axis));
    X86_MEMF32(lamp + 0x20) = axis.x * sign;
    X86_MEMF32(lamp + 0x24) = axis.y * sign;
    X86_MEMF32(lamp + 0x28) = axis.z * sign;
}
