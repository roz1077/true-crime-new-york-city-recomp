# Vulkan port of receiver GTAO

Goal: the Linux / Steam Deck backend (`platform/linux/backend_vk.cpp`) gets the same ambient
occlusion as Windows: same look, same settings, same fixes. It should also be fast enough to
leave on at Half on the Deck.

Reference implementation: D3D12 in `sdk/runtime/gpu/d3d12.cpp`, described in
`deferred-renderer.md` and `gtao-plan.md`. Everything that lives in shared code (`PixelHlsl`
in `shaders.cpp`, `PixelKey::ao`, `AoRoles`-style rules, the GTAO and blur HLSL) should be
reused as-is, not rewritten.

## What carries over unchanged

- **Pixel shader variants.** `PixelKey::ao`:
  - 1: multiply by `1 - occlusion` before fog;
  - 3: write view distance (clip w).

  These live in `PixelHlsl`, which Vulkan already compiles through shaderc. Only the `t4`
  register needs a Vulkan binding (see step 3).
- **AO kernel and blur HLSL.** `kAoHlsl` and `kAoOcclusionHlsl` are plain full-screen HLSL:
  - move them out of `d3d12.cpp` into shared code, for example a `sdk/runtime/gpu/ao_shaders.h`,
    so both backends compile the same text;
  - Vulkan needs `[[vk::binding]]` attributes and a push constant block in place of
    `cbuffer B : register(b1)`, as the present shader already does.
- **The rules:**
  - blend-mode roles (none, ONE/ZERO and plain alpha);
  - the depth-only alpha pass feeds AO depth, and the alpha colour pass receives AO;
  - draws that sample a colour render target take no role (water, mirrors);
  - pretransformed and non-scene-view draws are excluded.

  Move `AoRoles` into shared code, taking `State` and the per-stage "samples a colour target"
  flags.
- **Settings.** `render_.ao` (Off / Half / Full) and the `TCNYC_AO_RADIUS`, `TCNYC_AO_STRENGTH`
  and `TCNYC_AO_METHOD` overrides. `settings_linux.cpp` already loads `ao`.

## What differs on Vulkan, and the design for each

### 1. Deferring scene draws: a CPU display list, not secondary command buffers

D3D12 records each scene draw into a bundle and replays the bundles at a flush. Vulkan has no
root-signature inheritance, and every draw happens inside a render pass (`BindTargets` /
`FlushPass`). Secondary command buffers would need render-pass inheritance info fixed at record
time, and a second compatible pass for the depth prepass.

The simpler fit for this backend is a **display list of fully resolved draws**:
- At record time, `Draw` does everything it does now: assemble, upload vertex and index data,
  pick shaders, create the pipeline, allocate and write the per-draw descriptor set, compute
  push constants and dynamic state. It stores the result instead of calling
  `vkCmdDraw*`:
  - the pipeline;
  - the descriptor set;
  - vertex and index buffer offsets;
  - the draw counts;
  - viewport, scissor, blend constants and stencil reference;
  - the render target and depth target.
- A second pipeline is created for AO-depth draws: the distance variant (`ao = 3`) writing
  R32F distance plus depth, with no stencil. It is stored with the draw.
- At a flush, the list is replayed as plain `vkCmd*` calls. Replay is cheap: no shader or
  pipeline work, just binds and draws.

**Hazard rules:** mirror D3D12's `defer_reads_` / `defer_writes_`. Flush before any of these
touches something a pending draw reads or writes:
- clears;
- copies;
- resolves;
- `ReadTarget` / `Sync` / `RecordLate` read-backs;
- `UploadTarget` / CPU writes;
- feedback copies.

The D3D12 list of `DeferredBarrier` call sites is the checklist.

**Upload and descriptor lifetime:** all of a frame's pending draws must keep their upload ring
ranges and descriptor sets until the flush. Any path that resets or wraps them mid-frame must
flush first. D3D12 does this in `Upload`, `AllocSrvSlots` and the ring-full checks in `Draw`.

### 2. Flush order (same as D3D12)

1. **AO depth prepass.**
   - Begin a render pass on the AO depth image (D32, same size and samples as the scene
     depth) plus the R32F distance image.
   - On a new scene view, clear them to depth 1.0 and distance 1e30 with load-op CLEAR.
     Otherwise use LOAD, because the prepass accumulates across a view's flushes.
   - Replay only the AO-depth draws, with their distance pipelines.
2. **GTAO pass:** the distance image goes to a half or full-size R8 AO image (`kAoHlsl`,
   3 × 6 at Half, 3 × 8 at Full).
3. **Occlusion pass:** the blur and upsample into a full-size R8 occlusion image
   (`kAoOcclusionHlsl`: plane- and depth-aware, 4x4 linear taps, seam-gap fill).
4. **Replay every draw in its original order**, re-opening the scene render pass with LOAD.
   Receivers use their `ao = 1` pipelines, with the occlusion image bound at `t4`.

Layout transitions between steps: the distance image goes COLOR_ATTACHMENT, then
SHADER_READ_ONLY, then back. The AO images likewise. Use the backend's existing `Transition`
helper.

### 3. Binding the occlusion texture (`t4`)

- Add the occlusion image as a **separate descriptor set (set 1)** with one sampled image, bound
  once per flush before the replay. This leaves the per-draw sets (set 0) untouched and
  mirrors D3D12's root parameter 4.
