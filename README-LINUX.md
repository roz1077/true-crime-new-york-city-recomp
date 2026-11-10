# True Crime: New York City - native Linux / SteamOS

This is the native SDL3 + Vulkan build. It does not use Proton.

## Requirements

- 64-bit x86 Linux with glibc 2.38 or newer (SteamOS 3.6+, current Arch, Ubuntu 24.04+).
- A Vulkan 1.1 GPU and driver (Mesa RADV/Intel, current NVIDIA). MSAA needs a
  Vulkan 1.2 driver; without it the other options still work and MSAA falls back
  to one sample.
- Your own copy of the matching Xbox release: extracted files or the `.iso`.
  No game assets are included.

## Install

1. Extract the tarball into a folder you can write to.
2. Put the game in `assets/` beside `TrueCrimeNYC`:
   - extracted files: `assets/default.xbe` with the game's folders beside it, or
   - a disc image: any `.iso` in `assets/`.
   The extracted files are used when both are present.
3. Run `./TrueCrimeNYC`.

You can also point at a folder or image directly:

```sh
./TrueCrimeNYC --assets /path/to/game
./TrueCrimeNYC --iso "/path/to/True Crime - New York City (USA).iso"
```

The XBE is checked against the release this binary was built from. A different
release is refused, because it would run its own data under this recompiled code.

The package also contains:

- `TrueCrimeNYC`: the executable.
- `lib/libshaderc.so.1`: the bundled shader compiler, loaded from beside the
  executable.
- `README.md`, `README-LINUX.md`, `BUILD.txt` and `THIRD_PARTY.txt`.
- `LICENSES/XboxXBE-Recomp-LICENSE.txt` for the SDK, plus `LICENSES/imgui-LICENSE.txt`
  and the shaderc/glslang copyright files when the build host provides them.
- `assets/PUT-THE-GAME-HERE.txt`; no game files are included.

## Steam and SteamOS handhelds

The same steps apply to Steam Deck and other SteamOS handhelds.

1. Steam -> Games -> **Add a Non-Steam Game** -> **Browse** -> select `TrueCrimeNYC`.
2. Properties -> Compatibility: leave Proton **off**. This is a native build.
3. Optionally set **Start In** to the folder that contains `TrueCrimeNYC`.
4. Launch from Desktop Mode or Game Mode.

In Game Mode, leave the in-game display mode on **Borderless**; the window fills
gamescope's output.

## Controls

See `README.md`. **F10**, or **Back + Start** on controller 1, opens the Graphics
menu. The menu works with keyboard, mouse and controller.

Button prompts follow controller 1: Steam Deck prompts for the Deck's own controller, PlayStation
prompts for a PlayStation pad (also behind Steam Input), Xbox prompts for an Xbox pad. If the guess is
wrong, for example an Xbox pad used through Steam Input on a Deck, set `prompt_style` in
`settings.toml`: 0 automatic, 1 original, 2 Xbox, 3 PlayStation, 4 Steam Deck.

## Paths

- Logs: `logs/` beside the executable.
- Game data and saves: `hdd/` beside the executable.
- Texture packs: `textures/replacement/` beside the executable.
- Texture dumps: `textures/dumps/` beside the executable.
- Settings: `$XDG_CONFIG_HOME/TrueCrimeNYC/settings.toml`
  (normally `~/.config/TrueCrimeNYC/settings.toml`).
- Shader cache: `$XDG_CACHE_HOME/TrueCrimeNYC/shaders`
  (normally `~/.cache/TrueCrimeNYC/shaders`).

## Texture packs

The Graphics menu has **Custom textures** and **Dump textures**. Dumps go to
`textures/dumps/` (PNG by default, or DDS from the menu) with names such as
`256x256_0123456789abcdef_DXT1.png`. To replace a texture, put an image with the
same name in `textures/replacement/`; PNG of any resolution and DDS are read.
The key in the name is a hash of the texture's bytes, so names match the Windows
build and packs can be shared. Replacement images get a mip chain automatically
when the file has a single level. Turning **Custom textures** off ignores the
pack without deleting it.

Keep the package in a writable folder: logs, saves and the shader cache must be
written. The first frames compile shaders; later runs reuse the cache. Deleting the
cache is safe and only costs one slower start.

## Troubleshooting

- `TCNYC_PERF=1 ./TrueCrimeNYC` logs a per-frame CPU breakdown every 60 frames:
  draw, texture/upload, descriptor, overlay and present wait. Include those lines
  when reporting performance.
- `TCNYC_TRACE=1 ./TrueCrimeNYC`, then press **F3** when a visual problem is on
  screen: `logs/trace_<frame>.tga` (the guest front buffer),
  `logs/trace_<frame>_present.tga` (the finished swapchain image, after gamma,
  contrast and the overlay) and `.txt` (the complete draw list with targets,
  textures, blend, depth and cull) are saved. Include all three when reporting.
  `TCNYC_TRACE_AT=<frame>` captures that frame without pressing F3;
  `TCNYC_DUMP_TARGETS=1` also writes every live render target, and
  `TCNYC_DUMP_TEX=<hex,hex>` writes named source textures.
- `TCNYC_AO_PROBE=1 ./TrueCrimeNYC` writes the first AO distance and occlusion images to
  `logs/ao_dist.pgm` and `logs/ao_probe.pgm` for comparing against Windows.
- `TCNYC_RENDERER=software ./TrueCrimeNYC` selects the CPU reference renderer
  (very slow). It is useful to check whether a problem is GPU-specific.
- `SDL_VIDEODRIVER=wayland` or `SDL_VIDEODRIVER=x11` forces a video backend.
  Game Mode uses Wayland/gamescope.
- The log is the first place to look. Include `logs/tcnyc.log` when reporting a
  problem. The `vulkan:` lines name the GPU and the present mode.
- "another release" means the game files do not match this build's XBE. There is no
  dialog on Linux; set `TCNYC_ALLOW_OTHER_RELEASE=1` only if you deliberately want to
  run a mismatched release.
- On a system without a Vulkan driver, install the distribution's Mesa Vulkan
  package (`mesa-vulkan-drivers`, `vulkan-radeon`, `vulkan-intel`, ...).

## Licenses

The executable links SDL3 (statically when the SDL3 package provides the static
target, otherwise the shared SDL3 library) and statically links Dear ImGui.
shaderc is loaded at run time from the bundled `lib/libshaderc.so.1`. See
`THIRD_PARTY.txt` and `LICENSES/`.
