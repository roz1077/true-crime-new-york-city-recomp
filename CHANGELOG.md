# Changelog

## 0.9.2.4.3 - Beta 4.3 (2026-11-19)

- Club Tadow no longer crashes when Hamilton's head is drawn. His head mesh is a single skinned
  record whose skin buffer the game leaves uninitialised, so the four bone indices were garbage
  and the skin pass read far outside the 23-bone palette. The port now gives an invalid head
  record its declared bone before the pass. The head still falls off at the start of the mission,
  as scripted.
- A new Audio tab in the launcher chooses the output mix (Mono, Stereo or Dolby Pro Logic II) and
  can force a stereo 48 kHz output for TVs and soundbars that cut out or play quietly (Windows).
  It can also force a master, music, SFX, ambient, voice and cinema volume mix in place of the
  game's Audio menu levels; music ducking and fades still work. The mix applies to the US and both
  European PAL discs, on Windows and Linux.

## 0.9.2.4.2 - Beta 4.2 (2026-11-18)

- Escape Crazy now keeps its 60 FPS gameplay: Zeke walks to his waiting spot instead of going out
  through the window, the four shower doors unlock reliably once the last shower enemy is defeated,
  and the lower-corridor patrol reaches its waypoint instead of turning on the spot.
- The fixes apply to the US disc and both European PAL discs.

## 0.9.2.4.1 - Beta 4.1 (2026-11-17)

- The European French/Spanish/Italian disc can show its text and subtitles in French, Spanish or Italian:
  choose under Language in the launcher, or set `language` (`english`, `french`, `spanish` or `italian`) in
  `settings-pal.toml`. Voices stay in English. English is the default; the US and other European PAL discs
  have English text only, so the choice is not offered for them.

## 0.9.2.4 - Beta 4 (2026-11-16)

- One launcher detects the supported US NTSC and both European PAL disc builds. Each has its own saves and settings.
  Both European PAL versions are experimental in this release.
- Start `TrueCrimeNYC` (`TrueCrimeNYC.exe` on Windows) for every disc. The two PAL game executables are in the
  `regions` folder; the launcher starts the one that matches your disc, and they share the main folder's game
  files, music, logs and shader cache.
- Release builds ignore the developer diagnostic, trace and tuning environment switches. The player and
  troubleshooting switches in README.md and README-LINUX.md still work; the rest need a developer build.

### Controls
- Rebind the keyboard, mouse and controller from a new Controls tab in the launcher or the Controls page
  of the in-game menu (F10). Keyboard actions take up to four keys, with separate on-foot and driving
  layouts; controller buttons swap with each other. Enter, the arrows and Escape always work in menus.
- Button prompts follow the bindings: keyboard prompts show the bound key, and a remapped controller
  shows the physical button now doing the job, in every prompt style.
- Bindings are saved in `settings.toml` under `[controls]`. The older `reload_key`, `heavy_attack_key`,
  `use_key` and `climb_key` settings are carried over and then replaced.
- Controller aiming uses one radial dead zone across the whole right stick, then a quadratic curve on the
  stick's magnitude, so a diagonal push turns the camera as far as a straight push at the same deflection.
  The original per-axis dead zone made the centre feel uneven; full-stick speed is unchanged. The same curve
  applies while looking around with L2. An `Original` response keeps the Xbox behaviour, and Controller aim
  sensitivity (100-400%) can raise full-stick turn speed.
- Controller aim response and sensitivity, mouse sensitivity and mouse vertical sensitivity are now on the
  launcher's Controls tab, in the F10 menu, and in the game's own Options > Controller tab.
- The camera no longer sticks after looking hard up or down with the controller.

### Audio
- Custom music: songs in a folder you choose replace the Music Player's, by song number
  (`6000.mp3`, `6001.ogg`, `6002.flac`, ...). MP3, OGG Vorbis, FLAC and WAV are decoded by the port, so no
  conversion is needed. Songs go in the new `music` folder beside the game; another folder can be chosen on
  the launcher's Music tab or as `music_folder` in `settings.toml`. Updates never touch the music folder.
