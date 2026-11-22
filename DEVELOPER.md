# Developer builds

This guide is for people who build the port themselves: a developer build with the F9 debug menu, an experimental
DLSS build, and the switches that help when testing or reporting a problem. For a normal build, see
[Building from source](README.md#building-from-source) in the main README first; everything here starts from a
checkout that already builds.

Developer and DLSS builds are for your own use. Release packages are made from the plain release build, and the
packaging script refuses a developer build.

## Windows developer build (F9 menu)

The `dev` preset is the release game with debugging tools added: an **F9** menu with mission and stage jumps,
player cheats, game speed, time of day, the log and a script trace.

1. Run `build.bat` once, as for a normal build. It checks the toolchain and writes `config\game.toml`.
2. Run `build-dev.bat`. It sets up the same toolchain and builds into `out\build\dev`, apart from the release
   build. The first developer build recompiles the game code, which takes as long as the first normal build.
3. Run `out\build\dev\TrueCrimeNYC.exe`.

To do the same by hand, from a shell where `build.bat`'s toolchain is on `PATH`:

```bat
cmake --preset dev
cmake --build out\build\dev
```

## Experimental 120 FPS render test

Use the Windows developer build with the in-game frame limit set to 60 FPS. For a display running at 120 Hz or
higher, start the game from PowerShell in the project folder.

```powershell
$env:XBE_EXPERIMENTAL_120FPS = '1'
$env:XBE_TRACE_FRAME_TIMING = '1'
$env:XBE_PERF = '1'
$env:XBE_VERBOSE = '1'
.\out\build\dev\TrueCrimeNYC.exe
```

The prototype tries a second guest render and swap after each eligible one-tick gameplay update. Both renders
use the same simulation state. This test measures whether the render path can run twice per update before adding
camera or object interpolation. Pause menus, movies, audio-clock cutscenes, and input recording or replay use the
ordinary path. For a baseline run, clear `XBE_EXPERIMENTAL_120FPS` with `Remove-Item Env:XBE_EXPERIMENTAL_120FPS`
and relaunch. Compare updates, ticks, renders and swaps in `out\build\dev\logs\tcnyc.log`, along with the
renderer FPS from `XBE_PERF`.

## Higher frame rate gameplay test

Set `XBE_NATIVE_FPS` to a cap from 61 through 120 before launching the developer build. Keep the in-game frame
limit at 60 FPS and turn V-Sync off for the VRR test. The test currently waits for a simulation tick before
rendering because zero-tick renders can flash. This limits gameplay to about 60 FPS. Use `XBE_TRACE_FRAME_TIMING=1`
to compare updates, ticks and swaps in the log. Clear the variable and relaunch for standard timing.
The earlier `XBE_NATIVE_90FPS=1` switch still selects 90 when `XBE_NATIVE_FPS` is unset.

## Vehicle timestep probe

In a developer build with the 60 FPS limit, launch once with `XBE_VEHICLE_STEP_PROBE=full` and once with
`XBE_VEHICLE_STEP_PROBE=split`. The probe picks the first eligible moving vehicle and logs up to 60 samples
of displacement, horizontal speed and object flags. In `split` mode it runs the vehicle routine twice at half
the world step. Use `XBE_VEHICLE_STEP_PROBE=motion` to run only the force and motion integrator twice with
half-sized steps and the same force and torque inputs. The `motion` log field counts split integrator calls
per vehicle update. The probe can run with the main-menu attract traffic. The runs may select different vehicles,
so compare model and starting speed before drawing conclusions. Object flags are diagnostic data, not a contact trace.

For the straight-line comparison, launch with `XBE_VEHICLE_STEP_PROBE=straight-full` or `straight-motion`.
Park the player vehicle on a clear straight stretch and press F7 once. The probe applies 240 ticks of
throttle and 240 ticks of brake/reverse with neutral steering, then releases control. Use the same save,
car, position and heading for each run. The brake input can reverse the vehicle after it stops.

## Experimental DLSS

DLSS is experimental. It has been checked through OptiScaler, but not yet on real NVIDIA RTX hardware, so expect
rough edges and please report what you find. It needs:

- an NVIDIA RTX GPU and a current NVIDIA driver (the proprietary driver on Linux);
- NVIDIA's DLSS SDK. Clone [NVIDIA/DLSS](https://github.com/NVIDIA/DLSS) with Git LFS installed, so its library
  files are downloaded and not left as LFS pointers. By using it you accept NVIDIA's licence in that repository.

NVIDIA's SDK is not part of this repository, and DLSS files are not included in release packages. NVIDIA's licence
sets conditions (NVIDIA branding and their approval) for distributing an application that includes DLSS, so keep
DLSS builds to yourself.

### Windows

1. Run `build-dev.bat` with the SDK's folder:

   ```bat
   build-dev.bat C:\path\to\DLSS
   ```

   It builds the developer version with DLSS and copies the SDK's `nvngx_dlss.dll` beside
   `out\build\dev\TrueCrimeNYC.exe`. Running `build-dev.bat` again without a path turns DLSS off in that folder.
   By hand, add `-DTCNYC_DLSS_SDK=C:\path\to\DLSS` to the configure step and copy
   `lib\Windows_x86_64\rel\nvngx_dlss.dll` beside the executable yourself.
2. In the game, set **Render scale** to 50%, 58%, 67% or 100% and **Upscaler** to **DLSS** (F10 or
   **Options > Display**). These are the Performance, Balanced, Quality and DLAA (Native AA) presets. The Upscaler
   row says `DLSS (unavailable)` when NVIDIA's runtime finds no supported GPU.

### Linux

1. Configure with the SDK's folder; it needs `include/nvsdk_ngx_helpers_vk.h` and
   `lib/Linux_x86_64/libnvsdk_ngx.a`:

   ```sh
   cmake -S platform/linux -B build/linux-dlss -G Ninja -DCMAKE_BUILD_TYPE=Release \
     -DTCNYC_DLSS_SDK=/path/to/DLSS
   cmake --build build/linux-dlss --parallel
   ```

2. Either make a package, which adds NVIDIA's runtime to its `lib/` folder:

   ```sh
   scripts/package-linux.sh build/linux-dlss dist-dlss
   ```

   or, to run the build folder directly, copy `lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.*` from the SDK into a
   `lib/` folder beside the executable.
3. Choose DLSS in the game as on Windows. The device also needs independent blending, which every RTX GPU has.

### DLSS switches

Set these before launching. They are for testing and may change.

| Variable | Effect |
|---|---|
| `TCNYC_DLSS_PRESET` | `J`, `K`, `L` or `M`: ask for that DLSS render preset (Windows). |
| `TCNYC_TRACE_DLSS=1` | Verbose NVIDIA NGX logging in the game's log (Windows). |
| `TCNYC_NGX_PATH` | Load NGX from this file instead of the driver's (Windows). |
| `TCNYC_DLSS_FORCE=1` | Try DLSS even when NGX reports it unavailable, for example through OptiScaler (Windows). |
| `TCNYC_DLSS_REACTIVE=1` | Pass the reactive mask to DLSS as its colour bias mask (Windows). |
| `TCNYC_DLSS_JITTER_X`, `TCNYC_DLSS_JITTER_Y`, `TCNYC_DLSS_MV_X`, `TCNYC_DLSS_MV_Y` | `-1` flips that axis of the jitter or the motion vectors (Windows), for checking DLSS's conventions. |

## PAL / European build

The European `(Fr,Es,It)` image is a separate XBE with its own function and data
layout. It has its own config (`config/game-pal.toml`), generated code
(`generated/game-pal`) and build folder:

```bat
build-pal.bat
build-pal.bat dev
```

Put the European `default.xbe` in `assets-pal/` first. The PAL build uses the same
`src/game` hooks through `TC_ADDR` and aliases in `src/game/pal_addresses.h`.
Check shared hook aliases in `src/game/pal_addresses.h` and timing in
`src/game/timing.cpp`. The generated-code manifest ties each region's output to
its XBE and generator inputs. The `dev` argument builds the F9 tools
in `out/build/pal-dev`. Put the European ISO in the `assets` folder next to
the executable you want to run. PAL cutscenes use the title's performance-counter clock.
PAL deterministic replay records the resulting per-frame simulation ticks for
gameplay and cutscenes. Record and replay with the same region.

`XBE_NO_STREAM_RING_FIX=1` runs the PAL build without the stream callback-ring
growth, for bisecting streaming problems.

The second European disc uses the `pal-eu` profile. Extract its unmodified
`default.xbe` into `assets-pal-eu/`, then run `build-pal.bat eu`. The Linux
profile uses `-DTCNYC_PAL_EU=ON` and the Windows-generated `generated/game-pal-eu`.
Both PAL profiles use the same PAL timing and gameplay code with image-specific
addresses. PAL EU uses its own `settings-pal-eu.toml` and `hdd-pal-eu/`.

The launcher detects either PAL disc from an ISO, a folder, or local `assets/`
and `assets-pal*/` folders. The supported XBE SHA-1s live in
`sdk/runtime/app/game_source.h`, so an NTSC build can identify both PAL discs
even when the PAL images are not present at configure time. A PAL build only
starts its own profile. `build-pal.bat` still needs the matching XBE to compile
that profile.

For the Windows release, `release.bat` builds all three profiles. The package
includes their host executables and shared compiler tools. Windows players
compile the game code for each disc build on first use.

## Linux builds and host tests

Regenerate the guest code for the region in a Windows build shell before configuring
Linux. Use the `release` preset for the USA build or `pal` for the European build.

```bat
cmake --preset release
cmake --build --preset release --target recompile_TrueCrimeNYC
```

On Linux, configure `platform/linux` with SDL3, Vulkan and shaderc available. Pass
`-DTCNYC_PAL=ON` for the PAL build. Configure compares the recorded input and output
hashes with the files on disk and reports which region needs regeneration.
Pass the NTSC, output, Fr/Es/It PAL, and second PAL directories to
`scripts/package-linux.sh <ntsc-build> <output> <pal-build> <pal-eu-build>`.

With `BUILD_TESTING` enabled, build the tests and run
`ctest --test-dir <build-directory> --output-on-failure`. The settings and release
metadata tests run on both hosts. With Python available, the updater test creates
temporary installs, mocks network downloads and exercises restart and recovery.

## Switches for testing

Set them before launching; on Linux, for example, `XBE_VERBOSE=1 ./TrueCrimeNYC`. Release builds read
only `TCNYC_SCENE_FP16`, `TCNYC_PIPELINE_CACHE`, `TCNYC_PIPELINE_WARM` and `TCNYC_DRAW_WORKER` from this table,
plus the player and troubleshooting switches in README.md and README-LINUX.md. The other diagnostic, trace and
tuning switches need a developer build (`build-dev.bat`); release builds ignore them (`core/dev_env.h`).

| Variable | Effect |
|---|---|
| `XBE_VERBOSE=1` | Detailed log in `logs/`. |
| `XBE_PERF=1` | Timing lines in the log every second, and how long the game waited on the renderer. The timers themselves slow the game a little. |
| `TCNYC_SAMPLE=1` | A sampling profiler for the render thread; results go to `logs/samples.txt`. |
| `TCNYC_SCENE_FP16=0` | The original 8-bit scene colour instead of FP16. |
| `TCNYC_PIPELINE_CACHE=0` | Do not load or save the pipeline cache. |
| `TCNYC_PIPELINE_WARM=0` | Do not pre-build saved pipelines at start. |
| `TCNYC_PIPELINE_ASYNC=0` | Compile a shader or build a pipeline first seen during play in the draw, as before. By default both run on a worker and that one draw is skipped. |
| `TCNYC_DRAW_WORKER=0` | Linux: record the scene on the render thread only, without the second recording thread. |
| `TCNYC_TAA_VIEW` | `1` or `2`: show TAA's motion instead of the picture; `3`: where motion is missing (red) and the HUD (green). |
| `TCNYC_TRACE_TAA=1` | Log TAA's camera and reset decisions. |
| `TCNYC_AO_VIEW=1` | Show the AO factor as greyscale on receivers; useful for diagnosing AO band edges. |
| `TCNYC_PROFILE_AO=1` | Windows D3D12: log AO cost every 120 frames that ran AO: frame wall time, the frame's GPU span, GPU ms for the AO prepass, horizon, temporal and resolve passes, flushes per frame and CPU ms in the AO flush path. |
| `TCNYC_AO_SAMPLES=N` | Force the AO prepass sample count (default 1; higher is the old multisampled prepass). |
| `TCNYC_AO_BLUR_DEPTH=N` | AO resolve depth tolerance (default 50; lower softens across depth breaks). |
| `TCNYC_AO_BLUR_TAPS=N` | AO resolve width in texels (default 4 = 5x5; up to 8 = 9x9; wider trades sharpness for banding). |
| `TCNYC_AO_THIN=1` | Enable the isolated-depth-sample rejection experiment (off by default). |
| `TCNYC_AO_FLAT=1` | Enable the derivative normal on flat surfaces experiment (off by default). |
| `TCNYC_AO_RADIUS=N` | AO contact radius (default 0.75; lower keeps bands off flat walls). |
| `TCNYC_AO_STRENGTH=N` | AO strength (default 1.0; lower reduces band contrast). |
| `TCNYC_AO_FLATS=0` | Turn off the vertical-surface AO reduction (walls, signs and billboards otherwise keep only `TCNYC_AO_WALL` of their AO). |
| `TCNYC_AO_WALL=N` | Fraction of AO kept on vertical surfaces (default 0.5; 0 hides it, 1 restores the old look). |
| `XBE_D3D12_DEBUG=1` | Windows: the Direct3D 12 debug layer (needs the Windows "Graphics Tools" optional feature). |

## Reporting a problem

Include `logs/` from beside the executable, your `settings.toml` (beside the executable on Windows,
`~/.config/TrueCrimeNYC/settings.toml` on Linux), your GPU and driver, and whether the build was a release, `dev`
or DLSS build. For visual problems on Linux, the
[Linux troubleshooting section](README-LINUX.md#troubleshooting) explains the F3 trace files to attach.
