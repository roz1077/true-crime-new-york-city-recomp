# Deferred scene renderer for ambient occlusion

Status: **experimental / disabled by default**. The D3D12 deferred path and AO composite are
retained on the `deferred-ao` branch, but the F10 AO row no longer enables them: normal play
uses the original immediate renderer. Re-enable only with `TCNYC_DEFER_SCENE=1` and `ao = 1`.

The room save at 3840x2160 showed correct AO and layer ordering, but open-world/menu scenes
showed bright white bands across walls and snow, white distant trees/NPCs/buildings, and a
streetlight/fog streak. The captures (F3 guest frame and host screenshots) show these are
**bright guest light/fog/effect layers** that AO does not darken; AO's contrast makes them read
as artifacts. The two-group replay can also reorder transparent effects, so this needs the
clean-depth + effect-mask/order-preserving design before it can ship. See "Known issue and
plan" below.

## Goal

Render the opaque scene into a dedicated pass, run depth-based AO there, then render
transparent/effect/HUD layers over it. This is the only placement where AO can behave
like an in-scene effect rather than a screen-space overlay.

## Non-goals

- Reimplementing the whole game renderer.
- Reordering game logic that reads render targets between draws.
- Changing the Xbox draw API exposed to the title.

## Constraints discovered

- The title issues hundreds of draws, with depth-writing and blended draws interleaved.
  There is no usable "first transparent draw" boundary in the stream.
- D3D12 and Vulkan currently execute each draw immediately.
- The title can render to offscreen/CPU/movie targets and can copy between surfaces.
- The game's depth is reversed in D3D12 and standard 0..1 in Vulkan.
- A second render pass cannot read guest memory that has been overwritten by later draws,
  so the replay must either deep-copy the draw inputs or keep the original draw order
  within a scene target.

## Proposed architecture

### Phase 1: deferred main-scene capture

For the main full-size scene colour target only:

1. Intercept draws while the game is rendering the scene target.
2. Record enough state to replay each draw later:
   - primitive, vertex/index data copied out of guest memory;
   - the complete `State` snapshot and pipeline key;
   - the vertex/pixel shader handles and constants;
   - texture handles/descriptors used by the draw.
3. Do not execute the recorded draws immediately.
4. Execute other targets/clears/copies immediately.
5. At the scene target's end-of-frame boundary, replay:
   - opaque draws first,
   - AO pass,
   - transparent/effect draws,
   - HUD/UI as the game ordered it after the scene.

This is a deferred display list for one target, not a general engine rewrite. If a draw
reads the scene target before it is replayed, fall back to immediate rendering for that
frame and log it; those scenes are not eligible for deferred AO.

### Phase 2: AO insertion

- Record opaque draws (depth-writing, blend off, full scene target) and transparent
  draws separately.
- Replay opaque draws into the scene colour/depth target.
- Run AO from the completed opaque depth.
- Composite AO into the scene colour with a multiply blend.
- Replay transparent/effect draws in their original relative order.

### Phase 3: D3D12 implementation

- Use D3D12 command lists/bundles per recorded draw, or replay the existing `Draw` path
  with a captured `State` + deep-copied `Assembled` data.
- Bundles are attractive because each bundle captures PSO/root arguments/descriptors; the
  direct list only executes them in the desired order.
- Descriptor heaps must be bound on the direct list; bundles inherit them.
- Upload-ring addresses and descriptor slots must stay valid across the deferred replay:
  allocate them for the frame and do not reset until after execution.
- Queries/captures that need draw results must force immediate mode for that frame.

### Phase 4: Vulkan implementation

Vulkan has no bundles. Use secondary command buffers with render-pass inheritance, or
record a compact draw display list and replay it through the existing `Draw` path.
MSAA scene targets need the same sample count and resolve handling as the current path.

## Milestones

- **M0**: D3D12 capture/replay identical frame. Record main-scene draws, replay in the
  original order at the scene boundary, compare against immediate rendering. No AO.
- **M1**: classify opaque/transparent and replay opaque then transparent; compare.
- **M2**: insert AO between the two groups; tune.
- **M3**: Vulkan equivalent.
- **M4**: enable the F10 option and update docs.

## Implementation notes (D3D12)

- Opaque means **depth-writing**, not blend-disabled. The engine blends most world geometry
  while still writing depth (alpha-tested materials). In the room capture: 280 depth-writing
  bundles vs 77 no-depth-write effects. The no-depth-write group draws after AO.
