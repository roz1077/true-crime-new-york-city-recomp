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
 if (kRows != 11) return 1;
 // down walks every row and wraps; the game's own count for the tab is one
 Step s = Input(0, kDown); if (!s.handled || s.row != 1 || s.focus != kRowListFocus) return 2;
 s = Input(kRows - 1, kDown); if (s.row != 0) return 3;
 // up moves back, and from the first row returns to the tab bar
 s = Input(3, kUp); if (!s.handled || s.row != 2) return 4;
 s = Input(0, kUp); if (!s.handled || s.focus != kTabBarFocus || s.row != 0) return 5;
 // left and right change an added row, but stay the game's toggle on SUBTITLES
 s = Input(0, kRight); if (s.handled) return 6;
 s = Input(1, kLeft); if (!s.handled || s.change != -1 || s.row != 1) return 7;
 s = Input(kRows - 1, kRight); if (!s.handled || s.change != 1) return 8;
 // A applies the resolution row only; elsewhere A, Back (0x20) and B (0x200) are the game's
 s = Input(RowOf(Option::Resolution), kAccept); if (!s.handled || !s.apply) return 9;
 if (Input(RowOf(Option::Aspect), kAccept).handled || Input(2, 0x20).handled || Input(2, 0x200).handled) return 10;
 // five slots show a window that follows the cursor
 if (kShownRows != 5) return 11;
 if (Scroll(4, 0) != 0 || Scroll(5, 0) != 1 || Scroll(3, 1) != 1 || Scroll(0, 1) != 0 || Scroll(8, 0) != 4) return 12;

 const uint64_t sizes[] = {0, Size(1920, 1080), Size(2560, 1080), Size(3440, 1440)};
 Values v{100, 100, 16, 144, 0, 0, true, false, false, 0, 0};
 Change(v, Option::Resolution, 1, sizes); if (v.size != Size(1920, 1080)) return 20;
 Change(v, Option::Resolution, 1, sizes); Change(v, Option::Resolution, 1, sizes); Change(v, Option::Resolution, 1, sizes);
 if (v.size != Size(3440, 1440)) return 21;  // stops at the last size
 Change(v, Option::Resolution, -1, sizes); if (v.size != Size(2560, 1080)) return 22;
 Values gone = v; gone.size = Size(1024, 768);
 Change(gone, Option::Resolution, 1, sizes); if (gone.size != Size(1920, 1080)) return 23;  // unlisted: from desktop
 Change(v, Option::Aspect, 1, sizes); if (v.aspect != 1333) return 24;
 Change(v, Option::Aspect, -1, sizes); if (v.aspect != 0) return 25;
 Change(v, Option::RenderScale, -1, sizes); if (v.render_percent != 75) return 26;
 Change(v, Option::FieldOfView, 1, sizes); if (v.fov_percent != 110) return 27;
 Change(v, Option::Anisotropic, -1, sizes); if (v.aniso != 8) return 28;
 Change(v, Option::VSync, -1, sizes); if (v.vsync) return 29;
 Change(v, Option::Smaa, 1, sizes); if (!v.smaa) return 30;
 Change(v, Option::FrameCap, -1, sizes); if (v.fps_cap != 30) return 31;  // 144 runs at 60, one step down is 30

 char text[kTextBytes];
 Text(v, Option::ShowFps, text); if (std::strcmp(text, "SHOW FPS OFF")) return 57;
 Change(v, Option::ShowFps, 1, sizes);
 Text(v, Option::ShowFps, text); if (!v.show_fps || std::strcmp(text, "SHOW FPS ON")) return 58;
 Change(v, Option::ShowFps, -1, sizes); if (v.show_fps) return 59;
 Text(v, Option::Msaa, text); if (std::strcmp(text, "MSAA OFF")) return 53;
 for (uint32_t samples : {2u, 4u, 8u, 8u}) {
  Change(v, Option::Msaa, 1, sizes); if (v.msaa != samples) return 54;
 }
 Text(v, Option::Msaa, text); if (std::strcmp(text, "MSAA 8X")) return 55;
 Change(v, Option::Msaa, -1, sizes); if (v.msaa != 4) return 56;
 Text(v, Option::Resolution, text); if (std::strcmp(text, "RESOLUTION 2560X1080 - PRESS A")) return 40;
 v.applied = v.size; Text(v, Option::Resolution, text); if (std::strcmp(text, "RESOLUTION 2560X1080")) return 41;
 v.size = v.applied = 0; Text(v, Option::Resolution, text); if (std::strcmp(text, "RESOLUTION DESKTOP")) return 42;
 Text(v, Option::Aspect, text); if (std::strcmp(text, "ASPECT RATIO AUTO")) return 43;
 v.aspect = 2370; Text(v, Option::Aspect, text); if (std::strcmp(text, "ASPECT RATIO 21:9")) return 44;
 Text(v, Option::RenderScale, text); if (std::strcmp(text, "RENDER SCALE 0.75X")) return 45;
 Text(v, Option::FieldOfView, text); if (std::strcmp(text, "FIELD OF VIEW 1.10X")) return 46;
 Text(v, Option::Anisotropic, text); if (std::strcmp(text, "ANISOTROPIC 8X")) return 47;
 v.aniso = 0; Text(v, Option::Anisotropic, text); if (std::strcmp(text, "ANISOTROPIC OFF")) return 48;
 Text(v, Option::Smaa, text); if (std::strcmp(text, "SMAA ON")) return 49;
 Text(v, Option::VSync, text); if (std::strcmp(text, "V-SYNC OFF")) return 50;
 Text(v, Option::FrameCap, text); if (std::strcmp(text, "FRAME LIMIT 30")) return 51;
 for (Option o : kOptions) {  // the game's font has no '%'
  Text(v, o, text);
  for (const char* c = text; *c; ++c) if (*c == '%') return 52;
 }
 return 0;
}
'''
rc = run_native(source, prefix="tcnyc-display_rows-", needs_crt=True, compiler_flags=includes)
assert rc == 0, f"Display rows regression {rc}"
print('PASS: Display tab rows: movement, scrolling, resolution apply on A, aspect, value changes and text')
