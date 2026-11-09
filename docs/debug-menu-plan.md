# Developer debug menu: plan

A developer-only ImGui overlay for testing: start or skip missions, move the
player around, change game state, and inspect what the game is doing. It is
built only into dev builds; release builds contain no debug code, no symbols
for it and no hotkey.

## 1. Keeping it out of release builds

This matters most, so it gets settled first.

**Compile-time switch, not a runtime flag.** A settings key or command-line
flag would still ship the code, and anyone could turn it on. Use a CMake
option instead:

```cmake
# CMakeLists.txt
option(TCNYC_DEV_TOOLS "Build the developer debug menu" OFF)
if(TCNYC_DEV_TOOLS)
    target_compile_definitions(TrueCrimeNYC PRIVATE TCNYC_DEV_TOOLS=1)
else()
    list(FILTER XBE_HOST_SOURCES EXCLUDE REGEX "/src/dev/")
endif()
```

- All debug code goes in a new `src/dev/` folder. The existing
  `file(GLOB XBE_HOST_SOURCES ... src/*/*.cpp)` picks it up; the filter above
  drops the whole folder when the option is off, so the release build does not
  even compile it.
- Code outside `src/dev/` that needs to call in (one hook in
  `game_info.cpp`, see section 3) does it behind `#if TCNYC_DEV_TOOLS`.
  Keep those spots to as few lines as possible.
- `src/game/` is already excluded from that glob and handled separately; check
  how those files get built before adding any dev-only hooks there (the
  `X86_FUNC` replacements), and give them the same `#if` guard.

**Presets.** Add a `dev` configure/build preset to `CMakePresets.json`
(inherits `debug` or `relwithdebinfo`, sets `TCNYC_DEV_TOOLS=ON`, its own
`out/build/dev` folder). `release` never sets it, so it stays `OFF`.

**Safety checks so it can't leak by accident:**

1. `release.bat` / `scripts/package-release.ps1`: fail if the build's
   `CMakeCache.txt` has `TCNYC_DEV_TOOLS:BOOL=ON`.
2. A small script, `scripts/test_no_dev_tools.py`, that searches the release
   executable and DLL for a marker string compiled only into dev builds (for
   example `"TCNYC-DEV-MENU"`) and fails if found. Run it from
   `release.bat` after building.
3. Dev builds show a small "DEV BUILD" label in a screen corner, so a
   screenshot or bug report always reveals which kind of build it came from.

## 2. Menu layout (what goes in it)

Opens with **F9** (F10 is already the Graphics menu) or **Back + LB + RB** on
controller 1. Uses the same ImGui frame, `UiLock` and pad filtering the
Graphics menu uses (`menu_input.h`), so keyboard and controller both work.
Tabs:

| Tab | Features | Priority |
| --- | --- | --- |
| **Missions** | List of every mission/case by chapter; Start, Restart, Complete (pass), Fail; jump to a chapter; set story progress | 1 |
| **Player** | God mode, infinite ammo, max health, set Good/Bad Cop points, give money, give weapons, wanted/heat level clear, no ragdoll | 1 |
| **Teleport** | Show current position; jump to named spots (precincts, safehouses, mission starts); save/load custom spots to a dev file; teleport to map marker | 1 |
| **World** | Time of day, weather, pause the game clock, traffic and pedestrian density, freeze AI, spawn a vehicle | 2 |
| **Game speed** | Pause, single-frame step, slow motion (0.25x/0.5x) and fast-forward (2x/4x) through the existing timing hooks in `timing.cpp` | 2 |
| **Rendering** | Wireframe, show/hide passes (AO, HDR, LOD), force LOD level, free camera, hide HUD | 2 |
| **Info** | FPS and frame time graph, position/heading, current mission ID and state, memory use, loaded streaming sectors | 2 |
| **Log** | Recent log lines in-game, filter by text, toggle verbose categories (traffic trace, options trace) | 3 |
| **Saves** | Quick save/load state slots outside the game's own save system, for repeating one moment fast | 3 |

Each change the menu makes is written to the log
(`LOG("dev: start mission %s")`) so a bug report can be traced back.

## 3. Code structure

```
src/dev/
  dev_menu.h / dev_menu.cpp     window, tabs, F9 toggle, controller combo
  dev_missions.cpp              Missions tab
  dev_player.cpp                Player and Teleport tabs
  dev_world.cpp                 World, Game speed, Rendering tabs
  dev_info.cpp                  Info and Log tabs
  game_state.h / game_state.cpp the one place that knows game addresses:
                                read/write the player, mission manager, clock
  dev_spots.json                (or .toml) teleport spots, mission list names
```

