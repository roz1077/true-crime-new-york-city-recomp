# True Crime: New York City (original Xbox) — static recompilation to PC

Investigation log, newest first within each day. Later entries supersede earlier hypotheses; sections marked
superseded or fixed are kept as evidence. Graphics options research is in `GRAPHICS_NOTES.md`, performance
measurements in `PERFORMANCE.md`, and the build, checks and diagnostics in `DEVELOPMENT.md`.

## Current state (2026-11-03)

- The game boots, plays its movies and menus and reaches gameplay. The parking-ramp lockups and crash were
  caused by leaked file-object references (fixed in the runtime, see 2026-11-02); the user drove past the old
  failure point. The locker preview cutoff (scissor) is fixed.
- The in-game Graphics menu (F10) and the native Options > Display rows offer resolution, aspect ratio, render
  scale, field of view, anisotropic filtering, MSAA, SMAA, V-Sync, a 30 or 60 FPS limit and an FPS counter.
  60 FPS gameplay and real-time cutscenes keep the game's tick rate. See `GRAPHICS_NOTES.md`.
- `TrueCrimeNYC.exe` opens an ISO picker before the game starts; `release.bat` packages a release.
- The modified SDK is tracked in this repository under `sdk/`. `patches/xboxxberecomp-local.patch` is the same
  change set as a diff against upstream XboxXBERecomp `f7b13fd` (see `DEVELOPMENT.md`).
- Seventeen regression checks (`scripts/run_checks.py`) cover the runtime fixes and the menu rules.
- Mouse and keyboard play is supported: raw mouse aim on foot and in vehicles, rebindable
  keyboard actions, digital WASD with normalized diagonals, and automatic Xbox/PlayStation/
  keyboard-mouse prompts (see the 2026-11-05 mouse transition follow-up and the 2026-11-03
  mouse aim section).
- Open: traffic variety after free roam (below), draw distance, HDR and ambient occlusion.

## PC default keybinds for keyboard/mouse (2026-11-05)

- The keyboard/mouse defaults now follow the PC version's mapper: WASD movement, left mouse
  fire, middle or right mouse precision aim, R reload, E use/action, G climb, Space jump or
  handbrake, C crouch, Shift target lock, Q badge or siren, F fire, Num 7 / Button 3 / Ctrl
  light attack, Num 9 / Button 4 heavy attack, + grapple, wheel weapon or camera, and Esc
  pause/map.
- `use_key` now defaults to E (69) and `climb_key` to G (71). `heavy_attack_key` defaults to 0,
  which disables the custom heavy key; Num 9 and Button 4 still select heavy attack. Existing
  settings files keep their own values, so an old `heavy_attack_key = 70` keeps F on heavy
  attack and suppresses F as a fire alias.
- Middle mouse feeds the aim trigger, the side mouse buttons feed the light/heavy pad buttons,
  and wheel notches become D-pad taps (weapon select on foot, camera in a vehicle). The keyboard
  V right-stick-click duplicate was removed; Shift is target lock.

## Horizontal mouse movement through transitions — follow-up (2026-11-05)

- The user reported the earlier fix was incomplete: horizontal mouse movement still failed during
  vehicle entry/exit, area transitions, and controller-to-mouse switching. The 2026-11-03 mouse
  section below records the previous implementation, not verified resolution of those cases.
- The shared camera hook injected yaw before controller methods, but unarmed/transition
  controllers can replace that yaw before calling the shared orientation builder `0x1FD40`.
  The unarmed correction after `0x2E200` returned was too late for the view already built.
  It also assumed the controller always replaced yaw; branches that retain it could stack the offset.
- Replaced the unarmed offset/cache with a heading scoped to the current gameplay camera update.
  Inline camera builders still receive the pre-step yaw; the shared builder restores that heading
  immediately before consuming it. No post-update offset is reapplied. Other cameras and calls
  outside the update retain their own yaw.
- The input fallback discarded raw counts collected within the last heartbeat's 100 ms grace
  period. Those counts now join the existing bounded transition buffer, including when no pad
  poll ran during the load. The existing 150-count cap and 20-count release steps remain.
- A single raw count selects mouse input even when the camera consumes it before the next pad
  poll. Actual controller input releases the recent direct-aim hold and pending transition counts;
  vehicle aim reseeds from the current chase heading on the next mouse sample. UI/focus changes
  clear the hold, and leaving the vehicle controller invalidates its cached heading.
- `scripts/test_mouse_aim.py` compiles the actual hooks/input routines with deterministic camera,
  clock and device fixtures. `--baseline` reproduces failures against HEAD's previous code.
  Covers same-update yaw, missing targets, changing camera modes, inline builders, isolated
  camera calls, grace-period/stale counts, one-count handoff, vehicle handoff and focus/UI reset.
  All 17 regression checks pass. Release build: `logs/codex-mouse-transition-build.log`.
  **User confirmed the fix in live gameplay on 2026-11-05**, following the reports of
  vehicle/area transitions and controller-to-mouse switching.

## Scripted voice-over through the car radio (2026-11-04)

- The mission dialogue played over the car radio was silent while the radio ducked. The
  `.wma` files were opened and read, and the WMA decoder produced non-zero PCM, so neither the
  file nor the decoder was at fault.
- The title's voice-over slots are mono **3D** `CDirectSoundStream`s. The host positional mixer
  attenuated them to silence because the scripted voice never set a usable minimum distance.
  The PCM was already being submitted to the stream (`RMS 800..900` in the audio trace).
- The PC build uses FMOD 3.74, which attenuates a 3D sound by `minDistance / distance` and
  plays it at full volume inside the minimum distance. The PC title also clamps the values it
  passes to `FSOUND_3D_SetMinMaxDistance`: the minimum is lifted to at least `1.0` and the
  maximum held near `1e9` (`tcnyc.exe` `0x64EDDC..0x64EE5A`). Without that clamp a stream with
  `min = 0` computes `0 / distance`, which is the silence heard here.
- `Attenuation` now applies the same PC clamp and `min / distance` law to **every** 3D voice,
  buffer or stream, mono or stereo. The console-only DS3D rolloff curve
  (`IDirectSoundBuffer_SetRolloffCurve`, a 7-point curve the Xbox build sets on the buffer)
  has no PC equivalent and no longer changes the level. The radio still ducks
  `0.75 -> 0.19` during the line and returns to `0.75` afterwards.
- The unmodified Xbox release leaves the scripted stream's minimum distance at 0, so the same
  `min / distance` law silences the line there as well; the PC build's clamp is what keeps it
  audible. This fixes that official-release bug on the port.
- PC reference: `Data\NPCVO\INT_M4\M4_CarTalk_A.WMA` is bit-identical between the Xbox assets
  and the PC install; the PC `Cine` tracks are about 1 dB louder than the Xbox ones. The
  remaining level difference is mix/bus gain, not the asset. The temporary host-side
  Media Foundation fallback was removed.

## Menu previews follow the centred UI (2026-11-03)

- The character outfit (Street Flack) and car selection previews are 3D draws into a
  sub-viewport of the main target: `358 216 244 264` in the locker and `260 170 380 271` in
  the garage (640x480 guest frame). The surrounding 2D panel was squeezed into the centred
  16:9 region on a wider frame, but the 3D pass kept its guest viewport, so the model drifted
  to the right of its panel.
- `D3D12Backend::UiSqueeze` now returns the centred-UI squeeze for a non-pretransformed draw
  when the bound target is the main guest target and the viewport is smaller than the frame in
  both dimensions. `SetViewportScissor` already applies that squeeze to the viewport and
  scissor, so the preview and its 2D panel share the same transform. Full-screen 3D (gameplay
  and menu backgrounds) stays unsqueezed, and the 256x256 preview textures keep their own
  layout because they are composited through a squeezed 2D quad.
- Verified with F3 captures of both menus (`captures/223700`, `captures/223707`) and the user
  confirmed both previews are centred on the ultrawide display.

## Input handle and menu pointer hardening (2026-11-03)

- XInput handles now carry a generation. `XInputGetState` and `XInputGetCapabilities` release
  the port lock while polling, so a guest handle can be closed and its address reused during
  that window; a generation mismatch now reports `ERROR_INVALID_HANDLE` instead of touching the
  new handle. `XInputClose` only frees a handle it actually removed, and `XInputOpen` checks the
  allocation.
- `Pad` compares by value instead of `memcmp`, so padding bytes cannot make an unchanged report
  look new and bump the XInput packet number.
- The two stick-click keys are shared constants used by both the keyboard input map and the
  keyboard-mouse prompt glyphs. Raw mouse input is discarded while the Graphics menu owns the
  pointer, and focus transitions use an atomic edge so the log is emitted once.

## Mouse aim, keyboard movement and controller prompts (2026-11-03)

The unarmed offset and transition claims here describe the earlier attempt, superseded by the
2026-11-05 follow-up above. The user confirmed the issue remained after this implementation.

- Mouse look is no longer folded into the right stick. `WM_INPUT` raw counts are applied at the
  PC WidescreenFix scale (`fMouseSens 4.0 * RawInputMouse 1.0 / 1000 = 0.004 rad/count`), with the
  game's invert-X/invert-Y options and the `mouse_sensitivity_percent` /
  `mouse_vertical_sensitivity_percent` settings.
- The shared camera update `sub_0002E200` is the injection point for every mode except vehicle
  (13) and unarmed (23). It reads the active controller from `camera+0x174`, checks that
  `controller+0x34` owns the gameplay camera, and writes yaw/pitch before the controller's
  virtual methods run. Per-controller hooks miss the armed modes because those bodies run after
  the camera pre-step has already rebuilt the angles.
- Unarmed `sub_0002D230` (mode 23, vtable `0x2D3A38`) is target-driven: it replaces `0x348B20`
  with the followed target's forward angle (`atan2(row+0x60.z, row+0x60.x)`), and that mapping
  has a negative sign. The raw yaw is therefore kept as `g_unarmed_yaw` and added back after the
  controller; the target-derived base is cached while a transition clears `camera+0x17C`, so the
  offset neither stacks nor resets. On foot the PC pitch range is -89..+60 degrees; in vehicle
  it is -89..+10. `sub_0001FD40` is the shared orientation builder for the vehicle path and
  negates the pitch accumulator, so that path keeps its own sign and clamp. The controller's
  pitch limits are widened only for the direct update and restored afterwards.
- The vehicle path holds absolute yaw/pitch while the mouse is active and hands back to the
  game's chase camera after 5 s without a sample, matching the PC fix. If the look controller
  stops running, input falls back to the right stick after 100 ms.
- A zone load or car exit stops the camera update entirely: `sub_0002E200` is not called for any
  camera object and the game does not read the look stick during that window. The fallback curve
  still matters because the look controllers cube the stick, so a linear mouse sample sits near
  zero. The fallback now applies the inverse curve with a 45% floor, ignores one-count jitter, and
  holds the sample for 150 ms so a flick behaves like a held pad. Movement made while no camera
  is updating is also kept in a bounded 150-count (per axis) buffer and released in 20-count
  steps over the first camera frames after the transition.
- The cursor is hidden and confined to the focused game window; it is released while the F10
  Graphics menu owns the pointer and on focus loss.
- Keyboard movement is digital full-speed WASD with normalized diagonals; a real controller keeps
  its analog movement. Use, climb, reload and heavy attack are rebindable in `settings.toml`.
- Inline controller glyphs switch automatically between Xbox, PlayStation and keyboard/mouse
  styles; `Capture Controller Prompts.cmd` records the prompt atlases for checking.
- The named-object/child-node table detaches both `[record+0xA8]` and `[child+0xC0]` at world
  reset and table rebuild (`scene_nodes.cpp`), fixing the crash that followed the earlier mouse
  work.

## Traffic variety and Copland (2026-11-04)

