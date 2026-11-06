# Vulkan renderer parity plan

Status: audit complete; Phases 1-6 and the Phase 0 cleanup are implemented and
validated. Phase 7 (Deck validation, performance, packaging) remains.
Scope: make `platform/linux/backend_vk.cpp` feature-complete against the Windows
D3D12 renderer in `sdk/runtime/gpu/d3d12.cpp`.

Windows and Linux stay separate. This plan does not change `d3d12.cpp`, `ext.h`,
`texturedumper.cpp`, or the Windows executable. The shared files that both
backends already use (`shaders.cpp`, `texture.cpp`, `vertex.cpp`, `state.h`) are
read-only reference here; any new behaviour is implemented in the Linux backend
or in new Linux-only files.

## Executive summary

The stock game's core draw path is already at parity: clear, draw, copy, present,
sync, fixed-function and programmable shaders, blend/depth/stencil/cull state,
texture decode/upload, mips, BC, palettes, cube/volume/depth textures, samplers,
render-target feedback, target-address aliasing, occlusion reports, F3 capture,
gamma, PC contrast and dither.

What was missing was almost entirely the optional PC graphics layer plus a few
edge cases. Those are implemented now: render scale and stretch, MSAA and its
colour/depth resolves, SMAA, bloom, depth of field, the widescreen UI squeeze,
texture replacement/dumping, console point/line expansion, point sprites, the
`UnitTexcoords` sampler clamp, late-lock delivery (`DeliverLate`/`RecordLate`)
and the guest sampler border colour through `VK_EXT_custom_border_color`.

Still open:

1. HDR/range targets, only if a scene ever reports a range other than 1.
2. ~~The no-BC fallback~~: implemented; BC1/2/3 guest and replacement textures are
   decoded to BGRA8 when the device has no `textureCompressionBC`.

Order matters: render scale and MSAA change the shape of every render target, so
they come before SMAA/bloom/DOF, which operate on those targets. Texture packs and
point/line expansion are independent and can be scheduled later.

## How the audit was done

- Compared the `gpu::Backend` interface (`sdk/runtime/gpu/backend.h`) against the
  overrides in both backends.
- Walked the Windows target lifecycle (`CreateTargetResource`, `ApplySettings`,
  `ResolveColor`, `ResolveDepth`, `ApplySmaa`, `Present`, `Blit`) and compared it
  with `VulkanBackend::EnsureTarget`, `RenderPassFor`, `CreatePipeline`,
  `EnsureTexture`, `EnsureSampler`, `FillConstants` and `Present`.
- Compared settings use (`app::Settings` in `sdk/runtime/app/app.h`) with the
  Linux backend's reads of `app::Snapshot()`.
- Checked every shader hook the Windows path relies on (`ShaderHooks`,
  `kTexWide`, `spreadtap`, `rtshift`, `QuadHlsl`, point/line shaders).

## Parity matrix

| Area | Windows D3D12 | Vulkan Linux | Verdict |
| --- | --- | --- | --- |
| Init/present/clear/copy/sync/gamma | yes | yes | parity |
| Fixed-function and programmable VS/PS | yes | yes | parity |
| Blend factors/ops, constant colour/alpha | yes | yes | parity |
| Depth/stencil/cull/fill/depth bias | yes | yes | parity |
| Fog, alpha test, two-sided, flat shading | yes | yes | parity |
| Texture formats, mips, BC, palette, swizzle | yes | yes | parity |
| Cube/volume/depth texture sampling | yes | yes | parity |
| Render-target feedback and address aliasing | yes | yes | parity (aliasing fixed this pass) |
| Sampler: filters, LOD bias, aniso, shadow | yes | yes | parity |
| Sampler: guest border colour | yes | yes | parity when `VK_EXT_custom_border_color` is present |
| Sampler: `UnitTexcoords` clamp | yes | yes | parity (small all-unit-UV draws clamp wrap) |
| Occlusion/visibility reports | yes | yes | parity (added this pass) |
| Late locks (`DeliverLate`/`RecordLate`) | yes | yes | parity (finished-copy ring) |
| Render scale + stretch | yes | yes | parity |
| MSAA + colour/depth resolve | yes | yes | parity (depth MAX, SAMPLE_ZERO fallback) |
| SMAA 1x | yes | yes | parity |
| Bright-pass bloom | yes | yes | parity |
| Depth of field | yes | yes | parity (needs MSAA depth, as Windows) |
| Widescreen UI squeeze / picture mode | yes | yes | parity |
| Texture replacement / dumping | yes | yes | parity (same keys and names, portable loader) |
| Console point/line expansion | yes | yes | parity (geometry shader quads) |
| Point sprites on stage 3 | yes | yes | parity (stage 3 rewritten to stage 0, sprite GS) |
| HDR/range targets and resolve | yes | no | missing, only if a title uses it |
| F3 frame/target/texture capture | yes | yes | parity (extended this pass) |
| PC contrast + ordered dither | yes | yes | parity |
| Anisotropic filtering setting | yes | yes | parity |

