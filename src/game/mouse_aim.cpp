// Direct mouse aim for the gameplay camera.
//
// The camera object is [[0x360E00] + 0xAC]. It owns one look controller per
// camera mode in an array at +0x114, indexed by the mode byte at +0x19; the
// active controller is cached at +0x174. The shared per-frame camera update
// sub_0002E200 reads that active controller and calls its virtual methods, so
// injecting the mouse there covers the camera modes that accumulate the globals:
// on foot (mode 5), precision aim / crosshair (modes 0/6/11), drive-by, sniper
// and the other mission cameras, not just the four modes the PC fix names. The
// unarmed and transition controllers can replace the yaw before building the
// view. Keep the requested heading for this update and restore it immediately
// before their shared orientation builder reads it.
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
// the mouse is active (PC convention, positive up; the game's accumulator is
// positive looking down, so the on-foot clamp below is mirrored), and the
// controllers' own -40..+60 limit is widened for that one update and restored
// afterwards. Direct aim stops feeding the stick for 5 s after the last
// sample, matching the PC fix's idle fallback.
//
// The profile's camera auto-center also has to be held off while the mouse is
// in use. It pulls the pitch back toward the controller's rest angle whenever
// the right stick is idle, and direct aim keeps the stick centred, so without
// that it would erase each mouse adjustment on the same frame. The setting is
// restored after the update; when a controller takes over the input, the
// game's centering returns with it.
//
// The on-foot controller also writes its rest pitch over 0x348A80 before the
// shared orientation builder runs, independently of the auto-center setting.
// The mouse therefore keeps its own accumulated pitch and restores it at that
// builder; reading the global back each frame would only ever apply one frame
// of vertical movement.
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
// Profile setting: while the right stick is idle, the look controller blends
// the pitch back toward its rest angle. Set by the game's camera settings
// update; read only by the shared look-input routine sub_000205A0.
constexpr uint32_t kAutoCenter = 0x348C24;
constexpr uint32_t kInvertX = 0xFEF;   // game + 0xFEF
constexpr uint32_t kInvertY = 0xFEE;   // game + 0xFEE

constexpr uint8_t kModeVehicle = 13;

// The PC WidescreenFix takes raw counts straight to camera angles:
//     scale = fMouseSens (4.0) * RawInputMouse (1.0) / 1000 = 0.004 rad/count
// and applies the same scale to both axes.
constexpr float kRadiansPerPixel = 0.004f;
constexpr float kPi = 3.14159265358979f;

// 0x348A80 is positive looking down: sub_0001FD40 negates it when it builds
// the orientation, and the look controllers subtract a stick-up input from it.
// The PC fix's limits are quoted in its own positive-up convention, so the
// on-foot pair is mirrored here (60 up / 89 down). The vehicle controller
// already uses positive down, so its pair needs no mirroring.
constexpr float kOnFootPitchMin = -60.0f * kPi / 180.0f;  // view up
constexpr float kOnFootPitchMax = 89.0f * kPi / 180.0f;   // view down
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
// A zone load or a car exit clears the followed target for a moment, so the
// target cannot be part of this test: the direct path has to stay live through
// that window or the mouse drops to the pad and its horizontal dead zone.
bool IsGameplayCamera(uint8_t *base, uint32_t camera) {
    const uint32_t game = Rd(base, kGame);
    if (!camera || !game || camera != Rd(base, game + kCamera)) return false;
    return Rd8(base, camera + kActiveFlag) != 0;
}

// Scoped to a shared camera update: other cameras and unrelated calls to the
// orientation builder must keep their own angles. Thread-local because guest
// threads can build other cameras while the gameplay update is running.
struct CameraAim {
    uint32_t controller = 0;
    float yaw = 0;
    float pitch = 0;
    bool active = false;   // the mouse sampled recently (yaw handoff)
    bool mouse_aim = false; // keyboard/mouse owns the aim (pitch hold)
};
thread_local CameraAim g_camera_aim;

// Pitch accumulated by the mouse while keyboard/mouse owns the on-foot camera.
// The controller's target pass writes its rest angle over 0x348A80 before the
// orientation builder runs, so reading that global back each frame would only
// ever apply one frame of mouse movement. Keep our own angle instead, seeded
// from the game's pitch once, and use it for the view and the update.
struct AimHold {
    uint32_t controller = 0;
    uint8_t mode = 0;
    float pitch = 0;
    bool held = false;
};
thread_local AimHold g_aim_hold;

