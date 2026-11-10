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
 const Page M = Page::Main, H = Page::Hdr;
 if (RowCount(M) != 19 || RowCount(H) != 5) return 1;
 // down walks every row and wraps; the game's own count for the tab is one
 Step s = Input(M, 0, kDown); if (!s.handled || s.row != 1 || s.focus != kRowListFocus) return 2;
 s = Input(M, RowCount(M) - 1, kDown); if (!s.handled || s.row != RowCount(M) - 1) return 3;  // hard stop
 s = Input(H, RowCount(H) - 1, kDown); if (s.row != 0) return 68;  // the HDR page wraps
 // up moves back, and from the first row returns to the tab bar
 s = Input(M, 3, kUp); if (!s.handled || s.row != 2) return 4;
 s = Input(M, 0, kUp); if (!s.handled || s.focus != kTabBarFocus || s.row != 0) return 5;
 // left and right change an added row, but stay the game's toggle on SUBTITLES
 s = Input(M, 0, kRight); if (s.handled) return 6;
 s = Input(M, 1, kLeft); if (!s.handled || s.change != -1 || s.row != 1) return 7;
 s = Input(M, RowCount(M) - 2, kRight); if (!s.handled || s.change != 1) return 8;
 // A applies the display mode and resolution rows; elsewhere A, Back and B are the game's
 s = Input(M, RowOf(M, Option::Resolution), kAccept); if (!s.handled || !s.apply) return 9;
 s = Input(M, RowOf(M, Option::DisplayMode), kAccept); if (!s.handled || !s.apply) return 13;
 if (Input(M, RowOf(M, Option::Aspect), kAccept).handled || Input(M, 2, kBack).handled || Input(M, 2, kCancel).handled) return 10;
 // the HDR page: opened with A, left with its BACK row, Back or B; up wraps instead of leaving the list
 const uint32_t hdr_row = RowOf(M, Option::HdrPage);
 s = Input(M, hdr_row, kAccept); if (!s.handled || s.page != H || s.row != 0) return 14;
 s = Input(M, hdr_row, kRight); if (!s.handled || s.change) return 15;
 s = Input(H, RowOf(H, Option::HdrBack), kAccept); if (s.page != M || s.row != hdr_row) return 16;
 s = Input(H, 2, kBack); if (!s.handled || s.page != M || s.row != hdr_row) return 17;
 s = Input(H, 2, kCancel); if (!s.handled || s.page != M) return 18;
 s = Input(H, 0, kUp); if (!s.handled || s.focus != kRowListFocus || s.row != RowCount(H) - 1) return 19;
 s = Input(H, 0, kRight); if (!s.handled || s.change != 1 || s.page != H) return 60;
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
 Change(v, Option::ObjectDetail, 1, sizes); if (v.object_detail != 1) return 32;
 for (int i = 0; i < 3; ++i) Change(v, Option::AmbientOcclusion, 1, sizes);
 if (v.ao != 2) return 33;  // stops at full
 Change(v, Option::Bloom, 1, sizes); Change(v, Option::DepthOfField, 1, sizes);
 Change(v, Option::PcContrast, 1, sizes); Change(v, Option::BuildingSeamFix, 1, sizes);
 if (v.bloom || v.dof || v.pc_contrast || v.building_seam_fix) return 34;
 Change(v, Option::Hdr, 1, sizes); if (!v.hdr) return 35;
 Change(v, Option::HdrPaperWhite, 1, sizes); if (v.hdr_paper_white != 210) return 36;
 Change(v, Option::HdrPeak, 1, sizes); if (v.hdr_peak != 400) return 37;
 Change(v, Option::HdrHighlights, -1, sizes); if (v.hdr_highlights != 30) return 38;
 Change(v, Option::DisplayMode, 1, sizes); Change(v, Option::DisplayMode, 1, sizes); if (v.mode != 2) return 39;

 char text[kTextBytes];
 Text(v, Option::DisplayMode, text); if (std::strcmp(text, "MODE WINDOWED - A")) return 61;
 Text(v, Option::ObjectDetail, text); if (std::strcmp(text, "DETAIL HIGH")) return 62;
 Text(v, Option::AmbientOcclusion, text); if (std::strcmp(text, "OCCLUSION FULL")) return 63;
 Text(v, Option::HdrPage, text); if (std::strcmp(text, "HDR SETTINGS")) return 64;
 Text(v, Option::HdrPeak, text); if (std::strcmp(text, "PEAK 400 NITS")) return 65;
 v.hdr_peak = 0; v.display_peak = 750;
 Text(v, Option::HdrPeak, text); if (std::strcmp(text, "PEAK AUTO 750")) return 66;
 v.mode = v.applied_mode;
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
 Text(v, Option::Resolution, text); if (std::strcmp(text, "RES 2560X1080 - A")) return 40;
 v.applied = v.size; Text(v, Option::Resolution, text); if (std::strcmp(text, "RES 2560X1080")) return 41;
 v.size = v.applied = 0; Text(v, Option::Resolution, text); if (std::strcmp(text, "RES DESKTOP")) return 42;
 Text(v, Option::Aspect, text); if (std::strcmp(text, "ASPECT AUTO")) return 43;
 v.aspect = 2370; Text(v, Option::Aspect, text); if (std::strcmp(text, "ASPECT 21:9")) return 44;
 Text(v, Option::RenderScale, text); if (std::strcmp(text, "RENDER SCALE 0.75X")) return 45;
 Text(v, Option::FieldOfView, text); if (std::strcmp(text, "FOV 1.10X")) return 46;
 Text(v, Option::Anisotropic, text); if (std::strcmp(text, "ANISOTROPIC 8X")) return 47;
 v.aniso = 0; Text(v, Option::Anisotropic, text); if (std::strcmp(text, "ANISOTROPIC OFF")) return 48;
 Text(v, Option::Smaa, text); if (std::strcmp(text, "SMAA ON")) return 49;
 Text(v, Option::VSync, text); if (std::strcmp(text, "V-SYNC OFF")) return 50;
 Text(v, Option::FrameCap, text); if (std::strcmp(text, "FRAME LIMIT 30")) return 51;
 for (uint32_t o = 0; o < kOptionCount; ++o) {  // every option has text, and the game's font has no '%'
  Text(v, Option(o), text);
  if (!*text || std::strlen(text) > kMaxTextChars) return 67;
  for (const char* c = text; *c; ++c) if (*c == '%') return 52;
 }
 return 0;
}
'''
rc = run_native(source, prefix="tcnyc-display_rows-", needs_crt=True, compiler_flags=includes)
assert rc == 0, f"Display rows regression {rc}"
print('PASS: Display tab rows: pages, movement, scrolling, display apply on A, value changes and text')
