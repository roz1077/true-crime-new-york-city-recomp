// Direct mouse aim for the gameplay camera.
//
// The camera object is [[0x360E00] + 0xAC]. It owns one look controller per
// camera mode in an array at +0x114, indexed by the mode byte at +0x19; the
// active controller is cached at +0x174. The shared per-frame camera update
// sub_0002E200 reads that active controller and calls its virtual methods, so
// injecting the mouse there covers the camera modes that accumulate the globals:
// on foot (mode 5), precision aim / crosshair (modes 0/6/11), drive-by, sniper
// and the other mission cameras, not just the four modes the PC fix names. The
// unarmed camera (mode 23) derives its yaw from the followed target instead of
// accumulating it, so there the raw counts are kept as a separate offset and
// added back after the controller, skipping the character-turn smoothing and
// the stick dead zone that used to swallow small horizontal steps.
//
// The look controllers accumulate a yaw and a pitch in 0x348B20 (yaw) and
// 0x348A80 (pitch); the camera update turns those globals into the final
// orientation later in the frame.
//
// Mouse motion is already sampled on the host, but folding it into the pad's
// right stick loses most of it: the guest reads the stick as a signed byte, so
// small movements quantise away and the stick's dead zone throws away the rest.
// With direct aim the host no longer feeds the mouse into the stick; instead
// the shared camera update adds the raw count delta straight to the two
// accumulators before the active controller runs, so the camera keeps its own
// clamps and smoothing.
//
// The feel follows the PC version's WidescreenFix (RawInput.ixx): raw counts
// are scaled by fMouseSens (4.0) * RawInputMouse (1.0) / 1000 = 0.004 rad per
// count, with the same scale on both axes. Its direct camera path also allows
// -89..+60 degrees of pitch on foot and -89..+10 degrees in a vehicle while
// the mouse is active, so the controllers' own -40..+60 limit is widened for
// that one update and restored afterwards. Direct aim stops feeding the stick
// for 5 s after the last sample, matching the PC fix's idle fallback.
//
// In a vehicle the active controller (sub_00024040) recomputes its chase yaw
// every frame instead of accumulating the stick into the globals, and then
// calls sub_0001FD40 to build the camera orientation. That path therefore
// applies the mouse to the globals between the controller's computation and
// sub_0001FD40, and holds the resulting absolute yaw/pitch while the mouse is
// active, which is what the PC fix does. When the mouse goes idle the game's
// own chase camera comes back.
//
// TCNYC_MOUSE_STICK=1 restores the old right-stick path.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "core/input.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

