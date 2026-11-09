# HDR fixes plan

Brief for an agent fixing the review findings on the HDR output work described in
`docs/hdr-plan.md`. Read that document's "What shipped" section first, then this one.

## Rules

- All HDR work is uncommitted in the working tree. **Do not commit, stash, reset or revert
  anything.** Edit only the lines this plan names, plus whatever they directly need.
- With HDR off, output must stay identical to the current SDR path. None of these fixes may change
  a pixel of SDR output.
- Fix the D3D12 shader (`kBlitHlsl` in `sdk/runtime/gpu/d3d12.cpp`) and the Vulkan present shader
  (embedded HLSL in `platform/linux/backend_vk.cpp`, `HdrHighlights` ~line 2371, `psmain` ~line 2451)
  identically. They are deliberate copies of each other.
- Line numbers are approximate. Find the code by name.

## Fix 1: discontinuous highlight curve (high, both backends)

**Bug:** in `HdrHighlights`,

```hlsl
l2 = knee + (1.0 - knee) * (1.0 + t * t * (P - 1.0));
```

gives `l2 = 1.0` at `t = 0`, so luminance jumps from 0.6 to 1.0 at the knee. All of 0.6–1.0 is
compressed into ≥1.0, even at 0% strength (`P = 1`).

**Fix:**

```hlsl
l2 = l + (1.0 - knee) * t * t * (P - 1.0);
```

Properties to keep (check them in a comment or a small test):
- At `l = knee`, `l2 = knee`, with slope 1 (no kink at the knee).
- At `l = 1`, `l2 = P`.
- At `P = 1` (0% highlights), `l2 = l` everywhere, so HDR is just SDR at paper white.
- It only increases, for `P ≥ 1`.

Also correct the formula in `docs/hdr-plan.md` ("Shader" step 4), which is where the bug came from.

## Fix 2: bloom nearly vanishes in HDR (medium, both backends)

**Bug:** the HDR branch linearises the bloom term on its own
(`SrgbToLinear(saturate(Bloom(i.uv)))`) and adds it after PC contrast. Bloom is a small offset in
display (gamma) space, so linearising it on its own shrinks it by ~2.5x on dark pixels and ~30x
near white. The SDR order is: gamma → **+bloom** → PC contrast. The HDR order is: gamma → PC contrast
→ highlights → +bloom.

**Fix:** match the SDR order up to the linearisation, but don't clamp the bloomed colour at 1 before
the highlight expansion:

```hlsl
c.rgb = ToGamma(c.rgb);                      // existing
if (hdr.x > 0.5) {
    float3 g = c.rgb;
    float3 b = post.y > 0 ? Bloom(i.uv) * 1.2 : 0;   // the same term SDR adds
    g += b;
    if (post.x > 0) g = lerp(g, PcContrast(g), post.x);
    // PcContrast saturates; bloom energy above 1 is carried separately so it can exceed white:
    float3 over = max(c.rgb + b - 1.0, 0.0);
    float3 lin = SrgbToLinear(saturate(g));
    lin = HdrHighlights(lin, hdr.y, hdr.z, hdr.w);
    lin += over * (2.4 * hdr.w);             // slope of the sRGB EOTF at 1 ≈ 2.4, scaled by strength
    ...encode as today
}
```

Notes:
- `PcContrast` returns `saturate(c)`. That's why the part of the bloom above 1 is carried
  separately as `over`. Keep `over` scaled by the strength (`hdr.w`), so 0% highlights gives SDR's
  bloom look clipped at white.
- The Vulkan shader names are different (`BloomAt`, `pc.bloom`, `pc.pc`, `pc.hdr`). Map them
  carefully.
- Acceptance: at 0% highlights, `lin` must equal `SrgbToLinear(SDR output without the dither)` for
  every pixel. Check it with a capture comparison (see Verification).

## Fix 3: Windows HDR toggle not detected at runtime (medium, D3D12)

**Bug:** `ResizeSwap` re-runs `DetectHdrOutput(hwnd, factory_.Get(), ...)` once a second. The DXGI
factory created in `CreateDevice` (`CreateDXGIFactory2`, ~line 1255) keeps returning the outputs and
colour spaces it saw at startup until it is recreated. `IsCurrent()` is never checked.

**Fix:**
1. Add a helper `bool RefreshFactory()`: if `!factory_->IsCurrent()`, call `CreateDXGIFactory2` with
   the same flags as `CreateDevice` (look at what it passes, including any debug flag) into a new
   `ComPtr`, and swap it into `factory_`. Log once when this happens.
2. Call it at the start of the once-a-second check in `ResizeSwap`, and at the start of
   `CreateSwapChain`, before `DetectHdrOutput`.
3. The tearing support check (`f5->CheckFeatureSupport`) does not need repeating.
4. Make sure nothing else holds an `IDXGIFactory` pointer that would go stale (`grep -n factory_`).
   Swap chains made by the old factory remain valid.