- A materialisation barrier (target sampled, copied, cleared, resolved, read back, or written
  immediately) flushes pending bundles, keeps the frame deferred, and continues. Only a
  conflict while a bundle is open falls back to immediate for the rest of the frame.
- AO runs once per scene view (`defer_ao_applied_`); a clear of the scene colour/depth starts
  a new view and re-arms it. This avoids double-darkening when a frame has several barriers.
- Depth range uses `DrawWRange(s, sh.prog)` with the `far > 1000 -> 1000` normalisation. A
  fixed-function `ProjectionWRange` alone fails on program draws.
- D3D12 reversed depth is converted with `1 - depth` before the hyperbolic linearisation.
- The AO kernel is the 8-tap hemisphere estimate: strength 0.15 (WIP), radius 1.0 world units,
  depth-aware 5x5 composite blur. Smaller radius reduces the silhouette halo.
- A present-time highlight pass samples a scene-colour copy and compresses bright near-camera
  effects toward the AO-darkened scene. It is not sufficient alone; the bright band still reads
  on the wall in the room/alley test.

## Known issue and plan

### Failure mode

- AO darkens the opaque scene but leaves bright guest layers (streetlight gobos/light shafts,
  fog, snow, additive reflections, lens-flare-like quads) at full brightness. With AO off, the
  surrounding wall is bright enough that the band blends in; with AO on, the wall darkens and
  the effect reads as a white band.
- The menu/attract camera is the open world: `game camera: active 1 mode 23 target 0` was
  observed. A prototype `GameplayCameraActive()` gate exists but is not enabled in the current
  default path.
- The two-group opaque/transparent replay reorders effects relative to the original stream.
  That is unsafe for order-dependent fog/light/reflection layers and was a known risk in the
  original review.
- F3 captures the guest frame before the host presentation/bloom pass. The wall band is present
  in the guest capture; the port's bloom can add contrast but is not the only source.

### Plan (next attempt)

1. **Clean opaque-only depth** — give AO a depth target that contains only opaque world
   geometry. Two viable implementations:
   - a depth-only prepass of opaque draws into a separate depth target; or
   - a linear-depth MRT written by opaque PSOs.
   AO must never read fog/sky/light/effect quads.
2. **Preserve original draw order** — do not globally group all opaque then all transparent.
   Find an API-level scene boundary (target transition, first scene read/copy, depth clear,
   EndScene). Apply AO there and keep the recorded order within each boundary.
3. **Effect mask** — classify light shafts/gobos/fog/reflections/lens flares by blend/depth
   state and target association. Exclude them from the AO depth and either leave them out of
   AO or apply a separate highlight compression. A stencil bit written by additive/bright
   effects is the robust option.
4. **Highlight-aware composite** — keep the scene-colour copy/near-bright compression
   prototype, but tune it after the depth/effect mask is correct. It must not compress the
   far sky or the HUD.
5. **Front-end gate** — keep AO on the immediate path until a gameplay camera is confirmed
   (mode/target, not just object presence). The menu/mode 23 camera showed why the first gate
   was too permissive.
6. **Validation matrix** — same-angle AO on/off captures for: room save, corridor, outdoor
   street, park/trees, rain/fog, fire barrel, reflections, menu/attract, cutscenes, HUD;
   Bloom on/off; F3 guest captures and host captures.
7. **Vulkan** — port only after the D3D12 design is stable: secondary command buffers or a
   compact draw-command replay, clean opaque depth, effect mask, and the same validation.

### Re-enabling for experiments

```
TCNYC_DEFER_SCENE=1
ao = 1
```

Expect the white-effect artifacts above. `TCNYC_AO_VIEW=1` / `TCNYC_AO_DEPTH=1` show the AO
factor/linear depth. The default settings and the F10 row should stay Off until the plan is
complete.

## Risks / tests

- Render-target reads/feedback between draws: detect and fall back.
- UI/HUD ordering: HUD must remain after the scene; do not defer UI-target draws.
- MSAA resolves, depth conventions, and CPU-written movies: force immediate mode.
- Save/load, pause menu, cutscenes, mirrors, reflections, weapon overlays.
- Performance: bundle/secondary-buffer overhead and extra offscreen memory.

## Worktree

This work lives in the `deferred-ao` worktree so the broken present-pass AO experiments do
not contaminate `main`.
