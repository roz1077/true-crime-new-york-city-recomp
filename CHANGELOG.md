# Changelog

## 0.9.1.0 — Beta 2 (unreleased)

### Gameplay
- A voice volume of 0 no longer blocks gameplay phone calls: the mix-bin
  volume getter reports an exact 0 as a tiny positive value (the saved volume
  is unchanged), so calls still start (silently) and their completion
  advances informant and precinct missions.
- Car-scrape damage is capped: impact (collision) damage to the player, most
  visibly from a passing car while entering or exiting a vehicle, is limited
  to 15% of max health per second of game time instead of the car's full
  velocity-derived damage. Other damage types are unchanged.
- Direct mouse pitch is held against the on-foot controller's rest-pitch reset and
  the profile's camera auto-center, so vertical aim accumulates instead of snapping
  back every frame. Both return to the game's control when a pad takes over.
- Mouse pitch now follows the stock (non-inverted) vertical direction; the game's
  invert-Y setting still flips it.
- Human Cargo: the push-off window on the cargo container fight with Tommy lasts
  about 250 ms instead of 166 ms, so the B press is no longer frame-perfect.
- Keyboard driving keeps full steering while W (accelerate) is held.

### Controls and prompts
- Button prompts follow the active device on Linux as well as Windows, including
  PlayStation pads behind Steam Input.
- The Steam Deck's built-in controller gets its own glyphs (lettered face buttons,
  View and Menu); PlayStation prompts show Share and Options.
- Keyboard prompts switch to the pedal keys (W, S, Space) in a vehicle and show the
  configured heavy-attack key.

### Graphics
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
- HDR output on Windows (scRGB or HDR10) and Linux (HDR10 or extended sRGB) with paper
  white, peak brightness and highlight strength settings. Off by default; it follows a
  Windows HDR toggle without a restart.
- Object detail (F10 and the Display tab): Original, High, Very high or Maximum, for how
  far buildings and props keep their detailed models. Vehicles keep the game's own
  detail level, so distant parked cars are never drawn untextured.
- Building windows blend their lit interiors as on the console: a fixed-function stage
  that writes a temporary register now leaves texture 0's alpha as the current alpha.

### Menus
- The native Options > Display tab has the remaining Graphics options: display mode,
  object detail, bloom, depth of field, ambient occlusion, seam fix, PC contrast, and an
  HDR SETTINGS page. A display change there is saved only once it has been applied, and
  a refused change is rolled back.

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