The Xbox glitch guide reports that ordinary traffic can dwindle to cruisers
and parked sedans after returning to free roam. It is reproducible in normal
free roam in the port. See the
[Xbox glitch guide](https://gamefaqs.gamespot.com/xbox/925118-true-crime-new-york-city/faqs/40332).

The PC v1.0 build was compared directly: its selector (`0x402E00`), RNG,
spawn weights and `SpawnInfo`/`Vehicle` data match the Xbox build, so there is
no PC-side Copland fix to copy. The fix below is a port-side repair of the
shared logic.

Mechanism: the selector at `0x158F0` scans 16 references at world `+0x2660`
(stride `0x14`), and `0x14170` resolves a model from the 16-entry model table
at world `+0x27A0`. The scripted/random-crime vehicle queue clears the table
(`0x8EA32`/`0x8804D` -> `0x83240`) and then reserves records by calling
`0x7DBD0` from `0x1CC6C`/`0x1E492` with weight 0. Those zero weights leave the
availability mask built by `0x818A0` without the civilian categories: `0x14170`
returns `-1`, the caller at `0x1850F` rejects the civilian ref, and only police
still resolve (the "Copland" state).

Fix: after `0x7DBD0` runs, a queue-created record with weight 0 is repaired
from the matching traffic ref's weight (`ref+8`) for the same slot, and its
idle bit is cleared. The queue still receives its record, so it does not spin
(skipping the call hangs the vehicle queue); the civilian model stays
selectable. Longer play testing is still worthwhile.

`XBE_TRACE_TRAFFIC=1` enables the read-only hooks in `src/game/traffic_trace.cpp`.
The trace records the candidate pool, exclusion bits, effective weights,
selected models, the model-table add/clear callers, the `0x14170` pick table,
and the resource and vehicle model IDs at the `0x823C0` and `0x7DD80` spawn
paths. Guest model IDs 7, 62, 63 and 89 identify Crownwig, police sedan, taxi
and training police sedan respectively. A resource with no data pointer can
still produce a vehicle in a healthy run, so a zero residency field alone does
not establish a spawn failure.

The 30 FPS spawn baseline and 60 FPS selector baseline are saved as
`traffic-baseline.log` and `traffic-selector-baseline.log` under
`out/build/release/logs`.

## Follow-up performance review and user-driven street profile (2026-11-02)

- User reported “All good” after the requested first-pass street/ramp/locker validation, then authorised
  continued optimisation. No game was running at the start of the follow-up. Backed up the current 15 save
  files under `.local/perf-pass2/saves/hdd` before automated runs, preserving the user's later progress.
- Added CreatePipeline wrapper timing and complete PSO counts in d3d12.cpp, including blit/clear/depth
  pipelines. The automated horizon input replay actually reaches the precinct elevator, not a busy street.
  Capture before: `captures/perf-pass2-before/220810/final.bmp`; after: `perf-pass2-after/221659/final.bmp`.
  Logs under `logs/perf-pass2-before` / `after`. Largest startup pipeline one-second average 0.820ms/frame;
  settled pipeline creation 0. No extra pipeline cache implemented based on those small measured costs.
- Found/corrected conservative shader-cache invalidation: &D3DCompile can point at an executable import thunk,
  so the old fingerprint changed with every executable rebuild. ShaderCompilerFingerprint now resolves
  GetModuleHandleW(D3DCOMPILER_DLL_W), hashes the actual compiler DLL and flags. Unit test compares against
  an independent DLL hash; --baseline-fingerprint restores the old lookup and fails regression 15.
- Evaluated removing AssembleConverted's temporary index-sequence vector. Prototype and complete actual
  assembly oracle matched 320 cases; eliminated one allocation per converted draw. Three O2 microbenchmarks
  (7 alternating pairs of 50000 draws) ratios 0.9595,0.9546,0.9584. But 40..70s live replay assembly medians
  were 1.85ms before /1.95ms after, renderer 4.02/4.33ms, draws 870/868. No proven in-game improvement.
  **Reverted the vertex experiment**, confirmed vertex.cpp/.h byte-for-byte against pre-experiment copies.
  Candidate/test retained in `.local/perf-pass2`, not in the build. Do not report the microbenchmark as a
  game-wide gain or quietly reapply that candidate. Standard test count remains 7.
- Builds `logs/codex-pipeline-profile-build.log`, `codex-vertex-assembly-build.log`, final
  `codex-perf-pass2-final-build.log`; relink and SDK patch export/reverse-check completed. All 7 maintained
  regression checks pass after the fingerprint fix. Extended scripts/summarize_perf.py with assembly,
  pipeline and draw-count medians plus --until for a bounded comparison window.
- User agreed to drive a busy street for ~2 minutes and press F3. Launched normal-controls profiling build
  PID 25328 (`.local/perf-pass2/live-street.pid`), XBE_PERF=1/XBE_VERBOSE=1, F3 enabled every 25 draws,
  no scripted/replayed/recorded input or deterministic overrides. Logs `logs/perf-live-street/stderr.log`.
  Original 15 files were restored and verified BEFORE this manual session. **Do not restore that backup
  again over the user's real driving progress.** User controls the game; don't close it to rebuild unasked.
  The street profile's results and the optimisation chosen from it are in `PERFORMANCE.md`.

## First optimisation pass: profiling gates, audio scratch, shader cache (2026-11-02)

- User accepted the proposed measured first pass after the cleanup. No port was running when measurements
  began. Preserved the prior executable/DLL under `logs/perf-baseline`, controller input under
  `.local/perf-pass/benchmark-pad.txt`, and all 15 save files under `.local/perf-pass/save-baseline/hdd`.
- Baseline 125 seconds, then cold/warm70-second runs, with XBE_PERF/XBE_VERBOSE and ordinary frame-indexed
  input replay. No captures, input recording or deterministic clock/RNG overrides. Benchmark processes were
  owned/bounded by the scripts and stopped individually; user save contents restored after benchmarking.
- `render_thread.cpp`: skip diagnostic Clock::now and atomic counter updates when profiling is disabled,
  including Draw/Clear/Copy/Present recording, waits and readback cause counters. Actual rendering commands,
  lock/wait/submit behavior and completion order are unchanged. `d3d12.cpp`: existing PerfScope now gates
  texture signature, frame-fence wait and swap timers too. Actual texture hashing/upload decisions are unchanged.
- `dsound.cpp`: AudibleLanes fills a bounded 320-entry stack array and returns a span instead of allocating
  a vector per audible voice/mix block. Same iteration order, gains and ramp steps; no output-rate change.
- `d3d12.cpp`: persistent DXBC cache under the executable's shader-cache/v1/compiler-fingerprint directory.
  Source+entry+profile keys, compiler DLL content+flags fingerprint; header/size/key/checksum/DXBC validation
  and D3DReflect on load. Normal compilation fallback on miss/corruption. PID-specific temporary writes and
  MoveFileEx replacement; failed IO does not prevent compilation/rendering. XBE_SHADER_CACHE=0 disables it.
  Only bytecode cached, not adapter/driver-dependent PSOs. Added cache-hit counts to performance log lines.
- Added meaningful native regressions `test_perf_gating.py`, `test_audio_lanes.py`, `test_shader_cache.py`.
  Old Draw fails disabled-profiler check; corrected path passes with zero clock reads/counter changes.
  Audio lane construction allocates nothing and matches old lanes/ramped output byte-for-byte across 64
  scenarios. Cache test uses real D3DCompile output and covers corrupt/truncated/oversized/key mismatch,
  IO failure and atomic replacement recovery. All 7 current regression checks pass.
- Builds: `logs/codex-perf-pass-build.log`, `logs/codex-shader-cache-build.log`; maintained relink helper passes.
  SDK patch re-exported and reverse-apply check passes. `scripts/summarize_perf.py` added for reproducible
  log summaries; DEVELOPMENT.md documents cache control. Full results and limitations in PERFORMANCE.md.
- Reported shader counts: baseline 79 compiled; cold 74 compiled/0 hits; warm 9 compiled/74 hits. Cache after
  both runs 83 files/525100 bytes. All steady medians 30fps, no audio-starvation messages. Replay's scene/draw
  workloads differ; **do not claim 5.01ms ->1.52ms as a speedup**, or compile averages as frame-time percentiles.
  The baseline settled view had no readback waits/overflow and ~0.1ms texture checking, so those riskier
  changes remain measurement-driven candidates. Manual street/ramp/locker verification remains outstanding
  after these performance edits. Game timing/quality/scene content have not been intentionally altered.
- Verified all 15 original save files restored byte-for-byte with SHA256. Launched normal interactive
  optimised build PID 10232 (`.local/perf-pass/interactive.pid`), responsive with no startup error. Profiling,
  verbose logging, input record/replay/script and deterministic overrides are off; shader cache defaults on.
  F3 capture remains available every 25 draws. User now has control for the manual compatibility check.

## Code cleanup and optimisation review (2026-11-02)

- User requested a codebase cleanup and a list of possible optimisations, then authorised additional
  improvements within that cleanup. Reviewed title source, local SDK changes and rendering/audio/streaming
  paths. Kept the verified timing, file-lifetime, APC, retained-attribute and scissor fixes.
- Removed obsolete negative-head ramp diagnostics and body-entry lifecycle wrappers from `src/game/hooks.cpp`.
  Those wrappers only logged and delegated to the original functions. The original guest functions now run
  directly; no readiness, allocation or release behavior was changed. Earlier logs/dumps remain preserved.
- Separated the 48 kHz sample clock and opt-in replay/RNG/timing hooks into `src/game/timing.cpp`; removed an
  unused replay frame counter and unused headers. Compatibility hooks, including the established callback-ring
  extension, remain in hooks.cpp. No normal-play RNG or clock override was enabled.
- Moved controller scripts and frame-indexed recording/replay into `src/common/pad_tools.cpp` / `.h`.
  `game_info.cpp` now holds metadata and callback wiring. Controller recording/replay behavior is unchanged.
- Removed a duplicate 256-byte current-attribute copy inside ApplyImmediate's Begin branch: the same copy already
  runs at function entry. Retained-attribute regression passes. Exported that SDK change into the local patch.
- Added `.clang-format` for consistent title-source formatting. Removed empty README feature sections;
  `DEVELOPMENT.md` documents source layout, diagnostics, regression checks and compatibility constraints.
- Added `scripts/native_test.py` shared compiler/DLL harness: CRT-based tests discover Visual Studio through
  vswhere rather than a fixed VS18 path. All four regression scripts use it. `scripts/run_checks.py` runs them
  and reports failures. `scripts/relink_game.py` promotes the previous ignored local helper into maintained
  source, reads partition/library settings from game.toml, validates inputs and updates the runtime build stamp.
- Validation: all 4 checks pass (APC context,1100 balanced file cycles/reference ownership, retained attributes,
  viewport/scissor). Full build and DLL link succeed (`logs/codex-cleanup-build.log`); maintained relink helper
  succeeds; whitespace and SDK patch reverse-apply checks pass. Started cleaned build (PID 15692, recorded in
  `.local/cleanup-test.pid`); startup/guest loading succeeds, process responsive, no logged error on smoke check.
  This was a startup check, not another full parking-ramp/locker playthrough. User had closed the port for building.
- `OPTIMISATIONS.md` ranks 8 source-based candidates with constraints and measurement plans. First candidates:
  gate unused profiling timers/counters, persist shader/PSO caches, reduce texture-change checks with correct
  invalidation, reuse audio scratch storage, improve render-state dirty tracking, investigate measured readback
  waits, cache immutable vertex-layout work, and pool uploads if overflow is observed. No unmeasured speedup
  claims or speculative performance/quality/game-speed changes were made in this cleanup.
- The cleaned game is left running for the user, with F3 capture every 25 draws and no record/replay/script
  overrides. The normal launcher retains its existing diagnostic capture/controller-recording settings.

## Locker preview cutoff: explicit scissor incorrectly intersected with viewport (2026-11-02)

- User resumed the locker investigation after accepting the xemu-like distant skyline. New capture
  `out/build/release/captures/211746` confirms the same head/upper-torso cutoff as old capture 130045.
- **Confirmed cause:** preview draws 497..501 use viewport(358,216,244,264) and explicitly set scissor
  (0,0,639,479). Model geometry is present; the cutoff lies exactly at viewport y216. The D3D12 backend
  intersected that larger explicit scissor with the viewport, incorrectly discarding everything above 216.
- Verified the actual XDK 5849 implementation: SetViewport 0x2B89C2 calls SetScissors(0,0,null).
  SetScissors 0x2B8ED0 chooses viewport bounds only for count 0 (0x2B8F0F..0x2B8FC5). For count 1,
  non-exclusive, it uses the supplied rectangle directly (0x2B8FC7..0x2B9040), then writes surface clip
  0x200/0x204. It does **not** intersect the explicit clip with viewport bounds.
- Corrected `D3D12Backend::SetViewportScissor`: an explicit scissor replaces the viewport clip; no-scissor
  draws retain the viewport clip, and the result remains scaled/clamped to the target. No locker-specific
  coordinates or mesh/head-loading changes. Existing scissor coordinate conventions otherwise unchanged.
- Added `scripts/test_viewport_scissor.py`, compiling the actual method with a recording command-list stub.
  `--baseline` restores the old intersection and fails regression 2 (locker cutoff). Corrected code passes
  default viewport clip, larger and smaller explicit clips, cache reuse,2x scaling, target clamp and reset.
  Retained-attribute regression also passes; SDK diff check and exported-patch reverse check pass.
- Built successfully (`logs/codex-locker-scissor-build.log`), relinked guest DLL and re-exported SDK patch.
  Launched `.local/launch_locker_test.ps1` for user-controlled visual validation (capture every 25 draws,
  all vertex inputs enabled, separate `locker_pad_record.txt`).
- **Visually verified fixed:** user confirmed "done and its fixed"; inspected fresh capture
  `out/build/release/captures/212132/final.bmp`. The complete head, upper torso, hands, legs and shoes render,
  and the model remains inside the right-hand Street Flack preview panel without visible spill. The locker
  cutoff is resolved in the current build; the earlier missing-head/resource-loading hypothesis did not
  explain this rendering issue.

## Pale horizon blocks: both captures inspected (2026-11-02)

- **New xemu reference checked:** latest snapshot `vm-20261102211116` (2026-11-02,21:11:16),
  Mulberry St looking down the long billboard-lined street. Copied its QCOW2 disk read-only from the live
  installation into `.local/horizon-reference/snapshot-disk.qcow2`, then extracted VM/RAM/front buffers
  into that directory, preserving the earlier reference artifacts. No emulator UI control or original-disk edits.
- Recovered front buffers `xemu-front-1.png` and `xemu-front-2.png` show **pale block-shaped distant
  skyline silhouettes too**, clearly visible at the end of the street. This establishes that this visual motif
  also occurs in xemu. Combined with the matching texture structure and verified port shader math below,
  there is no demonstrated port-specific skybox defect requiring a skyline suppression/recolour/fog hack.
  Exact colour/brightness equality is not established: the user explicitly notes different time of day.
- Broader visual review of that xemu view against port captures 204458,204506,205809: no additional clear
  structural fault identified in visible world geometry/building surfaces, character model/materials,
  character ground shadow, road/lane markings, foliage cutouts, street props/vehicles, billboard transparency,
  or HUD placement/alpha. Clouds and distant silhouettes are present in both. These are different camera
  positions/locations, so this is a visual sanity check, **not pixel-exact or complete rendering parity**.
  Do not infer lighting accuracy from these unmatched scenes. The locker model clipping remains a separate,
  deferred issue and was not validated by these street snapshots; timing/audio cannot be validated by a still.
- No runtime rendering change made for the horizon report. User indicated that matching xemu's pale backdrop
  is acceptable. Normal launcher still captures every 25 draws; the diagnostic session captures every draw.

- Follow-up detailed capture: `out/build/release/captures/205809`, Broadway/W46th, different location/time
  but user confirms the same visible issue. Launched unchanged clean build with every-draw images and full
  input capture (`.local/launch_horizon_capture.ps1`, PID 10912). Original two captures remain preserved.
- **Exact writer identified:** draw 1381, a22-vertex distant-city silhouette strip (VS B0000C21,
  texture 82B8FB80, linear ARGB128x66), after sky draw 1370. The city texture has white RGB and a skyline
  in alpha. Draw 1380 is its colour-disabled depth/alpha prepass;1381 supplies the visible blended colour.
  This is distant backdrop geometry, not the cloud mesh failing to render.
- White RGB alone is **not evidence of a missing texture**. Scanned the saved offline xemu RAM:
  nine comparable linear 128-wide skyline textures have the same white RGB / skyline-alpha structure.
  Comparison sheet `.local/xemu-horizon-textures.png`; port RGB/alpha sheet `.local/horizon-texture-alpha.png`.
- Checked the actual captured shader calculation numerically. For the backdrop's front z=-1 face,
  solving the captured projection at pixel(320,205) gives modelposition(0.27481,0.61211,-1), clip w 214.198,
  vertex fog factor 0.184694. White diffuse blended with captured fogRGB(49,51,54) predicts(87,89,91),
  exactly the observed pre-postprocess pixel. At(330,210), factor 0.190568 predicts(88,90,92), also exact;
  (320,210) and(330,215) match too. This supports the identified writer and expected current shader math;
  it does not establish that all source state matches xemu or that the visual complaint is resolved.
- No brightness multiplier, texture recolour, backdrop suppression or fog-distance workaround applied.
  Requested a saved xemu snapshot looking toward a distant skyline to establish whether its rendered
  backdrop differs. Exact location/time is optional; existing offline cutscene reference is obstructed
  by nearby buildings and cannot validate this distant skyline's appearance.

- User clarified that the issue is the pale building-shaped blocks near the horizon, rather than the clouds.
  Inspected both F3 captures: `out/build/release/captures/204458` and `204506`, including their final images,
  intermediate render targets, translated shaders and draw logs. They face opposite directions; both exhibit
  the pale distant silhouettes. No rendering fix is established yet.
- First capture: the affected pixel at (309,202) changes from (36,38,42) before draw 1125 to (142,144,149)
  by draw 1150, then (156,158,163) after final processing. Sky draw 1131 (544 vertices, VS B0101A61) and
  distant-city draws 1132..1139 (VS B0000C31) lie between the intermediate saves. Second capture has the
  corresponding distant-city draws around 1370. Current 25-draw capture spacing cannot distinguish the exact writer.
- Investigated a possible missing vertex-fog mode conversion: **already handled** by `FillFogConstants` for
  programmable vertex shaders with FogTableMode NONE. It supplies identity factor parameters (1,0,0,3).
  Do not change fog mode based solely on the original mode 0 printed in frame.txt. This is not an established cause.
- Existing captures omit the large draws' vertex inputs/constants. Prepared `.local/launch_horizon_capture.ps1`
  to use the existing executable with XBE_CAPTURE_EVERY=1 and XBE_CAPTURE_INPUTS=1. It refuses to launch while
  the current port is running. A fresh F3 capture will distinguish sky versus city draws and expose their inputs.
  No shader, brightness, fog or game-behavior changes made for this investigation.
- Live run continues past 25 minutes with frames advancing and 8..9 active CRT streams (limit 512), after the
  user's confirmed passage beyond the old ramp failure point. Current user-controlled process remains open.

## Confirmed live failure: file-object references leak CRT streams (2026-11-02)

- User took control of the port and requested live failure monitoring. No input replay/clock/RNG overrides were
  enabled. Used `.local/watch_resources.py` with hardware data breakpoints, following body hash 912C89FE and the
  primary descriptor's mapped head. In this run those were body 35 (0x375918) and head 8/absolute48 (0x375EFC), not
  the slots from the old replay. Frame/tick heartbeats detect possible stalls; thread-name exceptions 0x406D1388
  and an initial frame 0 while booting are not game failures.
- A frame stall was captured at frame 10860, sim tick 494, session elapsed ~381 s. Artifacts are preserved in
  `logs/live-resource-failure/`: full memory dump, thread samples, original log, pointer trace, controller recording.
  The watched body 35 was cleared by **original** `0xDB860` called from bulk release `0xDB8C0` (return 0xDB8E4),
  from transition 0x8EA88. There were no pending spawn-list states in the captured dump. Do not equate that ordinary
  transition cleanup with the earlier unproven "resource released while spawns need it" hypothesis.
- **Actual stall in this capture:** thread samples show repeated NtCreateFile/XAPI open activity for
  `D:\Data\Shell\MissionMenu.tag`, not the old spawn wait. Guest NtCreateFile status was 0xC0000022 (access denied).
  The host asset exists (`assets/data/Shell/MissionMenu.tag`,117248 bytes).
- Examined the captured UCRT DLL's own `_getmaxstdio` / `_setmaxstdio` instructions and its stream table: all
  **512 of 512 streams were in use**. UCRT build-specific diagnostic globals: DLLbase+0x138D48 = stream limit;
  DLLbase+0x139800 = stream-pointer table; FILE flags+0x14, allocated bit 0x2000. This is diagnostic-only knowledge
  in `.local/watch_resources.py`, not hard-coded into the game/runtime fix.
- **Ownership defect:** ObReferenceObjectByHandle allocates/binds a guest object body. `Bind` kept an unconditional
  strong shared_ptr in the pointer registry, and ObfDereferenceObject was a no-op. XAPI CancelIo0x263A88 references
  and dereferences the file during cleanup; NtClose erased the guest handle, but the registry still owned the File
  and its FILE*. Repeated streaming opens therefore retained host streams until fopen could not allocate another.
  Kernel file-open code maps such failures to access denied; no user permission problem or missing asset was shown.
- **Runtime fix:** file pointer bindings are now weak address lookups; handles and counted explicit Ob references
  own files. ObReferenceObjectByHandle / ObReferenceObjectByPointer / ObfReferenceObject add explicit ownership;
  ObfDereferenceObject releases it. Final release happens outside the pointer-map lock. File destruction closes
  the CRT stream, unregisters its guest address, and frees its host-allocated guest body. Guest-owned dispatcher
  objects retain their existing host-state ownership. No file-stream limit increase or artificial loader readiness.
- Added `scripts/test_file_references.py`, compiling actual binding/handle/reference functions and actual File
  class. Before: regression 4, closed file retained by its binding. After: duplicate handles and multiple pointer
  references preserve the file until the last owner, weak lookup works while a handle remains, and **1100 balanced
  file cycles** complete with all object bodies reclaimed. APC and retained-attribute regressions also pass.
- Removed the unproven behavioral guards from `src/game/hooks.cpp`: skipped head attachment; patched path/grid
  coordinates; altered allocator eligibility; suppressed resource release; cutscene-mode changes in destructor
  and sample-clock hook. Original guest functions now execute. Read-only head/lifecycle diagnostics remain.
  Backup for comparison: `.local/hooks-before-file-lifetime-cleanup.cpp`.
- Built/relinked: `logs/codex-file-reference-build.log`, then `logs/codex-file-reference-clean-build.log` after
  removing guards. Reopened normal interactive port for the user, with recording/captures enabled and replay
  disabled. Passive monitor: `.local/clean-file-watch.jsonl`, tool session 56068, PID recorded in
  `.local/clean-file-test.pid`. It also records active CRT streams (10-second samples).
- Initial live check after the fix: 9 active streams, peak 18 during boot/menu/world loading, limit still 512.
- Follow-up live monitoring: beyond 9 minutes, frames and simulation still advance, active streams remain 8..10
  (peak 18), with no post-boot exception or stalled-frame event. The watched body was released during transition
  while rendering continued. Monitoring is still active on the user-controlled run; asked the user to confirm
  passage through the original parking-ramp failure spot. Partial clean log: `logs/codex-file-reference-clean-live.log`.
  **User confirmed passage beyond the original parking-ramp failure point in the clean build (2026-11-02).**
  This validates that route with the file-lifetime fix and unproven guards removed. Locker rendering remains open.
  User next reports the skybox looks wrong; requested an F3 capture of the affected sky for diagnosis.

## Resource watch replay interrupted for user control (2026-11-02)

- Began the approved lifecycle investigation using `.local/watch_resources.py`: Windows hardware write
  watchpoints on body 28 data 0x3755EC and head 7/absolute47 data 0x375E88, installed on all replay threads. The tool
  records writer function, thread, new/old pointer, entry bytes, recovered guest registers and stack in
  `.local/resource-watch.jsonl`. It changes debug registers, not guest instructions or loader behavior.
- Replayed `logs/lockup-194849/pad_record.txt` with `XBE_DETERMINISTIC_REPLAY=1` solely to try reproducing the old
  experimental run. Initial loads and releases were captured; no ramp root cause established before interruption.
  Old run PID 13256. Save backup: `.local/resource-watch-save-backup/` (contents of the original hdd directory).
- User asked to take control. Stopped only that replay process, preserved its log as
  `logs/resource-watch-interrupted.log`, restored the backed-up saves by copying them back over hdd, and launched
  a normal interactive port (PID 24516). Replay/script/deterministic environment overrides were cleared. Captures
  enabled, ordinary controller recording to `pad_record.txt`. User now controls the game. Do not close it or rebuild
  locked binaries while the user is playing. The earlier ramp/locker objectives remain active.

## Fresh review: runtime defects versus symptom guards (2026-11-02)

- This section supersedes earlier "real cause" claims about entry release/reuse below. The resource-loading root
  cause is **still unproven**. Several later guards prevented one failure only to expose another; menu smoke tests
  did not validate the parking-ramp sequence. User asked for a cleaner solution instead of accumulating workarounds.
- Confirmed symptom guards in `src/game/hooks.cpp`: skipping missing-head attachment (`0x8C3A0`); replacing invalid
  path coordinates (`0x2600A0`); skipping grid updates (`0x14CF70`); suppressing body release based on spawn state
  (`0xDB860`); changing the slot allocator (`0xDB4B0`); turning off cutscene mode from the destructor/sample clock
  (`0x48F80` / `0x2914D2`). These change title behavior and must not be called established root-cause fixes.
  They remain present for now; this review did not silently remove all guards or claim the ramp is fixed.
- **Concrete runtime defect reproduced and corrected:** `kernel::RunApcs` used `CallGuest`, which restores only
  EAX/ECX/EDX/EBX/ESI/EDI/EBP/ESP. An I/O completion callback could leave x87 values/top/control state, SIMD
  registers, MXCSR and flags changed on the interrupted thread. `RunApcs` now snapshots/restores the complete
  `X86Context` around each normally returning callback. Ordinary synchronous `CallGuest` behavior is unchanged.
  This is a runtime boundary fix, not a title-specific head/position/resource exemption. Its connection to the
  particular ramp resource failure remains a hypothesis until a controlled gameplay reproduction verifies it.
- Added `scripts/test_apc_context.py`, compiling the actual `RunApcs` and `CallGuest` implementations with a stub
  callback that deliberately clobbers processor state. Before the fix: fails with regression 3 (interrupted context
  corrupted). After: passes, including two callbacks (one queued by another), callback argument delivery, empty
  queue behavior, and preservation of guest-memory side effects. Queue/lock stubs mean this is not a concurrency test.
- **Replay experiment leaked into normal gameplay:** `sub_00240CC0` unconditionally restored RNG state around
  world rendering, changing the game's original random sequence even without replay. Such an altered recording
  is not a faithful baseline for the original game. Intrusive RNG/clock/seed replay hooks now require the explicit
  environment variable `XBE_DETERMINISTIC_REPLAY`. Normal play and ordinary `XBE_PAD_RECORD` use the original RNG
  and vblank reader and the continuous 48 kHz sample clock. Input recording itself remains available normally.
  To reproduce the old intrusive experiments, set that variable for BOTH recording and replay; old timing files
  must not be interpreted as evidence of unmodified game behavior. Async streaming still limits determinism.
- Evidence corrections: a loaded marker (-2), zero data and a relocated buffer do not by themselves establish
  who cleared or invalidated the resource. `0x21BFF0` clears decoded fields from+4 onward, **not the data pointer
  at +0**; its presence on a path is not proof of the pointer's writer. No "kept" or "still loading" lines fired in
  the cited later failures, so the release/allocator hypotheses were not confirmed by those replays.
- Cleaner next step: use the existing entry-lifetime trace (allocate, setup, clear, release) and identify the exact
  first write that takes body 28/head7 from a valid data pointer to zero, with its call stack, thread, request/job
  state and pending callback state. Correct that lifecycle/dispatch violation, then remove the matching guards
  and validate the complete ramp progression with original RNG behavior. Avoid additional guessed waits,
  thresholds, coordinate substitutions, selective retention or forced "ready" flags.
- Retain the earlier evidence-based compatibility fixes: viewport/scissor restoration, retained vertex attributes,
  fixed-function current-colour fallback, presentation interval and missing 48 kHz sample counter. The 1024-node
  callback-ring extension is a proven overflow mitigation from initial bring-up, but still a title-specific capacity
  workaround; a bounded completion queue/backpressure design is the longer-term replacement, not another size increase.
- Built/relinked with these two scoped corrections: `logs/codex-clean-review-build.log`. APC regression and retained
  attribute checks pass. The full ramp/crash/locker outcomes have NOT been revalidated by this review.
- Brief smoke test: `logs/codex-clean-review-smoke.log`, 70 seconds, menu/world rendering and resource turnover,
  no error or symptom-guard messages. Stopped only the test process we launched. This is not a ramp regression test.
- Trace limitation found during review: current body trace stops after 400 events and covers only entries 28..39;
  it may exhaust before the ramp and misses head 7 (absolute entry 47). The next lifecycle trace must include both
  affected entries and retain recent events through the first failure, rather than logging only early startup.

## Resume checkpoint before ramp replay (2026-11-02)

- Current priority: investigate and fix the parking-ramp crash first. The user explicitly deferred the locker
  preview issue until afterward. Neither new issue is fixed yet. The user asked to save this checkpoint before
  performing the requested diagnostic replay.
- Current runnable diagnostic build: `out/build/release/TrueCrimeNYC.exe` (built 12:17:49) and
  `out/build/release/TrueCrimeNYC.dll` (relinked 12:17:55), 2026-11-02. Build log:
  `logs/codex-ramp-diagnostic-build.log`. Launch normally, repeat the drive out of the parking area and up the
  ramp, then report the crash. No F3 capture or special environment variables are needed for this diagnostic.
- The temporary hook in `src/game/hooks.cpp`, `sub_0008C3A0`, logs negative head indices and writes
  `out/build/release/logs/ramp-state.bin` once per run before executing the original failing function. It records
  committed pages from guest 0x10000 up to 0x08000000. It collects evidence; it does not prevent the crash.
- After the replay, preserve `logs/tcnyc.log`, `logs/ramp-state.bin` (both beneath the executable directory),
  and the newest `CrashDumps/*.dmp` before launching another session. Run:
  `tools/venv/Scripts/python.exe .local/read_ramp_state.py` from the repository root. This reconstructs
  `.local/ramp-low-ram.bin` and reports the head candidates, mappings, resource entries and async job states.
- Distinguish observation from inference: the old dump directly proves that the head argument is -1 and that
  the following resource lookup returns null. The preceding selector's empty result is inferred from the
  disassembly; its exact resource/readiness state is not in the old dump and is what the replay must establish.
- Useful layout: world pointer 0x360E00; primary character descriptor world+0x27A0, candidate mask at descriptor+4;
  head mapping rows world+0x2958, stride 12, signed slot byte at +0. Head resource entries start 0x375B5C, stride 0x74;
  parent resource entries start 0x37493C with the same stride. Resource data at +0, loaded marker at +0x5E,
  parent index at +0x66, async job handle at +0x3C. Job table 0x4D3720, stride 48, indexed by handle&255.
- Keep the completed graphics and timing fixes while investigating. The final timing fix combines a working
  48 kHz DirectSound sample clock with the requested presentation interval; the rejected cap-only experiment
  must not be restored on its own. Evidence and validation are recorded below.
- Locker screenshot: `C:/Users/user/Pictures/Screenshots/Screenshot 2026-11-02 120919.png`. The right-hand
  Street Flack preview has a sharp horizontal cutoff removing the head and upper torso. Clipping/depth state
  is only an initial hypothesis; no locker frame capture has been collected. Request that capture after the crash.
- Build/relink workflow: set `CI=1`, run `build.bat` with output redirected to a log, then run
  `tools/venv/Scripts/python.exe .local/relink_game.py` to link the precompiled guest partitions against the new
  executable and update its build stamp. Close the port before rebuilding if it locks either binary.
- SDK changes were exported to `patches/xboxxberecomp-local.patch` from the SDK's own repository at the time;
  the SDK is now tracked in this repository (see `DEVELOPMENT.md` for regenerating the patch). Title hooks live
  in the root repository, not in that SDK patch.

## Parking-ramp crash and locker preview (2026-11-02) — both fixed, see sections at the top

- User reports the locker preview loses the character's upper half (screenshot 120919), and a crash while driving
  out of the parking area and up the ramp. **User requested the crash be fixed first; locker investigation is deferred.**
- Latest crash: `out/build/release/CrashDumps/TrueCrimeNYC.2026.11.02_12.09.43.dmp`, session 11:54:26, elapsed 917.103 s.
  Preserved the original log in `logs/codex-ramp-crash-original.log` and pad input in `.local/ramp-input-original.txt`.
- Parsed the minidump's module list and mapped fault DLL RVA0x19145D to `__imp__sub_0008C3A0+0x7D` using exports.
  Original Xbox instruction is `0x8C3C1: movsx ecx, word ptr [edi+0x2C]`; EDI is zero, producing the read at guest 0x2C.
- Guest stack confirms `0x8C3A0(actor=0x644440, descriptor=0x519CE0, head=-1)`, called at 0x8C57A. World 0x517540;
  descriptor is world+0x27A0. Body resource 0x37598C is present; head selection 0x7E800 returns -1 (no candidates),
  then caller 0x8C4C0 forwards it without checking. Head attachment asks 0xDB450 for slot 39 (-1+40), gets null and
  dereferences it. Caller chain includes mission event 0x1FF0A0 at 0x1FF811, spawning a character at ramp progression.
- This is not a D3D/graphics fault. Need establish why no usable head is available (loader state, mappings,
  resource readiness), rather than merely suppressing the null dereference and leaving an incomplete actor.
- Prepared a **temporary diagnostic build, not a crash fix**: hook 0x8C3A0 saves committed guest pages below 128 MiB
  into `out/build/release/logs/ramp-state.bin` (RAMP magic, repeated uint32VA/size + bytes) before the negative-head
  failure. The normal minidump excludes this resource memory. Build and DLL relink complete.
- Asked the user to replay the ramp crash with that build. Awaiting capture. Analysis tools (ignored):
  `.local/analyze_latest_crash.py`, `.local/read_ramp_state.py`, `.local/disasm.py`.
- Remove the temporary diagnostic hook after resolving and validating the cause. Do not mark either new issue fixed.
- Diagnostic replay check (2026-11-02, session 12:52:24, 712 s): **no crash**, clean `Exit (0)`. The hook never saw
  a negative head (no "ramp diagnostic" line, no `ramp-state.bin`), no new dump (newest still 12:09). The only event
  is an F3 capture at 500 s (`captures/130045`) = the locker screen (Street Flack preview missing head/upper body).
- Static reading: head selector 0x7E800 picks randomly from 0x7E730's candidates (descriptor mask bits, mapping
  rows world+0x2958 stride 12). A head counts when its entry (0x375B5C + i*0x74) has marker +0x5E == -2, data +0 set,
  and its async job (+0x3C) passes 0x228F30 (true) and 0x228F40 (false), or when its parent entry (+0x66 -> 0x37493C)
  passes 0x1C810. So "no candidates" = every head of that character not ready. The locker preview also lacks the head:
  possibly the same root cause (unverified).
