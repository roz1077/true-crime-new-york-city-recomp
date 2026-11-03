"""Check the Graphics rows added to the native Options > Display tab: cursor movement, value changes, text."""
from pathlib import Path
from native_test import run_native

root = Path(__file__).resolve().parents[1]
includes = [f'-I{root / "src"}', f'-I{root / "src/game"}']

source = r'''
#include <cstring>
#include "display_rows.h"
using namespace game::display_rows;

extern "C" __declspec(dllexport) int run() {
 if (kRows != 7) return 1;
 // down walks every row and wraps; the game's own count for the tab is one
 Step s = Input(0, kDown); if (!s.handled || s.row != 1 || s.focus != kRowListFocus) return 2;
 s = Input(kRows - 1, kDown); if (s.row != 0) return 3;
 // up moves back, and from the first row returns to the tab bar
 s = Input(3, kUp); if (!s.handled || s.row != 2) return 4;
 s = Input(0, kUp); if (!s.handled || s.focus != kTabBarFocus || s.row != 0) return 5;
 // left and right change an added row, but stay the game's toggle on SUBTITLES
 s = Input(0, kRight); if (s.handled) return 6;
 s = Input(1, kLeft); if (!s.handled || s.change != -1 || s.row != 1) return 7;
 s = Input(6, kRight); if (!s.handled || s.change != 1) return 8;
 // accept (0x100), Back (0x20) and B (0x200) are the game's
 if (Input(2, 0x100).handled || Input(2, 0x20).handled || Input(2, 0x200).handled) return 9;
 // five slots show a window that follows the cursor: rows 0-4, then 1-5, and back to 0 on wrap
 if (kShownRows != 5) return 10;
 if (Scroll(4, 0) != 0 || Scroll(5, 0) != 1 || Scroll(3, 1) != 1 || Scroll(0, 1) != 0) return 11;

 Values v{100, 100, 16, 144, true, false};
 Change(v, Option::RenderScale, -1); if (v.render_percent != 75) return 20;
 Change(v, Option::FieldOfView, 1); if (v.fov_percent != 110) return 26;
 Change(v, Option::Anisotropic, 1); if (v.aniso != 16) return 21;
 Change(v, Option::Anisotropic, -1); if (v.aniso != 8) return 22;
 Change(v, Option::VSync, -1); if (v.vsync) return 23;
 Change(v, Option::Smaa, 1); if (!v.smaa) return 27;
 Change(v, Option::FrameCap, -1); if (v.fps_cap != 30) return 24;  // 144 runs at 60, one step down is 30
 Change(v, Option::FrameCap, 1); if (v.fps_cap != 60) return 25;

 char text[kTextBytes];
 Text(v, Option::RenderScale, text); if (std::strcmp(text, "RENDER SCALE 0.75X")) return 30;
 Text(v, Option::FieldOfView, text); if (std::strcmp(text, "FIELD OF VIEW 1.10X")) return 36;
 Text(v, Option::Anisotropic, text); if (std::strcmp(text, "ANISOTROPIC 8X")) return 31;
 v.aniso = 0; Text(v, Option::Anisotropic, text); if (std::strcmp(text, "ANISOTROPIC OFF")) return 32;
 Text(v, Option::VSync, text); if (std::strcmp(text, "V-SYNC OFF")) return 33;
 Text(v, Option::Smaa, text); if (std::strcmp(text, "SMAA ON")) return 37;
 Text(v, Option::FrameCap, text); if (std::strcmp(text, "FRAME LIMIT 60")) return 34;
 for (const char* c = text; *c; ++c) if (*c == '%') return 35;  // the game's font has no '%'
 return 0;
}
'''
rc = run_native(source, prefix="tcnyc-display_rows-", needs_crt=True, compiler_flags=includes)
assert rc == 0, f"Display rows regression {rc}"
print('PASS: Display tab row movement, scrolling, game-handled inputs, value changes and row text')
