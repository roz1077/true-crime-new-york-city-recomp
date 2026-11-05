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

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>

#include "app/app.h"
#include "common/graphics_choices.h"
#include "common/graphics_menu.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "xdk/d3d8.h"
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

// a projection call on the shared renderer from one of the gameplay camera's sites
bool GameplayProjection(X86Context &ctx, uint8_t *base) {
    const uint32_t ret = X86_MEM32(ctx.esp);
    return ctx.ecx == kRenderer && std::ranges::find(kGameplayProjection, ret) != std::end(kGameplayProjection);
}

// Applies fov_percent to the k argument of a projection call from the gameplay camera.
void AdjustProjection(X86Context &ctx, uint8_t *base, const char *which, bool gameplay) {
    const uint32_t self = ctx.ecx, ret = X86_MEM32(ctx.esp), arg = ctx.esp + 4;
    if (self != kRenderer) return;
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
    const uint32_t percent = [] {
        app::SettingsLock lock;
        return game::choices::ClampFovPercent(game::g_fov_percent);
    }();
    if (!gameplay || percent == 100) return;
    const float scaled = k * 100.0f / float(percent);
    std::memcpy(base + arg, &scaled, 4); // the caller's pushed argument
}

// Temporarily widen the gameplay projection to the frame's aspect ratio. Other users of the renderer's
// horizontal scale, including the HUD and picture-in-picture, see the guest value.
void RunProjection(X86Context &ctx, uint8_t *base, const char *which, void (*original)(X86Context &, uint8_t *)) {
    const bool gameplay = GameplayProjection(ctx, base);
    const float previous = gameplay ? F32(base, kRenderer + kScaleX) : 0.0f;
    constexpr float kWideAspect = 16.0f / 9.0f;
    // 0.75 is the game's x scale for a 16:9 picture; other set-ups keep the game's own
    const bool adjust = gameplay && previous == 0.75f;
    if (adjust) {
        const float scale = previous * kWideAspect / app::AspectRatio();
        std::memcpy(base + kRenderer + kScaleX, &scale, sizeof(scale));
    }
    AdjustProjection(ctx, base, which, gameplay);
    original(ctx, base);
    if (adjust) std::memcpy(base + kRenderer + kScaleX, &previous, sizeof(previous));
}

} // namespace

X86_EXTERN_FUNC(__imp__sub_00245BB0);
X86_FUNC(sub_00245BB0) { RunProjection(ctx, base, "projection", __imp__sub_00245BB0); }

X86_EXTERN_FUNC(__imp__sub_00245CE0);
X86_FUNC(sub_00245CE0) { RunProjection(ctx, base, "infinite projection", __imp__sub_00245CE0); }

// Apply the depth priority after the guest's vertex program, which can
// upload its own viewport constants after the viewport is set. The guest
// depth clip still sees the original depth and preserves the mesh coverage.
X86_EXTERN_FUNC(__imp__sub_002453C0);
X86_FUNC(sub_002453C0) {
    constexpr uint32_t kDistantCityViewportReturn = 0x13F188;
    constexpr uint32_t kRestoreCityViewportReturn = 0x13F3DA;
    const uint32_t caller = X86_MEM32(ctx.esp);
    if (caller == kDistantCityViewportReturn || caller == kRestoreCityViewportReturn) {
        std::lock_guard lock(d3d::Lock());
        auto &state = d3d::State();
        // A full-range pass includes scenery that needs its original depth.
        const bool distant = caller == kDistantCityViewportReturn && state.zclip_on &&
                             state.zclip[0] >= 0 && state.zclip[1] > state.zclip[0] &&
                             state.zclip[1] < state.zscale;
        state.program_depth_scale = distant ? 1.0f / 32.0f : 1.0f;
    }
    __imp__sub_002453C0(ctx, base);
}

// Comparison mode for the scene submission's distant-city pass.
X86_EXTERN_FUNC(__imp__sub_0013F090);
X86_FUNC(sub_0013F090) {
    static const bool hide = [] {
        const char *value = std::getenv("XBE_HIDE_DISTANT_CITY");
        return value && std::strcmp(value, "1") == 0;
    }();
    if (hide && X86_MEM32(ctx.esp) == 0x142CEE) {
        ctx.eax = 0;
        ctx.esp += 8; // Return address and the renderer argument.
        return;
    }
    __imp__sub_0013F090(ctx, base);
}