## Phase 0 - small correctness and cleanup

Status: the exploratory `TCNYC_PROBE*`, `TCNYC_SKIP_TEX`, `TCNYC_SKIP_LIGHT`
and `TCNYC_NO_MIPS` code is removed; the supported diagnostics are
`TCNYC_TRACE`, `TCNYC_TRACE_AT`, `TCNYC_TRACE_EVERY`, `TCNYC_TRACE_PRESENT`,
`TCNYC_DUMP_TARGETS` and `TCNYC_DUMP_TEX`. The `UnitTexcoords` clamp, the guest
sampler border colour (through `VK_EXT_custom_border_color`) and late-lock
delivery are implemented. The no-BC fallback is implemented; only HDR/range
targets remain open (see the remaining list).

Goal: remove the remaining edge cases and the experimental diagnostics before
adding large features.

- Sampler border colour: done - `Color(t[kTssBorderColor])` is passed through
  `VkSamplerCustomBorderColorCreateInfoEXT` when the driver offers
  `VK_EXT_custom_border_color` with `customBorderColorWithoutFormat`; transparent
  black is the fallback.
- `UnitTexcoords` clamp: done - the Windows `clamp` decision is computed from the
  draw's texture coordinates and passed to `EnsureSampler`.
- No-BC fallback: done - when `bc_supported_` is false, BC1/2/3 levels (guest and
  replacement) are decoded to BGRA8 and uploaded as `VK_FORMAT_B8G8R8A8_UNORM`.
- Late locks: done - `DeliverLate`/`RecordLate` keep a three-slot ring of
  finished copies per surface, recorded in order with the draws and tagged with
  the draw submission that carries them; a `NOOVERWRITE` lock delivers the newest
  finished copy into guest memory without waiting. A self-test compared the
  delivered bytes with a normal read-back of the same target and matched exactly.
- Remove the exploratory `TCNYC_PROBE*`, `TCNYC_PROBE_BAND`, `TCNYC_SKIP_TEX`,
  `TCNYC_SKIP_LIGHT` and `TCNYC_NO_MIPS` code from `backend_vk.cpp`. Keep
  `TCNYC_TRACE`, `TCNYC_TRACE_AT`, `TCNYC_TRACE_PRESENT`, `TCNYC_DUMP_TARGETS`
  and `TCNYC_DUMP_TEX` as the supported diagnostics.
- Re-run Vulkan validation to 0 errors and package.

Files: `platform/linux/backend_vk.cpp`.
Acceptance: validation clean; no visual change in the stock scenes; late-lock
stress (movies + gameplay) has no stalls or corruption.

## Phase 1 - render scale and stretch

Status: implemented. Targets carry guest and host extents, are created at the
scaled size, scale the viewport/scissor, mark sampled targets `kTexWide` with the
shared `spreadtap` values, live-rebuild when the percentage/aspect changes, and
scale uploads/readbacks to and from guest memory. Widescreen UI squeeze is still
Phase 4, and MSAA/image quality is Phase 2.

Goal: make the Graphics menu's render percentage and aspect actually change the
rendered resolution, without changing the guest's back-buffer size.

Windows reference: `CreateTargetResource`, `ApplySettings`, `SetViewportScissor`,
`FillSurfaceConstants`, `Blit`.

Tasks:

1. Extend the Vulkan `Target` with guest size and host size:
   `gw`, `gh` (console surface), `w`, `h` (scaled host image), `want_scale`,
   `want_stretch`. Keep `data`, `pitch`, `guest_format` and the readback path at
   guest semantics.
2. Create target images at `round(gw * scale * stretch) x round(gh * scale)`.
   Scale depth targets the same way. All guest byte sizes/pitches remain the
   console's; `ReadTarget`/`UploadTarget` resolve or scale down to guest size.
3. Pass the host extent to the viewport/scissor and to the pipeline's
   rasterization. Keep guest texture coordinates in `FillConstants` and set the
   `rtshift`/`tscale` values the shared shaders already expect for scaled
   targets.
4. When a render target is sampled as a texture, set `kTexWide` and the
   `DrawConstants::spread[stage]` value so `spreadtap` averages the host texels
   that cover one console texel. This is the same contract the Windows backend
   gives the shared shader.
