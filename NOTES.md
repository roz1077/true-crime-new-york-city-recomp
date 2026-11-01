# True Crime: New York City (original Xbox) — static recompilation to PC

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

- 0x293541 CDirectSoundStream SetMode-like 3D method (no SDK version) -> DS_OK.
- 0x26381D XAPI drive-head position for the stream scheduler (reads kernel device object) -> 0.
- 0x2BAD20 D3DDevice_SetDepthClipPlanes (no SDK version): keeps values, skips hardware push.
- 0x2BEDE0 D3DDevice_SetBackBufferScale: keeps values, skips hardware render target rebuild.

## SDK changes (patches/xboxxberecomp-local.patch)

- Kernel 76 IoQueryVolumeInformation (shares NtQueryVolumeInformationFile's classes).
- Kernel 232 NtUserIoApcDispatcher (ReadFileEx completion routines).
- `XBE_WAITLOG=1`: logs waits blocked > 3 s with the guest stack. `XBE_VERBOSE=1`: keep info/warn log lines.
- Known SDK gap: ObReferenceObjectByHandle never writes the object out (worked around by the 0x26381D hook).

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

## Status (2026-11-01)

- Crash-by-crash bring-up: 0.15 s -> 0.5 s -> 5.7 s -> 15 s -> 22 s -> no crash.
- Run 14 (`logs/run-14-waitlog.log`): runs 166 s, no crash, **black screen, no Present at all**; no kernel wait
  over 3 s, so the main thread is busy, not blocked. Next: find the spin (likely a loop polling a GPU register or
  push buffer get pointer that the host never advances), e.g. by sampling guest EIP or tracing candidate functions.
- Unmapped D3D functions still running original code (judged safe): GetDesc/GetLevelDesc, GetSurfaceLevel2,
  LockRect, CreateVertexBuffer2, VB Lock2, SetVertexData*, SetVertexShaderConstant1 inline 0x2BBB10, Reset 0x2B81A0.

## Repository (2026-11-01)

- Git repo at the project root (branch `main`). Not committed: the ISO, `assets/`, `generated/`, `out/`, `logs/`,
  `tools/` (Python venv, reference clone of SCDA-XBERecomp).
- `sdk/` is a submodule (URL: upstream TGP482/XboxXBERecomp) checked out on local branch `tcnyc` (upstream `f7b13fd` +
  our fixes). That commit exists only on this PC: on a fresh clone, check out `f7b13fd` and apply
  `patches/xboxxberecomp-local.patch`. Re-export the patch after SDK commits:
  `git -C sdk diff f7b13fd tcnyc > patches/xboxxberecomp-local.patch`.
- Fresh setup: put `default.xbe` + data in `assets/` (`python scripts/extract_xiso.py <iso> assets`), then `build.bat`.

## Lighting / darkness investigation (2026-11-01)

- **Fixed: constant-alpha blending.** D3DBLEND_CONSTANTALPHA/INVCONSTANTALPHA (0x8003/0x8004) were mapped to the D3D12
  blend factor with the blend colour's RGB; they must use its alpha. The game's soft-shadow buffer (subtracted by the
  city material: r0 = t0*v0 - t2) and its bloom chain (bright pass B0000C10 -> blur -> added with CONSTANTALPHA, ONE)
  came out black. SDK commit on `tcnyc`.
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
