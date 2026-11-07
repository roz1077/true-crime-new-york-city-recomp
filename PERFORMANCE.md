# First optimisation pass — 2026-11-02

Implemented three targeted changes, preserving the original presentation interval,48 kHz sample clock,
scene content and resource synchronisation semantics.

| Change | Validation |
| --- | --- |
| Gate render-thread profiling timers and atomic diagnostic counters when `XBE_PERF` is unset; use already-gated scopes for texture checks, GPU frame waits and swap timing. | The actual Draw/Wait regression harness counts zero diagnostic clock reads/counter updates while disabled. Enabled collection still works, with unchanged recorded commands and waits. The previous Draw implementation fails this check. |
| Replace each audible voice's per-block vector lane list with bounded stack scratch and a span. | Across64 scenarios, lane order, gains and ramped mix-bin output match the previous vector implementation byte for byte. Lane-list construction performs no heap allocations. The output rate and packet/DSP behavior are unchanged. |
| Persist portable compiled DXBC shader blobs between launches. | Cache keys cover generated source, entry point, shader profile, compiler-DLL contents, compilation flags and a format version. Cache reads validate length, key, magic, payload checksum and D3D reflection; misses or damaged entries compile normally. Writes use a process-specific temporary file and replacement. A real-DXBC regression checks round-trip/reflection, corruption, truncation, oversized headers, wrong keys, failed writes and recovery. |

The shader cache is under `shader-cache/v1/<compiler fingerprint>/` beside the executable.
`XBE_SHADER_CACHE=0` disables it. This caches bytecode, not adapter-specific D3D12 pipeline state.

## Measurements and their limits

Ran the cleaned baseline for125 seconds, then cold-cache and warm-cache builds for70 seconds each, with
the same ordinary controller recording and restored save backup. Captures, input recording and deterministic
RNG/timing overrides were disabled. Performance logging was enabled on each run. Saves were restored afterward.

| Run | Compiled shaders reported | Disk-cache hits | Steady median presented fps |
| --- | ---: | ---: | ---: |
| Previous cleaned build | 79 | 0 | 30.0 |
| Optimised build, empty cache | 74 | 0 | 30.0 |
| Optimised build, repeat launch | 9 | 74 | 30.0 |

The warm run demonstrably reused all74 blobs produced by the cold run and compiled9 additional variants.
Cache files after the runs:83 files,525100 bytes. No audio starvation messages were logged.

