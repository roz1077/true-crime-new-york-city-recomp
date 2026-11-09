# GTAO

Status: **implemented on D3D12 and Vulkan**. It replaces the earlier SSAO
kernel in the receiver AO path described in `deferred-renderer.md`.

## What is in

- GTAO kernel (`AoGtao` in `kAoHlsl`), after Jimenez et al. 2016 and Intel's XeGTAO: per slice,
  the highest occluder on each side, then the cosine-weighted visible arc.
- Slices × steps per side: **Half 3 × 6**, **Full 3 × 8** (`GTAO_SLICES`, `GTAO_STEPS`, set by
  `RenderHalfAo`).
- Strength is applied as a power on visibility, as in XeGTAO.
- Samples outside the radius are skipped rather than resetting that side's horizon. This keeps
  corner shadows intact behind characters.
- Everything upstream is shared with the earlier kernel:
  - receivers and the depth prepass;
  - the R32F view-distance target (clip w, no near/far guess);
  - texel-snapped centres and pixel-exact sample reads;
  - the 4x4 rotation pattern and the plane-aware 4x4 blur;
  - the `t4` multiply in receiver shaders.

## Defaults (tuned in game)

| Setting | Default | Override | Notes |
|---|---|---|---|
| Radius | **0.75** | `TCNYC_AO_RADIUS` | × 5% of distance. At 1.0 a wall half a body away darkened a character's arm; at 0.5 corners were too faint. |
| Strength | **1.0** | `TCNYC_AO_STRENGTH` | Power. 1 matches the game's soft baked lighting; 1.5 and 2 drew hard dark contact lines. |

## Settings decision: no SSAO/GTAO toggle

The F10 row stays **Off / Half / Full**, GTAO only. The earlier SSAO kernel is not offered as a
low-end fallback:

- **It is barely cheaper:** about 17 depth reads per AO pixel, against about 25 for GTAO at
  Half. The blur, the depth prepass and the deferred replay cost the same either way, and
  together they are a large share of the total.
- **It looks worse for the same cost:** this is the kernel that showed stripes and grain.
- **GTAO scales lower:** a 1 × 4 preset is about 9 reads, half the old kernel's.
- **Resolution is the bigger lever:** Half already cuts the AO work by 4×.

`TCNYC_AO_METHOD=ssao` stays as a hidden developer switch for A/B comparisons only. Remove it
(and `AoFactor`) once GTAO is settled.

## Steam Deck / low-end plan

1. Port the receiver AO path and GTAO to Vulkan (`platform/linux/backend_vk.cpp`); see
   `vulkan-ao-plan.md`. **Done:** the same shared shaders and the CPU scene display list are
   in the Vulkan backend.
2. Measure frame time on the Deck at Half, AO on against off, in the room, alley, street
   and menu scenes. **Initial Deck run:** in the draw-heavy attract scene at 1280x720,
   Half adds about 3–6 ms at 1x/2x and 8–12 ms at 4x. Use Half with 2x MSAA; see
   `vulkan-ao-plan.md` for the table.
3. Only if GTAO is too heavy there, add a **GTAO quality** setting, not a second method:
   - **Low:** 1 slice × 4 steps (Half only).
   - **High:** the current 3 × 6 at Half, 3 × 8 at Full.
   - Default to Low on the Deck if the numbers call for it.

## Still to do

- **Depth mip chain (XeGTAO's prefiltered depth):** only if profiling shows the far steps are
  slow.
- **Thin geometry:** railings and grates can over-darken. Use a thickness heuristic, or the
  visibility-bitmask variant (2023), if it shows up in play.
- **Characters receiving AO:** a wall within the radius darkens a character standing next to
  it. Radius 0.75 keeps that mild. A "cast but don't receive" option needs a reliable way to
  tell character draws apart first.
- **Validation:** same-angle Off / Half / Full captures in the scenes above, plus frame time at
  3840x2160 on Windows.
