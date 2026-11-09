# Reverse-engineering notes (for the dev debug menu)

Findings from `assets/default.xbe` and `assets/data`, gathered for
[debug-menu-plan.md](debug-menu-plan.md). Addresses are XBE virtual
addresses, which match the `sub_XXXXXXXX` names in `generated/game`.
"Confirmed" means read from the disassembly; "guess" means it still needs a
test in the running game.

## Leftover debug code

- No cheat or level-select strings in the executable. `MenuDebugInterface`
  (string `0x2D6FE4`, used at `0x271B30`) and `MenuDebugTuner` (`0x2DF148`,
  used at `0x27E440`) are class names. They are registered in the script
  class table, but no shipped menu seems to use them. **Next step:** follow
  those two functions to see if the classes still have working code behind
  them.
- `assets/data/ScriptDev/AI_DevMission.*` and `AI_DevNPC.*` exist, but the
  `.tag` is 4 zero bytes and the `.def` is empty: the dev mission was stripped
  before release.
- `Shell/ResTestScript` is a small test screen (resolution/safe-area boxes).
  It is harmless but useless for missions.
- "Camera debug poisition set/unset" (`0x2D2F9C`/`0x2D2F7C`, used near
  `0x26385` and `0x271DA`) is a leftover debug camera toggle. Worth checking
  for a free camera.
- `Shell/MissionMenu` is the normal in-game "accept mission?" screen, not a
  level select.

## Name hashes

Every script, mission and object name is stored as **plain CRC-32 of the exact
name** (`zlib.crc32(b"INT_M2_PROLOGUE") == 0x4F2C14DD`). Case matters.

Every `.def` file in `assets/data` is a `hash: name` table
(`0b1ca2b8: MissionOption1Highlight`), so together they work as a free
dictionary. Load all of them to turn any hash in the code or data back into a
name.

## Missions