- `InjectBindings` maps `register(t4)` to `[[vk::binding(0, 1)]]`.
- The pipeline layout gains set 1. Pipelines that don't use it ignore it.
- When no AO ran in a flush, bind a set pointing at a 1x1 R8 image holding 0, which means no
  occlusion. Vulkan has no null descriptors without an extension, so a real image is needed.

### 4. Depth and distance conventions

- The distance target removes all near/far guessing, so the D3D12 depth-convention bugs (the
  reversed versus standard clear value, the decode flip) don't apply to the AO math. The only
  depth the AO reads is R32F distance.
- The AO depth image's depth test must match the scene's. Use the same compare op as the
  draw's pipeline and clear to the value that op treats as far. Check `ReversedCmp` and the
  Vulkan equivalent before assuming 1.0. This exact mistake emptied the AO depth on D3D12.
- `SV_Position.w` in the distance variant compiles to `gl_FragCoord.w`, which in SPIR-V is
  **1 / w**, not w. The `ao = 3` variant must write `1.0 / i.pos.w` on Vulkan, or pass clip w
  through an interpolant. This is the most likely Vulkan-only bug, so test it first.
- The vertex epilogue's y-flip fix in `CompileShader` stays. Check that `ViewAt`'s `-n.y`
  still matches Vulkan's uv direction (top-left origin in both, so it should).

### 5. MSAA

- The Deck defaults to MSAA 2x or off. Support both sample counts for the AO depth and
  distance images.
- **R32F as an MSAA colour attachment:** check
  `VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT` with sample counts for R32_SFLOAT on the Deck.
  It is widely supported, but if a sample count is missing, fall back to sample 0 only by
  rendering the prepass single-sample.
- Read sample 0 in the AO shaders (`Texture2DMS.Load`), with the pixel-exact
  `uv * size` mapping.

## Steps

1. **Shared code first:** move `AoRoles`, the AO shader HLSL and the constants (strength,
   radius, `kAoDistClear`) out of `d3d12.cpp`. Rebuild D3D12 and check it looks identical
   (probe image diff, same scenes).
2. **Display list on Vulkan, no AO:** defer scene draws and replay them in order. Behind
   `TCNYC_DEFER_SCENE=1` it must render identically to immediate mode: menu, street, corridor,
   water, cutscenes, the HUD, and MSAA off, 2x and 4x.
3. **Hazard flushes:** port every D3D12 `DeferredBarrier` case and the upload and descriptor
   lifetime rules. Soak test 15 minutes of play with `TCNYC_DEFER_SCENE=1` and no AO, with no
   crashes and no visual changes. Watch for the effect-manager crash seen once on D3D12.
4. **Prepass, GTAO and occlusion passes:**
   - first, verify the distance variant (1 / `gl_FragCoord.w`);
   - then port the probe from the D3D12 investigation, temporarily: R8 dumps to PGM;
   - compare the Vulkan probe images against D3D12 at the same camera.
5. **Receivers:** set 1, the `ao = 1` pipelines and the fog-correct multiply. Turn on from the
   F10 row like D3D12.
6. **Deck performance:**
   - frame time, AO Off / Half / Full, in the street, corridor, room and menu scenes;
   - check whether the replay overhead (CPU) or GTAO (GPU) dominates;
   - only if Half doesn't fit the frame budget, add the GTAO quality preset from `gtao-plan.md`
     (Low: 1 slice × 4 steps).
7. **Cleanup:** remove the probe, update the docs, README and CHANGELOG, commit.

## Acceptance criteria ("as good as Windows")

- **Same images:**
  - Vulkan and D3D12 probe dumps at the same camera match closely (mean occlusion within a
    few levels on 0–255);
  - no stripes on grazing floors, no beads on creases, no seam dots, no edge bands.
- **Same exclusions:** water, reflections, mirrors, light shafts, fog and HUD untouched; no
  white bands.
- **Same settings and defaults:** Off / Half / Full, off by default, radius 0.75, strength 1.
- **Stable:** a 15-minute soak with AO on, with no crashes, no device loss, and no
  `VK_ERROR_*`. The validation layers are clean on desktop Linux.
- **Deck:** Half adds an acceptable frame-time cost at the Deck's native resolution, measured
  in step 6, and holds 60 FPS where the game did before (or 30 with the cap).

## Risks

- **`gl_FragCoord.w` is 1 / w** (step 4): the distance variant silently writes the wrong value
  unless handled.
- **Replay correctness:** the replay is the riskiest part, as on D3D12. The water spikes turned
  out to be AO on water, not the replay, but one crash with the replay forced on is still
  unexplained. Steps 2 and 3 exist to catch replay bugs before AO hides them.
- **Render-pass churn:** each flush closes and reopens the scene pass. Keep flushes rare, and
  measure them in the log, as D3D12 reports flushes per frame.
- **Memory on the Deck:**
  - extra full-size targets: D32 AO depth, R32F distance (×2 with 2x MSAA), the R8 AO and the
    R8 occlusion;
  - about 2 × 4 + 4 + 1 + 1 bytes per pixel at 2x MSAA, roughly 14 bytes per pixel, about
    13 MB at 1280×800. Fine.
- **Shared-code moves:** moving HLSL and rules out of `d3d12.cpp` risks changing D3D12. Step 1
  checks that with probe diffs before any Vulkan work.