5. Present: the source rect covers the guest frame inside the scaled target;
   the destination is the swapchain with the existing letterbox/pillarbox
   rules. Do not stretch console 4:3 content when a widescreen aspect is
   selected.
6. Live apply: in `Present`, compare `app::RenderScale(guest_h)` and
   `app::RenderStretch(guest_w, guest_h)` with the current values. On change,
   flush, wait idle, resolve/read back any live targets, drop the target cache
   (not textures) and rebuild at the new size, like `ApplySettings`.
7. Readback/lock correctness: a scaled/MSAA target read for the guest must be
   resolved to guest size and guest format before `WriteGuestPixels`.
8. Log the chosen scale/stretch once per change.

Files: `platform/linux/backend_vk.cpp` (`Target`, `EnsureTarget`,
`CreateTargetImage`, `RenderPassFor`, `FramebufferFor`, `Present`,
`FillConstants`, `ReadTarget`, `UploadTarget`).
Acceptance: 50/75/100/150/200% render scale on the Deck; UI remains crisp;
guest locks/saves/movies are unchanged; performance scales roughly with pixels;
0 validation errors.

## Phase 2 - MSAA

Status: implemented. Targets allocate a multisampled image plus a single-sample
resolve image; the render pass resolves colour and depth (MAX when the driver
offers it, SAMPLE_ZERO otherwise), sampling/readback/present use the resolved
image, pipelines key on the sample count, clear pipelines pick the matching
render pass and occlusion counts divide by sample count. The requested count is
clamped to the best count at or below it that both the colour and depth formats
support. One root cause worth remembering: the VkInstance used to be created for
Vulkan 1.1, and the loader hides 1.2+ device entry points from
`vkGetDeviceProcAddr` in that case, so `vkCreateRenderPass2` resolved to a null
trampoline; the instance now negotiates up to 1.3.

Goal: make the Graphics menu's 2/4/8 sample options work.

Windows reference: `SupportedMsaa`, `CreateTargetResource` (`SampleDesc`),
`ResolveColor`, `ResolveDepth`, pipeline `SampleDesc`, `SamplesPerPixel`.

Tasks:

1. Query `framebufferColorSampleCounts` / `framebufferDepthSampleCounts` and
   clamp the requested count, logging when unavailable.
2. Extend `Target` with `samples`, `resolved_image`, `resolved_view`.
   Create multisampled images with `VK_IMAGE_TILING_OPTIMAL` and
   `VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT`
   (colour) or depth/stencil attachment (depth).
3. Render passes: use `pResolveAttachments` for colour when the target will be
   sampled/read/presented, or `vkCmdResolveImage` at the end of the pass. Depth
   resolves to a single-sample `VK_FORMAT_D24_UNORM_S8_UINT`/D32 image for
   shadow/DOF sampling.
4. Pipelines: `rasterizationSamples = target samples`; update the pipeline key
   so the same state does not collide across sample counts.
5. Occlusion query counts are samples: divide by the sample count (the Windows
   `SamplesPerPixel` behaviour) so visibility reports remain console pixels.
6. Sampling a multisampled target: use the resolved image, not the MS image.
7. Live apply: same flush/drop/rebuild path as render scale.
8. Readback: resolve/downsample to guest size before writing guest pixels.

Files: `platform/linux/backend_vk.cpp`.
Acceptance: 2x/4x/8x on RADV; no resolve validation errors; text/UI stable;
occlusion-driven scenes do not pop; performance cost expected.

## Phase 3 - SMAA 1x

Status: implemented. `backend_vk.cpp` compiles the reference SMAA 1x shader from
`sdk/third_party/smaa` with shaderc (the metrics go in as push constants; the
reference's `register(tN)/register(sN)` names are mapped to bindings 2..4 and
6..7 by the existing `InjectBindings`), uploads the area/search lookups once
through the frame upload ring, and runs edge/weights/blend into three
frame-sized images before the present pass samples the result. Targets are
rebuilt with the frame size, and MSAA/scale changes simply feed the resolved
scene into the first pass.

Goal: apply the Windows SMAA 1x high-preset pass to the finished frame.

Windows reference: `ApplySmaa`, `EnsureSmaa`, `SmaaSource`, the three SMAA
passes, `third_party/smaa/{AreaTex,SearchTex,SMAAShader}.h`.

Tasks:

1. Add frame-sized edge, weight and output images; upload the area/search
   lookup textures once.
2. Port the four SMAA shader stages to the Vulkan shader path (shaderc can
   compile the same HLSL; the Windows code already defines the
   `SMAATexture2D`/`SMAASample*` macros and targets SV_Target).
