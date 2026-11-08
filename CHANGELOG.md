# Changelog

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
- Ambient occlusion is available as an optional depth-based screen-space effect on both
  renderers, off by default.
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
