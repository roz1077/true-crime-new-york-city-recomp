# Graphics options investigation

## Native Display rows (2026-11-03)

The native Options > Display tab now carries Graphics rows under SUBTITLES: resolution, aspect ratio,
render scale, field of view, anisotropic filtering, MSAA, SMAA, V-Sync, frame limit and FPS counter. The tab's frame holds five rows, so the list
scrolls: the game's row field (`+0x34`) keeps the slot on screen, which its highlight frame follows, and
`native_graphics.cpp` keeps the window's first row (`src/game/native_graphics.cpp`, rules in
`src/game/display_rows.h`, checked by `scripts/test_display_rows.py`). They look and behave like the
game's own rows. D-pad up and down move between rows, and left and right change a value live. Back
restores what the tab had on entry, A keeps the changes, and settings are saved to `settings.toml`. A
scripted controller verified this flow for the original six graphics rows. Resolution changes are staged
with left and right. A applies a pending resolution, and otherwise passes through to the native menu.
Back restores the entry resolution and aspect ratio along with the other settings. The F10 overlay
offers the same settings and also controls display mode. The user verified the resolution and aspect controls,
ultrawide gameplay, HUD and movies in-game on 2026-11-03.

How the tab works, from the menu data and the getter:

- Shell names are CRC-32 hashes of the case-sensitive name (`zlib.crc32`), which is how the `.def`
  files map them. Language strings are hashed the same way in `data/Lang/<lang>/LangTable.dat`:
  a count, then `(hash, offset)` pairs, then NUL-terminated strings.
- A `.tag` object is a field count and class hash, then fields of
  `(type hash, name hash, count << 16 | kind, value)`. Kind 0 is an inline value, 1 a self-relative
  pointer, 3 an embedded object and 7 an array.
- Text and colour reach the menu elements through ports on the shared `MenuPauseInterface`
  (`0xCDEB6BFB`). The port number is a property hash the C++ getter `0xC7B80` answers:
  `thiscall get(uint32_t* out, uint32_t port)`, which returns 0, or -1 when the port is unknown. With
  `offsetPorts` or `offsetPort` set, a duplicated row asks for `port + row`.
- The `pages` junctions are Controls (`0x41148`, text `0x043E9964` + row, count `0x44EAB429` = 5),
  Audio (`0x41EB0`), Display (`0x42930`) and Credits (`0x431F4`). The Display duplicator
  (`0x42A44`) lays rows out by `start` and `step` boxes, not fixed slots, so it shows as many rows as
  `0x08015D89` reports. The game reports 1. Row text is `0xDBCC9F60` + row, where row 0 is
  "SUBTITLES %s" from byte `+0xFF2` of the game object. Row colour is `0xA471F488` + row:
  `0x80808080` when `row == [+0x34]` and `[+0x7C] == 4`, else `0x60606060`.
- `MenuPauseInterface` fields: `+0x30` is the page (`0x0F` Controls, `0x10` Audio, `0x11` Display,
  `0x12` Credits), `+0x34` the selected row, and `+0x7C` the focus (1 tab bar, 4 row list).
- The dispatcher `0xCB400` receives event 9 with an input mask: 1 up, 2 down, 4 left, 8 right,
  `0x20` Back, `0x100` A, `0x200` B. Row counts for up and down come from a per-page jump table
  (`0xCD1D4` index, `0xCD1B8` targets): Display and Audio have 1, Controls 5. Left and right call
  `0xB1140`, which toggles the page's setting without a direction. Back in the row list calls the
  tab's restore (`0xB14F0` for Display), and `0xB13C0` takes the copy on opening Options and on
  entering the tab.

