// Developer-only player cheats and game speed. Built only with TCNYC_DEV_TOOLS.
//
// Player character: the global [0x37D284] (its wallet at +0xB4 is what the
// script native AddMoney uses on a character). Found through the script
// natives (see docs/reverse-engineering-notes.md):
//   +0xB4  wallet; vtable +0x8C = AddMoney(amount), thiscall   (AddMoney, 0x184460)
//   +0xBC  health component: max +0x08, health +0x10 (floats),
//          flags +0x18, bit 0x04 = ignore damage            (Get/SetHealth, IgnoreDamage)
//   SetAmmo: 0x16FCC0(count), cdecl                          (SetAmmo, 0x181CC0)
//   Good Cop points: 0x854A0(points) on game+0x1D0, thiscall (GiveGoodCopPoints, 0x17E810)
//   the player's Good / Bad Cop points: ints at game+0x1F8 / +0x1FC        (GoodCopPct, 0x170E50)
//   career points: 0x802E0(points) on game+0x1D0, thiscall               (GiveCareerPoints, 0x17E870)
//   giving a weapon = CreateInventoryItem(name) then AddToInventory(item), as a script does:
//     CreateInventoryItem (0x1714F0): item = 0x9F460(name hash, character position from 0x21EC70);
//       inventory.0x191650(character, item); item+0x14 |= 3
//     AddToInventory (0x1848C0) takes that item object, not a name:
//       0x191650(character, item); vtable +0x20(character, 1, item, 0); vtable +0x24(character, 1, item, 0, 0)
// Writes and calls happen on the game thread (PlayerFrame, from OnFrame).

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

#include "common/guest_values.h"
#include "core/log.h"
#include "dev/dev.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace {

constexpr uint32_t kPlayer = 0x37D284;
constexpr uint32_t kGame = 0x360E00;
constexpr uint32_t kWallet = 0xB4;
constexpr uint32_t kHealth = 0xBC;
constexpr uint32_t kHealthMax = 0x08, kHealthNow = 0x10, kHealthFlags = 0x18;
constexpr uint32_t kIgnoreDamage = 0x04;
constexpr uint32_t kAddMoneySlot = 0x8C;
constexpr uint32_t kSetAmmo = 0x16FCC0;
constexpr uint32_t kGiveGoodCop = 0x854A0;
constexpr uint32_t kCopPoints = 0x1D0;
constexpr uint32_t kGoodCop = 0x1F8, kBadCop = 0x1FC;
constexpr uint32_t kGiveCareer = 0x802E0;
constexpr uint32_t kInventoryAdd = 0x191650;   // thiscall on the inventory (character, item)
constexpr uint32_t kGetPosition = 0x21EC70;    // thiscall on an object (out vec4*) -> vec4*
constexpr uint32_t kSpawnItem = 0x9F460;       // cdecl (item name hash, vec4* position) -> item object
constexpr uint32_t kItemInHand = 0x191800;     // thiscall on the inventory (hand: 1 right, 0 left) -> item
constexpr uint32_t kRemoveNickWeapons = 0x110AD0; // cdecl (character): removes every weapon (RemoveNickWeapon)

std::atomic<int> g_bad{0}, g_career{0};
std::mutex g_items_lock;
std::vector<std::pair<uint32_t, uint32_t>> g_items; // item name hash, hand (1 right, 0 left)

std::atomic<bool> g_god{false}, g_ammo{false}, g_allow_switch{false};
constexpr uint32_t kSwitchDisabled = 0xA8; // inventory byte set by DisableSwitchItems
constexpr uint32_t kInventoryFlags = 0x50, kNoWeapons = 0x4020;
std::atomic<int> g_money{0}, g_cop{0};
std::atomic<bool> g_heal{false};
// game speed: 0 paused, else ticks per 4 frames (1 = quarter speed .. 16 = four times)
std::atomic<int> g_speed{4};
std::atomic<int> g_steps{0};

bool Readable(uint32_t va, uint32_t size = 4) {
    return va >= 0x10000 && mem::IsCommitted(va) && mem::IsCommitted(va + size - 1);
}

uint32_t Player() {
    if (!Readable(kPlayer)) return 0;
    const uint32_t player = kernel::R32(kPlayer);
    return Readable(player + kHealth) ? player : 0;
}

