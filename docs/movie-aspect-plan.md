# Intro movies and legal screen: aspect ratio plan

Goal: the intro movies and the legal page fill the screen on wide displays, with no new setting.

## What step 1 found (probe runs, 2026-11-10)

A temporary probe in `D3DDevice_Swap` logged the non-black bounding box of every CPU-written frame (frames with no
draws) and dumped every 10th one. Three runs: 16:9, 4:3, and 16:9 with the game made to load the full-width movies.

### The intros: `ATVI`, `Luxo`, `EXAKT`

| Aspect setting | File the game opens | Where it lands in the 640x480 buffer |
| --- | --- | --- |
| 4:3 (`aspect_ratio = 1333`) | `D:\Data\movies\ATVI.bik` (640x480) | all 640 columns |
| 16:9 (`aspect_ratio = 1778`) | `D:\Data\movies\scrunchedATVI.bik` (480x480) | columns 80..559, black outside |

The present shows the 640x480 buffer at 16:9, so columns 80..559 come out as a 4:3 picture with black side bars.

- The movies are full-frame video, not logos on black: the Activision fly-in, the Luxoflux opening and the Exakt
  road reach rows 0 and 479.
- The logos themselves sit in the middle: Activision rows 166..303, Luxoflux 200..306, Exakt 166..317. A 16:9 crop
  keeps rows 60..419, so every logo stays whole; only background at the top and bottom of the busy shots is cut.
- `Title.bik` and `Legal.*.bik` were never opened in the first 35 seconds.

### The legal page is not a movie

The page after the intros ("TRUE CRIME NEW YORK CITY" and the copyright text) is a still the game writes itself.
No `.bik` is opened for it.

| Aspect setting | Where it lands |
| --- | --- |
| 4:3 | columns 67..573, rows 44..383 (centred) |
| 16:9 | columns 46..552, rows 44..383 |

- The game does not squeeze it for widescreen, so at 16:9 it is shown 33% too wide today.
- Its background is black, so shown at its correct 4:3 shape it still looks full screen: the side bars are the same
  black as the page.
- At 16:9 it sits 21 columns left of where it does at 4:3. 21 columns is 0x54 bytes, the same offset
  `D3DDevice_Swap` corrects on Linux (the surface's frame header). Possibly the same issue on Windows, for this page
  only. Look into it separately; it is not needed for this change.

### Loading the full-width movies at 16:9 does not work on its own

With `scrunched` removed from the file name, the game opens `ATVI.bik` at 16:9 but still writes it starting at
column 80. The rightmost 80 columns spill onto the start of the next row (the Activision "N" shows at the left
edge). The 80-column offset is fixed in the game's movie code, not worked out from the movie's width. Getting the
sharper movies would mean finding and changing that offset in the recompiled code. That is left as an optional
follow-up (see the end).

## Plan

### 1. Know what is on screen (`sdk/runtime/kernel/files.cpp`, `sdk/runtime/xdk/d3d8.cpp`)

The present needs to tell an intro movie from the legal page. Both are CPU-written frames of the same size.

- In `NtCreateFile`, when the name ends in `.bik`, remember the handle; in `NtClose`, forget it. Expose
  `kernel::MovieOpen()`: true while any `.bik` handle is open.
- In `D3DDevice_Swap`, for frames with no draws (`g_frame_draws == 0`), tell the backend which kind of picture this
  is: `Movie` when `MovieOpen()`, otherwise `Still`. Add it to `gpu::Surface` (a `uint8_t picture_kind`), which
  `Present` already receives.
- Frames with draws are unchanged.

Check in the logs that the handle stays open for the whole movie and is closed before the legal page. If the game
keeps the last movie open, fall back on the content: movie frames at 16:9 never light columns 0..79, the legal page
does (from column 46).

### 2. Fill the frame (`D3D12Backend::BlitFrame`)

Today a CPU picture is drawn whole (`uv = {0,0,1,1}`) into a 16:9 viewport (`aspect` capped at 16:9 for
`t.picture`). New layout, only when the console reports a widescreen TV (`app::Widescreen()`):

- **Movie**: the source is the 4:3 picture in columns 80..559 (`uv` x = 80/640 .. 560/640, full height),
  stretched across the whole frame at the aspect setting, ultrawide included. (Chosen over cropping to 16:9 after
  trying both in game.)
- **Still** (legal page): show it at its real 4:3 shape. Viewport = the largest 4:3 rectangle centred in the frame;
  `uv` = the whole buffer. The page is on black, so this looks full screen and the text is no longer stretched.
- At the 4:3 setting nothing changes: the game plays the full-width movies and they already fill the frame.

Check that `Blit` leaves the area outside the viewport black (it does for the 4:3 case today) and that HDR output
keeps it black.

`BlitOptions` needs no change: post, bloom and depth of field are already off for pictures.

### 3. Vulkan (Linux) (`platform/linux/backend_vk.cpp`)

Done. The present shader takes a source rectangle (`rect`, appended to its push constants) and the present pass
uses `gpu::LayoutPicture` with `Surface::picture`: a movie's 4:3 picture is stretched across the window, a still
page gets a centred 4:3 viewport. Checked under Xvfb at 1280x720 with the validation layer: no errors.

### 4. Help text

Update "HUD, menus and movies stay 16:9 in the middle" (`src/common/graphics_choices.h:40`,
`src/common/graphics_options.cpp:453`): movies stretch to fill the screen.

### 5. Testing

At 4:3, 16:10, 16:9, 21:9 and 32:9:

- Activision, Luxoflux and Exakt: fill the whole screen at every shape (stretched from 4:3), logos whole.
- Legal page: correct shape, text whole, no visible bars on 16:9.
- 4:3: same as today.
- Skipping a movie with a button press, and the first menu frame after the legal page: no leftover crop.
- HDR on and off.
- Linux (Vulkan) build.
- Screenshots before and after at 16:9 of the Activision logo and the legal page.

## Files touched

- `sdk/runtime/kernel/files.cpp`: track open `.bik` handles
- `sdk/runtime/gpu/backend.h`: `picture_kind` on `Surface`
- `sdk/runtime/xdk/d3d8.cpp`: set it in `D3DDevice_Swap`
- `sdk/runtime/gpu/d3d12.cpp`: `BlitFrame` crop and layout
- `platform/linux/backend_vk.cpp`: the same for Vulkan
- `src/common/graphics_choices.h`, `src/common/graphics_options.cpp`: help text

## Optional follow-up: sharper movies

The scrunched movies have 480 columns; the full-width ones have 640. To use them at 16:9, find where the game's
movie code adds the 80-column offset for widescreen (the code that calls Bink's copy-to-buffer with the back
buffer) and make it 0 when the full-width file is loaded, plus the `scrunched` rename in `NtCreateFile`. That is
reverse-engineering work of its own; the fill change above does not depend on it.

## Probe notes

The probe was `XBE_MOVIE_PROBE=<dir>` (log bounding boxes and dump raw BGRA frames) and `XBE_MOVIE_UNSCRUNCH=1`
(drop `scrunched` from movie names) in `d3d8.cpp` and `files.cpp`. It has been removed; recreate it from this
description if the numbers need checking again, for example on Linux.