namespace {

constexpr uint32_t kGame = 0x360E00;   // [0x360E00] -> game/session object
constexpr uint32_t kCamera = 0xAC;     // game + 0xAC -> gameplay camera
constexpr uint32_t kOwner = 0x34;      // controller + 0x34 -> camera
constexpr uint32_t kActive = 0x174;    // camera + 0x174 -> active controller
constexpr uint32_t kTarget = 0x17C;    // camera + 0x17C -> followed pawn
constexpr uint32_t kActiveFlag = 0x18C;
constexpr uint32_t kMode = 0x19;       // camera + 0x19 -> camera mode
constexpr uint32_t kYaw = 0x348B20;    // radians, accumulated by the controller
constexpr uint32_t kPitch = 0x348A80;  // radians
constexpr uint32_t kInvertX = 0xFEF;   // game + 0xFEF
constexpr uint32_t kInvertY = 0xFEE;   // game + 0xFEE

constexpr uint8_t kModeVehicle = 13;

// The PC WidescreenFix takes raw counts straight to camera angles:
//     scale = fMouseSens (4.0) * RawInputMouse (1.0) / 1000 = 0.004 rad/count
// and applies the same scale to both axes.
constexpr float kRadiansPerPixel = 0.004f;
constexpr float kPi = 3.14159265358979f;

// The two consumers do not agree on the sign of 0x348A80.  The on-foot
// controllers build the camera inline (positive looks up); sub_0001FD40
// negates the value (positive looks down).  Each path keeps the PC view limits
// in its own sign.
constexpr float kOnFootPitchMin = -89.0f * kPi / 180.0f;  // view down
constexpr float kOnFootPitchMax = 60.0f * kPi / 180.0f;   // view up
constexpr float kVehiclePitchMin = -10.0f * kPi / 180.0f; // view up
constexpr float kVehiclePitchMax = 89.0f * kPi / 180.0f;  // view down

uint32_t Rd(uint8_t *base, uint32_t va) { return mem::IsCommitted(va) ? X86_MEM32(va) : 0; }
uint8_t Rd8(uint8_t *base, uint32_t va) { return mem::IsCommitted(va) ? X86_MEM8(va) : 0; }
float RdF(uint8_t *base, uint32_t va) {
    float f = 0;
    if (mem::IsCommitted(va)) std::memcpy(&f, base + va, sizeof(f));
    return f;
}

uint64_t NowMs() {
    static const auto start = std::chrono::steady_clock::now();
    return uint64_t(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
}

// The gameplay camera, when this specific look controller belongs to it and it
// has a pawn to follow.  The original methods bail out on the same two fields,
// so injecting there would move angles no camera reads.
uint32_t GameplayCamera(uint8_t *base, uint32_t controller) {
    if (!controller || !mem::IsCommitted(controller + kOwner)) return 0;
    const uint32_t owner = X86_MEM32(controller + kOwner);
    const uint32_t game = Rd(base, kGame);
    if (!game || !mem::IsCommitted(game + kCamera)) return 0;
    const uint32_t camera = X86_MEM32(game + kCamera);
    if (!camera || owner != camera) return 0;
    if (!Rd(base, camera + kTarget) || !Rd8(base, camera + kActiveFlag)) return 0;
    return camera;
}

// The shared camera object itself (not a look controller): the per-frame update
// asks it for the active controller and runs that controller's virtual methods.
bool IsGameplayCamera(uint8_t *base, uint32_t camera) {
    const uint32_t game = Rd(base, kGame);
    if (!camera || !game || camera != Rd(base, game + kCamera)) return false;
    return Rd(base, camera + kTarget) && Rd8(base, camera + kActiveFlag);
}

// The unarmed camera, sub_0002D230. It does not accumulate the look stick into
// the globals and does not read them back either: it derives yaw from the
// followed target's forward vector and overwrites 0x348B20 with that value
// every frame. Rotating the target works but the game eases the character's
// facing into the camera, which made the first second of horizontal movement
// disappear. Instead keep our own raw yaw offset and add it back after the
// controller has written the target-derived yaw, so the view turns on the next
// camera build without waiting for the character to catch up.
constexpr uint8_t kModeUnarmed = 23;

// radians of raw mouse yaw on top of the unarmed controller's target angle
float g_unarmed_yaw = 0;

// Adds the pending raw mouse delta to the camera angles.  The caller injects
// before the controller runs and widens that controller's own pitch clamp, so
// the controller does not clamp the result back to its stock -40..+60 range.
void ApplyAim(uint8_t *base, uint32_t controller, float pitch_min, float pitch_max) {
    // tells input this camera is still the one updating; when it stops, input
    // falls back to the right stick for the new camera mode
    input::UseDirectMouseAim();
    float dx = 0, dy = 0;
    if (!input::ConsumeMouseAim(dx, dy)) return;

    const float percent = float(std::clamp(input::g_mouse_sensitivity_percent, 10u, 400u)) * 0.01f;
    const float vertical = float(std::clamp(input::g_mouse_vertical_sensitivity_percent, 10u, 400u)) * 0.01f;
    const float yaw_scale = kRadiansPerPixel * percent;
    const float pitch_scale = kRadiansPerPixel * percent * vertical;
    const uint32_t game = Rd(base, kGame);
    const float yaw_sign = Rd8(base, game + kInvertX) ? -1.0f : 1.0f;
    // on foot the accumulator is positive looking up; raw Y is positive when
    // the mouse moves down, so the default sign is -1 and invert-Y flips it
    const float pitch_sign = Rd8(base, game + kInvertY) ? -1.0f : 1.0f;

    const float yaw = RdF(base, kYaw) + dx * yaw_scale * yaw_sign;
    const float pitch = std::clamp(RdF(base, kPitch) + dy * pitch_scale * pitch_sign, pitch_min, pitch_max);
    std::memcpy(base + kYaw, &yaw, sizeof(yaw));
    std::memcpy(base + kPitch, &pitch, sizeof(pitch));
}

// Unarmed camera: keep the raw yaw offset and write the pitch accumulator.
// Pitch survives the controller; yaw does not, because the controller replaces
// it with the target-derived angle, so the caller adds g_unarmed_yaw back after
// the controller runs.
void ApplyUnarmedAim(uint8_t *base, uint32_t controller, float pitch_min, float pitch_max) {
    // tells input this camera is still the one updating; when it stops, input
    // falls back to the right stick for the new camera mode
    input::UseDirectMouseAim();
    float dx = 0, dy = 0;
    if (!input::ConsumeMouseAim(dx, dy)) return;

    const float percent = float(std::clamp(input::g_mouse_sensitivity_percent, 10u, 400u)) * 0.01f;
    const float vertical = float(std::clamp(input::g_mouse_vertical_sensitivity_percent, 10u, 400u)) * 0.01f;
    const uint32_t game = Rd(base, kGame);
    const float yaw_sign = Rd8(base, game + kInvertX) ? -1.0f : 1.0f;
    const float pitch_sign = Rd8(base, game + kInvertY) ? -1.0f : 1.0f;

    g_unarmed_yaw += dx * kRadiansPerPixel * percent * yaw_sign;
    const float pitch =
        std::clamp(RdF(base, kPitch) + dy * kRadiansPerPixel * percent * vertical * pitch_sign, pitch_min, pitch_max);
    std::memcpy(base + kPitch, &pitch, sizeof(pitch));
}

// Widen a look controller's pitch clamp for one update, so the controller's own
// clamp does not undo the PC range.  The stock fields are restored by the
// caller before it returns, keeping the gamepad path original.
bool WidenPitch(uint8_t *base, uint32_t controller, float min, float max, uint32_t &old_min, uint32_t &old_max) {
    if (!mem::IsCommitted(controller + 0x20) || !mem::IsCommitted(controller + 0x24)) return false;
    old_min = X86_MEM32(controller + 0x20);
    old_max = X86_MEM32(controller + 0x24);
    std::memcpy(base + controller + 0x20, &min, sizeof(min));
    std::memcpy(base + controller + 0x24, &max, sizeof(max));
    return true;
}

void RestorePitch(uint8_t *base, uint32_t controller, uint32_t old_min, uint32_t old_max) {
    std::memcpy(base + controller + 0x20, &old_min, sizeof(old_min));
    std::memcpy(base + controller + 0x24, &old_max, sizeof(old_max));
}

} // namespace

// Every camera mode. The camera update reads the active look controller and
// calls its virtual methods; injecting here, before those methods, covers the
// on-foot, precision aim, crosshair, drive-by and mission cameras in one place.
// The vehicle path is left to sub_0001FD40 below because it rebuilds its chase
// yaw every frame. The unarmed mode keeps the raw counts as an offset on top
// of that target angle, because the controller replaces the accumulator with
// the target's forward angle.
X86_EXTERN_FUNC(__imp__sub_0002E200);
X86_FUNC(sub_0002E200) {
    const uint32_t camera = ctx.ecx;
    const uint32_t controller = camera ? Rd(base, camera + kActive) : 0;
    const uint8_t mode = camera ? Rd8(base, camera + kMode) : 0;
    static uint8_t last_mode = 0xFF;
    if (mode != last_mode) {
        last_mode = mode;
        // the unarmed view starts centred on the target again
        if (mode == kModeUnarmed) g_unarmed_yaw = 0;
    }
    const bool owned = controller && Rd(base, controller + kOwner) == camera;
    if (input::g_direct_mouse_aim && owned && mode != kModeVehicle && IsGameplayCamera(base, camera)) {
        uint32_t old_min = 0, old_max = 0;
        const bool widen = input::DirectAimRecent() &&
                           WidenPitch(base, controller, kOnFootPitchMin, kOnFootPitchMax, old_min, old_max);
        if (mode == kModeUnarmed) {
            ApplyUnarmedAim(base, controller, kOnFootPitchMin, kOnFootPitchMax);
        } else {
            ApplyAim(base, controller, kOnFootPitchMin, kOnFootPitchMax);
        }
        __imp__sub_0002E200(ctx, base);
        if (widen) RestorePitch(base, controller, old_min, old_max);
        if (mode == kModeUnarmed) {
            // the controller just replaced the yaw with the target's angle
            const float yaw = RdF(base, kYaw) + g_unarmed_yaw;
            std::memcpy(base + kYaw, &yaw, sizeof(yaw));
        }
        return;
    }
    __imp__sub_0002E200(ctx, base);
}

// In vehicle.  sub_00024040 (mode 13) rebuilds its chase yaw each frame and
// calls this method to turn the yaw/pitch globals into the camera orientation.
// Injecting before this call puts the mouse angles in the same place the PC
// fix does, and holding them across frames gives the same absolute aim instead
// of a per-frame nudge that the chase camera would erase.
//
// The method is shared by every look controller, so it only touches the
// gameplay camera when mode 13 is active and the caller is that mode's active
// controller.
X86_EXTERN_FUNC(__imp__sub_0001FD40);
X86_FUNC(sub_0001FD40) {
    static bool held = false;
    static float yaw = 0, pitch = 0;
    static uint64_t last_vehicle_at = 0;

    const uint32_t controller = ctx.ecx;
    if (input::g_direct_mouse_aim) {
        const uint32_t camera = GameplayCamera(base, controller);
        if (camera && Rd8(base, camera + kMode) == kModeVehicle && Rd(base, camera + kActive) == controller) {
            // every look controller calls this shared method, so only the
            // vehicle controller updates the hold; an unrelated call must not
            // clear it and force the chase pitch back in
            const uint64_t now = NowMs();
            if (now - last_vehicle_at > 500) held = false;
            last_vehicle_at = now;
            input::UseDirectMouseAim();

            float dx = 0, dy = 0;
            if (input::ConsumeMouseAim(dx, dy)) {
                if (!held) {
                    // start from the chase angles the vehicle controller just
                    // computed, exactly like the PC fix seeding its accumulator
                    yaw = RdF(base, kYaw);
                    pitch = RdF(base, kPitch);
                    held = true;
                }
                const float percent = float(std::clamp(input::g_mouse_sensitivity_percent, 10u, 400u)) * 0.01f;
                const float vertical =
                    float(std::clamp(input::g_mouse_vertical_sensitivity_percent, 10u, 400u)) * 0.01f;
                const uint32_t game = Rd(base, kGame);
                const float yaw_sign = Rd8(base, game + kInvertX) ? -1.0f : 1.0f;
                // here the accumulator is positive looking down, the opposite
                // of the on-foot controllers
                const float pitch_sign = Rd8(base, game + kInvertY) ? 1.0f : -1.0f;
                yaw += dx * kRadiansPerPixel * percent * yaw_sign;
                pitch = std::clamp(pitch + dy * kRadiansPerPixel * percent * vertical * pitch_sign, kVehiclePitchMin,
                                   kVehiclePitchMax);
            }

            if (held && input::DirectAimRecent()) {
                std::memcpy(base + kYaw, &yaw, sizeof(yaw));
                std::memcpy(base + kPitch, &pitch, sizeof(pitch));
            } else {
                // 5 s without a sample: let the game's chase camera take over
                held = false;
            }
        }
    }

    __imp__sub_0001FD40(ctx, base);
}