- Other songs in that folder are added to the Music Player as new, already-owned songs (up to 35),
  named from the file (`Band - Title.mp3`).

### Launcher
- A Music tab: explains custom songs, opens the music folder, and can point the game at another one.
- Redesigned in the game's night-city look, with every graphics option grouped into sections.
- The launcher recognises both PAL discs from an ISO or extracted folder even in a build made without the PAL game images present. A source build still needs the matching PAL executable to play them.
- Checks GitHub for a newer release and can download, verify (SHA-256) and install it, then restart.
  Settings, game files and textures are never touched. Turn it off on the Play tab.

### Performance
- Windows (D3D12) and Linux (Vulkan): a draw's vertex streams and its index list now go into the upload
  ring as one block instead of one allocation per stream.
- Vulkan: vertex and draw constants are uploaded only when they change, saving about 5 KB of host writes
  per draw; blend factor and stencil reference are set only when they differ.
- Vulkan: targets are found through an address index instead of walking every target up to three times
  per sampled texture; sampler lookups and scene deferral bookkeeping are cheaper per draw.
- Vulkan: CPU-written surfaces upload with a straight row copy when the sizes match (no per-texel
  division, no full-size intermediate), and a read-back takes one submission instead of two.
- Vulkan: scene deferral now runs only with Ambient occlusion on, or when `TCNYC_DEFER_SCENE=1` asks for
  it; with AO off it was pure record/replay overhead.
- Vulkan: creating or reading the pipeline cache is serialised, as the API requires now that the
  ambient-occlusion variant workers also build pipelines.
- Windows (D3D12): each target remembers its sample count and whether it was ever drawn with depth, so a
  draw no longer queries the resource for both or hashes a target address; `Clear` keeps its rect storage
  between calls, and the scene-depth map is written once per target rather than once per draw.
- Both: ambient-occlusion variant readiness is read from a per-frame snapshot instead of taking a worker
  mutex on every scene draw, and scheduling a variant no longer copies its shader source.
- Both: the background shader/module handoff caches are capped, so an AO-heavy session cannot grow them
  without bound.

### World
- Pedestrian detail (F10): distant pedestrians become full models up to 2x, 4x or the whole stand-in range
  (~47 units) away instead of ~8, with nearer stand-ins served first. Very high and Maximum also let spawn
  points fill the street 2x or 3x farther out. Maximum loads characters from their single full-detail files.
  Full pedestrians made far away no longer delete themselves within a second (the per-character despawn
  check now matches the setting), which removed the stand-in/full flicker.
- Pedestrian limit (F10, experimental above 30): 15 to 75 full pedestrians at once. The per-frame character
  cache moved from the game's fixed 32 slots to 128. From 20 up, more pedestrian types load at once (up to 9,
  capped by the game's asset table) without the type loader rotating them out, and type quotas are evened so
  one type no longer fills the street.
- Crowd density (F10): 1.5x, 2x or 3x pedestrians per spawn point; 3x raises the crowd budget to 350.
- Traffic density (F10): 1.5x to 5x the ambient car budget and each road spawn point's cap.
- Traffic distance (F10 and launcher): Original (default), 1.5x or 2x spawn range for ambient cars. It spreads the cars Traffic density allows over a wider area, so raise density with it.
- Object detail now applies to moving cars as well; parked cars that have not been touched keep the game's
  own detail (they only carry low-detail textures until woken).

### Fixes
- City detail High no longer crashes the game after a while of driving: the visible-instance list's count
  is now read from its moved location by every function that uses it.
- Options > Display offers the rest of the F10 graphics settings (pedestrian, traffic and city detail, texture
  replacement and dumping) and Restore Defaults.
