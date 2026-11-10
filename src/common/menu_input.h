// Controller handling for the in-game Graphics menu, kept apart from ImGui so
// a test can compile it alone.
#pragma once

#include <cstdint>

#include "core/input.h"

namespace game::menu_input {

enum Nav : uint32_t {
    kUp = 1u << 0,
    kDown = 1u << 1,
    kLeft = 1u << 2,
    kRight = 1u << 3,
    kAccept = 1u << 4,
    kBack = 1u << 5,
};

// input::Pad::buttons bits
inline constexpr uint16_t kDpadUp = 1u << 0, kDpadDown = 1u << 1, kDpadLeft = 1u << 2, kDpadRight = 1u << 3,
                          kStart = 1u << 4, kBackButton = 1u << 5;
inline constexpr int kAnalogA = 0, kAnalogB = 1;
inline constexpr uint8_t kAnalogPressed = 0x40;
inline constexpr int16_t kStickNav = 16000;     // left stick counts as a direction past this
inline constexpr int16_t kStickNeutral = 8000;  // and as released inside this
inline constexpr uint8_t kAnalogNeutral = 0x20; // analog buttons and triggers, released below this

// Navigation comes from the controller only while an ImGui overlay owns the
// keyboard: input.cpp stops mirroring keyboard arrows/Enter/Escape into the
// pad then, because ImGui already delivers those keys to the menu.
inline uint32_t NavBits(const input::Pad &p) {
    uint32_t bits = 0;
    if ((p.buttons & kDpadUp) || p.ly > kStickNav) bits |= kUp;
    if ((p.buttons & kDpadDown) || p.ly < -kStickNav) bits |= kDown;
    if ((p.buttons & kDpadLeft) || p.lx < -kStickNav) bits |= kLeft;
    if ((p.buttons & kDpadRight) || p.lx > kStickNav) bits |= kRight;
    if (p.analog[kAnalogA] >= kAnalogPressed) bits |= kAccept;
    if (p.analog[kAnalogB] >= kAnalogPressed) bits |= kBack;
    return bits;
}

inline bool OpenChord(const input::Pad &p) { return (p.buttons & (kStart | kBackButton)) == (kStart | kBackButton); }

inline bool Neutral(const input::Pad &p) {
    if (p.buttons) return false;
    for (uint8_t a : p.analog)
        if (a >= kAnalogNeutral) return false;
    auto centred = [](int16_t v) { return v > -kStickNeutral && v < kStickNeutral; };
    return centred(p.lx) && centred(p.ly) && centred(p.rx) && centred(p.ry);
}

// Decides what the game sees from controller 1. While the menu is open the
// game gets a connected pad with nothing pressed. After it closes, input stays
// held back until the pad is released, so the button that closed the menu
// does not also act in the game.
class PadGate {
  public:
    // Returns true when the open chord was just pressed.
    bool Filter(input::Pad &p, bool menu_open) {
        const bool chord = OpenChord(p);
        const bool toggle = chord && !chord_held_;
        chord_held_ = chord;
        if (menu_open || toggle) {
            holding_ = true;
        } else if (holding_ && Neutral(p)) {
            holding_ = false;
        }
        if (holding_) {
            const bool connected = p.connected;
            p = input::Pad{};
            p.connected = connected;
        }
        return toggle;
    }

  private:
    bool chord_held_ = false;
    bool holding_ = false;
};

// Turns held navigation bits into presses: once on press, then repeated for
// directions after a delay.
class Repeater {
  public:
    static constexpr double kDelay = 0.35, kInterval = 0.08;
    static constexpr uint32_t kRepeating = kUp | kDown | kLeft | kRight;

    uint32_t Update(uint32_t held, double now) {
        uint32_t pressed = held & ~held_;
        for (int i = 0; i < 32; ++i) {
            const uint32_t bit = 1u << i;
            if (!(held & bit)) continue;
            if (pressed & bit) {
                next_[i] = now + kDelay;
            } else if ((bit & kRepeating) && now >= next_[i]) {
                pressed |= bit;
                next_[i] = now + kInterval;
            }
        }
        held_ = held;
        return pressed;
    }

  private:
    uint32_t held_ = 0;
    double next_[32] = {};
};

} // namespace game::menu_input
