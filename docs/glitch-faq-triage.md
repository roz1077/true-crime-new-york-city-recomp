# Glitch FAQ triage (yukichigai's Xbox bug list)

Source: [True Crime: New York City – Glitch/Error Guide (Xbox), v0.2](https://gamefaqs.gamespot.com/xbox/925118-true-crime-new-york-city/faqs/40332).
Addresses are XBE virtual addresses, matching `sub_XXXXXXXX` in `generated/game`.

**Already fixed by the port**

- **General IV — "Copland"**: `src/game/traffic_trace.cpp` repairs the zeroed
  traffic-model weights after the scripted/random-crime vehicle queue.
- **Human Cargo I — Tommy / cargo crate**: `src/game/sumo_grapple.cpp` slows the
  grapple clock during the push-off window.

**Fixed in this change**

- **General III — damage while entering/exiting a car**:
  `src/game/impact_damage.cpp` hooks the central damage routine `0x19D3B0`.
  For the player and `DAMAGETYPE_IMPACT` (`0x2B2D2DAF`,
  `crc32("DAMAGETYPE_IMPACT")`), it caps the total impact damage in each
  second of game time (60 simulation ticks, `0x363D0C`) to 15% of max health.
  The cap applies to all impact damage to the player, not only during
  enter/exit. The collision/scrape category is identified; bullets,
  explosions, fire and falls use other hashes.
- **General V/e — no informant calls or "Precinct Cleaned" message at voice
  volume 0**: `src/game/audio_volume.cpp` hooks the mix-bin volume getter
  `0x2291F0` (table at `0x337640`) and replaces an exact 0.0 with a tiny
  positive value. The stream start check in `0x25DA90` then succeeds, so the
  phone call plays (silently) and its completion advances the mission.

## Findings that apply to several entries

### Script native table

`0x2ECE10`–`0x2EE...` is the script-native table. Each entry is 12 bytes:
`{name_crc32, function, next}`. Useful entries:

| Native | Function |
| --- | --- |
| `TakeDamage` | `0x183020` |
| `SetScriptedDeath` | `0x183060` |
| `PlayPhoneVO` | `0x182620` |
| `PlayVO` | `0x182660` |
| `Shoot` | `0x181B80` |
| `IsProne` | `0x17FFE0` (pose at `[obj+0xAC]+0x50 == 2`) |
| `IsDazed` | `0x182FE0` (`[obj+0xA8]+0x1C == 2`) |
| `InVehicle` | `0x184F90` |
| `StartEnterCarThread` | `0x1853E0` |
| `StartExitCarThread` | `0x185430` |
| `SetWeatherValue` | `0x17EB10` |
| `SetRainType` | `0x17EC70` |
| `SetPartFogDensity` | `0x17EC00` |

### Damage and health

`0x19D3B0` is the routine that actually subtracts health:

- `ecx` = health component; owner object at `+0x04`, max at `+0x08`,
  current at `+0x10`, flags at `+0x18`.
- `[esp+4]` = damage type hash, `[esp+8]` = amount (float),
  `[esp+0xC]` = source object.
- `health+0x18` bit `0x04` is `IgnoreDamage`; the routine returns before
  subtracting when it is set (checked at `0x19D401`).
- `DAMAGETYPE_EXPLOSION` is `0xE78846D1`; `DAMAGETYPE_IMPACT` is
  `0x2B2D2DAF`.

### Effects registry

Effect classes are linked at `0x41DAF4`; each descriptor is
`{next, factory, 0}`. The `Gibs` factory is `0x74580`, `DamageVolume` is
`0x69440`, `FlameSpreader` is `0x72CE0`, `FlameDamage` is `0x733D0`.

## Triage

| FAQ entry | Verdict | Why |
| --- | --- | --- |
| General I — freeze / buzzing | **Needs a crash dump** | No single cause. The port already fixes several console freezes (stream callback ring, save flush, device removal). Without a repro or dump, a change would be guesswork. |
| General II — HUD/compass disappears, shading breaks with weather | **Needs a runtime trace** | Weather natives are known (`SetWeatherValue`, `SetRainType`). The symptom sounds like a render-target/state problem. Reproduce by forcing rain/snow; no mission replay needed. |
| General III — massive damage entering/exiting a car | **Fixed** (`impact_damage.cpp`) | Central damage routine and `DAMAGETYPE_IMPACT` identified. Cap + per-second limit. |
| General IV — Copland | **Already fixed** | `traffic_trace.cpp`. |
| General V — no informant calls after interrogation | **Fixed** (`audio_volume.cpp`) | Phone path is `PlayPhoneVO` (`0x182620`) → `0x7BAA0` → `0x796A0`; stream start `0x25DA90` fails when the mix-bin volume from `0x2291F0` is 0. The getter now raises an exact 0.0 to a tiny positive value. |
| General a — headlights point upwards | **Low priority / needs visual repro** | `Environment.def` has `Headlights`/`HeadlightSegment`; likely a transform or matrix issue. No code path identified yet. |
| General b — car launched at Hwy 9a & 10th | **Data/level only** | A hole in the road collision. A code fix would need a location-specific collision hack; better to patch the level data. |
| General c — purchased car color mismatch | **Needs a UI trace** | Dealership preview/purchase state. No gameplay event needed, just the menu; but the menu script is binary and the state bug is not identified. |
| General d — flamethrower keeps firing when knocked down | **Probably fixable, needs the fire-stop path** | `IsProne` is known (`[obj+0xAC]+0x50 == 2`). The missing piece is the engine call that stops a held weapon. The AI scripts `AI_BC4M1` / `AI_Crime_Misc` also have `FlameAttack`/`flameAttackTimer`, so a generic engine stop may need an AI-side stop too. |
| General e — "Precinct Cleaned" message missing | **Fixed** (same as General V) | The FAQ links it to the voice-volume-0 phone-call bug. The volume clamp lets the Chief's call start. |
| Bad Accounting I — ghost body after arrest by sliding shelf | **Needs a short trace** | Damage respects `IgnoreDamage`, so this is not the normal damage path. The `Gibs` effect is a separate path. Trace the shelf contact and the `Gibs` factory (`0x74580`) during BC1_M6; the dev menu can jump straight there. |
| Vertigo I — goon walks through elevator wall | **Data/level only** | The elevator interior is not collidable. Could enable collision on the elevator object, but the level/object data is needed; a blind code change is risky. |
| A Bad Crowd a — Goth Leader flame after knockdown | **Same as General d** | Plus the AI `FlameAttack` state can restart the flame. Needs a trace of the AI state and the weapon stop. |
| Human Cargo I — Tommy off the crate | **Already fixed** | `sumo_grapple.cpp`. |
| Human Cargo II — Jing stuck on ladder | **Needs a script trace** | Script limitation in `AI_BC4M5`. A safe fix would block the player from passing Jing before the ladder, or reset Jing's state. Mission jump reaches it without replaying the story. |
| Human Cargo III — can't grab/interrogate Jing | **Same script issue** | Same as II. |
| Human Cargo IV — Strike button does nothing in Jing interrogation | **Same script/proximity issue** | Likely the same "player got ahead of Jing" state. Same trace. |
| Red Man Bonus a — normal civilians appear | **No fix needed** | Harmless spawn quirk; the FAQ itself says it makes the mode slightly easier. |

## Suggested next steps (no full playthrough)

1. **Impact cap**: in a normal save, let a car scrape during enter/exit and
   check `logs/` for `tcnyc: player damage type 2B2D2DAF` followed by
   `tcnyc: impact damage on player capped`. The hook also logs the first 16
   player damage types; if the scrape shows a different hash, extend the cap
   to that type.
2. **Voice volume 0**: load any save, set Voice Volume to 0, and trigger a
   phone call or informant mission. `logs/` should show `tcnyc: mix bin N was
   0`; the call is silent but its completion should advance the mission.
3. **Ghost body**: dev-jump to BC1_M6, trace `0x19D3B0` (to confirm it is not
   damage) and the `Gibs` factory `0x74580` / `DamageVolume` factory
   `0x69440`. The fix is likely "do not gib a live player while the arrest
   interaction is active".
4. **Flamethrower**: trace the weapon update while the player or Goth Leader
   is knocked down (`IsProne` / `IsDazed`). Find the function that keeps the
   flame emitter alive, then call the game's own stop/put-away path on the
   knockdown transition.
5. **Weather/HUD**: add a dev weather toggle using `SetWeatherValue` /
   `SetRainType`, reproduce the transition, and trace the HUD render target
   and the weather state. This is independent of missions.

The mission jump (`docs/reverse-engineering-notes.md`) makes the mission-specific
entries reachable without replaying earlier cases. General entries need only a
save and the relevant trigger (car, phone, weather), not a story run.