- Second replay (session ~13:10, crash at 431.891 s, dump `logs/ramp-crash-2/`): hook fired, `attach head -1, actor
  00646D40, descriptor 00519CE0, world 00517540, caller 0008C57F`, same fault. `ramp-state.bin` was NOT written
  (fopen of the relative path failed; no file anywhere). The minidump has no guest resource memory.
- Readiness rules (disassembled): job slot 0x4D3720 + (handle & 255) * 48 must hold the handle; +4 state != 0 means a
  tracked job; 0x248C50 "done" = state 3 or 5. A head is a candidate if marker -2, data != 0 and (no tracked job or
  job done), else if its parent entry passes the same test (0x1C810).
- Diagnostic hook rewritten (build 2026-11-02 afternoon, log `logs/codex-ramp-diagnostic-build2.log`): on a negative
  head it logs to tcnyc.log the descriptor mask and, per mask bit, the mapping slot, head entry data/marker/job
  handle/slot/state and the parent entry's same fields (first 3 occurrences). No memory dump any more.
- Lockup session (`logs/ramp-lockup-1.log`): game froze at the same point (F3 at 432.6 s vs crash at 431.9 s);
  F3's capture never completed; stack words 0x22EC10/0x24FB77/0x2601B7 are game maths, not a wait. Not conclusive.
- **Second crash type (2026-11-02 13:52, 650.8 s, `logs/ramp-crash-3/`), head diagnostic did not fire:** fault in
  main loop 0xA7810 at 0xA796D `mov esi,[ecx+0x9C]`, ecx = [0x348E10] = 0. That is the audio-clock cutscene branch
  (mode flag 0x360E0C, previous 0x360DE0): ticks = audio samples * -0.00125 vs the timing object's +0x9C counter.
  Start 0xA5760 (from cutscene start 0x815F0, which also creates the object via 0x49260 -> [0x348E10]); stop 0xA5780
  (from end path 0x48600 -> 0x7EB37). The object's destructor 0x48F80 (called from its message handler 0x49120) clears
  [0x348E10] but not the mode. **Safeguard added** (hooks.cpp sub_00048F80): when the current object is destroyed with
  the mode on, the mode is turned off (logged "cutscene timing object ... destroyed with audio-clock mode on").
  Root cause of the ordering not proven (might be timing from host audio/streams). Smoke test: boots and runs 100 s.
- **Lockup caught live (2026-11-02 14:05, `logs/ramp-lockup-2/`: thread samples `logs/ramp-lockup-sample.txt`, full
  memory dump `frozen-full.dmp`, reader `.local/read_frozen.py`, guest base 0x100000000):** the game thread spins 100%
  in 0x260101 (loop of 0x2600A0, closest point on a path; called from 0x405C4 in an actor update, chain 0x8F240 <
  0xFFA70 < 0x44090 < 0xFD220 < 0x40590). Frame: target = (-83.3, **1e37**, -750.5) = an actor position
  ([actor+0xB4]+0x70) with the "no height" placeholder; best distance = inf. The search keeps every not-worse step
  and exits only on a worse step with |step| < eps, so with all distances inf it never ends.
- Same dump: the ramp descriptor's only head (mask 1, head 7) **is** ready there (marker -2, data 0x82122000, no job)
  -> at the crash it was simply not loaded yet. Both symptoms = the ramp spawn running before its head / ground
  collision were available.
- Ruled out by measurement: read throughput (0x24C080 completions up to 602/s, not capped per frame) and game speed
  (0x363D0C exactly 60 ticks/s outside cutscenes, like xemu). Root cause of the late resources still unknown.