The hooks answer the count, text and colour ports for the added rows, handle up, down, left and
right in the Display row list (the game still handles SUBTITLES' toggle, A and Back), and extend
`0xB13C0` and `0xB14F0` to the added settings. Not yet exercised: the in-game pause menu path (the
replays use the title-screen Options, which runs the same controller and tag) and languages other
than English (the added row text is English only).

## Current build

The modified SDK renderer and runtime are tracked in this repository under `sdk/`. The release opens the ISO picker interface from `TrueCrimeNYC.exe` before starting the game.

The shipped Graphics menu is an overlay drawn through the SDK's `GameInfo::ui` callback
(`src/common/graphics_options.cpp`). It opens with F10 or Back + Start on controller 1 and is driven by the
keyboard, the mouse, or controller 1 through `GameInfo::pad`. That path reads SDL, so PlayStation and Switch pads
work too; ImGui's own gamepad navigation only reads XInput. While the menu is open, the game gets a connected pad
with nothing pressed. After the menu closes, input is held back until the pad is released. Options apply live,
because the renderer re-reads render scale, anisotropy, V-Sync and texture settings every frame and `app::Pace`
re-reads the frame cap. Display mode and resolution wait for an explicit Apply. The settings file is written when
the menu closes. F10's `WM_SYSKEY*` messages are swallowed, so Windows does not enter menu mode on its release.

The frame limit offers 30 and 60 FPS. In 60 FPS mode, the gameplay loop uses the elapsed 60 Hz
vblank count for simulation and rendering, and its Swap uses interval one. Real-time cutscenes
also use interval one at 60 FPS. They keep the 48 kHz sample clock and can render a frame without
advancing the scene when no tick is due. `XBE_TRACE_FRAME_TIMING=1` logs frame and tick rates.
The manual cutscene run on 2026-11-03 held roughly 60 FPS and 60 ticks per second through the
scene and back into gameplay. The audio-clock interval lasted 158.096 seconds at 60 FPS and
158.139 seconds in the 30 FPS replay. Both runs held 60 simulation ticks per second in steady
playback and exited cleanly. Logs are `cutscene-manual-60.log` and `cutscene-baseline-30.log`
under `out/build/release/logs`.
The 2026-11-03 controller replay reached 60 frames and 60 simulation ticks per second in steady
gameplay. The timing trace is in `out/build/release/logs/frame-timing-60.log`.

Verified on 2026-11-03 with a scripted controller (`XBE_PAD_SCRIPT`). Back + Start opened the menu. Navigation
changed render scale to 75% and anisotropy to 8x live. B closed the menu, saved both values, and the intro
carried on. `scripts/test_graphics_choices.py` covers value stepping, frame-cap mapping, pad gating and repeat.
Mouse use and the display-mode Apply path have not been exercised in-game yet.

The native Options tracing hooks moved to `src/game/options_trace.cpp`. The launcher no longer sets
`XBE_TRACE_OPTIONS_PAGE`, so set it by hand to trace.

Reference projects are [BLACK](https://github.com/TGP482/BLACK-XBERecomp), [From Russia with Love](https://github.com/TGP482/007FRWL-XBERecomp), and [Nightfire](https://github.com/TGP482/007Nightfire-XBERecomp). They offer native menu integration and an F10 overlay. Their documented graphics options include monitor selection, display mode, resolution, render resolution, frame cap, V-Sync, HDR, FPS counter, FMV scaling, colour-blind modes, MSAA, SMAA, ambient occlusion, anisotropic filtering, and texture tools. FOV, viewmodel FOV, draw distance, and reload blur vary by title.

## Native Options code

Addresses are guest virtual addresses in the US Xbox executable and its generated C++.

| Address | Finding |
| --- | --- |
| `0x000B64C7` | References the `OptionsWrapper` identifier `0x7BA0A5FA`. |
| `0x000C7B80` | Reads menu values. The `OptionsPageController` request `0x6ADBF5A0` branches at `0x000C8813` to `0x000C88BB`. It returns the value at menu object offset `+0x30` minus `0x0F` through the output argument. |
| `0x000C2B00` | Checks for native page `0x12` before handling a page-specific request. |
| `0x000CDC60` | Forwards an event selector and two arguments to `0x000CB400`. |
| `0x000CB400` | Dispatches selectors 0 through 13. Page writes at `0x000CC48C` and `0x000CC5F5` cycle backward and forward over `0x0F–0x12`. The code wraps at both ends and performs page-specific setup. |
| `0x000B1370`, `0x000B13C0` | Setup for pages `0x0F` and `0x11`, respectively. Copies existing game settings into menu object fields. The other two pages initialize fields directly. No common registration call was found. |
| `0x0025278E` | References `controlNum` (`0xB0F1EBB0`), reads it via `0x0025D2D0`, and stores the result at object offset `+0x2C` during construction. |
| `0x00250215`, `0x00250314` | The tab renderer reads and updates `numTabs` (`0xEF15D709`) at renderer object offset `+0xA8`. The Options renderer instance has yet to be identified. |

The tracing wrappers, now in `src/game/native_graphics.cpp` and `src/game/options_trace.cpp`, call the original functions. Set `XBE_TRACE_OPTIONS_PAGE=1` by hand; output goes to `out/build/release/logs/tcnyc.log`. A live session verified page queries cycling 0 through 3 and back to 0. Dispatcher event 9 with argument `0x08` advances and `0x04` goes backward. Arguments `0x01`, `0x02`, and `0x100` were observed, but their meanings were not verified. Events 0 and 10 recur during ordinary menu updates. The hooks observe the existing menu and do not add controls.

## Native menu assets

- `assets/data/Shell/PauseMenu.def` maps IDs to names. `PauseMenu.tag` contains the native layout and control graph. `menushell.def` names the wrappers, and `menuscript.def` identifies other pause-menu states.
- The tag begins with a count of 420, then 16-byte records. The second word is the named identifier, and the fourth word points into the payload. Display entries run from record offsets `0x14B4` through `0x1524`. The Display row duplicator points to `0x41590`, and its text renderer points to `0x41AE8`.
- The Display row data includes `MenuJunction`, `portnumber`, and `controlNum`. The Select/Accept/Cancel data references `MenuPauseInterface`, `MenuButtonJunction`, `MenuSystemPageController`, `pages`, `triggers`, and `numTabs`.
- The `OptionsPageController` table record at `0x1164` points to a region filled with `0xAD`. A separate reference at `0x46504` sits near relative references to shared and Audio records.
- The `pages` field at `0x41118` references an eight-entry list at `0x464C4`. Its signed offsets resolve from base `0x464C8` to junctions at `0x41148`, `0x41EB0`, `0x42930`, `0x431F4`, `0x43EE0`, `0x441E4`, `0x449C4`, and `0x45BB4`. They are distributed across existing Options and Audio submenu records. This list is not the four visible tabs.
- No literal `numTabs = 4` was found using the encoding seen in other shell tags. The native setter supports a dynamic count, but its Options-specific caller or data binding remains unverified.

## Field of view and draw distance (2026-11-03)

The renderer is the global object `0x36FE80`. `0x245830` builds the viewport (0, width, 0, height, 0, 1) and
`0x244970` stores near and far at `+0x1C` and `+0x20`. The projection comes from `0x245BB0` (finite far,
`0x22CFD0`) or `0x245CE0` (infinite far, `0x22D0D0`). Both are thiscall with one float, k, the focal scale
cot(fov / 2), and the matrix's x and y scales are k times `+0x08` and `+0x10` (0.75 and 1.333 at 16:9).
`0x2459C0` stores the matrix at `+0x340` and its inverse at `+0x380`. The gameplay camera passes its own k
(camera `+0x184`, near at `+0x188`) from `0x7E1B5` and `0xA7553`, with far hard-coded to 2000. A traced
session (`XBE_TRACE_CAMERA=1`) showed k = 1.6 on foot, about 80 x 50 degrees, and 2.414 while aiming.
Menus and previews pass constants (1.5556, 1.0) or use stack-local renderers.

Menu previews that draw 3D into a sub-viewport of the main target belong to the 16:9 UI layer.
`D3D12Backend::UiSqueeze` applies the same horizontal squeeze as the surrounding 2D panel when
the bound target is the main guest target and the viewport is smaller than the frame in both
directions; full-screen world draws and offscreen preview textures keep their own layout. The
locker preview uses viewport `358 216 244 264` and the garage car preview `260 170 380 271` in
the 640x480 guest frame.

`src/game/camera.cpp` divides k by `fov_percent / 100` at the two gameplay sites only. The user first saw
the view narrow as the value rose; k is a focal scale, not a width, and the hook now divides.

The view that changes with aiming is the infinite projection (`0x245CE0`), so the 2000 far plane does not
limit how far the city is drawn. The `.def` distance names found so far belong to pedestrians
(`exteriorDistRemoval*` in `NPCTune`), character LOD (`lod`, `uLODStart`), AI sight (`defaultViewDist`)
and depth of field (`DepthDef` near 10, far 200). The city streams from one `City\Brick_NYC` set with
`.idx`, `.exm`, `.exp`, `.ext` and an `.occ` occlusion file.

The city update at `0x147C40` runs three spatial caches. The first, `0x146FC0`,
selects seven of nine mesh cells on a 174-unit grid. The cache constructor
`0x1480B0` allocates nine entries at stride `0x9C`, and its loader `0x137890`
opens `!CITY!\%d_%d.XB2`. The 70-unit cache at `0x146CA0` loads `.g` files
through `0x1378F0`. The third path at `0x1472E0` uses a 256-unit grid, with
the occlusion file opened at `0x13819F`.

The seven-to-nine mesh experiment was removed after it showed no visible benefit.
An expanded grid needs independently allocated mesh slots. In the stock
city object the nine mesh entries start at `+0x4FA4` and use stride `0x9C`.
The selection scratch list begins at `+0x5520` and the next spatial cache
begins at `+0x55B0`, so increasing the stock loop bound would overwrite
both. Extra slots would need to participate in the async load and eviction
state machine at `0x137600` and unregister their render list entries before
the city object is reset or destroyed. The relevant attach and detach
callbacks are `0x13C240` and `0x13C1D0`.
The 2026-11-03 replay confirmed 7/7 resident meshes at stock and 9/9 with
the experiment enabled. The first two additional meshes were `-2_-4.XB2`
and `-2_-6.XB2`. See `mesh-residency-replay-7.log` and
`mesh-residency-replay-9.log` under `out/build/release/logs`.

## Outstanding work

A fifth native tab is no longer needed for the main options, because the Display tab takes
extra rows. Display mode stays in the overlay. Ambient occlusion and HDR still need renderer work, and draw distance
needs the city streaming radius or occlusion data located (see above).

SMAA 1x (high preset) runs on the finished frame in `D3D12Backend::ApplySmaa`, from the reference
implementation in `sdk/third_party/smaa` (iryoku/smaa 71c806a8, MIT): luma edges, blending weights and
neighbourhood blending through the existing full-screen `Pass`, before the gamma ramp blit. The HUD is
part of that frame and is smoothed with it.

MSAA (2x, 4x or 8x, `msaa` in `settings.toml`) is implemented in `sdk/runtime/gpu/d3d12.cpp`. Render targets,
depth buffers and every pipeline that draws into them use the chosen sample count. `ResolveColor` resolves a
colour target once per change for presentation, SMAA, guest copies, readback and sampling; `ResolveDepth` keeps
the nearest sample (reversed depth) for depth reads and copies. Changing the setting recreates the targets live.
A sample count the adapter does not support for the target formats falls back to one sample.

## Batched controller trace

The latest live session exited normally. The page query produced
`0 → 1 → 6 → 1 → 2 → 3 → 0 → 4 → 0`. Values 4 and 6 are already used by native
menu states, even though horizontal cycling covers only 0 through 3. Guest page
`0x13` therefore cannot be assigned to Graphics as an unused fifth page.

The session captured renderer object `0x006F3010` with a tab count of 4.
The observed return chain was `0x00251138 → 0x00250E7F → 0x0025346F`.
At `0x0025346A`, function `0x00253360` calls the generic dispatcher
`0x00250E10` with selector `0x0B`. The dispatcher calls through the target
object's function pointer at `+0x04`. This identifies the property update
path. It does not yet identify the Options tag record supplying the value.
Object addresses are session-specific.

## Property dispatch and renderer audit

The call at `0x0025346A` gets a value through `0x0025D020` using
the object field at `+0x54`, then passes it to the target selected through
`+0x1C`. Nearby code dispatches selector `0x0A` through the same target.
This narrows the tab-count source to the property binding on that menu
object. The specific Options layout binding and a free native page ID
are still unverified.

The MSAA plan, as implemented above: MSAA needs to cover more than presentation. In `sdk/runtime/gpu/d3d12.cpp`,
trace target allocation from `FindTarget`, target selection for draws, and
pipeline sample counts together. A multisampled render surface needs a
single-sample representation for presentation, guest surface copies and
readback, and any render-to-texture sampling that expects a regular SRV.
The existing `Resolve(Target&)` only invokes an extension for a non-unit
target range. That call is not a hardware multisample resolve. Copy and
readback share `PrepareCopySource`; presentation and guest copies also have
their own resolve call sites. Depth reads and copies need an explicit
strategy because color resolves do not cover depth surfaces.

The guest Swap path calls `app::Pace` with an interval-derived maximum.
The title's 60 FPS override applies only to the main loop's interval-two
Swap. Frame limit changes are published atomically to the thread that runs Pace.
Display mode, resolution, render scale, V-Sync, anisotropic filtering and
texture controls already have host paths. SMAA and MSAA have since been
added; ambient occlusion and HDR need renderer implementations. FOV is
done; viewmodel FOV and draw distance need title-specific work. Reload
blur is a BLACK-specific option.

## Destination port binding

The live return chain reached `0x00253360`, which calls the generic
dispatcher with selector `0x0B`. The call at `0x002578F7` reaches
`0x00253360`. Its setup path reads field `0x93FD92F6` via
`0x0025D600` and copies a three-word binding to object offset `+0x54`.
Both `PauseMenu.def` and `menuscript.def` name this field `destPortNums`.

This corrects the earlier interpretation of `+0x54` as the source of
the tab count. It is a destination port binding. The count source and
the mapping from the top-level Options pages to their native layouts
remain unverified. Nearby functions also reference `inmessage`,
`outmessage`, `nextstate`, and `states`, but those references alone
do not establish that the Options tab count originates in that state
machine. No new executable or controller capture was needed for this
finding.

## City lighting and presentation effects (2026-11-04)

The PC shader comparison led to
two experiments: first replacing ambient and flipping normals, then applying a fixed light to
opaque walls based on previously seen shader and texture IDs. The second experiment made
shading depend on draw history and guest address reuse. The PC shaders establish how the PC
release lights its assets. The Xbox shaders and captures are the reference for this port.

The Xbox soft-shadow and bloom passes use the game's own targets, sampling and blending.
A trial high-resolution shadow option treated any 256x256 render target sampled on texture
stage 2 as a shadow buffer. The city also samples projected road masks on that stage.
The trial also resized only the colour surface, which could leave an incompatible depth
attachment. The option was removed.

PC contrast defaults on and applies after the Xbox gamma ramp, excluding movie and legal-screen
frames. The original comparison used a 1.35 contrast slope around 0.114 luminance and a soft
shoulder above 0.80. The original toe (0.5 * luminance + 0.004) darkened the intro staircase
after the Xbox gamma ramp. Below the pivot the curve now uses a square-root lift anchored
at black and 0.114. Extra bloom and distance blur are optional F10 effects and default off.
Distance blur uses a full-size multisampled depth target from the current frame and clears its
reference before target reuse or destruction. The blit always binds valid descriptors for its
texture reads, including a null depth view when blur is disabled.

A Windows release build and all ten regression checks passed. The 8x MSAA replay captured the
city with the original wall shader (including vertex key `A7C64CECC5C46C51`) and exited cleanly.
The captures in `out/lighting-before/lighting` and `out/lighting-replay/lighting` are guest-target
captures before the final gamma and presentation effects. The isolated profile reached a save
prompt, so these captures confirm shader selection but do not establish Xbox visual parity in
the opening cutscene or free roam. The earlier `captures-intro5` and `shots/ps2-opening4` pairs
remain historical references for the opening. See NOTES.md for the open Xbox light-balance
comparison and the verified blend, retained-attribute and scissor fixes.

### Precinct shadow fill

The wall variants observed in the precinct exterior use a capped ambient fill after the
guest diffuse calculation. The fill is weighted by the actual ambient mask, including streamed
vertex attribute 4 on the facade and stair variants identified in capture 183857.
It falls off with increasing vertex brightness and outside the ambient range observed in the
exterior. It leaves diffuse alpha, fog, and the original light terms intact. The initial selection used captured program and declaration keys and required a textured, opaque,
depth-writing draw on a full-size target. It does not learn from texture addresses or include the separate additive
light passes. City shadow fill defaults on and can be switched live in F10.

The first pass was too subtle in the live precinct test. Capture 183857 confirmed the fill ran
on its three selected variants, while opaque draws around the entrance also used shader keys
AB7F16AFF7B98886, 8BCD20F6A1BB8B05 and 4DFD3329F274D76B with a streamed ambient mask.

The flicker pair (184219 and 184225) shows the same 504-vertex precinct frieze using texture
8284B000 in both frames. Draw 1153 in the dark frame used key 71F35A5AB47530F9 without fill.
Draw 1208 in the bright frame used key 8BCD20F6A1BB8B05 with fill. Both programs read the
streamed ambient mask, and both now receive the same treatment for opaque wall draws.

The second flicker pair (184519 and 184545) exposed two more variants in the dark frame.
The 504-vertex frieze used FD91B234BC0B4FA9 and the surrounding stone used 7A05371AA7B57460.
Both omitted the fill. Matching the captured texture contents across the precinct captures
confirmed that the wall and frieze variants seen so far are now covered.

The storefront pair (184802 and 184807) confirmed the same coverage gap beyond the precinct.
For example, draws 0765 and 1148 use the same 738-vertex mesh and texture 82580180, with the
fill in the first capture and without it in the second. The renderer now recognizes the
prelit program family by the ambient mask multiply, normal and texture scale used by the
translated shader. This check runs on shader generation and accepts both retained and streamed
ambient masks across light-count variants. The existing opaque, depth-writing, full-size-target
draw gate and the ambient range are still required.

### W 47th St facade

Capture 191443 shows the facade go from absent to flat-coloured between draws 875 and 900,
while the billboard textures are already visible. The draws in that range bind real city textures
and the capture dumps show their texel detail. A captured draw in the facade batch (0888) has
guest constant c83.z = 3.32995e38. The city shader multiplies its input texture coordinates by
that constant. The same value (0x7F7A8486) is in the mesh transform record at 0x65D31C in
Brick_NYC.exm. The extracted file was compared byte for byte with the original Xbox ISO.
The title copies the transform's scale to the constant through 0x245EF0 and 0x244C80.
There is no evidence that the port dropped a texture or generated this UV scale.
The exact appearance on Xbox hardware has not been compared for this camera position.
