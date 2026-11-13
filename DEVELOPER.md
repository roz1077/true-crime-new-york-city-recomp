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

## Switches for testing

These work in any build, release included. Set them before launching; on Linux, for example,
`XBE_VERBOSE=1 ./TrueCrimeNYC`.

| Variable | Effect |
|---|---|
| `XBE_VERBOSE=1` | Detailed log in `logs/`. |
| `XBE_PERF=1` | Timing lines in the log every second, and how long the game waited on the renderer. The timers themselves slow the game a little. |
| `TCNYC_SAMPLE=1` | A sampling profiler for the render thread; results go to `logs/samples.txt`. |
| `TCNYC_SCENE_FP16=0` | The original 8-bit scene colour instead of FP16. |
| `TCNYC_PIPELINE_CACHE=0` | Do not load or save the pipeline cache. |
| `TCNYC_PIPELINE_WARM=0` | Do not pre-build saved pipelines at start. |
| `TCNYC_DRAW_WORKER=0` | Linux: record the scene on the render thread only, without the second recording thread. |
| `TCNYC_TAA_VIEW` | `1` or `2`: show TAA's motion instead of the picture; `3`: where motion is missing (red) and the HUD (green). |
| `TCNYC_TRACE_TAA=1` | Log TAA's camera and reset decisions. |
| `XBE_D3D12_DEBUG=1` | Windows: the Direct3D 12 debug layer (needs the Windows "Graphics Tools" optional feature). |

## Reporting a problem

Include `logs/` from beside the executable, your `settings.toml` (beside the executable on Windows,
`~/.config/TrueCrimeNYC/settings.toml` on Linux), your GPU and driver, and whether the build was a release, `dev`
or DLSS build. For visual problems on Linux, the
[Linux troubleshooting section](README-LINUX.md#troubleshooting) explains the F3 trace files to attach.
