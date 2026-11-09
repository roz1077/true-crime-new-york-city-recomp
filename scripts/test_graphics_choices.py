"""Check the Graphics menu's value stepping, frame cap mapping and controller gating."""
from pathlib import Path
from native_test import run_native

root = Path(__file__).resolve().parents[1]
includes = [f'-I{root / "src/common"}', f'-I{root / "sdk/runtime"}']

source = r'''
#include "graphics_choices.h"
#include "menu_input.h"
using namespace game;
using namespace game::menu_input;

extern "C" __declspec(dllexport) int run() {
 // stepping through a list stops at the ends and recovers from values between entries
 if (choices::Step(choices::kAnisoLevels, 0, 1) != 2) return 1;
 if (choices::Step(choices::kAnisoLevels, 16, 1) != 16) return 2;
 if (choices::Step(choices::kAnisoLevels, 0, -1) != 0) return 3;
 if (choices::Step(choices::kRenderPercents, 90, 1) != 100) return 4;
 if (choices::Step(choices::kRenderPercents, 90, -1) != 75) return 5;
 if (choices::Step(choices::kRenderPercents, 20, 1) != 25) return 6;
 if (choices::Step(choices::kRenderPercents, 20, -1) != 20) return 7;
 if (choices::Step(choices::kRenderPercents, 250, -1) != 200) return 8;

 // caps above the game's own limit, unlimited and refresh all run at 60
 if (choices::EffectiveFrameCap(0) != 60 || choices::EffectiveFrameCap(1) != 60) return 10;
 if (choices::EffectiveFrameCap(144) != 60 || choices::EffectiveFrameCap(30) != 30) return 11;
 if (choices::ClampRenderPercent(5) != 20 || choices::ClampRenderPercent(500) != 200) return 12;
 if (choices::RenderHeight(1440, 50) != 720) return 13;
 // field of view: 100% is unchanged; 200% of a 90 degree view doubles tan(45) to 2
 auto near = [](float a, float b) { return a > b - 0.01f && a < b + 0.01f; };
 if (!near(choices::ScaledFovDegrees(70, 100), 70) || !near(choices::ScaledFovDegrees(90, 200), 126.87f)) return 14;
 if (choices::ClampFovPercent(10) != 50 || choices::Step(choices::kFovPercents, 100, 1) != 110) return 15;
 // HDR: paper white, peak (0 means the display's report) and highlight strength step through their lists
 if (choices::Step(choices::kHdrPaperWhiteNits, 200, 1) != 210) return 16;
 if (choices::Step(choices::kHdrPeakNits, 0, -1) != 0 || choices::Step(choices::kHdrPeakNits, 0, 1) != 400) return 17;
 if (choices::Step(choices::kHdrHighlightPercents, 70, -1) != 60 || choices::Step(choices::kHdrHighlightPercents, 100, 1) != 100) return 18;

 // navigation from dpad, stick and face buttons
 input::Pad p; p.connected = true;
 p.buttons = kDpadUp; if (NavBits(p) != kUp) return 20;
 p.buttons = 0; p.lx = 20000; if (NavBits(p) != kRight) return 21;
 p.lx = 9000; if (NavBits(p) != 0) return 22;
 p.lx = 0; p.analog[kAnalogA] = 0xFF; if (NavBits(p) != kAccept) return 23;
 p.analog[kAnalogA] = 0;

 PadGate gate;
 // closed menu: the game sees the pad unchanged
 p.analog[kAnalogA] = 0xFF;
 if (gate.Filter(p, false) || p.analog[kAnalogA] != 0xFF) return 30;
 // the chord asks for the menu once and is held back from the game
 input::Pad chord; chord.connected = true; chord.buttons = kStart | kBackButton;
 input::Pad q = chord;
 if (!gate.Filter(q, false) || q.buttons || !q.connected) return 31;
 q = chord;
 if (gate.Filter(q, true) || q.buttons) return 32;
 // open menu: nothing reaches the game
 q = p;
 gate.Filter(q, true);
 if (q.analog[kAnalogA] || !q.connected) return 33;
 // closed while B is held: still held back until everything is released
 input::Pad b; b.connected = true; b.analog[kAnalogB] = 0xFF;
 q = b; gate.Filter(q, false);
 if (q.analog[kAnalogB]) return 34;
 input::Pad idle; idle.connected = true;
 q = idle; gate.Filter(q, false);
 q = p; gate.Filter(q, false);
 if (q.analog[kAnalogA] != 0xFF) return 35;

 // repeats: once on press, again after the delay, then at the interval; accept never repeats
 Repeater r;
 if (r.Update(kDown | kAccept, 0.0) != (kDown | kAccept)) return 40;
 if (r.Update(kDown | kAccept, 0.2) != 0) return 41;
 if (r.Update(kDown | kAccept, 0.36) != kDown) return 42;
 if (r.Update(kDown | kAccept, 0.40) != 0) return 43;
 if (r.Update(kDown | kAccept, 0.45) != kDown) return 44;
 if (r.Update(0, 0.5) != 0) return 45;
 if (r.Update(kDown, 0.51) != kDown) return 46;
 return 0;
}
'''
rc = run_native(source, prefix="tcnyc-graphics_choices-", needs_crt=True, compiler_flags=includes)
assert rc == 0, f"Graphics menu regression {rc}"
print('PASS: option stepping, frame cap mapping, field of view, controller navigation, input gating and repeat')