Mission IDs found in code (each pushed as a constant in its level's handler):

- Story: `BC1_M1`…`BC4_M8`, where BC = "Big Case" (chapter) and M = mission.
  Not every number exists (no `BC1_M2`, `BC3_M3`, …; check against `.def`
  files).
- Follow-up parts: `BCn_MmF` (e.g. `BC1_M1F`), and `BCn_M0F` chapter intros.
- Academy/intro: `INT_M1`…`INT_M6`, plus sub-parts `INT_M2_PROLOGUE`,
  `INT_M2_PHYSICAL`, `INT_M2_SHOOTING`.
- Endings: `EndG_M1` (good cop), `EndB_M1` (bad cop).
- Side content (names from `.def` files): fight clubs `FC_M01`…`FC_M10`,
  street racing `SR_*`, informant tasks `CI_*` (Cabbie, King, Madam), Redman
  bonus.

Mission script data lives in `assets/data/Missions/*.exm` (one per chapter:
`BC01.exm`…`BC04.exm`, `INTRO.exm`, `EndGame.exm`, `FightClub.exm`,
`StreetRacing.exm`, the `CI*` files, `Global.exm`). The game opens them as
`!MISSIONS!\%s.exm` (string `0x2D4AA8`, used at `0x7F094`).

### Level handler table — confirmed

`0x3359BC`–`0x335CD4`: 67 entries of 12 bytes, one per interior/mission
level. Each entry holds a handler `int handler(Game *game, int phase)`
(`cdecl`, game in `[ebp+8]`, phase in `[ebp+0xC]`):

- phase 0: level loaded. Sets up the mission (time of day, starts scripts,
  places the player).
- phase 1: second setup step.
- phase 2: level finished. Stops this level's scripts, queues the next one,
  then posts the "transition" message.

Example: `0x1FC540` (academy `!INT!\Intro\M2_MainFloor.XB2`) starts
`INT_M2_PHYSICAL` and later hands over to the shooting range.

Just before this table, `0x3358D8`–`0x3359A4` is a **fight-club arena
table**, 20 bytes per entry: `{level file string, arena name hash, 0, 0, 0}`,
e.g. `!INT!\FC\M01_FishMarket.XB2` / `FC_M01_FishMarket`. It is read at
`0x1FBC47`. This gives a ready-made list for a "jump to fight" option.

### Key functions — confirmed from disassembly, not yet tested live

| Address | Signature (thiscall: `ecx` = game object) | What it does |
| --- | --- | --- |
| `0x7FA90` | `bool StartScript(uint32 hash, bool refresh)` | Starts the script/mission with that hash in the script manager at `game+0x2A8C` (calls `0x1DA10`), then refreshes `game+0x1B4` (`0x1E720`) if `refresh`. 111 callers. |
| `0x7FAE0` | `bool StopScript(uint32 hash)` | Stops that script (`0x1D2B0`). 110 callers. |
| `0x7CE70` | `int EndLevel(bool flag)` | Posts message `0xC3` to the message system at `0x374938` (level transition), clears `game+0x2A5C` and `0x37F944`. 69 callers. |
| `0x8EBC0` | `int StartMode(int startMode, int missionMode)` | Logs `startMode: %i, missionMode: %i`, makes a 0x140-byte mission controller (vtable `0x2DA270`) and hands it `missionMode` (`0xCF060`). Called by academy handlers with (1,2), (2,2), (1,1). |
| `0x7EAC0` | `SetClock(int hour, int minute, int ?)` | `[0x37F940] = (hour*60 + minute) * 3600`. **This is the time of day.** |
| `0x7EA40` | `FadeOrSettle(bool)` | Runs 30 frames of the `0x351BA8` system and sets `game+0x19F`. Probably a fade-in/settle before play. |
| `0x8C890` | `Object *FindObject(int, const char *name, int)` | Called as `FindObject(-1, "Player", 1)`: **returns the player object.** `obj+0x138B` is a flag set on load; `obj+0xB8` points to a sub-object whose `+0x3C` has flag `0x80` set during setup. |

Globals seen: `0x37D284` (object with `+0xB8`, also gets flag `0x80`; likely
the player's camera or controller), `0x37F610` (manager given the new mission
controller at `0x8EC78`), `0x374938` (message system).

## What this means for the debug menu

**Jump to a mission (most likely to work):** from the game thread, between
frames, with the game object pointer:

1. `StopScript(current)` for the running mission, if any.
2. `StartScript(crc32("BC2_M3"), true)`.

If a mission needs its own level (interiors), the handler table shows the
order the game uses: load the level, then the handler starts the scripts in
phase 0. The safer path is to imitate a phase-2 handover: `StartScript(next)`
followed by `EndLevel(true)`, so the game runs its own transition.

**Time of day:** call `SetClock(h, m, 0)`, or write `0x37F940` directly.

**Player:** `FindObject(-1, "Player", 1)` gives the player, a starting point
for health, money and position offsets.

## Live test results (2026-11-09)

**Game object: `[0x360E00]`** (used ~2,300 times; `game_info.cpp` already
reads it). Live values: game `0x517540`, script manager (`+0x2A8C`)
`0x83746000`, `+0x1B4` `0x51A140`, both set from the title screen on.

Test driver: `src/dev/dev_missions.cpp`, run each frame from the simulation
hook in `timing.cpp`, dev builds only (`cmake --preset dev`). Settings:

```
XBE_DEV_SKIP_LAUNCHER=1  XBE_DEV_MISSION=BC1_M1  XBE_DEV_CLOCK=22:00  XBE_DEV_DELAY=900
```

Results, all called from the title screen's attract demo:

- `SetClock(22, 0, 0)`: **works.** `0x37F940` went to exactly 4752000, and
  the attract scene's street lamps came on.
- `StartScript(BC1_M1)`: returned 1 (accepted), no crash, game kept running
  for 30+ s. Nothing visible changed on the title screen. **Expected:** the
  attract demo is not gameplay, so the real test has to run after loading a
  save.

Notes: dev output must use `LOGR` (the dev preset is RelWithDebInfo, so
`NDEBUG` hides `LOGI`). The first run after rebuilding the exe recompiles the
game DLL at startup (about 30 s, see `logs/compile.log`).

**F8 hotkey test (same day):** F8 fired correctly in gameplay (script
manager had moved to `0x83B47000`, so a save had loaded). `StartScript(BC1_M1)`
returned 1 every time, but **no mission started**: starting the script alone
is not enough. Next to try: `XBE_DEV_END_LEVEL=1` (start, then the level
transition, like a handler's phase 2), then the per-mission handlers in the
`0x3359BC` table, then save progress.

**`EndLevel(1)` after `StartScript` (same day): crashes, in gameplay and on
the title screen alike.** It crashes inside the call: `0x7CE70` → message
`0xC3` via `0xDB900` → handled at once → null read of `[0 + 0x234]` at
`0x1AD367` (callers `0x1AC0BB`, `0x1B049F`, `0x64B73`, `0x139FD0`, main
loop). The transition expects state that the game's own mission flow sets up
first (probably the next level or mission controller), so **don't use
`XBE_DEV_END_LEVEL`**. Next: find what `0x1AD367` reads at `+0x234` and who
sets it, or go through the per-mission handlers / `0x8EBC0`
(`startMode`/`missionMode`) instead.

## Mission jump: WORKS (2026-11-09)

Confirmed in game: F8 in a loaded save with `XBE_DEV_MISSION=BC1_M3` loads the
mortuary mission.

How the game starts a mission (and how `src/dev/dev_missions.cpp` does it):

1. **Story table** from `!ASSETLISTS!\LevelInfo.tag` (`masterCaseInfoTable`),
   built at startup by `0x83CC0`: case count at `0x35C30C`, cases at
   `[0x35C310]` (16 bytes: mission count `+8`, missions `+0xC`), missions are
   32 bytes with the name CRC-32 at `+0`, scene handler at `+0x18`. Fields:
   `missionName`, `successMission`, `secondarySuccessMission`,
   `secondaryFailureMission`, `freeRoamingMission`. `0x7EFE0(hash, &case,
   &mission)` looks a name up (cdecl).
2. **Queue:** set `game+0x2AA4` = case, `game+0x2AA8` = mission index,
   `game+0x19D` = 0, then call `0x7F510(4)` (thiscall on the game). This is
   exactly what closing the pause menu does at `0xA56DE`. The pause menu
   writes its choice through the menu-variable setter `0xAABA0` into
   `0x3735A4` (case) / `0x373590` (mission + 1); writing those yourself does
   not work, because the pause menu overwrites them when it closes.
3. `0x7F370` then applies the pending mission with
   `SetMission(case, mission, flag)` (`0x7F160`) at the level load, and the
   scene handler's phase 0 sets the mission up.

Current mission: `game+0x2A84` (case), `game+0x2A88` (mission). Mission
progress states: `int[16][16]` at `game+0x268`.

**Scene registry:** 197 named scene scripts `{handler, next, name hash}`
linked at `0x35C330` and registered by static init around `0x27A000`.
Besides the story, there are leftover test scenes: `TestLevel`,
`TestInterior`, `TestPolicePrecinct`, `TestGunShop`, `TestDrivingCourse`,
`TestFiringRange`, `FightArena`, `WeaponArena`, `TestCarVariants`, etc.

**Dead ends:** `StartScript` alone does nothing visible; `EndLevel`
(`0x7CE70`) directly crashes, because handlers first delete the player
(`0x88060(-1)`, `0x88060(-2)`) and rebuild it after.

## Script stages (dev menu Stages tab)

Script natives are in a `{.., hash, function}` table in `.data` (e.g.
`SetState` 0x53C60D3B → `0x17D680`, `GetState` → `0x17D6C0`,
`FindObjectByName` → `0x17E200`, `SetPos` → `0x1809E0`). A script instance
keeps its state hash at `+0x424`, its state table (tag node) at `+0x478`,
and a requested state at `+0x490` (applied by its update, `0xFD220`).
`src/dev/dev_scripts.cpp` hooks `0xFD220` to list live instances and to
request states. Confirmed in game: in Human Cargo, one instance (owner
`0x66DD6C`) holds Tommy/Jing's states (`WaitForNick`, `TommyFight`,
`TryToFlee`, `CallOutGoons`, `RunToNextWaypoint`, `StartJing`), and
jumping between them works.

Dead ends: world-object names are hashes of names that appear in no game
file (stripped), and Tommy is spawned by the mission rather than placed, so a
name list or teleport-to-Tommy doesn't work. The generic enemy scripts have no
`Die` state, so "defeat all" by stage does nothing; killing enemies needs
their health instead.

## Player (dev menu Player tab) — confirmed in game

- Player character: `[0x37D284]`. Inventory/wallet `+0xB4`, health component
  `+0xBC` (max `+0x08`, health `+0x10` floats, flags `+0x18`, bit `0x04` =
  ignore damage).
- Money: inventory vtable `+0x8C` AddMoney(amount). Good / Bad Cop points:
  ints at `game+0x1F8` / `+0x1FC`; Good Cop via `0x854A0` on `game+0x1D0`,
  career points via `0x802E0` on the same object. Ammo: `0x16FCC0(count)`.
- Weapons: item names like `Pistol_DE`, `Shotgun_M3`, `Special_RPG7`,
  `Special_FThrower`. Spawn with `0x9F460(name hash, position)` (position from
  `0x21EC70`), attach with `inventory.0x191650(character, item)`, put in a hand
  with inventory vtable `+0x20(character, hand, item, 0)` (1 right, 0 left).
  `0x110AD0(character)` (RemoveNickWeapon) clears all weapons; mission starts
  call it.
- Missions block guns two ways: inventory byte `+0xA8` (DisableSwitchItems)
  and bits `0x4020` in inventory `+0x50` (set by Human Cargo, Ristorante and
  Sour Candy after loading; `0x18DD80` refuses items while set).
- Game speed: the simulation hook in `timing.cpp` scales the ticks it passes.

## Tommy container prompt bug (investigation, 2026-11-09)

Trace of a full Human Cargo run (`XBE_DEV_TRACE=1`): Tommy's script
(`AI_BC4M5`) goes StartJing → WaitForNick/RunToNextWaypoint/CallOutGoons
loops → WaitForTommyFight → TommyFight → TryToFlee. Its only `SetGrabbable`
call is `SetGrabbable(0)` in StartJing (Tommy can't be grabbed for the whole
mission), and it never calls a scripted-sequence or prompt native during the
fight. So the throw-off prompt is not driven by Tommy's script: it's the
engine's own combat move (C++), probably the HUD hint/prompt path
(`HUDFetchHintRequest`, `ShowHint`, `Nick_Press_*` names). Next: find and
trace that path to measure the window.

Update: the container fight is a "sumo" grapple (animations
`Nick_Grappled_Sumo_*` / `NPC_Grapple_Sumo_*` in `Anims/BC4_M5_CargoShip`),
run by `0x58D90`: state byte `+0x0C`, timers `+0x30` / `+0x44`, balance
`+0x40`; it reads the buttons through `0x113D40` and posts
`BC4_M5_PlayerLosing` to the mission. Its timers count down by the frame time
`[0x37F630]` (main timer object `0x37F610` + 0x20), so the push-off window is
measured in time, not frames. The script clock is 60 units a second
(`Secs2Lux` multiplies by 60.0), so trace timer values divide by 60.

Conclusion: the window's length in seconds is the same in the port as on the
Xbox; the port just checks the button more often (60 fps instead of the
Xbox's 30 fps or lower in this scene), so a window that is about one Xbox
frame long is easier to hit. The Xbox bug is most likely a short window
combined with frame drops.

Measured (dev build hook on `0x58D90`, `XBE_DEV_TRACE=1`, game at 30 fps):
the grapple's timers and the frame time are in 1/60 s ticks. Both sides of
the shove run as grapple objects that take turns (state 3 = winding up for
about 700-870 ms, then 1 = recovering, 2 = being pushed). Nick's B window is
state 1 with balance at 1.0 (Tommy at the edge), and it ends when Tommy's side
starts its next shove. In the run: the window lasted **166 ms (10 ticks, 5
frames at 30 fps)**; the second time B was pressed 65 ms in and the grapple
went to state 5 (win). The window is tight, but it lasts the same time in the
port as the game intends and spans several frames even at 30 fps, so the port
does not have the one-frame bug.

Fix (all builds): `src/game/sumo_grapple.cpp` hooks `0x58D90` and, while a
side is in state 1 with the other at the edge (balance ≥ 0.999), runs that
side's update with the frame time divided by `kWindowScale` (1.5). Measured
at 30 fps: the window went from 166 ms (5 frames) to 200 ms (6 frames), and
the push-off was won. The other side's update sometimes runs nested inside
this one and then also sees the slower clock for that frame, which is
harmless.

## Still unknown

1. **Mission jump during gameplay.** Load a save, then trigger
   `StartScript` (and try `StopScript` on the current mission +
   `EndLevel`). Needs a later trigger than "N frames after start": a key, or
   waiting for the player object (`FindObject("Player")`).
2. Which levels each mission needs. The `BCn` handlers between `0x1DE000`
   and `0x1FF000` push the mission IDs; mapping each handler to its table
   entry gives the mission → level list.
3. Story progress in the save (what "BC2 done" looks like), for a safe "start
   from save before mission X" fallback.
4. Player structure offsets: health, money, Good/Bad Cop points, position.
   Next step: log `FindObject("Player")` and watch memory while each value
   changes in game.
5. Whether `MenuDebugInterface` / `MenuDebugTuner` still have working code
   behind them.

## Tools used

Small Python helpers (no dependencies) that read XBE section headers, turn
string addresses into file offsets, and load every `.def` as a hash
dictionary. The disassembly comments in `generated/game/x86_recomp.*.cpp`
(`// 0008EC2F push 0x2D4F28`) are searchable with `grep`; each function starts
with `X86_FUNC(__imp__sub_XXXXXXXX)`. Worth turning these into
`tools/re/` scripts if the work continues.
