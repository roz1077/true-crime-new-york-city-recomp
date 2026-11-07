# Development workflow

The title-specific C++ code is kept small:

- `src/common/game_info.cpp`: game metadata and runtime callbacks.
- `src/common/pad_tools.cpp`: optional controller scripting, recording and replay.
- `src/common/graphics_options.cpp`: the in-game Graphics menu (F10 or Back + Start). Value lists are in
  `graphics_choices.h` and controller gating in `menu_input.h`; `scripts/test_graphics_choices.py` covers both.
- `src/game/hooks.cpp`: required compatibility hooks for the original Xbox executable.
- `src/game/timing.cpp`: the free-running 48 kHz sample clock, 60 FPS gameplay and cutscene timing, and
  explicitly enabled replay experiments.
- `src/game/camera.cpp`: field of view and the wide-frame projection for the gameplay camera.
- `src/game/native_graphics.cpp`: Graphics rows in the native Options > Display tab, with rules in
  `display_rows.h` (checked by `scripts/test_display_rows.py`). `XBE_TRACE_OPTIONS_PAGE=1` logs the
  Options page, row, focus and input.
- `src/game/options_trace.cpp`: read-only tracing of the Options tab renderer, also under
  `XBE_TRACE_OPTIONS_PAGE=1`.
- `src/game/traffic_trace.cpp`: read-only vehicle spawn tracing under `XBE_TRACE_TRAFFIC=1`.
- `launcher/launcher.cpp`: the ISO picker `TrueCrimeNYC.exe` shows before the game starts.
- `sdk/`: the XboxXBERecomp SDK with this port's runtime changes, tracked in this repository.

`NOTES.md` contains the investigation history; `GRAPHICS_NOTES.md` the graphics options research. Its newer entries supersede earlier hypotheses. Captures,
crash dumps, replay recordings and one-off analysis scripts under `.local/`, `out/` and `logs/` are local
evidence, not source files to distribute. Generated game code and assets should remain unmodified.

## Build and checks

Close the running port before linking: Windows locks the executable and guest DLL. Create the local Python environment once (Python 3.11+), then build and run the checks from PowerShell:

```powershell
if (!(Test-Path tools/venv)) { python -m venv tools/venv }
$env:CI = '1'
cmd /c .\build.bat
tools/venv/Scripts/python.exe scripts/run_checks.py
```

On Linux the interpreter is `tools/venv/bin/python`.

`cmd` is given the path explicitly because some shells do not search the current directory for batch files.

The checks compile isolated harnesses around the actual runtime implementations. They cover full CPU
context preservation across APCs, file-object reference lifetimes, retained vertex attributes, converted
strip assembly, viewport/scissor behavior, disabled profiler overhead, audio lane equivalence, shader-cache
IO/compiler fingerprints, the Graphics menu and Display row rules, the explicit city-grid patch (confirmed
sites, idempotency and drift rejection) and the guest frame walker over the `0xD0...` stack range. They do
not replace gameplay or visual verification. Windows, LLVM clang++
and Visual Studio C++ tools are needed; the CRT-based checks locate Visual Studio using `vswhere`.

After a host-only change, if the guest partitions already exist, this avoids recompiling them:

```powershell
tools/venv/Scripts/python.exe scripts/relink_game.py
```

That helper reads the configured partition count and library name; use the normal compiler path for
changes to the original image, recompiler configuration or generated partitions. Python 3.11+ is required
for the helper's TOML reader. The repository's local venv is a convenience, not a shipped dependency.

CMake generation recompiles the XBE and then runs `grid_patch` over the generated sources before they are
compiled or linked, so `generated/` should never be patched by hand. The same header is used by the
first-launch compiler. A mismatch in the confirmed city-grid sites fails the build rather than producing
partially corrected code.