3. Run luma-edge, blending-weight and neighbourhood-blend passes in a
   dedicated command buffer before present. Keep the scene target readable.
4. Present samples the SMAA output instead of the scene when enabled.
5. Handle resize/render-scale/MSAA changes by recreating the SMAA images.
6. Live toggle from the Graphics menu.

Files: new `platform/linux/smaa_vk.*` (or a section of `backend_vk.cpp`) plus
shared third-party headers; `backend_vk.cpp` Present.
Acceptance: edge quality matches the Windows build on the same shot; no halo
on UI/text; toggle works live; 0 validation errors.

## Phase 4 - bloom, depth of field, widescreen UI squeeze

Status: implemented. The Vulkan present shader has the same bright-pass bloom as
Windows (4-tap box, 0.94 threshold, 9-tap gather, 1.2 gain), the 13-tap
depth-weighted distance blur (same 40..65 world-unit ramp, driven by the resolved
MSAA scene depth and the draw's near/far w range), and the UI squeeze is applied
through `DrawConstants::tscale[2]` exactly like `UiSqueeze` (16:9 UI centred in a
wider frame; full-width textured pictures squeezed, untextured passes left
full-width). As on Windows, DOF only engages when MSAA depth is available.

Goal: finish the PC present finish.

Windows reference: `Bloom()`, the present `Blit` shader, `dof_depth_`,
`dof_near_/far_`, `UiSqueeze`, `SetViewportScissor`, `picture`.

Tasks:

1. Bloom: port the single-pass bright-pass approximation from the Windows
   present shader to the Vulkan present HLSL. Add `bloom` and source size
   uniforms; toggle from `app::Snapshot().bloom`.
2. DOF: sample the resolved scene depth in the present shader (the same
   distance blur as `d3d12.cpp`). Track near/far from the draw constants or the
   projection; toggle from `app::Snapshot().dof`.
3. Widescreen UI squeeze: compute `squeeze = (4/3) / AspectRatio()` for HUD/UI
   draws exactly like `UiSqueeze`, and apply it in the viewport/scissor path.
   Leave full-width/full-height viewports unsqueezed.
4. Picture presentation: when the front buffer is CPU-written (movie/legal),
   present it at 16:9 on a wider frame with the same picture rules as
   `D3D12Backend::Present`.
5. Keep PC contrast and dither after the post chain, matching Windows ordering.

Files: `platform/linux/backend_vk.cpp` (`kPresentHlsl`, `Present`,
`SetViewport`/scissor path), plus a small depth-source hook for DOF.
Acceptance: A/B against the Windows build for the same shot; bloom/DOF/UI
squeeze toggles live; movies keep their aspect; 0 validation errors.

## Phase 5 - texture replacement and dumping

Status: implemented as the portable `platform/linux/texturedumper_linux.{h,cpp}`.
It uses the same texture key (level 0 bytes + size + format, plus the palette for
P8), the same `textures/replacement` and `textures/dumps` folders and the same
file names as Windows, so a pack made for the Windows build works unchanged.
PNG is read and written with the vendored stb headers; DDS (DXT1/3/5, DX10
BC1/2/3/7 and uncompressed BGRA/RGBA) is parsed and written natively; DXT
blocks are decoded in-process for PNG dumps. Images load off the render thread;
the pack folder is polled with a directory signature and re-listed after a
short settle instead of a native file watch. Replacement textures are cached per
key and shared by every guest texture with the same bytes, and are released
(the next draw rebuilds them) when the pack changes.

Goal: match `textures/replacement` and `textures/dumps` on Linux.

Windows reference: `gpu/texturedumper.h/.cpp`, `LatchPackKey`, `LatchPackImage`,
`CreatePackTexture`, `ReleasePackTextures`, `texturedumper::Update`.

Tasks:

1. Make a portable Linux texture pack module. Replace WIC/DDS-only loading with
   a Linux image loader (stb_image for PNG; a small DDS reader, or keep DDS
   support behind a tiny parser). The key is the existing hash of level 0
   bytes + size + format (and palette for P8).
2. Add the module to the Linux build (today `texturedumper.cpp` is excluded).
3. In `EnsureTexture`, after the guest texture key/signature is known, look up
   the replacement image and, when found, upload it instead of the guest
   texture. Keep a `textures_`-parallel cache by pack key and swap it like the
   Windows `tx.swap`.
4. Invalidate the pack cache when `texturedumper::Generation()` changes.
5. Dumping: when enabled, write each texture once as PNG or DDS, matching the
   Windows naming/format.
6. Wire `custom_textures` and `dump_textures` from `app::Snapshot()`.

Files: new `platform/linux/texturedumper_linux.*` (portable), `texture` cache in
`backend_vk.cpp`, `CMakeLists.txt`.
Acceptance: a replacement PNG/DDS for a known texture shows in game; dumps are
readable and match the Windows hash/name; no re-upload churn; 0 validation
errors.

## Phase 6 - points, lines, point sprites, HDR

Status: console point/line expansion and point sprites are implemented. Point
and line pipelines get a geometry stage compiled from the shared `PointHlsl` /
`LineHlsl` sources, so each console point/line covers its console pixels at any
render scale (the glslang HLSL parser crashes on a geometry entry point that
copies `SV_ClipDistance0`, so the geometry stage's copy of `VOut` drops that
member; the vertex shader still writes it). A program's point list with point
sprites enabled is rewritten so stage 3 becomes stage 0 and a sprite geometry
stage expands each point to the program's point size. When the device has no
geometry shaders the previous native point/line rendering is used. HDR/range
targets are not needed by this title's scenes (range stays 1); the task stays
here as the hook if a scene ever reports otherwise.

Goal: remove the remaining geometry/pipeline gaps.

Tasks:

1. Console point/line expansion: implement a CPU expansion path (duplicate
   vertices/indices into a small scratch buffer) or a vertex-shader expansion
   using `SV_VertexID`. Do not depend on geometry shaders, which crashed on
   llvmpipe. Match the Windows `ExpandPoints`/`ExpandLines` sizes and the
   console pixel centre rules.
2. Point sprites: implement the Windows `SpriteOnStage3`/`SpriteStageZero`
   special case for point-list draws with a programmable VS, no PS, point
   sprite enable and active stage 3.
3. HDR/range targets: if a title uses a target `range` other than 1 (the
   Windows `Target::range`/resolve path), add float/half-float target formats
   and the range resolve. Only implement when a scene actually needs it; log
   the first non-1 range and treat it as a parity task.
4. Border colour and `UnitTexcoords` clamp if not finished in Phase 0.

Files: `platform/linux/backend_vk.cpp`, `vertex.cpp` only if a shared
declaration change is unavoidable (prefer Linux-only).
Acceptance: console point/line size and sprite scenes match Windows; no crash on
llvmpipe; 0 validation errors.

## Phase 7 - validation, performance and Deck

1. Validation matrix: RADV (Deck), llvmpipe, a desktop NVIDIA/AMD driver.
2. Regression scenes: title, opening cutscene, gameplay on foot, driving,
   menus, movies, legal screens, save/load.
3. Graphics-menu matrix: render scale, MSAA, SMAA, bloom, DOF, aniso, frame
   cap, vsync, FOV/aspect, texture packs, F3 capture.
4. Performance: CPU/GPU timing with `TCNYC_PERF=1`; compare 100% vs scaled;
   keep 30/60 FPS targets on Deck.
5. Package and document the new options in `README-LINUX.md` and
   `LINUX_PORT_PLAN.md`.

## Suggested order and rough size

| Phase | What | Rough size | Depends on |
| --- | --- | --- | --- |
| 0 | sampler edges, no-BC fallback, late locks, cleanup | S | - |
| 1 | render scale + stretch | M/L | 0 |
| 2 | MSAA + resolves | M | 1 |
| 3 | SMAA | M | 1, 2 |
| 4 | bloom + DOF + UI squeeze | M | 1, 2, 3 |
| 5 | texture packs/dumps | M | 0 |
| 6 | points/lines/sprites/HDR | S/M | 0 |
| 7 | validation/perf/package | S | all |

Recommended first step: Phase 0 + Phase 1, because render scale is the
foundation for MSAA and the post chain, and the cleanup removes the experimental
code before the target struct changes.

## Non-goals

- Do not modify `sdk/runtime/gpu/d3d12.cpp`, `ext.h`, `texturedumper.cpp`, or the
  Windows executable for these features.
- Do not introduce shared-code changes unless a Linux-only equivalent is
  impossible; if a shared change is unavoidable, keep it behaviour-neutral on
  Windows and call it out explicitly.
- Do not port DirectX-only APIs (WIC, DXGI, D3D12 descriptors) into the Vulkan
  path; use Vulkan-native equivalents.

## Known open issue parked by the user

The hard-edged road brightness in some daytime shots is not part of this plan.
It is parked; the evidence so far points at the light/reflection chain, not the
road texture, and the next step is a Deck capture with `TCNYC_DUMP_TARGETS=1`.
This plan deliberately does not depend on solving it.