- Updates are validated before extraction: symbolic links, special files and traversal names are refused, and
  tar runs with `--no-same-owner --no-same-permissions`. A failed install no longer leaves a pending update
  that every launch retries, and an active journal without its staged installer rolls back in-process.
- The updater reads up to five release pages, reports a failed check instead of "up to date", resumes large
  downloads, and makes an already-staged release visible when update checks are off. Files removed from a
  newer package are deleted; they are backed up first, so an interrupted or failed install restores them.
  Directories created by a failed install are removed.
- D3D12 ambient occlusion uses the scene's depth clear value. This restores visible occlusion on both PAL disc builds.
- Vulkan: MSAA scene attachments keep their contents across pass reopens, borrowed depth uses its depth view,
  and a multisampled target's feedback copy resolves into a separate image instead of reading its own resolve
  target.
- Moving car lamps stay steady through dusk and dawn in both the body and distant instance draws.
  Full-opacity car paint keeps its depth writes.
- Input: Return, Backspace and the arrow keys can no longer be bound (they always drive menus). Duplicate
  controller sources are repaired, sources without a glyph clear their prompt, and the launcher promotes a
  new key to the prompt slot. The in-game editor accepts every key the launcher does.
- Settings: a legacy `msaa = true` still means 4x, saves publish on a worker instead of holding the UI lock
  (a save that fails is retried with every unsaved change), settings are saved when the game exits,
  the launcher keeps unknown keys, and Linux reads the field of view and city detail settings and remaps
  DLSS to FSR 3.1 when DLSS is not built in.
- Files: renames share delete, `ReplaceIfExists` and directory/file mismatches are honored, missing CD-ROM
  files report not-found, and volume queries stay inside the caller's buffer.
- Kernel: timer DPCs set `Inserted`, wait-all no longer spends a pulse, huge timeouts are clamped, and
  `MEM_RELEASE` validates its base. Crash dumps avoid logger deadlocks and recursion, and a crash on one
  thread waits briefly for another thread's log line so its own lines still reach `tcnyc.log`.
- `DbgPrint`/`RtlSprintf` handle `*` widths and precisions and 64-bit arguments, a Linux thread exit no longer
  frees its start closure twice, and 16-bit `rep stos` fills only the requested length.
- Larger character caches and the city grid store allocate from the top of user space, away from the game's
  heap. The last-vertex diagnostic clamps its index and checks the stream bound.
- Non-ASCII install paths work for config parsing, logs, crash dumps and the splash screen.
- Recompiler: `FNSAVE`/`FRSTOR` keep the x87 register stack at the right offset, `push`/`pop` with a 16-bit
  operand size move the stack by two bytes, and `cvtss2si` follows MXCSR rounding.

## 0.9.2.0 — Beta 3 (2026-11-12)

### Graphics
- Linux / Steam Deck: TAA (with the same per-object motion and road handling as Windows) and experimental
  AMD FSR 3.1 (Performance, Balanced, Quality, Native AA), built from AMD's open FidelityFX SDK 1.1.4 Vulkan
  backend. FSR 3.1 protects marked HUD pixels and supplies a reactive mask for effects. It uses FP16 and
  wave64 shaders where supported. The Vulkan bloom chain now runs after fixing a shader cache key collision.
- Experimental Vulkan DLSS in optional SDK builds, with the same four presets and clean fallback
  on unsupported hardware. Native RTX rendering still needs validation.
- Linux Vulkan records large deferred scene batches on two threads by default.
  Set `TCNYC_DRAW_WORKER=0` to disable parallel recording.
- FP16 scene colour on D3D12 and Vulkan, on by default where supported. Set `TCNYC_SCENE_FP16=0` to turn it off.
- Pipelines are kept between runs: the driver's compiled pipelines are saved beside the
  shader cache (`shader-cache/.../pipelines.bin` on Windows, `~/.cache/TrueCrimeNYC/pipelines.bin`
  on Linux) a few seconds after the last new one, so effects and areas seen before no longer
  stall a frame the first time they appear in a later run. A cache from another GPU or driver
  is discarded. `TCNYC_PIPELINE_CACHE=0` turns it off.
