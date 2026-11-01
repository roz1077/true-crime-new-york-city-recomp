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
- Still reported "very dark" by the user; only menu captures so far. Next: a gameplay capture
  (`XBE_CAPTURE_AT=<s>` now captures without F3).