Commit SDK edits from the project root alongside the game changes that depend on them.
`patches/xboxxberecomp-local.patch` records the same SDK changes as a diff against upstream XboxXBERecomp
`f7b13fd`, for reviewing or porting them. After changing `sdk/`, regenerate it from a clone of upstream
(new SDK files must be added to that clone's index first, for example with `git add -N`):

```bash
git -C <upstream clone> --work-tree="$PWD/sdk" diff --no-ext-diff f7b13fd -- > patches/xboxxberecomp-local.patch
```

## Optional diagnostics

Set variables before launching; the runtime reads most of them once per process. Unset them to disable.

| Variable | Purpose |
| --- | --- |
| `XBE_CAPTURE_DIR=captures` | Enable F3 frame captures beneath the executable directory. |
| `XBE_CAPTURE_AT=40,p1200` | Take captures automatically, at seconds after the first present or (`p`) at a presented picture number. |
| `XBE_CAPTURE_EVERY=25` | Save an intermediate target every 25 draws during the captured frame. `1` produces much larger captures. |
| `XBE_CAPTURE_INPUTS=1` | Include large draws' first vertex inputs and shader constants. |
| `XBE_PAD_RECORD=pad_record.txt` | Record controller changes by presented frame. |
| `XBE_PAD_REPLAY=pad_record.txt` | Replay that controller recording. |
| `XBE_PAD_SCRIPT=5:start,8:a` | Press named buttons at the specified elapsed seconds. |
| `XBE_DETERMINISTIC_REPLAY=1` | Enable intrusive timing/seed/RNG overrides and `.timing` files for controlled experiments only. |
| `XBE_PERF=1` | Collect runtime performance counters. Use with `XBE_VERBOSE=1` to retain informational performance log lines in release builds. |
| `XBE_SHADER_CACHE=0` | Disable the persistent DXBC shader cache for comparisons. Default is enabled; compiled blobs are kept under `shader-cache/` beside the executable. |
| `XBE_VERBOSE=1` | Keep info and warning log lines in release builds. |
| `XBE_WAITLOG=1` | Log kernel waits blocked for more than 3 s, with the guest stack. |
| `XBE_TRACE_CAMERA=1` | Log gameplay and menu projection calls and their field of view. |
| `XBE_TRACE_FRAME_TIMING=1` | Log frame and simulation tick rates every 2 s. |
| `XBE_TRACE_OPTIONS_PAGE=1` | Log native Options page queries, dispatcher input and the tab renderer. |
| `XBE_TRACE_TRAFFIC=1` | Log traffic candidate pools, selections and vehicle spawns. |
| `XBE_SERIAL_GUEST=1` | Experimental: run only one guest thread at a time (single-core Xbox model), releasing the lock around HLE calls. Used to test guest-thread race hypotheses. |
| `XBE_EFFECT_WATCH=1` | Diagnostic: hardware write watchpoint on cell slot 8 +0x70. Logs the writing instruction's module offset plus the guest registers and stack frame. |

Ordinary recording does not enable deterministic overrides. Input replay alone cannot guarantee identical
asynchronous streaming or time of day. Keep captures and heavy logging off when measuring frame times.
`Play True Crime NYC.cmd`, the development launcher, enables F3 capture and controller recording.

`scripts/summarize_perf.py <log> [<log> ...]` summarizes those performance logs. Its timings are
one-second averages, not individual-frame percentiles. Shader-cache keys include generated source,
entry point, profile, compiler-DLL contents and compilation flags; damaged entries are recompiled.
Deleting the generated `shader-cache/` directory while the port is closed starts the cache cold again.

## Compatibility constraints

Preserve the original presentation interval and the 48 kHz sample clock: raising the presentation rate
is not a valid performance fix for game-speed problems. The 60 FPS option changes only the main loop's
interval-two Swap and keeps 60 simulation ticks per second (`timing.cpp`). Explicit scissors replace the viewport clip;
default scissors use viewport bounds. Current vertex attributes persist across both streamed and immediate
draws. File handles and counted pointer references own files; address lookups do not keep closed files alive.

Keep compatibility corrections separate from title-behavior changes. Do not reinstate the removed missing-head,
resource-release, path-coordinate or cutscene-mode guards as substitutes for diagnosing a new failure.