uint32_t HealthComponent(uint32_t player) {
    const uint32_t health = player ? kernel::R32(player + kHealth) : 0;
    return Readable(health + kHealthFlags) ? health : 0;
}

uint32_t CallMethod(X86Context &ctx, uint32_t fn, uint32_t self, std::initializer_list<uint32_t> args) {
    const uint32_t ecx = ctx.ecx;
    ctx.ecx = self;
    const uint32_t r = kernel::CallGuest(ctx, fn, args);
    ctx.ecx = ecx;
    return r;
}

} // namespace

namespace dev {

bool PlayerStatus(float &health, float &max_health) {
    const uint32_t h = HealthComponent(Player());
    if (!h) return false;
    health = guest::ReadF32(h + kHealthNow);
    max_health = guest::ReadF32(h + kHealthMax);
    return true;
}

void SetGodMode(bool on) { g_god = on; }
bool GodMode() { return g_god; }
void SetInfiniteAmmo(bool on) { g_ammo = on; }
void SetAllowWeaponSwitch(bool on) { g_allow_switch = on; }
bool AllowWeaponSwitch() { return g_allow_switch; }
bool InfiniteAmmo() { return g_ammo; }
void RequestHeal() { g_heal = true; }
void RequestMoney(int amount) { g_money += amount; }
void RequestGoodCopPoints(int points) { g_cop += points; }
void RequestBadCopPoints(int points) { g_bad += points; }
void RequestCareerPoints(int points) { g_career += points; }

void RequestItem(const char *item_name, bool left_hand) {
    std::lock_guard guard(g_items_lock);
    g_items.push_back({guest::Crc32(item_name), left_hand ? 0u : 1u});
}

bool CopPoints(int &good, int &bad) {
    if (!Readable(kGame)) return false;
    const uint32_t game = kernel::R32(kGame);
    if (!Readable(game + kBadCop)) return false;
    good = int(kernel::R32(game + kGoodCop));
    bad = int(kernel::R32(game + kBadCop));
    return true;
}

void SetGameSpeed(int quarters) { g_speed = quarters; }
int GameSpeed() { return g_speed; }
void StepFrame() { g_steps += 1; }

uint32_t AdjustTicks(uint32_t ticks) {
    const int speed = g_speed.load();
    if (speed == 4) return ticks;
    if (speed == 0) {
        // paused: only a requested single step runs
        if (g_steps.load() > 0) {
            --g_steps;
            return 1;
        }
        return 0;
    }
    // quarters of a tick, carried from frame to frame
    static int carry = 0;
    carry += int(ticks) * speed;
    const int whole = carry / 4;
    carry %= 4;
    return uint32_t(whole);
}

void PlayerFrame(X86Context &ctx) {
    const uint32_t player = Player();
    const uint32_t health = HealthComponent(player);
    if (!player || !health) return;
    // god mode: keep the IgnoreDamage flag on; turning it off clears it once
    static bool god_was = false;
    const bool god = g_god.load();
    if (god || god_was) {
        uint32_t flags = kernel::R32(health + kHealthFlags);
        flags = god ? flags | kIgnoreDamage : flags & ~kIgnoreDamage;
        kernel::W32(health + kHealthFlags, flags);
        god_was = god;
    }
    if (g_heal.exchange(false)) {
        kernel::W32(health + kHealthNow, kernel::R32(health + kHealthMax));
        LOGR("dev: healed player to %.0f", guest::ReadF32(health + kHealthMax));
    }
    static int frames = 0;
    if (g_ammo && ++frames % 30 == 0) kernel::CallGuest(ctx, kSetAmmo, {999});
    // missions like Human Cargo block weapon switching through the script native
    // DisableSwitchItems (0x184D40), a byte at inventory +0xA8; keep it off while asked
    // ... and missions' handlers block using weapons with bits 0x4020 in inventory +0x50 (Human Cargo's
    // phase 1 at 0x1EBF61; 0x18DD80 refuses an item while they are set)
    if (g_allow_switch) {
        const uint32_t inventory = kernel::R32(player + kWallet);
        if (Readable(inventory + kSwitchDisabled) && kernel::G<uint8_t>(inventory + kSwitchDisabled)) {
            kernel::G<uint8_t>(inventory + kSwitchDisabled) = 0;
            LOGR("dev: weapon switching allowed again");
        }
        if (Readable(inventory + kInventoryFlags) && (kernel::R32(inventory + kInventoryFlags) & kNoWeapons)) {
            kernel::W32(inventory + kInventoryFlags, kernel::R32(inventory + kInventoryFlags) & ~kNoWeapons);
            LOGR("dev: weapons allowed again");
        }
    }
    if (const int money = g_money.exchange(0)) {
        const uint32_t wallet = kernel::R32(player + kWallet);
        if (Readable(wallet) && Readable(kernel::R32(wallet) + kAddMoneySlot)) {
            CallMethod(ctx, kernel::R32(kernel::R32(wallet) + kAddMoneySlot), wallet, {uint32_t(money)});
            LOGR("dev: gave $%d", money);
        }
    }
    const uint32_t game = Readable(kGame) ? kernel::R32(kGame) : 0;
    if (game) {
        if (const int points = g_cop.exchange(0)) {
            CallMethod(ctx, kGiveGoodCop, game + kCopPoints, {uint32_t(points)});
            LOGR("dev: gave %d Good Cop points", points);
        }
        if (const int points = g_bad.exchange(0)) {
            // no game function adds Bad Cop points directly; the counter is a plain int
            const int bad = std::max(0, int(kernel::R32(game + kBadCop)) + points);
            kernel::W32(game + kBadCop, uint32_t(bad));
            LOGR("dev: Bad Cop points now %d", bad);
        }
        if (const int points = g_career.exchange(0)) {
            CallMethod(ctx, kGiveCareer, game + kCopPoints, {uint32_t(points)});
            LOGR("dev: gave %d career points", points);
        }
    }
    std::vector<std::pair<uint32_t, uint32_t>> items;
    {
        std::lock_guard guard(g_items_lock);
        items.swap(g_items);
    }
    const uint32_t inventory = kernel::R32(player + kWallet);
    if (!items.empty() && Readable(inventory) && Readable(kernel::R32(inventory) + 0x24)) {
        const uint32_t vtable = kernel::R32(inventory);
        // a 16-byte aligned guest buffer for the character's position; host-owned, so nothing the
        // game frees on a level change can take it away
        static const uint32_t position = mem::AllocSystem(16, 16);
        for (auto [name, hand] : items) {
            if (!Readable(position, 16)) break;
            // first clear out the old weapons. The inventory has more slots than the two hands (right,
            // left, "either"), and a new weapon can land in a stored slot, so for the right hand clear
            // everything with the game's own RemoveNickWeapon (0x110AD0, as mission starts do); for the
            // left hand only take out what it holds: inventory.0x191800(hand) is the item in a hand,
            // vtable +0x1C(character, item, 1) removes it
            if (hand) {
                kernel::CallGuest(ctx, kRemoveNickWeapons, {player});
                LOGR("dev: removed Nick's weapons");
            } else {
                const uint32_t held = CallMethod(ctx, kItemInHand, inventory, {hand});
                if (Readable(held)) {
                    CallMethod(ctx, kernel::R32(vtable + 0x1C), inventory, {player, held, 1});
                    LOGR("dev: removed %08X from the left hand", held);
                }
            }
            // as the script native CreateInventoryItem (0x1714F0): spawn the item at the character
            const uint32_t at = CallMethod(ctx, kGetPosition, player, {position});
            const uint32_t item = kernel::CallGuest(ctx, kSpawnItem, {name, at ? at : position});
            if (!Readable(item + 0x14)) {
                LOGR("dev: item %08X could not be created", name);
                continue;
            }
            CallMethod(ctx, kInventoryAdd, inventory, {player, item});
            kernel::W32(item + 0x14, kernel::R32(item + 0x14) | 3);
            // then as AddToInventory (0x1848C0) with that item; vtable +0x20 puts it in a hand
            // (SetItemToHand, 0x184690: 1 = right, 0 = left), replacing what that hand held
            CallMethod(ctx, kInventoryAdd, inventory, {player, item});
            CallMethod(ctx, kernel::R32(vtable + 0x20), inventory, {player, hand, item, 0});
            CallMethod(ctx, kernel::R32(vtable + 0x24), inventory, {player, hand, item, 0, 0});
            LOGR("dev: gave item %08X (object %08X) to the %s hand", name, item, hand ? "right" : "left");
        }
    }
}

} // namespace dev
