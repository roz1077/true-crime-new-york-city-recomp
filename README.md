# True Crime: New York City - XBE Recomp

An unofficial native PC port of the original Xbox version of True Crime: New York City, with extended graphics options and other enhancements.

This port was created using the [Xbox XBE Recomp](https://github.com/TGP482/XboxXBE-Recomp) tool.

> [!NOTE]
> This project does not include any game assets. You must provide the files from your own legally acquired copy of the game to install or build.

## Installation

1. Extract the Windows release to a folder you can write to and open **TrueCrimeNYC.exe**.
2. Click **Browse** and choose your own dumped original Xbox True Crime NYC ISO. Use the game release that matches the recomp.
3. Click **Launch**. The game copies the ISO to the local `assets` folder, so allow enough space for one copy. The first start compiles the game code.

If an ISO or extracted game files are already in `assets`, click **Launch** without browsing. To switch dumps, move the previous ISO or extracted files out of `assets` first. Recomp settings are stored in `settings.toml` beside the executable.

## Features

### Graphics menu
Press **F10**, or **Back + Start** on controller 1, at any time. Use the arrow keys or D-pad to move and change
values, Enter or A to select, and Esc or B to close. The mouse works too. The game receives no controller input
while the menu is open, so pause the game first during play. Settings are saved to `settings.toml` when the menu
closes.

- **Display mode** - Fullscreen, Borderless or Windowed.
- **Resolution** - Desktop or any mode the monitor lists. Display mode and resolution take effect with *Apply display changes*.
- **Render scale** - 25% to 200% of the output height.
- **Field of view** - 80% to 150% of the game's view, gameplay camera only; aiming still zooms.
- **Anisotropic filtering** - Game default, 2x, 4x, 8x or 16x.
- **SMAA** - SMAA 1x anti-aliasing on the finished frame.
- **V-Sync** and **Frame rate limit** (30 or 60 FPS; the game's timing is built for at most 60).
- **FPS counter**.
- **Texture replacement**, **texture dumping** and **dump format** (PNG or DDS).

The game's own **Options > Display** tab also has rows for render scale, field of view, anisotropic filtering,
SMAA, V-Sync and frame limit; the list scrolls. Left and right change them; Back cancels and A keeps the changes, as with the game's own options.

### Controller
Supports Xbox, PlayStation and Switch controllers, with hot plugging and rumble.

- **Boot Splash** - Place a `splash.png` (or `.bmp`) beside the exe to display a splash screen at launch.

## System Requirements

### Minimum
- **OS**
  - Windows 10 64-bit
- **GPU** with support for Direct3D 12

## Building

### 1. Clone the Repository
Clone this repository using [Git](https://git-scm.com/).

### 2. Add the Required Game Files
Place your extracted game data files in the `assets` folder. `default.xbe` must be there, with the game's folders beside it.

### 3. Install Dependencies
Install Visual Studio 2022 with **Desktop development with C++**, **C++ Clang tools for Windows** and **C++ CMake tools for Windows** (`build.bat` offers to add the last two).

### 4. Build the Project
Run `build.bat`. The first build recompiles the game and takes a while. The exe is written to `out\build\release`.

See [DEVELOPMENT.md](DEVELOPMENT.md) for the source layout, regression checks, diagnostics and host-only
relink workflow. [OPTIMISATIONS.md](OPTIMISATIONS.md) lists performance candidates and how to validate them.

## Credits

- [Xbox XBE Recomp](https://github.com/TGP482/XboxXBE-Recomp), the recompiler and runtime this is built on.
- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) for the overridable function layout and instruction hooks.
- [Zydis](https://github.com/zyantific/zydis) for decoding x86 instructions, [toml++](https://github.com/marzer/tomlplusplus) for the configs, [Dear ImGui](https://github.com/ocornut/imgui) for the settings overlay and [SDL](https://github.com/libsdl-org/SDL) for the window, input and controllers.