5. Correct the claim in `docs/hdr-plan.md` ("still covers Windows HDR toggles") to match what was
   actually verified.

Optional, only if it stays small: make the once-a-second poll cheap by checking `IsCurrent()` first
and only re-enumerating outputs when it returns false or the monitor changed.

## Fix 4: ImGui render pass format mismatch in SDR (medium, Vulkan)

**Bug:** ImGui is initialised once against `ui_pass_` (always `VK_FORMAT_B8G8R8A8_UNORM`), but in SDR
mode it draws inside `swap_pass_`, whose format is `swapchain_format_`. The two render passes are
only compatible when the surface chose BGRA8. On an RGBA8-only surface this is a validation error
and undefined behaviour.

**Fix (preferred): always render ImGui through the offscreen BGRA8 overlay image, in SDR mode too.**
1. In `CreateUiTarget`, allocate the overlay image whenever the UI is in use, not only in HDR mode.
2. In SDR mode, the composite pass outputs `c` unchanged (add `hdr.x == 0` → `return c;` to the
   overlay shader). Because of the premultiplied-alpha "over" blend, the result matches ImGui's
   direct blend for the colours ImGui produces. Check the tolerance with the comparison below. If
   it doesn't match exactly, use the alternative instead.
3. The composite pipeline is created against `swap_pass_`. Check that it's rebuilt when the
   swapchain format changes.

**Alternative, if the comparison shows SDR overlay pixel differences:** keep the direct path in SDR,
but build `ui_pass_` with the same format as `swap_pass_` when `hdr_output_ == Off`. Re-initialise
ImGui (`ImGui_ImplVulkan_Shutdown` + `Init`) whenever the `ui_pass_` format changes, on swapchain
rebuild, using the existing shutdown helper near line 1895. The overlay image itself must stay BGRA8
for HDR.

Also: when HDR is on but `CreateHdrUiComposite()` fails, log once (with `LOGE`) instead of dropping
the overlay silently. Do the same on D3D12 in `RenderUi` if `EnsureHdrUiTarget`,
`ScratchRtv` or `EnsureHdrUiPipeline` fail.

## Fix 5: game HUD pushed to peak brightness (design, both backends)

The game's HUD and menus are in the guest frame, so their white gets the highlight boost. Separating
them from the scene is out of scope here. Do only this:
- Change the default `hdr_highlights` from 70 to **40** in `sdk/runtime/app/app.h` (the struct
  default, used by `RestoreDefaults`). Values already saved in user settings stay as they are.
- Raise the knee from 0.6 to **0.75** linear (about 0.88 in sRGB), so mid-brights and most UI greys
  stay at paper white. Keep the knee as a single named constant in each shader.
- Add a "Known limitations" line to `docs/hdr-plan.md`: HUD white is expanded like scene
  highlights, and lower highlight strength reduces it.

## Verification

Build both targets the same way the original HDR work did (see "Verification results" in
`docs/hdr-plan.md`). The D3D12 release build needs `mt.exe` from the Windows SDK on `PATH`. The
Linux build runs in WSL Ubuntu.

Required:
1. **Curve unit check:** a small standalone test (Python is fine, alongside
   `scripts/test_*.py`). Copy the `HdrHighlights` maths and check continuity at the knee, the value
   at 1, monotonicity, and that `P = 1` leaves the input unchanged, for several `P` values. Also
   check that the two shader sources contain the same curve line (a text check against both files,
   like `scripts/test_resize_recovery.py` reads the source).
2. **SDR identical:** with HDR off, capture the same frame (`TCNYC_CAPTURE_AT=p<N>`) from a build of
   `HEAD` + the current working tree **before** your edits (build it first and keep the exe) and
   after your edits. Diff `final.bmp` byte for byte. Do this on both D3D12 and Vulkan/lavapipe. For
   fix 4, also diff the `_present.tga` with the FPS overlay visible.
3. **0% highlights equals SDR:** on the HDR display (D3D12 scRGB), set highlights to 0% and compare by
   eye with SDR output. If you can, read back the FP16 back buffer and check it equals the SDR frame
   linearised times paper white / 80, within 1e-3.
4. **Runtime Windows HDR toggle:** start in SDR with the HDR setting on, turn on Windows HDR
   (Win+Alt+B), and confirm the log shows the chain rebuild to scRGB within about 2 s. Then turn it
   off again. If you can't toggle it yourself, mark it "not run" and say so.
5. **Vulkan validation:** run lavapipe/Xvfb with `VK_LAYER_KHRONOS_validation` and the overlay
   visible. If possible, force an RGBA8 surface format choice to exercise fix 4: temporarily prefer
   R8G8B8A8 in the format search, test, then revert that.
6. Re-run the existing `scripts/test_*.py`.

## Report back

For each of the five fixes: what changed (file and function), and the result of each verification
item (passed/failed/not run, with the reason). Also report any deviations from this plan, and the
updates you made to `docs/hdr-plan.md`.
