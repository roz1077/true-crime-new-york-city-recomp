# Deferred scene renderer for ambient occlusion

Status: **M0 prototype builds; visual validation pending**. `TCNYC_DEFER_SCENE=1` records
full-size main-scene draws into D3D12 bundles and replays them at present. The first
runtime run reported `replayed 15 bundle(s), no fallback`; the test capture was partly
obscured by another window, so a clean corridor comparison is still needed. The next
milestone is splitting opaque/transparent bundles and inserting AO between them.

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

## Risks / tests

- Render-target reads/feedback between draws: detect and fall back.
- UI/HUD ordering: HUD must remain after the scene; do not defer UI-target draws.
- MSAA resolves, depth conventions, and CPU-written movies: force immediate mode.
- Save/load, pause menu, cutscenes, mirrors, reflections, weapon overlays.
- Performance: bundle/secondary-buffer overhead and extra offscreen memory.

## Worktree

This work lives in the `deferred-ao` worktree so the broken present-pass AO experiments do
not contaminate `main`.
