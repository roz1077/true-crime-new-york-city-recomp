# HDR output plan

Brief for an agent implementing HDR display output. Read the whole document before editing.

## Goal

Add an optional HDR output mode on D3D12 (Windows) and Vulkan (Linux/SteamOS). It expands the
finished 8-bit frame into HDR highlights in the **final present pass only**. With HDR off, or on a
display that cannot do HDR, output must be bit-identical to today.

## Non-goals

- Do not change guest render targets, texture formats, fixed-function blending or the AO/SMAA
  passes. The guest renders into `B8G8R8A8_UNORM` and stays that way.
- No new third-party dependencies.
- Do not commit. Leave the changes in the working tree for review. The tree already has unrelated
  uncommitted edits (input, prompt glyphs, camera, settings), so touch only what this plan needs.
  Be careful with `sdk/runtime/app/settings.cpp`, `src/common/graphics_*` and
  `platform/linux/backend_vk.cpp`, which already contain other edits. Add to them; do not revert
  anything.

## Background (current pipeline)

- `sdk/runtime/gpu/d3d12.cpp`
  - Swapchain: `CreateSwapChain` (~line 1168) uses `DXGI_FORMAT_B8G8R8A8_UNORM`, and the
    resize path (~line 4687) calls `ResizeBuffers` with the same format. `tearing_` sets
    `ALLOW_TEARING`.
  - Present blit shader (embedded HLSL, ~lines 140–265), `psmain`: optional supersample taps →
    `DistanceBlur` → `ToGamma` (the title's 256-entry gamma ramp LUT) → `+ Bloom * 1.2` →
    `PcContrast` blended by `post.x` → 4x4 ordered dither → output. Everything is in display-referred
    0–1 space.
  - `BlitOptions` (~line 505) and the constant upload (~line 1956) carry `gamma`, `post`, `bloom`
    and `dof` into the shader as `opts`/`post`/`src`. The final frame blit is ~lines 4780–4795.
  - ImGui is initialised with BGRA8 (~line 1130). Screenshot/readback code assumes BGRA8 (~line
    1831).
- `platform/linux/backend_vk.cpp`
  - `CreateSwapchain` (~line 1494) picks the first B8G8R8A8/R8G8B8A8 UNORM surface format and stores
    it in `swapchain_format_`. The present render pass and framebuffers use that format (~line
    1622).
  - The present shader (embedded HLSL compiled to SPIR-V, ~line 1866+) mirrors the D3D12 one:
    `gammaTex`, push constants `{pc, bloom, dof, pad, src, zrange}`.
  - ImGui Vulkan init is at ~line 1683. Swapchain rebuilds happen at ~6317, 6341, 6476, 6546 and 6784.
- Settings: `sdk/runtime/app/settings.cpp` and `app::Snapshot()` (for example `.vsync`, `.bloom`),
  the shared graphics options/menu in `src/common/graphics_options.cpp`, `graphics_choices.h` and
  `graphics_menu.h`, and the Linux menu stub `src/linux/graphics_menu_stub.cpp`. Follow the
  existing bloom/vsync options exactly for storage, the menu and the snapshot.

## Design

### Output modes

`enum class HdrOutput { Off, ScRgb, Hdr10 }`, chosen at swapchain creation:

| Backend | Preferred | Fallback |
|---|---|---|
| D3D12 | scRGB: `R16G16B16A16_FLOAT` + `DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709` | HDR10: `R10G10B10A2_UNORM` + `DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020` |
| Vulkan | HDR10: `A2B10G10R10_UNORM_PACK32` + `VK_COLOR_SPACE_HDR10_ST2084_EXT` (gamescope) | extended sRGB linear: `R16G16B16A16_SFLOAT` + `VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT` |

Use HDR only if the setting is on **and** the display or surface supports it. Otherwise use `Off`
with today's format, and log the reason once.

- D3D12 detection: `swap_->GetContainingOutput()` → `IDXGIOutput6::GetDesc1()`. HDR is available
  when `ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020`. Read `MaxLuminance` as the
  default peak. Confirm with `IDXGISwapChain3::CheckColorSpaceSupport` before `SetColorSpace1`.
  Re-check when the window moves to another monitor or the size changes (WM_DISPLAYCHANGE, the
  resize path).
- Vulkan detection: enable the instance extension `VK_EXT_swapchain_colorspace` if it is available
  (it is optional, so don't fail without it), then search the surface formats for the pairs above.
  Optionally use `VK_EXT_hdr_metadata` (`vkSetHdrMetadataEXT`) after creation to pass the peak and
  paper-white values.

### Shader (shared logic, written twice: D3D12 HLSL and Vulkan HLSL)

Add constants: `hdr.x = mode (0 off, 1 scRGB, 2 PQ)`, `hdr.y = paperWhiteNits`,
`hdr.z = peakNits`, `hdr.w = highlightStrength (0..1)`.

In `psmain`, with `mode != 0`:

1. Run the existing taps, DistanceBlur and ToGamma unchanged, so SDR tone is preserved.
   **Skip the dither.**
2. Bloom: compute `Bloom()` as today and add it to the display-referred colour before PC contrast,
   matching the SDR order. The part of the bloom that lands above 1.0 is carried separately as
   `over`, so it can exceed white after linearisation instead of being clipped.
3. PC contrast: apply the existing `PcContrast` blend to the bloomed colour.
4. Linearise: `lin = SrgbToLinear(saturate(c))` (exact piecewise sRGB EOTF). Inverse tone map the
   highlights using luminance `Y = dot(lin, Rec709Y)`:
   - Below the knee `k = 0.75` (linear), the colour is unchanged.
   - Above it, map `t = (Y - k)/(1 - k)` through `Y' = Y + t * t * (P - 1)`, where
     `P = lerp(1, peakNits/paperWhiteNits, highlightStrength)`. This is continuous at the knee
     with slope 1 and equals `P` at Y=1. Scale the RGB by `Y'/Y`, preserving hue.
   - Add the bloom energy above white: `lin += over * 2.4 * highlightStrength`.
5. Encode:
   - scRGB: `out = lin * paperWhiteNits / 80`.
   - PQ: `rgb2020 = mul(Rec709to2020, lin)`, `nits = rgb2020 * paperWhiteNits`, clamp to
     `[0, 10000]`, then the ST.2084 OETF.
6. The alpha path (`opts.y`) is unchanged.

The `Off` path must stay exactly the existing code (branch on `mode` at the top, and keep the old
code path verbatim).

Only the final frame blit sends `mode != 0`. Every other `Blit` caller (intermediate composites,
SMAA, AO) keeps `mode = 0`.

### UI and overlays

ImGui, the prompt glyphs and the overlays are drawn after the frame blit into the back buffer.
In HDR mode they must come out at paper white:
- Initialise ImGui with the actual swapchain format. Re-initialise the ImGui backend when the
  format changes, or initialise it once per format.
- Simplest correct approach: render ImGui into an intermediate BGRA8 target (existing helpers
  create one at the back-buffer size), then composite it with a tiny pass that linearises,
  multiplies by paper white and encodes like step 5, using premultiplied-alpha "over". If that's
  too invasive, document the limitation instead: in scRGB mode, sRGB UI values are interpreted
  linearly at 80 nits, which looks dim and washed out, so the composite pass is preferred.

### Readbacks

Screenshots (F3 captures, `XBE_CAPTURE_AT`), the texture dumper and any back-buffer readback must
still produce SDR BGRA8. Prefer capturing the pre-HDR frame (the source of the final blit),
or run the blit once in `Off` mode into a BGRA8 target for the capture. Check every readback site:
`grep -n "ReadBack\|Capture\|Screenshot" sdk/runtime/gpu platform/linux`.

### Settings

Add to the shared graphics settings, following the existing pattern for bloom/vsync:
- `hdr` (bool, default **off**)
- `hdr_paper_white` (nits, 80–400, default 200, step 10)
- `hdr_peak` (nits, 400–2000, default 0 = use the display-reported value, else 1000)
- `hdr_highlights` (0–100 %, default 70)

Menu: an "HDR" toggle, and when it is on, the three sliders. Grey them out with a note ("Display
does not report HDR") when it is unavailable. Toggling `hdr` triggers a swapchain rebuild (the same
mechanism vsync/tearing changes already use). The sliders take effect immediately through the
constants.

## Work order

1. **Settings plumbing** (shared): fields, persistence, snapshot, the menu entries, the Linux stub.
   Build both targets.
2. **D3D12 swapchain**: detection, format/colour-space selection in `CreateSwapChain` and the
   resize path, `SetColorSpace1`, the fallback logging. The `ALLOW_TEARING` flag stays valid with
   FP16 flip-model; keep it.
3. **D3D12 shader**: the constants, the HDR branch, wired only into the final blit.
4. **D3D12 UI composite and readbacks.**
5. **Vulkan**: the same steps 2–4 in `backend_vk.cpp` (extension, format choice, the present render
   pass/framebuffer format, push constants: extend the range and keep the 128-byte guarantee, the
   shader, ImGui, captures).
6. **Docs**: a short section in this file recording what shipped and how it was verified.

## Verification

Build commands: check `CMakeLists.txt`/`README` and the existing `docs/*-plan.md` for the exact
commands. Build the Windows D3D12 target, and the Linux target if a toolchain is available. If it is
not, say so explicitly.

Required checks:
- **HDR off is bit-identical**: capture the same frame (`XBE_CAPTURE_AT=p<N>`) before and after
  the change with HDR off, and diff the images. They must match exactly.
- **HDR on, non-HDR display**: falls back to Off and logs it; no crash on toggling.
- **HDR on, HDR display** (if one is available; otherwise state that it is unverified): the
  swapchain format/colour space is logged; the picture is not washed out; neon/headlights are
  brighter than the UI; the UI is readable; captures are still normal SDR images.
- Toggle HDR at runtime and resize/fullscreen repeatedly; no D3D12 debug-layer or Vulkan
  validation errors (enable both).
- Fades to black via the gamma ramp still reach black in HDR.

## Report back

List the changed files, the build results per target, each verification item marked
passed/failed/not run (with the reason), the known limitations, and any deviations from this plan
and why.

## What shipped

- **Settings**: `hdr`, `hdr_paper_white`, `hdr_peak` and `hdr_highlights` live in `app::Settings`,
  persist through both settings backends, and appear in the Graphics menu under an **HDR** heading.
  New installs default to 40% highlights. The sliders are disabled when the display does not report
  HDR; the toggle stays reachable so a failed detection can still be retried and the fallback is
  visible in the value/help text.
- **D3D12**: the swapchain is FP16 scRGB (`R16G16B16A16_FLOAT` +
  `RGB_FULL_G10_NONE_P709`) when the setting is on and the output reports HDR, falling back to
  HDR10 (`R10G10B10A2_UNORM` + `RGB_FULL_G2084_NONE_P2020`) and then to today's BGRA8 SDR.
  The once-a-second poll recreates the DXGI factory when `IsCurrent()` returns FALSE, so the
  output is re-read after Windows HDR toggles and monitor moves; `ALLOW_TEARING` is unchanged.
- **Present shader (both backends)**: the Off path is the original code. In HDR mode it keeps the
  existing taps/DistanceBlur/ToGamma, adds the same bloom term as SDR before PC contrast, carries
  bloom energy above white separately, linearises with the exact sRGB EOTF, expands only
  highlights above a 0.75 linear knee towards the peak (continuous at the knee, equals the peak at
  white, hue preserving), and encodes scRGB or ST.2084 PQ. Only the final frame blit sends
  `mode != 0`.
- **Overlays**: D3D12 draws ImGui straight into the SDR back buffer and through a BGRA8 target in
  HDR; Vulkan always draws it into a BGRA8 target and composites it, unchanged in SDR and at paper
  white with premultiplied-alpha over blending in HDR. ImGui stays initialised for BGRA8 on both
  backends.
- **Readbacks**: D3D12 F3/`TCNYC_CAPTURE_AT` captures remain guest BGRA8 images. Vulkan's
  `_present.tga` decodes FP16 scRGB and 10-bit PQ readbacks back to an SDR TGA.
- **Vulkan**: `VK_EXT_swapchain_colorspace` is enabled when available; the surface is searched for
  HDR10 then extended-sRGB-linear. `VK_EXT_hdr_metadata` is used when available to publish BT.2020
  primaries and peak/paper-white values. The present push-constant block grew to 48 bytes.

## Verification results

- **Builds**: Windows D3D12 Release passed (`cmake --build out\build\release`; `mt.exe` from the
  Windows 10 SDK must be on `PATH`). Linux Vulkan passed in WSL Ubuntu 24.04
  (`cmake --build out/build/linux`). Both changed D3D12 shaders compile with D3DCompile; the
  Vulkan present and overlay shaders compile with both glslangValidator and `glslc`.
- **HDR off, SDR run**: passed on D3D12 (game rendered normally) and on Vulkan with lavapipe under
  Xvfb. The Vulkan `trace_10.tga` guest capture and `_present.tga` present capture are byte-identical
  before and after the fixes (no overlay visible in that run). With the FPS overlay visible, the
  overlay crop is pixel-identical (`compare -metric AE` = 0) to the pre-fix direct-draw screenshot.
  The D3D12 `final.bmp` before/after comparison was **not run**: no pre-fix host binary was kept, and
  the pre-HDR binary available in the tree could not load the current game-code DLL. The D3D12 Off
  render path is unchanged.
- **HDR curve**: passed `scripts/test_hdr_curve.py`: continuous at the 0.75 knee with slope 1,
  reaches `P` at white, is monotonic, and is the identity at 0% highlights; both backend functions
  textually match.
- **0% highlights equals SDR**: the curve identity is unit-tested; a live FP16 back-buffer readback
  was **not run**.
- **HDR on, non-HDR display**: passed. D3D12 logged
  `HDR requested but the display does not report HDR; using SDR output`; Vulkan logged
  `HDR requested but the surface offers no HDR format; using SDR output`. No crash or swapchain
  failure on either backend.
- **HDR on, HDR display**: D3D12 scRGB passed. With Windows HDR enabled it logged
  `HDR scRGB output, 3440x1440 FP16, peak 455 nits`, the Graphics menu composited correctly and was
  readable, neon/lamps were visibly above paper white, and the SDR GDI screenshot still showed the
  game and overlay. HDR10 fallback, Vulkan HDR10 and Vulkan extended-linear output were **not run**
  (no HDR-capable Vulkan surface was available; lavapipe/Xvfb only offers SDR formats).
- **Captures**: D3D12 `TCNYC_CAPTURE_AT` in HDR mode produced the normal 640x480 32-bit SDR
  `final.bmp`; Vulkan trace TGAs were verified in SDR, and the HDR decode path is compile-checked
  but was not run on an HDR swapchain.
- **Runtime Windows HDR toggle**: passed. Starting in SDR with the HDR setting on, turning Windows
  HDR on rebuilt the chain to `HDR scRGB output, 3440x1440 FP16, peak 455 nits` within about 2 s;
  turning it off rebuilt to `SDR output, 3440x1440 BGRA8`. This run used a forced refresh every second;
  the poll now refreshes only when `IsCurrent()` is FALSE and has not been re-tested live.
- **Repeated resize / fullscreen**: not run as a separate test. The resize paths were exercised by
  the builds and capture runs; menu keyboard automation was not reliable in this setup.
- **Debug/validation**: D3D12 debug layer was not enabled. Vulkan was run with
  `VK_LAYER_KHRONOS_validation` under lavapipe/Xvfb for an SDR session that drew the ImGui FPS
  overlay; it produced no validation errors (only loader notices). The lavapipe X11 surface does not
  offer RGBA8, so the forced-RGBA8 surface check was **not run**; the BGRA8 SDR composite was
  verified pixel-identical above. The app does not install a debug messenger, so validation is a
  best-effort check.
- **Fades to black**: the gamma-ramp path runs before linearisation and reaches zero; not separately
  captured in HDR.

## Known limitations and deviations

- Vulkan has no standard display-peak query, so the reported default is 1000 nits unless the user
  sets a peak.
- Vulkan's HDR overlay composite blends in the PQ-encoded target for HDR10. This is exact for the
  scRGB/extended-linear target and slightly approximate at overlay edges for HDR10; the alternative
  is a second full-size HDR scene target.
- The game's HUD and menus are part of the guest frame, so their white is expanded like scene
  highlights; lower highlight strength reduces it, and the default is 40%.
- `IsCurrent` once looked unreliable only because its BOOL result was compared against `S_OK`
  (TRUE != 0), which forced a new factory every second. It is now read as a BOOL and the factory
  is recreated only when stale.
- The fixes plan's replacement curve line (`l2 = l + (1 - knee) * t * t * (P - 1)`) does not reach
  `P` at white. The shaders use `l2 = l + t * t * (P - 1)`, which satisfies every listed property
  (and the new curve test).
- Vulkan always keeps one extra full-size BGRA8 overlay image (SDR and HDR) because ImGui draws
  through that target in both modes. D3D12 allocates its overlay target only while HDR is active.
- `HdrOutput` is shared from `gpu/backend.h` so both backends and the menu use the same enum. The
  rest of the design follows this document.
