// Impact damage cap. The Xbox version applies "impact" damage from a passing
// car to a player who is standing still, most visibly during the enter/exit
// animation. The amount comes from the car's velocity, so a scrape can deal
// thousands of damage per second and kill the player before the animation
// finishes.
//
// 0x19D3B0 is the central health-damage routine:
//   ecx      = health component (owner at +0x04, max at +0x08, current +0x10)
//   [esp+4]  = damage type hash
//   [esp+8]  = damage amount (float)
//   [esp+0xC]= source object
// DAMAGETYPE_IMPACT (0x2B2D2DAF) is the collision/impact category; bullets,
// explosions, fire and falls use other hashes. All impact damage to the
// player, from any source, is limited to a fraction of max health per second
// of game time. That keeps a car hit dangerous but stops a scrape from being
// instant death. The cap is based on the game's own max-health value, so it
// follows difficulty and mission changes.

#include <algorithm>

#include "core/log.h"
#include "common/guest_values.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

namespace {

constexpr uint32_t kPlayer = 0x37D284;
constexpr uint32_t kPlayerHealth = 0xBC;    // player + 0xBC: the player's health component
constexpr uint32_t kHealthOwner = 0x04;
constexpr uint32_t kHealthMax = 0x08;
constexpr uint32_t kSimTicks = 0x363D0C;    // simulation tick counter, 60 a second (timing.cpp)
constexpr uint32_t kTicksPerWindow = 60;    // one second of game time
constexpr uint32_t kDamageTypeImpact = guest::Crc32("DAMAGETYPE_IMPACT"); // 0x2B2D2DAF
constexpr float kMaxImpactPerWindow = 0.15f;       // of max health

// impact damage already taken in the current window; game thread only
uint32_t g_window_start = 0;
float g_window_damage = 0.0f;
bool g_window_open = false;

} // namespace

X86_EXTERN_FUNC(__imp__sub_0019D3B0);
X86_FUNC(sub_0019D3B0) {
    const uint32_t health = ctx.ecx;
    const uint32_t type = X86_MEM32(ctx.esp + 4);
    if (health && mem::IsCommitted(health + kHealthOwner) && mem::IsCommitted(health + kHealthMax + 3)) {
        const uint32_t player = kernel::R32(kPlayer);
        const uint32_t player_health =
            player && mem::IsCommitted(player + kPlayerHealth + 3) ? kernel::R32(player + kPlayerHealth) : 0;
        const bool is_player = player && (kernel::R32(health + kHealthOwner) == player || health == player_health);
        const float amount = guest::ReadF32(ctx.esp + 8);
        const float max_health = guest::ReadF32(health + kHealthMax);
        // the first few player damage types, so a scrape with another hash can be found (docs/glitch-faq-triage.md)
        if (is_player) LOG_FIRST(16, "tcnyc: player damage type %08X amount %.1f", type, amount);
        if (is_player && type == kDamageTypeImpact && amount > 0.0f && max_health > 0.0f) {  // also false for NaN
            // game time, so pause and slow motion do not refill the allowance
            const uint32_t now = kernel::R32(kSimTicks);
            if (!g_window_open || now - g_window_start >= kTicksPerWindow) {
                g_window_start = now;
                g_window_damage = 0.0f;
                g_window_open = true;
            }
            const float remaining = std::max(0.0f, max_health * kMaxImpactPerWindow - g_window_damage);
            const float allowed = std::min(amount, remaining);
            // every hit counts toward the window, so a stream of small hits is capped too
            g_window_damage += allowed;
            if (allowed < amount) {
                guest::WriteF32(ctx.esp + 8, allowed);
                LOG_FIRST(8, "tcnyc: impact damage on player capped %.1f -> %.1f (max %.1f)", amount, allowed,
                          max_health);
            }
        }
    }
    __imp__sub_0019D3B0(ctx, base);
}
