// Night shadows that pop in and out.
//
// Each frame 0x143650 takes the scene's shadow light: a vector at 0x37F8D0 (a direction with w 0 for the moon,
// a position with w 1 for a street lamp), its colour at 0x37F8BC and its strength at 0x37F8F0. When the light
// changes it fades the shadow out and the new one in, about a second each way. At night the light chosen flips
// between the moon and nearby lamps as the player moves or turns the camera, sometimes every few frames, so
// the shadows keep fading away and back. The console does the same.
//
// The shadow pass keeps using the light it has committed to until another one has been the game's choice for
// kSettle frames, and if the game finds no light at all it keeps the last one for up to kHoldNone frames.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/log.h"

#include "kernel/kernel.h"
#include "xdk/hle.h"

namespace {

constexpr uint32_t kLight = 0x37F8D0, kColour = 0x37F8BC, kStrength = 0x37F8F0;
constexpr int kSettle = 30;     // frames (0.5 s at 60 fps)
constexpr int kHoldNone = 300;  // frames (5 s at 60 fps)

struct Light {
    uint8_t vec[16];
    uint32_t colour;
    float strength;
    bool operator==(const Light &o) const {
        return !std::memcmp(vec, o.vec, 16) && colour == o.colour && strength == o.strength;
    }
};

Light Read(uint8_t *base) {
    Light l;
    std::memcpy(l.vec, base + kLight, 16);
    std::memcpy(&l.colour, base + kColour, 4);
    std::memcpy(&l.strength, base + kStrength, 4);
    return l;
}

void Write(uint8_t *base, const Light &l) {
    std::memcpy(base + kLight, l.vec, 16);
    std::memcpy(base + kColour, &l.colour, 4);
    std::memcpy(base + kStrength, &l.strength, 4);
}

// the same light: same kind and within a unit of it (a lamp's colour flickers slightly from frame to frame)
bool SameLight(const Light &a, const Light &b) {
    float va[4], vb[4];
    std::memcpy(va, a.vec, 16);
    std::memcpy(vb, b.vec, 16);
    if (va[3] != vb[3]) return false;
    float d = 0;
    for (int i = 0; i < 3; i++) d += (va[i] - vb[i]) * (va[i] - vb[i]);
    return d < (va[3] != 0 ? 1.0f : 0.01f);
}

bool g_have = false;
float g_cam_distance = 0, g_drop_distance = 0;

// distance from the camera to a lamp; the scene's view matrix is at +0xD0 of 0x143650's argument
float CameraDistance(uint8_t *base, uint32_t scene, const Light &lamp) {
    float m[16], p[4];
    std::memcpy(m, base + scene + 0xD0, 64);
    std::memcpy(p, lamp.vec, 16);
    float d = 0;
    for (int j = 0; j < 3; j++) {
        float cam = 0;
        for (int i = 0; i < 3; i++) cam -= m[12 + i] * m[j * 4 + i];
        d += (p[j] - cam) * (p[j] - cam);
    }
    return std::sqrt(d);
}
Light g_committed{};
int g_away_frames = 0, g_none_frames = 0;
int g_fit = -1;  // the last fit of the shadow to the view: -1 not reached, 0 failed, 1 done

bool Tracing() {
    static const bool on = std::getenv("TCNYC_TRACE_SHADOW") != nullptr;
    return on;
}

float F(uint8_t *base, uint32_t a) {
    float f;
    std::memcpy(&f, base + a, 4);
    return f;
}

// TCNYC_TRACE_SHADOW=1: the light choice and outcome, logged when they change
void Trace(uint8_t *base, uint32_t self, const Light &now) {
    if (!Tracing()) return;
    float n[4], c[4];
    std::memcpy(n, now.vec, 16);
    std::memcpy(c, g_committed.vec, 16);
    char line[320];
    std::snprintf(line, sizeof(line),
                  "night shadow: game (%.0f %.0f %.0f %.0f) str %.2f, used (%.0f %.0f %.0f %.0f) str %.2f, away %d none %d, "
                  "%s, fade %.1f, cam %.0f drop %.0f",
                  n[0], n[1], n[2], n[3], now.strength, c[0], c[1], c[2], c[3], g_committed.strength, g_away_frames,
                  g_none_frames, g_fit < 0 ? "skipped" : g_fit ? "drawn" : "fit failed", F(base, self + 0x24), g_cam_distance, g_drop_distance);
    static char last[320];
    static int count = 0;
    if (std::strcmp(line, last) && count < 20000) {
        std::strcpy(last, line);
        count++;
        LOGR("%s", line);
    }
}
// the other lights the game picked while the committed one was not picked, with how often
struct Candidate {
    Light light;
    int frames;
};
Candidate g_candidates[4];
int g_candidate_count = 0;

void Note(const Light &l) {
    for (int i = 0; i < g_candidate_count; i++)
        if (SameLight(l, g_candidates[i].light)) {
            g_candidates[i].light = l;
            g_candidates[i].frames++;
            return;
        }
    if (g_candidate_count < 4) {
        g_candidates[g_candidate_count++] = {l, 1};
        return;
    }
    int least = 0;  // full: replace the least picked
    for (int i = 1; i < 4; i++)
        if (g_candidates[i].frames < g_candidates[least].frames) least = i;
    g_candidates[least] = {l, 1};
}

const Light &MostPicked() {
    int best = 0;
    for (int i = 1; i < g_candidate_count; i++)
        if (g_candidates[i].frames > g_candidates[best].frames) best = i;
    return g_candidates[best].light;
}

}  // namespace