- Low latency (F10 and the Display tab, Windows): one frame waits for the display instead of
  three, so the controls respond sooner. Off by default.
- Bloom is built from a chain of half- to 1/16-size images instead of 36 full-size reads per
  pixel: cheaper at high resolutions and smoother, without the faint eight-way star. Same
  threshold and overall brightness; the glow spreads somewhat wider than before. A new Bloom
  strength setting (50% to 400%, F10 and the Display tab) makes it brighter; 100% is the original look.
- Shadow quality (Original, High 2x, Ultra 4x): the game's 256x256 shadow image (silhouettes the
  game blurs and projects onto the world) is drawn at a fixed 1024 or 2048 pixels a side, without
  MSAA, instead of at the render scale. Shadow softness (Off, Soft, Softer) reads it through a 4x4 grid of taps for wider, smoother
  edges. Both off by default.
- With MSAA on, alpha-tested cut-outs (fences, leaves, wires) get alpha to coverage, so their edges
  are smoothed like geometry edges. This applies only to the main picture.
- Texture packs: generated mipmaps average in linear light (distant bright detail no longer
  darkens) and keep the coverage of cut-out textures, so fences and leaves do not thin out.
- Depth of field no longer smears sharp nearer objects into the blurred background behind them.
- AMD FSR 1 upscaling (Upscaler: Bilinear or AMD FSR 1) when Render scale is below 100%, and
  RCAS sharpening (Sharpening: Off to 100%) of the 3D frame at any render scale. The HUD,
  movies and still pictures are not sharpened. Windows and Linux.
- TAA (F10 and the Display tab): temporal anti-aliasing in place of SMAA, using the game camera and per-object motion
  for cars, people and the road. Needs no extra DLLs. Off by default.
- FSR 3.1, FSR 4.1 and DLSS offer Performance (50%) and Balanced (58%) presets beside Quality and
  Native AA; the jitter pattern lengthens at lower scales. The F10 Upscaler row says when AMD's DLLs
  are missing or only FSR 3.1 is offered. Sharpening now also applies to DLSS output. Brief far-plane
  changes around camera cuts no longer restart FSR's history.
- Experimental FSR 4.1 upscaling on Windows through AMD's FidelityFX SDK 2.4 loader/upscaler DLLs,
  using the same temporal inputs as FSR 3.1. Performance (50%), Balanced (58%), Quality (67%) and Native AA (100%) are available;
  it falls back to FSR 3.1 when the FSR 4 provider is unavailable. Dispatches logged successfully
  with provider 4.1.2 on the RX 7900 XTX and checked in game at every preset.
- Experimental DLSS Super Resolution on Windows when the optional NVIDIA DLSS SDK is configured
  (`TCNYC_DLSS_SDK`) and NVIDIA's `nvngx_dlss.dll` sits beside the executable. It reuses the
  temporal colour, reversed depth, motion and jitter inputs, and keeps the HUD preserved. RTX
  native validation is still pending; the AMD/OptiScaler path is a developer test only.
- F10: the Depth of field help no longer says it needs MSAA.

### Performance
- The renderers do less CPU work per draw on both backends. Vulkan resolves each texture and
  target once a frame, reuses draw descriptor sets and records large deferred scene batches on
  two threads (above); D3D12 repacks unaligned vertex streams instead of converting every
  element, tracks vertex-constant changes on the render thread instead of comparing 3 KB per
  draw, and caches guest allocation sizes per thread until the memory layout changes. Linux and
  Windows.
- Fewer first-time stutters: the draw pipelines a run uses are listed beside the pipeline cache,
  and the next start builds them on background threads before the game needs them. The list
  survives a driver update that empties the driver's own cache, and both are now saved when the
  game exits. `TCNYC_PIPELINE_WARM=0` turns the pre-build off. Linux and Windows.