// Adds the pending raw mouse delta to the camera angles.  The caller injects
// before the controller runs and widens that controller's own pitch clamp, so
// the controller does not clamp the result back to its stock -40..+60 range.
// Returns the pitch the view should use and whether the mouse owns the aim.
float ApplyAim(uint8_t *base, uint32_t controller, uint8_t mode, float pitch_min, float pitch_max, bool &mouse_aim) {
    // tells input this camera is still the one updating; when it stops, input
    // falls back to the right stick for the new camera mode
    input::UseDirectMouseAim();
    float dx = 0, dy = 0;
    const bool sampled = input::ConsumeMouseAim(dx, dy);
    mouse_aim = input::KeyboardMouseActive() || input::DirectAimRecent();
    if (!mouse_aim) {
        g_aim_hold.held = false;
        return RdF(base, kPitch);
    }
    if (!g_aim_hold.held || g_aim_hold.controller != controller || g_aim_hold.mode != mode) {
        // Start from the game's current angle, then keep the mouse's own so the
        // controller's per-frame rest-pitch reset cannot undo the accumulation.
        g_aim_hold = {controller, mode, RdF(base, kPitch), true};
    }

    const float percent = float(std::clamp(input::g_mouse_sensitivity_percent, 10u, 400u)) * 0.01f;
    const float vertical = float(std::clamp(input::g_mouse_vertical_sensitivity_percent, 10u, 400u)) * 0.01f;
    const float yaw_scale = kRadiansPerPixel * percent;
    const float pitch_scale = kRadiansPerPixel * percent * vertical;
    const uint32_t game = Rd(base, kGame);
    const float yaw_sign = Rd8(base, game + kInvertX) ? -1.0f : 1.0f;
    // 0x348A80 is positive looking down, and SDL/WM_INPUT report raw Y
    // positive when the mouse moves down, so the default adds dy. Invert-Y
    // flips that. Playing with the stock setting must not invert the view.
    const float pitch_sign = Rd8(base, game + kInvertY) ? -1.0f : 1.0f;

    float yaw = RdF(base, kYaw);
    if (sampled) {
        yaw += dx * yaw_scale * yaw_sign;
        g_aim_hold.pitch = std::clamp(g_aim_hold.pitch + dy * pitch_scale * pitch_sign, pitch_min, pitch_max);
    }
    std::memcpy(base + kYaw, &yaw, sizeof(yaw));
    std::memcpy(base + kPitch, &g_aim_hold.pitch, sizeof(g_aim_hold.pitch));
    return g_aim_hold.pitch;
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
// yaw every frame. Controllers using the shared orientation builder also get
// the requested yaw there, after their own target/transition calculations.
X86_EXTERN_FUNC(__imp__sub_0002E200);
X86_FUNC(sub_0002E200) {
    const uint32_t camera = ctx.ecx;
    const uint32_t controller = camera ? Rd(base, camera + kActive) : 0;
    const uint8_t mode = camera ? Rd8(base, camera + kMode) : 0;

    const bool owned = controller && Rd(base, controller + kOwner) == camera;
    if (input::g_direct_mouse_aim && owned && mode != kModeVehicle && IsGameplayCamera(base, camera)) {
        uint32_t old_min = 0, old_max = 0;
        bool mouse_aim = false;
        const float pitch = ApplyAim(base, controller, mode, kOnFootPitchMin, kOnFootPitchMax, mouse_aim);
        const bool recent = input::DirectAimRecent();
        const bool widen = mouse_aim &&
                           WidenPitch(base, controller, kOnFootPitchMin, kOnFootPitchMax, old_min, old_max);
        // The look controller's input step centers the pitch when the stick is
        // idle. Suppress that for this update, or it pulls the view straight
        // back to the rest angle the mouse just moved away from.
        const uint8_t auto_center = Rd8(base, kAutoCenter);
        if (mouse_aim) base[kAutoCenter] = 0;
        const CameraAim previous = g_camera_aim;
        g_camera_aim = {controller, RdF(base, kYaw), pitch, recent, mouse_aim};
        __imp__sub_0002E200(ctx, base);
        g_camera_aim = previous;
        if (mouse_aim) base[kAutoCenter] = auto_center;
        if (widen) RestorePitch(base, controller, old_min, old_max);
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
// active gameplay controller. Non-vehicle updates carry a scoped requested
// heading from sub_0002E200, including unarmed and transition controllers.
X86_EXTERN_FUNC(__imp__sub_0001FD40);
X86_FUNC(sub_0001FD40) {
    static bool held = false;
    static float yaw = 0, pitch = 0;
    static uint64_t last_vehicle_at = 0;
    static uint32_t last_vehicle_camera = 0, last_vehicle_controller = 0;

    const uint32_t controller = ctx.ecx;
    if (input::g_direct_mouse_aim && g_camera_aim.controller == controller) {
        // Applying this after sub_0002E200 returns is too late: this method
        // has already used the controller's replacement yaw to build the view.
        // The same update also rewrites the pitch to the controller's rest
        // angle before coming here, so put the held mouse pitch back as well.
        if (g_camera_aim.active) std::memcpy(base + kYaw, &g_camera_aim.yaw, sizeof(g_camera_aim.yaw));
        if (g_camera_aim.mouse_aim) std::memcpy(base + kPitch, &g_camera_aim.pitch, sizeof(g_camera_aim.pitch));
    }
    if (input::g_direct_mouse_aim) {
        const uint32_t camera = GameplayCamera(base, controller);
        if (camera && Rd(base, camera + kActive) == controller && Rd8(base, camera + kMode) != kModeVehicle)
            held = false;
        if (camera && Rd8(base, camera + kMode) == kModeVehicle && Rd(base, camera + kActive) == controller) {
            // every look controller calls this shared method, so only the
            // vehicle controller updates the hold; an unrelated call must not
            // clear it and force the chase pitch back in
            const uint64_t now = NowMs();
            if (now - last_vehicle_at > 500 || !input::DirectAimRecent() ||
                camera != last_vehicle_camera || controller != last_vehicle_controller)
                held = false;
            last_vehicle_at = now;
            last_vehicle_camera = camera;
            last_vehicle_controller = controller;
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
                // Here the accumulator is positive looking down as well, so
                // the default does not negate dy; invert-Y flips that.
                const float pitch_sign = Rd8(base, game + kInvertY) ? -1.0f : 1.0f;
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
