# Development workflow

The title-specific C++ code is kept small:

- `src/common/game_info.cpp`: game metadata and runtime callbacks.
- `src/common/pad_tools.cpp`: optional controller scripting, recording and replay.
- `src/common/graphics_options.cpp`: the in-game Graphics menu (F10 or Back + Start). Value lists are in
  `graphics_choices.h` and controller gating in `menu_input.h`; `scripts/test_graphics_choices.py` covers both.
- `src/game/hooks.cpp`: required compatibility hooks for the original Xbox executable.
- `src/game/timing.cpp`: the free-running 48 kHz sample clock and explicitly enabled replay experiments.
- `src/game/native_graphics.cpp`: Graphics rows in the native Options > Display tab, with rules in
  `display_rows.h` (checked by `scripts/test_display_rows.py`). `XBE_TRACE_OPTIONS_PAGE=1` logs the
  Options page, row, focus and input.
- `src/game/options_trace.cpp`: read-only tracing of the Options tab renderer, also under
  `XBE_TRACE_OPTIONS_PAGE=1`.
- `sdk/runtime`: shared runtime tracked in this repository.

`NOTES.md` contains the investigation history. Its newer entries supersede earlier hypotheses. Captures,
crash dumps, replay recordings and one-off analysis scripts under `.local/`, `out/` and `logs/` are local
evidence, not source files to distribute. Generated game code and assets should remain unmodified.

## Build and checks

Close the running port before linking: Windows locks the executable and guest DLL. From PowerShell:

```powershell
$env:CI = '1'
cmd /c build.bat
tools/venv/Scripts/python.exe scripts/run_checks.py
```

The checks compile isolated harnesses around the actual runtime implementations. They cover full CPU
context preservation across APCs, file-object reference lifetimes, retained vertex attributes, and
viewport/scissor behavior, disabled profiler overhead, audio lane equivalence, and shader-cache IO/compiler
fingerprints. They do not replace gameplay or visual verification. Windows, LLVM clang++
and Visual Studio C++ tools are needed; the CRT-based checks locate Visual Studio using `vswhere`.

After a host-only change, if the guest partitions already exist, this avoids recompiling them:

```powershell
tools/venv/Scripts/python.exe scripts/relink_game.py
```

That helper reads the configured partition count and library name; use the normal compiler path for
changes to the original image, recompiler configuration or generated partitions. Python 3.11+ is required
for the helper's TOML reader. The repository's local venv is a convenience, not a shipped dependency.

Commit SDK edits from the project root alongside the game changes that depend on them.

## Optional diagnostics

Set variables before launching; the runtime reads most of them once per process. Unset them to disable.

| Variable | Purpose |
| --- | --- |
| `XBE_CAPTURE_DIR=captures` | Enable F3 frame captures beneath the executable directory. |
| `XBE_CAPTURE_EVERY=25` | Save an intermediate target every 25 draws during the captured frame. `1` produces much larger captures. |
| `XBE_CAPTURE_INPUTS=1` | Include large draws' first vertex inputs and shader constants. |
| `XBE_PAD_RECORD=pad_record.txt` | Record controller changes by presented frame. |
| `XBE_PAD_REPLAY=pad_record.txt` | Replay that controller recording. |
| `XBE_PAD_SCRIPT=5:start,8:a` | Press named buttons at the specified elapsed seconds. |
| `XBE_DETERMINISTIC_REPLAY=1` | Enable intrusive timing/seed/RNG overrides and `.timing` files for controlled experiments only. |
| `XBE_PERF=1` | Collect runtime performance counters. Use with `XBE_VERBOSE=1` to retain informational performance log lines in release builds. |
| `XBE_SHADER_CACHE=0` | Disable the persistent DXBC shader cache for comparisons. Default is enabled; compiled blobs are kept under `shader-cache/` beside the executable. |

Ordinary recording does not enable deterministic overrides. Input replay alone cannot guarantee identical
asynchronous streaming or time of day. Keep captures and heavy logging off when measuring frame times.
The normal launcher enables F3 capture and controller recording for the current debugging workflow.

`scripts/summarize_perf.py <log> [<log> ...]` summarizes those performance logs. Its timings are
one-second averages, not individual-frame percentiles. Shader-cache keys include generated source,
entry point, profile, compiler-DLL contents and compilation flags; damaged entries are recompiled.
Deleting the generated `shader-cache/` directory while the port is closed starts the cache cold again.

## Compatibility constraints

Preserve the original presentation interval and the 48 kHz sample clock: raising the presentation rate
is not a valid performance fix for game-speed problems. Explicit scissors replace the viewport clip;
default scissors use viewport bounds. Current vertex attributes persist across both streamed and immediate
draws. File handles and counted pointer references own files; address lookups do not keep closed files alive.

Keep compatibility corrections separate from title-behavior changes. Do not reinstate the removed missing-head,
resource-release, path-coordinate or cutscene-mode guards as substitutes for diagnosing a new failure.