### Menus
- A native Exit Game row in the main menu and the pause menu, with a confirmation step. It
  saves the Display tab's settings, then closes the window (an SDL quit event on Linux). An exit
  chosen while a display mode change is still being applied waits for it to finish.

## 0.9.1.0 — Beta 2 (2026-11-11)

### Gameplay
- A voice volume of 0 no longer blocks gameplay phone calls: the mix-bin
  volume getter reports an exact 0 as a tiny positive value (the saved volume
  is unchanged), so calls still start (silently) and their completion
  advances informant and precinct missions.
- Car-scrape damage is capped: impact (collision) damage to the player, most
  visibly from a passing car while entering or exiting a vehicle, is limited
  to 15% of max health per second of game time instead of the car's full
  velocity-derived damage. The cap counts the health actually lost, so hits the
  game ignores do not use it up, and it restarts on a level load. Other damage
  types are unchanged.
- Direct mouse pitch is held against the on-foot controller's rest-pitch reset and
  the profile's camera auto-center, so vertical aim accumulates instead of snapping
  back every frame. The hold starts when the mouse moves, so keyboard-only play keeps
  the game's auto-center, and ends on a level load or when a pad takes over.
- Mouse pitch now follows the stock (non-inverted) vertical direction; the game's
  invert-Y setting still flips it.
- Direct mouse aim now also drives the precision-aim camera. Its own look step
  receives the mouse, and an active lock-on target snaps the crosshair onto it
  while the game's aim assist runs.
- Human Cargo: the push-off window on the cargo container fight with Tommy lasts
  about 250 ms instead of 166 ms, so the B press is no longer frame-perfect.
- Keyboard driving keeps full steering while W (accelerate) is held.

### Controls and prompts
- Button prompts follow the active device on Linux as well as Windows, including
  PlayStation pads behind Steam Input.
- The Steam Deck's built-in controller gets its own glyphs (lettered face buttons,
  View and Menu); PlayStation prompts show Share and Options. Other handhelds' built-in
  controllers under Steam Input (ROG Ally, Legion Go) get Xbox prompts, and prompts
  follow a controller swapped in through Steam Input while it is in use.
- `prompt_style` in `settings.toml` overrides the automatic prompt choice: 0 automatic,
  1 original, 2 Xbox, 3 PlayStation, 4 Steam Deck.
- Keyboard prompts switch to the pedal keys (W, S, Space) in a vehicle and show the
  configured heavy-attack key.

### Graphics
- MSAA is off by default (it is the most demanding option); existing settings files keep
  their choice. Depth of field no longer needs MSAA: distance blur reads the scene depth
  with MSAA off too, on Windows and Linux.
- The intro movies (Activision, Luxoflux, Exakt) fill wide screens: their 4:3
  picture is stretched across the whole frame at every aspect setting instead of
  sitting squeezed in the middle. The legal page keeps its 4:3 shape, centred. On
  Linux the movies are no longer shifted 21 pixels to the right.
- Ambient occlusion (F10: Off / Half / Full, off by default) on Windows (D3D12) and Linux
  (Vulkan). It uses GTAO from a depth prepass of world geometry only, and is applied inside
  the world draws before fog, so light shafts, fog and other effects are not darkened and no
  longer show as white bands.
- Fixed an offset copy of the scene that showed along the right and bottom edges of the
  frame and moved with the camera, caused by the game's glow pass reading past its own
  region of a shared render target.
- The game's glow no longer fades to a dark band at the right and bottom edges. This
  deliberately differs from the console, where the glow texture's black border shows there.
- Fixed the FS placeholder replacing the top-right HUD slot icon after weapon swaps:
  small cached HUD render targets are kept across idle frames instead of being swept.
- Vulkan now binds swizzled render targets and leaves target UV clamps open, so projected
  light textures such as the garage floor pools are drawn on Linux.