The ordinary replay reached different draw workloads as loading/cutscene timing varied (roughly222..646
draws in optimised samples versus roughly479 in the baseline's settled view). Consequently, renderer-time
medians **are not an apples-to-apples speedup measurement** and are deliberately not compared here. The
one-second compile averages also do not constitute individual-frame hitch percentiles. Cold caching still
pays compilation and disk-write costs; cache reuse improves subsequent encounters/launches.

The baseline's settled view used about5 ms of renderer CPU time per frame. Texture signature checks were
about0.1 ms/frame; there were no readback waits or upload overflows in that view. These observations do not
justify changing texture invalidation, dropping waits or growing upload rings for this scene. They also do
not establish performance on a busy street or the parking-ramp transition.

Local evidence is preserved under `logs/perf-baseline`, `logs/perf-cache-cold` and `logs/perf-cache-warm`.
Use `scripts/summarize_perf.py` to reproduce the summaries. Runtime counters do not provide full GPU timing.

## Validation and remaining work

- Full build and guest DLL relink pass. All7 regression checks pass: APC context, file references,
  retained attributes, viewport/scissor, profiler gating, audible lanes and shader cache.
- Cache-enabled cold and repeat runs boot and render; existing file/scissor regressions still pass.
  After the requested manual street/ramp/locker check, the user reported “All good” and authorised continuation.
- A controlled matched-scene frame-time/CPU/GPU capture is the next useful measurement. Keep the previous
  visual fixes as acceptance criteria. The generated cache can always be disabled when comparing.
- Pipeline-state persistence, stronger texture dirty tracking, render-state generations, readback changes,
  vertex metadata caching and overflow pooling remain candidates. They were not implemented speculatively
  when this baseline did not demonstrate a worthwhile bottleneck. See `OPTIMISATIONS.md` for constraints.

## Follow-up: cache invalidation and rejected vertex experiment

Added timing around all graphics-pipeline creation calls. In the automated precinct/elevator workload,
the largest one-second pipeline average was0.820 ms/frame during startup; settled creation time was zero.
That does not currently justify adapter/driver-specific pipeline persistence. The counters now include
the fixed-function blit/clear/depth-copy pipelines as well as ordinary draw pipelines.

Fixed a cache-fingerprint issue found by rebuilding: taking `&D3DCompile` could resolve to an import thunk
inside the executable, making an executable rebuild unnecessarily invalidate all shader blobs. The key
now explicitly fingerprints the DLL named by `D3DCOMPILER_DLL_W`. A native regression independently hashes
that DLL and checks the result; restoring the thunk-based lookup fails regression15. Source/profile/flags,
payload integrity checks and normal-compilation fallback remain unchanged. Existing older cache directories
are harmless; the corrected compiler directory is used from now on.

Evaluated removing the converted vertex path's temporary index-sequence vector. The prototype eliminated
one allocation per converted draw and matched the prior output in320 primitive/index/stride cases.
Three independent O2 microbenchmarks showed roughly4% faster assembly for the tested non-indexed case.
However, a similar precinct replay did not demonstrate an in-game improvement: median assembly time in
the40..70-second windows was1.85 ms before versus1.95 ms after, and renderer time was4.02 versus4.33 ms.
Draw counts were close (870 versus868), but loading/camera/world timing was still not perfectly deterministic.
**The prototype was therefore reverted.** Current vertex.cpp and vertex.h match their pre-experiment contents.
The prototype, test and captures remain under `.local/perf-pass2` and `logs/perf-pass2-*` for investigation.

Requested a user-driven busy-street profile with normal controls to replace the automated precinct workload.
The profiling build has replay/recording/deterministic overrides disabled. Its logs are preserved under
`logs/perf-live-street`; the user's actual driving progress must not be overwritten with the automated-run backup.

## Converted triangle strips — 2026-11-03

The Madison Avenue capture at `out/build/release/captures/222440` contains 1,800 triangle-strip draws
out of 1,975 logged draws. During driving, renderer CPU time reached about 12 ms per frame, including
5.1 ms for vertex assembly. Texture checks accounted for 0.36 ms in that sample, with zero upload
overflows and zero readback waits. The run reused 67 cached shaders by 134 seconds after rebuilding,
confirming that the corrected compiler fingerprint survives executable changes.

CPU-converted smooth triangle strips now use the raw path's existing native submission. Dense indexed
draws use the copied 16-bit indices with the existing base adjustment. Sparse draws keep the converted
vertices in draw order. Flat-shaded strips retain triangle-list expansion to preserve provoking-vertex
behavior. The regression check compares converted vertex bytes and primitive order across 320 cases
and separately checks the flat-shading fallback.

The focused O2 benchmark measured 166.6 ms for 50,000 converted draws with list expansion and 90.5 ms
with native strip submission. This measures the CPU assembly harness. The build and all eight regression
checks passed. The game started with normal controls and performance logging under `logs/perf-native-strips`.
In a workload-binned comparison of live street logs, the median assembly time fell from 3.86 to
1.96 ms/frame and median upload volume from 20.55 to 17.50 MiB/frame. Both samples held 30 fps.
The comparison used 36 prior and 83 new one-second samples with 1,650–2,150 draws, 170,000–240,000
vertices and at least 29 fps. The sample medians were 2,012 versus 1,939 draws and 222,000 versus
202,000 vertices. The routes and workloads differed, so these figures describe the sampled runs.
The user's W 50th Street capture `out/build/release/captures/012827/final.bmp` shows intact road,
vehicle and building geometry with no obvious clipping or winding defects in the frame.

## Render-state comparisons — 2026-11-03

Street samples generally spent about 0.8–1.0 ms per frame recording commands on the title thread.
`DeltaRange` now checks complete 256-byte groups before comparing individual 64-byte pieces.
Equal groups need one comparison. Changed groups and the final partial group use the existing
delta encoding.

The local O2 harness (`.local/bench_delta.py`) ran the actual recorder methods on 100,000 synthetic
draws per sample. Median time was 34.60 → 20.15 ms for unchanged state, 39.05 → 24.92 ms for sparse
changes and 143.33 → 148.55 ms when every piece changed. Across 500 generated update sequences,
including unaligned ranges and partial groups, the emitted bytes and saved state matched exactly.
These timings cover the comparison and encoding path in isolation.

In the subsequent live profile, 42 one-second samples in the same draw and vertex-count bands
had a median recording cost of 0.95 ms/frame, versus 0.92 ms/frame in the earlier 83 samples.
The routes had different draw and vertex medians, so this comparison leaves the in-game gain unresolved.

## Texture preparation — 2026-11-03

Texture binding now keeps the decoded descriptor in the draw's stage data. Shader constant setup
and frame capture read that descriptor. Mip-chain sizing runs only when the texture enters its existing
change-check path. Both the signature inputs and the upload decision use the same data as before.

## Beta hardening pass — 2026-11-07

This pass prioritised correctness and release readiness over speculative speedups. The only
renderer-path change with a performance intent is the Vulkan target-upload batch.

### Vulkan target uploads

CPU-written targets used to submit and wait twice per upload: once after the copy and once
after the following draw state. `UploadTarget` now records the copy into the current command
buffer in order with the draws already queued, unmaps the staging buffer, and keeps it in
`staging_pending_` until the frame fence retires it. This removes two full GPU waits per
CPU-written surface and avoids creating and destroying a staging buffer inside the critical
path.

Validation: 60-second scripted Deck gameplay at 4x MSAA + SMAA + bloom + DOF reported
2,940 frames (~49 fps), ~676 draws/frame, 0 VUIDs, 0 audio underruns/starvation and no device
loss; the in-world capture was correct. The frame rate stayed in the same ~47–50 fps band as
the unbatched build, so the benefit here is fewer synchronisation points and less allocation
churn, not a measured present-rate increase.

### Target lifetime and HUD correctness

MSAA and render-scale changes no longer call `DestroyTargets()`. `EnsureTarget` rebuilds a
target lazily only when it is next bound as a render target with a different size or sample
count, so a HUD slot that is only being sampled keeps its contents across the change. This
mirrors the Windows fix for the static red "FS" placeholder and was verified with a live
4x-to-8x MSAA change on the Deck (0 VUIDs, intact HUD).

### D3D12 and audio

The D3D12 backend now uses one command list per frame slot, waits on its own fence with a
bounded timeout and device-removal detection, reuses readback buffers per frame, and caps the
CPU shader-blob cache. These changes target TDRs, unbounded memory growth and frame-to-frame
allocation rather than raw throughput. On Linux SDL3, audio is produced through a small
jitter buffer; a 60-second Deck run logged zero underruns and zero starvation events. The
48 kHz clock, 60 Hz simulation and original presented-frame cap are unchanged.

### Acceptance

`scripts/run_checks.py` passes all 18 checks, including the save-flush and PulseEvent
regressions. A clean Windows Release build reached the title and gameplay on an RX 7900 XTX at
8x MSAA (~2,000 draws, ~50 fps) with no device loss, and a clean Linux Release build ran the
Deck checks above. The shipped Windows zip was extracted fresh and reached gameplay on the
same machine (~60 fps, 1,500–1,800 draws at the default 2x MSAA); its internal SHA256SUMS
verify. Several manual checks (mouse pitch in both modes, DSound callback
re-entrancy, fullscreen rollback, Alt+F4, focus/controller handoff, full-disk saves, Deck
suspend/resume and multi-hour soak) remain for the maintainer; see `DECK_TEST.md` and the
open-items list in `NOTES.md`.
