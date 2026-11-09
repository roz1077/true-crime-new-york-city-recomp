// Developer-only script stage jumps. Built only with TCNYC_DEV_TOOLS.
//
// Every AI/mission script instance passes through 0xFD220 (its per-frame
// update) once a frame: global mission scripts from the game's list
// (game+0x1C4, 0x99B40), character scripts from each character's list
// (character+0x28, 0xFD340). This hook records every instance it sees, so the
// debug menu can list them with their stages, and applies a requested stage
// right before the game's own update.
//
// Instance layout (see docs/reverse-engineering-notes.md):
//   +0x424  current state (CRC-32 of the state name)
//   +0x478  the script's state table: a tag node whose children are the states
//   +0x490  requested state: the script native SetState (0x17D680 -> 0xFC090)
//           writes it; the update enters it (0xFD0F0) and moves it to +0x424
// Tag node: u16 count, u16 flags, then 16-byte entries {.., name hash +4,
// flags +8, u16 type +0xA, child offset +0xC (from the entry)}.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "core/log.h"
#include "dev/dev.h"
#include "dev/script_names.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

namespace {

constexpr uint32_t kCurrentState = 0x424;
constexpr uint32_t kStateTable = 0x478;
constexpr uint32_t kRequestedState = 0x490;
// stack of characters whose scripts 0xFD340 is running: depth, then the slots
constexpr uint32_t kOwnerDepth = 0x2E78C0;
constexpr uint32_t kOwnerStack = 0x37BE28;

struct Live {
    uint32_t owner = 0;
    uint64_t seen = 0;
};

std::mutex g_lock;
std::unordered_map<uint32_t, Live> g_live;            // instance -> owner, last frame seen
std::unordered_map<uint32_t, uint32_t> g_requests;    // instance -> state hash
uint64_t g_frame = 0;

uint32_t Crc32(const char *s) {
    uint32_t crc = 0xFFFFFFFFu;
    for (; *s; ++s) {
        crc ^= uint8_t(*s);
        for (int i = 0; i < 8; ++i) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

bool Readable(uint32_t va, uint32_t size = 4) {
    return va && mem::IsCommitted(va) && mem::IsCommitted(va + size - 1);
}

} // namespace

namespace dev {

std::string ScriptName(uint32_t hash) {
    static const std::unordered_map<uint32_t, const char *> names = [] {
        std::unordered_map<uint32_t, const char *> map;
        for (const char *name : kScriptNames) map.emplace(Crc32(name), name);
        return map;
    }();
    if (auto it = names.find(hash); it != names.end()) return it->second;
    char hex[16];
    std::snprintf(hex, sizeof(hex), "%08X", hash);
    return hex;
}

void ScriptsFrame() {
    std::lock_guard guard(g_lock);
    ++g_frame;
    // forget instances the game stopped updating (deleted, or the level changed)
    for (auto it = g_live.begin(); it != g_live.end();)
        it = g_frame - it->second.seen > 30 ? g_live.erase(it) : std::next(it);
}

std::vector<Script> LiveScripts() {
    std::vector<std::pair<uint32_t, Live>> live;
    {
        std::lock_guard guard(g_lock);
        live.assign(g_live.begin(), g_live.end());
    }
    std::vector<Script> out;
    for (auto [instance, info] : live) {
        if (!Readable(instance + kCurrentState) || !Readable(instance + kRequestedState)) continue;
        // the owner slot holds the character's object first (0xFCFA0); its name as dev_objects.cpp reads it
        uint32_t owner_name = 0;
        if (Readable(info.owner)) {
            const uint32_t object = kernel::R32(info.owner);
            if (Readable(object + 0x38)) {
                const uint32_t names = kernel::R32(object + 0x38);
                if (Readable(names, 8) && kernel::R32(names)) {
                    const uint32_t entry = kernel::R32(names + 4) + uint32_t(kernel::R16(object + 0x1E)) * 64;
                    if (Readable(entry + 0x20)) owner_name = kernel::R32(entry + 0x20);
                }
            }
        }
        Script script{instance, info.owner, owner_name, kernel::R32(instance + kCurrentState), {}};
        const uint32_t table = kernel::R32(instance + kStateTable);
        if (Readable(table)) {
            const uint16_t count = kernel::R16(table);
            for (uint32_t i = 0; i < count && i < 256; ++i) {
                const uint32_t entry = table + 4 + i * 16;
                if (!Readable(entry, 16)) break;
                // states are child nodes (type 1)
                if (kernel::R16(entry + 0xA) != 1) continue;
                script.states.push_back(kernel::R32(entry + 4));
            }
        }
        if (!script.states.empty()) out.push_back(std::move(script));
    }
    std::sort(out.begin(), out.end(), [](const Script &a, const Script &b) {
        return a.owner != b.owner ? a.owner < b.owner : a.instance < b.instance;
    });
    return out;
}

void RequestState(uint32_t instance, uint32_t state_hash) {
    std::lock_guard guard(g_lock);
    g_requests[instance] = state_hash;
}

} // namespace dev

// 0xFD220: one script instance's update (thiscall, ecx = instance)
X86_EXTERN_FUNC(__imp__sub_000FD220);
X86_FUNC(sub_000FD220) {
    const uint32_t instance = ctx.ecx;
    {
        std::lock_guard guard(g_lock);
        // the stack starts at -1; 0xFCFA0 reads slot [depth] for depth >= 0
        const int32_t depth = int32_t(kernel::R32(kOwnerDepth));
        const uint32_t owner = depth >= 0 && depth < 8 ? kernel::R32(kOwnerStack + uint32_t(depth) * 4) : 0;
        g_live[instance] = {owner, g_frame};
        if (auto it = g_requests.find(instance); it != g_requests.end()) {
            // as the script's own SetState: the update below enters it
            kernel::W32(instance + kRequestedState, it->second);
            LOGR("dev: script %08X (owner %08X): state %s -> %s", instance, owner,
                 dev::ScriptName(kernel::R32(instance + kCurrentState)).c_str(), dev::ScriptName(it->second).c_str());
            g_requests.erase(it);
        }
    }
    const uint32_t before = kernel::R32(instance + kCurrentState);
    __imp__sub_000FD220(ctx, base);
    const uint32_t after = kernel::R32(instance + kCurrentState);
    if (dev::TraceScripts() && after != before)
        LOGR("dev trace: frame %llu script %08X state %s -> %s", static_cast<unsigned long long>(g_frame), instance,
             dev::ScriptName(before).c_str(), dev::ScriptName(after).c_str());
}

// Script event trace (debug menu Stages tab): natives that matter for timed moves, with the
// frame, the running script instance (0x37BE08[[0x2E78BC]]) and its state. A native gets the
// script VM stack pointer as its argument; its arguments sit just below it.
namespace {
constexpr uint32_t kRunningDepth = 0x2E78BC;
constexpr uint32_t kRunningStack = 0x37BE08;

std::string Running() {
    const int32_t depth = int32_t(kernel::R32(kRunningDepth));
    const uint32_t instance = depth >= 0 && depth < 8 ? kernel::R32(kRunningStack + uint32_t(depth) * 4) : 0;
    if (!Readable(instance + kCurrentState)) return "no script";
    char text[96];
    std::snprintf(text, sizeof(text), "script %08X in %s", instance,
                  dev::ScriptName(kernel::R32(instance + kCurrentState)).c_str());
    return text;
}

float StackFloat(uint32_t va) {
    const uint32_t bits = kernel::R32(va);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}
} // namespace

// SetGrabbable(bool) 0x17FA70: character +0xA8 -> +0x24 bit 0x200 = not grabbable
X86_EXTERN_FUNC(__imp__sub_0017FA70);
X86_FUNC(sub_0017FA70) {
    if (dev::TraceScripts()) {
        const uint32_t vm = kernel::R32(ctx.esp + 4);
        LOGR("dev trace: frame %llu SetGrabbable(%u) from %s", static_cast<unsigned long long>(g_frame),
             kernel::G<uint8_t>(vm - 4), Running().c_str());
    }
    __imp__sub_0017FA70(ctx, base);
}

// SetTimer(seconds, id) 0x1828C0
X86_EXTERN_FUNC(__imp__sub_001828C0);
X86_FUNC(sub_001828C0) {
    if (dev::TraceScripts()) {
        const uint32_t vm = kernel::R32(ctx.esp + 4);
        LOGR("dev trace: frame %llu SetTimer(%.2f s, %s) from %s", static_cast<unsigned long long>(g_frame),
             StackFloat(vm - 8), dev::ScriptName(kernel::R32(vm - 4)).c_str(), Running().c_str());
    }
    __imp__sub_001828C0(ctx, base);
}

// EnablePlayerScriptedSequence 0x185D80 / Disable 0x185D90
X86_EXTERN_FUNC(__imp__sub_00185D80);
X86_FUNC(sub_00185D80) {
    if (dev::TraceScripts())
        LOGR("dev trace: frame %llu EnablePlayerScriptedSequence from %s", static_cast<unsigned long long>(g_frame),
             Running().c_str());
    __imp__sub_00185D80(ctx, base);
}
X86_EXTERN_FUNC(__imp__sub_00185D90);
X86_FUNC(sub_00185D90) {
    if (dev::TraceScripts())
        LOGR("dev trace: frame %llu DisablePlayerScriptedSequence from %s", static_cast<unsigned long long>(g_frame),
             Running().c_str());
    __imp__sub_00185D90(ctx, base);
}

// The sumo grapple trace (Tommy's container fight in Human Cargo), called around
// 0x58D90 by src/game/sumo_grapple.cpp: logs every state change and every time the
// +0x30 timer crosses zero, with real time, so the push-off window can be measured.
namespace {
uint8_t g_sumo_state = 0;
float g_sumo_timer = 0;
} // namespace

namespace dev {

void SumoBefore(uint32_t grapple) {
    g_sumo_state = kernel::G<uint8_t>(grapple + 0x0C);
    g_sumo_timer = StackFloat(grapple + 0x30);
}

void SumoAfter(uint32_t grapple, bool push_window) {
    if (!TraceScripts() || !Readable(grapple + 0x44)) return;
    const uint8_t state = kernel::G<uint8_t>(grapple + 0x0C);
    const float timer = StackFloat(grapple + 0x30);
    if (state == g_sumo_state && (g_sumo_timer > 0) == (timer > 0)) return;
    static const auto start = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    LOGR("dev sumo: %.1f ms frame %llu grapple %08X state %u -> %u, timer %.3f -> %.3f, balance %.3f, "
         "timer2 %.3f, dt %.4f%s",
         ms, static_cast<unsigned long long>(g_frame), grapple, g_sumo_state, state, g_sumo_timer, timer,
         StackFloat(grapple + 0x40), StackFloat(grapple + 0x44), StackFloat(0x37F630),
         push_window ? " (push window, slowed)" : "");
}

} // namespace dev

namespace dev {
namespace {
// XBE_DEV_TRACE=1 turns it on from the start
std::atomic<bool> g_trace{std::getenv("XBE_DEV_TRACE") != nullptr};
}
bool TraceScripts() { return g_trace; }
void SetTraceScripts(bool on) {
    g_trace = on;
    LOGR("dev trace: %s", on ? "on" : "off");
}
} // namespace dev