- HDR output on Windows (scRGB or HDR10) and Linux (HDR10 or extended sRGB) with paper
  white, peak brightness and highlight strength settings. Off by default; it follows a
  Windows HDR toggle without a restart.
- Object detail (F10 and the Display tab): Original, High, Very high or Maximum, for how
  far buildings and props keep their detailed models. Vehicles keep the game's own
  detail level, so distant parked cars are never drawn untextured.
- Building windows blend their lit interiors as on the console: a fixed-function stage
  that writes a temporary register now leaves texture 0's alpha as the current alpha.
- Fixed an occasional white flash in the intro cutscene: a cutscene frame that comes
  early now waits for the audio clock instead of drawing a frame in which nothing moved.
- Linux: fixed a band of the last intro movie frame that sometimes showed under the legal
  page; a front-buffer clear between pictures now reaches the buffer's memory too.

### Menus
- The native Options > Display tab has the remaining Graphics options: display mode,
  object detail, bloom, depth of field, ambient occlusion, seam fix, PC contrast, and an
  HDR SETTINGS page. A display change there is saved only once it has been applied, and
  a refused change is rolled back.
- Display changes from either menu are applied with their own values and reported per
  request on Windows and Linux; a refused or failed change (including a failed window
  move) puts the previous mode back, and Back during an apply restores the entry mode
  without saving the cancelled one.
- The F10 Graphics menu scrolls when it is taller than the screen, and keeps the row
  highlighted by the keyboard or a pad in view.

### Developer
- A `dev` CMake preset builds a debug menu (F9: mission and stage jumps, player cheats,
  game speed, time of day, log, script trace). Release packaging refuses dev builds.

## 0.9.0.0 — beta hardening (2026-11-07)

### Stability
- Save data is flushed and committed at shutdown; file flush/truncate/rename errors are
  reported instead of acknowledged.
- D3D12 uses one command list per frame slot, fenced before reset, with bounded fence
  waits and device-removal detection.
- Vulkan target rebuilds retire framebuffers/views instead of destroying them in flight;
  framebuffers are destroyed before their views; sampled depth uses its real layout;
  submit/acquire failures stop cleanly.
- Texture, vertex, index, shader and push-buffer access from guest data is bounded.
- CDROM0 is read-only even when the game runs from extracted assets.

### Correctness / gameplay
- Ambient occlusion is present as an experimental D3D12 path, off and disabled in normal play
  while bright effect-layer artifacts are resolved; Vulkan remains in progress.
- `PulseEvent` releases waiters correctly.
- Thread/stack creation failures propagate; memory commits report failure; timer
  reinitialisation cancels the old timer.
- Direct mouse pitch defaults follow the documented on-foot/vehicle conventions.
- DirectSound release runs completion callbacks before freeing the stream.
- F10 settings save on window close; display changes apply before saving and roll back
  on failure.
- Keyboard menu navigation is delivered once; unfocused controllers are ignored.
- XInput capability layout, settings defaults, FOV clamping and the 60 FPS timing path
  were corrected.

### Platform / release
- Unicode host paths and long-path-aware Windows manifest.
- Native Linux target rebuilds are lazy: a sampled HUD target keeps its contents
  across MSAA/render-scale changes and is rebuilt only when next bound as a render
  target, matching the Windows fix for the static FS placeholder.
- Native Linux target uploads are batched into the current command buffer; staging
  buffers are retired at the frame fence instead of submitting and waiting twice per
  CPU-written surface.
- Native Linux audio pre-renders through a small jitter buffer; a 60-second Xbox Ally X
  (SteamOS) run reported zero underruns and zero starvation events.
- Native Linux suspend/resume audio pause and device-change handling.
- Windows exe version 0.9.0.0 and product metadata.
- Both packages include the SDK licence, project MIT licence, Zydis/SDL/ImGui/toml++/SMAA
  notices, build metadata and deterministic archives.
- Original application icon and Windows version metadata.
- CI workflows for Windows checks and Linux packaging; SDK fallback pinned.