- **`game_state` is the key piece.** Every address and structure offset found
  in the reverse-engineering work (section 4) lives there, with a comment on
  how it was found — the same style as `hooks.cpp`. The menu tabs only call
  functions like `game::dev::Player()->SetHealth(...)`, never raw addresses.
  If an offset turns out wrong, there is one place to fix it.
- **Thread safety.** The UI runs on the window thread; the game runs on its
  own thread. The menu must not write game memory directly from the UI.
  Instead it queues commands (`std::mutex` + `std::vector<std::function>>`),
  and a per-frame hook on the game thread (the same place timing hooks run)
  drains the queue. Reads for the Info tab can copy values into atomics.
- **Hook into the app:** in `src/common/game_info.cpp`, the `ui`, `pad` and
  `message` callbacks call `DrawDevMenu`, `FilterDevPad` and
  `DevMenuMessage` after the Graphics menu ones, inside `#if TCNYC_DEV_TOOLS`.
- **Command-line shortcuts** (dev build only), handy for automated tests and
  for skipping the menus every launch:
  `--dev-mission=<id>`, `--dev-teleport=<name>`, `--dev-god`,
  `--dev-skip-intro`. These pair well with the existing Python test scripts in
  `scripts/`.

## 4. The hard part: finding the game's systems

The port is a static recompilation, so the game's own code is all there, but
nothing yet names the mission system, the player object or the cheats. The
menu's UI is easy; this research is most of the work. Do it in this order,
because each step makes the next easier:

1. **Look for leftover debug code first.** Many games of this era shipped
   with dev menus, cheat handlers or debug strings still in the binary. Search
   `default.xbe` and the game data on the ISO for strings like `debug`,
   `cheat`, `mission`, `case`, `chapter`, `warp`, `godmode`, `skip`. If the
   original dev menu or a level-select function is still there, hook it and
   call it instead of rebuilding it — far less work and less risk.
2. **Known cheats.** The retail game has cheat codes. Find the code that
   checks them; it leads straight to the functions that change player state
   (money, health, etc.).
3. **Mission data files.** Find the mission/case scripts or tables in the game
   data (mission names, IDs, trigger locations). These give the mission list
   for the Missions tab and usually the function that loads them.
4. **Player object.** Watch memory while health or money changes (the same
   approach used for the camera in `camera.cpp` / `mouse_aim.cpp`, which
   already find the pawn the camera follows — a good starting point for the
   player pointer).
5. **Save data.** The save file stores story progress; finding where the game
   reads it back shows which values mean "chapter X, mission Y done".
   Changing those and reloading is a reliable fallback for "start mission"
   if calling the mission loader directly proves unsafe.

Write findings into `docs/reverse-engineering-notes.md` as they come in.

**Status (first pass done):** see
[reverse-engineering-notes.md](reverse-engineering-notes.md). No usable
leftover debug menu, but the mission system is mostly mapped: mission IDs are
CRC-32 names, there are start/stop script functions, a level handler table,
a fight-club arena table, the time-of-day setter and a player lookup. The
main missing piece is the global game object pointer.

**Risk:** starting a mission out of order can leave the game in a state it
never expects (missing flags, wrong vehicles). Mitigation: start missions by
setting save progress to just before it and reloading, rather than jumping
into the middle of a running game, at least until the direct way is proven.

## 5. Phases

| Phase | Work | Result |
| --- | --- | --- |
| 1 | CMake option, `dev` preset, release checks, empty F9 window, DEV BUILD label | Dev/release split working and checked |
| 2 | Info tab, game speed, rendering toggles (use code that already exists) | Useful tools on day one, no new research needed |
| 3 | Research steps 1–4; `game_state` with player and position | Player and Teleport tabs |
| 4 | Research step 3 and 5; Missions tab | Start/skip/complete missions |
| 5 | World tab, state save slots, log viewer, command-line shortcuts | Full tool set |

Phase 1 and 2 are safe and quick. Phases 3–4 depend on what the research
finds; if step 4.1 finds leftover debug code, they get much shorter.

## 6. Testing

- Build both `release` and `dev` presets in CI or before each release; run
  `scripts/test_no_dev_tools.py` on the release output.
- Each menu action gets a command-line form, so the Python test scripts can
  launch the dev build at a given mission and check the log, without anyone
  clicking through menus.
