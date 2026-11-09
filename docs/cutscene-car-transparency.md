# Opening cutscene: yellow car body / bonnet transparency

Status: **closed as Xbox behaviour (won't fix).** The car "transparency" is the original Xbox
look: the paint reflects a copy of last frame's screen (see findings below), and the
reference video shows the Xbox has it too. It is not matched to PS2, and no car-specific
change is in the tree. The black building windows/doors in the same cutscene were a port
bug and are **fixed** (NV2A `r0.a = t0.a` in the fixed-function generator, `shaders.cpp`),
confirmed by the user against the Xbox reference.
HEAD when these notes were written: `aea3d4e` (`vulkan-ao` branch), working tree clean.
The temporary per-frame dump diagnostic was reverted and the release build was rebuilt.

## Symptom

In the opening cutscene (reference video 00:45-1:40), the yellow car's body/bonnet
sometimes looks transparent or missing. The background street, fence, signs and
buildings show where the driver-side front body should be. The headlight pods,
wheel and parts of the hood can remain visible.

The user wants PS2-accurate behavior: the car must be solid. The Xbox release is
believed to have the same visual defect, but the reference comparison video shows
both the Xbox and PS2 cars solid at the matching shots.

Reference video: <https://www.youtube.com/watch?v=Rgohhau18NM> (cutscene 00:45-1:40).
The video is a GameCube / Xbox / PS2 comparison.

## Confirmed evidence

### Captures that show the bug

- `out/build/release/captures/143339` - **user-confirmed transparent frame.**
  Same run as 143338, one second later, slightly different camera angle.
  The driver-side front body disappears into the background.
  Full-size crop kept at `out/crop_143339.png`.
- `out/build/release/captures/143338` - solid frame from the same run, front view.
  Full-size crop kept at `out/crop_143338.png`.

### Captures that do not show it

- `144651` / `144652` - both guest finals look solid despite being labelled as a
  bug/solid pair earlier. Do not use these as the primary comparison.
- `152317` - per-20-draw F3 capture; final solid.
- `153032` - old strong-override build; solid but visibly deformed.
- `165823` - F3 capture at backend frame 1321, with `XBE_CAPTURE_EVERY=10`.
  The captured frame is the car just entering the shot (hood at the bottom), solid.
- Temporary frame dump (sampled every 6th presented frame, since deleted): frames
  `f_001104`-`f_001500` covered the hood/mirror and front/three-quarter shots.
  Every sampled frame looked solid. Because the dump sampled only `frame_ % 6 == 0`,
  a transparent frame on another present/parity could have been skipped.

### User host screenshots

Two screenshots from the user's machine (not in the repository) match the hood/mirror and
front-left moments of the cutscene.

## What the draw logs show

Car draws (identified by textures `8345DB80` / `8344C380` and reflection target
`81CD8000`) are **the same in the solid and transparent captures**:

- 5 body draws: `ps B04016E0`, `blend 1 1 302`, `zen 1 206`, `zwrite 1`.
- 8 clear-coat draws: `ps B04016C0`, `blend 1 1 302`, `zen 1 206`, `zwrite 0`.
- Optional window/trim draws: `ps B04016D0`, `blend 1 1 302`, `zen 1 206`,
  `zwrite 0`.
- Body pass: `t0 8345DB80` (P8 256x256), `t1 81CD8000` (target 256x256).
- Clear pass: `t0 8344C380` (P8 256x256), `t1 81CD8000`.
- Body vertex shader: `vs B04014B1`, host `vk 7AA4C4263D95A580`.
- Solid 144652 used `vs B04014A1` / `vk 68C0E80A1DBCB4E9`, but that is a different
  camera/lighting moment, not the same shot.

The paint textures `tex_8345DB80.bmp` and `tex_8344C380.bmp` are nearly identical
between 143338 and 143339. The `81CD8000` reflection target differs with camera
angle, as expected.

Conclusion from the logs: this is **not** a missing draw, a changed material or a
changed texture in the transparent frame. It is state/raster/constant dependent.

## Shader notes

`ps_12E47ACA2D01FC95.hlsl` (handle mapping not recorded) samples only `tex1` and
mixes vertex colours with `pc[6].a`; its output alpha is
`(1 - r0.a) * (1 - pc[8].a)`. This is consistent with a reflection/clear-coat
shader, but the exact `ps B04016E0` / `B04016C0` -> HLSL mapping is not stored in
the capture. The next investigation should log that mapping.

Likely candidates, all unproven:

1. **Pixel-shader constants (`pc[6].a`, `pc[8].a`)** are stale or wrong on some
   frames/draws. If the reflection amount is 1, the paint term disappears and the
   car can look like a transparent reflection of the street.
2. **Present parity / alternate buffers.** The transparency may be on alternating
   presents; sampling every 6th frame can miss it, and F3 captures the *next*
   frame (`StartCapture` happens on the present after the trigger).
3. **Target lifetime.** The 256x256 reflection target `81CD8000` (or another
   per-frame target) may be released/recreated between drawing and sampling.
   The earlier small-target-retention experiment was inconclusive and was reverted.
4. **Deferred replay / constant cache order.** The same draw state can still get a
   different constant buffer or depth state after deferred scene replay.

## Failed experiments (all reverted)

Do not repeat these without new evidence:

- Force the clear-coat pass opaque: car became solid but deformed.
- Skip the clear-coat pass: still transparent.
- Force the additive body pass opaque with depth test ALWAYS: broke car texturing.
- Zero `pc[6]` / `pc[8]` for the car: **never matched.** The guard required
  `k.src == 0x302 && k.dst == 0x303`, but the logged fields are
  `blend 1 1 302`, i.e. `k.src == 1`, `k.dst == 0x302`. This explains why the
  "paint" experiment did nothing.
- Retain small targets at 256x256 for 3600 frames: no confirmed fix, reverted.

No broad material/blend/depth override should be reintroduced. The previous ones
caused worse artifacts than the original bug.

## Next steps (evidence-first)

1. **Record and replay the exact run by picture number.**
   - `XBE_PAD_RECORD=<file>` records controller state indexed by
     `d3d::PresentCount()` (`pad_tools.cpp`).
   - `XBE_PAD_REPLAY=<file>` replays it by picture number, so load timing does
     not shift the inputs.
   - `XBE_DETERMINISTIC_REPLAY=1` plus `XBE_PAD_RECORD` / `XBE_PAD_REPLAY`
     also records/restores the cutscene timing steps (`timing.cpp`).
2. **Find the transparent picture.** Use `XBE_CAPTURE_AT=p<picture>` with
   `XBE_CAPTURE_DIR=captures` and `XBE_CAPTURE_EVERY=10` to capture the exact
   picture with per-draw step images. `XBE_CAPTURE_AT` accepts a comma list;
   `TCNYC_CAPTURE_AT=<backend frame>` captures a single backend frame (it starts
   the capture on the following present).
3. **Log the car draw constants and targets.** For each `ps B04016E0` /
   `B04016C0` / `B04016D0` draw, log: ps handle, `pc[6]`, `pc[8]`, blend factors,
   depth func/write, alpha test, render target, depth target, and the actual
   `81CD8000` target generation. Compare a solid and a transparent picture of the
   *same camera angle*.
4. **Check present parity.** Dump every present in a short window (not every 6th),
   or capture consecutive `p<n>`, `p<n+1>` pictures. If the hole alternates,
   address the per-frame target/constant state rather than the material.
5. Only after the offending draw/pass is identified, implement a narrow fix or
   document the Xbox behavior and add a PS2-matching option.

## Review findings (2026-11-09)

From `143339/frame.txt` and its dumped shaders (no new runs):

- **Shader mapping is in the capture** (line after each draw): body `ps B04016E0` ->
  `ps_FE5E6D7A9C62204D`; the other two car shaders are `ps_12E47ACA2D01FC95`
  (reflection only, samples only `tex1`) and `ps_9F2DD67E59B59B8F`.
- **Blend is `ONE, SRC_ALPHA`** (`blend 1 1 302` = enable, src 1, dst 0x302), so every car
  pass writes `colour + background * alpha`.
- **Body shader:** `rgb = lerp(paint * v0.rgb * v0.a, reflection * v1.rgb, pc[6].a)`,
  `alpha = (1 - v0.a) * (1 - pc[8].a)`.
- **`v0.a` is a per-draw constant**: the vertex shader (`vs_7AA4C4263D95A580`) sets
  `oD0.w = c[186].w`. So `c[186].w` is the material opacity. When it is below 1 the paint
  darkens *and* the street shows through. That is exactly the symptom, and a whole body
  panel (one draw) can go at once while the hood/headlights (other draws) stay.
- **Reflection target `81CD8000` is a 2x2 atlas** of four views from the car
  (`1089_81CD8000.bmp`). It is drawn at draws 1084-1089, *after* the car samples it
  (734-989), so the car reads the previous frame's atlas. It also contains the car itself.
  The reflection-only shader gives `rgb ~= reflection * v1`, so if this atlas is black or
  read from the wrong place, glass/trim render black. **This is the likely source of the
  black doors/windows**, and it may also feed the transparency (a strong reflection of the
  street looks like a hole).
- `RegionClip` does not apply to the car (not a pretransformed quad), so `rtclip` is open.

**Update:** a reference frame from the video (front view, headlights) shows the **Xbox
original also has the transparent right-front body**, while PS2 is solid. So the port is
likely reproducing Xbox behaviour. Matching PS2 is a deliberate deviation: once step 1
confirms the cause (expected `c[186].w < 1` on the affected body draws), add a narrow
option, on by default, that forces the opacity to 1 for those car body draws only
(matched by ps handle `B04016E0` + vs `B04014B1`, or better, at the game code that writes
`c[186]`). Do not touch blend/depth state. The black windows still need checking against
the Xbox reference separately, because Xbox windows look dark but not solid black.

**Step 1 result (captures `cap_run/171240` transparent, `171250`, `171254`, `171257`;
`XBE_CAPTURE_INPUTS=1`, played live):** the opacity theory is **ruled out**. Note that
constants print with a -96 bias, so shader `c[186]` is the `c90` line. Every body draw
has `c[186].w = 1` and `pc[6].a = pc[8].a = 0.078` in both solid and transparent captures.
Only glass draws have low `c[186].w` (0.098 / 0.012, with pc 0.349 / 0.251), as intended.
So the paint is fully opaque, and the hole means the body pixels are **not drawn at all**.
The missing area is always the part of the car nearest the camera, so the next suspect is
**near-plane clipping** (host `DepthClipEnable` / `ReversedClip` vs the console's
clip range, `d3d12.cpp:2275`, `shaders.cpp:1003`), or depth rejection.
Next test: an env toggle that turns off depth clipping for the scene pass. If the
hole fills in, look at what near distance the cutscene camera uses.

**Near clipping ruled out:** turning off `DepthClipEnable` changed nothing.

**Step capture `cap_steps/171810` (every 2 draws):** the hole is already in the frame
before post-processing. After the base coat, only clear-coat draws (`ps B04016C0`) change
the wing. Fitting `after = src + k * before` over the changed pixels: draw 1009 keeps
k ~ 0.8 of the street and draw 1030 keeps 0.51, while 1027 keeps 0.11. All of them have
identical constants (`c[186].w = 1`, `pc[6].a = pc[8].a = 0.078`) and the same texture
(`8344C380`, alpha 241-255). The big panels have tiny base-coat draws (10-59 verts) and
are painted mostly by the clear coat.

**Reflection test (`XBE_NO_CAR_REFLECT=1`, temp toggle in `FillCombinerConstants`, sets
`pc[6].a = 0` for ps `B04016E0/C0/D0`), captures `cap_noref/172242`, `172245`:** the car
looks solid, and no clear-coat draw keeps the background (k <= 0.17). So the
"transparency" is the reflection atlas `81CD8000`. It is rendered from the car's own
position, so it shows the same street that sits behind the car, and close up it reads as
see-through. This matches the Xbox original having the bug.
Open: why a 0.078 weight has this much effect (check `Color()` decoding of `pc[6]` and
the `v1` term). The test shot was not the identical camera angle.

**Reflection source:** draws 1123-1127 (in 171810) build `81CD8000` from four quads that
copy the finished main target `83DA4000` (640x480). The car "reflection" is a copy of
last frame's screen, which is why it reads as transparency.

**Black windows (first look, 171810):** glass draw 1012 (`ps B04016D0`, `c[186].w` 0.098,
`pc[6].a` 0.349) lands on pixels that are already ~black (6,7,9) and only lifts them to
~(20,18,17). Behind the glass is black, and the reflection adds little. In the reference,
the Xbox and PS2 windows read as a lit grey. The user is taking fresh captures of the
black windows/doors (`cap_win`).

**Black windows/doors are on the buildings (captures `cap_win/1726xx`).** In 172719 a
black pane is written by draw 100: vs `B04012B1` (`vs_C691EA1A5AF06D15`), fixed-function
`ps_947ED22AB44D8624`, `t0 826E2780` (window glass/frame), `t1 82891000` (sheet of lit
room interiors). The stages generate
`tmp = t0*v1; cur = t1*v0; cur = lerp(cur, tmp, cur.a); cur += v1`, i.e. stage 2 op 13
(`BLENDCURRENTALPHA`) with arg1 = TEMP and arg2 = CURRENT. Here `v0.a = c[186].w = 1`, so the
result is glass * specular (~black at night) and the lit interiors never show. The
reference shows lit interiors. The stage indices match the XDK layout. Temp toggle
`XBE_FLIP_BLENDCURRENT=1` (`FfOp` case 13 in `shaders.cpp`) reverses the blend for a
visual test.

### Plan

1. **Log the per-draw inputs.** Rerun the recorded cutscene with `XBE_CAPTURE_INPUTS=1`
   and capture a solid and a transparent picture of the same shot. For the car draws,
   compare `c[186].w`, `pc[6].a`, `pc[8].a`, `v1` (`c[187]`/`c[167]` lighting).
   - If `c[186].w < 1` only in the bad picture: the game is fading the car (likely a
     camera-near fade). Trace where the game writes `c[186]` and what drives it (camera
     distance, near plane, aspect/FOV differences from the Xbox build). Fix the input
     to the fade, not the blend.
   - If `pc[6].a` is high in the bad picture: it is the reflection term; go to step 2.
2. **Check the reflection atlas on the frame that samples it.** Dump `81CD8000` at the
   moment of draw 734 (before the 1084 rewrite), not just after 1089. Confirm the previous
   frame's contents survive: no clear/discard/recreate between frames, MSAA resolve done,
   and the deferred replay does not move the 1084-1089 writes ahead of the car draws.
   The recent "Flush before reusing a sampled scene target" change is the first suspect.
3. **Check the atlas lookup.** Log `oT1` (`tc1`) range for a window draw and see which
   quadrant it lands in. If it lands in a black/empty area, compare with how the Xbox
   build sets `texsize[1]`/texture transform for this target.
4. **Black doors/windows:** capture a frame where they are black, find their draws
   (`ps B04016D0` and any door draws), and check whether `t1` is black at sample time
   (step 2) or the output alpha is ~0 with dark rgb (step 3).
5. Implement the narrow fix that the data points to; add a test capture pair to keep.

## Useful capture/env controls

- `XBE_CAPTURE_DIR=captures` - enables F3 captures; required for any capture.
- `XBE_CAPTURE_EVERY=<n>` - save the current render target every `n` draws.
- `XBE_CAPTURE_AT=<seconds>` or `p<picture>` - comma-separated capture triggers.
- `TCNYC_CAPTURE_AT=<backend frame>` - single backend-frame capture trigger.
- `XBE_CAPTURE_INPUTS=1` - log every draw's first vertices/constants.
- `XBE_PAD_SCRIPT=<seconds>:<button>,...` - scripted button presses.
- `XBE_PAD_RECORD=<file>` / `XBE_PAD_REPLAY=<file>` - picture-indexed input.
- `XBE_DETERMINISTIC_REPLAY=1` - deterministic cutscene timing for replay experiments.

## Key files

- `sdk/runtime/gpu/d3d12.cpp` - renderer, deferred scene, target lifetime, capture.
  - `Present` capture trigger around line 5355.
  - `CapStep` / `Capture` around line 5866.
  - `FillCombinerConstants` around line 3186.
- `src/common/pad_tools.cpp` - picture-indexed input record/replay.
- `src/game/timing.cpp` - cutscene clock and deterministic replay.
- `out/build/release/captures/143338`, `143339`, `165823` - primary captures.
- `out/ref_frames/` - extracted reference-video frames.
- `out/crop_143338.png`, `out/crop_143339.png` - bug crops.
- `out/car_dump_sheet.jpg`, `out/car_dump_sheet2.jpg` - sampled frame-dump sheets
  (raw dump frames were deleted).
