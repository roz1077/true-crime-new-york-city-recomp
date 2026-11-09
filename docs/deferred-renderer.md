# Deferred scene renderer for ambient occlusion

Status: **experimental, D3D12 only**. Setting F10 Ambient occlusion to Half or Full (`ao = 1`
or `2`) turns on the deferred scene path live; Off returns to the immediate renderer.
`TCNYC_DEFER_SCENE=1` still forces the deferred path with AO off.

The first version (opaque group, AO, effect group, AO multiply, present highlight pass) showed
white bands across walls and snow, white distant trees/NPCs/buildings and a streetlight/fog
streak in open-world/menu scenes. It has been replaced by the **receiver AO** design below,
with a GTAO kernel (see `gtao-plan.md`). Checked in game on Windows at Half and Full.

## Receiver AO (current design)

### Why the white bands happened

1. **Effects were in the AO depth.** "Opaque" meant depth-writing only. Light shafts, gobos,
   fog sheets and flat alpha cards that write depth landed in the AO input. AO on a flat effect
   quad is 1, so the wall behind it stayed undarkened while the wall around it darkened: a
   white strip in the effect's shape.
2. **AO was multiplied after the effects**, so effect brightness depended on the AO under them.
3. **The present highlight pass** used a non-monotonic curve (brighter pixels came out darker
   than mid-tones), banding smooth gradients. It also ran over the HUD.
4. The opaque-then-transparent regrouping reordered order-dependent effects.

### What it does now (D3D12, `sdk/runtime/gpu/d3d12.cpp`)

- **Roles** (`AoRoles`): scene-view, non-pretransformed draws that blend not at all, ONE/ZERO
  (most of the world) or as plain alpha (SRCALPHA/INVSRCALPHA, add).
  - Depth-writing ones go into the AO depth (`kAoDepth`). That includes the game's depth-only
    pass for alpha-tested geometry.
  - Colour-writing ones receive AO (`kAoColour`). That includes the alpha colour pass that
    does not write depth.
  - Additive, multiply and other blends are effects and take neither role.
- Each deferred AO-depth draw records its normal bundle **plus a prepass bundle** (same VS, the
  pixel shader's distance variant `PixelKey::ao = 3`, no stencil; recorded in
  `EndDeferredBundle`). The prepass writes depth plus each pixel's **view distance** (clip w)
  into an R32F target (`ao_dist_`). AO never converts hyperbolic depth with a guessed
  near/far: most draws are vertex programs whose projection the port cannot see.
- `FlushDeferredScene`:
  1. runs the prepass bundles into `ao_depth_` / `ao_dist_` (cleared to depth 1.0 and
     distance 1e30 on a full-size depth clear or a new frame);
  2. runs **GTAO** (`AoGtao` in `kAoHlsl`) from the distance target. It writes a half or
     full-size R8 factor, which `kAoOcclusionHlsl` blurs (plane- and depth-aware, 4x4 linear
     taps, seam gaps filled) into a full-size occlusion target;
  3. replays **every bundle in the game's original order**. Receivers' pixel shaders scale
     their colour by `1 - occlusion` read at `t4` (root parameter 4, inherited by the
     bundles), before fog. A null view reads 0.

### Bugs found and fixed while tuning

- Pixel shaders are `ps_5_0`: no register spaces, so the occlusion texture is `t4`.
- Host depth is 0 near, 1 far (the title's GREATEREQUAL is mirrored to LESSEQUAL): clear the AO
  depth to 1.0. Copying the game's last clear sometimes cleared to "nearest" and rejected
  every prepass draw.
- The occlusion pass constants are `src` (texel size) then `planes` (near/far). Packing near/far
  into `src` made every pixel read as sky.
- `ViewAt` must scale screen x/y by distance. Without it every point sat on one plane.
- Stripes on grazing floors had three causes:
  - near/far guessed for hyperbolic depth (fixed by the distance target);
  - comparing a sample's depth with its nearest pixel, which beat against the pixel grid (fixed
    by the horizon test on the pixel actually read);
  - half-resolution centres placed half a texel off their own surface (fixed by snapping to the
    depth texel).
- Dark lines along edges: normals bent across creases. Normals are now an edge-weighted blend
  of all four neighbour pairs, which also fixed a beaded line on every crease.
- MSAA lookups must map uv to `uv * size` texels, not `uv * (size - 1)` (a line across the AO).
- The blur weighs each tap by the smaller of its plane-predicted and plain relative depth error
  (stripes on grazing floors, beads on creases). It fills single-pixel mesh-seam gaps from their
  neighbours and skips them as taps (bright dots along curbs).

### Settings

- F10 Off / Half / Full. Half is GTAO 3 slices × 6 steps at half resolution; Full is 3 × 8.
- Defaults: radius 0.75 (× 5% of distance), strength 1 (a power on visibility).
- Overrides: `TCNYC_AO_RADIUS`, `TCNYC_AO_STRENGTH`, and `TCNYC_AO_METHOD=ssao` (the earlier
  hemisphere kernel, for comparison only).

### Remaining caveats

- Screen-space: hidden geometry casts nothing. A character standing in front of a crease can
  soften the shadow right at its outline.
- A wall within the radius darkens a character standing next to it (mild at 0.75).
- Scene draws that cannot be deferred (rare fallback frames) get no AO.
- Vulkan is not ported.

The sections below are the original design notes and history.

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

Expect the white-effect artifacts above. (Historical: the AO debug views mentioned here were removed.) The AO
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
