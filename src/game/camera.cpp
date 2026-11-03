// Field of view for the gameplay camera.
//
// The renderer (global object 0x36FE80) builds its perspective projection in
// 0x245BB0 (finite far plane, 0x22CFD0) or 0x245CE0 (infinite far plane,
// 0x22D0D0). Both are thiscall with one float, k: the camera's focal scale,
// cot(fov / 2). The matrix's x and y scales are k times renderer +0x08 and
// +0x10 (0.75 and 1.333 at 16:9), so the horizontal field of view is
// 2 * atan(1 / (k * [+0x08])). Aiming raises k (1.6 to 2.414), which zooms in.
//
// The gameplay camera passes its own k (camera +0x184) from two sites, 0x7E1B5
// and 0xA7553; menus, loading screens and previews pass constants or use other
// renderer objects and keep their framing. fov_percent scales tan(fov / 2),
// that is divides k, at those two sites only, so aiming still zooms relative
// to it. XBE_TRACE_CAMERA=1 logs each change. See GRAPHICS_NOTES.md.

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "common/graphics_choices.h"
#include "common/graphics_menu.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "xdk/hle.h"

namespace {

constexpr uint32_t kRenderer = 0x36FE80;
// return addresses of the gameplay camera's projection calls
constexpr uint32_t kGameplayProjection[] = {0x0007E1BA, 0x000A7558};
// renderer fields: x and y scale per unit k, near and far planes
constexpr uint32_t kScaleX = 0x08, kScaleY = 0x10, kNear = 0x1C, kFar = 0x20;

bool Tracing() {
    static const bool on = std::getenv("XBE_TRACE_CAMERA") != nullptr;
    return on;
}

float F32(uint8_t *base, uint32_t address) {
    float f;
    std::memcpy(&f, base + address, 4);
    return f;
}

// the field of view for a projection scale (cot of the half angle)
float Degrees(float scale) { return scale > 0 ? std::atan(1.0f / scale) * 360.0f / 3.14159265358979f : 0.0f; }

// Applies fov_percent to the k argument of a projection call from the gameplay camera.
void AdjustProjection(X86Context &ctx, uint8_t *base, const char *which) {
    const uint32_t self = ctx.ecx, ret = X86_MEM32(ctx.esp), arg = ctx.esp + 4;
    if (self != kRenderer) return;
    bool gameplay = false;
    for (uint32_t site : kGameplayProjection)
        gameplay |= ret == site;

    const float k = F32(base, arg), scale_x = F32(base, self + kScaleX), scale_y = F32(base, self + kScaleY);
    if (gameplay) game::g_camera_fov_degrees.store(Degrees(k * scale_x));
    if (Tracing()) {
        static thread_local uint32_t last[2] = {};
        const uint32_t now[2] = {ret, X86_MEM32(arg)};
        if (std::memcmp(now, last, sizeof(now))) {
            std::memcpy(last, now, sizeof(now));
            LOGR("camera: %s from %08X k=%g near %g far %g -> fov %.1f x %.1f deg%s", which, ret, k,
                 F32(base, self + kNear), F32(base, self + kFar), Degrees(k * scale_x), Degrees(k * scale_y),
                 gameplay ? " (gameplay)" : "");
        }
    }
    const uint32_t percent = game::choices::ClampFovPercent(game::g_fov_percent);
    if (!gameplay || percent == 100) return;
    const float scaled = k * 100.0f / float(percent);
    std::memcpy(base + arg, &scaled, 4); // the caller's pushed argument
}

} // namespace

X86_EXTERN_FUNC(__imp__sub_00245BB0);
X86_FUNC(sub_00245BB0) {
    AdjustProjection(ctx, base, "projection");
    __imp__sub_00245BB0(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_00245CE0);
X86_FUNC(sub_00245CE0) {
    AdjustProjection(ctx, base, "infinite projection");
    __imp__sub_00245CE0(ctx, base);
}