X86_EXTERN_FUNC(__imp__sub_00143650);
X86_FUNC(sub_00143650) {
    const Light now = Read(base);
    // A lamp's shadow, not the moon's (too faint to see), is what shows at night. When the game falls back to the
    // moon, keep the last lamp until the camera is half as far again from it as where the game let it go.
    static bool have_lamp = false, lamp_dropped = false;
    static Light lamp{};
    static float drop_distance = 0;
    float w;
    std::memcpy(&w, now.vec + 12, 4);
    const float cam_distance = CameraDistance(base, X86_MEM32(ctx.esp + 4), lamp);
    Light pick = now;
    if (now.strength != 0.0f && w != 0.0f) {
        have_lamp = true;
        lamp_dropped = false;
        lamp = now;
    } else if (have_lamp && now.strength != 0.0f) {
        if (!lamp_dropped) {
            lamp_dropped = true;
            drop_distance = cam_distance;
        }
        if (cam_distance <= drop_distance * 1.5f) pick = lamp;
        else have_lamp = false;
    }
    g_cam_distance = cam_distance;
    g_drop_distance = drop_distance;
    const bool none = pick.strength == 0.0f;
    if (!g_have) {
        if (!none) {
            g_have = true;
            g_committed = pick;
        }
    } else if (none) {
        if (++g_none_frames > kHoldNone) g_committed = pick;  // gone for good
    } else {
        g_none_frames = 0;
        if (SameLight(pick, g_committed)) {
            g_committed = pick;  // follow small changes of the same light
            g_away_frames = 0;
            g_candidate_count = 0;
        } else {
            // the committed light has not been picked for a while: move to the one picked most meanwhile,
            // even when the game keeps alternating between several
            Note(pick);
            if (++g_away_frames >= kSettle) {
                g_committed = MostPicked();
                g_away_frames = 0;
                g_candidate_count = 0;
            }
        }
    }
    const uint32_t self = ctx.ecx;
    g_fit = -1;
    if (!g_have || g_committed == now) {
        __imp__sub_00143650(ctx, base);
    } else {
        Write(base, g_committed);
        __imp__sub_00143650(ctx, base);
        Write(base, now);
    }
    Trace(base, self, now);
}

X86_EXTERN_FUNC(__imp__sub_0025F100);
X86_FUNC(sub_0025F100) {
    const bool ours = X86_MEM32(ctx.esp) == 0x14396F;
    __imp__sub_0025F100(ctx, base);
    if (ours) g_fit = ctx.eax & 0xFF;
}