- **Safeguards added (hooks.cpp):** sub_0008C3A0 with a negative head now returns like the caller's own "no head"
  branch (0x8C581; actor+0x148 stays -1), logged "no ready head ... attach skipped"; sub_002600A0 replaces a
  non-finite or >= 1e30 target coordinate (caller's copy) with the path's first point's, logged "path search target
  had an invalid coordinate". The negative-head readiness log stays (first 3). Build 14:18+, smoke test 200 s clean.
- **New lockup (2026-11-02 afternoon, `logs/lockup-3/`: sample.txt, tcnyc.log, frozen-full.dmp, reader
  `.local/read_lock3.py`):** game thread spins in 0x1E720 `while (!0x1E370(list)) 0xDBE60(loader 0x374938, 1, 0)`,
  called from mission code (0x7FA90 < 0x1FDAC0 < 0x83C40 < 0x25D070). 0x1E370 runs up to 4 character spawn requests
  (state 1..4); state 3 waits for 0x7E570(descriptor world+0x27A0+k*20) = body resource ready (head readiness is
  computed but ignored). Body entry 0x37493C + idx*0x74, idx = byte+2 +0x10 (flag bit 0) or mapping(world+(b+0x368)*12)
  +0x1C. No load job pending in the dump; all head/parent entries 0..63 marked loaded. Several descriptors' body
  entries (idx 16, 28, 29, 31, 32) are marked loaded (-2) with **data 0**, so they can never become ready and nothing
  re-requests them. Not yet established which descriptor the frozen request waits on (spawn list object = ecx of
  0x1E720, not recoverable from the stack yet) or why marked-loaded entries have no data (freed/evicted?).
- Lockup-3 follow-up: spawn list [world+0x1B4] = 0x51A140, entries from +0xC, stride 0x20 (state +0, descriptor -4):
  four requests in state 3 for descriptors 1-4, all needing **body entry 28 (0x3755EC): mark -2, data 0**; its
  +0x50 buffer 0x82BEB000, +0x58 size 0x2DF00, +0x4C callback 0xDBD40 (post-read relocate), +0x60 0x01FF, +0x68 2.
  Callback ring empty; no disc requests queued or in flight; scheduler idle. Loader 0x374938: pump 0xDBE60 services
  loading entries (mark = disc slot) via 0xDBA10 (mark -> -2, publish via 0x21BD50); releases entries flagged 0x10
  after > 2 frames unused (frame counter 0x4C6294 via 0x243080) with 0xDB860, which nulls +0 but leaves the mark -2.
  So entry 28 looks released while four spawns still wait for it; nothing re-requests a "loaded" entry.
- Ruled out: frame counter rate (30 frames/s vs 60 ticks/s in gameplay = xemu's ratio; cutscenes 61-69 ticks/s).
  Next: find which path freed entry 28 (0xDB860 callers, flag 0x10 set) while requests were pending.
- **Release guard (hooks.cpp sub_000DB860):** before the loader frees an entry, if a spawn request in state 2/3 needs
  it as its body (same index rule as 0x7E570), the free is skipped and logged "loader wanted to release entry ...
  kept" with the caller. Smoke-tested (menus, no errors). Waiting for a gameplay session to see whether it triggers.
- **Replay inputs rewritten (user: replays not working):** the old recorder kept only button-press moments, replayed
  as 0.2 s taps (holds lost, sticks/triggers never recorded) and timed in wall seconds (load-time drift). New
  (`src/common/game_info.cpp`): `XBE_PAD_RECORD=<file>` writes "pad2" + a line per change of controller 1:
  `<picture> <buttons hex> <8 analog> <lx> <ly> <rx> <ry>`, timed by presented pictures (new `d3d::PresentCount()`
  in the SDK); `XBE_PAD_REPLAY=<file>` plays it back holding each state until the next line, real controller ignored
  until 2 s past the last line. The recording holds what the game received (scripted presses included).
  `XBE_PAD_SCRIPT` taps unchanged. Round trip checked: scripted run recorded, replay lands on the same screen.
  Launcher still records to pad_record.txt (now pad2). Old recordings (button lists) cannot be replayed with it.
- User also reports buildings on both sides of the road disappearing (new captures in captures/, not yet reviewed).
- Checked and ruled unlikely: stream read ordering. 0x24BD40 is a C-SCAN elevator over (file disc sector from
  0x26381D << 11) + request offset, priority +0x34 first; our 0x26381D hook returns 0 for every file (the original
  returns the GDFX file's start sector, FsContext[0]), but host reads finish at once, so the queue rarely holds more
  than one request and the scan wraps constantly. Not changed. No "callback is lost" errors in the crash log.

## Fixed: gameplay speed and the yellow-car cutscene's missing audio clock (2026-11-02)

- The earlier blanket 30 fps experiment was reverted after the user correctly reported that it made the
  yellow-car cutscene too slow. There were **two interacting timing defects**, not just an incorrect fps cap.
- Compared the user's three new xemu snapshots, working only from a copy of the QCOW2 disk. Guest times were
  144.707790 / 151.337741 / 172.487941 seconds; SwapCount 3688 / 3887 / 4522; simulation ticks at `0x363D0C`
  were 576 / 976 / 2246. Thus xemu presents ~30 fps while advancing simulation ~60 ticks per second. All three
  snapshots have the audio-clock cutscene mode (`0x360E0C`) enabled. Snapshot-menu pauses are excluded by using
  guest time rather than the snapshots' wall-clock dates. Analysis artifacts are under `.local/` (ignored).
- **Missing audio sample clock:** original function `0x2914D2` (DirectSoundGetSampleTime) checks the original
  DirectSound singleton at `0x29EB60`, then reads APU register `0xFE80200C`. Host audio replaces the singleton,
  so the original function returned zero forever. A runtime trace confirmed the singleton is zero in the port.
- Main loop `0xA7810` uses that sample clock for cutscenes: `(samples - start) * (60 / 48000)` determines how many
  simulation ticks to run, with the scale stored as -0.00125 at `0x2D5DC8`. A frozen counter makes it fall back to
  one tick per present. Ordinary gameplay uses at least two ticks per present. This explains both observations:
  at 60 fps gameplay ran too fast while the cutscene advanced 60 ticks/s; at 30 fps gameplay corrected but the
  cutscene dropped to 30 ticks/s, while audio continued normally.
- Added a title hook for `0x2914D2` supplying the free-running 48 kHz counter from steady elapsed time, including
  natural 32-bit wrap. This counter is independent of render rate and voice pitch. Audio resampling, sample
  formats, buffer playback rates and the CPU/kernel clocks are unchanged.
- Restored presentation-interval handling **together with the sample-clock fix**: preserve the interval from
  CreateDevice/Reset, read deferred changes at Swap, and cap pacing to 60/interval while respecting a lower user
  cap. The title requests interval TWO (2), confirmed in xemu render state `0x2CB408 + 127*4` and CreateDevice.
- Runtime verification (`logs/codex-audio-clock-verification.log`): presents ~30 fps; cutscene ticks advance from
  177 to 2697 over exactly 42 seconds (60 ticks/s), then switch out of audio-clock mode. Following that transition,
  ticks advance 2756 to 3056 over 5 seconds (60 ticks/s). The run exits normally with code 0. Temporary simulation
  tracing was removed after validation. This checks game speed as well as presentation rate.
- Reference: xemu's APU implementation reads XGSCNT from its processed audio-frame/sample counter:
  https://github.com/xemu-project/xemu/blob/master/hw/xbox/mcpx/apu/apu.c (`mcpx_apu_read`).
- Release executable rebuilt, game DLL relinked, retained-attribute regression checks pass, SDK patch re-exported.

## Fixed: excessive reflection on one side of the road (2026-11-02)

- The user's xemu snapshot supplied the missing evidence. Worked from a copy of its QCOW2 disk, extracted the
  VM state and RAM, found the guest page directory at physical 0xF000, and resolved the title's surface objects.
  Recovered raw 640x480 front buffers and the 256x256 reflection at physical 0x03454000. Local analysis artifacts
  and scripts are in `.local/` (ignored); the original emulator disk was not modified.
- Reflection images match closely: a uniform region at (150,190)-(230,240) has RGB means
  66.0/67.6/73.1 in xemu vs 68.4/69.8/75.4 in the port. The final road alpha sample also matches (~138 in both).
  Therefore the reflection rendering/blur and wetness alpha did not explain the excess brightness.
- **Root cause: fixed-function vertex shaders substituted white for absent diffuse inputs**, ignoring the
  current vertex attribute. The reflection composite uses XYZRHW|TEX1 (no diffuse stream), MODULATE2X and
  DESTALPHA+ONE. The title sets current v3 to (0.451,0.451,0.451,0.502); the port multiplied by white instead.
- Fixed non-lit fixed-function diffuse/specular fallback to read `vdef[3]`/`vdef[4]`, matching programmable draws.
  Also implemented the original SetVertexShader default-colour writes for absent inputs 3/4/7/8, so an FVF or
  declaration bind resets the current colour before a subsequent explicit SetVertexData override. Original 5849
  code at 0x2BBA57..0x2BBAA7 writes methods 0x194C/0x1950/0x195C/0x1960 with white/zero/white/zero respectively.
  Pending inline vertex-data writes are consumed before changing the shader.
- Verified capture `out/build/release/codex-reflection-fixed/104939/final.bmp`; prior capture
  `codex-road-final/101542/final.bmp`. Raw road RGB averages at (400,350)-(500,400): xemu 55.6/54.7/57.1,
  prior port 92.3/91.6/95.3, fixed port 59.9/58.7/60.2. These are pre-gamma pixels; the camera/particles differ
  slightly, so this is a close visual/region comparison rather than an exact frame match.
- No manual reflection brightness factor was introduced. The dark-patch scissor fix remains. Temporary camera
  capture trigger removed; release executable rebuilt, game DLL relinked, SDK patch updated.

## Fixed: camera-following dark patch up the road (Codex, 2026-11-02)

- **Root cause: regular SetRenderTarget did not reset the host scissor.** The title initially clips a 256x256
  projected mask to (1,1)-(255,255). Each subsequent target bind should restore the full surface clip, but the
  SDK kept that rectangle across the 256/96-pixel blur chain and the final border-clearing lines. An old opaque
  outer ring survived and blurred inward. The road's second projected mask then returned alpha 199/255 at its
  inner edge; the shader inverted it to 56/255, producing the camera-following dark band.
- Proven against original 5849 code: SetRenderTarget calls 0x2BA880 at 0x2B8576; that helper tail-calls SetViewport
  at 0x2BABD7; SetViewport calls SetScissors(0,0,0) at 0x2B89C2..0x2B89C8. SDK SetViewport already cleared the host
  scissor; SDK SetRenderTarget rebuilt its viewport directly but omitted that reset.
- Fix: clear `g_state.scissor_count` with the regular SetRenderTarget viewport reset. SetRenderTargetFast keeps
  its existing behavior. Removed the previous global `uvclamp` / `insetuv` workaround and its draw constants:
  normal texture-edge sampling now works because the title's own border-clearing operations execute correctly.
- Controlled matched-camera captures (all under `out/build/release/`):
  `codex-road-auto-baseline/100809`; first-mask neutralization `codex-road-auto-mask1/100923` (band unchanged);
  second-mask neutralization `codex-road-auto-mask2/101021` (band disappears);
  scissor fix `codex-road-scissor-fix/101329`; final fix without UV workaround `codex-road-final/101542`.
  No mask neutralization, texture-mip override, or special camera trigger remains in the normal executable.
- Wetness alpha means across x=380..519 before reflection composition: rows 325/330/335/340 were
  54.8/53.9/52.8/54.1; now 71.4/75.7/78.2/84.5. Row 345 remains 82.0; rows 350/355/360 remain 85.4/86.6/87.0.
  The flat artificial strip is gone, without suppressing either projected map. The mask's top/left border is
  now zero instead of 255, with no 199-alpha halo leaking into the interior.
- Ruled out mip filtering: forcing the road texture to mip zero retained the band; the identified road texture's
  five mip alpha averages were 72.67/72.29/71.92/71.50/71.33. Sampling its proper mip chain is restored.
- Release build and retained-attribute regression checks pass. SDK patch re-exported. The separate overall
  right-road reflection brightness comparison against xemu remains open; this fix addresses the moving patch.

## Fresh graphics review (Codex, 2026-11-01)

- **Earlier investigation, superseded by the fix above; screenshot 224525 / F3 capture 224517:** a broad dark patch extends up the road and its
  boundary follows the approaching camera. Distinct from the earlier single-row seam. In capture 224517 the
  discontinuity is already present in framebuffer alpha at `0433_83ED0000.bmp`, before mirror composition.
  Alpha at x=450 is flat 56 on rows 325/330/335 and returns to detailed values farther down (126 at row 350,
  82 at 355, 108 at 360). Mirror composite draw 492 uses DESTALPHA+ONE, so it amplifies the discontinuity;
  it does not originate solely in the mirror target. The road shader B016C060 derives alpha through the
  t0/t1/t2 mux chain; investigate texture LOD/mips and projected masks separately. **No visual fix established.**
  Capture texture lines now include mip count/filter, LOD bias and max mip, which were missing from the evidence.
  A timed replay attempt reached a different scene; do not use `codex-road/225122` as a matched comparison.
  A live texture read taken before the intended scene loaded was also not a valid road-texture sample.

- **Implemented and replay-tested: retained vertex attributes were incomplete.** The earlier `vdef` fix only
  remembered setters outside Begin/End. NV2A also retains immediate setters inside Begin/End and streamed vertex
  attributes. A streamed mesh carrying white diffuse colour therefore replaces a previous lamp tint before a
  subsequent shader reads unfed v3. The SDK instead kept the tint indefinitely across those meshes.
- `gpu::RetainVertexAttributes` now decodes the last submitted vertex with the existing vertex converter after
  DrawVertices, DrawIndexedVertices and DrawUp. Indexed draws include base_vertex. Immediate setters retain every
  component write and synchronize with current streamed attributes, including partial updates.
- Reference behavior checked against xemu's implementation, without copying its code:
  https://github.com/xemu-project/xemu/blob/master/hw/xbox/nv2a/pgraph/gl/vertex.c
  (`pgraph_gl_bind_vertex_attributes`, updates retained attributes from the submitted vertex), and
  https://github.com/xemu-project/xemu/blob/master/hw/xbox/nv2a/pgraph/pgraph.c (immediate setters).
- Replay captures: `out/build/release/codex-graphics/223813` = immediate-only correction;
  `224118` = streamed + immediate carryover; comparison baseline `street/t42.5`. The final small follow-up
  synchronizing partial setters with streamed attributes is regression-tested and rebuilt, rather than captured.
- Raw BMP luminance means, same fixed sample rectangles (640x480; bounds right/bottom exclusive):

  | Sample rectangle | Baseline | Immediate only | Streamed + immediate |
  | --- | ---: | ---: | ---: |
  | Facade (95,150)-(215,290) | 27.3 | 24.4 | 52.8 |
  | Sky (250,80)-(370,150) | 71.7 | 72.5 | 73.1 |
  | Road left (130,345)-(270,390) | 29.0 | 27.1 | 28.1 |
  | Road right (400,345)-(540,390) | 90.2 | 90.6 | 90.4 |

  The camera/particles have small replay timing differences. These are raw port-capture comparisons, not
  gamma-corrected xemu measurements. Nevertheless the facade becomes substantially brighter without doubling the
  sky, unlike the earlier forced-white experiment. **The overbright right road remains unresolved.**
- **Correction to the cache theory below:** disassembly of 0x136E70 shows that 0x3F54CC is assembled from bitfields,
  lookup tables and comparison flags, not a monotonically increasing version. 0x2409F0 snapshots render state and
  light records; 0x240250 queues the snapshot in a depth bucket; 0x240E70 restores it when drawing. Unchanged
  +0x1C does not demonstrate stale lighting or a failed cache invalidation. No forced refresh/picker hook was added.
- Validation: `scripts/test_immediate_attributes.py` compiles the actual SDK function bodies in an isolated DLL
  and checks immediate/streamed carryover, partial setters, implicit z/w, index order, base vertex and empty draws.
  Full release build passed. SDK changes are included in `patches/xboxxberecomp-local.patch`.
- Next investigation should isolate road reflection colour and road destination alpha separately. Do not infer
  another CPU lighting-cache bug from the old facade measurements, or compensate by globally whitening quads.

## Tool choice (2026-11-01)

- **XboxXBERecomp** (https://github.com/TGP482/XboxXBERecomp, MIT, cloned at `f7b13fd`, 2026-10-31) — x86 XBE ->
  C++ -> native x64 Windows exe; kernel + XDK (D3D8, DirectSound, XAPI) reimplemented at API level; D3D12 renderer,
  XAudio2, SDL3 pads; Bink fixes. Has full ports: Splinter Cell Double Agent, BLACK, 007 FRWL, 007 Nightfire.
- Alternative considered: sp00nznet/xboxrecomp (x86 -> C, D3D11). No title past early boot yet (Oct 2026), so not used.
- Reference port cloned at `tools/SCDA-XBERecomp` (how a finished game repo fills in hooks/game_info).

## Disc

- `True Crime - New York City (USA).iso`, 7.8 GB Redump full dump; XDVDFS game partition at 0x18300000.
- `scripts/extract_xiso.py <iso> <out> [--list]` -> `assets/` (15,473 files, 2.3 GB). File list `logs/disc-files.txt`.
- `default.xbe`: 3.5 MB, title id 41560045, base 0x10000, entry 0x263BC5, .text 0x12000 (2.4 MB).
  Sections include BINK*, DSOUND, WMADEC, D3D, XPP, DOLBY. **All libraries XDK 5849** (XAPILIB, DSOUND, XBOXKRNL,
  LIBCMT, D3D8LTCG, XGRAPHCL) -> SDK has `xdk/5849` signatures. D3D is the LTCG variant (register args; may need
  `[abi]` entries in `config/game.toml`).

## Project layout

- Repo root = game repo from `sdk/templates/game`. `sdk/` = the SDK. Exe/target name `TrueCrimeNYC`, log prefix `tcnyc`.
- Toolchain: VS Build Tools 2026 (MSVC 14.51, Win SDK 10.0.26100) + LLVM 23.1.4 (`C:\Program Files\LLVM`, winget
  LLVM.LLVM, MSVC target). CMake 4.4.4, Ninja. (llvm-mingw from the PS2 port is GNU ABI, not used here.)
- Build: `build.bat` (set `CI=1` for no prompts). Step 2 writes `config/game.toml` via `xbe_symbols`; step 3 recompiles.

## Build notes

- Run `build.bat` as `cmd //c '.\build.bat'` from Git Bash (the session blocks running from the current dir otherwise);
  batch files need CRLF (template copy came with LF, fixed with unix2dos).
- The exe rebuilds `TrueCrimeNYC.dll` (game code) on the first start after a runtime change (~95 s).
- Analysis: `tools/venv` (Python + capstone), `scripts/xbe_disasm.py <xbe> <va> [n]`, `--callers <va>`,
  `--targets <lo> <hi>`. Full disassembly of unmapped D3D functions: `logs/d3d-unmapped.txt`.

## Config fixes (config/game.toml, all commented in place)

- xbe_symbols found 110/243 functions, 5/12 data. `[abi]` for 5 LTCG D3D functions worked out from the code.
- `IDirectSoundBuffer_SetFilter` = 0 (signature hit SetEG's wrapper, duplicate address -> compile error).
- `[data]`: D3D_DirtyFlags 0x2CB400, RenderState 0x2CB408 (3 states agree), DeferredTextureState 0x2C8D48,
  Streams 0x2CC3C8, IndexData 0x2CC2A8, SimpleRenderStateEncode 0x2C7A38 (92-entry table found by scan).
- By hand: SetTile 0x2B8E70, GetTile 0x2B8E90, SetRenderTarget 0x2B8260, Begin 0x2BC9E0, End 0x2BCA20,
  GetBackBuffer2, GetRenderTarget2, SetPalette, SetRenderState_TextureFactor/OcclusionCullEnable/Simple,
  SetShaderConstantMode, BeginPush/EndPush (addresses in config).
- `[[function]] 0x293541` so the hook below can replace it.

## Game hooks (src/game/hooks.cpp)

Current list (updated 2026-11-03; camera, timing, Options and trace hooks are in the other `src/game` files):

- 0x293541 CDirectSoundStream SetMode-like 3D method (no SDK version) -> DS_OK.
- 0x29366D CDirectSoundStream SetConeAngles-like 3D method -> DS_OK (the host mixer has no sound cones).
- 0x26381D XAPI drive-head position for the stream scheduler (reads kernel device object) -> 0.
- 0x2BEDE0 D3DDevice_SetBackBufferScale: keeps values, skips hardware render target rebuild.
- 0x24C020 stream completion: grows the 48-slot callback ring by 1024 slots once (see "no 3D behind the menu").
- The early 0x2BAD20 D3DDevice_SetDepthClipPlanes hook was replaced by an SDK version (see "black wedge").

## SDK changes (patches/xboxxberecomp-local.patch)

- Kernel 76 IoQueryVolumeInformation (shares NtQueryVolumeInformationFile's classes).
- Kernel 232 NtUserIoApcDispatcher (ReadFileEx completion routines).
- `XBE_WAITLOG=1`: logs waits blocked > 3 s with the guest stack. `XBE_VERBOSE=1`: keep info/warn log lines.
- ObReferenceObjectByHandle writes a host-allocated object body; since 2026-11-02 handles and counted references
  own file objects (see "file-object references leak CRT streams"). The 0x26381D hook remains because host file
  objects have no device object to read a drive-head position from.

## Picture fixes (2026-11-01, later)

- "Black screen" was not a hang: the SDK's release log drops everything below error level, so perf lines never showed.
  SDK change: `XBE_VERBOSE=1` keeps every level. With it: steady 60 fps, presents every frame.
- `scripts/sample_threads.py <exe> <game dll> [s] [hz]`: per-thread sampling of guest functions (DLL exports) +
  runtime frames (`llvm-symbolizer --obj=TrueCrimeNYC.exe 0x140000000+off`). `SAMPLE_ALL_RAW=1` dumps all stacks.
- Frame capture: `XBE_CAPTURE_DIR=captures`, F3 in game -> `out/build/release/captures/<time>/frame.txt` + BMPs.
- **Fixed: title frame buffers.** CreateDevice passes its own BufferSurfaces[3] (+0x34) and DepthStencilSurface
  (+0x40) (game objects 0x4C629C/0x4C62C0/0x4C62E4, picked by back buffer memory in 0x242F80). SDK ignored them and
  presented its own empty buffers. `SetFrameBuffers` now uses the title's surfaces. -> menu ("Press START") visible.
- **Fixed: pretransformed immediate-mode quads** (XYZRHW FVF) are packed in the FVF layout and drawn as such.
- **Fixed: intro movies.** 0x2BD270 is D3D_BlockOnResource (eax), not D3DResource_IsBusy as xbe_symbols said; the
  SDK version marks CPU-written surfaces dirty, so Bink frames reach the GPU now.
- SetBackBufferScale hook also sets/clears device flag 0x4000; SDK Swap presents +BackBuffer when it is set.

## Fixed: no 3D behind the menu, endless loading screen (same cause)

- The world-ready check 0x147C40 (polled by the menu scene and the loader loop 0x8DAF0 via 0x148030) needs all tile
  slots (state machine 0x137600, finish methods per tile type) and their shared resources (table world+0x2C,
  0x54 each, +0xC state 1 = loaded) ready. Geometry tiles waited on resource 64, whose read completed but whose
  callback was lost.
- Cause: stream completion 0x24C020 (streaming thread) writes callbacks into a 48-slot ring (producer 0x4D48A8,
  consumer 0x4D48A4 drained once a frame by 0x24BEA0) without checking for space. Host reads finish instantly, so
  a burst laps the ring and overwrites callbacks. Hook 0x24C020 grows the ring by 1024 slots while it is empty
  (waiting for the main thread instead deadlocked). Logged once: "stream callback ring grown from 48 to 1072".
- SDK Swap now also runs the presenting thread's queued APCs (the console's present waits alertably). Not the fix
  here, but matches the hardware.
- Then: 0x29366D CDirectSoundStream SetConeAngles-like (4 args) crashed like SetMode -> no-op hook + [[function]].
- Result: 3D city drawn (millions of DrawVertices, 5-9 ms draw per frame), 70 s run without errors.
  **Confirmed by the user 2026-11-01: 3D menu background shows, New Game gets past the loading screen into gameplay.**
- Analysis helpers used: `[xbe] trace = [...]` in config (logs calls/returns), temporary X86_FUNC hooks calling
  `__imp__sub_...`; the 0x26381D and 0x24C3F0 return values are not errors (0x24C3F0 always returns -1).
- Temporary diagnostics were removed again (2026-11-01). The wait log stays as an SDK option: `XBE_WAITLOG=1`.

## Status (2026-11-01, early bring-up; superseded by the fixes above)

- Crash-by-crash bring-up: 0.15 s -> 0.5 s -> 5.7 s -> 15 s -> 22 s -> no crash.
- Run 14 (`logs/run-14-waitlog.log`): runs 166 s, no crash, **black screen, no Present at all**; no kernel wait
  over 3 s, so the main thread is busy, not blocked. Next: find the spin (likely a loop polling a GPU register or
  push buffer get pointer that the host never advances), e.g. by sampling guest EIP or tracing candidate functions.
- Unmapped D3D functions still running original code (judged safe): GetDesc/GetLevelDesc, GetSurfaceLevel2,
  LockRect, CreateVertexBuffer2, VB Lock2, SetVertexData*, SetVertexShaderConstant1 inline 0x2BBB10, Reset 0x2B81A0.

## Repository

- The project and its modified XboxXBERecomp SDK are tracked together on `main`. The SDK snapshot in `sdk/` comes from the former local SDK commit `ff08e632` and includes the graphics and runtime fixes.
- The ISO, extracted assets, generated code, build output, logs and local reference tools stay outside Git.
- For a fresh build, put your own `default.xbe` and game data in `assets/`, then run `build.bat`. Run `release.bat` to package the combined launcher and game executable.

## Lighting / darkness investigation (2026-11-01)

- **Fixed: constant-alpha blending.** D3DBLEND_CONSTANTALPHA/INVCONSTANTALPHA (0x8003/0x8004) were mapped to the D3D12
  blend factor with the blend colour's RGB; they must use its alpha. The game's soft-shadow buffer (subtracted by the
  city material: r0 = t0*v0 - t2) and its bloom chain (bright pass B0000C10 -> blur -> added with CONSTANTALPHA, ONE)
  came out black. SDK commit on the former `tcnyc` SDK branch, now part of `sdk/`.
- Checked and fine: texture decoding (contact sheet of all 219 textures), combiner output scaling, gamma ramp (set
  once: S-curve 0,18,57,148,223,255 for 0,32,64,128,192,255, applied by the SDK), D3D fog (disabled: the game fogs in
  its vertex programs), the clock (menu scene is a rainy night at any time of day).
- City material: ps B016C050, vs lighting from constants (ambient c[161] ~0.2, point lights c[169..176],
  oD0 = min(R0, c[179].y)). Reflection pass (rt 256x256, FFP texture-only) is dark because the facades are night art.
- "Black road" captures (incl. the user's) were frames during a **fade-in**: helper 0x2468F0 draws a full-screen
  black quad blended with CONSTANTALPHA/INVCONSTANTALPHA, amount from 0x1DC70A (0x2463E0(mode 2, amount)), counting
  128 -> 0 after each menu camera cut (holds at 33 while the city loads). It sits mid-frame (after the street, before
  objects) by design. Before the blend fix the fade never applied (factor came from the black RGB).
- Debug captures: `XBE_CAPTURE_AT=<s>` (auto F3), `XBE_CAPTURE_EVERY=<n>` (save the target every n draws), stencil
  state in frame.txt. Alpha is in the 32-bit BMPs (read the 4th byte; Pillow drops it).
- Reference: Xbox screenshots (Bing image search) show night streets with grey wet asphalt and visible facades.
- Far-road "dark band" (user report): lamp light pools (blend DESTALPHA, ONE) only light pixels whose frame buffer
  alpha (wetness) is set. Near road (puddle material) writes partial alpha; far paving (material writing t0.a) uses a
  DXT5 texture (0x81E70000 in capture 110327) whose alpha is ~3%, uploaded natively as BC3, so the light stops there.
  Game data, kept as original by the user's choice (2026-11-01).

## Fixed: black areas in cutscenes and on the road (2026-11-01)

- Root cause: **D3D_InitDefaultState was never found** (0 in config), so the Xbox default render states were never
  applied; the host's simple states started at 0. Visible symptom: STENCILMASK/STENCILWRITEMASK 0 (Xbox default
  0xFF), so a stencil-masked darkening quad (helper 0x2468F0: marks with stencil INCR, then a black quad with
  LEQUAL ref 1) covered the whole screen and blacked out everything drawn before it (road, near geometry, the
  "black wedge" in a cutscene). The earlier "fade-in" explanation for those frames was wrong.
- Mapped D3D_InitDefaultState = 0x2BFAF0 (called by device init 0x2C058B). To let it run:
  - all complex render state setters from its table 0x2C7BC8 (slot k = RS 136 + k), 18 newly mapped; ZBias (149)
    has no SDK version and stays original;
  - D3DDevice_SetTextureStageStateNotInline = 0x2BA2F0 with `[abi]` ecx stage, eax type, edx value;
  - `[data] D3D_FvfShader = 0x2CC2B0` (device init stores it at CDevice+0x794; many D3D internals read it).
- Result: menu/cutscene streets lit and wet, close to the Xbox screenshots. 120 s run: 60 fps, no errors.
- One earlier run logged "pipeline creation failed" (347 keys) from 17.6 s; not reproduced since. Failures now log
  their pipeline description at error level.
- Launcher `Play True Crime NYC.cmd` sets XBE_CAPTURE_DIR so F3 always saves a capture.

## Fixed: black wedge in cutscenes (2026-11-01)

- Step images (`XBE_CAPTURE_EVERY`) pinned it to one draw: a 544-vertex immediate-mode backdrop mesh (vs B0101A61,
  untextured, opaque, no z write; game code returning to 0x247A13) drawn with SetDepthClipPlanes(0, 70748) around
  it. The game uses reversed screen z (near = large), so the console clipped the mesh's near part; the host ignored
  the planes and drew it over the road and buildings.
- SDK: D3DDevice_SetDepthClipPlanes (mapped 0x2BAD20, replaces the game hook) stores the planes; vertex program
  draws clip through SV_ClipDistance (o.zclip in VOut, zclips in the draw constants). Fixed function planes are kept
  but not applied (not used by this game so far).
- Also learned: the car "shadow" stencil system (0x138DFC..0x138FA5) works as intended once defaults are applied.

## Intro cutscene vs reference video (2026-11-01)

- Reference: YouTube "True Crime: New York City Gameplay #1 (XBOX)" (Mjj585hinHs), intro from 2:05. Frames grabbed in the
  browser pane by drawing the video into a canvas.
- Replay: `XBE_PAD_RECORD` (launcher writes `pad_record.txt`) and `XBE_PAD_SCRIPT` replay the user's menu path (Load
  Game, Intro Act save); `XBE_CAPTURE_AT` takes a list of times. Captures slow the game: start them after the inputs.
- The black wedge shot (yellow car, ~2:08-2:12) now renders fully (skyline, trees, lamps, road) after the depth clip
  plane fix. The dark alley shots (~3:06-3:20) match the video (mean ~12, ~73% near black in both).
- Open: bright foggy street shots are ~2x brighter than the video (top third ~110 vs ~60). Video levels are close to
  ours (bars ~8, white ~239). The game's gamma ramp (S-curve, 32->18, 128->148) is applied. Fog is off for nearly all
  draws. Main suspect: the 49 additive lamp halo strips (vs B0101A61, tex 83723380 radial glow, SRCALPHA+ONE,
  MODULATE2X) at the end of the scene, which add ~+13 mean in capture 160003.
- Capture lines now include fog state; the gamma ramp is logged when set.

## Fixed: hard bright/dark line across the road (2026-11-01)

- The road shader (ps B016C060) builds its wetness alpha from two projected 256x256 render-target maps (t1, t2),
  sampled clamped through a swizzled A8R8G8B8 view. Light pools and the reflection add light weighted by that alpha.
- The maps rotate between uses. The title draws into them under a scissor (1,1)-(w-1,h-1) (0x25F981), so their
  1 pixel border keeps what an earlier opaque scene pass into the same memory left: alpha 255. Its own border lines
  (black, alpha 0) fall under that scissor too. Reads past the map edge then switched the wetness off: the far road
  stayed dark behind a hard line.
- SDK: clamped reads of a render target through a swizzled A8R8G8B8 view use the border color (0) past the edge.
  Verified by replay on the street shot: one even wet road to the horizon.
- Also: lines are drawn one console pixel wide on upscaled targets (geometry shader LineHlsl) and on host pixel
  centers; draws without a depth buffer skip depth clipping. Capture lines show address modes, filters, border,
  viewport/scissor, alpha test; the raw vertices of small draws.

## Fixed: lamp halos and fog far too bright (2026-11-01)

- The halo strips (vs B0101A61, radial glow texture, SRCALPHA+ONE, MODULATE2X) read the vertex colour register v3,
  which their stream does not feed. The title sets it just before with SetVertexData outside begin/end: warm, dim
  values such as (0.20 0.15 0.10 a 0.25) and (0.10 0.08 0.05 a 0.125). The console keeps that value for unfed inputs;
  the SDK used white. Now vertex programs read unfed inputs from State::vdef (the last value set; v3 starts white),
  passed in the draw constants. Street shots: mean ~80 -> ~40-55, close to the video (~39-48).
- Open: in some street shots the road is lit on the right and dark on the left with a straight edge. It comes from
  the wet-road reflection (draw ~533 adds target 81C98000, the blurred mirror pass into 01528000, by dest alpha).
  The mirror pass is mostly black; a reflected hillside wall on the right is cut along a straight vertical edge.
  Not the depth clip planes (tested off). Next suspect: clipping of mirrored geometry with vertices behind the
  camera (negative w; the console does not clip programs at w = 0 the way D3D12 does).
- Split narrowed: the reflection's grey slab is draw ~508, the 544-vertex skyline backdrop (vs B0101A61) drawn into
  the mirror target, cut along a straight vertical edge. Tried rebuilding clip space with |w| for every program draw:
  it breaks normal scenes (teal shapes behind the menu), reverted. Unconfirmed whether the backdrop's vertices
  really have negative w in the mirror pass (a debug run drifted off the replay path).
- Replay caution: the first start after a rebuild relinks the game code and loads slower, so XBE_PAD_SCRIPT timings
  drift (twice it reached the New Game name screen; nothing was saved). Start the game once after a build before
  replaying, or make the script wait on screens instead of times.
- User capture 174128 (split shot): video road is uniformly dark (~12-19 across); ours ~25-34 left, ~76-97 right.
  The bright right comes from the mirror pass's skyline backdrop (draw 509, 544 verts, vs B0101A61) reflected.
  Ruled out: depth clip planes (SetDepthClipPlanes HLE matches the library's flags 1-4, offsets 0x1C08-0x1C18;
  GetDepthClipPlanes 0x2BADD0 reads them back with flags 10/11), and dropping triangles with a vertex at w < 0
  (tested with NaN positions: the split stayed). Next: compare that draw's vertex colour/lighting constants in the
  mirror pass against the main pass, and the reflection target's blur chain (8003/8004 constant alpha blends).
- Video 2:21 (same shot as replay capture 175516): the Xbox road is lit evenly across its width (strips 49-64, wet
  sparkle to ~210). So the reflection does work there, over the whole road. Ours only lights where the mirror image
  has content; its left part (reflected buildings, draws ~475-484 into the mirror target) comes out black.
- Checked and matching the console: the composite (XYZRHW|TEX1 quad, MODULATE2X, v3 is white then), blur chain
  (8003/8004 with blend colours FF/7F/55/3F then 78, E6), skyline vertex colour (white), clip planes (full range).
- Mirror pass skyline lit by c[186] = (1,1,1,0.65) vs (0.20,0.21,0.23,1) in the main pass: by design.
- Next: decode the mirror building draws' packed positions (vs B01012C1/D1) to see whether they land in the mirror
  target at all (culled, off target, or drawn black). XBE_CAPTURE_INPUTS=1 dumps every draw's first vertices.
- Found the mirror-pass difference: packed-position meshes (vs B01012C1, normalized short positions, decode checked:
  the main view projects them on screen at scale 1) get a wrong matrix in the mirror pass. c[188..191] at draw 400
  (mirror) has coefficients ~1 and translations 0.007/-1.46/-0.07, giving w ~0.1 and positions thousands of pixels
  off the 256x256 target; the same mesh type in the main view (draw 476) has coefficients ~7 and translations
  -4.1/0.25/19.0. Float-position meshes in the mirror pass are fine (draw 407: w ~14, on target).
  So the CPU side that builds the mirror matrix for packed meshes (mirror transform x the object's dequantization
  scale/offset) goes wrong in the recompiled code or an HLE it calls. Next: find where the game writes c[188] for the
  mirror pass (SetVertexShaderConstant callers around the mirror render, 0x25F9xx) and trace the matrix math.
- Traced the matrix: every write of c[188..191] goes through SetVertexShaderConstantNotInlineFast (0x2BBB60) from two
  places, 0x244D2E (A = [esi+0x670], B = [esi+0x90]) and 0x243F7C (A = [[esi+0x20]], B = [ebp+8]); both multiply with
  0x24C6E0 (checked by hand: R = transpose(B x A), correct) and transpose. Main view: A = projection only, B = world
  x view (translations ~1e3). Mirror (256 viewport): A = a full mirror view-projection (entries ~1-10, shared by all
  objects), B = per object with translations ~0.1 and scales ~0.2-0.9, giving w ~0.1 for the packed meshes.
  Not yet known whether that B is what the console gets too (then the difference is elsewhere) or comes from data
  the port fills differently. Next: dump the same values on real hardware or in xemu, or trace where [ebp+8] of
  0x243EF0 is built for the mirror pass.
- Mirror object path: 0x25FAF0 builds B = (scale from [obj+0x40] via 0x22BFF0, offset [obj+0x30]) x parent
  [ebp+0x10], unless obj flag 0x40 (then B = parent). Logged in the cutscene: parents are rotation + translations
  within ~10 units of the mirror camera, objects small (scales 0.05-1.7). Consistent data: the mirror pass reflects
  nearby props with its own camera; the CPU-side matrices look as intended. With D3D12 they mostly fall outside
  the 256x256 mirror target or behind the eye. Remaining open question is how the console rasterizes those
  near/behind-eye triangles (both "keep |w|" and "drop w<0" were wrong). Best next step: compare the mirror
  target (and the road) in xemu at this shot.

## xemu reference (user screenshots, 2026-11-01) vs ours (captures 184206, 184218)

- Region means (xemu / ours now / ours before the vdef halo fix): sky 87/85/154, lamp-lit facade 62/37/63,
  road left 12/25/70, road right 46/87/174, lamp glow 162/120/199. Sky now matches; facades too dark; road ~2x.
- Proven correct on the CPU side: per-object lamp positions (c[169], written by 0x24515C through
  SetVertexShaderConstant1 0x2BBB10, from 0x22D810 = a correct 4x4 inverse, checked err ~3e-5); the building
  normals; the facade in question faces away from that lamp, so its light in xemu comes from the additive passes.
- Unfed v3 per draw: the 47 haze strips (prim 5, 228 verts) are white either way; count-4 quads get the tints
  (blend 302,1: 0.41 0.30 0.20 a 0.50; blend 304,1 light pools: 0.20 0.15 0.10 a 0.25 and 0.10 0.08 0.05 a 0.125).
  The light pools add lamp light by dest alpha: facades have alpha 255, the road ~0.35.
- Likely remaining cause: the road's wetness alpha (ps B016C050 mux chain over the t1/t2 maps) is too high and
  the light pool strength too low, so light lands on the road instead of the facades. Also the user reports a
  dark band across the road in 184218 that xemu does not have (far road just beyond the bright near road).

## Fixed: thin dark line across the far road (2026-11-01)

- User marked it in capture 184218 (xemu has none). One screen row dipped (alpha 99 vs ~115 already when the road
  is drawn): the edge of the projected wetness map, where the earlier border-colour reads blended with 0.
- Replaced the border workaround: for clamped reads of a render target through a swizzled A8R8G8B8 view, the
  shader keeps uv on the second texel from each edge (DrawConstants::uvclamp, insetuv in the pixel shader), so the
  unwritten outer ring is never read. Replay of the street shot: no dipped rows, far road continuous.

## Light balance vs xemu (2026-11-01, open)

- xemu screenshots include the console gamma ramp; captures do not. Applying the logged ramp (32->18, 64->57,
  128->148, 192->223) to ours: sky 88 (xemu 87), road left 16 (12), road right 85 (46), lamp-lit facade 26 (62).
- Light pools (B0101A61 count-4 quads, blend DESTALPHA+ONE, no v3 in the stream, positions a unit quad) take the
  tint the title sets with SetVertexData. Experiment with them white (XBE_EXP_POOLWHITE, removed): facade stayed
  ~17, road right went to 157. So they are not what lights the facade in xemu.
- No unhandled vertex data methods in the push buffer (only 0x1710, unrelated).
- Open: what brightens the lamp-side facades on the console (before the vdef change it measured 63 here, so one of
  the draws whose unfed v3 changed from white to a tint is involved: the count-4 SRCALPHA+ONE quads with
  0.41 0.30 0.20 a 0.50 are the remaining candidate), and why the right road is ~2x.
- Experiment XBE_EXP_302WHITE (removed): the count-4 SRCALPHA+ONE quads white -> facade 66 (xemu 62) but sky 161
  (87), lamp 224 (162), road right 76 (46). These quads are what lights the facades; on the console they light the
  buildings strongly without brightening the sky, so the difference is likely where they land or what masks them
  (depth or alpha), not only their colour. Next: capture those quads' vertices/positions and depth state and see
  which part of them covers sky.
- Building lamp lighting (vs B01012E1/F1, evaluated in Python on captured inputs, scratchpad vseval.py):
  lamp term = (n . L) / (1 + c169.w * d^2) - c170.w, clamped at 0, with c169.w 0.528 and c170.w 0.05: a lamp only
  lights surfaces within ~6 units. Facades in the street shot are 10-14 units from the lamps, so they get only
  ambient (~0.06-0.14). Inputs verified: lamp positions (correct inverse), normals (camera-facing walls +0.98),
  dequant scales. So by the title's own maths these walls are not lamp lit; the xemu facade difference is not
  explained yet (possibly a different moment or camera in the xemu shot).
- xemu recording (user, Radeon ReLive, 21:12): best frame match 35.3 s to replay capture 205003. In xemu the lamp at
  the upper left lights the facade beside it (warm brick, windows); ours stays dark.
- Street shot with all inputs (out/build/release/street/t42.5): the big left facade draws 666/670 (B0101311) and 691
  get lamps that project BEHIND the camera, ~190-285 units away (c73..c79 through the object's own WVP), none of
  them the lamp next to the facade. Other objects get sensible lamps (866 -> on-screen lamp, 1134/1140 -> 11-13
  units). The per-object lamp list (count [obj+0x418], positions from [obj+0x450], consumed at 0x245060) picks the
  wrong lamps for some objects. Next: trace who fills that list (lamp selection by distance / spatial lookup).
- Lamp list path: per-object light sets are global objects (0x42xxxx-0x48xxxx, count +0x70, 64-byte entries from
  +0x80, lamp position in each entry's last row, attenuation in its second row) copied into the renderer by
  0x240E70 (this = set, arg = renderer) to renderer+0x420.., then 0x245060 transforms each into object space.
  During the street shot most sets hold world-space street lamps (around -153..-178, -5, 1888..1903), but some
  hold small coordinates such as (0.1 0.1 -0.1), (2.2 0.4 9.2), (9.7 2.2 1.0): a different space or stale data,
  a candidate for the facade's far-away lamps. Not yet tied to draws 666/670. Next: log set pointer per draw
  (CaptureDraw could print the renderer's light set) to see which set the facade uses, then find who fills it.
- Facade light set traced (hooks, removed): the facade's object-space lamp (19.67 -0.31 5.94) comes from a set with
  4 world lamps at x -172.7..-177.9, z 1895..1903, while the lamps near the camera in this shot are at x -153 / -162,
  z ~1888-1890 (seen in other objects' sets). Sets are snapshots (0x2409F0, from env 0x36FE80 count +0x418, entries
  +0x420, 64 bytes) taken when each draw is queued; the world env count is set to 4 by the frame code (0xA834E ->
  SetLightCount 0x2446F0). The code that writes the env's 4 lamp entries per object is not found yet (many writers
  of +0x420; a guest stack scan at the facade snapshot gave stale return addresses: 0x240DDF, 0x137E52, 0x137EC6,
  0x13EF8C, 0x14B5B3, 0xA76C2, 0xA794B). Next: hook writes of the env's entries (find the function that stores
  64-byte lamp records into 0x36FE80+0x420) and check its nearest-lamp choice for the facade object.
- Lamp choice chain (traced with a page-protection write watch on 0x36FE80+0x420.., llvm-symbolizer on the DLL):
  world objects (e.g. 0x138888/0x138908) call 0x21B230 -> 0x21B170 -> picker 0x21AD60 every frame. The picker
  (this = global active lamp list [0x3F50B0] = 0x3705C0, count word +0x1000, 64-byte lamps) skips lamps out of
  (range + sphere radius) and keeps the top 4 by brightness / distance^2; its SSE (cmpeqss mask, movhlps sums) and
  sse_cmp are recompiled correctly.
- The active list is dynamic: 4 lamps near (-173..-178, 1895..1903) up to t~37 s, then (-173,1895) (-162,1888)
  (-178,1895) (-153,1890) by the street shot (t~42). No picker call after t 40 returns the old lamps, yet the facade
  (draws 666/670) is drawn at t 41.8 with a light set holding (-177.9,1902.9) and (-172.9,1903.4): the facade uses a
  CACHED light set (pool objects 0x46xxxx-0x49xxxx built by 0x2409F0) that is not refreshed. Sets carry a version
  word at +0x1C (from/to global 0x3F54CC, set from 0x136E70's return before queuing). Next: find what decides
  to rebuild a cached set (version compare) and why it never triggers here.

## Replay determinism (2026-11-02)

Superseded in part: the RNG and timing overrides below now run only with `XBE_DETERMINISTIC_REPLAY=1`
(see "Fresh review: runtime defects versus symptom guards").

- `XBE_PAD_RECORD=<f>` now also writes `<f>.timing`: per main-loop frame `v` (vblank step), `s` (audio sample step),
  `c` (check: random state ^ sim ticks, not replayed), plus `r` for every timer-based srand seed (`0x229D50`).
  Seed sites that repeat by themselves (positions / saved random restores: 0x16E2A 0x173D5 0x240D22 0x240E62 0x240F74
  0x240F8E) are skipped; they run thousands of times a frame.
- `XBE_PAD_REPLAY=<f>` reads `<f>.timing` too. Record + replay of 90 s: every `c` line and the present count per frame
  are identical. Setting both (replay + record to another file) and diffing the `c` lines finds the first frame two
  runs part.
- Captures at `p<N>` of the title menu still differ a little between runs: its background movie plays in real time
  and does not touch game state.
- Drift found 2026-11-02: `0x240CC0` (world object drawing, called from 0xA837C) draws one random number per drawn
  object. The number drawn follows streaming (real time), so the game's random state depended on timing. The hook now
  saves/restores the random state (`0x3376C8`, 8 bytes) around it. After that a 736-frame record+replay matched.
- Recordings made before this change cannot replay (user's lockup-4 recording parts at frame 5).
- Lockup 4 (logs/lockup-4: sample, full dump, recording): stuck again in the spawn wait loop 0x1E720.

## Lockup 5 (logs/lockup-184705, 2026-11-02)

Superseded: the release and allocator explanations below were not confirmed, and their guards were removed.
The actual cause was leaked file-object references (see "file-object references leak CRT streams").

- Stuck in spawn wait 0x1E720; spawn requests 1..7 in state 3, all on body slot rows world+0x28E4..0x2934 -> entry 28
  (hash 912C89FE). No disc jobs pending, callback ring empty.
- Entry 28: data and +4..+0x44 zero, job 0, mark -2, but buffer 82BEB000 holds the loaded, relocated file. So it was
  released (0xDB860 -> 0x21BFF0 clears the fields) while its load was still running: the read finished into the buffer,
  but nothing set the entry up again, and 0x7D780 (lookup by hash) keeps returning it.
- The 0xDB860 guard only protected entries with data set; it now also protects entries still loading.
- Lockup 6 (logs/lockup-185948): identical state (entry 28, 7 spawns), no "kept" line, so not 0xDB860. Real cause:
  allocator 0xDB4B0 (loader 0x374938, entries 28..39 for bodies) treats any entry with data 0 and no parent as free,
  including one still loading. A second request reset entry 28 mid-load. Hook now also skips entries with a disc slot
  (+0x5E >= 0) or a job (+0x3C), logging "loader slot N is still loading; not reused".
- Run after the allocator fix (logs/exit-191134): no lockup, but no "still loading; not reused" line either, so the fix
  is not proven yet. Crash at 471.9 s in 0x14CF70 (same as crash 4): grid cells come from the position at 0x370000;
  a garbage position overflows cvttss2si and indexes outside 0x386760. 3.7 s earlier the path-target guard fired.
  Hook now skips the grid update when that position is not finite or > 1e6 and logs the values.
- Run logs/exit-193829: crash at 429.7 s, null read at 0xA796D (main loop reads [0x348E10]+0x9C with audio-clock mode
  on but the object gone). 0.1 s before: "no ready head ... attach skipped". The sample-clock hook (called at 0xA786A,
  just before the mode check) now turns the mode off when the object is null.
- Lockup 7 (logs/lockup-194849): cutscene-mode fix worked at 429.8 s, then the same entry-28 lockup; allocator guard
  never fired, so it is not slot reuse either. It always follows "no ready head" at ~429.8 s (scripted moment).
  Added a trace ("tcnyc: body N ...") of allocate / load service 0xDBA10 / set up 0x21BD50 / clear 0x21BFF0 /
  release 0xDB860 on entries 28..39.

## 2026-11-04 · Review follow-up and HUD capture

- Hardened XISO extraction with extent checks, path containment and exclusive temporary files. Runtime disc reads validate extents before allocation and bound directory traversal. XBE boot rejects invalid TLS records, bounds the import scan and checks guest image reservation before copying.
- The launcher validates the selected ISO against the build's XBE fingerprint, copies on a cancellable worker and preserves existing images. The SDK patch was regenerated and passed a reverse-apply check.
- CMake, the first-launch compiler and the developer relink helper publish DLL stamps containing the executable, XBE and DLL hashes. The runtime rejects missing or mismatched stamps. Older unstamped installations require a rebuild. The relink helper checks its cached partitions against the developer build.
- Distance blur tracks multisampled depth per color target and carries the association through full-frame copies. Overwrites and target release invalidate the association. Camera FOV reads use the settings lock.
- Capture `203445` identified the black line as the last pixel of the hidden phone HUD sprite (draw 0270). The title hook changes only the hidden X key from -64 to -65 in the loaded retail HUD data. Draw 0224 uses the original Fighting Style HUD slot texture for the FS icon. Its reported connection to a settings change is now verified and fixed (next bullet).
- **Fixed: FS icon replacing the stamina icon after an MSAA or render-scale change.** The top-right HUD slot is a 104x60 render target the game draws into (`t0 83267C80 ... target` in captures `205448`/`210136`). `ApplySettings` cleared every target on a sample-count or scale change, so the game sampled that address as an ordinary texture and got the static red "FS" placeholder (captures `205456`/`210144`, no `target` marker). Target rebuilds are now lazy: a target that is only sampled keeps its contents across the change and is recreated only when it is next bound as a render target whose MSAA/scale differ. Verified in-game with both MSAA and render-scale changes.
- **Fixed: PC contrast banding on dark gradients** (the blue car's neon reflection and the character's trousers, captures `210849`/`210910`). The contrast curve's square-root toe had unbounded gain near black, turning low-level reflection detail and 8-bit steps into visible streaks. It is now a smooth exponential toe capped at 2x (the best balance found in testing), followed by an ordered dither before the back-buffer write. Verified by toggling PC contrast and in-game. The trousers' DXT1 source texture still carries some 4x4 blocking of its own.
- The release build, developer relink, native retail-disc parsing and all 12 regression checks passed. Isolated checks rejected missing or changed DLL stamps and confirmed the phone hook changes only the intended byte. The user's smoke test reached gameplay, switched to 4x MSAA with depth of field enabled and exited cleanly. Still to verify in-game are the phone fix, MSAA Off with depth of field enabled, and a clean first launch with the D3D12 debug layer.

## 2026-11-04 — Character LOD and PC asset import (pinned)

Pinned by request. Nothing in `assets/` is modified and no SDK hook is compiled in; this section records what was
learned and what the next step would be.

**Reported symptom.** A nearby pedestrian renders with a very low-poly body (capture `214052`, and the left-edge
figure in the 4:3 capture `214508`). It does not change with aspect, FOV, Bloom, DOF or PC contrast. The player model
and other pedestrians look normal. The user believes it is draw distance or LOD.

**Character LOD in the Xbox build.**
- Updated with captures `002209` and `002217`. The woman in the white top
  and gold trousers uses the same atlas address `811CF680` and shader in
  both frames. Draw 1547 submits 621 indices, while draw 1252 submits
  2004. Both counts occur in different mesh blocks of
  `LResChar/Bodies/Body_F_Sporty.xb2`. This confirms an in-asset mesh
  switch and supersedes the earlier conclusion that asset-set selection
  was the only switch.
- The optional `Play With High Detail NPCs.cmd` comparison redirected the
  ordinary low-detail body request at `0x7D976` to the retail `Char`
  body. All 98 installed low-detail bodies have a counterpart. The
  Sporty counterpart has one mesh block with 3469 indices. Seven of the
  98 counterparts have two blocks, so this is an asset-set experiment,
  rather than a general change to the LOD threshold. Heads and the
  separate low-pedestrian asset are outside this hook.
- The first comparison build crashed at `0x7D97B` on a null guest path.
  This loader also accepts a file ID with a null filename. The hook now
  checks the guest address before translating it and leaves those calls
  to the original loader.
- The comparison was removed after capture `003040` still showed a
  598-index body (draw 1561, atlas `81170180`, matched to
  `Body_F_Casual_Heavy`) and the user reported fewer or disappearing
  pedestrians. The frame and session log do not establish the cause of
  that population change. The normal loader is restored. The next
  investigation should trace selection among the loaded body meshes.
- The shader draw trace led through `0x21CF50` and `0x21DC00`.
  The scene walker compares camera-space depth to entries in the node's
  mesh list, following the next entry when depth exceeds a threshold.
  The standalone selector at `0x21C330` does the same. The opt-in
  `Play With Longer NPC Detail.cmd` test halves the depth passed to
  these selectors for body loader slots 28–39. It keeps the current
  assets and streaming path. The user still saw low-detail NPCs with
  this build. Investigation is pinned at the user's request. Both NPC
  experiments and the temporary draw trace were removed from the release.
  The captures and selector addresses above are the starting point if
  the work resumes.
- The game asks the file layer for `D:\Data\Lreschar\Bodies\BODY_*.XB2` and `\Heads\HEAD_*.XB2` when pedestrians
  spawn. This was proven by logging the open paths (see the hook note below).
- `uLODStart` in the `BodyHeadInfo` table (`0x3520F0 + (id-0x85)*0x58 + 0x14`) is read only by `0x7D350`, whose
  single caller (`0xFB...`) copies it into a vector property (`0x21F420`, hash `0x7789F8A9`). It does not choose a
  mesh, so it is not the per-object LOD switch.
- The only character detail switch is the asset set: `LResChar` versus `Char`. The container's chunk 0 (mesh) has
  `3` sub-blocks in `LResChar` and `1` in `Char`; the single `Char` mesh is larger than all three `LResChar` blocks
  combined (e.g. `Body_F_Bum`: 119,488 vs 89,312 bytes).
- Forcing every character open from `LResChar` to `Char` is safe and was verified in game (24 body types redirected
  in one session). The visual gain is small because the Xbox `Char` asset is only ~10% larger than `LResChar`
  (e.g. `Body_F_Casual_Heavy`: 192,896 → 213,120 bytes). There is no higher-detail Xbox character asset to switch to.

**PC assets are the only higher-detail source, but they are not drop-in.**
- PC character bodies are ~5x larger (`Body_F_Casual_Heavy.pcf` 934,400 B vs Xbox 213,120 B).
- Textures are the same resolution: PC is 256x256 DXT1, Xbox is 256x256 palettised (P8) plus some 128x64/32x32
  detail maps. Texture replacement therefore does not add detail; the gap is geometry.
- The container layout is shared (12-entry chunk table at `0x0C`, stride 8; chunk 0 mesh, chunk 1 materials,
  chunk 2 texture table), but:
  - PC texture records are 0x28 bytes with twelve `0xCD` bytes where the Xbox records (0x24 bytes) carry runtime
    fields. Those fields are cooked per file, not content hashes, and cannot be copied from another file.
  - PC meshes use an 88-byte vertex; Xbox meshes use 104 bytes. The Xbox stream is reordered and carries 16 extra
    bytes of packed normal/skinning data per vertex (PC vertex 1's position bytes appear inside Xbox vertex 0), so
    this is not a stride change or a chunk splice.

**Experiments (all reverted; originals backed up under `out/asset-backup/`).**
1. Raw PC `.pcf` copied over the Xbox `.xb2`: crash, `read 1CDF880F0`, `edi CDCD0000` — the `0xCD` runtime fields.
2. Hybrid (PC data with the Xbox texture-table runtime fields copied in): crash, `read 139680F1`,
   `edi 136B0001` — the fields are layout-dependent, not hashes.
3. Mesh splice (Xbox container and textures, only the PC mesh chunk inserted, later chunk offsets shifted): the
   loader accepted it, but the renderer crashed reading the PC vertex stream (`read 3DE72DFD`, stack `0x243EB2`).
4. Texture-only replacement via the custom texture pack was investigated: the matched PC and Xbox atlases have the
   same resolution, so it does not address the low-poly silhouette.

**Scripts kept for the follow-up** (all operate on `assets/data`, back up before writing, and have a restore path):
- `scripts/pc_char_swap.py` — copy PC character bodies/heads over the Xbox ones; `apply LResChar|Char`, `restore`, `status`.
- `scripts/pc_hybrid_body.py` — single body: PC data plus Xbox texture-table runtime fields.
- `scripts/pc_bulk_body.py` — same for every body/head (463 converted, 2 skipped: `Body_F_Int_Dominatrix`,
  texture-count mismatch; `Head_Invisible`, no textures); `restore`, `status`.
- `scripts/pc_mesh_splice.py` — Xbox container, PC mesh chunk only.
- `scripts/pc_texture_match.py` — decode PC containers, match dumped Xbox textures, write custom-texture replacements.
- PC container facts used by the scripts: header `09 00 00 00 | 06 00 00 00 | 6c 00 00 00`; chunk table at `0x0C`;
  entry `(u16 id, u16 count, u32 offset)`; mesh sub-block header `10 00 00 00`, size, `42 00 00 00`,
  `01 00 00 00`, magic, FVF, stride; PC texture records stride `0x28`, Xbox `0x24`.

**Diagnostic hook used (removed again; re-add only with the SDK patch).** In `sdk/runtime/kernel/files.cpp` an
`ApplyCharOverride` rewrote `\LRESCHAR\` to `\CHAR\` in `NtCreateFile`/`NtOpenFile`, and `TraceCharOpen` wrote
`char asset request ...` / `char override ...` report lines. Gate both on environment variables
(`XBE_FORCE_CHAR_ASSETS`, `XBE_TRACE_CHAR`) if re-added; release logs drop Info/Warn, so the trace must use `LOGR`.
`patches/xboxxberecomp-local.patch` must be regenerated if the hook is kept.

**Next step if this is unpinned.** Write a real vertex converter: decode both streams, map the shared 52-byte prefix
and the packed normal/skinning fields, re-encode PC vertices into the 104-byte Xbox layout, and rebuild the mesh
chunk with the Xbox header. Validate on one body (`Body_F_Bum`) before batching. The same approach is needed for
vehicle and city meshes, which share the container but also differ in vertex size. Budget for the 64 MB guest
memory: PC bodies are ~1 MB each.

### City LOD distance experiment (same day, reverted)

Tried the "make higher-detail city meshes load farther away" option and traced the render path with a temporary
D3D8 draw tracer:

- The indexed draws come through `0x20C470` (per mesh) ← `0x1389B0` (draw list at renderer+0x6438, count +0x6634)
  ← `0x142C90` (scene submit) ← `0x13E5A0`.
- Each city cell is drawn by `0x13C4C0`. It selects the LOD/node with the index at `[cell+0x90]` and the 0x40-byte
  node table at `[cell+0x34]`; nodes are chained `0 → 1 → 2` in the model files.
- Hooked `0x13C4C0` to log the replaced index and force `[cell+0x90] = 0` (highest-detail node) for every cell.
- **Result: the visible line / low-detail building did not change.** So that artifact is not city mesh LOD, and
  raising a mesh-LOD distance will not fix it. The remaining candidates are the projected light/reflection composite
  or a texture edge, not geometry LOD.
- Hook and tracer removed again; no source change remains.

### Broadway / W 46th façade seam (capture 231610)

- The marked façade is below the purple billboard. The dividing line is near x=333 in the
  640x480 capture, with the lower-detail surface to its right.
- The whole façade has visible detail before the distant-city pass. Compare
  `1685_83DA4000_s.bmp` and `1686_83DA4000_s.bmp`. The change occurs across the
  `B0000C21` depth and colour pair (draws 1686–1687), using the 128x128
  texture `82AC4A00` and the restricted screen-depth range 0–70749. The seam is
  already in the scene target before post-processing.
- Earlier detailed geometry writes depth to the same surface. The distant pass uses a
  different mesh and a 0.99 viewport depth scale. The title sets up the restricted
  clip range in `0x13F090` and later restores the full range. The capture isolates
  the overwrite, but does not establish whether the original Xbox shows it.
- A rendering change needs a matched comparison that checks both the overlap and
  distant areas where only the simplified geometry is resident.

- The user confirmed the seam also occurs in the original game and verified that
  moving the clipped distant-city pass farther back fixes it at Broadway.
- Opening cutscene capture `234912` exposed a regression. Draws 995–1008 use
  the distant-city program with a clip maximum of 524288, the original full
  24-bit depth range divided by 32. The dark wedge appears in the main scene
  target by draw 1025, before bloom and the final composite. The port now
  applies the façade adjustment only to the restricted pass outside
  audio-clock cutscenes. The cutscene and unrestricted passes use their
  original depth. In-game confirmation of this correction is pending.

- For a fix that also works in cutscenes, `scripts/trace_city_meshes.py`
  matches packed positions from a frame capture to retail mesh containers.
  Broadway draws 1152–1154 and 1941 match the detailed container at
  `Brick_NYC.exm+0x6CA800`. The overwrite at draw 1686 matches
  `Brick_NYC/0_-5.xb2+0x348`; another batch from that tile begins at
  `+0x3AB0` in draw 1688. The detailed container and the tile use different
  node identifiers. The adjacent draw 1685 matches `-1_-5.xb2`.
  Matching a whole tile would affect multiple batches, so it does not
  establish which individual surfaces should have priority.
- The opening capture `234912` lacks vertex inputs for draws 995–1008.
  Collect the same shot with `XBE_CAPTURE_INPUTS=1` to identify the
  opening geometry and separate it from the façades that need priority.
- Opening capture `235803` shows the skyline and hillside without the
  dark wedge. The adjustment was active in this cutscene frame. Draws
  997–1010 use the scaled clip maximum 17685 and match five city tiles
  spanning `-2_11`, `-1_9`, `-2_9`, `-1_10` and `-2_10`.
  The earlier dark frame used the scaled full depth range. Scope the
  adjustment by the pass's clip range in both gameplay and cutscenes.
  The full-range pass retains original depth because it includes geometry
  outside the restricted distant band. This is a pass-based rule, with
  no inferred pairing between individual mesh surfaces.
- Capture `000135` shows the reported left façade before the adjusted
  distant-city draws 739–752. Sampled façade pixels change by draw 600,
  with little change across the adjusted pass. This differs from the
  Broadway overwrite. The installed PC widescreen and DX wrapper INIs
  expose distant blur and mip generation, but no city LOD or streaming
  controls. The PC city tiles have corresponding node IDs and different
  mesh payloads. For example, `0_-5.pcf` is 356352 bytes versus 248448
  bytes for `0_-5.xb2`. The PC city index differs too. A matched PC
  opening shot is needed to establish whether the façade differs there.
- The user clarified that the PC reference is the building immediately
  right of the fire escape. In capture `000135`, the neighboring wall
  behind the streetlamp appears between snapshots 575 and 600, already
  showing the dark stretch beside the poster. It remains visible through
  snapshots 725 and 750, across the adjusted distant-city draws. Draw
  593 uses the matching Expression poster texture `82887000`, an anchor
  for tracing the neighboring wall draws. The capture saves every 25
  draws, so it does not isolate the wall's exact draw yet.
- The user's red outline identifies the lower brick row in front of the
  poster wall. In `000135` the red end wall and near windows are present
  by draw 600. Between snapshots 725 and 750 the restricted distant-city
  pass changes pixels on the receding end of the row and the roofline.
  The near end remains visible. The earlier poster-wall finding does
  not identify the draw responsible for this row's transition.
- Every-draw capture `001129` isolates the overwrite to draw 747. The
  snapshot numbered 748 is the first image after that draw. The mesh
  comes from `-1_10.xb2+0x3E00`. At (426,263), detailed draw 567 writes
  brick and depth from `Brick_NYC.exm+0x8D3800`; draw 747 replaces the
  resulting RGB (41,28,24) with (18,19,19). The earlier clip adjustment
  was active (maximum 17685), but c-38 still held the stock 0.99 viewport
  scale. The new adjustment scales host Z after vertex-program depth
  clipping for the restricted pass. This preserves the guest clip
  coverage and actually moves overlapping proxy geometry behind the
  detailed geometry. Capture logs mark affected draws with `depthscale`.
  The user reported that the brick-row issue appears fixed in-game
  with this build.


## 2026-11-05 - Effect-batch crash: guest-thread race investigation

A release build crashed after ~16 minutes of play (`C0000005`, guest thread 0x14). The log and the
minidump (`TrueCrimeNYC.2026.11.05_02.12.22.dmp`) agree on the fault:

- Read of guest address `FFFFFFF8` (i.e. -8), in guest function `sub_00141D40` at `00141EF0`
  (`mov al, [edx]`). DLL-relative fault address `+0x3DFD49`.
- The function walks up to nine polymorphic objects at `renderer+0x55C0` (stride 0x80; reset by
  `0x13D880` through the virtual `0x137420`). Each object carries a count `+0x70`, a data pointer
  `+0x74`, a world box `+0x30`, a vertex base `+0x54`, and walks 32-byte element records. It is an
  effect/particle-style batch builder, not the city mesh LOD path.
- The computed figure was `+0x74 + index*32 + 2`; host registers show the `+0x74` field held
  `FFFFFFF6` (-10), so the first byte read landed at -8. Nothing in the image writes -10 as a
  pointer, so the object's field was corrupt or the object was reinterpreted as another subclass.
- 0x80000000 in the guest index register is the shifted index term; it does not by itself prove a
  NaN conversion.

**Root-cause candidates, in order**

1. Guest threads run in parallel: the runtime gives every guest thread its own host thread
   (`kernel/threads.cpp`), while the Xbox is a single-core Pentium III and never runs two guest
   threads simultaneously. A thread that recycles one of the nine polymorphic effect objects
   (count update, vtable swap, subclass field layout) while the frame thread iterates it makes
   `+0x74` a different field - exactly the small integer seen here. The 16-minute delay and the
   absence of a fixed scene fit a timing race.
2. A genuine use-after-free in the title's effect lifecycle (same signature, but expected to
   reproduce at a fixed scene).
3. Uninitialised/stale effect data producing an out-of-range index.

**Diagnostics added**

- `LogGuestStack` now walks the EBP chain (`LogGuestFrames`) and prints real guest frames; the raw
  stack scan it also keeps reports stale return addresses and should not be read as a call stack.
- The crash handler logs the module base and DLL-relative fault offset, and writes three guest
  snapshots beside the minidump: `guest-<time>-fault.bin` (pages around the fault address),
  `-stack.bin` (128 KB below the faulting thread's stack) and `-renderer55c0.bin` (the nine effect
  objects) so the post-mortem no longer depends on guest RAM being absent from the dump.
- Experimental `XBE_SERIAL_GUEST=1`: only one guest thread executes guest code at a time, with the
  lock released for HLE calls (where the console scheduler would switch). A watchdog reports a lock
  held for more than 5 s. Default off; enable it to test the race hypothesis.

**Next step.** Reproduce on the user's save with and without `XBE_SERIAL_GUEST=1`. If serial mode
removes the crash, choose between keeping serialisation (measure the frame-time cost) and locking
only the effect-object lifecycle. If the crash is deterministic at one scene, treat it as a
data/lifecycle bug instead and add a validity check before the batch traversal.

### Update (same day): the crash is deterministic, not a race

The user's second and third runs (normal and `XBE_SERIAL_GUEST=1`) crashed with identical registers,
stack pointer and effect-slot contents; the serial run's dump even has one extra thread (the
serial-mode watchdog), confirming the mode was active. So guest-thread parallelism is not the cause.

- The corrupt slot is slot 8; only +0x70..+0x7C differ. The activation method `0x13C8F0`
  derives +0x70/+0x74 from the data block at +0x1C: `+0x70 = block + [block+0x18]`,
  `+0x74 = block + [block+0x20]`. Slot 8's block is `City\Brick_NYC\1_-8.g` (id 201) and its real
  fields are `+0x18 = 0x80`, `+0x20 = 0x15C0`, so activation should have produced
  `0x8150E080`/`0x8150F5C0` - the same pattern every healthy slot has. The other derived
  fields (+0x54/+0x5C/+0x68/+0x6C) match that same block exactly.
- Therefore the last 16 bytes were overwritten after activation. +0x78/+0x7C are not written by
  any effect method (padding), so the runtime values there are stale allocation content; the
  meaningful corruption is +0x70/+0x74.
- Diagnostic-only additions (no game changes): the crash dump now also keeps the effect manager
  region (`-effectmanager.bin`) and the first 0x200 bytes of each active slot's data block
  (`-blockN.bin`); the activation method logs when it produces an invalid pointer; and
  `XBE_EFFECT_WATCH=1` installs a hardware write watchpoint (debug register DR0) on slot 8 +0x70
  in every guest thread. The process VEH logs the writing instruction's module offset plus the
  guest stack, so the writer can be symbolized against the shipped PDB. No guard or state change
  is shipped.

**Watchpoint verification on the developer machine.** With `XBE_EFFECT_WATCH=1` the batch builder arm
points DR0 at slot 8 +0x70 in every guest thread. The log then showed the expected activation and
deactivation writers (`0x13C8F0` at module +0x3C73F7, `0x13C840` at module +0x3C71A9), and the
activation log printed correct pointers (`block+0x80` / `block+0x15C0`) for every slot, confirming
that a bad slot comes from a write *after* activation. The activation validity test was corrected to
use `mem::IsCommitted` alone: effect blocks live in the contiguous 0x8xxxxxxx alias, which is valid
guest memory but outside the user range 0x10000..0x7FFE0000.

**Diagnostic packaging gotcha (same day).** The first two diagnostic zips shipped the exe, DLL and pdb
but not `TrueCrimeNYC.dll.stamp`. Without the stamp the launcher rebuilt the DLL from the XBE using the
user's existing `compiler/TrueCrimeNYC.lib`, which predates these hooks: lld then bound the generated
`sub_...` symbols locally instead of importing the executable's exports, so every hook in the EXE was
silently bypassed (and no `watch:` line appeared). The fix is to ship the regenerated
`TrueCrimeNYC.dll.stamp` (host/XBE/DLL SHA-1s) so the launcher loads the shipped DLL, plus the current
`compiler/TrueCrimeNYC.lib` as a fallback if a rebuild is ever forced. The EXE now logs
`diagnostics: effect watch enabled|off` at startup so this is unambiguous. This also explains why the
earlier city-LOD hook had no visible effect: its strong override was never called in the user's
install.

**Root cause found and fixed (same day).** The hardware watchpoint on slot 8 +0x70 caught the writer at
module +0x3F6CA8, i.e. guest `0x1472E0` at `0x1473EE..0x147403`:

    index = ((coordA + 1000) % 3) + 3 * ((coordB + 1000) % 3)
    dest  = cellManager + (index + 0x49) * 16

`cellManager` is the nine-cell object at renderer+0x55B0 and `+0x490` is its 3x3 table of 16-byte cell
records (right after the nine 0x80-byte cell slots). At the crash spot `coordA` is below -1000, so the
signed `idiv` remainder is -1 and `index` becomes -1: the record is written at `manager+0x480`, which is
exactly slot 8 +0x70. That overwrites the slot's data pointer with `[esp+0x1C]`/`esi+ebx` from the record
(0x80000000 / 0xFFFFFFF6), and the cell batch builder then faults at guest -8. The values in the record
match the other records of that table; the tile blocks and activation are intact.

Only the remainders are consumed (the quotients are overwritten immediately), so the fix widens the
offset instead of adding a guard: `+1000` becomes `+0x100000` in all ten copies of the routine
(`0x146CA0`, `0x146CC8`, `0x146D66`, `0x146FC0`, `0x146FE8`, `0x1472E0`, `0x147308`, `0x138B90`,
`0x13F090`, `0x14A240`; 20 sites). `0x100000-1000` is a multiple of three, so every coordinate that
worked before yields exactly the same remainder, while coordinates down to -1,000,000 now produce the
correct 0..2 index. `scripts/patch_grid_offset.py` reapplies the edit after a clean recompile of the XBE.
Build `2026-11-05-r4` contains the patched DLL; the user is testing the crash spot.

**Offset widening was not enough (watch run).** With the r4 DLL loaded (verified from the module
timestamp in the dump), the watchpoint fired at the same store and its frame showed the reason:

    watch: hit +3F6CA8 value 80000000
    guest eax 00577E40 ecx FFFFFFFF ... esp D007AC90
    stack+18 005779C0 80000000
    stack+20 FFFFFFFF 3E94672C FFFFFFF7 ...

`ecx = -1` is the computed index and `[esp+0x20] = -1` is the signed `idiv` remainder. The cell
coordinate here is far below -1,000,000, so moving the +1000 offset to +0x100000 did not make the
dividend positive. The correct fix is the remainder itself: the grid wants the mathematical modulo
(0..2), but signed `idiv` returns a negative remainder for a negative dividend.

All 27 `idiv` blocks in the ten grid routines now add the divisor when the remainder is negative.
For every non-negative dividend this is byte-for-byte the same value as before; for a negative one
it yields the intended 0..2 bucket instead of a negative index. The original +1000 constants are
restored. `scripts/patch_grid_offset.py` applies this after a clean recompile; build
`2026-11-05-r6` contains the corrected DLL.

**Final fix for the grid index (build 6).** The watch frame on the r5 build showed `ecx = -1` and
`[esp+0x20] = -1`: the first division's signed remainder is negative. Widening the +1000 offset only
moves the boundary. The fix now makes the two **index** divisions use the mathematical remainder
(add the divisor when the signed remainder is negative). This is identical to the original for every
non-negative dividend and yields the intended 0..2 bucket for a negative one; the quotients are
discarded. The sub-position division (`A/3` used for a float offset) and all `value * 1000` divisions
are deliberately left signed - changing them produced a new null-pointer crash at 0x3BC90 during
level load in local testing, so only the index divisions are corrected (20 sites in the ten grid
routines). `scripts/patch_grid_offset.py` applies this after a clean recompile. Local 55 s smoke test
is clean; build `2026-11-05-r6` is with the user for the crash-spot test.

**Confirmed fixed (same day).** The user played the crash spot with build r6 and it no longer crashes. Build 6 corrects the
signed remainder only for the two divisions whose dividend is `coordinate + 1000`, which the 3x3 index uses; the
sub-position division and unrelated `*1000` divisions stay signed. Local A/B testing had shown that correcting those
other divisions caused a new null-pointer crash at 0x3BC90 during level load.

**Durable fix for rebuilt game code (review follow-up).** `generated/` is ignored and is regenerated from the XBE, so the
source edit alone would not survive a clean build. The correction now has one implementation,
`sdk/runtime/xbe/grid_patch.h`, used by both paths:
- `sdk/tools/grid_patch` runs in the CMake generation step after `xbe_recomp` and before compilation, with the tool
  itself as a dependency, so every CMake/IDE/release build produces corrected sources and changes to the fix rerun
  generation.
- `runtime/app/game_code.cpp` calls the same header after `xbe_recomp` finishes in the first-launch compiler, before
  clang compiles the sources. No Python is involved.
The patcher names the 12 confirmed guest sites explicitly, verifies the idiv operand and divide-error trap at every
occurrence, requires the expected 20 occurrences across the configured partitions, and publishes each file through a
temporary file. Any missing file, changed instruction or wrong count stops the build instead of producing a partial
patch. `scripts/test_grid_patch.py` exercises the fixture, idempotency and drift failures; `scripts/test_guest_frames.py`
covers the guest frame walker and the `0xD0...` stack range. The earlier `scripts/patch_grid_offset.py` reference script
was removed so there is no second, divergent transformation.

### 2026-11-05 · Review completion and failure-path verification

The renderer now reports whether swap-chain back buffers remain usable after resize. A recoverable resize failure reacquires the original buffers and retains their dimensions. Failure to acquire views or device loss stops the process before further frame commands are submitted.

Grid patching validates every partition and all expected occurrences before staging changed files. Staging uses exclusive temporary files. Each original is replaced atomically without first deleting it. If publication stops partway through, each partition remains complete and retry accepts already-corrected partitions. The fixture checks that rejected inputs remain untouched and that a blocked replacement preserves sources and can be retried.

F3 stack logging explicitly passes the inspected guest thread to the frame walker. It therefore uses that thread's allocated stack bounds even when called from the window thread. The frame fixture models a caller with no current guest thread.

Watchpoint thread IDs are atomic, cleared before thread exit, and checked again after suspension before debug registers are changed. Helpers retain the thread object while installing the watch. The debug-register length encoding was also corrected so an 8-byte watch covers 8 bytes. A native Windows fixture installs the watch on live worker threads and skips workers that have exited.

Verification completed with the release build, all 16 regression checks, a matching executable/XBE/DLL stamp, and a reverse-apply check of the regenerated SDK patch. The grid failure/retry test was also run separately after its assertions were extended. In-game F3 capture and window/display-mode changes remain manual checks.

## 2026-11-05 — Experimental building seam toggle

The F10 Graphics menu now includes Building seam fix (Experimental), default On.
It saves as building_seam_fix in settings.toml and applies on the next city pass.
Off restores the original depth scale. Reset defaults enables it again.
The native Options > Display tab has no additional row.
Release build and all 16 regression checks passed. Live switching remains a manual check.

### 2026-11-05 · Experiment removal and folder cleanup

Removed the five PC character conversion/matching scripts after retiring the experiment. Compared all 465 installed character body/head assets against the retail ISO and found no differences. Original asset backups remain available for recovery.

Fixed fallback mouse vertical sensitivity, bounded loaded output/render settings and checked target dimensions against D3D12 limits, and made the compiler process owner close every child handle on completion or failure. Diagnostic packaging validates inputs before staging and publishes a completed ZIP from unique staging, preserving an existing package when required inputs are missing.

Moved 106 historical capture/log folders and old diagnostic package files into archive/2026-11-05. cleanup-manifest.csv maps every original path to its archive location and identifies retained current captures and r6/r7 comparison packages. The archive is ignored by Git. This reorganizes evidence on the same drive and does not reclaim its disk space. Saves, asset backups, active release files, reference tools and both game installations remain available.

Verification completed with the release build, all 17 regression checks, SDK patch reverse application and a packaging check that missing inputs preserve the previous ZIP. Existing uncommitted graphics-option changes were retained. Further deletion or consolidation of installations requires choosing which evidence and saves to retain.

### 2026-11-05 · Correct PC keyboard actions for the Xbox layout

Corrected keyboard action translation after checking the documented Xbox button functions. Space and G now send Y for jump/climb, Num 7 / Ctrl / side mouse button 1 send A for light attack, Num 9 / side mouse button 2 and the optional heavy key send X, and + sends B for grapple. R sends Black for reload/block, E sends White for use/vehicle entry, and Q sends Back for badge/horn/siren. Enter confirms menus and Backspace cancels.

The title supplies active gameplay camera vehicle mode to the keyboard reader. In that mode W sends A for acceleration, S sends X for brake/reverse, Space sends B for handbrake, and A/D steer. Shift right-stick click and middle/right mouse left-trigger assignments were explicitly retained at the user's request. Prompt glyphs now match the corrected buttons, including Shift instead of the removed V alias.

The release build and the keyboard/mouse fixture passed, including checks of every corrected button, badge, the retained aim keys and driving pedals. Live gameplay verification remains needed for vehicle transitions and contextual actions.

### 2026-11-05 · Approved combat keyboard layout

Light attack now uses Left Ctrl or mouse side button 1. Heavy attack uses Left Alt or mouse side button 2, with the existing optional custom heavy key still available. V sends the contextual frisk/grapple/arrest/human-shield action. Removed the previous Num 7, Num 9 and + combat defaults and the F fire alias. E use, R reload/block, C crouch and the user's preferred aim/lock assignments remain available. Menu confirmation/cancellation use Enter/Backspace.

Updated combat glyphs to CTL, ALT and V. Suppressed Windows Alt menu activation while preserving Alt+F4. The keyboard fixture verifies both side buttons, the primary keys, the absence of the F fire alias, and the window-message handling. Gameplay verification remains needed for contextual frisk and grapple behavior.

### 2026-11-05 · Cleanup deletion and combat verification

At the user's explicit request, all 109 entries in cleanup-manifest.csv were subsequently deleted, including the 106 archived entries and the three entries originally marked retain (the capture directory and diagnostic r6/r7 staging directories). Verified that every listed target is absent. The manifest now records the deleted paths; the earlier archive-only description above is historical.

The final Left Ctrl / Left Alt / V combat layout passed the release build and all 17 regression checks. Keyboard fixtures exercise the mouse side-button alternatives, primary combat keys, badge, driving pedals, preserved aim/lock keys, and Alt menu suppression with Alt+F4 still available. The SDK patch passed reverse application. README.md describes the resulting bindings. In-game contextual frisk/grapple and vehicle transitions remain manual checks.

## 2026-11-06 - Native Linux port: current package and road-artifact investigation

### Delivered package

The native port is the delivery path: an SDL3 + Vulkan ELF that boots the recompiled game from `assets/`, an ISO, `--assets`, `--iso` or a positional path. The current package is:

- `dist/TrueCrimeNYC-linux-x86_64.tar.gz`
- size 11,098,881 bytes
- SHA-256 `e3749a0c936e1c22a90e6c23424b3952f19922286fa29074b6e1ef5f63e26bcd`
- requires glibc 2.38+, so SteamOS 3.6+; bundles `lib/libshaderc.so.1` with an `$ORIGIN/lib` runpath
- validation runs: 0 Vulkan validation errors through the title and gameplay, including 4x MSAA,
  SMAA, bloom, DOF and a 21:9 aspect

The Windows executable is unchanged (`SHA-1 8511AD8588F0CA6A569D6D47B508DF00805C4A1E`); `TrueCrimeNYC.zip` was refreshed only for the updated docs (74,667,561 bytes).

### Renderer parity work in this pass

- Blend factors now follow the console GL values, including the constant-alpha factors `0x8003`/`0x8004` the light-buffer/road chain uses. The dynamic blend colour (`D3DRS_BLENDCOLOR`) is supplied for every draw and the pipeline includes `VK_DYNAMIC_STATE_BLEND_CONSTANTS`. This corrected the original white road band on the title screen.
- Solid polygon offset (depth bias) and the D3D12 sampler behaviour (mip LOD bias, anisotropic setting, shadow samplers without anisotropy) match the Windows renderer.
- Raw guest vertices (`XBE_RAW_VERTICES`), the render thread (`XBE_RENDER_THREAD`), descriptor-set ring reuse, target recreation only on size/format/pitch changes, full mip-chain uploads, BC block alignment, content-signature texture refresh, render-target feedback copies and the render-target sampling offset (`kTexTarget`/`rtshift`) are in place.
- The present pass applies the title gamma ramp and the PC contrast curve; CPU-written frames (movies, legal screen) wait for the render thread before the guest decodes the next one.

### Diagnostics added

- `TCNYC_TRACE=1` plus **F3** saves `logs/trace_<frame>.tga` (guest front buffer), `logs/trace_<frame>_present.tga` (the finished swapchain image, after gamma, contrast and the overlay) and `logs/trace_<frame>.txt` (every draw with target, textures, blend factors/constants, depth and cull state).
- `TCNYC_TRACE_AT=<frame>` captures a frame without pressing F3; `TCNYC_TRACE_EVERY=<n>` samples a run.
- `TCNYC_DUMP_TARGETS=1` writes every live render target for a capture. `TCNYC_DUMP_TEX=<hex,...>` writes named source textures.

### Road-artifact findings

The earliest 11 F3 captures (frames 262, 646, 882, 961, 974, 1155, 1185, 1320, 1345, 1425, 1465) had correctly textured roads and no white band. Later captures reproduce the bad frames, but the cause is not the road texture draw:

- In the worst frames the whole road is a flat gray/white band. The 320x240 downsample (e.g. `813A9000`) and the 256x256 accumulation buffer (`81AF8000`) already contain that band, and the final composite (`src = 0x304 DST_ALPHA`, `dst = ONE`) adds it to the main scene. The light/reflection chain is therefore the source, not the scene geometry.
- Some night/rain shots (e.g. frame 1112) are legitimate wet-road reflections of the bright sky and street lights; those were previously being read as the same artifact. The defect is the hard-edged daytime variant.
- Checked against the D3D12 backend and found matching: constant-alpha blend translation and dynamic blend colour, fog constants, target formats, sampler state, and vertex element formats in title/gameplay (no unknown element formats encountered).
- The software reference renderer crashes (`EXIT 139`) when the title 3D scene starts, so it could not be used as a ground-truth A/B for these shots.

Still open: identify whether the band is introduced in the 320x240 downsample, the 256x256 accumulation, or the final composite on the failing hardware. The next capture should come from the Deck with `TCNYC_TRACE=1 TCNYC_DUMP_TARGETS=1`, pressing F3 on the artifact, and include the guest TGA, the present TGA, the draw list and the `target_<frame>_*.tga` files.

Parked (2026-11-06, at the user's request): the band framing was not resolving the actual complaint, and no concrete cause was proven for the road texture itself. The Linux-only changes kept from this pass are the D3D12-parity fixes that are defensible on their own: a compressed/cube/volume texture that shares a render target's address now samples the guest texture (D3D12 already does this), the missing visibility/occlusion-query path is implemented, and an oversized upload ring grows instead of ending an open render pass mid-draw. Shared renderer files and the Windows executable were deliberately left unchanged.

Parity plan Phases 1-4 are implemented in the Vulkan backend (render scale and
stretch; MSAA with colour and depth resolves; SMAA 1x using the reference shader
and its area/search lookups; bloom, depth of field and the widescreen UI squeeze
in the present pass). Targets carry guest and host extents, are created at the
Graphics menu's percentage/aspect, scale viewport/scissor, mark sampled targets
`kTexWide` with the shared `spreadtap` values, rebuild live when a setting
changes, and scale target uploads/readbacks to and from guest memory. MSAA uses
render-pass resolves (depth `MAX`, or `SAMPLE_ZERO` where the driver only offers
that), pipelines key on the sample count and the resolved image is what gets
sampled, read back and presented. SMAA and the post chain sample the resolved
scene. Validated at 25/50/100% render scale, 4x/8x MSAA requests and 16:9/21:9
aspects with 0 Vulkan validation errors.

Texture packs and dumps are also implemented, in
`platform/linux/texturedumper_linux.{h,cpp}`: the same key, folders and file
names as the Windows backend, PNG via the vendored stb headers, DDS
(DXT1/3/5/DX10) natively, an in-process DXT decoder for PNG dumps, asynchronous
loading and a polled directory signature for pack changes. Dumping a full run
produced 541 PNGs whose level 0 matches the DDS dump of the same key exactly,
and a replacement pack (every dumped texture made solid magenta) changed the
whole scene, confirming the swap path. The stb headers are noted in
`sdk/THIRD_PARTY.txt`.

Console point/line expansion and point sprites are now geometry stages as well:
point/line pipelines get `PointHlsl`/`LineHlsl` compiled with shaderc (a
geometry-stage copy of `VOut` without `SV_ClipDistance0`, because glslang's HLSL
parser crashes on the struct copy), and a program's point list with point sprites
enabled rewrites stage 3 to stage 0 and expands each point by `tc3.x` (oPts) with
`SpriteHlsl`. Devices without geometry shaders keep the previous native
points/lines. The shader-stage flags on the b0/b1 descriptor bindings gained
`VK_SHADER_STAGE_GEOMETRY_BIT` for this.

One root cause worth remembering: the Vulkan instance used to request 1.1, and
the loader hides 1.2+ entry points from `vkGetDeviceProcAddr` in that case, so
`vkCreateRenderPass2` resolved to a null trampoline. The instance now negotiates
up to 1.3 and MSAA only enables when the render pass 2 path is available.

The current package after this pass is `dist/TrueCrimeNYC-linux-x86_64.tar.gz`
(11,098,881 bytes, SHA-256
`e3749a0c936e1c22a90e6c23424b3952f19922286fa29074b6e1ef5f63e26bcd`). The
Windows executable is still unchanged.

### Remaining parity gaps versus Windows

- HDR/range targets are not modelled; the title's scenes keep range 1.
- Without `textureCompressionBC`, BC1/2/3 guest and replacement textures are decoded
  to BGRA8 rather than falling back to the white 1x1 texture.
- The software reference renderer has no Graphics overlay.

## 2026-11-06 - Review fixes

The full code review found and the current tree fixes these correctness issues:

- Windows right Ctrl/Right Alt use real `VK_RCONTROL`/`VK_RMENU`; the mouse regression
  harness now includes `ApplyMouseDelta` and the platform constants.
- Linux SDL events have one owner (the window thread); guest threads no longer pump or
  drain the queue, joystick hints are set before `SDL_Init`, and the XAPI port poller
  uses `input::Connected` instead of running the pad filters/recorder.
- The Vulkan texture view aspect uses the format's depth flag, not volume depth, so 2D
  colour textures get colour views. CPU-uploaded targets are rebuilt single-sample
  instead of being written to the resolve image, `AllocUpload` keeps its buffer with each
  offset, pass reuse checks the full target shape, and `Sync`/`Copy` respect the dirty
  epoch. `Clear` scales guest rects to the host target, stencil masks/reference are
  applied, and `Present` resets its per-frame rings on every exit path.
- D3D12 sampler descriptors are partitioned per frame; scratch RTVs are not borrowed
  twice in a frame; target views and targets are retired after the fence; depth
  signatures use the format's bytes per texel. The threaded backend joins its worker,
  `FencePending` submits the inserted fence, and `GetReport` completes the list it
  reports instead of relying on a later present.
- Kernel fixes: directory queries keep the entry on `BufferTooSmall`, file query/set
  paths take the file I/O lock, thread stacks/KPCRs are released on exit, allocator size
  rounding rejects overflow, the ISO overlapped event is reset per read, ADPCM channels
  are clamped, and the XBE section/boot bounds are checked.
- Packaging and docs were refreshed: the Linux licence glob is optional, shaderc's
  dependencies are checked, the WSL deck helper passes the assets path and refuses
  unsafe directories, and README/LINUX_PORT_PLAN/OPTIMISATIONS no longer describe the
  old renderer limits.
- Deck/RADV validation found two driver-level bugs: D24S8 is not an optimal-tiling
  sampled depth format on RADV (now D32S8 is selected from the supported depth formats),
  and one render-done semaphore was reused across swapchain images (now one per image).
  With the KHRONOS validation layer loaded, the 100% 4x MSAA + SMAA + bloom + DOF run
  reports 0 VUIDs. The Deck package is `dist/TrueCrimeNYC-linux-x86_64.tar.gz`, 11,121,095
  bytes, SHA-256 `216001c00e9cac31f8f08bc0c41a45014279919a9b8e2c2e5561dcb81a02e15b`.

## 2026-11-07 - Linux garage puddles, title input and save-load deadlock

- **Linux input at the title:** XInput reported no controller port unless a physical pad
  was present, so the title never polled the port that `input::Poll` fills from the
  keyboard or `XBE_PAD_SCRIPT`. Port 0 now reports connected on Linux when no pad exists;
  the Windows path is unchanged. Escape (Start), Enter (A), Backspace (B) and the arrow
  keys now reach the title and menus. The deck's scripted pad path also works again.
- **Save-load deadlock:** `NtSetInformationFile` took `f->io` at entry and then took the
  same non-recursive mutex again in the file-allocation/end-of-file case. Pressing Start
  made the title truncate the save metadata, so the guest stopped on the second lock.
  Removing the inner lock fixes the freeze. This is a shared runtime fix; the Windows
  executable was not rebuilt.
- **Garage puddles:** the guest reuses one data address for a 128x128 reflection target
  and a 320x240 light/bloom target. The Vulkan backend's borrowed-target-view cache was
  keyed by the guest address and format only, so whichever shape sampled first created
  the cached view (host 960x540 for 320x240) and the other shape reused it. The 15-tap
  reflection blur therefore sampled the bright light buffer instead of its own 128x128
  downsample; the `DESTALPHA, ONE` composite then added those highlights to the floor
  and saturated the puddles. Dropping the composite only hid the symptom, and scaling
  its gain only masked it.
- **Fix:** key `borrowed_` by the selected `Target*` plus the guest format and depth
  flag, so every live target keeps its own view. No blend factor, render state, shader
  output or Windows path is scaled. The earlier 0.5 `outscale`/`reflect_gain_`
  diagnostic was removed and is not part of this change.
- Corrected guest frame against the Windows capture: mean 31.17 vs 31.23, p99 131 vs
  140, p99.9 191 vs 197, and no clipped puddle pixels (the old frame had 1.48% at 255).
  The optional present post chain (bloom/SMAA/DOF/PC contrast) still runs on the window;
  this comparison is the guest frame, the same image the Windows capture stores.
- All temporary reflection dump/log hooks used during the investigation were removed
  before the final build; only the functional fix remains.
- **Legal screen centering:** the intro legal page is the 640x448 `Legal.EnUS.bik` Bink
  movie. The console writes the frame at the movie buffer base and presents that base;
  the Linux front surface data field points 0x54 bytes (its frame header) into that
  buffer, so the page was presented 21 pixels left. Against an xemu 4:3 snapshot the
  Xbox frame is L67/R66/T44/B96; Linux was L46/R87. CPU-written movie frames now present
  from the Bink base and restore the surface data field afterwards. The fixed 640x480
  frame measures L67/R66/T44/B96, matching xemu exactly. 3D frames are untouched, and
  the correction is Linux-only so the Windows path is unchanged.
